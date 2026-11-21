// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Binding shape only, both pins (sf-1.7.9 d8a394bc, sf-1.8.0 38ef448d), byte-identical:
// modules/mod-runtime/lua/bindings/image/{mod.rs:57-105, pixel_format.rs:1-170}.
//! Pure `image.decode_texture` / `image.encode_texture` over the RGBA8 Image and the Buffer.
//!
//! The codecs here are an independent MIT implementation. Uncompressed and packed layouts were
//! reconstructed from black-box differential-oracle observations of ShroudForge (exhaustive or
//! dense sweeps per format), never from the GPL codec source. Block formats use
//! block_compression 0.6.0 (MIT; Nils Hasenbanck, Intel ISPC-derived), the same crate and
//! settings both pins lock, so BC bytes match by construction and are oracle-checked.
//!
//! Observed ShroudForge behaviour this file reproduces on purpose (not standard-intent):
//! - decode fills absent R/G/B with 0 and alpha with 255; B5G6R5 leaves alpha 0; BC4/BC5/BC6H
//!   leave alpha 0 (block_compression's RGBA8 adapters); sRGB block names decode as unorm and
//!   BC4/BC5 snorm as unorm; no gamma handling;
//! - 8-bit signed components are biased (x ^ 0x80) both ways; 16-bit decode keeps the HIGH byte
//!   (biased when signed); floats decode as `(f * 255) as u8` (saturating, NaN = 0);
//! - packed 4-bit encode truncates, 5/6/1-bit encode rounds; packed decode replicates bits;
//! - A8B8G8R8_*_pack32 stores bytes A,B,G,R (R in byte 3);
//! - B10G11R11 decode reads R from bits 0-9 as 5e5m (bit 10 ignored), G from bits 11-21 as
//!   5e6m, B from bits 22-31 as a 4e6m value without inf/NaN; E5B9G9R9 decodes each channel as
//!   (1 + m/512) * 2^(e-15) with e = 31 meaning inf/NaN;
//! - multi-byte unsigned encode writes each channel at byte offset `c` (not `c*size`), as the
//!   replicated byte, later channels overwriting earlier ones;
//! - signed/float encode wider than 8 bits works only for single-channel formats and at most
//!   one texel.
//!
//! Every ShroudForge panic (decode `unwrap` of codec errors, short data, 32/64-bit
//! integer decode, partial BC blocks, wide signed/float encode, `None`, u32 wrap of the RGBA
//! length) is a Lua error here. SF's panic object has no message; ours carries one, which is the
//! only recorded difference. Decode/encode never panic or abort the process: inputs are checked
//! first and the block codec additionally runs under `catch_unwind`.
use crate::{
    eml::{
        buffer::{Buffer, ByteView},
        image::Image,
    },
    lua::{CastLua, FunctionArgs, LuaError},
};
use block_compression::{BC6HSettings, BC7Settings, CompressionVariant};
use mlua::Value;
use std::{
    borrow::Cow,
    panic::{AssertUnwindSafe, catch_unwind},
};

