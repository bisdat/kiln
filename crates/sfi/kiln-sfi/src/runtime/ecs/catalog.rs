// SPDX-License-Identifier: MIT
// the interpreter's runtime.ecs metadata calls.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (identical in both pins): modules/mod-runtime/lua/ingame/ecs/read_component_catalog.rs:2-193
// (get_catalog), ecs/read_component_registry.rs:3-31 (get_components), :33-107 (a registration entry), :109-151
// (get_registry), :153-180 (get_component); modules/mod-runtime/lua/values/validate_owned_type.rs:5-38; the
// RuntimeComponentRegistration / RuntimeEcsCatalog contract in lua/shroudforge/v1/definitions/runtime_api.lua:456-605.
//! `runtime.ecs.get_catalog`, `get_registry`, `get_component` and `get_components`.
//!
//! Each follows its ShroudForge source line by line, on Kiln's facts:
//! - the registry report and `resolve` are [`super::registry`]'s join of kiln.types' `ecs_registry` with the
//!   archive registry;
//! - readiness (`entity_access_ready`) and the write context (`entity_writes_ready`) are the package host's
//!   provider facts (`ready`, `can_write`), the static contract its `static_support`, the denials the registrar's
//!   `runtime_denial_reason`;
//! - `can_write_component` is kiln.world's `ecs_write_policy` for the row's component hash: writable when the
//!   member answers OK with status `KILN_ECS_OK`; a missing member, another status or another result is not
//!   writable. It is asked only where ShroudForge asks its provider.
//!
//! **No phase gate on the metadata calls** (`get_catalog`, `get_registry`, `get_component`): they answer in any
//! lifecycle state, as ShroudForge's do; only the read and write facts inside them carry the denials.
//! `get_components` keeps ShroudForge's query denial and readiness check.
//!
//! **Partial until `kiln.types.ecs_registration`** (and `read_original` for callback code): a registration's
//! `callbacks` list is empty, `storage_flags_bits` is absent, `template_type` is nil, its storage is its own type
//! (`storage = "entity"`, the catalog role `runtime`), and no catalog entry gets a `template` role. The seam is
//! [`RegistrationDetail`]: a later version fills it from `ecs_registration`.

use super::Ecs;
use super::registry::{Registration, Snapshot};
use crate::runtime::types::{Registry, selectors};
use mlua::{Lua, Table, Value};
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op, runtime_denial_reason};
use std::collections::{BTreeMap, HashSet};
use std::rc::Rc;

/// ShroudForge's texts.
pub mod text {
    /// get_components without a ready provider (read_component_registry.rs:12).
    pub const NOT_READY: &str = concat!("K", "FC Runtime is not ready");
    /// read_component_catalog.rs:41.
    pub const NO_WRITE_CONTEXT: &str = "runtime entity write context is not ready";
    /// read_component_catalog.rs:43.
    pub const PHASE_NOT_VALIDATED: &str =
        "component requires an original command phase that is not validated for this profile";
    /// read_component_registry.rs:62.
    pub const PHASE_UNAVAILABLE: &str =
        "component write context or original command phase is unavailable";
    /// read_component_registry.rs:99.
    pub const TEMPLATE_ONLY: &str = "template-only registration has no entity storage column";
    /// read_component_catalog.rs:148.
    pub const NOT_REGISTERED: &str = "reflected type has no engine component registration";
    /// read_component_catalog.rs:150.
    pub const SIZES_DISAGREE: &str = "engine registration and reflected storage size do not agree";
    /// get_component without a matching registration (read_component_registry.rs:178), followed by the name.
    pub const NO_REGISTRATION: &str = "no engine component registration for ";
    /// The report's source (refresh_component_registry.cpp:163).
    pub const SOURCE: &str = "live-engine-registration";
}

/// The registration detail `kiln.types.ecs_registration` will give: until then [`RegistrationDetail::PART_A`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RegistrationDetail {
    /// Whether the registration owns an entity storage column (`storage = "entity"`), or only a configuration
    /// template.
    pub has_storage: bool,
    /// The raw packed storage flags, when known.
    pub storage_flags_bits: Option<u32>,
}
impl RegistrationDetail {
    /// What a kiln.types row says on its own: storage of its type, nothing more.
    pub const PART_A: Self = Self {
        has_storage: true,
        storage_flags_bits: None,
    };
}

