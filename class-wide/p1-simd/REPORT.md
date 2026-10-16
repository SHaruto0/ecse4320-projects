# Project 1 Report: SIMD Advantage Profiling

Haruto Suzuki, ECSE 4320/6320. Every number here comes from `results/raw/*.csv` (3 full sweeps). The processed tables are in `results/processed/summary.md`.

## 1. AI-use note

I directed this project and used Claude (Anthropic) as a coding and writing assistant. I had it generate the benchmark driver, kernels, Makefile variants, experiment and plotting scripts, and a first report draft, then iterated with it on the analysis.

**My work:**

- Ran the whole project on my own hardware. Set up the WSL2 toolchain (repaired a broken apt/dpkg state, installed GCC and the Python stack) and moved the work off OneDrive to keep syncing out of the measurements.
- Characterized the machine: CPU and cache details from `lscpu`, memory configuration from Windows. Confirmed there's no AVX-512 and cut that build.
- Controlled the measurement conditions: timed all 12 vCPUs to choose the pinned core, ran on AC power in Best performance mode with background apps closed.
- Ran the full 3-repeat sweep and the follow-up test for the `mul` anomaly, which rejected run order and partly supported 4K aliasing.
- Reviewed each report draft against the rubric and my data, sent it back when claims overstated the results, and owned the final version.
- Set up and organized the GitHub repo so all three projects (and the upcoming track project) build and run from documented scripts.

**Changed during review (with Claude):**

- The first kernel file defined the kernels with a C macro, so the compiler's vectorization report pinned all 12 loops to the same source line. That made the report useless as evidence, so the kernels were rewritten as normal functions.
- GCC says the f32 dot loop was "vectorized using 32 byte vectors." The disassembly shows a strict in-order reduction, so the report wasn't taken at face value (Section 8).
- The first plots used ±std error bars, which went below zero for noisy points. They now show min..max across the 3 runs, with speedup computed per run.
- The first "L1" comparison used one N for every kernel, which actually put f64 `mul` (3 arrays, 69 KB) in L2. The L1 point is now picked per kernel and data type.
- The 5.0 GHz turbo clock in `machine.json` was replaced with a clock estimated from my own data (Section 2).
- The AVX-512 build was dropped because this CPU doesn't support AVX-512.

## 2. Environment and methodology

| item            | value                                                                                                                                                                                                  |
| --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| CPU             | Intel Core i7-1355U (Raptor Lake; 2 P-cores + 8 E-cores, 12 logical CPUs), AVX2 + FMA, no AVX-512                                                                                                      |
| caches (P-core) | L1d 48 KiB, L2 1.25 MiB per core; L3 12 MiB shared                                                                                                                                                     |
| memory          | 32 GB DDR5-5200 (15 GiB visible to WSL2)                                                                                                                                                               |
| OS              | Ubuntu 22.04.5 on WSL2, kernel 6.18.40.1-microsoft-standard-WSL2                                                                                                                                       |
| compiler        | GCC 11.4.0. Driver: `-O2 -fno-tree-vectorize`. Kernels: see the table below                                                                                                                            |
| frequency       | Windows "Best performance" power mode, on AC power. The governor and turbo can't be controlled from WSL2                                                                                               |
| SMT             | on                                                                                                                                                                                                     |
| pinning         | `taskset -c 4`                                                                                                                                                                                         |
| timing          | `clock_gettime(CLOCK_MONOTONIC)`; reps calibrated so each trial lasts at least 30 ms; 7 trials per config, median reported; whole sweep repeated 3 times; error bars are min..max of the 3 run medians |
| data            | uniform [-1, 1), fixed-seed xorshift; arrays 64 B aligned and first-touched before timing                                                                                                              |
| correctness     | every run checks one call against a double-precision scalar reference (relative tolerance 1e-5 f32, 1e-4 f32 dot, 1e-12 f64): 0 failures                                                               |

