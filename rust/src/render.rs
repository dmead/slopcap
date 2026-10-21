//! Port of `src/preview/render_cpu.cpp`: the CPU reference for the preview (histogram, auto levels, bilinear Bayer
//! demosaic + levels stretch to 8-bit RGB). Same algorithm, same double-precision arithmetic, so the output bytes
//! are identical.

use crate::Bayer;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Levels {
    pub black: f64,
    pub white: f64,
    pub gamma: f64,
    pub wb: [f64; 3],
}

impl Default for Levels {
    fn default() -> Self {
        Levels { black: 0.0, white: 1.0, gamma: 1.0, wb: [1.0, 1.0, 1.0] }
    }
}

/// Mirror an out-of-range coordinate back inside (-1 -> 1, n -> n-2): unlike clamping this keeps the Bayer parity.
#[inline(always)]
fn mirror(mut v: i32, n: i32) -> i32 {
    if n == 1 {
        return 0;
    }
    if v < 0 {
        v = -v;
    }
    if v >= n {
        v = 2 * n - 2 - v;
    }
    v.clamp(0, n - 1)
}

/// Sample at (x, y) as 0..1, edges mirrored.
struct Raw<'a> {
    p: &'a [u8],
    w: i32,
    h: i32,
    bits: u32,
}

impl Raw<'_> {
    #[inline(always)]
    fn at(&self, x: i32, y: i32) -> f64 {
        let x = mirror(x, self.w);
        let y = mirror(y, self.h);
        let i = y as usize * self.w as usize + x as usize;
        if self.bits > 8 {
            let v = self.p[2 * i] as u32 | (self.p[2 * i + 1] as u32) << 8;
            v as f64 / 65535.0
        } else {
            self.p[i] as f64 / 255.0
        }
    }
}

/// Colour of the Bayer site at (x, y): 0 = R, 1 = G, 2 = B.
#[inline(always)]
fn site_color(b: Bayer, x: i32, y: i32) -> usize {
    let idx = ((y & 1) * 2 + (x & 1)) as usize;
    const RGGB: [usize; 4] = [0, 1, 1, 2];
    const GRBG: [usize; 4] = [1, 0, 2, 1];
    const GBRG: [usize; 4] = [1, 2, 0, 1];
    const BGGR: [usize; 4] = [2, 1, 1, 0];
    match b {
        Bayer::Rggb => RGGB[idx],
        Bayer::Grbg => GRBG[idx],
        Bayer::Gbrg => GBRG[idx],
        Bayer::Bggr => BGGR[idx],
        Bayer::None => 1,
    }
}

#[inline(always)]
fn stretch(v: f64, lv: &Levels, channel: usize) -> u8 {
    let v = v * lv.wb[channel];
    let span = (lv.white - lv.black).max(1e-9);
    let mut x = ((v - lv.black) / span).clamp(0.0, 1.0);
    if lv.gamma != 1.0 {
        x = x.powf(1.0 / lv.gamma);
    }
    (x * 255.0).round() as u8 // round half away from zero, like std::lround
}

/// 256-bin histogram of one frame (8-bit samples directly, 16-bit by their high byte), on every `stride`-th pixel.
pub fn histogram(pixels: &[u8], bit_depth: u32, stride: usize) -> [u32; 256] {
    let mut h = [0u32; 256];
    let stride = stride.max(1);
    if bit_depth > 8 {
        for &b in pixels.iter().skip(1).step_by(2 * stride) {
            h[b as usize] += 1;
        }
    } else {
        for &b in pixels.iter().step_by(stride) {
            h[b as usize] += 1;
        }
    }
    h
}

/// Auto-stretch: black just above the background, white at the brightest `white_percentile` of the histogram.
pub fn auto_levels(hist: &[u32; 256], black_percentile: f64, white_percentile: f64) -> Levels {
    let total: u64 = hist.iter().map(|&c| c as u64).sum();
    if total == 0 {
        return Levels::default();
    }
    let level_at = |p: f64| -> i32 {
        let target = (p * total as f64) as u64;
        let mut seen = 0u64;
        for (b, &c) in hist.iter().enumerate() {
            seen += c as u64;
            if seen > target {
                return b as i32;
            }
        }
        255
    };
    let mut lv = Levels::default();
    lv.black = level_at(black_percentile) as f64 / 255.0;
    lv.white = (level_at(white_percentile) + 1).max((lv.black * 255.0) as i32 + 2) as f64 / 255.0;
    lv.white = lv.white.min(1.0);
    lv
}

/// Demosaic + stretch one raw frame to 8-bit RGB (3 bytes per pixel). Mono frames are replicated to grey.
pub fn render_rgb(pixels: &[u8], width: usize, height: usize, bit_depth: u32, bayer: Bayer, lv: &Levels) -> Vec<u8> {
    let (w, h) = (width as i32, height as i32);
    let mut out = vec![0u8; width * height * 3];
    let raw = Raw { p: pixels, w, h, bits: bit_depth };
    for y in 0..h {
        for x in 0..w {
            let mut rgb = [0.0f64; 3];
            if bayer == Bayer::None {
                let v = raw.at(x, y);
                rgb = [v, v, v];
            } else {
                // Bilinear: own colour from the site, the others averaged from the neighbours that carry them.
                let mut sum = [0.0f64; 3];
                let mut n = [0i32; 3];
                for dy in -1..2 {
                    for dx in -1..2 {
                        if dx == 0 && dy == 0 {
                            continue;
                        }
                        let c = site_color(bayer, mirror(x + dx, w), mirror(y + dy, h));
                        sum[c] += raw.at(x + dx, y + dy);
                        n[c] += 1;
                    }
                }
                let own = site_color(bayer, x, y);
                for c in 0..3 {
                    rgb[c] = if c == own {
                        raw.at(x, y)
                    } else if n[c] != 0 {
                        sum[c] / n[c] as f64
                    } else {
                        0.0
                    };
                }
            }
            let o = (y as usize * width + x as usize) * 3;
            for c in 0..3 {
                out[o + c] = stretch(rgb[c], lv, c);
            }
        }
    }
    out
}
