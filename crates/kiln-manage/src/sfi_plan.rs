// SPDX-License-Identifier: MIT
//! The launch-plan producer: at activation, kiln-manage writes the hash-pinned launch plan
//! `<game>/kiln/launch-plans/<plan id>.json` that the core freezes at boot and kiln-sfi and kiln-eml read.
//!
//! **When a plan is written** ([`produce`]): only when kiln.json selects Kiln's interpreter (`shroudforge.backend:
//! "kiln"`, read as the core reads it at boot: [`crate::sfi_backend::resolve_game`]). Hosted and installed starts
//! get no plan and nothing here touches them ([`Outcome::NotKiln`]). While the capability `kiln-sfi.interpreter` is
//! held (the release hold, lifted only by kiln.json `"preview"` exactly as the core's gate lifts it), `kiln` is
//! [`Outcome::Refused`]: no plan, so the core's own refusal stands at boot (every ShroudForge backend stands aside,
//! with its reason), never a silent hosted start.
//!
//! **Fail closed.** Any input that cannot be read or does not hold together (kiln.json, the mods folder, a chain
//! file, the placement receipts, the compatibility rules' location, the archive inspection, the shipped policies) is
//! a [`Outcome::Refused`] with its reason, and no plan is written. A plan that kiln-manage's own validation (the
//! strict reader kiln-sfi uses, plus the core's checks) would refuse is never written either.
//!
//! **What a plan says.** Every package folder under `<game>/mods` (the pre-select inventory, `crate::sfi_scan`): the
//! candidates in ShroudForge's load order, and every other package with its decision (`skip` with its actionable
//! reasons and dependency chain, `disabled`, `other-side`). Only the inventory's authoritative blockers keep a package out;
//! its advisories (the Lua scan, parity answers) never do and stay out of the plan ([`Written::advisories`]). A
//! candidate's required dependencies are candidates ordered before it, transitively (a package runs whole or not at
//! all); its DLLs are planned with their SHA-256 and a gate pass. A folder the plan cannot describe exactly (no
//! readable manifest, a duplicate mod id, a folder name Windows would not take, two names that differ only in letter
//! case, files that cannot be fingerprinted, a folder with a `mod.json` the inventory could not open or name) is not
//! planned: a top-level warning names it, and nothing of it runs in a `kiln` start. A mods folder that is a link or
//! junction, or that cannot be listed in full, gives no plan. Disabled and other-side packages are statuses, not warnings.
//!
//! **Identity.** The plan's SHA-256 is its identity: the profile's `sfLaunchPlan {path, sha256}` pins it
//! ([`PlanRef`], written into profile.json by the activation). The plan id is a random UUID per activation,
//! never a time, PID, generation or hash; everything else is a pure function of the inputs, so equal inputs and an
//! equal id give equal bytes ([`render`]): keys in byte order, two-space indents, one trailing newline.
//!
//! **Fingerprints** (each a SHA-256 over length-framed, domain-separated fields, [`Frame`]): `packages` (every
//! package record and every folder left out), `settings` (kiln.json's `shroudforge` and `native` sections, the
//! DLL Manager inputs), `chain` (the chain files the plan's start reads: `kiln/chain.ini` and `dinput8-chain.ini`
//! for the active profile's plan; a play-once plan also `kiln/chain/once.ini` and `kiln/play-once.ini`, so a play-once
//! start leaves the active plan current), `loaderInventory` (the inspector's archive inventory and every `.dll`/`.asi` file in the
//! game folder itself, with its SHA-256), `compat` (the rules file's SHA-256, ShroudForge's embedded default's, or the
//! error), `releasePolicy` (the gate list, the 1.8 record, the save-sensitive policy) and `placementReceipts`
//! (`kiln/state/library-placements.json`, the archive inspector's receipt, base evidence, input class, origin and
//! notes). The archive digests and `game.archiveBaseOrigin` come from kiln-eml's read-only inspector
//! ([`crate::sfi_inspection`]).
//!
//! **Compatibility rules** (one constant: [`RULES_FILE`]): Kiln's own
//! `<game>/kiln/config/sfi/compatibility/rules.json`, read as ShroudForge reads its own; a missing file is
//! ShroudForge's embedded default, a malformed one ShroudForge's result: no package of the family runs, each says why,
//! and the plan warns.
//!
//! **Never** read or written: anything below `<game>/shroudforge` (the rules reader and the inventory refuse it; the
//! game folder's listing skips it by name); no Lua runs and no DLL loads.
use crate::manifest::RunSide;
use crate::sfi_backend::{self, Backend, Env, Resolution};
use crate::sfi_inspection::{self, Facts, Inspect};
use crate::sfi_scan::{
    self, Inventory, Kind, PackageEntry, Presence, native::NativeConfig, parity::CatalogInput,
    safety,
};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use sfi_package::package::rules_file::{self, RulesRead};
use sfi_policy::digest;
use sfi_policy::launch_plan::{LaunchPlan, ValidationContext};
use sfi_policy::native_selection::{NativePermission, NativePolicy};
use sfi_policy::same_folder;
use sha2::{Digest as _, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::io::Read as _;
use std::path::{Path, PathBuf};

/// The folder below `<game>/kiln` that holds launch plans.
pub const PLAN_FOLDER: &str = "launch-plans";
/// The largest plan a reader takes (kiln_launch.h, sfi-policy).
pub const PLAN_MAX: usize = 8 * 1024 * 1024;
/// The most packages a plan lists (sfi-policy).
pub const MAX_PACKAGES: usize = 4096;
/// Where Kiln keeps ShroudForge's compatibility rules for its interpreter, game-relative (a
/// Kiln-owned location; ShroudForge's own copy in `<game>/shroudforge` is never read).
pub const RULES_FILE: &str = "kiln/config/sfi/compatibility/rules.json";
/// The ShroudForge release Kiln's interpreter answers for, and its source pin.
pub const API_VERSION: &str = "1.7.9";
pub const SF179_SOURCE_COMMIT: &str = "d8a394bcb180ceb5bdfce48048dcc4f2e0108bd8";
/// The gate list and service ABI inventory this build ships (the core and kiln-sfi are built from the same files).
const GATE_LIST: &str = include_str!("../../../native/core/gates/sfi.json");
const ABI_MANIFEST: &[u8] = include_bytes!("../../sfi/kiln-abi/abi-manifest.json");

/// `profile.mode`: the active profile (profile.json) or a play-once one (profile.once.json).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Mode {
    Active,
    Once,
}

/// `profile` of the plan: the profile the activation is about to publish.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ProfileIdentity {
    pub id: String,
    pub generation: u64,
    pub mode: Mode,
    pub semantic_sha256: String,
}
impl ProfileIdentity {
    /// The identity of the profile document `profile` (profile.json or profile.once.json as it will be written, with or
    /// without its `sfLaunchPlan`): its id, generation and [`semantic_sha256`].
    pub fn of(profile: &Value, mode: Mode) -> Result<Self, String> {
        let id = profile["id"]
            .as_str()
            .filter(|s| !s.is_empty() && !s.contains('\0'))
            .ok_or("the profile has no id")?;
        let generation = profile["generation"]
            .as_u64()
            .filter(|g| *g <= i64::MAX as u64)
            .ok_or("the profile has no generation")?;
        Ok(Self {
            id: id.to_owned(),
            generation,
            mode,
            semantic_sha256: semantic_sha256(profile)?,
        })
    }
}

/// The profile's execution projection, hashed: the whole document but `$schema`, `written`,
/// `writer` and `sfLaunchPlan` (no circular hash: the plan reference goes into the profile last), as canonical JSON.
pub fn semantic_sha256(profile: &Value) -> Result<String, String> {
    let mut doc = profile
        .as_object()
        .ok_or("the profile is not a JSON object")?
        .clone();
    for key in ["$schema", "written", "writer", "sfLaunchPlan"] {
        doc.remove(key);
    }
    let mut f = Frame::new("profile-semantic");
    f.text(&canonical(&Value::Object(doc)));
    Ok(f.finish())
}

