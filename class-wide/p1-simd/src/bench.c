/* SIMD benchmark driver. Compiled once with fixed flags (-O2, no vectorize)
 * and linked against per-variant kernel objects.
 * Prints one CSV row per run. Use --header to print the header line. */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernels.h"

#ifndef VARIANT
#define VARIANT "unknown"
#endif

static double now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static double urand(void) { /* xorshift64*, uniform [-1,1) */
  rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
  return ((rng * 2685821657736338717ull) >> 11) * (2.0 / 9007199254740992.0) - 1.0;
}

static void *alloc_off(size_t bytes, size_t off_bytes, void **base) {
  size_t total = ((bytes + off_bytes + 4096) / 64 + 1) * 64;
  *base = aligned_alloc(64, total);
  if (!*base) { perror("alloc"); exit(1); }
  memset(*base, 0, total); /* first touch before timing */
  return (char *)*base + off_bytes;
}

static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

volatile double sink;

int main(int argc, char **argv) {
  const char *kernel = "saxpy", *dtype = "f32", *idxmode = "rand";
  size_t n = 1 << 16, mis = 0, stride = 1, zskew = 0;
  int trials = 7;
  double min_ms = 20, ghz = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--header")) {
      puts("variant,kernel,dtype,n,misalign,stride,idx,ws_bytes,reps,trials,"
           "med_ns,min_ns,std_ns,ns_per_elem,gflops,gibps,cpe,max_err,pass");
      return 0;
    }
#define ARG(name) (!strcmp(argv[i], name) && i + 1 < argc)
    else if (ARG("--kernel")) kernel = argv[++i];
    else if (ARG("--dtype")) dtype = argv[++i];
    else if (ARG("--n")) n = strtoull(argv[++i], 0, 10);
    else if (ARG("--misalign")) mis = strtoull(argv[++i], 0, 10);
    else if (ARG("--stride")) stride = strtoull(argv[++i], 0, 10);
    else if (ARG("--idx")) idxmode = argv[++i];
    else if (ARG("--zskew")) zskew = strtoull(argv[++i], 0, 10); /* extra bytes on z only (4K-aliasing test) */
    else if (ARG("--trials")) trials = atoi(argv[++i]);
    else if (ARG("--min-ms")) min_ms = atof(argv[++i]);
    else if (ARG("--ghz")) ghz = atof(argv[++i]);
    else { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
  }
  int f64 = !strcmp(dtype, "f64");
  size_t sz = f64 ? 8 : 4;
  int K_SAXPY = !strcmp(kernel, "saxpy"), K_DOT = !strcmp(kernel, "dot"),
      K_MUL = !strcmp(kernel, "mul"), K_ST = !strcmp(kernel, "stencil"),
      K_STR = !strcmp(kernel, "saxpy_stride"),
      K_GA = !strcmp(kernel, "saxpy_gather");
  if (!(K_SAXPY || K_DOT || K_MUL || K_ST || K_STR || K_GA)) {
    fprintf(stderr, "unknown kernel\n"); return 2;
  }
  size_t len = K_STR ? n * stride : n; /* elements allocated per array */
  void *bx, *by, *bz, *bi = 0;
  char *x = alloc_off(len * sz, mis * sz, &bx);
  char *y = alloc_off(len * sz, mis * sz, &by);
  char *z = alloc_off(len * sz, mis * sz + zskew, &bz);
  int *idx = 0;
  double *x0 = malloc(len * 8), *y0 = malloc(len * 8);
  for (size_t i = 0; i < len; i++) { x0[i] = urand(); y0[i] = urand(); }
  for (size_t i = 0; i < len; i++) {
    if (f64) { ((double *)x)[i] = x0[i]; ((double *)y)[i] = y0[i]; }
    else { ((float *)x)[i] = x0[i]; ((float *)y)[i] = y0[i];
           x0[i] = ((float *)x)[i]; y0[i] = ((float *)y)[i]; }
  }
  if (K_GA) {
    idx = alloc_off(n * sizeof(int), 0, &bi);
    for (size_t i = 0; i < n; i++) idx[i] = (int)i;
    if (!strcmp(idxmode, "rand"))
      for (size_t i = n - 1; i > 0; i--) {
        size_t j = (size_t)((urand() + 1) / 2 * (i + 1)); if (j > i) j = i;
        int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
      }
  }
  const double a = 1e-3, sa = 0.25, sb = 0.5, sc = 0.25;

