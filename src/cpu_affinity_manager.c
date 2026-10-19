
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "monitor_data.h"
#include "imbalance_detector.h"
#include "candidate_selector.h"
#include "benefit_cost_policy.h"
#include "locality_policy.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>

#define DEFAULT_INTERVAL_MS 1000
#define ACTION_COOLDOWN_SECONDS 10.0
#define MAX_ACTIONS_PER_MINUTE 6

static volatile sig_atomic_t keep_running = 1;

/* Stop the main loop safely when Ctrl+C or SIGTERM is received. */
static void handle_signal(int signo)
{
    (void)signo;
    keep_running = 0;
}

/* Return elapsed time in seconds: a - b. */
static double timespec_diff_seconds(struct timespec a, struct timespec b)
{
    return (double)(a.tv_sec - b.tv_sec) +
           (double)(a.tv_nsec - b.tv_nsec) / 1e9;
}

/* Parse an integer and reject invalid or out-of-range sampling intervals. */
static const char *candidate_reason_text(CandidateSelectReason reason)
{
    switch (reason) {
    case CANDIDATE_REASON_FOUND:
        return "candidate found";
    case CANDIDATE_REASON_INVALID_INPUT:
        return "invalid selector input";
    case CANDIDATE_REASON_ALREADY_BALANCED:
        return "CPU samples show no imbalance";
    case CANDIDATE_REASON_NO_ELIGIBLE_THREAD:
        return "no thread met the candidate eligibility requirements";
    case CANDIDATE_REASON_AFFINITY_READ_FAILED:
        return "could not read thread affinity";
    case CANDIDATE_REASON_NO_ALLOWED_DESTINATION:
        return "no lower-utilization destination is allowed by an eligible thread's affinity mask";
    case CANDIDATE_REASON_PROJECTION_FAILED:
        return "could not estimate projected system-wide imbalance";
    case CANDIDATE_REASON_NOT_SET:
    default:
        return "selector did not provide a reason";
    }
}

static const char *candidate_pair_reason_text(CandidatePairReason reason)
{
    switch (reason) {
    case CANDIDATE_PAIR_REASON_DESTINATION_NOT_LOWER:
        return "destination_not_lower_than_last_cpu";
    case CANDIDATE_PAIR_REASON_INELIGIBLE:
        return "candidate_failed_eligibility";
    case CANDIDATE_PAIR_REASON_PROJECTION_FAILED:
        return "projection_failed_or_implausible";
    case CANDIDATE_PAIR_REASON_SCORED:
        return "pair_scored_but_not_selected";
    case CANDIDATE_PAIR_REASON_NONE:
    default:
        return "no_pair_reason";
    }
}

static int parse_positive_int(const char *text, int *value)
{
    if (text == NULL || value == NULL || *text == '\0')
        return -1;

    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' ||
        parsed <= 0 || parsed > 60000)
        return -1;

    *value = (int)parsed;
    return 0;
}

/*
 * Read field 22 (starttime) from /proc/<PID>/task/<TID>/stat.
 * Using the final ')' handles task names that contain ')' characters.
 * The start time helps distinguish a reused TID from the original thread.
 */
static int read_task_start_ticks(pid_t pid, pid_t tid,
                                 unsigned long long *start_ticks)
{
    if (start_ticks == NULL) {
        errno = EINVAL;
        return -1;
    }

    char path[128];
    int path_length = snprintf(path, sizeof(path),
                               "/proc/%ld/task/%ld/stat",
                               (long)pid, (long)tid);

    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL)
        return -1;

    char *line = NULL;
    size_t capacity = 0;
    ssize_t length = getline(&line, &capacity, file);
    int saved_errno = errno;
    fclose(file);

    if (length < 0) {
        free(line);
        errno = saved_errno ? saved_errno : EIO;
        return -1;
    }

    char *right_paren = strrchr(line, ')');

    if (right_paren == NULL || right_paren[1] == '\0') {
        free(line);
        errno = EPROTO;
        return -1;
    }

    char *save = NULL;
    char *token = strtok_r(right_paren + 1, " \t\n", &save);

    /* The first token after ')' is field 3; starttime is field 22. */
    for (int field = 3; token != NULL && field < 22; ++field)
        token = strtok_r(NULL, " \t\n", &save);

    if (token == NULL) {
        free(line);
        errno = EPROTO;
        return -1;
    }

    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(token, &end, 10);

    if (errno != 0 || end == token || *end != '\0') {
        free(line);
        errno = EPROTO;
        return -1;
    }

    *start_ticks = parsed;
    free(line);
    return 0;
}

/*
 * Revalidate thread identity and current affinity immediately before changing
 * anything. This helps avoid acting on an exited/reused TID or a stale choice.
 */
