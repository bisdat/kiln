// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/player_input/read_building_cursor.rs:1-262 (cursor.get,
// its reflected record fallback reflected_local_cursor and reflected_cursor_unsigned), game-runtime/bridge/windows/
// building/read_cursor.rs:12-48 (the texts of each outcome), game-runtime/native/player-input/
// read_local_cursor_input.cpp:229-312 (ReadLocalCursorInput: one stable copy of the local ClientPlayerInput, a sequence
// per published copy, LocalCursorInputAvailable), engine-context/track_world_context.cpp:189-190.
//! `runtime.world.cursor.get`: the local player's building cursor, read from its `ClientPlayerInput`.
//!
//! ShroudForge 1.7.9 has two cursor paths: a hook mailbox, which needs its profile's `hooks` (no profile of a build
//! from 25966634 on has them), and the reflected record fallback, which reads the local `ClientPlayerInput` component
//! and decodes its `data.cursorInput` fields by their reflected offsets. Kiln implements the fallback, with no new
//! native member:
//! - **the entity**: kiln.client's `state` names the local player (`local_player` with `KILN_CLIENT_LOCAL_PLAYER`).
//!   ShroudForge scans the client world for at most 32 `ClientPlayerInput`s and picks one by a client-only marker; Kiln
//!   asks for the local one, so its "ambiguous" answer is never given;
//! - **the copy**: kiln.world's `entity_open` of that entity in the client world (always the client world, whichever
//!   world the VM's shared ECS reads: on a host that is the server world), in its live session, then one stable copy of
//!   the component (`ecs_snapshot`: copied twice between identity checks, a value that changes during the read is
//!   refused as unreadable). The world session marker and kiln.client's state are read again after the copy: a
//!   changed marker is "session changed", a changed local player or world generation "retry";
//! - **the sequence**: each published copy takes the next number of one process-wide counter (never 0), as
//!   ShroudForge's reader numbers every published sample;
//! - **the decode**: each field through the VM's inline field plans ([`super::super::ecs::Ecs::inline_field`]) and
//!   Rust decoding of its scalar leaf: positions are integers, rotations and scales finite floats, flags, the material
//!   and the selection's version and object id unsigned scalars of their reflected storage width (typedefs and enums
//!   looked through), `isCursorAttachedToProp` a boolean. Every text is ShroudForge's.
//!
//! Its readiness (the world-operation fact, [`read_available`]) is ShroudForge's `LocalCursorInputAvailable`: a
//! client world kiln.world reads with an exact profile, in a live session, and a known local player.

use crate::runtime::ecs::{Ecs, of_vm, probe_reads, ready};
use crate::runtime::install::Family;
use crate::runtime::services::{ServiceAdapter, ServiceError};
use crate::runtime::types::registry::fnv1a32;
use crate::service_member;
use kiln_abi::services::kiln_client::{
    KILN_CLIENT_LOCAL_PLAYER, KILN_CLIENT_SERVICE, KILN_CLIENT_VERSION, KilnClientApi,
    KilnClientState,
};
use kiln_abi::services::kiln_world::{
    KILN_ECS_ERR_UNREADABLE, KILN_WORLD_ERR_CHANGED, KilnObjectRef, KilnWorldApi,
};
use kiln_abi::{KILN_ERR_BUFFER_TOO_SMALL, KILN_OK, KILN_WORLD_CLIENT};
use mlua::{Lua, Table, Value};
use sfi_core::bindings::runtime::{
    RuntimeHost, RuntimeOperation as Op, RuntimeProviderFacts, runtime_denial_reason,
};
use sfi_core::lua::{FunctionArgs, LuaError};
use std::rc::Rc;
use std::sync::atomic::{AtomicU64, Ordering};

/// The family's name.
pub const FAMILY: &str = "world.cursor";

/// The name it adds inside the installer's `runtime.world.cursor` container.
pub const CLAIMS: [&str; 1] = ["runtime.world.cursor.get"];

/// The component the cursor is read from.
pub const INPUT: &str = "keen::ecs::ClientPlayerInput";

/// The largest component the reader copies (read_local_cursor_input.cpp:19).
pub const MAX_INPUT: usize = 65_536;

/// The client world, where the local input lives.
pub const CLIENT_WORLD: u32 = KILN_WORLD_CLIENT as u32;

