// SPDX-License-Identifier: GPL-3.0-or-later
//! `runtime.values` (values/register_value_api.rs): owned KFC values over the session's registry. No phase check: it
//! works before the game starts. `decode`, `new` and `encode` run the value codec with the value wrappers; `normalize_field`
//! resolves one reflected inline field (values/{parse_field_path,resolve_inline_field,collect_inline_leaves,
//! reject_external_overlap,inline_field,validate_runtime_scalar}.rs) and writes and reads it in a zeroed root.
//!
//! Adapted from ShroudForge 1.7.9 modules/mod-runtime/lua/values/*.rs (MIT, Copyright (c) 2026 bonsaibauer; notice in
//! sf_startup/mod.rs). SF caches resolved fields per session (session/mod.rs:138-174); the cache is not observable and
//! is left out.
use std::{collections::HashSet, ops::Range, rc::Rc};

use kfc::reflection::{PrimitiveType, TypeIndex, TypeRegistry};
use mlua::{Lua, Table};
use sfi_core::lua::LuaValue;

use super::PregameRuntime;
use crate::sf_startup::{
    game::resolve_type::{fields, inheritance, resolve},
    resource_values::{self, TypeHandle, ValueObjects},
};

/// SF `validate_owned_type`: a type the value codec can own (no engine-allocated storage anywhere inside it).
pub fn validate_owned_type(registry: &TypeRegistry, index: TypeIndex) -> Result<(), String> {
    supported(registry, index, &mut HashSet::new())
}

fn supported(registry: &TypeRegistry, index: TypeIndex, seen: &mut HashSet<TypeIndex>) -> Result<(), String> {
    if !seen.insert(index) {
        return Ok(());
    }
    let ty = registry.get(index).ok_or("unresolved value type")?;
    if matches!(
        ty.primitive_type,
        PrimitiveType::DsArray | PrimitiveType::DsString | PrimitiveType::DsOptional | PrimitiveType::DsVariant
    ) {
        return Err(format!(
            "{} requires engine allocation and ownership. The KFC value codec cannot construct it",
            ty.qualified_name
        ));
    }
    inheritance(registry, index)?;
    if let Some(inner) = ty.inner_type {
        supported(registry, inner, seen)?;
    }
    for field in ty.struct_fields.values() {
        supported(registry, field.r#type, seen)?;
    }
    Ok(())
}

/// SF `resolve_owned_type::handle`: the selector's type, which must be an owned value type.
fn handle(registry: &Rc<TypeRegistry>, selector: &LuaValue) -> Result<TypeHandle, String> {
    let index = resolve(registry, selector)?;
    validate_owned_type(registry, index)?;
    TypeHandle::try_new(registry.clone(), index).ok_or_else(|| "unresolved value type".into())
}

/// SF `decode_owned_value::decode`.
fn decode(lua: &Lua, ty: &TypeHandle, bytes: &[u8], objects: &ValueObjects) -> Result<LuaValue, String> {
    if bytes.len() < ty.size as usize {
        return Err(format!(
            "truncated {}: expected at least {} bytes",
            ty.qualified_name, ty.size
        ));
    }
    resource_values::decode(bytes.into(), ty, lua, objects)
}

/// SF `lua_value_result::outcome`.
fn outcome(result: Result<LuaValue, String>) -> (Option<LuaValue>, Option<String>) {
    match result {
        Ok(value) => (Some(value), None),
        Err(reason) => (None, Some(reason)),
    }
}

/// SF `register_value_api::create` (values/register_value_api.rs:9-40), in its order and with its argument types.
pub(super) fn create(lua: &Lua, context: &PregameRuntime) -> mlua::Result<Table> {
    let table = lua.create_table()?;
    let registry = context.types.registry().clone();
    let objects = context.objects.clone();
    let (r, o) = (registry.clone(), objects.clone());
    table.set(
        "normalize_field",
        lua.create_function(move |lua, (selector, path, value): (LuaValue, String, LuaValue)| {
            let normalize = || -> Result<LuaValue, String> {
                let root = handle(&r, &selector)?;
                let field = inline_field(&r, root.index, &path)?;
                let mut bytes = vec![0; root.size as usize];
                let mut mask = vec![0; bytes.len()];
                field.write_into(lua, &value, &mut bytes, &mut mask, &o)?;
                field.read(lua, &bytes, &o)
            };
            Ok(outcome(normalize()))
        })?,
    )?;
    let (r, o) = (registry.clone(), objects.clone());
    table.set(
        "decode",
        lua.create_function(move |lua, (selector, bytes): (LuaValue, mlua::String)| {
            Ok(outcome(
                handle(&r, &selector).and_then(|ty| decode(lua, &ty, &bytes.as_bytes(), &o)),
            ))
        })?,
    )?;
    let (r, o) = (registry.clone(), objects.clone());
    table.set(
        "new",
        lua.create_function(move |lua, selector: LuaValue| {
            Ok(outcome(handle(&r, &selector).and_then(|ty| {
                let bytes = ty
                    .default_value
                    .as_ref()
                    .ok_or_else(|| format!("no reflected default for {}", ty.qualified_name))?;
                decode(lua, &ty, bytes, &o)
            })))
        })?,
    )?;
    table.set(
        "encode",
        lua.create_function(move |lua, (selector, value): (LuaValue, LuaValue)| {
            let result = handle(&registry, &selector).and_then(|ty| {
                let bytes = resource_values::encode(&value, &ty, lua, &objects)?;
                lua.create_string(bytes)
                    .map(LuaValue::String)
                    .map_err(|e| e.to_string())
            });
            Ok(outcome(result))
        })?,
    )?;
    Ok(table)
}

#[derive(Clone, Copy)]
enum FieldStep<'a> {
    Name(&'a str),
    Index(usize),
}

/// SF `parse_field_path::field_path`.
fn field_path(mut path: &str) -> Result<Vec<FieldStep<'_>>, String> {
    if path.is_empty() || path.len() > 1024 {
        return Err("expected a nonempty field path of at most 1024 bytes".into());
    }
    let mut steps = Vec::new();
    while !path.is_empty() {
        if steps.len() >= 64 {
            return Err("field path exceeds 64 steps".into());
        }
        if let Some(tail) = path.strip_prefix('[') {
            let close = tail.find(']').ok_or("unterminated array index")?;
            let digits = &tail[..close];
            if digits.is_empty() || !digits.bytes().all(|byte| byte.is_ascii_digit()) {
                return Err("invalid array index".into());
            }
            let position = digits.parse::<usize>().map_err(|_| "array index overflow")?;
            if position == 0 {
                return Err("array indices are one-based".into());
            }
            steps.push(FieldStep::Index(position - 1));
            path = &tail[close + 1..];
        } else {
            let end = path.find(['.', '[', ']']).unwrap_or(path.len());
            if end == 0 {
                return Err("expected a reflected field name".into());
            }
            steps.push(FieldStep::Name(&path[..end]));
            path = &path[end..];
        }
        if let Some(tail) = path.strip_prefix('.') {
            if tail.is_empty() || tail.starts_with(['.', '[', ']']) {
                return Err("expected a field name after '.'".into());
            }
            path = tail;
        } else if !path.is_empty() && !path.starts_with('[') {
            return Err("expected '.' or an array index in field path".into());
        }
    }
    Ok(steps)
}

