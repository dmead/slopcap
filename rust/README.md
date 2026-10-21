# Rust ports of SlopCap's per-frame kernels, and a benchmark against the C++

Would SlopCap's processing be faster in Rust? This crate ports the code that runs on every frame, algorithm for
algorithm, and `tools/rust-vs-cpp.sh` times it against the C++ on the same recordings.

**Short answer: no.** With the same algorithm, Rust runs at the speed of C++ compiled by clang, because both are LLVM
(the same LLVM 21.1.8 here). The compiler backend matters far more than the language: GCC is 5.6–9.2× faster than LLVM on
one kernel and 1.4–1.75× slower on another. Where the C++ steers vectorization with `#pragma omp simd`, stable Rust has
no equivalent, and matching it took AVX2 intrinsics.

## What's ported

| Kernel | C++ | Rust | What it does |
|---|---|---|---|
| `render` | `src/preview/render_cpu.cpp` | `src/render.rs` | histogram, auto levels, bilinear debayer + levels stretch to 8-bit RGB (the preview's CPU reference) |
| `focus-gradient` | `src/analysis/focus.cpp` | `src/focus.rs` | per-channel focus scores: locate the target, extract each colour plane's window, levels, signal and centroid, gradient metric |
| `focus-fourier` | same | same | the same with the Fourier metric (a 2-D FFT of a 256² window) |
| `lossless` | `src/net/frame_codec.cpp` (`net::losslessResiduals`) | `src/lossless.rs` | the remote stream's lossless codec, prediction stage: JPEG-LS median predictor + zigzag residuals (its deflate step is zlib on both sides, so it isn't compared) |

`src/ser.rs` reads SER files; `src/bin/kernelbench.rs` and `tools/kernelbench/main.cpp` are the twin drivers (same
frames in memory, one untimed pass, then 5 timed passes, the median per frame, and a check value from the outputs).
The Rust is standard library only, no crates, like the C++ kernels.

Outputs are checked before any timing counts: `render` and `lossless` are byte-identical across all four builds (same
double-precision arithmetic, same rounding), and the focus scores agree to about 1e-8 (the float sums are reordered
differently by each vectorizer).

## Running it

```bash
MSYS_NO_PATHCONV=1 tools/rust-vs-cpp.sh 5 build/bench-data/*.ser     # (MSYS_NO_PATHCONV: from Git Bash on Windows)
```

It builds `docker/Dockerfile.rust` (the project's build image plus Ubuntu's rustc, cargo and clang), then four
versions: the C++ with GCC and with clang (both `-O3 -march=x86-64-v3`, as the `linux-release` preset), and the Rust
with `-C target-cpu=x86-64-v3` (opt-level 3, fat LTO, one codegen unit), once as plain safe code
(`--no-default-features`) and once with the `simd` feature (AVX2 intrinsics in the focus scorer's sums). They run in
rotating order, ROUNDS times, pinned to one CPU, and each keeps its best median. Results go to
`build/rust-vs-cpp/results.md`.

## Results

Intel i9-14900K, one thread; GCC 15.2, clang 21.1.8, rustc 1.93.1 (LLVM 21.1.8); 5 rounds, best median per frame in
ms (lower is better). Rust safe: no intrinsics. Rust SIMD: the focus scorer's sums through AVX2 intrinsics (the
default). Ratios are Rust SIMD's time over the C++'s: below 1, Rust is faster.

| Recording | Kernel | C++ GCC | C++ clang | Rust safe | Rust SIMD | SIMD / GCC | SIMD / clang |
|---|---|---:|---:|---:|---:|---:|---:|
| jupiter1800 1800x1800 8-bit | focus-fourier | 2.808 | 2.602 | 2.547 | 2.559 | 0.91 | 0.98 |
| jupiter1800 1800x1800 8-bit | focus-gradient | 0.437 | 0.399 | 0.677 | 0.496 | 1.13 | 1.24 |
| jupiter1800 1800x1800 8-bit | lossless | 2.586 | 12.813 | 14.528 | 14.543 | 5.62 | 1.14 |
| jupiter1800 1800x1800 8-bit | render | 106.165 | 60.755 | 70.506 | 70.548 | 0.66 | 1.16 |
| moon1468 1468x1188 16-bit | focus-fourier | 0.963 | 0.893 | 0.898 | 0.916 | 0.95 | 1.03 |
| moon1468 1468x1188 16-bit | focus-gradient | 0.156 | 0.124 | 0.288 | 0.218 | 1.39 | 1.75 |
| moon1468 1468x1188 16-bit | lossless | 1.117 | 8.408 | 9.865 | 10.234 | 9.16 | 1.22 |
| moon1468 1468x1188 16-bit | render | 18.295 | 14.079 | 11.708 | 11.653 | 0.64 | 0.83 |
| moon3008 3008x3008 16-bit | focus-fourier | 1.026 | 0.936 | 0.947 | 0.948 | 0.92 | 1.01 |
| moon3008 3008x3008 16-bit | focus-gradient | 0.173 | 0.146 | 0.333 | 0.253 | 1.46 | 1.73 |
| moon3008 3008x3008 16-bit | lossless | 6.567 | 45.712 | 53.013 | 54.566 | 8.31 | 1.19 |
| moon3008 3008x3008 16-bit | render | 91.105 | 67.287 | 63.896 | 63.855 | 0.70 | 0.95 |
| saturn512 512x320 8-bit | focus-fourier | 0.705 | 0.654 | 0.703 | 0.708 | 1.00 | 1.08 |
| saturn512 512x320 8-bit | focus-gradient | 0.170 | 0.153 | 0.310 | 0.272 | 1.60 | 1.78 |
| saturn512 512x320 8-bit | lossless | 0.139 | 0.771 | 0.863 | 0.866 | 6.24 | 1.12 |
| saturn512 512x320 8-bit | render | 6.102 | 3.853 | 3.491 | 3.483 | 0.57 | 0.90 |

