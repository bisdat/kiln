// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln-eml in kiln mode: Kiln's interpreter has the start (kiln.launch backend `kiln`).
//!
//! The core's frozen kiln.launch record (native/core/abi/services/kiln_launch.h) is the in-process authority on which
//! ShroudForge backend has this start; `plugin.rs` reads it ([`Launch`]). Only backend `kiln`, without
//! `KILN_SHROUDFORGE=0`, brings kiln-eml here ([`select`]):
//! - The launch plan the record froze says which packages Kiln admits and in which startup dialect. kiln-eml reads it
//!   again from the profile's `sfLaunchPlan` and uses it only when its SHA-256 is the record's ([`read_plan`]), through
//!   the interpreter's own reader (sfi-policy's `LaunchPlan`, with the core's executable digest and side): kiln-eml
//!   never decides from a plan Kiln's interpreter would not read.
//! - A package of the startup dialect's pass ([`crate::startup::PlanFacts::pass_members`]) is the startup pass's
//!   (`startup_seam`, the startup controller): kiln-eml never runs it as an EML mod on its own; its row comes from the
//!   startup outcome ([`Decision::Pass`]). One the plan admits outside the pass is the interpreter's alone: kiln-eml
//!   has nothing to apply for it.
//! - A package the plan lists but does not admit (`skip`, `disabled`, `other-side`) never runs here, whatever it needs:
//!   the plan's decision stands, so no EML route brings back a package the plan left out (a skipped dependent of a
//!   skipped package, for one). The reason names the plan and its decision.
//! - A package that needs ShroudForge's runtime and that the plan does not admit is skipped; the reason names the plan.
//! - An EML package the plan does not list (an add_mod handover, a drop-in: an unrelated EML contributor) follows
//!   [`unlisted_rule`] (owner question Q1): skipped with a reason in the ShroudForge and EML dialects (one pass per
//!   start, the plan's), the EML pass's own selection when the plan has no startup pass.
//! - A plan that cannot be read again (changed or gone since the freeze, or not a plan kiln-sfi would read: see
//!   [`parse_plan`]) runs nothing: kiln-eml cannot tell which package belongs to which pass.
//! - So kiln mode never runs a package the rules from before would skip: it runs the same packages or fewer.
//!
//! `kiln/state/shroudforge.json` is never read here: kiln-shroudforge stands aside under `kiln`, and its mode `off` says
//! nothing about Kiln's interpreter. Every other record (none, hosted, installed, the idle class, no table, a table
//! that cannot be read) and `KILN_SHROUDFORGE=0` keep the rules from before kiln.launch (runner::runtime_skip_reason),
//! byte for byte.

use std::io::Read;

use camino::{Utf8Path, Utf8PathBuf};
use sha2::{Digest as _, Sha256};

use sfi_policy::launch_plan::{self, LaunchPlan, ValidationContext};

use crate::discover::Entry;
use crate::startup::{PackageKind, PlanDependency, PlanFacts, PlanPackage};

/// kiln.launch's frozen record as kiln-eml reads it (plugin.rs `read_launch`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Launch {
    /// No kiln.launch (an older core), or a table or record that cannot be read.
    Absent,
    /// Backend none, a reason of the idle class: the core did not decide.
    Idle,
    /// Backend none, a reason of the refused class: every ShroudForge backend stands aside.
    Refused,
    /// Hosted or installed: kiln-shroudforge has this start.
    Shroudforge,
    /// Kiln's interpreter has this start (kiln.launch's launch id), with the launch plan of this SHA-256.
    Kiln { plan_hash: [u8; 32], launch_id: u64 },
}

/// The largest launch plan the core reads (kiln_launch.h).
const PLAN_MAX: u64 = 8 * 1024 * 1024;
/// Where the profile's plan reference points, below `<game>/kiln`.
const PLAN_FOLDER: &str = "launch-plans";

/// The startup dialect a launch plan freezes (`startupDialect`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Dialect {
    /// Kiln's ShroudForge startup pass runs the admitted packages' startup data.
    Shroudforge,
    /// kiln-eml's EML pass runs it, as before.
    Eml,
    /// No startup pass this start.
    NoPass,
}

