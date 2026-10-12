#!/usr/bin/env python3
"""Run all Project 1 experiments, one CSV per experiment in results/raw/.
Usage: python3 scripts/run_experiments.py [--cpu 2] [--ghz 3.0] [--quick] [--only baseline,align,...]
Pins to one core with taskset if available. Every config runs `--trials`
timed trials inside the binary (median/min/std reported); the whole sweep can
be repeated with --repeat to get run-to-run variability."""
import argparse, os, shutil, subprocess, sys, time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
os.chdir(ROOT)
ap = argparse.ArgumentParser()
ap.add_argument("--cpu", default="2", help="core to pin to (avoid core 0)")
ap.add_argument("--ghz", type=float, default=0, help="fixed core GHz for cycles/elem")
ap.add_argument("--trials", type=int, default=7)
ap.add_argument("--min-ms", type=float, default=30)
ap.add_argument("--quick", action="store_true", help="fewer sizes, for a smoke test")
ap.add_argument("--repeat", type=int, default=1)
ap.add_argument("--only", default="baseline,align,tail,stride")
args = ap.parse_args()

variants = [v for v in ["scalar", "auto", "native", "native512", "native_fm"]
            if os.path.exists(f"bin/bench_{v}")]
pin = ["taskset", "-c", args.cpu] if shutil.which("taskset") else []
os.makedirs("results/raw", exist_ok=True)
header = subprocess.run(["bin/bench_native", "--header"], capture_output=True, text=True).stdout

def run(exp, variant, **kw):
    cmd = pin + [f"bin/bench_{variant}", "--trials", str(args.trials),
                 "--min-ms", str(args.min_ms), "--ghz", str(args.ghz)]
    for k, v in kw.items():
        cmd += [f"--{k.replace('_', '-')}", str(v)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode not in (0, 3):
        print("FAILED:", " ".join(cmd), r.stderr, file=sys.stderr); return
    line = r.stdout.strip()
    if r.returncode == 3:
        print("CORRECTNESS FAIL:", line, file=sys.stderr)
    with open(f"results/raw/{exp}.csv", "a") as f:
        f.write(line + "\n")

def start(exp):
    path = f"results/raw/{exp}.csv"
    if not os.path.exists(path):
        open(path, "w").write(header)
    print(f"[{time.strftime('%H:%M:%S')}] {exp}", flush=True)

# sizes: 2^9 .. 2^24 elements with half-octave steps
sizes = sorted({int(2 ** (k / 2)) for k in range(18, 49)})
if args.quick:
    sizes = [2 ** k for k in range(10, 23, 3)]
L1N, DRAMN = 2048, 1 << 22  # L1-resident and DRAM-resident element counts
only = set(args.only.split(","))

for rep in range(args.repeat):
    if "baseline" in only:  # scalar vs auto vs native across working sets (+ locality, dtype)
        start("baseline")
        for k in ["saxpy", "dot", "mul", "stencil"]:
            for dt in ["f32", "f64"]:
                for n in sizes:
                    for v in variants:
                        run("baseline", v, kernel=k, dtype=dt, n=n)
    if "align" in only:  # misalignment in elements (0 = 64B aligned)
        start("align")
        for dt in ["f32", "f64"]:
            for n in [L1N, DRAMN]:
                for mis in [0, 1, 2, 3, 4, 8, 15]:
                    for v in ["scalar", "native", "native512"]:
                        if v in variants:
                            run("align", v, kernel="saxpy", dtype=dt, n=n, misalign=mis)
    if "tail" in only:  # remainder iterations: N = 1024 .. 1088
        start("tail")
        for n in range(1024, 1089):
            for v in ["scalar", "auto", "native", "native512"]:
                if v in variants:
                    run("tail", v, kernel="saxpy", dtype="f32", n=n)
    if "stride" in only:  # stride and gather
        start("stride")
        for dt in ["f32", "f64"]:
            for n in [L1N // 8, DRAMN // 2]:
                for v in ["scalar", "native", "native512"]:
                    if v not in variants: continue
                    for s in [1, 2, 4, 8, 16]:
                        run("stride", v, kernel="saxpy_stride", dtype=dt, n=n, stride=s)
                    for idx in ["seq", "rand"]:
                        run("stride", v, kernel="saxpy_gather", dtype=dt, n=n, idx=idx)
print("done ->", "results/raw/")
