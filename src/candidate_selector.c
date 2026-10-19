
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "candidate_selector.h"
#include "benefit_cost_policy.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

CandidateSelectorConfig candidate_selector_default_config(void)
{
    CandidateSelectorConfig config = {
        .minimum_thread_utilization_percent = 5.0
    };

    return config;
}

static int valid_cpu_id(int cpu_id)
{
    return cpu_id >= 0 && cpu_id < CPU_SETSIZE;
}

static int valid_cpu_sample(const CpuSample *cpu)
{
    return cpu != NULL &&
           cpu->utilization_valid &&
           valid_cpu_id(cpu->cpu_id) &&
           isfinite(cpu->utilization_percent) &&
           cpu->utilization_percent >= 0.0 &&
           cpu->utilization_percent <= 100.0;
}

static const CpuSample *find_cpu_sample(
    const MonitorSnapshot *snapshot,
    int cpu_id)
{
    for (size_t i = 0; i < snapshot->cpu_count; ++i) {
        if (snapshot->cpus[i].cpu_id == cpu_id &&
            valid_cpu_sample(&snapshot->cpus[i])) {
            return &snapshot->cpus[i];
        }
    }

    return NULL;
}

static int task_owned_by_effective_user(pid_t pid, pid_t tid)
{
    char path[128];
    int length = snprintf(path, sizeof(path), "/proc/%ld/task/%ld",
                          (long)pid, (long)tid);
    if (length < 0 || (size_t)length >= sizeof(path))
        return 0;

    struct stat task_info;
    return stat(path, &task_info) == 0 && task_info.st_uid == geteuid();
}

/*
 * Compute the population standard deviation of valid per-CPU utilization.
 *
 * If projected == 1, estimate the result after transferring the measured
 * thread utilization from source_cpu to destination_cpu. Standard deviation
 * is used instead of max-minus-min because an unrelated idle CPU can keep the
 * range at 100 percentage points even when a move reduces a hot CPU's load.
 *
 * This remains a first-order heuristic: /proc/<tid>/stat reports the last CPU
 * observed, not a complete per-CPU execution history for the sampling window.
 */
static int estimate_system_imbalance(
    const MonitorSnapshot *snapshot,
    int projected,
    int source_cpu,
    int destination_cpu,
    double thread_utilization,
    double *imbalance)
{
    if (snapshot == NULL || imbalance == NULL) {
        errno = EINVAL;
        return -1;
    }

    double sum = 0.0;
    size_t count = 0;
    double source_before = 0.0;
    double destination_before = 0.0;
    int source_found = 0;
    int destination_found = 0;

    for (size_t i = 0; i < snapshot->cpu_count; ++i) {
        const CpuSample *cpu = &snapshot->cpus[i];
        if (!valid_cpu_sample(cpu)) {
            continue;
        }

        double value = cpu->utilization_percent;
        if (cpu->cpu_id == source_cpu) {
            source_before = value;
            source_found = 1;
        }
        if (cpu->cpu_id == destination_cpu) {
            destination_before = value;
            destination_found = 1;
        }
        sum += value;
        ++count;
    }

    if (count < 2 ||
        (projected && (!source_found || !destination_found))) {
        errno = ENODATA;
        return -1;
    }

    /*
     * The thread's measured interval utilization is only attributable to
     * source_cpu as a first-order estimate based on /proc's last_cpu field.
     * If that estimate exceeds the source's entire observed utilization, or
     * cannot fit on the destination, the projection is not physically
     * plausible. Reject it instead of manufacturing a benefit by clamping.
     */
    if (projected &&
        (thread_utilization > source_before + 1e-9 ||
         destination_before + thread_utilization > 100.0 + 1e-9)) {
        errno = ERANGE;
        return -1;
    }

    double projected_sum = 0.0;
    for (size_t i = 0; i < snapshot->cpu_count; ++i) {
        const CpuSample *cpu = &snapshot->cpus[i];
        if (!valid_cpu_sample(cpu)) {
            continue;
        }

        double value = cpu->utilization_percent;
        if (projected && cpu->cpu_id == source_cpu) {
            value = source_before - thread_utilization;
        } else if (projected && cpu->cpu_id == destination_cpu) {
            value = destination_before + thread_utilization;
        }
        if (value < 0.0) value = 0.0;
        if (value > 100.0) value = 100.0;
        projected_sum += value;
    }

    double mean = projected_sum / (double)count;
    double sum_squared_deviations = 0.0;

    for (size_t i = 0; i < snapshot->cpu_count; ++i) {
        const CpuSample *cpu = &snapshot->cpus[i];
        if (!valid_cpu_sample(cpu)) {
            continue;
        }

        double value = cpu->utilization_percent;
        if (projected && cpu->cpu_id == source_cpu) {
            value = source_before - thread_utilization;
        } else if (projected && cpu->cpu_id == destination_cpu) {
            value = destination_before + thread_utilization;
        }
        if (value < 0.0) value = 0.0;
        if (value > 100.0) value = 100.0;

        double deviation = value - mean;
        sum_squared_deviations += deviation * deviation;
    }

    *imbalance = sqrt(sum_squared_deviations / (double)count);
    if (!isfinite(*imbalance)) {
        errno = ERANGE;
        return -1;
    }

    return 0;
}

