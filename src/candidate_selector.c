#define _GNU_SOURCE
#include "candidate_selector.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

CandidateSelectorConfig candidate_selector_default_config(void)
{
    CandidateSelectorConfig config;
    config.minimum_thread_utilization_percent = 5.0;
    return config;
}

static int valid_cpu_id(int cpu_id)
{
    return cpu_id >= 0 && cpu_id < CPU_SETSIZE;
}

int candidate_thread_is_eligible(const ThreadSample *thread,
                                 int source_cpu,
                                 int destination_cpu,
                                 const cpu_set_t *allowed_cpus,
                                 double minimum_utilization_percent)
{
    if (thread == NULL || allowed_cpus == NULL || source_cpu == destination_cpu ||
        !valid_cpu_id(source_cpu) || !valid_cpu_id(destination_cpu) ||
        !isfinite(minimum_utilization_percent) || minimum_utilization_percent < 0.0)
        return 0;

    if (thread->pid <= 0 || thread->tid <= 0 || thread->last_cpu != source_cpu ||
        !thread->utilization_valid || !isfinite(thread->utilization_percent) ||
        thread->utilization_percent < minimum_utilization_percent ||
        thread->utilization_percent < 0.0)
        return 0;

    if (!CPU_ISSET(source_cpu, allowed_cpus) ||
        !CPU_ISSET(destination_cpu, allowed_cpus))
        return 0;

    /* A one-CPU affinity mask leaves no alternative destination. */
    if (CPU_COUNT(allowed_cpus) < 2) return 0;
    return 1;
}

CandidateSelectStatus candidate_select(const MonitorSnapshot *snapshot,
                                       const CandidateSelectorConfig *config,
                                       MigrationCandidate *candidate)
{
    if (snapshot == NULL || candidate == NULL || !snapshot->cpu_summary.valid ||
        snapshot->cpu_summary.valid_cpu_count < 2 || snapshot->cpus == NULL ||
        snapshot->threads == NULL) {
        errno = EINVAL;
        return CANDIDATE_SELECT_ERROR;
    }

    CandidateSelectorConfig effective = config ? *config : candidate_selector_default_config();
    if (!isfinite(effective.minimum_thread_utilization_percent) ||
        effective.minimum_thread_utilization_percent < 0.0 ||
        effective.minimum_thread_utilization_percent > 100.0) {
        errno = EINVAL;
        return CANDIDATE_SELECT_ERROR;
    }

    int source_cpu = -1, destination_cpu = -1;
    double maximum = -1.0, minimum = 101.0;
    for (size_t i = 0; i < snapshot->cpu_count; ++i) {
        const CpuSample *cpu = &snapshot->cpus[i];
        if (!cpu->utilization_valid || !isfinite(cpu->utilization_percent) ||
            cpu->utilization_percent < 0.0 || cpu->utilization_percent > 100.0 ||
            !valid_cpu_id(cpu->cpu_id))
            continue;
        if (cpu->utilization_percent > maximum) {
            maximum = cpu->utilization_percent;
            source_cpu = cpu->cpu_id;
        }
        if (cpu->utilization_percent < minimum) {
            minimum = cpu->utilization_percent;
            destination_cpu = cpu->cpu_id;
        }
    }
    if (source_cpu < 0 || destination_cpu < 0 || source_cpu == destination_cpu) {
        errno = ENODATA;
        return CANDIDATE_SELECT_ERROR;
    }

    int found = 0;
    MigrationCandidate best;
    memset(&best, 0, sizeof(best));
    double best_utilization = -1.0;

    for (size_t i = 0; i < snapshot->thread_count; ++i) {
        const ThreadSample *thread = &snapshot->threads[i];
        if (thread->pid <= 0 || thread->tid <= 0 || thread->last_cpu != source_cpu ||
            !thread->utilization_valid || !isfinite(thread->utilization_percent) ||
            thread->utilization_percent < effective.minimum_thread_utilization_percent ||
            thread->utilization_percent < 0.0)
            continue;

        cpu_set_t allowed;
        CPU_ZERO(&allowed);
        if (sched_getaffinity(thread->tid, sizeof(allowed), &allowed) != 0)
            continue; /* Task may have exited or may not be inspectable. */

        if (!candidate_thread_is_eligible(thread, source_cpu, destination_cpu,
                                          &allowed,
                                          effective.minimum_thread_utilization_percent))
            continue;

        if (!found || thread->utilization_percent > best_utilization) {
            found = 1;
            best_utilization = thread->utilization_percent;
            best.pid = thread->pid;
            best.tid = thread->tid;
            snprintf(best.name, sizeof(best.name), "%s", thread->name);
            best.source_cpu = source_cpu;
            best.destination_cpu = destination_cpu;
            best.thread_utilization_percent = thread->utilization_percent;
            best.allowed_cpu_count = (size_t)CPU_COUNT(&allowed);
        }
    }

    if (!found) return CANDIDATE_SELECT_NONE;
    *candidate = best;
    return CANDIDATE_SELECT_FOUND;
}
