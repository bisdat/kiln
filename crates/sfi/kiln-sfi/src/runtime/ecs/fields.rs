// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/values/parse_field_path.rs:1-48, resolve_inline_field.rs:6-107,
// collect_inline_leaves.rs:5-124, reject_external_overlap.rs:5-84, inline_field.rs:10-76, decode_owned_value.rs:10-21;
// lua/ingame/ecs/access_component_fields.rs:2-157 (read_fields); session/mod.rs:138-175 (the plan cache).
//! ShroudForge's reflected inline fields and `runtime.ecs.read_fields`.
//!
//! - [`field_path`]: names and one-based static indices (`transform.position[1]`), at most 1,024 bytes and 64 steps.
//! - [`inline_field`]: a path resolved through reflected inline containment only (struct fields with their bases,
//!   static array elements, typedefs looked through), never a live pointer. The field must lie inside its parent and the
//!   component, and its storage is its scalar leaves: a field that holds external storage or a reference (arrays,
//!   strings, optionals, variants, blobs, object references), or overlaps one in the component, is refused with
//!   ShroudForge's text. [`super::Ecs::inline_field`] keeps the plans per VM.
//! - [`InlineField::read`]: the field's bytes decoded as its type by the VM's typed values (Source's decode: a scalar
//!   for a scalar field, a typed value otherwise).
//! - [`read_fields`]: ShroudForge's checks in its order (the denial, the availability, the argument count, the entity,
//!   the component, its metadata and size, a dense array of distinct string paths, at most 1,024), one stable copy of
//!   the component, then each path in path order.

use super::entities::text::COMPONENT_UNAVAILABLE;
use super::{Ecs, available, runtime_type_name};
use crate::runtime::types::{Registry, selectors};
use crate::runtime::values::Values;
use mlua::{Lua, Table, Value};
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op};
use sfi_core::lua::FunctionArgs;
use std::collections::{BTreeMap, HashSet};
use std::ops::Range;
use std::rc::Rc;

/// ShroudForge's texts.
pub mod text {
    /// access_component_fields.rs:46.
    pub const READ_ARGUMENTS: &str =
        "runtime.ecs.read_fields expects entity, component, and an array of field paths";
    /// access_component_fields.rs:44.
    pub const WRITE_ARGUMENTS: &str =
        "runtime.ecs.write_fields expects entity, component, changes, and optional expected fields";
    pub const TOO_MANY: &str = "at most 1024 field paths may be selected";
    pub const WRITE_KEYS: &str = "write_fields keys must be reflected field paths";
    pub const DENSE: &str = "read_fields expects a dense array of field paths";
    pub const STRING_PATHS: &str = "read_fields expects string field paths";
    pub const DUPLICATE: &str = "duplicate field path";
    pub const NONEMPTY: &str = "select a nonempty set of field paths";
}

/// One step of a field path.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FieldStep<'a> {
    Name(&'a str),
    /// A zero-based static array index (the path's one-based index minus one).
    Index(usize),
}

/// ShroudForge's field path (parse_field_path.rs:7-48).
pub fn field_path(mut path: &str) -> Result<Vec<FieldStep<'_>>, String> {
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
            let position = digits
                .parse::<usize>()
                .map_err(|_| "array index overflow")?;
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

/// The primitive kinds whose storage is outside the inline bytes or a reference.
pub const EXTERNAL: [&str; 9] = [
    "DsArray",
    "DsString",
    "DsOptional",
    "DsVariant",
    "BlobArray",
    "BlobString",
    "BlobOptional",
    "BlobVariant",
    "ObjectReference",
];
/// The integer kinds an enum may store in.
const ENUM_STORAGE: [&str; 8] = [
    "UInt8", "SInt8", "UInt16", "SInt16", "UInt32", "SInt32", "UInt64", "SInt64",
];

/// One explicitly selected inline field: its type, its offset in the component, and its scalar leaves (kind and byte
/// range relative to the field). Other fields of the component may own dynamic storage; theirs are never decoded or
/// written.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct InlineField {
    pub type_index: usize,
    pub qualified_name: String,
    pub size: usize,
    pub offset: usize,
    pub leaves: Vec<(&'static str, Range<usize>)>,
}

impl InlineField {
    /// The field's bytes in the component.
    pub fn byte_range(&self) -> Range<usize> {
        self.offset..self.offset + self.size
    }

