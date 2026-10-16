Adaptive CPU Affinity Manager for Linux: A Userspace Approach to Intelligent Load Balancing
Project scope
A userspace Linux controller that observes CPU and task activity and selectively adjusts CPU affinity. It is not a replacement for the Linux kernel scheduler and does not modify the kernel.
Core architecture and requirements
1. CPU monitoring: Read per-CPU counters from /proc/stat and calculate utilization from sampling deltas.
2. Process/thread monitoring: Collect process information from /proc/<PID>/stat and thread information from /proc/<PID>/task/<TID>/stat. Estimate CPU usage over successive samples; handle exiting/reused tasks and invalid counter deltas.
3. Persistent imbalance detection: Require imbalance across multiple samples; use hysteresis and cooldown to avoid reacting to transient spikes or oscillating.
4. Candidate selection: Consider recent process/thread CPU usage, current affinity/allowed CPU sets, destination CPU eligibility, and whether moving the candidate could plausibly help.
5. Benefit-versus-cost decision: Estimate expected imbalance reduction and subtract/penalize migration overhead and cache-locality costs. Migrate only when the estimated score exceeds a configurable threshold. The initial model is heuristic and should be described honestly unless calibrated experimentally.
6. Affinity management: Inspect and change affinity using sched_getaffinity() and sched_setaffinity(). Linux affinity is per-thread/task: changing one thread's affinity does not automatically change every thread in the process.
7. Safety and observability: Include dry-run mode, decision logs (including rejected migrations), rate limits/cooldowns, validation of CPU IDs and allowed sets, safe handling of exiting/reused PIDs/TIDs and affinity failures, and a way to stop/disable the controller. Operate only on processes the user owns or is authorized to manage.
8. Evaluation: Compare ordinary Linux scheduling (baseline), static affinity (for example, taskset), and the adaptive controller. Use repeatable CPU-bound and mixed workloads, multiple runs, CPU imbalance, execution time/throughput, and number of affinity changes; optionally use performance counters. Lower imbalance alone does not prove a performance improvement; discuss trade-offs and negative results honestly.
Thread-level management decision
Thread-level monitoring/affinity management was not an explicit mandatory requirement in the original scope. The project now includes a thread-monitoring module as an advanced extension within the same architecture, not as a separate project. Thread monitoring exists; automatic thread-level affinity management is not implemented yet.
The thread-level extension should:
- Enumerate threads under /proc/<PID>/task/<TID>/ and read /proc/<PID>/task/<TID>/stat.
- Estimate per-thread CPU usage across sampling intervals, handling thread exit and TID reuse where feasible.
- Inspect each thread's current affinity and eligible destination CPUs during candidate evaluation.
- Select a thread when thread-level movement is more appropriate than process-level handling.
- Apply affinity to the relevant Linux task/thread using the affinity system calls.
- Reuse the existing persistence, hysteresis, cooldown, candidate-selection, benefit-versus-cost, logging, and safety policies.
- Evaluate both load-distribution effects and monitoring/controller overhead.
Current implementation status (2026-10-15)
- src/cpu_monitor.c: reads per-CPU counters from /proc/stat, calculates utilization from counter deltas, and reports average/min/max utilization and imbalance. Corrected version compiled with -std=c11 -Wall -Wextra -Wpedantic -O2 in the assistant environment.
- src/process_monitor.c: estimates process CPU usage from /proc/<PID>/stat, handles process names containing spaces/parentheses, and uses process start time to reduce PID-reuse confusion. Corrected version compiled with the same warning flags.
- src/affinity_test.c: inspects, temporarily changes, verifies, and restores affinity using sched_getaffinity() / sched_setaffinity(). Corrected version compiled with the same warning flags. It targets one Linux task/thread; it does not automatically change all threads in a process.
- src/thread_monitor.c: new module that enumerates visible threads from /proc/<PID>/task/, parses /proc/<PID>/task/<TID>/stat, estimates per-thread CPU usage from sampling deltas, tracks PID/TID plus task start time, handles common exit/race/invalid-sample cases, and displays the top CPU-consuming threads. It compiled cleanly with -std=c11 -Wall -Wextra -Wpedantic -O2 and completed a short runtime smoke test in the assistant environment. This is not yet runtime-tested in the user's WSL environment.
- The corrected versions of cpu_monitor.c, process_monitor.c, and affinity_test.c were uploaded to the Library under /adaptive_cpu_affinity_manager/src/. thread_monitor.c is now also stored in that directory.
- Not yet implemented/integrated: persistent imbalance detector, candidate selection, benefit-versus-cost policy, automatic thread affinity selection/change, main controller, full safeguards/dry-run integration, and systematic evaluation.
- Compilation/smoke tests in the assistant environment are not a substitute for testing in the user's WSL/Linux environment.
Recommended next steps
1. User copies the complete src/thread_monitor.c source into their local VS Code/WSL project and compiles/tests it.
2. Design a shared snapshot/interface for CPU utilization and per-thread CPU usage before integrating modules. Avoid coupling standalone monitors by scraping their terminal output; later modules should share structured data or use common headers/functions.
3. Implement persistent imbalance detection with multi-sample confirmation, hysteresis, and cooldown.
4. Implement candidate selection using recent per-thread CPU usage and the thread's current allowed CPU mask.
5. Implement benefit-versus-cost scoring and a dry-run-first affinity controller; log accepted and rejected decisions.
6. Evaluate baseline Linux scheduling vs static affinity vs adaptive control on repeatable workloads.
Do not rewrite unrelated modules unnecessarily. When fixing code, provide complete updated files, explain changes, and handle edge cases proactively.
Known starting-point formulas
For each logical CPU i:
- Total_i = user + nice + system + idle + iowait + irq + softirq + steal
- DeltaTotal_i = Total_i(t) - Total_i(t-1)
- Idle_i = idle + iowait
- DeltaIdle_i = Idle_i(t) - Idle_i(t-1)
- U_i = 100 * (1 - DeltaIdle_i / DeltaTotal_i)
- U_avg = (1/N) * sum(U_i)
- I = U_max - U_min
Guard zero deltas, counter resets/decreases, unavailable CPUs, and invalid samples. A starting experimental configuration discussed was approximately a 1-second sampling interval, an imbalance threshold around 30 percentage points, persistence for 3 samples, and a 5-second cooldown; these are tunable starting values, not universal truths.
For a simplified two-CPU model:
- U_src' = U_src - PUtil_p
- U_dst' = U_dst + PUtil_p
- I_before = abs(U_src - U_dst)
- I_after = abs(U_src' - U_dst')
- Benefit = I_before - I_after
- C(p) = C0 + lambda * M_p
- Score = Benefit - C(p)
Move only when the score clears a threshold and the candidate/destination are valid. This is a heuristic: process CPU usage does not necessarily equal the load removed from a particular CPU, especially for multithreaded processes.
Shared monitoring interface (implementation step in progress)
A shared header, monitor_data.h, defines the intended contract for the
monitoring modules and future controller. It provides CpuSample,
ProcessSample, ThreadSample, CpuUtilizationSummary, and
MonitorSnapshot, plus snapshot lifecycle functions and the planned
monitor_collect_snapshot() API.
Important interface rules:
- A first snapshot is a baseline; utilization is invalid until a matching prior
  cumulative counter and elapsed interval exist.
- Match CPUs by CPU ID, processes by PID + process start time, and threads by
  PID + TID + thread start time to reduce PID/TID-reuse errors.
- Use CLOCK_MONOTONIC for sample timing.
- Distinguish collection-level errors from partial samples; a process or thread
  disappearing during /proc enumeration is a normal race and should not
  invalidate every other observation.
- CPU utilization is bounded to 0–100% per logical CPU. Process and thread
  utilization are each normalized to one logical CPU, so a multithreaded
  process can exceed 100%.
- Process and thread measurements are alternative levels of detail. Never sum
  process utilization and its constituent thread utilization as separate work.
- monitor_collect_snapshot() is implemented in monitor_data.c; compile it
  together with a client that includes monitor_data.h. The three existing
  command-line monitors are intentionally unchanged and remain independently
  runnable. Refactoring them to share this collector is a later, controlled step.
- Always call monitor_snapshot_destroy() before reusing a populated snapshot.
  Initialize snapshots with monitor_snapshot_init() before first use.
- Process/thread matching currently uses linear searches through the previous
  snapshot. This is simple for the first implementation but can become O(N²);
  measure overhead and optimize the lookup strategy if experiments justify it.