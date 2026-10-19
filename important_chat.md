For your accepted project, Adaptive CPU Affinity Manager for Linux: A Userspace Approach to Intelligent Load Balancing, I recommend a focused set of changes rather than rewriting the entire project.

The priorities are:

1. Fix monitoring measurements so the controller does not make decisions using misleading utilization values.
2. Make candidate selection and migration decisions consistent with actual CPU load and affinity restrictions.
3. Integrate the accepted Level 1, soft-affinity-inspired locality preference, without misrepresenting it as a kernel scheduler feature.
4. Make `--apply` safer and ensure its behavior matches the intended design.
5. Add tests and evaluation evidence that demonstrate the controller is useful—not merely that `sched_setaffinity()` works.

## 1. Complete change list

## 2. Monitoring: fix measurement consistency

Priority 1

Files: `src/monitor_data.h`, `src/monitor_data.c`

### Changes

1. Add individual sample timestamps. Add a `struct timespec sampled_at` field to `ProcessSample` and `ThreadSample`.
2. Timestamp each task after reading its `/proc/.../stat` record. Use the previous and current timestamps for that exact task to calculate its utilization.
3. Keep snapshot-level timing separate. Preserve `MonitorSnapshot.sampled_at` for overall sampling cadence and detector timing; do not use it as a substitute for individual task timestamps.
4. Validate counter deltas and elapsed intervals. A counter decrease, invalid timestamp, or nonpositive interval makes that utilization measurement invalid.
5. Handle anomalous utilization safely. Do not blindly clamp an individual thread's utilization to 100%. Mark clearly inconsistent measurements invalid or handle them conservatively in candidate selection.
6. Preserve existing parsing and identity checks. Continue matching processes by PID plus start ticks and threads by PID, TID, and start ticks.
7. Keep the existing collection API where possible. `monitor_collect_snapshot()` should continue supporting baseline and subsequent snapshots.

### Why this matters

A migration decision is only as reliable as the load estimate used to justify it. We should fix the measurement problem before adjusting thresholds to make more migrations pass.

## 3. Candidate selection: consider valid destinations

Files: `src/candidate_selector.c` and its corresponding header and tests.

### Changes

- Continue selecting only eligible, sufficiently active threads.
- Reject candidates whose utilization measurement is invalid or clearly inconsistent.
- Revalidate the thread's current affinity mask and CPU eligibility.
- Consider all valid destination CPUs permitted by the thread's affinity, instead of always choosing the globally least-loaded CPU and giving up if that CPU is not allowed.
- Prefer a destination that improves the load distribution while accounting for locality.
- Handle missing CPU samples, invalid `last_cpu` values, threads that exit during selection, and single-CPU affinity masks safely.

The current approach can miss a valid migration when the least-loaded CPU is not permitted by the candidate's affinity mask. Exploring valid alternatives makes the controller more useful without changing its overall architecture.

## 4. Benefit-versus-cost policy: retain the core formula, improve its inputs

Files: `src/benefit_cost_policy.c` and its corresponding header and tests.

Your existing policy already compares estimated load-balancing benefit against migration cost. I want to preserve that central idea rather than replace it with an unrelated algorithm.

The current calculation is:

\\[ I\_{\text{before}}=|U_s-U_d| \\]

\\[ I\_{\text{after}}= |(U_s-U_t)-(U_d+U_t)| \\]

\\[ B=I\_{\text{before}}-I\_{\text{after}} \\]

\\[ S=B-C \\]

Here, \\(U_s\\) and \\(U_d\\) are source and destination utilization, \\(U_t\\) is the candidate thread's estimated utilization, \\(B\\) is estimated benefit, \\(C\\) is migration cost, and \\(S\\) is the resulting score.

### Changes

- Ensure only valid, internally consistent utilization measurements reach the policy.
- Evaluate the actual candidate-specific source and destination pair.
- Preserve minimum-benefit and minimum-score thresholds.
- Preserve locality penalties and reject migrations that are unlikely to improve the imbalance enough.
- Handle extreme or inconsistent projected loads conservatively instead of allowing clamping to conceal bad inputs.
- Keep the policy's decision reasons understandable in logs and tests.