    /// `value` encoded as the field's type (Source's validate, convert and serialize, on the VM's typed values) into
    /// `component`: only its scalar leaves are copied (struct and array padding stays as it was) and marked in `mask`;
    /// a leaf an earlier field of the same write already marked is an overlap (inline_field.rs:41-75).
    pub fn write_into(
        &self,
        values: &Values,
        value: &Value,
        component: &mut [u8],
        mask: &mut [u8],
    ) -> Result<(), String> {
        let encoded = values
            .encode(&self.qualified_name, value.clone())
            .map_err(|error| error.to_string())?;
        if encoded.len() != self.size {
            return Err("serialized inline field size changed".into());
        }
        for (kind, range) in &self.leaves {
            let bytes = &encoded[range.clone()];
            super::cas::validate_runtime_scalar(kind, bytes)?;
            let (start, end) = (self.offset + range.start, self.offset + range.end);
            let destination = component
                .get_mut(start..end)
                .ok_or("field outside component")?;
            let selected = mask.get_mut(start..end).ok_or("field outside write mask")?;
            if selected.iter().any(|byte| *byte != 0) {
                return Err("selected fields overlap".into());
            }
            destination.copy_from_slice(bytes);
            selected.fill(1);
        }
        Ok(())
    }

    /// The field of `component` (a snapshot of the whole component) decoded as its type (inline_field.rs:34-39).
    pub fn read(&self, lua: &Lua, values: &Values, component: &[u8]) -> Result<Value, String> {
        let bytes = component
            .get(self.byte_range())
            .ok_or("field outside component snapshot")?;
        decode(lua, values, &self.qualified_name, self.size, bytes)
    }
}

/// Source's `decode` (decode_owned_value.rs:10-21) on the VM's typed values.
pub fn decode(
    lua: &Lua,
    values: &Values,
    qualified_name: &str,
    size: usize,
    bytes: &[u8],
) -> Result<Value, String> {
    if bytes.len() < size {
        return Err(format!(
            "truncated {qualified_name}: expected at least {size} bytes"
        ));
    }
    values
        .open_bytes(lua, qualified_name, bytes)
        .map_err(|error| error.to_string())
}

/// ShroudForge's `inline_field` (resolve_inline_field.rs:8-107): `path` in registry type `component`.
pub fn inline_field(
    registry: &Registry,
    component: usize,
    path: &str,
) -> Result<InlineField, String> {
    let root = registry.get(component).ok_or("unresolved component type")?;
    let mut index = component;
    let mut offset = 0usize;
    for step in field_path(path)? {
        for current in selectors::inheritance(registry, index)? {
            let ty = registry.get(current).ok_or("unresolved field type")?;
            if ty.primitive_type != "Typedef" {
                index = current;
                break;
            }
        }
        let ty = registry.get(index).ok_or("unresolved field type")?;
        let (child, relative) = match step {
            FieldStep::Index(position) => {
                if ty.primitive_type != "StaticArray" {
                    return Err(format!("{} is not a static array", ty.qualified_name));
                }
                let inner = ty
                    .inner_type
                    .and_then(|i| registry.get(i))
                    .ok_or("missing static array element type")?;
                if position >= ty.field_count as usize
                    || (inner.size as usize).checked_mul(ty.field_count as usize)
                        != Some(ty.size as usize)
                {
                    return Err("static array index or reflected layout is invalid".into());
                }
                (inner.index, position * inner.size as usize)
            }
            FieldStep::Name(name) => {
                if ty.primitive_type != "Struct" {
                    return Err(format!(
                        "{} has no reflected inline fields",
                        ty.qualified_name
                    ));
                }
                let fields = selectors::fields(registry, index)?;
                let (_, field) = fields
                    .iter()
                    .find(|(_, field)| field.name == name)
                    .ok_or_else(|| {
                        format!("reflected field not found: {}.{name}", ty.qualified_name)
                    })?;
                (
                    field.type_index,
                    usize::try_from(field.data_offset).map_err(|_| "field offset overflow")?,
                )
            }
        };
        let child_type = registry.get(child).ok_or("unresolved field type")?;
        if relative
            .checked_add(child_type.size as usize)
            .is_none_or(|end| end > ty.size as usize)
        {
            return Err("field exceeds its reflected parent storage".into());
        }
        offset = offset
            .checked_add(relative)
            .ok_or("field offset overflow")?;
        index = child;
    }
    let ty = registry.get(index).ok_or("unresolved field type")?;
    let size = ty.size as usize;
    if offset
        .checked_add(size)
        .is_none_or(|end| end > root.size as usize)
    {
        return Err("field exceeds component storage".into());
    }
    let mut leaves = Vec::new();
    inline_leaves(registry, index, 0, size, &mut HashSet::new(), &mut leaves)?;
    leaves.sort_by_key(|(_, range)| range.start);
    if leaves.is_empty()
        || leaves
            .windows(2)
            .any(|pair| pair[0].1.end > pair[1].1.start)
    {
        return Err("inline field has empty or overlapping reflected storage".into());
    }
    reject_external_overlap(
        registry,
        component,
        0,
        &(offset..offset + size),
        &mut HashSet::new(),
    )?;
    Ok(InlineField {
        type_index: index,
        qualified_name: ty.qualified_name.clone(),
        size,
        offset,
        leaves,
    })
}

