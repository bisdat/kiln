// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (identical in both pins): modules/mod-runtime/lua/ingame/check_ingame_access.rs:12-414;
// lua/shroudforge/v1/runtime-operations.json:5-35; lua/ingame/mod.rs:84-113.
//! Finite, worker-local host ports. No engine addresses, service calls or archive types.
use crate::{
    host::{HostContext, Phase, Side, SourceVersion},
    package::{Manifest, manifest::Capability},
};

macro_rules! operations {
    ($($variant:ident => $name:literal),+ $(,)?) => {
        /// Exact pinned operation catalog, not arbitrary engine-name dispatch.
        #[derive(Clone, Copy, Debug, Eq, PartialEq)]
        #[repr(usize)]
        pub enum RuntimeOperation { $($variant),+ }
        impl RuntimeOperation {
            /// Source order. The result exposed to Lua is keyed, not an ordered array.
            pub const ALL: &'static [Self] = &[$(Self::$variant),+];
            pub const fn as_str(self) -> &'static str { match self {$(Self::$variant => $name),+} }
            pub fn from_name(name: &str) -> Option<Self> {
                match name { $($name => Some(Self::$variant)),+, _ => None }
            }
        }
    }
}
operations! {
    Lifecycle => "runtime.lifecycle",
    IsMainMenu => "runtime.is_main_menu",
    PlayerName => "runtime.player.name",
    NetworkStatus => "runtime.network.status",
    NetworkSend => "runtime.network.send",
    NetworkSendMod => "runtime.network.send_mod",
    NetworkReceive => "runtime.network.receive",
    NetworkReceiveMod => "runtime.network.receive_mod",
    NetworkAccept => "runtime.network.accept",
    NetworkConnectedPeers => "runtime.network.connected_peers",
    EcsQuery => "runtime.ecs.query",
    EcsResolve => "runtime.ecs.resolve",
    EcsRead => "runtime.ecs.read",
    EcsWrite => "runtime.ecs.write",
    AssetsWrite => "runtime.assets.write",
    WorldContextActive => "runtime.world.context.active",
    WorldCursorGet => "runtime.world.cursor.get",
    WorldBuildingInput => "runtime.world.building.input",
    WorldVoxelRead => "runtime.world.voxel.read",
    WorldVoxelWrite => "runtime.world.voxel.write",
    WorldVoxelGridSpec => "runtime.world.voxel.grid_spec",
    WorldEntitySpawn => "runtime.world.entity.spawn",
    WorldEntityQueryProps => "runtime.world.entity.query_props",
    WorldEntityQueryPropsInBounds => "runtime.world.entity.query_props_in_bounds",
    WorldEntityRegisterPropRecipes => "runtime.world.entity.register_prop_recipes",
    WorldEntityGetTransform => "runtime.world.entity.get_transform",
    WorldEntitySetScale => "runtime.world.entity.set_scale",
    WorldEntityPlace => "runtime.world.entity.place",
    WorldEntityDestroy => "runtime.world.entity.destroy",
    WorldEntityFinishBuilding => "runtime.world.entity.finish_building",
    GameplayPatch => "runtime.gameplay.patch",
}

/// Source contract availability. Preserve the provider's exact static reason.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Availability {
    Available,
    Unavailable { reason: String },
}
impl Availability {
    pub fn is_available(&self) -> bool {
        matches!(self, Self::Available)
    }
}

