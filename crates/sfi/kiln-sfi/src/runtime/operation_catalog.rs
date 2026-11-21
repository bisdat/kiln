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

/// The live world context needs kiln.world's `game_world_session` and the core's `session`
/// record (`world::context::probe`), and the core's `world.lifecycle` row: without the world-destroy
/// hook a stop is not seen.
static CONTEXT: [Need; 1] = [Need {
    member: super::world::context::probe,
    capability: super::world::context::CAPABILITY,
}];

/// Production: what this interpreter implements today. `runtime.lifecycle` needs nothing
/// from Kiln; nor do the refusal-only operations (`place`, `finish_building`,
/// `runtime.gameplay.patch`): they are in ShroudForge's contract on every build, and their
/// provider facts are false on every current profile (no `worldOperations`, no
/// `runtimePatches` from build 25966634 on), so those facts have no row
/// (`WorldOperation(place / finish_building)`, `WorldEntityContextReady`, which needs place and
/// finish_building, and `GameplayPatchAvailable`). The network core, the mod messages
/// and `connected_peers` need kiln.net's lease. `runtime.world.context.active`
/// and its two facts (the live context, and `world_operation_available` of that operation,
/// which is the live context too) need the world context's members and `world.lifecycle`. ECS readiness
/// (`Ready`) waits for the ECS queries: it needs `ecs_context_snapshot`'s view of the engine's entity table (ECS
/// memory, held with the ECS reads), so it stays that seam. Every other family lands with its
/// own rows.
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
        op(Op::EcsQuery, later("ECS queries (ecs_context_snapshot)")),
        op(Op::EcsResolve, later("ECS queries (ecs_context_snapshot)")),
        op(Op::EcsRead, later("ECS component reads and writes")),
        op(Op::EcsWrite, later("ECS component reads and writes")),
        op(Op::AssetsWrite, later("asset writes")),
        op(Op::WorldContextActive, Backing::Kiln(&CONTEXT)),
        op(Op::WorldCursorGet, later("the world cursor")),
        op(Op::WorldBuildingInput, later("building input")),
        op(Op::WorldVoxelRead, later("voxel reads and writes")),
        op(Op::WorldVoxelWrite, later("voxel reads and writes")),
        op(Op::WorldVoxelGridSpec, later("voxel reads and writes")),
        op(Op::WorldEntitySpawn, later("entity spawn and destroy")),
        op(
            Op::WorldEntityQueryProps,
            later("entity props and transforms"),
        ),
        op(
            Op::WorldEntityQueryPropsInBounds,
            later("entity props and transforms"),
        ),
        op(
            Op::WorldEntityRegisterPropRecipes,
            later("entity props and transforms"),
        ),
        op(
            Op::WorldEntityGetTransform,
            later("entity props and transforms"),
        ),
        op(
            Op::WorldEntitySetScale,
            later("entity props and transforms"),
        ),
        op(Op::WorldEntityPlace, Backing::Interpreter),
        op(Op::WorldEntityDestroy, later("entity spawn and destroy")),
        op(Op::WorldEntityFinishBuilding, Backing::Interpreter),
        op(Op::GameplayPatch, Backing::Interpreter),
    ],
    facts: &[
        FactRow {
            fact: Fact::Ready,
            backing: later("ECS queries (ecs_context_snapshot, held with the ECS reads)"),
            read: None,
        },
        FactRow {
            fact: Fact::CanWrite,
            backing: later("the CAS write core"),
            read: None,
        },
        FactRow {
            fact: Fact::ResourceCanWrite,
            backing: later("asset writes"),
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
        FactRow {
            fact: Fact::WorldEntityQueryPropsReady,
            backing: later("entity props and transforms"),
            read: None,
        },
        FactRow {
            fact: Fact::CanWriteCurrentTransform,
            backing: later("entity props and transforms"),
            read: None,
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
