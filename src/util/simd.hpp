// Vectorization helpers.
//
// SlopCap relies on the compiler's auto-vectorizer (release builds target x86-64-v3 = AVX2) rather than intrinsics, so
// the same loops vectorize for NEON on aarch64 later. Plain element-wise loops vectorize on their own. Float sums do
// not: vectorizing them reorders the additions, which the compiler won't do without permission. SLOPCAP_SIMD_SUM
// grants it for one loop (OpenMP's simd pragma; -fopenmp-simd enables only the pragma, no OpenMP runtime).
// Check what vectorized with:  -fopt-info-vec-optimized  (GCC).
#pragma once

#define SLOPCAP_PRAGMA(x) _Pragma(#x)
#if defined(__GNUC__) || defined(__clang__)
#define SLOPCAP_SIMD_SUM(...) SLOPCAP_PRAGMA(omp simd reduction(+ : __VA_ARGS__))
#else
#define SLOPCAP_SIMD_SUM(...)
#endif
