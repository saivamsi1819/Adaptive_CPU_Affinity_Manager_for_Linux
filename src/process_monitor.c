#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>

#define LINE_SIZE 4096
#define NAME_SIZE 256
#define MAX_DISPLAY 15

/*
 * A process snapshot.
 * starttime (field 22 in /proc/<pid>/stat) helps distinguish a process from
 * a later process that reuses the same PID.
 */
typedef struct {
    pid_t pid;
    char name[NAME_SIZE];
    unsigned long long ticks;      /* utime + stime */
    unsigned long long starttime;  /* process start time in clock ticks */
} ProcessStat;

typedef struct {
    pid_t pid;
    char name[NAME_SIZE];
    double utilization;
} ProcessUsage;

/*
 * Parse /proc/<pid>/stat.
 *
 * The comm field (process name) is enclosed in parentheses and may contain
 * spaces and ')' characters, so we parse after the final ')'.
 *
 * After ')' the first token is field 3 (state):
 *   utime field 14 -> token index 11
 *   stime field 15 -> token index 12
 *   starttime field 22 -> token index 19
 */
static int read_process_stat(pid_t pid, ProcessStat *result)
{
    char path[64];
    char line[LINE_SIZE];

    int written = snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return -1; /* Process may have exited or access may be denied. */
    }

    if (fgets(line, sizeof(line), fp) == NULL) {
        fclose(fp);
        return -1;
    }

    int read_error = ferror(fp);
    fclose(fp);
    if (read_error) {
        return -1;
    }

    char *left = strchr(line, '(');
    char *right = strrchr(line, ')');
    if (left == NULL || right == NULL || right <= left) {
        return -1;
    }

    size_t name_length = (size_t)(right - left - 1);
    if (name_length >= sizeof(result->name)) {
        name_length = sizeof(result->name) - 1;
    }
    memcpy(result->name, left + 1, name_length);
    result->name[name_length] = '\0';

    unsigned long long utime = 0;
    unsigned long long stime = 0;
    unsigned long long starttime = 0;

    char *saveptr = NULL;
    char *token = strtok_r(right + 1, " \t\n", &saveptr);
    int token_index = 0;
    int got_utime = 0, got_stime = 0, got_starttime = 0;

    while (token != NULL) {
        char *endptr = NULL;
        errno = 0;

        if (token_index == 11 || token_index == 12 || token_index == 19) {
            unsigned long long value = strtoull(token, &endptr, 10);
            if (errno != 0 || endptr == token || *endptr != '\0') {
                return -1;
            }

            if (token_index == 11) {
                utime = value;
                got_utime = 1;
            } else if (token_index == 12) {
                stime = value;
                got_stime = 1;
            } else {
                starttime = value;
                got_starttime = 1;
            }
        }

        token_index++;
        token = strtok_r(NULL, " \t\n", &saveptr);
    }

    if (!got_utime || !got_stime || !got_starttime) {
        return -1;
    }

    result->pid = pid;
    result->ticks = utime + stime;
    result->starttime = starttime;
    return 0;
}

/*
 * Collect currently visible process snapshots.
 * Returns a heap array (caller frees it), and sets *count.
 * On failure returns NULL.
 */
static ProcessStat *collect_processes(size_t *count)
{
    *count = 0;
    size_t capacity = 128;

    ProcessStat *list = malloc(capacity * sizeof(*list));
    if (list == NULL) {
        perror("malloc process list");
        return NULL;
    }

    DIR *proc = opendir("/proc");
    if (proc == NULL) {
        perror("opendir /proc");
        free(list);
        return NULL;
    }

    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL) {
        if (!isdigit((unsigned char)entry->d_name[0])) {
            continue;
        }

        errno = 0;
        char *endptr = NULL;
        long value = strtol(entry->d_name, &endptr, 10);

        if (errno != 0 || endptr == entry->d_name || *endptr != '\0' ||
            value <= 0 || value > 2147483647L) {
            continue;
        }

        ProcessStat item;
        if (read_process_stat((pid_t)value, &item) != 0) {
            continue; /* Normal race: process exited during scan. */
        }

        if (*count == capacity) {
            if (capacity > (size_t)-1 / 2 / sizeof(*list)) {
                fprintf(stderr, "Process list capacity overflow.\n");
                free(list);
                closedir(proc);
                return NULL;
            }

            size_t new_capacity = capacity * 2;
            ProcessStat *new_list =
                realloc(list, new_capacity * sizeof(*list));

            if (new_list == NULL) {
                perror("realloc process list");
                free(list);
                closedir(proc);
                return NULL;
            }

            list = new_list;
            capacity = new_capacity;
        }

        list[(*count)++] = item;
    }

    if (closedir(proc) != 0) {
        perror("closedir /proc");
    }

    return list;
}