/// One package of the plan, the members kiln-eml uses.
#[derive(Clone, Debug)]
struct Package {
    id: String,
    /// game-relative `mods/<folder>`
    root: String,
    /// `decision: "candidate"`: Kiln admits it this start
    admitted: bool,
    decision: String,
    /// A participant of the startup dialect's pass (`PlanFacts::participants`): a candidate the pass registers, with
    /// startup data. Its row is the startup outcome's.
    pass: bool,
    reasons: Vec<String>,
}

/// The frozen launch plan, as far as kiln-eml needs it: the interpreter's reader's view of it and the members the
/// decisions here use.
#[derive(Clone, Debug)]
pub struct Plan {
    /// `launch-plans/<plan id>.json`
    name: String,
    dialect: Dialect,
    packages: Vec<Package>,
    /// The plan as sfi-policy's reader accepted it.
    read: std::sync::Arc<LaunchPlan>,
    /// SHA-256 of the bytes read (kiln.launch's plan hash).
    sha256: [u8; 32],
}

impl Plan {
    /// `launch-plans/<plan id>.json`
    pub fn name(&self) -> &str {
        &self.name
    }
    /// The plan as the interpreter's reader accepted it.
    pub fn launch_plan(&self) -> &std::sync::Arc<LaunchPlan> {
        &self.read
    }
    /// SHA-256 of the bytes read (kiln.launch's plan hash).
    pub fn sha256(&self) -> [u8; 32] {
        self.sha256
    }
    pub fn dialect(&self) -> Dialect {
        self.dialect
    }
    /// The startup controller's view of the plan (`PlanFacts`).
    pub fn facts(&self) -> PlanFacts {
        plan_facts(&self.read)
    }
}

/// kiln-eml's route when Kiln's interpreter has this start.
#[derive(Clone, Debug)]
pub struct KilnMode {
    game: Utf8PathBuf,
    /// Err: why the frozen plan cannot be read again
    plan: Result<Plan, String>,
}

/// What kiln-eml does with one package in kiln mode.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Decision {
    /// The plan does not list it (an unrelated EML contributor): [`unlisted_rule`] decides.
    Run,
    /// A participant of the startup dialect's pass: the startup pass applies its data (its row is the startup outcome's).
    Pass,
    /// Skipped whole (nothing of it runs here), with the reason.
    Skip(String),
    /// The plan gives it to Kiln's interpreter and it has no startup data: kiln-eml has nothing to run.
    Interpreter(String),
    /// The plan lists it switched off or for the other side (a normal status, not a failure): nothing of it runs here.
    Off(String),
}

/// Kiln's interpreter has this start for kiln-eml: the record froze backend `kiln` and `KILN_SHROUDFORGE=0` (`env`, only
/// the exact value 0) does not stand it aside. kiln-eml is then the launch plan's data writer, which nobody reserves.
pub fn active(launch: &Launch, env: Option<&str>) -> bool {
    matches!(launch, Launch::Kiln { .. }) && env != Some("0")
}

/// kiln-eml's route this start: Some only when [`active`]. `plan` reads the frozen plan; it is called for `kiln` alone.
pub fn select(launch: &Launch, env: Option<&str>, game: &Utf8Path, plan: impl FnOnce(&[u8; 32]) -> Result<Plan, String>) -> Option<KilnMode> {
    match launch {
        Launch::Kiln { plan_hash, .. } if active(launch, env) => Some(KilnMode { game: game.to_owned(), plan: plan(plan_hash) }),
        _ => None,
    }
}

