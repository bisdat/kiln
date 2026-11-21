// SPDX-License-Identifier: MIT
//! The record behind `<game>/kiln/state/sfi.json` (schema/sfi/sfi-state.schema.json, format 1) and the rules every
//! change to it keeps. Pure: no IO, no clock, no thread; [`super::StateHub`] owns the one instance and the writes.
//!
//! The rules:
//! - **Monotonic phase.** starting → running → ended; starting → ended or refused. aside, refused and ended are final:
//!   the record is sealed and every later event is ignored, so nothing after the end can clear a loss.
//! - **Committed data stays present.** A package whose data is `committed` or `cached-verified` keeps it and
//!   `archivePresent: true`, whatever its runtime does later.
//! - **Unknown stays null.** `archivePresent` is `false` only when a producer proved the data absent; `uncertain` data
//!   and every unproven case is `null`, and a warning about it says so (presence `unknown`), never "not applied".
//! - **A loss is never cleared.** The first skip of a package stays; a later skip, a late `lua: active` or a stale
//!   event changes nothing. `warningRevision` only rises.
//! - **Safety reserve.** The schema bounds the rows to 4096. A row that carries a warning evicts a plain row when the
//!   table is full; plain rows are what gets left out, never a warning.
//! - **Every value fits the schema.** Ids that are not ShroudForge identifiers are refused (counted), texts are cut at a
//!   character boundary to their bounds, chains and DLL lists to 64 entries.

use super::writer::PreviouslyActive;
use serde_json::{Map, Value, json};

pub const FORMAT: u64 = 1;
/// The schema's bounds (sfi-state.schema.json).
pub const MAX_PACKAGES: usize = 4096;
pub const MAX_DLLS: usize = 64;
pub const MAX_CHAIN: usize = 64;
const TEXT_MAX: usize = 4096;
const WARNING_MAX: usize = 8192;
const NAME_MAX: usize = 200;
const VERSION_MAX: usize = 64;
const MENU_KEY_MAX: usize = 64;

/// The sentence a warning about a save-sensitive package that is off ends with (sfi PLAN §4); a package that is not
/// save-sensitive is safe to save without, so its warning does not say it.
pub const DO_NOT_SAVE: &str = "Do not save while this mod is off; restore it and restart first.";
/// A late loss whose startup data is still in the game (sfi PLAN §4).
pub const LATE_LOSS: &str = "Its startup data is still applied, but its runtime stopped; it cannot be undone until restart. Do not save.";
const SAVE_SENSITIVE: &str =
    "Saving while it is off can lose items or other saved data that depend on it.";

macro_rules! words {
    ($(#[$meta:meta])* $name:ident { $($variant:ident = $word:literal),+ $(,)? }) => {
        $(#[$meta])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        pub enum $name { $($variant),+ }
        impl $name {
            /// The schema's word.
            pub fn word(self) -> &'static str {
                match self { $(Self::$variant => $word),+ }
            }
        }
    };
}

words!(
    /// `phase`.
    Phase { Starting = "starting", Running = "running", Ended = "ended", Aside = "aside", Refused = "refused" }
);
impl Phase {
    /// starting and running are a start still going on: `incomplete` true.
    pub fn incomplete(self) -> bool {
        matches!(self, Self::Starting | Self::Running)
    }
}
words!(
    /// `launch.backend.selected`.
    Backend { Hosted = "hosted", Installed = "installed", Kiln = "kiln", None = "none" }
);
words!(
    /// `launch.side`.
    Side { Client = "client", Server = "server" }
);
words!(
    /// `previousStart.disposition`.
    Disposition { None = "none", Clean = "clean", Incomplete = "incomplete", Suspect = "suspect" }
);
words!(
    /// `packages[].intent`.
    Intent { Selected = "selected", Disabled = "disabled", OtherSide = "other-side", Absent = "absent" }
);
words!(
    /// `packages[].data`.
    Data {
        NotNeeded = "not-needed", Staged = "staged", Prepared = "prepared", Committed = "committed",
        CachedVerified = "cached-verified", VerifiedRestored = "verified-restored", Excluded = "excluded",
        Failed = "failed", Uncertain = "uncertain",
    }
);
impl Data {
    /// The data is in the game archive for good this process (the schema: archivePresent true).
    pub fn present(self) -> bool {
        matches!(self, Self::Committed | Self::CachedVerified)
    }
}
words!(
    /// `packages[].lua`.
    Lua {
        NotNeeded = "not-needed", Admitted = "admitted", Ready = "ready", Active = "active", Failed = "failed",
        Excluded = "excluded", Stopped = "stopped",
    }
);
words!(
    /// `packages[].dlls[].state`.
    DllState {
        Admitted = "admitted", Loaded = "loaded", NotLoaded = "not-loaded", Refused = "refused", Failed = "failed",
        Retained = "retained", ForeignOwner = "foreign-owner",
    }
);
words!(
    /// `skip.part`.
    Part { Package = "package", Data = "data", Lua = "lua", Dll = "dll" }
);
words!(
    /// `saveSensitive.basis`.
    Basis { Policy = "policy", UnknownMixed = "unknown-mixed", None = "none" }
);
words!(
    /// `warning.trigger`.
    Trigger { Skipped = "skipped", SaveSensitiveOff = "save-sensitive-off" }
);
words!(
    /// `warning.presence`.
    Presence { NotApplied = "not-applied", DataAppliedRuntimeStopped = "data-applied-runtime-stopped", Unknown = "unknown" }
);
words!(
    /// `menu.by`.
    MenuBy { Kiln = "kiln", Off = "off", Unavailable = "unavailable" }
);

/// A stable machine word (`code` in the schema); anything else is `unknown`.
pub fn code(word: &str) -> String {
    let ok = !word.is_empty()
        && word.len() <= 64
        && word
            .bytes()
            .next()
            .is_some_and(|b| b.is_ascii_lowercase() || b.is_ascii_digit())
        && word
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'.' || b == b'-');
    if ok {
        word.to_owned()
    } else {
        "unknown".to_owned()
    }
}