macro_rules! pixel_formats {
    ($($name:ident),+ $(,)?) => {
        /// Every name both pins' pixel_format.rs accepts, in source order (exact, case-sensitive).
        #[allow(non_camel_case_types)]
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        pub enum PixelFormat { $($name),+ }
        impl PixelFormat {
            pub const ALL: &'static [PixelFormat] = &[$(PixelFormat::$name),+];
            pub fn name(self) -> &'static str {
                match self { $(PixelFormat::$name => stringify!($name)),+ }
            }
            pub fn from_name(name: &[u8]) -> Option<Self> {
                match name { $(n if n == stringify!($name).as_bytes() => Some(PixelFormat::$name),)+ _ => None }
            }
        }
    };
}
pixel_formats!(
    None,
    R4G4_unorm_pack8,
    R4G4B4A4_unorm_pack16,
    B4G4R4A4_unorm_pack16,
    R5G6B5_unorm_pack16,
    B5G6R5_unorm_pack16,
    R5G5B5A1_unorm_pack16,
    B5G5R5A1_unorm_pack16,
    A1R5G5B5_unorm_pack16,
    R8_unorm,
    R8_snorm,
    R8_uscaled,
    R8_sscaled,
    R8_uint,
    R8_sint,
    R8_srgb,
    R8G8_unorm,
    R8G8_snorm,
    R8G8_uscaled,
    R8G8_sscaled,
    R8G8_uint,
    R8G8_sint,
    R8G8_srgb,
    R8G8B8_unorm,
    R8G8B8_snorm,
    R8G8B8_uscaled,
    R8G8B8_sscaled,
    R8G8B8_uint,
    R8G8B8_sint,
    R8G8B8_srgb,
    B8G8R8_unorm,
    B8G8R8_snorm,
    B8G8R8_uscaled,
    B8G8R8_sscaled,
    B8G8R8_uint,
    B8G8R8_sint,
    B8G8R8_srgb,
    R8G8B8A8_unorm,
    R8G8B8A8_snorm,
    R8G8B8A8_uscaled,
    R8G8B8A8_sscaled,
    R8G8B8A8_uint,
    R8G8B8A8_sint,
    R8G8B8A8_srgb,
    B8G8R8A8_unorm,
    B8G8R8A8_snorm,
    B8G8R8A8_uscaled,
    B8G8R8A8_sscaled,
    B8G8R8A8_uint,
    B8G8R8A8_sint,
    B8G8R8A8_srgb,
    A8B8G8R8_unorm_pack32,
    A8B8G8R8_snorm_pack32,
    A8B8G8R8_uscaled_pack32,
    A8B8G8R8_sscaled_pack32,
    A8B8G8R8_uint_pack32,
    A8B8G8R8_sint_pack32,
    A8B8G8R8_srgb_pack32,
    A2R10G10B10_unorm_pack32,
    A2R10G10B10_snorm_pack32,
    A2R10G10B10_uscaled_pack32,
    A2R10G10B10_sscaled_pack32,
    A2R10G10B10_uint_pack32,
    A2R10G10B10_sint_pack32,
    A2B10G10R10_unorm_pack32,
    A2B10G10R10_snorm_pack32,
    A2B10G10R10_uscaled_pack32,
    A2B10G10R10_sscaled_pack32,
    A2B10G10R10_uint_pack32,
    A2B10G10R10_sint_pack32,
    R16_unorm,
    R16_snorm,
    R16_uscaled,
    R16_sscaled,
    R16_uint,
    R16_sint,
    R16_sfloat,
    R16G16_unorm,
    R16G16_snorm,
    R16G16_uscaled,
    R16G16_sscaled,
    R16G16_uint,
    R16G16_sint,
    R16G16_sfloat,
    R16G16B16_unorm,
    R16G16B16_snorm,
    R16G16B16_uscaled,
    R16G16B16_sscaled,
    R16G16B16_uint,
    R16G16B16_sint,
    R16G16B16_sfloat,
    R16G16B16A16_unorm,
    R16G16B16A16_snorm,
    R16G16B16A16_uscaled,
    R16G16B16A16_sscaled,
    R16G16B16A16_uint,
    R16G16B16A16_sint,
    R16G16B16A16_sfloat,
    R32_uint,
    R32_sint,
    R32_sfloat,
    R32G32_uint,
    R32G32_sint,
    R32G32_sfloat,
    R32G32B32_uint,
    R32G32B32_sint,
    R32G32B32_sfloat,
    R32G32B32A32_uint,
    R32G32B32A32_sint,
    R32G32B32A32_sfloat,
    R64_uint,
    R64_sint,
    R64_sfloat,
    R64G64_uint,
    R64G64_sint,
    R64G64_sfloat,
    R64G64B64_uint,
    R64G64B64_sint,
    R64G64B64_sfloat,
    R64G64B64A64_uint,
    R64G64B64A64_sint,
    R64G64B64A64_sfloat,
    B10G11R11_ufloat_pack32,
    E5B9G9R9_ufloat_pack32,
    D16_unorm,
    X8_D24_unorm_pack32,
    D32_sfloat,
    S8_uint,
    D16_unorm_S8_uint,
    D24_unorm_S8_uint,
    D32_sfloat_S8_uint,
    BC1_RGB_unorm_block,
    BC1_RGB_srgb_block,
    BC1_RGBA_unorm_block,
    BC1_RGBA_srgb_block,
    BC2_unorm_block,
    BC2_srgb_block,
    BC3_unorm_block,
    BC3_srgb_block,
    BC4_unorm_block,
    BC4_snorm_block,
    BC5_unorm_block,
    BC5_snorm_block,
    BC6H_ufloat_block,
    BC6H_sfloat_block,
    BC7_unorm_block,
    BC7_srgb_block,
);

