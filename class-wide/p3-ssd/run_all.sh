#!/usr/bin/env bash
# Run everything: env capture, fio experiments (3 repeats), parse + plot.
# Usage: ./run_all.sh            (test file: ~/fio-test/testfile, 8 GiB; needs ~9 GB free)
set -e
TF=${TF:-$HOME/fio-test/testfile}; SIZE=${SIZE:-8G}
mkdir -p "$(dirname "$TF")"
avail=$(df -BG --output=avail "$(dirname "$TF")" | tail -1 | tr -dc 0-9)
[ "$avail" -ge 12 ] || { echo "need >= 12 GB free for an $SIZE test file (have ${avail}G)"; exit 1; }
bash scripts/collect_env.sh "$TF"
python3 scripts/run_experiments.py --file "$TF" --size "$SIZE" --repeat ${REPEAT:-3}
python3 scripts/parse_plot.py
echo "done. Delete the test file when finished: rm $TF"
