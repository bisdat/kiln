// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (both pins): modules/mod-runtime/lua/cast_values.rs:11-446;
// modules/mod-runtime/lua/format_type_name.rs:29-45 (uncached short names).
use super::{LuaError, LuaValue};
use std::borrow::Cow;

pub trait CastLua<'a> {
    type Output;
    fn try_cast_lua(value: &'a LuaValue) -> mlua::Result<Option<Self::Output>>;
    fn name() -> Cow<'static, str>;
}
impl<'a> CastLua<'a> for LuaValue {
    type Output = &'a Self;
    fn try_cast_lua(value: &'a LuaValue) -> mlua::Result<Option<Self::Output>> {
        Ok(Some(value))
    }
    fn name() -> Cow<'static, str> {
        "any".into()
    }
}
macro_rules! borrowed {
    ($ty:ty, $variant:ident, $name:literal) => {
        impl<'a> CastLua<'a> for $ty {
            type Output = &'a Self;
            fn try_cast_lua(value: &'a LuaValue) -> mlua::Result<Option<Self::Output>> {
                Ok(match value {
                    LuaValue::$variant(v) => Some(v),
                    _ => None,
                })
            }
            fn name() -> Cow<'static, str> {
                $name.into()
            }
        }
    };
}
borrowed!(mlua::AnyUserData, UserData, "userdata");
borrowed!(mlua::Table, Table, "table");
impl CastLua<'_> for bool {
    type Output = Self;
    fn try_cast_lua(value: &LuaValue) -> mlua::Result<Option<Self>> {
        Ok(match value {
            LuaValue::Boolean(v) => Some(*v),
            _ => None,
        })
    }
    fn name() -> Cow<'static, str> {
        "boolean".into()
    }
}
impl CastLua<'_> for String {
    type Output = Self;
    fn try_cast_lua(value: &LuaValue) -> mlua::Result<Option<Self>> {
        Ok(match value {
            LuaValue::String(v) => Some(v.to_str()?.to_string()),
            LuaValue::Integer(v) => Some(v.to_string()),
            LuaValue::Number(v) => Some(v.to_string()),
            _ => None,
        })
    }
    fn name() -> Cow<'static, str> {
        "string".into()
    }
}
impl<'a> CastLua<'a> for &[u8] {
    type Output = mlua::BorrowedBytes<'a>;
    fn try_cast_lua(value: &'a LuaValue) -> mlua::Result<Option<Self::Output>> {
        Ok(match value {
            LuaValue::String(v) => Some(v.as_bytes()),
            _ => None,
        })
    }
    fn name() -> Cow<'static, str> {
        "string".into()
    }
}

// Preserve SF's order: cast to a wide primitive FIRST, then compare that converted
// value to bounds. Thus u64(-1) preserves bits and integral huge floats saturate.
macro_rules! primitive {
    ($ty:ty, $wide:ty, $name:literal, $fractional:expr) => {
        impl CastLua<'_> for $ty {
            type Output = Self;
            fn try_cast_lua(value: &LuaValue) -> mlua::Result<Option<Self>> {
                let fits = |wide: $wide| wide >= <$ty>::MIN as $wide && wide <= <$ty>::MAX as $wide;
                Ok(match value {
                    LuaValue::Integer(v) => {
                        let wide = *v as $wide;
                        fits(wide).then_some(wide as Self)
                    }
                    LuaValue::Number(v) => {
                        let wide = *v as $wide;
                        ((!$fractional || v.fract() == 0.0) && fits(wide)).then_some(wide as Self)
                    }
                    LuaValue::String(v) => v.to_str()?.parse::<Self>().ok(),
                    _ => None,
                })
            }
            fn name() -> Cow<'static, str> {
                $name.into()
            }
        }
    };
}
primitive!(u8, u64, "8-bit unsigned integer", true);
primitive!(u16, u64, "16-bit unsigned integer", true);
primitive!(u32, u64, "32-bit unsigned integer", true);
primitive!(u64, u64, "64-bit unsigned integer", true);
primitive!(usize, u64, "64-bit unsigned integer", true);
primitive!(i8, i64, "8-bit signed integer", true);
primitive!(i16, i64, "16-bit signed integer", true);
primitive!(i32, i64, "32-bit signed integer", true);
primitive!(i64, i64, "64-bit signed integer", true);
primitive!(isize, i64, "64-bit signed integer", true);
primitive!(f32, f64, "32-bit floating point", false);
primitive!(f64, f64, "64-bit floating point", false);