/// A reason as the plan carries it (`reasons[]`, `warnings[]`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Reason {
    pub code: String,
    pub message: String,
    /// The dependency chain, nearest first, or what the reason is about.
    pub caused_by: Vec<String>,
    /// info, warning, save-warning or infrastructure-error.
    pub severity: &'static str,
}
impl Reason {
    /// A reason every reader takes: code and message not empty, no NUL, `causedBy` without empty or repeated entries.
    fn new(
        code: &str,
        message: impl Into<String>,
        caused_by: Vec<String>,
        severity: &'static str,
    ) -> Self {
        let clean = |s: &str| s.replace('\0', "?");
        let code = if code.is_empty() {
            "unknown".to_owned()
        } else {
            clean(code)
        };
        let message = clean(&message.into());
        let message = if message.is_empty() {
            code.clone()
        } else {
            message
        };
        let mut seen = BTreeSet::new();
        let caused_by = caused_by
            .iter()
            .map(|c| clean(c))
            .filter(|c| !c.is_empty() && seen.insert(c.clone()))
            .collect();
        Self {
            code,
            message,
            caused_by,
            severity,
        }
    }
    fn value(&self) -> Value {
        json!({"code": self.code, "message": self.message, "causedBy": self.caused_by, "severity": self.severity})
    }
}

/// What the producer needs besides the game folder's own files.
pub struct Request<'a> {
    pub game: &'a Path,
    pub side: RunSide,
    /// The environment the game process will see (`KILN_SHROUDFORGE`, `KILN_SHROUDFORGE_RUNTIME`).
    pub env: &'a Env,
    pub profile: &'a ProfileIdentity,
    /// kiln-eml's archive inspector (the shipped helper, [`sfi_inspection::Tool`]).
    pub inspector: &'a dyn Inspect,
    /// Kiln's DLL Manager inputs and the native review registry. Its `permission` is replaced by kiln.json's
    /// `native.policy` as the core reads it ([`native_permission`]).
    pub native: NativeConfig,
    pub policy: &'a safety::Policy,
    pub catalog: CatalogInput<'a>,
    /// Findings the launcher carries into the plan (a save-sensitive package switched off since the last
    /// activation); kiln-sfi copies the plan's warnings into sfi.json, so the in-game notice shows them too.
    pub carried: &'a [Reason],
}

/// Where a written plan is: the profile's `sfLaunchPlan`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PlanRef {
    /// `launch-plans/<plan id>.json`, relative to `<game>/kiln`.
    pub path: String,
    /// The exact file's SHA-256, lower-case hex.
    pub sha256: String,
}
impl PlanRef {
    /// The profile member: `{"path": ..., "sha256": ...}`.
    pub fn profile_value(&self) -> Value {
        json!({"path": self.path, "sha256": self.sha256})
    }
}

/// A plan written, and what it decided.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Written {
    pub reference: PlanRef,
    pub plan_id: String,
    /// The candidates in load order.
    pub load_order: Vec<String>,
    /// Every listed package's decision, by id.
    pub decisions: BTreeMap<String, &'static str>,
    /// The plan's top-level warnings.
    pub warnings: Vec<Reason>,
    /// Warning-only findings of the pre-select scan (never in the plan): (folder, class, detail).
    pub advisories: Vec<Advisory>,
    pub resolution: Resolution,
}

/// A warning-only finding of the pre-select scan: (folder, class, detail).
pub type Advisory = (String, String, String);

/// What [`produce`] did.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase", tag = "outcome")]
pub enum Outcome {
    /// kiln.json does not select Kiln's interpreter: no plan, nothing written, hosted/installed unchanged.
    NotKiln {
        resolution: Resolution,
    },
    /// Kiln's interpreter was asked for and no plan was written: the core refuses that start at boot with its own
    /// reason (every ShroudForge backend stands aside). `code`: interpreter-held, interpreter-off (`KILN_SHROUDFORGE=0`),
    /// interpreter-missing (no `kiln/plugins/kiln-sfi.dll`), eml-r-loader, input-unreadable, no-archive-base, plan-invalid or
    /// write-failed.
    Refused {
        code: &'static str,
        reason: String,
        resolution: Resolution,
    },
    Written(Box<Written>),
}

