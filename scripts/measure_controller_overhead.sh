#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTROLLER="$ROOT/execute/cpu_affinity_manager"
RESULTS_DIR="$ROOT/results"
TIME_BIN="/usr/bin/time"
TIMEOUT_BIN="$(command -v timeout || true)"

duration="${1:-10}"
runs="${2:-3}"
task_count="${3:-0}"
if [[ ! "$duration" =~ ^[1-9][0-9]*$ ]] || ((duration > 600)) ||
   [[ ! "$runs" =~ ^[1-9][0-9]*$ ]] || ((runs > 25)) ||
   [[ ! "$task_count" =~ ^[0-9]+$ ]] || ((task_count > 500)); then
  echo "Usage: $0 [duration_seconds: 1..600] [runs: 1..25] [idle_tasks: 0..500]" >&2
  exit 2
fi

if [[ ! -x "$CONTROLLER" ]]; then
  echo "Controller binary missing. Run: make -C '$ROOT' all" >&2
  exit 1
fi
if [[ ! -x "$TIME_BIN" || -z "$TIMEOUT_BIN" ]]; then
  echo "GNU time and timeout are required; no controller was started." >&2
  exit 1
fi

mkdir -p "$RESULTS_DIR"
run_id="$(date -u +%Y%m%dT%H%M%SZ)_$$"
summary="$RESULTS_DIR/controller_overhead_${run_id}.csv"
printf 'trial,utc_time,idle_tasks,elapsed_seconds,user_cpu_seconds,system_cpu_seconds,cpu_percent,timeout_status,samples,log_file\n' > "$summary"

printf 'Measuring controller overhead: %ss per run, %s runs, %s idle tasks.\n' \
  "$duration" "$runs" "$task_count"
printf 'Mode: --dry-run; helper tasks sleep and consume negligible CPU.\n'
timing_file=""
idle_pids=()
cleanup() {
  for pid in "${idle_pids[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  for pid in "${idle_pids[@]:-}"; do wait "$pid" 2>/dev/null || true; done
  if [[ -n "$timing_file" ]]; then
    rm -f "$timing_file"
  fi
}
trap cleanup EXIT INT TERM

for ((task = 0; task < task_count; ++task)); do
  sleep 600 &
  idle_pids+=("$!")
done
sleep 1

for ((trial = 1; trial <= runs; ++trial)); do
  log_file="$RESULTS_DIR/controller_overhead_${run_id}_${trial}.log"
  timing_file="$RESULTS_DIR/.controller_overhead_${run_id}_${trial}.tmp"

  if "$TIME_BIN" -f '%e,%U,%S' -o "$timing_file" \
      bash -c '
        timeout_bin="$1"
        duration="$2"
        controller="$3"
        "$timeout_bin" --signal=INT --kill-after=2s "$duration" \
          "$controller" --dry-run --interval-ms 1000
        status=$?
        printf "[timing-wrapper] timeout_status=%d\\n" "$status" >&2
        if ((status == 124)); then
          exit 0
        fi
        exit "$status"
      ' bash "$TIMEOUT_BIN" "${duration}s" "$CONTROLLER" \
      > "$log_file" 2>&1; then
    run_status=0
  else
    run_status=$?
  fi

  if [[ -s "$timing_file" ]]; then
    IFS=, read -r elapsed user_cpu system_cpu < "$timing_file"
    cpu_percent="$(awk -v user="$user_cpu" -v sys="$system_cpu" \
      -v elapsed="$elapsed" \
      'BEGIN { if (elapsed > 0) printf "%.3f%%", 100 * (user + sys) / elapsed }')"
  else
    elapsed=""
    user_cpu=""
    system_cpu=""
    cpu_percent=""
    run_status=125
  fi
  rm -f "$timing_file"
  timing_file=""

  samples="$(grep -c '^\[sample\]' "$log_file" || true)"
  timeout_status="$(sed -n 's/^\[timing-wrapper\] timeout_status=//p' \
    "$log_file" | tail -n 1)"
  run_time="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf '%d,%s,%d,%s,%s,%s,%s,%d,%s,%s\n' \
    "$trial" "$run_time" "$task_count" "$elapsed" "$user_cpu" "$system_cpu" \
    "$cpu_percent" "$timeout_status" "$samples" "$log_file" >> "$summary"
  printf 'Run %d: CPU=%s, elapsed=%ss, samples=%s, timeout_status=%s, measurement_status=%d\n' \
    "$trial" "$cpu_percent" "$elapsed" "$samples" \
    "${timeout_status:-missing}" "$run_status"
  printf 'Full controller log: %s\n' "$log_file"
done

printf '\nOverhead summary written to: %s\n' "$summary"
printf 'These are WSL dry-run measurements; they do not measure migration benefit.\n'