/// ShroudForge's texts.
pub mod text {
    /// read_building_cursor.rs:15.
    pub const ARGUMENTS: &str = "runtime.world.cursor.get does not accept arguments";
    /// read_building_cursor.rs:98.
    pub const REFLECTION: &str = "local ClientPlayerInput reflection is unavailable";
    /// read_cursor.rs:17.
    pub const SIZE: &str = "local cursor component size is outside the supported bounds";
    /// read_cursor.rs:21.
    pub const WAITING: &str = "local cursor is waiting for the current world session";
    /// read_cursor.rs:37.
    pub const SESSION: &str = "local cursor world session changed or is unavailable";
    /// read_cursor.rs:38.
    pub const BUFFER: &str = "local cursor component does not match the requested buffer";
    /// read_cursor.rs:39.
    pub const CONTRACT: &str = "local cursor original ECS contract is unavailable";
    /// read_cursor.rs:41.
    pub const RETRY: &str =
        "Local cursor query is still scanning or changed. Retry on the next update";
    /// read_building_cursor.rs:223.
    pub const ATTACHED: &str = "local cursor attachment field is not boolean";
    /// read_building_cursor.rs:254.
    pub const SCALAR_TYPE: &str = "local cursor scalar type is unresolved";
    /// read_building_cursor.rs:265.
    pub const SCALAR_STORAGE: &str = "local cursor scalar storage is unresolved";
    /// read_building_cursor.rs:281.
    pub const SCALAR_EXTENT: &str = "local cursor scalar exceeds its reflected snapshot";
}

/// The process's cursor sequence: the number of the last published copy.
static SEQUENCE: AtomicU64 = AtomicU64::new(0);

fn next_sequence() -> u64 {
    loop {
        let next = SEQUENCE.fetch_add(1, Ordering::Relaxed).wrapping_add(1);
        if next != 0 {
            return next;
        }
    }
}

/// kiln.client's `state` now, or None.
fn client_state(services: &ServiceAdapter) -> Option<KilnClientState> {
    let table = services
        .bind::<KilnClientApi>(KILN_CLIENT_SERVICE, KILN_CLIENT_VERSION)
        .ok()?;
    let state = service_member!(table, KilnClientApi, state).ok()?;
    // SAFETY: an all-zero record is valid (integers only).
    let mut out: KilnClientState = unsafe { std::mem::zeroed() };
    out.size = std::mem::size_of::<KilnClientState>() as u32;
    // SAFETY: kiln_client.h state: a sized caller record; any thread.
    (unsafe { services.call(state, (&raw mut out,)) } == KILN_OK).then_some(out)
}

/// The local player kiln.client knows, and the world generation it was seen in.
fn local_player(services: &ServiceAdapter) -> Option<(u32, u32)> {
    let state = client_state(services)?;
    (state.local_player != 0 && state.flags & KILN_CLIENT_LOCAL_PLAYER as u32 != 0)
        .then_some((state.local_player, state.generation))
}

/// The cursor's members (presence only): kiln.world's ECS reads and kiln.client's `state`.
pub fn probe(services: &ServiceAdapter) -> Result<(), ServiceError> {
    probe_reads(services)?;
    let table = services.bind::<KilnClientApi>(KILN_CLIENT_SERVICE, KILN_CLIENT_VERSION)?;
    service_member!(table, KilnClientApi, state)?;
    Ok(())
}

/// The cursor's world-operation fact (`LocalCursorInputAvailable`): kiln.world reads a live client world with an exact
/// profile and kiln.client knows the local player.
pub fn read_available(services: &ServiceAdapter, facts: &mut RuntimeProviderFacts) {
    facts.world_operations[Op::WorldCursorGet as usize] =
        ready(services, CLIENT_WORLD) && local_player(services).is_some();
}

/// What the reader answers: ShroudForge's outcomes of its native local cursor reader.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Outcome {
    /// 2: the component's size disagrees.
    Buffer,
    /// 3: the component is not in the engine's registry join.
    Contract,
    /// 4: no live session, client world or local input.
    Session,
    /// 5: the copy changed under the read.
    Retry,
}
impl Outcome {
    fn text(self) -> &'static str {
        match self {
            Self::Buffer => text::BUFFER,
            Self::Contract => text::CONTRACT,
            Self::Session => text::SESSION,
            Self::Retry => text::RETRY,
        }
    }
}

