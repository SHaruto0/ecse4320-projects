#!/usr/bin/env bash
# One-shot: build, record env, verify vectorization, run experiments, counters, plots.
# Usage: CPU=2 GHZ=3.0 ./run_all.sh        (GHZ = your fixed core clock, for cycles/element)
set -e
CPU=${CPU:-2}; GHZ=${GHZ:-0}
make -j
bash scripts/collect_env.sh
python3 scripts/verify_vec.py > /dev/null && echo "wrote reports/vec_summary.md"
python3 scripts/run_experiments.py --cpu "$CPU" --ghz "$GHZ" --repeat ${REPEAT:-3}
command -v perf >/dev/null && CPU=$CPU bash scripts/perf_counters.sh || echo "perf not found, skipping counters"
python3 scripts/plot.py