impl CastLua<'_> for PixelFormat {
    type Output = Self;
    fn try_cast_lua(value: &Value) -> mlua::Result<Option<Self>> {
        // pixel_format.rs: strings only, invalid UTF-8 and unknown names are not this type.
        let Value::String(s) = value else {
            return Ok(None);
        };
        Ok(Self::from_name(&s.as_bytes()))
    }
    fn name() -> Cow<'static, str> {
        "a valid keen::PixelFormat variant".into()
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    Unsigned,
    Signed,
    Float,
}
#[derive(Clone, Copy)]
enum Codec {
    /// Rejected before any size/data check, even for empty images.
    Refused(Refusal),
    /// (bytes per texel, [(channel, bits, shift)]) of a little-endian packed word.
    Packed(usize, &'static [(usize, u32, u32)]),
    /// Byte i of each component group holds RGBA channel `order[i]`; `size` bytes per component.
    Plain(&'static [usize], usize, Kind),
    B10G11R11,
    E5B9G9R9,
    Block(Bc),
}
#[derive(Clone, Copy)]
enum Refusal {
    None,
    Srgb,
    Packed10,
    Opaque,
    Bc6hSigned,
}
#[derive(Clone, Copy)]
enum Bc {
    One,
    Two,
    Three,
    Four,
    Five,
    SixH,
    Seven,
}
impl Bc {
    fn variant(self) -> CompressionVariant {
        match self {
            Bc::One => CompressionVariant::BC1,
            Bc::Two => CompressionVariant::BC2,
            Bc::Three => CompressionVariant::BC3,
            Bc::Four => CompressionVariant::BC4,
            Bc::Five => CompressionVariant::BC5,
            Bc::SixH => CompressionVariant::BC6H(BC6HSettings::basic()),
            Bc::Seven => CompressionVariant::BC7(BC7Settings::alpha_basic()),
        }
    }
    fn block_bytes(self) -> usize {
        match self {
            Bc::One | Bc::Four => 8,
            _ => 16,
        }
    }
}

const R: &[usize] = &[0];
const RG: &[usize] = &[0, 1];
const RGB: &[usize] = &[0, 1, 2];
const BGR: &[usize] = &[2, 1, 0];
const RGBA: &[usize] = &[0, 1, 2, 3];
const BGRA: &[usize] = &[2, 1, 0, 3];
const ABGR: &[usize] = &[3, 2, 1, 0];

impl PixelFormat {
    fn codec(self) -> Codec {
        use Codec::*;
        use Kind::*;
        use PixelFormat as F;
        let name = self.name();
        let kind = if name.contains("_sfloat") {
            Float
        } else if name.contains("_snorm") || name.contains("_sscaled") || name.contains("_sint") {
            Signed
        } else {
            Unsigned
        };
        match self {
            F::None => Refused(Refusal::None),
            F::R4G4_unorm_pack8 => Packed(1, &[(0, 4, 4), (1, 4, 0)]),
            F::R4G4B4A4_unorm_pack16 => Packed(2, &[(0, 4, 12), (1, 4, 8), (2, 4, 4), (3, 4, 0)]),
            F::B4G4R4A4_unorm_pack16 => Packed(2, &[(2, 4, 12), (1, 4, 8), (0, 4, 4), (3, 4, 0)]),
            F::R5G6B5_unorm_pack16 => Packed(2, &[(0, 5, 11), (1, 6, 5), (2, 5, 0)]),
            F::B5G6R5_unorm_pack16 => Packed(2, &[(2, 5, 11), (1, 6, 5), (0, 5, 0)]),
            F::R5G5B5A1_unorm_pack16 => Packed(2, &[(0, 5, 11), (1, 5, 6), (2, 5, 1), (3, 1, 0)]),
            F::B5G5R5A1_unorm_pack16 => Packed(2, &[(2, 5, 11), (1, 5, 6), (0, 5, 1), (3, 1, 0)]),
            F::A1R5G5B5_unorm_pack16 => Packed(2, &[(3, 1, 15), (0, 5, 10), (1, 5, 5), (2, 5, 0)]),
            F::B10G11R11_ufloat_pack32 => B10G11R11,
            F::E5B9G9R9_ufloat_pack32 => E5B9G9R9,
            F::BC1_RGB_unorm_block
            | F::BC1_RGB_srgb_block
            | F::BC1_RGBA_unorm_block
            | F::BC1_RGBA_srgb_block => Block(Bc::One),
            F::BC2_unorm_block | F::BC2_srgb_block => Block(Bc::Two),
            F::BC3_unorm_block | F::BC3_srgb_block => Block(Bc::Three),
            F::BC4_unorm_block | F::BC4_snorm_block => Block(Bc::Four),
            F::BC5_unorm_block | F::BC5_snorm_block => Block(Bc::Five),
            F::BC6H_ufloat_block => Block(Bc::SixH),
            F::BC6H_sfloat_block => Refused(Refusal::Bc6hSigned),
            F::BC7_unorm_block | F::BC7_srgb_block => Block(Bc::Seven),
            _ if name.starts_with("A2") => Refused(Refusal::Packed10),
            _ if name.starts_with('D') || name.starts_with("S8") || name.starts_with("X8") => {
                Refused(Refusal::Opaque)
            }
            _ if name.contains("_srgb") => Refused(Refusal::Srgb),
            _ => {
                let (order, rest) = [
                    ("A8B8G8R8_", ABGR),
                    ("R8G8B8A8_", RGBA),
                    ("B8G8R8A8_", BGRA),
                    ("R16G16B16A16_", RGBA),
                    ("R32G32B32A32_", RGBA),
                    ("R64G64B64A64_", RGBA),
                    ("R8G8B8_", RGB),
                    ("B8G8R8_", BGR),
                    ("R16G16B16_", RGB),
                    ("R32G32B32_", RGB),
                    ("R64G64B64_", RGB),
                    ("R8G8_", RG),
                    ("R16G16_", RG),
                    ("R32G32_", RG),
                    ("R64G64_", RG),
                    ("R8_", R),
                    ("R16_", R),
                    ("R32_", R),
                    ("R64_", R),
                ]
                .into_iter()
                .find_map(|(prefix, order)| name.strip_prefix(prefix).map(|_| (order, prefix)))
                .expect("every remaining accepted name is a plain component format");
                let size = if rest.contains("64") {
                    8
                } else if rest.contains("32") {
                    4
                } else if rest.contains("16") {
                    2
                } else {
                    1
                };
                Plain(order, size, kind)
            }
        }
    }
    fn refusal_text(self, refusal: Refusal, operation: &str) -> String {
        match refusal {
            Refusal::None => "Unsupported pixel format: None".into(),
            Refusal::Opaque => format!("Opaque pixel format: {}", self.name()),
            Refusal::Packed10 => format!("A2B10G10R10_*_pack32 {operation} not implemented"),
            Refusal::Srgb | Refusal::Bc6hSigned => {
                format!("{} {operation} not implemented", self.name())
            }
        }
    }
}

fn texels(width: u32, height: u32) -> usize {
    // u32*u32 always fits u64 (and usize on the 64-bit targets Kiln ships).
    (u64::from(width) * u64::from(height)) as usize
}
fn checked_bytes(texels: usize, per: usize) -> Result<usize, String> {
    texels
        .checked_mul(per)
        .ok_or_else(|| "texture dimensions overflow".into())
}
fn zeroed(len: usize) -> Result<Vec<u8>, String> {
    let mut bytes = Vec::new();
    bytes
        .try_reserve_exact(len)
        .map_err(|_| format!("cannot allocate {len} bytes"))?;
    bytes.resize(len, 0);
    Ok(bytes)
}
fn guarded<T>(f: impl FnOnce() -> T) -> Result<T, String> {
    catch_unwind(AssertUnwindSafe(f)).map_err(|_| "block codec panicked".into())
}
fn need(data: &[u8], required: usize) -> Result<&[u8], String> {
    data.get(..required).ok_or_else(|| {
        format!(
            "texture data too short: expected {required} bytes, got {}",
            data.len()
        )
    })
}

/// Bit replication of an n-bit field to 8 bits (SF packed decode, oracle-exhaustive).
fn expand(value: u32, bits: u32) -> u8 {
    if bits == 1 {
        return if value != 0 { 255 } else { 0 };
    }
    let mut out = value << (8 - bits);
    let mut filled = bits;
    while filled < 8 {
        out |= out >> filled;
        filled *= 2;
    }
    out as u8
}
/// 8-bit to n-bit: 4-bit truncates, 5/6/1-bit round to nearest (SF encode, oracle-exhaustive).
fn quantize(value: u8, bits: u32) -> u32 {
    let value = u32::from(value);
    if bits == 4 {
        value >> 4
    } else {
        let max = (1 << bits) - 1;
        (value * max + 127) / 255
    }
}
fn unsigned_float(field: u32, mantissa_bits: u32, exponent_bits: u32, special: bool) -> f32 {
    let exponent = field >> mantissa_bits;
    let mantissa = field & ((1 << mantissa_bits) - 1);
    let scale = (1u32 << mantissa_bits) as f32;
    if special && exponent == (1 << exponent_bits) - 1 {
        return if mantissa == 0 {
            f32::INFINITY
        } else {
            f32::NAN
        };
    }
    if exponent == 0 {
        mantissa as f32 / scale * 2f32.powi(-14)
    } else {
        (1.0 + mantissa as f32 / scale) * 2f32.powi(exponent as i32 - 15)
    }
}
fn unit_f32(value: f32) -> u8 {
    (value * 255.0) as u8
}

/// Decoded RGBA8 bytes, or the cause of SF's (panicking) failure.
pub fn decode(
    format: PixelFormat,
    width: u32,
    height: u32,
    data: &[u8],
) -> Result<Vec<u8>, String> {
    let codec = format.codec();
    if let Codec::Refused(refusal) = codec {
        return Err(format.refusal_text(refusal, "decoding"));
    }
    let count = texels(width, height);
    // SF allocates `width * height * 4` in u32 arithmetic: any wrap leaves a wrongly sized
    // destination and panics before a result exists.
    let out_len = count
        .checked_mul(4)
        .filter(|&len| len <= u32::MAX as usize)
        .ok_or_else(|| "texture dimensions overflow".to_string())?;
    // Every input check runs before the destination exists, so a few bytes of Lua data never
    // make sfi allocate and zero up to 4 GiB first.
    let data = match codec {
        Codec::Refused(_) => unreachable!(),
        Codec::Packed(bytes, _) => need(data, checked_bytes(count, bytes)?)?,
        Codec::Plain(order, size, kind) => {
            let data = need(data, checked_bytes(count, order.len() * size)?)?;
            if count > 0 && size >= 4 && kind != Kind::Float {
                return Err(format!(
                    "{} decoding is not supported: ShroudForge's 32/64-bit integer decode fails for every non-empty image",
                    format.name()
                ));
            }
            data
        }
        Codec::B10G11R11 | Codec::E5B9G9R9 => need(data, checked_bytes(count, 4)?)?,
        Codec::Block(bc) => {
            // block_compression writes whole 4x4 blocks: any partial edge block overruns the
            // RGBA8 destination in SF (oracle: every unaligned non-empty size panics).
            if count > 0 && (!width.is_multiple_of(4) || !height.is_multiple_of(4)) {
                return Err(format!(
                    "{} decoding requires dimensions that are multiples of 4, got {width}x{height}",
                    format.name()
                ));
            }
            let blocks = checked_bytes(
                texels(width.div_ceil(4), height.div_ceil(4)),
                bc.block_bytes(),
            )?;
            need(data, blocks)?
        }
    };
    let mut out = zeroed(out_len)?;
    match codec {
        Codec::Refused(_) => unreachable!(),
        Codec::Packed(bytes, fields) => {
            for (texel, px) in data
                .chunks_exact(bytes)
                .zip(out.as_chunks_mut::<4>().0.iter_mut())
            {
                let word = texel
                    .iter()
                    .rev()
                    .fold(0u32, |acc, &b| (acc << 8) | u32::from(b));
                px[3] = if format == PixelFormat::B5G6R5_unorm_pack16 {
                    0
                } else {
                    255
                };
                for &(channel, bits, shift) in fields {
                    px[channel] = expand((word >> shift) & ((1 << bits) - 1), bits);
                }
            }
        }
        Codec::Plain(order, size, kind) => {
            let group = order.len() * size;
            for (texel, px) in data
                .chunks_exact(group)
                .zip(out.as_chunks_mut::<4>().0.iter_mut())
            {
                px.copy_from_slice(&[0, 0, 0, 255]);
                for (i, &channel) in order.iter().enumerate() {
                    let c = &texel[i * size..(i + 1) * size];
                    px[channel] = match (kind, size) {
                        (Kind::Float, 2) => {
                            unit_f32(half::f16::from_le_bytes([c[0], c[1]]).to_f32())
                        }
                        (Kind::Float, 4) => unit_f32(f32::from_le_bytes(c.try_into().unwrap())),
                        (Kind::Float, _) => {
                            (f64::from_le_bytes(c.try_into().unwrap()) * 255.0) as u8
                        }
                        (Kind::Signed, _) => c[size - 1] ^ 0x80,
                        (Kind::Unsigned, _) => c[size - 1],
                    };
                }
            }
        }
        Codec::B10G11R11 | Codec::E5B9G9R9 => {
            for (texel, px) in data
                .as_chunks::<4>()
                .0
                .iter()
                .zip(out.as_chunks_mut::<4>().0.iter_mut())
            {
                let v = u32::from_le_bytes(*texel);
                let rgb = if matches!(codec, Codec::B10G11R11) {
                    [
                        unsigned_float(v & 0x3ff, 5, 5, true),
                        unsigned_float((v >> 11) & 0x7ff, 6, 5, true),
                        unsigned_float(v >> 22, 6, 4, false),
                    ]
                } else {
                    let exponent = v >> 27;
                    [0, 9, 18].map(|shift| {
                        let mantissa = (v >> shift) & 0x1ff;
                        if exponent == 31 {
                            if mantissa == 0 {
                                f32::INFINITY
                            } else {
                                f32::NAN
                            }
                        } else {
                            (1.0 + mantissa as f32 / 512.0) * 2f32.powi(exponent as i32 - 15)
                        }
                    })
                };
                px.copy_from_slice(&[unit_f32(rgb[0]), unit_f32(rgb[1]), unit_f32(rgb[2]), 255]);
            }
        }
        Codec::Block(bc) => {
            if count > 0 {
                guarded(|| {
                    block_compression::decode::decompress_blocks_as_rgba8(
                        bc.variant(),
                        width,
                        height,
                        data,
                        &mut out,
                    )
                })?;
            }
        }
    }
    Ok(out)
}

/// SF `size_of_format(format, width, height)` for the formats that reach the encoder.
fn encoded_len(codec: Codec, width: u32, height: u32) -> Result<usize, String> {
    match codec {
        Codec::Packed(bytes, _) => checked_bytes(texels(width, height), bytes),
        Codec::Plain(order, size, _) => checked_bytes(texels(width, height), order.len() * size),
        Codec::B10G11R11 | Codec::E5B9G9R9 => checked_bytes(texels(width, height), 4),
        Codec::Block(bc) => checked_bytes(
            texels(width.div_ceil(4), height.div_ceil(4)),
            bc.block_bytes(),
        ),
        Codec::Refused(_) => Ok(0),
    }
}

/// Encoded bytes. `Err(Ok(text))` is SF's returned error (exact text); `Err(Err(text))` is a
/// path where SF panics, raised as a Lua error instead.
pub fn encode(
    format: PixelFormat,
    width: u32,
    height: u32,
    rgba: &[u8],
) -> Result<Vec<u8>, Result<String, String>> {
    let codec = format.codec();
    match codec {
        // SF panics in size_of_format before encoding; the contract's cause text is reused.
        Codec::Refused(Refusal::None) => {
            return Err(Err(format.refusal_text(Refusal::None, "encoding")));
        }
        Codec::Refused(refusal) => return Err(Ok(format.refusal_text(refusal, "encoding"))),
        Codec::B10G11R11 | Codec::E5B9G9R9 => {
            return Err(Ok("E5B9G9R9_ufloat_pack32 encoding not implemented".into()));
        }
        _ => {}
    }
    let count = texels(width, height);
    // The panic-path refusals run before the output exists (no allocation for a refused call).
    match codec {
        Codec::Plain(order, size, kind)
            if size > 1
                && kind != Kind::Unsigned
                && count > 0
                && (order.len() > 1 || count > 1) =>
        {
            return Err(Err(format!(
                "{} encoding supports only one single-channel texel in ShroudForge, got {width}x{height}",
                format.name()
            )));
        }
        Codec::Block(_) if !width.is_multiple_of(4) || !height.is_multiple_of(4) => {
            return Err(Err(format!(
                "{} encoding requires dimensions that are multiples of 4, got {width}x{height}",
                format.name()
            )));
        }
        _ => {}
    }
    let mut out = zeroed(encoded_len(codec, width, height).map_err(Err)?).map_err(Err)?;
    debug_assert_eq!(rgba.len(), count * 4);
    match codec {
        Codec::Packed(bytes, fields) => {
            for (px, texel) in rgba
                .as_chunks::<4>()
                .0
                .iter()
                .zip(out.chunks_exact_mut(bytes))
            {
                let word = fields.iter().fold(0u32, |word, &(channel, bits, shift)| {
                    word | (quantize(px[channel], bits) << shift)
                });
                texel.copy_from_slice(&word.to_le_bytes()[..bytes]);
            }
        }
        Codec::Plain(order, 1, kind) => {
            let bias = if kind == Kind::Signed { 0x80 } else { 0 };
            for (px, texel) in rgba
                .as_chunks::<4>()
                .0
                .iter()
                .zip(out.chunks_exact_mut(order.len()))
            {
                for (byte, &channel) in texel.iter_mut().zip(order) {
                    *byte = px[channel] ^ bias;
                }
            }
        }
        Codec::Plain(order, size, Kind::Unsigned) => {
            let group = order.len() * size;
            for (px, texel) in rgba
                .as_chunks::<4>()
                .0
                .iter()
                .zip(out.chunks_exact_mut(group))
            {
                for (offset, &channel) in order.iter().enumerate() {
                    texel[offset..offset + size].fill(px[channel]);
                }
            }
        }
        Codec::Plain(_, size, kind) => {
            if let Some(px) = rgba.as_chunks::<4>().0.iter().next() {
                let x = px[0];
                match (kind, size) {
                    (Kind::Signed, _) => {
                        out[0] = x;
                        out[size - 1] |= 0x80;
                    }
                    (_, 2) => out
                        .copy_from_slice(&half::f16::from_f32(f32::from(x) / 255.0).to_le_bytes()),
                    (_, 4) => out.copy_from_slice(&(f32::from(x) / 255.0).to_le_bytes()),
                    _ => out.copy_from_slice(&(f64::from(x) / 255.0).to_le_bytes()),
                }
            }
        }
        Codec::Block(bc) => {
            if count > 0 {
                let stride = width
                    .checked_mul(4)
                    .ok_or_else(|| Err("texture row overflow".into()))?;
                guarded(|| {
                    block_compression::encode::compress_rgba8(
                        bc.variant(),
                        rgba,
                        &mut out,
                        width,
                        height,
                        stride,
                    )
                })
                .map_err(Err)?;
            }
        }
        Codec::Refused(_) | Codec::B10G11R11 | Codec::E5B9G9R9 => unreachable!(),
    }
    Ok(out)
}

fn lua_decode_texture(_: &mlua::Lua, args: FunctionArgs) -> mlua::Result<Image> {
    let view = args.get::<ByteView>(0)?;
    // SF reads the Buffer state before converting the remaining operands.
    let bytes = view.as_bytes()?;
    let format = args.get::<PixelFormat>(1)?;
    let width = args.get::<u32>(2)?;
    let height = args.get::<u32>(3)?;
    // Converted (so its type is checked) and then ignored, as in both pins.
    let _mip_level = args.get::<Option<u32>>(4)?.unwrap_or(1);
    let rgba = decode(format, width, height, bytes)
        .map_err(|cause| LuaError::generic(format!("failed to decode texture: {cause}")))?;
    Image::from_rgba8(rgba, width, height)
}
fn lua_encode_texture(_: &mlua::Lua, args: FunctionArgs) -> mlua::Result<Buffer> {
    let image = args.get::<&Image>(0)?;
    let format = args.get::<PixelFormat>(1)?;
    let encoded = encode(format, image.width(), image.height(), image.rgba8()).map_err(
        |cause| match cause {
            Ok(text) | Err(text) => LuaError::generic(format!("failed to encode image: {text}")),
        },
    )?;
    Ok(Buffer::wrap(encoded))
}
/// The two texture entries in SF's registration order (mod.rs:21-26); callers interleave
/// them with the Image's ordinary codecs on the ONE image table.
pub fn decode_texture_function(lua: &mlua::Lua) -> mlua::Result<mlua::Function> {
    lua.create_function(lua_decode_texture)
}
pub fn encode_texture_function(lua: &mlua::Lua) -> mlua::Result<mlua::Function> {
    lua.create_function(lua_encode_texture)
}