/// A ShroudForge identifier (SF 1.7.9 `valid_identifier`): 1-80 of `[A-Za-z0-9._-]`.
pub fn sf_id(id: &str) -> bool {
    (1..=80).contains(&id.len())
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'_' || b == b'-')
}

/// `text` cut to `max` characters at a character boundary (the schema counts code points).
fn clamp(text: &str, max: usize) -> String {
    match text.char_indices().nth(max) {
        Some((at, _)) => text[..at].to_owned(),
        None => text.to_owned(),
    }
}
/// A required text (minLength 1): an empty one says so.
fn required(text: &str, max: usize) -> String {
    let text = clamp(text.trim(), max);
    if text.is_empty() {
        "no reason was given".to_owned()
    } else {
        text
    }
}
fn optional(text: Option<&str>, max: usize) -> Option<String> {
    text.map(|t| clamp(t, max)).filter(|t| !t.is_empty())
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
/// kiln.launch's u64 as launch.json writes it: 16 lower-case hex digits.
pub fn launch_id(id: u64) -> String {
    format!("{id:016x}")
}

/// `launch`: who this start is, copied from the core's frozen kiln.launch record.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Identity {
    /// kiln.launch's launch id; `None` when the core has no kiln.launch (then the backend is `none`).
    pub launch_id: Option<u64>,
    /// The frozen plan's SHA-256; `None` without a plan.
    pub plan_hash: Option<[u8; 32]>,
    pub pid: u32,
    pub process_start_ms: u64,
    pub side: Side,
    /// The booted profile's id and generation; `None` without one.
    pub profile: Option<(String, u64)>,
    pub backend: Backend,
    /// The record's reason as a word (`code`), e.g. `plan`, `hosted-default`, `no-kiln-launch`.
    pub reason_code: String,
    pub reason_text: Option<String>,
    /// kiln.launch's freeze stamp (ms since 1970); `None` without kiln.launch.
    pub frozen_at_ms: Option<u64>,
}
impl Identity {
    /// No kiln.launch record (an older core, or one that cannot be read): source `absent`.
    pub fn absent(&self) -> bool {
        self.launch_id.is_none()
    }
    fn value(&self) -> Value {
        let absent = self.absent();
        let profile = match &self.profile {
            Some((id, generation)) if profile_id(id) => {
                json!({ "id": id, "generation": generation })
            }
            _ => Value::Null,
        };
        json!({
            "launchId": self.launch_id.map(launch_id),
            "planHash": self.plan_hash.filter(|h| !absent && h.iter().any(|&b| b != 0)).map(|h| hex(&h)),
            "pid": self.pid,
            "processStartMs": self.process_start_ms,
            "side": self.side.word(),
            "profile": profile,
            "backend": {
                "selected": if absent { Backend::None.word() } else { self.backend.word() },
                "source": if absent { "absent" } else { "kiln.launch" },
                "reasonCode": code(&self.reason_code),
                "text": optional(self.reason_text.as_deref(), TEXT_MAX),
            },
            "frozenAtMs": if absent { None } else { self.frozen_at_ms },
        })
    }
}
/// common.schema.json `profileId`.
fn profile_id(id: &str) -> bool {
    (1..=48).contains(&id.len())
        && id
            .bytes()
            .next()
            .is_some_and(|b| b.is_ascii_lowercase() || b.is_ascii_digit())
        && id
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
}

/// `previousStart`: what the file this start replaced said.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PreviousStart {
    pub disposition: Disposition,
    pub launch_id: Option<u64>,
    pub pid: Option<u32>,
}
impl PreviousStart {
    pub const NONE: Self = Self {
        disposition: Disposition::None,
        launch_id: None,
        pid: None,
    };
    fn value(&self) -> Value {
        let none = self.disposition == Disposition::None;
        json!({
            "disposition": self.disposition.word(),
            "launchId": if none { None } else { self.launch_id.map(launch_id) },
            "pid": if none { None } else { self.pid },
        })
    }
}

/// `reason` of an aside or refused record.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Reason {
    pub code: String,
    pub text: String,
}
impl Reason {
    pub fn new(code: &str, text: impl Into<String>) -> Self {
        Self {
            code: self::code(code),
            text: required(&text.into(), TEXT_MAX),
        }
    }
}

/// `saveSensitive`, plus the policy's own line for the warning text (not written as a field).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SaveSensitive {
    pub value: bool,
    pub basis: Basis,
    pub policy_row: Option<(String, String)>,
    /// The policy row's `text` (e.g. Quiver & Spellbook's ammo line), added to every warning about the package.
    pub line: Option<String>,
}
impl SaveSensitive {
    pub fn none() -> Self {
        Self {
            value: false,
            basis: Basis::None,
            policy_row: None,
            line: None,
        }
    }
    pub fn policy(id: &str, version: &str, line: Option<&str>) -> Self {
        Self {
            value: true,
            basis: Basis::Policy,
            policy_row: Some((id.to_owned(), version.to_owned())),
            line: line.map(str::to_owned),
        }
    }
    pub fn unknown_mixed() -> Self {
        Self {
            value: true,
            basis: Basis::UnknownMixed,
            policy_row: None,
            line: None,
        }
    }
    /// The schema's rules: policy needs a valid row and means true; the others carry no row.
    fn normalised(mut self) -> Self {
        match self.basis {
            Basis::Policy => match &self.policy_row {
                Some((id, version))
                    if sf_id(id) && (1..=VERSION_MAX).contains(&version.chars().count()) =>
                {
                    self.value = true
                }
                // a policy verdict without a usable row is still save-sensitive: say it conservatively
                _ => {
                    return Self {
                        line: self.line,
                        ..Self::unknown_mixed()
                    };
                }
            },
            Basis::UnknownMixed => {
                self.value = true;
                self.policy_row = None;
            }
            Basis::None => {
                self.value = false;
                self.policy_row = None;
            }
        }
        self
    }
    fn value_json(&self) -> Value {
        json!({
            "value": self.value,
            "basis": self.basis.word(),
            "policyRow": self.policy_row.as_ref().map(|(id, version)| json!({ "id": id, "version": version })),
        })
    }
}

