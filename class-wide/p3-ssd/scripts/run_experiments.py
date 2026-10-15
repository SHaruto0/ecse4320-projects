#!/usr/bin/env python3
"""Project 3: SSD profiling with fio. Writes one .fio job file per run to configs/<exp>/
and fio JSON output to results/raw/<exp>/<name>__rep<k>.json.

SAFETY: all I/O goes to ONE disposable test file (default ~/fio-test/testfile).
It never touches a raw device or any existing data.

Usage: python3 scripts/run_experiments.py [--file PATH] [--size 8G] [--runtime 10] [--repeat 3]
                                          [--quick] [--only qd1,bs,mix,qd,ws,state,pitfall]"""
import argparse, json, os, shutil, subprocess, sys, time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
os.chdir(ROOT)
ap = argparse.ArgumentParser()
ap.add_argument("--file", default=os.path.expanduser("~/fio-test/testfile"))
ap.add_argument("--size", default="8G")
ap.add_argument("--runtime", type=int, default=10)
ap.add_argument("--ramp", type=int, default=2)
ap.add_argument("--repeat", type=int, default=3)
ap.add_argument("--quick", action="store_true")
ap.add_argument("--only", default="qd1,bs,mix,qd,ws,state,pitfall")
args = ap.parse_args()
only = set(args.only.split(","))
if args.quick:
    args.runtime, args.ramp, args.repeat = 3, 1, 1

if not shutil.which("fio"):
    sys.exit("fio not found: sudo apt install fio")
tf = os.path.abspath(args.file)
if tf.startswith("/dev/"):
    sys.exit("refusing to run against a raw device; use a test file")
if tf.startswith("/mnt/") and "pitfall" not in only:
    print("WARNING: /mnt/* is the Windows filesystem via 9P in WSL2, not the SSD directly", file=sys.stderr)
os.makedirs(os.path.dirname(tf), exist_ok=True)

GLOBAL = f"""[global]
filename={tf}
size={args.size}
ioengine=libaio
direct=1
time_based=1
runtime={{runtime}}
ramp_time={{ramp}}
group_reporting=1
randrepeat=0
norandommap=1
refill_buffers=1
buffer_compress_percentage=0
lat_percentiles=1
clat_percentiles=0
percentile_list=50:90:95:99:99.9:99.99
"""

def run(exp, name, rep, opts, glob_over=None, runtime=None, extra_args=()):
    g = GLOBAL.format(runtime=runtime or args.runtime, ramp=args.ramp)
    for k, v in (glob_over or {}).items():
        lines = [l for l in g.splitlines() if not l.startswith(k + "=")]
        g = "\n".join(lines) + "\n" + (f"{k}={v}\n" if v is not None else "")
    job = g + f"\n[{name}]\n" + "".join(f"{k}={v}\n" for k, v in opts.items())
    os.makedirs(f"configs/{exp}", exist_ok=True); os.makedirs(f"results/raw/{exp}", exist_ok=True)
    cfg = f"configs/{exp}/{name}.fio"
    open(cfg, "w").write(job)
    out = f"results/raw/{exp}/{name}__rep{rep}.json"
    r = subprocess.run(["fio", "--output-format=json", f"--output={out}", *extra_args, cfg],
                       capture_output=True, text=True)
    if r.returncode:
        print(f"FAILED {cfg}: {r.stderr.strip()[:300]}", file=sys.stderr)

def say(x): print(f"[{time.strftime('%H:%M:%S')}] {x}", flush=True)

# --- precondition: lay out the full file once with sequential incompressible writes ---
if not os.path.exists(tf) or os.path.getsize(tf) < 1024 ** 3:
    say(f"preconditioning: sequential fill of {args.size} test file at {tf}")
    run("precondition", "seqfill", 0, {"rw": "write", "bs": "1M", "iodepth": 32},
        glob_over={"time_based": None, "runtime": None, "ramp_time": None})

