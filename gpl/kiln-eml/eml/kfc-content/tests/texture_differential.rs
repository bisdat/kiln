// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln-eml: a dependency differential harness for the image paths EML's startup pass reaches (mod-loader-lua
//! env/image.rs), for a lock refresh such as image 0.25.6 -> 0.25.10 (half 2.6.0 -> 2.7.1 with it):
//! - `image.encode_texture` / `image.decode_texture`: `kfc_content::image::{size_of_format, encode, decode}` for every
//!   PixelFormat, on a synthetic RGBA image (encode, and decode of that encoding) and on fixed pseudo-random block bytes;
//! - `image.encode` / `image.decode`: `RgbaImage::write_to` and `image::load_from_memory[_with_format]` for every
//!   format name the Lua binding accepts.
//!
//! With `TEXDIFF_OUT=<dir>` it writes every output (raw bytes, or the error/panic text) there; with `TEXDIFF_CORPUS=<dir>`
//! (another run's output) it also decodes that run's encodings, so each build decodes the other's bytes. Two builds of
//! this same file with different locks are then compared file by file. Without them it only checks that it is
//! deterministic. All inputs are synthetic; nothing comes from a game.
use image::{ImageFormat, RgbaImage};
use kfc_content::image::{PixelFormat, decode, encode, size_of_format};
use std::{
    fs,
    io::Cursor,
    panic::{AssertUnwindSafe, catch_unwind},
    path::{Path, PathBuf},
};