// Both SF pins: cast_values.rs:139-140,181-182,193-194,232-233,264-301,315.
// Numeric inputs are range-checked BEFORE rounding. Strings use half's parser
// directly, which intentionally accepts NaN/Inf and values outside that range.
impl CastLua<'_> for half::f16 {
    type Output = Self;
    fn try_cast_lua(value: &LuaValue) -> mlua::Result<Option<Self>> {
        let convert = |v: f64| {
            (v >= Self::MIN.to_f64() && v <= Self::MAX.to_f64()).then(|| Self::from_f64(v))
        };
        match value {
            LuaValue::Integer(v) => Ok(convert(*v as f64)),
            LuaValue::Number(v) => Ok(convert(*v)),
            LuaValue::String(v) => Ok(v.to_str()?.parse::<Self>().ok()),
            _ => Ok(None),
        }
    }
    fn name() -> Cow<'static, str> {
        "16-bit floating point".into()
    }
}

pub fn short_type_name<T: ?Sized>() -> String {
    let mut name = std::any::type_name::<T>();
    let mut result = String::new();
    while let Some(index) = name.find([' ', '<', '>', '(', ')', '[', ']', ',', ';']) {
        let (segment, rest) = name.split_at(index);
        result += segment.rsplit(':').next().unwrap_or(segment);
        result.push_str(&rest[..1]);
        name = &rest[1..];
    }
    if !name.is_empty() {
        result += name.rsplit(':').next().unwrap_or(name);
    }
    result
}
impl<U: mlua::UserData + 'static> CastLua<'_> for &U {
    type Output = mlua::UserDataRef<U>;
    fn try_cast_lua(value: &LuaValue) -> mlua::Result<Option<Self::Output>> {
        match value {
            LuaValue::UserData(ud) if ud.is::<U>() => match ud.borrow::<U>() {
                Ok(value) => Ok(Some(value)),
                Err(mlua::Error::UserDataTypeMismatch) => Ok(None),
                Err(error) => Err(error),
            },
            _ => Ok(None),
        }
    }
    fn name() -> Cow<'static, str> {
        short_type_name::<U>().into()
    }
}
impl<U: mlua::UserData + 'static> CastLua<'_> for &mut U {
    type Output = mlua::UserDataRefMut<U>;
    fn try_cast_lua(value: &LuaValue) -> mlua::Result<Option<Self::Output>> {
        match value {
            LuaValue::UserData(ud) if ud.is::<U>() => match ud.borrow_mut::<U>() {
                Ok(value) => Ok(Some(value)),
                Err(mlua::Error::UserDataTypeMismatch) => Ok(None),
                Err(error) => Err(error),
            },
            _ => Ok(None),
        }
    }
    fn name() -> Cow<'static, str> {
        short_type_name::<U>().into()
    }
}
impl<'a, T: CastLua<'a>> CastLua<'a> for Option<T> {
    type Output = Option<T::Output>;
    fn try_cast_lua(value: &'a LuaValue) -> mlua::Result<Option<Self::Output>> {
        if value.is_nil() {
            Ok(Some(None))
        } else {
            T::try_cast_lua(value).map(|v| v.map(Some))
        }
    }
    fn name() -> Cow<'static, str> {
        format!("{}?", T::name()).into()
    }
}
#[derive(Debug, PartialEq)]
pub enum Either<A, B> {
    A(A),
    B(B),
}
impl<'a, A: CastLua<'a>, B: CastLua<'a>> CastLua<'a> for Either<A, B> {
    type Output = Either<A::Output, B::Output>;
    fn try_cast_lua(value: &'a LuaValue) -> mlua::Result<Option<Self::Output>> {
        if let Some(value) = A::try_cast_lua(value)? {
            Ok(Some(Either::A(value)))
        } else {
            Ok(B::try_cast_lua(value)?.map(Either::B))
        }
    }
    fn name() -> Cow<'static, str> {
        format!("{} or {}", A::name(), B::name()).into()
    }
}
pub trait CastLuaExt {
    fn cast<'a, T: CastLua<'a>>(&'a self) -> mlua::Result<T::Output>;
    fn try_cast<'a, T: CastLua<'a>>(&'a self) -> mlua::Result<Option<T::Output>>;
}
impl CastLuaExt for LuaValue {
    fn cast<'a, T: CastLua<'a>>(&'a self) -> mlua::Result<T::Output> {
        self.try_cast::<T>()?
            .ok_or_else(|| LuaError::type_mismatch(T::name(), self))
    }
    fn try_cast<'a, T: CastLua<'a>>(&'a self) -> mlua::Result<Option<T::Output>> {
        T::try_cast_lua(self)
    }
}
