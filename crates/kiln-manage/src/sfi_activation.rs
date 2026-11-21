// SPDX-License-Identifier: MIT
//! The activation journal of a kiln-mode activation: one durable record, written before the
//! activation's first change in the game folder, of what the activation stages, so that a stop anywhere (an error, a
//! killed launcher) ends in one of two states, never between them:
//!
//! - **the earlier activation, whole**: its `kiln/profile.json` (and the launch plan it pins), `kiln/chain.ini` as it
//!   was, the game-folder files of `crate::game_files` as they were, and the copies of ShroudForge-format mods as they
//!   were; or
//! - **the new activation, whole**: its profile, which pins the launch plan staged for exactly this generation.
//!
//! A profile never points at a plan of another generation: the plan is written first (a new file, never over another,
//! `crate::sfi_plan::write_plan`), the profile commits last and only with the plan [`Staging::write_plan`] staged for
//! it ([`Staging::commit`] checks the reference, the generation and the profile's semantic hash before anything is
//! written). When neither state can be proven (the copies of ShroudForge-format mods changed, a file Kiln was to put back
//! was changed by hand, the profile was written by someone else), the journal stays as **refused**: [`start_check`]
//! refuses the planned start of the ShroudForge family until the player activates again (the explicit recovery,
//! [`begin`]); every other loader's files and the player's files are left as they are, and nothing is rolled back
//! wholesale.
//!
//! **A staging seam, not a second rollback.** Each step keeps its own journal and recovery: `crate::game_files` its
//! intents and commits (`kiln/state/game-files.json`), `crate::engine_mods` its publications
//! (`kiln/state/placement-journal.json`). This journal only records which of them the activation began, what
//! `kiln/chain.ini` held, which plan it published and which profile it is about to commit. [`recover`] lets each step
//! settle itself (`game_files::placed`, `engine_mods::recover_placements`, which also takes out a first copy published
//! before its receipt was written), then puts the earlier selection back with
//! the steps' own operations (`game_files::place_mods` with the packages placed before, as `profile::activate` does
//! after an error), restores `kiln/chain.ini` only while it holds the bytes this activation wrote, and removes only the
//! plan file this activation published (its exact SHA-256, pinned by no profile) and the temporary files a stop inside
//! its publication left.
//!
//! The order, as an activation runs it (`profile::activate`, and `profile::refresh_active` when it must publish a new
//! plan; `crate::profile::sfi` wires it):
//!
//! 1. [`begin`]: the lock (one activation of a game at a time, across processes), an earlier journal settled
//!    ([`recover`]), the earlier state recorded, the journal written durably. The route of the ShroudForge-format mods is
//!    frozen here (`crate::sfi_routing`), and recovery uses it, whatever kiln.json says later: no fallback.
//! 2. [`Staging::game_files`] around `game_files::place`.
//! 3. [`Staging::chain`] around `chain::write_profile_plan_staged`: the bytes chain.ini held and the SHA-256 of the
//!    bytes about to be written are recorded first.
//! 4. [`Staging::placements`] around `engine_mods::place_routed` with [`Staging::route`].
//! 5. [`Staging::write_plan`]: the plan's name and SHA-256 recorded, then the plan published (`sfi_plan::produce_with`
//!    takes it as its writer).
//! 6. [`Staging::commit`]: the profile's SHA-256 recorded, `kiln/profile.json` written durably, the journal removed.
//!
//! [`Staging::abort`] is [`recover`] for an activation that failed in this process. [`Staging::release`] ends one that
//! found its profile and the plan it pins still current (a refresh): the journal goes, nothing else changes. A play-once
//! start ([`begin_mode`] with `Mode::Once`) stages only its plan and commits `kiln/profile.once.json`. Plans no profile
//! file pins any more go with [`prune_plans`], under the same lock.

use crate::engine_mods::refuse_symlink_path;
use crate::paths::DataDirs;
use crate::profile::ProfileMod;
use crate::sfi_plan::{Mode, PLAN_FOLDER, PlanRef, ProfileIdentity};
use crate::sfi_routing::{Legacy, Route, Runner};
use crate::util::io_at;
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::path::{Path, PathBuf};

/// The journal, game-relative.
pub const JOURNAL: &str = "kiln/state/sfi-activation.json";
/// The lock one activation of a game holds from [`begin`] to its commit (another one waits), game-relative.
const LOCK: &str = "kiln/state/sfi-activation.lock";
/// The placement receipts of `crate::engine_mods`, game-relative.
const PLACEMENTS: &str = "kiln/state/library-placements.json";
/// The largest `kiln/chain.ini` the journal keeps a copy of (the proxy reads at most `chain::MAX_BYTES`).
const CHAIN_KEEP_MAX: usize = 4 * 1024 * 1024;

/// The steps of an activation's staging and of its recovery, in order: where a crash test stops the process
/// ([`set_crash_test_hook`]). `GameFiles*` are inside `crate::game_files`' own journal, `Placement*` inside
/// `crate::engine_mods`' publication of a copy.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Boundary {
    /// The journal is written; nothing else changed yet.
    JournalBegun,
    /// The journal records that the game-folder files change; `game_files::place` not called yet.
    GameFilesStaged,
    /// game_files: a package's intent is in its receipts, no file of it changed yet.
    GameFilesIntent,
    /// game_files: a file of a package changed.
    GameFilesOp,
    /// game_files: a package's receipt is committed, its side files not cleaned up yet.
    GameFilesCommitted,
    /// The journal records chain.ini's earlier bytes and the new bytes' SHA-256; chain.ini not written yet.
    ChainStaged,
    /// chain.ini is written.
    ChainWritten,
    /// The journal records that the copies of ShroudForge-format mods change; no copy changed yet.
    PlacementsStaged,
    /// engine_mods: a copy's publication is journaled and its earlier copy moved aside, the new one not published yet.
    PlacementParked,
    /// engine_mods: the new copy is published, its receipt not written yet.
    PlacementPublished,
    /// engine_mods: the receipt is written, the earlier copy not deleted yet.
    PlacementReceipt,
    /// Every copy is placed.
    PlacementsDone,
    /// The journal records the plan's name and SHA-256; the plan is not published yet.
    PlanStaged,
    /// The plan is published under its name.
    PlanWritten,
    /// The journal records the new profile's SHA-256; kiln/profile.json not written yet.
    ProfileStaged,
    /// kiln/profile.json is written; the journal not removed yet.
    ProfileWritten,
    /// The journal is removed: the activation is committed.
    Committed,
    /// The journal is removed without a new profile: the profile and the plan it pins still describe the game
    /// ([`Staging::release`], a refresh that found nothing to change).
    Released,
    /// Recovery removed the plan an unfinished activation published.
    RecoverPlanRemoved,
    /// Recovery put chain.ini back.
    RecoverChainRestored,
    /// Recovery put the earlier game-folder files back.
    RecoverGameFilesRestored,
    /// Recovery removed the journal, or wrote it as refused.
    RecoverSettled,
}