const PIXEL_FORMATS: &[(&str, PixelFormat)] = &[
    ("None", PixelFormat::None),
    ("R4G4_unorm_pack8", PixelFormat::R4G4_unorm_pack8),
    ("R4G4B4A4_unorm_pack16", PixelFormat::R4G4B4A4_unorm_pack16),
    ("B4G4R4A4_unorm_pack16", PixelFormat::B4G4R4A4_unorm_pack16),
    ("R5G6B5_unorm_pack16", PixelFormat::R5G6B5_unorm_pack16),
    ("B5G6R5_unorm_pack16", PixelFormat::B5G6R5_unorm_pack16),
    ("R5G5B5A1_unorm_pack16", PixelFormat::R5G5B5A1_unorm_pack16),
    ("B5G5R5A1_unorm_pack16", PixelFormat::B5G5R5A1_unorm_pack16),
    ("A1R5G5B5_unorm_pack16", PixelFormat::A1R5G5B5_unorm_pack16),
    ("R8_unorm", PixelFormat::R8_unorm),
    ("R8_snorm", PixelFormat::R8_snorm),
    ("R8_uscaled", PixelFormat::R8_uscaled),
    ("R8_sscaled", PixelFormat::R8_sscaled),
    ("R8_uint", PixelFormat::R8_uint),
    ("R8_sint", PixelFormat::R8_sint),
    ("R8_srgb", PixelFormat::R8_srgb),
    ("R8G8_unorm", PixelFormat::R8G8_unorm),
    ("R8G8_snorm", PixelFormat::R8G8_snorm),
    ("R8G8_uscaled", PixelFormat::R8G8_uscaled),
    ("R8G8_sscaled", PixelFormat::R8G8_sscaled),
    ("R8G8_uint", PixelFormat::R8G8_uint),
    ("R8G8_sint", PixelFormat::R8G8_sint),
    ("R8G8_srgb", PixelFormat::R8G8_srgb),
    ("R8G8B8_unorm", PixelFormat::R8G8B8_unorm),
    ("R8G8B8_snorm", PixelFormat::R8G8B8_snorm),
    ("R8G8B8_uscaled", PixelFormat::R8G8B8_uscaled),
    ("R8G8B8_sscaled", PixelFormat::R8G8B8_sscaled),
    ("R8G8B8_uint", PixelFormat::R8G8B8_uint),
    ("R8G8B8_sint", PixelFormat::R8G8B8_sint),
    ("R8G8B8_srgb", PixelFormat::R8G8B8_srgb),
    ("B8G8R8_unorm", PixelFormat::B8G8R8_unorm),
    ("B8G8R8_snorm", PixelFormat::B8G8R8_snorm),
    ("B8G8R8_uscaled", PixelFormat::B8G8R8_uscaled),
    ("B8G8R8_sscaled", PixelFormat::B8G8R8_sscaled),
    ("B8G8R8_uint", PixelFormat::B8G8R8_uint),
    ("B8G8R8_sint", PixelFormat::B8G8R8_sint),
    ("B8G8R8_srgb", PixelFormat::B8G8R8_srgb),
    ("R8G8B8A8_unorm", PixelFormat::R8G8B8A8_unorm),
    ("R8G8B8A8_snorm", PixelFormat::R8G8B8A8_snorm),
    ("R8G8B8A8_uscaled", PixelFormat::R8G8B8A8_uscaled),
    ("R8G8B8A8_sscaled", PixelFormat::R8G8B8A8_sscaled),
    ("R8G8B8A8_uint", PixelFormat::R8G8B8A8_uint),
    ("R8G8B8A8_sint", PixelFormat::R8G8B8A8_sint),
    ("R8G8B8A8_srgb", PixelFormat::R8G8B8A8_srgb),
    ("B8G8R8A8_unorm", PixelFormat::B8G8R8A8_unorm),
    ("B8G8R8A8_snorm", PixelFormat::B8G8R8A8_snorm),
    ("B8G8R8A8_uscaled", PixelFormat::B8G8R8A8_uscaled),
    ("B8G8R8A8_sscaled", PixelFormat::B8G8R8A8_sscaled),
    ("B8G8R8A8_uint", PixelFormat::B8G8R8A8_uint),
    ("B8G8R8A8_sint", PixelFormat::B8G8R8A8_sint),
    ("B8G8R8A8_srgb", PixelFormat::B8G8R8A8_srgb),
    ("A8B8G8R8_unorm_pack32", PixelFormat::A8B8G8R8_unorm_pack32),
    ("A8B8G8R8_snorm_pack32", PixelFormat::A8B8G8R8_snorm_pack32),
    (
        "A8B8G8R8_uscaled_pack32",
        PixelFormat::A8B8G8R8_uscaled_pack32,
    ),
    (
        "A8B8G8R8_sscaled_pack32",
        PixelFormat::A8B8G8R8_sscaled_pack32,
    ),
    ("A8B8G8R8_uint_pack32", PixelFormat::A8B8G8R8_uint_pack32),
    ("A8B8G8R8_sint_pack32", PixelFormat::A8B8G8R8_sint_pack32),
    ("A8B8G8R8_srgb_pack32", PixelFormat::A8B8G8R8_srgb_pack32),
    (
        "A2R10G10B10_unorm_pack32",
        PixelFormat::A2R10G10B10_unorm_pack32,
    ),
    (
        "A2R10G10B10_snorm_pack32",
        PixelFormat::A2R10G10B10_snorm_pack32,
    ),
    (
        "A2R10G10B10_uscaled_pack32",
        PixelFormat::A2R10G10B10_uscaled_pack32,
    ),
    (
        "A2R10G10B10_sscaled_pack32",
        PixelFormat::A2R10G10B10_sscaled_pack32,
    ),
    (
        "A2R10G10B10_uint_pack32",
        PixelFormat::A2R10G10B10_uint_pack32,
    ),
    (
        "A2R10G10B10_sint_pack32",
        PixelFormat::A2R10G10B10_sint_pack32,
    ),
    (
        "A2B10G10R10_unorm_pack32",
        PixelFormat::A2B10G10R10_unorm_pack32,
    ),
    (
        "A2B10G10R10_snorm_pack32",
        PixelFormat::A2B10G10R10_snorm_pack32,
    ),
    (
        "A2B10G10R10_uscaled_pack32",
        PixelFormat::A2B10G10R10_uscaled_pack32,
    ),
    (
        "A2B10G10R10_sscaled_pack32",
        PixelFormat::A2B10G10R10_sscaled_pack32,
    ),
    (
        "A2B10G10R10_uint_pack32",
        PixelFormat::A2B10G10R10_uint_pack32,
    ),
    (
        "A2B10G10R10_sint_pack32",
        PixelFormat::A2B10G10R10_sint_pack32,
    ),
    ("R16_unorm", PixelFormat::R16_unorm),
    ("R16_snorm", PixelFormat::R16_snorm),
    ("R16_uscaled", PixelFormat::R16_uscaled),
    ("R16_sscaled", PixelFormat::R16_sscaled),
    ("R16_uint", PixelFormat::R16_uint),
    ("R16_sint", PixelFormat::R16_sint),
    ("R16_sfloat", PixelFormat::R16_sfloat),
    ("R16G16_unorm", PixelFormat::R16G16_unorm),
    ("R16G16_snorm", PixelFormat::R16G16_snorm),
    ("R16G16_uscaled", PixelFormat::R16G16_uscaled),
    ("R16G16_sscaled", PixelFormat::R16G16_sscaled),
    ("R16G16_uint", PixelFormat::R16G16_uint),
    ("R16G16_sint", PixelFormat::R16G16_sint),
    ("R16G16_sfloat", PixelFormat::R16G16_sfloat),
    ("R16G16B16_unorm", PixelFormat::R16G16B16_unorm),
    ("R16G16B16_snorm", PixelFormat::R16G16B16_snorm),
    ("R16G16B16_uscaled", PixelFormat::R16G16B16_uscaled),
    ("R16G16B16_sscaled", PixelFormat::R16G16B16_sscaled),
    ("R16G16B16_uint", PixelFormat::R16G16B16_uint),
    ("R16G16B16_sint", PixelFormat::R16G16B16_sint),
    ("R16G16B16_sfloat", PixelFormat::R16G16B16_sfloat),
    ("R16G16B16A16_unorm", PixelFormat::R16G16B16A16_unorm),
    ("R16G16B16A16_snorm", PixelFormat::R16G16B16A16_snorm),
    ("R16G16B16A16_uscaled", PixelFormat::R16G16B16A16_uscaled),
    ("R16G16B16A16_sscaled", PixelFormat::R16G16B16A16_sscaled),
    ("R16G16B16A16_uint", PixelFormat::R16G16B16A16_uint),
    ("R16G16B16A16_sint", PixelFormat::R16G16B16A16_sint),
    ("R16G16B16A16_sfloat", PixelFormat::R16G16B16A16_sfloat),
    ("R32_uint", PixelFormat::R32_uint),
    ("R32_sint", PixelFormat::R32_sint),
    ("R32_sfloat", PixelFormat::R32_sfloat),
    ("R32G32_uint", PixelFormat::R32G32_uint),
    ("R32G32_sint", PixelFormat::R32G32_sint),
    ("R32G32_sfloat", PixelFormat::R32G32_sfloat),
    ("R32G32B32_uint", PixelFormat::R32G32B32_uint),
    ("R32G32B32_sint", PixelFormat::R32G32B32_sint),
    ("R32G32B32_sfloat", PixelFormat::R32G32B32_sfloat),
    ("R32G32B32A32_uint", PixelFormat::R32G32B32A32_uint),
    ("R32G32B32A32_sint", PixelFormat::R32G32B32A32_sint),
    ("R32G32B32A32_sfloat", PixelFormat::R32G32B32A32_sfloat),
    ("R64_uint", PixelFormat::R64_uint),
    ("R64_sint", PixelFormat::R64_sint),
    ("R64_sfloat", PixelFormat::R64_sfloat),
    ("R64G64_uint", PixelFormat::R64G64_uint),
    ("R64G64_sint", PixelFormat::R64G64_sint),
    ("R64G64_sfloat", PixelFormat::R64G64_sfloat),
    ("R64G64B64_uint", PixelFormat::R64G64B64_uint),
    ("R64G64B64_sint", PixelFormat::R64G64B64_sint),
    ("R64G64B64_sfloat", PixelFormat::R64G64B64_sfloat),
    ("R64G64B64A64_uint", PixelFormat::R64G64B64A64_uint),
    ("R64G64B64A64_sint", PixelFormat::R64G64B64A64_sint),
    ("R64G64B64A64_sfloat", PixelFormat::R64G64B64A64_sfloat),
    (
        "B10G11R11_ufloat_pack32",
        PixelFormat::B10G11R11_ufloat_pack32,
    ),
    (
        "E5B9G9R9_ufloat_pack32",
        PixelFormat::E5B9G9R9_ufloat_pack32,
    ),
    ("D16_unorm", PixelFormat::D16_unorm),
    ("X8_D24_unorm_pack32", PixelFormat::X8_D24_unorm_pack32),
    ("D32_sfloat", PixelFormat::D32_sfloat),
    ("S8_uint", PixelFormat::S8_uint),
    ("D16_unorm_S8_uint", PixelFormat::D16_unorm_S8_uint),
    ("D24_unorm_S8_uint", PixelFormat::D24_unorm_S8_uint),
    ("D32_sfloat_S8_uint", PixelFormat::D32_sfloat_S8_uint),
    ("BC1_RGB_unorm_block", PixelFormat::BC1_RGB_unorm_block),
    ("BC1_RGB_srgb_block", PixelFormat::BC1_RGB_srgb_block),
    ("BC1_RGBA_unorm_block", PixelFormat::BC1_RGBA_unorm_block),
    ("BC1_RGBA_srgb_block", PixelFormat::BC1_RGBA_srgb_block),
    ("BC2_unorm_block", PixelFormat::BC2_unorm_block),
    ("BC2_srgb_block", PixelFormat::BC2_srgb_block),
    ("BC3_unorm_block", PixelFormat::BC3_unorm_block),
    ("BC3_srgb_block", PixelFormat::BC3_srgb_block),
    ("BC4_unorm_block", PixelFormat::BC4_unorm_block),
    ("BC4_snorm_block", PixelFormat::BC4_snorm_block),
    ("BC5_unorm_block", PixelFormat::BC5_unorm_block),
    ("BC5_snorm_block", PixelFormat::BC5_snorm_block),
    ("BC6H_ufloat_block", PixelFormat::BC6H_ufloat_block),
    ("BC6H_sfloat_block", PixelFormat::BC6H_sfloat_block),
    ("BC7_unorm_block", PixelFormat::BC7_unorm_block),
    ("BC7_srgb_block", PixelFormat::BC7_srgb_block),
];

