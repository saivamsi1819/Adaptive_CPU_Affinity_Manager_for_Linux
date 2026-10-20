# DESIGN_REQUIREMENTS.md

# Adaptive CPU Affinity Manager for Linux: A Userspace Approach to Intelligent Load Balancing

## 1. Project Overview

The Adaptive CPU Affinity Manager is a userspace Linux controller that observes CPU and task activity and selectively adjusts CPU affinity to reduce CPU load imbalance.

The project does **not** replace the Linux kernel scheduler and does not modify the kernel. It uses existing Linux interfaces to monitor system activity and, when justified by its decision policy, change the CPU affinity of an eligible process or thread.

The goal is to investigate whether a userspace controller can improve CPU load distribution for selected workloads while accounting for migration overhead, cache locality, controller overhead, and the behavior of the existing Linux scheduler.

### 1.1 Problem Statement

Static CPU affinity and imperfect load distribution can result in underutilized CPU cores despite available runnable work.

### 1.2 Main Objective

Design, implement, and evaluate a userspace controller that:

- Monitors per-CPU utilization and process/thread activity.
- Detects persistent CPU load imbalance.
- Identifies suitable processes or threads for possible movement.
- Evaluates whether an affinity change is likely to improve load distribution.
- Accounts for migration overhead and cache-locality costs.
- Applies affinity changes selectively and safely.
- Evaluates the effectiveness and overhead of the controller against suitable baselines.

### 1.3 Research Focus

The project is not intended to demonstrate only `sched_setaffinity()`. Its main contribution is the decision policy built around monitoring, persistent imbalance detection, candidate eligibility, locality-aware scoring, benefit-versus-cost evaluation, safeguards, and empirical evaluation.

Any performance improvement must be demonstrated experimentally. Lower CPU imbalance alone does not prove higher throughput or shorter execution time.

---

## 2. Scope and Boundaries

### 2.1 Included

- Per-logical-CPU utilization monitoring.
- Process-level CPU usage estimation.
- Thread-level CPU usage estimation.
- Persistent imbalance detection.
- Hysteresis and cooldown.
- Candidate selection based on recent CPU usage and affinity constraints.
- CPU topology discovery and locality-aware destination preference.
- Benefit-versus-cost migration scoring.
- Process/thread affinity inspection and modification.
- Dry-run operation and decision logging.
- Safe handling of partial monitoring failures and task races.
- Configurable policy parameters.
- Repeatable performance and overhead evaluation.

### 2.2 Excluded

- Replacing or modifying the Linux kernel scheduler.
- Implementing a new kernel scheduling class.
- Guaranteeing that a userspace affinity change improves application performance.
- Treating an estimated CPU-utilization value as an exact prediction of the load removed from a particular CPU.
- Assuming that all threads belonging to a process share the same affinity.
- Assuming that cache-sharing information alone provides a complete model of CPU performance.

### 2.3 Linux Interface Boundary

The controller uses `/proc` for observations and Linux affinity system calls for actions.

The main affinity interfaces are:

- `sched_getaffinity()` — obtains the CPU affinity mask of a specific task.
- `sched_setaffinity()` — changes the CPU affinity mask of a specific task.

Linux affinity is task/thread-specific. Changing the affinity of one thread does not automatically change the affinity of every thread in its process. Process-level management must account for the individual threads and their allowed CPU sets.

The controller must operate only on processes and threads that the user owns or is otherwise authorized to manage.

---

## 3. Core Architecture

The intended architecture consists of the following modules.

### 3.1 CPU Monitoring

**Responsibilities:**

- Read cumulative per-CPU counters from `/proc/stat`.
- Sample counters over time.
- Calculate per-CPU utilization from counter deltas.
- Report average, minimum, maximum, and imbalance measurements.
- Identify invalid samples rather than interpreting invalid counter changes as genuine utilization.

### 3.2 Process Monitoring

**Responsibilities:**

- Read process information from `/proc/<PID>/stat`.
- Estimate process CPU usage across successive samples.
- Parse process names safely when they contain spaces or parentheses.
- Track process start time to reduce PID-reuse confusion.
- Handle processes that exit during collection.

### 3.3 Thread Monitoring

**Responsibilities:**

- Enumerate visible threads under `/proc/<PID>/task/`.
- Read per-thread information from `/proc/<PID>/task/<TID>/stat`.
- Estimate thread CPU usage across successive samples.
- Track process ID, thread ID, and thread start time where available.
- Handle thread exits, invalid counter deltas, and TID-reuse confusion.
- Provide structured observations for candidate selection.

Thread monitoring is an advanced extension of the same project architecture, not a separate project. The current controller uses thread-level observations for candidate selection and evaluates affinity changes in dry-run mode by default.

### 3.4 Shared Monitoring Data Interface

The shared monitoring interface is defined in:

- `src/monitor_data.h`
- `src/monitor_data.c`

The interface defines:

- `CpuSample`
- `ProcessSample`
- `ThreadSample`
- `CpuUtilizationSummary`
- `MonitorSnapshot`

It also provides snapshot lifecycle functions and the planned/implemented `monitor_collect_snapshot()` interface.

The shared collector allows future controller modules to consume structured monitoring data rather than scraping the terminal output of standalone programs.

The standalone CPU, process, and affinity-test programs remain independently runnable. Refactoring every standalone monitor to use the shared collector is a later, controlled change and must not be performed unnecessarily.

### 3.5 Load Imbalance Detector

**Responsibilities:**

- Consume valid CPU-utilization summaries.
- Detect sustained imbalance rather than reacting to a single sample.
- Require a configurable number of consecutive qualifying samples.
- Apply hysteresis when entering and leaving the imbalanced state.
- Apply cooldown to prevent repeated actions too close together.
- Reject or defer decisions when the monitoring data is invalid or insufficient.

