//! slopcap-kernelbench (Rust): time SlopCap's per-frame kernels on a recording, the twin of the C++
//! tools/kernelbench (same frames, same kernels, same output format), for tools/rust-vs-cpp.sh.
//!   kernelbench FILE.ser [--frames N] [--repeat R] [--kernels render,focus-gradient,focus-fourier,lossless]
//! Prints one line per kernel: name, median and mean ms per frame over every timed call, and a check value
//! (a hash of the output bytes, or the sum of the focus scores) to compare with the C++.

use slopcap_kernels::{focus, lossless, render, ser::Ser};
use std::hint::black_box;
use std::path::PathBuf;
use std::time::Instant;

/// FNV-1a, 64-bit: a cheap fingerprint of output bytes.
fn fnv(hash: u64, bytes: &[u8]) -> u64 {
    bytes.iter().fold(hash, |h, &b| (h ^ b as u64).wrapping_mul(0x100000001b3))
}
const FNV0: u64 = 0xcbf29ce484222325;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!("usage: kernelbench FILE.ser [--frames N] [--repeat R] [--kernels a,b,...]");
        std::process::exit(2);
    }
    let path = PathBuf::from(&args[1]);
    let (mut max_frames, mut repeat) = (usize::MAX, 5usize);
    let mut kernels = "render,focus-gradient,focus-fourier,lossless".to_string();
    let mut i = 2;
    while i + 1 < args.len() {
        match args[i].as_str() {
            "--frames" => max_frames = args[i + 1].parse().expect("--frames N"),
            "--repeat" => repeat = args[i + 1].parse().expect("--repeat R"),
            "--kernels" => kernels = args[i + 1].clone(),
            other => panic!("unknown option {other}"),
        }
        i += 2;
    }

    let mut ser = Ser::open(&path).expect("open SER");
    let (w, h, bits, bayer) = (ser.width, ser.height, ser.bit_depth, ser.bayer());
    let frames: Vec<Vec<u8>> = (0..ser.frames.min(max_frames))
        .map(|k| {
            let mut b = vec![0u8; ser.frame_bytes()];
            ser.read_frame(k, &mut b).expect("read frame");
            b
        })
        .collect();
    let name = path.file_stem().unwrap().to_string_lossy();

    for kernel in kernels.split(',') {
        // One untimed pass (caches, page faults), then `repeat` timed passes over every frame.
        let mut times = Vec::with_capacity(frames.len() * repeat);
        let mut check_hash = FNV0;
        let mut check_sum = 0f64;
        for pass in 0..=repeat {
            for f in &frames {
                let t0 = Instant::now();
                match kernel {
                    "render" => {
                        let hist = render::histogram(f, bits, 1);
                        let lv = render::auto_levels(&hist, 0.5, 0.9995);
                        let rgb = render::render_rgb(f, w, h, bits, bayer, &lv);
                        let dt = t0.elapsed();
                        if pass == 0 {
                            check_hash = fnv(check_hash, &rgb);
                        } else {
                            times.push(dt.as_secs_f64() * 1e3);
                        }
                        black_box(rgb);
                        continue;
                    }
                    "focus-gradient" | "focus-fourier" => {
                        let metric = if kernel == "focus-gradient" { focus::Metric::Gradient } else { focus::Metric::Fourier };
                        let opt = focus::FocusOptions { metric, ..Default::default() };
                        let s = focus::score_frame(f, w, h, bits, bayer, &opt);
                        let dt = t0.elapsed();
                        if pass == 0 {
                            for c in 0..s.channels as usize {
                                check_sum += s.ch[c].sharpness;
                            }
                        } else {
                            times.push(dt.as_secs_f64() * 1e3);
                        }
                        black_box(s);
                        continue;
                    }
                    "lossless" => {
                        let mut out = vec![0u8; w * h * if bits > 8 { 2 } else { 1 }];
                        let shift = lossless::residuals(f, w, h, bits, bayer, &mut out);
                        let dt = t0.elapsed();
                        if pass == 0 {
                            check_hash = fnv(fnv(check_hash, &out), &[shift as u8]);
                        } else {
                            times.push(dt.as_secs_f64() * 1e3);
                        }
                        black_box(out);
                        continue;
                    }
                    other => panic!("unknown kernel {other}"),
                }
            }
        }
        times.sort_by(|a, b| a.partial_cmp(b).unwrap());
        let median = times[times.len() / 2];
        let mean = times.iter().sum::<f64>() / times.len() as f64;
        let check = if kernel.starts_with("focus") { format!("{check_sum:.9e}") } else { format!("{check_hash:016x}") };
        println!(
            "rust {name} {w}x{h} {bits}-bit {kernel} frames={} calls={} median_ms={median:.4} mean_ms={mean:.4} min_ms={:.4} check={check}",
            frames.len(),
            times.len(),
            times[0]
        );
    }
}
