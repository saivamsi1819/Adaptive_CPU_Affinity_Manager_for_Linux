#ifndef CANDIDATE_SELECTOR_H
#define CANDIDATE_SELECTOR_H

#include "monitor_data.h"
#include <sched.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double minimum_thread_utilization_percent;
} CandidateSelectorConfig;

typedef struct {
    pid_t pid;
    pid_t tid;
    char name[MONITOR_NAME_MAX];
    int source_cpu;
    int destination_cpu;
    double thread_utilization_percent;
    size_t allowed_cpu_count;
} MigrationCandidate;

typedef enum {
    CANDIDATE_SELECT_ERROR = -1,
    CANDIDATE_SELECT_NONE = 0,
    CANDIDATE_SELECT_FOUND = 1
} CandidateSelectStatus;

CandidateSelectorConfig candidate_selector_default_config(void);

/*
 * Pure eligibility check, separated so affinity constraints can be unit-tested.
 * The candidate must have valid recent CPU usage, have last run on source_cpu,
 * meet the utilization threshold, and be allowed on both source and destination.
 * At least two CPUs must be allowed, otherwise moving would be impossible.
 */
int candidate_thread_is_eligible(const ThreadSample *thread,
                                 int source_cpu,
                                 int destination_cpu,
                                 const cpu_set_t *allowed_cpus,
                                 double minimum_utilization_percent);

/*
 * Select the highest-utilization eligible thread from the current snapshot.
 * Source is the most utilized valid CPU; destination is the least utilized.
 * sched_getaffinity() is queried for each TID. This proposes a candidate only;
 * it never changes affinity. The last-CPU field is a heuristic, not proof that
 * the thread consumed all its measured CPU time on that CPU.
 */
CandidateSelectStatus candidate_select(const MonitorSnapshot *snapshot,
                                       const CandidateSelectorConfig *config,
                                       MigrationCandidate *candidate);

#ifdef __cplusplus
}
#endif

#endif /* CANDIDATE_SELECTOR_H */
