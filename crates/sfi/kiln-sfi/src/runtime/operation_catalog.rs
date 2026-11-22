// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (identical in both pins): modules/mod-runtime/lua/shroudforge/v1/
// runtime-operations.json:5-35 (the 31 operations, in order), lua/ingame/
// check_ingame_access.rs:12-135,179-299 (which provider fact each operation reads),
// game-runtime/compatibility/lib.rs:30-44 (an operation outside the contract).
//! The runtime operation catalog as host facts for the runtime registrar.
//!
//! ShroudForge answers two questions per operation: is it in this game build's contract
//! (`state.api().runtime(op)`, static), and is its provider ready now (`runtime_provider`,
//! dynamic). Kiln answers them from its own services (readiness is dynamic, as
//! in ShroudForge):
//! - **Static support** ([`OperationRow`]): an operation is supported when the interpreter
//!   implements it ([`Backing::Interpreter`]), or when its family has landed
//!   ([`Backing::Kiln`]) and every member it needs is in this core's tables and none of
//!   its capability rows says unavailable. Otherwise ShroudForge's reason for an operation
//!   outside the contract: `operation <op> has not been verified for game build <build>`.
//!   A row whose family or member has not landed answers that until it does.
//! - **Provider facts** ([`FactRow`]): a fact holds when its backing's members are present
//!   and every capability row it names is available now (a pending session row is "not yet").
//!   Facts are read on every query, so a capability that turns on or off is seen without
//!   re-admitting anything.
//!
//! Each later family only edits its rows here: its operations' and facts' backing goes from
//! [`Backing::NotLanded`] to [`Backing::Kiln`] with the members and capability rows it
//! uses (and a reader for the facts that carry values).

use super::services::{ServiceAdapter, ServiceError};
use sfi_core::bindings::runtime::{RuntimeOperation as Op, RuntimeProviderFacts};

/// Binds a member's table and copies the member (`service_member!`): present or why not.
pub type Probe = fn(&ServiceAdapter) -> Result<(), ServiceError>;

/// One thing an operation or a fact needs from Kiln: a member, and the capability row
/// (kiln.caps) that says whether it works here and now.
#[derive(Clone, Copy)]
pub struct Need {
    pub member: Probe,
    pub capability: &'static str,
}

/// What backs an operation or a fact.
#[derive(Clone, Copy)]
pub enum Backing {
    /// The interpreter itself; nothing from Kiln.
    Interpreter,
    /// A landed family, on these members and capability rows.
    Kiln(&'static [Need]),
    /// The family has not landed: what it waits for.
    NotLanded(&'static str),
}

#[derive(Clone, Copy)]
pub struct OperationRow {
    pub operation: Op,
    pub backing: Backing,
}

/// The provider facts ShroudForge's availability reads (`RuntimeProviderFacts`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Fact {
    Ready,
    CanWrite,
    ResourceCanWrite,
    GameplayPatchAvailable,
    NetworkAvailable,
    NetworkServiceReady,
    WorldContextActive,
    WorldEntityQueryPropsReady,
    WorldEntityContextReady,
    CanWriteCurrentTransform,
    /// `runtime_provider::world_operation_available(op)`.
    WorldOperation(Op),
    /// The cached `is_main_menu` value (needs a reader).
    MainMenu,
    /// The cached selected character name (needs a reader).
    CharacterName,
}

/// Fills the facts a satisfied row carries (values, or flags that need more than presence).
pub type Read = fn(&ServiceAdapter, &mut RuntimeProviderFacts);

#[derive(Clone, Copy)]
pub struct FactRow {
    pub fact: Fact,
    pub backing: Backing,
    /// Without one, a satisfied flag fact is true.
    pub read: Option<Read>,
}

/// The catalog of one runtime-create: every operation once, in ShroudForge's order, and
/// the provider facts. A fact without a row is false (or unknown).
pub struct Catalog {
    pub operations: &'static [OperationRow],
    pub facts: &'static [FactRow],
}

impl Catalog {
    /// The row of `operation`.
    pub fn row(&self, operation: Op) -> Option<&OperationRow> {
        self.operations.iter().find(|r| r.operation == operation)
    }

