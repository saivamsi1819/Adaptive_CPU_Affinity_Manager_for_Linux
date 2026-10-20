#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXPERIMENT="$ROOT/scripts/dry_run_candidate_experiment.sh"
RESULTS_DIR="$ROOT/results"

runs="${1:-5}"
if [[ ! "$runs" =~ ^[1-9][0-9]*$ ]] || ((runs > 25)); then
  echo "Usage: $0 [runs: 1..25]" >&2
  exit 2
fi

if [[ ! -f "$EXPERIMENT" ]]; then
  echo "Experiment script is missing: $EXPERIMENT" >&2
  exit 1
fi

mkdir -p "$RESULTS_DIR"
timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
run_id="${timestamp}_$$"
summary="$RESULTS_DIR/dry_run_trials_${run_id}.csv"
printf 'trial,utc_time,outcome,decisions,dry_run_proposals,log_file\n' > "$summary"

printf 'Running %s dry-run trials. Summary: %s\n' "$runs" "$summary"
for ((trial = 1; trial <= runs; ++trial)); do
  log_file="$RESULTS_DIR/dry_run_trial_${run_id}_${trial}.log"
  printf '\n===== Trial %d/%d =====\n' "$trial" "$runs"

  if bash "$EXPERIMENT" > "$log_file" 2>&1; then
    run_status=0
  else
    run_status=$?
  fi
  cat "$log_file"

  decisions="$(grep -c '^\[decision\]' "$log_file" || true)"
  proposals="$(grep -c '^\[dry-run\] would set ' "$log_file" || true)"

  if ((run_status != 0)); then
    outcome="error"
  elif ((proposals > 0)); then
    outcome="proposal"
  elif ((decisions > 0)); then
    outcome="rejected"
  else
    outcome="no_candidate"
  fi

  trial_time="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf '%d,%s,%s,%s,%s,%s\n' \
    "$trial" "$trial_time" "$outcome" "$decisions" "$proposals" \
    "$log_file" \
    >> "$summary"
  printf 'Trial result: %s (decisions=%s, dry-run proposals=%s)\n' \
    "$outcome" "$decisions" "$proposals"

  printf 'Full trial log: %s\n' "$log_file"
done

printf '\nTrial summary written to: %s\n' "$summary"
printf 'All controller runs used --dry-run; no affinity was changed.\n'