| build     | flags                                                           | vector width                   |
| --------- | --------------------------------------------------------------- | ------------------------------ |
| scalar    | `-O3 -march=native -fno-tree-vectorize -fno-tree-slp-vectorize` | none (scalar FMA allowed)      |
| auto      | `-O3`                                                           | 128-bit SSE2, no FMA           |
| native    | `-O3 -march=native`                                             | 256-bit AVX2 + FMA             |
| native_fm | `-O3 -march=native -ffast-math`                                 | 256-bit, reassociation allowed |

**Clock estimate.** I couldn't fix the clock, so I estimated it from two loops that are limited purely by instruction latency. The scalar dot product is a chain of dependent `vfmadd231ss` instructions (4-cycle latency) and runs at 1.29 ns/element. The `native` dot product is a chain of dependent `vaddss` (2-cycle latency) and runs at 0.636 ns/element. Both work out to about **3.1 GHz**. The exact 2.0 ratio also suggests the runs were on a P-core, since Golden Cove has a fast 2-cycle FP adder and an E-core wouldn't give that ratio. I use 3.1 GHz for the roofline and for cycle counts (cycles/element = ns/element x 3.1), and report ns/element as the main unit.

**Pinning caveat.** In WSL2, `taskset` pins to a virtual CPU, and Windows decides which physical core actually runs it. All 12 vCPUs timed within about 16% of each other on an L1 SAXPY test (0.049 to 0.057 ns/element), so I couldn't target a specific P-core.

## 3. Pre-experiment hypotheses (written before running)

1. **L1-resident:** speedup close to the lane count minus some overhead, so about 8x for f32 and 4x for f64 with 256-bit vectors. The 128-bit `auto` build should get about half of what `native` gets.
2. **Memory-bound:** past the LLC every build hits the same DRAM bandwidth, so speedup falls to about 1x. The stencil has the highest arithmetic intensity, so it should keep its gains the longest.
3. **Dot product:** without `-ffast-math` the compiler can't reorder the FP sum, so the loop stays latency-bound and wider vectors shouldn't help. Only `native_fm` should speed it up.
4. **Alignment:** a visible cost in L1 when 256-bit loads split cache lines, and basically nothing in DRAM.
5. **Tail:** time per call should show a sawtooth with a period equal to the vector width.
6. **Stride:** cost grows with how much of each cache line is wasted. Random gather should be the worst case in DRAM.
7. **f32 vs f64:** f32 has twice the lanes, so it should get about 2x the speedup when compute-bound.
8. **AVX-512:** not applicable on this CPU.

## 4. Vectorization verification

`reports/vec_summary.md` counts instruction types in each kernel's disassembly and lists the compiler messages. The key evidence:

- **scalar:** no "loop vectorized" messages, and `saxpy_f32` is one `vfmadd231ss` per element.
- **auto:** "loop vectorized using 16 byte vectors" for all 12 loops. Only `xmm` registers, `mulps`/`addps`, and no FMA, since the baseline x86-64 ISA doesn't have FMA.
- **native:** "32 byte vectors", and `saxpy_f32` uses `vfmadd231ps ymm`.
- **saxpy_stride_f32 (native):** the loop is versioned. The compiler reports both "couldn't vectorize loop / Loop costings may not be worthwhile" and "vectorized using 32 byte vectors." The vector path only runs when stride == 1, and every other stride falls back to scalar code (Section 7).
- **saxpy_gather_f32:** the gather count is 0 in every build. GCC never emits `vgatherdps`. It loads the 8 indexed values one at a time and inserts them into a vector.
- **dot_f32 (native):** reported as vectorized, but the inner loop is a `vmulps ymm` followed by 8 serial `vaddss` into a single accumulator (Section 8).

## 5. Baseline and locality (Fig. 1, Fig. 2)

![speedup](figures/fig1_speedup_vs_ws.png)
![locality](figures/fig2_locality_saxpy.png)

Speedup over scalar for `native` at the largest L1-resident N and at DRAM size (N = 16M):

