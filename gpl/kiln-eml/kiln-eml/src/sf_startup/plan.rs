// SPDX-License-Identifier: GPL-3.0-or-later
//! The frozen host selections of one SF-dialect startup attempt: which packages exist, which the pass
//! registers and in which order, which of them run early, the global PATCH/EXPORT features, side and dialect, the export
//! root, the runtime operation catalog of the selected build and the frozen locale. Everything is copied in before the
//! VM exists; nothing here reads a ShroudForge config, settings or state path, and nothing changes during an attempt.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! - modules/mod-runtime/lua/mod_runner.rs:49-74 (`run`: which registered packages run their entrypoint early);
//! - modules/modloader/mod_manifest_types.rs `requires_pregame` (through sfi-package's `Manifest::requires_pregame`);
//! - modules/mod-runtime/startup/run_startup_mods.rs:18-97 (one VM per pass, every planned package registered first).
//!
//! What the host supplies and this module only checks:
//! - `registry`: every discovered package, disabled and other-side ones included. It is `loader.has_mod`'s
//!   membership and never an admission;
//! - `order`: the frozen load order of the packages this start admits to the pass (the startup controller seals it; until then the
//!   caller's validated selection). Every one is registered in the VM before any entry runs;
//! - the retry's `PassPlan` only ever removes packages; the order it runs them in is its planner's.
//!
//! Entry selection is SF's: a registered package runs its entrypoint early only when it `requires_pregame` (patch, or
//! export without a runtime capability) AND (it has `patch` and the pass patches, or it has `export` and the pass
//! exports). A runtime package with `export` never runs early; neither does a runtime-only one, whose top level is never
//! executed to look for anything.
use std::{
    collections::BTreeSet,
    fs, io,
    path::{Path, PathBuf},
    rc::Rc,
};

use sfi_core::{
    bindings::runtime::{Availability, RuntimeOperation},
    host::{Side, SourceVersion},
    package::{Manifest, discover::Registry, manifest::Capability},
};

use crate::same_path::within;
use crate::sf_pass::{PassPlan, PlannedPackage};

/// The pass's global features (SF `AppFeatures`), apart from any package's capabilities. The startup pass never loads
/// a runtime DLL: SF's RUNTIME_DLL feature has no switch here, and `loader.features.runtime` stays the runtime phase's.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Features {
    /// PATCH: the pass stages, prepares and publishes one archive generation.
    pub patch: bool,
    /// EXPORT: packages with `export` may write below the export root.
    pub export: bool,
}

/// The runtime registrar's static operation support for the selected game build: `runtime.status`/`get_operations` report it in the
/// pregame phase as SF reports its build contract there. A host input (production: the build's own contract, wired by
/// kiln mode), never a fabricated "everything available" table.
pub trait RuntimeCatalog {
    /// The build's display version (the runtime registrar's unknown-operation reason names it).
    fn game_build(&self) -> &str;
    fn static_support(&self, operation: RuntimeOperation) -> Availability;
}

/// A Kiln-owned export root, proven not to overlap the archive, its baselines and receipts, the package sources or
/// ShroudForge's own folder (`select`), so exports can never become a second game-data writer. Every package of the
/// pass exports below this one root, as SF's packages share its one export folder.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExportRoot(PathBuf);

impl ExportRoot {
    /// `root` checked against the game folder `game` (the folder holding the archive and its baselines). Refused,
    /// rather than redirected:
    /// - a relative path, a missing folder, or one that is itself a link (a later swap could point it elsewhere);
    /// - after resolving every link: the game folder, `<game>/kiln`, `<game>/kiln/eml` (ownership receipts, the
    ///   change cache), `<game>/mods` or `<game>/shroudforge`, any folder that contains one of them, and any folder
    ///   inside `<game>/kiln/eml`, `<game>/mods` or `<game>/shroudforge`. Each protected folder is compared where
    ///   it resolves too (one that is a link or junction protects its target), by name and by identity (see [`within`]:
    ///   another case, an 8.3 name or another drive letter for the same folder is the same folder), and as named while
    ///   it does not exist (a root cannot lie inside a missing folder, and one containing it contains its protected
    ///   parent). A protected folder that exists but cannot be resolved or identified refuses the root.
    ///
    /// The checked, resolved path is what the pass hands to the core's IO, whose own containment keeps every
    /// export below it.
    pub fn select(root: &Path, game: &Path) -> anyhow::Result<Self> {
        if !root.is_absolute() {
            anyhow::bail!("the export root must be an absolute path: {}", root.display());
        }
        let meta = fs::symlink_metadata(root)
            .map_err(|e| anyhow::anyhow!("the export root {} cannot be used: {e}", root.display()))?;
        if !meta.is_dir() || is_link(&meta) {
            anyhow::bail!("the export root {} is not a plain folder", root.display());
        }
        let resolved = fs::canonicalize(root)?;
        let game = fs::canonicalize(game)?;
        let resolve = |path: PathBuf| match fs::canonicalize(&path) {
            Ok(path) => Ok(path),
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(path),
            Err(e) => Err(anyhow::anyhow!(
                "the export root {} cannot be checked against {}: {e}",
                root.display(),
                path.display()
            )),
        };
        let protected = [
            game.clone(),
            resolve(game.join("kiln"))?,
            resolve(game.join("kiln").join("eml"))?,
            resolve(game.join("mods"))?,
            resolve(game.join("shroudforge"))?,
        ];
        let inside = [&protected[2], &protected[3], &protected[4]];
        let overlap = |path: &Path, base: &Path| {
            within(path, base).map_err(|e| {
                anyhow::anyhow!(
                    "the export root {} cannot be checked against {}: {e}",
                    root.display(),
                    path.display()
                )
            })
        };
        // a protected folder inside the root, or the root inside one of `inside`
        let pairs = protected.iter().map(|p| (p.as_path(), resolved.as_path()));
        for (path, base) in pairs.chain(inside.iter().map(|p| (resolved.as_path(), p.as_path()))) {
            if overlap(path, base)? {
                anyhow::bail!(
                    "the export root {} overlaps the game's data, Kiln's receipts, the package sources or ShroudForge's folder",
                    root.display()
                );
            }
        }
        Ok(Self(resolved))
    }