BS = ["4k", "16k", "32k", "64k", "128k", "256k", "512k", "1m"]
QDS = [1, 2, 4, 8, 16, 32, 64, 128, 256]
for rep in range(args.repeat):
    if "qd1" in only:
        say(f"rep {rep}: QD=1 baselines")
        for name, rw, bs in (("randread_4k", "randread", "4k"), ("randwrite_4k", "randwrite", "4k"),
                             ("seqread_128k", "read", "128k"), ("seqwrite_128k", "write", "128k")):
            run("qd1", name, rep, {"rw": rw, "bs": bs, "iodepth": 1})
    if "bs" in only:
        say(f"rep {rep}: block-size sweep (QD=32)")
        for rw, tag in (("randread", "rand"), ("read", "seq")):
            for bs in BS:
                run("bs", f"{tag}read_{bs}", rep, {"rw": rw, "bs": bs, "iodepth": 32})
    if "mix" in only:
        say(f"rep {rep}: read/write mix (4k random, QD=32)")
        for rp in (100, 70, 50, 0):
            run("mix", f"rand4k_r{rp}", rep, {"rw": "randrw", "rwmixread": rp, "bs": "4k", "iodepth": 32})
    if "qd" in only:
        say(f"rep {rep}: queue-depth sweep (4k random read)")
        for qd in QDS:
            run("qd", f"randread_4k_qd{qd}", rep, {"rw": "randread", "bs": "4k", "iodepth": qd})
        for nj in (2, 4, 8):  # parallelism via jobs at QD=32 each
            run("qd", f"randread_4k_qd32_jobs{nj}", rep, {"rw": "randread", "bs": "4k", "iodepth": 32, "numjobs": nj})
    if "ws" in only:
        say(f"rep {rep}: working-set / LBA range (4k random read, QD=32)")
        for rng in ("64m", "256m", "1g", "4g", args.size.lower()):
            run("ws", f"randread_4k_range{rng}", rep, {"rw": "randread", "bs": "4k", "iodepth": 32, "size": rng})
    if "pitfall" in only:
        say(f"rep {rep}: misleading setups")
        # 1) buffered I/O on a range that fits in page cache -> measures RAM, not the SSD
        run("pitfall", "buffered_randread_4k_256m", rep, {"rw": "randread", "bs": "4k", "iodepth": 1, "size": "256m"},
            glob_over={"direct": "0", "ioengine": "psync"})
        # 1b) same, but warm the page cache first and stop fio from invalidating it (invalidate=0)
        run("pitfall_warm", "warm_seqread_256m", rep, {"rw": "read", "bs": "1m", "iodepth": 1, "size": "256m"},
            glob_over={"direct": "0", "ioengine": "psync", "invalidate": "0", "time_based": None, "runtime": None, "ramp_time": None})
        run("pitfall", "buffered_warm_randread_4k_256m", rep, {"rw": "randread", "bs": "4k", "iodepth": 1, "size": "256m"},
            glob_over={"direct": "0", "ioengine": "psync", "invalidate": "0"})
        run("pitfall", "direct_randread_4k_256m", rep, {"rw": "randread", "bs": "4k", "iodepth": 1, "size": "256m"})
        # 2) iodepth=32 with a synchronous engine -> effective QD is still 1
        run("pitfall", "psync_iodepth32_randread_4k", rep, {"rw": "randread", "bs": "4k", "iodepth": 32},
            glob_over={"ioengine": "psync"})
        run("pitfall", "libaio_iodepth32_randread_4k", rep, {"rw": "randread", "bs": "4k", "iodepth": 32})

if "state" in only:  # once, last: sustained random write to look for SLC-cache exhaustion / GC
    say("device state: sustained 4k random write, 1 s bandwidth log")
    T = 30 if args.quick else 180
    os.makedirs("results/raw/state", exist_ok=True)
    run("state", "sustained_randwrite_4k_qd32", 0, {"rw": "randwrite", "bs": "4k", "iodepth": 32,
        "write_bw_log": "results/raw/state/sustained", "write_lat_log": "results/raw/state/sustained",
        "log_avg_msec": 1000}, runtime=T, glob_over={"ramp_time": "0"})
say("done -> results/raw/")
