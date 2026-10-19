#ifndef BENEFIT_COST_POLICY_H
#define BENEFIT_COST_POLICY_H

#include "candidate_selector.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Heuristic policy for evaluating a proposed single-thread affinity move.
 * When the selector supplies a system-wide projection, benefit is the reduction
 * in population standard deviation of per-CPU utilization, in percentage
 * points. Cost parameters are tunable policy weights, not measured cache
 * costs. The locality weight is scaled by the selected topology score;
 * unknown topology receives the full locality weight.
 */
typedef struct BenefitCostPolicyConfig {
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
 * It prefers the selector's system-wide projection. If that estimate is absent,
 * it uses a compatibility pairwise fallback. All estimates are heuristics:
 * /proc's last_cpu is not a per-CPU execution history. Revalidate task identity,
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