/// The format names `image.decode`/`image.encode` accept (mod-loader-lua env/image/format.rs).
const IMAGE_FORMATS: &[(&str, ImageFormat)] = &[
    ("png", ImageFormat::Png),
    ("jpeg", ImageFormat::Jpeg),
    ("gif", ImageFormat::Gif),
    ("webp", ImageFormat::WebP),
    ("pnm", ImageFormat::Pnm),
    ("tiff", ImageFormat::Tiff),
    ("tga", ImageFormat::Tga),
    ("dds", ImageFormat::Dds),
    ("bmp", ImageFormat::Bmp),
    ("ico", ImageFormat::Ico),
    ("hdr", ImageFormat::Hdr),
    ("openexr", ImageFormat::OpenExr),
    ("farbfeld", ImageFormat::Farbfeld),
    ("avif", ImageFormat::Avif),
    ("qoi", ImageFormat::Qoi),
    ("pcx", ImageFormat::Pcx),
];

const SIZES: &[(usize, usize)] = &[(1, 1), (7, 5), (64, 64)];

/// A deterministic RGBA image: gradients, a checker, extreme values and varying alpha.
fn rgba(width: usize, height: usize) -> Vec<u8> {
    let mut out = Vec::with_capacity(width * height * 4);
    for y in 0..height {
        for x in 0..width {
            let checker = if (x / 4 + y / 4) % 2 == 0 { 255 } else { 0 };
            out.extend_from_slice(&[
                (x * 255 / width.max(2).saturating_sub(1).max(1)) as u8,
                (y * 255 / height.max(2).saturating_sub(1).max(1)) as u8,
                checker,
                (255 - ((x + y) * 37) % 256) as u8,
            ]);
        }
    }
    out
}