/// The plan's facts before an id is drawn: [`render`] makes the bytes.
#[derive(Debug, Clone)]
pub struct Draft {
    pub writer_version: String,
    pub profile: ProfileIdentity,
    pub side: RunSide,
    pub facts: Facts,
    pub selection_reason: String,
    pub contract: [String; 3],
    pub fingerprints: [(&'static str, String); 7],
    pub packages: Vec<Value>,
    pub load_order: Vec<String>,
    pub dialect: &'static str,
    pub warnings: Vec<Reason>,
}

fn side_name(side: RunSide) -> &'static str {
    match side {
        RunSide::Client => "client",
        RunSide::Server => "server",
    }
}

/// Writes this activation's plan when kiln.json selects Kiln's interpreter (see the module docs).
pub fn produce(req: Request<'_>) -> Outcome {
    let game = req.game;
    produce_with(req, &mut |id, bytes| write_plan(game, id, bytes))
}

/// What publishes a plan for [`produce_with`]: (`id`, the plan's bytes) -> its reference; Ok(None): that name is
/// taken, and another id is drawn.
pub type PlanWriter<'a> = dyn FnMut(&str, &[u8]) -> Result<Option<PlanRef>, String> + 'a;

/// [`produce`], the plan published by `write`: the activation journal's writer (crate::sfi_activation,
/// `Staging::write_plan`), which records the plan's name and SHA-256 before it calls [`write_plan`].
pub fn produce_with(req: Request<'_>, write: &mut PlanWriter<'_>) -> Outcome {
    let resolution = sfi_backend::resolve_game(req.game, req.side, req.env);
    produce_resolved(req, resolution, write)
}

/// [`produce_with`] for the answer `resolution` the caller read kiln.json to (`sfi_backend::resolve_game` with this
/// request's side and environment): an activation reads kiln.json once and routes the ShroudForge-format mods by the
/// same answer (`crate::sfi_routing::route_from`). A plan is written only for a start Kiln's interpreter can run:
/// `KILN_SHROUDFORGE=0` stands it aside and without `kiln/plugins/kiln-sfi.dll` nothing runs a plan, so both are
/// refused before anything is read (a plan then would only bring the startup data of packages whose runtime never
/// starts).
pub fn produce_resolved(
    req: Request<'_>,
    resolution: Resolution,
    write: &mut PlanWriter<'_>,
) -> Outcome {
    let route = crate::sfi_routing::route_from(req.game, &resolution);
    produce_routed(req, resolution, &route, write)
}

pub(crate) fn produce_routed(
    req: Request<'_>,
    resolution: Resolution,
    route: &crate::sfi_routing::Route,
    write: &mut PlanWriter<'_>,
) -> Outcome {
    if resolution.selected != Some(Backend::Kiln) {
        if resolution.requested == Some(Backend::Kiln) {
            let reason = format!(
                "{}; no launch plan was written, so the game starts without ShroudForge packages until it is lifted",
                resolution.reason
            );
            return Outcome::Refused {
                code: "interpreter-held",
                reason,
                resolution,
            };
        }
        return Outcome::NotKiln { resolution };
    }
    if matches!(route, crate::sfi_routing::Route::Legacy(crate::sfi_routing::Legacy::EmlR(_))) {
        return eml_r_blocked(resolution);
    }
    if resolution.env_off_note.is_some() {
        return interpreter_off(resolution);
    }
    if !req
        .game
        .join(crate::sfi_routing::INTERPRETER_PLUGIN)
        .is_file()
    {
        return interpreter_missing(resolution);
    }
    let refused = |code, reason: String| Outcome::Refused {
        code,
        reason: format!(
            "{reason}; no launch plan was written, so the game starts without ShroudForge packages"
        ),
        resolution: resolution.clone(),
    };
    let (draft, advisories) = match draft(&req, &resolution) {
        Ok(d) => d,
        Err((code, why)) => return refused(code, why),
    };
    let mut last = String::new();
    for _ in 0..4 {
        let id = match crate::util::uuid_v4() {
            Ok(id) => id,
            Err(e) => return refused("write-failed", format!("no plan id: {}", e.message)),
        };
        let bytes = render(&draft, &id);
        if let Err(why) = self_check(&bytes, &draft, &id) {
            return refused(
                "plan-invalid",
                format!("the launch plan Kiln made does not pass its own checks ({why})"),
            );
        }
        match write(&id, &bytes) {
            Ok(Some(reference)) => {
                let decisions = draft
                    .packages
                    .iter()
                    .filter_map(|p| {
                        Some((
                            p["id"].as_str()?.to_owned(),
                            decision_word(p["decision"].as_str()?),
                        ))
                    })
                    .collect();
                return Outcome::Written(Box::new(Written {
                    reference,
                    plan_id: id,
                    load_order: draft.load_order.clone(),
                    decisions,
                    warnings: draft.warnings.clone(),
                    advisories,
                    resolution,
                }));
            }
            // the id's file name is taken (never by Kiln: a fresh random id): another id
            Ok(None) => last = format!("{PLAN_FOLDER}/{id}.json exists already"),
            Err(why) => return refused("write-failed", why),
        }
    }
    refused("write-failed", last)
}

pub(crate) const EML_R_LOADER_CODE: &str = "eml-r-loader";

pub(crate) fn eml_r_blocked(resolution: Resolution) -> Outcome {
    Outcome::Refused {
        code: EML_R_LOADER_CODE,
        reason: crate::sfi_routing::EML_R_BLOCK_REASON.into(),
        resolution,
    }
}

/// The refusal of a start `KILN_SHROUDFORGE=0` stands Kiln's interpreter aside for (`interpreter-off`).
pub(crate) fn interpreter_off(resolution: Resolution) -> Outcome {
    Outcome::Refused {
        code: "interpreter-off",
        reason: "KILN_SHROUDFORGE=0 stands every ShroudForge backend aside, Kiln's interpreter too; no launch plan \
                 was written, so the game starts without ShroudForge packages"
            .into(),
        resolution,
    }
}

/// The refusal of a start whose interpreter is not installed (`interpreter-missing`: no `kiln/plugins/kiln-sfi.dll`).
pub(crate) fn interpreter_missing(resolution: Resolution) -> Outcome {
    Outcome::Refused {
        code: "interpreter-missing",
        reason: format!(
            "kiln.json selects Kiln's interpreter, which is not installed ({}); no launch plan was \
             written, so the game starts without ShroudForge packages",
            crate::sfi_routing::INTERPRETER_PLUGIN
        ),
        resolution,
    }
}

fn decision_word(d: &str) -> &'static str {
    match d {
        "candidate" => "candidate",
        "disabled" => "disabled",
        "other-side" => "other-side",
        _ => "skip",
    }
}