| kernel          | f32 L1 | f32 DRAM | f64 L1 | f64 DRAM |
| --------------- | ------ | -------- | ------ | -------- |
| saxpy           | 6.6x   | 1.5x     | 3.7x   | 0.93x    |
| mul             | 6.6x   | 0.76x    | 3.7x   | 0.52x    |
| stencil         | 6.1x   | 1.3x     | 2.8x   | 1.3x     |
| dot (fast-math) | 9.7x   | 2.6x     | 4.4x   | 1.4x     |

The SIMD gain shrinks in steps that line up with the cache sizes. Using f32 SAXPY as the example:

- In L1 (up to 48 KiB), `native` moves about 190 GiB/s. That's about 0.057 ns/element, or roughly 5.6 elements per cycle at 3.1 GHz.
- In L2 it drops to about 100 GiB/s, which is still 4x scalar.
- Past L2 (1.25 MiB) it falls to 35 to 40 GiB/s, about 2x scalar.
- Past the 12 MiB LLC it sits at 15 to 20 GiB/s, which is the same as scalar within run-to-run noise.

Scalar SAXPY stays roughly flat at about 25 GiB/s through L1, L2 and L3. The scalar loop is limited to about one element per cycle (2 loads and 1 store per element), not by the caches. Once the vector code becomes bandwidth-bound, its lead shrinks to however much bandwidth the scalar loop was leaving unused.

The 128-bit `auto` build gets about half of `native` in L1 (3.0x vs 6.6x for f32 SAXPY), which matches the 4-lane vs 8-lane ratio. So hypothesis 1 mostly holds (I got about 80% of the lane count, not the full amount), and hypothesis 2 holds.

## 6. Alignment and tail (Fig. 3, Fig. 4)

![align](figures/fig3_alignment.png)
![tail](figures/fig4_tail.png)

**Alignment (L1, N = 2048, native).**

- **f32:** aligned runs at 0.050 ns/element. Offsets of 4, 8, 12 and 16 B run at 0.077 to 0.092, which is 55 to 85% slower. A 32 B offset runs at 0.051, back to full speed.
- **f64:** aligned is 0.093. Offsets of 8 to 24 B run at 0.16 to 0.17, about 75 to 90% slower. A 32 B offset runs at 0.091, also full speed.

The pattern points straight at the cause. A 32-byte `ymm` load splits across two cache lines unless its start is a multiple of 32 B, and a split load costs two L1 accesses instead of one.

The scalar build stays flat at about 0.33 to 0.38 ns/element, since 4- and 8-byte loads never split. In DRAM (N = 4M) the misalignment effect disappears into the noise (0.51 to 0.73 ns/element for f32 with no trend). Each cache line gets fetched from DRAM once whether or not the loads split it. Hypothesis 4 holds.

**Tail (N = 1024 to 1088).** The best time per call is linear in N: 0.026 ns/element for native, 0.085 for auto, and 0.220 for scalar. After subtracting the linear fit, the leftover time grouped by N mod 8 only varies by 0.9 ns for native, which is about 1.5% of a 58 ns call. There's no visible sawtooth.

GCC 11 handles the remainder with a 128-bit vector epilogue and then at most 3 scalar iterations. That costs a few scalar iterations of about 0.2 ns each at most, which is below the noise at this N. So hypothesis 5 doesn't show up at this size. The remainder would only matter when N is small enough that a few extra iterations are a big fraction of the call.

## 7. Stride/gather (Fig. 5) and data type (Fig. 6)

![stride](figures/fig5_stride_gather.png)
![dtype](figures/fig6_dtype.png)

**Stride in L1 (N = 256 elements processed).** Stride 1 runs at 0.053 ns/element. Strides 2 through 16 all run at 0.48, a **9x cliff** as soon as the stride stops being 1. Scalar also runs at 0.48 at every stride above 1.

This is a code-generation effect, not a memory effect. Section 4 showed the loop is versioned so only the stride-1 path is vectorized. Any other stride falls back to scalar code, even when everything is sitting in L1.

**Stride in DRAM (N = 2M, f32, native).** Time per element is 0.53, 1.11, 1.92, 5.74 and 13.2 ns for strides 1, 2, 4, 8 and 16. Useful bandwidth falls from 21 to 0.85 GiB/s.