/// Fixed pseudo-random bytes (64-bit LCG), the same in every build.
fn noise(len: usize, seed: u64) -> Vec<u8> {
    let mut state = seed
        .wrapping_mul(6364136223846793005)
        .wrapping_add(1442695040888963407);
    (0..len)
        .map(|_| {
            state = state
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            (state >> 56) as u8
        })
        .collect()
}

fn panic_text(p: Box<dyn std::any::Any + Send>) -> String {
    p.downcast_ref::<&str>()
        .map(|s| s.to_string())
        .or_else(|| p.downcast_ref::<String>().cloned())
        .unwrap_or_else(|| "non-text panic".into())
}

/// The run's outputs: name -> bytes (a result) or text (an error, prefixed `ERR ` / `PANIC `).
struct Outputs(Vec<(String, Vec<u8>)>);
impl Outputs {
    fn put(&mut self, name: String, result: Result<Vec<u8>, String>) {
        self.0.push(match result {
            Ok(bytes) => (name, bytes),
            Err(text) => (format!("{name}.err"), text.into_bytes()),
        });
    }
    fn write(&self, dir: &Path) {
        for (name, bytes) in &self.0 {
            let path = dir.join(name);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, bytes).unwrap();
        }
    }
}

fn guarded(f: impl FnOnce() -> Result<Vec<u8>, String>) -> Result<Vec<u8>, String> {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or_else(|p| Err(format!("PANIC {}", panic_text(p))))
}

