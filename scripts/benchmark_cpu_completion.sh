#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCHMARK="$ROOT/execute/cpu_workload_benchmark"
CONTROLLER="$ROOT/execute/cpu_affinity_manager"
RESULTS_DIR="$ROOT/results"

workers="${1:-2}"
iterations="${2:-2000000000}"
skew_percent="${3:-100}"
runs="${4:-3}"

if [[ ! "$workers" =~ ^[1-9][0-9]*$ ]] || ((workers > 64)) ||
   [[ ! "$iterations" =~ ^[1-9][0-9]*$ ]] || ((iterations < 1000 || iterations > 5000000000)) ||
   [[ ! "$skew_percent" =~ ^[0-9]+$ ]] || ((skew_percent > 900)) ||
   [[ ! "$runs" =~ ^[1-9][0-9]*$ ]] || ((runs > 25)); then
  echo "Usage: $0 [workers: 1..64] [base_iterations: 1000..5000000000] [skew_percent: 0..900] [runs: 1..25]" >&2
  exit 2
fi

if [[ ! -x "$BENCHMARK" ]]; then
  echo "Benchmark binary missing. Run: make -C '$ROOT' benchmark" >&2
  exit 1
fi
if [[ ! -x "$CONTROLLER" ]]; then
  echo "Controller binary missing. Run: make -C '$ROOT' all" >&2
  exit 1
fi
if ! command -v taskset >/dev/null 2>&1; then
  echo "taskset is required for the static-affinity comparison." >&2
  exit 1
fi

allowed_list="$(awk '/Cpus_allowed_list:/ {print $2}' /proc/self/status)"
cpus=()
IFS=',' read -ra ranges <<< "$allowed_list"
for range in "${ranges[@]}"; do
  if [[ "$range" == *-* ]]; then
    first="${range%-*}"
    last="${range#*-}"
  else
    first="$range"
    last="$range"
  fi
  for ((cpu=first; cpu<=last; ++cpu)); do cpus+=("$cpu"); done
done
if (("${#cpus[@]}" < 2)); then
  echo "At least two allowed logical CPUs are required." >&2
  exit 1
fi
static_mask="${cpus[0]},${cpus[1]}"

mkdir -p "$RESULTS_DIR"
run_id="$(date -u +%Y%m%dT%H%M%SZ)_$$"
summary="$RESULTS_DIR/cpu_completion_${run_id}.csv"
printf 'trial,case,workers,base_iterations,skew_percent,affinity_mask,elapsed_seconds,completed_iterations,checksum,decisions,dry_run_proposals,applied_actions,restored_leases,log_file\n' > "$summary"

manager_pid=""
manager_log=""
cleanup_manager() {
  if [[ -n "$manager_pid" ]]; then
    kill -INT "$manager_pid" 2>/dev/null || true
    wait "$manager_pid" 2>/dev/null || true
    manager_pid=""
  fi
}
trap cleanup_manager EXIT INT TERM

