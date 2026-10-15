# Project 3: SSD Performance Profiling (ECSE 4320/6320)

fio-based characterization of the laptop NVMe SSD (WD PC SN740 512 GB, accessed from WSL2): QD=1 latency, block-size and pattern sweeps, read/write mixes, the queue-depth throughput-latency knee, tail latency, LBA range and device state, and two misleading setups.

**Report:** [REPORT.md](REPORT.md) · **Processed tables:** [results/processed/summary.md](results/processed/summary.md) · **Figures:** [figures/](figures)

## Safety

All I/O goes to **one disposable 8 GiB test file** (`~/fio-test/testfile`) on the WSL2 ext4 filesystem. No raw device or partition is touched. `run_experiments.py` refuses `/dev/*` targets. Delete the file afterwards with `rm ~/fio-test/testfile`.

## Layout

| path | what |
|---|---|
| `scripts/run_experiments.py` | generates one `.fio` job file per run in `configs/`, runs it, saves fio JSON to `results/raw/<exp>/` |
| `configs/<exp>/*.fio` | the exact fio job files used (global section + job) |
| `scripts/parse_plot.py` | JSON -> `results/processed/*.csv`, `summary.md`, `figures/` |
| `scripts/collect_env.sh` | kernel, fio version, CPU, filesystem, block-queue settings -> `results/env.txt` |
| `run_all.sh` | env capture + all experiments (3 repeats) + plots |

## Run

```bash
sudo apt install fio            # or: conda install -c conda-forge fio
./run_all.sh                    # ~35 min; leave the machine idle
```

## Fixed settings (all runs unless noted)

`ioengine=libaio`, `direct=1` (O_DIRECT, bypasses the page cache), `norandommap`, `randrepeat=0`, incompressible data (`refill_buffers`, `buffer_compress_percentage=0`), 10 s measured after 2 s ramp, 3 repeats, latency = fio total latency (submission + completion) with percentiles 50/90/95/99/99.9/99.99. Block sizes are multiples of 4 KiB and fio aligns offsets to the block size.

| experiment | varied | fixed |
|---|---|---|
| `qd1` | 4k randread/randwrite, 128k seq read/write | QD=1 |
| `bs` | 4k to 1m, random vs sequential read | QD=32 |
| `mix` | 100/70/50/0 % read | 4k random, QD=32 |
| `qd` | iodepth 1 to 256; then 32 x 2/4/8 jobs | 4k random read |
| `ws` | LBA range 64 MiB to 8 GiB | 4k random read, QD=32 |
| `state` | 180 s sustained, 1 s bandwidth log | 4k random write, QD=32 |
| `pitfall` | direct vs buffered (cold, and warm with `invalidate=0`); psync vs libaio at iodepth=32 | 4k random read |
