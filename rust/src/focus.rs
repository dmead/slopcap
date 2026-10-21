//! Port of `src/analysis/focus.cpp`: per-channel focus scores on a raw frame. Same steps and constants: a sparse,
//! box-blurred sample locates the target; a window around it is extracted per colour plane (no interpolation); each
//! plane gets its levels, signal and centroid, and one of four sharpness metrics.
//!
//! Float sums run eight lanes at once ([`crate::lanes::F8`]) where the C++ uses `SLOPCAP_SIMD_SUM`, so both are
//! allowed the same reordering (results agree to float rounding, not bit for bit).

use crate::lanes::{F8, L};
use crate::Bayer;
use std::ops::{Add, Mul, Sub};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Metric {
    Gradient,
    Laplacian,
    Variance,
    Fourier,
}

impl Metric {
    pub fn from_name(n: &str) -> Option<Metric> {
        match n {
            "gradient" => Some(Metric::Gradient),
            "laplacian" => Some(Metric::Laplacian),
            "variance" => Some(Metric::Variance),
            "fourier" => Some(Metric::Fourier),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct FocusOptions {
    pub metric: Metric,
    pub window: i32,
    pub band_low: f64,
    pub band_high: f64,
}

impl Default for FocusOptions {
    fn default() -> Self {
        FocusOptions { metric: Metric::Gradient, window: 384, band_low: 0.10, band_high: 0.50 }
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct ChannelScore {
    pub sharpness: f64,
    pub cx: f64,
    pub cy: f64,
    pub peak: f64,
    pub background: f64,
    pub clipped: f64,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct FocusScores {
    pub channels: i32,
    pub ch: [ChannelScore; 3],
    pub window: [i32; 4],
    pub target: [i32; 4],
}

/// Where each colour sits in the 2x2 Bayer cell. Mono: one channel, a 1-pixel cell.
#[derive(Clone, Copy)]
struct Layout {
    cell: i32,
    channels: i32,
    sx: [[i32; 2]; 3],
    sy: [[i32; 2]; 3],
    sites: [i32; 3],
}

fn layout_of(b: Bayer) -> Layout {
    let mut l = Layout { cell: 2, channels: 3, sx: [[0; 2]; 3], sy: [[0; 2]; 3], sites: [1, 2, 1] };
    if b == Bayer::None {
        l.cell = 1;
        l.channels = 1;
        l.sites[0] = 1;
        return l;
    }
    let (rx, ry) = match b {
        Bayer::Grbg => (1, 0),
        Bayer::Gbrg => (0, 1),
        Bayer::Bggr => (1, 1),
        _ => (0, 0),
    };
    l.sx[0][0] = rx;
    l.sy[0][0] = ry;
    l.sx[2][0] = 1 - rx;
    l.sy[2][0] = 1 - ry;
    l.sx[1][0] = 1 - rx;
    l.sy[1][0] = ry;
    l.sx[1][1] = rx;
    l.sy[1][1] = 1 - ry;
    l
}

/// One raw sample, 0..1 (only used by the sparse locate pass).
#[inline(always)]
fn sample_at(p: &[u8], width: usize, bits: u32, x: i32, y: i32) -> f32 {
    let i = y as usize * width + x as usize;
    if bits > 8 {
        u16::from_le_bytes([p[2 * i], p[2 * i + 1]]) as f32 * (1.0 / 65535.0)
    } else {
        p[i] as f32 * (1.0 / 255.0)
    }
}

/// The q-quantile of values in 0..1, to 1/4096 of full scale (a histogram, as in the C++).
fn quantile01(v: &[f32], q: f64) -> f32 {
    const BINS: usize = 4096;
    let mut h = [0u32; BINS]; // an array, not a Vec: its length is known, so the clamped index needs no bounds check
    for &x in v {
        // Clamped as a float, then cast: same bins as the C++'s cast-then-clamp, without the saturating int cast's
        // range checks (Rust's `as` from float saturates; C++'s static_cast doesn't need to).
        let b = (x * (BINS - 1) as f32 + 0.5).clamp(0.0, (BINS - 1) as f32) as usize;
        h[b] += 1;
    }
    let target = (q * v.len() as f64) as u64;
    let mut seen = 0u64;
    for (b, &c) in h.iter().enumerate() {
        seen += c as u64;
        if seen > target {
            return b as f32 / (BINS - 1) as f32;
        }
    }
    1.0
}

/// A window of one channel, row-major floats.
struct Plane {
    w: usize,
    h: usize,
    v: Vec<f32>,
}

impl Plane {
    #[inline(always)]
    fn row(&self, y: usize) -> &[f32] {
        &self.v[y * self.w..(y + 1) * self.w]
    }
}

/// One plane's window, scaled to 0..1. `bpp` 1 or 2 (16-bit little-endian). Each output row is a strided walk over
/// whole cells of the raw row (chunks_exact: no per-sample bounds checks).
#[allow(clippy::too_many_arguments)]
fn extract(pixels: &[u8], width: usize, bpp: usize, l: &Layout, c: usize, px0: usize, py0: usize, pw: usize, ph: usize, scale: f32) -> Plane {
    let cell = l.cell as usize;
    let mut v = vec![0f32; pw * ph];
    for (y, out) in v.chunks_exact_mut(pw).enumerate() {
        let fy = (py0 + y) * cell;
        let row = |site: usize| {
            let start = ((fy + l.sy[c][site] as usize) * width + px0 * cell) * bpp;
            &pixels[start..start + pw * cell * bpp]
        };
        let (sx0, sx1) = (l.sx[c][0] as usize * bpp, l.sx[c][1] as usize * bpp);
        if bpp == 1 {
            if l.sites[c] == 2 {
                let half = 0.5 * scale;
                for ((o, a), b) in out.iter_mut().zip(row(0).chunks_exact(cell)).zip(row(1).chunks_exact(cell)) {
                    *o = (a[sx0] as f32 + b[sx1] as f32) * half;
                }
            } else {
                for (o, a) in out.iter_mut().zip(row(0).chunks_exact(cell)) {
                    *o = a[sx0] as f32 * scale;
                }
            }
        } else {
            let at = |s: &[u8], k: usize| u16::from_le_bytes([s[k], s[k + 1]]) as f32;
            if l.sites[c] == 2 {
                let half = 0.5 * scale;
                for ((o, a), b) in out.iter_mut().zip(row(0).chunks_exact(2 * cell)).zip(row(1).chunks_exact(2 * cell)) {
                    *o = (at(a, sx0) + at(b, sx1)) * half;
                }
            } else {
                for (o, a) in out.iter_mut().zip(row(0).chunks_exact(2 * cell)) {
                    *o = at(a, sx0) * scale;
                }
            }
        }
    }
    Plane { w: pw, h: ph, v }
}

/// Sky level (low percentile of a sparse sample), peak and clipped fraction of a plane. The max compares bit
/// patterns (non-negative floats order like their bits), as in the C++: an integer max vectorizes, a float one doesn't.
fn plane_levels(p: &Plane) -> (f32, f32, f64) {
    let step = ((p.w as f64 * p.h as f64 / 16384.0).sqrt() as usize).max(1);
    let mut s = Vec::new();
    for y in (0..p.h).step_by(step) {
        for x in (0..p.w).step_by(step) {
            s.push(p.v[y * p.w + x]);
        }
    }
    let bg = quantile01(&s, 0.25);
    let mut peak = 0f32;
    let mut clip = 0usize;
    for y in 0..p.h {
        let mut m = 0i32;
        let mut c = 0i32;
        for &v in p.row(y) {
            m = m.max(v.to_bits() as i32);
            c += (v >= 0.995) as i32;
        }
        peak = peak.max(f32::from_bits(m as u32));
        clip += c as usize;
    }
    (bg, peak, clip as f64 / (p.w as f64 * p.h as f64))
}

/// Squared signal above the sky, and the brightness centroid of the pixels above `thresh` (plane coordinates).
fn signal_and_centroid(p: &Plane, bg: f32, thresh: f32) -> (f64, f64, f64) {
    let (mut s2, mut m, mut mx, mut my) = (0f64, 0f64, 0f64, 0f64);
    let (bgv, tv) = (F8::splat(bg), F8::splat(thresh));
    for y in 0..p.h {
        let r = p.row(y);
        let (chunks, tail) = r.as_chunks::<L>();
        let (mut a2, mut am, mut amx) = (F8::default(), F8::default(), F8::default());
        for (ci, ch) in chunks.iter().enumerate() {
            let v = F8::load(ch);
            let s = v - bgv;
            a2 = a2 + s * s;
            let w = v.gt_or_zero(tv, s);
            am = am + w;
            amx = amx + w * F8::iota((ci * L) as f32);
        }
        let (mut rs2, mut rm, mut rmx) = (a2.total(), am.total(), amx.total());
        let x0 = r.len() - tail.len();
        for (k, &v) in tail.iter().enumerate() {
            let s = v - bg;
            rs2 += s * s;
            let w = if v > thresh { s } else { 0.0 };
            rm += w;
            rmx += w * (x0 + k) as f32;
        }
        s2 += rs2 as f64;
        m += rm as f64;
        mx += rmx as f64;
        my += rm as f64 * y as f64;
    }
    let cx = if m > 0.0 { mx / m } else { p.w as f64 / 2.0 };
    let cy = if m > 0.0 { my / m } else { p.h as f64 / 2.0 };
    (s2, cx, cy)
}

/// Numbers the per-pixel formulas run on: one f32, or eight at once ([`F8`]).
trait Num: Copy + Add<Output = Self> + Sub<Output = Self> + Mul<Output = Self> {
    fn splat(v: f32) -> Self;
}
impl Num for f32 {
    #[inline(always)]
    fn splat(v: f32) -> f32 {
        v
    }
}
impl Num for F8 {
    #[inline(always)]
    fn splat(v: f32) -> F8 {
        F8::splat(v)
    }
}

/// A three-row stencil metric: its value at one pixel from left, centre, right, up and down.
trait Stencil {
    fn at<T: Num>(l: T, c: T, r: T, u: T, d: T) -> T;
}
struct Gradient;
impl Stencil for Gradient {
    #[inline(always)]
    fn at<T: Num>(l: T, _c: T, r: T, u: T, d: T) -> T {
        let (dx, dy) = (r - l, d - u);
        dx * dx + dy * dy
    }
}
struct Laplacian;
impl Stencil for Laplacian {
    #[inline(always)]
    fn at<T: Num>(l: T, c: T, r: T, u: T, d: T) -> T {
        let lap = l + r + u + d - T::splat(4.0) * c;
        lap * lap
    }
}

/// Sum over x in 1..w-1 of S at (x, y), eight pixels at a time, the rest one by one.
#[inline(always)]
fn stencil_sum<S: Stencil>(up: &[f32], r: &[f32], dn: &[f32]) -> f32 {
    let n = r.len() - 2;
    let (left, centre, right, u, d) = (&r[..n], &r[1..n + 1], &r[2..n + 2], &up[1..n + 1], &dn[1..n + 1]);
    let mut acc = F8::default();
    let it = left.as_chunks::<L>().0.iter().zip(centre.as_chunks::<L>().0).zip(right.as_chunks::<L>().0)
        .zip(u.as_chunks::<L>().0).zip(d.as_chunks::<L>().0);
    for ((((a, b), c), e), g) in it {
        acc = acc + S::at(F8::load(a), F8::load(b), F8::load(c), F8::load(e), F8::load(g));
    }
    let mut sum = acc.total();
    for i in n - n % L..n {
        sum += S::at(left[i], centre[i], right[i], u[i], d[i]);
    }
    sum
}

fn stencil_energy<S: Stencil>(p: &Plane) -> f64 {
    let mut g = 0f64;
    for y in 1..p.h.saturating_sub(1) {
        g += stencil_sum::<S>(p.row(y - 1), p.row(y), p.row(y + 1)) as f64;
    }
    g
}

fn normalised_variance(p: &Plane, bg: f32) -> f64 {
    let (mut s, mut s2) = (0f64, 0f64);
    let bgv = F8::splat(bg);
    for y in 0..p.h {
        let r = p.row(y);
        let (chunks, tail) = r.as_chunks::<L>();
        let (mut a, mut a2) = (F8::default(), F8::default());
        for ch in chunks {
            let v = F8::load(ch) - bgv;
            a = a + v;
            a2 = a2 + v * v;
        }
        let (mut rs, mut rs2) = (a.total(), a2.total());
        for &v in tail {
            let v = v - bg;
            rs += v;
            rs2 += v * v;
        }
        s += rs as f64;
        s2 += rs2 as f64;
    }
    let n = p.w as f64 * p.h as f64;
    let mean = s / n;
    if mean > 0.0 { (s2 / n - mean * mean) / (mean * mean) } else { 0.0 }
}

/// A complex float, enough for the FFT (std::complex<float> on the C++ side).
#[derive(Clone, Copy, Default)]
struct C32 {
    re: f32,
    im: f32,
}

impl C32 {
    #[inline(always)]
    fn add(self, o: C32) -> C32 {
        C32 { re: self.re + o.re, im: self.im + o.im }
    }
    #[inline(always)]
    fn sub(self, o: C32) -> C32 {
        C32 { re: self.re - o.re, im: self.im - o.im }
    }
    #[inline(always)]
    fn mul(self, o: C32) -> C32 {
        C32 { re: self.re * o.re - self.im * o.im, im: self.re * o.im + self.im * o.re }
    }
    #[inline(always)]
    fn norm(self) -> f32 {
        self.re * self.re + self.im * self.im
    }
}

/// In-place radix-2 FFT of n = 2^k complex values.
fn fft(a: &mut [C32], tw: &[C32]) {
    let n = a.len();
    let mut j = 0usize;
    for i in 1..n {
        let mut bit = n >> 1;
        while j & bit != 0 {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if i < j {
            a.swap(i, j);
        }
    }
    let mut len = 2;
    while len <= n {
        let step = n / len;
        let half = len / 2;
        for block in a.chunks_exact_mut(len) {
            let (lo, hi) = block.split_at_mut(half);
            for k in 0..half {
                let u = lo[k];
                let v = hi[k].mul(tw[k * step]);
                lo[k] = u.add(v);
                hi[k] = u.sub(v);
            }
        }
        len <<= 1;
    }
}

fn band_power_share(p: &Plane, bg: f32, lo: f64, hi: f64) -> f64 {
    let n = p.w; // square, power of two
    let tw: Vec<C32> = (0..n / 2)
        .map(|k| {
            let a = (-2.0 * std::f64::consts::PI * k as f64 / n as f64) as f32;
            C32 { re: a.cos(), im: a.sin() }
        })
        .collect();
    let hann: Vec<f32> =
        (0..n).map(|i| (0.5 - 0.5 * (2.0 * std::f64::consts::PI * i as f64 / (n - 1) as f64).cos()) as f32).collect();
    let mut f = vec![C32::default(); n * n];
    for (y, o) in f.chunks_exact_mut(n).enumerate() {
        let r = p.row(y);
        for x in 0..n {
            o[x] = C32 { re: (r[x] - bg) * hann[x] * hann[y], im: 0.0 };
        }
        fft(o, &tw);
    }
    let mut col = vec![C32::default(); n];
    let (mut band, mut total) = (0f64, 0f64);
    for x in 0..n {
        for y in 0..n {
            col[y] = f[y * n + x];
        }
        fft(&mut col, &tw);
        let u = if x <= n / 2 { x as i64 } else { x as i64 - n as i64 };
        for (y, c) in col.iter().enumerate() {
            let v = if y <= n / 2 { y as i64 } else { y as i64 - n as i64 };
            if u == 0 && v == 0 {
                continue; // DC: overall brightness, not detail
            }
            let r = ((u * u + v * v) as f64).sqrt() / (n as f64 / 2.0);
            let pw = c.norm() as f64;
            if r <= 1.0 {
                total += pw;
            }
            if r >= lo && r < hi {
                band += pw;
            }
        }
    }
    if total > 0.0 { band / total } else { 0.0 }
}

/// Score one raw frame (8-bit, or 16-bit little-endian).
pub fn score_frame(pixels: &[u8], width: usize, height: usize, bit_depth: u32, bayer: Bayer, opt: &FocusOptions) -> FocusScores {
    let mut out = FocusScores::default();
    let bpp = if bit_depth > 8 { 2 } else { 1 };
    if width < 8 || height < 8 || pixels.len() < width * height * bpp {
        return out;
    }
    let l = layout_of(bayer);
    let cell = l.cell;
    let (pw, ph) = (width as i32 / cell, height as i32 / cell);
    let ref_c = if l.channels == 3 { 1 } else { 0 };

    // 1. Locate the target on a sparse sample (~16k points) of the reference channel, box-blurred 3x3.
    let step = ((pw as f64 * ph as f64 / 16384.0).sqrt().ceil() as i32).max(1);
    let ref_at = |x: i32, y: i32| -> f32 {
        let mut v = 0f32;
        for k in 0..l.sites[ref_c] as usize {
            v += sample_at(pixels, width, bit_depth, x * cell + l.sx[ref_c][k], y * cell + l.sy[ref_c][k]);
        }
        v / l.sites[ref_c] as f32
    };
    let (gw, gh) = ((pw + step - 1) / step, (ph + step - 1) / step);
    let mut s = Vec::with_capacity(((pw / step + 1) * (ph / step + 1)) as usize);
    for y in (0..ph).step_by(step as usize) {
        for x in (0..pw).step_by(step as usize) {
            s.push(ref_at(x, y));
        }
    }
    // The 3x3 box blur, clipped at the edges: each cell's neighbour rows are slices, so bounds are checked per row
    // rather than per neighbour (a per-neighbour test and index check made this pass 2.5x slower than the C++). Same
    // sums in the same order as the C++.
    let (gwu, ghu) = (gw as usize, gh as usize);
    let mut g = vec![0f32; s.len()];
    for (y, grow) in g.chunks_exact_mut(gwu).enumerate().take(ghu) {
        let (ya, yb) = (y.saturating_sub(1), (y + 1).min(ghu - 1));
        for (x, out) in grow.iter_mut().enumerate() {
            if y > 0 && y + 1 < ghu && x > 0 && x + 1 < gwu {
                // Interior: three whole rows of three, as arrays (no inner loops, no per-element checks).
                let row = |yy: usize| -> [f32; 3] { s[yy * gwu + x - 1..yy * gwu + x + 2].try_into().unwrap() };
                let (a, b, c) = (row(y - 1), row(y), row(y + 1));
                let sum = a[0] + a[1] + a[2] + b[0] + b[1] + b[2] + c[0] + c[1] + c[2];
                *out = sum / 9.0;
                continue;
            }
            let (xa, xb) = (x.saturating_sub(1), (x + 1).min(gwu - 1));
            let mut sum = 0f32;
            for yy in ya..yb + 1 {
                for &v in &s[yy * gwu + xa..yy * gwu + xb + 1] {
                    sum += v;
                }
            }
            *out = sum / ((xb - xa + 1) * (yb - ya + 1)) as f32;
        }
    }
    let bg = quantile01(&g, 0.25);
    let peak = g.iter().copied().fold(f32::MIN, f32::max);
    if peak - bg < 0.02 {
        return out; // nothing there (dark frame, cloud)
    }
    let thresh = bg + 0.15 * (peak - bg);
    let (mut m, mut mx, mut my) = (0f64, 0f64, 0f64);
    let (mut bx0, mut by0, mut bx1, mut by1) = (gw, gh, 0, 0);
    for y in 0..gh {
        for x in 0..gw {
            let v = g[(y * gw + x) as usize];
            if v > thresh {
                let w = (v - bg) as f64;
                m += w;
                mx += w * (x * step) as f64;
                my += w * (y * step) as f64;
                bx0 = bx0.min(x);
                by0 = by0.min(y);
                bx1 = bx1.max(x);
                by1 = by1.max(y);
            }
        }
    }
    out.target = [bx0 * step * cell, by0 * step * cell, (width as i32).min((bx1 + 1) * step * cell), (height as i32).min((by1 + 1) * step * cell)];
    let (cx, cy) = ((mx / m) as i32, (my / m) as i32);

    // 2. Window around it (a square power of two for the FFT), clamped to the frame.
    let mut win = opt.window.min(pw).min(ph);
    if opt.metric == Metric::Fourier {
        win = 1 << (win.max(8) as f64).log2().floor() as i32;
    }
    let (ww, wh) = (win.min(pw), win.min(ph));
    let x0 = (cx - ww / 2).clamp(0, pw - ww);
    let y0 = (cy - wh / 2).clamp(0, ph - wh);
    out.window = [x0 * cell, y0 * cell, ww * cell, wh * cell];

    // 3. Extract and score each channel on the same window.
    out.channels = l.channels;
    let scale = if bpp == 2 { 1.0 / 65535.0 } else { 1.0 / 255.0 };
    for c in 0..l.channels as usize {
        let p = extract(pixels, width, bpp, &l, c, x0 as usize, y0 as usize, ww as usize, wh as usize, scale);
        let (pbg, ppeak, clipped) = plane_levels(&p);
        let (signal2, pcx, pcy) = signal_and_centroid(&p, pbg, pbg + 0.15 * (ppeak - pbg));
        let (mut off_x, mut off_y) = (0f64, 0f64);
        for k in 0..l.sites[c] as usize {
            off_x += l.sx[c][k] as f64;
            off_y += l.sy[c][k] as f64;
        }
        let sharpness = match opt.metric {
            Metric::Gradient => if signal2 > 0.0 { stencil_energy::<Gradient>(&p) / signal2 } else { 0.0 },
            Metric::Laplacian => if signal2 > 0.0 { stencil_energy::<Laplacian>(&p) / signal2 } else { 0.0 },
            Metric::Variance => normalised_variance(&p, pbg),
            Metric::Fourier => band_power_share(&p, pbg, opt.band_low, opt.band_high),
        };
        out.ch[c] = ChannelScore {
            sharpness,
            cx: (x0 as f64 + pcx) * cell as f64 + off_x / l.sites[c] as f64,
            cy: (y0 as f64 + pcy) * cell as f64 + off_y / l.sites[c] as f64,
            peak: ppeak as f64,
            background: pbg as f64,
            clipped,
        };
    }
    out
}