type Refusal = (&'static str, String);

/// Gathers every input and decides the plan (no id yet). Read-only.
pub fn draft(
    req: &Request<'_>,
    resolution: &Resolution,
) -> Result<(Draft, Vec<Advisory>), Refusal> {
    let unreadable = |why: String| ("input-unreadable", why);
    let game = req.game;
    if !digest::plain_folder(&game.join(crate::paths::game::KILN)) {
        return Err(unreadable(
            "the game folder has no plain kiln folder (is Kiln installed in it?)".into(),
        ));
    }
    // kiln.json as the core reads it: the native policy and the fingerprint
    let kiln_json = read_optional(game, "kiln/kiln.json").map_err(unreadable)?;
    let parsed = kiln_json
        .as_deref()
        .and_then(|b| crate::jsonc::parse_tree_bytes(b).ok());
    let doc = parsed.as_ref().map(|(_, tree)| tree.to_value());
    let config = parsed
        .as_ref()
        .zip(doc.as_ref())
        .and_then(|((_, tree), doc)| sfi_backend::config_view(tree, doc));
    let n = &req.native;
    let native = NativeConfig {
        manager_enabled: n.manager_enabled,
        exclusions: n.exclusions.clone(),
        selections: n.selections.clone(),
        registrations: n.registrations.clone(),
        permission: native_permission(config, req.side),
        contracts: n.contracts.clone(),
    };
    // the mods folder must be readable when it is there (an empty plan from an unreadable folder would hide it)
    let mods = game.join("mods");
    match std::fs::symlink_metadata(&mods) {
        Ok(m) if m.is_dir() && !m.file_type().is_symlink() => {
            std::fs::read_dir(&mods)
                .map_err(|e| unreadable(format!("the mods folder cannot be read ({e})")))?;
        }
        Ok(_) => {
            return Err(unreadable(
                "mods in the game folder is a link or junction, or not a folder: Kiln's interpreter plans only a plain \
                 mods folder"
                    .into(),
            ));
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
        Err(e) => return Err(unreadable(format!("the mods folder cannot be read ({e})"))),
    }
    // the compatibility rules, read as ShroudForge reads its own; refused below <game>/shroudforge
    let rules = rules_file::read_rules(&game.join(RULES_FILE), game);
    let rules_ok = match &rules {
        RulesRead::Embedded(r) | RulesRead::File { rules: r, .. } => Some(r),
        RulesRead::Error(_) => None,
    };
    // the archive: kiln-eml's read-only inspector, a separate process
    let facts = sfi_inspection::inspect(req.inspector, game, req.side)
        .map_err(|why| ("no-archive-base", why))?;
    let inventory = sfi_scan::inventory(&sfi_scan::Inputs {
        game,
        side: req.side,
        profiles: None,
        rules: rules_ok,
        native: &native,
        policy: req.policy,
        catalog: match &req.catalog {
            CatalogInput::Missing => CatalogInput::Missing,
            CatalogInput::Provided { bytes, expected } => {
                CatalogInput::Provided { bytes, expected }
            }
        },
    });
    let mut decided = decide(game, &inventory, req.side, &rules, req.carried);
    // folders the inventory could not open or name are not left out without a word
    for (folder, why) in unlisted(game, &inventory).map_err(unreadable)? {
        decided.warnings.push(Reason::new(
            "package-not-planned",
            format!("{folder}: {why}; nothing of it runs this start"),
            vec![folder.clone()],
            "warning",
        ));
        decided.unplanned.push((folder, why));
    }
    if decided.packages.len() > MAX_PACKAGES {
        return Err((
            "plan-invalid",
            format!(
                "{} packages, more than a plan lists ({MAX_PACKAGES})",
                decided.packages.len()
            ),
        ));
    }
    // fingerprints
    let mut fp_packages = Frame::new("packages");
    for p in &decided.packages {
        fp_packages.text(&canonical(p));
    }
    for (folder, why) in &decided.unplanned {
        fp_packages.text(folder);
        fp_packages.text(why);
    }
    let mut fp_settings = Frame::new("settings");
    let section = |key: &str| {
        doc.as_ref()
            .and_then(|d| d.get(key))
            .map_or_else(|| "absent".to_owned(), canonical)
    };
    fp_settings.text(&section("shroudforge"));
    fp_settings.text(&section("native"));
    fp_settings.text(&format!("{:?}", native.permission));
    fp_settings.text(&format!(
        "{} {:?} {:?} {:?}",
        native.manager_enabled, native.exclusions, native.selections, native.registrations
    ));
    fp_settings.text(&resolution.reason);
    // the chain files this plan's start reads: a start of the active profile never reads a play-once start's files
    // (play-once.ini there means the start is not this plan's), so writing or removing them leaves the plan current
    let chain_files: &[&str] = match req.profile.mode {
        Mode::Active => &[crate::paths::game::CHAIN, crate::paths::game::EMF_CHAIN],
        Mode::Once => &[
            crate::paths::game::CHAIN,
            crate::paths::game::CHAIN_ONCE,
            crate::paths::game::PLAY_ONCE,
            crate::paths::game::EMF_CHAIN,
        ],
    };
    let mut fp_chain = Frame::new("chain");
    for rel in chain_files.iter().copied() {
        fp_chain.text(rel);
        fp_chain.optional(read_optional(game, rel).map_err(unreadable)?.as_deref());
    }
    let mut fp_loaders = Frame::new("loader-inventory");
    for (role, file, size) in &facts.inventory {
        fp_loaders.text(role);
        fp_loaders.text(file);
        fp_loaders.text(&size.to_string());
    }
    for (name, size, sha) in game_modules(game).map_err(unreadable)? {
        fp_loaders.text(&name);
        fp_loaders.text(&size.to_string());
        fp_loaders.text(&sha);
    }
    let compat = match &rules {
        RulesRead::File { sha256, .. } => crate::hash::hex(sha256),
        RulesRead::Embedded(_) => crate::hash::sha256_hex(rules_file::EMBEDDED_RULES.as_bytes()),
        RulesRead::Error(e) => {
            let mut f = Frame::new("compat-error");
            f.text(e);
            f.finish()
        }
    };
    let mut fp_release = Frame::new("release-policy");
    fp_release.text(GATE_LIST);
    fp_release.text(if crate::sfi_release::UPSTREAM_180_FINAL {
        "180-final"
    } else {
        "180-not-final"
    });
    fp_release.bytes(sfi_scan::SHIPPED_POLICY);
    let mut fp_receipts = Frame::new("placement-receipts");
    fp_receipts.optional(
        read_optional(game, "kiln/state/library-placements.json")
            .map_err(unreadable)?
            .as_deref(),
    );
    for part in &facts.receipt {
        fp_receipts.text(part);
    }
    for part in [&facts.evidence, &facts.input_class, &facts.containers] {
        fp_receipts.text(part);
    }
    fp_receipts.text(facts.origin);
    for note in &facts.notes {
        fp_receipts.text(note);
    }
    // the contract: the parity catalog's revision inventory and digest when it is verified, else said unknown
    let catalog = |v: Option<&String>, what: &str| match v
        .filter(|_| inventory.catalog.status == "verified")
    {
        Some(s)
            if s.len() == 64
                && s.bytes()
                    .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)) =>
        {
            s.clone()
        }
        Some(s) => crate::hash::sha256_hex(s.as_bytes()),
        None => {
            let mut f = Frame::new("contract-unknown");
            f.text(what);
            f.finish()
        }
    };
    let mut services = Frame::new("service-contracts");
    services.bytes(ABI_MANIFEST);
    services.bytes(sfi_policy::native_contracts::SHIPPED);
    let contract = [
        catalog(inventory.catalog.inventory_sha256.as_ref(), "inventory"),
        catalog(inventory.catalog.digest.as_ref(), "parity"),
        services.finish(),
    ];
    let warnings = decided.warnings.clone();
    let advisories = inventory
        .packages
        .iter()
        .flat_map(|p| {
            p.advisories
                .iter()
                .map(move |a| (p.directory.clone(), a.class.clone(), a.detail.clone()))
        })
        .collect();
    let draft = Draft {
        writer_version: env!("CARGO_PKG_VERSION").to_owned(),
        profile: req.profile.clone(),
        side: req.side,
        facts,
        selection_reason: resolution.reason.clone(),
        contract,
        fingerprints: [
            ("packages_sha256", fp_packages.finish()),
            ("settings_sha256", fp_settings.finish()),
            ("chain_sha256", fp_chain.finish()),
            ("loaderInventory_sha256", fp_loaders.finish()),
            ("compat_sha256", compat),
            ("releasePolicy_sha256", fp_release.finish()),
            ("placementReceipts_sha256", fp_receipts.finish()),
        ],
        packages: decided.packages,
        load_order: decided.load_order,
        dialect: decided.dialect,
        warnings,
    };
    Ok((draft, advisories))
}

/// Package folders under `<game>/mods` that ShroudForge would read (a plain folder with a `mod.json`, or one that cannot
/// be looked into) but the pre-select inventory does not list, because it could not open the folder or read its name, or could
/// not read the mods folder as a whole: (`mods/<folder>`, why), for the plan's warnings. Hidden folders, links and
/// junctions, files, and folders without a `mod.json` are not packages to ShroudForge either. Err when `<game>/mods`
/// cannot be listed in full.
fn unlisted(game: &Path, inv: &Inventory) -> Result<Vec<(String, String)>, String> {
    let entries = match std::fs::read_dir(game.join("mods")) {
        Ok(entries) => entries,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(format!("the mods folder cannot be read ({e})")),
    };
    let listed: BTreeSet<&str> = inv.packages.iter().map(|p| p.directory.as_str()).collect();
    let mut out = Vec::new();
    for entry in entries {
        let entry = entry.map_err(|e| format!("the mods folder cannot be read ({e})"))?;
        let kind = entry
            .file_type()
            .map_err(|e| format!("the mods folder cannot be read ({e})"))?;
        if !kind.is_dir() {
            continue;
        }
        let name = entry.file_name();
        let shown = name.to_string_lossy();
        if shown.starts_with('.') || name.to_str().is_some_and(|n| listed.contains(n)) {
            continue;
        }
        let why = match std::fs::symlink_metadata(entry.path().join("mod.json")) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => continue,
            _ if name.to_str().is_none() => "its folder name is not valid Unicode".to_owned(),
            Ok(_) => "Kiln could not read it as a package".to_owned(),
            Err(e) => format!("it cannot be read ({e})"),
        };
        out.push((format!("mods/{shown}"), why));
    }
    out.sort();
    Ok(out)
}

/// kiln.json `native.policy` as config.cpp reads it for `side` (`config`: kiln.json as config.cpp parses it,
/// [`sfi_backend::config_view`]; None: the defaults), as the plan producer applies it: `never` drops every package
/// that ships native code (the inventory's wording); `verified-only` keeps them, since only the core knows at boot whether
/// this build is verified, and kiln-sfi withdraws them there when it is not (the core may only remove).
pub fn native_permission(config: Option<&Value>, side: RunSide) -> NativePermission {
    let read = |sec: &Value, policy: &mut NativePolicy| {
        if let Some(p) = sec
            .get("native")
            .filter(|n| n.is_object())
            .and_then(|n| n.get("policy"))
        {
            match p.as_str() {
                Some("always") => *policy = NativePolicy::Always,
                Some("verified-only") => *policy = NativePolicy::VerifiedOnly,
                Some("never") => *policy = NativePolicy::Never,
                _ => {} // config.cpp: an unknown value keeps the current one
            }
        }
    };
    // config.cpp reads policy, clr, plugins and log_level first; a wrong type there stops that section's reading
    let typed = |sec: &Value| {
        let ok = |key: &str, f: fn(&Value) -> bool| sec.get(key).is_none_or(f);
        ok("policy", Value::is_string)
            && ok("clr", Value::is_boolean)
            && ok("plugins", Value::is_boolean)
            && ok("log_level", Value::is_string)
    };
    let mut policy = NativePolicy::Always;
    if let Some(doc) = config.filter(|d| d.is_object()) {
        if typed(doc) {
            read(doc, &mut policy);
        }
        if let Some(sec) = doc.get(side_name(side)).filter(|s| s.is_object())
            && typed(sec)
        {
            read(sec, &mut policy);
        }
    }
    match policy {
        NativePolicy::Always => NativePermission::ALWAYS,
        NativePolicy::VerifiedOnly => NativePermission {
            policy,
            verified: true,
        },
        NativePolicy::Never => NativePermission {
            policy,
            verified: false,
        },
    }
}

