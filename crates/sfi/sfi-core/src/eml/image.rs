// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Both pins (sf-1.7.9 d8a394bc, sf-1.8.0 38ef448d), byte-identical:
// modules/mod-runtime/lua/bindings/image/{mod.rs:18-55,75-89,
// image.rs:6-149,image_format.rs:1-45}.
//! Owned RGBA8 Image and ordinary image-rs codecs, without any engine/texture types.
//!
//! The texture codecs (`eml::texture`) use the data-only boundary `Image::from_rgba8`, `width`, `height`, `rgba8`.
//! The constructor requires exactly width*height*4 bytes; dimensions and storage
//! cannot be changed independently. The borrowed byte view cannot outlive Image
//! (or its mlua borrow guard). Pixel methods are the only mutable Lua access.
//! `install` returns the ONE per-package namespace table for texture registration;
//! extensions add their functions to that table, never replace Image or its alias.
//!
//! Parity notes: packed colors are 0xAABBGGRR, not the documented 0xRRGGBBAA
//! (both pins image.rs:103-121). Binary strings decode as well as Buffers
//! (mod.rs:38-42). Getter-only assignments raise Lua's "attempt to index a Image
//! value", NOT the unknown-field setter error asserted by FINAL: neither pin
//! registers any setter/__newindex, so mlua does not generate __mlua_newindex
//! (mlua 0.11.6 userdata/util.rs:283-298; two-pin v2 image fixture).
//! Zero dimensions are valid. Arithmetic/allocation
//! failures and dependency panics raise Lua errors; zero-dimension setter bounds
//! retain SF release's wrapping usize maximum instead of a debug subtraction panic
//! (image.rs:129-144). No added memory quota; the decoder retains image-rs defaults.
//!
//! ```
//! use sfi_core::eml::image::Image;
//! let image = Image::from_rgba8(vec![1, 2, 3, 4], 1, 1)?;
//! assert_eq!((image.width(), image.height()), (1, 1));
//! assert_eq!(image.rgba8(), &[1, 2, 3, 4]);
//! # Ok::<(), mlua::Error>(())
//! ```
//!
//! The byte view cannot be mutated or resized independently of the dimensions.
//! ```compile_fail
//! use sfi_core::eml::image::Image;
//! let image = Image::new(1, 1).unwrap();
//! image.rgba8()[0] = 9;
//! ```
use crate::{
    eml::buffer::{Buffer, ByteView},
    lua::{CastLua, FunctionArgs, LuaError, MethodArgs, install_namespace},
};
use image::{ImageFormat, Rgba, RgbaImage};
use mlua::{IntoLuaMulti, UserData, Value};
use std::{
    borrow::Cow,
    io::Cursor,
    panic::{AssertUnwindSafe, catch_unwind},
};

pub struct Image {
    inner: RgbaImage,
}
fn dimensions_error() -> mlua::Error {
    LuaError::generic("failed to create image: dimensions exceed maximum possible size")
}
fn rgba8_len(width: u32, height: u32) -> mlua::Result<usize> {
    (width as usize)
        .checked_mul(height as usize)
        .and_then(|pixels| pixels.checked_mul(4))
        .filter(|&bytes| bytes <= isize::MAX as usize)
        .ok_or_else(dimensions_error)
}
impl Image {
    /// Checked, fallibly allocated transparent-black RGBA8 storage. No size quota.
    pub fn new(width: u32, height: u32) -> mlua::Result<Self> {
        let len = rgba8_len(width, height)?;
        let mut bytes = Vec::new();
        bytes
            .try_reserve_exact(len)
            .map_err(|_| dimensions_error())?;
        bytes.resize(len, 0);
        Self::from_rgba8(bytes, width, height)
    }
    /// Data-only boundary for independent texture codecs. Requires exact RGBA8 length.
    pub fn from_rgba8(bytes: Vec<u8>, width: u32, height: u32) -> mlua::Result<Self> {
        if bytes.len() != rgba8_len(width, height)? {
            return Err(LuaError::generic("failed to create image"));
        }
        RgbaImage::from_vec(width, height, bytes)
            .map(|inner| Self { inner })
            .ok_or_else(|| LuaError::generic("failed to create image"))
    }
    pub fn width(&self) -> u32 {
        self.inner.width()
    }
    pub fn height(&self) -> u32 {
        self.inner.height()
    }
    pub fn rgba8(&self) -> &[u8] {
        self.inner.as_raw()
    }

