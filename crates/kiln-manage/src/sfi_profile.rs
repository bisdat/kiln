// SPDX-License-Identifier: MIT
//! Kiln mode in [`crate::profile`]: the activation, the refresh and the play-once start of a game whose
//! kiln.json selects Kiln's interpreter, through the activation journal ([`crate::sfi_activation`]), with
//! the launch plan ([`crate::sfi_plan`]) their profile file pins.
//!
//! A child module of `profile`
//! (`profile::sfi`): it runs profile's own steps (prepare, render, the activation record) and changes none of main's.
//!
//! **One decision per operation** ([`Decision`]): kiln.json is read once, with the install's side and this process's
//! environment (`sfi_backend::resolve_game`); the route of the ShroudForge-format mods comes from that answer
//! (`sfi_routing::route_from`), and the plan producer is given the same answer (`sfi_plan::produce_resolved`). Render,
//! placement, the plan and the profile all follow it: a route on which the interpreter does not run this start
//! (`KILN_SHROUDFORGE=0`, no kiln-sfi.dll when it was decided) gets no plan, whatever the folder holds by then. One read
//! stays the producer's own: its draft reads kiln.json again for the native policy and the settings fingerprint.
//! A change between the two reads is a change made right after the operation: the core decides again at boot, and the
//! next refresh finds the plan out of date. Only the interpreter's route is journaled
//! ([`Decision::journaled`]); main's routes stay main's code with two additions around it: an unfinished kiln-mode
//! activation is settled before anything changes a copy ([`settle`]), and afterwards a refused journal is superseded and
//! the plans no profile file pins any more go ([`after`]).
//!
//! **Activation** ([`activate`]): prepare (copies, configs) on the decided route, then the journal: the game-folder
//! files, chain.ini (staged), the copies with the kept settings put back, the final render, the plan (produced with the
//! journal's writer: one archive inspection) and the profile, committed last. A refused plan (`KILN_SHROUDFORGE=0`, no
//! interpreter, no archive inspector, an input that cannot be read, ...) commits the profile without one: the core then
//! refuses the start of ShroudForge packages at boot, and every other mod starts. Any error before the commit settles
//! the journal (rolled back, or refused when the copies changed).
//!
//! Prepare runs before the journal begins, as in main, and the journal does not see it: Copy mode's copies under
//! `kiln/profiles/<profile>/`, the config copy and its sync record, the native mods' switches (native.json). A stop
//! between prepare and the journal leaves those changed beside the earlier profile file, as main's activation does. None
//! of them is an input of the launch plan (`<game>/mods`, chain files, kiln.json, the game's DLLs, the archive, the
//! placement receipts) or of [`sfi_activation::start_check`], so the earlier profile and the plan it pins still agree;
//! prepare places or removes no copy of `crate::engine_mods`, so the earlier journal is still settled
//! (`sfi_activation::begin`) before any copy changes.
//!
//! **Refresh** ([`refresh`], what Play runs): a profile that renders otherwise is activated again. A current one is
//! reconciled inside a journal (game-folder files, copies, chain.ini: never an unplanned change first). The plan it pins
//! is kept when it is still exactly what the producer would write ([`fresh`]: the inspection it was made from replayed,
//! every other input read again, the same bytes), and the journal is released. Otherwise a new plan is produced (one
//! inspection) and the profile committed with the next generation; a production that is refused releases the journal
//! and keeps what the profile pinned (kiln-sfi still withdraws a package whose files changed). So a plan is produced
//! only when its inputs changed: a Play that keeps it reads no archive. A refusal is retried only when its inputs
//! changed too ([`inputs`]): with the same ones a refresh, or a play-once start, answers the recorded refusal and
//! inspects nothing, so a Play with no plan possible reads the archive once, not at every refresh (the check before Play
//! refreshes twice); an activation always produces again. Only a refusal recorded for the profile file as it is on disk
//! is answered (a record that names another plan than the one the file pins is no answer), and never `write-failed`: a
//! plan that could not be written is no answer of its inputs, so the next operation tries again (the journal forgets a
//! plan it could not publish, [`sfi_activation::Staging::write_plan`], so a refresh is released as usual). The archive
//! files' size and time are a trigger,
//! not a proof: a rewrite that keeps both keeps the plan, and kiln-eml's boot witness, which proves the plan's archive
//! and executable digests at every start, then refuses its startup data (`plan-stale`); an activation always inspects
//! again.
//!
//! **Play-once** ([`once`]): main's checks (fixed folders unchanged), the XHL-Core copies, prepare and `once.ini` as
//! main writes them, then a journal of mode once: the start's own plan (`profile.mode` `once`) and
//! `kiln/profile.once.json` pinning it, committed last. The active profile and its plan stay as they are; a refused
//! active journal refuses the play-once start too (activate first).
//!
//! **What the launcher reads** ([`status`]): why a start of the ShroudForge family will not happen, and the plan's
//! warnings and decisions, for the check before Play. Each operation keeps its plan's outcome in
//! `<data>/installs/<install id>.sfi.json` ([`Record`]) with the inspection a later refresh replays.
//!
//! **The archive inspector** ([`set_inspector`]): kiln-eml's `kiln-eml-inspect` as this Kiln ships it. The launcher
//! and kilnctl set the one installed with each game's Kiln at start ([`use_installed_inspector`],
//! `sfi_inspection::Installed`): each operation resolves it for its game (the install receipt's path, SHA-256
//! and version), and a game without a usable one gets no plan, with that reason. Until one is set no plan is written
//! (`no-archive-base`).
//!
//! The first-managed origin of the game's archive is kiln-eml's ownership receipt (`baseOrigin`), which the
//! inspector reads and the plan carries; nothing here records it again.

use super::*;
use crate::ErrorCode;
use crate::sfi_activation::{self, Begin, Recovered, Staging};
use crate::sfi_backend::{Backend, Env};
use crate::sfi_inspection::Inspect;
use crate::sfi_plan::{Mode, Outcome, PlanRef, ProfileIdentity, Reason};
use crate::sfi_routing::{Route, Runner};
use serde_json::Value;
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, RwLock};

// ---- the decision ----

/// What one operation decides once and every step of it follows (see the module docs).
pub(crate) struct Decision {
    pub(crate) side: RunSide,
    pub(crate) env: Env,
    pub(crate) selection: crate::sfi_backend::Resolution,
    pub(crate) route: Route,
}

impl Decision {
    pub(crate) fn of(install: &GameInstall) -> Decision {
        let side = match install.kind {
            crate::game::GameKind::Client => RunSide::Client,
            crate::game::GameKind::Server => RunSide::Server,
        };
        let env = Env::from_process();
        let selection = crate::sfi_backend::resolve_game(&install.path, side, &env);
        let route = crate::sfi_routing::route_from(&install.path, &selection);
        Decision {
            side,
            env,
            selection,
            route,
        }
    }

    /// The interpreter's route: the operation runs through the activation journal.
    pub(crate) fn journaled(&self) -> bool {
        self.route.interpreter()
    }
}

