#!/usr/bin/env bash
# Hardware counters for a few representative configs -> results/raw/perf.txt
# Intel events; on AMD replace fp_arith_inst_retired.* with fp_ret_sse_avx_ops.all
CPU=${CPU:-2}; out=results/raw/perf.txt; : > $out
EV=cycles,instructions,fp_arith_inst_retired.scalar_single,fp_arith_inst_retired.128b_packed_single,fp_arith_inst_retired.256b_packed_single,fp_arith_inst_retired.512b_packed_single,L1-dcache-load-misses,LLC-load-misses
for cfg in "saxpy 2048" "saxpy 4194304" "dot 2048" "dot 4194304"; do
  set -- $cfg
  for v in scalar native native512 native_fm; do
    [ -x bin/bench_$v ] || continue
    echo "### $v $1 n=$2" >> $out
    perf stat -x, -e $EV taskset -c $CPU bin/bench_$v --kernel $1 --n $2 --trials 3 >> $out 2>&1 || \
      perf stat -x, -e cycles,instructions,cache-misses taskset -c $CPU bin/bench_$v --kernel $1 --n $2 --trials 3 >> $out 2>&1
  done
done
echo "wrote $out"
