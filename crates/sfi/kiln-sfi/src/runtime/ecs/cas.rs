// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/game-runtime/bridge/windows/ecs/{update_component.rs:5-41 (compare_exchange: the
// write context first, then the texts), check_component_write_access.rs:4-18}; modules/mod-runtime/lua/ingame/ecs/
// write_component.rs:2-220 (write, collect_changed_ranges); lua/values/validate_runtime_scalar.rs:4-18;
// lua/ingame/ecs/attributes/{recalculate_component_update.rs:9-31,check_attribute_component.rs:4-17};
// lua/ingame/overrides/claim_runtime_storage.rs:123-139 (check_write).
//! The compare-exchange core of the component writes, and `runtime.ecs.write`.
//!
//! **The core** ([`Ecs::compare_exchange`]): ShroudForge's provider `compare_exchange(entity, name, expected, mask,
//! desired)` on kiln.world's `ecs_compare_exchange` (the whole component compared with `expected` and, when equal,
//! `desired` written under `mask` in one hold of the engine's gate, between frames, read back):
//! - first the write context: kiln.world's `ecs_write_policy` for the component (ShroudForge's
//!   `can_write_component`); then the member, the buffer sizes and the handle;
//! - **conflicts**: a CONFLICT whose current bytes (`confirmed`) differ from `expected` only where the caller's change
//!   does not depend on them (the frame wrote other fields) is tried again against those bytes, at most three holds in
//!   all; the third conflict answers the session latch's busy text (`ecs-scene-frame-busy`, not latching). A conflict
//!   on a byte the caller depends on (a field it writes or expected, another writer's change) is ShroudForge's
//!   "Attribute snapshot changed before update" at once: such a write is never retried over a foreign write;
//! - **busy** (a frame holds the gate): the busy text at once, never retried within the call;
//! - **uncertain** (a write that could not be proven): reported to the session write latch, which latches it
//!   (`ecs-write-outcome-uncertain-rollback-unverified`): every later write of the session is denied with it;
//! - not writable or unavailable: ShroudForge's write-context text; stale, absent, unreadable or any other refusal: its
//!   "guarded attribute update failed".
//!
//! **`runtime.ecs.write`**: ShroudForge's order (the denial, the contract, readiness, the component, its metadata, the
//! size proof, an owned-value type, the mapped value `runtime.ecs.read` gave and its source bytes, the value serialized
//! as the component by the VM's typed values, the changed ranges of reflected scalar leaves only (a changed dynamic
//! field is an error; a float must stay finite), the attribute rule, the ownership check), then the core over the
//! changed ranges.
//!
//! **Attribute components** (a type whose bases include `keen::ecs::AttributeComponent`) go through ShroudForge's
//! attribute recalculation before the compare-exchange (`super::attributes::writes::recalculate_component_update`: the
//! archive's catalog, the profile's published calculation model), which depends on the whole component; in a VM without
//! the attribute family they answer ShroudForge's text for a provider without the function inventory
//! ([`text::ATTRIBUTES`]).

use super::catalog::text::NOT_READY;
use super::entities::text::COMPONENT_UNAVAILABLE;
use super::{Ecs, runtime_type_name};
use crate::runtime::overrides::{ClaimAuthority, check_write, component_key};
use crate::runtime::types::{Registry, selectors};
use crate::runtime::values::proxies::{MappedStructValue, lease_of};
use crate::runtime::write_fault::WriteOutcome;
use crate::service_member;
use kiln_abi::services::kiln_world::{
    KILN_ECS_BUSY, KILN_ECS_CONFLICT, KILN_ECS_NOT_WRITABLE, KILN_ECS_OK, KILN_ECS_UNAVAILABLE,
    KILN_ECS_UNCERTAIN, KILN_MUTATION_CONFIRMED, KILN_WORLD_SERVICE, KILN_WORLD_VERSION,
    KilnEcsWritePolicy, KilnMutationReply, KilnWorldApi,
};
use kiln_abi::{KILN_ERR_INVALID_ARGUMENT, KILN_OK};
use mlua::{Lua, Value};
use sfi_core::bindings::runtime::{
    Availability, RuntimeHost, RuntimeOperation as Op, runtime_denial_reason,
};
use sfi_core::lua::{FunctionArgs, LuaError};

/// ShroudForge's texts.
pub mod text {
    /// update_component.rs:13 (and access_component_fields.rs:102).
    pub use crate::runtime::ecs::catalog::text::PHASE_UNAVAILABLE as NO_WRITE_CONTEXT;
    /// update_component.rs:17.
    pub const NO_GUARDED_UPDATES: &str = "native runtime lacks guarded attribute updates";
    /// update_component.rs:20.
    pub const SIZES: &str = "attribute update buffer sizes disagree";
    /// update_component.rs:33-35.
    pub const CONFLICT: &str =
        "Attribute snapshot changed before update. Read current values and retry";
    /// update_component.rs:39.
    pub const FAILED: &str = "guarded attribute update failed";
    /// read_function_catalog.rs:11: the attribute recalculation's function inventory (a VM without the attribute
    /// family).
    pub const ATTRIBUTES: &str = "native provider lacks the function inventory extension";
    /// validate_runtime_scalar.rs:15.
    pub const NOT_FINITE: &str = "runtime field writes require finite floating-point values";
}