/// An unfinished kiln-mode activation of the game, settled before an operation on main's routes changes a copy
/// (`sfi_activation::recover`, on the route that activation froze): what it did, as notes. A journal this Kiln cannot
/// read stays: a main-route activation supersedes it ([`after`]), a kiln-mode one is its explicit recovery.
pub(crate) fn settle(dirs: &DataDirs, install: &GameInstall) -> Result<Vec<String>> {
    match sfi_activation::recover(dirs, &install.path) {
        // refused: what it refuses is the planned start of the ShroudForge family, which main's routes do not make; an
        // activation or a deactivation supersedes it ([`after`])
        Ok(Recovered::Refused { notes, .. }) => Ok(notes),
        Ok(r) => Ok(notes_of(&r)),
        Err(e) if matches!(e.code, ErrorCode::Schema | ErrorCode::Parse) => Ok(Vec::new()),
        Err(e) => Err(e),
    }
}

/// What settling an earlier activation did, as an activation's notes.
fn notes_of(r: &Recovered) -> Vec<String> {
    match r {
        Recovered::Nothing | Recovered::Completed { .. } => Vec::new(),
        Recovered::RolledBack { generation, notes } => std::iter::once(format!(
            "An earlier activation of this game (profile generation {generation}) did not finish; Kiln put the one \
             before it back whole."
        ))
        .chain(notes.iter().cloned())
        .collect(),
        Recovered::Refused { reason, notes } => notes.iter().cloned().chain(std::iter::once(reason.clone())).collect(),
    }
}

/// What ran on main's routes ([`after`]).
pub(crate) enum After {
    Activated,
    Once,
    Deactivated,
}

/// After an operation on main's routes: a refused journal goes once an activation (or a deactivation) replaced what it
/// describes, the plans no profile file pins any more go, and so does the record of the plan of the profile file it
/// wrote (main's routes pin none). Nothing here fails the operation: a plan left behind is harmless and goes next time.
/// A game that never ran in kiln mode is not touched (no journal, no launch-plans folder, no record).
pub(crate) fn after(dirs: &DataDirs, install: &GameInstall, _: &Decision, what: After) {
    let game = &install.path;
    if matches!(what, After::Activated | After::Deactivated) {
        let _ = sfi_activation::supersede_refused(game);
    }
    let _ = sfi_activation::prune_plans(game);
    let _ = update_record(dirs, install, |r| match what {
        After::Once => r.once = None,
        After::Activated | After::Deactivated => {
            r.active = None;
            r.once = None;
        }
    });
}

// ---- the archive inspector ----

/// The archive inspector of this process's kiln-mode activations.
pub type SharedInspector = Arc<dyn Inspect + Send + Sync>;

static INSPECTOR: RwLock<Option<SharedInspector>> = RwLock::new(None);

/// Sets the archive inspector this process's kiln-mode activations run: kiln-eml's `kiln-eml-inspect` as this Kiln
/// ships it (`crate::sfi_inspection::Tool` with its staged path, SHA-256 and version), or a test's stand-in. None
/// (the default): no plan is written (`no-archive-base`), so the core refuses the start of ShroudForge packages at boot.
pub fn set_inspector(inspector: Option<SharedInspector>) {
    *INSPECTOR.write().unwrap_or_else(|e| e.into_inner()) = inspector;
}

fn inspector() -> Option<SharedInspector> {
    INSPECTOR.read().unwrap_or_else(|e| e.into_inner()).clone()
}

/// What the launcher and kilnctl call at start: this process's kiln-mode activations run the archive inspector
/// the Kiln installed in each game folder brought (`crate::sfi_inspection::Installed`: kiln-eml's component, its SHA-256
/// and version from the install receipt and `version.json`, the helper hashed again when it starts).
pub fn use_installed_inspector() {
    set_inspector(Some(Arc::new(crate::sfi_inspection::Installed)));
}

/// The inspector to run for `game`: the set one, resolved for that game folder ([`Inspect::for_game`]); one that
/// cannot be resolved answers with why (no plan, that reason).
enum ForGame {
    Set(SharedInspector),
    Resolved(Box<dyn Inspect>),
    Unavailable(String),
    None,
}

impl ForGame {
    fn of(game: &Path) -> ForGame {
        match inspector() {
            None => ForGame::None,
            Some(i) => match i.for_game(game) {
                Ok(None) => ForGame::Set(i),
                Ok(Some(r)) => ForGame::Resolved(r),
                Err(e) => ForGame::Unavailable(e),
            },
        }
    }
}

/// Why the inspector set for this process cannot be used for `game` now (the installed one does not resolve), or None
/// (it can, or none is set). The check before Play says it: a refresh keeps the pinned plan without one.
pub fn inspector_unresolved(game: &Path) -> Option<String> {
    match ForGame::of(game) {
        ForGame::Unavailable(why) => Some(why),
        _ => None,
    }
}

/// An inspector that has none for this game, and says why.
struct Unavailable(String);

impl Inspect for Unavailable {
    fn run(&self, _: &Path, _: RunSide) -> std::result::Result<Vec<u8>, String> {
        Err(self.0.clone())
    }
    fn version(&self) -> &str {
        ""
    }
}

/// Why there is no inspection without an inspector.
pub const NO_INSPECTOR: &str =
    "Kiln's archive inspector (kiln-eml-inspect) is not installed with this Kiln";

struct NoInspector;

impl Inspect for NoInspector {
    fn run(&self, _: &Path, _: RunSide) -> std::result::Result<Vec<u8>, String> {
        Err(NO_INSPECTOR.into())
    }
    fn version(&self) -> &str {
        ""
    }
}

/// An inspector whose answer is kept: the inspection a plan was made from, for a later refresh to replay ([`fresh`]).
struct Recording<'a> {
    inner: &'a dyn Inspect,
    answer: RefCell<Option<Vec<u8>>>,
}

impl Inspect for Recording<'_> {
    fn run(&self, game: &Path, side: RunSide) -> std::result::Result<Vec<u8>, String> {
        let out = self.inner.run(game, side);
        if let Ok(b) = &out {
            *self.answer.borrow_mut() = Some(b.clone());
        }
        out
    }
    fn version(&self) -> &str {
        self.inner.version()
    }
}

/// The inspection a plan was made from, given again.
struct Replayed<'a> {
    answer: &'a [u8],
    version: &'a str,
}

impl Inspect for Replayed<'_> {
    fn run(&self, _: &Path, _: RunSide) -> std::result::Result<Vec<u8>, String> {
        Ok(self.answer.to_vec())
    }
    fn version(&self) -> &str {
        self.version
    }
}

// ---- the record: <data>/installs/<install id>.sfi.json ----

/// What the last kiln-mode operation of each profile file decided about its launch plan
/// (`<data>/installs/<install id>.sfi.json`, the launcher's data folder): [`status`] reads it.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Record {
    pub format: u32,
    /// kiln/profile.json's plan.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub active: Option<PlanRecord>,
    /// kiln/profile.once.json's plan.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub once: Option<PlanRecord>,
}

