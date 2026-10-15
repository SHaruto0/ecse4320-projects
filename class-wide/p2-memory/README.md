# Project 2: Cache & Memory Performance Profiling (ECSE 4320/6320)

Measures the cache/DRAM hierarchy with a custom tool (`bin/mem`, an Intel-MLC-equivalent written in C): zero-queue latency per level, bandwidth by pattern/granularity/stride and read/write mix, the throughput-latency knee, working-set transitions, cache-miss and TLB-miss impact, and two benchmark pitfalls.

**Report:** [REPORT.md](REPORT.md) · **Processed tables:** [results/processed/summary.md](results/processed/summary.md) · **Figures:** [figures/](figures)

## Layout

| path | what |
|---|---|
| `src/mem.c` | the tool: `lat` (pointer chase), `bw` (bandwidth), `loaded` (latency under load), `kern` (saxpy + perf counters) |
| `src/chains.h` | K independent pointer chases kept in registers (memory-level-parallelism sweep) |
| `scripts/run_experiments.py` | all sweeps -> `results/raw/*.csv` (one row per run, `rep` column = repeat) |
| `scripts/plot.py` | figures -> `figures/`, tables -> `results/processed/` |
| `scripts/collect_env.sh` | CPU, OS, compiler, THP, memory -> `results/env.txt` |
| `machine.json` | clock estimate, cache sizes, TLB entries (used for annotations and cycle conversion) |

## Run

```bash
pip install pandas matplotlib tabulate
CPU=4 ./run_all.sh          # ~15-20 min, 3 repeats; don't use the machine meanwhile
```
Measured on: Intel Core i7-1355U, 32 GB DDR5-5200 (15 GiB visible to WSL2), Ubuntu 22.04 on WSL2, GCC 11.4, `CPU=4`, background apps closed.

Single commands, for example:

```bash
bin/mem lat --size 1073741824 --huge 1 --cpu 4                  # DRAM latency, huge pages
bin/mem lat --size 1073741824 --huge 1 --chains 16 --cpu 4      # 16 outstanding misses
bin/mem bw --size 2147483648 --cpus 0-11 --read-pct 70          # 70R/30W bandwidth, all cores
bin/mem loaded --chase-cpu 4 --load-cpus 0-3,5-11 --delay 100   # loaded latency
```

## Method notes

- **Latency:** a random cyclic permutation (Sattolo) of 64 B nodes, each holding the address of the next, so every load depends on the previous one and hardware prefetchers can't predict it. Time per load = load-to-use latency.
- **Memory:** `mmap`, 2 MiB aligned, explicitly `madvise(MADV_HUGEPAGE)` or `MADV_NOHUGEPAGE`. The achieved huge-page fraction is read from `/proc/self/smaps_rollup` and recorded per run (`huge_frac`). All memory is first-touched before timing.
- **Bandwidth:** each thread is pinned and streams its own slice; `read_pct` sets the share of 64 B accesses that are reads vs full-line writes. "Useful" GB/s counts bytes the program touched; "line" GB/s counts 64 B lines.
- **Intensity:** (1) one core with K = 1..32 independent chases; (2) a chasing thread on `CPU` while 0..N loader threads stream, or all loaders with an injected delay. Little's law: outstanding lines = bandwidth x latency / 64 B.
- **Repeats:** every experiment runs 3 times; plots show the median with min..max error bars.
- **Counters:** `kern` and `lat` read cycles, instructions, L1D/LLC/dTLB misses with `perf_event_open`, and print `NA` when the PMU isn't exposed (as in WSL2).