Important limitation: this is a first-order estimate. A thread may not consume the same amount of CPU after migration, and migration can affect cache locality, contention, and runtime. The formula predicts a potential benefit; it does not guarantee one.

## 5. Level 1 locality: make topology affect destination choice

Files: `src/locality_policy.c`, `src/locality_policy.h`, candidate-selection code, and the manager.

Your current locality module discovers cache-sharing relationships from `/sys/devices/system/cpu/` and classifies destinations as sharing L1, L2, or L3, being in a different domain, or having unknown locality.

That is a useful foundation, but a locality score alone is not a complete locality-preference policy.

### Changes

- Retain the existing cache-topology discovery and scoring.
- Integrate locality into the selection of valid destination CPUs, rather than using it only after a destination has already been selected.
- Prefer a cache-local destination when the load-balancing benefit remains worthwhile.
- Allow movement to a less-local destination when that is necessary to address meaningful imbalance.
- Fall back to the existing load-balancing policy if topology discovery fails or returns incomplete information.
- Test missing cache files, incomplete topology, and CPUs not represented in the discovered topology.

The intended decision is:

1. Identify a persistent imbalance.
2. Find eligible candidate threads and valid destination CPUs.
3. Compare expected load-balancing benefit against migration and locality costs.
4. Prefer a suitable cache-local destination when the policy still approves the move.
5. Reject the move if the expected benefit is insufficient.

This is soft-affinity-inspired locality preference implemented in userspace, not the kernel's true soft-affinity mechanism. `sched_setaffinity()` still imposes a hard allowed-CPU mask.

## 6. Make `--apply` safer

Priority 1

File: `src/cpu_affinity_manager.c`

This is an important change because your current apply path restricts an approved thread to a single destination CPU.

### Changes

- Keep dry-run as the default.
- Revalidate thread identity immediately before applying a change, protecting against TID reuse.
- Re-read the current affinity mask before applying a change.
- Ensure the destination remains allowed and valid.
- Preserve existing action cooldown and per-minute migration limits.
- Record the original affinity mask before the first change to a thread.
- On clean shutdown, restore the original mask only if the same thread identity still exists.
- Log successful changes, rejected changes, restoration attempts, and failures.
- Keep the controller's disable mechanism and ensure it does not initiate new changes after shutdown begins.

### One important distinction

Restoring affinity on clean shutdown does not protect against a crash or forced termination. Also, pinning a thread to one CPU is a hard restriction. The policy may be locality-aware, but that alone does not make the action soft affinity.

I would not implement a temporary pin-and-restore trick as a substitute for a genuine preference mechanism: the thread could be rescheduled elsewhere after restoration, and the behavior would not provide a reliable locality guarantee.

For this project, the safest initial implementation is to keep apply mode explicitly documented as a controlled hard-affinity action, with preservation and restoration safeguards. We can evaluate a broader preferred-CPU-group design separately without falsely claiming it is equivalent to kernel soft affinity.

## 7. Preserve persistent imbalance detection

Files: `src/imbalance_detector.c`, its header, and tests.

The existing persistence, hysteresis, and cooldown decisions should remain in place. I do not recommend redesigning the detector unless testing reveals a concrete issue.

We should verify that it:

- Requires repeated imbalance samples before triggering.
- Avoids reacting to a single utilization spike.
- Uses hysteresis to avoid rapid switching around the threshold.
- Respects cooldown after a detection or action, according to the existing design.
- Handles incomplete CPU samples and invalid summaries conservatively.

The goal is to improve the reliability of the surrounding modules without removing safeguards that are already part of your project's design.

## 8. Expand tests around the actual failure cases

Files: existing tests under `tests/`, plus targeted tests where needed.

| Area                | Cases to cover                                                                                                  |
| ------------------- | --------------------------------------------------------------------------------------------------------------- |
| Monitoring          | Baseline snapshot, valid delta, zero elapsed time, decreasing counters, task exit, PID/TID reuse                |
| Utilization         | Normal busy thread, idle thread, invalid sample, implausible measurement                                        |
| Candidate selection | Destination permitted, destination forbidden, alternate valid destination, invalid source CPU                   |
| Benefit-cost        | Benefit above threshold, insufficient benefit, locality penalty, invalid inputs                                 |
| Locality            | Same L1/L2/L3, different domain, unknown topology, partial discovery                                            |
| Affinity action     | Dry-run makes no changes, valid apply, stale TID, changed affinity, failed system call                          |
| Shutdown            | Original affinity restored for a surviving, matching thread; exited or reused TIDs are not incorrectly modified |
| Detector            | Persistent imbalance, transient spike, hysteresis, cooldown                                                     |

