
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>

#define LINE_SIZE 4096
#define NAME_SIZE 256
#define INITIAL_CAPACITY 256
#define MAX_DISPLAY 20

/*
 * Snapshot of one Linux thread (task).
 *
 * Linux exposes each thread at /proc/<PID>/task/<TID>/stat.
 * starttime helps distinguish a thread from a later thread
 * that reuses the same TID.
 */
typedef struct {
    pid_t pid;
    pid_t tid;
    char name[NAME_SIZE];
    unsigned long long ticks;
    unsigned long long starttime;
} ThreadStat;

typedef struct {
    pid_t pid;
    pid_t tid;
    char name[NAME_SIZE];
    double utilization;
} ThreadUsage;

/* Parse a positive numeric /proc directory name. */
static int parse_id(const char *text, pid_t *id_out)
{
    if (text == NULL || *text == '\0') {
        return -1;
    }

    for (const unsigned char *p = (const unsigned char *)text;
         *p; p++) {
        if (!isdigit(*p)) {
            return -1;
        }
    }

    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' ||
        value <= 0 || value > INT_MAX) {
        return -1;
    }

    *id_out = (pid_t)value;
    return 0;
}

/*
 * Read one thread's stat file.
 *
 * The comm/name field can contain spaces or ')' characters,
 * so parse after the final ')'.
 *
 * After ')', the first token is field 3 (state):
 * utime = field 14 -> token index 11
 * stime = field 15 -> token index 12
 * starttime = field 22 -> token index 19
 *
 * Returns 0 on success, -1 if the record is invalid or the
 * thread disappeared during collection.
 */
