#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTROLLER="$ROOT/execute/cpu_affinity_manager"
INTERVAL_MS="${INTERVAL_MS:-500}"
DURATION_SECONDS="${DURATION_SECONDS:-7}"

if [[ ! -x "$CONTROLLER" ]]; then
  echo "Controller binary missing. Run: make -C '$ROOT' all test" >&2
  exit 1
fi

allowed_list="$(awk '/Cpus_allowed_list:/ {print $2}' /proc/self/status)"
first_cpu="$(awk -F '[-,]' '{print $1}' <<< "$allowed_list")"
if [[ -z "$first_cpu" ]]; then
  echo "Could not determine an allowed CPU from /proc/self/status" >&2
  exit 1
fi

worker_pids=()
cleanup_workers() {
  for pid in "${worker_pids[@]:-}"; do
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
  worker_pids=()
}
cleanup() { cleanup_workers; }
trap cleanup EXIT INT TERM

start_worker() {
  local mode="$1"
  if [[ "$mode" == "restricted" ]]; then
    taskset -c "$first_cpu" yes > /dev/null &
  else
    yes > /dev/null &
  fi
  worker_pids+=("$!")
}

run_case() {
  local name="$1" count="$2" mode="${3:-unrestricted}"
  echo
  echo "================================================================"
  echo "CASE: $name"
  echo "================================================================"
  cleanup_workers
  for ((i=0; i<count; i++)); do start_worker "$mode"; done
  sleep 1
  local rc=0
  timeout "$DURATION_SECONDS" "$CONTROLLER" --dry-run --interval-ms "$INTERVAL_MS" || rc=$?
  if [[ "$rc" -ne 0 && "$rc" -ne 124 ]]; then
    echo "Controller returned unexpected status $rc in case: $name" >&2
    cleanup_workers
    return "$rc"
  fi
  cleanup_workers
  sleep 1
}

run_case "baseline: no synthetic CPU-bound workers" 0
run_case "one unrestricted CPU-bound worker" 1 unrestricted
run_case "two unrestricted CPU-bound workers" 2 unrestricted
run_case "two workers both restricted to one CPU (negative affinity edge case)" 2 restricted

echo
echo "Runtime matrix completed. Every controller invocation used --dry-run."
echo "A rejected move is not automatically a failure; inspect the policy reason."