/// SF `InlineField`: one selected inline field and its value leaves.
struct InlineField {
    ty: TypeHandle,
    offset: usize,
    leaves: Vec<(PrimitiveType, Range<usize>)>,
}

impl InlineField {
    fn read(&self, lua: &Lua, component: &[u8], objects: &ValueObjects) -> Result<LuaValue, String> {
        let bytes = component
            .get(self.offset..self.offset + self.ty.size as usize)
            .ok_or("field outside component snapshot")?;
        decode(lua, &self.ty, bytes, objects)
    }

    fn write_into(
        &self,
        lua: &Lua,
        value: &LuaValue,
        component: &mut [u8],
        mask: &mut [u8],
        objects: &ValueObjects,
    ) -> Result<(), String> {
        let checked = resource_values::validate_and_clone(value, &self.ty, lua, objects).map_err(|e| e.to_string())?;
        let value = resource_values::convert_lua_to_value(&checked, &self.ty, objects).map_err(|e| e.to_string())?;
        let encoded = resource_values::value_bytes(&value, &self.ty)?;
        if encoded.len() != self.ty.size as usize {
            return Err("serialized inline field size changed".into());
        }
        for (kind, range) in &self.leaves {
            let bytes = &encoded[range.clone()];
            validate_runtime_scalar(*kind, bytes)?;
            let start = self.offset + range.start;
            let end = self.offset + range.end;
            let destination = component.get_mut(start..end).ok_or("field outside component")?;
            let selected = mask.get_mut(start..end).ok_or("field outside write mask")?;
            if selected.iter().any(|byte| *byte != 0) {
                return Err("selected fields overlap".into());
            }
            destination.copy_from_slice(bytes);
            selected.fill(1);
        }
        Ok(())
    }
}

/// SF `validate_runtime_scalar`.
fn validate_runtime_scalar(kind: PrimitiveType, bytes: &[u8]) -> Result<(), String> {
    let finite = match kind {
        PrimitiveType::Float32 => {
            f32::from_le_bytes(bytes.try_into().map_err(|_| "invalid float32 width")?).is_finite()
        }
        PrimitiveType::Float64 => {
            f64::from_le_bytes(bytes.try_into().map_err(|_| "invalid float64 width")?).is_finite()
        }
        _ => true,
    };
    if !finite {
        return Err("runtime field writes require finite floating-point values".into());
    }
    Ok(())
}

