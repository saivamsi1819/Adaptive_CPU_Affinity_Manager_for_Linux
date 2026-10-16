#define _POSIX_C_SOURCE 200809L

#include "monitor_data.h"

#include <ctype.h>
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

static double seconds_between(struct timespec a, struct timespec b)
{
    return (double)(b.tv_sec - a.tv_sec) +
           (double)(b.tv_nsec - a.tv_nsec) / 1e9;
}

void monitor_snapshot_init(MonitorSnapshot *snapshot)
{
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
}

void monitor_snapshot_destroy(MonitorSnapshot *snapshot)
{
    if (snapshot == NULL) return;
    free(snapshot->cpus);
    free(snapshot->processes);
    free(snapshot->threads);
    monitor_snapshot_init(snapshot);
}

static int parse_positive_id(const char *text, pid_t *out)
{
    if (text == NULL || *text == '\0') return -1;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (!isdigit(*p)) return -1;
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno || end == text || *end || value <= 0 || value > INT_MAX)
        return -1;
    *out = (pid_t)value;
    return 0;
}

static int parse_stat_file(const char *path, char *name, size_t name_size,
                           unsigned long long *ticks,
                           unsigned long long *start_ticks)
{
    char line[PROC_LINE_SIZE];
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -1;
    }
    int read_error = ferror(fp);
    fclose(fp);
    if (read_error) return -1;

    char *left = strchr(line, '(');
    char *right = strrchr(line, ')');
    if (!left || !right || right <= left) return -1;

    size_t n = (size_t)(right - left - 1);
    if (n >= name_size) n = name_size - 1;
    memcpy(name, left + 1, n);
    name[n] = '\0';

    unsigned long long utime = 0, stime = 0, start = 0;
    int got_u = 0, got_s = 0, got_start = 0;
    char *save = NULL;
    char *token = strtok_r(right + 1, " \t\n", &save);
    int index = 0;
    while (token) {
        if (index == 11 || index == 12 || index == 19) {
            errno = 0;
            char *end = NULL;
            unsigned long long v = strtoull(token, &end, 10);
            if (errno || end == token || *end) return -1;
            if (index == 11) { utime = v; got_u = 1; }
            else if (index == 12) { stime = v; got_s = 1; }
            else { start = v; got_start = 1; }
        }
        ++index;
        token = strtok_r(NULL, " \t\n", &save);
    }
    if (!got_u || !got_s || !got_start || ULLONG_MAX - utime < stime)
        return -1;
    *ticks = utime + stime;
    *start_ticks = start;
    return 0;
}

static const ProcessSample *find_process(const MonitorSnapshot *s,
                                         const ProcessSample *cur)
{
    for (size_t i = 0; i < s->process_count; ++i)
        if (s->processes[i].pid == cur->pid &&
            s->processes[i].start_ticks == cur->start_ticks)
            return &s->processes[i];
    return NULL;
}

static const ThreadSample *find_thread(const MonitorSnapshot *s,
                                       const ThreadSample *cur)
{
    for (size_t i = 0; i < s->thread_count; ++i)
        if (s->threads[i].pid == cur->pid && s->threads[i].tid == cur->tid &&
            s->threads[i].start_ticks == cur->start_ticks)
            return &s->threads[i];
    return NULL;
}

static int append_process(MonitorSnapshot *s, const ProcessSample *item,
                          size_t *capacity)
{
    if (s->process_count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : INITIAL_CAPACITY;
        if (next < *capacity || next > (size_t)-1 / sizeof(*s->processes))
            return -1;
        void *p = realloc(s->processes, next * sizeof(*s->processes));
        if (!p) return -1;
        s->processes = p;
        *capacity = next;
    }
    s->processes[s->process_count++] = *item;
    return 0;
}

static int append_thread(MonitorSnapshot *s, const ThreadSample *item,
                         size_t *capacity)
{
    if (s->thread_count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : INITIAL_CAPACITY;
        if (next < *capacity || next > (size_t)-1 / sizeof(*s->threads))
            return -1;
        void *p = realloc(s->threads, next * sizeof(*s->threads));
        if (!p) return -1;
        s->threads = p;
        *capacity = next;
    }
    s->threads[s->thread_count++] = *item;
    return 0;
}