/// One profile file's launch plan, as its last kiln-mode operation left it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct PlanRecord {
    pub profile_id: String,
    /// The generation of the profile file the operation wrote or kept.
    pub generation: u64,
    /// `written` (a new plan), `kept` (the plan the profile pins is current) or `refused` (no new plan).
    pub outcome: String,
    /// Refused: the producer's code and reason.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub code: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
    /// The plan the profile file pins.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub plan: Option<PlanRef>,
    /// The plan's top-level warnings (a folder not planned, a save-sensitive package switched off, ...).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub warnings: Vec<Note>,
    /// Each listed package's decision, by id: candidate, skip, disabled or other-side.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub decisions: BTreeMap<String, String>,
    /// What settling an earlier unfinished activation did.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub recovered: Vec<String>,
    /// The inspection the plan was made from, for [`fresh`].
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub inspection: Option<Inspection>,
    /// Refused: what the refusal was made from ([`inputs`]). A refresh or a play-once start with the same inputs answers
    /// the refusal again and produces nothing (no inspection). None for `write-failed` (no answer of the inputs: the next
    /// operation tries again).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub inputs: Option<String>,
}

impl PlanRecord {
    fn new(profile_id: &str, generation: u64) -> Self {
        PlanRecord {
            profile_id: profile_id.to_owned(),
            generation,
            outcome: String::new(),
            code: None,
            reason: None,
            plan: None,
            warnings: Vec::new(),
            decisions: BTreeMap::new(),
            recovered: Vec::new(),
            inspection: None,
            inputs: None,
        }
    }
}

/// A plan's reason (`warnings[]`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Note {
    pub code: String,
    pub message: String,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub caused_by: Vec<String>,
    pub severity: String,
}

impl Note {
    fn of(r: &Reason) -> Note {
        Note {
            code: r.code.clone(),
            message: r.message.clone(),
            caused_by: r.caused_by.clone(),
            severity: r.severity.to_owned(),
        }
    }
}

/// The inspector's answer a plan was made from, and the archive files it read as they were then.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Inspection {
    /// The inspector's version.
    pub version: String,
    /// Its answer (one JSON document).
    pub answer: String,
    pub files: Vec<ArchiveFile>,
}

/// One file of the inspector's `inventory`, with its size and modification time (None: not there).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct ArchiveFile {
    pub file: String,
    pub size: Option<u64>,
    pub modified: Option<String>,
}

fn record_path(dirs: &DataDirs, install: &GameInstall) -> PathBuf {
    dirs.installs().join(format!("{}.sfi.json", install.id))
}

/// The record (empty when there is none, or it is not one this Kiln reads).
pub fn record(dirs: &DataDirs, install: &GameInstall) -> Record {
    crate::util::read_json::<Record>(&record_path(dirs, install))
        .ok()
        .flatten()
        .filter(|r| r.format == 1)
        .unwrap_or_default()
}

/// Changes the record: written when it says something, removed when it says nothing; a record that is not there and
/// would say nothing is not made.
fn update_record(
    dirs: &DataDirs,
    install: &GameInstall,
    change: impl FnOnce(&mut Record),
) -> Result<()> {
    let path = record_path(dirs, install);
    let there = std::fs::symlink_metadata(&path).is_ok();
    let mut r = record(dirs, install);
    change(&mut r);
    if r.active.is_none() && r.once.is_none() {
        return if there {
            remove_if_there(&path)
        } else {
            Ok(())
        };
    }
    r.format = 1;
    std::fs::create_dir_all(dirs.installs()).map_err(io_at(&dirs.installs(), "creating"))?;
    crate::util::write_json_atomic(&path, &r)
}

/// The files an inspection read (its `inventory`), each with its size and modification time now. A game update changes
/// them, and a refresh then inspects again; only a trigger: the boot proves the archive at every start.
fn archive_files(game: &Path, answer: &[u8]) -> Vec<ArchiveFile> {
    let v: Value = serde_json::from_slice(answer).unwrap_or(Value::Null);
    let mut out: Vec<ArchiveFile> = v["inventory"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|i| i["file"].as_str())
        .map(|f| {
            let path = f
                .split(['/', '\\'])
                .fold(game.to_path_buf(), |p, c| p.join(c));
            let meta = std::fs::symlink_metadata(&path).ok();
            ArchiveFile {
                file: f.to_owned(),
                size: meta.as_ref().map(|m| m.len()),
                modified: meta
                    .and_then(|m| m.modified().ok())
                    .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                    .map(|d| format!("{}.{:09}", d.as_secs(), d.subsec_nanos())),
            }
        })
        .collect();
    out.sort_by(|a, b| a.file.cmp(&b.file));
    out.dedup();
    out
}

// ---- the plan ----

/// What producing one profile file's plan gave.
struct Produced {
    outcome: Outcome,
    inspection: Option<Inspection>,
}

/// The plan for the profile file `ident` names, on the operation's decision, published by `write` (the journal's).
fn produce(
    game: &Path,
    d: &Decision,
    ident: &ProfileIdentity,
    write: &mut crate::sfi_plan::PlanWriter<'_>,
) -> Produced {
    // the plan follows the route the operation decided, as placement and render did: a start the interpreter does not
    // run gets no plan, whatever the game folder holds by now (kiln-sfi.dll installed since the decision)
    match d.route {
        Route::Interpreter(Runner::EnvOff) => {
            return Produced {
                outcome: crate::sfi_plan::interpreter_off(d.selection.clone()),
                inspection: None,
            };
        }
        Route::Interpreter(Runner::Missing) => {
            return Produced {
                outcome: crate::sfi_plan::interpreter_missing(d.selection.clone()),
                inspection: None,
            };
        }
        _ => {}
    }
    let resolved = ForGame::of(game);
    let none = NoInspector;
    let unavailable;
    let inner: &dyn Inspect = match &resolved {
        ForGame::Set(i) => i.as_ref(),
        ForGame::Resolved(r) => r.as_ref(),
        ForGame::Unavailable(why) => {
            unavailable = Unavailable(why.clone());
            &unavailable
        }
        ForGame::None => &none,
    };
    let refused = |reason: String| Produced {
        outcome: Outcome::Refused {
            code: "plan-invalid",
            reason: format!(
                "{reason}; no launch plan was written, so the game starts without ShroudForge packages"
            ),
            resolution: d.selection.clone(),
        },
        inspection: None,
    };
    let policy = match crate::sfi_scan::shipped_policy() {
        Ok(p) => p,
        Err(e) => {
            return refused(format!(
                "the save-sensitive policy this Kiln ships cannot be read ({e})"
            ));
        }
    };
    let native = match crate::sfi_scan::native::NativeConfig::shipped() {
        Ok(n) => n,
        Err(e) => {
            return refused(format!(
                "the DLL Manager inputs this Kiln ships cannot be read ({e})"
            ));
        }
    };
    let recording = Recording {
        inner,
        answer: RefCell::new(None),
    };
    let outcome = crate::sfi_plan::produce_resolved(
        crate::sfi_plan::Request {
            game,
            side: d.side,
            env: &d.env,
            profile: ident,
            inspector: &recording,
            native,
            policy: &policy,
            catalog: crate::sfi_scan::parity::CatalogInput::Missing,
            carried: &[],
        },
        d.selection.clone(),
        write,
    );
    let inspection = match (&outcome, recording.answer.into_inner()) {
        (Outcome::Written(_), Some(answer)) => {
            let files = archive_files(game, &answer);
            String::from_utf8(answer).ok().map(|answer| Inspection {
                version: inner.version().to_owned(),
                answer,
                files,
            })
        }
        _ => None,
    };
    Produced {
        outcome,
        inspection,
    }
}