/// The launch plan the profile names (`sfLaunchPlan.path`, `launch-plans/<plan id>.json` below `kiln`), read again and
/// used only when its SHA-256 is `frozen`, the record's: the file the core checked and froze, not a later one. It is
/// read as kiln-sfi reads it (plan_source.rs `read_checked`): sfi-policy's reader with the core's executable digest and
/// side (`context`, from `build_info`), no two packages in one folder of `game` under two names (sfi-policy's
/// `check_folders`: an 8.3 name, a link, a junction), the plan id its file name carries, and backend `kiln` selected.
pub fn read_plan(profile_json: Option<&str>, kiln: &Utf8Path, game: &Utf8Path, frozen: &[u8; 32], context: &ValidationContext)
    -> Result<Plan, String> {
    let profile = profile_json.ok_or("this start has no profile")?;
    let doc: serde_json::Value = serde_json::from_str(profile.trim_start_matches('\u{feff}'))
        .map_err(|_| "the profile does not parse".to_owned())?;
    let name = doc.pointer("/sfLaunchPlan/path").and_then(|p| p.as_str()).ok_or("the profile names no launch plan")?;
    let id = name.strip_prefix(PLAN_FOLDER).and_then(|r| r.strip_prefix('/')).and_then(|r| r.strip_suffix(".json"))
        .filter(|id| canonical_uuid(id)).ok_or("the profile's launch plan is not launch-plans/<plan id>.json")?;
    // as the core reads it (launch_boot.cpp read_plan): the folder a plain one, the file opened without following a
    // link and checked through that handle, so nothing can be swapped in between the check and the read
    let folder = kiln.join(PLAN_FOLDER);
    let plain_folder = std::fs::symlink_metadata(&folder).is_ok_and(|m| m.is_dir() && !reparse(&m));
    if !plain_folder { return Err(format!("kiln/{PLAN_FOLDER} is not a plain folder")); }
    let path = folder.join(format!("{id}.json"));
    let meta = std::fs::symlink_metadata(&path).map_err(|e| format!("{name}: {e}"))?;
    if !meta.is_file() || reparse(&meta) { return Err(format!("{name} is not a plain file")); }
    let file = open_no_follow(&path).map_err(|e| format!("{name}: {e}"))?;
    let held = file.metadata().map_err(|e| format!("{name}: {e}"))?;
    if !held.is_file() || reparse(&held) || !same_file(&meta, &held) { return Err(format!("{name} is not a plain file")); }
    let mut bytes = Vec::new();
    file.take(PLAN_MAX + 1).read_to_end(&mut bytes).map_err(|e| format!("{name}: {e}"))?;
    if bytes.len() as u64 > PLAN_MAX { return Err(format!("{name} is larger than 8 MiB")); }
    // the bytes hashed are the bytes parsed: no second read
    if Sha256::digest(&bytes).as_slice() != frozen.as_slice() {
        return Err(format!("{name} changed since this start froze it"));
    }
    let plan = parse_plan(name, &bytes, context)?;
    // two packages in one folder under two names: the strict reader's overlap, by identity (as plan_source.rs)
    plan.read.check_folders(game.as_std_path())
        .map_err(|why| format!("{name} is not a launch plan Kiln's interpreter reads ({why})"))?;
    let hex: String = plan.read.plan_id().iter().map(|b| format!("{b:02x}")).collect();
    if hex != id.replace('-', "") {
        return Err(format!("{name} has a plan id that is not the one its file name carries"));
    }
    if plan.read.permission(true) != launch_plan::Permission::EligiblePendingAdmission {
        return Err(format!("{name} does not select Kiln's interpreter"));
    }
    Ok(plan)
}

/// A reparse point of any kind (a link, a junction, a cloud placeholder): the core refuses them all.
#[cfg(windows)]
fn reparse(m: &std::fs::Metadata) -> bool {
    use std::os::windows::fs::MetadataExt as _;
    m.file_type().is_symlink() || m.file_attributes() & 0x400 != 0 // FILE_ATTRIBUTE_REPARSE_POINT
}
#[cfg(not(windows))]
fn reparse(m: &std::fs::Metadata) -> bool {
    m.file_type().is_symlink()
}

/// Opens `path` for reading without following a link at its last component (Windows: FILE_FLAG_OPEN_REPARSE_POINT, as
/// the core opens the plan; the handle's attributes then show a reparse point).
#[cfg(windows)]
fn open_no_follow(path: &Utf8Path) -> std::io::Result<std::fs::File> {
    use std::os::windows::fs::OpenOptionsExt as _;
    std::fs::OpenOptions::new().read(true).custom_flags(0x0020_0000).open(path) // FILE_FLAG_OPEN_REPARSE_POINT
}
/// Elsewhere (the host tests) the handle is compared with the checked path's identity instead ([`same_file`]).
#[cfg(not(windows))]
fn open_no_follow(path: &Utf8Path) -> std::io::Result<std::fs::File> {
    std::fs::File::open(path)
}