/// `dlls[]`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Dll {
    pub name: String,
    pub sha256: Option<[u8; 32]>,
    pub state: DllState,
}
/// common.schema.json `dllName`.
fn dll_name(name: &str) -> bool {
    name.len() > 4
        && name[name.len() - 4..].eq_ignore_ascii_case(".dll")
        && !name.contains(['/', '\\', ':'])
}

/// `skip`: why a selected package does not run whole this start.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Skip {
    pub code: String,
    pub text: String,
    pub part: Part,
    /// The dependency chain, nearest first.
    pub chain: Vec<String>,
    /// Lost after it started (runtime error, watchdog, interpreter stop).
    pub late: bool,
    /// Another loader that still runs part of it (`xhl`).
    pub loader: Option<String>,
}
impl Skip {
    pub fn new(code: &str, part: Part, text: impl Into<String>) -> Self {
        Self {
            code: code.to_owned(),
            text: text.into(),
            part,
            chain: Vec::new(),
            late: false,
            loader: None,
        }
    }
    fn normalised(self) -> Self {
        let loader = self.loader.filter(|l| {
            (1..=32).contains(&l.len())
                && l.bytes()
                    .next()
                    .is_some_and(|b| b.is_ascii_lowercase() || b.is_ascii_digit())
                && l.bytes()
                    .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'.' || b == b'-')
        });
        Self {
            code: code(&self.code),
            text: required(&self.text, TEXT_MAX),
            part: self.part,
            chain: self
                .chain
                .into_iter()
                .filter(|id| sf_id(id))
                .take(MAX_CHAIN)
                .collect(),
            late: self.late,
            loader,
        }
    }
    fn value(&self) -> Value {
        json!({
            "code": self.code,
            "text": self.text,
            "part": self.part.word(),
            "chain": self.chain,
            "late": self.late,
            "loader": self.loader,
        })
    }
}

/// `warning`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Warning {
    pub revision: u64,
    pub trigger: Trigger,
    pub presence: Presence,
    pub save_sensitive: bool,
    pub text: String,
}

/// One `packages[]` row.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Package {
    pub id: String,
    pub version: Option<String>,
    pub name: Option<String>,
    pub intent: Intent,
    pub data: Data,
    pub lua: Lua,
    pub dlls: Vec<Dll>,
    pub archive_present: Option<bool>,
    pub runtime_active: Option<bool>,
    pub save_sensitive: SaveSensitive,
    pub skip: Option<Skip>,
    pub warning: Option<Warning>,
    /// `ownerEpoch`, the epoch of the package's live runtime generation (the UI envelope's `ownerEpoch`); `None`
    /// (absent from the file) while it has no live runtime.
    pub owner_epoch: Option<u64>,
    /// A save-sensitive package active at the previous start that is off or absent now.
    off_since_previous: bool,
}

/// A row as the launch plan describes it, before anything happened to it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PackageInit {
    pub id: String,
    pub version: Option<String>,
    pub name: Option<String>,
    pub intent: Intent,
    pub data: Data,
    pub lua: Lua,
    /// `None` unless a producer proved where its data is.
    pub archive_present: Option<bool>,
    pub save_sensitive: SaveSensitive,
}
impl PackageInit {
    pub fn selected(id: &str, version: Option<&str>, name: Option<&str>) -> Self {
        Self {
            id: id.to_owned(),
            version: version.map(str::to_owned),
            name: name.map(str::to_owned),
            intent: Intent::Selected,
            data: Data::NotNeeded,
            lua: Lua::NotNeeded,
            archive_present: None,
            save_sensitive: SaveSensitive::none(),
        }
    }
}

/// A save-sensitive package that was active at the previous start, and what it is now.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TurnedOff {
    pub id: String,
    pub version: Option<String>,
    pub name: Option<String>,
    /// `Disabled` (listed, switched off) or `Absent` (gone from the profile); anything else is not a turn-off.
    pub now: Intent,
    pub save_sensitive: SaveSensitive,
    /// Proven absent from the game archive: `Some(false)`; unknown: `None`.
    pub archive_present: Option<bool>,
}

/// One package of a ShroudForge startup pass's record (the retry record; the startup pass makes it final and serves it as
/// kiln.eml.startup's `startup_outcome`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OutcomePackage {
    pub id: String,
    pub version: Option<String>,
    pub name: Option<String>,
    /// The fork's text for this package (its error, or "needs X, which failed in the pregame pass").
    pub text: String,
    /// The dependency chain for a package left out with the failing one, nearest first; empty means "the failing
    /// package" alone.
    pub chain: Vec<String>,
    /// The committed generation proves its data absent from the archive (else unknown).
    pub absent_proven: bool,
}