/// `entity_open` of `entity` in the client world's live session.
fn open(ecs: &Ecs, entity: u32) -> Result<KilnObjectRef, Outcome> {
    let open = ecs
        .world()
        .and_then(|table| service_member!(table, KilnWorldApi, entity_open))
        .map_err(|_| Outcome::Session)?;
    // SAFETY: an all-zero ref is valid (integers only).
    let mut object: KilnObjectRef = unsafe { std::mem::zeroed() };
    object.size = std::mem::size_of::<KilnObjectRef>() as u32;
    let owner = ecs.owner();
    // SAFETY: kiln_world.h entity_open: plain values and a caller record.
    let result = unsafe {
        ecs.services().call(
            open,
            (owner, owner, CLIENT_WORLD, 0u64, entity, &raw mut object),
        )
    };
    match result {
        KILN_OK => Ok(object),
        KILN_ECS_ERR_UNREADABLE | KILN_WORLD_ERR_CHANGED => Err(Outcome::Retry),
        _ => Err(Outcome::Session),
    }
}

/// The component hash of `ClientPlayerInput`: the registry join's when the shared [`Ecs`]'s world registers it (its
/// size must be the reflected one), else its qualified hash (on a host the shared world is the server world, which has
/// no player input; kiln.world then checks the client world's registration and size on the copy).
fn input_component(ecs: &Ecs, size: u32) -> Result<u32, Outcome> {
    match ecs.resolve_component(INPUT) {
        Some(component) if component.size == size => Ok(component.component),
        Some(_) => Err(Outcome::Contract),
        None => Ok(fnv1a32(INPUT.as_bytes())),
    }
}

/// One stable copy of the local `ClientPlayerInput` (`size` bytes) of the client world while the world session marker
/// is `session`, and its sequence.
fn read_input(ecs: &Ecs, size: u32, session: u64) -> Result<(Vec<u8>, u64), Outcome> {
    let component = input_component(ecs, size)?;
    let services = ecs.services();
    let (player, generation) = local_player(services).ok_or(Outcome::Session)?;
    let object = open(ecs, player)?;
    let snapshot = ecs
        .world()
        .and_then(|table| service_member!(table, KilnWorldApi, ecs_snapshot))
        .map_err(|_| Outcome::Session)?;
    let mut bytes = vec![0u8; size as usize];
    let mut required = 0u32;
    // SAFETY: kiln_world.h ecs_snapshot: the caller's ref, a buffer of `size` bytes and an out count.
    let result = unsafe {
        services.call(
            snapshot,
            (
                ecs.owner(),
                &raw const object,
                component,
                bytes.as_mut_ptr().cast::<core::ffi::c_void>(),
                size,
                &raw mut required,
            ),
        )
    };
    match result {
        KILN_OK if required == size => {}
        KILN_OK | KILN_ERR_BUFFER_TOO_SMALL => return Err(Outcome::Buffer),
        KILN_ECS_ERR_UNREADABLE | KILN_WORLD_ERR_CHANGED => return Err(Outcome::Retry),
        _ => return Err(Outcome::Session),
    }
    // the copy counts only if its session and its local player held across it
    if local_player(services) != Some((player, generation)) {
        return Err(Outcome::Retry);
    }
    if ecs.world_session() != session {
        return Err(Outcome::Session);
    }
    Ok((bytes, next_sequence()))
}

/// A field's scalar leaf, decoded by its reflected kind (Source's decode of an inline scalar).
enum Scalar {
    Integer(i64),
    Number(f64),
    Boolean(bool),
    Other,
}

/// The decoder of one copy: the inline field plans of `ClientPlayerInput` (registry index `root`).
struct Fields<'a> {
    ecs: &'a Ecs,
    root: usize,
    bytes: &'a [u8],
}

