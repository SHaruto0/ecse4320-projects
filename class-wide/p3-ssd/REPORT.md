# Project 3 Report: SSD Performance Profiling

Haruto Suzuki, ECSE 4320/6320. All numbers come from fio JSON in `results/raw/` (3 repeats per point unless noted). Tables are in `results/processed/summary.md`; job files are in `configs/`. Values are medians, with min..max of the 3 repeats in brackets or as error bars. Latency is fio total latency (submission + completion).

## 1. AI-use note

- **Tools.** Claude (Anthropic) generated the fio experiment runner, the job-file templates, the JSON parser and plotting script, and a report draft.
- **What I did and checked myself.** I ran every experiment on my laptop, confirmed the target was a disposable test file (not a device), looked up the SSD model, and checked the quoted numbers against the CSVs.
- **Rejected or changed.**
  - **The first buffered-I/O pitfall didn't show the page cache at all.** Buffered and direct reads were about equal (6.1k vs 4.4k IOPS), because fio invalidates the file's page cache before each job (`invalidate=1` by default). The test was redesigned: warm the cache with a sequential read, then run with `invalidate=0` (Section 9).
  - **The first throughput-latency plot joined the iodepth sweep and the multi-job points into one line.** The two use different submission paths, so they were split into two curves.
- **Not done.** No SMART temperature or throttling data. Reading it needs admin rights on Windows, and the WSL2 guest can't see the physical NVMe device.

## 2. Setup and methodology

| item | value |
|---|---|
| SSD | WD PC SN740 512 GB (SDDQNQD-512G-1201), NVMe, PCIe 4.0 x4 client drive |
| path to the SSD | fio (libaio, O_DIRECT) -> WSL2 ext4 -> Hyper-V virtual SCSI disk (VHDX) -> Windows NTFS -> NVMe driver -> SSD |
| target | one 8 GiB test file, `~/fio-test/testfile`, on the WSL2 ext4 root (`/dev/sdd`, 1 TB virtual disk). Never a raw device; the script refuses `/dev/*` |
| guest block queue | scheduler `none`, `nr_requests=950`, 4 KiB-aligned I/O (block sizes are multiples of 4 KiB) |
| OS / tools | Ubuntu on WSL2, kernel 6.18.54.1-microsoft-standard-WSL2; fio 3.36 |
| CPU / memory | Intel Core i7-1355U; 32 GB DDR5 (15 GiB visible to WSL2) |
| I/O settings | `direct=1`, `ioengine=libaio`, `norandommap`, `randrepeat=0`, incompressible data (`refill_buffers`, `buffer_compress_percentage=0`) |
| timing | 2 s ramp (excluded) + 10 s measured per point; 3 full repeats; 180 s sustained-write run once |
| preconditioning | one sequential 1 MiB write fill of the whole 8 GiB file before any test |
| power | Windows "Best performance", on AC power, apps closed |

**What is actually measured.** A WSL2 file is not the bare SSD. Every I/O crosses the guest kernel, the Hyper-V storage stack and the Windows NTFS/NVMe path. So this report measures the SSD **as seen from a WSL2 VM**. Section 10 discusses how that shifts the absolute numbers. The shapes of the curves (scaling, knees, tails, state effects) are still governed by the SSD.

**Run-to-run variability.** Repeat 1 (14:53 to 15:03) ran 30 to 50% slower than repeats 0 and 2 in nearly every experiment, for example QD=32 random read at 51.9k, 16.1k and 34.9k IOPS. That looks like a Windows background task competing for the SSD during that window. Medians are reported, and claims are only made where effects exceed the min..max spread.

## 3. Pre-experiment hypotheses (written before running)

1. **QD=1:** 4 KiB random read about 50 to 100 us (NAND read plus controller and software stack). 4 KiB random write is faster at the median because writes land in the SLC/DRAM cache, but its p99 is worse. 128 KiB sequential is a few hundred us.
2. **Block size:** IOPS falls and MB/s rises with block size until the interface or controller ceiling (PCIe 4.0 x4: about 7 GB/s raw). Random and sequential converge at large blocks, because a 256 KiB+ random read already spans many flash pages and dies.
3. **Mix:** throughput drops as writes are added. Reads that queue behind program/erase operations show higher p99.
4. **QD:** IOPS scales almost linearly up to about QD 32 to 64 (enough requests to keep all flash dies busy), then flattens while latency keeps rising. Little's law, QD = IOPS x mean latency, should hold within a few percent, since fio keeps the queue full.
5. **Tails:** p99 and p99.9 grow faster than the median past the knee. Write tails are dominated by garbage collection.
6. **Device state:** sustained random writes slow down once the SLC cache fills.
7. **Pitfall:** buffered I/O reports RAM latency, not SSD latency. `iodepth` has no effect with a synchronous engine.

