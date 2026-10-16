#ifndef MONITOR_DATA_H
#define MONITOR_DATA_H

/*
 * Shared data contract for Adaptive CPU Affinity Manager.
 *
 * This header contains the shared data structures and API used by
 * monitor_data.c. The existing cpu_monitor.c, process_monitor.c, and
 * thread_monitor.c command-line programs remain standalone for now; the
 * shared collector is available to a future controller or diagnostic client.
 *
 * Units:
 * - CPU utilization: percent of one logical CPU, in [0, 100].
 * - Process/thread utilization: percent of one logical CPU; a multithreaded
 *   process can therefore exceed 100 percent.
 * - *_ticks: cumulative kernel CPU-time ticks from /proc, not percentages.
 * - *_start_ticks: process/thread start time from /proc stat, used with IDs to
 *   reduce the risk of confusing a newly reused PID/TID with an old task.
 */

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#define MONITOR_NAME_MAX 256

/* Collection functions return one of these status values. */
typedef enum {
    MONITOR_STATUS_ERROR = -1, /* Collection-level failure; inspect errno/log. */
    MONITOR_STATUS_OK = 0,     /* Collection completed without known omissions. */
    MONITOR_STATUS_PARTIAL = 1 /* Some CPUs/tasks were unavailable or skipped. */
} MonitorStatus;

/* One logical CPU's cumulative counters and derived utilization. */
typedef struct {
    int cpu_id;
    unsigned long long total_ticks;
    unsigned long long idle_ticks; /* Includes iowait for utilization purposes. */
    double utilization_percent;
    int counters_valid;            /* Cumulative counters were read successfully. */
    int utilization_valid;         /* A valid previous/current delta was available. */
} CpuSample;

/* Summary over only CPUs with valid utilization samples. */
typedef struct {
    double average_percent;
    double minimum_percent;
    double maximum_percent;
    double imbalance_percent_points; /* maximum - minimum */
    size_t valid_cpu_count;
    int valid;
} CpuUtilizationSummary;

/* One process-level observation from /proc/<PID>/stat. */
typedef struct {
    pid_t pid;
    char name[MONITOR_NAME_MAX];
    unsigned long long start_ticks;
    unsigned long long cpu_time_ticks; /* utime + stime */
    double utilization_percent;
    int utilization_valid;
} ProcessSample;

/* One Linux task/thread observation from /proc/<PID>/task/<TID>/stat. */
typedef struct {
    pid_t pid; /* Thread-group/process ID. */
    pid_t tid; /* Linux task ID; affinity operations target a task/TID. */
    int last_cpu; /* Last CPU reported by /proc/<pid>/task/<tid>/stat; -1 if unknown. */
    char name[MONITOR_NAME_MAX];
    unsigned long long start_ticks;
    unsigned long long cpu_time_ticks; /* utime + stime */
    double utilization_percent;
    int utilization_valid;
} ThreadSample;

/*
 * One monitoring epoch. Arrays are owned by the snapshot after collection;
 * callers must release them with monitor_snapshot_destroy().
 *
 * The timestamp uses CLOCK_MONOTONIC so elapsed-time calculations are not
 * distorted by wall-clock adjustments.
 */
typedef struct {
    struct timespec sampled_at;

    CpuSample *cpus;
    size_t cpu_count;
    CpuUtilizationSummary cpu_summary;

    ProcessSample *processes;
    size_t process_count;

    ThreadSample *threads;
    size_t thread_count;

    MonitorStatus cpu_status;
    MonitorStatus process_status;
    MonitorStatus thread_status;
} MonitorSnapshot;

/* Set a snapshot to an empty, safely destroyable state. */
void monitor_snapshot_init(MonitorSnapshot *snapshot);

/* Free all arrays owned by the snapshot and reset it to an empty state. */
void monitor_snapshot_destroy(MonitorSnapshot *snapshot);

/*
 * Collect one monitoring epoch from /proc.
 *
 * Pass previous == NULL for an initial baseline: cumulative counters are
 * collected, but utilization_valid remains false because no delta exists.
 * On later calls, previous is used to calculate CPU/process/thread utilization.
 * Matching must use CPU ID for CPUs, PID + start_ticks for processes, and
 * PID + TID + start_ticks for threads. Do not add process and thread CPU
 * utilization together: thread observations are a more detailed view of work
 * already represented at process level.
 *
 * `current` must have been initialized with monitor_snapshot_init() and must
 * not alias `previous`. On MONITOR_STATUS_ERROR, the collector must leave
 * `current` safely destroyable. A PARTIAL result means some individual
 * observations could not be read; it is not automatically a fatal failure.
 *
 * This function is the target API for the next refactoring step; it is declared
 * here first so all collector modules and the future controller share one
 * explicit contract. It is not implemented by this header alone yet.
 */
MonitorStatus monitor_collect_snapshot(const MonitorSnapshot *previous,
                                       MonitorSnapshot *current);

#endif /* MONITOR_DATA_H */