/// Is `pin`, the plan the profile file `doc` (as it is on disk) pins, still exactly what the producer would write for
/// it now? The inspection the plan was made from (`record`) is replayed: the archive files it read have the size and
/// time they had and the inspector this process runs, when it has one, is the same version. Every other input is read
/// again, and the plan rendered with its own id must be its bytes.
fn fresh(
    game: &Path,
    d: &Decision,
    doc: &Value,
    mode: Mode,
    pin: &PlanRef,
    record: Option<&PlanRecord>,
) -> bool {
    let Some(insp) = record
        .filter(|r| r.plan.as_ref() == Some(pin))
        .and_then(|r| r.inspection.as_ref())
    else {
        return false;
    };
    // the inspector this process runs for this game, when it has one: an upgraded Kiln's (another version) inspects
    // again; one that cannot be resolved now is no inspector (the plan's other inputs decide)
    let current = match ForGame::of(game) {
        ForGame::Set(i) => Some(i.version().to_owned()),
        ForGame::Resolved(r) => Some(r.version().to_owned()),
        ForGame::Unavailable(_) | ForGame::None => None,
    };
    if current.is_some_and(|v| v != insp.version) {
        return false;
    }
    if archive_files(game, insp.answer.as_bytes()) != insp.files {
        return false;
    }
    let Some(id) = pin
        .path
        .strip_prefix(&format!("{}/", crate::sfi_plan::PLAN_FOLDER))
        .and_then(|n| n.strip_suffix(".json"))
    else {
        return false;
    };
    let file = game
        .join(crate::paths::game::KILN)
        .join(crate::sfi_plan::PLAN_FOLDER)
        .join(format!("{id}.json"));
    if !std::fs::symlink_metadata(&file).is_ok_and(|m| m.is_file()) {
        return false;
    }
    let Ok(bytes) = std::fs::read(&file) else {
        return false;
    };
    if crate::hash::sha256_hex(&bytes) != pin.sha256 {
        return false;
    }
    let (Ok(ident), Ok(policy), Ok(native)) = (
        ProfileIdentity::of(doc, mode),
        crate::sfi_scan::shipped_policy(),
        crate::sfi_scan::native::NativeConfig::shipped(),
    ) else {
        return false;
    };
    let replayed = Replayed {
        answer: insp.answer.as_bytes(),
        version: &insp.version,
    };
    let req = crate::sfi_plan::Request {
        game,
        side: d.side,
        env: &d.env,
        profile: &ident,
        inspector: &replayed,
        native,
        policy: &policy,
        catalog: crate::sfi_scan::parity::CatalogInput::Missing,
        carried: &[],
    };
    crate::sfi_plan::draft(&req, &d.selection)
        .is_ok_and(|(draft, _)| crate::sfi_plan::render(&draft, id) == bytes)
}

/// A profile file about to be committed: rendered on the operation's route, the plan produced for exactly it.
struct Publish<'a> {
    dirs: &'a DataDirs,
    profile: &'a Profile,
    install: &'a GameInstall,
    d: &'a Decision,
    path_mode: PathMode,
    /// The file as prepared (generation and time set): its mods and EML lists are rendered again here.
    base: ProfileFile,
}

/// What a plan of the profile file `doc` (of `mode`) is made from, short of the archive's contents: this build
/// ([`build`]), the operation's decision, the inspector this process runs for the game (its version, or why it has none,
/// and by size and modification time the files of the one installed in the game folder), the profile
/// file without its generation and stamps, kiln.json, the chain files the plan frames for `mode`, the compatibility
/// rules, the placement and install receipts, and by size and modification time the game folder's own files (the
/// executable, the archive files and their backups, DLLs), kiln-eml's ownership receipts and every entry under
/// `<game>/mods`. A refused production is answered again while these are the same ([`stage_plan`]), so a Play with no
/// plan possible does not read the archive at every refresh. None when one of them cannot be read: the production runs.
fn inputs(game: &Path, d: &Decision, doc: &Value, mode: Mode) -> Option<String> {
    use crate::paths::game::{CHAIN, CHAIN_ONCE, EMF_CHAIN, PLAY_ONCE, RECEIPT};
    let mut f = crate::sfi_plan::Frame::new("refusal-inputs");
    f.text(&build()?);
    f.text(&format!(
        "{:?} {:?} {:?} {:?}",
        d.side, d.route, d.selection, d.env
    ));
    match ForGame::of(game) {
        ForGame::None => f.text("no inspector"),
        ForGame::Unavailable(why) => f.text(&format!("unavailable {why}")),
        ForGame::Set(i) => f.text(&format!("set {}", i.version())),
        ForGame::Resolved(r) => f.text(&format!("resolved {}", r.version())),
    }
    let mut profile = doc.as_object()?.clone();
    for key in ["$schema", "written", "writer", "sfLaunchPlan", "generation"] {
        profile.remove(key);
    }
    f.text(&crate::sfi_plan::canonical(&Value::Object(profile)));
    f.text(&format!("{mode:?}"));
    let mut files = vec![
        "kiln/kiln.json",
        CHAIN,
        EMF_CHAIN,
        crate::sfi_plan::RULES_FILE,
        "kiln/state/library-placements.json",
        RECEIPT,
    ];
    if mode == Mode::Once {
        files.extend([CHAIN_ONCE, PLAY_ONCE]);
    }
    for rel in files {
        f.text(rel);
        match std::fs::read(rel.split('/').fold(game.to_path_buf(), |p, c| p.join(c))) {
            Ok(b) => f.bytes(&b),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => f.optional(None),
            Err(_) => return None,
        }
    }
    entries(&mut f, game, game, false)?;
    for rel in [
        "kiln/eml/ownership",
        crate::sfi_inspection::INSTALL_DIR,
        "mods",
    ] {
        f.text(rel);
        entries(
            &mut f,
            game,
            &rel.split('/').fold(game.to_path_buf(), |p, c| p.join(c)),
            true,
        )?;
    }
    Some(f.finish())
}

/// This build, for [`inputs`]: the workspace version names no release (every development build is `0.2.0-dev`), so
/// the program file too, by size and modification time (an update replaces it). None when it cannot be read.
fn build() -> Option<String> {
    let meta = std::fs::metadata(std::env::current_exe().ok()?).ok()?;
    Some(format!(
        "{} {} {}",
        env!("CARGO_PKG_VERSION"),
        meta.len(),
        stamp(&meta)
    ))
}

/// A file's modification time for [`inputs`] (`-`: none).
fn stamp(meta: &std::fs::Metadata) -> String {
    meta.modified()
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map_or_else(
            || "-".to_owned(),
            |d| format!("{}.{:09}", d.as_secs(), d.subsec_nanos()),
        )
}