## 4. Zero-queue (QD=1) baselines (Fig. 1)

![qd1](figures/fig1_qd1_latency.png)

| workload | IOPS | MB/s | mean | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 4 KiB random read | 3,981 | 16.3 | 248 us | 208 | 477 | 815 | 1,237 |
| 4 KiB random write | 3,859 | 15.8 | 254 us | 192 | 528 | 668 | 1,942 |
| 128 KiB seq read | 2,559 | 335 | 387 us | 375 | 586 | 889 | 1,466 |
| 128 KiB seq write | 1,337 | 175 | 700 us | 334 | 1,073 | 9,372 | 13,959 |

- **Latency is 2 to 4x my hypothesis.** A 4 KiB read takes 208 us at the median, versus the 50 to 100 us typical of bare-metal NVMe. Section 9's direct-I/O control run on a 256 MiB range gives 212 us too, so this is a fixed per-I/O cost of the virtualized path (guest -> VHDX -> NTFS), not the flash.
- **Random write's median (192 us) is slightly below random read's (208 us).** Writes are acknowledged once they reach the controller's cache, while reads must wait for NAND. That confirms the first half of hypothesis 1.
- **Write tails are much heavier.** 128 KiB sequential write has p99 = 9.4 ms, 28x its median, and the mean (700 us) is twice the median. A few writes stall behind flushes or garbage collection. Reads stay within about 4x of the median at p99.

## 5. Block-size and pattern sweep (Fig. 2)

![blocksize](figures/fig2_blocksize.png)

Reads at QD=32, same runs for throughput and latency:

| block | random IOPS | random MB/s | seq IOPS | seq MB/s | random mean / p99 latency |
|---|---|---|---|---|---|
| 4 KiB | 28.8k | 118 | 26.7k | 109 | 1.1 / 7.0 ms |
| 16 KiB | 22.9k | 376 | 23.3k | 382 | 1.4 / 7.2 ms |
| 64 KiB | 17.5k | 1,150 | 22.7k | 1,486 | 1.8 / 6.3 ms |
| 128 KiB | 16.9k | 2,214 | 16.3k | 2,143 | 1.9 / 5.2 ms |
| 256 KiB | 8.9k | 2,346 | 9.8k | 2,564 | 3.6 / 14.5 ms |
| 1 MiB | 2.7k | 2,825 | 2.7k | 2,813 | 11.9 / 56.9 ms |

There are two regimes:

- **IOPS-bound (4 to 128 KiB).** IOPS only drops from 29k to 17k while the block grows 32x, so MB/s rises almost linearly with block size. Per-request cost dominates, and in this stack a large share of it is per-I/O software overhead.
- **Bandwidth-bound (256 KiB and up).** MB/s flattens at about **2.3 to 2.8 GB/s** and latency rises in proportion to block size, because each request now waits for data transfer. The best single repeat reached 4.9 GB/s (seq 1 MiB), so the median ceiling is set by the virtualized path and host contention, not by the PCIe 4.0 x4 link (about 7.9 GB/s raw).

**Random vs sequential** differ only at 32 to 64 KiB (sequential is 30 to 40% faster there) and converge from 128 KiB up. Hypothesis 2 is confirmed. Two effects close the gap: a large random read already spans several flash pages across dies, and the SSD's read-ahead has less to add.

Little's law checks the latency column: at QD=32 and 28.8k IOPS, mean latency should be 32 / 28.8k = 1.11 ms. The measured value is 1.109 ms.

## 6. Read/write mix (Fig. 3)

![mix](figures/fig3_rw_mix.png)

4 KiB random, QD=32:

| mix | total IOPS | read mean / p99 | write mean / p99 |
|---|---|---|---|
| 100% read | 56.7k | 0.56 / 1.96 ms | - |
| 70R / 30W | 48.7k | 0.69 / 2.51 ms | 0.59 / 4.05 ms |
| 50R / 50W | 35.3k | 1.17 / 4.05 ms | 0.77 / 6.52 ms |
| 100% write | 14.3k | - | 2.25 / **56.9 ms** |

- **Throughput falls as writes increase**, down 4x from all-read to all-write, which confirms hypothesis 3. NAND programs take roughly 10x longer than reads, and a die busy programming blocks any read queued to it.
- **Reads suffer from writes they don't issue.** Read p99 doubles from 1.96 to 4.05 ms at 50/50, which is read-behind-write interference.
- **Pure random writes have extreme tails.** p99 is 57 ms, 25x the mean. That's consistent with garbage collection pausing writes while it frees blocks (Section 8).