fn texture_encode(format: PixelFormat, w: usize, h: usize, src: &[u8]) -> Result<Vec<u8>, String> {
    guarded(|| {
        let mut dst = vec![0u8; size_of_format(format, w, h)];
        encode(format, w, h, src, &mut dst).map_err(|e| format!("ERR {e}"))?;
        Ok(dst)
    })
}

fn texture_decode(format: PixelFormat, w: usize, h: usize, src: &[u8]) -> Result<Vec<u8>, String> {
    guarded(|| {
        let mut dst = vec![0u8; w * h * 4];
        decode(format, w, h, src, &mut dst).map_err(|e| format!("ERR {e}"))?;
        Ok(dst)
    })
}

/// `texture_decode` of fixed pseudo-random block bytes of the format's size (sized inside the guard: `size_of_format`
/// itself panics for `None`).
fn texture_decode_noise(
    format: PixelFormat,
    w: usize,
    h: usize,
    seed: u64,
) -> Result<Vec<u8>, String> {
    guarded(|| Ok(noise(size_of_format(format, w, h), seed)))
        .and_then(|bytes| texture_decode(format, w, h, &bytes))
}

/// `image.decode` as the Lua binding does it: decode, then to RGBA8. Written as "<w> <h>\n" + pixels.
fn image_decode(bytes: &[u8], format: Option<ImageFormat>) -> Result<Vec<u8>, String> {
    guarded(|| {
        let image = match format {
            Some(format) => image::load_from_memory_with_format(bytes, format),
            None => image::load_from_memory(bytes),
        }
        .map_err(|e| format!("ERR failed to decode image: {e}"))?
        .to_rgba8();
        let mut out = format!("{} {}\n", image.width(), image.height()).into_bytes();
        out.extend_from_slice(image.as_raw());
        Ok(out)
    })
}

/// `image.encode`: `RgbaImage::write_to` into memory.
fn image_encode(image: &RgbaImage, format: ImageFormat) -> Result<Vec<u8>, String> {
    guarded(|| {
        let mut out = Cursor::new(Vec::new());
        image
            .write_to(&mut out, format)
            .map_err(|e| format!("ERR failed to encode image: {e}"))?;
        Ok(out.into_inner())
    })
}