/// SF `resolve_inline_field::inline_field`: names and one-based static indices along reflected inline containment.
fn inline_field(registry: &Rc<TypeRegistry>, component: TypeIndex, path: &str) -> Result<InlineField, String> {
    let root = registry.get(component).ok_or("unresolved component type")?;
    let mut index = component;
    let mut offset = 0usize;
    for step in field_path(path)? {
        let chain = inheritance(registry, index)?;
        for current in chain {
            let ty = registry.get(current).ok_or("unresolved field type")?;
            if ty.primitive_type != PrimitiveType::Typedef {
                index = current;
                break;
            }
        }
        let ty = registry.get(index).ok_or("unresolved field type")?;
        let (child, relative) = if let FieldStep::Index(position) = step {
            if ty.primitive_type != PrimitiveType::StaticArray {
                return Err(format!("{} is not a static array", ty.qualified_name));
            }
            let inner = ty
                .inner_type
                .and_then(|i| registry.get(i))
                .ok_or("missing static array element type")?;
            if position >= ty.field_count as usize
                || (inner.size as usize).checked_mul(ty.field_count as usize) != Some(ty.size as usize)
            {
                return Err("static array index or reflected layout is invalid".into());
            }
            (inner.index, position * inner.size as usize)
        } else {
            if ty.primitive_type != PrimitiveType::Struct {
                return Err(format!("{} has no reflected inline fields", ty.qualified_name));
            }
            let FieldStep::Name(name) = step else {
                return Err("expected a reflected field name".into());
            };
            let fields = fields(registry, index)?;
            let (_, field) = fields
                .iter()
                .find(|(_, field)| field.name == name)
                .ok_or_else(|| format!("reflected field not found: {}.{name}", ty.qualified_name))?;
            (
                field.r#type,
                usize::try_from(field.data_offset).map_err(|_| "field offset overflow")?,
            )
        };
        let child_type = registry.get(child).ok_or("unresolved field type")?;
        if relative
            .checked_add(child_type.size as usize)
            .is_none_or(|end| end > ty.size as usize)
        {
            return Err("field exceeds its reflected parent storage".into());
        }
        offset = offset.checked_add(relative).ok_or("field offset overflow")?;
        index = child;
    }
    let ty = TypeHandle::try_new(registry.clone(), index).ok_or("unresolved field type")?;
    if offset
        .checked_add(ty.size as usize)
        .is_none_or(|end| end > root.size as usize)
    {
        return Err("field exceeds component storage".into());
    }
    let mut leaves = Vec::new();
    inline_leaves(registry, index, 0, ty.size as usize, &mut HashSet::new(), &mut leaves)?;
    leaves.sort_by_key(|(_, range)| range.start);
    if leaves.is_empty() || leaves.windows(2).any(|pair| pair[0].1.end > pair[1].1.start) {
        return Err("inline field has empty or overlapping reflected storage".into());
    }
    reject_external_overlap(
        registry,
        component,
        0,
        &(offset..offset + ty.size as usize),
        &mut HashSet::new(),
    )?;
    Ok(InlineField { ty, offset, leaves })
}