Caveat: the 100% read point (56.7k IOPS) is about 2x the same configuration in the block-size sweep (28.8k). Section 10 discusses this.

## 7. Queue depth: throughput-latency knee and Little's law (Fig. 4)

![qd knee](figures/fig4_qd_knee.png)

4 KiB random read, one fio job, iodepth 1 to 256:

| QD | 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 256 |
|---|---|---|---|---|---|---|---|---|---|
| kIOPS | 4.0 | 7.7 | 12.9 | 26.2 | 25.0 | 34.9 | 45.9 | **74.3** | 74.5 |
| mean latency (ms) | 0.25 | 0.26 | 0.31 | 0.30 | 0.64 | 0.91 | 1.39 | 1.72 | 3.44 |
| Little's N = IOPS x latency | 1.0 | 2.0 | 4.0 | 8.0 | 16.0 | 31.9 | 63.9 | 127.9 | 255.9 |

- **Linear region (QD 1 to 8).** Throughput scales 6.5x while latency rises only 23%. The SSD is serving the extra requests in parallel across its flash dies and channels.
- **Diminishing returns (QD 16 to 128).** Each doubling of QD now adds more latency than throughput. From QD 8 to 128, IOPS rises 2.8x while latency rises 5.7x.
- **Knee at QD ≈ 128.** Going from QD 128 to 256 adds no throughput (74.3k to 74.5k) and doubles latency (1.72 to 3.44 ms). Past that point, requests just wait in a queue. Hypothesis 4 predicted a knee at QD 32 to 64; the virtualized path's per-I/O overhead pushes it later, since more requests in flight are needed to cover the longer service time.
- **Little's law holds to 3 significant figures at every point** (last row). fio runs a closed loop that keeps exactly `iodepth` requests outstanding, so N = X x R is guaranteed, but the check still proves the requested queue depth was actually reached. Section 9 shows a setup where it isn't.

**Parallelism instead of depth.** Spreading the same total across jobs (32 x 1, 2, 4, 8 jobs) gets more: **92.5k IOPS at 128 outstanding (4 jobs)** vs 74.3k with one job at QD 128, at lower latency (1.38 vs 1.72 ms). One submitting thread hits its own limit: `sys_cpu` reaches 58% at QD 128 with one job. 8 jobs (256 outstanding) give no more than 4 (85k vs 92k), so the system-wide knee is about **90k IOPS at around 128 outstanding I/Os**.

## 8. Tail latency and device state (Fig. 5, Fig. 6)

![tail](figures/fig5_tail.png)

| 4 KiB random read | p50 | p95 | p99 | p99.9 | p99.9 / p50 |
|---|---|---|---|---|---|
| QD 1 | 177 us | 602 | 758 | 2,023 | 11x |
| QD 16 (mid) | 490 us | 1,434 | 1,974 | 5,734 | 12x |
| QD 128 (knee) | 1,581 us | 2,802 | 4,489 | 9,896 | 6x |
| 32 x 8 jobs (past knee) | 2,212 us | 7,504 | 14,484 | 27,656 | 13x |

- **At mid-QD the tail is set by device events.** The p99.9 / p50 ratio stays around 11 to 12x from QD 1 to 16: a fixed fraction of requests hits a busy die or a host hiccup, whatever the load.
- **At the knee, queueing dominates.** The median itself grows (1.6 ms) because every request waits behind about 127 others, so the whole distribution shifts right and the ratio shrinks to 6x.
- **Past the knee, the tail explodes.** With 8 jobs, p99.9 is 27.7 ms and p99 is 14.5 ms, 3x the knee's p99 for less than 1.2x the knee's throughput. Running a latency-sensitive service past the knee buys almost no throughput and costs a lot of tail.

![state](figures/fig6_state_range.png)

**Burst vs steady state (180 s sustained 4 KiB random write, QD=32, 1 s samples).**

| window | 0-20 s | 20-40 s | 40-60 s | 60-180 s |
|---|---|---|---|---|
| median MB/s | 162 | 133 | 109 | 60 to 66 |

Write throughput holds at about 147 MB/s (median) for the first 50 s, then drops off a cliff at about 55 s and settles at about 64 MB/s for the remaining two minutes, a **2.3x** loss. Over the whole run, p99.9 write latency is 54 ms. About 7.4 GB had been written when the cliff hit. This is the burst-to-steady-state transition from hypothesis 6. Early writes land in the SLC cache; once it fills, the drive writes directly to TLC while also garbage-collecting. There's a second explanation: 7.4 GB is close to one full overwrite of the 8 GiB test file. After that, random 4 KiB overwrites leave many partially valid blocks, and GC has to copy live data before it can erase. Both mechanisms would show this kind of sharp, permanent drop, and without SMART or controller counters I can't separate them.