/// collect_inline_leaves.rs:5-124.
fn inline_leaves(
    registry: &Registry,
    index: usize,
    base: usize,
    limit: usize,
    stack: &mut HashSet<usize>,
    leaves: &mut Vec<(&'static str, Range<usize>)>,
) -> Result<(), String> {
    if stack.len() >= 64 || !stack.insert(index) {
        return Err("cyclic or excessively nested inline field layout".into());
    }
    let ty = registry.get(index).ok_or("unresolved inline field type")?;
    let end = base
        .checked_add(ty.size as usize)
        .filter(|end| *end <= limit)
        .ok_or_else(|| {
            format!(
                "inline field outside reflected storage: {}",
                ty.qualified_name
            )
        })?;
    match ty.primitive_type {
        "Typedef" => {
            let inner = ty.inner_type.ok_or("missing typedef value type")?;
            inline_leaves(registry, inner, base, end, stack, leaves)?;
        }
        "Struct" => {
            for (_, field) in selectors::fields(registry, index)? {
                let offset =
                    usize::try_from(field.data_offset).map_err(|_| "field offset overflow")?;
                let start = base.checked_add(offset).ok_or("field offset overflow")?;
                inline_leaves(registry, field.type_index, start, end, stack, leaves)?;
            }
        }
        "StaticArray" => {
            let inner = ty
                .inner_type
                .and_then(|i| registry.get(i))
                .ok_or("missing static array element type")?;
            if (inner.size as usize).checked_mul(ty.field_count as usize) != Some(ty.size as usize)
            {
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
        kind if EXTERNAL.contains(&kind) => {
            return Err(format!(
                "{} has external storage or reference ownership. Select an inline sibling field",
                ty.qualified_name
            ));
        }
        kind => {
            let width = match kind {
                "Bool" | "UInt8" | "SInt8" | "Bitmask8" => 1,
                "UInt16" | "SInt16" | "Bitmask16" => 2,
                "UInt32" | "SInt32" | "Float32" | "Bitmask32" => 4,
                "UInt64" | "SInt64" | "Float64" | "Bitmask64" => 8,
                "Guid" => 16,
                "Enum" => {
                    let inner = ty
                        .inner_type
                        .and_then(|i| registry.get(i))
                        .ok_or("missing enum storage type")?;
                    if !ENUM_STORAGE.contains(&inner.primitive_type) {
                        return Err("enum has no integer storage type".into());
                    }
                    inner.size as usize
                }
                _ => {
                    return Err(format!(
                        "{} is not an inline value field",
                        ty.qualified_name
                    ));
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

/// reject_external_overlap.rs:5-84: nothing of `selected` lies inside external storage or a reference descriptor.
fn reject_external_overlap(
    registry: &Registry,
    index: usize,
    base: usize,
    selected: &Range<usize>,
    stack: &mut HashSet<usize>,
) -> Result<(), String> {
    let ty = registry
        .get(index)
        .ok_or("unresolved enclosing field type")?;
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
        "Typedef" => {
            reject_external_overlap(
                registry,
                ty.inner_type.ok_or("missing typedef value type")?,
                base,
                selected,
                stack,
            )?;
        }
        "Struct" => {
            for (_, field) in selectors::fields(registry, index)? {
                let relative =
                    usize::try_from(field.data_offset).map_err(|_| "field offset overflow")?;
                let offset = base.checked_add(relative).ok_or("field offset overflow")?;
                reject_external_overlap(registry, field.type_index, offset, selected, stack)?;
            }
        }
        "StaticArray" => {
            let inner = ty
                .inner_type
                .and_then(|i| registry.get(i))
                .ok_or("missing static array element type")?;
            if inner.size == 0
                || (inner.size as usize).checked_mul(ty.field_count as usize)
                    != Some(ty.size as usize)
            {
                return Err("invalid enclosing static array layout".into());
            }
            let width = inner.size as usize;
            let first = selected.start.saturating_sub(base) / width;
            let last = (selected.end.min(end) - base - 1) / width;
            for position in first..=last {
                reject_external_overlap(
                    registry,
                    inner.index,
                    base + position * width,
                    selected,
                    stack,
                )?;
            }
        }
        kind if EXTERNAL.contains(&kind) => {
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

/// The selected paths of a `read_fields` or `write_fields` call, with their values, in path order.
pub(crate) type Selection = BTreeMap<String, (Rc<InlineField>, Value)>;

/// What `read_fields` and `write_fields` share (access_component_fields.rs:30-99): the component's name, its
/// resolved storage, the selection and one stable copy of the component.
pub(crate) struct Selected {
    pub entity: u32,
    pub name: String,
    pub component: super::registry::Resolved,
    pub type_index: usize,
    pub fields: Selection,
    pub before: Vec<u8>,
}

pub(crate) fn select(
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
    write: bool,
) -> Result<Selected, String> {
    let operation = if write { Op::EcsWrite } else { Op::EcsRead };
    if let Some(reason) = ecs.denial_reason(host, operation) {
        return Err(reason);
    }
    if !available(host, operation) {
        return Err(format!("{} is currently unavailable", operation.as_str()));
    }
    if args.len() != 3 && !(write && args.len() == 4) {
        return Err(if write {
            text::WRITE_ARGUMENTS
        } else {
            text::READ_ARGUMENTS
        }
        .into());
    }
    let entity = args.get::<u32>(0).map_err(|e| e.to_string())?;
    let name = runtime_type_name(ecs, args, 1).map_err(|e| e.to_string())?;
    let component = ecs
        .resolve_component(&name)
        .ok_or_else(|| format!("{COMPONENT_UNAVAILABLE}{name}"))?;
    let registry = ecs.types().clone();
    let metadata = registry
        .by_qualified_name(&name)
        .ok_or_else(|| format!("component type metadata unavailable: {name}"))?;
    if metadata.size != component.size {
        return Err(format!(
            "component layout mismatch for {name}: parser={}, runtime={}",
            metadata.size, component.size
        ));
    }
    let selections: Table = args.get::<Table>(2).map_err(|e| e.to_string())?.clone();
    let mut fields = Selection::new();
    for pair in selections.pairs::<Value, Value>() {
        let (key, value) = pair.map_err(|e| e.to_string())?;
        if fields.len() >= 1024 {
            return Err(text::TOO_MANY.into());
        }
        let path = if write {
            let Value::String(path) = key else {
                return Err(text::WRITE_KEYS.into());
            };
            (*path.to_str().map_err(|e| e.to_string())?).to_owned()
        } else {
            let Value::Integer(position) = key else {
                return Err(text::DENSE.into());
            };
            if position <= 0 || position as usize > selections.raw_len() {
                return Err(text::DENSE.into());
            }
            let Value::String(path) = &value else {
                return Err(text::STRING_PATHS.into());
            };
            (*path.to_str().map_err(|e| e.to_string())?).to_owned()
        };
        let field = ecs
            .inline_field(metadata.index, &path)
            .map_err(|e| format!("{path}: {e}"))?;
        if fields.insert(path, (field, value)).is_some() {
            return Err(text::DUPLICATE.into());
        }
    }
    if fields.is_empty() || (!write && fields.len() != selections.raw_len()) {
        return Err(text::NONEMPTY.into());
    }
    let before = ecs
        .snapshot(entity, component.component, component.size)
        .ok_or_else(|| format!("component read failed: {name}"))?;
    Ok(Selected {
        entity,
        name,
        component,
        type_index: metadata.index,
        fields,
        before,
    })
}

/// `runtime.ecs.read_fields(entity, component, paths)`: `fields` or `nil, reason`.
pub fn read_fields(
    lua: &Lua,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<(Value, Option<String>)> {
    let read = || -> Result<Value, String> {
        let selected = select(ecs, host, args, false)?;
        let values = ecs.values().map_err(|e| e.to_string())?;
        let result = lua
            .create_table_with_capacity(0, selected.fields.len())
            .map_err(|e| e.to_string())?;
        for (path, (field, _)) in selected.fields {
            let value = field
                .read(lua, &values, &selected.before)
                .map_err(|e| format!("{path}: {e}"))?;
            result
                .raw_set(path.as_str(), value)
                .map_err(|e| e.to_string())?;
        }
        Ok(Value::Table(result))
    };
    Ok(match read() {
        Ok(value) => (value, None),
        Err(reason) => (Value::Nil, Some(reason)),
    })
}

/// ShroudForge's `expect_inline_fields` (expect_inline_fields.rs:8-41): every expected path, encoded through the same
/// field codec as a write, must equal `before` there. The paths' leaves are marked in `depends` (the bytes the write
/// was decided on).
pub(crate) fn expect_inline_fields(
    ecs: &Ecs,
    values: &Values,
    component: usize,
    before: &[u8],
    expected: &Table,
    depends: &mut [u8],
) -> Result<(), String> {
    let mut candidate = before.to_vec();
    let mut mask = vec![0u8; before.len()];
    let mut count = 0usize;
    for pair in expected.pairs::<String, Value>() {
        let (path, value) = pair.map_err(|error| error.to_string())?;
        count += 1;
        if count > 1024 {
            return Err("at most 1024 expected field paths may be selected".into());
        }
        ecs.inline_field(component, &path)?
            .write_into(values, &value, &mut candidate, &mut mask)
            .map_err(|error| format!("expected {path}: {error}"))?;
    }
    if count == 0 {
        return Err("expected fields must be nonempty when provided".into());
    }
    if mask
        .iter()
        .enumerate()
        .any(|(index, selected)| *selected != 0 && candidate[index] != before[index])
    {
        return Err(
            "Selected field snapshot changed before update. Read current fields and retry".into(),
        );
    }
    for (depend, selected) in depends.iter_mut().zip(&mask) {
        *depend |= *selected;
    }
    Ok(())
}

/// `runtime.ecs.write_fields(entity, component, changes, expected?)` (access_component_fields.rs:13-22,100-140):
/// `true, nil, confirmed` or `false, reason, nil`.
pub fn write_fields(
    lua: &Lua,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<(bool, Option<String>, Value)> {
    Ok(match write_selected(lua, ecs, host, args) {
        Ok(confirmed) => (true, None, confirmed),
        Err(reason) => (false, Some(reason), Value::Nil),
    })
}

fn write_selected(
    lua: &Lua,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> Result<Value, String> {
    let selected = select(ecs, host, args, true)?;
    let before = &selected.before;
    if !ecs.can_write_component(selected.component.component) {
        return Err(super::cas::text::NO_WRITE_CONTEXT.into());
    }
    let values = ecs.values().map_err(|e| e.to_string())?;
    let mut depends = vec![0u8; before.len()];
    if args.len() == 4 {
        let expected = args.get::<Table>(3).map_err(|e| e.to_string())?;
        expect_inline_fields(
            ecs,
            &values,
            selected.type_index,
            before,
            expected,
            &mut depends,
        )?;
    }
    let mut after = before.clone();
    let mut mask = vec![0u8; before.len()];
    for (path, (field, value)) in &selected.fields {
        field
            .write_into(&values, value, &mut after, &mut mask)
            .map_err(|e| format!("{path}: {e}"))?;
    }
    // an attribute component's candidate is recalculated as ShroudForge's recalculate_component_update does
    let attribute = super::attributes::writes::recalculate_component_update(
        lua,
        ecs,
        &selected.name,
        before,
        &mut after,
        &mut mask,
    )?;
    let writer = super::cas::Writer {
        host,
        session: ecs.world_session(),
    };
    if let Some(runtime) = ecs.runtime() {
        let key =
            crate::runtime::overrides::component_key(&writer, selected.entity, &selected.name);
        crate::runtime::overrides::check_write(runtime.facts.ownership(), &writer, &key)?;
    }
    let confirmed = lua
        .create_table_with_capacity(0, selected.fields.len())
        .map_err(|e| e.to_string())?;
    for (path, (field, _)) in &selected.fields {
        confirmed
            .raw_set(path.as_str(), field.read(lua, &values, &after)?)
            .map_err(|e| e.to_string())?;
    }
    if after != *before {
        for (depend, selected) in depends.iter_mut().zip(&mask) {
            *depend |= *selected;
        }
        // an attribute write depends on the whole component (its header, identity and root were checked on it)
        if attribute {
            depends.fill(1);
        }
        ecs.compare_exchange(
            selected.entity,
            selected.component.component,
            before,
            &mask,
            &after,
            &depends,
        )?;
    }
    Ok(Value::Table(confirmed))
}

/// ShroudForge's `normalize_field` (values/normalize_field.rs): `value` written into a zeroed component of type
/// `root` through `path`'s field codec, then read back: the canonical value a write of it confirms.
pub fn normalize_field(
    lua: &Lua,
    ecs: &Ecs,
    root: usize,
    path: &str,
    value: &Value,
) -> Result<Value, String> {
    let size = ecs
        .types()
        .get(root)
        .ok_or("unresolved component type")?
        .size as usize;
    let field = ecs.inline_field(root, path)?;
    let values = ecs.values().map_err(|e| e.to_string())?;
    let mut bytes = vec![0u8; size];
    let mut mask = vec![0u8; size];
    field.write_into(&values, value, &mut bytes, &mut mask)?;
    field.read(lua, &values, &bytes)
}