/// What [`decide`] made of the inventory.
#[derive(Debug, Clone)]
pub struct Decided {
    /// The plan's package records, sorted by folder (case folded).
    pub packages: Vec<Value>,
    /// The candidates in ShroudForge's load order.
    pub load_order: Vec<String>,
    /// shroudforge, eml or none (the strict reader's rule over the listed packages).
    pub dialect: &'static str,
    pub warnings: Vec<Reason>,
    /// Folders not planned: (`mods/<folder>`, why).
    pub unplanned: Vec<(String, String)>,
}

/// The plan's decisions from the pre-select inventory (pure but for hashing each listed package folder).
pub fn decide(
    game: &Path,
    inv: &Inventory,
    side: RunSide,
    rules: &RulesRead,
    carried: &[Reason],
) -> Decided {
    let side = side_name(side);
    let mut warnings: Vec<Reason> = Vec::new();
    let mut seen = BTreeSet::new();
    for r in carried {
        if seen.insert((r.code.clone(), r.caused_by.clone(), r.message.clone())) {
            warnings.push(Reason::new(
                &r.code,
                r.message.clone(),
                r.caused_by.clone(),
                r.severity,
            ));
        }
    }
    let rules_error = match rules {
        RulesRead::Error(e) => {
            let text = sfi_package::package::plan::Plan::compatibility_error(e.clone())
                .issues
                .remove(0)
                .detail;
            warnings.push(Reason::new(
                "compatibility-rules",
                format!(
                    "{text}: no ShroudForge package runs this start; fix or remove {RULES_FILE}"
                ),
                vec![RULES_FILE.to_owned()],
                "warning",
            ));
            Some(text)
        }
        _ => None,
    };
    // which folders the plan can describe exactly
    let mut ids: BTreeMap<&str, usize> = BTreeMap::new();
    let mut folded: BTreeMap<String, usize> = BTreeMap::new();
    for p in &inv.packages {
        if let Some(id) = &p.id {
            *ids.entry(id.as_str()).or_default() += 1;
        }
        // the strict reader's name key, so the reader never refuses for a collision the producer did not see
        *folded
            .entry(same_folder::name_key(&p.directory))
            .or_default() += 1;
    }
    let mut unplanned: Vec<(String, String)> = Vec::new();
    let mut off: BTreeSet<String> = BTreeSet::new();
    let mut listed: Vec<(&PackageEntry, String)> = Vec::new();
    for p in &inv.packages {
        let why = match (&p.id, &p.version) {
            _ if !folder_ok(&p.directory) => {
                Some("its folder name is not one Windows takes as it is".to_owned())
            }
            _ if folded[&same_folder::name_key(&p.directory)] > 1 => {
                Some("another folder has the same name in another letter case".to_owned())
            }
            (Some(id), Some(_)) if id == "shroudforge-api" => {
                Some("its mod id is ShroudForge's own API".to_owned())
            }
            (Some(id), Some(_)) if ids[id.as_str()] > 1 => Some(format!("duplicate mod ID: {id}")),
            (Some(_), Some(_)) => None,
            _ => Some(match p.blockers.first() {
                Some(b) => format!("its mod.json cannot be read ({})", b.detail),
                None => "it has no mod.json Kiln can read".to_owned(),
            }),
        };
        // the digest kiln-sfi computes again at runtime-create (sfi-policy, one function for both sides)
        let content = match why {
            Some(why) => Err(why),
            None => digest::package_content_sha256(&game.join("mods").join(&p.directory))
                .map(|d| crate::hash::hex(&d))
                .map_err(|e| format!("its files cannot be fingerprinted ({e})")),
        };
        match content {
            Ok(content) => listed.push((p, content)),
            Err(why) => {
                let folder = format!("mods/{}", p.directory);
                // a switched-off package is a status, not a warning: nothing of it runs this start either way
                if p.presence == Presence::Disabled {
                    off.insert(folder.clone());
                }
                unplanned.push((folder, why));
            }
        }
    }
    for (folder, why) in unplanned.iter().filter(|(f, _)| !off.contains(f)) {
        warnings.push(Reason::new(
            "package-not-planned",
            format!("{folder}: {why}; nothing of it runs this start"),
            vec![folder.clone()],
            "warning",
        ));
    }
    // first decisions
    struct Pkg<'a> {
        entry: &'a PackageEntry,
        content: String,
        decision: &'static str,
        reasons: Vec<Reason>,
        dlls: Vec<Value>,
    }
    let severity = |e: &PackageEntry| {
        if e.save_sensitive.basis != "none" {
            "save-warning"
        } else {
            "warning"
        }
    };
    let mut pkgs: Vec<Pkg<'_>> = listed
        .into_iter()
        .map(|(entry, content)| {
            let mut reasons = Vec::new();
            let decision = match entry.presence {
                Presence::Disabled => "disabled",
                Presence::OtherSide => "other-side",
                Presence::Unknown => {
                    for b in &entry.blockers {
                        reasons.push(Reason::new(
                            &b.code,
                            b.detail.clone(),
                            b.caused_by.clone(),
                            severity(entry),
                        ));
                    }
                    "skip"
                }
                Presence::OnSide => {
                    if let Some(text) = &rules_error {
                        reasons.push(Reason::new(
                            "plan:compatibility-rules",
                            text.clone(),
                            vec![RULES_FILE.to_owned()],
                            severity(entry),
                        ));
                        "skip"
                    } else if !entry.blockers.is_empty() {
                        for b in &entry.blockers {
                            reasons.push(Reason::new(
                                &b.code,
                                b.detail.clone(),
                                b.caused_by.clone(),
                                severity(entry),
                            ));
                        }
                        "skip"
                    } else if !entry.planned {
                        reasons.push(Reason::new(
                            "not-planned",
                            "ShroudForge's planner did not place it in this side's load order",
                            Vec::new(),
                            severity(entry),
                        ));
                        "skip"
                    } else {
                        "candidate"
                    }
                }
            };
            let decision = if decision == "skip" && reasons.is_empty() {
                reasons.push(Reason::new(
                    "not-planned",
                    "it cannot run this start",
                    Vec::new(),
                    severity(entry),
                ));
                "skip"
            } else {
                decision
            };
            Pkg {
                entry,
                content,
                decision,
                reasons,
                dlls: Vec::new(),
            }
        })
        .collect();
    // a candidate's DLLs: one record per file, its SHA-256 pinned, the gate passed
    for p in pkgs.iter_mut().filter(|p| p.decision == "candidate") {
        let mut paths = BTreeSet::new();
        let mut why = None;
        for dll in &p.entry.dlls {
            let path = dll
                .path
                .replace('\\', "/")
                .split('/')
                .filter(|c| !c.is_empty())
                .collect::<Vec<_>>()
                .join("/");
            if !paths.insert(same_folder::name_key(&path)) {
                continue;
            }
            match (&dll.sha256, dll.eligible, plan_path_ok(&path)) {
                (Some(sha), true, true) => {
                    let mut proof = Frame::new("gate-proof");
                    for part in [
                        path.as_str(),
                        sha.as_str(),
                        dll.verdict,
                        &dll.reasons.join("\n"),
                    ] {
                        proof.text(part);
                    }
                    p.dlls.push(json!({"path": path, "sha256": sha, "mode": dll.mode, "source": dll.source, "required": true,
                        "gate": "pass", "gateProof_sha256": proof.finish(), "bridgeAbi": 0}));
                }
                _ => {
                    why = Some(format!(
                        "{}: its DLL cannot be planned ({})",
                        dll.path, dll.verdict
                    ));
                    break;
                }
            }
        }
        if let Some(why) = why {
            p.decision = "skip";
            p.dlls.clear();
            p.reasons.push(Reason::new(
                "native-gate",
                why,
                Vec::new(),
                severity(p.entry),
            ));
        }
    }
    // a package runs whole or not at all: a candidate whose required dependency (or an installed, switched-on optional
    // one) is not a candidate does not run either, transitively; the chain is nearest first
    // switched on, by id, over every folder the inventory lists (planned or not: an installed, switched-on optional
    // dependency the plan cannot describe is not run either, so its dependents follow it)
    let mut enabled: BTreeMap<String, bool> = BTreeMap::new();
    for p in &inv.packages {
        if let Some(id) = &p.id {
            *enabled.entry(id.clone()).or_default() |= p.enabled;
        }
    }
    loop {
        let candidates: BTreeMap<String, usize> = pkgs
            .iter()
            .enumerate()
            .filter(|(_, p)| p.decision == "candidate")
            .filter_map(|(i, p)| Some((p.entry.id.clone()?, i)))
            .collect();
        let chains: BTreeMap<String, Vec<String>> = pkgs
            .iter()
            .filter_map(|p| {
                Some((
                    p.entry.id.clone()?,
                    p.reasons
                        .iter()
                        .find(|r| r.code == "dependency-skipped")
                        .map(|r| r.caused_by.clone())
                        .unwrap_or_default(),
                ))
            })
            .collect();
        let mut demote = None;
        'outer: for (i, p) in pkgs
            .iter()
            .enumerate()
            .filter(|(_, p)| p.decision == "candidate")
        {
            for d in &p.entry.dependencies {
                if d.id == "shroudforge-api" || candidates.contains_key(&d.id) {
                    continue;
                }
                let installed_on = enabled.get(&d.id).copied().unwrap_or(false);
                if !d.optional || installed_on {
                    let mut chain = vec![d.id.clone()];
                    chain.extend(chains.get(&d.id).cloned().unwrap_or_default());
                    let what = if d.optional {
                        "an optional dependency that is installed and switched on"
                    } else {
                        "a dependency"
                    };
                    demote = Some((
                        i,
                        Reason::new(
                            "dependency-skipped",
                            format!("needs {} ({what}), which does not run this start", d.id),
                            chain,
                            severity(p.entry),
                        ),
                    ));
                    break 'outer;
                }
            }
        }
        match demote {
            Some((i, reason)) => {
                pkgs[i].decision = "skip";
                pkgs[i].dlls.clear();
                pkgs[i].reasons.push(reason);
            }
            None => break,
        }
    }
    let candidates: BTreeSet<String> = pkgs
        .iter()
        .filter(|p| p.decision == "candidate")
        .filter_map(|p| p.entry.id.clone())
        .collect();
    let load_order: Vec<String> = inv
        .load_order
        .iter()
        .filter(|id| candidates.contains(*id))
        .cloned()
        .collect();
    // the dialect, as the strict reader derives it from every enabled package of this side
    let (mut needs_sf, mut has_eml) = (false, false);
    let records: Vec<Value> = pkgs.iter().map(|p| {
        let e = p.entry;
        let candidate = p.decision == "candidate";
        let targets: Vec<&str> = if e.targets.is_empty() { vec![side] } else { e.targets.clone() };
        let mut caps: Vec<String> = Vec::new();
        for c in &e.capabilities {
            if !c.is_empty() && !caps.contains(c) {
                caps.push(c.clone());
            }
        }
        let kind = match e.kind {
            Kind::Shroudforge => "shroudforge",
            Kind::Eml => "eml",
            Kind::NativeSelection => "native-selection",
        };
        if e.enabled && targets.contains(&side) {
            needs_sf |= kind == "shroudforge" && caps.iter().any(|c| c == "patch")
                || kind == "eml" && caps.iter().any(|c| c == "runtime" || c == "runtime-register-dll");
            has_eml |= kind == "eml";
        }
        let id = e.id.clone().unwrap_or_default();
        let mut dep_ids = BTreeSet::new();
        let deps: Vec<Value> = e.dependencies.iter()
            .filter(|d| d.id != id && !d.id.is_empty() && dep_ids.insert(d.id.clone()))
            .map(|d| {
                let range = if d.range.is_empty() { "*".to_owned() } else { d.range.clone() };
                let included = candidate && (d.id == "shroudforge-api" || candidates.contains(&d.id));
                json!({"id": d.id, "range": range, "optional": d.optional, "included": included})
            })
            .collect();
        let part = |required: bool| {
            let admission = match (candidate, required) {
                (true, true) => "admit",
                (false, true) => "refuse",
                (_, false) => "not-needed",
            };
            json!({"required": required, "admission": admission})
        };
        let reasons: Vec<Value> = p.reasons.iter().map(Reason::value).collect();
        let mut config = Frame::new("package-config");
        config.text(&e.directory);
        for d in &e.dlls {
            config.text(&format!("{} {} {} {} {:?}", d.path, d.mode, d.source, d.status_id, d.aliases));
        }
        json!({
            "id": id,
            "version": e.version.clone().unwrap_or_default(),
            "kind": kind,
            "root": format!("mods/{}", e.directory),
            "content_sha256": p.content,
            "config_sha256": config.finish(),
            "enabled": e.enabled,
            "targets": targets,
            "capabilities": caps,
            "dependencies": deps,
            "requiredApis": [],
            "services": [],
            "requires180": e.requires180,
            "saveSensitive": e.save_sensitive.basis != "none",
            "decision": p.decision,
            "reasons": reasons,
            "data": part(e.parts.data),
            "lua": part(e.parts.runtime),
            "dlls": p.dlls,
        })
    }).collect();
    let dialect = if needs_sf {
        "shroudforge"
    } else if has_eml {
        "eml"
    } else {
        "none"
    };
    for w in &inv.warnings {
        warnings.push(Reason::new("inventory", w.clone(), Vec::new(), "info"));
    }
    Decided {
        packages: records,
        load_order,
        dialect,
        warnings,
        unplanned,
    }
}