/// The entries of `dir` for [`inputs`], by name: a file with its size and modification time, a folder by its name
/// (`deep`: and its entries), a link as a link (never followed); `<game>/shroudforge` is never looked into.
fn entries(f: &mut crate::sfi_plan::Frame, game: &Path, dir: &Path, deep: bool) -> Option<()> {
    let list = match std::fs::read_dir(dir) {
        Ok(l) => l,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            f.text("absent");
            return Some(());
        }
        Err(_) => return None,
    };
    let mut items = Vec::new();
    for e in list {
        let e = e.ok()?;
        items.push((e.file_name(), e.path()));
    }
    items.sort();
    for (name, path) in items {
        if dir == game && name.to_string_lossy().eq_ignore_ascii_case("shroudforge") {
            continue;
        }
        let meta = std::fs::symlink_metadata(&path).ok()?;
        f.bytes(name.as_encoded_bytes());
        if meta.file_type().is_symlink() {
            f.text("link");
        } else if meta.is_dir() {
            f.text("folder");
            if deep {
                entries(f, game, &path, true)?;
                f.text("end");
            }
        } else {
            f.text(&format!("{} {}", meta.len(), stamp(&meta)));
        }
    }
    Some(())
}

/// What [`stage_plan`] made: the file, its bytes, whether a plan was published for it, and the record.
struct Staged {
    file: ProfileFile,
    bytes: Vec<u8>,
    written: bool,
    record: PlanRecord,
}

impl Publish<'_> {
    /// The base file with the mods and EML lists the profile renders to now (after placement: the copies placed).
    fn render(&self, planned: bool) -> Result<ProfileFile> {
        let r = render_routed(
            self.dirs,
            self.profile,
            self.install,
            self.path_mode,
            &self.d.route,
            planned,
        )?;
        Ok(ProfileFile {
            mods: r.mods,
            eml_runtime: r.eml_runtime,
            eml_runtime_blocked: r.eml_runtime_blocked,
            eml_partial: r.eml_partial,
            ..self.base.clone()
        })
    }
}

/// The final file of an activation and its plan: rendered for a plan when the interpreter runs this start, the plan
/// produced for exactly that file with the journal's writer; refused, the file as main's rule renders it (no plan).
/// `earlier`: the record of this profile file's last operation, when it names the plan the file on disk pins (or none
/// when it pins none). When it is a refusal made from the same [`inputs`], that refusal is the answer and nothing is
/// produced (no inspection); only a change of an input retries. An activation passes none: it always produces. A plan that
/// could not be written (`write-failed`) records no inputs, so it is never answered again.
fn stage_plan(
    p: &Publish<'_>,
    tx: &mut Staging,
    mode: Mode,
    earlier: Option<&PlanRecord>,
) -> Result<Staged> {
    let planned = p.d.route.runs();
    let mut file = p.render(planned)?;
    let doc = serde_json::to_value(&file)?;
    let ident = ProfileIdentity::of(&doc, mode).map_err(|e| Error::new(ErrorCode::Schema, e))?;
    let inputs = inputs(&p.install.path, p.d, &doc, mode);
    let mut record = PlanRecord::new(&file.id, file.generation);
    let again =
        earlier.filter(|r| r.outcome == "refused" && inputs.is_some() && r.inputs == inputs);
    let written = if let Some(r) = again {
        // the refusal made from exactly these inputs, answered again
        record.outcome = "refused".into();
        record.code = r.code.clone();
        record.reason = r.reason.clone();
        false
    } else {
        let produced = produce(&p.install.path, p.d, &ident, &mut |id, bytes| {
            tx.write_plan(id, bytes)
        });
        match produced.outcome {
            Outcome::Written(w) => {
                file.sf_launch_plan = Some(w.reference.clone());
                record.outcome = "written".into();
                record.plan = Some(w.reference.clone());
                record.warnings = w.warnings.iter().map(Note::of).collect();
                record.decisions = w
                    .decisions
                    .iter()
                    .map(|(k, v)| (k.clone(), (*v).to_owned()))
                    .collect();
                record.inspection = produced.inspection;
                true
            }
            Outcome::Refused { code, reason, .. } => {
                record.outcome = "refused".into();
                record.code = Some(code.to_owned());
                record.reason = Some(reason);
                false
            }
            Outcome::NotKiln { resolution } => {
                record.outcome = "refused".into();
                record.code = Some("not-kiln".into());
                record.reason = Some(resolution.reason);
                false
            }
        }
    };
    if !written && record.code.as_deref() != Some("write-failed") {
        record.inputs = inputs;
    }
    if !written && planned {
        file = p.render(false)?;
    }
    let mut bytes = serde_json::to_vec_pretty(&file)?;
    bytes.push(b'\n');
    Ok(Staged {
        file,
        bytes,
        written,
        record,
    })
}

/// An error before the commit: the journal settled now (rolled back, or refused when the copies changed), and said.
fn aborted(tx: Staging, mut e: Error) -> Error {
    match tx.abort() {
        Ok(Recovered::Refused { reason, .. }) => e.message.push_str(&format!("; {reason}")),
        Ok(_) => {}
        Err(back) => e.message.push_str(&format!(
            "; and the activation could not be settled: {}",
            back.message
        )),
    }
    e
}

/// chain.ini as the profile wants it, staged in the journal: whether it wrote, and the rules' notes on the load order.
fn chain(
    dirs: &DataDirs,
    profile: &Profile,
    install: &GameInstall,
    file: &ProfileFile,
    tx: &mut Staging,
) -> Result<(bool, Vec<String>)> {
    let entries = crate::compat::profile_entries(dirs, profile, install, file)?;
    let plan = crate::compat::plan(dirs, profile, install, &entries);
    let wrote = tx.chain(|stage| {
        crate::chain::write_profile_plan_staged(&install.path, &plan.chain, stage)
    })?;
    Ok((wrote, plan.order_notes()))
}

/// The copies of the profile on the decided route, and the kept settings of a rolled back version put back, staged in
/// the journal (before the plan hashes the copies).
fn place(
    dirs: &DataDirs,
    profile: &Profile,
    install: &GameInstall,
    d: &Decision,
    tx: &mut Staging,
) -> Result<crate::engine_mods::PlaceReport> {
    tx.placements(|| {
        let mut placed = crate::engine_mods::place_routed(dirs, profile, &install.path, &d.route)?;
        placed.kept_files.extend(crate::kept::restore_after_place(
            dirs,
            profile,
            &install.path,
        ));
        Ok(placed)
    })
}

/// The profile file of `mode` as it is on disk (Null: none, or not JSON).
fn profile_doc(game: &Path, mode: Mode) -> Value {
    let rel = match mode {
        Mode::Active => crate::paths::game::PROFILE,
        Mode::Once => crate::paths::game::PROFILE_ONCE,
    };
    std::fs::read(game.join(rel))
        .ok()
        .and_then(|b| serde_json::from_slice(&b).ok())
        .unwrap_or(Value::Null)
}

// ---- activate, refresh, once ----