/// ShroudForge's `validate_owned_type`: a type the value codec can own (no engine-allocated storage anywhere in it).
pub fn validate_owned_type(registry: &Registry, index: usize) -> Result<(), String> {
    fn supported(
        registry: &Registry,
        index: usize,
        seen: &mut HashSet<usize>,
    ) -> Result<(), String> {
        if !seen.insert(index) {
            return Ok(());
        }
        let ty = registry.get(index).ok_or("unresolved value type")?;
        if matches!(
            ty.primitive_type,
            "DsArray" | "DsString" | "DsOptional" | "DsVariant"
        ) {
            return Err(format!(
                "{} requires engine allocation and ownership. The {} value codec cannot construct it",
                ty.qualified_name,
                concat!("K", "FC")
            ));
        }
        selectors::inheritance(registry, index)?;
        if let Some(inner) = ty.inner_type {
            supported(registry, inner, seen)?;
        }
        for field in &ty.struct_fields {
            supported(registry, field.type_index, seen)?;
        }
        Ok(())
    }
    supported(registry, index, &mut HashSet::new())
}

type Reason = (Value, Option<String>);

fn archive(ecs: &Ecs) -> mlua::Result<Rc<Registry>> {
    Ok(ecs.types().clone())
}

/// `runtime_provider::can_write_component` for a resolved component: kiln.world's `ecs_write_policy`.
fn can_write_component(ecs: &Ecs, component: u32) -> bool {
    ecs.can_write_component(component)
}