### 3.6 Candidate Selector

**Responsibilities:**

- Identify a suitable process or thread for possible movement.
- Consider recent process/thread CPU usage.
- Inspect the task's current allowed CPU set.
- Verify that a destination CPU is valid and permitted.
- Enumerate destination CPUs in each thread's current allowed affinity mask.
- Require a valid, lower-utilization destination and valid thread utilization.
- Project the move against all valid CPU samples using population standard deviation, not only the source/destination pair or max-minus-min range.
- Account for thread-level eligibility when managing multithreaded processes.
- Reject candidates that are invalid, ineligible, or unlikely to help.
- Rank each valid thread/destination pair by the configured benefit-minus-cost score, including its locality score, so a lower-cost alternative can be selected when the raw benefit leader is too costly.

### 3.7 CPU Locality and Topology Policy

**Responsibilities:**

- Discover CPU topology from Linux sysfs when available.
- Identify CPUs that share relevant cache levels.
- Provide a locality-aware preference or score for candidate destinations.
- Penalize potential locality loss in the migration decision.
- Fall back safely when topology information is missing or incomplete.

This module provides a **userspace approximation of locality-aware placement**. It is inspired by the general principle of preferring placements that preserve useful cache locality; it does not reproduce the Linux kernel scheduler's internal soft-affinity behavior.

The controller's topology information and locality scores are heuristics, not proof of actual cache-miss costs or performance gains.

### 3.8 Benefit-versus-Cost Policy

**Responsibilities:**

- Prefer the candidate selector's whole-system before/after population-standard-deviation estimate.
- Estimate or approximate the cost of an affinity change.
- Scale locality penalty by the destination's topology score.
- Reject non-improving moves, moves below the minimum benefit, and moves whose score does not cover modeled cost.
- Allow scoring parameters to be configured and experimentally evaluated.

### 3.9 Affinity Controller

**Responsibilities:**

- Inspect current affinity before acting.
- Validate destination CPU IDs and allowed CPU sets.
- Apply affinity changes only to authorized tasks.
- Support dry-run mode.
- Log accepted, rejected, attempted, and failed decisions.
- Enforce cooldowns and rate limits.
- Handle affinity errors and task exits safely.
- Provide a way to stop or disable automatic control.

The controller must not assume an affinity change succeeded merely because the request was issued. Where appropriate, it should verify the resulting affinity.

### 3.10 Evaluation Module or Harness

**Responsibilities:**

- Run repeatable workload scenarios.
- Compare baseline scheduling, static affinity, and adaptive control.
- Collect CPU imbalance, execution time or throughput, affinity-change counts, and monitoring overhead.
- Preserve results across multiple runs.
- Report negative results and trade-offs honestly.

---

## 4. CPU Utilization Model

For each logical CPU \(i\), let the cumulative counters read from `/proc/stat` be sampled at times \(t-1\) and \(t\).

### 4.1 Total CPU Time

For the counter fields used by this implementation:

\[
Total_i = user + nice + system + idle +
iowait + irq + softirq + steal
\]

\[
\Delta Total_i = Total_i(t)-Total_i(t-1)
\]

### 4.2 Idle Time

\[
Idle_i = idle+iowait
\]

\[
\Delta Idle_i = Idle_i(t)-Idle_i(t-1)
\]

### 4.3 CPU Utilization

When the counter deltas are valid and \(\Delta Total_i>0\):

\[
U_i = 100\left(1-
\frac{\Delta Idle_i}{\Delta Total_i}\right)
\]

The implementation must validate counter relationships and clamp or reject results according to its documented validation policy so that reported per-logical-CPU utilization remains within 0–100%. For thread utilization, a one-accounting-tick overshoot is tolerated to account for tick quantization and per-thread read timestamps; it is clamped to 100%, while larger impossible overshoots are rejected rather than silently accepted.

### 4.4 Average Utilization

For \(N\) valid logical CPUs:

\[
U_{avg}=\frac{1}{N}\sum_{i=1}^{N}U_i
\]

### 4.5 Load Imbalance

\[
I=U_{max}-U_{min}
\]

where \(U_{max}\) and \(U_{min}\) are the maximum and minimum valid per-CPU utilization values for the same sampling interval.

This range is a simple imbalance indicator. It does not fully describe runnable-queue lengths, CPU capacity differences, or application performance.

### 4.6 Invalid Samples and Counter Safety

The monitoring implementation must account for:

- First samples without a valid prior baseline.
- Zero total-counter deltas.
- Decreasing counters or apparent counter resets.
- CPUs that disappear or are unavailable during sampling.
- Partial `/proc/stat` reads.
- Invalid or incomplete samples.

Invalid observations must not be silently treated as valid zero-utilization readings. The first snapshot establishes a baseline; utilization becomes valid only when matching cumulative counters and an elapsed sampling interval are available.

---

## 5. Process and Thread CPU Usage

Process and thread CPU usage are estimated from cumulative CPU-time counters across successive samples.

Conceptually, for a task with cumulative CPU time \(T_p\):

\[
\Delta T_p=T_p(t)-T_p(t-1)
\]

A normalized usage estimate is derived from the task's CPU-time delta and the elapsed sampling interval, using the appropriate clock units and conversion factors for the platform.

Process and thread utilization are normalized to one logical CPU. Therefore, a multithreaded process may report more than 100% process utilization when its threads execute concurrently on multiple CPUs.

### 5.1 Identity Matching

Snapshots must match:

- CPUs by CPU ID.
- Processes by PID plus process start time.
- Threads by PID plus TID plus thread start time, where available.