/// [`super::activate`] on the interpreter's route (see the module docs).
pub(crate) fn activate(
    dirs: &DataDirs,
    profile_id: &str,
    install: &GameInstall,
    d: &Decision,
) -> Result<Activation> {
    crate::library::migrate_identities(dirs)?;
    let profile = get(dirs, profile_id)?;
    check_profile(&profile)?;
    let prev = read_game_profile(&install.path)
        .ok()
        .flatten()
        .map(|f| f.generation)
        .unwrap_or(0);
    let generation = next_generation(prev)?;
    let (mut file, mode, copied, copy_warnings) =
        prepare(dirs, &profile, install, &d.route, d.route.runs())?;
    file.generation = generation;
    let now = now_rfc3339();
    file.written = Some(now.clone());
    let pj = install.path.join(crate::paths::game::PROFILE);
    let legacy_stage = crate::util::with_suffix(&pj, ".tmp");
    if std::fs::symlink_metadata(&legacy_stage).is_ok_and(|m| m.is_dir()) {
        return Err(Error::new(
            ErrorCode::Io,
            format!(
                "{}: legacy profile staging path is a directory",
                legacy_stage.display()
            ),
        ));
    }
    let (mut tx, recovered) = sfi_activation::begin(
        dirs,
        &install.path,
        Begin {
            profile_id: &profile.id,
            generation,
            route: &d.route,
        },
    )?;
    let steps = (|| -> Result<_> {
        // native mods that run from the game folder itself, then chain.ini (it names the loader placed for them), then the
        // copies: main's order, each step staged before its first change
        let game_files =
            tx.game_files(|| crate::game_files::place(dirs, &profile, &install.path))?;
        let (_, order_notes) = chain(dirs, &profile, install, &file, &mut tx)?;
        let placed = place(dirs, &profile, install, d, &mut tx)?;
        Ok((game_files, order_notes, placed))
    })();
    let (game_files, order_notes, placed) = match steps {
        Ok(s) => s,
        Err(e) => return Err(aborted(tx, e)),
    };
    let p = Publish {
        dirs,
        profile: &profile,
        install,
        d,
        path_mode: mode,
        base: file,
    };
    let staged = match stage_plan(&p, &mut tx, Mode::Active, None) {
        Ok(s) => s,
        Err(e) => return Err(aborted(tx, e)),
    };
    tx.commit(&staged.bytes)?;
    crate::kept::crash_point(crate::kept::Step::GameProfileWritten);
    remove_if_there(&install.path.join(crate::paths::game::CHAIN_ONCE))?;
    remove_if_there(&install.path.join(crate::paths::game::PROFILE_ONCE))?;
    let _ = sfi_activation::prune_plans(&install.path);
    let recovered = notes_of(&recovered);
    let a = Activation {
        game_id: install.id.clone(),
        profile_id: profile.id.clone(),
        generation: staged.file.generation,
        path_mode: mode,
        profile_json: pj.display().to_string(),
        copied,
        fingerprint: staged.file.fingerprint.clone().unwrap_or_default(),
        activated: now,
        own_loaders: placed.own_loaders,
        kept_files: placed.kept_files,
        warnings: skipped_native_loaders(dirs, &profile)
            .into_iter()
            .chain(copy_warnings)
            .chain(placed.warnings)
            .chain(placed.own_notes)
            .chain(game_files.notes)
            .chain(order_notes)
            .chain(xhl_twins(dirs, &profile, &install.path))
            .chain(recovered.iter().cloned())
            .collect(),
        retained: placed
            .retained
            .into_iter()
            .chain(game_files.retained)
            .collect(),
        safety_notes: placed.safety_notes,
    };
    let _ = crate::durable::json(
        &activation_record(dirs, install),
        &a,
        std::slice::from_ref(&pj),
    );
    let mut record = staged.record;
    record.recovered = recovered;
    let _ = update_record(dirs, install, |r| {
        r.active = Some(record);
        r.once = None;
    });
    Ok(a)
}

/// [`super::refresh_active`] on the interpreter's route (see the module docs): `file` is the game's profile.json, a
/// profile of this data folder.
pub(crate) fn refresh(
    dirs: &DataDirs,
    install: &GameInstall,
    d: &Decision,
    file: ProfileFile,
    profile: Profile,
    mode: PathMode,
) -> Result<Option<Activation>> {
    let game = &install.path;
    let pinned = file.sf_launch_plan.is_some();
    // a profile that no longer renders (a mod gone from the library), or renders to another file, is activated again
    if !render_routed(dirs, &profile, install, mode, &d.route, pinned)
        .is_ok_and(|fresh| same_file(&fresh, &file))
    {
        return super::activate(dirs, &profile.id, install).map(Some);
    }
    // never change a copy mid-game
    if running_process(install)?.is_some() {
        return Ok(None);
    }
    let generation = next_generation(file.generation)?;
    let (mut tx, recovered) = sfi_activation::begin(
        dirs,
        game,
        Begin {
            profile_id: &profile.id,
            generation,
            route: &d.route,
        },
    )?;
    // the file under the lock is the one read (an earlier activation completed by begin, or another process's, wrote a
    // new one): refresh that one
    if read_game_profile(game).ok().flatten().as_ref() != Some(&file) {
        tx.release()?;
        return super::refresh_active(dirs, install);
    }
    let staged = (|| -> Result<_> {
        let game_files = tx.game_files(|| crate::game_files::place(dirs, &profile, game))?;
        let mut placed = place(dirs, &profile, install, d, &mut tx)?;
        placed.warnings.extend(game_files.notes);
        placed.retained.extend(game_files.retained);
        Ok(placed)
    })();
    let mut placed = match staged {
        Ok(p) => p,
        Err(e) => return Err(aborted(tx, e)),
    };
    let mut first = notes_of(&recovered);
    first.append(&mut placed.warnings);
    placed.warnings = first;
    // a copy placed only now (an older Kiln's file names the Library entry): the file must name the copy
    let current = render_routed(dirs, &profile, install, mode, &d.route, pinned)
        .is_ok_and(|fresh| same_file(&fresh, &file));
    let (wrote, order_notes) = match chain(dirs, &profile, install, &file, &mut tx) {
        Ok(c) => c,
        Err(e) => return Err(aborted(tx, e)),
    };
    let order_notes = if wrote || !current {
        order_notes
    } else {
        Vec::new()
    };
    let kept = record(dirs, install).active;
    let keep = current
        && d.route.runs()
        && file.sf_launch_plan.as_ref().is_some_and(|pin| {
            fresh(
                game,
                d,
                &profile_doc(game, Mode::Active),
                Mode::Active,
                pin,
                kept.as_ref(),
            )
        });
    let staged = if keep {
        None
    } else {
        let now = now_rfc3339();
        // (never the plan the file pinned: it was made for the file as it was; stage_plan pins the one it publishes)
        let base = ProfileFile {
            generation,
            written: Some(now),
            sf_launch_plan: None,
            ..file.clone()
        };
        let p = Publish {
            dirs,
            profile: &profile,
            install,
            d,
            path_mode: mode,
            base,
        };
        // (a record that names another plan than the file pins is older than the file: no answer)
        let earlier = kept.as_ref().filter(|k| k.plan == file.sf_launch_plan);
        match stage_plan(&p, &mut tx, Mode::Active, earlier) {
            Ok(s) => Some(s),
            Err(e) => return Err(aborted(tx, e)),
        }
    };
    match staged {
        // nothing to publish: the plan the profile pins is current, or no new one can be made for a current profile
        // (the profile keeps what it pinned; the refusal is recorded for the check before Play)
        Some(s) if !s.written && current => {
            tx.release()?;
            // the refusal, with what the record knew of the plan the profile keeps (its warnings, its inspection)
            let pin = file.sf_launch_plan.clone();
            let before = kept.filter(|k| pin.is_some() && k.plan == pin);
            let record = PlanRecord {
                plan: pin,
                warnings: before
                    .as_ref()
                    .map(|k| k.warnings.clone())
                    .unwrap_or_default(),
                decisions: before
                    .as_ref()
                    .map(|k| k.decisions.clone())
                    .unwrap_or_default(),
                inspection: before.and_then(|k| k.inspection),
                recovered: notes_of(&recovered),
                ..s.record
            };
            let _ = update_record(dirs, install, |r| r.active = Some(record));
        }
        None => {
            tx.release()?;
        }
        Some(s) => {
            tx.commit(&s.bytes)?;
            if !current {
                // what an activation of a profile that renders otherwise does
                remove_if_there(&game.join(crate::paths::game::CHAIN_ONCE))?;
                remove_if_there(&game.join(crate::paths::game::PROFILE_ONCE))?;
            }
            let _ = sfi_activation::prune_plans(game);
            let pj = game.join(crate::paths::game::PROFILE);
            let warnings: Vec<String> = if current {
                placed
                    .warnings
                    .into_iter()
                    .chain(placed.safety_notes.iter().cloned())
                    .chain(order_notes)
                    .collect()
            } else {
                skipped_native_loaders(dirs, &profile)
                    .into_iter()
                    .chain(placed.warnings)
                    .chain(placed.own_notes)
                    .chain(order_notes)
                    .chain(xhl_twins(dirs, &profile, game))
                    .collect()
            };
            let a = Activation {
                game_id: install.id.clone(),
                profile_id: profile.id.clone(),
                generation: s.file.generation,
                path_mode: mode,
                profile_json: pj.display().to_string(),
                copied: 0,
                fingerprint: s.file.fingerprint.clone().unwrap_or_default(),
                activated: s.file.written.clone().unwrap_or_default(),
                own_loaders: placed.own_loaders,
                kept_files: placed.kept_files,
                warnings,
                retained: placed.retained,
                safety_notes: placed.safety_notes,
            };
            let _ = crate::durable::json(&activation_record(dirs, install), &a, &[pj]);
            let record = PlanRecord {
                recovered: notes_of(&recovered),
                ..s.record
            };
            let _ = update_record(dirs, install, |r| {
                r.active = Some(record);
                if !current {
                    r.once = None;
                }
            });
            return Ok(Some(a));
        }
    }
    // main's report of a reconcile: kept or restored files, notes, a player's folder that still runs
    if !(placed.retained.is_empty()
        && placed.kept_files.is_empty()
        && placed.warnings.is_empty()
        && placed.safety_notes.is_empty()
        && order_notes.is_empty())
        && let Some(mut a) = active(dirs, install)?
    {
        a.own_loaders = placed.own_loaders;
        a.kept_files = placed.kept_files;
        a.warnings = placed
            .warnings
            .into_iter()
            .chain(placed.safety_notes.iter().cloned())
            .chain(order_notes)
            .collect();
        a.safety_notes = placed.safety_notes;
        a.retained = placed.retained;
        return Ok(Some(a));
    }
    Ok(None)
}