static int revalidate_candidate(const MigrationCandidate *candidate,
                                cpu_set_t *allowed)
{
    if (candidate == NULL || allowed == NULL) {
        errno = EINVAL;
        return -1;
    }

    unsigned long long current_start_ticks = 0;

    if (read_task_start_ticks(candidate->pid, candidate->tid,
                              &current_start_ticks) != 0)
        return -1;

    if (current_start_ticks != candidate->start_ticks) {
        errno = ESRCH;
        return -1;
    }

    char task_path[128];
    int task_path_length = snprintf(
        task_path, sizeof(task_path), "/proc/%ld/task/%ld",
        (long)candidate->pid, (long)candidate->tid);
    struct stat task_info;
    if (task_path_length < 0 ||
        (size_t)task_path_length >= sizeof(task_path) ||
        stat(task_path, &task_info) != 0)
        return -1;
    if (task_info.st_uid != geteuid()) {
        errno = EPERM;
        return -1;
    }

    CPU_ZERO(allowed);

    if (sched_getaffinity(candidate->tid, sizeof(*allowed), allowed) != 0)
        return -1;

    if (candidate->destination_cpu < 0 ||
        candidate->destination_cpu >= CPU_SETSIZE ||
        !CPU_ISSET(candidate->destination_cpu, allowed) ||
        CPU_COUNT(allowed) < 2) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

/* Enforce the maximum number of successful affinity changes per minute. */
static int action_rate_limited(
    const struct timespec action_times[MAX_ACTIONS_PER_MINUTE],
    size_t action_count, struct timespec now)
{
    size_t recent = 0;

    for (size_t i = 0; i < action_count; ++i) {
        double age = timespec_diff_seconds(now, action_times[i]);

        /* Ignore future timestamps if the clock data is unexpected. */
        if (age >= 0.0 && age < 60.0)
            ++recent;
    }

    return recent >= MAX_ACTIONS_PER_MINUTE;
}

/* Record a successful change, retaining only the latest fixed-size history. */
static void record_action_time(
    struct timespec action_times[MAX_ACTIONS_PER_MINUTE],
    size_t *action_count, struct timespec now)
{
    if (*action_count < MAX_ACTIONS_PER_MINUTE) {
        action_times[(*action_count)++] = now;
        return;
    }

    memmove(&action_times[0], &action_times[1],
            (MAX_ACTIONS_PER_MINUTE - 1) * sizeof(action_times[0]));

    action_times[MAX_ACTIONS_PER_MINUTE - 1] = now;
}

static void print_usage(const char *program)
{
    printf("Usage: %s [--dry-run | --apply] [--interval-ms N]\n",
           program);
    puts("  --dry-run       Report proposed changes only (default)");
    puts("  --apply         Pin an approved TID to its destination CPU");
    puts("  --interval-ms N Sampling interval, 100..60000 ms (default 1000)");
    puts("  --help          Show this help");
}

int main(int argc, char **argv)
{
    int apply_changes = 0;
    int interval_ms = DEFAULT_INTERVAL_MS;

    /* Dry-run is the default so the controller does not alter affinity by accident. */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            apply_changes = 0;
        } else if (strcmp(argv[i], "--apply") == 0) {
            apply_changes = 1;
        } else if (strcmp(argv[i], "--interval-ms") == 0 &&
                   i + 1 < argc) {
            if (parse_positive_int(argv[++i], &interval_ms) != 0 ||
                interval_ms < 100) {
                fprintf(stderr,
                        "Invalid interval; use 100..60000 ms.\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown or incomplete argument: %s\n",
                    argv[i]);
            print_usage(argv[0]);
            return 2;
        }
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    ImbalanceDetector detector;
    ImbalanceDetectorConfig detector_config =
        imbalance_detector_default_config();

    if (imbalance_detector_init(&detector, &detector_config) != 0) {
        perror("imbalance_detector_init");
        return 1;
    }

    /* Discover cache-sharing topology once; unknown locality remains conservative. */
    LocalityTopology topology;
    locality_topology_init(&topology);

    LocalityStatus locality_status =
        locality_topology_discover(&topology, NULL);

    fprintf(stderr,
            "[startup] mode=%s interval=%dms locality_status=%d "
            "discovered_cpus=%zu\n",
            apply_changes ? "APPLY" : "DRY-RUN",
            interval_ms, (int)locality_status,
            topology.discovered_cpu_count);

    if (apply_changes) {
        fprintf(stderr,
                "[warning] --apply pins the selected TID to one CPU. "
                "Use only on workloads you control.\n");
    }

    BenefitCostPolicyConfig cost_config =
        benefit_cost_policy_default_config();

    CandidateSelectorConfig selector_config =
        candidate_selector_default_config();

    struct timespec action_times[MAX_ACTIONS_PER_MINUTE];
    size_t action_count = 0;
    struct timespec last_action_at = {0, 0};
    int has_last_action = 0;

    MonitorSnapshot previous, current;
    monitor_snapshot_init(&previous);
    monitor_snapshot_init(&current);

    MonitorStatus first_status =
        monitor_collect_snapshot(NULL, &previous);

    if (first_status == MONITOR_STATUS_ERROR) {
        perror("initial monitor snapshot");
        monitor_snapshot_destroy(&previous);
        return 1;
    }

    fprintf(stderr,
            "[monitor] baseline collected: CPUs=%zu threads=%zu status=%d\n",
            previous.cpu_count, previous.thread_count,
            (int)first_status);

    while (keep_running) {
        struct timespec delay = {
            interval_ms / 1000,
            (long)(interval_ms % 1000) * 1000000L
        };

        while (nanosleep(&delay, &delay) != 0 &&
               errno == EINTR && keep_running) {
        }

        if (!keep_running)
            break;

        monitor_snapshot_init(&current);

        MonitorStatus monitor_status =
            monitor_collect_snapshot(&previous, &current);

        if (monitor_status == MONITOR_STATUS_ERROR) {
            fprintf(stderr, "[monitor] snapshot failed: %s\n",
                    strerror(errno));
            monitor_snapshot_destroy(&current);
            continue;
        }

        if (current.cpu_summary.valid) {
            fprintf(stderr,
                    "[sample] avg=%.1f%% min=%.1f%% max=%.1f%% "
                    "imbalance=%.1fpp cpus=%zu threads=%zu status=%d\n",
                    current.cpu_summary.average_percent,
                    current.cpu_summary.minimum_percent,
                    current.cpu_summary.maximum_percent,
                    current.cpu_summary.imbalance_percent_points,
                    current.cpu_summary.valid_cpu_count,
                    current.thread_count, (int)monitor_status);
        }

        /* The detector filters out brief spikes using persistence and hysteresis. */
        ImbalanceDetectorResult detection =
            imbalance_detector_update(&detector,
                                      &current.cpu_summary,
                                      current.sampled_at);

        if (detection == IMBALANCE_RESULT_DETECTED) {
            MigrationCandidate candidate;
            memset(&candidate, 0, sizeof(candidate));

            CandidateSelectDiagnostics selector_diagnostics;
            CandidateSelectStatus selected =
                candidate_select_policy_aware(
                    &current, &selector_config, &topology, &cost_config,
                    &candidate, &selector_diagnostics);

            if (selected == CANDIDATE_SELECT_FOUND) {
                BenefitCostResult evaluation;

                /*
                 * FIX: pass the original, unscaled cost_config.
                 * This avoids counting locality penalties twice.
                 */
                BenefitCostDecision decision =
                    benefit_cost_evaluate(&current.cpu_summary,
                                          &candidate,
                                          &cost_config,
                                          &evaluation);

                fprintf(stderr,
                        "[decision] tid=%ld pid=%ld %d->%d "
                        "source_basis=last_cpu_estimate "
                        "thread=%.1f%% cpu_load=%.1f%%->%.1f%% "
                        "projected_spread=%.2f->%.2fpp "
                 "allowed_cpus=%zu locality=%d "
                        "benefit=%.1fpp cost=%.1fpp score=%.1fpp "
                        "result=%d reason=\"%s\"\n",
                        (long)candidate.tid, (long)candidate.pid,
                        candidate.source_cpu, candidate.destination_cpu,
                        candidate.thread_utilization_percent,
                        candidate.source_cpu_utilization_percent,
                        candidate.destination_cpu_utilization_percent,
                        candidate.estimated_system_imbalance_before_percent_points,
                        candidate.estimated_system_imbalance_after_percent_points,
                        candidate.allowed_cpu_count,
                        candidate.locality_score,
                        evaluation.estimated_benefit_percent_points,
                        evaluation.estimated_migration_cost_percent_points,
                        evaluation.score_percent_points,
                        (int)decision, evaluation.reason);

                if (decision == BENEFIT_COST_APPROVE) {
                    struct timespec now;

                    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
                        perror("clock_gettime");
                    } else {
                        int cooldown_ok =
                            !has_last_action ||
                            timespec_diff_seconds(now, last_action_at) >=
                                ACTION_COOLDOWN_SECONDS;

                        if (!cooldown_ok) {
                            fprintf(stderr,
                                    "[action] skipped: controller cooldown active\n");
                        } else if (action_rate_limited(action_times,
                                                       action_count, now)) {
                            fprintf(stderr,
                                    "[action] skipped: maximum %d changes/minute reached\n",
                                    MAX_ACTIONS_PER_MINUTE);
                        } else if (!apply_changes) {
                            fprintf(stderr,
                                    "[dry-run] would set TID %ld affinity to CPU %d\n",
                                    (long)candidate.tid,
                                    candidate.destination_cpu);
                        } else {
                            cpu_set_t fresh_allowed;

                            if (revalidate_candidate(&candidate,
                                                     &fresh_allowed) != 0) {
                                fprintf(stderr,
                                        "[action] revalidation rejected TID %ld: %s\n",
                                        (long)candidate.tid,
                                        strerror(errno));
                            } else {
                                cpu_set_t target;
                                CPU_ZERO(&target);
                                CPU_SET(candidate.destination_cpu, &target);

                                if (sched_setaffinity(candidate.tid,
                                                      sizeof(target),
                                                      &target) != 0) {
                                    fprintf(stderr,
                                            "[action] sched_setaffinity failed "
                                            "for TID %ld: %s\n",
                                            (long)candidate.tid,
                                            strerror(errno));
                                } else {
                                    last_action_at = now;
                                    has_last_action = 1;

                                    record_action_time(action_times,
                                                       &action_count, now);

                                    fprintf(stderr,
                                            "[action] applied: TID %ld pinned "
                                            "to CPU %d\n",
                                            (long)candidate.tid,
                                            candidate.destination_cpu);
                                }
                            }
                        }
                    }
                }
            } else {
                if (selector_diagnostics.system_error != 0) {
                    fprintf(stderr,
                            "[decision] persistent imbalance; selector status=%d "
                            "reason=\"%s\" system_error=%s\n",
                            (int)selected,
                            candidate_reason_text(selector_diagnostics.reason),
                            strerror(selector_diagnostics.system_error));
                } else {
                    fprintf(stderr,
                            "[decision] persistent imbalance; selector status=%d "
                            "reason=\"%s\"\n",
                            (int)selected,
                            candidate_reason_text(selector_diagnostics.reason));
                }
                if (selector_diagnostics.diagnostic_pair_valid) {
                    fprintf(stderr,
                            "[selector-pair] pid=%ld tid=%ld "
                            "source=%d(last_cpu_estimate) destination=%d "
                            "thread=%.1f%% cpu_load=%.1f%%->%.1f%% "
                            "reason=%s\n",
                            (long)selector_diagnostics.diagnostic_pid,
                            (long)selector_diagnostics.diagnostic_tid,
                            selector_diagnostics.diagnostic_source_cpu,
                            selector_diagnostics.diagnostic_destination_cpu,
                            selector_diagnostics.diagnostic_thread_utilization_percent,
                            selector_diagnostics.diagnostic_source_cpu_utilization_percent,
                            selector_diagnostics.diagnostic_destination_cpu_utilization_percent,
                            candidate_pair_reason_text(
                                selector_diagnostics.diagnostic_pair_reason));
                }
                fprintf(stderr,
                        "[selector-debug] threads=%zu invalid_or_low_util=%zu "
                        "invalid_last_cpu=%zu missing_source_sample=%zu "
                        "affinity_fail=%zu mask_lt2=%zu source_not_allowed=%zu "
                        "allowed_dest=%zu dest_not_lower=%zu lower_dest=%zu "
                        "not_owned=%zu eligibility_reject=%zu "
                        "projection_fail=%zu scored=%zu\n",
                        selector_diagnostics.threads_seen,
                        selector_diagnostics.threads_rejected_invalid_or_low_utilization,
                        selector_diagnostics.threads_rejected_invalid_last_cpu,
                        selector_diagnostics.threads_rejected_missing_source_sample,
                        selector_diagnostics.affinity_read_failures,
                        selector_diagnostics.threads_rejected_affinity_mask_too_narrow,
                        selector_diagnostics.threads_rejected_source_not_allowed,
                        selector_diagnostics.allowed_destinations_seen,
                        selector_diagnostics.destinations_rejected_not_lower,
                        selector_diagnostics.lower_utilization_destinations_seen,
                        selector_diagnostics.threads_rejected_not_owned,
                        selector_diagnostics.eligibility_rejections,
                        selector_diagnostics.projection_failures,
                        selector_diagnostics.candidates_scored);
            }
        }

        monitor_snapshot_destroy(&previous);
        previous = current;
        monitor_snapshot_init(&current);
    }

    monitor_snapshot_destroy(&previous);
    monitor_snapshot_destroy(&current);

    fprintf(stderr, "[shutdown] controller stopped\n");
    return 0;
}