/// The startup pass's skips: the package the first pass failed on, the packages left out with it,
/// and, when the retry failed too, both errors and every ShroudForge package of the pass.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StartupOutcome {
    pub failed: Option<OutcomePackage>,
    pub dependents: Vec<OutcomePackage>,
    /// The first attempt's error (the record's `error`).
    pub first_error: Option<String>,
    /// The retry's error (the record's `retryError`): the retry failed too.
    pub retry_error: Option<String>,
    /// Every ShroudForge package of the pass.
    pub packages: Vec<OutcomePackage>,
}

/// One row of kiln-eml's startup outcome (startup-outcome.schema.json v1 `packages[]`), as kiln-sfi read and
/// checked it: the package's data (sfi-state's words), whether that data is in the live generation (`None`: unknown),
/// and for a package whose data is not there the outcome's code, text and cause chain (nearest first).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StartupRow {
    pub id: String,
    pub version: Option<String>,
    /// `Committed`, `CachedVerified`, `Failed` or `Excluded`.
    pub data: Data,
    pub archive_present: Option<bool>,
    pub code: Option<String>,
    pub text: Option<String>,
    pub chain: Vec<String>,
}

/// The skip part of a startup outcome's code: a package dropped with another is skipped whole, one the native terminal
/// withdrew for its DLL, any other for its startup data.
pub fn startup_part(code: &str) -> Part {
    match code {
        "dependency-skipped" => Part::Package,
        "native-refused" => Part::Dll,
        _ => Part::Data,
    }
}

/// What a change did, for the writer: nothing, only memory, a warning (written soon), a fact a reader acts on at once
/// (written now, like a phase), or a phase (written now).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Change {
    None,
    Quiet,
    Warning,
    /// Not a phase change, but written at once as one is (an owner epoch, which Kiln.Ui stamps into UI actions).
    Urgent,
    Phase,
}

/// The whole record of this start.
#[derive(Clone, Debug)]
pub struct Reducer {
    identity: Identity,
    kiln_sfi: &'static str,
    phase: Phase,
    reason: Option<Reason>,
    previous: PreviousStart,
    started_ms: u64,
    menu: Option<(MenuBy, Option<String>)>,
    warning_revision: u64,
    packages: Vec<Package>,
    /// Rows the schema's bound or a malformed id left out (logged by the hub).
    dropped: u64,
    /// The previous start's active save-sensitive packages, written as `carriedSaveSensitive` on aside and refused:
    /// this start runs none of them, so the next kiln start must still compare against them.
    carried: Vec<PreviouslyActive>,
    /// Raised by every change: a later snapshot always has a higher number.
    seq: u64,
}

impl Reducer {
    /// The first record of this start: `starting`, `aside` or `refused` (any other phase starts as `starting`).
    pub fn new(
        identity: Identity,
        previous: PreviousStart,
        started_ms: u64,
        phase: Phase,
        reason: Option<Reason>,
    ) -> Self {
        let phase = match phase {
            Phase::Aside | Phase::Refused => phase,
            _ => Phase::Starting,
        };
        let mut record = Self {
            identity,
            kiln_sfi: env!("CARGO_PKG_VERSION"),
            phase,
            reason: None,
            previous,
            started_ms,
            menu: None,
            warning_revision: 0,
            packages: Vec::new(),
            dropped: 0,
            carried: Vec::new(),
            seq: 1,
        };
        if !phase.incomplete() {
            record.reason = Some(reason.unwrap_or_else(|| Reason::new("unknown", "")));
        }
        record
    }

    /// The previous start's active save-sensitive list, to carry forward if this record ends aside or refused
    /// (bounded and clamped to the schema here). Set once, when the record is made: not a change.
    pub fn with_carried(mut self, previous: &[PreviouslyActive]) -> Self {
        self.carried = carried(previous.iter().cloned());
        self
    }
    /// What an aside or refused record writes as `carriedSaveSensitive`.
    pub fn carried(&self) -> &[PreviouslyActive] {
        &self.carried
    }

    pub fn phase(&self) -> Phase {
        self.phase
    }
    /// aside, refused and ended: nothing changes any more.
    pub fn sealed(&self) -> bool {
        !self.phase.incomplete()
    }
    pub fn seq(&self) -> u64 {
        self.seq
    }
    pub fn warning_revision(&self) -> u64 {
        self.warning_revision
    }
    pub fn dropped(&self) -> u64 {
        self.dropped
    }
    pub fn identity(&self) -> &Identity {
        &self.identity
    }
    pub fn previous(&self) -> &PreviousStart {
        &self.previous
    }
    pub fn packages(&self) -> &[Package] {
        &self.packages
    }
    pub fn package(&self, id: &str) -> Option<&Package> {
        self.packages.iter().find(|p| p.id == id)
    }

    fn changed(&mut self, change: Change) -> Change {
        if change != Change::None {
            self.seq += 1;
        }
        change
    }

    /// The phase moves forward only: starting → running → ended; starting → ended or refused. `reason` is required for
    /// refused (a missing one is written as unknown) and ignored otherwise.
    pub fn set_phase(&mut self, to: Phase, reason: Option<Reason>) -> Change {
        let allowed = matches!(
            (self.phase, to),
            (
                Phase::Starting,
                Phase::Running | Phase::Ended | Phase::Refused
            ) | (Phase::Running, Phase::Ended)
        );
        if !allowed {
            return Change::None;
        }
        self.phase = to;
        if !to.incomplete() {
            // no live runtime after the end: no owner epoch is current
            for row in &mut self.packages {
                row.owner_epoch = None;
            }
        }
        if to == Phase::Refused {
            self.reason = Some(reason.unwrap_or_else(|| Reason::new("unknown", "")));
        }
        self.changed(Change::Phase)
    }

