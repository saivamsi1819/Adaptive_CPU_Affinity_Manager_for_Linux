#define _POSIX_C_SOURCE 200809L

#include "monitor_data.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#define PROC_LINE_SIZE 4096
#define CPU_LINE_SIZE 512
#define INITIAL_CAPACITY 128

int monitor_compute_thread_utilization(unsigned long long delta_ticks,
                                      double elapsed_seconds,
                                      long ticks_per_second,
                                      double *utilization_percent)
{
    if (utilization_percent == NULL ||
        !isfinite(elapsed_seconds) || elapsed_seconds <= 0.0 ||
        ticks_per_second <= 0) {
        errno = EINVAL;
        return -1;
    }

    double tick_window = (double)ticks_per_second * elapsed_seconds;
    if (!isfinite(tick_window) || tick_window <= 0.0) {
        errno = ERANGE;
        return -1;
    }

    double usage = 100.0 * (double)delta_ticks / tick_window;
    double one_tick_tolerance = 100.0 / tick_window;

    if (!isfinite(usage) || !isfinite(one_tick_tolerance) ||
        usage < 0.0 || usage > 100.0 + one_tick_tolerance + 1e-9) {
        errno = ERANGE;
        return -1;
    }

    *utilization_percent = usage > 100.0 ? 100.0 : usage;
    return 0;
}

static int valid_timespec(struct timespec value)
{
    return value.tv_sec >= 0 &&
           value.tv_nsec >= 0 &&
           value.tv_nsec < 1000000000L;
}

static double seconds_between(struct timespec earlier,
                              struct timespec later)
{
    if (!valid_timespec(earlier) ||
        !valid_timespec(later) ||
        later.tv_sec < earlier.tv_sec ||
        (later.tv_sec == earlier.tv_sec &&
         later.tv_nsec <= earlier.tv_nsec)) {
        return 0.0;
    }

    return (double)(later.tv_sec - earlier.tv_sec) +
           (double)(later.tv_nsec - earlier.tv_nsec) / 1e9;
}

static int snapshot_is_empty(const MonitorSnapshot *snapshot)
{
    return snapshot != NULL &&
           snapshot->cpus == NULL &&
           snapshot->cpu_count == 0 &&
           snapshot->processes == NULL &&
           snapshot->process_count == 0 &&
           snapshot->threads == NULL &&
           snapshot->thread_count == 0;
}

void monitor_snapshot_init(MonitorSnapshot *snapshot)
{
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
}

void monitor_snapshot_destroy(MonitorSnapshot *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    free(snapshot->cpus);
    free(snapshot->processes);
    free(snapshot->threads);

    monitor_snapshot_init(snapshot);
}

static int parse_positive_id(const char *text, pid_t *out)
{
    if (text == NULL || *text == '\0') {
        return -1;
    }

    for (const unsigned char *p = (const unsigned char *)text;
         *p;
         ++p) {
        if (!isdigit(*p)) {
            return -1;
        }
    }

    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);

    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        value <= 0 ||
        value > INT_MAX) {
        return -1;
    }

    *out = (pid_t)value;
    return 0;
}

static int parse_stat_file(const char *path,
                           char *name,
                           size_t name_size,
                           unsigned long long *ticks,
                           unsigned long long *start_ticks,
                           int *last_cpu)
{
    char line[PROC_LINE_SIZE];

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

    /*
     * The command name in /proc/<pid>/stat is enclosed in parentheses.
     * Use the final ')' because the command name itself can contain ')'.
     */
    char *left = strchr(line, '(');
    char *right = strrchr(line, ')');

    if (left == NULL || right == NULL || right <= left) {
        return -1;
    }

    size_t n = (size_t)(right - left - 1);

    if (n >= name_size) {
        n = name_size - 1;
    }

    memcpy(name, left + 1, n);
    name[n] = '\0';

    unsigned long long utime = 0;
    unsigned long long stime = 0;
    unsigned long long start = 0;

    int got_u = 0;
    int got_s = 0;
    int got_start = 0;
    int got_cpu = 0;
    int processor = -1;

    char *save = NULL;
    char *token = strtok_r(right + 1, " \t\n", &save);
    int index = 0;

    /*
     * Tokens start at field 3 (state) in /proc/<pid>/stat:
     * utime = field 14 -> token index 11
     * stime = field 15 -> token index 12
     * starttime = field 22 -> token index 19
     * processor = field 39 -> token index 36
     */
    while (token != NULL) {
        if (index == 11 ||
            index == 12 ||
            index == 19 ||
            index == 36) {
            errno = 0;
            char *end = NULL;
            unsigned long long value = strtoull(token, &end, 10);

            if (errno != 0 || end == token || *end != '\0') {
                return -1;
            }

            if (index == 11) {
                utime = value;
                got_u = 1;
            } else if (index == 12) {
                stime = value;
                got_s = 1;
            } else if (index == 19) {
                start = value;
                got_start = 1;
            } else if (value <= INT_MAX) {
                processor = (int)value;
                got_cpu = 1;
            }
        }

        ++index;
        token = strtok_r(NULL, " \t\n", &save);
    }

    if (!got_u ||
        !got_s ||
        !got_start ||
        ULLONG_MAX - utime < stime) {
        return -1;
    }

    *ticks = utime + stime;
    *start_ticks = start;

    if (last_cpu != NULL) {
        *last_cpu = got_cpu ? processor : -1;
    }

    return 0;
}