/// `runtime.ecs.get_catalog()` (read_component_catalog.rs:2-193).
pub fn get_catalog(lua: &Lua, ecs: &Ecs, host: &dyn RuntimeHost) -> mlua::Result<Table> {
    let registry = archive(ecs)?;
    let result = lua.create_table()?;
    result.raw_set("version", registry.registry_version())?;
    let entries = lua.create_table()?;
    let report = ecs.registry().read(&ecs.services, &registry).ok();
    let available = report.as_ref().and_then(|r| r.available().ok());
    let registration_available = available.is_some();
    let mut registrations = BTreeMap::<&str, Vec<(&Registration, &'static str)>>::new();
    for row in available.into_iter().flatten() {
        if let Some(ty) = registry.get(row.type_index) {
            // part A: every row's storage is its own type; template roles come with `ecs_registration`
            registrations
                .entry(ty.qualified_name.as_str())
                .or_default()
                .push((row, "runtime"));
        }
    }
    let resolve = |name: &str| report.as_ref().and_then(|r| r.resolve(name));
    let (mut registered_count, mut runtime_count, mut template_count, mut resolved_count) =
        (0usize, 0usize, 0usize, 0usize);
    let facts = host.provider_facts();
    let (ready, can_write) = (facts.ready, facts.can_write);
    let read_denial = runtime_denial_reason(host, Op::EcsRead);
    let write_denial = runtime_denial_reason(host, Op::EcsWrite);
    let read_contract = host.static_support(Op::EcsRead).is_available();
    let write_contract = host.static_support(Op::EcsWrite).is_available();
    for metadata in registry
        .iter()
        .filter(|ty| ty.qualified_name.starts_with(super::registry::ECS_PREFIX))
    {
        let name = metadata.qualified_name.as_str();
        let entry = lua.create_table()?;
        entry.raw_set("type", ecs.type_value(lua, metadata.index)?)?;
        entry.raw_set("qualified_name", name)?;
        entry.raw_set("size", metadata.size)?;
        let roles = registrations.get(name);
        let registered = roles.is_some();
        let has_role = |wanted: &str| {
            roles.is_some_and(|rows| {
                rows.iter()
                    .any(|(row, role)| *role == wanted && row.runtime_size == metadata.size)
            })
        };
        let runtime_registered = has_role("runtime");
        let template_registered = has_role("template");
        // Mapping and current entity access are different facts (read_component_catalog.rs:54-55).
        let resolved_storage = resolve(name).filter(|c| c.size == metadata.size);
        let runtime_resolved =
            resolved_storage.is_some() && (!registration_available || runtime_registered);
        let type_writable = runtime_resolved
            && can_write
            && resolved_storage.is_some_and(|c| can_write_component(ecs, c.component));
        let type_write_reason = if !can_write {
            Some(text::NO_WRITE_CONTEXT.to_owned())
        } else if runtime_resolved && !type_writable {
            Some(text::PHASE_NOT_VALIDATED.to_owned())
        } else {
            None
        };
        let resolved = runtime_resolved || template_registered;
        registered_count += usize::from(registered);
        runtime_count += usize::from(runtime_registered);
        template_count += usize::from(template_registered);
        resolved_count += usize::from(resolved);
        entry.raw_set("registered", registered)?;
        entry.raw_set("runtime_resolved", runtime_resolved)?;
        entry.raw_set(
            "registration_state",
            if registered {
                "registered"
            } else {
                "reflected-only"
            },
        )?;
        entry.raw_set(
            "storage",
            if runtime_registered {
                "entity"
            } else if template_registered {
                "template-only"
            } else {
                "unregistered"
            },
        )?;
        let role_entries = lua.create_table()?;
        for (row, role) in roles.into_iter().flatten() {
            let association = lua.create_table()?;
            association.raw_set("role", *role)?;
            association.raw_set("index", row.index)?;
            association.raw_set("qualified_name", name)?;
            role_entries.push(association)?;
        }
        entry.raw_set("registrations", role_entries)?;
        entry.raw_set("resolved", resolved)?;
        let codec_reason = validate_owned_type(&registry, metadata.index).err();
        entry.raw_set("partial_value", codec_reason.is_some())?;
        entry.raw_set("value_reason", codec_reason.clone())?;
        let readable = runtime_resolved && ready && read_denial.is_none() && read_contract;
        entry.raw_set("read_available", readable)?;
        entry.raw_set(
            "write_available",
            runtime_resolved
                && codec_reason.is_none()
                && type_writable
                && write_denial.is_none()
                && write_contract,
        )?;
        entry.raw_set("read_fields_available", readable)?;
        entry.raw_set(
            "write_fields_available",
            runtime_resolved && type_writable && write_denial.is_none() && write_contract,
        )?;
        entry.raw_set("read_reason", read_denial.clone())?;
        entry.raw_set(
            "write_fields_reason",
            write_denial.clone().or_else(|| type_write_reason.clone()),
        )?;
        entry.raw_set(
            "write_reason",
            write_denial.clone().or(codec_reason).or(type_write_reason),
        )?;
        entry.raw_set(
            "reason",
            if !registered && !runtime_resolved {
                Some(text::NOT_REGISTERED)
            } else if !resolved {
                Some(text::SIZES_DISAGREE)
            } else {
                None
            },
        )?;
        entries.push(entry)?;
    }
    result.raw_set("registry_available", registration_available)?;
    result.raw_set("registered_type_count", registered_count)?;
    result.raw_set("runtime_type_count", runtime_count)?;
    result.raw_set("template_type_count", template_count)?;
    result.raw_set("resolved_count", resolved_count)?;
    result.raw_set("reflected_only_count", entries.raw_len() - registered_count)?;
    result.raw_set("entity_access_ready", ready)?;
    result.raw_set("entity_writes_ready", can_write)?;
    result.raw_set("entries", entries)?;
    Ok(result)
}

/// One registration as ShroudForge's `ecs_registry_entry` gives it (read_component_registry.rs:33-107).
fn registration_entry(
    lua: &Lua,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    registry: &Registry,
    row: &Registration,
    detail: RegistrationDetail,
) -> mlua::Result<Table> {
    let ty = registry
        .get(row.type_index)
        .filter(|ty| ty.size == row.runtime_size)
        .ok_or_else(|| mlua::Error::runtime("engine component registry and parser disagree"))?;
    let entry = lua.create_table()?;
    entry.raw_set("index", row.index)?;
    entry.raw_set("qualified_name", ty.qualified_name.as_str())?;
    entry.raw_set("qualified_hash", row.qualified_hash)?;
    entry.raw_set("runtime_size", row.runtime_size)?;
    entry.raw_set("flags_bits", row.flags_bits)?;
    entry.raw_set("storage_flags_bits", detail.storage_flags_bits)?;
    entry.raw_set(
        "storage",
        if detail.has_storage {
            "entity"
        } else {
            "template-only"
        },
    )?;
    entry.raw_set("callbacks", lua.create_table()?)?;
    let has_storage = detail.has_storage;
    let mut codec_reason = None;
    if has_storage {
        entry.raw_set("runtime_type", ecs.type_value(lua, ty.index)?)?;
        codec_reason = validate_owned_type(registry, ty.index).err();
    }
    let type_writable = has_storage && can_write_component(ecs, row.qualified_hash);
    let phase_reason = (has_storage && !type_writable).then(|| text::PHASE_UNAVAILABLE.to_owned());
    // `available(op) && runtime_denial_reason(op).is_none()`: without a denial the phase, the capability and the
    // static contract hold, so `available` is the provider fact (check_ingame_access.rs:37-50).
    let facts = host.provider_facts();
    let readable = runtime_denial_reason(host, Op::EcsRead).is_none() && facts.ready;
    let writable = runtime_denial_reason(host, Op::EcsWrite).is_none() && facts.can_write;
    entry.raw_set("read_fields_available", has_storage && readable)?;
    entry.raw_set(
        "write_fields_available",
        has_storage && type_writable && writable,
    )?;
    entry.raw_set("read_available", has_storage && readable)?;
    entry.raw_set(
        "write_available",
        has_storage && type_writable && codec_reason.is_none() && writable,
    )?;
    entry.raw_set("partial_value", codec_reason.is_some())?;
    entry.raw_set("value_reason", codec_reason.clone())?;
    entry.raw_set("write_fields_reason", phase_reason.clone())?;
    entry.raw_set("write_reason", codec_reason.or(phase_reason))?;
    if !has_storage {
        entry.raw_set("reason", text::TEMPLATE_ONLY)?;
    }
    Ok(entry)
}

/// The report, as `registry()` gives it: the provider's text, or the snapshot.
fn report(ecs: &Ecs, registry: &Registry) -> Result<Rc<Snapshot>, String> {
    ecs.registry().read(&ecs.services, registry)
}

/// `runtime.ecs.get_registry()` (read_component_registry.rs:109-151).
pub fn get_registry(lua: &Lua, ecs: &Ecs, host: &dyn RuntimeHost) -> mlua::Result<Reason> {
    let registry = archive(ecs)?;
    let snapshot = match report(ecs, &registry) {
        Ok(snapshot) => snapshot,
        Err(reason) => return Ok((Value::Nil, Some(reason))),
    };
    let rows = match snapshot.available() {
        Ok(rows) => rows,
        Err(reason) => return Ok((Value::Nil, Some(reason.to_owned()))),
    };
    let result = lua.create_table()?;
    result.raw_set("schema_version", 1)?;
    result.raw_set("layout_version", 1)?;
    result.raw_set("source", text::SOURCE)?;
    result.raw_set("count", rows.len())?;
    result.raw_set("runtime_type_count", snapshot.runtime_type_count())?;
    let entries = lua.create_table()?;
    for row in rows {
        entries.push(registration_entry(
            lua,
            ecs,
            host,
            &registry,
            row,
            RegistrationDetail::PART_A,
        )?)?;
    }
    result.raw_set("entries", entries)?;
    Ok((Value::Table(result), None))
}

/// `runtime.ecs.get_component(selector)` (read_component_registry.rs:153-180): the selector first, then the report.
pub fn get_component(
    lua: &Lua,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    selector: Value,
) -> mlua::Result<Reason> {
    let registry = archive(ecs)?;
    let index = match selectors::resolve(&registry, &selector) {
        Ok(index) => index,
        Err(reason) => return Ok((Value::Nil, Some(reason))),
    };
    let name = registry
        .get(index)
        .map(|ty| ty.qualified_name.clone())
        .unwrap_or_default();
    let snapshot = match report(ecs, &registry) {
        Ok(snapshot) => snapshot,
        Err(reason) => return Ok((Value::Nil, Some(reason))),
    };
    // an unavailable report has no entries
    for row in snapshot.available().unwrap_or_default() {
        // part A: the registration's name is its storage type's
        if registry
            .get(row.type_index)
            .is_some_and(|ty| ty.qualified_name == name)
        {
            let entry =
                registration_entry(lua, ecs, host, &registry, row, RegistrationDetail::PART_A)?;
            return Ok((Value::Table(entry), None));
        }
    }
    Ok((Value::Nil, Some(format!("{}{name}", text::NO_REGISTRATION))))
}

/// `runtime.ecs.get_components()` (read_component_registry.rs:3-31): the query denial, readiness, then every
/// `keen::ecs::` type whose storage resolves with its reflected size, in registry order.
pub fn get_components(lua: &Lua, ecs: &Ecs, host: &dyn RuntimeHost) -> mlua::Result<Reason> {
    if let Some(reason) = runtime_denial_reason(host, Op::EcsQuery) {
        return Ok((Value::Nil, Some(reason)));
    }
    if !host.provider_facts().ready {
        return Ok((Value::Nil, Some(text::NOT_READY.to_owned())));
    }
    let registry = archive(ecs)?;
    let snapshot = report(ecs, &registry).ok();
    let result = lua.create_table()?;
    for metadata in registry
        .iter()
        .filter(|ty| ty.qualified_name.starts_with(super::registry::ECS_PREFIX))
    {
        let Some(component) = snapshot
            .as_ref()
            .and_then(|s| s.resolve(&metadata.qualified_name))
        else {
            continue;
        };
        if component.size != metadata.size {
            continue;
        }
        let value = ecs.type_value(lua, metadata.index)?;
        if !value.is_nil() {
            result.push(value)?;
        }
    }
    Ok((Value::Table(result), None))
}