    /// `menu`: who answers ShroudForge's mod-menu toggle this start.
    pub fn set_menu(&mut self, by: MenuBy, key: Option<&str>) -> Change {
        if self.sealed() {
            return Change::None;
        }
        let menu = (by, optional(key, MENU_KEY_MAX));
        if self.menu.as_ref() == Some(&menu) {
            return Change::None;
        }
        self.menu = Some(menu);
        self.changed(Change::Quiet)
    }

    /// Room for one more row. A row that will carry a warning may evict the last plain row (no skip, no warning);
    /// a plain row never evicts anything. False: the row is left out (counted).
    fn room(&mut self, warns: bool) -> bool {
        if self.packages.len() < MAX_PACKAGES {
            return true;
        }
        if warns
            && let Some(at) = self
                .packages
                .iter()
                .rposition(|p| p.skip.is_none() && p.warning.is_none() && !p.off_since_previous)
        {
            self.packages.remove(at);
            self.dropped += 1;
            return true;
        }
        self.dropped += 1;
        false
    }

    fn insert(&mut self, init: PackageInit, warns: bool) -> Option<usize> {
        if !sf_id(&init.id) {
            self.dropped += 1;
            return None;
        }
        if let Some(at) = self.packages.iter().position(|p| p.id == init.id) {
            return Some(at);
        }
        if !self.room(warns) {
            return None;
        }
        let archive_present = if init.data.present() {
            Some(true)
        } else if init.data == Data::Uncertain {
            None
        } else {
            init.archive_present
        };
        self.packages.push(Package {
            version: optional(init.version.as_deref(), VERSION_MAX),
            name: optional(init.name.as_deref(), NAME_MAX),
            id: init.id,
            intent: init.intent,
            data: init.data,
            lua: init.lua,
            dlls: Vec::new(),
            archive_present,
            runtime_active: Some(init.lua == Lua::Active),
            save_sensitive: init.save_sensitive.normalised(),
            skip: None,
            warning: None,
            owner_epoch: None,
            off_since_previous: false,
        });
        Some(self.packages.len() - 1)
    }

    /// A row of the launch plan. A second row with the same id changes nothing.
    pub fn add_package(&mut self, init: PackageInit) -> Change {
        if self.sealed() {
            return Change::None;
        }
        let before = self.packages.len();
        match self.insert(init, false) {
            Some(_) if self.packages.len() != before => self.changed(Change::Quiet),
            _ => Change::None,
        }
    }

    /// The index of `id`, creating a selected row for a producer's report about a package the table does not have
    /// (left out by the bound, or reported before the plan): a skip is never lost for want of a row.
    fn row_for_report(&mut self, id: &str) -> Option<usize> {
        if let Some(at) = self.packages.iter().position(|p| p.id == id) {
            return Some(at);
        }
        let mut init = PackageInit::selected(id, None, None);
        init.data = Data::Uncertain;
        init.lua = Lua::Excluded;
        self.insert(init, true)
    }

    /// `data` of a package, with `archive_present` (`None` unless proven). Committed data never changes again.
    pub fn set_data(&mut self, id: &str, data: Data, archive_present: Option<bool>) -> Change {
        if self.sealed() {
            return Change::None;
        }
        let Some(at) = self.packages.iter().position(|p| p.id == id) else {
            return Change::None;
        };
        let row = &mut self.packages[at];
        if row.data.present() {
            return Change::None;
        }
        let present = if data.present() {
            Some(true)
        } else if data == Data::Uncertain {
            None
        } else {
            archive_present
        };
        if (row.data, row.archive_present) == (data, present) {
            return Change::None;
        }
        row.data = data;
        row.archive_present = present;
        let change = self.refresh(at).max(Change::Quiet);
        self.changed(change)
    }

    /// `lua` of a package. A package with a skip never becomes active again.
    pub fn set_lua(&mut self, id: &str, lua: Lua) -> Change {
        if self.sealed() {
            return Change::None;
        }
        let Some(at) = self.packages.iter().position(|p| p.id == id) else {
            return Change::None;
        };
        let row = &mut self.packages[at];
        if row.lua == lua || (lua == Lua::Active && row.skip.is_some()) {
            return Change::None;
        }
        row.lua = lua;
        row.runtime_active = Some(lua == Lua::Active);
        if lua != Lua::Active {
            row.owner_epoch = None;
        }
        let change = self.refresh(at).max(Change::Quiet);
        self.changed(change)
    }

    /// The `ownerEpoch` of a package's live runtime generation (`None`: it has none now). Kiln.Ui stamps it into the
    /// package's UI actions, and the runtime drops an action of another epoch. Only a row whose runtime is active holds
    /// one (an epoch for another row, or 0, changes nothing); a skip, a non-active lua and the end clear it. A change is
    /// written at once ([`Change::Urgent`]), not after [`RUNNING_GAP`](super::RUNNING_GAP): until Kiln.Ui reads a reload's
    /// new epoch, every action it stamps with the old one is dropped unread.
    pub fn set_owner_epoch(&mut self, id: &str, epoch: Option<u64>) -> Change {
        if self.sealed() {
            return Change::None;
        }
        let Some(row) = self.packages.iter_mut().find(|p| p.id == id) else {
            return Change::None;
        };
        if epoch == Some(0) || (epoch.is_some() && row.runtime_active != Some(true)) {
            return Change::None;
        }
        if row.owner_epoch == epoch {
            return Change::None;
        }
        row.owner_epoch = epoch;
        self.changed(Change::Urgent)
    }