/// A package folder name the plan can carry as `mods/<folder>` (the strict reader's path rule).
fn folder_ok(folder: &str) -> bool {
    !folder.contains('/') && plan_path_ok(folder)
}

/// A relative `/`-path the strict reader takes (no `\`, `:`, reserved or control characters, no empty, `.` or
/// `..` component, no trailing dot or space, no device name).
fn plan_path_ok(path: &str) -> bool {
    if path.is_empty()
        || path.len() > 32768
        || path.contains(['\\', ':', '<', '>', '"', '|', '?', '*'])
        || path.chars().any(char::is_control)
    {
        return false;
    }
    path.split('/').all(|c| {
        let base = c.split('.').next().unwrap_or("").to_ascii_uppercase();
        !(c.is_empty()
            || c == "."
            || c == ".."
            || c.ends_with([' ', '.'])
            || ["CON", "PRN", "AUX", "NUL"].contains(&base.as_str())
            || (base.len() == 4
                && (base.starts_with("COM") || base.starts_with("LPT"))
                && matches!(base.as_bytes()[3], b'1'..=b'9')))
    })
}

/// The plan's bytes for `draft` with plan id `id` (equal inputs, equal bytes).
pub fn render(draft: &Draft, id: &str) -> Vec<u8> {
    let f = &draft.facts;
    let fingerprints: serde_json::Map<String, Value> = draft
        .fingerprints
        .iter()
        .map(|(k, v)| ((*k).to_owned(), json!(v)))
        .collect();
    let mode = match draft.profile.mode {
        Mode::Active => "active",
        Mode::Once => "once",
    };
    let warnings: Vec<Value> = draft.warnings.iter().map(Reason::value).collect();
    let doc = json!({
        "format": 1,
        "minReader": 1,
        "plan_id": id,
        "launch_id": null,
        "startupDialect": draft.dialect,
        "writer": {"component": "kiln-manage", "version": draft.writer_version},
        "profile": {"id": draft.profile.id, "generation": draft.profile.generation, "mode": mode,
            "semantic_sha256": draft.profile.semantic_sha256},
        "game": {"side": side_name(draft.side), "exe_sha256": f.exe_sha256, "archiveBase_sha256": f.base_sha256,
            "archiveInput_sha256": f.input_sha256, "archiveBaseOrigin": f.origin},
        "backend": {"requested": "kiln", "selected": "kiln", "fallbackOrder": [], "dataWriter": "kiln-eml",
            "selectionReason": draft.selection_reason.replace('\0', "?")},
        "contract": {"apiVersion": API_VERSION, "sourceCommit": SF179_SOURCE_COMMIT,
            "upstream180Final": crate::sfi_release::UPSTREAM_180_FINAL,
            "inventory_sha256": draft.contract[0], "parity_sha256": draft.contract[1], "serviceContracts_sha256": draft.contract[2]},
        "fingerprints": fingerprints,
        "packages": draft.packages,
        "loadOrder": draft.load_order,
        "warnings": warnings,
    });
    let mut out = String::new();
    pretty(&doc, 0, &mut out);
    out.push('\n');
    out.into_bytes()
}

