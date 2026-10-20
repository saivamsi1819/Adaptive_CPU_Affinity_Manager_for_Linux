#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTROLLER="$ROOT/execute/cpu_affinity_manager"
POLICY="${1:-locality}"
INTERVAL_MS=250

if [[ "$POLICY" != "locality" && "$POLICY" != "no-locality" ]]; then
  echo "Usage: $0 [locality|no-locality]" >&2
  exit 2
fi
if [[ ! -x "$CONTROLLER" ]]; then
  echo "Controller binary missing. Run: make -C '$ROOT' all test" >&2
  exit 1
fi
for command_name in taskset timeout; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "$command_name is required; no workload was started." >&2
    exit 1
  fi
done

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
if ((${#cpus[@]} < 2)); then
  echo "At least two allowed logical CPUs are required." >&2
  exit 1
fi

source_cpu="${cpus[0]}"
source_core=""
destination_cpu=""
if [[ -r "/sys/devices/system/cpu/cpu${source_cpu}/topology/core_id" ]]; then
  source_core="$(cat "/sys/devices/system/cpu/cpu${source_cpu}/topology/core_id")"
fi
for cpu in "${cpus[@]:1}"; do
  if [[ -n "$source_core" && -r "/sys/devices/system/cpu/cpu${cpu}/topology/core_id" ]]; then
    candidate_core="$(cat "/sys/devices/system/cpu/cpu${cpu}/topology/core_id")"
    if [[ "$candidate_core" != "$source_core" ]]; then
      destination_cpu="$cpu"
      break
    fi
  fi
done
if [[ -z "$destination_cpu" ]]; then
  destination_cpu="${cpus[1]}"
fi

mkdir -p "$ROOT/results"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
log_file="$ROOT/results/lab_apply_smoke_${stamp}.log"
worker_pid=""
manager_pid=""
cleanup() {
  if [[ -n "$manager_pid" ]]; then
    kill -INT "$manager_pid" 2>/dev/null || true
    wait "$manager_pid" 2>/dev/null || true
    manager_pid=""
  fi
  for pid in "$worker_pid"; do
    if [[ -n "$pid" ]]; then kill "$pid" 2>/dev/null || true; fi
  done
  for pid in "$worker_pid"; do
    if [[ -n "$pid" ]]; then wait "$pid" 2>/dev/null || true; fi
  done
}
trap cleanup EXIT INT TERM

timeout 30s taskset -c "$source_cpu" yes >/dev/null &
worker_pid=$!
sleep 1

controller_args=(--apply --interval-ms "$INTERVAL_MS")
if [[ "$POLICY" == "no-locality" ]]; then
  controller_args+=(--no-locality)
fi
"$CONTROLLER" "${controller_args[@]}" >"$log_file" 2>&1 &
manager_pid=$!

until grep -q '^\[monitor\] baseline collected:' "$log_file"; do
  if ! kill -0 "$manager_pid" 2>/dev/null; then
    cat "$log_file"
    echo "Controller exited before collecting its baseline." >&2
    exit 1
  fi
  sleep 0.05
done
while [[ "$(grep -c '^\[sample\]' "$log_file" || true)" -lt 2 ]]; do
  if ! kill -0 "$manager_pid" 2>/dev/null; then
    cat "$log_file"
    echo "Controller exited before collecting two samples." >&2
    exit 1
  fi
  sleep 0.05
done

taskset -pc "$source_cpu,$destination_cpu" "$worker_pid" >/dev/null
expected_mask="$source_cpu,$destination_cpu"
action_deadline=$((SECONDS + 8))
while ! grep -q '^\[action\] applied:' "$log_file"; do
  if ! kill -0 "$manager_pid" 2>/dev/null || ((SECONDS >= action_deadline)); then
    cat "$log_file"
    echo "No affinity action was applied in the smoke-test window; no successful apply test is claimed." >&2
    exit 1
  fi
  sleep 0.05
done

applied_tid="$(sed -n 's/^\[action\] applied: TID \([0-9][0-9]*\).*/\1/p' "$log_file" | head -n 1)"
if [[ "$applied_tid" != "$worker_pid" ]]; then
  cat "$log_file"
  echo "Controller selected TID ${applied_tid:-unknown}, not the dedicated test worker $worker_pid." >&2
  exit 1
fi

restore_deadline=$((SECONDS + 8))
while ! grep -q "^\[lease\] restored TID $worker_pid original affinity" "$log_file"; do
  if ! kill -0 "$manager_pid" 2>/dev/null || ((SECONDS >= restore_deadline)); then
    cat "$log_file"
    echo "The test worker's lease did not log a normal restoration." >&2
    exit 1
  fi
  sleep 0.05
done

kill -INT "$manager_pid" 2>/dev/null || true
wait "$manager_pid" || true
manager_pid=""
observed_mask="$(taskset -pc "$worker_pid" | sed 's/.*current affinity list: //')"
cat "$log_file"
if [[ "$observed_mask" != "$expected_mask" ]]; then
  echo "FAIL: expected restored mask $expected_mask; observed $observed_mask" >&2
  exit 1
fi

printf '\nPASS: controller applied and restored TID %s to its original mask %s.\n' \
  "$worker_pid" "$observed_mask"
printf 'Log: %s\n' "$log_file"