static const ProcessSample *find_process(
    const MonitorSnapshot *snapshot,
    const ProcessSample *current)
{
    for (size_t i = 0; i < snapshot->process_count; ++i) {
        const ProcessSample *old = &snapshot->processes[i];

        if (old->pid == current->pid &&
            old->start_ticks == current->start_ticks) {
            return old;
        }
    }

    return NULL;
}

static const ThreadSample *find_thread(
    const MonitorSnapshot *snapshot,
    const ThreadSample *current)
{
    for (size_t i = 0; i < snapshot->thread_count; ++i) {
        const ThreadSample *old = &snapshot->threads[i];

        if (old->pid == current->pid &&
            old->tid == current->tid &&
            old->start_ticks == current->start_ticks) {
            return old;
        }
    }

    return NULL;
}

static int append_process(MonitorSnapshot *snapshot,
                          const ProcessSample *item,
                          size_t *capacity)
{
    if (snapshot->process_count == *capacity) {
        size_t next = *capacity
                    ? *capacity * 2
                    : INITIAL_CAPACITY;

        if (next < *capacity ||
            next > (size_t)-1 / sizeof(*snapshot->processes)) {
            return -1;
        }

        void *memory = realloc(
            snapshot->processes,
            next * sizeof(*snapshot->processes));

        if (memory == NULL) {
            return -1;
        }

        snapshot->processes = memory;
        *capacity = next;
    }

    snapshot->processes[snapshot->process_count++] = *item;
    return 0;
}

static int append_thread(MonitorSnapshot *snapshot,
                         const ThreadSample *item,
                         size_t *capacity)
{
    if (snapshot->thread_count == *capacity) {
        size_t next = *capacity
                    ? *capacity * 2
                    : INITIAL_CAPACITY;

        if (next < *capacity ||
            next > (size_t)-1 / sizeof(*snapshot->threads)) {
            return -1;
        }

        void *memory = realloc(
            snapshot->threads,
            next * sizeof(*snapshot->threads));

        if (memory == NULL) {
            return -1;
        }

        snapshot->threads = memory;
        *capacity = next;
    }

    snapshot->threads[snapshot->thread_count++] = *item;
    return 0;
}

