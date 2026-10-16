#ifndef BENEFIT_COST_POLICY_H
#define BENEFIT_COST_POLICY_H

#include "candidate_selector.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Heuristic policy for evaluating a proposed single-thread affinity move.
 * All cost/benefit values are expressed in percentage points of CPU imbalance.
 * These are tunable model parameters, not measured physical cache-miss costs.
 */
typedef struct {
    double base_migration_cost_percent_points;
    double locality_cost_percent_points;
    double minimum_expected_benefit_percent_points;
    double minimum_score_percent_points;
} BenefitCostPolicyConfig;

typedef enum {
    BENEFIT_COST_ERROR = -1,
    BENEFIT_COST_REJECT_INVALID = 0,
    BENEFIT_COST_REJECT_LOW_BENEFIT = 1,
    BENEFIT_COST_REJECT_COST = 2,
    BENEFIT_COST_APPROVE = 3
} BenefitCostDecision;

typedef struct {
    BenefitCostDecision decision;
    double imbalance_before_percent_points;
    double estimated_imbalance_after_percent_points;
    double estimated_benefit_percent_points;
    double estimated_migration_cost_percent_points;
    double score_percent_points;
    char reason[160];
} BenefitCostResult;

BenefitCostPolicyConfig benefit_cost_policy_default_config(void);

/*
 * Estimate whether a candidate move is worthwhile. This function is pure:
 * it does not inspect or change affinity and never calls sched_setaffinity().
 * The simple projection assumes the candidate's measured thread utilization
 * can be removed from source and added to destination. Since /proc's last_cpu
 * is not a per-CPU execution history, this is a heuristic and must be validated
 * experimentally. A controller must independently revalidate task identity,
 * affinity, and safety immediately before any eventual action.
 */
BenefitCostDecision benefit_cost_evaluate(
    const CpuUtilizationSummary *summary,
    const MigrationCandidate *candidate,
    const BenefitCostPolicyConfig *config,
    BenefitCostResult *result);

#ifdef __cplusplus
}
#endif

#endif /* BENEFIT_COST_POLICY_H */
