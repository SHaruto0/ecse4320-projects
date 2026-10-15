/* Cache & memory profiling tool (ECSE 4320 Project 2).
 *
 *   mem lat    : pointer-chase latency (zero-queue), optional K independent chains (MLP)
 *   mem bw     : multi-threaded bandwidth (pattern, granularity, stride, read/write mix)
 *   mem loaded : latency of one chasing thread while N loader threads generate traffic
 *   mem kern   : saxpy-style kernel with perf counters (cache / TLB miss correlation)
 *
 * Every mode prints one CSV row per run (pass --header for the header line).
 * Memory comes from mmap, 2 MiB aligned, and is explicitly madvise()d to
 * MADV_HUGEPAGE or MADV_NOHUGEPAGE so "4K" runs are not silently backed by THP.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/perf_event.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "chains.h"

static double now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}
static uint64_t rs = 88172645463325252ull;
static inline uint64_t xr(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static inline uint64_t rnd(uint64_t n) { return (uint64_t)(((__uint128_t)xr(&rs) * n) >> 64); }

static inline uint64_t mix64(uint64_t z) { /* splitmix64 finalizer */
  z += 0x9E3779B97F4A7C15ull; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31);
}
static void pin(int cpu) {
  if (cpu < 0) return;
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
  if (sched_setaffinity(0, sizeof s, &s)) perror("sched_setaffinity");
}

/* ---------- memory ---------- */
#define HUGE_SZ (2ul << 20)
static size_t huge_kb_before;
static size_t anon_huge_kb(void) {
  FILE *f = fopen("/proc/self/smaps_rollup", "r");
  char line[256]; size_t kb = 0;
  if (!f) return 0;
  while (fgets(line, sizeof line, f))
    if (!strncmp(line, "AnonHugePages:", 14)) kb = strtoull(line + 14, 0, 10);
  fclose(f); return kb;
}
static void *alloc_mem(size_t bytes, int huge) {
  size_t len = (bytes + HUGE_SZ - 1) / HUGE_SZ * HUGE_SZ;
  char *raw = mmap(0, len + HUGE_SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (raw == MAP_FAILED) { perror("mmap"); exit(1); }
  char *p = (char *)(((uintptr_t)raw + HUGE_SZ - 1) & ~(HUGE_SZ - 1));
  madvise(p, len, huge ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
  huge_kb_before = anon_huge_kb();
  memset(p, 1, len); /* first touch outside timing */
  return p;
}
static double huge_frac(size_t bytes) {
  size_t kb = anon_huge_kb();
  double f = (double)(kb > huge_kb_before ? kb - huge_kb_before : 0) * 1024 / bytes;
  return f > 1 ? 1 : f;
}

/* ---------- perf counters (optional; NA when the PMU is not exposed, e.g. WSL2) ---------- */
enum { PC_CYC, PC_INS, PC_L1D, PC_LLC, PC_DTLB, PC_N };
static int pfd[PC_N];
static const char *PC_NAME[PC_N] = {"cycles", "instructions", "l1d_miss", "llc_miss", "dtlb_miss"};
static void perf_open(void) {
  struct { uint32_t type; uint64_t cfg; } ev[PC_N] = {
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
    {PERF_TYPE_HW_CACHE, PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)},
    {PERF_TYPE_HW_CACHE, PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)},
    {PERF_TYPE_HW_CACHE, PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)},
  };
  for (int i = 0; i < PC_N; i++) {
    struct perf_event_attr a; memset(&a, 0, sizeof a);
    a.type = ev[i].type; a.size = sizeof a; a.config = ev[i].cfg;
    a.disabled = 1; a.exclude_kernel = 1; a.exclude_hv = 1;
    pfd[i] = syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
  }
}
static void perf_ctl(int on) {
  for (int i = 0; i < PC_N; i++)
    if (pfd[i] >= 0) { if (on) ioctl(pfd[i], PERF_EVENT_IOC_RESET, 0); ioctl(pfd[i], on ? PERF_EVENT_IOC_ENABLE : PERF_EVENT_IOC_DISABLE, 0); }
}
static void perf_print(double denom) { /* per-access values; NA if unsupported */
  for (int i = 0; i < PC_N; i++) {
    long long v;
    if (pfd[i] >= 0 && read(pfd[i], &v, sizeof v) == sizeof v) printf(",%.4f", v / denom);
    else printf(",NA");
  }
}