/// The same synthetic picture in the color type the format's encoder takes, for the formats that refuse RGBA8.
fn alternative(image: &RgbaImage, format: ImageFormat) -> Option<image::DynamicImage> {
    let dynamic = image::DynamicImage::ImageRgba8(image.clone());
    match format {
        ImageFormat::Jpeg => Some(image::DynamicImage::ImageRgb8(dynamic.to_rgb8())),
        ImageFormat::Farbfeld => Some(image::DynamicImage::ImageRgba16(dynamic.to_rgba16())),
        ImageFormat::Hdr => Some(image::DynamicImage::ImageRgb32F(dynamic.to_rgb32f())),
        ImageFormat::OpenExr => Some(image::DynamicImage::ImageRgba32F(dynamic.to_rgba32f())),
        _ => None,
    }
}

fn image_encode_dynamic(
    image: &image::DynamicImage,
    format: ImageFormat,
) -> Result<Vec<u8>, String> {
    guarded(|| {
        let mut out = Cursor::new(Vec::new());
        image
            .write_to(&mut out, format)
            .map_err(|e| format!("ERR failed to encode image: {e}"))?;
        Ok(out.into_inner())
    })
}

fn run(corpus: Option<&Path>) -> Outputs {
    let mut out = Outputs(Vec::new());
    for (i, (name, format)) in PIXEL_FORMATS.iter().enumerate() {
        for &(w, h) in SIZES {
            let case = format!("tex/{name}.{w}x{h}");
            let encoded = texture_encode(*format, w, h, &rgba(w, h));
            if let Ok(bytes) = &encoded {
                out.put(format!("{case}.dec"), texture_decode(*format, w, h, bytes));
            }
            out.put(format!("{case}.enc"), encoded);
            let seed = (i * 7 + w * 3 + h) as u64;
            out.put(
                format!("{case}.rnd.dec"),
                texture_decode_noise(*format, w, h, seed),
            );
        }
    }
    let image = RgbaImage::from_raw(32, 24, rgba(32, 24)).unwrap();
    for (name, format) in IMAGE_FORMATS {
        let encoded = image_encode(&image, *format);
        if let Ok(bytes) = &encoded {
            out.put(
                format!("img/{name}.self.dec"),
                image_decode(bytes, Some(*format)),
            );
            out.put(
                format!("img/{name}.self.guess.dec"),
                image_decode(bytes, None),
            );
        }
        out.put(format!("img/{name}.enc"), encoded);
        // Formats whose encoder refuses RGBA8 still reach `image.decode` from a mod's own files: an input in the color
        // type the encoder takes, so both builds' decoders are compared on the same bytes.
        if let Some(alt) = alternative(&image, *format) {
            let encoded = image_encode_dynamic(&alt, *format);
            if let Ok(bytes) = &encoded {
                out.put(
                    format!("img/{name}.alt.self.dec"),
                    image_decode(bytes, Some(*format)),
                );
            }
            out.put(format!("img/{name}.alt.enc"), encoded);
        }
        if let Some(corpus) = corpus {
            for kind in ["", ".alt"] {
                let path: PathBuf = corpus.join(format!("img/{name}{kind}.enc"));
                if let Ok(bytes) = fs::read(&path) {
                    out.put(
                        format!("cross/{name}{kind}.dec"),
                        image_decode(&bytes, Some(*format)),
                    );
                    out.put(
                        format!("cross/{name}{kind}.guess.dec"),
                        image_decode(&bytes, None),
                    );
                }
            }
        }
    }
    out
}

#[test]
fn texture_and_image_paths_are_deterministic_and_recorded_on_request() {
    let corpus = std::env::var_os("TEXDIFF_CORPUS").map(PathBuf::from);
    let first = run(corpus.as_deref());
    let second = run(corpus.as_deref());
    // A build that does not reproduce its own outputs cannot be compared with another build.
    assert!(
        first.0 == second.0,
        "the paths are not deterministic within one build"
    );
    assert_eq!(
        first
            .0
            .iter()
            .filter(|(n, _)| n.starts_with("tex/")
                && (n.ends_with(".rnd.dec") || n.ends_with(".rnd.dec.err")))
            .count(),
        PIXEL_FORMATS.len() * SIZES.len()
    );
    if let Some(dir) = std::env::var_os("TEXDIFF_OUT") {
        first.write(Path::new(&dir));
    }
}
