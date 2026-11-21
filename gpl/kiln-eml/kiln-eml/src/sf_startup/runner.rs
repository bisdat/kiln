// SPDX-License-Identifier: GPL-3.0-or-later
//! One SF-dialect startup attempt end to end: the stage, the baseline reader, ONE startup VM with its ports,
//! every package registered, the selected entrypoints run, the dirty resources serialized into the stage while the VM
//! still lives, then the VM closed and the prepared generation handed back for the sole publisher.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! - modules/mod-runtime/startup/run_startup_mods.rs:18-97 (one VM, setup of every planned package, `run`, then the
//!   commit with the VM alive);
//! - modules/mod-runtime/lua/mod_runner.rs:49-74,99-135 (`run` in registration order, `setup`/`setup_mod`);
//! - modules/mod-runtime/lua/require_mod.rs:70-175 (entrypoints by exact id, through the core's `LuaVm::entrypoint`);
//! - modules/mod-runtime/startup/commit_asset_changes.rs:5-36 (through `AssetSession::prepare`).
//!
//! On Kiln's one writer, never SF's transaction, lock or SF-root path:
//! 1. PATCH: `StartupHost::begin_stage` and `lend`: the host's one private stage, its writer lent to the VM. Without
//!    PATCH no stage exists and every archive write is refused, as in SF.
//! 2. `StartupHost::reference_reader`, opened after the stage (`begin_stage` re-proves the inputs first).
//! 3. The VM (`LuaVm::new`, startup phase, the plan's side, 1.7.9's dialect, the host's clock and log) with its two
//!    ports attached before any Lua access: the wrappers' `ValueTables` behind `table.insert/remove/clear` and the asset session's
//!    `BufferResources` over the pass's registry and the asset session's own typed-object seam
//!    (`ValueObjects::with_assets`), so a decoded Buffer value and `Resource.data` are the same wrappers.
//! 4. One type session (with the wrappers' recognizer for `game.types.of`) and one asset session for the VM.
//! 5. Every package of the pass registered in its order (`common::register`), before any entry runs.
//! 6. The selected entrypoints in that order (`StartupPlan::entries`). A Lua error there is that package's
//!    (`PassError::Package`, which the retry driver may retry without it); anything else is `PassError::Other`.
//! 7. PATCH: `AssetSession::prepare` closes the reader, takes the writer back from every Lua handle, serializes every
//!    replaced or edited record into the stage and prepares it, with the VM still alive (the records' values live in
//!    it). Without PATCH the reader is closed.
//! 8. The sessions and the VM are dropped: finalizers that run at its close meet the closed writer.
//!
//! Any error before the end drops the lent stage, which aborts it: the game folder stays as it was. The prepared
//! generation is published only by [`PreparedStartup::publish`] (the sole publisher), which kiln mode calls after its commit
//! barrier. Kiln mode also wires the retry driver's retry around this (`run_pass` with a NEW `StartupHost` per attempt), writes BaseOrigin
//! into the receipt and the startup outcome. The caller holds the install lock for the whole pass.
use std::{io, rc::Rc};

use mod_loader::lua::startup_host::{PreparedArchive, PublishedArchive, StartupHost};
use sfi_core::{
    bindings::loader::LoaderSnapshot,
    host::{Clock, HostContext, LogSink, Phase, SourceVersion},
    lua::{LuaVm, VmAttachments},
};

use super::{
    SelectedTarget, TypeSession,
    buffer_resource::BufferResources,
    common::{self, FrozenLocale, Shared},
    game::{assets::AssetSession, resource::AssetIdentities},
    journal::Journal,
    plan::StartupPlan,
    resource_values::{self, ValueObjects, objects::ValueTables},
};
use crate::sf_pass::{PackageFailure, PassError, PassPlan};

/// A point the attempt has reached, reported to the caller's `on_step` (progress, cancellation). An error returned
/// from `on_step` stops the attempt there, as any other failure: `PassError::Other`, the stage aborted.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Step<'a> {
    /// PATCH: the host's stage exists and its writer is lent.
    StageBegun,
    /// The baseline reader is open.
    ReaderOpened,
    /// The VM exists with its ports attached, and its sessions.
    VmReady,
    /// One package's environment and bindings are registered.
    Registered(&'a str),
    /// One package's entrypoint returned.
    EntryRan(&'a str),
    /// PATCH: the stage is prepared (dirty resources serialized, writer taken back); the VM still lives.
    Prepared,
    /// The VM and its sessions are closed.
    VmClosed,
}

/// What an attempt did that the stage cannot take back, kept even when the attempt fails (the startup controller records it with the
/// generation; a retry does not undo it).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct SideEffects {
    /// Packages whose entrypoint the attempt entered, in order.
    pub entered: Vec<String>,
    /// Of those, the ones that could export: every entered package when the pass exports, whatever its capabilities,
    /// as `io.export` follows the global EXPORT only (SF manage_exports.rs:5; the core's `ExportConfig`); a module of
    /// another package that an entry requires runs inside that entry. Files written below the export root stay,
    /// whatever happens to the archive generation.
    pub exporting: Vec<String>,
}

/// The host's clock and log for the attempt's VM (`shroudforge.clock`, the packages' log lines and Lua's warnings).
#[derive(Clone)]
pub struct Ports {
    pub clock: Rc<dyn Clock>,
    pub log: Rc<dyn LogSink>,
}

/// A finished attempt, not yet published.
pub struct PreparedStartup<'guard> {
    archive: Option<PreparedArchive<'guard>>,
    applied: usize,
    ran: Vec<String>,
    journal: Journal,
}