    /// One DLL of a package (by name, ASCII case ignored, as on Windows).
    pub fn set_dll(&mut self, id: &str, dll: Dll) -> Change {
        if self.sealed() || !dll_name(&dll.name) {
            return Change::None;
        }
        let Some(row) = self.packages.iter_mut().find(|p| p.id == id) else {
            return Change::None;
        };
        match row
            .dlls
            .iter()
            .position(|d| d.name.eq_ignore_ascii_case(&dll.name))
        {
            Some(at) if row.dlls[at] == dll => return Change::None,
            Some(at) => row.dlls[at] = dll,
            None if row.dlls.len() < MAX_DLLS => row.dlls.push(dll),
            None => return Change::None,
        }
        self.changed(Change::Quiet)
    }

    /// A selected package does not run whole: the first skip stays for good (a later one changes nothing). A skip
    /// stops its runtime (an active Lua becomes `stopped`). Disabled, other-side and absent rows are never skipped.
    pub fn skip(&mut self, id: &str, skip: Skip) -> Change {
        if self.sealed() || !sf_id(id) {
            return Change::None;
        }
        let Some(at) = self.row_for_report(id) else {
            return Change::None;
        };
        let row = &mut self.packages[at];
        if row.intent != Intent::Selected || row.skip.is_some() {
            return Change::None;
        }
        if row.lua == Lua::Active {
            row.lua = Lua::Stopped;
        }
        row.runtime_active = Some(false);
        row.owner_epoch = None;
        row.skip = Some(skip.normalised());
        let change = self.refresh(at).max(Change::Quiet);
        self.changed(change)
    }

    /// A late loss: the package's runtime stopped after it started (runtime error, watchdog, interpreter stop).
    pub fn late_loss(&mut self, id: &str, code: &str, text: impl Into<String>) -> Change {
        let mut skip = Skip::new(code, Part::Lua, text);
        skip.late = true;
        self.skip(id, skip)
    }

    /// A save-sensitive package active at the previous start that is disabled or absent now (warning trigger 2).
    pub fn turned_off(&mut self, off: TurnedOff) -> Change {
        if self.sealed() || !matches!(off.now, Intent::Disabled | Intent::Absent) {
            return Change::None;
        }
        let save_sensitive = off.save_sensitive.normalised();
        if !save_sensitive.value {
            return Change::None;
        }
        let existing = self.packages.iter().position(|p| p.id == off.id);
        let at = match existing {
            Some(at) if self.packages[at].intent == off.now => at,
            Some(_) => return Change::None,
            None => {
                let init = PackageInit {
                    id: off.id,
                    version: off.version,
                    name: off.name,
                    intent: off.now,
                    data: Data::NotNeeded,
                    lua: Lua::NotNeeded,
                    archive_present: off.archive_present,
                    save_sensitive: save_sensitive.clone(),
                };
                match self.insert(init, true) {
                    Some(at) => at,
                    None => return Change::None,
                }
            }
        };
        let row = &mut self.packages[at];
        if row.off_since_previous {
            return Change::None;
        }
        row.off_since_previous = true;
        row.save_sensitive = save_sensitive;
        row.runtime_active = Some(false);
        row.owner_epoch = None;
        let change = self.refresh(at).max(Change::Quiet);
        self.changed(change)
    }

    /// The startup pass's record:
    /// - the failing package: `data: failed`, `lua: excluded`, skip `startup-failed` (part data, the fork's text);
    /// - the packages left out with it: `data: excluded`, skip `dependency-skipped`, chain nearest first (the failing package
    ///   last; for a direct dependent the failing package alone);
    /// - the retry failed too: every ShroudForge package of the pass `data: excluded`, skip `startup-retry-failed`
    ///   with both errors.
    ///
    /// `archivePresent` is false only where the committed generation proves the data absent, else null; each skip
    /// warns (presence not-applied or unknown).
    pub fn startup_outcome(&mut self, outcome: &StartupOutcome) -> Change {
        if self.sealed() {
            return Change::None;
        }
        let mut change = Change::None;
        let mut apply = |record: &mut Self, p: &OutcomePackage, data: Data, skip: Skip| {
            if !sf_id(&p.id) {
                record.dropped += 1;
                return;
            }
            let Some(at) = record.row_for_report(&p.id) else {
                return;
            };
            let row = &mut record.packages[at];
            if row.version.is_none() {
                row.version = optional(p.version.as_deref(), VERSION_MAX);
            }
            if row.name.is_none() {
                row.name = optional(p.name.as_deref(), NAME_MAX);
            }
            change = change.max(record.set_data(
                &p.id,
                data,
                if p.absent_proven { Some(false) } else { None },
            ));
            change = change.max(record.set_lua(&p.id, Lua::Excluded));
            change = change.max(record.skip(&p.id, skip));
        };
        match (&outcome.failed, &outcome.retry_error) {
            (failed, Some(retry_error)) => {
                let first = outcome.first_error.as_deref().unwrap_or("unknown error");
                let without = failed
                    .as_ref()
                    .map_or_else(String::new, |f| format!(" without {}", f.id));
                let text = format!(
                    "the ShroudForge startup pass failed ({first}), and failed again{without} ({retry_error}): the game starts without the mods of the startup pass"
                );
                let mut all: Vec<&OutcomePackage> = outcome.packages.iter().collect();
                for p in failed.iter().chain(&outcome.dependents) {
                    if !all.iter().any(|q| q.id == p.id) {
                        all.push(p);
                    }
                }
                for p in all {
                    apply(
                        self,
                        p,
                        Data::Excluded,
                        Skip::new("startup-retry-failed", Part::Data, text.clone()),
                    );
                }
            }
            (Some(failed), None) => {
                apply(
                    self,
                    failed,
                    Data::Failed,
                    Skip::new("startup-failed", Part::Data, failed.text.clone()),
                );
                for p in &outcome.dependents {
                    let mut skip = Skip::new("dependency-skipped", Part::Package, p.text.clone());
                    skip.chain = if p.chain.is_empty() {
                        vec![failed.id.clone()]
                    } else {
                        p.chain.clone()
                    };
                    apply(self, p, Data::Excluded, skip);
                }
            }
            (None, None) => {}
        }
        change
    }