    fn check_coordinates(&self, x: u32, y: u32) -> mlua::Result<()> {
        for (name, value, bound) in [("x", x, self.width()), ("y", y, self.height())] {
            if value >= bound {
                return Err(LuaError::position_out_of_bounds_named(
                    name,
                    value as usize,
                    0,
                    (bound as usize).wrapping_sub(1),
                ));
            }
        }
        Ok(())
    }
}
impl UserData for Image {
    fn add_fields<F: mlua::UserDataFields<Self>>(fields: &mut F) {
        fields.add_field_method_get("width", |_, this| Ok(this.width()));
        fields.add_field_method_get("height", |_, this| Ok(this.height()));
    }
    fn add_methods<M: mlua::UserDataMethods<Self>>(methods: &mut M) {
        methods.add_function("get_pixel", get_pixel);
        methods.add_function("set_pixel", set_pixel);
        methods.add_function("get_pixel_packed", get_pixel_packed);
        methods.add_function("set_pixel_packed", set_pixel_packed);
    }
}
fn get_pixel(lua: &mlua::Lua, args: MethodArgs) -> mlua::Result<mlua::MultiValue> {
    let this = args.this::<&Image>()?;
    let x = args.get::<u32>(0)?;
    let y = args.get::<u32>(1)?;
    if x >= this.width() || y >= this.height() {
        return Value::Nil.into_lua_multi(lua);
    }
    let [r, g, b, a] = this.inner.get_pixel(x, y).0;
    (r, g, b, a).into_lua_multi(lua)
}
fn set_pixel(_: &mlua::Lua, args: MethodArgs) -> mlua::Result<()> {
    let mut this = args.this::<&mut Image>()?;
    let x = args.get::<u32>(0)?;
    let y = args.get::<u32>(1)?;
    let r = args.get::<u8>(2)?;
    let g = args.get::<u8>(3)?;
    let b = args.get::<u8>(4)?;
    let a = args.get::<u8>(5)?;
    this.check_coordinates(x, y)?;
    this.inner.put_pixel(x, y, Rgba([r, g, b, a]));
    Ok(())
}
fn get_pixel_packed(_: &mlua::Lua, args: MethodArgs) -> mlua::Result<Value> {
    let this = args.this::<&Image>()?;
    let x = args.get::<u32>(0)?;
    let y = args.get::<u32>(1)?;
    if x >= this.width() || y >= this.height() {
        return Ok(Value::Nil);
    }
    Ok(Value::Integer(i64::from(u32::from_le_bytes(
        this.inner.get_pixel(x, y).0,
    ))))
}
fn set_pixel_packed(_: &mlua::Lua, args: MethodArgs) -> mlua::Result<()> {
    let mut this = args.this::<&mut Image>()?;
    let x = args.get::<u32>(0)?;
    let y = args.get::<u32>(1)?;
    let color = args.get::<u32>(2)?;
    this.check_coordinates(x, y)?;
    this.inner.put_pixel(x, y, Rgba(color.to_le_bytes()));
    Ok(())
}
// Local wrapper is solely the ordinary format conversion; `eml::texture` owns PixelFormat.
struct Format(ImageFormat);
impl CastLua<'_> for Format {
    type Output = Self;
    #[allow(deprecated)]
    fn try_cast_lua(value: &Value) -> mlua::Result<Option<Self>> {
        let Value::String(s) = value else {
            return Ok(None);
        };
        Ok(Some(Self(match s.as_bytes().as_ref() {
            b"png" => ImageFormat::Png,
            b"jpeg" | b"jpg" => ImageFormat::Jpeg,
            b"gif" => ImageFormat::Gif,
            b"webp" => ImageFormat::WebP,
            b"pnm" => ImageFormat::Pnm,
            b"tiff" => ImageFormat::Tiff,
            b"tga" => ImageFormat::Tga,
            b"dds" => ImageFormat::Dds,
            b"bmp" => ImageFormat::Bmp,
            b"ico" => ImageFormat::Ico,
            b"hdr" => ImageFormat::Hdr,
            b"openexr" => ImageFormat::OpenExr,
            b"farbfeld" => ImageFormat::Farbfeld,
            b"avif" => ImageFormat::Avif,
            b"qoi" => ImageFormat::Qoi,
            b"pcx" => ImageFormat::Pcx,
            _ => return Ok(None),
        })))
    }
    fn name() -> Cow<'static, str> {
        "'png', 'jpeg', 'gif', 'webp', 'pnm', 'tiff', 'tga', 'dds', 'bmp', 'ico', 'hdr', 'openexr', 'farbfeld', 'avif', 'qoi' or 'pcx'".into()
    }
}
fn codec<T>(operation: &str, f: impl FnOnce() -> mlua::Result<T>) -> mlua::Result<T> {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or_else(|_| {
        Err(LuaError::generic(format!(
            "failed to {operation} image: codec panicked"
        )))
    })
}
fn decode(_: &mlua::Lua, args: FunctionArgs) -> mlua::Result<Image> {
    let view = args.get::<ByteView>(0)?;
    // SF checks the Buffer state BEFORE converting the optional format.
    let bytes = view.as_bytes()?;
    let format = args.get::<Option<Format>>(1)?;
    codec("decode", || {
        let decoded = match format {
            Some(Format(f)) => image::load_from_memory_with_format(bytes, f),
            None => image::load_from_memory(bytes),
        }
        .map_err(|e| LuaError::generic(format!("failed to decode image: {e}")))?;
        rgba8_len(decoded.width(), decoded.height())?;
        // Same pinned conversion (including color-space handling) as SF, not a
        // per-pixel approximation. Reuse already-RGBA8 storage where possible.
        let inner = decoded.into_rgba8();
        Ok(Image { inner })
    })
}
fn encode(_: &mlua::Lua, args: FunctionArgs) -> mlua::Result<Buffer> {
    let image = args.get::<&Image>(0)?;
    let format = args
        .get::<Option<Format>>(1)?
        .map_or(ImageFormat::Png, |f| f.0);
    codec("encode", || {
        let mut encoded = Cursor::new(Vec::new());
        // RgbaImage, NOT DynamicImage: unsupported RGBA8 color types must fail.
        image
            .inner
            .write_to(&mut encoded, format)
            .map_err(|e| LuaError::generic(format!("failed to encode image: {e}")))?;
        Ok(Buffer::wrap(encoded.into_inner()))
    })
}
pub fn create(lua: &mlua::Lua) -> mlua::Result<mlua::Table> {
    let table = lua.create_table()?;
    table.raw_set(
        "create",
        lua.create_function(|_, args: FunctionArgs| {
            let width = args.get::<u32>(0)?;
            let height = args.get::<u32>(1)?;
            Image::new(width, height)
        })?,
    )?;
    // SF's order (mod.rs:21-26): create, decode, decode_texture, encode, encode_texture.
    table.raw_set("decode", lua.create_function(decode)?)?;
    table.raw_set(
        "decode_texture",
        crate::eml::texture::decode_texture_function(lua)?,
    )?;
    table.raw_set("encode", lua.create_function(encode)?)?;
    table.raw_set(
        "encode_texture",
        crate::eml::texture::encode_texture_function(lua)?,
    )?;
    Ok(table)
}
/// Install once, returning the real table for additive texture registration.
/// Repeated installs preserve table identity and any already-registered extensions.
pub fn install(lua: &mlua::Lua, env: &mlua::Table) -> mlua::Result<mlua::Table> {
    let table = match env.raw_get::<Value>("image")? {
        Value::Nil => create(lua)?,
        Value::Table(table) => table,
        _ => return Err(LuaError::generic("namespace image must be a table")),
    };
    install_namespace(env, "image", table.clone())?;
    Ok(table)
}