impl Fields<'_> {
    fn plan(&self, path: &str) -> Result<Rc<crate::runtime::ecs::fields::InlineField>, String> {
        self.ecs
            .inline_field(self.root, &format!("data.cursorInput.{path}"))
    }
    /// The registry type a field stores in: typedefs and enums looked through (at most 64 steps).
    fn storage(&self, index: usize) -> Result<&'static str, String> {
        let registry = self.ecs.types();
        let mut ty = registry.get(index).ok_or(text::SCALAR_TYPE)?;
        for _ in 0..64 {
            if !matches!(ty.primitive_type, "Typedef" | "Enum") {
                break;
            }
            ty = ty
                .inner_type
                .and_then(|inner| registry.get(inner))
                .ok_or(text::SCALAR_STORAGE)?;
        }
        Ok(ty.primitive_type)
    }
    fn field(&self, path: &str) -> Result<Scalar, String> {
        let plan = self.plan(path)?;
        let bytes = self
            .bytes
            .get(plan.byte_range())
            .ok_or("field outside component snapshot")?;
        let kind = self.storage(plan.type_index)?;
        let int = |width: usize, signed: bool| -> Option<Scalar> {
            let mut out = [0u8; 8];
            out[..width].copy_from_slice(bytes.get(..width)?);
            let value = u64::from_le_bytes(out);
            Some(Scalar::Integer(if signed && width < 8 {
                let shift = 64 - 8 * width as u32;
                ((value << shift) as i64) >> shift
            } else {
                value as i64
            }))
        };
        Ok(match kind {
            "Bool" => Scalar::Boolean(bytes.first().is_some_and(|b| *b != 0)),
            "UInt8" => int(1, false).unwrap_or(Scalar::Other),
            "SInt8" => int(1, true).unwrap_or(Scalar::Other),
            "UInt16" => int(2, false).unwrap_or(Scalar::Other),
            "SInt16" => int(2, true).unwrap_or(Scalar::Other),
            "UInt32" => int(4, false).unwrap_or(Scalar::Other),
            "SInt32" => int(4, true).unwrap_or(Scalar::Other),
            "UInt64" | "SInt64" => int(8, true).unwrap_or(Scalar::Other),
            "Float32" => bytes
                .get(..4)
                .map(|b| Scalar::Number(f32::from_le_bytes(b.try_into().unwrap()).into()))
                .unwrap_or(Scalar::Other),
            "Float64" => bytes
                .get(..8)
                .map(|b| Scalar::Number(f64::from_le_bytes(b.try_into().unwrap())))
                .unwrap_or(Scalar::Other),
            _ => Scalar::Other,
        })
    }
    fn integer(&self, path: &str) -> Result<i64, String> {
        match self.field(path)? {
            Scalar::Integer(value) => Ok(value),
            _ => Err(format!("local cursor field is not an integer: {path}")),
        }
    }
    fn number(&self, path: &str) -> Result<f64, String> {
        match self.field(path)? {
            Scalar::Number(value) if value.is_finite() => Ok(value),
            _ => Err(format!("local cursor field is not a finite float: {path}")),
        }
    }
    /// `reflected_cursor_unsigned` (read_building_cursor.rs:236-287): the raw unsigned storage of the field.
    fn unsigned(&self, path: &str) -> Result<u64, String> {
        let plan = self.plan(path)?;
        let width = match self.storage(plan.type_index)? {
            "UInt8" | "Bitmask8" => 1,
            "UInt16" | "Bitmask16" => 2,
            "UInt32" | "Bitmask32" => 4,
            "UInt64" | "Bitmask64" => 8,
            _ => {
                return Err(format!(
                    "local cursor field is not an unsigned scalar: {path}"
                ));
            }
        };
        let source = self
            .bytes
            .get(plan.byte_range())
            .filter(|source| source.len() == width)
            .ok_or(text::SCALAR_EXTENT)?;
        let mut out = [0u8; 8];
        out[..width].copy_from_slice(source);
        Ok(u64::from_le_bytes(out))
    }
    fn boolean(&self, path: &str) -> Result<bool, String> {
        match self.field(path)? {
            Scalar::Boolean(value) => Ok(value),
            _ => Err(text::ATTACHED.into()),
        }
    }
}