int candidate_thread_is_eligible(
    const ThreadSample *thread,
    int source_cpu,
    int destination_cpu,
    const cpu_set_t *allowed_cpus,
    double minimum_utilization_percent)
{
    if (thread == NULL || allowed_cpus == NULL ||
        source_cpu == destination_cpu ||
        !valid_cpu_id(source_cpu) ||
        !valid_cpu_id(destination_cpu) ||
        !isfinite(minimum_utilization_percent) ||
        minimum_utilization_percent < 0.0 ||
        minimum_utilization_percent > 100.0) {
        return 0;
    }

    if (thread->pid <= 0 || thread->tid <= 0 ||
        thread->last_cpu != source_cpu ||
        !thread->utilization_valid ||
        !isfinite(thread->utilization_percent) ||
        thread->utilization_percent < 0.0 ||
        thread->utilization_percent > 100.0 ||
        thread->utilization_percent < minimum_utilization_percent) {
        return 0;
    }

    if (!CPU_ISSET(source_cpu, allowed_cpus) ||
        !CPU_ISSET(destination_cpu, allowed_cpus) ||
        CPU_COUNT(allowed_cpus) < 2) {
        return 0;
    }

    return 1;
}

static void set_diagnostics(
    CandidateSelectDiagnostics *diagnostics,
    CandidateSelectReason reason,
    int system_error)
{
    if (diagnostics != NULL) {
        diagnostics->reason = reason;
        diagnostics->system_error = system_error;
    }
}

