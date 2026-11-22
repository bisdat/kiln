// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/player_input/submit_building_input.rs:1-170 (submit,
// status, cancel, the request owner), game-runtime/native/engine-context/track_world_context.cpp:185-188
// (OperationAvailable of runtime.world.building.input).
//! `runtime.world.building.submit`, `status` and `cancel` on kiln.client's building input members.
//!
//! Every ShroudForge package of this process reaches kiln.client under kiln-sfi's one plugin owner, so ShroudForge's
//! own request ownership lives here, as in ShroudForge's Lua binding:
//! - **One owner of the sequence**: each package's `runtime.world.building` has its own request owner (a nonzero id);
//!   the first package whose submit gets past the argument checks becomes the active owner, and every other package's
//!   submit answers "another mod owns the building input sequence" until that owner cancels, its request fails
//!   with no earlier one, or its VM goes (the owner's drop cancels its latest request and releases the sequence).
//! - **The latest id**: `status` and `cancel` answer only for the package's own latest request id ("unknown" and
//!   false for any other id, 0 included); a cancel forgets that id and releases the sequence.
//!
//! The members (`building_input_submit` / `status` / `cancel`, kiln_client.h, Kiln-internal) answer
//! KILN_ERR_UNAVAILABLE while no input producer is bound on the running game build; the catalog backs
//! `runtime.world.building.input` and its world-operation fact on them and the core's `client.building_input` row
//! ([`CAPABILITY`]), so the operation is refused until that row says available and lights up by itself when a producer
//! binds. Kiln's member checks the request again (fields, the live client session, one request for the whole client):
//! any refusal is ShroudForge's "building input unavailable, busy, or request invalid". A dispatched request is the
//! game's ordinary input, not the world's acknowledgement.
//!
//! The family keeps the sequence's owner per runtime-create (a runtime hook, so a live reload's VM shares it). Every
//! call runs on the worker and calls kiln.client directly through the service adapter: no hop, no wait.

use crate::runtime::install::{Family, RuntimeHook};
use crate::runtime::services::{ServiceAdapter, ServiceError};
use crate::service_member;
use kiln_abi::KILN_OK;
use kiln_abi::KilnOwner;
use kiln_abi::services::kiln_client::{
    KILN_CLIENT_SERVICE, KILN_CLIENT_VERSION, KilnBuildingInput, KilnClientApi,
};
use mlua::{Lua, Table};
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op, runtime_denial_reason};
use sfi_core::host::Side;
use sfi_core::lua::FunctionArgs;
use std::cell::Cell;
use std::rc::Rc;

/// The family's name.
pub const FAMILY: &str = "world.building";

/// The names it adds inside the installer's `runtime.world.building` container.
pub const CLAIMS: [&str; 3] = [
    "runtime.world.building.submit",
    "runtime.world.building.status",
    "runtime.world.building.cancel",
];

/// The core's capability row of the building input producer (docs/kiln/services/client-building-input.md).
pub const CAPABILITY: &str = "client.building_input";

/// ShroudForge's texts (submit_building_input.rs).
pub mod text {
    pub const SERVER: &str = "building input belongs to a connected client";
    pub const ACTION: &str = "building action must be select, place, remove, dismantle or undo";
    pub const POSITION: &str = "building position is required";
    pub const DISMANTLE: &str = "dismantle requires an exact targetEntityId";
    pub const FINITE: &str = "building transform must be finite";
    pub const OWNED: &str = "another mod owns the building input sequence";
    pub const REFUSED: &str = "building input unavailable, busy, or request invalid";
}

/// kiln.client's building input members (presence only): the three of them past the 56-byte table.
pub fn probe(services: &ServiceAdapter) -> Result<(), ServiceError> {
    let table = services.bind::<KilnClientApi>(KILN_CLIENT_SERVICE, KILN_CLIENT_VERSION)?;
    service_member!(table, KilnClientApi, building_input_submit)?;
    service_member!(table, KilnClientApi, building_input_status)?;
    service_member!(table, KilnClientApi, building_input_cancel)?;
    Ok(())
}

/// The sequence of one runtime-create: which package's owner holds it (0: none) and the next owner id.
#[derive(Default)]
pub struct Sequence {
    active: Cell<u32>,
    next: Cell<u32>,
}

impl Sequence {
    fn next_owner(&self) -> u32 {
        let mut id = self.next.get().wrapping_add(1);
        if id == 0 {
            id = 1;
        }
        self.next.set(id);
        id
    }
    /// The active owner (0: nobody holds the sequence).
    pub fn active(&self) -> u32 {
        self.active.get()
    }
}