/// What every reader of a `kiln` plan requires, as kiln-manage checks it before writing: sfi-policy's strict reader
/// (kiln-sfi's) for this executable and side with this build's 1.8 record, and the core's own checks (Kiln's
/// interpreter requested and selected). The shared vectors in `schema/fixtures/sfi/launch-plan` go through this.
pub fn validate(bytes: &[u8], exe_sha256: &str, side: RunSide) -> Result<LaunchPlan, String> {
    if bytes.len() > PLAN_MAX {
        return Err("larger than 8 MiB".into());
    }
    let plan = LaunchPlan::parse(
        bytes,
        &ValidationContext {
            exe_sha256: exe_sha256.to_owned(),
            side: side_name(side).to_owned(),
            upstream_180_final: crate::sfi_release::UPSTREAM_180_FINAL,
        },
    )?;
    let backend = &plan.document()["backend"];
    if backend["requested"] != "kiln" || backend["selected"] != "kiln" {
        return Err("the plan does not select Kiln's interpreter".into());
    }
    Ok(plan)
}

/// [`validate`], and this plan is the one drafted: its id, its archive base's origin and the candidates.
fn self_check(bytes: &[u8], draft: &Draft, id: &str) -> Result<(), String> {
    let plan = validate(bytes, &draft.facts.exe_sha256, draft.side)?;
    if crate::hash::hex(&plan.plan_id()) != id.replace('-', "") {
        return Err("the plan id is not the one drawn".into());
    }
    if plan.archive_base_origin() != Some(draft.facts.origin) {
        return Err("the archive base origin is not the inspector's".into());
    }
    if plan.load_order()
        != draft
            .load_order
            .iter()
            .map(String::as_str)
            .collect::<Vec<_>>()
    {
        return Err("the load order is not the one decided".into());
    }
    Ok(())
}

/// Writes `bytes` as `<game>/kiln/launch-plans/<id>.json`, a new file only (never over another): an exclusively
/// created temporary file in the same folder, written and flushed, then given the plan's name only while no file has
/// it (Unix: a hard link, else a rename the system refuses over an existing name, else no plan; Windows: `MoveFileExW` without `MOVEFILE_REPLACE_EXISTING`,
/// written through), the folder flushed, and the file read back through the readers' checked open. Ok(None): the name
/// was taken. The plan folder is created when missing and must be a plain folder (no link or junction).
pub fn write_plan(game: &Path, id: &str, bytes: &[u8]) -> Result<Option<PlanRef>, String> {
    let kiln = game.join(crate::paths::game::KILN);
    if !digest::plain_folder(&kiln) {
        return Err("the game folder has no plain kiln folder".into());
    }
    let folder = kiln.join(PLAN_FOLDER);
    match std::fs::create_dir(&folder) {
        Ok(()) => crate::durable::flush_dir(&kiln).map_err(|e| e.message)?,
        Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {}
        Err(e) => return Err(format!("kiln/{PLAN_FOLDER}: {e}")),
    }
    if !digest::plain_folder(&folder) {
        return Err(format!("kiln/{PLAN_FOLDER} is not a plain folder"));
    }
    let name = format!("{id}.json");
    let target = folder.join(&name);
    let random = crate::util::random_bytes::<8>().map_err(|e| e.message)?;
    let tmp = folder.join(format!(".{name}.{}.kiln-tmp", crate::hash::hex(&random)));
    let written = (|| -> std::io::Result<()> {
        use std::io::Write as _;
        let mut f = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&tmp)?;
        f.write_all(bytes)?;
        f.sync_all()
    })();
    if let Err(e) = written {
        let _ = std::fs::remove_file(&tmp);
        return Err(format!("kiln/{PLAN_FOLDER}/{name}: {e}"));
    }
    match publish_new(&tmp, &target) {
        Ok(true) => {}
        Ok(false) => {
            let _ = std::fs::remove_file(&tmp);
            return Ok(None);
        }
        Err(e) => {
            let _ = std::fs::remove_file(&tmp);
            return Err(format!("kiln/{PLAN_FOLDER}/{name}: {e}"));
        }
    }
    crate::durable::flush_dir(&folder).map_err(|e| e.message)?;
    // read back as the core and kiln-sfi open it
    let mut back = Vec::new();
    let read = digest::open_plain_file(&target).and_then(|f| {
        f.take(PLAN_MAX as u64 + 1)
            .read_to_end(&mut back)
            .map_err(|e| e.to_string())
    });
    if let Err(e) = read.map(|_| ()) {
        return Err(format!(
            "kiln/{PLAN_FOLDER}/{name} cannot be read back ({e})"
        ));
    }
    if back != bytes {
        let _ = std::fs::remove_file(&target);
        return Err(format!(
            "kiln/{PLAN_FOLDER}/{name} does not read back as written"
        ));
    }
    Ok(Some(PlanRef {
        path: format!("{PLAN_FOLDER}/{name}"),
        sha256: crate::hash::sha256_hex(bytes),
    }))
}

/// Moves `from` to `to` only while there is no file at `to`; false when one is there (`from` stays).
#[cfg(not(windows))]
fn publish_new(from: &Path, to: &Path) -> std::io::Result<bool> {
    match std::fs::hard_link(from, to) {
        Ok(()) => std::fs::remove_file(from).map(|()| true),
        Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => Ok(false),
        // a file system without hard links (FAT, exFAT): a rename the system refuses over an existing name, or none
        Err(_) => rename_no_replace(from, to),
    }
}