/// [`super::write_once`] on the interpreter's route, after main's checks (see the module docs).
pub(crate) fn once(
    dirs: &DataDirs,
    profile: &Profile,
    install: &GameInstall,
    d: &Decision,
) -> Result<ProfileFile> {
    let game = &install.path;
    // a refused activation (the active profile's copies changed): nothing of a play-once start is placed until the
    // profile is activated again (begin_mode refuses it too, under the lock)
    if let Recovered::Refused { reason, .. } = sfi_activation::recover(dirs, game)? {
        return Err(Error::new(
            ErrorCode::Refused,
            format!("{reason}; activate the profile again before playing once"),
        ));
    }
    // XHL-Core named dinput8.dll runs from Kiln's copy under its own name; the active profile's copy stays as it is
    // (engine_mods' own publication journal; receipts spelled by this start's route)
    crate::engine_mods::place_xhl_core_for_once_routed(dirs, profile, game, &d.route)?;
    let (mut file, mode, _, _) = prepare(dirs, profile, install, &d.route, d.route.runs())?;
    file.written = Some(now_rfc3339());
    let (mut tx, recovered) = sfi_activation::begin_mode(
        dirs,
        game,
        Begin {
            profile_id: &profile.id,
            generation: file.generation,
            route: &d.route,
        },
        Mode::Once,
    )?;
    // the single start follows this profile's chain plan (main's once.ini, which the plan's fingerprint reads)
    let written = (|| -> Result<()> {
        let entries = crate::compat::profile_entries(dirs, profile, install, &file)?;
        crate::chain::write_once(
            game,
            &crate::compat::plan(dirs, profile, install, &entries).chain,
        )
    })();
    if let Err(e) = written {
        return Err(aborted(tx, e));
    }
    let p = Publish {
        dirs,
        profile,
        install,
        d,
        path_mode: mode,
        base: file,
    };
    // (a record that names another plan than profile.once.json pins is older than the file: no answer)
    let pin =
        serde_json::from_value::<PlanRef>(profile_doc(game, Mode::Once)["sfLaunchPlan"].clone())
            .ok();
    let earlier = record(dirs, install).once.filter(|r| r.plan == pin);
    let staged = match stage_plan(&p, &mut tx, Mode::Once, earlier.as_ref()) {
        Ok(s) => s,
        Err(e) => return Err(aborted(tx, e)),
    };
    tx.commit(&staged.bytes)?;
    let _ = sfi_activation::prune_plans(game);
    let record = PlanRecord {
        recovered: notes_of(&recovered),
        ..staged.record
    };
    let _ = update_record(dirs, install, |r| r.once = Some(record));
    Ok(staged.file)
}

// ---- what the launcher reads ----

/// Why a start does not run ShroudForge-format mods.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Refused {
    /// `setting-invalid`, `interpreter-held`, `interpreter-off`, `interpreter-missing`, `stale` (an unfinished or
    /// refused activation, a plan changed or gone), `plan-missing`, or the producer's code (`no-archive-base`,
    /// `input-unreadable`, `plan-invalid`, `write-failed`).
    pub code: String,
    pub reason: String,
}

/// What a start of a profile file means for the ShroudForge family, as the launcher shows it before Play.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Status {
    /// kiln.json asks for Kiln's interpreter, or its ShroudForge backend setting is invalid.
    pub requested: bool,
    /// `interpreter`, `interpreter-env-off`, `interpreter-missing` or `main`.
    pub route: &'static str,
    /// The start of ShroudForge packages does not happen: why (None: it may).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub refused: Option<Refused>,
    /// The plan the profile file pins.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub plan: Option<PlanRef>,
    /// The plan's top-level warnings and decisions, as its operation recorded them.
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub warnings: Vec<Note>,
    #[serde(skip_serializing_if = "BTreeMap::is_empty")]
    pub decisions: BTreeMap<String, String>,
}