All outputs matched: render and lossless byte for byte, focus scores within 1e-4 (in practice about 1e-8).

In short: Fourier metric Rust = clang (within 8%); debayer Rust from 17% faster to 16% slower than clang; lossless
predictor Rust 12-22% slower than clang (bounds checks on its per-pixel and row-buffer reads); focus gradient Rust
SIMD 1.24-1.78x clang, plain safe Rust 1.7-2.3x. GCC vs LLVM: GCC 5.6-9.2x faster on the predictor, LLVM
1.4-1.75x faster on the debayer.

## What the numbers say

- **Same backend, same speed.** Where the work is plain loops that LLVM optimizes by itself (the Fourier metric's
  FFT, the debayer, the predictor), Rust and clang are within a few percent, either way. The language doesn't add or
  remove speed there; bounds checks are hoisted or cost nothing measurable.
- **GCC vs LLVM is the big difference.** On the lossless predictor GCC is 6–8× faster than both LLVM builds: it turns
  the median predictor's branches into conditional moves, while LLVM keeps branches, which mispredict on noisy pixel
  data. On the naive double-precision debayer, LLVM is 1.3–2.5× faster than GCC. Switching compilers would change
  SlopCap's speed more than switching languages.
- **Hand-guided vectorization is where Rust needs more work.** The focus scorer's float sums vectorize in C++ because
  `SLOPCAP_SIMD_SUM` (`#pragma omp simd reduction`) lets the compiler reorder them. Stable Rust can't grant that
  (`f32::algebraic_add` and friends are still unstable in 1.93), so the Rust spells out eight lanes. That went in
  steps (gradient metric, µs per Saturn frame; C++ clang: 6):

  | Rust version | gradient | signal + centroid |
  |---|---:|---:|
  | eight partial sums over `chunks_exact(8)` | 22 | 41 |
  | an `[f32; 8]` type with whole-array operations (the `simd` feature off) | 22 | 42 |
  | the same type over AVX2 intrinsics (`simd` on) | 7 | 15 |

  The first two vectorize in a small test file, but inside the crate LLVM's SLP vectorizer turned them into 2-wide
  pieces with shuffles, comparisons one lane at a time, and stack spills. Intrinsics leave it nothing to discover.

- **Safe Rust costs a little in scalar bookkeeping.** After the fixes below, what's left of the focus scorer's gap
  (Rust + AVX2 is 1.24–1.78× clang, plain safe Rust 1.7–2.3×) is in the irregular, scalar parts: the sparse sampling and 3×3 blur that locate the
  target, and the level estimate's histogram. That's bounds checks and Rust's saturating float→int casts, in code
  the C++ writes with unchecked indexing. Closing it would take `unsafe` unchecked indexing, which the C++ effectively
  is, but which defeats the reason to port.

## Rust performance traps found on the way

- `for dy in -1..=1`: `RangeInclusive` carries an extra "exhausted" flag that LLVM optimizes poorly in hot nested
  loops. `-1..2` is also the literal translation of the C++ loop.
- A heap `Vec` for a fixed-size histogram keeps a bounds check on every increment; a stack array `[u32; 4096]` doesn't.
- `x as i32` from a float saturates (NaN to 0, clamped to the range), which costs instructions per sample;
  clamping in floating point first gives the same bins as the C++'s cast-then-clamp, more cheaply.
- A 3×3 blur written as nine bounds-tested neighbour reads was 2.5× slower than the C++. Row slices plus an interior
  fast path that reads whole three-element arrays keeps the same sums in the same order without per-element checks.
- Checking vectorization: with `lto = "fat"`, `cargo rustc --lib -- --emit asm` shows LLVM's *pre-link* pipeline,
  which hasn't vectorized anything yet. Disassemble the final binary (`objdump -d`) instead.

## Stage breakdown (focus-gradient, µs per Saturn frame)

Measured with temporary timers around each stage (not committed):

| Stage | C++ GCC | C++ clang | Rust + AVX2 |
|---|---:|---:|---:|
| locate (sparse sample, 3×3 blur, quantile, centroid) | 46 | 44 | ~95 |
| extract each colour plane's window | 28 | 25 | 18 |
| levels (sky quantile, peak, clipping) | 139 | 134 | 182 |
| signal and centroid | 7 | 7 | 15 |
| gradient metric | 7 | 6 | 7 |

## Caveats

One machine (Intel i9-14900K, in Docker under WSL2), one thread, x86-64-v3 (AVX2) targets. GCC 15.2, clang 21.1.8,
rustc 1.93.1 with LLVM 21.1.8. The render kernel is the preview's CPU *reference* (the app debayers on the GPU), kept
deliberately simple, so it measures compilers on naive code rather than SlopCap's real preview speed. An Apple-silicon
run (NEON) would be worth adding.
