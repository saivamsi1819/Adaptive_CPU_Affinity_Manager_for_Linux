
#include "benefit_cost_policy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int finite_nonnegative(double value)
{
    return isfinite(value) && value >= 0.0;
}

static int locality_cost_factor(LocalityScore score, double *factor)
{
    if (factor == NULL)
        return -1;

    switch (score) {
    case LOCALITY_SCORE_SAME_L1:
        *factor = 0.25;
        return 0;
    case LOCALITY_SCORE_SAME_L2:
        *factor = 0.50;
        return 0;
    case LOCALITY_SCORE_SAME_L3:
        *factor = 0.75;
        return 0;
    case LOCALITY_SCORE_UNKNOWN:
        *factor = 1.0;
        return 0;
    case LOCALITY_SCORE_DIFFERENT_DOMAIN:
        *factor = 2.0;
        return 0;
    case LOCALITY_SCORE_ERROR:
    default:
        return -1;
    }
}

static void set_result(BenefitCostResult *result,
                       BenefitCostDecision decision,
                       const char *reason)
{
    result->decision = decision;
    (void)snprintf(result->reason, sizeof(result->reason), "%s", reason);
}

BenefitCostPolicyConfig benefit_cost_policy_default_config(void)
{
    BenefitCostPolicyConfig config;

    config.base_migration_cost_percent_points = 5.0;
    config.locality_cost_percent_points = 5.0;
    config.minimum_expected_benefit_percent_points = 5.0;
    config.minimum_score_percent_points = 0.0;

    return config;
}

BenefitCostDecision benefit_cost_evaluate(
    const CpuUtilizationSummary *summary,
    const MigrationCandidate *candidate,
    const BenefitCostPolicyConfig *config,
    BenefitCostResult *result)
{
    if (result == NULL)
        return BENEFIT_COST_ERROR;

    memset(result, 0, sizeof(*result));
    result->decision = BENEFIT_COST_ERROR;

    if (summary == NULL || candidate == NULL || !summary->valid ||
        summary->valid_cpu_count < 2 ||
        !isfinite(summary->minimum_percent) ||
        !isfinite(summary->maximum_percent) ||
        !isfinite(summary->imbalance_percent_points) ||
        summary->minimum_percent < 0.0 ||
        summary->maximum_percent > 100.0 ||
        summary->minimum_percent > summary->maximum_percent ||
        summary->imbalance_percent_points < 0.0 ||
        summary->imbalance_percent_points > 100.0 ||
        candidate->pid <= 0 ||
        candidate->tid <= 0 ||
        candidate->source_cpu < 0 ||
        candidate->destination_cpu < 0 ||
        candidate->source_cpu == candidate->destination_cpu ||
        !isfinite(candidate->thread_utilization_percent) ||
        candidate->thread_utilization_percent <= 0.0 ||
        candidate->thread_utilization_percent > 100.0) {
        set_result(result, BENEFIT_COST_REJECT_INVALID,
                   "Invalid summary or candidate fields");
        return result->decision;
    }

    BenefitCostPolicyConfig effective =
        config != NULL ? *config : benefit_cost_policy_default_config();

    if (!finite_nonnegative(
            effective.base_migration_cost_percent_points) ||
        !finite_nonnegative(
            effective.locality_cost_percent_points) ||
        !finite_nonnegative(
            effective.minimum_expected_benefit_percent_points) ||
        !isfinite(effective.minimum_score_percent_points)) {
        set_result(result, BENEFIT_COST_REJECT_INVALID,
                   "Invalid policy configuration");
        return result->decision;
    }

    /*
     * Prefer the selector's system-wide projection. It accounts for the
     * actual source/destination CPUs and all valid CPU samples.
     *
     * Keep the simpler extrema-based estimate as a compatibility fallback
     * for callers/tests that construct candidates without a projection.
     */
    if (candidate->system_imbalance_estimate_valid) {
        double before =
            candidate->estimated_system_imbalance_before_percent_points;
        double after =
            candidate->estimated_system_imbalance_after_percent_points;

        if (!isfinite(before) || !isfinite(after) ||
            before < 0.0 || before > 100.0 ||
            after < 0.0 || after > 100.0 ||
            after > before + 1e-9) {
            set_result(result, BENEFIT_COST_REJECT_INVALID,
                       "Invalid or non-improving selector projection");
            return result->decision;
        }

        result->imbalance_before_percent_points = before;
        result->estimated_imbalance_after_percent_points = after;
        result->estimated_benefit_percent_points = before - after;
    } else {
        /*
         * Compatibility fallback only. This assumes the move transfers
         * utilization from the busiest CPU to the least busy CPU. It is
         * less accurate than the selector's system-wide projection.
         */
        if (summary->imbalance_percent_points <= 0.0) {
            set_result(result, BENEFIT_COST_REJECT_LOW_BENEFIT,
                       "No positive CPU imbalance");
            return result->decision;
        }

        double source_after =
            summary->maximum_percent -
            candidate->thread_utilization_percent;

        double destination_after =
            summary->minimum_percent +
            candidate->thread_utilization_percent;

        if (source_after < 0.0)
            source_after = 0.0;
        if (source_after > 100.0)
            source_after = 100.0;

        if (destination_after < 0.0)
            destination_after = 0.0;
        if (destination_after > 100.0)
            destination_after = 100.0;

        result->imbalance_before_percent_points =
            summary->imbalance_percent_points;

        result->estimated_imbalance_after_percent_points =
            fabs(source_after - destination_after);

        result->estimated_benefit_percent_points =
            result->imbalance_before_percent_points -
            result->estimated_imbalance_after_percent_points;

        if (result->estimated_benefit_percent_points < 0.0)
            result->estimated_benefit_percent_points = 0.0;
    }

    double locality_factor = 0.0;
    if (locality_cost_factor((LocalityScore)candidate->locality_score,
                             &locality_factor) != 0) {
        set_result(result, BENEFIT_COST_REJECT_INVALID,
                   "Invalid destination locality score");
        return result->decision;
    }

    result->estimated_migration_cost_percent_points =
        effective.base_migration_cost_percent_points +
        effective.locality_cost_percent_points * locality_factor;

    result->score_percent_points =
        result->estimated_benefit_percent_points -
        result->estimated_migration_cost_percent_points;

    if (result->estimated_benefit_percent_points <
        effective.minimum_expected_benefit_percent_points) {
        set_result(result, BENEFIT_COST_REJECT_LOW_BENEFIT,
                   "Estimated imbalance reduction is below the minimum benefit");
    } else if (result->score_percent_points <
               effective.minimum_score_percent_points) {
        set_result(result, BENEFIT_COST_REJECT_COST,
                   "Estimated benefit does not exceed modeled migration cost and score threshold");
    } else {
        set_result(result, BENEFIT_COST_APPROVE,
                   "Estimated benefit clears minimum benefit and score thresholds");
    }

    return result->decision;
}