- Up to stride 4, the cost follows the number of cache lines touched per element (1/16, 1/8, 1/4), so time is set by line traffic.
- At strides 8 and 16 the cost per line goes up too, from about 8.5 to about 13 ns per line. At stride 16 every access is a new line and every 64 elements is a new 4 KiB page, so TLB misses and fewer prefetchable lines add to the cost.
- f64 already touches one line per element at stride 8 (13.6 ns/element) and behaves the same way.

**Gather.**

- In L1, `gather-seq` and `gather-rand` cost the same as scalar (0.48 to 0.50 ns/element). The emulated gather gets no SIMD benefit at all.
- In DRAM, random indices cost 11.1 ns/element vs 0.76 for sequential indices, 15x slower. Every element is a cache miss with no spatial locality.
- Native random gather (11.1) was slower than scalar (7.5) for f32 but faster for f64 (12.1 vs 13.7). Both are inside the large spread in this regime, so I'm not reading anything into that difference.

**Data type.** In L1, f32 speedups are 6.1 to 6.6x and f64 speedups are 2.8 to 3.7x. That's about 80% of the 8 and 4 lane counts, with the rest lost to loop overhead and the limit of two loads per cycle. In elements per ns, f32 SAXPY is 2.0x faster than f64 in L1 (0.057 vs 0.112 ns/element) and about 1.7x faster in DRAM (0.66 vs 1.12), since f32 moves half the bytes per element. Hypothesis 7 holds in L1.

## 8. Negative case: the dot product that "vectorized" but didn't really use SIMD (Fig. 8)

![dot](figures/fig8_negative_dot.png)

GCC's `-fopt-info-vec` output says the f32 dot loop was vectorized with 32-byte vectors. If you trusted that message (or wrote a benchmark that only checks it), you'd expect something like 8x. Here's what actually happened:

| build                             | inner loop                                         | L1 ns/element | speedup |
| --------------------------------- | -------------------------------------------------- | ------------- | ------- |
| scalar                            | dependent `vfmadd231ss` chain                      | 1.29          | 1.0x    |
| native (report says "vectorized") | `vmulps ymm`, then 8 serial `vaddss`               | 0.636         | 2.0x    |
| native_fm                         | `vaddps`/`vfmadd` with several vector accumulators | 0.132         | 9.7x    |

IEEE floating point doesn't allow reordering the sum. So the "vectorized" loop multiplies 8 lanes at once and then adds them into one scalar accumulator, in order. Throughput is set by FP-add latency, not by vector width.

The 2.0x win over scalar isn't a SIMD effect either. It comes from moving the multiply off the loop-carried dependency chain, so the chain becomes a 2-cycle add instead of a 4-cycle FMA (0.636 ns vs 1.29 ns, both at about 3.1 GHz). Only `-ffast-math`, which lets the compiler split the sum into several independent vector partial sums, gives the real SIMD speedup. It also changes the result slightly, which shows up in the `max_err` column.

There's a side effect too: the scalar dot product never becomes memory-bound. It runs at 1.42 ns/element in DRAM for both f32 and f64, because the latency chain is slower than DRAM.

The takeaway is that a "loop vectorized" message doesn't prove anything about speed. The instruction mix and the timing do.

## 9. Roofline (Fig. 7)

![roofline](figures/fig7_roofline.png)

- **Arithmetic intensity** (f32, useful bytes, no write-allocate): SAXPY 2/12 = 0.167, dot 2/8 = 0.25, mul 1/12 = 0.083, stencil 5/8 = 0.625 FLOP/B.
- **Bandwidth roof:** the best sustained useful DRAM bandwidth from any build was **26.2 GB/s**.
- **Compute roof:** peak f32 compute = 3.1 GHz x 2 FMA units x 8 lanes x 2 FLOP = **99 GFLOP/s**.
- **Ridge point:** 99 / 26.2 = **3.8 FLOP/B**. Every kernel here is at least 6x below that, so all of them are memory-bound once they leave cache, no matter how wide the vectors are. That's exactly why SIMD speedup falls toward 1x past the LLC.