I also want a regression test for the specific measurement inconsistency that produced values such as `100.9%`, where practical. Since `/proc` collection is tied to the running system, this may require separating the utilization calculation into a small testable helper.

Your current linear snapshot matching is \\(O(N^2)\\) in the worst case. I recommend keeping it for the first implementation, documenting the limitation, and measuring its overhead before deciding whether to introduce a more complex lookup structure.

## 9. Update project documentation

File: `DESIGN_REQUIREMENTS.md`

Keep the accepted title and scope. Update the design requirements to reflect:

- Per-task timestamp-based utilization measurements.
- Persistent imbalance detection, hysteresis, and cooldown.
- Candidate selection across valid affinity-constrained destinations.
- Benefit-versus-cost migration decisions.
- Topology-aware locality preference and fallback behavior.
- The distinction between soft-affinity-inspired policy and hard affinity enforcement.
- Dry-run default, apply-mode safeguards, and affinity restoration.
- The linear snapshot-matching limitation and its performance implications.
- The limits of the benefit estimate and the need to evaluate actual workload performance.

The documentation should clearly distinguish what the implementation actually does from future enhancements.

## 10. Evaluation: demonstrate that it helps

The implementation should be compared under three configurations:

A. Default Linux scheduler

Baseline; no controller intervention.

B. Static affinity with `taskset`

Comparison against manually selected CPU restrictions.

C. Adaptive CPU Affinity Manager

Persistent imbalance detection, benefit-cost decisions, and locality-aware selection.

Measure:

- CPU utilization imbalance across logical CPUs.
- Workload completion time or throughput.
- Number of proposed, approved, rejected, and completed affinity changes.
- Monitoring overhead and sampling duration.
- Stability: unnecessary migrations, repeated switching, and behavior after a workload changes.
- Whether locality-aware decisions improve outcomes compared with the same policy without locality preference.

Use repeatable CPU-bound and mixed workloads. Run the same workload several times per configuration, record the environment, and report the results rather than assuming adaptive affinity must win.

WSL limitation: your laptop is sufficient for development, unit tests, and preliminary experiments. However, WSL results are not a substitute for validating Linux affinity behavior and performance on the institute's native Linux environment.

## 11. Implementation order

I recommend the following sequence so we can isolate problems rather than change every module at once.

1. Monitoring correction: update `monitor_data.h` and `monitor_data.c`, then build and run monitoring tests.
2. Candidate and policy correction: improve valid-destination selection and validate benefit-cost decisions with unit tests.
3. Locality integration: connect topology-aware destination ranking to the decision process and verify fallback behavior.
4. Apply-mode safeguards: preserve original masks, revalidate identities, and restore surviving threads' affinity on clean shutdown.
5. Regression tests and documentation: run the complete test suite and update `DESIGN_REQUIREMENTS.md`.
6. Evaluation: run dry-run experiments first, then carefully controlled apply-mode tests on native Linux.

### What I would change—and what I would leave alone

| Change                                               | Decision                      |
| ---------------------------------------------------- | ----------------------------- |
| Per-task sampling timestamps                         | Implement                     |
| Blindly clamp thread utilization to 100%             | Do not implement              |
| Search alternate affinity-permitted destinations     | Implement                     |
| Replace the benefit-cost approach entirely           | Do not implement              |
| Integrate topology into destination selection        | Implement                     |
| Claim userspace policy is true kernel soft affinity  | Do not claim                  |
| Keep dry-run as default                              | Preserve                      |
| Preserve and restore original affinity in apply mode | Implement                     |
| Replace linear snapshot matching immediately         | Defer; measure overhead first |
| Redesign the entire project architecture             | Not needed                    |

My recommendation: start with the monitoring correction, because every later decision depends on those measurements. Then improve candidate selection and locality integration, and only after those pass tests should we change apply-mode behavior.

I have not modified your local files or run new tests in this turn. These are the proposed changes, not claims that the implementation has already been updated.