    /// kiln-eml's startup outcome, row by row: each package's `data` and `archivePresent` as the outcome proved
    /// them, and for a package whose data is not there (`failed`, `excluded`) its Lua is excluded and it gets the
    /// outcome's skip (code, text, chain; part by [`startup_part`]), so its warning says whether its data is in the game
    /// (not applied, or unknown). A package whose data is committed keeps it for good. A row for a package the table
    /// does not have yet gets one.
    pub fn startup_rows(&mut self, rows: &[StartupRow]) -> Change {
        if self.sealed() {
            return Change::None;
        }
        let mut change = Change::None;
        for row in rows {
            if !sf_id(&row.id) {
                self.dropped += 1;
                continue;
            }
            let Some(at) = self.row_for_report(&row.id) else {
                continue;
            };
            if self.packages[at].version.is_none() {
                self.packages[at].version = optional(row.version.as_deref(), VERSION_MAX);
            }
            change = change.max(self.set_data(&row.id, row.data, row.archive_present));
            if row.data.present() {
                continue;
            }
            if self.packages[at].lua != Lua::NotNeeded {
                change = change.max(self.set_lua(&row.id, Lua::Excluded));
            }
            let code = row.code.as_deref().unwrap_or("startup-not-committed");
            let mut skip = Skip::new(
                code,
                startup_part(code),
                row.text
                    .clone()
                    .unwrap_or_else(|| "its startup data was not applied this start".into()),
            );
            skip.chain = row.chain.clone();
            change = change.max(self.skip(&row.id, skip));
        }
        change
    }

    /// Recomputes row `at`'s warning from its skip, intent and presence; a new or changed warning raises
    /// `warningRevision`. A warning is never removed.
    fn refresh(&mut self, at: usize) -> Change {
        let row = &self.packages[at];
        let trigger = if row.intent == Intent::Selected && row.skip.is_some() {
            Trigger::Skipped
        } else if matches!(row.intent, Intent::Disabled | Intent::Absent) && row.off_since_previous
        {
            Trigger::SaveSensitiveOff
        } else {
            return Change::None;
        };
        let presence = match row.archive_present {
            Some(false) => Presence::NotApplied,
            Some(true) => Presence::DataAppliedRuntimeStopped,
            None => Presence::Unknown,
        };
        let text = clamp(&warning_text(row, trigger, presence), WARNING_MAX);
        let sensitive = row.save_sensitive.value;
        if let Some(w) = &row.warning
            && (w.trigger, w.presence, w.save_sensitive, &w.text)
                == (trigger, presence, sensitive, &text)
        {
            return Change::None;
        }
        self.warning_revision += 1;
        let revision = self.warning_revision;
        self.packages[at].warning = Some(Warning {
            revision,
            trigger,
            presence,
            save_sensitive: sensitive,
            text,
        });
        Change::Warning
    }

    /// The whole document, as the schema says it, written at `updated_ms`.
    pub fn to_value(&self, updated_ms: u64) -> Value {
        let mut doc = Map::new();
        doc.insert("format".into(), FORMAT.into());
        doc.insert("writer".into(), "kiln-sfi".into());
        doc.insert("kilnSfi".into(), self.kiln_sfi.into());
        doc.insert("launch".into(), self.identity.value());
        doc.insert("phase".into(), self.phase.word().into());
        if let Some(reason) = self
            .reason
            .as_ref()
            .filter(|_| !self.phase.incomplete() && self.phase != Phase::Ended)
        {
            doc.insert(
                "reason".into(),
                json!({ "code": reason.code, "text": reason.text }),
            );
        }
        doc.insert("incomplete".into(), self.phase.incomplete().into());
        doc.insert("previousStart".into(), self.previous.value());
        doc.insert("startedMs".into(), self.started_ms.into());
        doc.insert("updatedMs".into(), updated_ms.into());
        if let Some((by, key)) = &self.menu {
            doc.insert("menu".into(), json!({ "by": by.word(), "key": key }));
        }
        doc.insert("warningRevision".into(), self.warning_revision.into());
        // aside: the frozen backend is not kiln, kiln-sfi lists nothing (the schema: no rows)
        let rows = if self.phase == Phase::Aside {
            &[][..]
        } else {
            &self.packages[..]
        };
        doc.insert(
            "packages".into(),
            Value::Array(rows.iter().map(package_value).collect()),
        );
        if matches!(self.phase, Phase::Aside | Phase::Refused) {
            doc.insert(
                "carriedSaveSensitive".into(),
                Value::Array(self.carried.iter().map(carried_value).collect()),
            );
        }
        Value::Object(doc)
    }

    /// The bytes written to sfi.json: the document, pretty, with a final newline.
    pub fn to_bytes(&self, updated_ms: u64) -> Vec<u8> {
        let mut bytes = serde_json::to_vec_pretty(&self.to_value(updated_ms)).unwrap_or_default();
        bytes.push(b'\n');
        bytes
    }
}