/// The opened file is the one checked (no link swapped in after the check). Windows opens without following one, and
/// the handle is checked on its own; elsewhere the device and inode must be the checked path's.
#[cfg(unix)]
fn same_file(checked: &std::fs::Metadata, held: &std::fs::Metadata) -> bool {
    use std::os::unix::fs::MetadataExt as _;
    (checked.dev(), checked.ino()) == (held.dev(), held.ino())
}
#[cfg(not(unix))]
fn same_file(_: &std::fs::Metadata, _: &std::fs::Metadata) -> bool {
    true
}

/// `8-4-4-4-12` lower-case hex digits, as the core names plan files.
fn canonical_uuid(s: &str) -> bool {
    s.len() == 36 && s.bytes().enumerate().all(|(i, b)| if matches!(i, 8 | 13 | 18 | 23) { b == b'-' } else {
        b.is_ascii_digit() || (b'a'..=b'f').contains(&b)
    })
}

/// The members kiln-eml uses of a plan whose bytes are the frozen ones, read by the interpreter's own reader
/// (sfi-policy's `LaunchPlan::parse`, the plugin wiring): kiln-eml never decides from a plan Kiln's interpreter would not read, and
/// the shared accept and refuse vectors (schema/fixtures/sfi/launch-plan) hold for both.
pub fn parse_plan(name: &str, bytes: &[u8], context: &ValidationContext) -> Result<Plan, String> {
    let read = LaunchPlan::parse(bytes, context).map_err(|why| format!("{name} is not a launch plan Kiln's interpreter reads ({why})"))?;
    let dialect = match read.startup_dialect() {
        launch_plan::Dialect::Shroudforge => Dialect::Shroudforge,
        launch_plan::Dialect::Eml => Dialect::Eml,
        launch_plan::Dialect::None => Dialect::NoPass,
    };
    let facts = plan_facts(&read);
    let members: std::collections::HashSet<String> = facts.participants().into_iter().map(|p| p.id.clone()).collect();
    let packages = read.packages().map(|p| Package {
        id: p.id.to_owned(),
        root: p.root.to_owned(),
        admitted: p.decision == launch_plan::Decision::Candidate,
        decision: match p.decision {
            launch_plan::Decision::Candidate => "candidate",
            launch_plan::Decision::Skip => "skip",
            launch_plan::Decision::Disabled => "disabled",
            launch_plan::Decision::OtherSide => "other-side",
        }.to_owned(),
        pass: members.contains(p.id),
        reasons: p.reasons.iter().map(|r| r.message.to_owned()).collect(),
    }).collect();
    Ok(Plan { name: name.to_owned(), dialect, packages, read: std::sync::Arc::new(read), sha256: Sha256::digest(bytes).into() })
}

/// The startup controller's view of an accepted plan (`PlanFacts`): the startup controller takes no sfi-policy type, so this is the one
/// adapter from the frozen launch plan. `content_sha256` is sfi-policy's `package_content_sha256` (the startup controller's
/// digest port is the same function).
pub fn plan_facts(plan: &LaunchPlan) -> PlanFacts {
    let document = plan.document();
    let game = &document["game"];
    let text = |v: &serde_json::Value| v.as_str().unwrap_or_default().to_owned();
    let configs = document["packages"].as_array().map(Vec::as_slice).unwrap_or_default();
    let packages = plan.packages().zip(configs).map(|(p, raw)| PlanPackage {
        id: p.id.to_owned(),
        version: p.version.to_owned(),
        kind: match p.kind {
            launch_plan::Kind::Shroudforge => PackageKind::Shroudforge,
            launch_plan::Kind::Eml => PackageKind::Eml,
            launch_plan::Kind::NativeSelection => PackageKind::NativeSelection,
        },
        root: p.root.to_owned(),
        content_sha256: p.content_sha256,
        config_sha256: crate::startup::unhex(raw["config_sha256"].as_str().unwrap_or_default()).unwrap_or([0; 32]),
        candidate: p.decision == launch_plan::Decision::Candidate,
        data_required: p.data.required,
        native: p.has_native(),
        dependencies: p.dependencies.iter().map(|d| PlanDependency { id: d.id.to_owned(), optional: d.optional, included: d.included }).collect(),
    }).collect();
    PlanFacts {
        plan_id: plan.plan_id(),
        dialect: match plan.startup_dialect() {
            launch_plan::Dialect::Shroudforge => crate::sf_pass::StartupDialect::Shroudforge,
            launch_plan::Dialect::Eml => crate::sf_pass::StartupDialect::Eml,
            launch_plan::Dialect::None => crate::sf_pass::StartupDialect::None,
        },
        exe_sha256: text(&game["exe_sha256"]),
        archive_base_sha256: text(&game["archiveBase_sha256"]),
        archive_input_sha256: text(&game["archiveInput_sha256"]),
        archive_base_origin: plan.archive_base_origin().map(str::to_owned),
        packages,
        load_order: plan.load_order().into_iter().map(str::to_owned).collect(),
    }
}

