#include "benefit_cost_policy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int expect(const char *label, int condition)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", label);
        return 0;
    }
    return 1;
}

static CpuUtilizationSummary make_summary(double min, double max)
{
    CpuUtilizationSummary summary;
    memset(&summary, 0, sizeof(summary));
    summary.minimum_percent = min;
    summary.maximum_percent = max;
    summary.average_percent = (min + max) / 2.0;
    summary.imbalance_percent_points = max - min;
    summary.valid_cpu_count = 2;
    summary.valid = 1;
    return summary;
}

static MigrationCandidate make_candidate(double utilization)
{
    MigrationCandidate candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.pid = 100;
    candidate.tid = 101;
    candidate.source_cpu = 2;
    candidate.destination_cpu = 3;
    candidate.thread_utilization_percent = utilization;
    return candidate;
}

int main(void)
{
    int ok = 1;
    BenefitCostResult result;
    BenefitCostPolicyConfig config = benefit_cost_policy_default_config();
    CpuUtilizationSummary summary = make_summary(20.0, 80.0);
    MigrationCandidate candidate = make_candidate(25.0);

    ok &= expect("beneficial move approved",
                 benefit_cost_evaluate(&summary, &candidate, &config, &result) ==
                 BENEFIT_COST_APPROVE);
    ok &= expect("estimated benefit is 50 percentage points",
                 fabs(result.estimated_benefit_percent_points - 50.0) < 1e-9);
    ok &= expect("modeled cost is 10 percentage points",
                 fabs(result.estimated_migration_cost_percent_points - 10.0) < 1e-9);
    ok &= expect("score is benefit minus cost",
                 fabs(result.score_percent_points - 40.0) < 1e-9);

    /* The whole-system projection must take precedence over the old pairwise
     * fallback. This represents [100,100,0,0] -> [50,100,50,0]: max-min
     * remains 100pp, but population standard deviation falls from 50 to
     * sqrt(1250), so the move is meaningfully beneficial. */
    summary = make_summary(0.0, 100.0);
    candidate = make_candidate(50.0);
    candidate.system_imbalance_estimate_valid = 1;
    candidate.estimated_system_imbalance_before_percent_points = 50.0;
    candidate.estimated_system_imbalance_after_percent_points = sqrt(1250.0);
    ok &= expect("system-wide spread reduction is approved",
                 benefit_cost_evaluate(&summary, &candidate, &config, &result) ==
                 BENEFIT_COST_APPROVE);
    ok &= expect("system-wide estimate is used for benefit",
                 fabs(result.estimated_benefit_percent_points -
                      (50.0 - sqrt(1250.0))) < 1e-9);

    /* A 9pp projected benefit clears the 5pp minimum. Same-L3 locality
       scales the locality cost enough to approve; a remote domain does not. */
    candidate.system_imbalance_estimate_valid = 1;
    candidate.estimated_system_imbalance_before_percent_points =
        sqrt(10000.0 / 12.0 - (100.0 / 12.0) * (100.0 / 12.0));
    candidate.estimated_system_imbalance_after_percent_points =
        sqrt(5000.0 / 12.0 - (100.0 / 12.0) * (100.0 / 12.0));
    candidate.locality_score = LOCALITY_SCORE_SAME_L3;
    ok &= expect("same-L3 locality scales the migration cost",
                 benefit_cost_evaluate(&summary, &candidate, &config, &result) ==
                     BENEFIT_COST_APPROVE &&
                 fabs(result.estimated_migration_cost_percent_points - 8.75) <
                     1e-9);
    candidate.locality_score = LOCALITY_SCORE_DIFFERENT_DOMAIN;
    ok &= expect("different locality domain receives the larger penalty",
                 benefit_cost_evaluate(&summary, &candidate, &config, &result) ==
                     BENEFIT_COST_REJECT_COST &&
                 fabs(result.estimated_migration_cost_percent_points - 15.0) <
                     1e-9);

    candidate.estimated_system_imbalance_before_percent_points = 50.0;
    candidate.estimated_system_imbalance_after_percent_points = 50.0;
    ok &= expect("zero system-wide improvement is rejected even if pairwise fallback looks good",
                 benefit_cost_evaluate(&summary, &candidate, &config, &result) ==
                 BENEFIT_COST_REJECT_LOW_BENEFIT);

    candidate = make_candidate(50.0);
    candidate.system_imbalance_estimate_valid = 1;
    candidate.estimated_system_imbalance_before_percent_points = 50.0;
    candidate.estimated_system_imbalance_after_percent_points = NAN;
    ok &= expect("non-finite system-wide estimate rejected",
                 benefit_cost_evaluate(&summary, &candidate, &config, &result) ==
                 BENEFIT_COST_REJECT_INVALID);

    summary = make_summary(20.0, 80.0);
    candidate = make_candidate(2.0);
    ok &= expect("small projected benefit rejected",
                 benefit_cost_evaluate(&summary, &candidate, &config, &result) ==
                 BENEFIT_COST_REJECT_LOW_BENEFIT);

    candidate = make_candidate(40.0);
    summary = make_summary(45.0, 55.0);
    ok &= expect("move that worsens imbalance rejected",
                 benefit_cost_evaluate(&summary, &candidate, &config, &result) !=
                 BENEFIT_COST_APPROVE);

    summary = make_summary(20.0, 80.0);
    candidate = make_candidate(25.0);
    config.base_migration_cost_percent_points = 30.0;
    config.locality_cost_percent_points = 30.0;
    ok &= expect("high modeled cost rejected",
                 benefit_cost_evaluate(&summary, &candidate, &config, &result) ==
                 BENEFIT_COST_REJECT_COST);

    config = benefit_cost_policy_default_config();
    candidate.thread_utilization_percent = NAN;
    ok &= expect("non-finite candidate utilization rejected",
                 benefit_cost_evaluate(&summary, &candidate, &config, &result) ==
                 BENEFIT_COST_REJECT_INVALID);

    config.minimum_score_percent_points = NAN;
    candidate = make_candidate(25.0);
    ok &= expect("invalid configuration rejected",
                 benefit_cost_evaluate(&summary, &candidate, &config, &result) ==
                 BENEFIT_COST_REJECT_INVALID);

    if (!ok) return 1;
    puts("PASS: benefit projection, low-benefit rejection, cost rejection, invalid inputs, and config validation");
    return 0;
}
