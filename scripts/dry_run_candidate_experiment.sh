#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTROLLER="$ROOT/execute/cpu_affinity_manager"
INTERVAL_MS="${INTERVAL_MS:-250}"
POLICY="${1:-locality}"

if [[ "$POLICY" != "locality" && "$POLICY" != "no-locality" &&
      "$POLICY" != "compare" ]]; then
  echo "Usage: $0 [locality|no-locality|compare]" >&2
  exit 2
fi

if [[ ! -x "$CONTROLLER" ]]; then
  echo "Controller binary missing. Run: make -C '$ROOT' all" >&2
  exit 1
fi
if ! command -v stress-ng >/dev/null 2>&1; then
  echo "stress-ng is not installed; no workload was started." >&2
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
  for ((cpu=first; cpu<=last; ++cpu)); do
    cpus+=("$cpu")
  done
done

if ((${#cpus[@]} < 2)); then
  echo "At least two allowed logical CPUs are required." >&2
  exit 1
fi

source_cpu="${cpus[0]}"
destination_cpu=""
source_core=""
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

log_file="$(mktemp)"
hog_pid=""
candidate_pid=""
manager_pid=""
cleanup() {
  for pid in "$manager_pid" "$candidate_pid" "$hog_pid"; do
    if [[ -n "$pid" ]]; then kill "$pid" 2>/dev/null || true; fi
  done
  for pid in "$manager_pid" "$candidate_pid" "$hog_pid"; do
    if [[ -n "$pid" ]]; then wait "$pid" 2>/dev/null || true; fi
  done
  rm -f "$log_file"
}
trap cleanup EXIT INT TERM

# stress-ng creates sustained source pressure; the candidate shares that CPU.
taskset -c "$source_cpu" stress-ng --cpu 1 --cpu-load 100 \
  --cpu-load-slice 100 --timeout 7s --quiet &
hog_pid=$!
taskset -c "$source_cpu" yes >/dev/null &
candidate_pid=$!
sleep 1

# Widen the candidate immediately after two persistence samples. Keeping the
# observation window short helps preserve the source CPU as its last CPU.
controller_args=(--dry-run --interval-ms "$INTERVAL_MS")
if [[ "$POLICY" == "no-locality" ]]; then
  controller_args+=(--no-locality)
elif [[ "$POLICY" == "compare" ]]; then
  controller_args+=(--compare-locality)
fi
"$CONTROLLER" "${controller_args[@]}" \
  >"$log_file" 2>&1 &
manager_pid=$!
until grep -q "baseline collected" "$log_file"; do
  if ! kill -0 "$manager_pid" 2>/dev/null; then
    cat "$log_file"
    echo "Controller exited before baseline collection." >&2
    exit 1
  fi
  sleep 0.05
done
while [[ "$(grep -c '^\[sample\]' "$log_file" || true)" -lt 2 ]]; do
  if ! kill -0 "$manager_pid" 2>/dev/null; then
    cat "$log_file"
    echo "Controller exited before two persistence samples." >&2
    exit 1
  fi
  sleep 0.05
done
taskset -pc "$source_cpu,$destination_cpu" "$candidate_pid" >/dev/null
decision_deadline=$((SECONDS + 5))
while ! grep -q '^\[decision\]' "$log_file"; do
  if ! kill -0 "$manager_pid" 2>/dev/null || ((SECONDS >= decision_deadline)); then
    kill -INT "$manager_pid" 2>/dev/null || true
    wait "$manager_pid" 2>/dev/null || true
    manager_pid=""
    cat "$log_file"
    echo "Controller did not emit a decision within five seconds." >&2
    exit 1
  fi
  sleep 0.05
done
sleep 0.05
kill -INT "$manager_pid" 2>/dev/null || true
wait "$manager_pid" || true
manager_pid=""

printf 'Experiment policy=%s CPUs: source=%s destination=%s\n' \
  "$POLICY" "$source_cpu" "$destination_cpu"
cat "$log_file"
printf '\nThis experiment used --dry-run; no controller affinity action was applied.\n'