/* ---------- lat: pointer chase ---------- */
/* Builds one random cycle (Sattolo) over nodes spaced `stride` bytes apart and
 * splits it into K equal arcs, so K chains are independent and disjoint.
 * --pagerand puts each node at a random 64B line inside its stride slot, so
 * page-stride chases do not all collide in the same cache sets. */
static int cmd_lat(int argc, char **argv) {
  size_t size = 1 << 20, stride = 64; int huge = 0, K = 1, seq = 0, pagerand = 0, cpu = -1;
  double min_ms = 200;
  for (int i = 2; i < argc; i++) {
#define A(n) (!strcmp(argv[i], n) && i + 1 < argc)
    if (A("--size")) size = strtoull(argv[++i], 0, 10);
    else if (A("--stride")) stride = strtoull(argv[++i], 0, 10);
    else if (A("--huge")) huge = atoi(argv[++i]);
    else if (A("--chains")) K = atoi(argv[++i]);
    else if (A("--pattern")) seq = !strcmp(argv[++i], "seq");
    else if (A("--pagerand")) pagerand = atoi(argv[++i]);
    else if (A("--cpu")) cpu = atoi(argv[++i]);
    else if (A("--min-ms")) min_ms = atof(argv[++i]);
    else { fprintf(stderr, "lat: bad arg %s\n", argv[i]); return 2; }
  }
  pin(cpu);
  void *(*fn)(void **, size_t) = chase_fn(K);
  if (!fn) { fprintf(stderr, "unsupported --chains %d\n", K); return 2; }
  size_t n = size / stride;
  if (n < (size_t)K * 2) { fprintf(stderr, "too few nodes\n"); return 2; }
  char *buf = alloc_mem(size, huge);
  double hf = huge_frac(size);
  uint32_t *ord = malloc(n * sizeof *ord);
  for (size_t i = 0; i < n; i++) ord[i] = i;
  if (!seq) for (size_t i = n - 1; i > 0; i--) { size_t j = rnd(i); uint32_t t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
  size_t lines = stride / 64;
/* line offset inside each slot: a full 64-bit mix of the slot index. (v1 used i*2654435761 % lines,
   which only cycles through 64 offsets: with physically contiguous huge pages that maps every node
   into a small subset of L2/L3 sets and causes conflict misses. Kept as --pagerand 2 for the demo.) */
#define NODE(i) (buf + (size_t)(i) * stride + (pagerand && lines > 1 ? \
    (pagerand == 2 ? ((size_t)(i) * 2654435761u % lines) : (size_t)(mix64(i) % lines)) * 64 : 0))
  for (size_t i = 0; i < n; i++) *(void **)NODE(ord[i]) = NODE(ord[(i + 1) % n]);
  void *starts[32];
  for (int k = 0; k < K; k++) starts[k] = NODE(ord[(size_t)k * (n / K)]);
  free(ord);
  /* warm-up (and TLB/cache fill), then calibrate steps */
  size_t warm = n / K < (1u << 22) ? n / K : (1u << 22);
  fn(starts, warm);
  size_t steps = 1 << 12; double dt;
  for (;;) {
    double t0 = now_ns(); fn(starts, steps); dt = now_ns() - t0;
    if (dt > min_ms * 1e6 / 4) break;
    steps *= 4;
  }
  steps = (size_t)(steps * (min_ms * 1e6 / dt)) + 1;
  perf_open(); perf_ctl(1);
  double t0 = now_ns(); fn(starts, steps); dt = now_ns() - t0;
  perf_ctl(0);
  double ns_step = dt / steps, acc_per_ns = K / ns_step;
  printf("lat,%zu,%zu,%s,%d,%.2f,%d,%d,%.3f,%.4f,%.3f", size, stride, seq ? "seq" : "rand", huge, hf, K,
         pagerand, ns_step, acc_per_ns, acc_per_ns * 64);
  perf_print((double)steps * K);
  printf("\n");
  return 0;
}