static MonitorStatus collect_cpus(const MonitorSnapshot *previous,
                                  MonitorSnapshot *current)
{
    FILE *fp = fopen("/proc/stat", "r");

    if (fp == NULL) {
        return MONITOR_STATUS_ERROR;
    }

    size_t capacity = 0;
    int partial = 0;
    char line[CPU_LINE_SIZE];

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strncmp(line, "cpu", 3) != 0 ||
            !isdigit((unsigned char)line[3])) {
            continue;
        }

        unsigned int id;
        unsigned long long user;
        unsigned long long nice;
        unsigned long long system;
        unsigned long long idle;
        unsigned long long iowait;
        unsigned long long irq;
        unsigned long long softirq;
        unsigned long long steal;

        if (sscanf(line,
                   "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu",
                   &id,
                   &user,
                   &nice,
                   &system,
                   &idle,
                   &iowait,
                   &irq,
                   &softirq,
                   &steal) != 9) {
            partial = 1;
            continue;
        }

        if (current->cpu_count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;

            if (next < capacity ||
                next > (size_t)-1 / sizeof(*current->cpus)) {
                fclose(fp);
                return MONITOR_STATUS_ERROR;
            }

            void *memory = realloc(
                current->cpus,
                next * sizeof(*current->cpus));

            if (memory == NULL) {
                fclose(fp);
                return MONITOR_STATUS_ERROR;
            }

            current->cpus = memory;
            capacity = next;
        }

        CpuSample *cpu = &current->cpus[current->cpu_count++];
        memset(cpu, 0, sizeof(*cpu));

        cpu->cpu_id = (int)id;
        cpu->total_ticks =
            user + nice + system + idle +
            iowait + irq + softirq + steal;
        cpu->idle_ticks = idle + iowait;
        cpu->counters_valid = 1;

        if (previous != NULL) {
            const CpuSample *old = NULL;

            for (size_t i = 0; i < previous->cpu_count; ++i) {
                if (previous->cpus[i].cpu_id == cpu->cpu_id) {
                    old = &previous->cpus[i];
                    break;
                }
            }

            if (old != NULL &&
                old->counters_valid &&
                cpu->total_ticks >= old->total_ticks &&
                cpu->idle_ticks >= old->idle_ticks) {
                unsigned long long delta_total =
                    cpu->total_ticks - old->total_ticks;
                unsigned long long delta_idle =
                    cpu->idle_ticks - old->idle_ticks;

                if (delta_total > 0 && delta_idle <= delta_total) {
                    cpu->utilization_percent =
                        100.0 *
                        (1.0 - (double)delta_idle /
                               (double)delta_total);

                    if (cpu->utilization_percent < 0.0) {
                        cpu->utilization_percent = 0.0;
                    }

                    if (cpu->utilization_percent > 100.0) {
                        cpu->utilization_percent = 100.0;
                    }

                    cpu->utilization_valid = 1;
                }
            }
        }
    }

    if (ferror(fp)) {
        fclose(fp);
        return MONITOR_STATUS_ERROR;
    }

    fclose(fp);

    CpuUtilizationSummary *summary = &current->cpu_summary;
    double total = 0.0;
    double minimum = 100.0;
    double maximum = 0.0;

    for (size_t i = 0; i < current->cpu_count; ++i) {
        CpuSample *cpu = &current->cpus[i];

        if (!cpu->utilization_valid) {
            continue;
        }

        if (cpu->utilization_percent < minimum) {
            minimum = cpu->utilization_percent;
        }

        if (cpu->utilization_percent > maximum) {
            maximum = cpu->utilization_percent;
        }

        total += cpu->utilization_percent;
        summary->valid_cpu_count++;
    }

    if (summary->valid_cpu_count > 0) {
        summary->average_percent =
            total / (double)summary->valid_cpu_count;
        summary->minimum_percent = minimum;
        summary->maximum_percent = maximum;
        summary->imbalance_percent_points = maximum - minimum;
        summary->valid = 1;
    }

    if (current->cpu_count == 0) {
        return MONITOR_STATUS_ERROR;
    }

    if (partial || (previous != NULL && !summary->valid)) {
        return MONITOR_STATUS_PARTIAL;
    }

    return MONITOR_STATUS_OK;
}