static MonitorStatus collect_cpus(const MonitorSnapshot *previous,
                                  MonitorSnapshot *current)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return MONITOR_STATUS_ERROR;
    size_t cap = 0;
    int partial = 0;
    char line[CPU_LINE_SIZE];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu", 3) != 0 ||
            !isdigit((unsigned char)line[3])) continue;
        unsigned int id;
        unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
        if (sscanf(line, "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu",
                   &id, &user, &nice, &system, &idle, &iowait, &irq,
                   &softirq, &steal) != 9) { partial = 1; continue; }
        if (current->cpu_count == cap) {
            size_t next = cap ? cap * 2 : 16;
            if (next < cap || next > (size_t)-1 / sizeof(*current->cpus)) {
                fclose(fp); return MONITOR_STATUS_ERROR;
            }
            void *p = realloc(current->cpus, next * sizeof(*current->cpus));
            if (!p) { fclose(fp); return MONITOR_STATUS_ERROR; }
            current->cpus = p; cap = next;
        }
        CpuSample *c = &current->cpus[current->cpu_count++];
        memset(c, 0, sizeof(*c));
        c->cpu_id = (int)id;
        c->total_ticks = user + nice + system + idle + iowait + irq + softirq + steal;
        c->idle_ticks = idle + iowait;
        c->counters_valid = 1;
        if (previous) {
            const CpuSample *old = NULL;
            for (size_t i = 0; i < previous->cpu_count; ++i)
                if (previous->cpus[i].cpu_id == c->cpu_id) { old = &previous->cpus[i]; break; }
            if (old && old->counters_valid && c->total_ticks >= old->total_ticks &&
                c->idle_ticks >= old->idle_ticks) {
                unsigned long long dt = c->total_ticks - old->total_ticks;
                unsigned long long di = c->idle_ticks - old->idle_ticks;
                if (dt > 0 && di <= dt) {
                    c->utilization_percent = 100.0 * (1.0 - (double)di / (double)dt);
                    if (c->utilization_percent < 0) c->utilization_percent = 0;
                    if (c->utilization_percent > 100) c->utilization_percent = 100;
                    c->utilization_valid = 1;
                }
            }
        }
    }
    if (ferror(fp)) { fclose(fp); return MONITOR_STATUS_ERROR; }
    fclose(fp);

    CpuUtilizationSummary *sum = &current->cpu_summary;
    double total = 0.0, min = 100.0, max = 0.0;
    for (size_t i = 0; i < current->cpu_count; ++i) {
        CpuSample *c = &current->cpus[i];
        if (!c->utilization_valid) continue;
        if (c->utilization_percent < min) min = c->utilization_percent;
        if (c->utilization_percent > max) max = c->utilization_percent;
        total += c->utilization_percent;
        sum->valid_cpu_count++;
    }
    if (sum->valid_cpu_count) {
        sum->average_percent = total / (double)sum->valid_cpu_count;
        sum->minimum_percent = min;
        sum->maximum_percent = max;
        sum->imbalance_percent_points = max - min;
        sum->valid = 1;
    }
    if (!current->cpu_count) return MONITOR_STATUS_ERROR;
    if (partial || (previous && !sum->valid)) return MONITOR_STATUS_PARTIAL;
    return MONITOR_STATUS_OK;
}

