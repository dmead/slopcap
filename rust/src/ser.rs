//! A minimal SER reader: the 178-byte header and raw frames (no timestamps), as `src/io/ser.cpp` reads them.

use crate::Bayer;
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::Path;

const HEADER: u64 = 178;

pub struct Ser {
    file: File,
    pub width: usize,
    pub height: usize,
    pub bit_depth: u32,
    pub color_id: i32,
    /// Frames actually on disk (a cut-off recording has fewer than its header says).
    pub frames: usize,
}

fn i32_at(b: &[u8], off: usize) -> i32 {
    i32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

impl Ser {
    pub fn open(path: &Path) -> io::Result<Ser> {
        let mut file = File::open(path)?;
        let mut h = [0u8; HEADER as usize];
        file.read_exact(&mut h)?;
        if &h[0..14] != b"LUCAM-RECORDER" {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "not a SER file"));
        }
        let color_id = i32_at(&h, 18);
        let width = i32_at(&h, 26).max(0) as usize;
        let height = i32_at(&h, 30).max(0) as usize;
        let bit_depth = i32_at(&h, 34).max(1) as u32;
        let in_header = i32_at(&h, 38).max(0) as usize;
        let mut s = Ser { file, width, height, bit_depth, color_id, frames: 0 };
        let len = s.file.metadata()?.len();
        let on_disk = (len.saturating_sub(HEADER) / s.frame_bytes().max(1) as u64) as usize;
        s.frames = in_header.min(on_disk);
        Ok(s)
    }

    pub fn bytes_per_sample(&self) -> usize {
        if self.bit_depth > 8 { 2 } else { 1 }
    }

    pub fn frame_bytes(&self) -> usize {
        let planes = if self.color_id >= 100 { 3 } else { 1 };
        self.width * self.height * self.bytes_per_sample() * planes
    }

    pub fn bayer(&self) -> Bayer {
        match self.color_id {
            8 => Bayer::Rggb,
            9 => Bayer::Grbg,
            10 => Bayer::Gbrg,
            11 => Bayer::Bggr,
            _ => Bayer::None,
        }
    }

    pub fn read_frame(&mut self, index: usize, dst: &mut [u8]) -> io::Result<()> {
        self.file.seek(SeekFrom::Start(HEADER + (index * self.frame_bytes()) as u64))?;
        self.file.read_exact(&mut dst[..self.frame_bytes()])
    }
}
