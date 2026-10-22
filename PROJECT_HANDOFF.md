# Project Handoff: Adaptive CPU Affinity Manager

This file is the continuation brief for working on this project from another machine, including the institute's native Linux lab. Read it together with `DESIGN_REQUIREMENTS.md`; that document remains the detailed specification. Do not treat dry-run estimates as applied migrations or measured performance gains.

## Project goal

Build a Linux userspace controller that samples CPU and thread activity, detects persistent imbalance, chooses eligible threads and destination CPUs, scores candidate moves using estimated benefit and migration/locality cost, and optionally applies a bounded CPU-affinity change. The controller complements Linux's scheduler; it does not stop or replace it.

The normal mode is dry-run. Apply mode is opt-in and experimental. No WSL configuration, system scheduler settings, passwords, or unrelated machine configuration should be changed as part of project validation.

## Implemented so far

- CPU, process, and thread monitors read Linux `/proc` data. The controller uses structured snapshots from `monitor_data`.
- The controller integrates persistent-imbalance detection, candidate selection, topology/locality scoring, benefit-versus-cost decisions, cooldowns, action rate limits, and explanations in its logs.
- `--no-locality` disables the locality cost; `--compare-locality` scores both policy variants against the same snapshot and is dry-run only.
- Dry-run stress experiments, repeated decision sampling, same-snapshot policy comparisons, controller-overhead measurement, a fixed-work pthread benchmark, and a native-Linux apply smoke script are present under `scripts/`.
- Apply mode saves the original per-thread affinity and applies a five-second lease. It checks task identity and owner, restores on lease expiry or normal shutdown, and registers the lease with an independent guardian before changing affinity. The guardian attempts restoration if the controller exits unexpectedly.
- The isolated guardian test simulates a manager exit after changing affinity on a dedicated test worker. It does not exercise the full controller apply path.
- Build outputs in `execute/` and experimental output in `results/` are ignored by Git. Preserve result CSVs and logs for the report, but do not commit generated binaries.

## Evidence currently available

As of the latest recorded validation (2026-10-20), strict-warning WSL builds and all seven test programs passed. The dry-run controller produced candidate decisions, and synthetic WSL measurements compared baseline, static affinity, and dry-run observer overhead. Same-snapshot WSL trials showed that locality changes policy approvals.

These are development checks only. WSL scheduling and topology differ from native Linux. The integrated controller has not yet been validated in apply mode, and no adaptive performance improvement has been established. A guardian cannot guarantee recovery if the guardian or whole system is killed, the machine reboots, or the OS refuses restoration.

## Work remaining to complete the project

1. **Native Linux build and test:** confirm compiler and tools, then run the commands below. Fix any portability or kernel/topology issues found, while preserving logs.
2. **Integrated apply safety check:** run `scripts/lab_apply_smoke_test.sh locality`. Confirm it reports PASS and verifies the dedicated worker's original affinity mask. If it fails, stop before performance trials and inspect the saved log.
3. **Repeatable workload measurements:** run the fixed-work benchmark matrix covering baseline scheduling, static affinity, dry-run observation, and adaptive apply. Confirm benchmark checksums and completed work match; inspect applied-action and restored-lease counts. A run with no applied action does not evaluate adaptive migration.
4. **Broader overhead and dry-run checks:** repeat the controller-overhead and dry-run experiments on native Linux, record environment details, and retain CSVs/logs.
5. **Analyze performance:** compare repeated completion times and relevant load distribution across workloads. Report variability and failures. Do not claim improvement unless repeatable native-Linux data supports it. Compare locality and no-locality only on controlled, equivalent workloads; a higher approval count is not itself a performance gain.
6. **Resolve issues and finalize:** fix bugs revealed by lab runs, rerun strict build/tests and critical experiments, update `DESIGN_REQUIREMENTS.md` with actual results, and prepare the final report and limitations.

## First commands on native Linux

Run from the cloned repository root:

```bash
gcc --version
make --version
command -v stress-ng
make clean
make -j2 all benchmark test
bash scripts/lab_apply_smoke_test.sh locality
```

If the build/tests fail, or if the smoke test does not pass, save the complete output and stop there. Do not launch the adaptive performance matrix until the smoke test succeeds. The smoke script starts a CPU-bound worker constrained to one allowed CPU and caps its runtime; it does not change global scheduler settings.

After a successful smoke test, run the measurements documented in §14.1 of `DESIGN_REQUIREMENTS.md`:

```bash
bash scripts/benchmark_cpu_completion.sh 2 2000000000 100 5
bash scripts/benchmark_cpu_completion.sh 4 2000000000 200 5
bash scripts/measure_controller_overhead.sh 10 5 0
bash scripts/measure_controller_overhead.sh 10 5 25
bash scripts/measure_controller_overhead.sh 10 5 100
bash scripts/repeat_dry_run_experiment.sh 5
```

The adaptive benchmark invokes the controller with `--apply` for its explicit `adaptive_apply` case. Review the CSV columns `applied_actions` and `restored_leases`, plus the associated logs. If runs finish without actions, increase workload duration or tune the experiment after reviewing why no eligible candidate was selected; do not interpret those timings as adaptive results. The scripts may require `stress-ng` and GNU `time`; check availability before the full run.

Record at least:

- Git commit, date/time, Linux distribution and kernel (`uname -a`), CPU model, allowed CPU list (`grep Cpus_allowed_list /proc/self/status`), compiler and `stress-ng` versions.
- Exact command and workload parameters for each run.
- CSVs and logs from `results/`, including failures and runs with no action.
- Whether the smoke test restored the original mask and whether every adaptive benchmark lease restored normally.

`results/` is ignored by Git. Back it up separately or copy selected results into the project report after review. Do not discard the raw measurements.

## Instructions for the next Codex session

Start by inspecting the cloned repository, `PROJECT_HANDOFF.md`, and `DESIGN_REQUIREMENTS.md`. Check `git status` before editing and preserve existing user work. Run the first-command sequence above in order. Do not change WSL or system configuration, and do not claim project completion until native-Linux validation, apply safety, and repeated performance analysis have been completed and documented.