At DRAM size the kernels reach 50 to 72% of the bandwidth roof (`results/processed/roofline.csv`): stencil 72%, SAXPY 69%, dot 56%, mul 50%.

Two caveats on those percentages:

- `mul` writes `z` without reading it first, so write-allocate adds a hidden 4 B read per element. Real traffic is 16 B/element, not 12, which puts mul at about 67% of the roof. SAXPY doesn't have this problem because it already reads `y` before writing it.
- The bandwidth number is from a single core. One core can't saturate the memory controller, so this roof is a single-thread ceiling, not the DRAM spec.

The L1-resident points sit 2.3x to 9x above the DRAM roof, since they're limited by L1 load/store bandwidth instead. Even so, they only reach 15 to 51% of the 99 GFLOP/s compute peak. At under 1 FLOP/B, even L1 bandwidth runs out before the FMA units do.

## 10. Anomalies and limitations

**`mul` gets slower with SIMD in DRAM.** Native `mul` at N = 16M runs at 0.52x scalar for f64 and 0.76x for f32. This happened in all 3 runs: native took 1.91 to 1.96 ns/element against 0.96 to 1.66 for scalar. I had two guesses:

- **(H1) 4K aliasing.** x, y and z are separate 128 MB allocations whose addresses share the same low 12 bits. So every store to `z[i]` can look like it conflicts with the loads from `x[i]` and `y[i]`. The vector loop keeps more loads in flight, so it would hit more of these false conflicts.
- **(H2) Run order and power limits.** `native` always runs after `scalar` in a sweep, and a 15 W U-series chip might clock down under sustained load. SAXPY f64 doesn't show the same drop, though, which already argues against this one.

`scripts/anomaly_mul.sh` tests both. It alternates the two builds over 5 rounds and shifts `z` by 2,112 B to break the aliasing. f64 results in ns/element:

| z shift | native median (min..max) | scalar median (min..max) |
| ------- | ------------------------ | ------------------------ |
| 0 B     | 1.85 (1.43..2.00)        | 1.16 (1.02..1.31)        |
| 2,112 B | 1.50 (1.32..1.99)        | 1.14 (1.02..1.28)        |

- **H2 is out.** With the builds interleaved, native is still slower than scalar in every pairing, so it's not run order or heat.
- **H1 explains part of it.** Breaking the aliasing cut native's median by 19% and didn't change scalar. But the ranges overlap, and native is still 1.3x slower than scalar afterwards, so aliasing is at most part of the story.
- **The rest is unexplained.** One idea I couldn't test (H3): the vector loop issues stores to new `z` lines faster, so the extra read-for-ownership traffic competes with the `x`/`y` stream prefetchers. Checking that needs memory-traffic counters, and `perf` doesn't work in WSL2.

**Scalar f32 SAXPY is slow in DRAM.** It runs at 9.2 GiB/s, compared with 21.6 GiB/s for f64. That's why f32 SAXPY shows a 1.5x "DRAM speedup" that f64 doesn't. My best guess is memory-level parallelism. In the scalar f32 loop, the out-of-order window covers fewer cache lines ahead per stream than the f64 or vector loops do, and by Little's law fewer lines in flight means less bandwidth. `mul` f32 scalar (17 GiB/s) doesn't quite fit that explanation, so I'm leaving it as a hypothesis.

**Noise.** Run-to-run spread is ±20 to 40% from L2 out to DRAM (the Fig. 1 error bars), and in a few L1 alignment points. The likely causes are Windows background activity, the hypervisor moving the vCPU between physical cores, and a laptop clock that isn't fixed. I report medians of 3 x 7 trials, and I only make claims where the effect is bigger than the min..max spread.

**Limitations.**

- WSL2 doesn't let me fix the frequency, turn off turbo or SMT, or isolate a core, and `perf` isn't available, so there are no hardware counter results. The 3.1 GHz clock is an estimate.
- This is one machine and one compiler (GCC 11). Clang might vectorize the stride and gather loops differently.
- The useful-byte accounting ignores write-allocate traffic.
- The alignment and tail studies only use SAXPY.
