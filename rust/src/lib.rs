//! Rust ports of SlopCap's per-frame kernels, kept algorithm-for-algorithm with the C++ so the two can be benchmarked
//! against each other (`tools/rust-vs-cpp.sh`):
//!
//! - [`render`]: `src/preview/render_cpu.cpp` (histogram, auto levels, bilinear debayer + levels stretch to RGB)
//! - [`focus`]: `src/analysis/focus.cpp` (per-channel focus scores: locate, window, four metrics incl. a 2-D FFT)
//! - [`lossless`]: the prediction stage of `src/net/frame_codec.cpp`'s lossless codec (JPEG-LS median predictor,
//!   zigzagged residuals; the deflate step is zlib on both sides, so it isn't compared)
//! - [`ser`]: just enough of a SER reader to feed them
//!
//! Safe Rust, standard library only. Where the C++ lets the compiler reorder float sums (`SLOPCAP_SIMD_SUM`, OpenMP's
//! simd reduction), the Rust keeps several partial sums ([`lanes`]): the same permission, given the Rust way.

pub mod focus;
pub mod lossless;
pub mod render;
pub mod ser;

/// The 2x2 colour filter pattern, named by its top-left block in reading order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Bayer {
    None,
    Rggb,
    Grbg,
    Gbrg,
    Bggr,
}

/// Eight f32 lanes, one AVX2 register: the reordering of float sums the C++ grants with `#pragma omp simd reduction`.
/// Stable Rust has no way to grant that reordering to a loop, so the sums are written lane-wise with [`F8`].
///
/// What it took (measured on the focus scorer, details in rust/README.md):
/// - Loops of eight partial sums (`acc[k] += ...` over `chunks_exact(8)`) vectorize standalone, but in this crate
///   LLVM's SLP vectorizer turned them into 2-wide pieces with shuffles, lane-by-lane compares and stack spills.
/// - An `[f32; 8]` type with whole-array operations (feature `simd` off) fares no better: same partial vectorization.
/// - AVX2 intrinsics behind the same interface (feature `simd`, the default, when the target has AVX2: build with
///   `-C target-cpu=x86-64-v3` or `native`) give one vector instruction per operation, as the C++ gets.
pub(crate) mod lanes {
    use std::ops::{Add, Mul, Sub};

    pub const L: usize = 8;

    #[cfg(not(all(feature = "simd", target_arch = "x86_64", target_feature = "avx2")))]
    mod imp {
        use super::L;

        #[derive(Clone, Copy, Default)]
        pub struct F8(pub [f32; L]);

        impl F8 {
            #[inline(always)]
            pub fn splat(v: f32) -> F8 {
                F8([v; L])
            }
            #[inline(always)]
            pub fn load(a: &[f32; L]) -> F8 {
                F8(*a)
            }
            #[inline(always)]
            pub fn to_array(self) -> [f32; L] {
                self.0
            }
            #[inline(always)]
            pub(super) fn zip(self, o: F8, f: impl Fn(f32, f32) -> f32) -> F8 {
                F8(std::array::from_fn(|k| f(self.0[k], o.0[k])))
            }
            /// Where self > t: `then`, else 0.
            #[inline(always)]
            pub fn gt_or_zero(self, t: F8, then: F8) -> F8 {
                F8(std::array::from_fn(|k| if self.0[k] > t.0[k] { then.0[k] } else { 0.0 }))
            }
        }
    }

    #[cfg(all(feature = "simd", target_arch = "x86_64", target_feature = "avx2"))]
    #[allow(unused_unsafe)] // intrinsics are safe to call where AVX2 is enabled on newer compilers, unsafe on older ones
    mod imp {
        use super::L;
        use std::arch::x86_64::*;

        #[derive(Clone, Copy)]
        pub struct F8(__m256);

        impl Default for F8 {
            #[inline(always)]
            fn default() -> F8 {
                F8(unsafe { _mm256_setzero_ps() })
            }
        }

        impl F8 {
            #[inline(always)]
            pub fn splat(v: f32) -> F8 {
                F8(unsafe { _mm256_set1_ps(v) })
            }
            #[inline(always)]
            pub fn load(a: &[f32; L]) -> F8 {
                // SAFETY: `a` is eight readable f32s; loadu has no alignment requirement.
                F8(unsafe { _mm256_loadu_ps(a.as_ptr()) })
            }
            #[inline(always)]
            pub fn to_array(self) -> [f32; L] {
                let mut a = [0f32; L];
                // SAFETY: `a` is eight writable f32s; storeu has no alignment requirement.
                unsafe { _mm256_storeu_ps(a.as_mut_ptr(), self.0) };
                a
            }
            #[inline(always)]
            pub(super) fn vadd(self, o: F8) -> F8 {
                F8(unsafe { _mm256_add_ps(self.0, o.0) })
            }
            #[inline(always)]
            pub(super) fn vsub(self, o: F8) -> F8 {
                F8(unsafe { _mm256_sub_ps(self.0, o.0) })
            }
            #[inline(always)]
            pub(super) fn vmul(self, o: F8) -> F8 {
                F8(unsafe { _mm256_mul_ps(self.0, o.0) })
            }
            /// Where self > t: `then`, else 0 (ordered greater-than, like the C++ `>` on floats).
            #[inline(always)]
            pub fn gt_or_zero(self, t: F8, then: F8) -> F8 {
                F8(unsafe { _mm256_and_ps(_mm256_cmp_ps::<_CMP_GT_OQ>(self.0, t.0), then.0) })
            }
        }
    }

    pub use imp::F8;

    impl F8 {
        /// 0, 1, ..., 7 plus `base`.
        #[inline(always)]
        pub fn iota(base: f32) -> F8 {
            F8::load(&std::array::from_fn(|k| base + k as f32))
        }
        /// Pairwise, like a vector horizontal add.
        #[inline(always)]
        pub fn total(self) -> f32 {
            let a = self.to_array();
            ((a[0] + a[4]) + (a[1] + a[5])) + ((a[2] + a[6]) + (a[3] + a[7]))
        }
    }

    #[cfg(not(all(feature = "simd", target_arch = "x86_64", target_feature = "avx2")))]
    mod ops {
        use super::F8;
        impl F8 {
            #[inline(always)]
            pub(super) fn vadd(self, o: F8) -> F8 {
                self.zip(o, |a, b| a + b)
            }
            #[inline(always)]
            pub(super) fn vsub(self, o: F8) -> F8 {
                self.zip(o, |a, b| a - b)
            }
            #[inline(always)]
            pub(super) fn vmul(self, o: F8) -> F8 {
                self.zip(o, |a, b| a * b)
            }
        }
    }

    impl Add for F8 {
        type Output = F8;
        #[inline(always)]
        fn add(self, o: F8) -> F8 {
            F8::vadd(self, o)
        }
    }
    impl Sub for F8 {
        type Output = F8;
        #[inline(always)]
        fn sub(self, o: F8) -> F8 {
            F8::vsub(self, o)
        }
    }
    impl Mul for F8 {
        type Output = F8;
        #[inline(always)]
        fn mul(self, o: F8) -> F8 {
            F8::vmul(self, o)
        }
    }
}