/// The most holds one compare-exchange takes when only bytes its caller does not depend on changed.
pub const HOLDS: usize = 3;

/// The base type of ShroudForge's attribute components.
pub const ATTRIBUTE_COMPONENT: &str = "keen::ecs::AttributeComponent";

/// ShroudForge's `validate_runtime_scalar` (validate_runtime_scalar.rs:4-18): a written float must stay finite.
pub fn validate_runtime_scalar(kind: &str, bytes: &[u8]) -> Result<(), String> {
    let finite =
        match kind {
            "Float32" => f32::from_le_bytes(bytes.try_into().map_err(|_| "invalid float32 width")?)
                .is_finite(),
            "Float64" => f64::from_le_bytes(bytes.try_into().map_err(|_| "invalid float64 width")?)
                .is_finite(),
            _ => true,
        };
    if finite {
        Ok(())
    } else {
        Err(text::NOT_FINITE.into())
    }
}

/// ShroudForge's `is_attribute_component` (check_attribute_component.rs:4-17).
pub fn is_attribute_component(registry: &Registry, name: &str) -> Result<bool, String> {
    let ty = registry
        .by_qualified_name(name)
        .ok_or_else(|| format!("unresolved component type: {name}"))?;
    Ok(selectors::inheritance(registry, ty.index)?
        .iter()
        .any(|index| {
            registry
                .get(*index)
                .is_some_and(|base| base.qualified_name == ATTRIBUTE_COMPONENT)
        }))
}

/// ShroudForge's `collect_changed_ranges` (write_component.rs:118-220): the byte ranges of reflected scalar leaves
/// of `index` (at `base`) where `candidate` differs from `source`, adjacent ranges joined. A changed dynamic field
/// or reference is an error, and so is a changed float that is not finite.
pub fn collect_changed_ranges(
    registry: &Registry,
    index: usize,
    base: usize,
    source: &[u8],
    candidate: &[u8],
    ranges: &mut Vec<(usize, usize)>,
) -> Result<(), String> {
    let ty = registry
        .get(index)
        .ok_or("unresolved runtime layout type")?;
    let name = &ty.qualified_name;
    let end = base
        .checked_add(ty.size as usize)
        .ok_or_else(|| format!("runtime layout overflow: {name}"))?;
    if end > source.len() || end > candidate.len() {
        return Err(format!("runtime layout outside component: {name}"));
    }
    match ty.primitive_type {
        "Typedef" => {
            let inner = ty
                .inner_type
                .ok_or_else(|| format!("missing inner type: {name}"))?;
            collect_changed_ranges(registry, inner, base, source, candidate, ranges)
        }
        "Struct" => {
            if let Some(parent) = ty.inner_type {
                collect_changed_ranges(registry, parent, base, source, candidate, ranges)?;
            }
            for field in &ty.struct_fields {
                if registry.get(field.type_index).is_none() {
                    return Err(format!("missing field type: {name}.{}", field.name));
                }
                let offset = usize::try_from(field.data_offset)
                    .map_err(|_| format!("invalid field offset: {name}.{}", field.name))?;
                collect_changed_ranges(
                    registry,
                    field.type_index,
                    base + offset,
                    source,
                    candidate,
                    ranges,
                )?;
            }
            Ok(())
        }
        "StaticArray" => {
            let inner = ty
                .inner_type
                .and_then(|i| registry.get(i))
                .ok_or_else(|| format!("missing array element type: {name}"))?;
            for position in 0..ty.field_count as usize {
                collect_changed_ranges(
                    registry,
                    inner.index,
                    base + position * inner.size as usize,
                    source,
                    candidate,
                    ranges,
                )?;
            }
            Ok(())
        }
        kind if super::fields::EXTERNAL.contains(&kind) => {
            if source[base..end] != candidate[base..end] {
                return Err(format!(
                    "runtime write for dynamic field type is not supported: {name}"
                ));
            }
            Ok(())
        }
        kind => {
            if source[base..end] != candidate[base..end] {
                validate_runtime_scalar(kind, &candidate[base..end])?;
                match ranges.last_mut().filter(|(_, previous)| *previous == base) {
                    Some((_, previous)) => *previous = end,
                    None => ranges.push((base, end)),
                }
            }
            Ok(())
        }
    }
}