Caveat: without SMART access I can't rule out thermal throttling as a contributor. The drop is too abrupt (one step at about 55 s) and too stable afterwards to look like a thermal ramp, though.

**LBA range (4 KiB random read, QD=32).** IOPS is 57k to 65k for 64 MiB to 1 GiB spans and falls to 34k at 4 GiB and 28k at 8 GiB, with median latency doubling. A likely cause is the SSD's mapping table. The SN740 is a client design that caches its logical-to-physical map in a small buffer (I believe it uses host memory buffer rather than on-drive DRAM). A small LBA span keeps its map entries cached, while a wide span forces an extra flash read to fetch the mapping before the data read. The min..max spread here is large (13.8k to 79k at 4 GiB), so the trend is suggestive, not conclusive.

## 9. Misleading setups (Fig. 7)

![pitfall](figures/fig7_pitfall.png)

**Pitfall 1: buffered I/O measures RAM, not the SSD.** 4 KiB random read, QD=1, 256 MiB range, 3 repeats:

| setup | IOPS | mean latency | p99 |
|---|---|---|---|
| direct I/O (correct) | 4.7k [4.6k..5.9k] | 212 us | 578 us |
| buffered, fio default (`invalidate=1`, cold cache) | 17.2k [14.0k..47.7k] | 58 us | 391 us |
| buffered, cache pre-warmed, `invalidate=0` | **863k** [783k..899k] | **1.0 us** | 2.0 us |

With the cache warm, buffered "SSD latency" is **1 us at 863k IOPS**, which is 212x lower latency and 185x more IOPS than the real device. That is DRAM speed through the page cache, and no NAND access happens at all.

The cold-cache buffered case is a subtler trap. fio drops the file's cached pages before the job (`invalidate=1`), so the run starts on the SSD. But every block it reads stays cached, and the 256 MiB range fits in memory, so later reads increasingly hit RAM. The result (58 us mean, with IOPS swinging 14k to 48k between repeats) describes neither the SSD nor RAM, and depends on how long the run lasted. In the first full run this case looked almost like direct I/O (6.1k IOPS). That's why the warm-cache version was added: it shows the effect unambiguously.

A benchmark that forgets `direct=1` reports somewhere between SSD and DRAM speed depending on cache state. The fix is `direct=1` plus checking that QD=1 latency is physically plausible for flash (tens to hundreds of us, never about 1 us).

**Pitfall 2: wrong effective queue depth.** `iodepth=32` with a synchronous engine (`psync`) gives **4.7k IOPS at 209 us, identical to QD=1** (4.7k at 212 us). With `libaio`, the same `iodepth=32` gives 56.1k IOPS at 569 us, 12x more. A synchronous engine blocks on each I/O, so `iodepth` is silently ignored. Little's law exposes it: N = 4.7k x 209 us = 0.99 outstanding, not 32. With libaio, N = 56.1k x 569 us = 31.9.

## 10. Anomalies and limitations

- **The virtualization layer dominates absolute latency.** QD=1 reads take about 210 us instead of the 50 to 100 us expected for this drive on bare metal, and the median bandwidth ceiling (2.8 GB/s) sits below the drive's rating. Running fio natively on Windows (`windowsaio`) or on bare-metal Linux would isolate the SSD better. The WSL2 file was used so the setup could be completely safe, with no raw device involved.
- **Same configuration, different experiments, different answers.** 4 KiB random read at QD=32 measured 28.8k (block-size sweep), 34.9k (QD sweep), 56.7k (mix), 56.1k (pitfall rerun) and 55.1k (1 GiB range) IOPS in median. These are the same workload at different times. The likely causes are host-side contention (Windows services and the VHDX on the system drive), the drive's background garbage collection after write-heavy experiments, and possibly the range-dependent mapping effect from Section 8. Within each experiment the trends are consistent; comparisons *across* experiments are only made at the level of factors of 2 or more.
- **Repeat 1 was disturbed** (Section 2). Using medians of 3 limits its impact, but more repeats would tighten the error bars.
- **No SMART or temperature data, no host-side device stats.** Thermal throttling can't be fully excluded for the sustained-write test.
- **Limited preconditioning.** One sequential fill of 8 GiB is not a full-drive steady state (SNIA PTS). The sustained-write run shows the transition but doesn't establish the drive's worst-case steady state.
- **The test file shares the drive with the OS.** Windows I/O to the same SSD can't be excluded during runs.
