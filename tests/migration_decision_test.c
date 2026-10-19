#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "benefit_cost_policy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

static int expect(const char *label, int condition)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", label);
        return 0;
    }
    return 1;
}

static int test_policy_aware_destination_ranking(void)
{
    int ok = 1;
    cpu_set_t original_allowed;
    if (sched_getaffinity(0, sizeof(original_allowed), &original_allowed) != 0) {
        perror("sched_getaffinity(locality ranking)");
        return 0;
    }

    int ids[4];
    size_t count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE && count < 4; ++cpu)
        if (CPU_ISSET(cpu, &original_allowed)) ids[count++] = cpu;
    if (count < 4) {
        puts("SKIP: policy-aware destination test requires four allowed CPUs");
        return 1;
    }

    cpu_set_t test_mask;
    CPU_ZERO(&test_mask);
    for (size_t i = 0; i < 4; ++i) CPU_SET(ids[i], &test_mask);
    if (sched_setaffinity(0, sizeof(test_mask), &test_mask) != 0) {
        perror("sched_setaffinity(locality ranking)");
        return 0;
    }

    CpuSample cpus[4] = {0};
    const double loads[4] = {60.0, 5.0, 0.0, 0.0};
    for (size_t i = 0; i < 4; ++i) {
        cpus[i].cpu_id = ids[i];
        cpus[i].utilization_percent = loads[i];
        cpus[i].utilization_valid = 1;
    }

    ThreadSample thread = {0};
    thread.pid = getpid();
    thread.tid = (pid_t)syscall(SYS_gettid);
    thread.last_cpu = ids[0];
    thread.utilization_percent = 15.0;
    thread.utilization_valid = 1;
    snprintf(thread.name, sizeof(thread.name), "locality-ranking-worker");

    MonitorSnapshot snapshot = {0};
    snapshot.cpus = cpus;
    snapshot.cpu_count = 4;
    snapshot.threads = &thread;
    snapshot.thread_count = 1;
    snapshot.cpu_summary.valid = 1;
    snapshot.cpu_summary.valid_cpu_count = 4;
    snapshot.cpu_summary.minimum_percent = 0.0;
    snapshot.cpu_summary.maximum_percent = 60.0;
    snapshot.cpu_summary.average_percent = 16.25;
    snapshot.cpu_summary.imbalance_percent_points = 60.0;

    LocalityTopology topology;
    locality_topology_init(&topology);
    for (size_t i = 0; i < 4; ++i) {
        CPU_SET(ids[i], &topology.present_cpus);
        topology.discovered_cpu_count++;
        CPU_SET(ids[i], &topology.l1_shared[ids[i]]);
        CPU_SET(ids[i], &topology.l2_shared[ids[i]]);
        CPU_SET(ids[i], &topology.l3_shared[ids[i]]);
    }
    CPU_SET(ids[0], &topology.l1_shared[ids[0]]);
    CPU_SET(ids[1], &topology.l1_shared[ids[0]]);
    CPU_SET(ids[0], &topology.l1_shared[ids[1]]);
    CPU_SET(ids[1], &topology.l1_shared[ids[1]]);
    CPU_SET(ids[2], &topology.l1_shared[ids[2]]);
    CPU_SET(ids[3], &topology.l1_shared[ids[2]]);
    CPU_SET(ids[2], &topology.l1_shared[ids[3]]);
    CPU_SET(ids[3], &topology.l1_shared[ids[3]]);

    CandidateSelectorConfig selector_config = candidate_selector_default_config();
    BenefitCostPolicyConfig policy_config = benefit_cost_policy_default_config();
    MigrationCandidate candidate = {0};
    CandidateSelectDiagnostics diagnostics;
    CandidateSelectStatus status = candidate_select_policy_aware(
        &snapshot, &selector_config, &topology, &policy_config,
        &candidate, &diagnostics);
    ok &= expect("policy-aware selector chooses the lower-cost same-L1 destination",
                 status == CANDIDATE_SELECT_FOUND &&
                 candidate.destination_cpu == ids[1] &&
                 candidate.locality_score == LOCALITY_SCORE_SAME_L1);

    BenefitCostResult result;
    BenefitCostDecision decision = status == CANDIDATE_SELECT_FOUND
        ? benefit_cost_evaluate(&snapshot.cpu_summary, &candidate,
                                &policy_config, &result)
        : BENEFIT_COST_ERROR;
    ok &= expect("same-L1 alternative clears benefit and cost thresholds",
                 decision == BENEFIT_COST_APPROVE &&
                 result.estimated_benefit_percent_points >= 5.0 &&
                 result.score_percent_points >= 0.0);

    status = candidate_select(&snapshot, &selector_config, &candidate,
                              &diagnostics);
    decision = status == CANDIDATE_SELECT_FOUND
        ? benefit_cost_evaluate(&snapshot.cpu_summary, &candidate,
                                &policy_config, &result)
        : BENEFIT_COST_ERROR;
    ok &= expect("highest-benefit unknown-locality destination fails policy cost",
                 status == CANDIDATE_SELECT_FOUND &&
                 candidate.destination_cpu == ids[2] &&
                 decision == BENEFIT_COST_REJECT_COST);

    if (sched_setaffinity(0, sizeof(original_allowed), &original_allowed) != 0) {
        perror("sched_setaffinity(restore locality ranking)");
        return 0;
    }
    return ok;
}

