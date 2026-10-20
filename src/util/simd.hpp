// Vectorization helpers.
//
// SlopCap relies on the compiler's auto-vectorizer (release builds target x86-64-v3 = AVX2) rather than intrinsics, so
// the same loops vectorize for NEON on aarch64 (Apple silicon). Plain element-wise loops vectorize on their own. Float
// sums do not: vectorizing them reorders the additions, which the compiler won't do without permission. SLOPCAP_SIMD_SUM
// grants it for one loop (OpenMP's simd pragma; -fopenmp-simd enables only the pragma, no OpenMP runtime).
// SLOPCAP_SIMD_SUM_MAX(sum, max) also names a running maximum: GCC finds one by itself, clang needs telling. Clang
// still won't vectorize a float max (NaNs), so keep the maximum an integer (see planeLevels in analysis/focus.cpp).
// A loop under the pragma that clang can't vectorize is an error there (-Wpass-failed with -Werror).
// Check what vectorized with:  -fopt-info-vec-optimized  (GCC),  -Rpass=loop-vectorize -Rpass-analysis=loop-vectorize  (clang).
#pragma once

#define SLOPCAP_PRAGMA(x) _Pragma(#x)
#if defined(__GNUC__) || defined(__clang__)
#define SLOPCAP_SIMD_SUM(...) SLOPCAP_PRAGMA(omp simd reduction(+ : __VA_ARGS__))
#define SLOPCAP_SIMD_SUM_MAX(sumVar, maxVar) SLOPCAP_PRAGMA(omp simd reduction(+ : sumVar) reduction(max : maxVar))
#else
#define SLOPCAP_SIMD_SUM(...)
#define SLOPCAP_SIMD_SUM_MAX(sumVar, maxVar)
#endif