/// kiln.client's members for one package: the adapter and kiln-sfi's owner.
#[derive(Clone)]
struct Members {
    services: Rc<ServiceAdapter>,
    owner: KilnOwner,
}

impl Members {
    fn table(&self) -> Option<crate::runtime::services::ServiceTable<KilnClientApi>> {
        self.services
            .bind::<KilnClientApi>(KILN_CLIENT_SERVICE, KILN_CLIENT_VERSION)
            .ok()
    }
    /// The request id (never 0), or 0 for every refusal.
    fn submit(&self, input: &KilnBuildingInput) -> u32 {
        let Some(submit) = self
            .table()
            .and_then(|table| service_member!(table, KilnClientApi, building_input_submit).ok())
        else {
            return 0;
        };
        let mut id = 0u32;
        // SAFETY: kiln_client.h building_input_submit: a sized caller record and an out id; any thread.
        let result = unsafe {
            self.services.call(
                submit,
                (self.owner, 0u64, std::ptr::from_ref(input), &raw mut id),
            )
        };
        if result == KILN_OK { id } else { 0 }
    }
    /// The request's KILN_BUILDING_INPUT_* state (0 when the member refuses).
    fn status(&self, id: u32) -> u32 {
        let Some(status) = self
            .table()
            .and_then(|table| service_member!(table, KilnClientApi, building_input_status).ok())
        else {
            return 0;
        };
        let mut state = 0u32;
        // SAFETY: kiln_client.h building_input_status: plain values and an out state.
        let result = unsafe { self.services.call(status, (self.owner, id, &raw mut state)) };
        if result == KILN_OK { state } else { 0 }
    }
    /// Whether the member cancelled the request.
    fn cancel(&self, id: u32) -> bool {
        let Some(cancel) = self
            .table()
            .and_then(|table| service_member!(table, KilnClientApi, building_input_cancel).ok())
        else {
            return false;
        };
        let mut cancelled = 0u32;
        // SAFETY: kiln_client.h building_input_cancel: plain values and an out flag.
        let result = unsafe {
            self.services
                .call(cancel, (self.owner, id, &raw mut cancelled))
        };
        result == KILN_OK && cancelled == 1
    }
}

/// One package's request owner (ShroudForge's `RequestOwner`): its id and its latest request id.
pub struct RequestOwner {
    id: u32,
    request: Cell<u32>,
    sequence: Rc<Sequence>,
    members: Members,
}

impl RequestOwner {
    fn new(sequence: Rc<Sequence>, members: Members) -> Self {
        Self {
            id: sequence.next_owner(),
            request: Cell::new(0),
            sequence,
            members,
        }
    }
    fn acquire(&self) -> bool {
        match self.sequence.active.get() {
            0 => {
                self.sequence.active.set(self.id);
                true
            }
            id => id == self.id,
        }
    }
    fn release(&self) {
        if self.sequence.active.get() == self.id {
            self.sequence.active.set(0);
        }
    }
}

impl Drop for RequestOwner {
    /// The package's VM went: its latest request is cancelled and the sequence released (ShroudForge's drop).
    fn drop(&mut self) {
        let request = self.request.get();
        if request != 0 {
            self.members.cancel(request);
        }
        self.release();
    }
}

/// submit_building_input.rs:48-60: the optional array `name` of `N` finite numbers, or `default`.
fn vector<const N: usize>(table: &Table, name: &str, default: [f64; N]) -> mlua::Result<[f64; N]> {
    let Some(values) = table.raw_get::<Option<Table>>(name)? else {
        return Ok(default);
    };
    let mut out = default;
    for (index, value) in out.iter_mut().enumerate() {
        *value = values.raw_get(index + 1)?;
        if !value.is_finite() {
            return Err(mlua::Error::runtime(text::FINITE));
        }
    }
    Ok(out)
}

type Reply = (Option<u32>, Option<String>);