#[cfg(debug_assertions)]
static HOOK: std::sync::OnceLock<fn(Boundary)> = std::sync::OnceLock::new();

/// Debug and test builds only (a release build has no hook): the function a crash test runs at every [`Boundary`] of
/// this process (it stops the process there). Set once per process; false when one is set already.
#[cfg(debug_assertions)]
#[doc(hidden)]
pub fn set_crash_test_hook(hook: fn(Boundary)) -> bool {
    HOOK.set(hook).is_ok()
}

/// A [`Boundary`] passed (the crash test's hook runs, in debug and test builds only).
#[inline]
pub(crate) fn boundary(b: Boundary) {
    #[cfg(debug_assertions)]
    if let Some(hook) = HOOK.get() {
        hook(b);
    }
    #[cfg(not(debug_assertions))]
    let _ = b;
}

// ---- the record ----

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct Record {
    format: u32,
    /// A random id of this activation (16 hex digits).
    txn: String,
    state: State,
    /// Why it is refused ([`State::Refused`]).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    refused: Option<String>,
    profile_id: String,
    /// The generation the new kiln/profile.json carries.
    generation: u64,
    /// The route frozen at [`begin`].
    route: RouteMark,
    /// The profile file the activation commits (absent: kiln/profile.json).
    #[serde(default, skip_serializing_if = "Target::is_active")]
    mode: Target,
    prior: Prior,
    #[serde(default)]
    stages: Vec<Stage>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
enum State {
    /// The activation runs, or stopped: [`recover`] settles it.
    Staging,
    /// Neither the earlier nor the new state could be proven: the planned start stays refused until the next activation.
    Refused,
}

/// The profile file an activation commits: the active profile (`kiln/profile.json`) or a play-once start's
/// (`kiln/profile.once.json`, `profile::write_once`).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
enum Target {
    #[default]
    Active,
    Once,
}

impl Target {
    fn of(mode: Mode) -> Self {
        match mode {
            Mode::Active => Target::Active,
            Mode::Once => Target::Once,
        }
    }
    fn mode(self) -> Mode {
        match self {
            Target::Active => Mode::Active,
            Target::Once => Mode::Once,
        }
    }
    fn is_active(&self) -> bool {
        *self == Target::Active
    }
    /// The file, game-relative.
    fn file(self) -> &'static str {
        match self {
            Target::Active => crate::paths::game::PROFILE,
            Target::Once => crate::paths::game::PROFILE_ONCE,
        }
    }
    /// A plan's `profile.mode`.
    fn word(self) -> &'static str {
        match self {
            Target::Active => "active",
            Target::Once => "once",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
enum RouteMark {
    Interpreter,
    Legacy,
}

impl RouteMark {
    fn of(route: &Route) -> Self {
        if route.interpreter() {
            RouteMark::Interpreter
        } else {
            RouteMark::Legacy
        }
    }
    /// The route recovery uses: the one frozen at [`begin`] (main's routes name their folder as main reads it).
    fn route(self) -> Route {
        match self {
            RouteMark::Interpreter => Route::Interpreter(Runner::Ready),
            RouteMark::Legacy => Route::Legacy(Legacy::Refused),
        }
    }
}

/// What the game held before the activation.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct Prior {
    /// The profile file's SHA-256 (kiln/profile.json, or kiln/profile.once.json for a play-once start; None: there was
    /// none).
    profile_sha256: Option<String>,
    /// kiln/state/library-placements.json's SHA-256 (None: there was none).
    placements_sha256: Option<String>,
    /// The packages game_files had placed.
    game_files: Vec<Package>,
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Package {
    id: String,
    version: String,
    hash: String,
}

/// A step the activation began, recorded before its first change.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, tag = "stage", rename_all = "kebab-case")]
enum Stage {
    GameFiles,
    Chain {
        /// chain.ini's bytes before, hex (None: there was none).
        prior: Option<String>,
        /// The SHA-256 of the bytes written.
        sha256: String,
    },
    Placements,
    Plan {
        /// `launch-plans/<id>.json`, relative to `<game>/kiln`.
        path: String,
        sha256: String,
    },
    Profile {
        sha256: String,
    },
}

// ---- what callers get ----

/// What settling an earlier activation did ([`recover`], [`begin`], [`Staging::abort`]).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase", tag = "outcome")]
pub enum Recovered {
    /// No activation was unfinished.
    Nothing,
    /// The activation had committed its profile: only its journal was left.
    Completed { generation: u64 },
    /// The activation had not committed: the earlier state is back, whole.
    RolledBack { generation: u64, notes: Vec<String> },
    /// Neither state can be proven: the journal stays, the planned start of ShroudForge-format mods is refused until the
    /// next activation ([`start_check`]); nothing else was changed for it.
    Refused { reason: String, notes: Vec<String> },
}

/// What the activation is about to do ([`begin`]).
pub struct Begin<'a> {
    pub profile_id: &'a str,
    /// The generation the new kiln/profile.json carries.
    pub generation: u64,
    /// The route of the ShroudForge-format mods, frozen for this activation.
    pub route: &'a Route,
}

/// A committed activation.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Committed {
    pub generation: u64,
    /// The plan its profile pins.
    pub plan: Option<PlanRef>,
}