/// SF `collect_inline_leaves::inline_leaves`.
fn inline_leaves(
    registry: &TypeRegistry,
    index: TypeIndex,
    base: usize,
    limit: usize,
    stack: &mut HashSet<TypeIndex>,
    leaves: &mut Vec<(PrimitiveType, Range<usize>)>,
) -> Result<(), String> {
    if stack.len() >= 64 || !stack.insert(index) {
        return Err("cyclic or excessively nested inline field layout".into());
    }
    let ty = registry.get(index).ok_or("unresolved inline field type")?;
    let end = base
        .checked_add(ty.size as usize)
        .filter(|end| *end <= limit)
        .ok_or_else(|| format!("inline field outside reflected storage: {}", ty.qualified_name))?;
    match ty.primitive_type {
        PrimitiveType::Typedef => {
            let inner = ty.inner_type.ok_or("missing typedef value type")?;
            inline_leaves(registry, inner, base, end, stack, leaves)?;
        }
        PrimitiveType::Struct => {
            for (_, field) in fields(registry, index)? {
                let offset = usize::try_from(field.data_offset).map_err(|_| "field offset overflow")?;
                let start = base.checked_add(offset).ok_or("field offset overflow")?;
                inline_leaves(registry, field.r#type, start, end, stack, leaves)?;
            }
        }
        PrimitiveType::StaticArray => {
            let inner = ty
                .inner_type
                .and_then(|i| registry.get(i))
                .ok_or("missing static array element type")?;
            if (inner.size as usize).checked_mul(ty.field_count as usize) != Some(ty.size as usize) {
                return Err("static array size differs from reflected element count".into());
            }
            for position in 0..ty.field_count as usize {
                inline_leaves(
                    registry,
                    inner.index,
                    base + position * inner.size as usize,
                    end,
                    stack,
                    leaves,
                )?;
            }
        }
        PrimitiveType::DsArray
        | PrimitiveType::DsString
        | PrimitiveType::DsOptional
        | PrimitiveType::DsVariant
        | PrimitiveType::BlobArray
        | PrimitiveType::BlobString
        | PrimitiveType::BlobOptional
        | PrimitiveType::BlobVariant
        | PrimitiveType::ObjectReference => {
            return Err(format!(
                "{} has external storage or reference ownership. Select an inline sibling field",
                ty.qualified_name
            ));
        }
        kind => {
            let width = match kind {
                PrimitiveType::Bool | PrimitiveType::UInt8 | PrimitiveType::SInt8 | PrimitiveType::Bitmask8 => 1,
                PrimitiveType::UInt16 | PrimitiveType::SInt16 | PrimitiveType::Bitmask16 => 2,
                PrimitiveType::UInt32 | PrimitiveType::SInt32 | PrimitiveType::Float32 | PrimitiveType::Bitmask32 => 4,
                PrimitiveType::UInt64 | PrimitiveType::SInt64 | PrimitiveType::Float64 | PrimitiveType::Bitmask64 => 8,
                PrimitiveType::Guid => 16,
                PrimitiveType::Enum => {
                    let inner = ty
                        .inner_type
                        .and_then(|i| registry.get(i))
                        .ok_or("missing enum storage type")?;
                    if !matches!(
                        inner.primitive_type,
                        PrimitiveType::UInt8
                            | PrimitiveType::SInt8
                            | PrimitiveType::UInt16
                            | PrimitiveType::SInt16
                            | PrimitiveType::UInt32
                            | PrimitiveType::SInt32
                            | PrimitiveType::UInt64
                            | PrimitiveType::SInt64
                    ) {
                        return Err("enum has no integer storage type".into());
                    }
                    inner.size as usize
                }
                _ => {
                    return Err(format!("{} is not an inline value field", ty.qualified_name));
                }
            };
            if ty.size as usize != width || leaves.len() >= 65_536 {
                return Err("invalid or excessively large inline field layout".into());
            }
            leaves.push((kind, base..end));
        }
    }
    stack.remove(&index);
    Ok(())
}

/// SF `reject_external_overlap`.
fn reject_external_overlap(
    registry: &TypeRegistry,
    index: TypeIndex,
    base: usize,
    selected: &Range<usize>,
    stack: &mut HashSet<TypeIndex>,
) -> Result<(), String> {
    let ty = registry.get(index).ok_or("unresolved enclosing field type")?;
    let end = base
        .checked_add(ty.size as usize)
        .ok_or("enclosing field offset overflow")?;
    if end <= selected.start || base >= selected.end {
        return Ok(());
    }
    if stack.len() >= 64 || !stack.insert(index) {
        return Err("cyclic or excessively nested enclosing field layout".into());
    }
    match ty.primitive_type {
        PrimitiveType::Typedef => {
            reject_external_overlap(
                registry,
                ty.inner_type.ok_or("missing typedef value type")?,
                base,
                selected,
                stack,
            )?;
        }
        PrimitiveType::Struct => {
            for (_, field) in fields(registry, index)? {
                let relative = usize::try_from(field.data_offset).map_err(|_| "field offset overflow")?;
                let offset = base.checked_add(relative).ok_or("field offset overflow")?;
                reject_external_overlap(registry, field.r#type, offset, selected, stack)?;
            }
        }
        PrimitiveType::StaticArray => {
            let inner = ty
                .inner_type
                .and_then(|i| registry.get(i))
                .ok_or("missing static array element type")?;
            if inner.size == 0 || (inner.size as usize).checked_mul(ty.field_count as usize) != Some(ty.size as usize) {
                return Err("invalid enclosing static array layout".into());
            }
            let width = inner.size as usize;
            let first = selected.start.saturating_sub(base) / width;
            let last = (selected.end.min(end) - base - 1) / width;
            for position in first..=last {
                reject_external_overlap(registry, inner.index, base + position * width, selected, stack)?;
            }
        }
        PrimitiveType::DsArray
        | PrimitiveType::DsString
        | PrimitiveType::DsOptional
        | PrimitiveType::DsVariant
        | PrimitiveType::BlobArray
        | PrimitiveType::BlobString
        | PrimitiveType::BlobOptional
        | PrimitiveType::BlobVariant
        | PrimitiveType::ObjectReference => {
            return Err(format!(
                "selected field overlaps external storage or reference descriptor {}",
                ty.qualified_name
            ));
        }
        _ => {}
    }
    stack.remove(&index);
    Ok(())
}
