#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CPUS 1024
#define LINE_SIZE 512

/*
 * One cumulative /proc/stat snapshot for a logical CPU.
 * total and idle are counters, not percentages.
 */
typedef struct {
    unsigned long long total;
    unsigned long long idle;
    int seen;
} CPUStats;

/*
 * Read individual CPU counters from /proc/stat.
 *
 * Returns:
 *   >= 0 : number of individual CPU lines read
 *   -1   : file could not be opened
 *
 * Notes:
 * - The aggregate "cpu" line is intentionally ignored.
 * - Guest and guest_nice are not added separately because Linux includes
 *   them in user and nice, respectively.
 * - A CPU ID >= MAX_CPUS is skipped rather than indexing outside the array.
 */
static int read_cpu_stats(CPUStats cpus[])
{
    FILE *fp = fopen("/proc/stat", "r");
    if (fp == NULL) {
        perror("fopen /proc/stat");
        return -1;
    }

    for (int i = 0; i < MAX_CPUS; i++) {
        cpus[i].seen = 0;
    }

    char line[LINE_SIZE];
    int count = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strncmp(line, "cpu", 3) != 0 ||
            !isdigit((unsigned char)line[3])) {
            continue;
        }

        unsigned int id;
        unsigned long long user, nice, system, idle, iowait;
        unsigned long long irq, softirq, steal;

        int fields = sscanf(line,
            "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu",
            &id, &user, &nice, &system, &idle, &iowait,
            &irq, &softirq, &steal);

        if (fields != 9 || id >= MAX_CPUS) {
            continue;
        }

        cpus[id].total = user + nice + system + idle +
                         iowait + irq + softirq + steal;
        cpus[id].idle = idle + iowait;
        cpus[id].seen = 1;
        count++;
    }

    if (ferror(fp)) {
        perror("fgets /proc/stat");
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return count;
}

/*
 * Calculate utilization between two cumulative snapshots.
 *
 * Returns -1.0 when the sample is not usable:
 * - CPU was not present in both snapshots
 * - counters moved backwards (e.g. reset or inconsistent sample)
 * - no total time elapsed
 * - idle delta exceeds total delta
 */
static double calculate_utilization(const CPUStats *previous,const CPUStats *current)
{
    if (!previous->seen || !current->seen ||
        current->total < previous->total ||
        current->idle < previous->idle) {
        return -1.0;
    }

    unsigned long long delta_total = current->total - previous->total;
    unsigned long long delta_idle = current->idle - previous->idle;

    if (delta_total == 0 || delta_idle > delta_total) {
        return -1.0;
    }

    double utilization = 100.0 *
        (1.0 - (double)delta_idle / (double)delta_total);

    /* Clamp tiny floating-point excursions to the valid range. */
    if (utilization < 0.0) utilization = 0.0;
    if (utilization > 100.0) utilization = 100.0;
    return utilization;
}

int main(void)
{
    CPUStats previous[MAX_CPUS] = {0};
    CPUStats current[MAX_CPUS] = {0};

    printf("====================================\n");
    printf(" Adaptive CPU Affinity Manager\n");
    printf(" Module: CPU Monitoring\n");
    printf("====================================\n");

    int initial_count = read_cpu_stats(previous);
    if (initial_count <= 0) {
        fprintf(stderr, "Could not read individual CPU statistics.\n");
        return EXIT_FAILURE;
    }

    printf("Detected %d CPU entries (IDs below %d).\n",
           initial_count, MAX_CPUS);
    printf("Sampling interval: approximately 1 second\n");
    printf("Press Ctrl+C to stop.\n");

    while (1) {
        sleep(1);

        int current_count = read_cpu_stats(current);
        if (current_count <= 0) {
            fprintf(stderr, "Failed to read CPU statistics; retrying.\n");
            continue;
        }

        printf("\nCPU Utilization\n");
        printf("---------------------------------------------\n");

        double min_util = 100.0;
        double max_util = 0.0;
        double sum_util = 0.0;
        int valid_cpus = 0;

        for (int i = 0; i < MAX_CPUS; i++) {
            if (!previous[i].seen || !current[i].seen) {
                continue;
            }

            double utilization =
                calculate_utilization(&previous[i], &current[i]);

            if (utilization < 0.0) {
                printf("CPU %-3d: sample unavailable (counter changed)\n", i);
                continue;
            }

            printf("CPU %-3d: %6.2f%%\n", i, utilization);

            if (utilization < min_util) min_util = utilization;
            if (utilization > max_util) max_util = utilization;
            sum_util += utilization;
            valid_cpus++;
        }

        if (valid_cpus > 0) {
            printf("---------------------------------------------\n");
            printf("Average utilization : %.2f%%\n",
                   sum_util / (double)valid_cpus);
            printf("Minimum utilization : %.2f%%\n", min_util);
            printf("Maximum utilization : %.2f%%\n", max_util);
            printf("Load imbalance      : %.2f percentage points\n",
                   max_util - min_util);
            printf("Valid CPUs sampled  : %d\n", valid_cpus);
        } else {
            printf("No CPUs had a valid pair of samples.\n");
        }

        /*
         * Always use the latest snapshot as the next baseline, including
         * after a counter anomaly. This avoids repeatedly comparing against
         * an old baseline.
         */
        memcpy(previous, current, sizeof(previous));
        fflush(stdout);
    }

    return EXIT_SUCCESS;
}