# Project 1: SIMD Advantage Profiling (ECSE 4320/6320)

Scalar vs. auto-vectorized vs. CPU-targeted SIMD for four kernels (SAXPY, dot, elementwise multiply, 3-point stencil), swept across working-set size, alignment, tail length, stride/gather, and data type.

**Report:** [REPORT.md](REPORT.md) · **Processed tables:** [results/processed/summary.md](results/processed/summary.md) · **Figures:** [figures/](figures)

## Layout

| path | what |
|---|---|
| `src/kernels.c` | the kernels (only file compiled with per-variant flags) |
| `src/bench.c` | driver: init, correctness check vs. double-precision reference, calibrated timing, CSV row |
| `Makefile` | builds `bin/bench_<variant>` + vectorization reports + disassembly |
| `scripts/run_experiments.py` | all sweeps -> `results/raw/*.csv` |
| `scripts/verify_vec.py` | compiler-report + disassembly summary -> `reports/vec_summary.md` |
| `scripts/perf_counters.sh` | `perf stat` counters -> `results/raw/perf.txt` (not run: no perf in WSL2) |
| `scripts/plot.py` | figures -> `figures/`, processed tables -> `results/processed/` |
| `scripts/collect_env.sh` | hardware/OS/compiler/SMT -> `results/env.txt` |
| `scripts/anomaly_mul.sh` | follow-up test for the DRAM `mul` anomaly (run order vs 4K aliasing) |
| `machine.json` | clock, FMA units, cache sizes (roofline and cache markers) |

## Build variants

| variant | flags | purpose |
|---|---|---|
| scalar | `-O3 -march=native -fno-tree-vectorize -fno-tree-slp-vectorize` | same ISA, vectorizer off |
| auto | `-O3` | default auto-vectorization (x86-64 baseline = SSE2, 128-bit) |
| native | `-O3 -march=native` | CPU-targeted (256-bit AVX2 + FMA here) |
| native_fm | `-O3 -march=native -ffast-math` | allows FP reassociation (vectorized reductions) |
| native512 | `-O3 -march=native -mprefer-vector-width=512` | AVX-512 CPUs only; **not run** (i7-1355U has no AVX-512) |

The driver is always built with `-O2 -fno-tree-vectorize`, so only kernel code changes between variants.

## Measured on

Intel Core i7-1355U (Raptor Lake), 32 GB DDR5-5200 (15 GiB visible to WSL2), Ubuntu 22.04.5 on WSL2, GCC 11.4. Frequency, turbo and SMT can't be controlled from WSL2 (see REPORT.md section 2).

```bash
pip install pandas matplotlib tabulate
VARIANTS="scalar auto native native_fm" CPU=4 ./run_all.sh      # ~25 min, 3 repeats
make VARIANTS="scalar native" && CPU=4 bash scripts/anomaly_mul.sh   # ~3 min
```

## Reproduce on native Linux

```bash
sudo apt install build-essential python3-pandas python3-matplotlib python3-tabulate linux-tools-common linux-tools-$(uname -r)
bash scripts/setup_machine.sh            # performance governor, turbo off
# edit machine.json (lscpu shows cache sizes)
CPU=2 GHZ=<fixed clock> ./run_all.sh
```

Smoke test: `make && python3 scripts/run_experiments.py --quick --trials 3 --min-ms 5 && python3 scripts/plot.py`

Clang / Apple Silicon: `make CC=clang NATIVE=-mcpu=native VARIANTS="scalar auto native native_fm"`.

## Method notes

- Single-threaded, pinned with `taskset`. Each config: warm-up, then a rep count calibrated so one trial is >= 30 ms, then 7 trials; the median is reported. The whole sweep is repeated 3 times; plots show the median of the 3 run medians with min..max error bars, and speedup is computed per run.
- Data: uniform [-1, 1), fixed-seed xorshift; arrays first-touched before timing; 64 B aligned plus an optional element offset.
- Correctness: every run compares one kernel call against a double-precision scalar reference (relative tolerance 1e-5 f32, 1e-4 f32 dot, 1e-12 f64). Rows carry `max_err` and `pass`; 0 failures.
- Throughput uses *useful* bytes (no write-allocate traffic): saxpy 3 x size, dot 2, mul 3, stencil 2 per element.