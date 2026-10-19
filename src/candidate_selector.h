
#ifndef CANDIDATE_SELECTOR_H
#define CANDIDATE_SELECTOR_H

#include "monitor_data.h"
#include "locality_policy.h"

#include <sched.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BenefitCostPolicyConfig BenefitCostPolicyConfig;

typedef struct {
    double minimum_thread_utilization_percent;
} CandidateSelectorConfig;

typedef struct {
    pid_t pid;
    pid_t tid;
    unsigned long long start_ticks;
    char name[MONITOR_NAME_MAX];

    int source_cpu;
    int destination_cpu;

    double thread_utilization_percent;
    size_t allowed_cpu_count;

    /* Utilization of the selected source and destination CPUs. */
    double source_cpu_utilization_percent;
    double destination_cpu_utilization_percent;
    int cpu_utilization_valid;

    /* Estimated whole-system imbalance before and after migration. */
    double estimated_system_imbalance_before_percent_points;
    double estimated_system_imbalance_after_percent_points;
    int system_imbalance_estimate_valid;

    /* Locality score from locality_policy.h. */
    int locality_score;
} MigrationCandidate;

typedef enum {
    CANDIDATE_SELECT_ERROR = -1,
    CANDIDATE_SELECT_NONE = 0,
    CANDIDATE_SELECT_FOUND = 1
} CandidateSelectStatus;

typedef enum {
    CANDIDATE_REASON_NOT_SET = 0,
    CANDIDATE_REASON_FOUND,
    CANDIDATE_REASON_INVALID_INPUT,
    CANDIDATE_REASON_ALREADY_BALANCED,
    CANDIDATE_REASON_NO_ELIGIBLE_THREAD,
    CANDIDATE_REASON_AFFINITY_READ_FAILED,
    CANDIDATE_REASON_NO_ALLOWED_DESTINATION,
    CANDIDATE_REASON_PROJECTION_FAILED
} CandidateSelectReason;

typedef enum {
    CANDIDATE_PAIR_REASON_NONE = 0,
    CANDIDATE_PAIR_REASON_DESTINATION_NOT_LOWER,
    CANDIDATE_PAIR_REASON_INELIGIBLE,
    CANDIDATE_PAIR_REASON_PROJECTION_FAILED,
    CANDIDATE_PAIR_REASON_SCORED
} CandidatePairReason;

typedef struct {
    CandidateSelectReason reason;

    /* Nonzero only when a system call or calculation failed. */
    int system_error;

    /* Diagnostic counters: these do not change selection policy. */
    size_t threads_seen;
    size_t threads_rejected_invalid_or_low_utilization;
    size_t threads_rejected_invalid_last_cpu;
    size_t threads_rejected_missing_source_sample;
    size_t threads_rejected_not_owned;
    size_t affinity_read_failures;
    size_t threads_rejected_affinity_mask_too_narrow;
    size_t threads_rejected_source_not_allowed;

    size_t allowed_destinations_seen;
    size_t destinations_rejected_not_lower;
    size_t lower_utilization_destinations_seen;
    size_t eligibility_rejections;
    size_t projection_failures;
    size_t candidates_scored;

    /* Most active thread/destination pair observed, for concise rejection logs. */
    int diagnostic_pair_valid;
    pid_t diagnostic_pid;
    pid_t diagnostic_tid;
    int diagnostic_source_cpu;
    int diagnostic_destination_cpu;
    double diagnostic_thread_utilization_percent;
    double diagnostic_source_cpu_utilization_percent;
    double diagnostic_destination_cpu_utilization_percent;
    CandidatePairReason diagnostic_pair_reason;
} CandidateSelectDiagnostics;

CandidateSelectorConfig candidate_selector_default_config(void);

int candidate_thread_is_eligible(
    const ThreadSample *thread,
    int source_cpu,
    int destination_cpu,
    const cpu_set_t *allowed_cpus,
    double minimum_utilization_percent);

CandidateSelectStatus candidate_select(
    const MonitorSnapshot *snapshot,
    const CandidateSelectorConfig *config,
    MigrationCandidate *candidate,
    CandidateSelectDiagnostics *diagnostics);

/* Rank eligible pairs by the configured benefit-minus-cost score. */
CandidateSelectStatus candidate_select_policy_aware(
    const MonitorSnapshot *snapshot,
    const CandidateSelectorConfig *config,
    const LocalityTopology *topology,
    const BenefitCostPolicyConfig *policy_config,
    MigrationCandidate *candidate,
    CandidateSelectDiagnostics *diagnostics);

#ifdef __cplusplus
}
#endif

#endif