static MonitorStatus collect_tasks(const MonitorSnapshot *previous,
                                   MonitorSnapshot *current,
                                   long ticks_per_sec,
                                   int threads)
{
    DIR *proc = opendir("/proc");

    if (proc == NULL) {
        return MONITOR_STATUS_ERROR;
    }

    int partial = 0;
    size_t capacity = 0;
    struct dirent *proc_entry;

    while ((proc_entry = readdir(proc)) != NULL) {
        pid_t pid;

        if (parse_positive_id(proc_entry->d_name, &pid) != 0) {
            continue;
        }

        if (!threads) {
            char path[128];
            char name[MONITOR_NAME_MAX];
            unsigned long long ticks;
            unsigned long long start;
            int last_cpu = -1;

            int length = snprintf(
                path, sizeof(path), "/proc/%ld/stat", (long)pid);

            if (length < 0 ||
                (size_t)length >= sizeof(path) ||
                parse_stat_file(path,
                                name,
                                sizeof(name),
                                &ticks,
                                &start,
                                &last_cpu) != 0) {
                partial = 1;
                continue;
            }

            struct timespec task_sampled_at;

            if (clock_gettime(CLOCK_MONOTONIC,
                              &task_sampled_at) != 0) {
                partial = 1;
                continue;
            }

            ProcessSample item = {0};

            item.pid = pid;
            item.start_ticks = start;
            item.cpu_time_ticks = ticks;
            item.sampled_at = task_sampled_at;

            snprintf(item.name, sizeof(item.name), "%s", name);

            if (previous != NULL) {
                const ProcessSample *old =
                    find_process(previous, &item);

                if (old != NULL &&
                    ticks >= old->cpu_time_ticks) {
                    double elapsed =
                        seconds_between(old->sampled_at,
                                        item.sampled_at);

                    if (elapsed > 0.0) {
                        double usage =
                            100.0 *
                            (double)(ticks - old->cpu_time_ticks) /
                            ((double)ticks_per_sec * elapsed);

                        /*
                         * A multithreaded process can use more than
                         * 100% of one CPU in aggregate.
                         */
                        if (isfinite(usage) &&
                            usage >= 0.0 &&
                            usage < DBL_MAX) {
                            item.utilization_percent = usage;
                            item.utilization_valid = 1;
                        }
                    }
                }
            }

            if (append_process(current, &item, &capacity) != 0) {
                closedir(proc);
                return MONITOR_STATUS_ERROR;
            }
        } else {
            char task_path[128];

            int length = snprintf(
                task_path,
                sizeof(task_path),
                "/proc/%ld/task",
                (long)pid);

            if (length < 0 ||
                (size_t)length >= sizeof(task_path)) {
                partial = 1;
                continue;
            }

            DIR *task_dir = opendir(task_path);

            if (task_dir == NULL) {
                partial = 1;
                continue;
            }

            struct dirent *task_entry;

            while ((task_entry = readdir(task_dir)) != NULL) {
                pid_t tid;

                if (parse_positive_id(task_entry->d_name, &tid) != 0) {
                    continue;
                }

                char path[160];
                char name[MONITOR_NAME_MAX];
                unsigned long long ticks;
                unsigned long long start;
                int last_cpu = -1;

                length = snprintf(
                    path,
                    sizeof(path),
                    "/proc/%ld/task/%ld/stat",
                    (long)pid,
                    (long)tid);

                if (length < 0 ||
                    (size_t)length >= sizeof(path) ||
                    parse_stat_file(path,
                                    name,
                                    sizeof(name),
                                    &ticks,
                                    &start,
                                    &last_cpu) != 0) {
                    partial = 1;
                    continue;
                }

                struct timespec task_sampled_at;

                if (clock_gettime(CLOCK_MONOTONIC,
                                  &task_sampled_at) != 0) {
                    partial = 1;
                    continue;
                }

                ThreadSample item = {0};

                item.pid = pid;
                item.tid = tid;
                item.start_ticks = start;
                item.last_cpu = last_cpu;
                item.cpu_time_ticks = ticks;
                item.sampled_at = task_sampled_at;

                snprintf(item.name, sizeof(item.name), "%s", name);

                if (previous != NULL) {
                    const ThreadSample *old =
                        find_thread(previous, &item);

                    if (old != NULL &&
                        ticks >= old->cpu_time_ticks) {
                        double elapsed =
                            seconds_between(old->sampled_at,
                                            item.sampled_at);

                        if (elapsed > 0.0) {
                            double usage = 0.0;

                            /*
                             * A one-tick overshoot can occur when the two
                             * procfs reads straddle CPU-accounting tick edges.
                             * Normalize that quantization noise instead of
                             * dropping a genuinely busy thread from selection.
                             */
                            if (monitor_compute_thread_utilization(
                                    ticks - old->cpu_time_ticks,
                                    elapsed, ticks_per_sec, &usage) == 0) {
                                item.utilization_percent = usage;
                                item.utilization_valid = 1;
                            }
                        }
                    }
                }

                if (append_thread(current,
                                  &item,
                                  &capacity) != 0) {
                    closedir(task_dir);
                    closedir(proc);
                    return MONITOR_STATUS_ERROR;
                }
            }

            if (closedir(task_dir) != 0) {
                partial = 1;
            }
        }
    }

    /*
     * readdir() signals end-of-directory with NULL.
     * DIR does not provide an ferror() equivalent.
     */
    if (closedir(proc) != 0) {
        partial = 1;
    }

    if ((!threads && current->process_count == 0) ||
        (threads && current->thread_count == 0)) {
        return MONITOR_STATUS_PARTIAL;
    }

    return partial ? MONITOR_STATUS_PARTIAL : MONITOR_STATUS_OK;
}

MonitorStatus monitor_collect_snapshot(const MonitorSnapshot *previous,
                                       MonitorSnapshot *current)
{
    if (current == NULL || current == previous) {
        errno = EINVAL;
        return MONITOR_STATUS_ERROR;
    }

    if (!snapshot_is_empty(current)) {
        errno = EBUSY;
        return MONITOR_STATUS_ERROR;
    }

    monitor_snapshot_init(current);

    if (clock_gettime(CLOCK_MONOTONIC, &current->sampled_at) != 0) {
        return MONITOR_STATUS_ERROR;
    }

    long ticks_per_sec = sysconf(_SC_CLK_TCK);

    if (ticks_per_sec <= 0) {
        return MONITOR_STATUS_ERROR;
    }

    current->cpu_status = collect_cpus(previous, current);

    if (current->cpu_status == MONITOR_STATUS_ERROR) {
        goto fail;
    }

    current->process_status =
        collect_tasks(previous, current, ticks_per_sec, 0);

    if (current->process_status == MONITOR_STATUS_ERROR) {
        goto fail;
    }

    current->thread_status =
        collect_tasks(previous, current, ticks_per_sec, 1);

    if (current->thread_status == MONITOR_STATUS_ERROR) {
        goto fail;
    }

    if (current->cpu_status == MONITOR_STATUS_PARTIAL ||
        current->process_status == MONITOR_STATUS_PARTIAL ||
        current->thread_status == MONITOR_STATUS_PARTIAL) {
        return MONITOR_STATUS_PARTIAL;
    }

    return MONITOR_STATUS_OK;

fail:
    monitor_snapshot_destroy(current);
    return MONITOR_STATUS_ERROR;
}