static const ProcessStat *find_previous(const ProcessStat *previous,
                                        size_t previous_count,
                                        const ProcessStat *current)
{
    for (size_t i = 0; i < previous_count; i++) {
        if (previous[i].pid == current->pid &&
            previous[i].starttime == current->starttime) {
            return &previous[i];
        }
    }
    return NULL;
}

static double elapsed_seconds(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1e9;
}

static int compare_usage(const void *a, const void *b)
{
    const ProcessUsage *x = a;
    const ProcessUsage *y = b;

    if (x->utilization < y->utilization) return 1;
    if (x->utilization > y->utilization) return -1;
    return 0;
}

int main(void)
{
    long ticks_per_second = sysconf(_SC_CLK_TCK);
    if (ticks_per_second <= 0) {
        fprintf(stderr, "Could not determine _SC_CLK_TCK.\n");
        return EXIT_FAILURE;
    }

    printf("========================================\n");
    printf(" Adaptive CPU Affinity Manager\n");
    printf(" Module: Process Monitoring\n");
    printf("========================================\n");

    size_t previous_count = 0;
    ProcessStat *previous = collect_processes(&previous_count);
    if (previous == NULL) {
        return EXIT_FAILURE;
    }

    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        perror("clock_gettime");
        free(previous);
        return EXIT_FAILURE;
    }

    printf("Collecting process CPU usage...\n");
    printf("Sampling interval: approximately 1 second\n");
    printf("CPU usage is normalized to one logical CPU = about 100%%.\n");
    printf("Press Ctrl+C to stop.\n");

    while (1) {
        sleep(1);

        size_t current_count = 0;
        ProcessStat *current = collect_processes(&current_count);
        if (current == NULL) {
            fprintf(stderr, "Could not collect process snapshot; retrying.\n");
            continue;
        }

        struct timespec end;
        if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
            perror("clock_gettime");
            free(current);
            free(previous);
            return EXIT_FAILURE;
        }

        double elapsed = elapsed_seconds(start, end);
        if (elapsed <= 0.0) {
            free(current);
            continue;
        }

        ProcessUsage *usage = NULL;
        if (current_count > 0) {
            usage = malloc(current_count * sizeof(*usage));
            if (usage == NULL) {
                perror("malloc usage list");
                free(current);
                free(previous);
                return EXIT_FAILURE;
            }
        }

        size_t usage_count = 0;
        for (size_t i = 0; i < current_count; i++) {
            const ProcessStat *old =
                find_previous(previous, previous_count, &current[i]);

            /* New process, reused PID, or reset CPU counter: no valid delta. */
            if (old == NULL || current[i].ticks < old->ticks) {
                continue;
            }

            unsigned long long delta_ticks =
                current[i].ticks - old->ticks;

            double cpu_percent = 100.0 * (double)delta_ticks /
                ((double)ticks_per_second * elapsed);

            usage[usage_count].pid = current[i].pid;
            snprintf(usage[usage_count].name,
                     sizeof(usage[usage_count].name), "%s", current[i].name);
            usage[usage_count].utilization = cpu_percent;
            usage_count++;
        }

        /* Avoid passing a NULL base to qsort when there are no samples. */
        if (usage_count > 1) {
            qsort(usage, usage_count, sizeof(*usage), compare_usage);
        }

        printf("\nTop CPU-consuming processes\n");
        printf("--------------------------------------------------\n");
        printf("%-8s %-24s %12s\n", "PID", "PROCESS", "CPU Usage");
        printf("--------------------------------------------------\n");

        size_t display_count =
            usage_count < MAX_DISPLAY ? usage_count : MAX_DISPLAY;

        for (size_t i = 0; i < display_count; i++) {
            printf("%-8ld %-24.24s %10.2f%%\n",
                   (long)usage[i].pid,
                   usage[i].name,
                   usage[i].utilization);
        }

        if (usage_count == 0) {
            printf("No processes had a valid pair of samples yet.\n");
        }

        printf("--------------------------------------------------\n");
        fflush(stdout);

        free(usage);
        free(previous);
        previous = current;
        previous_count = current_count;
        start = end;
    }

    /* Reached only if the loop is changed to terminate normally. */
    free(previous);
    return EXIT_SUCCESS;
}