impl KilnMode {
    /// For a test, or a caller that has the plan already.
    pub fn new(game: &Utf8Path, plan: Result<Plan, String>) -> Self {
        Self { game: game.to_owned(), plan }
    }

    /// The frozen plan was read again (else no EML mod runs this start).
    pub fn plan_read(&self) -> bool {
        self.plan.is_ok()
    }

    /// The kiln.log line that says which route kiln-eml follows.
    pub fn summary(&self) -> String {
        match &self.plan {
            Ok(plan) => format!("kiln-eml: Kiln's interpreter has this start (kiln.launch); kiln-eml follows its \
                launch plan {} (startup dialect {})", plan.name, dialect_name(plan.dialect)),
            Err(why) => format!("kiln-eml: Kiln's interpreter has this start (kiln.launch), but its launch plan \
                cannot be read again ({why}): no EML mod runs this start"),
        }
    }

    /// The package at `path` with manifest id `id` (None: not an EML package kiln-eml can read). `runtime`: it needs
    /// ShroudForge's runtime (capability `runtime` or `runtime-register-dll`, or the launcher said so).
    pub fn decide(&self, path: &Utf8Path, id: Option<&str>, runtime: bool) -> Decision {
        let plan = match &self.plan {
            Ok(plan) => plan,
            Err(why) => return Decision::Skip(unreadable(why)),
        };
        if let Some(held) = self.admitted(plan, path, id) {
            return held;
        }
        if runtime {
            return Decision::Skip(self.runtime_reason(plan, path, id));
        }
        match self.entry(plan, path, id, false) {
            // listed and not admitted: the plan's decision stands, whatever the package needs (LAUNCH-PLAN §1.1: no
            // EML route brings back a package the plan left out; §3.1: a skipped package has no admitted part)
            Some(p) if !p.admitted => left_out(plan, p),
            // admitted for kiln-eml's own EML pass, or not listed: an unrelated EML contributor, as before
            _ => Decision::Run,
        }
    }

