#!/usr/bin/env bash
# Follow-up test for the DRAM 'mul' anomaly (native slower than scalar).
# H1 (run order / thermal): alternate scalar and native, 5 rounds.
# H2 (4K aliasing): shift z by 2112 B (33 lines) so z[i] no longer shares
#     its low 12 address bits with x[i], y[i].
CPU=${CPU:-4}; out=results/raw/anomaly_mul.csv
echo "round,zskew,$(bin/bench_native --header)" > $out
for r in 1 2 3 4 5; do
  for skew in 0 2112; do
    for v in native scalar; do
      echo "$r,$skew,$(taskset -c $CPU bin/bench_$v --kernel mul --dtype f64 --n 16777216 --trials 5 --zskew $skew)" >> $out
    done
  done
done
python3 - <<'PY'
import pandas as pd
d = pd.read_csv("results/raw/anomaly_mul.csv")
print(d.pivot_table(index="zskew", columns="variant", values="ns_per_elem", aggfunc=["median", "min", "max"]).round(3))
PY