This reduces the risk of interpreting a newly created task as the same task represented by an earlier PID or TID.

### 5.2 Collection Races

A process or thread may exit between enumeration and reading its `/proc` entry. Such races are normal.

A disappearing process or thread should ordinarily invalidate that observation, not the entire monitoring snapshot, provided the remaining data can still be collected and validated.

### 5.3 Process and Thread Levels Are Alternatives

Process and thread utilization are alternative levels of detail.

**Do not add a process's utilization to the utilization of its constituent threads as if they were independent workloads.** That would double-count the same work.

Thread-level observations are particularly useful for multithreaded applications whose individual threads have different affinity masks or CPU usage patterns.

### 5.4 Current Matching Complexity

The initial snapshot-matching implementation uses linear searches through the previous snapshot for process and thread matches.

This approach is acceptable for the first implementation, but repeated linear searches can produce \(O(N^2)\) matching work as the number of observed tasks grows.

The evaluation must measure monitoring overhead. A more efficient lookup strategy may be introduced later if measurements show that the current implementation is too expensive. Optimization must preserve the task-identity and correctness requirements.

---

## 6. Shared Snapshot Lifecycle and Interface Rules

The shared collector is the integration boundary for monitoring data.

### 6.1 Snapshot Lifecycle

- Initialize snapshots with `monitor_snapshot_init()` before first use.
- Call `monitor_snapshot_destroy()` before reusing a populated snapshot.
- Maintain a previous valid snapshot when calculating deltas.
- Ensure the previous and current snapshots are not accidentally aliased or destroyed prematurely.
- Handle allocation and collection errors without using partially initialized data as if it were valid.

### 6.2 Timing

Use `CLOCK_MONOTONIC` for sampling elapsed time. Wall-clock adjustments must not create incorrect utilization intervals.

### 6.3 Collection-Level and Observation-Level Errors

Distinguish between:

- Collection-level failures that prevent the snapshot from being used.
- Partial observations, such as an individual process or thread disappearing.
- Invalid per-CPU deltas.
- Missing optional topology information.

A single disappearing task must not automatically invalidate every otherwise valid CPU observation.

### 6.4 Integration Rule

Future modules should consume the structured snapshot API or clearly defined shared interfaces.

They should not parse human-readable terminal output from `cpu_monitor`, `process_monitor`, or `thread_monitor` to recover monitoring data.

The standalone monitoring utilities should remain independently usable while the integrated controller is developed.

---

## 7. Persistent Imbalance Detection

A single utilization spike is insufficient evidence that an affinity change is needed.

The detector must use persistence, hysteresis, and cooldown.

### 7.1 Persistence

Let \(I_t\) be the measured imbalance for sample \(t\). The detector enters the imbalanced state only after the configured number of consecutive valid samples meets the entry condition.

For an entry threshold \(I_{enter}\):

\[
I_t \ge I_{enter}
\]

must hold for the configured number of consecutive qualifying samples.

Invalid samples must not be treated as evidence of persistent imbalance. The handling of invalid samples must be explicit and consistent; they should not silently increase the persistence count.

### 7.2 Hysteresis

Use different entry and exit thresholds:

\[
I_{exit}<I_{enter}
\]

The detector enters the imbalanced state at or above the entry threshold and exits only when the imbalance falls below the exit threshold according to the documented comparison rule.

This reduces rapid switching when utilization fluctuates near one threshold.

### 7.3 Cooldown

After an affinity change or another configured control event, suppress additional actions for a configurable cooldown period unless a deliberately documented safety rule requires otherwise.

Cooldown is intended to prevent oscillation and excessive affinity changes.

### 7.4 Starting Parameters

The following are initial experimental values, not universal optimal settings:

| Parameter | Initial value |
|---|---:|
| Sampling interval | Approximately 1 second |
| Imbalance entry threshold | 30 percentage points |
| Imbalance exit threshold | 20 percentage points |
| Persistence requirement | 3 consecutive valid samples |
| Cooldown | 5 seconds |

These values must remain configurable and should be tuned or evaluated using repeatable workloads.

---

## 8. Candidate Selection

A candidate is not eligible merely because it has high CPU usage.

The selector should evaluate the candidate's recent activity, affinity restrictions, destination eligibility, and potential impact on load distribution.

### 8.1 Required Checks

For every candidate:

1. Confirm that the process or thread observation is valid.
2. Confirm that task identity is sufficiently reliable.
3. Inspect its current affinity with `sched_getaffinity()`.
4. Confirm that the destination CPU exists and is available.
5. Confirm that the destination belongs to the task's allowed CPU set.
6. Estimate whether the move could reduce the observed imbalance.
7. Consider the expected cost, including locality penalties.
8. Reject a move if any required safety or eligibility condition fails.

### 8.2 Thread-Level Selection

For a thread candidate, selection must use that thread's recent CPU usage and affinity, not just the aggregate utilization of its parent process.

The controller must operate on the relevant Linux task/thread. A thread that is not allowed to run on the proposed destination must never be assigned there.

### 8.3 Conservative Selection

Candidate selection is an estimate, not a guarantee. The controller should prefer no action when the available observations are invalid, the benefit is uncertain, or the destination is not eligible.

---

## 9. CPU Topology and Locality-Aware Selection

### 9.1 Motivation

Moving a task to a different CPU may improve load distribution but can reduce cache locality. A move that reduces a utilization metric can still make an application slower.

The controller therefore includes a topology-aware preference as part of the migration policy.

### 9.2 Topology Discovery

Where available, inspect Linux sysfs CPU topology and cache-sharing information.

The locality policy should:

- Identify the relevant source and destination CPUs.
- Compare known cache-sharing relationships.
- Prefer destinations that preserve useful cache locality when other factors are comparable.
- Penalize moves that are expected to lose locality.
- Handle incomplete, unavailable, or unexpected topology information safely.