/* ---------- bw: bandwidth ---------- */
typedef struct {
  char *base; size_t len, gran, stride; int seq, rdpct, cpu, delay;
  _Atomic int *go; volatile int *stop; double bytes, lines, secs; uint64_t sink, seed;
} bwarg_t;

static inline uint64_t touch(char *p, size_t gran, int write, uint64_t v) {
  uint64_t *q = (uint64_t *)p, s = 0;
  if (write) for (size_t w = 0; w < gran / 8; w++) q[w] = v + w;
  else for (size_t w = 0; w < gran / 8; w++) s += q[w];
  return s;
}
static void *bw_worker(void *vp) {
  bwarg_t *a = vp; pin(a->cpu);
  size_t naccess = a->seq ? a->len / a->stride : a->len / a->gran;
  uint64_t s = 0, cnt = 0, seed = a->seed | 1; size_t pos = 0; int ph = 0;
  while (!atomic_load(a->go)) ;
  double t0 = now_ns();
  while (!*a->stop) {
    for (int b = 0; b < 1024; b++) {
      size_t off = a->seq ? pos * a->stride : (size_t)(((__uint128_t)xr(&seed) * naccess) >> 64) * a->gran;
      if (++pos == naccess) pos = 0;
      s += touch(a->base + off, a->gran, ph >= a->rdpct, cnt);  /* ph cycles 0..99: read_pct% reads */
      if (++ph == 100) ph = 0;
      cnt++;
      for (int d = 0; d < a->delay; d++) __asm__ volatile("");
    }
  }
  a->secs = (now_ns() - t0) / 1e9;
  a->bytes = (double)cnt * a->gran;
  a->lines = (double)cnt * ((a->gran + 63) / 64);
  a->sink = s;
  return 0;
}
static int parse_cpus(const char *s, int *out, int max) {
  int n = 0; char *dup = strdup(s), *tok, *sp;
  for (tok = strtok_r(dup, ",", &sp); tok && n < max; tok = strtok_r(0, ",", &sp)) {
    int a, b;
    if (sscanf(tok, "%d-%d", &a, &b) == 2) for (int c = a; c <= b && n < max; c++) out[n++] = c;
    else out[n++] = atoi(tok);
  }
  free(dup); return n;
}
static int cmd_bw(int argc, char **argv) {
  size_t size = 1ul << 30, gran = 64, stride = 64; int huge = 1, seq = 1, rdpct = 100, delay = 0, nc;
  int cpus[256] = {0}; nc = 1; double ms = 500;
  for (int i = 2; i < argc; i++) {
    if (A("--size")) size = strtoull(argv[++i], 0, 10);
    else if (A("--gran")) gran = strtoull(argv[++i], 0, 10);
    else if (A("--stride")) stride = strtoull(argv[++i], 0, 10);
    else if (A("--huge")) huge = atoi(argv[++i]);
    else if (A("--pattern")) seq = !strcmp(argv[++i], "seq");
    else if (A("--read-pct")) rdpct = atoi(argv[++i]);
    else if (A("--delay")) delay = atoi(argv[++i]);
    else if (A("--cpus")) nc = parse_cpus(argv[++i], cpus, 256);
    else if (A("--ms")) ms = atof(argv[++i]);
    else { fprintf(stderr, "bw: bad arg %s\n", argv[i]); return 2; }
  }
  if (gran < 8 || gran % 8) { fprintf(stderr, "gran must be multiple of 8\n"); return 2; }
  if (stride < gran) stride = gran;
  char *buf = alloc_mem(size, huge);
  double hf = huge_frac(size);
  pthread_t th[256]; bwarg_t a[256]; _Atomic int go = 0; volatile int stop = 0;
  size_t per = size / nc / 4096 * 4096;
  for (int t = 0; t < nc; t++) {
    a[t] = (bwarg_t){buf + t * per, per, gran, stride, seq, rdpct, cpus[t], delay, &go, &stop, 0, 0, 0, 0, 0x1234567ull * (t + 1)};
    pthread_create(&th[t], 0, bw_worker, &a[t]);
  }
  usleep(20000);
  atomic_store(&go, 1);
  struct timespec ts = {(time_t)(ms / 1000), (long)(fmod(ms, 1000) * 1e6)};
  nanosleep(&ts, 0);
  stop = 1;
  double bytes = 0, lines = 0, secs = 0;
  for (int t = 0; t < nc; t++) { pthread_join(th[t], 0); bytes += a[t].bytes; lines += a[t].lines; secs += a[t].secs; }
  secs /= nc;
  printf("bw,%zu,%d,%s,%zu,%zu,%d,%d,%.2f,%d,%.3f,%.3f,%.3f\n", size, nc, seq ? "seq" : "rand", gran, stride, rdpct,
         huge, hf, delay, bytes / secs / 1e9, lines * 64 / secs / 1e9, secs);
  return 0;
}

