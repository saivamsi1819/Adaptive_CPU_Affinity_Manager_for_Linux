#ifndef MONITOR_DATA_H
#define MONITOR_DATA_H

/* Shared data contract and snapshot collector for the CPU affinity manager. */

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#define MONITOR_NAME_MAX 256

typedef enum {
    MONITOR_STATUS_ERROR = -1,
    MONITOR_STATUS_OK = 0,
    MONITOR_STATUS_PARTIAL = 1
} MonitorStatus;

typedef struct {
    int cpu_id;
    unsigned long long total_ticks;
    unsigned long long idle_ticks;
    double utilization_percent;
    int counters_valid;
    int utilization_valid;
} CpuSample;

typedef struct {
    double average_percent;
    double minimum_percent;
    double maximum_percent;
    double imbalance_percent_points;
    size_t valid_cpu_count;
    int valid;
} CpuUtilizationSummary;

typedef struct {
    pid_t pid;
    char name[MONITOR_NAME_MAX];
    unsigned long long start_ticks;
    unsigned long long cpu_time_ticks;
    struct timespec sampled_at;
    double utilization_percent;
    int utilization_valid;
} ProcessSample;

typedef struct {
    pid_t pid;
    pid_t tid;
    int last_cpu;
    char name[MONITOR_NAME_MAX];
    unsigned long long start_ticks;
    unsigned long long cpu_time_ticks;
    struct timespec sampled_at;
    double utilization_percent;
    int utilization_valid;
} ThreadSample;

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

/*
 * Convert a thread CPU-time delta to utilization. A one-tick overshoot is
 * tolerated to account for procfs tick quantization and per-thread sampling
 * timestamps; such values are clamped to 100%. Larger overshoots are rejected.
 * Returns 0 on success, -1 for invalid/impossible input.
 */
int monitor_compute_thread_utilization(unsigned long long delta_ticks,
                                      double elapsed_seconds,
                                      long ticks_per_second,
                                      double *utilization_percent);

/* Initialize an empty snapshot. */
void monitor_snapshot_init(MonitorSnapshot *snapshot);

/* Free snapshot-owned memory and reset the snapshot. */
void monitor_snapshot_destroy(MonitorSnapshot *snapshot);

/*
 * Collect CPU counters, process observations, and thread observations.
 *
 * Pass previous == NULL for the initial baseline. Utilization deltas are
 * invalid until a matching previous observation exists.
 *
 * Process identity uses PID + start time.
 * Thread identity uses PID + TID + start time.
 *
 * Do not add process and thread utilization together: thread samples describe
 * work already represented by their process sample.
 *
 * current must be initialized and empty and must not alias previous.
 * On MONITOR_STATUS_ERROR, current remains safely destroyable.
 * MONITOR_STATUS_PARTIAL means some observations could not be read.
 */
MonitorStatus monitor_collect_snapshot(const MonitorSnapshot *previous,
                                       MonitorSnapshot *current);

#endif /* MONITOR_DATA_H */