/// An activation between [`begin`] and [`Staging::commit`]: it holds the lock.
pub struct Staging {
    dirs: DataDirs,
    game: PathBuf,
    _lock: std::fs::File,
    record: Record,
    /// The route given to [`begin`], whole (the journal keeps only which folder it names): whether the interpreter runs
    /// this start decides what placement places (crate::sfi_routing `Runner`).
    route: Route,
    /// The plan published, and the profile it names.
    plan: Option<(PlanRef, PlanProfile)>,
}

/// A plan's `profile` member.
#[derive(Debug, Clone, PartialEq, Eq)]
struct PlanProfile {
    id: String,
    generation: u64,
    mode: String,
    semantic_sha256: String,
}

impl PlanProfile {
    fn of_plan(bytes: &[u8]) -> std::result::Result<Self, String> {
        let v: Value =
            serde_json::from_slice(bytes).map_err(|e| format!("the plan is not JSON ({e})"))?;
        let p = &v["profile"];
        Ok(Self {
            id: p["id"]
                .as_str()
                .ok_or("the plan names no profile")?
                .to_owned(),
            generation: p["generation"]
                .as_u64()
                .ok_or("the plan names no profile generation")?,
            mode: p["mode"]
                .as_str()
                .ok_or("the plan names no profile mode")?
                .to_owned(),
            semantic_sha256: p["semantic_sha256"]
                .as_str()
                .ok_or("the plan names no profile hash")?
                .to_owned(),
        })
    }
    fn of_identity(p: &ProfileIdentity) -> Self {
        Self {
            id: p.id.clone(),
            generation: p.generation,
            mode: match p.mode {
                Mode::Active => "active",
                Mode::Once => "once",
            }
            .to_owned(),
            semantic_sha256: p.semantic_sha256.clone(),
        }
    }
}

// ---- files ----

fn at(game: &Path, rel: &str) -> PathBuf {
    rel.split('/').fold(game.to_path_buf(), |p, c| p.join(c))
}

/// SHA-256 of a regular file below `game` (None: no file). A link, a folder or anything else there is refused.
fn file_sha(game: &Path, path: &Path) -> Result<Option<String>> {
    refuse_symlink_path(game, path)?;
    match std::fs::symlink_metadata(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(io_at(path, "inspecting")(e)),
        Ok(m) if m.is_file() => Ok(Some(crate::hash::sha256_file(path)?.0)),
        Ok(_) => Err(Error::new(
            ErrorCode::Refused,
            format!("{}: not a regular file", path.display()),
        )),
    }
}

fn read_bytes(game: &Path, path: &Path) -> Result<Option<Vec<u8>>> {
    refuse_symlink_path(game, path)?;
    match std::fs::read(path) {
        Ok(b) => Ok(Some(b)),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(io_at(path, "reading")(e)),
    }
}

fn lock(game: &Path) -> Result<std::fs::File> {
    let path = at(game, LOCK);
    refuse_symlink_path(game, &path)?;
    crate::durable::ensure_dir(path.parent().expect("lock folder"))?;
    let f = std::fs::OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(&path)
        .map_err(io_at(&path, "opening"))?;
    f.lock().map_err(io_at(&path, "locking"))?;
    Ok(f)
}

fn read_record(game: &Path) -> Result<Option<Record>> {
    let path = at(game, JOURNAL);
    refuse_symlink_path(game, &path)?;
    let Some(r) = crate::util::read_json::<Record>(&path)? else {
        return Ok(None);
    };
    if r.format != 1 || r.txn.len() != 16 || !r.txn.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(Error::new(
            ErrorCode::Schema,
            format!(
                "{}: not an activation journal this Kiln reads",
                path.display()
            ),
        ));
    }
    Ok(Some(r))
}

fn save_record(game: &Path, r: &Record) -> Result<()> {
    let path = at(game, JOURNAL);
    refuse_symlink_path(game, &path)?;
    crate::durable::ensure_dir(path.parent().expect("journal folder"))?;
    let mut bytes = serde_json::to_vec_pretty(r)?;
    bytes.push(b'\n');
    crate::durable::journal(&path, &bytes, &[], r.stages.len(), 0)
}

fn remove_record(game: &Path) -> Result<()> {
    let path = at(game, JOURNAL);
    refuse_symlink_path(game, &path)?;
    match std::fs::remove_file(&path) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => {
            return Err(io_at(&path, "removing")(e));
        }
        _ => {}
    }
    crate::durable::flush_dir(path.parent().expect("journal folder"))
}

fn packages(mods: Vec<ProfileMod>) -> Vec<Package> {
    let mut v: Vec<Package> = mods
        .into_iter()
        .map(|m| Package {
            id: m.id,
            version: m.version,
            hash: m.hash,
        })
        .collect();
    v.sort();
    v
}

/// A plan path the journal may name: `launch-plans/<file name>.json`, one plain name.
fn plan_rel_ok(rel: &str) -> bool {
    rel.strip_prefix(&format!("{PLAN_FOLDER}/"))
        .is_some_and(|name| {
            name.len() > ".json".len()
                && name.ends_with(".json")
                && !name.starts_with('.')
                && name
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'.' || b == b'_')
        })
}

/// Does kiln/profile.json or kiln/profile.once.json pin the plan `rel` (`launch-plans/<id>.json`)? A profile file that
/// is not JSON keeps every plan.
fn pinned(game: &Path, rel: &str) -> Result<bool> {
    for file in [
        crate::paths::game::PROFILE,
        crate::paths::game::PROFILE_ONCE,
    ] {
        let Some(bytes) = read_bytes(game, &at(game, file))? else {
            continue;
        };
        let Ok(v) = serde_json::from_slice::<Value>(&bytes) else {
            return Ok(true);
        };
        if v["sfLaunchPlan"]["path"].as_str() == Some(rel) {
            return Ok(true);
        }
    }
    Ok(false)
}

// ---- begin, stage, commit ----

