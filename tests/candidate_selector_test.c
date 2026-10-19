
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "candidate_selector.h"

#include <errno.h>
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

static int test_affinity_constrained_destination(void)
{
    int ok = 1;
    cpu_set_t original_allowed;

    if (sched_getaffinity(0, sizeof(original_allowed),
                          &original_allowed) != 0) {
        perror("sched_getaffinity");
        return 0;
    }

    int selected_cpus[3];
    size_t selected_count = 0;

    for (int cpu = 0; cpu < CPU_SETSIZE && selected_count < 3; ++cpu) {
        if (CPU_ISSET(cpu, &original_allowed)) {
            selected_cpus[selected_count++] = cpu;
        }
    }

    if (selected_count < 3) {
        puts("SKIP: affinity-constrained projection test needs 3 allowed CPUs");
        return 1;
    }

    int source_cpu = selected_cpus[0];
    int allowed_destination = selected_cpus[1];
    int globally_least_busy_cpu = selected_cpus[2];
    int other_busy_cpu = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &original_allowed) &&
            cpu != source_cpu && cpu != allowed_destination &&
            cpu != globally_least_busy_cpu) {
            other_busy_cpu = cpu;
            break;
        }
    }
    if (other_busy_cpu < 0) {
        puts("SKIP: could not find a synthetic fourth CPU ID");
        return 1;
    }

    cpu_set_t restricted;
    CPU_ZERO(&restricted);
    CPU_SET(source_cpu, &restricted);
    CPU_SET(allowed_destination, &restricted);

    if (sched_setaffinity(0, sizeof(restricted), &restricted) != 0) {
        perror("sched_setaffinity (test restriction)");
        return 0;
    }

    CpuSample cpus[4];
    memset(cpus, 0, sizeof(cpus));

    cpus[0].cpu_id = source_cpu;
    cpus[0].utilization_percent = 100.0;
    cpus[0].utilization_valid = 1;

    cpus[1].cpu_id = allowed_destination;
    cpus[1].utilization_percent = 50.0;
    cpus[1].utilization_valid = 1;

    cpus[2].cpu_id = globally_least_busy_cpu;
    cpus[2].utilization_percent = 0.0;
    cpus[2].utilization_valid = 1;

    cpus[3].cpu_id = other_busy_cpu;
    cpus[3].utilization_percent = 100.0;
    cpus[3].utilization_valid = 1;

    ThreadSample thread;
    memset(&thread, 0, sizeof(thread));
    thread.pid = getpid();
    thread.tid = (pid_t)syscall(SYS_gettid);
    thread.last_cpu = source_cpu;
    thread.utilization_percent = 25.0;
    thread.utilization_valid = 1;
    snprintf(thread.name, sizeof(thread.name), "affinity-test-worker");

    MonitorSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.cpus = cpus;
    snapshot.cpu_count = 4;
    snapshot.threads = &thread;
    snapshot.thread_count = 1;
    snapshot.cpu_summary.valid = 1;
    snapshot.cpu_summary.valid_cpu_count = 4;
    snapshot.cpu_summary.minimum_percent = 0.0;
    snapshot.cpu_summary.maximum_percent = 100.0;
    snapshot.cpu_summary.average_percent = 62.5;
    snapshot.cpu_summary.imbalance_percent_points = 100.0;

    CandidateSelectorConfig config = candidate_selector_default_config();
    MigrationCandidate candidate;
    memset(&candidate, 0, sizeof(candidate));
    CandidateSelectDiagnostics diagnostics;

    CandidateSelectStatus status =
        candidate_select(&snapshot, &config, &candidate, &diagnostics);

    ok &= expect("successful selection reports FOUND reason",
                 status == CANDIDATE_SELECT_FOUND &&
                 diagnostics.reason == CANDIDATE_REASON_FOUND &&
                 diagnostics.system_error == 0 &&
                 diagnostics.diagnostic_pair_valid &&
                 diagnostics.diagnostic_pair_reason ==
                     CANDIDATE_PAIR_REASON_SCORED);

    cpus[1].utilization_percent = 100.0;
    CandidateSelectStatus busier_destination_status =
        candidate_select(&snapshot, &config, &candidate, &diagnostics);
    ok &= expect("diagnostics retain the candidate pair rejected by load",
                 busier_destination_status == CANDIDATE_SELECT_NONE &&
                 diagnostics.diagnostic_pair_valid &&
                 diagnostics.diagnostic_pid == thread.pid &&
                 diagnostics.diagnostic_tid == thread.tid &&
                 diagnostics.diagnostic_source_cpu == source_cpu &&
                 diagnostics.diagnostic_destination_cpu ==
                     allowed_destination &&
                 diagnostics.diagnostic_pair_reason ==
                     CANDIDATE_PAIR_REASON_DESTINATION_NOT_LOWER);
    cpus[1].utilization_percent = 50.0;

    /* A thread cannot be subtracted from a source CPU when its measured
       interval usage exceeds that CPU's entire observed utilization. */
    cpus[0].utilization_percent = 10.0;
    cpus[1].utilization_percent = 0.0;
    CandidateSelectStatus implausible_projection_status =
        candidate_select(&snapshot, &config, &candidate, &diagnostics);
    ok &= expect("projection rejects thread usage above source CPU load",
                 implausible_projection_status == CANDIDATE_SELECT_NONE &&
                 diagnostics.reason == CANDIDATE_REASON_PROJECTION_FAILED &&
                 diagnostics.projection_failures > 0);
    cpus[0].utilization_percent = 100.0;
    cpus[1].utilization_percent = 50.0;

    cpu_set_t source_only;
    CPU_ZERO(&source_only);
    CPU_SET(source_cpu, &source_only);
    if (sched_setaffinity(0, sizeof(source_only), &source_only) != 0) {
        perror("sched_setaffinity (single-CPU test restriction)");
        (void)sched_setaffinity(0, sizeof(original_allowed), &original_allowed);
        return 0;
    }

    CandidateSelectStatus no_destination_status =
        candidate_select(&snapshot, &config, &candidate, &diagnostics);
    ok &= expect("single-CPU affinity produces no candidate",
                 no_destination_status == CANDIDATE_SELECT_NONE);
    ok &= expect("no destination has a policy reason, not a syscall error",
                 diagnostics.reason == CANDIDATE_REASON_NO_ALLOWED_DESTINATION &&
                 diagnostics.system_error == 0);
    ok &= expect("diagnostics identify the single-CPU affinity rejection",
                 diagnostics.threads_seen == 1 &&
                 diagnostics.threads_rejected_affinity_mask_too_narrow == 1);

    int restore_result =
        sched_setaffinity(0, sizeof(original_allowed), &original_allowed);
    if (restore_result != 0) {
        perror("sched_setaffinity (restore)");
        return 0;
    }

    ok &= expect("affinity-constrained candidate is found",
                 status == CANDIDATE_SELECT_FOUND);

    if (status == CANDIDATE_SELECT_FOUND) {
        ok &= expect("destination is in the allowed affinity mask",
                     candidate.destination_cpu == allowed_destination);
        ok &= expect("global minimum outside affinity is not selected",
                     candidate.destination_cpu != globally_least_busy_cpu);
        ok &= expect("system-wide estimate is available",
                     candidate.system_imbalance_estimate_valid);
        ok &= expect("standard-deviation estimate improves although max-min remains 100pp",
                     candidate.system_imbalance_estimate_valid &&
                     candidate.estimated_system_imbalance_before_percent_points >
                         candidate.estimated_system_imbalance_after_percent_points &&
                     snapshot.cpu_summary.maximum_percent -
                         snapshot.cpu_summary.minimum_percent == 100.0);
    }

    return ok;
}