/* ---------- loaded: latency under load ---------- */
void *volatile sink_ptr;
static int cmd_loaded(int argc, char **argv) {
  size_t csize = 1ul << 30, lsize = 1ul << 30; int ccpu = 0, lc[256], nl = 0, delay = 0, rdpct = 100, huge = 1;
  double ms = 1000;
  for (int i = 2; i < argc; i++) {
    if (A("--chase-size")) csize = strtoull(argv[++i], 0, 10);
    else if (A("--load-size")) lsize = strtoull(argv[++i], 0, 10);
    else if (A("--chase-cpu")) ccpu = atoi(argv[++i]);
    else if (A("--load-cpus")) nl = parse_cpus(argv[++i], lc, 256);
    else if (A("--delay")) delay = atoi(argv[++i]);
    else if (A("--read-pct")) rdpct = atoi(argv[++i]);
    else if (A("--huge")) huge = atoi(argv[++i]);
    else if (A("--ms")) ms = atof(argv[++i]);
    else { fprintf(stderr, "loaded: bad arg %s\n", argv[i]); return 2; }
  }
  pin(ccpu);
  size_t n = csize / 64;
  char *cb = alloc_mem(csize, huge);
  uint32_t *ord = malloc(n * sizeof *ord);
  for (size_t i = 0; i < n; i++) ord[i] = i;
  for (size_t i = n - 1; i > 0; i--) { size_t j = rnd(i); uint32_t t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
  for (size_t i = 0; i < n; i++) *(void **)(cb + (size_t)ord[i] * 64) = cb + (size_t)ord[(i + 1) % n] * 64;
  void *p = cb + (size_t)ord[0] * 64;
  free(ord);
  char *lb = nl ? alloc_mem(lsize, huge) : 0;
  pthread_t th[256]; bwarg_t a[256]; _Atomic int go = 0; volatile int stop = 0;
  size_t per = nl ? lsize / nl / 4096 * 4096 : 0;
  for (int t = 0; t < nl; t++) {
    a[t] = (bwarg_t){lb + t * per, per, 64, 64, 1, rdpct, lc[t], delay, &go, &stop, 0, 0, 0, 0, 77ull * (t + 1)};
    pthread_create(&th[t], 0, bw_worker, &a[t]);
  }
  usleep(20000);
  atomic_store(&go, 1);
  usleep(50000); /* let loaders ramp */
  size_t steps = 0; double t0 = now_ns(), dt;
  void *s1[1] = {p};
  void *(*fn)(void **, size_t) = chase_fn(1);  /* called through a pointer so it can't be optimized away */
  do { fn(s1, 1 << 14); steps += 1 << 14; dt = now_ns() - t0; } while (dt < ms * 1e6);
  sink_ptr = s1[0];
  stop = 1;
  double bytes = 0, secs = 0;
  for (int t = 0; t < nl; t++) { pthread_join(th[t], 0); bytes += a[t].bytes; secs += a[t].secs; }
  double lbw = nl ? bytes / (secs / nl) / 1e9 : 0;
  double lat = dt / steps, chase_bw = 64 / lat; /* GB/s from chaser itself */
  printf("loaded,%d,%d,%d,%.3f,%.3f,%.3f\n", nl, delay, rdpct, lat, lbw, lbw + chase_bw);
  return 0;
}

/* ---------- kern: saxpy with cache/TLB counters ---------- */
static int cmd_kern(int argc, char **argv) {
  size_t size = 1 << 20; int huge = 0, randidx = 0, cpu = -1; double min_ms = 200;
  for (int i = 2; i < argc; i++) {
    if (A("--size")) size = strtoull(argv[++i], 0, 10);  /* total footprint of x and y */
    else if (A("--huge")) huge = atoi(argv[++i]);
    else if (A("--pattern")) randidx = !strcmp(argv[++i], "rand");
    else if (A("--cpu")) cpu = atoi(argv[++i]);
    else if (A("--min-ms")) min_ms = atof(argv[++i]);
    else { fprintf(stderr, "kern: bad arg %s\n", argv[i]); return 2; }
  }
  pin(cpu);
  size_t n = size / 2 / sizeof(float);
  float *x = alloc_mem(n * 4, huge), *y = alloc_mem(n * 4, huge);
  double hf = huge_frac(n * 8);
  uint32_t *idx = malloc(n * sizeof *idx); /* idx array is not counted in footprint */
  for (size_t i = 0; i < n; i++) { idx[i] = i; x[i] = 1.0f; y[i] = 2.0f; }
  if (randidx) for (size_t i = n - 1; i > 0; i--) { size_t j = rnd(i); uint32_t t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
  const float al = 1e-6f;
  size_t reps = 1; double dt;
#define PASS() for (size_t i = 0; i < n; i++) { size_t j = idx[i]; y[j] = al * x[j] + y[j]; }
  for (;;) {
    double t0 = now_ns(); for (size_t r = 0; r < reps; r++) { PASS(); __asm__ volatile("" ::: "memory"); }
    dt = now_ns() - t0;
    if (dt > min_ms * 1e6 / 4) break;
    reps *= 4;
  }
  reps = (size_t)(reps * (min_ms * 1e6 / dt)) + 1;
  perf_open(); perf_ctl(1);
  double t0 = now_ns();
  for (size_t r = 0; r < reps; r++) { PASS(); __asm__ volatile("" ::: "memory"); }
  dt = now_ns() - t0;
  perf_ctl(0);
  printf("kern,%zu,%s,%d,%.2f,%.4f", size, randidx ? "rand" : "seq", huge, hf, dt / ((double)reps * n));
  perf_print((double)reps * n);
  printf("\n");
  return 0;
}

int main(int argc, char **argv) {
  if (argc >= 3 && !strcmp(argv[2], "--header")) {
    if (!strcmp(argv[1], "lat")) {
      printf("mode,size,stride,pattern,huge,huge_frac,chains,pagerand,ns_per_step,acc_per_ns,gbps");
      for (int i = 0; i < PC_N; i++) printf(",%s", PC_NAME[i]);
      printf("\n");
    } else if (!strcmp(argv[1], "bw"))
      puts("mode,size,threads,pattern,gran,stride,read_pct,huge,huge_frac,delay,useful_gbps,line_gbps,secs");
    else if (!strcmp(argv[1], "loaded"))
      puts("mode,nload,delay,read_pct,lat_ns,load_gbps,total_gbps");
    else if (!strcmp(argv[1], "kern")) {
      printf("mode,size,pattern,huge,huge_frac,ns_per_elem");
      for (int i = 0; i < PC_N; i++) printf(",%s", PC_NAME[i]);
      printf("\n");
    }
    return 0;
  }
  if (argc >= 2 && !strcmp(argv[1], "lat")) return cmd_lat(argc, argv);
  if (argc >= 2 && !strcmp(argv[1], "bw")) return cmd_bw(argc, argv);
  if (argc >= 2 && !strcmp(argv[1], "loaded")) return cmd_loaded(argc, argv);
  if (argc >= 2 && !strcmp(argv[1], "kern")) return cmd_kern(argc, argv);
  fprintf(stderr, "usage: mem {lat|bw|loaded|kern} [--header] [options]\n");
  return 2;
}