static MonitorStatus collect_tasks(const MonitorSnapshot *previous,
                                   MonitorSnapshot *current, long ticks_per_sec,
                                   int threads)
{
    DIR *proc = opendir("/proc");
    if (!proc) return MONITOR_STATUS_ERROR;
    int partial = 0;
    size_t capacity = 0;
    struct dirent *pe;
    while ((pe = readdir(proc)) != NULL) {
        pid_t pid;
        if (parse_positive_id(pe->d_name, &pid) != 0) continue;
        if (!threads) {
            char path[128], name[MONITOR_NAME_MAX];
            unsigned long long ticks, start;
            int n = snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
            if (n < 0 || (size_t)n >= sizeof(path) ||
                parse_stat_file(path, name, sizeof(name), &ticks, &start) != 0) {
                partial = 1; continue;
            }
            ProcessSample item = {0};
            item.pid = pid; item.start_ticks = start; item.cpu_time_ticks = ticks;
            snprintf(item.name, sizeof(item.name), "%s", name);
            if (previous) {
                const ProcessSample *old = find_process(previous, &item);
                if (old && ticks >= old->cpu_time_ticks) {
                    double elapsed = seconds_between(previous->sampled_at, current->sampled_at);
                    if (elapsed > 0.0) {
                        item.utilization_percent = 100.0 * (double)(ticks - old->cpu_time_ticks) /
                                                   ((double)ticks_per_sec * elapsed);
                        item.utilization_valid = 1;
                    }
                }
            }
            if (append_process(current, &item, &capacity) != 0) {
                closedir(proc); return MONITOR_STATUS_ERROR;
            }
        } else {
            char task_path[128];
            int n = snprintf(task_path, sizeof(task_path), "/proc/%ld/task", (long)pid);
            if (n < 0 || (size_t)n >= sizeof(task_path)) { partial = 1; continue; }
            DIR *td = opendir(task_path);
            if (!td) { partial = 1; continue; }
            struct dirent *te;
            while ((te = readdir(td)) != NULL) {
                pid_t tid;
                if (parse_positive_id(te->d_name, &tid) != 0) continue;
                char path[160], name[MONITOR_NAME_MAX];
                unsigned long long ticks, start;
                n = snprintf(path, sizeof(path), "/proc/%ld/task/%ld/stat", (long)pid, (long)tid);
                if (n < 0 || (size_t)n >= sizeof(path) ||
                    parse_stat_file(path, name, sizeof(name), &ticks, &start) != 0) {
                    partial = 1; continue;
                }
                ThreadSample item = {0};
                item.pid = pid; item.tid = tid; item.start_ticks = start;
                item.cpu_time_ticks = ticks;
                snprintf(item.name, sizeof(item.name), "%s", name);
                if (previous) {
                    const ThreadSample *old = find_thread(previous, &item);
                    if (old && ticks >= old->cpu_time_ticks) {
                        double elapsed = seconds_between(previous->sampled_at, current->sampled_at);
                        if (elapsed > 0.0) {
                            item.utilization_percent = 100.0 * (double)(ticks - old->cpu_time_ticks) /
                                                       ((double)ticks_per_sec * elapsed);
                            item.utilization_valid = 1;
                        }
                    }
                }
                if (append_thread(current, &item, &capacity) != 0) {
                    closedir(td); closedir(proc); return MONITOR_STATUS_ERROR;
                }
            }
            if (closedir(td) != 0) partial = 1;
        }
    }
    /* readdir() reports end-of-directory as NULL; DIR has no ferror(). */
    if (closedir(proc) != 0) partial = 1;
    if ((!threads && !current->process_count) || (threads && !current->thread_count))
        return MONITOR_STATUS_PARTIAL;
    return partial ? MONITOR_STATUS_PARTIAL : MONITOR_STATUS_OK;
}

MonitorStatus monitor_collect_snapshot(const MonitorSnapshot *previous,
                                       MonitorSnapshot *current)
{
    if (!current || current == previous) { errno = EINVAL; return MONITOR_STATUS_ERROR; }
    monitor_snapshot_init(current);
    if (clock_gettime(CLOCK_MONOTONIC, &current->sampled_at) != 0)
        return MONITOR_STATUS_ERROR;
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) return MONITOR_STATUS_ERROR;

    current->cpu_status = collect_cpus(previous, current);
    if (current->cpu_status == MONITOR_STATUS_ERROR) goto fail;
    current->process_status = collect_tasks(previous, current, hz, 0);
    if (current->process_status == MONITOR_STATUS_ERROR) goto fail;
    current->thread_status = collect_tasks(previous, current, hz, 1);
    if (current->thread_status == MONITOR_STATUS_ERROR) goto fail;

    if (current->cpu_status == MONITOR_STATUS_PARTIAL ||
        current->process_status == MONITOR_STATUS_PARTIAL ||
        current->thread_status == MONITOR_STATUS_PARTIAL)
        return MONITOR_STATUS_PARTIAL;
    return MONITOR_STATUS_OK;

fail:
    monitor_snapshot_destroy(current);
    return MONITOR_STATUS_ERROR;
}