int main(void)
{
    ThreadSample thread;
    cpu_set_t allowed;
    int ok = 1;

    memset(&thread, 0, sizeof(thread));
    thread.pid = 100;
    thread.tid = 101;
    thread.last_cpu = 2;
    thread.utilization_valid = 1;
    thread.utilization_percent = 42.0;
    snprintf(thread.name, sizeof(thread.name), "synthetic-worker");

    CPU_ZERO(&allowed);
    CPU_SET(2, &allowed);
    CPU_SET(3, &allowed);

    ok &= expect("eligible task with source and destination allowed",
                 candidate_thread_is_eligible(
                     &thread, 2, 3, &allowed, 5.0));

    ok &= expect("reject task whose last CPU is not source",
                 !candidate_thread_is_eligible(
                     &thread, 1, 3, &allowed, 5.0));

    cpu_set_t only_source;
    CPU_ZERO(&only_source);
    CPU_SET(2, &only_source);

    ok &= expect("reject task if destination is not allowed",
                 !candidate_thread_is_eligible(
                     &thread, 2, 3, &only_source, 5.0));

    ok &= expect("reject task below utilization threshold",
                 !candidate_thread_is_eligible(
                     &thread, 2, 3, &allowed, 50.0));

    thread.utilization_valid = 0;

    ok &= expect("reject invalid utilization sample",
                 !candidate_thread_is_eligible(
                     &thread, 2, 3, &allowed, 5.0));

    thread.utilization_valid = 1;

    ok &= expect("reject identical source and destination",
                 !candidate_thread_is_eligible(
                     &thread, 2, 2, &allowed, 5.0));

    ok &= test_affinity_constrained_destination();

    if (!ok) {
        return 1;
    }

    puts("PASS: eligibility, affinity constraints, destination-aware selection, and selector diagnostics");
    return 0;
}