/// Begins an activation of `game`: takes the lock (another activation waits for it), settles an unfinished earlier
/// activation ([`recover`]; a refused one is superseded by this one, which is its explicit recovery), records what
/// the game holds now and writes the journal durably, before anything changes. Returns the staging and what settling
/// the earlier activation did.
pub fn begin(dirs: &DataDirs, game: &Path, b: Begin<'_>) -> Result<(Staging, Recovered)> {
    begin_mode(dirs, game, b, Mode::Active)
}

/// [`begin`] for the profile file of `mode`: the active profile (kiln/profile.json), or a play-once start's
/// (kiln/profile.once.json; its plan's `profile.mode` is `once`). A play-once activation stages only its plan and its
/// profile file ([`Staging::write_plan`], [`Staging::commit`]).
pub fn begin_mode(
    dirs: &DataDirs,
    game: &Path,
    b: Begin<'_>,
    mode: Mode,
) -> Result<(Staging, Recovered)> {
    let target = Target::of(mode);
    if b.profile_id.is_empty() || b.profile_id.contains('\0') {
        return Err(Error::invalid("an activation needs a profile id"));
    }
    let lock = lock(game)?;
    let recovered = match settle(dirs, game) {
        Ok(r) => r,
        // a journal this Kiln cannot read proves nothing either: this activation is its explicit recovery
        Err(e) if e.code == ErrorCode::Schema || e.code == ErrorCode::Parse => Recovered::Refused {
            reason: e.message,
            notes: Vec::new(),
        },
        Err(e) => return Err(e),
    };
    // a play-once start never supersedes a refused activation: what it refuses is the active profile's copies, which
    // only an activation puts right
    if let (Target::Once, Recovered::Refused { reason, .. }) = (target, &recovered) {
        return Err(Error::new(
            ErrorCode::Refused,
            format!("{reason}; activate the profile again before playing once"),
        ));
    }
    let prior = Prior {
        profile_sha256: file_sha(game, &at(game, target.file()))?,
        placements_sha256: file_sha(game, &at(game, PLACEMENTS))?,
        game_files: packages(crate::game_files::placed(game)?),
    };
    let record = Record {
        format: 1,
        txn: crate::hash::hex(&crate::util::random_bytes::<8>()?),
        state: State::Staging,
        refused: None,
        profile_id: b.profile_id.to_owned(),
        generation: b.generation,
        route: RouteMark::of(b.route),
        mode: target,
        prior,
        stages: Vec::new(),
    };
    // (over a refused journal: this activation supersedes it)
    save_record(game, &record)?;
    boundary(Boundary::JournalBegun);
    Ok((
        Staging {
            dirs: dirs.clone(),
            game: game.to_path_buf(),
            _lock: lock,
            record,
            route: b.route.clone(),
            plan: None,
        },
        recovered,
    ))
}

impl Staging {
    fn stage(&mut self, s: Stage) -> Result<()> {
        self.record.stages.push(s);
        save_record(&self.game, &self.record)
    }

    fn has(&self, f: impl Fn(&Stage) -> bool) -> bool {
        self.record.stages.iter().any(f)
    }

    /// The route frozen at [`begin`], for every step of this activation: the one [`begin`] was given (recovery, in another
    /// process, names the same folder: [`RouteMark`]).
    pub fn route(&self) -> Route {
        self.route.clone()
    }

    /// The generation the new profile carries.
    pub fn generation(&self) -> u64 {
        self.record.generation
    }

    /// Runs `place` (`game_files::place`) once the journal records that the game-folder files change.
    pub fn game_files<T>(&mut self, place: impl FnOnce() -> Result<T>) -> Result<T> {
        if !self.has(|s| matches!(s, Stage::GameFiles)) {
            self.stage(Stage::GameFiles)?;
        }
        boundary(Boundary::GameFilesStaged);
        place()
    }

