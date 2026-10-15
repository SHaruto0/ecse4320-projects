#!/usr/bin/env bash
# Build, record environment, run all experiments (3 repeats), plot.
# Usage: CPU=4 ./run_all.sh      (CPU = core for single-threaded tests; loaders use all others)
set -e
CPU=${CPU:-4}
make
bash scripts/collect_env.sh
python3 scripts/run_experiments.py --cpu "$CPU" --repeat ${REPEAT:-3}
python3 scripts/plot.py