/// A package's ownership identity for ShroudForge's `check_write`: its id and the family's world session.
pub(crate) struct Writer<'a> {
    pub host: &'a dyn RuntimeHost,
    pub session: u64,
}
impl ClaimAuthority for Writer<'_> {
    fn owner_id(&self) -> &str {
        self.host.owner_context().id()
    }
    fn world_session(&self) -> u64 {
        self.session
    }
    fn denial(&self, operation: Op) -> Option<String> {
        runtime_denial_reason(self.host, operation)
    }
    fn available(&self, operation: Op) -> bool {
        super::available(self.host, operation)
    }
}

impl Ecs {
    /// ShroudForge's `runtime_provider::can_write_component` for component hash `component`: kiln.world's
    /// `ecs_write_policy` answers OK with status `KILN_ECS_OK`. A missing member or any other answer is not writable.
    pub fn can_write_component(&self, component: u32) -> bool {
        let Ok(table) = self
            .services
            .bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)
        else {
            return false;
        };
        let Ok(policy) = service_member!(table, KilnWorldApi, ecs_write_policy) else {
            return false;
        };
        // SAFETY: an all-zero policy record is valid (integers and a char array).
        let mut out: KilnEcsWritePolicy = unsafe { std::mem::zeroed() };
        out.size = std::mem::size_of::<KilnEcsWritePolicy>() as u32;
        // SAFETY: plain values and a sized caller record.
        let result = unsafe {
            self.services
                .call(policy, (self.world_kind(), component, &raw mut out))
        };
        result == KILN_OK && out.status == KILN_ECS_OK as u32
    }

    /// The session write latch's text for `outcome` of a write in `session` (latching when its row says so).
    fn report(&self, session: u64, outcome: WriteOutcome) -> String {
        match self.runtime() {
            Some(runtime) => runtime
                .facts
                .write_faults()
                .report(self.world_kind(), session, outcome)
                .to_owned(),
            None => crate::runtime::write_fault::reason(outcome)
                .reason
                .to_owned(),
        }
    }

    /// The compare-exchange core (module docs): `desired` over the component of the entity `handle` names, under
    /// `mask`, when its bytes are `expected`; `depends` marks (nonzero) the bytes the caller's change was decided on.
    /// Every buffer is the component's size.
    pub fn compare_exchange(
        &self,
        handle: u32,
        component: u32,
        expected: &[u8],
        mask: &[u8],
        desired: &[u8],
        depends: &[u8],
    ) -> Result<(), String> {
        if !self.can_write_component(component) {
            return Err(text::NO_WRITE_CONTEXT.into());
        }
        let exchange = self
            .world()
            .and_then(|table| service_member!(table, KilnWorldApi, ecs_compare_exchange))
            .map_err(|_| text::NO_GUARDED_UPDATES)?;
        let size = desired.len();
        if expected.len() != size || mask.len() != size || depends.len() != size {
            return Err(text::SIZES.into());
        }
        let object = self.object(handle).ok_or(text::FAILED)?;
        let mut expected = expected.to_vec();
        for hold in 1..=HOLDS {
            let mut confirmed = vec![0u8; size];
            // SAFETY: an all-zero reply is valid (integers only).
            let mut reply: KilnMutationReply = unsafe { std::mem::zeroed() };
            reply.size = std::mem::size_of::<KilnMutationReply>() as u32;
            // SAFETY: kiln_world.h ecs_compare_exchange: the caller's ref, four buffers of `size` bytes and a sized
            // reply record.
            let result = unsafe {
                self.services.call(
                    exchange,
                    (
                        self.owner(),
                        &raw const object,
                        component,
                        expected.as_ptr().cast::<core::ffi::c_void>(),
                        mask.as_ptr().cast::<core::ffi::c_void>(),
                        desired.as_ptr().cast::<core::ffi::c_void>(),
                        size as u32,
                        confirmed.as_mut_ptr().cast::<core::ffi::c_void>(),
                        &raw mut reply,
                    ),
                )
            };
            if result == KILN_ERR_INVALID_ARGUMENT {
                return Err(text::FAILED.into());
            }
            let session = if reply.session != 0 {
                reply.session
            } else {
                object.session
            };
            match reply.status as i32 {
                KILN_ECS_OK if result == KILN_OK => return Ok(()),
                KILN_ECS_CONFLICT => {
                    let seen = reply.flags & KILN_MUTATION_CONFIRMED as u32 != 0;
                    let foreign = (0..size).any(|i| depends[i] != 0 && confirmed[i] != expected[i]);
                    if !seen || foreign {
                        return Err(text::CONFLICT.into());
                    }
                    if hold == HOLDS {
                        return Err(self.report(session, WriteOutcome::EcsWriteConflictExhausted));
                    }
                    // the frame changed bytes the change does not depend on: compare against them next hold
                    expected = confirmed;
                }
                KILN_ECS_BUSY => return Err(self.report(session, WriteOutcome::EcsWriteBusy)),
                KILN_ECS_UNCERTAIN => {
                    return Err(self.report(session, WriteOutcome::EcsWriteUncertain));
                }
                KILN_ECS_NOT_WRITABLE | KILN_ECS_UNAVAILABLE => {
                    return Err(text::NO_WRITE_CONTEXT.into());
                }
                _ => return Err(text::FAILED.into()),
            }
        }
        unreachable!("the last hold answers")
    }
}