    /// Runs `write` (`chain::write_profile_plan_staged`), which calls the stage it is given with chain.ini's bytes
    /// before (None: no file) and the bytes it is about to write, under chain.ini's own lock: the journal records them
    /// before the file changes. Once per activation. Returns what `write` returns (whether it wrote).
    pub fn chain(
        &mut self,
        write: impl FnOnce(&mut crate::chain::Stage<'_>) -> Result<bool>,
    ) -> Result<bool> {
        if self.has(|s| matches!(s, Stage::Chain { .. })) {
            return Err(Error::invalid("chain.ini is staged once per activation"));
        }
        let path = at(&self.game, crate::paths::game::CHAIN);
        refuse_symlink_path(&self.game, &path)?;
        let mut staged = false;
        let mut stage = |prior: Option<&[u8]>, new: &[u8]| -> Result<()> {
            if prior.is_some_and(|p| p.len() > CHAIN_KEEP_MAX) {
                return Err(Error::new(
                    ErrorCode::Refused,
                    format!(
                        "{}: larger than the activation journal keeps; make it smaller first",
                        path.display()
                    ),
                ));
            }
            self.record.stages.push(Stage::Chain {
                prior: prior.map(crate::hash::hex),
                sha256: crate::hash::sha256_hex(new),
            });
            save_record(&self.game, &self.record)?;
            staged = true;
            boundary(Boundary::ChainStaged);
            Ok(())
        };
        let wrote = write(&mut stage)?;
        if wrote && !staged {
            return Err(Error::invalid("chain.ini was written without its stage"));
        }
        if wrote {
            boundary(Boundary::ChainWritten);
        }
        Ok(wrote)
    }

    /// Runs `place` (`engine_mods::place_routed` with [`Staging::route`]) once the journal records that the copies of
    /// ShroudForge-format mods (and the other copies placed with them) may change.
    pub fn placements<T>(&mut self, place: impl FnOnce() -> Result<T>) -> Result<T> {
        if !self.has(|s| matches!(s, Stage::Placements)) {
            self.stage(Stage::Placements)?;
        }
        boundary(Boundary::PlacementsStaged);
        let out = place()?;
        boundary(Boundary::PlacementsDone);
        Ok(out)
    }

    /// Publishes the launch plan `bytes` as `launch-plans/<id>.json` (`sfi_plan::write_plan`: a new file only), once the
    /// journal records its name and SHA-256. The plan must name the profile and generation this activation commits (its
    /// `profile` member). Ok(None): the name is taken (that file is not touched, and the journal forgets the name). Err: no
    /// plan was published; what the write left of it goes and the journal forgets the name too, so the activation goes on
    /// as one that published none (a refresh is released, an activation commits its profile without a plan, and the
    /// producer's refusal `write-failed` says why). One plan per activation. The writer `sfi_plan::produce_with` takes.
    pub fn write_plan(
        &mut self,
        id: &str,
        bytes: &[u8],
    ) -> std::result::Result<Option<PlanRef>, String> {
        if self.plan.is_some() || self.has(|s| matches!(s, Stage::Plan { .. })) {
            return Err("a launch plan is staged already in this activation".into());
        }
        let rel = format!("{PLAN_FOLDER}/{id}.json");
        if !plan_rel_ok(&rel) {
            return Err(format!("{id:?} is not a plan id"));
        }
        let names = PlanProfile::of_plan(bytes)?;
        if names.id != self.record.profile_id
            || names.generation != self.record.generation
            || names.mode != self.record.mode.word()
        {
            return Err(format!(
                "the plan names profile {} generation {} ({}), not the one this activation commits ({} generation {}, {})",
                names.id,
                names.generation,
                names.mode,
                self.record.profile_id,
                self.record.generation,
                self.record.mode.word()
            ));
        }
        let sha256 = crate::hash::sha256_hex(bytes);
        if let Err(e) = self.stage(Stage::Plan {
            path: rel.clone(),
            sha256: sha256.clone(),
        }) {
            // nothing published: the journal on disk holds the stage or not, and either way names no file of this plan
            self.record.stages.pop();
            return Err(e.message);
        }
        boundary(Boundary::PlanStaged);
        match crate::sfi_plan::write_plan(&self.game, id, bytes) {
            Ok(Some(r)) => {
                boundary(Boundary::PlanWritten);
                self.plan = Some((r.clone(), names));
                Ok(Some(r))
            }
            Ok(None) => {
                // another file has the name: never this activation's, so recovery must not remove it
                self.record.stages.pop();
                save_record(&self.game, &self.record).map_err(|e| e.message)?;
                Ok(None)
            }
            Err(why) => Err(self.unwritten_plan(&rel, &sha256, why)),
        }
    }

    /// A plan whose publication failed ([`Staging::write_plan`]): its temporary files go, and so does the plan file
    /// when the write got that far (its exact bytes, pinned by no profile file); then the journal forgets the plan, as
    /// though it was never staged. A file that cannot be told or removed keeps the stage: recovery settles it, and the
    /// activation is then committed or settled, never released. `why`, with what could not be done.
    fn unwritten_plan(&mut self, rel: &str, sha256: &str, why: String) -> String {
        remove_plan_temps(&self.game, rel);
        let file = at(&self.game.join(crate::paths::game::KILN), rel);
        let absent = std::fs::symlink_metadata(&file).is_err_and(|e| {
            // (NotADirectory: the plans folder is no folder, so nothing was written below it)
            matches!(
                e.kind(),
                std::io::ErrorKind::NotFound | std::io::ErrorKind::NotADirectory
            )
        });
        let have = if absent {
            Ok(None)
        } else {
            file_sha(&self.game, &file)
        };
        let gone = match have {
            Ok(None) => Ok(()),
            // not the bytes it staged: never this activation's file
            Ok(Some(have)) if have != sha256 => Ok(()),
            Ok(Some(_)) => match pinned(&self.game, rel) {
                Ok(false) => std::fs::remove_file(&file)
                    .map_err(io_at(&file, "removing"))
                    .and_then(|()| crate::durable::flush_dir(file.parent().expect("plan folder"))),
                Ok(true) => Ok(()),
                Err(e) => Err(e),
            },
            Err(e) => Err(e),
        };
        if let Err(e) = gone {
            return format!(
                "{why}; and the plan it left could not be removed ({})",
                e.message
            );
        }
        self.record.stages.pop();
        // (a journal that keeps the stage names a plan file that is not there: recovery and a commit both pass it)
        let _ = save_record(&self.game, &self.record);
        why
    }

    /// Commits the activation. `profile` (the bytes of the new kiln/profile.json) must be this activation's profile (its
    /// id and generation) and pin exactly the plan [`Staging::write_plan`] published, whose `profile` names this
    /// profile's semantic hash (or pin none, when none was published). Then the journal records the profile's SHA-256,
    /// kiln/profile.json is written durably, and the journal goes. A profile that does not fit is refused before
    /// anything is written; on any error the activation is settled ([`Staging::abort`]) before the error returns.
    pub fn commit(mut self, profile: &[u8]) -> Result<Committed> {
        match self.commit_inner(profile) {
            Ok(c) => Ok(c),
            Err(mut e) => {
                match settle(&self.dirs, &self.game) {
                    // kiln/profile.json was written, only the journal was left: the activation is committed
                    Ok(Recovered::Completed { generation }) => {
                        return Ok(Committed {
                            generation,
                            plan: self.plan.map(|(r, _)| r),
                        });
                    }
                    Ok(Recovered::Refused { reason, .. }) => {
                        e.message.push_str(&format!("; {reason}"))
                    }
                    Ok(_) => {}
                    Err(back) => e.message.push_str(&format!(
                        "; and the activation could not be settled: {}",
                        back.message
                    )),
                }
                Err(e)
            }
        }
    }

    fn commit_inner(&mut self, profile: &[u8]) -> Result<Committed> {
        let v: Value = serde_json::from_slice(profile).map_err(|e| {
            Error::new(
                ErrorCode::Parse,
                format!("the new profile is not JSON: {e}"),
            )
        })?;
        let target = self.record.mode;
        let refuse = |why: String| {
            Err(Error::new(
                ErrorCode::Conflict,
                format!("the new {} was not written: {why}", target.file()),
            ))
        };
        if v["id"].as_str() != Some(self.record.profile_id.as_str())
            || v["generation"].as_u64() != Some(self.record.generation)
        {
            return refuse(format!(
                "it is not profile {} generation {}",
                self.record.profile_id, self.record.generation
            ));
        }
        match (&self.plan, v.get("sfLaunchPlan")) {
            (None, None) => {}
            (None, Some(_)) => {
                return refuse("it pins a launch plan this activation did not publish".into());
            }
            (Some((r, _)), None) => {
                return refuse(format!(
                    "it does not pin the launch plan {} this activation published",
                    r.path
                ));
            }
            (Some((r, names)), Some(pin)) => {
                if *pin != r.profile_value() {
                    return refuse(format!(
                        "it pins {pin}, not the launch plan {} this activation published",
                        r.path
                    ));
                }
                let ident = ProfileIdentity::of(&v, target.mode())
                    .map_err(|e| Error::new(ErrorCode::Conflict, e))?;
                if PlanProfile::of_identity(&ident) != *names {
                    return refuse(format!(
                        "the launch plan {} was made for another state of this profile",
                        r.path
                    ));
                }
            }
        }
        let pj = at(&self.game, target.file());
        refuse_symlink_path(&self.game, &pj)?;
        if std::fs::symlink_metadata(&pj).is_ok_and(|m| !m.is_file()) {
            return refuse(format!("{} is not a file", pj.display()));
        }
        self.stage(Stage::Profile {
            sha256: crate::hash::sha256_hex(profile),
        })?;
        boundary(Boundary::ProfileStaged);
        crate::durable::journal(&pj, profile, &[], 0, 0)?;
        boundary(Boundary::ProfileWritten);
        remove_record(&self.game)?;
        boundary(Boundary::Committed);
        Ok(Committed {
            generation: self.record.generation,
            plan: self.plan.as_ref().map(|(r, _)| r.clone()),
        })
    }

    /// The activation failed in this process: settles it now, as [`recover`] would after a stop.
    pub fn abort(self) -> Result<Recovered> {
        settle(&self.dirs, &self.game)
    }

    /// Ends the activation without a new profile: the journal goes and every step stays as it is. Only for an
    /// activation whose profile file and the plan it pins still describe the game after its steps (a refresh that found
    /// the plan current: `profile::refresh_active`), so it publishes no plan and stages no profile; Err otherwise, and
    /// the activation is settled as [`Staging::abort`] settles it.
    pub fn release(self) -> Result<()> {
        if self.plan.is_some()
            || self
                .record
                .stages
                .iter()
                .any(|s| matches!(s, Stage::Plan { .. } | Stage::Profile { .. }))
        {
            let mut e =
                Error::invalid("an activation that published a plan is committed, not released");
            if let Err(back) = settle(&self.dirs, &self.game) {
                e.message.push_str(&format!(
                    "; and the activation could not be settled: {}",
                    back.message
                ));
            }
            return Err(e);
        }
        remove_record(&self.game)?;
        boundary(Boundary::Released);
        Ok(())
    }
}

// ---- recovery ----

/// Settles an unfinished activation of `game` (none: [`Recovered::Nothing`]), under the lock (it waits while an
/// activation runs). A second call finds nothing more to do and changes nothing.
pub fn recover(dirs: &DataDirs, game: &Path) -> Result<Recovered> {
    if std::fs::symlink_metadata(at(game, JOURNAL)).is_err() {
        // (an activation that holds the lock writes its journal before it changes anything)
        return Ok(Recovered::Nothing);
    }
    let _lock = lock(game)?;
    settle(dirs, game)
}

fn refuse_record(
    game: &Path,
    mut r: Record,
    reason: String,
    notes: Vec<String>,
) -> Result<Recovered> {
    r.state = State::Refused;
    r.refused = Some(reason.clone());
    save_record(game, &r)?;
    boundary(Boundary::RecoverSettled);
    Ok(Recovered::Refused { reason, notes })
}

/// [`recover`], the lock held.
fn settle(dirs: &DataDirs, game: &Path) -> Result<Recovered> {
    let Some(r) = read_record(game)? else {
        return Ok(Recovered::Nothing);
    };
    if r.state == State::Refused {
        return Ok(Recovered::Refused {
            reason: r.refused.clone().unwrap_or_default(),
            notes: Vec::new(),
        });
    }
    let staged_profile = r.stages.iter().rev().find_map(|s| match s {
        Stage::Profile { sha256 } => Some(sha256.clone()),
        _ => None,
    });
    let target = r.mode.file();
    let profile = match file_sha(game, &at(game, target)) {
        Ok(p) => p,
        Err(e) => {
            let why = format!(
                "{target} cannot be read ({}), so Kiln cannot tell which activation it holds",
                e.message
            );
            return refuse_record(game, r, why, Vec::new());
        }
    };
    if staged_profile.is_some() && profile == staged_profile {
        remove_record(game)?;
        boundary(Boundary::RecoverSettled);
        return Ok(Recovered::Completed {
            generation: r.generation,
        });
    }
    if profile != r.prior.profile_sha256 {
        return refuse_record(
            game,
            r,
            format!(
                "{target} was changed by something else while Kiln activated a profile; Kiln leaves it, \
            and the game folder, as they are"
            ),
            Vec::new(),
        );
    }
    // not committed: the earlier state goes back, the newest step first
    let mut notes = Vec::new();
    let mut unproven: Vec<String> = Vec::new();
    for s in r.stages.iter().rev() {
        match s {
            Stage::Profile { .. } | Stage::GameFiles => {}
            Stage::Plan { path, sha256 } => {
                if !plan_rel_ok(path) {
                    unproven.push(format!("the journal names {path:?} as a launch plan"));
                    continue;
                }
                let file = at(&game.join(crate::paths::game::KILN), path);
                match file_sha(game, &file).and_then(|have| Ok((have, pinned(game, path)?))) {
                    Ok((Some(have), false)) if have == *sha256 => {
                        std::fs::remove_file(&file).map_err(io_at(&file, "removing"))?;
                        crate::durable::flush_dir(file.parent().expect("plan folder"))?;
                        boundary(Boundary::RecoverPlanRemoved);
                    }
                    // never published, not the bytes it staged, or pinned by a profile: not this activation's to remove
                    Ok(_) => {}
                    Err(e) => unproven.push(e.message),
                }
                remove_plan_temps(game, path);
            }
            Stage::Placements => {
                match crate::engine_mods::recover_placements(game, &r.route.route()) {
                    Ok(n) => notes.extend(n),
                    Err(e) => {
                        unproven.push(format!(
                            "the copies of ShroudForge-format mods could not be settled ({})",
                            e.message
                        ));
                        continue;
                    }
                }
                match file_sha(game, &at(game, PLACEMENTS)) {
                    Ok(now) if now == r.prior.placements_sha256 => {}
                    Ok(_) => unproven.push("the copies of ShroudForge-format mods (and of the mods placed with them) changed before the \
                        activation stopped, so the earlier launch plan no longer describes them".into()),
                    Err(e) => unproven.push(e.message),
                }
            }
            Stage::Chain { prior, sha256 } => {
                let path = at(game, crate::paths::game::CHAIN);
                let prior_bytes = match prior.as_deref().map(unhex).transpose() {
                    Ok(b) => b,
                    Err(e) => {
                        unproven.push(e);
                        continue;
                    }
                };
                match file_sha(game, &path) {
                    Ok(now) if now.as_deref() == Some(sha256.as_str()) => {
                        let back = match &prior_bytes {
                            Some(bytes) => crate::durable::replace(&path, bytes),
                            None => std::fs::remove_file(&path).map_err(io_at(&path, "removing"))
                                .and_then(|_| crate::durable::flush_dir(path.parent().expect("kiln folder"))),
                        };
                        match back {
                            Ok(()) => boundary(Boundary::RecoverChainRestored),
                            Err(e) => unproven.push(format!("kiln/chain.ini could not be put back ({})", e.message)),
                        }
                    }
                    Ok(now) if now == prior_bytes.as_deref().map(crate::hash::sha256_hex) => {}
                    Ok(_) => unproven.push("kiln/chain.ini was changed by hand since the activation wrote it; Kiln leaves your file as it \
                        is: check it, then activate the profile again".into()),
                    Err(e) => unproven.push(e.message),
                }
            }
        }
    }
    if r.stages.iter().any(|s| matches!(s, Stage::GameFiles)) {
        match restore_game_files(dirs, game, &r.prior.game_files) {
            Ok(n) => notes.extend(n),
            Err(e) => unproven.push(e.message),
        }
    }
    if !unproven.is_empty() {
        let reason = format!(
            "an activation of profile {} did not finish, and Kiln cannot put the earlier one back whole: {}. \
            ShroudForge-format mods do not start until a profile is activated again; every other mod and your own files are as they were",
            r.profile_id,
            unproven.join("; ")
        );
        return refuse_record(game, r, reason, notes);
    }
    remove_record(game)?;
    boundary(Boundary::RecoverSettled);
    Ok(Recovered::RolledBack {
        generation: r.generation,
        notes,
    })
}

/// The earlier packages of game_files back in the game folder, with its own operation (game_files settles its own
/// interrupted changes first). Only while every one of them is still in the Library: a package that is not would be
/// taken out, which is no putting back.
fn restore_game_files(dirs: &DataDirs, game: &Path, prior: &[Package]) -> Result<Vec<String>> {
    if packages(crate::game_files::placed(game)?) == prior {
        return Ok(Vec::new());
    }
    let mods: Vec<ProfileMod> = prior
        .iter()
        .map(|p| ProfileMod {
            id: p.id.clone(),
            version: p.version.clone(),
            hash: p.hash.clone(),
            enabled: true,
        })
        .collect();
    if let Some(m) = mods
        .iter()
        .find(|m| crate::game_files::plan_of(dirs, m).is_none())
    {
        return Err(Error::new(
            ErrorCode::Refused,
            format!(
                "native:{} is not in the Library any more, so its earlier files in the game \
            folder cannot be put back",
                m.id
            ),
        ));
    }
    let rep = crate::game_files::place_mods(dirs, &mods, game)?;
    if packages(crate::game_files::placed(game)?) != prior {
        return Err(Error::new(
            ErrorCode::Refused,
            format!(
                "the earlier files in the game folder could not all be put back ({})",
                rep.notes.join("; ")
            ),
        ));
    }
    boundary(Boundary::RecoverGameFilesRestored);
    Ok(rep.notes)
}

/// The temporary files a stop inside the publication of the plan `rel` (`launch-plans/<name>`) left beside it:
/// `sfi_plan::write_plan`'s `.<name>.<16 hex digits>.kiln-tmp` (created new, under a name only this activation's plan
/// id makes), plain files only, never a link. Their bytes are no plan anything reads; leaving one is harmless, so this
/// never refuses a recovery.
fn remove_plan_temps(game: &Path, rel: &str) {
    let folder = at(&game.join(crate::paths::game::KILN), PLAN_FOLDER);
    let Some(name) = rel.strip_prefix(&format!("{PLAN_FOLDER}/")) else {
        return;
    };
    if refuse_symlink_path(game, &folder).is_err() {
        return;
    }
    let Ok(entries) = std::fs::read_dir(&folder) else {
        return;
    };
    let mut removed = false;
    for e in entries.flatten() {
        let file_name = e.file_name();
        let ours = file_name
            .to_str()
            .and_then(|n| n.strip_prefix('.'))
            .and_then(|n| n.strip_prefix(name))
            .and_then(|n| n.strip_prefix('.'))
            .and_then(|n| n.strip_suffix(".kiln-tmp"))
            .is_some_and(|hex| {
                hex.len() == 16 && hex.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
            });
        if ours
            && e.file_type().is_ok_and(|t| t.is_file())
            && std::fs::remove_file(e.path()).is_ok()
        {
            removed = true;
        }
    }
    if removed {
        let _ = crate::durable::flush_dir(&folder);
    }
}

fn unhex(s: &str) -> std::result::Result<Vec<u8>, String> {
    let damaged = || "the journal's copy of kiln/chain.ini is damaged".to_string();
    if !s.len().is_multiple_of(2) {
        return Err(damaged());
    }
    (0..s.len())
        .step_by(2)
        .map(|i| {
            s.get(i..i + 2)
                .and_then(|h| u8::from_str_radix(h, 16).ok())
                .ok_or_else(damaged)
        })
        .collect()
}

/// After an activation that did not use this journal (main's routes) committed: a refused journal is superseded by it
/// and goes (true). A journal of an unfinished activation stays: [`recover`] it first.
pub fn supersede_refused(game: &Path) -> Result<bool> {
    if std::fs::symlink_metadata(at(game, JOURNAL)).is_err() {
        return Ok(false);
    }
    let _lock = lock(game)?;
    match read_record(game) {
        Ok(Some(r)) if r.state == State::Refused => {}
        Ok(_) => return Ok(false),
        Err(e) if e.code == ErrorCode::Schema || e.code == ErrorCode::Parse => {}
        Err(e) => return Err(e),
    }
    remove_record(game)?;
    Ok(true)
}

/// Removes the launch plans of `game` that neither kiln/profile.json nor kiln/profile.once.json pins any more (an
/// earlier activation's, a play-once start's after the next activation, an orphan a stop left): the plan files
/// `launch-plans/<name>.json`, plain files only, never through a link. Their names. Under the lock, so a plan an
/// activation has published and not yet pinned is never taken; nothing is removed while an activation is unfinished or
/// refused (its journal is there), or while a profile file cannot be read as JSON. A game with no launch-plans folder is
/// not touched at all (no lock file either).
pub fn prune_plans(game: &Path) -> Result<Vec<String>> {
    let folder = at(&game.join(crate::paths::game::KILN), PLAN_FOLDER);
    if !std::fs::symlink_metadata(&folder).is_ok_and(|m| m.is_dir()) {
        return Ok(Vec::new());
    }
    let _lock = lock(game)?;
    if std::fs::symlink_metadata(at(game, JOURNAL)).is_ok() {
        return Ok(Vec::new());
    }
    refuse_symlink_path(game, &folder)?;
    let mut removed = Vec::new();
    for e in std::fs::read_dir(&folder).map_err(io_at(&folder, "listing"))? {
        let e = e.map_err(io_at(&folder, "listing"))?;
        let Some(name) = e.file_name().to_str().map(str::to_owned) else {
            continue;
        };
        let rel = format!("{PLAN_FOLDER}/{name}");
        if !plan_rel_ok(&rel) || !e.file_type().is_ok_and(|t| t.is_file()) || pinned(game, &rel)? {
            continue;
        }
        let path = e.path();
        std::fs::remove_file(&path).map_err(io_at(&path, "removing"))?;
        removed.push(rel);
    }
    if !removed.is_empty() {
        crate::durable::flush_dir(&folder)?;
    }
    removed.sort();
    Ok(removed)
}

// ---- the planned start ----

/// Why the planned start of ShroudForge-format mods in `game` must not happen now (None: it may), for the launcher's
/// check before a Steam or a direct start. Only that family is refused; every other mod starts as it would.
///
/// - an activation of the game is unfinished or refused (its journal is there, or cannot be read);
/// - kiln/profile.json pins a launch plan (`sfLaunchPlan`) that is not there as a plain file, whose SHA-256 is not the
///   pinned one, or whose `profile` is not this profile's id, generation and semantic hash.
pub fn start_check(game: &Path) -> Option<String> {
    start_check_mode(game, Mode::Active)
}

/// [`start_check`] for a start of `mode`: the active profile's (kiln/profile.json) or a play-once start's
/// (kiln/profile.once.json, whose plan names it with `profile.mode` `once`).
pub fn start_check_mode(game: &Path, mode: Mode) -> Option<String> {
    let target = Target::of(mode);
    let stale = |why: &str| {
        Some(format!(
            "ShroudForge-format mods do not start: {why}; activate the profile again. Every other mod \
        starts as it is"
        ))
    };
    match read_record(game) {
        Ok(None) => {}
        Ok(Some(r)) if r.state == State::Refused => {
            return stale(
                r.refused
                    .as_deref()
                    .unwrap_or("an earlier activation did not finish"),
            );
        }
        Ok(Some(_)) => return stale("an activation of this game did not finish"),
        Err(e) => {
            return stale(&format!(
                "the activation journal cannot be read ({})",
                e.message
            ));
        }
    }
    let bytes = match read_bytes(game, &at(game, target.file())) {
        Ok(Some(b)) => b,
        Ok(None) => return None,
        Err(e) => return stale(&e.message),
    };
    let Ok(v) = serde_json::from_slice::<Value>(&bytes) else {
        return stale(&format!("{} is not JSON", target.file()));
    };
    let pin = v.get("sfLaunchPlan")?;
    let (Some(rel), Some(sha)) = (pin["path"].as_str(), pin["sha256"].as_str()) else {
        return stale("its launch plan reference is incomplete");
    };
    if !plan_rel_ok(rel) {
        return stale("its launch plan reference names no plan file");
    }
    let file = at(&game.join(crate::paths::game::KILN), rel);
    match file_sha(game, &file) {
        Ok(Some(have)) if have == sha => {}
        Ok(Some(_)) => {
            return stale(&format!(
                "its launch plan kiln/{rel} changed since the activation"
            ));
        }
        Ok(None) => return stale(&format!("its launch plan kiln/{rel} is gone")),
        Err(e) => return stale(&e.message),
    }
    let named = match read_bytes(game, &file)
        .map_err(|e| e.message)
        .and_then(|b| PlanProfile::of_plan(&b.unwrap_or_default()))
    {
        Ok(n) => n,
        Err(e) => return stale(&e),
    };
    match ProfileIdentity::of(&v, mode) {
        Ok(ident) if PlanProfile::of_identity(&ident) == named => None,
        Ok(_) => stale(&format!(
            "its launch plan kiln/{rel} was made for another state of the profile"
        )),
        Err(e) => stale(&e),
    }
}
