#define _POSIX_C_SOURCE 200809L
#include "monitor_data.h"

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    MonitorSnapshot previous, current;
    monitor_snapshot_init(&previous);
    monitor_snapshot_init(&current);

    MonitorStatus first = monitor_collect_snapshot(NULL, &previous);
    if (first == MONITOR_STATUS_ERROR) {
        fprintf(stderr, "Initial snapshot collection failed.\n");
        monitor_snapshot_destroy(&previous);
        return 1;
    }

    if (sleep(1) != 0) {
        perror("sleep");
        monitor_snapshot_destroy(&previous);
        return 1;
    }

    MonitorStatus second = monitor_collect_snapshot(&previous, &current);
    if (second == MONITOR_STATUS_ERROR) {
        fprintf(stderr, "Second snapshot collection failed.\n");
        monitor_snapshot_destroy(&previous);
        monitor_snapshot_destroy(&current);
        return 1;
    }

    printf("Initial status: %d\n", first);
    printf("Second status: %d\n", second);
    printf("CPUs: %zu; processes: %zu; threads: %zu\n",
           current.cpu_count, current.process_count, current.thread_count);

    if (current.cpu_summary.valid) {
        printf("Valid CPUs: %zu; average: %.2f%%; min: %.2f%%; max: %.2f%%; imbalance: %.2f percentage points\n",
               current.cpu_summary.valid_cpu_count,
               current.cpu_summary.average_percent,
               current.cpu_summary.minimum_percent,
               current.cpu_summary.maximum_percent,
               current.cpu_summary.imbalance_percent_points);
    } else {
        printf("CPU utilization summary is not valid yet.\n");
    }

    size_t valid_processes = 0, valid_threads = 0;
    for (size_t i = 0; i < current.process_count; ++i)
        if (current.processes[i].utilization_valid) ++valid_processes;
    for (size_t i = 0; i < current.thread_count; ++i)
        if (current.threads[i].utilization_valid) ++valid_threads;
    printf("Processes with valid utilization: %zu\n", valid_processes);
    printf("Threads with valid utilization: %zu\n", valid_threads);

    monitor_snapshot_destroy(&previous);
    monitor_snapshot_destroy(&current);
    return 0;
}