run_case() {
  local trial="$1" case_name="$2" mask="" log_file output rc=0
  log_file="$RESULTS_DIR/cpu_completion_${run_id}_${trial}_${case_name}.log"

  if [[ "$case_name" == "static_affinity" || "$case_name" == "adaptive_apply" ]]; then
    mask="$static_mask"
  fi

  if [[ "$case_name" == "dry_run_controller" || "$case_name" == "adaptive_apply" ]]; then
    manager_log="$RESULTS_DIR/cpu_completion_${run_id}_${trial}_controller.log"
    : > "$manager_log"
    controller_mode="--dry-run"
    if [[ "$case_name" == "adaptive_apply" ]]; then
      controller_mode="--apply"
    fi
    "$CONTROLLER" "$controller_mode" --interval-ms 1000 > "$manager_log" 2>&1 &
    manager_pid="$!"
    local deadline=$((SECONDS + 10))
    until grep -q '^\[monitor\] baseline collected:' "$manager_log"; do
      if ! kill -0 "$manager_pid" 2>/dev/null || ((SECONDS >= deadline)); then
        cat "$manager_log" >&2
        echo "Controller did not collect its baseline." >&2
        cleanup_manager
        return 1
      fi
      sleep 0.05
    done
  fi

  printf 'trial=%s case=%s mask=%s\n' "$trial" "$case_name" "${mask:-all-allowed}" > "$log_file"
  if [[ -n "$mask" ]]; then
    if output="$(taskset -c "$mask" "$BENCHMARK" "$workers" "$iterations" "$skew_percent" 2>&1)"; then
      rc=0
    else
      rc=$?
    fi
  else
    if output="$("$BENCHMARK" "$workers" "$iterations" "$skew_percent" 2>&1)"; then
      rc=0
    else
      rc=$?
    fi
  fi
  printf '%s\n' "$output" | tee -a "$log_file"

  if [[ "$case_name" == "dry_run_controller" || "$case_name" == "adaptive_apply" ]]; then
    cleanup_manager
    cat "$manager_log" >> "$log_file"
  fi
  if ((rc != 0)); then
    echo "Benchmark case failed: $case_name (status $rc)" >&2
    return "$rc"
  fi

  elapsed="$(sed -n 's/.*elapsed_seconds=\([^ ]*\).*/\1/p' <<< "$output")"
  completed="$(sed -n 's/.*completed_iterations=\([^ ]*\).*/\1/p' <<< "$output")"
  checksum="$(sed -n 's/.*checksum=\([^ ]*\).*/\1/p' <<< "$output")"
  if [[ -z "$elapsed" || -z "$completed" || -z "$checksum" ]]; then
    echo "Could not parse benchmark output; see $log_file" >&2
    return 1
  fi

  decisions=0
  proposals=0
  applied_actions=0
  restored_leases=0
  if [[ "$case_name" == "dry_run_controller" || "$case_name" == "adaptive_apply" ]]; then
    decisions="$(grep -c '^\[decision\]' "$manager_log" || true)"
    if [[ "$case_name" == "dry_run_controller" ]]; then
      proposals="$(grep -c '^\[dry-run\] would set ' "$manager_log" || true)"
    else
      applied_actions="$(grep -c '^\[action\] applied:' "$manager_log" || true)"
      restored_leases="$(grep -c '^\[lease\] restored TID' "$manager_log" || true)"
    fi
  fi
  printf '%s,%s,%s,%s,%s,"%s",%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$trial" "$case_name" "$workers" "$iterations" "$skew_percent" \
    "${mask:-all-allowed}" "$elapsed" "$completed" "$checksum" \
    "$decisions" "$proposals" "$applied_actions" "$restored_leases" \
    "$log_file" >> "$summary"
  printf '  completed in %ss; decisions=%s dry-run proposals=%s applied=%s restored=%s\n' \
    "$elapsed" "$decisions" "$proposals" "$applied_actions" "$restored_leases"
}

printf 'Fixed-work CPU completion comparison: workers=%s base_iterations=%s skew=%s%% runs=%s\n' \
  "$workers" "$iterations" "$skew_percent" "$runs"
printf 'Static/adaptive mask=%s; dry-run observer never applies affinity; adaptive_apply temporarily changes owned worker affinity and restores its lease.\n' \
  "$static_mask"
printf 'CSV: %s\n' "$summary"

cases=(baseline static_affinity dry_run_controller adaptive_apply)
for ((trial = 1; trial <= runs; ++trial)); do
  rotation=$(((trial - 1) % ${#cases[@]}))
  for ((offset = 0; offset < ${#cases[@]}; ++offset)); do
    index=$(((rotation + offset) % ${#cases[@]}))
    run_case "$trial" "${cases[index]}"
  done
done

printf '\nResults saved to %s\n' "$summary"
printf 'The benchmark reports fixed-work completion time. Dry-run results measure controller overhead only. adaptive_apply is an explicit live-affinity experiment; inspect its action and restoration counts before interpreting timing.\n'