/// Renames `from` to `to` in one step that fails when `to` exists (Linux `renameat2(RENAME_NOREPLACE)`, macOS
/// `renamex_np(RENAME_EXCL)`); false when it exists. A file system or system without such a rename gets an error: no
/// plan rather than a rename that could replace one.
#[cfg(any(target_os = "linux", target_vendor = "apple"))]
pub(crate) fn rename_no_replace(from: &Path, to: &Path) -> std::io::Result<bool> {
    use std::os::unix::ffi::OsStrExt as _;
    let c =
        |p: &Path| std::ffi::CString::new(p.as_os_str().as_bytes()).map_err(std::io::Error::other);
    let (a, b) = (c(from)?, c(to)?);
    // SAFETY: both are NUL-terminated paths that live until the call returns.
    #[cfg(target_os = "linux")]
    let rc = unsafe {
        libc::renameat2(
            libc::AT_FDCWD,
            a.as_ptr(),
            libc::AT_FDCWD,
            b.as_ptr(),
            libc::RENAME_NOREPLACE,
        )
    };
    // SAFETY: as above.
    #[cfg(target_vendor = "apple")]
    let rc = unsafe { libc::renamex_np(a.as_ptr(), b.as_ptr(), libc::RENAME_EXCL) };
    if rc == 0 {
        return Ok(true);
    }
    let e = std::io::Error::last_os_error();
    if e.kind() == std::io::ErrorKind::AlreadyExists {
        Ok(false)
    } else {
        Err(e)
    }
}
#[cfg(not(any(windows, target_os = "linux", target_vendor = "apple")))]
pub(crate) fn rename_no_replace(_: &Path, _: &Path) -> std::io::Result<bool> {
    Err(std::io::Error::other(
        "no hard links here and no rename that refuses to replace a file",
    ))
}

#[cfg(windows)]
fn publish_new(from: &Path, to: &Path) -> std::io::Result<bool> {
    use windows_sys::Win32::Storage::FileSystem::{MOVEFILE_WRITE_THROUGH, MoveFileExW};
    let wide = |p: &Path| crate::util::win32_path(p).map_err(|e| std::io::Error::other(e.message));
    let (src, dst) = (wide(from)?, wide(to)?);
    // SAFETY: both are NUL-terminated UTF-16 strings that live until the call returns.
    if unsafe { MoveFileExW(src.as_ptr(), dst.as_ptr(), MOVEFILE_WRITE_THROUGH) } == 0 {
        let e = std::io::Error::last_os_error();
        return if e.kind() == std::io::ErrorKind::AlreadyExists {
            Ok(false)
        } else {
            Err(e)
        };
    }
    Ok(true)
}

/// A game-relative file's bytes; None when there is none. Anything else that keeps it from being read is an error.
fn read_optional(game: &Path, rel: &str) -> Result<Option<Vec<u8>>, String> {
    match std::fs::read(game.join(rel)) {
        Ok(b) => Ok(Some(b)),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(format!("{rel} cannot be read ({e})")),
    }
}

/// Every `.dll` and `.asi` file in the game folder itself (proxies and loaders the game process may load), with its
/// size and SHA-256, by name. `shroudforge` is never looked into (it is a folder, and skipped by name first).
fn game_modules(game: &Path) -> Result<Vec<(String, u64, String)>, String> {
    let mut out = Vec::new();
    for entry in
        std::fs::read_dir(game).map_err(|e| format!("the game folder cannot be listed ({e})"))?
    {
        let entry = entry.map_err(|e| format!("the game folder cannot be listed ({e})"))?;
        let Some(name) = entry.file_name().to_str().map(str::to_owned) else {
            continue;
        };
        let lower = name.to_ascii_lowercase();
        if lower == "shroudforge" || !(lower.ends_with(".dll") || lower.ends_with(".asi")) {
            continue;
        }
        let path: PathBuf = entry.path();
        let meta =
            std::fs::symlink_metadata(&path).map_err(|e| format!("{name} cannot be read ({e})"))?;
        if !meta.is_file() {
            out.push((name, 0, "not-a-plain-file".to_owned()));
            continue;
        }
        let (sha, size) = crate::hash::sha256_file(&path)
            .map_err(|e| format!("{name} cannot be read ({})", e.message))?;
        out.push((name, size, sha));
    }
    out.sort();
    Ok(out)
}

/// A domain-separated SHA-256 over length-framed fields: `"kiln.sfi.plan.<name>.v1\0"`, then each field as its
/// length (u64 little-endian) and bytes; an absent optional field is the length `u64::MAX` alone.
pub struct Frame(Sha256);
impl Frame {
    pub fn new(name: &str) -> Self {
        let mut h = Sha256::new();
        h.update(format!("kiln.sfi.plan.{name}.v1\0").as_bytes());
        Self(h)
    }
    pub fn bytes(&mut self, b: &[u8]) {
        self.0.update((b.len() as u64).to_le_bytes());
        self.0.update(b);
    }
    pub fn text(&mut self, s: &str) {
        self.bytes(s.as_bytes());
    }
    pub fn optional(&mut self, b: Option<&[u8]>) {
        match b {
            Some(b) => self.bytes(b),
            None => self.0.update(u64::MAX.to_le_bytes()),
        }
    }
    pub fn finish(self) -> String {
        crate::hash::hex(&self.0.finalize())
    }
}

/// Canonical compact JSON: object keys in byte order, whatever map the value was built with.
pub fn canonical(v: &Value) -> String {
    match v {
        Value::Object(o) => {
            let mut keys: Vec<&String> = o.keys().collect();
            keys.sort();
            let items: Vec<String> = keys
                .iter()
                .map(|k| {
                    format!(
                        "{}:{}",
                        Value::String((*k).clone()),
                        canonical(&o[k.as_str()])
                    )
                })
                .collect();
            format!("{{{}}}", items.join(","))
        }
        Value::Array(a) => format!(
            "[{}]",
            a.iter().map(canonical).collect::<Vec<_>>().join(",")
        ),
        other => other.to_string(),
    }
}

/// Two-space indented JSON with object keys in byte order (the plan's layout).
fn pretty(v: &Value, depth: usize, out: &mut String) {
    let pad = |n: usize| "  ".repeat(n);
    match v {
        Value::Object(o) if !o.is_empty() => {
            let mut keys: Vec<&String> = o.keys().collect();
            keys.sort();
            out.push_str("{\n");
            for (i, k) in keys.iter().enumerate() {
                out.push_str(&pad(depth + 1));
                out.push_str(&Value::String((*k).clone()).to_string());
                out.push_str(": ");
                pretty(&o[k.as_str()], depth + 1, out);
                out.push_str(if i + 1 < keys.len() { ",\n" } else { "\n" });
            }
            out.push_str(&pad(depth));
            out.push('}');
        }
        Value::Array(a) if !a.is_empty() => {
            out.push_str("[\n");
            for (i, item) in a.iter().enumerate() {
                out.push_str(&pad(depth + 1));
                pretty(item, depth + 1, out);
                out.push_str(if i + 1 < a.len() { ",\n" } else { "\n" });
            }
            out.push_str(&pad(depth));
            out.push(']');
        }
        Value::Object(_) => out.push_str("{}"),
        Value::Array(_) => out.push_str("[]"),
        other => out.push_str(&other.to_string()),
    }
}

#[cfg(all(test, any(target_os = "linux", target_vendor = "apple")))]
mod tests {
    /// The publish step for file systems without hard links never replaces a file (the hard-link path is the one the
    /// tests' file systems take; this is the fallback itself).
    #[test]
    fn the_rename_without_hard_links_never_replaces() {
        let dir = std::env::temp_dir().join(format!("kiln-sfi-plan-rename-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let (tmp, taken, free) = (
            dir.join("tmp"),
            dir.join("taken.json"),
            dir.join("free.json"),
        );
        std::fs::write(&tmp, "new").unwrap();
        std::fs::write(&taken, "old").unwrap();
        assert!(!super::rename_no_replace(&tmp, &taken).unwrap());
        assert_eq!(std::fs::read_to_string(&taken).unwrap(), "old");
        assert_eq!(std::fs::read_to_string(&tmp).unwrap(), "new");
        assert!(super::rename_no_replace(&tmp, &free).unwrap());
        assert_eq!(std::fs::read_to_string(&free).unwrap(), "new");
        assert!(!tmp.exists());
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