/// `reflected_local_cursor` (read_building_cursor.rs:94-233): the snapshot table, or ShroudForge's reason.
fn reflected_local_cursor(lua: &Lua, ecs: Option<&Ecs>) -> Result<Table, String> {
    let ecs = ecs.ok_or(text::REFLECTION)?;
    let metadata = ecs
        .types()
        .by_qualified_name(INPUT)
        .ok_or(text::REFLECTION)?;
    let size = metadata.size as usize;
    if size == 0 || size > MAX_INPUT {
        return Err(text::SIZE.into());
    }
    let session = ecs.world_session();
    if session == 0 {
        return Err(text::WAITING.into());
    }
    let (bytes, sequence) =
        read_input(ecs, metadata.size, session).map_err(|outcome| outcome.text().to_owned())?;
    let fields = Fields {
        ecs,
        root: metadata.index,
        bytes: &bytes,
    };
    let lua_error = |error: mlua::Error| error.to_string();
    let transform = |name: &str| -> Result<Table, String> {
        let output = lua.create_table().map_err(lua_error)?;
        let position = lua.create_table().map_err(lua_error)?;
        let rotation = lua.create_table().map_err(lua_error)?;
        let scale = lua.create_table().map_err(lua_error)?;
        for (index, axis) in ["x", "y", "z"].iter().enumerate() {
            position
                .raw_set(*axis, fields.integer(&format!("{name}.position.{axis}"))?)
                .map_err(lua_error)?;
            scale
                .raw_set(index + 1, fields.number(&format!("{name}.scale.{axis}"))?)
                .map_err(lua_error)?;
        }
        for (index, axis) in ["x", "y", "z", "w"].iter().enumerate() {
            rotation
                .raw_set(
                    index + 1,
                    fields.number(&format!("{name}.orientation.{axis}"))?,
                )
                .map_err(lua_error)?;
        }
        output.raw_set("position", position).map_err(lua_error)?;
        output.raw_set("rotation", rotation).map_err(lua_error)?;
        output.raw_set("scale", scale).map_err(lua_error)?;
        Ok(output)
    };
    let value = lua.create_table().map_err(lua_error)?;
    value
        .raw_set("primaryTransform", transform("primaryTransform")?)
        .map_err(lua_error)?;
    value
        .raw_set("secondaryTransform", transform("secondaryTransform")?)
        .map_err(lua_error)?;
    for (public, reflected) in [
        ("primaryFlags", "primaryFlags"),
        ("secondaryFlags", "secondaryFlags"),
        ("material", "hoveredVoxelMaterialId"),
    ] {
        value
            .raw_set(public, fields.unsigned(reflected)?)
            .map_err(lua_error)?;
    }
    value
        .raw_set(
            "selectionVersion",
            fields
                .unsigned("selectObjectAction.versionData.version")?
                .to_string(),
        )
        .map_err(lua_error)?;
    let selected = lua.create_table().map_err(lua_error)?;
    selected
        .raw_set(
            "value",
            format!(
                "{:016x}",
                fields.unsigned("selectObjectAction.selectedObjectId.value")?
            ),
        )
        .map_err(lua_error)?;
    selected
        .raw_set(
            "type",
            fields.unsigned("selectObjectAction.selectedObjectId.type")?,
        )
        .map_err(lua_error)?;
    value
        .raw_set("selectedObject", selected)
        .map_err(lua_error)?;
    value
        .raw_set("attachedToProp", fields.boolean("isCursorAttachedToProp")?)
        .map_err(lua_error)?;
    let result = lua.create_table().map_err(lua_error)?;
    result.raw_set("sequence", sequence).map_err(lua_error)?;
    result.raw_set("value", value).map_err(lua_error)?;
    Ok(result)
}

/// `runtime.world.cursor.get()` (read_building_cursor.rs:3-24): the denial, no arguments, then the reflected record.
pub fn get(
    lua: &Lua,
    ecs: Option<&Ecs>,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<(Value, Option<String>)> {
    if let Some(reason) = runtime_denial_reason(host, Op::WorldCursorGet) {
        return Ok((Value::Nil, Some(reason)));
    }
    if !args.is_empty() {
        return Err(LuaError::generic(text::ARGUMENTS));
    }
    Ok(match reflected_local_cursor(lua, ecs) {
        Ok(snapshot) => (Value::Table(snapshot), None),
        Err(reason) => (Value::Nil, Some(reason)),
    })
}

/// Puts `get` into one package's `runtime.world.cursor` table. `ecs` gives the VM's shared [`Ecs`] at each call
/// (None without the ECS family).
pub fn install_into(
    lua: &Lua,
    cursor: &Table,
    host: Rc<dyn RuntimeHost>,
    ecs: super::EcsSource,
) -> mlua::Result<()> {
    cursor.raw_set(
        "get",
        lua.create_function(move |lua, args: FunctionArgs| {
            let ecs = ecs(lua);
            get(lua, ecs.as_deref(), host.as_ref(), &args)
        })?,
    )
}

/// The production family: `get` on the VM's shared [`Ecs`] ([`of_vm`]). A package without a `runtime` namespace
/// gets nothing.
pub fn family() -> Family {
    Family::new(FAMILY, &CLAIMS, move |lua, _, _, composed, cx| {
        let (Some(runtime), Some(host)) = (&composed.runtime, cx.package_host()) else {
            return Ok(());
        };
        let cursor: Table = runtime.raw_get::<Table>("world")?.raw_get("cursor")?;
        install_into(lua, &cursor, host, Rc::new(of_vm))
    })
}
