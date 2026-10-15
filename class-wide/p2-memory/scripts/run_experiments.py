#!/usr/bin/env python3
"""Run all Project 2 experiments -> results/raw/<exp>.csv (appends; one row per run).
Usage: python3 scripts/run_experiments.py --cpu 4 [--repeat 3] [--quick] [--only ws,tlb,...]
--cpu    : core for single-threaded tests (latency chaser)
--all    : cpu list for multi-threaded load (default: every online cpu)"""
import argparse, os, subprocess, sys, time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
os.chdir(ROOT)
ap = argparse.ArgumentParser()
ap.add_argument("--cpu", type=int, default=4)
ap.add_argument("--all", default=f"0-{os.cpu_count() - 1}")
ap.add_argument("--repeat", type=int, default=3)
ap.add_argument("--quick", action="store_true")
ap.add_argument("--only", default="ws,pattern,mix,intensity,tlb,tlbconflict,kern,pitfall")
args = ap.parse_args()
only = set(args.only.split(","))
M = "bin/mem"
KiB, MiB, GiB = 1 << 10, 1 << 20, 1 << 30
BIG = 256 * MiB if args.quick else 1 * GiB          # DRAM-resident footprint
LOADBUF = 512 * MiB if args.quick else 2 * GiB
MS = 150 if args.quick else 400

def cpus_expand(s):
    out = []
    for t in s.split(","):
        a, _, b = t.partition("-")
        out += list(range(int(a), int(b) + 1)) if b else [int(a)]
    return out
ALL = cpus_expand(args.all)
OTHERS = [c for c in ALL if c != args.cpu]
others_str = ",".join(map(str, OTHERS))
os.makedirs("results/raw", exist_ok=True)

def run(exp, mode, **kw):
    path = f"results/raw/{exp}.csv"
    if not os.path.exists(path):
        hdr = subprocess.run([M, mode, "--header"], capture_output=True, text=True).stdout.strip()
        open(path, "w").write("rep,label," + hdr + "\n")
    label = kw.pop("label", "")
    cmd = [M, mode] + sum([[f"--{k.replace('_', '-')}", str(v)] for k, v in kw.items()], [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print("FAILED:", " ".join(cmd), r.stderr, file=sys.stderr); return
    with open(path, "a") as f:
        f.write(f"{rep},{label},{r.stdout.strip()}\n")

def sizes(lo, hi, steps_per_octave=2):
    s, k = [], 0
    while True:
        v = int(lo * 2 ** (k / steps_per_octave)) // 64 * 64
        if v > hi: return s
        s.append(v); k += 1

def say(x): print(f"[{time.strftime('%H:%M:%S')}] rep {rep}: {x}", flush=True)

for rep in range(args.repeat):
    if "ws" in only:  # zero-queue latency vs working set, 4K vs huge pages
        say("working-set latency sweep")
        for huge in (1, 0):
            for s in sizes(4 * KiB, BIG, 1 if args.quick else 2):
                run("ws", "lat", size=s, huge=huge, cpu=args.cpu, min_ms=MS / 2)
    if "pattern" in only:  # pattern x granularity/stride, latency and bandwidth
        say("pattern / granularity sweep")
        for pat in ("seq", "rand"):
            for st in (64, 256, 1024):
                run("pattern_lat", "lat", size=BIG, stride=st, pattern=pat, huge=1, cpu=args.cpu, min_ms=MS / 2)
        for threads, cpus in (("1T", str(args.cpu)), ("allT", args.all)):
            for st in (64, 128, 256, 1024, 4096):
                run("pattern_bw", "bw", label=threads, size=LOADBUF, cpus=cpus, pattern="seq", gran=64, stride=st, ms=MS)
            for g in (64, 256, 1024, 4096):
                run("pattern_bw", "bw", label=threads, size=LOADBUF, cpus=cpus, pattern="rand", gran=g, ms=MS)
    if "mix" in only:  # read/write mix: bandwidth (1 and all threads) + loaded latency
        say("read/write mix sweep")
        for rp in (100, 0, 70, 50):
            for threads, cpus in (("1T", str(args.cpu)), ("allT", args.all)):
                run("mix_bw", "bw", label=threads, size=LOADBUF, cpus=cpus, read_pct=rp, ms=MS)
            run("mix_loaded", "loaded", chase_size=BIG, load_size=LOADBUF, chase_cpu=args.cpu,
                load_cpus=others_str, read_pct=rp, ms=MS * 2)
    if "intensity" in only:
        say("intensity: single-core MLP (independent chains)")
        for k in (1, 2, 3, 4, 6, 8, 10, 12, 14, 16, 20, 24, 32):
            run("mlp", "lat", size=BIG, huge=1, chains=k, cpu=args.cpu, min_ms=MS / 2)
        say("intensity: loaded latency vs #loader threads")
        for n in range(0, len(OTHERS) + 1):
            kw = dict(chase_size=BIG, chase_cpu=args.cpu, ms=MS * 2)
            if n: kw.update(load_size=LOADBUF, load_cpus=",".join(map(str, OTHERS[:n])))
            run("loaded_threads", "loaded", **kw)
        say("intensity: loaded latency vs injection delay (all loaders)")
        for d in (6400, 3200, 1600, 800, 400, 200, 100, 50, 25, 10, 0):
            run("loaded_delay", "loaded", chase_size=BIG, load_size=LOADBUF, chase_cpu=args.cpu,
                load_cpus=others_str, delay=d, ms=MS * 2)
    if "tlb" in only:  # one line per 4 KiB page, random order; 4K vs huge pages
        say("TLB sweep")
        for huge in (0, 1):
            for pages in [2 ** k for k in range(4, 19 if args.quick else 20)]:
                run("tlb", "lat", size=pages * 4096, stride=4096, pagerand=1, huge=huge, cpu=args.cpu, min_ms=MS / 2)
    if "tlbconflict" in only:  # v1 placement (offset cycles through only 64 values): cache-set conflicts
        say("set-conflict demo (old per-page placement)")
        for huge in (0, 1):
            for pages in [2 ** k for k in range(8, 19 if args.quick else 20, 2)]:
                run("tlbconflict", "lat", size=pages * 4096, stride=4096, pagerand=2, huge=huge, cpu=args.cpu, min_ms=MS / 2)
    if "kern" in only:  # saxpy runtime vs footprint and pattern (+ perf counters if available)
        say("cache-miss impact kernel")
        for pat in ("seq", "rand"):
            for s in sizes(16 * KiB, BIG, 1):
                run("kern", "kern", size=s, pattern=pat, cpu=args.cpu, min_ms=MS / 2)
    if "pitfall" in only:  # three ways to 'measure DRAM latency', two of them wrong
        say("pitfall demo")
        run("pitfall", "lat", label="seq chase 1GiB (prefetcher hides latency)", size=BIG, pattern="seq", huge=1, cpu=args.cpu, min_ms=MS / 2)
        run("pitfall", "lat", label="rand chase 8MiB (fits in LLC)", size=8 * MiB, huge=1, cpu=args.cpu, min_ms=MS / 2)
        run("pitfall", "lat", label="rand chase 1GiB 4K pages (adds TLB misses)", size=BIG, huge=0, cpu=args.cpu, min_ms=MS / 2)
        run("pitfall", "lat", label="rand chase 1GiB huge pages (correct)", size=BIG, huge=1, cpu=args.cpu, min_ms=MS / 2)
print("done -> results/raw/")