    /// Every operation exactly once, in source order; every fact at most once; value facts
    /// and Kiln-backed rows well formed; world facts only for world operations.
    pub fn check(&self) -> Result<(), String> {
        let order: Vec<Op> = self.operations.iter().map(|r| r.operation).collect();
        if order != Op::ALL {
            return Err(
                "the catalog must list every runtime operation once, in source order".into(),
            );
        }
        let rows = self
            .operations
            .iter()
            .map(|r| (format!("operation {}", r.operation.as_str()), r.backing))
            .chain(
                self.facts
                    .iter()
                    .map(|r| (format!("fact {:?}", r.fact), r.backing)),
            );
        for (what, backing) in rows {
            match backing {
                Backing::Kiln([]) => {
                    return Err(format!("{what}: a Kiln backing names its members"));
                }
                Backing::Kiln(needs) if needs.iter().any(|n| n.capability.is_empty()) => {
                    return Err(format!("{what}: every need names its capability row"));
                }
                Backing::NotLanded("") => {
                    return Err(format!("{what}: a missing family names what it waits for"));
                }
                _ => {}
            }
        }
        for (index, row) in self.facts.iter().enumerate() {
            if self.facts[..index].iter().any(|r| r.fact == row.fact) {
                return Err(format!("fact {:?} has two rows", row.fact));
            }
            if let Fact::WorldOperation(op) = row.fact
                && !op.as_str().starts_with("runtime.world.")
            {
                return Err(format!("{} is not a world operation", op.as_str()));
            }
            let value = matches!(row.fact, Fact::MainMenu | Fact::CharacterName);
            if value && matches!(row.backing, Backing::Kiln(_)) && row.read.is_none() {
                return Err(format!(
                    "fact {:?} carries a value and needs a reader",
                    row.fact
                ));
            }
        }
        Ok(())
    }
}

const fn op(operation: Op, backing: Backing) -> OperationRow {
    OperationRow { operation, backing }
}
const fn later(missing: &'static str) -> Backing {
    Backing::NotLanded(missing)
}

/// The network operations need kiln.net's Steam-messages lease (its `status` member at
/// slot 128; the others are checked where they are called) and its capability row.
static NETWORK: [Need; 1] = [Need {
    member: super::network::probe,
    capability: super::network::CAPABILITY,
}];

/// The ECS queries and reads need kiln.world's ECS-read members (`ecs::probe_reads`) and the core's `world.ecs_read`
/// row: an exact engine profile whose entity table checks out on the running exe.
static ECS_READ: [Need; 1] = [Need {
    member: super::ecs::probe_reads,
    capability: super::ecs::READ_CAPABILITY,
}];

/// The plain component writes need the reads and kiln.world's compare-exchange members with the core's
/// `world.ecs_write` row (the exact profile's write gate).
static ECS_WRITE: [Need; 2] = [
    Need {
        member: super::ecs::probe_reads,
        capability: super::ecs::READ_CAPABILITY,
    },
    Need {
        member: super::ecs::probe_writes,
        capability: super::ecs::WRITE_CAPABILITY,
    },
];

/// The loaded-resource writes need kiln.assets' loaded reads and its scalar compare-exchange with its
/// `assets.loaded_write` row (the exact profile's write gate and content-worker lifetime layout).
static ASSETS_WRITE: [Need; 1] = [Need {
    member: super::assets::probe_writes,
    capability: super::assets::WRITE_CAPABILITY,
}];

/// The voxel grid needs kiln.build's task-thread grid member (`grid_for_session`) and its row `build.voxel_grid_tasks`.
static VOXEL_GRID: [Need; 1] = [Need {
    member: super::world::voxels::probe,
    capability: super::world::voxels::GRID_CAPABILITY,
}];

/// The live world context needs kiln.world's `game_world_session` and the core's `session`
/// record (`world::context::probe`), and the core's `world.lifecycle` row: without the world-destroy
/// hook a stop is not seen.
static CONTEXT: [Need; 1] = [Need {
    member: super::world::context::probe,
    capability: super::world::context::CAPABILITY,
}];

/// The world cursor needs kiln.world's ECS reads and kiln.client's `state` (the local player), with `world.ecs_read`.
static CURSOR: [Need; 1] = [Need {
    member: super::world::cursor::probe,
    capability: super::ecs::READ_CAPABILITY,
}];

/// Building input needs kiln.client's building input members and the core's `client.building_input` row, which says
/// unavailable until an input producer is bound on the running build.
static BUILDING: [Need; 1] = [Need {
    member: super::world::building_input::probe,
    capability: super::world::building_input::CAPABILITY,
}];

/// The prop reads need kiln.world's ECS reads and `ecs_snapshot_many`, with `world.ecs_read`.
static PROPS: [Need; 1] = [Need {
    member: super::world::props::probe,
    capability: super::ecs::READ_CAPABILITY,
}];

/// `set_scale` needs the prop reads and the compare-exchange writes with `world.ecs_write`.
static PROPS_WRITE: [Need; 2] = [
    Need {
        member: super::world::props::probe,
        capability: super::ecs::READ_CAPABILITY,
    },
    Need {
        member: super::world::props::probe_scale,
        capability: super::ecs::WRITE_CAPABILITY,
    },
];

/// Production: what this interpreter implements today. `runtime.lifecycle` needs nothing
/// from Kiln; nor do the refusal-only operations (`place`, `finish_building`,
/// `runtime.gameplay.patch`): they are in ShroudForge's contract on every build, and their
/// provider facts are false on every current profile (no `worldOperations`, no
/// `runtimePatches` from build 25966634 on), so those facts have no row
/// (`WorldOperation(place / finish_building)`, `WorldEntityContextReady`, which needs place and
/// finish_building, and `GameplayPatchAvailable`). The network core, the mod messages
/// and `connected_peers` need kiln.net's lease. `runtime.world.context.active`
/// and its two facts (the live context, and `world_operation_available` of that operation,
/// which is the live context too) need the world context's members and `world.lifecycle`. The ECS queries, the
/// entity resolve and the component reads, and ECS readiness (`Ready`, which also needs `ecs_context_snapshot`'s view
/// of the family's world: an exact profile and a live session, `RuntimeFacts::provider_facts`), need kiln.world's
/// ECS-read members and `world.ecs_read`; the component writes and `CanWrite` (also the server world's ECS context:
/// only the local authority's world is written) those and kiln.world's compare-exchange members with
/// `world.ecs_write`. The cursor and its world-operation fact (a live client world kiln.world reads and a known local
/// player) need the ECS reads and kiln.client's `state`; building input and its fact kiln.client's building input
/// members and `client.building_input`; the prop reads, the recipe catalog and `WorldEntityQueryPropsReady` (ECS
/// readiness, read after `Ready`) the ECS reads and `ecs_snapshot_many`; `set_scale` and `CanWriteCurrentTransform`
/// (`CanWrite`, read after it, and `CurrentTransform`'s write policy) those and the compare-exchange writes. The voxel grid spec and
/// its readiness fact (the read operation's world fact) need kiln.build's grid member and `build.voxel_grid_tasks`.
/// Every other family lands with its own rows.
pub static CATALOG: Catalog = Catalog {
    operations: &[
        op(Op::Lifecycle, Backing::Interpreter),
        op(
            Op::IsMainMenu,
            later("the game flow state (game_flow_state)"),
        ),
        op(
            Op::PlayerName,
            later("the selected character name (selected_character_name)"),
        ),
        op(Op::NetworkStatus, Backing::Kiln(&NETWORK)),
        op(Op::NetworkSend, Backing::Kiln(&NETWORK)),
        op(Op::NetworkSendMod, Backing::Kiln(&NETWORK)),
        op(Op::NetworkReceive, Backing::Kiln(&NETWORK)),
        op(Op::NetworkReceiveMod, Backing::Kiln(&NETWORK)),
        op(Op::NetworkAccept, Backing::Kiln(&NETWORK)),
        op(Op::NetworkConnectedPeers, Backing::Kiln(&NETWORK)),
        op(Op::EcsQuery, Backing::Kiln(&ECS_READ)),
        op(Op::EcsResolve, Backing::Kiln(&ECS_READ)),
        op(Op::EcsRead, Backing::Kiln(&ECS_READ)),
        op(Op::EcsWrite, Backing::Kiln(&ECS_WRITE)),
        op(Op::AssetsWrite, Backing::Kiln(&ASSETS_WRITE)),
        op(Op::WorldContextActive, Backing::Kiln(&CONTEXT)),
        op(Op::WorldCursorGet, Backing::Kiln(&CURSOR)),
        op(Op::WorldBuildingInput, Backing::Kiln(&BUILDING)),
        op(Op::WorldVoxelRead, later("voxel reads (read_for_session)")),
        op(Op::WorldVoxelWrite, later("voxel writes")),
        op(Op::WorldVoxelGridSpec, Backing::Kiln(&VOXEL_GRID)),
        op(Op::WorldEntitySpawn, later("entity spawn and destroy")),
        op(Op::WorldEntityQueryProps, Backing::Kiln(&PROPS)),
        op(Op::WorldEntityQueryPropsInBounds, Backing::Kiln(&PROPS)),
        op(Op::WorldEntityRegisterPropRecipes, Backing::Kiln(&PROPS)),
        op(Op::WorldEntityGetTransform, Backing::Kiln(&PROPS)),
        op(Op::WorldEntitySetScale, Backing::Kiln(&PROPS_WRITE)),
        op(Op::WorldEntityPlace, Backing::Interpreter),
        op(Op::WorldEntityDestroy, later("entity spawn and destroy")),
        op(Op::WorldEntityFinishBuilding, Backing::Interpreter),
        op(Op::GameplayPatch, Backing::Interpreter),
    ],
    facts: &[
        FactRow {
            fact: Fact::Ready,
            backing: Backing::Kiln(&ECS_READ),
            read: None,
        },
        FactRow {
            fact: Fact::CanWrite,
            backing: Backing::Kiln(&ECS_WRITE),
            read: None,
        },
        FactRow {
            fact: Fact::ResourceCanWrite,
            backing: Backing::Kiln(&ASSETS_WRITE),
            read: None,
        },
        FactRow {
            fact: Fact::NetworkAvailable,
            backing: Backing::Kiln(&NETWORK),
            read: Some(super::network::read_available),
        },
        FactRow {
            fact: Fact::NetworkServiceReady,
            backing: Backing::Kiln(&NETWORK),
            read: Some(super::network::read_service_ready),
        },
        FactRow {
            fact: Fact::WorldContextActive,
            backing: Backing::Kiln(&CONTEXT),
            read: None,
        },
        FactRow {
            fact: Fact::WorldOperation(Op::WorldContextActive),
            backing: Backing::Kiln(&CONTEXT),
            read: None,
        },
        // ShroudForge's grid_spec readiness is `world_operation_available("runtime.world.voxel.read")`, its registered
        // grid's availability (track_world_context.cpp:182-184): here the grid member and its row.
        FactRow {
            fact: Fact::WorldOperation(Op::WorldVoxelRead),
            backing: Backing::Kiln(&VOXEL_GRID),
            read: None,
        },
        // ...and so is `world_operation_available("runtime.world.voxel.grid_spec")` (track_world_context.cpp:182-184).
        FactRow {
            fact: Fact::WorldOperation(Op::WorldVoxelGridSpec),
            backing: Backing::Kiln(&VOXEL_GRID),
            read: None,
        },
        FactRow {
            fact: Fact::WorldOperation(Op::WorldCursorGet),
            backing: Backing::Kiln(&CURSOR),
            read: Some(super::world::cursor::read_available),
        },
        FactRow {
            fact: Fact::WorldOperation(Op::WorldBuildingInput),
            backing: Backing::Kiln(&BUILDING),
            read: None,
        },
        FactRow {
            fact: Fact::WorldEntityQueryPropsReady,
            backing: Backing::Kiln(&PROPS),
            read: Some(super::world::props::read_ready),
        },
        FactRow {
            fact: Fact::CanWriteCurrentTransform,
            backing: Backing::Kiln(&PROPS_WRITE),
            read: Some(super::world::props::read_can_write_transform),
        },
        FactRow {
            fact: Fact::MainMenu,
            backing: later("the game flow state (game_flow_state)"),
            read: None,
        },
        FactRow {
            fact: Fact::CharacterName,
            backing: later("the selected character name (selected_character_name)"),
            read: None,
        },
    ],
};
