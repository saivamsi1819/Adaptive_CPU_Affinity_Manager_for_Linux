#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXPERIMENT="$ROOT/scripts/dry_run_candidate_experiment.sh"
RUNS="${1:-5}"

if [[ ! "$RUNS" =~ ^[1-9][0-9]*$ ]] || ((RUNS > 25)); then
  echo "Usage: $0 [runs: 1..25]" >&2
  exit 2
fi
if [[ ! -x "$ROOT/execute/cpu_affinity_manager" ]]; then
  echo "Controller binary missing. Run: make -C '$ROOT' all" >&2
  exit 1
fi

mkdir -p "$ROOT/results"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
csv="$ROOT/results/locality_policy_comparison_${stamp}.csv"
printf 'trial,sample,policy,status,approved,tid,source_cpu,destination_cpu,projected_benefit_pp,modeled_cost_pp,score_pp,log_file\n' >"$csv"

for ((trial=1; trial<=RUNS; ++trial)); do
  log="$ROOT/results/locality_${stamp}_trial${trial}_same_snapshot.log"
  status=complete
  if ! bash "$EXPERIMENT" compare >"$log" 2>&1; then
    status=failed
    echo "Trial $trial failed; see $log" >&2
  fi
  parsed_rows="$(awk -v trial="$trial" -v logfile="$log" '
    /^\[policy-compare\]/ {
      sample=policy=state=tid=source=destination=benefit=cost=score=result=""
      for (i=2; i<=NF; ++i) {
        split($i, pair, "=")
        key=pair[1]; value=pair[2]
        if (key == "sample") sample=value
        else if (key == "policy") policy=value
        else if (key == "status") state=value
        else if (key == "tid") tid=value
        else if (key == "source") source=value
        else if (key == "destination") destination=value
        else if (key == "benefit") benefit=value
        else if (key == "cost") cost=value
        else if (key == "score") score=value
        else if (key == "result") result=value
      }
      sub(/pp$/, "", benefit); sub(/pp$/, "", cost); sub(/pp$/, "", score)
      approved=(result == 3 ? 1 : 0)
      printf "%d,%s,%s,%s,%d,%s,%s,%s,%s,%s,%s,%s\n", trial,sample,policy,state,approved,tid,source,destination,benefit,cost,score,logfile
      count++
    }
    END { if (count == 0) exit 1 }
  ' "$log")" || status=failed

  if [[ -n "$parsed_rows" ]]; then
    printf '%s\n' "$parsed_rows" >>"$csv"
  else
    printf '%d,,both,%s,0,,,,,,,%s\n' "$trial" "$status" "$log" >>"$csv"
  fi
  printf 'Recorded same-snapshot trial %d/%d status=%s\n' "$trial" "$RUNS" "$status"
done

printf 'Same-snapshot locality comparison saved to %s\n' "$csv"
printf 'Both policies scored the same sampled state; controller actions remained dry-run.\n'