/// What a start of `mode` in `install` means for its ShroudForge-format mods, for the check before Play and the
/// warning dialog. Reads kiln.json, the journal, the profile file, its plan's bytes and the record; writes nothing.
pub fn status(dirs: &DataDirs, install: &GameInstall, mode: Mode) -> Status {
    use crate::sfi_backend::Refusal;
    let d = Decision::of(install);
    let requested = d.selection.requested == Some(Backend::Kiln)
        || d.selection.refusal == Some(Refusal::SettingInvalid);
    let route = match &d.route {
        Route::Interpreter(Runner::Ready) => "interpreter",
        Route::Interpreter(Runner::EnvOff) => "interpreter-env-off",
        Route::Interpreter(Runner::Missing) => "interpreter-missing",
        Route::Legacy(_) => "main",
    };
    let mut s = Status {
        requested,
        route,
        refused: None,
        plan: None,
        warnings: Vec::new(),
        decisions: BTreeMap::new(),
    };
    let refuse = |code: &str, reason: String| {
        Some(Refused {
            code: code.to_owned(),
            reason,
        })
    };
    if !requested {
        return s;
    }
    if let Some(r) = d.selection.refusal {
        let code = match r {
            Refusal::SettingInvalid => "setting-invalid",
            Refusal::InterpreterHeld => "interpreter-held",
        };
        s.refused = refuse(code, d.selection.reason.clone());
        return s;
    }
    match &d.route {
        // ShroudForge's own loader runs them (its own-loader note says so)
        Route::Legacy(_) => return s,
        Route::Interpreter(Runner::EnvOff) => {
            s.refused = refuse(
                "interpreter-off",
                crate::own_loaders::RUNTIME_ENV_OFF_NOTE.to_owned(),
            );
            return s;
        }
        Route::Interpreter(Runner::Missing) => {
            s.refused = refuse(
                "interpreter-missing",
                crate::sfi_routing::INTERPRETER_MISSING_NOTE.to_owned(),
            );
            return s;
        }
        Route::Interpreter(Runner::Ready) => {}
    }
    if let Some(why) = sfi_activation::start_check_mode(&install.path, mode) {
        s.refused = refuse("stale", why);
        return s;
    }
    let pin =
        serde_json::from_value::<PlanRef>(profile_doc(&install.path, mode)["sfLaunchPlan"].clone())
            .ok();
    let all = record(dirs, install);
    let rec = match mode {
        Mode::Active => all.active,
        Mode::Once => all.once,
    };
    match pin {
        Some(pin) => {
            if let Some(r) = rec.filter(|r| r.plan.as_ref() == Some(&pin)) {
                // a refresh found the plan out of date and could not make a new one: the start keeps this plan (kiln-sfi
                // withdraws a package whose files changed), and says why there is no newer one
                if r.outcome == "refused" {
                    s.warnings.push(Note {
                        code: r.code.clone().unwrap_or_else(|| "refused".into()),
                        message: r.reason.clone().unwrap_or_default(),
                        caused_by: Vec::new(),
                        severity: "warning".into(),
                    });
                }
                s.warnings.extend(r.warnings);
                s.decisions = r.decisions;
            }
            s.plan = Some(pin);
        }
        None => {
            s.refused = match rec.filter(|r| r.outcome == "refused") {
                Some(r) => refuse(r.code.as_deref().unwrap_or("refused"), r.reason.unwrap_or_default()),
                None => refuse(
                    "plan-missing",
                    "the profile names no launch plan, so the game starts without ShroudForge packages; activate the \
                     profile again"
                        .into(),
                ),
            };
        }
    }
    s
}

/// Whether the plan the profile file of `mode` pins is still exactly what the producer would write for it now ([`fresh`],
/// on this process's decision): the check before Play boots a play-once start's plan, made by its prepare, only
/// while this holds. False on a route where the interpreter does not run, with a journal open or refused, or with no plan
/// pinned. Reads; writes nothing.
pub fn plan_current(dirs: &DataDirs, install: &GameInstall, mode: Mode) -> bool {
    let d = Decision::of(install);
    if !d.route.runs() || sfi_activation::start_check_mode(&install.path, mode).is_some() {
        return false;
    }
    let doc = profile_doc(&install.path, mode);
    let Ok(pin) = serde_json::from_value::<PlanRef>(doc["sfLaunchPlan"].clone()) else {
        return false;
    };
    let all = record(dirs, install);
    let rec = match mode {
        Mode::Active => all.active,
        Mode::Once => all.once,
    };
    fresh(&install.path, &d, &doc, mode, &pin, rec.as_ref())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The build is an input of a refusal: every development build has the workspace version `0.2.0-dev`, so the program
    /// file's size and modification time tell an updated Kiln from the one that recorded the refusal.
    #[test]
    fn the_build_of_a_refusal_names_the_program_file() {
        let meta = std::fs::metadata(std::env::current_exe().unwrap()).unwrap();
        let b = build().expect("this program's file");
        assert_eq!(
            b,
            format!(
                "{} {} {}",
                env!("CARGO_PKG_VERSION"),
                meta.len(),
                stamp(&meta)
            )
        );
        assert_ne!(stamp(&meta), "-");
    }

    /// The plan follows the route its operation decided, as placement and render did: on a route where the interpreter
    /// does not run this start no plan is produced, even when kiln-sfi.dll is there by the time the plan would be made
    /// (the producer's own refusal; nothing is inspected or written).
    #[test]
    fn no_plan_for_a_route_on_which_the_interpreter_does_not_run() {
        let root =
            std::env::temp_dir().join(format!("km-sfi-profile-route-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let game = root.join("game");
        std::fs::create_dir_all(game.join("kiln/plugins")).unwrap();
        std::fs::write(
            game.join("kiln/kiln.json"),
            br#"{"preview": ["kiln-sfi.interpreter"], "shroudforge": {"backend": "kiln"}}"#,
        )
        .unwrap();
        let env = Env {
            shroudforge: None,
            runtime: None,
        };
        let selection = crate::sfi_backend::resolve_game(&game, RunSide::Client, &env);
        assert_eq!(selection.selected, Some(Backend::Kiln), "{selection:?}");
        let route = crate::sfi_routing::route_from(&game, &selection);
        assert_eq!(route, Route::Interpreter(Runner::Missing));
        let d = Decision {
            side: RunSide::Client,
            env,
            selection,
            route,
        };
        // the interpreter arrives after the decision
        std::fs::write(
            game.join(crate::sfi_routing::INTERPRETER_PLUGIN),
            b"interpreter",
        )
        .unwrap();
        let ident = ProfileIdentity {
            id: "p".into(),
            generation: 1,
            mode: Mode::Active,
            semantic_sha256: "0".repeat(64),
        };
        let p = produce(&game, &d, &ident, &mut |_, _| panic!("nothing is written"));
        match &p.outcome {
            Outcome::Refused { code, .. } => assert_eq!(*code, "interpreter-missing"),
            other => panic!("{other:?}"),
        }
        assert!(p.inspection.is_none());
        let _ = std::fs::remove_dir_all(&root);
    }
}