### 9.3 Locality Scoring

The locality module may assign relative scores to destination CPUs according to discovered cache-sharing relationships.

These scores are **heuristics for ranking choices**, not measured percentages of performance retained and not direct measurements of migration cost.

For example, a destination sharing a relevant cache with the source may receive a more favorable relative score than a destination that does not share that cache.

The actual score scale and its interpretation must be documented alongside the implementation.

### 9.4 Fallback Behavior

If topology discovery fails or returns incomplete information:

- Do not invent cache relationships.
- Mark locality information as unknown.
- Use the documented conservative fallback.
- Continue to enforce all affinity and destination-validity constraints.
- Avoid interpreting unknown locality as proof that a destination is either safe or harmful.

### 9.5 Soft Affinity Terminology

The project's locality preference is **soft-affinity-inspired**, not actual Linux kernel soft affinity.

The hard CPU mask enforced through `sched_setaffinity()` defines where a task is permitted to run. The userspace policy ranks eligible destinations but does not create a new kernel-level scheduling preference.

---

## 10. Benefit-versus-Cost Migration Policy

An affinity change should occur only when the estimated benefit exceeds the estimated cost and the move passes all eligibility and safety checks.

### 10.1 Whole-System Projection

The detector continues to use the configured max-minus-min range to identify persistent imbalance. For ranking a proposed move, the selector uses a whole-system metric that is less sensitive to an unrelated CPU remaining at an extreme.

For the set of \(N\) valid logical CPUs, let \(U_i\) be each CPU's utilization and let:

\[
\bar U = \frac{1}{N}\sum_{i=1}^{N} U_i
\]

The selector's load-variation metric is the population standard deviation:

\[
S = \sqrt{\frac{1}{N}\sum_{i=1}^{N}(U_i-\bar U)^2}
\]

For a candidate with estimated thread utilization \(PUtil_p\), the first-order projection is:

\[
U'_{src}=\operatorname{clamp}(U_{src}-PUtil_p,0,100)
\]

\[
U'_{dst}=\operatorname{clamp}(U_{dst}+PUtil_p,0,100)
\]

All other valid CPU samples remain unchanged, and the mean and standard deviation are recomputed over the projected whole-system vector. The estimated benefit is:

\[
Benefit=S_{before}-S_{after}
\]

A move is rejected when this benefit is zero or negative, below the configured minimum, or too small to cover the modeled migration/locality cost. This lets the policy recognize improvements even when max-minus-min remains unchanged due to a different CPU still being the maximum or minimum.

This remains a heuristic, not an exact per-CPU accounting of the candidate thread. `/proc/<PID>/task/<TID>/stat` reports the thread's recent total CPU time and its last observed CPU, not its complete execution distribution across CPUs. The selector treats `last_cpu` as an estimated source and rejects a projection when measured thread utilization exceeds the source CPU's entire observed utilization or cannot fit on the destination CPU. These checks reduce impossible projections but cannot prove that the thread ran only on the estimated source. A positive estimate must be validated experimentally and does not prove better application performance.

### 10.2 Migration Cost

The current implementation uses a configurable baseline penalty plus a locality penalty scaled by a topology-derived factor:

\[
C(p)=C_0+C_{locality}\cdot F_{locality}
\]

The current relative factors are 0.25 for a same-L1 destination, 0.50 for same L2, 0.75 for same L3, 1.0 for unknown locality, and 2.0 for a different locality domain. These are policy weights, not measured cache-miss costs or time units. They must be tuned and evaluated rather than presented as physical measurements.

Unless calibrated with measurements, these quantities are heuristic scores rather than physical units of execution time.

### 10.3 Decision Score

\[
Score=Benefit-C(p)
\]

Accept the move only if:

\[
Score>\theta
\]

where \(\theta\) is a configurable minimum acceptance threshold, and all candidate, destination, affinity, persistence, and safety requirements pass.

A move must be rejected when the expected benefit is too small, the estimated cost is too high, the destination is invalid, or the input observations are unreliable.

### 10.4 Interpretation

The policy should penalize unnecessary changes and cache-locality risks. Its purpose is not to maximize the number of migrations; it is to make fewer, better-justified decisions.

The score must be interpreted as a decision heuristic until the model is calibrated and validated experimentally.

---

## 11. Affinity Management and Safety

### 11.1 Affinity Operations

Use `sched_getaffinity()` to inspect the current affinity and `sched_setaffinity()` to request a change.

The implementation must:

- Check return values and errors.
- Validate CPU IDs and mask membership.
- Respect the task's current allowed CPU set and system constraints.
- Avoid assuming a process's threads all have identical affinity.
- Recheck or verify the result when appropriate.
- Handle tasks exiting between observation and action.
- Log failed affinity changes without crashing the controller.

An applied move must be temporary: save the thread's current allowed mask, apply the selected single-CPU mask only for a bounded lease, and restore the saved mask on lease expiry and normal controller shutdown. Restore only when the same owned task still has the lease mask, so an external affinity change is not overwritten. An independent guardian attempts restoration after controller exit, but cannot guarantee recovery if the guardian or whole system also stops; keep apply mode experimental until native Linux validation is complete.

### 11.2 Dry-Run Mode

The controller must support a mode that performs monitoring, selection, scoring, and logging without changing affinity.

Dry-run mode should be the initial mode for testing the integrated policy.

### 11.3 Decision Logs

Logs should explain both accepted and rejected decisions.

Where applicable, record:

