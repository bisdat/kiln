// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/ecs/attributes/{mod,register_attribute_api}.rs (the members
// and their installation), reflection/build_attribute_catalog.rs (read_header, bind_storage, the catalog's indexes).
//! ShroudForge's attribute members of `runtime.ecs`: the archive's attribute catalog (`get_attributes`,
//! `get_attribute`, `evaluate_attributes`), an entity's attribute storage (`read_attributes`, `read_attribute`), the
//! attribute writes (`update_attribute`, `write_attribute_storage`) and `runtime.overrides.attribute`.
//!
//! - [`catalog`]: the catalog of the archive generation granted to this start, read once per VM through
//!   kiln.eml.archive's attribute group (`attribute_catalog`; the recalculation of a root is `attribute_evaluate`): the
//!   GPL provider builds it with ShroudForge's own code; only counted JSON and bits cross C. Its first answer, a failure
//!   too, is the VM's for good, as ShroudForge's session catalog is.
//! - [`storage`]: an entity's attribute storage: the candidate components of the descriptor's root (a reflected layout
//!   of the root's size that kiln.types' join resolves with the same size), one checked batch read of them
//!   (kiln.world's `ecs_snapshot_many`, absence proven), the one whose initialized header names the root, and the
//!   descriptor bound to it.
//! - [`members`]: the read members, in ShroudForge's order of checks, its texts and return shapes.
//! - [`writes`]: `update_attribute`, `write_attribute_storage` and the attribute recalculation the plain component
//!   writes run, on kiln.world's compare-exchange; `crate::runtime::overrides::targets::attribute` is
//!   `runtime.overrides.attribute`, ShroudForge's attribute override target over these members.
//!
//! The family ([`family`]) shares one [`catalog::Attributes`] between the packages of one VM and works on the VM's
//! shared [`super::Ecs`] (its entity handles are the ones `runtime.ecs.query` gives), so it is listed after the ECS
//! family.

pub mod catalog;
pub mod members;
pub mod storage;
pub mod writes;

use super::Ecs;
use crate::runtime::context::RuntimeContext;
use crate::runtime::install::Family;
use catalog::Attributes;
use kiln_abi::services::kiln_eml_archive::KilnEmlArchiveIdentityV1;
use mlua::{Lua, Table};
use sfi_core::bindings::runtime::RuntimeHost;
use std::rc::Rc;

/// The family's name.
pub const FAMILY: &str = "ecs-attributes";

/// The names this family adds inside the installer's `runtime.ecs` and `runtime.overrides` containers.
pub const CLAIMS: [&str; 8] = [
    "runtime.ecs.get_attributes",
    "runtime.ecs.get_attribute",
    "runtime.ecs.evaluate_attributes",
    "runtime.ecs.read_attributes",
    "runtime.ecs.read_attribute",
    "runtime.ecs.update_attribute",
    "runtime.ecs.write_attribute_storage",
    "runtime.overrides.attribute",
];

/// SF `uint`: a JSON number that fits u32.
pub fn uint(value: &serde_json::Value) -> Result<u32, String> {
    value
        .as_u64()
        .and_then(|v| v.try_into().ok())
        .ok_or_else(|| "expected u32 attribute data".into())
}

/// The VM's shared [`Attributes`], as Lua app data.
struct VmAttributes(Rc<Attributes>);

/// The VM's attributes, made by the first package's install (the plain component writes recalculate attribute
/// components with them).
pub fn of_vm(lua: &Lua) -> Option<Rc<Attributes>> {
    lua.app_data_ref::<VmAttributes>().map(|a| a.0.clone())
}

/// Makes `attributes` the VM's.
pub fn set_vm(lua: &Lua, attributes: Rc<Attributes>) {
    lua.set_app_data(VmAttributes(attributes));
}

/// Puts the members into one package's `runtime.ecs` table; `host` is that package's registrar host.
pub fn install_into(
    lua: &Lua,
    ecs_table: &Table,
    host: Rc<dyn RuntimeHost>,
    ecs: Rc<Ecs>,
    attributes: Rc<Attributes>,
) -> mlua::Result<()> {
    let a = attributes.clone();
    let e = ecs.clone();
    ecs_table.raw_set(
        "get_attributes",
        lua.create_function(move |lua, ()| members::get_attributes(lua, &a, &e))?,
    )?;
    let a = attributes.clone();
    ecs_table.raw_set(
        "get_attribute",
        lua.create_function(move |lua, selector: mlua::Value| {
            members::get_attribute(lua, &a, selector)
        })?,
    )?;
    let a = attributes.clone();
    ecs_table.raw_set(
        "evaluate_attributes",
        lua.create_function(move |lua, (selector, values): (mlua::Value, Table)| {
            members::evaluate_attributes(lua, &a, selector, values)
        })?,
    )?;
    let (a, e, h) = (attributes.clone(), ecs.clone(), host.clone());
    ecs_table.raw_set(
        "read_attributes",
        lua.create_function(move |lua, args: mlua::MultiValue| {
            members::read_attributes(lua, &a, &e, h.as_ref(), args)
        })?,
    )?;
    let (a, e, h) = (attributes.clone(), ecs.clone(), host.clone());
    ecs_table.raw_set(
        "read_attribute",
        lua.create_function(move |lua, args: mlua::MultiValue| {
            members::read_attribute(lua, &a, &e, h.as_ref(), args)
        })?,
    )?;
    let (a, e, h) = (attributes.clone(), ecs.clone(), host.clone());
    ecs_table.raw_set(
        "update_attribute",
        lua.create_function(move |lua, args: mlua::MultiValue| {
            writes::update_attribute(lua, &a, &e, h.as_ref(), args)
        })?,
    )?;
    let (a, e, h) = (attributes, ecs, host);
    ecs_table.raw_set(
        "write_attribute_storage",
        lua.create_function(move |lua, args: mlua::MultiValue| {
            writes::write_attribute_storage(lua, &a, &e, h.as_ref(), args)
        })?,
    )?;
    Ok(())
}

/// The production family over the archive generation `identity` (the one `game.types` reads). A package without a
/// `runtime` namespace gets nothing.
pub fn family(identity: KilnEmlArchiveIdentityV1) -> Family {
    Family::new(
        FAMILY,
        &CLAIMS,
        move |lua, _, _, composed, cx: &RuntimeContext| {
            let (Some(runtime), Some(host)) = (&composed.runtime, cx.package_host()) else {
                return Ok(());
            };
            let ecs = super::of_vm(lua).ok_or_else(|| {
                mlua::Error::runtime(
                    "kiln: the attribute members need the ECS family installed before them",
                )
            })?;
            let attributes = match of_vm(lua) {
                Some(attributes) => attributes,
                None => {
                    let attributes =
                        Rc::new(Attributes::new(cx.services().clone(), cx.owner(), identity));
                    set_vm(lua, attributes.clone());
                    attributes
                }
            };
            let table: Table = runtime.raw_get("ecs")?;
            install_into(lua, &table, host, ecs, attributes)?;
            crate::runtime::overrides::targets::attribute::install_into(lua, runtime)
        },
    )
}
