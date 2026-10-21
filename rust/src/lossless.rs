//! Port of the prediction stage of `src/net/frame_codec.cpp`'s lossless codec: each CFA site's plane in turn, every
//! sample predicted from its neighbours by the median edge detector of LOCO-I / JPEG-LS, residuals zigzagged; 16-bit
//! residuals as all low bytes then all high bytes; padding bits that are zero in every sample shifted out first.
//! Output is byte-identical to the C++ (`net::losslessResiduals`).

use crate::Bayer;

#[inline(always)]
fn predict(a: i32, b: i32, c: i32) -> i32 {
    let (lo, hi) = (a.min(b), a.max(b));
    if c >= hi {
        lo
    } else if c <= lo {
        hi
    } else {
        a + b - c
    }
}

/// Residual bytes into `out` (width * height * bytes per sample); returns the padding shift.
pub fn residuals(pixels: &[u8], width: usize, height: usize, bit_depth: u32, bayer: Bayer, out: &mut [u8]) -> u32 {
    let (w, h) = (width, height);
    let wide = bit_depth > 8;
    let planes = bayer != Bayer::None && w % 2 == 0 && h % 2 == 0;
    let (step, sites) = if planes { (2, 2) } else { (1, 1) };
    let (pw, ph) = (w / step, h / step);
    let n = w * h;
    let at = |x: usize, y: usize| -> i32 {
        let i = y * w + x;
        if wide {
            u16::from_le_bytes([pixels[2 * i], pixels[2 * i + 1]]) as i32
        } else {
            pixels[i] as i32
        }
    };

    let mut used = 0u32; // bits set in any sample
    if wide {
        for s in pixels[..2 * n].chunks_exact(2) {
            used |= u16::from_le_bytes([s[0], s[1]]) as u32;
        }
    }
    let shift = if wide && used != 0 { used.trailing_zeros() } else { 0 };

    let (lo, hi) = out.split_at_mut(n);
    let mut k = 0usize;
    let mut prev = vec![0i32; pw];
    let mut cur = vec![0i32; pw];
    for dy in 0..sites {
        for dx in 0..sites {
            for y in 0..ph {
                for x in 0..pw {
                    let v = at(x * step + dx, y * step + dy) >> shift;
                    cur[x] = v;
                    let p = if y == 0 {
                        if x > 0 { cur[x - 1] } else { 0 }
                    } else if x == 0 {
                        prev[0]
                    } else {
                        predict(cur[x - 1], prev[x], prev[x - 1])
                    };
                    if wide {
                        let r = (v - p) as i16;
                        let z = ((r as u16) << 1) ^ ((r >> 15) as u16);
                        lo[k] = (z & 0xff) as u8;
                        hi[k] = (z >> 8) as u8;
                    } else {
                        let r = (v - p) as i8;
                        lo[k] = ((r as u8) << 1) ^ ((r >> 7) as u8);
                    }
                    k += 1;
                }
                std::mem::swap(&mut prev, &mut cur);
            }
        }
    }
    shift
}