- Timestamp or monotonic sample time.
- Source and destination CPU.
- PID and TID for the selected task.
- Relevant task identity information.
- Current affinity and destination eligibility.
- Measured imbalance and persistence state.
- Candidate utilization estimate.
- Expected benefit.
- Migration/locality cost and final score.
- Rejection reason or affinity-operation result.
- Whether the decision was dry-run or applied.

Logging must not require a successful migration to produce a useful record.

### 11.4 Rate Limits and Cooldowns

Enforce cooldowns and configurable limits on affinity changes to avoid thrashing and excessive overhead.

### 11.5 Stop and Disable

Provide a clear mechanism to stop or disable automatic affinity control. Disabling control must prevent new automatic affinity changes while allowing the user to terminate the monitoring program safely.

### 11.6 Permissions

Only manage tasks that the user owns or is otherwise authorized to manage. Permission failures must be handled explicitly rather than bypassed.

The current selector checks the task directory's effective owner before scoring it, and the action path repeats the ownership and task-identity checks immediately before an affinity change.

---

## 12. Evaluation Methodology

The project must evaluate both load-distribution behavior and overall system impact.

### 12.1 Comparison Configurations

Compare at least these configurations:

1. **Baseline:** ordinary Linux scheduling with no controller-imposed affinity changes.
2. **Static affinity:** manually restricted affinity, for example using `taskset`.
3. **Adaptive controller:** the proposed userspace controller, initially in dry-run mode for policy validation and later with actual changes where safe.

Use equivalent workload definitions and comparable system conditions for the configurations.

### 12.2 Workloads

Use repeatable workload classes such as:

- CPU-bound workloads.
- Multiple competing CPU-bound tasks.
- Mixed workloads with different CPU demands.
- Multithreaded workloads for evaluating thread-level monitoring and affinity.
- Workloads that exercise different allowed CPU masks.

Record the exact commands, parameters, duration, and affinity configuration so runs can be repeated.

### 12.3 Measurements

Measure, where applicable:

- Per-CPU utilization.
- CPU utilization imbalance.
- Average and maximum imbalance over a run.
- Execution time or throughput.
- Number of affinity changes.
- Number of rejected migration proposals.
- Monitoring and controller CPU overhead.
- Collection failures and invalid-sample frequency.
- The effect of locality-aware ranking.
- Stability, including repeated or oscillating decisions.

A lower value of \(I=U_{max}-U_{min}\) does not by itself establish a performance improvement. Report execution time and throughput alongside imbalance.

### 12.4 Repeated Runs

Perform multiple runs for each configuration. Record enough information to distinguish genuine trends from run-to-run variation.

Keep workload settings and comparison conditions consistent. Report relevant limitations, unexpected outcomes, and negative results.

### 12.5 Overhead and Scalability

Measure the cost of collecting CPU, process, and thread information and running the selection policy.

In particular, evaluate how monitoring overhead changes with the number of observed processes and threads. The initial linear-search snapshot matching may become expensive at high task counts and should be optimized only if measurements justify the change.

### 12.6 Locality Evaluation

Evaluate whether locality-aware ranking changes the selected destinations and whether that correlates with measurable changes in execution time or throughput.

A topology-derived locality score alone is not evidence of an actual performance improvement.

### 12.7 Experimental Environment

Initial development and testing use Windows with WSL2 Ubuntu, with later validation planned on a native Linux lab system.

WSL CPU topology and scheduling behavior may differ from native Linux on the physical system. Record the environment for each experiment and do not assume WSL results generalize to the lab system.

---

## 13. Current Implementation Status

Status below reflects the source tree audited and built in WSL through 2026-10-20. The strict build and seven test programs passed. Live controller experiments have been run in dry-run mode; the integrated controller's `--apply` mode still needs the native Linux smoke test.

### 13.1 Implemented Modules

#### `src/cpu_monitor.c`

- Reads per-CPU counters from `/proc/stat`.
- Calculates utilization from counter deltas.
- Reports average, minimum, maximum, and imbalance.
- Builds with strict warnings and completed a short WSL smoke run, sampling all 12 visible CPUs across multiple intervals.

#### `src/process_monitor.c`

- Estimates process CPU usage from `/proc/<PID>/stat`.
- Handles process names containing spaces or parentheses.
- Uses process start time to reduce PID-reuse confusion.
- Builds with strict warnings and completed a short WSL smoke run, displaying process CPU samples across multiple intervals.

#### `src/affinity_test.c`

- Inspects, temporarily changes, verifies, and restores affinity using `sched_getaffinity()` and `sched_setaffinity()`.
- Operates on one Linux task/thread at a time.
- Does not automatically change every thread in a process.
- A corrected version was reported to compile with the same warning flags.

#### `src/thread_monitor.c`

- Enumerates visible threads.
- Reads `/proc/<PID>/task/<TID>/stat`.
- Estimates per-thread CPU usage across samples.
- Tracks PID/TID and task start time to reduce reuse confusion.
- Handles common exit, race, and invalid-sample cases.
- Displays the highest CPU-consuming threads.
- Builds with strict warnings and completed a short WSL smoke run, reporting valid usage samples for visible threads across multiple intervals.

#### `src/monitor_data.h` and `src/monitor_data.c`

- Define the shared structured snapshot interface.
- Include CPU, process, thread, and utilization-summary data structures.
- Provide snapshot lifecycle functions.
- Implement `monitor_collect_snapshot()`.
- Establish the intended integration boundary for future controller modules.

The shared interface must be validated against the actual latest source files before further modules are integrated.

### 13.2 Additional Modules and Tests

The local project inventory previously reported the following modules:

- `src/imbalance_detector.c`
- `src/imbalance_detector.h`
- `src/candidate_selector.c`
- `src/candidate_selector.h`
- `src/benefit_cost_policy.c`
- `src/benefit_cost_policy.h`
- `src/locality_policy.c`
- `src/locality_policy.h`