    /// The plan's entry for the package at `path` with manifest id `id`: the one at its root, else (a copy elsewhere)
    /// the one of its id. `admitted`: only the packages the plan admits.
    fn entry<'p>(&self, plan: &'p Plan, path: &Utf8Path, id: Option<&str>, admitted: bool) -> Option<&'p Package> {
        let listed = || plan.packages.iter().filter(move |p| p.admitted || !admitted);
        listed().find(|p| crate::discover::relative_under(path, &self.game.join(&p.root)).is_some_and(|r| r.as_str().is_empty()))
            .or_else(|| listed().find(|p| id.is_some_and(|id| p.id == id)))
    }

    /// What becomes of a package the plan admits: a participant of the startup dialect's pass (startup data) is the
    /// startup pass's ([`Decision::Pass`]); any other is the interpreter's alone. None: the plan does not admit it.
    fn admitted(&self, plan: &Plan, path: &Utf8Path, id: Option<&str>) -> Option<Decision> {
        let p = self.entry(plan, path, id, true)?;
        Some(if p.pass {
            Decision::Pass
        } else {
            Decision::Interpreter(format!("this start's launch plan ({}) gives this package to Kiln's \
                interpreter, with no startup data to apply this start, so kiln-eml has nothing to run for it", plan.name))
        })
    }

    /// The startup dialect of the plan read again (None: it could not be read).
    pub fn dialect(&self) -> Option<Dialect> {
        self.plan.as_ref().ok().map(|p| p.dialect)
    }

    /// The plan read again, or why it could not be.
    pub fn plan(&self) -> Result<&Plan, &str> {
        self.plan.as_ref().map_err(String::as_str)
    }

    /// Why a package that needs ShroudForge's runtime does not get it from Kiln's interpreter: the plan does not admit it
    /// (or admits it for the EML pass, without the runtime).
    fn runtime_reason(&self, plan: &Plan, path: &Utf8Path, id: Option<&str>) -> String {
        let detail = match (self.entry(plan, path, id, true), self.entry(plan, path, id, false)) {
            (Some(_), _) => format!("it admits it for the {} startup pass, without ShroudForge's runtime (the package changed \
                since the launcher wrote the plan; activate the profile in the Kiln launcher again)", dialect_name(plan.dialect)),
            (None, Some(p)) if p.reasons.is_empty() => format!("it does not admit it ({})", p.decision),
            (None, Some(p)) => format!("it does not admit it ({}: {})", p.decision, cut(&p.reasons.join("; "), 512)),
            (None, None) => "it does not list it: the package was added after the launcher wrote the plan; activate the \
                profile in the Kiln launcher again".to_owned(),
        };
        format!("needs ShroudForge's runtime, and Kiln's interpreter runs only what this start's launch plan ({}) \
            admits: {detail}; its data was not applied, so the game is as without the mod", plan.name)
    }

    /// Kiln mode's part of a run: of `mods` (the paths the run would take), none runs as an EML mod on its own. A
    /// member of the startup dialect's pass ([`Decision::Pass`]) keeps its `pending` entry for the startup outcome to
    /// settle; an unlisted EML contributor follows [`unlisted_rule`] (returned when the EML pass takes it, else
    /// skipped with the rule's reason); every other `pending` entry says why it does not run. An entry discovery already
    /// set aside (not an EML package kiln-eml can run: a ShroudForge package) that the plan lists gets the plan's reason
    /// instead. Returns the unlisted contributors the EML pass of a start without a startup dialect runs.
    pub fn apply(&self, mods: Vec<Utf8PathBuf>, entries: &mut [Entry]) -> Vec<Utf8PathBuf> {
        let mut unlisted = Vec::new();
        for path in mods {
            let manifest = crate::discover::classify(&path).ok();
            let runtime = manifest.as_ref().is_some_and(|m| m.capabilities.iter()
                .any(|c| matches!(c, mod_loader::Capability::Runtime | mod_loader::Capability::RuntimeRegisterDll)));
            let reason = match self.decide(&path, manifest.as_ref().map(|m| m.id.as_str()), runtime) {
                Decision::Pass => continue,
                Decision::Run => match self.plan.as_ref().ok().and_then(unlisted_rule) {
                    None => {
                        unlisted.push(path);
                        continue;
                    }
                    Some(reason) => {
                        tracing::warn!(%path, %reason, "EML package skipped whole");
                        reason
                    }
                },
                Decision::Skip(reason) => {
                    tracing::warn!(%path, %reason, "EML package skipped whole");
                    reason
                }
                Decision::Interpreter(reason) => {
                    tracing::info!(%path, %reason, "EML package left to Kiln's interpreter");
                    reason
                }
                Decision::Off(reason) => {
                    tracing::info!(%path, %reason, "EML package off in the launch plan");
                    reason
                }
            };
            for e in entries.iter_mut().filter(|e| e.path == path.as_str() && e.state == "pending") {
                e.state = "skipped".into();
                e.reason = Some(reason.clone());
            }
        }
        if self.plan.is_err() { return unlisted; }
        for e in entries.iter_mut().filter(|e| e.state == "skipped" && e.id.is_none()) {
            match self.decide(Utf8Path::new(&e.path), None, false) {
                Decision::Skip(reason) | Decision::Interpreter(reason) | Decision::Off(reason) => e.reason = Some(reason),
                // a participant of the startup pass: the startup outcome settles its entry
                Decision::Pass => {
                    e.state = "pending".into();
                    e.reason = None;
                }
                Decision::Run => {}
            }
        }
        unlisted
    }

    /// The plan's package at game-relative `root` or with id `id` (any decision), when it lists one.
    pub fn listed(&self, path: &Utf8Path, id: Option<&str>) -> bool {
        self.plan.as_ref().is_ok_and(|plan| self.entry(plan, path, id, false).is_some())
    }

    /// The reason for a profile entry the launcher handed to ShroudForge's runtime (`emlRuntime`; discover's
    /// apply_runtime_blocks): it needs the runtime, whatever its manifest says now.
    pub fn delegated(&self, e: &Entry) -> String {
        let (path, id) = (Utf8Path::new(&e.path), e.id.as_deref());
        let plan = match &self.plan {
            Ok(plan) => plan,
            Err(why) => return unreadable(why),
        };
        match self.admitted(plan, path, id) {
            Some(Decision::Skip(reason) | Decision::Interpreter(reason)) => reason,
            Some(Decision::Pass) => format!("this start's launch plan ({}) admits this package for its startup pass: \
                Kiln's interpreter runs it, kiln-eml applies its startup data in that pass", plan.name),
            _ => self.runtime_reason(plan, path, id),
        }
    }
}