/// Frozen registration facts. Construction derives permissions ONLY from the manifest.
/// Global patch/export facts are explicit and independent of these permissions.
/// `game_build` is the source contract's display version, used for unknown status keys
/// (game-runtime/compatibility/lib.rs:30-39), not an engine resolver or package version.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RuntimeOwnerContext {
    id: String,
    runtime: bool,
    patch: bool,
    export: bool,
    phase: Phase,
    side: Side,
    version: SourceVersion,
    global_patch: bool,
    global_export: bool,
    game_build: String,
}
impl RuntimeOwnerContext {
    pub fn new(
        manifest: &Manifest,
        context: &HostContext,
        global_patch: bool,
        global_export: bool,
        game_build: impl Into<String>,
    ) -> Self {
        Self {
            id: manifest.id.clone(),
            runtime: manifest.capabilities.contains(&Capability::Runtime),
            patch: manifest.capabilities.contains(&Capability::Patch),
            export: manifest.capabilities.contains(&Capability::Export),
            phase: context.phase,
            side: context.side,
            version: context.version,
            global_patch,
            global_export,
            game_build: game_build.into(),
        }
    }
    pub fn id(&self) -> &str {
        &self.id
    }
    pub fn phase(&self) -> Phase {
        self.phase
    }
    pub fn side(&self) -> Side {
        self.side
    }
    pub fn version(&self) -> SourceVersion {
        self.version
    }
    pub fn has_runtime(&self) -> bool {
        self.runtime
    }
    pub fn has_patch(&self) -> bool {
        self.patch
    }
    pub fn has_export(&self) -> bool {
        self.export
    }
    pub fn global_patch(&self) -> bool {
        self.global_patch
    }
    pub fn global_export(&self) -> bool {
        self.global_export
    }
    pub fn game_build(&self) -> &str {
        &self.game_build
    }
}

/// One coherent copy of finite provider facts, not a request to probe every service.
/// Hosts copy their current cached facts without mixing session/provider epochs. Actual
/// handle validation stays in the phase host. No jobs, waits or game-thread queue are
/// implied: current engine operations use caller-thread gated services.
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct RuntimeProviderFacts {
    pub ready: bool,
    pub can_write: bool,
    pub resource_can_write: bool,
    pub gameplay_patch_available: bool,
    pub network_available: bool,
    pub network_service_ready: bool,
    pub world_context_active: bool,
    pub world_entity_query_props_ready: bool,
    pub world_entity_context_ready: bool,
    pub can_write_current_transform: bool,
    pub world_operations: [bool; RuntimeOperation::ALL.len()],
    pub is_main_menu: Option<bool>,
    pub selected_character_name: Option<String>,
}
impl RuntimeProviderFacts {
    pub fn world_operation_available(&self, operation: RuntimeOperation) -> bool {
        self.world_operations[operation as usize]
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EffectState {
    Waiting,
    NoTarget,
    NoChange,
    WriteConfirmed,
    WriteFailed,
}
impl EffectState {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Waiting => "waiting",
            Self::NoTarget => "no-target",
            Self::NoChange => "no-change",
            Self::WriteConfirmed => "write-confirmed",
            Self::WriteFailed => "write-failed",
        }
    }
    pub fn from_name(name: &str) -> Option<Self> {
        match name {
            "waiting" => Some(Self::Waiting),
            "no-target" => Some(Self::NoTarget),
            "no-change" => Some(Self::NoChange),
            "write-confirmed" => Some(Self::WriteConfirmed),
            "write-failed" => Some(Self::WriteFailed),
            _ => None,
        }
    }
}

/// Stored context must remain immutable for every closure's lifetime. Ports are lazy:
/// denied branches do not acquire later facts. This trait is intentionally not Send.
pub trait RuntimeHost {
    fn owner_context(&self) -> &RuntimeOwnerContext;
    fn static_support(&self, operation: RuntimeOperation) -> Availability;
    fn provider_facts(&self) -> RuntimeProviderFacts;
    fn lifecycle_active(&self) -> bool;
    /// Same-session write latch, relevant only for ECS/assets write denial.
    fn write_fault(&self) -> Option<String>;
    /// Infallible normal-path diagnostic sink. No invented refusal tuple; the host
    /// owns timestamp/truncation/deduplication and fatal host fault handling.
    fn report_effect(&self, state: EffectState, detail: &str);
}
