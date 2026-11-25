// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln-eml: a block-compressed texture encoded on several threads is byte for byte the encode on one thread.
//!
//! Each BC block depends only on its own 4x4 pixels (block_compression makes a new compressor per block), so the image
//! is cut into strips of whole block rows and each thread encodes its strip into its own rows of the output. These tests
//! hold that to the serial encode (`encode_with_threads(.., 1)`, which is EML's `encode`) for every BC format EML encodes,
//! for strips that do not divide evenly, more threads than block rows, the 1x1-block level and the error cases.
use kfc_content::image::{PixelFormat, encode, encode_threads, encode_with_threads, set_encode_threads, size_of_format};
use std::panic::{AssertUnwindSafe, catch_unwind};

const BC: &[PixelFormat] = &[
    PixelFormat::BC1_RGB_unorm_block,
    PixelFormat::BC1_RGB_srgb_block,
    PixelFormat::BC1_RGBA_unorm_block,
    PixelFormat::BC2_unorm_block,
    PixelFormat::BC3_srgb_block,
    PixelFormat::BC4_unorm_block,
    PixelFormat::BC5_unorm_block,
    PixelFormat::BC6H_ufloat_block,
    PixelFormat::BC7_unorm_block,
    PixelFormat::BC7_srgb_block,
];

/// A deterministic image with edges, gradients and alpha, so that every BC mode has something to choose between.
fn rgba(width: usize, height: usize, seed: u64) -> Vec<u8> {
    let mut state = seed.wrapping_mul(0x9e37_79b9_7f4a_7c15) | 1;
    let mut out = Vec::with_capacity(width * height * 4);
    for y in 0..height {
        for x in 0..width {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            let noise = (state >> 56) as u8;
            out.extend_from_slice(&[
                (x * 255 / width.max(1)) as u8 ^ (noise & 0x0f),
                (y * 255 / height.max(1)) as u8,
                if (x / 3 + y / 5) % 2 == 0 { 40 } else { 220 } ^ (noise >> 4),
                if x % 7 == 0 { 0 } else { 255 - (noise & 0x3f) },
            ]);
        }
    }
    out
}

fn encoded(format: PixelFormat, width: usize, height: usize, src: &[u8], threads: usize) -> Result<Vec<u8>, String> {
    let mut dst = vec![0xa5u8; size_of_format(format, width, height)];
    match catch_unwind(AssertUnwindSafe(|| encode_with_threads(format, width, height, src, &mut dst, threads))) {
        Ok(Ok(())) => Ok(dst),
        Ok(Err(e)) => Err(format!("error: {e}")),
        Err(_) => Err("panic".into()),
    }
}

#[test]
fn every_bc_format_encodes_the_same_bytes_on_any_number_of_threads() {
    // (width, height): one block; one block row; a square mip; 13 block rows (no thread count divides them evenly);
    // a tall strip; a wide one
    let sizes = [(4, 4), (16, 4), (64, 64), (128, 52), (8, 256), (256, 8)];
    for &format in BC {
        for (i, &(w, h)) in sizes.iter().enumerate() {
            let src = rgba(w, h, i as u64 + 1);
            let serial = encoded(format, w, h, &src, 1).unwrap_or_else(|e| panic!("{format:?} {w}x{h}: {e}"));
            assert!(serial.iter().any(|&b| b != 0xa5), "{format:?} {w}x{h}: the serial encode wrote nothing");
            for threads in [2, 3, 4, 7, 16, 64] {
                let split = encoded(format, w, h, &src, threads).unwrap_or_else(|e| panic!("{format:?} {w}x{h} x{threads}: {e}"));
                assert!(split == serial, "{format:?} {w}x{h} on {threads} threads differs from the serial encode");
            }
        }
    }
}

#[test]
fn sizes_that_are_not_whole_blocks_fail_as_the_serial_encode_does() {
    // block_compression asserts whole blocks; EML's Lua callers pad a level under 4x4 themselves. Whatever the serial
    // encode does with such a size (it panics), the split one does too, and never writes a partial result instead.
    for &format in BC {
        for (w, h) in [(6, 8), (8, 6), (2, 2), (1, 1)] {
            let src = rgba(w, h, 7);
            let serial = encoded(format, w, h, &src, 1);
            for threads in [2, 8] {
                assert_eq!(encoded(format, w, h, &src, threads), serial, "{format:?} {w}x{h} x{threads}");
            }
        }
    }
}

#[test]
fn uncompressed_formats_ignore_the_thread_count() {
    for format in [PixelFormat::R8G8B8A8_unorm, PixelFormat::R8_unorm, PixelFormat::R16G16B16A16_sfloat] {
        let src = rgba(32, 12, 3);
        assert_eq!(encoded(format, 32, 12, &src, 8), encoded(format, 32, 12, &src, 1), "{format:?}");
    }
}

#[test]
fn encode_uses_the_process_setting_one_thread_by_default() {
    // the only test in this binary that touches the process-wide setting
    assert_eq!(encode_threads(), 1, "EML's own behaviour unless the host asks for more");
    let src = rgba(64, 64, 9);
    let serial = encoded(PixelFormat::BC7_srgb_block, 64, 64, &src, 1).unwrap();
    assert_eq!(set_encode_threads(6), 1);
    assert_eq!(encode_threads(), 6);
    let mut dst = vec![0u8; serial.len()];
    encode(PixelFormat::BC7_srgb_block, 64, 64, &src, &mut dst).unwrap();
    assert!(dst == serial);
    assert_eq!(set_encode_threads(0), 6, "0 is taken as 1");
    assert_eq!(encode_threads(), 1);
}