/// The chosen rule, in this one
/// place: an EML package the plan does not list (an add_mod handover, a drop-in) in a start of Kiln's interpreter. A
/// start runs one startup pass, the plan's: in the ShroudForge dialect ShroudForge's pass (hosted ShroudForge does not run
/// such a package either) and in the EML dialect the plan's EML packages, so it is skipped with this reason (Some). A
/// plan without a startup dialect has no pass of its own: the EML pass runs the unlisted contributors as before (None).
pub fn unlisted_rule(plan: &Plan) -> Option<String> {
    match plan.dialect {
        Dialect::NoPass => None,
        dialect => Some(format!("this start's launch plan ({}) does not list this EML mod, and Kiln's \
            interpreter has this start: it runs one startup pass ({}), the plan's, so kiln-eml runs no EML mod the plan \
            leaves out; its data was not applied, so the game is as without the mod. Activate the profile in the Kiln \
            launcher again to plan it", plan.name, dialect_name(dialect))),
    }
}

/// A package the plan lists without admitting it. `skip` is a refusal (a warning); `disabled` and `other-side` are normal
/// statuses.
fn left_out(plan: &Plan, p: &Package) -> Decision {
    let detail = if p.reasons.is_empty() { p.decision.clone() } else { format!("{}: {}", p.decision, cut(&p.reasons.join("; "), 512)) };
    let text = |what: &str| format!("this start's launch plan ({}) {what} ({detail}), and Kiln's interpreter has \
        this start: kiln-eml runs no package the plan leaves out; its data was not applied, so the game is as without the \
        mod", plan.name);
    match p.decision.as_str() {
        "disabled" => Decision::Off(text("has this package switched off")),
        "other-side" => Decision::Off(text("has this package for the other side only")),
        _ => Decision::Skip(text("does not admit this package")),
    }
}

fn unreadable(why: &str) -> String {
    format!("Kiln could not read this start's launch plan again ({why}), so kiln-eml cannot tell how Kiln's \
        interpreter admits this package: it is skipped, its data was not applied, so the game is as without the mod. Start \
        the game again from the Kiln launcher")
}

fn dialect_name(d: Dialect) -> &'static str {
    match d {
        Dialect::Shroudforge => "ShroudForge",
        Dialect::Eml => "EML",
        Dialect::NoPass => "none",
    }
}

/// `text` cut to at most `max` bytes at a character boundary.
fn cut(text: &str, max: usize) -> &str {
    if text.len() <= max { return text; }
    let mut n = max;
    while !text.is_char_boundary(n) { n -= 1; }
    &text[..n]
}

#[cfg(test)]
#[path = "kiln_mode_tests.rs"]
pub(crate) mod tests;