/// `runtime.world.building.submit(input)` (submit_building_input.rs:67-139): the table is converted first (as the
/// binding's argument), then the denial, the side, the fields in ShroudForge's order, the sequence, then Kiln's member.
pub fn submit(
    host: &dyn RuntimeHost,
    owner: &RequestOwner,
    args: &FunctionArgs,
) -> mlua::Result<Reply> {
    let input = args.get::<Table>(0)?;
    if let Some(reason) = runtime_denial_reason(host, Op::WorldBuildingInput) {
        return Ok((None, Some(reason)));
    }
    if host.owner_context().side() == Side::Server {
        return Ok((None, Some(text::SERVER.into())));
    }
    let action = input.raw_get::<String>("action")?;
    let kind: u32 = match action.as_str() {
        "select" => 0,
        "place" => 1,
        "remove" => 2,
        "undo" => 3,
        "dismantle" => 4,
        _ => return Err(mlua::Error::runtime(text::ACTION)),
    };
    if matches!(kind, 1 | 2 | 4) && !input.contains_key("position")? {
        return Err(mlua::Error::runtime(text::POSITION));
    }
    let player = input.raw_get::<u32>("player")?;
    let item = input.raw_get::<Option<u32>>("itemId")?.unwrap_or(0);
    let material = input.raw_get::<Option<u32>>("materialItemId")?.unwrap_or(0);
    let slot = input.raw_get::<Option<u32>>("slot")?.unwrap_or(0);
    let target = input.raw_get::<Option<u32>>("targetEntityId")?.unwrap_or(0);
    if (kind == 4) != (target != 0) {
        return Ok((None, Some(text::DISMANTLE.into())));
    }
    let position = vector(input, "position", [0.0; 3])?;
    let rotation = vector(input, "rotation", [0.0, 0.0, 0.0, 1.0])?;
    let scale = vector(input, "scale", [1.0; 3])?;
    if !owner.acquire() {
        return Ok((None, Some(text::OWNED.into())));
    }
    let record = KilnBuildingInput {
        size: std::mem::size_of::<KilnBuildingInput>() as u32,
        action: kind,
        player,
        item_id: item,
        material_item_id: material,
        slot,
        target_entity_id: target,
        reserved: 0,
        position,
        rotation,
        scale,
    };
    match owner.members.submit(&record) {
        0 => {
            if owner.request.get() == 0 {
                owner.release();
            }
            Ok((None, Some(text::REFUSED.into())))
        }
        id => {
            owner.request.set(id);
            Ok((Some(id), None))
        }
    }
}

/// `runtime.world.building.status(id)` (submit_building_input.rs:141-155): only the package's latest id is known.
pub fn status(owner: &RequestOwner, args: &FunctionArgs) -> mlua::Result<&'static str> {
    let id = args.get::<u32>(0)?;
    if id == 0 || id != owner.request.get() {
        return Ok("unknown");
    }
    Ok(match owner.members.status(id) {
        1 => "queued",
        2 => "pressed",
        3 => "dispatched",
        4 => "timeout",
        5 => "cancelled",
        _ => "unknown",
    })
}

/// `runtime.world.building.cancel(id)` (submit_building_input.rs:157-168): the package's latest id only; it is
/// forgotten and the sequence released whatever Kiln's member answers.
pub fn cancel(owner: &RequestOwner, args: &FunctionArgs) -> mlua::Result<bool> {
    let id = args.get::<u32>(0)?;
    if id == 0 || id != owner.request.get() {
        return Ok(false);
    }
    let cancelled = owner.members.cancel(id);
    owner.request.set(0);
    owner.release();
    Ok(cancelled)
}

/// Puts the three functions into one package's `runtime.world.building` table, with a new request owner of
/// `sequence` over `services` and kiln-sfi's `owner`.
pub fn install_into(
    lua: &Lua,
    building: &Table,
    host: Rc<dyn RuntimeHost>,
    sequence: Rc<Sequence>,
    services: Rc<ServiceAdapter>,
    owner: KilnOwner,
) -> mlua::Result<()> {
    let request = Rc::new(RequestOwner::new(sequence, Members { services, owner }));
    let r = request.clone();
    building.raw_set(
        "submit",
        lua.create_function(move |_, args: FunctionArgs| submit(host.as_ref(), &r, &args))?,
    )?;
    let r = request.clone();
    building.raw_set(
        "status",
        lua.create_function(move |_, args: FunctionArgs| status(&r, &args))?,
    )?;
    building.raw_set(
        "cancel",
        lua.create_function(move |_, args: FunctionArgs| cancel(&request, &args))?,
    )
}

impl RuntimeHook for Sequence {}

/// The production family: one sequence per runtime-create (a live reload's VM gets the same one through the hook), a
/// request owner per package. A package without a `runtime` namespace gets nothing.
pub fn family() -> Family {
    let sequence = Rc::new(Sequence::default());
    let hook: Rc<dyn RuntimeHook> = sequence.clone();
    Family::new(FAMILY, &CLAIMS, move |lua, _, _, composed, cx| {
        let (Some(runtime), Some(host)) = (&composed.runtime, cx.package_host()) else {
            return Ok(());
        };
        let building: Table = runtime.raw_get::<Table>("world")?.raw_get("building")?;
        install_into(
            lua,
            &building,
            host,
            sequence.clone(),
            cx.services().clone(),
            cx.owner(),
        )
    })
    .with_hook(hook)
}
