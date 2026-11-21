// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (identical in both pins): modules/mod-runtime/lua/ingame/overrides/
// create_override_targets.lua:102-160,163-197,199-232,234-260 (the target tables each kind
// returns), track_runtime_overrides.lua:25-29,37-66,68-149 (what the tracker reads of them).
//! The generic override target.
//!
//! ShroudForge's tracker takes any table with `claim`, `read` and `write`, plus the optional
//! `key`, `shared` and `normalize`. Each family's target adapter (component, service,
//! resource, template, attribute) implements [`ClaimTarget`];
//! [`target_table`] gives it ShroudForge's Lua shape:
//! - `key`: the tracker key (`component/<session>/<entity>/<type>/<path>`, ...);
//! - `claim`: the target's `RuntimeWriteClaim` userdata;
//! - `shared`: `true` for storage that outlives one mod's world (resources, services,
//!   templates), absent otherwise;
//! - `read()`: the snapshot table (`value` plus the kind's own fields) or `nil, reason`;
//! - `write(snapshot, value)`: `true, actual` or `true, actual, nil, canonical`, or
//!   `false, nil, reason`;
//! - `normalize(snapshot, value)`: the canonical value or `nil, reason`; absent when the
//!   target does not normalize.
//!
//! A Lua error a body raises propagates (the tracker's `pcall` sees it).

use super::claims::RuntimeWriteClaim;
use mlua::{AnyUserData, Lua, MultiValue, Table, Value};
use std::rc::Rc;

/// A confirmed write: what the storage holds now and, when the target computed it, the
/// canonical value the tracker compares with.
pub struct Written {
    pub actual: Value,
    pub canonical: Option<Value>,
}

/// One override target of a family.
pub trait ClaimTarget {
    /// The tracker key.
    fn key(&self) -> String;
    /// The target's claim (a `RuntimeWriteClaim` userdata made by a claim factory).
    fn claim(&self) -> AnyUserData;
    /// Whether the storage is shared beyond one world (the tracker's fault rule).
    fn shared(&self) -> bool {
        false
    }
    /// A fresh snapshot: a table with `value`, or the refusal.
    fn read(&self, lua: &Lua) -> mlua::Result<Result<Table, String>>;
    /// Writes `value` over `snapshot`.
    fn write(
        &self,
        lua: &Lua,
        snapshot: Table,
        value: Value,
    ) -> mlua::Result<Result<Written, String>>;
    /// Whether [`ClaimTarget::normalize`] is offered (`normalize` present in the table).
    fn normalizes(&self) -> bool {
        false
    }
    /// The canonical form of `value` for `snapshot`, or the refusal.
    fn normalize(
        &self,
        _lua: &Lua,
        _snapshot: Table,
        value: Value,
    ) -> mlua::Result<Result<Value, String>> {
        Ok(Ok(value))
    }
}

fn refusal(lua: &Lua, leading: Value, reason: String) -> mlua::Result<MultiValue> {
    Ok(MultiValue::from_vec(vec![
        leading,
        Value::Nil,
        Value::String(lua.create_string(reason)?),
    ]))
}

/// The target's Lua table. Refused when its claim is not a `RuntimeWriteClaim`.
pub fn target_table(lua: &Lua, target: Rc<dyn ClaimTarget>) -> mlua::Result<Table> {
    let claim = target.claim();
    if !claim.is::<RuntimeWriteClaim>() {
        return Err(mlua::Error::runtime(
            "an override target's claim must be a RuntimeWriteClaim",
        ));
    }
    let table = lua.create_table()?;
    table.raw_set("key", target.key())?;
    table.raw_set("claim", claim)?;
    if target.shared() {
        table.raw_set("shared", true)?;
    }
    let reader = target.clone();
    table.raw_set(
        "read",
        lua.create_function(move |lua, ()| {
            Ok(match reader.read(lua)? {
                Ok(snapshot) => MultiValue::from_vec(vec![Value::Table(snapshot)]),
                Err(reason) => MultiValue::from_vec(vec![
                    Value::Nil,
                    Value::String(lua.create_string(reason)?),
                ]),
            })
        })?,
    )?;
    let writer = target.clone();
    table.raw_set(
        "write",
        lua.create_function(move |lua, (snapshot, value): (Table, Value)| {
            match writer.write(lua, snapshot, value)? {
                Ok(Written {
                    actual,
                    canonical: None,
                }) => Ok(MultiValue::from_vec(vec![Value::Boolean(true), actual])),
                Ok(Written {
                    actual,
                    canonical: Some(canonical),
                }) => Ok(MultiValue::from_vec(vec![
                    Value::Boolean(true),
                    actual,
                    Value::Nil,
                    canonical,
                ])),
                Err(reason) => refusal(lua, Value::Boolean(false), reason),
            }
        })?,
    )?;
    if target.normalizes() {
        let normalizer = target.clone();
        table.raw_set(
            "normalize",
            lua.create_function(move |lua, (snapshot, value): (Table, Value)| {
                Ok(match normalizer.normalize(lua, snapshot, value)? {
                    Ok(value) => MultiValue::from_vec(vec![value]),
                    Err(reason) => MultiValue::from_vec(vec![
                        Value::Nil,
                        Value::String(lua.create_string(reason)?),
                    ]),
                })
            })?,
        )?;
    }
    Ok(table)
}