impl<'guard> PreparedStartup<'guard> {
    /// The prepared generation (PATCH only).
    pub fn archive(&self) -> Option<&PreparedArchive<'guard>> {
        self.archive.as_ref()
    }
    /// Resources the attempt serialized into the stage.
    pub fn applied(&self) -> usize {
        self.applied
    }
    /// The packages whose entrypoint ran, in order.
    pub fn ran(&self) -> &[String] {
        &self.ran
    }
    /// kiln-eml: what each package's entrypoint changed in this attempt (`sf_startup::journal`), recorded by its
    /// asset session from before the first entrypoint until the stage was prepared.
    pub fn journal(&self) -> &Journal {
        &self.journal
    }
    /// The sole publisher, after the caller's barrier: publishes the prepared generation (`PreparedArchive::publish`).
    /// `None` when the pass did not patch.
    pub fn publish(self) -> io::Result<Option<PublishedArchive>> {
        self.archive.map(PreparedArchive::publish).transpose()
    }
}

/// One whole attempt of `pass` (`plan.pass_plan()` or the retry driver's narrowed retry) on `host`'s generation. See the module
/// docs for the order. `effects` is appended to as entrypoints are entered.
pub fn run_attempt<'guard>(
    host: &mut StartupHost<'guard>,
    plan: &StartupPlan,
    pass: &PassPlan,
    ports: &Ports,
    effects: &mut SideEffects,
    mut on_step: impl FnMut(Step<'_>) -> anyhow::Result<()>,
) -> Result<PreparedStartup<'guard>, PassError> {
    let registered = plan.registered(pass).map_err(PassError::Other)?;
    let entries = plan.entries(&registered);
    let mut step = |s: Step<'_>| on_step(s).map_err(PassError::Other);
    let other = |e: mlua::Error| PassError::Other(anyhow::Error::msg(e.to_string()));

    // 1-2: the stage first, then the reader of the captured baseline.
    let lent = if plan.features().patch {
        let lent = host.begin_stage().map_err(PassError::Other)?.lend();
        step(Step::StageBegun)?;
        Some(lent)
    } else {
        None
    };
    let reader = host.reference_reader().map_err(PassError::Other)?;
    step(Step::ReaderOpened)?;

    // 3-4: one VM, its ports attached before any Lua access, and its sessions.
    let mut context = HostContext::new(Phase::Startup, plan.side(), SourceVersion::Sf179);
    context.clock = Some(ports.clock.clone());
    context.log = Some(ports.log.clone());
    let vm = LuaVm::new(context).map_err(other)?;
    let objects = ValueObjects::with_assets(Rc::new(AssetIdentities));
    vm.attach(VmAttachments {
        typed_tables: Some(Rc::new(ValueTables)),
        buffer_resources: Some(Rc::new(BufferResources::new(host.registry().clone(), objects.clone()))),
    })
    .map_err(other)?;
    let lua = vm.lua();
    let types = TypeSession::new(
        lua,
        host.registry().clone(),
        SelectedTarget::from_index(host.stem(), host.baseline()),
    );
    types.add_type_recognizer(resource_values::recognizer());
    let assets = AssetSession::with_objects(
        &types,
        host.baseline().clone(),
        reader,
        lent.as_ref().map(|lent| lent.writer()),
        objects.clone(),
    );
    let shared = Shared {
        plan,
        types,
        assets,
        discovered: LoaderSnapshot {
            patch: plan.features().patch,
            export: plan.features().export,
            discovered_ids: plan
                .registry()
                .values()
                .map(|r| r.parsed().manifest.id.clone())
                .collect(),
        },
        locale: Rc::new(FrozenLocale(plan.locale().map(str::to_owned))),
        objects,
    };
    step(Step::VmReady)?;

    // 5: every package registered, in order, before any entry runs.
    for id in &registered {
        let record = plan
            .registry()
            .get(id)
            .ok_or_else(|| PassError::Other(anyhow::anyhow!("registered package is not discovered: {id}")))?;
        let mut package = record.runtime().clone();
        package.export_root = plan.export_root().map(|root| root.path().to_owned());
        vm.add_package(package, |lua, env, package| {
            common::register(lua, env, package, vm.context(), &shared)
        })
        .map_err(|e| PassError::Other(anyhow::anyhow!("package {id}: {e}")))?;
        step(Step::Registered(id))?;
    }

    // 6: the selected entrypoints, in registration order. A Lua error here is the package's own.
    let mut ran = Vec::with_capacity(entries.len());
    for id in entries {
        effects.entered.push(id.to_owned());
        if plan.features().export {
            effects.exporting.push(id.to_owned());
        }
        // kiln-eml: what the session is asked to change while this entrypoint runs is this package's
        shared.assets.begin_package(id);
        let entered = vm.entrypoint(id);
        shared.assets.end_package();
        if let Err(error) = entered {
            return Err(PassError::Package(PackageFailure::new(
                id,
                error.to_string(),
                error.chain(),
            )));
        }
        ran.push(id.to_owned());
        step(Step::EntryRan(id))?;
    }

    // 7: the commit into the stage, with the VM alive. The journal is taken first: no Lua runs from here until the
    // resources are serialized (`LentStage::prepare_with`), so it holds everything the stage will.
    let journal = shared.assets.take_journal();
    let (archive, applied) = match lent {
        Some(lent) => {
            let (archive, applied) = shared.assets.prepare(lent).map_err(PassError::Other)?;
            step(Step::Prepared)?;
            (Some(archive), applied)
        }
        None => {
            shared.assets.close_reader();
            (None, 0)
        }
    };

    // 8: the sessions and the VM close before anyone publishes.
    drop(shared);
    drop(vm);
    step(Step::VmClosed)?;
    Ok(PreparedStartup {
        archive,
        applied,
        ran,
        journal,
    })
}