/// ShroudForge's `mapped_source_bytes`: the source bytes of the mapped struct `runtime.ecs.read` gave, else None.
fn mapped_source_bytes(ecs: &Ecs, value: &Value) -> mlua::Result<Option<(u64, Vec<u8>)>> {
    let Value::UserData(userdata) = value else {
        return Ok(None);
    };
    if !userdata.is::<MappedStructValue>() {
        return Ok(None);
    }
    let Some(lease) = lease_of(userdata) else {
        return Ok(None);
    };
    Ok(ecs
        .values()?
        .source_bytes(lease)?
        .map(|bytes| (lease, bytes)))
}

/// `runtime.ecs.write(entity, component, value)` (write_component.rs:2-116): `true` or `false, reason`.
pub fn write(
    lua: &Lua,
    ecs: &Ecs,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<(bool, Option<String>)> {
    let refused = |reason: String| Ok((false, Some(reason)));
    if let Some(reason) = runtime_denial_reason(host, Op::EcsWrite) {
        return refused(reason);
    }
    if let Availability::Unavailable { reason } = host.static_support(Op::EcsWrite) {
        return refused(reason);
    }
    if !host.provider_facts().ready {
        return refused(NOT_READY.into());
    }
    let entity = args.get::<u32>(0)?;
    let name = runtime_type_name(ecs, args, 1)?;
    let value = args.get::<Value>(2)?.clone();
    let Some(component) = ecs.resolve_component(&name) else {
        return refused(format!("{COMPONENT_UNAVAILABLE}{name}"));
    };
    let registry = ecs.types().clone();
    let Some(metadata) = registry.by_qualified_name(&name) else {
        return refused(format!("type metadata unavailable: {name}"));
    };
    if metadata.size != component.size {
        return refused(format!(
            "component layout mismatch for {name}: parser={}, runtime={}",
            metadata.size, component.size
        ));
    }
    if let Err(reason) = super::catalog::validate_owned_type(&registry, metadata.index) {
        return refused(reason);
    }
    let Some((lease, source)) = mapped_source_bytes(ecs, &value)? else {
        return refused(format!(
            "runtime.ecs.write requires the value returned by runtime.ecs.read for {name}"
        ));
    };
    if source.len() != component.size as usize {
        return refused(format!("runtime source size mismatch: {name}"));
    }
    // validate_and_clone, convert and to_bytes as the component's type (raised as ShroudForge raises them)
    let bytes = ecs.values()?.export(lease, &name)?;
    if bytes.len() != component.size as usize {
        return refused(format!("serialized component size mismatch: {name}"));
    }
    let mut ranges = Vec::new();
    collect_changed_ranges(&registry, metadata.index, 0, &source, &bytes, &mut ranges)
        .map_err(LuaError::generic)?;
    if ranges.is_empty() {
        return Ok((true, None));
    }
    let mut mask = vec![0u8; bytes.len()];
    for (start, end) in ranges {
        mask[start..end].fill(1);
    }
    let mut merged: Vec<u8> = bytes
        .iter()
        .zip(&source)
        .zip(&mask)
        .map(|((new, old), selected)| if *selected != 0 { *new } else { *old })
        .collect();
    // an attribute component's candidate is recalculated as ShroudForge's recalculate_component_update does
    let attribute = match super::attributes::writes::recalculate_component_update(
        lua,
        ecs,
        &name,
        &source,
        &mut merged,
        &mut mask,
    ) {
        Ok(attribute) => attribute,
        Err(reason) => return refused(reason),
    };
    let writer = Writer {
        host,
        session: ecs.world_session(),
    };
    if let Some(runtime) = ecs.runtime() {
        let key = component_key(&writer, entity, &name);
        if let Err(reason) = check_write(runtime.facts.ownership(), &writer, &key) {
            return refused(reason);
        }
    }
    // an attribute write depends on the whole component (its header, identity and root were checked on these bytes)
    let depends = if attribute {
        vec![1u8; mask.len()]
    } else {
        mask.clone()
    };
    Ok(
        match ecs.compare_exchange(
            entity,
            component.component,
            &source,
            &mask,
            &merged,
            &depends,
        ) {
            Ok(()) => (true, None),
            Err(reason) => (false, Some(reason)),
        },
    )
}