static int read_thread_stat(pid_t pid, pid_t tid,
                            ThreadStat *result)
{
    char path[128];
    char line[LINE_SIZE];

    int length = snprintf(
        path, sizeof(path),
        "/proc/%ld/task/%ld/stat",
        (long)pid, (long)tid
    );

    if (length < 0 || (size_t)length >= sizeof(path)) {
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
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

    int got_utime = 0;
    int got_stime = 0;
    int got_starttime = 0;

    char *saveptr = NULL;
    char *token = strtok_r(right + 1, " \t\n", &saveptr);
    int token_index = 0;

    while (token != NULL) {
        if (token_index == 11 ||
            token_index == 12 ||
            token_index == 19) {

            errno = 0;
            char *end = NULL;

            unsigned long long value =
                strtoull(token, &end, 10);

            if (errno != 0 || end == token || *end != '\0') {
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

    if (!got_utime || !got_stime || !got_starttime ||
        ULLONG_MAX - utime < stime) {
        return -1;
    }

    result->pid = pid;
    result->tid = tid;
    result->ticks = utime + stime;
    result->starttime = starttime;

    return 0;
}

/* Grow the thread snapshot array safely. */
static int append_thread(ThreadStat **items,
                         size_t *count,
                         size_t *capacity,
                         const ThreadStat *item)
{
    if (*count == *capacity) {
        if (*capacity > (size_t)-1 / 2 / sizeof(**items)) {
            fprintf(stderr, "Thread list capacity overflow.\n");
            return -1;
        }

        size_t new_capacity = *capacity * 2;

        ThreadStat *new_items = realloc(
            *items, new_capacity * sizeof(**items)
        );

        if (new_items == NULL) {
            perror("realloc thread list");
            return -1;
        }

        *items = new_items;
        *capacity = new_capacity;
    }

    (*items)[(*count)++] = *item;
    return 0;
}

/*
 * Enumerate /proc/<PID>/task/<TID>/stat for visible threads.
 *
 * Returns a heap array; caller owns it.
 * NULL means a collection-level error.
 * Individual process/thread disappearance is expected.
 */
static ThreadStat *collect_threads(size_t *count)
{
    *count = 0;
    size_t capacity = INITIAL_CAPACITY;

    ThreadStat *items = malloc(capacity * sizeof(*items));

    if (items == NULL) {
        perror("malloc thread list");
        return NULL;
    }

    DIR *proc = opendir("/proc");

    if (proc == NULL) {
        perror("opendir /proc");
        free(items);
        return NULL;
    }

    struct dirent *process_entry;

    while ((process_entry = readdir(proc)) != NULL) {
        pid_t pid;

        if (parse_id(process_entry->d_name, &pid) != 0) {
            continue;
        }

        char task_path[64];

        int length = snprintf(
            task_path, sizeof(task_path),
            "/proc/%ld/task", (long)pid
        );

        if (length < 0 ||
            (size_t)length >= sizeof(task_path)) {
            continue;
        }

        DIR *task_dir = opendir(task_path);

        if (task_dir == NULL) {
            continue;
        }

        struct dirent *thread_entry;

        while ((thread_entry = readdir(task_dir)) != NULL) {
            pid_t tid;

            if (parse_id(thread_entry->d_name, &tid) != 0) {
                continue;
            }

            ThreadStat item;

            if (read_thread_stat(pid, tid, &item) != 0) {
                continue;
            }

            if (append_thread(&items, count,
                              &capacity, &item) != 0) {
                closedir(task_dir);
                closedir(proc);
                free(items);
                return NULL;
            }
        }

        if (closedir(task_dir) != 0) {
            perror("closedir task directory");
        }
    }

    if (closedir(proc) != 0) {
        perror("closedir /proc");
    }

    return items;
}

/*
 * Match a thread between samples using PID, TID, and starttime.
 * This reduces errors caused by PID/TID reuse.
 */
static const ThreadStat *find_previous(
    const ThreadStat *previous,
    size_t previous_count,
    const ThreadStat *current)
{
    for (size_t i = 0; i < previous_count; i++) {
        if (previous[i].pid == current->pid &&
            previous[i].tid == current->tid &&
            previous[i].starttime == current->starttime) {
            return &previous[i];
        }
    }

    return NULL;
}

static double elapsed_seconds(struct timespec start,
                              struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1e9;
}

static int compare_usage(const void *a, const void *b)
{
    const ThreadUsage *x = a;
    const ThreadUsage *y = b;

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
    printf(" Module: Thread Monitoring\n");
    printf("========================================\n");

    size_t previous_count = 0;

    ThreadStat *previous =
        collect_threads(&previous_count);

    if (previous == NULL) {
        return EXIT_FAILURE;
    }

    struct timespec start;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        perror("clock_gettime");
        free(previous);
        return EXIT_FAILURE;
    }

    printf("Visible threads in initial snapshot: %zu\n",
           previous_count);
    printf("Sampling interval: approximately 1 second\n");
    printf("CPU usage: one logical CPU = about 100%%.\n");
    printf("A multithreaded process can total more than 100%%.\n");
    printf("Press Ctrl+C to stop.\n");

    while (1) {
        sleep(1);

        size_t current_count = 0;

        ThreadStat *current =
            collect_threads(&current_count);

        if (current == NULL) {
            fprintf(stderr,
                    "Could not collect thread snapshot; retrying.\n");
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

        ThreadUsage *usage = NULL;

        if (current_count > 0) {
            if (current_count > (size_t)-1 / sizeof(*usage)) {
                fprintf(stderr,
                        "Thread usage array size overflow.\n");
                free(current);
                free(previous);
                return EXIT_FAILURE;
            }

            usage = malloc(current_count * sizeof(*usage));

            if (usage == NULL) {
                perror("malloc thread usage list");
                free(current);
                free(previous);
                return EXIT_FAILURE;
            }
        }

        size_t usage_count = 0;

        for (size_t i = 0; i < current_count; i++) {
            const ThreadStat *old =
                find_previous(previous, previous_count,
                              &current[i]);

            /*
             * New thread, reused TID, or counter anomaly:
             * wait for a valid pair of samples.
             */
            if (old == NULL ||
                current[i].ticks < old->ticks) {
                continue;
            }

            unsigned long long delta_ticks =
                current[i].ticks - old->ticks;

            double cpu_percent =
                100.0 * (double)delta_ticks /
                ((double)ticks_per_second * elapsed);

            usage[usage_count].pid = current[i].pid;
            usage[usage_count].tid = current[i].tid;

            snprintf(usage[usage_count].name,
                     sizeof(usage[usage_count].name),
                     "%s", current[i].name);

            usage[usage_count].utilization = cpu_percent;
            usage_count++;
        }

        if (usage_count > 1) {
            qsort(usage, usage_count,
                  sizeof(*usage), compare_usage);
        }

        printf("\nTop CPU-consuming threads\n");
        printf("----------------------------------------------------------------------------\n");
        printf("%-8s %-8s %-24s %12s\n",
               "PID", "TID", "THREAD", "CPU Usage");
        printf("----------------------------------------------------------------------------\n");

        size_t display_count =
            usage_count < MAX_DISPLAY
                ? usage_count : MAX_DISPLAY;

        for (size_t i = 0; i < display_count; i++) {
            printf("%-8ld %-8ld %-24.24s %10.2f%%\n",
                   (long)usage[i].pid,
                   (long)usage[i].tid,
                   usage[i].name,
                   usage[i].utilization);
        }

        if (usage_count == 0) {
            printf("No threads had a valid pair of samples yet.\n");
        }

        printf("----------------------------------------------------------------------------\n");
        printf("Threads in current snapshot: %zu | valid usage samples: %zu\n",
               current_count, usage_count);

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