/// A carried list as the schema allows it: ShroudForge ids only, each once (the first stays), at most
/// [`MAX_PACKAGES`]; versions and names cut to their bounds; every entry save-sensitive (a basis other than policy is
/// unknown-mixed, and policy without a usable row is unknown-mixed too, as [`SaveSensitive`] says it).
pub(crate) fn carried(list: impl IntoIterator<Item = PreviouslyActive>) -> Vec<PreviouslyActive> {
    let mut seen = std::collections::HashSet::new();
    list.into_iter()
        .filter(|p| sf_id(&p.id) && seen.insert(p.id.clone()))
        .take(MAX_PACKAGES)
        .map(|p| {
            let sensitive = SaveSensitive {
                value: true,
                basis: if p.basis == Basis::Policy.word() {
                    Basis::Policy
                } else {
                    Basis::UnknownMixed
                },
                policy_row: p.policy_row,
                line: None,
            }
            .normalised();
            PreviouslyActive {
                version: optional(p.version.as_deref(), VERSION_MAX),
                name: optional(p.name.as_deref(), NAME_MAX),
                basis: sensitive.basis.word().to_owned(),
                policy_row: sensitive.policy_row,
                ..p
            }
        })
        .collect()
}
fn carried_value(p: &PreviouslyActive) -> Value {
    json!({
        "id": p.id,
        "version": p.version,
        "basis": p.basis,
        "policyRow": p.policy_row.as_ref().map(|(id, version)| json!({ "id": id, "version": version })),
        "fromLaunchId": p.from_launch_id.map(launch_id),
    })
}

fn package_value(p: &Package) -> Value {
    let mut value = json!({
        "id": p.id,
        "version": p.version,
        "name": p.name,
        "intent": p.intent.word(),
        "data": p.data.word(),
        "lua": p.lua.word(),
        "dlls": p.dlls.iter().map(|d| json!({
            "name": d.name,
            "sha256": d.sha256.map(|h| hex(&h)),
            "state": d.state.word(),
        })).collect::<Vec<_>>(),
        "archivePresent": p.archive_present,
        "runtimeActive": p.runtime_active,
        "saveSensitive": p.save_sensitive.value_json(),
        "skip": p.skip.as_ref().map(Skip::value),
        "warning": p.warning.as_ref().map(|w| json!({
            "revision": w.revision,
            "trigger": w.trigger.word(),
            "presence": w.presence.word(),
            "saveSensitive": w.save_sensitive,
            "text": w.text,
        })),
    });
    if let Some(epoch) = p.owner_epoch {
        value["ownerEpoch"] = json!(epoch);
    }
    value
}

/// "Quiver & Spellbook (ammo-slots 0.8.2)", or "ammo-slots 0.8.2" when there is no other name.
fn label(p: &Package) -> String {
    let id = match &p.version {
        Some(version) => format!("{} {version}", p.id),
        None => p.id.clone(),
    };
    match &p.name {
        Some(name) if name != &p.id => format!("{name} ({id})"),
        _ => id,
    }
}

/// The warning for people (sfi PLAN §4): the package, the part, the reason and the chain; whether its data is in the
/// game (unknown said as unknown); another loader that still runs part of it; a late loss with its data applied ends
/// with LATE_LOSS whatever the package; and for a save-sensitive package "Do not save ..." (DO_NOT_SAVE, unless that
/// late loss already says it), the loss line and the policy's own line.
fn warning_text(p: &Package, trigger: Trigger, presence: Presence) -> String {
    let mut text = String::new();
    match (trigger, &p.skip) {
        (Trigger::Skipped, Some(skip)) => {
            let what = match skip.part {
                Part::Package => "",
                Part::Data => " (its startup data)",
                Part::Lua => " (its Lua runtime)",
                Part::Dll => " (its DLL)",
            };
            if skip.late {
                text.push_str(&format!(
                    "{} stopped{what}: {}.",
                    label(p),
                    skip.text.trim_end_matches('.')
                ));
            } else {
                text.push_str(&format!(
                    "{} was skipped this start{what}: {}.",
                    label(p),
                    skip.text.trim_end_matches('.')
                ));
            }
            if let Some((first, rest)) = skip.chain.split_first() {
                text.push_str(&format!(" It needs {first}"));
                for id in rest {
                    text.push_str(&format!(", which needs {id}"));
                }
                text.push('.');
            }
            text.push_str(" The game and the other mods go on without it.");
        }
        _ => {
            let now = if p.intent == Intent::Absent {
                "is no longer in the profile"
            } else {
                "is switched off this start"
            };
            text.push_str(&format!(
                "{} was active at the previous start and {now}.",
                label(p)
            ));
        }
    }
    match presence {
        Presence::NotApplied => text.push_str(" Its data is not in the game."),
        Presence::Unknown => {
            text.push_str(" Whether its data is in the game could not be verified.")
        }
        Presence::DataAppliedRuntimeStopped => {}
    }
    if let Some(loader) = p.skip.as_ref().and_then(|s| s.loader.as_deref()) {
        if loader == "xhl" {
            text.push_str(" XHL-Core still loads its DLL.");
        } else {
            text.push_str(&format!(
                " Another loader ({loader}) still runs part of it."
            ));
        }
    }
    if presence == Presence::DataAppliedRuntimeStopped {
        text.push(' ');
        text.push_str(LATE_LOSS);
    } else if p.save_sensitive.value {
        text.push(' ');
        text.push_str(DO_NOT_SAVE);
    }
    if p.save_sensitive.value {
        text.push(' ');
        text.push_str(SAVE_SENSITIVE);
        if let Some(line) = &p.save_sensitive.line {
            text.push(' ');
            text.push_str(line.trim());
        }
    }
    text
}