    pub fn path(&self) -> &Path {
        &self.0
    }
}

#[cfg(windows)]
fn is_link(meta: &fs::Metadata) -> bool {
    use std::os::windows::fs::MetadataExt as _;
    meta.file_type().is_symlink() || meta.file_attributes() & 0x400 != 0 // FILE_ATTRIBUTE_REPARSE_POINT
}
#[cfg(not(windows))]
fn is_link(meta: &fs::Metadata) -> bool {
    meta.file_type().is_symlink()
}

/// One startup attempt's frozen inputs. Built before the VM; immutable for the attempt (and its retry).
pub struct StartupPlan {
    registry: Registry,
    order: Vec<String>,
    side: Side,
    features: Features,
    export: Option<ExportRoot>,
    catalog: Rc<dyn RuntimeCatalog>,
    locale: Option<String>,
}

impl StartupPlan {
    /// Checks the selection; an unsupported configuration is refused, never repaired:
    /// - every package of `order` is discovered, enabled, for this side and listed once;
    /// - the pass exports only with an export root;
    /// - `version` is ShroudForge 1.7.9's dialect: nothing 1.8-only runs in the startup pass.
    ///
    /// `locale` is the start's UI locale as the host read it once (`shroudforge.locale` answers it for the whole
    /// attempt; `None` answers "en", as SF does for an unreadable one).
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        registry: Registry,
        order: Vec<String>,
        side: Side,
        version: SourceVersion,
        features: Features,
        export: Option<ExportRoot>,
        catalog: Rc<dyn RuntimeCatalog>,
        locale: Option<String>,
    ) -> anyhow::Result<Self> {
        if version != SourceVersion::Sf179 {
            anyhow::bail!("the startup pass runs ShroudForge 1.7.9's dialect only");
        }
        if features.export && export.is_none() {
            anyhow::bail!("the startup pass exports, but no export root was selected");
        }
        let mut seen = BTreeSet::new();
        for id in &order {
            let record = registry
                .get(id)
                .ok_or_else(|| anyhow::anyhow!("planned package is not discovered: {id}"))?;
            let manifest = &record.parsed().manifest;
            if !manifest.enabled {
                anyhow::bail!("planned package is switched off: {id}");
            }
            if !manifest.target.supports_process(side == Side::Server) {
                anyhow::bail!("planned package is for the other side: {id}");
            }
            if !seen.insert(id.as_str()) {
                anyhow::bail!("planned package is listed twice: {id}");
            }
        }
        Ok(Self {
            registry,
            order,
            side,
            features,
            export,
            catalog,
            locale,
        })
    }

    pub fn registry(&self) -> &Registry {
        &self.registry
    }
    pub fn order(&self) -> &[String] {
        &self.order
    }
    pub fn side(&self) -> Side {
        self.side
    }
    pub fn features(&self) -> Features {
        self.features
    }
    pub fn export_root(&self) -> Option<&ExportRoot> {
        self.export.as_ref()
    }
    pub fn catalog(&self) -> &Rc<dyn RuntimeCatalog> {
        &self.catalog
    }
    pub fn locale(&self) -> Option<&str> {
        self.locale.as_deref()
    }

    pub(crate) fn manifest(&self, id: &str) -> Option<&Manifest> {
        self.registry.get(id).map(|record| &record.parsed().manifest)
    }

    /// The plan the retry driver runs and narrows: every package of `order`, in order, with its required dependencies,
    /// its switched-on optional ones (those in `order`) and whether it has a pregame part.
    pub fn pass_plan(&self) -> PassPlan {
        let planned: BTreeSet<&str> = self.order.iter().map(String::as_str).collect();
        PassPlan::new(
            self.order
                .iter()
                .filter_map(|id| self.manifest(id))
                .map(|m| PlannedPackage {
                    id: m.id.clone(),
                    name: m.name.clone(),
                    version: m.version.to_string(),
                    requires: m
                        .dependencies
                        .iter()
                        .filter(|d| !d.optional.unwrap_or(false) && d.id != "shroudforge-api")
                        .map(|d| d.id.clone())
                        .collect(),
                    optional: m
                        .dependencies
                        .iter()
                        .filter(|d| d.optional.unwrap_or(false) && planned.contains(d.id.as_str()))
                        .map(|d| d.id.clone())
                        .collect(),
                    pregame: m.requires_pregame(),
                })
                .collect(),
        )
    }

    /// kiln-eml: the retry's plan when `failed` failed in its own entrypoint, as ShroudForge's fork plans its
    /// retry: this plan without `failed` and what needs it (`PassPlan::without`, the fork's closure), in the order the
    /// fork's planner gives the packages left (`fork_order`).
    pub fn retry_without(&self, failed: &str) -> PassPlan {
        self.fork_order(&self.pass_plan().without(failed))
    }

    /// kiln-eml: `pass`'s packages in the order ShroudForge's planner gives them when they are all that is
    /// switched on (modules/modloader/plan_mod_load_order.rs `visit`): the packages as roots in id order, each after
    /// its dependencies, visited in its manifest's dependency order (required ones and switched-on optional ones, the
    /// ones `pass` holds; `shroudforge-api` is no package). Only reorders: every package of `pass` once. Example
    /// `a` requires `c`, so the first pass runs `[c, a, b]`; without `a` the fork runs `[b, c]`,
    /// where keeping the first pass's order would run `[c, b]`.
    pub fn fork_order(&self, pass: &PassPlan) -> PassPlan {
        fn visit<'p>(
            plan: &StartupPlan,
            id: &'p str,
            kept: &BTreeSet<&'p str>,
            seen: &mut BTreeSet<&'p str>,
            out: &mut Vec<&'p str>,
        ) {
            if !seen.insert(id) {
                return;
            }
            if let Some(m) = plan.manifest(id) {
                for dependency in &m.dependencies {
                    if let Some(&dep) = kept.get(dependency.id.as_str()) {
                        visit(plan, dep, kept, seen, out);
                    }
                }
            }
            out.push(id);
        }
        let kept: BTreeSet<&str> = pass.packages().iter().map(|p| p.id.as_str()).collect();
        let (mut seen, mut order) = (BTreeSet::new(), Vec::with_capacity(kept.len()));
        for &id in &kept {
            visit(self, id, &kept, &mut seen, &mut order);
        }
        PassPlan::new(
            order
                .into_iter()
                .filter_map(|id| pass.packages().iter().find(|p| p.id == id).cloned())
                .collect(),
        )
    }

    /// The packages `pass` registers, in the pass's order. Refused unless every one is a package of this plan's order,
    /// listed once: a retry only ever removes packages, never adds or replaces one. The order is the pass's own:
    /// The first attempt's is this plan's, and a retry's is what its planner gives (kiln mode takes the fork's re-planned
    /// order, which can differ from this plan's when the failed package pulled a dependency ahead of its place).
    pub fn registered<'p>(&self, pass: &'p PassPlan) -> anyhow::Result<Vec<&'p str>> {
        let planned: BTreeSet<&str> = self.order.iter().map(String::as_str).collect();
        let mut seen = BTreeSet::new();
        let mut ids = Vec::new();
        for package in pass.packages() {
            if !planned.contains(package.id.as_str()) || !seen.insert(package.id.as_str()) {
                anyhow::bail!("the pass plan is not this startup plan's: {}", package.id);
            }
            ids.push(package.id.as_str());
        }
        Ok(ids)
    }

    /// SF `LuaModRunner::run`'s selection over the registered packages, in their order: the ones whose entrypoint the
    /// pass runs.
    pub fn entries<'p>(&self, registered: &[&'p str]) -> Vec<&'p str> {
        registered
            .iter()
            .copied()
            .filter(|id| self.manifest(id).is_some_and(|m| self.runs_early(m)))
            .collect()
    }

    fn runs_early(&self, m: &Manifest) -> bool {
        m.requires_pregame()
            && (m.capabilities.contains(&Capability::Patch) && self.features.patch
                || m.capabilities.contains(&Capability::Export) && self.features.export)
    }
}
