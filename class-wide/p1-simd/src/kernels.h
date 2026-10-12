#pragma once
#include <stddef.h>
#define NOINLINE __attribute__((noinline))
#define DECL(T, S)                                                             \
void saxpy_##S(size_t, T, const T *restrict, T *restrict);                    \
T dot_##S(size_t, const T *restrict, const T *restrict);                      \
void mul_##S(size_t, const T *restrict, const T *restrict, T *restrict);      \
void stencil_##S(size_t, T, T, T, const T *restrict, T *restrict);            \
void saxpy_stride_##S(size_t, size_t, T, const T *restrict, T *restrict);     \
void saxpy_gather_##S(size_t, T, const int *restrict, const T *restrict,      \
                      T *restrict);
DECL(float, f32)
DECL(double, f64)