The corresponding test files reported were:

- `tests/monitor_data_smoke_test.c`
- `tests/imbalance_detector_test.c`
- `tests/candidate_selector_test.c`
- `tests/benefit_cost_policy_test.c`
- `tests/locality_policy_test.c`
- `tests/migration_decision_test.c`

The controlled dry-run workload is in `scripts/dry_run_candidate_experiment.sh`; it uses the already installed `stress-ng` to create source pressure and never enables controller apply mode. Its optional `no-locality` argument disables the locality cost while retaining base migration cost and minimum-benefit checks. `--compare-locality` scores locality-aware and no-locality selectors against the same snapshot; `scripts/compare_locality_policies.sh [runs]` records candidate status, approvals, and projected benefit/cost for each policy in a timestamped CSV with per-trial logs under the ignored `results/` directory. This isolates policy scoring from between-run scheduling changes, but it does not establish performance improvement. `scripts/repeat_dry_run_experiment.sh` runs the default policy repeatedly. `scripts/measure_controller_overhead.sh` measures repeated dry-run controller CPU use at configurable idle-task counts. `scripts/benchmark_cpu_completion.sh` compares a fixed-work pthread workload under baseline scheduling, static two-CPU affinity, dry-run observation, and temporary adaptive affinity; it records timings, decisions, proposals, applied actions, and restored leases in `results/`. Dry-run measures monitoring overhead only and cannot demonstrate adaptive migration benefit. The project uses stress-ng to generate uneven loads, not as a performance benchmark; the [stress-ng manual](https://github.com/ColinIanKing/stress-ng/blob/master/stress-ng.1) cautions that bogo-op metrics are not reliable throughput measurements.

These modules are integrated by `src/cpu_affinity_manager.c` and the current Makefile.

### 13.3 Previously Reported Test Results

The following results were reported during development:

- Monitor-data smoke test (latest run): initial and second status `0`; 12 CPUs, 34 processes, and 54 threads; 12 CPUs, 33 processes, and 53 threads had valid utilization in the reported snapshot.
- Imbalance-detector test: passed persistence, invalid-sample, hysteresis, cooldown, and configuration-validation cases.
- Candidate-selector test: passed candidate eligibility, affinity constraints, utilization-threshold, and invalid-sample cases.
- Benefit-cost-policy test: passed benefit projection, locality penalties, rejection cases, and configuration validation.
- Locality-policy test: passed cache-locality scoring, allowed-mask enforcement, invalid inputs, and unknown-topology fallback.
- Locality topology discovery: reported complete for 12 CPUs in WSL, with CPU 0 to CPU 1 receiving a relative score of 30 and CPU 0 to CPUs 2–11 receiving a relative score of 10 under the then-current scoring policy.

These locality scores are implementation-specific heuristic outputs. They are not measured cache-miss rates, percentages of performance retained, or proof of a performance benefit.

### 13.4 Validation performed on 2026-10-19 and 2026-10-20

- Strict WSL build of all five Linux programs with `-std=c11 -Wall -Wextra -Wpedantic -Werror -O2`: passed.
- The fixed-work CPU benchmark compiled with the strict warning flags and `-pthread`; runtime comparison results are recorded below.
- `make -j2 all test`: all seven test programs passed, including monitor sampling, candidate selection, benefit/cost, locality, end-to-end migration-decision, and guardian recovery coverage.
- Standalone `cpu_monitor`, `process_monitor`, and `thread_monitor` each completed a short WSL smoke run and printed multiple sampling intervals; these runs only read `/proc`.
- A deterministic candidate-selector regression rejects a projection whose measured thread utilization exceeds the source CPU's observed utilization.
- Destination selection now compares each eligible pair with the actual benefit-cost policy and topology-derived locality factor. A regression confirms it selects a same-L1 destination whose lower raw benefit clears the policy, instead of a higher-benefit unknown-locality destination that fails the cost threshold.
- Locality score now scales the cost penalty: same L1 0.25, same L2 0.50, same L3 0.75, unknown 1.0, and a different locality domain 2.0. A regression verifies both same-L3 approval and different-domain rejection for the same projected benefit.
- A controlled dry-run produced a candidate on CPUs 0 to 2: estimated thread use 49.9%, projected standard deviation 27.61 to 18.60 percentage points, benefit 9.0 points, modeled cost 8.8 points, score 0.3 points. The controller logged that it would set the affinity and did not apply it.
- The stress-ng experiment pinned a full-load stressor to CPU 0. The user's positive dry-run showed an approved 0->2 proposal (8.8-point estimated benefit, 8.8-point modeled cost, 0.1-point score). Later runs observed the candidate already on CPU 2 at 17.3% or 24.0% while CPU 0 was at 100%; the selector correctly rejected moving it to the busier CPU. The result depends on scheduling and sampling timing.
- The controller has an opt-in `--no-locality` mode; locality-aware scoring remains the default. This sets the locality cost weight to zero while retaining base migration cost and minimum-benefit checks. The later `--compare-locality` mode scores both selectors against one monitor snapshot in dry-run mode. Five paired WSL trials on 2026-10-20 scored the same candidate and destination in each pair. Locality-aware approved 4/5, while no-locality approved 5/5; mean projected benefit was 8.94 points in both, with modeled costs 8.75 and 5.00 points respectively. This confirms that locality cost changes policy decisions on these samples, not that no-locality improves workload performance. See `results/locality_policy_comparison_20261020T045312Z.csv` and its per-trial logs.
- The paired comparison runner now evaluates both policies on the same live snapshot in one dry-run controller invocation, avoiding sequential scheduler state differences. The earlier sequential comparison produced three pairs with no scored candidates and one pair in which both policies approved; those runs exposed scheduler movement and are retained only as development records.
- A three-trial dry-run produced two approved proposals and one rejection because the candidate's estimated source was already cooler than CPU 0; its summary CSV is under the ignored `results/` directory. A later one-trial check confirmed the runner also saves the full log and links it from the CSV. These few trials show that positive proposals can recur, but do not establish repeatability or performance.
- Three 10-second overhead-harness runs completed in dry-run mode. GNU time measured 0.20%, 0.30%, and 0.30% CPU for the controller command tree, with nine samples per run. This is a preliminary idle-WSL estimate; more runs, workload-size variations, and a baseline comparison are needed before drawing an overhead conclusion.
- The overhead harness now accepts an idle-task count (`scripts/measure_controller_overhead.sh SECONDS RUNS IDLE_TASKS`) and records it in the CSV. Two 10-second runs each measured 0.50%/0.20% CPU at 0 idle tasks, 0.30%/0.29% at 25 sleeping tasks, and 0.90%/0.70% at 100 sleeping tasks, with nine samples per run. The 100-task runs are higher, but two repetitions per size and WSL noise are too weak to establish a scaling trend; repeat on native Linux before drawing a conclusion.
- A follow-up overhead series used two 10-second dry-run measurements at 0, 100, and 300 sleeping helper tasks, with nine samples per run. Measured CPU use was 0.10%/0.00%, 0.299%/0.300%, and 0.499%/0.699%, respectively. The increasing values suggest process-count sensitivity, but two runs per size in WSL remain preliminary; repeat with more samples and on native Linux.
- The fixed-work pthread benchmark completed three repetitions for each of two workloads. With two workers, 500 million base iterations each, and 100% extra work for worker 0, mean completion times were 2.102 s under baseline scheduling, 2.030 s with affinity restricted to CPUs 0–1, and 2.041 s with the controller observing in dry-run. With four workers and 200% extra work for worker 0, the respective means were 3.182 s, 3.987 s, and 3.151 s. Checksums and completed iteration counts matched across each workload's configurations. The dry-run observer made no proposals in either workload; in the four-worker case it logged three policy rejections for insufficient projected benefit. These small WSL samples are preliminary synthetic measurements, not evidence that adaptive migration improves performance.
- The four-case runtime workload matrix completed in dry-run mode: baseline (no workers), one unrestricted CPU worker, two unrestricted CPU workers, and two workers restricted to one CPU each. The unrestricted-worker cases triggered persistent imbalance, but the best proposals had only 0.0–0.6 percentage points of estimated standard-deviation reduction, below the configured 5-point minimum; the restricted case had no allowed destination. This is evidence of conservative rejection in these scenarios, not evidence of a performance improvement.
- The affinity tests temporarily restrict their own test process and restore its original mask. An isolated guardian test also simulates the manager exiting after a temporary affinity change; the guardian restored the test worker's original mask. The integrated controller has not yet been run in apply mode.

These results establish compilation and module-level behavior in this WSL environment. They do not establish that a live workload will produce an approved proposal or that changing affinity improves performance.

### 13.5 Remaining implementation and evaluation work

The following work remains outstanding:

- Repeated, stable dry-run runs across varied workloads; the same-snapshot comparison removes between-policy timing differences, but broader workloads and more scored candidates are still needed. Some live workload trials have no eligible candidate because the scheduler has already moved the thread before sampling.
- End-to-end comparisons among baseline scheduling, static affinity, and adaptive control; initial repeated synthetic comparisons exist for baseline, static-mask, and dry-run-overhead cases, but do not measure adaptive performance because controller runs remain dry-run only. Add larger workload variation and repeat on native Linux.
- Systematic measurements of controller overhead and performance effects; an initial idle-task-count baseline comparison is available, but needs more repetitions, larger task counts, and native Linux measurement. End-to-end performance comparisons still need to be designed and run.
- Native Linux lab validation, since WSL scheduling and topology may differ.
- Repeatable end-to-end measurements that include an adaptive apply case. The benchmark records proposals, applied actions, and normal lease restorations; an apply trial with no action is not evidence of adaptive performance.

The `--apply` path implements a five-second affinity lease with task identity/owner checks and conditional restoration on expiry or normal shutdown. An independent guardian process registers each lease before the affinity change and retries restoration if the controller exits unexpectedly. The guardian's recovery is covered by an isolated test, but integrated apply behavior still needs the native Linux smoke test below. A guardian crash, whole process-group termination, reboot, or failed OS affinity operation can still prevent recovery; apply mode remains experimental. Do not report an applied migration or performance improvement based on dry-run or synthetic test results.

---

## 14. Development Environment and Validation

The initial development environment is Windows with WSL2 Ubuntu and VS Code. The project is intended to be tested on the institute's native Linux system as well.

The WSL system previously reported:

- CPU: Intel Core 5 120U.
- Logical CPUs visible to Linux: 12, numbered 0–11.
- Kernel: WSL2 Linux kernel, previously reported as `6.6.87.2-microsoft-standard-WSL2`.

These details describe the reported development environment and should be rechecked when running experiments.

### 14.1 Native Linux Validation Runbook

Run the following from a clean checkout on the institute's native Linux host. First run the isolated live-affinity smoke test once; it only targets its own short-lived test worker, verifies the original mask is restored, and saves a log. If that fails, stop and inspect the log rather than running the adaptive benchmark.

```bash
make clean
make -j2 all benchmark test
bash scripts/lab_apply_smoke_test.sh locality
```

After the smoke test passes, run the repeatable measurements. The benchmark compares baseline scheduling, static affinity, a dry-run observer, and temporary adaptive affinity; inspect `applied_actions` and `restored_leases` in the CSV before interpreting adaptive timings. Increase iterations if adaptive runs finish before the controller gets a scored sample.

```bash
bash scripts/benchmark_cpu_completion.sh 2 2000000000 100 5
bash scripts/benchmark_cpu_completion.sh 4 2000000000 200 5
bash scripts/measure_controller_overhead.sh 10 5 0
bash scripts/measure_controller_overhead.sh 10 5 25
bash scripts/measure_controller_overhead.sh 10 5 100
bash scripts/repeat_dry_run_experiment.sh 5
```

The benchmark CSVs and logs are written beneath `results/`, which is ignored by Git; copy the run outputs to the project report or another backed-up location after reviewing them. Record the Linux kernel, CPU model, allowed CPU list, and stress-ng version with the measurements. The smoke test is the one-time apply safety check; performance runs use the benchmark's explicit `adaptive_apply` case and restore affinity on normal shutdown. Treat results as preliminary until repeated measurements across workload sizes are stable and applied/restored actions are confirmed. WSL results do not substitute for this native Linux validation.

### 14.2 Validation Requirements

For every significant module change:

1. Compile with strict warnings, for example:

   ```bash
   gcc -std=c11 -Wall -Wextra -Wpedantic -O2 ...
   ```

2. Run the module's targeted tests.
3. Exercise invalid inputs and error paths where practical.
4. Check behavior when processes or threads exit during monitoring.
5. Verify affinity restrictions and permissions.
6. Test the integrated controller in dry-run mode before enabling changes.
7. Repeat critical tests in WSL and on native Linux when available.

Use `-D_GNU_SOURCE` where required for the Linux/GNU interfaces used by the particular source files. Do not suppress warnings as a substitute for resolving genuine correctness problems.

### 14.3 Controlled Changes

Do not rewrite unrelated modules unnecessarily.

When fixing a module, provide the complete updated file, describe the changes, and preserve established interfaces unless there is a clear reason to change them. If an interface changes, update and validate all dependent modules and tests.

---

## 15. Recommended Implementation Order

The next steps should be completed in dependency order.

1. **Validate the shared monitoring interface.** Confirm the current `monitor_data.h` and `monitor_data.c` agree with the CPU, process, and thread data structures and lifecycle requirements.
2. **Validate the existing detector and policy modules.** Confirm their current headers and source files use compatible interfaces and that their tests pass.
3. **Integrate structured snapshots.** Ensure the controller consumes structured data rather than parsing command-line monitor output.
4. **Complete persistent imbalance handling.** Validate persistence, hysteresis, cooldown, and invalid-sample behavior.
5. **Complete candidate selection.** Use recent task activity, allowed affinity masks, valid destinations, and conservative rejection rules.
6. **Integrate topology and locality policy.** Use discovered topology when reliable and the documented fallback when it is not.
7. **Integrate benefit-versus-cost decisions.** Apply a configurable threshold and explain each accepted or rejected decision.
8. **Implement the controller and dry-run mode.** Add rate limits, logging, safe affinity operations, permission handling, and a stop/disable mechanism.
9. **Validate actual thread-level affinity changes.** Verify that changes apply to the intended Linux task/thread and do not assume process-wide behavior.
10. **Evaluate end to end.** Compare baseline, static affinity, and adaptive control on repeatable workloads, measuring both load distribution and performance/overhead.
11. **Validate on native Linux.** Repeat important tests outside WSL and document environmental differences.

---

## 16. Design Principles That Must Be Preserved

The following principles apply throughout implementation and evaluation:

1. **Userspace only:** Do not modify the kernel or claim to replace its scheduler.
2. **Structured integration:** Use the shared snapshot API rather than scraping command-line output.
3. **Task identity:** Use start-time information to reduce PID/TID-reuse errors.
4. **Persistent evidence:** Do not react to one transient utilization spike.
5. **Hysteresis and cooldown:** Avoid oscillation and excessive affinity changes.
6. **Eligibility first:** Never select a destination outside the task's valid allowed CPU set.
7. **Thread correctness:** Treat affinity as task/thread-specific.
8. **Benefit versus cost:** Do not move tasks simply because an imbalance exists.
9. **Locality awareness:** Include cache-locality considerations while describing their limitations honestly.
10. **Safe operation:** Prefer dry-run first, log rejected as well as accepted decisions, and provide a way to disable automatic changes.
11. **Partial failure tolerance:** A disappearing task should not invalidate unrelated valid observations.
12. **Measured claims:** Do not claim better performance solely because CPU utilization appears more balanced.
13. **Overhead awareness:** Measure monitoring and control costs, including the cost of snapshot matching.
14. **Controlled evolution:** Preserve working modules and interfaces unless a tested change is necessary.
15. **Evidence-based reporting:** Separate implemented features, individually tested modules, integrated behavior, and experimentally demonstrated benefits.

## 17. Project Success Criteria

The project will be considered technically successful when it:

- Collects and validates CPU, process, and thread observations.
- Detects sustained imbalance without responding indiscriminately to transient spikes.
- Selects only eligible candidate tasks and destination CPUs.
- Applies the documented benefit-versus-cost and locality-aware policy.
- Supports dry-run operation, decision logging, rate limiting, and safe shutdown.
- Changes affinity only for authorized tasks and handles failures safely.
- Demonstrates repeatable behavior across baseline, static-affinity, and adaptive configurations.
- Reports both load-distribution outcomes and controller overhead.
- Clearly documents limitations, environmental differences, and negative results.

The project does not need to outperform the Linux scheduler in every workload to be a valid research-oriented OS project. A careful demonstration of when the approach helps, when it does not, and why is a meaningful outcome.