#define CALL()                                                                 \
  do {                                                                         \
    if (f64) {                                                                 \
      double *X = (double *)x, *Y = (double *)y, *Z = (double *)z;             \
      if (K_SAXPY) saxpy_f64(n, a, X, Y);                                      \
      else if (K_DOT) sink = dot_f64(n, X, Y);                                 \
      else if (K_MUL) mul_f64(n, X, Y, Z);                                     \
      else if (K_ST) stencil_f64(n, sa, sb, sc, X, Y);                         \
      else if (K_STR) saxpy_stride_f64(n, stride, a, X, Y);                    \
      else saxpy_gather_f64(n, a, idx, X, Y);                                  \
    } else {                                                                   \
      float *X = (float *)x, *Y = (float *)y, *Z = (float *)z;                 \
      if (K_SAXPY) saxpy_f32(n, a, X, Y);                                      \
      else if (K_DOT) sink = dot_f32(n, X, Y);                                 \
      else if (K_MUL) mul_f32(n, X, Y, Z);                                     \
      else if (K_ST) stencil_f32(n, sa, sb, sc, X, Y);                         \
      else if (K_STR) saxpy_stride_f32(n, stride, a, X, Y);                    \
      else saxpy_gather_f32(n, a, idx, X, Y);                                  \
    }                                                                          \
    __asm__ volatile("" ::: "memory");                                         \
  } while (0)

  /* ---- correctness: one call vs. double-precision scalar reference ---- */
#define GET(p, i) (f64 ? ((double *)(p))[i] : (double)((float *)(p))[i])
  CALL();
  double max_err = 0;
  if (K_DOT) {
    double ref = 0, mag = 0;
    for (size_t i = 0; i < n; i++) { ref += x0[i] * y0[i]; mag += fabs(x0[i] * y0[i]); }
    max_err = fabs(sink - ref) / (mag + 1e-300);
  } else {
    for (size_t i = 0; i < n; i++) {
      double got, ref, mag;
      if (K_SAXPY || K_STR || K_GA) {
        size_t j = K_STR ? i * stride : i;
        double xv = K_GA ? x0[idx[i]] : x0[j];
        ref = a * xv + y0[j]; mag = fabs(a * xv) + fabs(y0[j]); got = GET(y, j);
      } else if (K_MUL) {
        ref = x0[i] * y0[i]; mag = fabs(ref); got = GET(z, i);
      } else {
        if (i == 0 || i + 1 >= n) continue;
        ref = sa * x0[i - 1] + sb * x0[i] + sc * x0[i + 1];
        mag = fabs(sa * x0[i - 1]) + fabs(sb * x0[i]) + fabs(sc * x0[i + 1]);
        got = GET(y, i);
      }
      double e = fabs(got - ref) / (mag + 1e-300);
      if (e > max_err) max_err = e;
    }
  }
  double tol = f64 ? 1e-12 : (K_DOT ? 1e-4 : 1e-5);
  int pass = max_err <= tol;

  /* ---- calibrate reps so each trial lasts >= min_ms ---- */
  size_t reps = 1;
  for (;;) {
    double t0 = now_ns();
    for (size_t r = 0; r < reps; r++) CALL();
    double dt = now_ns() - t0;
    if (dt >= min_ms * 1e6 || reps > (1ull << 40)) break;
    reps = dt < 1e5 ? reps * 16 : (size_t)(reps * (min_ms * 1e6 / dt) * 1.1) + 1;
  }
  double *t = malloc(trials * sizeof(double));
  for (int k = 0; k < trials; k++) {
    double t0 = now_ns();
    for (size_t r = 0; r < reps; r++) CALL();
    t[k] = (now_ns() - t0) / reps;
  }
  double mean = 0, var = 0;
  for (int k = 0; k < trials; k++) mean += t[k];
  mean /= trials;
  for (int k = 0; k < trials; k++) var += (t[k] - mean) * (t[k] - mean);
  double std = trials > 1 ? sqrt(var / (trials - 1)) : 0;
  qsort(t, trials, sizeof(double), cmp_d);
  double med = t[trials / 2], mn = t[0];

  /* useful flops / bytes per element (no write-allocate counted) */
  double fl = K_DOT ? 2 : K_MUL ? 1 : K_ST ? 5 : 2;
  double by_ = (K_DOT ? 2 : K_ST ? 2 : 3) * sz + (K_GA ? 4 : 0);
  double ws = (double)len * sz * (K_DOT || K_ST ? 2 : 3) - (K_SAXPY || K_STR || K_GA ? len * sz : 0)
              + (K_GA ? n * 4.0 : 0);
  if (K_MUL) ws = 3.0 * len * sz;
  printf("%s,%s,%s,%zu,%zu,%zu,%s,%.0f,%zu,%d,%.3f,%.3f,%.3f,%.5f,%.4f,%.4f,%.4f,%.3e,%d\n",
         VARIANT, kernel, dtype, n, mis, stride, K_GA ? idxmode : "-", ws, reps,
         trials, med, mn, std, med / n, fl * n / med, by_ * n / med * 1e9 / 1073741824.0,
         ghz > 0 ? med * ghz / n : NAN, max_err, pass);
  free(bx); free(by); free(bz); if (bi) free(bi);
  return pass ? 0 : 3;
}