int main(void)
{
    int ok = test_policy_aware_destination_ranking();
    cpu_set_t original_allowed;
    if (sched_getaffinity(0, sizeof(original_allowed), &original_allowed) != 0) {
        perror("sched_getaffinity");
        return 1;
    }

    int allowed_ids[2];
    size_t allowed_count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE && allowed_count < 2; ++cpu) {
        if (CPU_ISSET(cpu, &original_allowed)) allowed_ids[allowed_count++] = cpu;
    }
    if (allowed_count < 2) {
        puts("SKIP: migration decision integration test requires two allowed CPUs");
        return 0;
    }

    const int source_cpu = allowed_ids[0];
    const int destination_cpu = allowed_ids[1];
    cpu_set_t test_allowed;
    CPU_ZERO(&test_allowed);
    CPU_SET(source_cpu, &test_allowed);
    CPU_SET(destination_cpu, &test_allowed);
    if (sched_setaffinity(0, sizeof(test_allowed), &test_allowed) != 0) {
        perror("sched_setaffinity(test mask)");
        return 1;
    }

    CpuSample cpus[2];
    memset(cpus, 0, sizeof(cpus));
    cpus[0].cpu_id = source_cpu;
    cpus[0].utilization_percent = 100.0;
    cpus[0].utilization_valid = 1;
    cpus[1].cpu_id = destination_cpu;
    cpus[1].utilization_percent = 0.0;
    cpus[1].utilization_valid = 1;

    ThreadSample thread;
    memset(&thread, 0, sizeof(thread));
    thread.pid = getpid();
    thread.tid = (pid_t)syscall(SYS_gettid);
    thread.last_cpu = source_cpu;
    thread.utilization_percent = 50.0;
    thread.utilization_valid = 1;
    snprintf(thread.name, sizeof(thread.name), "synthetic-shared-core-worker");

    MonitorSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.cpus = cpus;
    snapshot.cpu_count = 2;
    snapshot.threads = &thread;
    snapshot.thread_count = 1;
    snapshot.cpu_summary.valid = 1;
    snapshot.cpu_summary.valid_cpu_count = 2;
    snapshot.cpu_summary.minimum_percent = 0.0;
    snapshot.cpu_summary.maximum_percent = 100.0;
    snapshot.cpu_summary.average_percent = 50.0;
    snapshot.cpu_summary.imbalance_percent_points = 100.0;

    CandidateSelectorConfig selector_config = candidate_selector_default_config();
    MigrationCandidate candidate;
    memset(&candidate, 0, sizeof(candidate));
    CandidateSelectDiagnostics diagnostics;
    CandidateSelectStatus status = candidate_select(
        &snapshot, &selector_config, &candidate, &diagnostics);

    ok &= expect("selector finds eligible thread-destination pair",
                 status == CANDIDATE_SELECT_FOUND &&
                 diagnostics.reason == CANDIDATE_REASON_FOUND);
    ok &= expect("selected destination is permitted and less loaded",
                 status == CANDIDATE_SELECT_FOUND &&
                 candidate.source_cpu == source_cpu &&
                 candidate.destination_cpu == destination_cpu);
    ok &= expect("projected whole-system spread improves",
                 status == CANDIDATE_SELECT_FOUND &&
                 candidate.system_imbalance_estimate_valid &&
                 candidate.estimated_system_imbalance_after_percent_points <
                     candidate.estimated_system_imbalance_before_percent_points);

    BenefitCostPolicyConfig policy_config = benefit_cost_policy_default_config();
    BenefitCostResult result;
    BenefitCostDecision decision = status == CANDIDATE_SELECT_FOUND
        ? benefit_cost_evaluate(&snapshot.cpu_summary, &candidate,
                                &policy_config, &result)
        : BENEFIT_COST_ERROR;
    ok &= expect("end-to-end selector plus policy approves beneficial move",
                 decision == BENEFIT_COST_APPROVE);
    if (decision == BENEFIT_COST_APPROVE) {
        printf("PASS DETAIL: %d->%d before=%.2f after=%.2f benefit=%.2f cost=%.2f score=%.2f\n",
               candidate.source_cpu, candidate.destination_cpu,
               result.imbalance_before_percent_points,
               result.estimated_imbalance_after_percent_points,
               result.estimated_benefit_percent_points,
               result.estimated_migration_cost_percent_points,
               result.score_percent_points);
    }

    /* Edge 1: one allowed CPU means no legal destination. */
    cpu_set_t source_only;
    CPU_ZERO(&source_only);
    CPU_SET(source_cpu, &source_only);
    if (sched_setaffinity(0, sizeof(source_only), &source_only) != 0) {
        perror("sched_setaffinity(source only)");
        (void)sched_setaffinity(0, sizeof(original_allowed), &original_allowed);
        return 1;
    }
    status = candidate_select(&snapshot, &selector_config, &candidate, &diagnostics);
    ok &= expect("single-CPU affinity safely rejects migration",
                 status == CANDIDATE_SELECT_NONE &&
                 diagnostics.reason == CANDIDATE_REASON_NO_ALLOWED_DESTINATION);

    /* Restore two-CPU mask for the remaining pure policy edge cases. */
    if (sched_setaffinity(0, sizeof(test_allowed), &test_allowed) != 0) {
        perror("sched_setaffinity(restore test mask)");
        (void)sched_setaffinity(0, sizeof(original_allowed), &original_allowed);
        return 1;
    }

    /* Edge 2: transferring a whole 100% thread just relocates the hotspot. */
    candidate = (MigrationCandidate){0};
    candidate.pid = getpid();
    candidate.tid = (pid_t)syscall(SYS_gettid);
    candidate.source_cpu = source_cpu;
    candidate.destination_cpu = destination_cpu;
    candidate.thread_utilization_percent = 100.0;
    candidate.locality_score = LOCALITY_SCORE_UNKNOWN;
    candidate.system_imbalance_estimate_valid = 1;
    candidate.estimated_system_imbalance_before_percent_points = 50.0;
    candidate.estimated_system_imbalance_after_percent_points = 50.0;
    decision = benefit_cost_evaluate(&snapshot.cpu_summary, &candidate,
                                    &policy_config, &result);
    ok &= expect("zero-benefit hotspot relocation is rejected",
                 decision == BENEFIT_COST_REJECT_LOW_BENEFIT);

    /* Edge 3: valid projected benefit must still cover the modeled cost. */
    candidate.thread_utilization_percent = 50.0;
    candidate.estimated_system_imbalance_before_percent_points = 50.0;
    candidate.estimated_system_imbalance_after_percent_points = 0.0;
    policy_config.base_migration_cost_percent_points = 100.0;
    decision = benefit_cost_evaluate(&snapshot.cpu_summary, &candidate,
                                    &policy_config, &result);
    ok &= expect("high migration cost rejects otherwise beneficial move",
                 decision == BENEFIT_COST_REJECT_COST);

    if (sched_setaffinity(0, sizeof(original_allowed), &original_allowed) != 0) {
        perror("sched_setaffinity(restore original mask)");
        return 1;
    }

    if (!ok) return 1;
    puts("PASS: end-to-end positive decision and migration safety edge cases");
    return 0;
}