CandidateSelectStatus candidate_select_policy_aware(
    const MonitorSnapshot *snapshot,
    const CandidateSelectorConfig *config,
    const LocalityTopology *topology,
    const BenefitCostPolicyConfig *policy_config,
    MigrationCandidate *candidate,
    CandidateSelectDiagnostics *diagnostics)
{
    if (diagnostics != NULL) {
        memset(diagnostics, 0, sizeof(*diagnostics));
        diagnostics->reason = CANDIDATE_REASON_NOT_SET;
    }

    if (snapshot == NULL || candidate == NULL ||
        !snapshot->cpu_summary.valid ||
        snapshot->cpu_summary.valid_cpu_count < 2 ||
        snapshot->cpus == NULL ||
        snapshot->cpu_count < 2 ||
        (snapshot->thread_count > 0 && snapshot->threads == NULL)) {
        errno = EINVAL;
        set_diagnostics(diagnostics, CANDIDATE_REASON_INVALID_INPUT, errno);
        return CANDIDATE_SELECT_ERROR;
    }

    CandidateSelectorConfig effective =
        config ? *config : candidate_selector_default_config();

    if (!isfinite(effective.minimum_thread_utilization_percent) ||
        effective.minimum_thread_utilization_percent < 0.0 ||
        effective.minimum_thread_utilization_percent > 100.0) {
        errno = EINVAL;
        set_diagnostics(diagnostics, CANDIDATE_REASON_INVALID_INPUT, errno);
        return CANDIDATE_SELECT_ERROR;
    }

    double imbalance_before = 0.0;

    if (estimate_system_imbalance(
            snapshot, 0, -1, -1, 0.0,
            &imbalance_before) != 0) {
        set_diagnostics(diagnostics, CANDIDATE_REASON_PROJECTION_FAILED, errno);
        return CANDIDATE_SELECT_ERROR;
    }

    /*
     * An already balanced system does not need a migration proposal.
     */
    if (imbalance_before <= 0.0) {
        errno = 0;
        set_diagnostics(diagnostics, CANDIDATE_REASON_ALREADY_BALANCED, 0);
        return CANDIDATE_SELECT_NONE;
    }

    int found = 0;
    int saw_source_sample_thread = 0;
    int saw_affinity_read_failure = 0;
    int saw_affinity_read_success = 0;
    int saw_valid_destination = 0;
    int affinity_error = 0;

    MigrationCandidate best;
    memset(&best, 0, sizeof(best));

    double best_score = -DBL_MAX;
    double best_thread_utilization = -1.0;

    for (size_t i = 0; i < snapshot->thread_count; ++i) {
        const ThreadSample *thread = &snapshot->threads[i];
        if (diagnostics != NULL) diagnostics->threads_seen++;

        if (thread->pid <= 0 || thread->tid <= 0 ||
            !thread->utilization_valid ||
            !isfinite(thread->utilization_percent) ||
            thread->utilization_percent < 0.0 ||
            thread->utilization_percent > 100.0 ||
            thread->utilization_percent <
                effective.minimum_thread_utilization_percent) {
            if (diagnostics != NULL)
                diagnostics->threads_rejected_invalid_or_low_utilization++;
            continue;
        }

        int source_cpu = thread->last_cpu;

        if (!valid_cpu_id(source_cpu)) {
            if (diagnostics != NULL)
                diagnostics->threads_rejected_invalid_last_cpu++;
            continue;
        }

        const CpuSample *source_sample =
            find_cpu_sample(snapshot, source_cpu);

        if (source_sample == NULL) {
            if (diagnostics != NULL)
                diagnostics->threads_rejected_missing_source_sample++;
            continue;
        }

        saw_source_sample_thread = 1;

        if (!task_owned_by_effective_user(thread->pid, thread->tid)) {
            if (diagnostics != NULL)
                diagnostics->threads_rejected_not_owned++;
            continue;
        }

        cpu_set_t allowed;
        CPU_ZERO(&allowed);

        if (sched_getaffinity(thread->tid, sizeof(allowed),
                              &allowed) != 0) {
            if (!saw_affinity_read_failure) {
                affinity_error = errno;
            }
            saw_affinity_read_failure = 1;
            if (diagnostics != NULL) diagnostics->affinity_read_failures++;
            continue;
        }
        saw_affinity_read_success = 1;

        if (CPU_COUNT(&allowed) < 2) {
            if (diagnostics != NULL)
                diagnostics->threads_rejected_affinity_mask_too_narrow++;
            continue;
        }
        if (!CPU_ISSET(source_cpu, &allowed)) {
            if (diagnostics != NULL)
                diagnostics->threads_rejected_source_not_allowed++;
            continue;
        }

        /*
         * Destination enumeration is thread-specific.
         * A CPU is considered only when it appears in this thread's
         * current allowed affinity mask and has a valid utilization sample.
         */
        for (size_t j = 0; j < snapshot->cpu_count; ++j) {
            const CpuSample *destination_sample =
                &snapshot->cpus[j];

            if (!valid_cpu_sample(destination_sample)) {
                continue;
            }

            int destination_cpu = destination_sample->cpu_id;

            if (destination_cpu == source_cpu ||
                !CPU_ISSET(destination_cpu, &allowed)) {
                continue;
            }
            if (diagnostics != NULL) diagnostics->allowed_destinations_seen++;

            int record_pair = diagnostics != NULL &&
                (!diagnostics->diagnostic_pair_valid ||
                 thread->utilization_percent >=
                    diagnostics->diagnostic_thread_utilization_percent);
            if (record_pair) {
                diagnostics->diagnostic_pair_valid = 1;
                diagnostics->diagnostic_pid = thread->pid;
                diagnostics->diagnostic_tid = thread->tid;
                diagnostics->diagnostic_source_cpu = source_cpu;
                diagnostics->diagnostic_destination_cpu = destination_cpu;
                diagnostics->diagnostic_thread_utilization_percent =
                    thread->utilization_percent;
                diagnostics->diagnostic_source_cpu_utilization_percent =
                    source_sample->utilization_percent;
                diagnostics->diagnostic_destination_cpu_utilization_percent =
                    destination_sample->utilization_percent;
                diagnostics->diagnostic_pair_reason =
                    CANDIDATE_PAIR_REASON_NONE;
            }

            /*
             * Require a strictly less-utilized destination.
             * Moving to an equally busy or busier CPU is not considered.
             */
            if (destination_sample->utilization_percent >=
                source_sample->utilization_percent) {
                if (diagnostics != NULL)
                    diagnostics->destinations_rejected_not_lower++;
                if (record_pair)
                    diagnostics->diagnostic_pair_reason =
                        CANDIDATE_PAIR_REASON_DESTINATION_NOT_LOWER;
                continue;
            }
            if (diagnostics != NULL)
                diagnostics->lower_utilization_destinations_seen++;

            if (!candidate_thread_is_eligible(
                    thread, source_cpu, destination_cpu, &allowed,
                    effective.minimum_thread_utilization_percent)) {
                if (diagnostics != NULL)
                    diagnostics->eligibility_rejections++;
                if (record_pair)
                    diagnostics->diagnostic_pair_reason =
                        CANDIDATE_PAIR_REASON_INELIGIBLE;
                continue;
            }

            saw_valid_destination = 1;

            double imbalance_after = 0.0;

            if (estimate_system_imbalance(
                    snapshot, 1, source_cpu, destination_cpu,
                    thread->utilization_percent,
                    &imbalance_after) != 0) {
                if (diagnostics != NULL) diagnostics->projection_failures++;
                if (record_pair)
                    diagnostics->diagnostic_pair_reason =
                        CANDIDATE_PAIR_REASON_PROJECTION_FAILED;
                continue;
            }

            if (diagnostics != NULL) diagnostics->candidates_scored++;
            if (record_pair)
                diagnostics->diagnostic_pair_reason =
                    CANDIDATE_PAIR_REASON_SCORED;
            MigrationCandidate proposed;
            memset(&proposed, 0, sizeof(proposed));
            proposed.pid = thread->pid;
            proposed.tid = thread->tid;
            proposed.start_ticks = thread->start_ticks;
            snprintf(proposed.name, sizeof(proposed.name), "%s",
                     thread->name);
            proposed.source_cpu = source_cpu;
            proposed.destination_cpu = destination_cpu;
            proposed.thread_utilization_percent =
                thread->utilization_percent;
            proposed.allowed_cpu_count = (size_t)CPU_COUNT(&allowed);
            proposed.source_cpu_utilization_percent =
                source_sample->utilization_percent;
            proposed.destination_cpu_utilization_percent =
                destination_sample->utilization_percent;
            proposed.cpu_utilization_valid = 1;
            proposed.estimated_system_imbalance_before_percent_points =
                imbalance_before;
            proposed.estimated_system_imbalance_after_percent_points =
                imbalance_after;
            proposed.system_imbalance_estimate_valid = 1;
            proposed.locality_score = topology != NULL
                ? (int)locality_score_destination(
                    topology, source_cpu, destination_cpu, &allowed)
                : LOCALITY_SCORE_UNKNOWN;

            BenefitCostResult evaluation;
            BenefitCostDecision policy_decision = benefit_cost_evaluate(
                &snapshot->cpu_summary, &proposed, policy_config,
                &evaluation);
            if (policy_decision == BENEFIT_COST_ERROR ||
                policy_decision == BENEFIT_COST_REJECT_INVALID) {
                if (diagnostics != NULL)
                    diagnostics->projection_failures++;
                if (record_pair)
                    diagnostics->diagnostic_pair_reason =
                        CANDIDATE_PAIR_REASON_PROJECTION_FAILED;
                continue;
            }

            double score = evaluation.score_percent_points;

            /*
             * Compare every valid pair using the actual configured policy.
             * This lets a slightly lower-benefit local destination win when
             * its lower modeled cost makes the overall decision better.
             */
            if (!found || score > best_score + 1e-9 ||
                (fabs(score - best_score) <= 1e-9 &&
                 thread->utilization_percent > best_thread_utilization)) {
                found = 1;
                best_score = score;
                best_thread_utilization =
                    thread->utilization_percent;
                best = proposed;
            }
        }
    }

    if (!found) {
        if (saw_valid_destination) {
            /*
             * Defensive fallback: a destination was seen but no estimate
             * could be retained.
             */
            errno = ENODATA;
            set_diagnostics(diagnostics,
                            CANDIDATE_REASON_PROJECTION_FAILED, errno);
        } else if (saw_source_sample_thread &&
                   saw_affinity_read_failure &&
                   !saw_affinity_read_success) {
            errno = affinity_error ? affinity_error : ESRCH;
            set_diagnostics(diagnostics,
                            CANDIDATE_REASON_AFFINITY_READ_FAILED, errno);
        } else if (!saw_source_sample_thread) {
            errno = 0;
            set_diagnostics(diagnostics,
                            CANDIDATE_REASON_NO_ELIGIBLE_THREAD, 0);
        } else {
            /* No syscall failed: diagnostics counters identify whether
             * affinity masks, CPU placement, or utilization blocked a move. */
            errno = 0;
            set_diagnostics(diagnostics,
                            CANDIDATE_REASON_NO_ALLOWED_DESTINATION, 0);
        }

        return CANDIDATE_SELECT_NONE;
    }

    *candidate = best;
    errno = 0;
    set_diagnostics(diagnostics, CANDIDATE_REASON_FOUND, 0);
    return CANDIDATE_SELECT_FOUND;
}

CandidateSelectStatus candidate_select(
    const MonitorSnapshot *snapshot,
    const CandidateSelectorConfig *config,
    MigrationCandidate *candidate,
    CandidateSelectDiagnostics *diagnostics)
{
    return candidate_select_policy_aware(snapshot, config, NULL, NULL,
                                         candidate, diagnostics);
}
