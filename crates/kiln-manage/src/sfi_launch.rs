// SPDX-License-Identifier: MIT
//! The check before Play: [`prepare`] and [`confirm`] around [`crate::launch::play_gated`], and the
//! launcher's own Play ([`play_interactive`]), which never starts a game whose current warnings nobody acknowledged.
//!
//! The warnings here are the ShroudForge family's, as
//! [`crate::profile::sfi::status`] and the launch plan the start's profile file pins give them. A Kiln built without the
//! `sfi-scan` feature has none: its starts run as main's do.
//!
//! **Prepare** runs the start as Play does (the game not running, the cook lock, the active profile written again or the
//! play-once profile written, the mod folders' locks) up to the point right before the play-once flag, and stops there:
//! no flag (one an earlier request left is removed), no save backup, nothing opened or started, no lock kept for the
//! dialog. It answers the warnings and a token.
//!
//! **The token**: 32 random bytes, kept only in this process (a restart forgets every token), single use (the first
//! confirm that names it takes it, whatever comes of it, and so does a cancel for its game), compared in constant time,
//! valid for [`TOKEN_TTL`], bound to the game install,
//! the request and the window that asked, and to what the warnings were made from (the binding: the profile file's
//! identity, the plan it pins and that plan's bytes, kiln.json, the route, the environment, the last boot of the game,
//! the warnings themselves). A newer token of the same game replaces it.
//!
//! **Confirm** runs the start again and asks again at the same point, under the same locks, right before the flag: with
//! no warnings left, or with exactly the binding the token holds, the start goes on (flag, save backup, Steam or the
//! exe); otherwise nothing is started and the answer is a new preflight to show (`consent_required` with
//! `detail.preflight`, as [`play_interactive`] answers). A play-once start reuses the play-once profile and plan its
//! prepare wrote while nothing they were made from changed (the saved profile, the active profile file, the play-once
//! file, the plan still exactly what the producer would write): one archive inspection per play-once start, not two.
//!
//! **The launcher's Play** ([`play_interactive`]): with warnings it starts nothing and answers them as a preflight, which
//! a confirm of its token then starts; without warnings it starts as main does.
//!
//! The check says three more things, all of them read only: a save-sensitive package that was active at an
//! earlier kiln start (kiln/state/sfi.json) and does not run this start, vanilla and no-mods starts included, is a
//! `previously-active-off` warning (a package the plan skips carries it on its own warning instead);
//! an installed archive inspector that cannot be resolved while the pinned plan is kept is an `inspector-unresolved`
//! warning with its reason (a refresh keeps that plan without one); and the archive base the pinned plan was made on is
//! a `base-origin` advisory with its exact label.
//!
//! An acknowledgement switches no package on, changes no setting and is recorded nowhere: the next start shows the same
//! warnings again. [`crate::launch::play_via`] and `play_with` stay the starts with no check.
//!
//! **Headless starts** (a local dedicated server through any runner, `kilnctl run`, `kilnctl client`'s activation,
//! play-once and bundle refresh): [`prepare_headless`] writes the active profile again as Play does and answers the same
//! warnings ([`Headless::lines`] for the console and the start's log); nobody is asked, no token is issued, and the start
//! goes on without what does not run. Only an infrastructure failure (the lock, a profile that cannot be applied, the
//! folder) refuses it. A remote server's start is kilnctl's on that machine and runs this there.

use crate::game::GameInstall;
use crate::launch::{Gate, GatePoint, LaunchMode, LaunchRequest, LaunchResult, Starter};
use crate::paths::DataDirs;
use crate::profile::{Activation, ProfileFile};
use crate::{Error, ErrorCode, Result};
use serde::Serialize;
use serde_json::{Value, json};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

/// How long a token waits for its confirm.
pub const TOKEN_TTL: Duration = Duration::from_secs(600);
/// The member of a pending-warning error's `detail` that holds the preflight to show.
pub const PREFLIGHT: &str = "preflight";
/// The message of the error a prepare stops the start with (never seen outside this module).
const PREPARED: &str = "prepared: the start stops before its flag";

/// What a start will do to the ShroudForge family, for the dialog before Play, and the token that confirms it.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Preflight {
    pub game_id: String,
    pub request: LaunchRequest,
    /// 64 hex characters: one confirm (or cancel) of this start, in this launcher process.
    pub token: String,
    /// When the token expires (RFC 3339).
    pub expires: String,
    /// What the player reads and acknowledges before the game starts. Empty: no dialog, confirm at once.
    pub warnings: Vec<Warning>,
    /// SHA-256 of `warnings` as JSON: the set the dialog shows.
    pub warnings_sha256: String,
    /// Shown, never acknowledged, and no reason for a dialog by themselves (info findings, the cost note).
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub advisories: Vec<Warning>,
    /// [`crate::profile::sfi::status`] of the start's profile file (None: not a start with mods, or no `sfi-scan`).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub shroudforge: Option<Value>,
    /// The plan for this profile could not be written (`write-failed`): every Play tries again, and each try reads the
    /// game's archive again. Another refusal is answered again until an input changes (no inspection).
    pub reinspects_every_play: bool,
    /// Mode profile: the activation when the active profile was written again for this start (its kept files, retained
    /// copies and notes, as launch_play returns them).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub activation: Option<Activation>,
}

/// One warning of a start.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Warning {
    /// `start-refused` (no ShroudForge-format mod runs this start), `package-skipped` (this package does not run),
    /// `plan-warning` (a finding of the plan: a folder it cannot describe, unreadable compatibility rules, ...) or
    /// `cost` (advisories only).
    pub kind: String,
    pub code: String,
    pub message: String,
    /// info, warning, save-warning or infrastructure-error.
    pub severity: String,
    /// The dependency chain, nearest first, or what the warning is about.
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub caused_by: Vec<String>,
    /// package-skipped: the package.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub package: Option<SkippedPackage>,
    /// start-refused: the ShroudForge mods the profile file switches on (none of them runs); keybinds-unsupported: the
    /// package (its id and version).
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub packages: Vec<String>,
    /// `previously-active-off`, or a `package-skipped` one that was active before: the
    /// save-sensitive package as kiln/state/sfi.json lists it active at an earlier start.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub previous: Option<PreviousPackage>,
}

/// A save-sensitive package that was active at an earlier kiln start (kiln/state/sfi.json: its active save-sensitive
/// packages, or the list an aside or refused record carried forward), and does not run this start.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PreviousPackage {
    pub id: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub version: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    /// `policy` (a package the shipped save-sensitive policy lists) or `unknown-mixed`.
    pub basis: String,
    /// The start it was active in (sfi.json's launch id, 16 hex digits).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub from_launch_id: Option<String>,
}

/// A package the plan skips, as the plan lists it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SkippedPackage {
    pub id: String,
    pub version: String,
    /// Its folder, relative to the game folder (`mods/<folder>`).
    pub root: String,
    /// Its save data may change when a world is saved without it.
    pub save_sensitive: bool,
    /// It has startup data, Lua, and these DLLs (what the plan still lists of them).
    pub data: bool,
    pub lua: bool,
    pub dlls: Vec<String>,
    pub reasons: Vec<Reason>,
}

/// Why a package does not run.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Reason {
    pub code: String,
    pub message: String,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub caused_by: Vec<String>,
    pub severity: String,
}

// ---- the tokens ----

/// The tokens of this process's prepared starts.
pub struct Book {
    ttl: Duration,
    pending: Mutex<HashMap<String, Pending>>,
}

struct Pending {
    game_id: String,
    game: PathBuf,
    request: LaunchRequest,
    caller: Option<String>,
    issued: Instant,
    binding: String,
    activation: Option<Activation>,
    once: Option<OnceSnapshot>,
}

impl Book {
    pub fn new(ttl: Duration) -> Book {
        Book {
            ttl,
            pending: Mutex::new(HashMap::new()),
        }
    }

    /// The launcher's book ([`TOKEN_TTL`]).
    pub fn global() -> &'static Book {
        static BOOK: OnceLock<Book> = OnceLock::new();
        BOOK.get_or_init(|| Book::new(TOKEN_TTL))
    }

    /// Cancel: the token of `game_id` goes (a token of another game stays). Whether there was one.
    pub fn cancel(&self, game_id: &str, token: &str) -> bool {
        let mut all = self.pending.lock().unwrap_or_else(|e| e.into_inner());
        match find(&all, token) {
            Some(key) if all[&key].game_id == game_id => {
                all.remove(&key);
                true
            }
            _ => false,
        }
    }

    /// How many tokens wait.
    pub fn waiting(&self) -> usize {
        self.pending.lock().unwrap_or_else(|e| e.into_inner()).len()
    }

    fn issue(&self, p: Pending) -> Result<(String, String)> {
        let token = crate::hash::hex(&crate::util::random_bytes::<32>()?);
        let expires = crate::util::rfc3339(crate::util::now_unix() + self.ttl.as_secs() as i64);
        let mut all = self.pending.lock().unwrap_or_else(|e| e.into_inner());
        // one prepared start per game; expired ones go
        all.retain(|_, q| q.game_id != p.game_id && q.issued.elapsed() < self.ttl);
        all.insert(token.clone(), p);
        Ok((token, expires))
    }

    fn take(&self, token: &str) -> Result<Pending> {
        let p = {
            let mut all = self.pending.lock().unwrap_or_else(|e| e.into_inner());
            find(&all, token).and_then(|key| all.remove(&key))
        };
        match p {
            None => Err(refused(
                "unknown",
                "This start was already confirmed, canceled or replaced by a newer one, or the launcher restarted since: \
                 press Play again.",
            )),
            Some(p) if p.issued.elapsed() >= self.ttl => Err(refused(
                "expired",
                "This start waited too long for its confirmation: press Play again.",
            )),
            Some(p) => Ok(p),
        }
    }
}

/// The key of the waiting token `token` names: every waiting token compared in constant time (never a lookup that stops at
/// the first byte that differs).
fn find(all: &HashMap<String, Pending>, token: &str) -> Option<String> {
    let mut found = None;
    for key in all.keys() {
        if ct_eq(key.as_bytes(), token.as_bytes()) {
            found = Some(key.clone());
        }
    }
    found
}

/// Equal bytes, in a time that depends on their length only.
fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let diff = a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y));
    std::hint::black_box(diff) == 0
}

fn refused(why: &str, message: &str) -> Error {
    Error::new(ErrorCode::Refused, message).with_detail(json!({ "token": why }))
}

/// The preflight a pending-warning error carries (`consent_required`, `detail.preflight`).
pub fn preflight_of(e: &Error) -> Option<&Value> {
    if e.code != ErrorCode::ConsentRequired {
        return None;
    }
    e.detail.as_ref()?.get(PREFLIGHT)
}

fn pending_error(p: &Preflight, message: &str) -> Error {
    let mut d = serde_json::Map::new();
    d.insert(
        PREFLIGHT.into(),
        serde_json::to_value(p).unwrap_or(Value::Null),
    );
    Error::new(ErrorCode::ConsentRequired, message).with_detail(Value::Object(d))
}

// ---- the three entries ----

/// Prepares a start: runs it up to the check before its flag and stops there (see the module docs). The refusals of a
/// start (the game running, a cook, a play-once start that would change a fixed folder, ...) come as they would.
pub fn prepare(
    book: &Book,
    dirs: &DataDirs,
    install: &GameInstall,
    request: &LaunchRequest,
    caller: Option<&str>,
) -> Result<Preflight> {
    let mut gate = Check::new(book, caller, Purpose::Prepare);
    let never =
        |_: &str| -> Result<()> { Err(Error::new(ErrorCode::Internal, "a prepare opens nothing")) };
    let r = crate::launch::play_gated(
        dirs,
        install,
        request,
        None,
        Starter::Steam(&never),
        &mut gate,
    );
    match (gate.prepared.take(), r) {
        (Some(p), _) => Ok(p),
        (None, Err(e)) => Err(e),
        (None, Ok(_)) => Err(Error::new(
            ErrorCode::Internal,
            "a prepare went past its check",
        )),
    }
}

/// Confirms a prepared start (see the module docs): the token is taken first, whatever comes of it.
#[allow(clippy::too_many_arguments)]
pub fn confirm(
    book: &Book,
    dirs: &DataDirs,
    install: &GameInstall,
    token: &str,
    request: &LaunchRequest,
    caller: Option<&str>,
    saves: Option<&[crate::saves::SaveDir]>,
    starter: Starter<'_>,
) -> Result<LaunchResult> {
    let p = book.take(token)?;
    if p.game_id != install.id || p.game != install.path {
        return Err(refused(
            "game",
            "This confirmation belongs to another game: press Play again.",
        ));
    }
    if p.request != *request {
        return Err(refused(
            "request",
            "This confirmation belongs to another kind of start: press Play again.",
        ));
    }
    if p.caller.as_deref() != caller {
        return Err(refused(
            "caller",
            "This confirmation belongs to another window: press Play again.",
        ));
    }
    let earlier = p.activation.clone();
    let mut gate = Check::new(book, caller, Purpose::Confirm(Box::new(p)));
    let mut r = crate::launch::play_gated(dirs, install, request, saves, starter, &mut gate)?;
    if r.activation.is_none() {
        r.activation = earlier;
    }
    Ok(r)
}

/// The launcher's Play: a start with warnings starts nothing and answers them as a preflight (`consent_required`,
/// `detail.preflight`); one without starts as main's does.
pub fn play_interactive(
    book: &Book,
    dirs: &DataDirs,
    install: &GameInstall,
    request: &LaunchRequest,
    caller: Option<&str>,
    saves: Option<&[crate::saves::SaveDir]>,
    starter: Starter<'_>,
) -> Result<LaunchResult> {
    let mut gate = Check::new(book, caller, Purpose::Interactive);
    crate::launch::play_gated(dirs, install, request, saves, starter, &mut gate)
}

// ---- the headless starts ----

/// What a headless start found right before it starts: a local dedicated server (the launcher's server start through
/// any runner, `kilnctl run`), and `kilnctl client`'s activation, play-once and bundle refresh. The same warnings the
/// check before Play shows ([`assess`]), for the console, the start's log and its progress; nobody is asked, no token is
/// issued, nothing waits: the start goes on without the packages that do not run.
#[derive(Debug, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Headless {
    /// The activation when the active profile was written again for this start.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub activation: Option<Activation>,
    pub warnings: Vec<Warning>,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub advisories: Vec<Warning>,
    /// The game's cook lock, held from the refresh until the caller has started the server (dropped with this).
    #[serde(skip)]
    lock: Option<crate::state::HeldLock>,
}

impl Headless {
    /// One console line per warning, then per advisory, then a closing line when there were warnings: plain text, every
    /// control character (a line break, an escape sequence, a bidi override in a package's own name or reason) written
    /// as `\u{..}`, so a package cannot add or rewrite lines.
    pub fn lines(&self) -> Vec<String> {
        let mut out: Vec<String> = self.warnings.iter().map(|w| line(w, false)).collect();
        out.extend(self.advisories.iter().map(|w| line(w, true)));
        if !self.warnings.is_empty() {
            let save = self.warnings.iter().any(|w| w.severity == "save-warning");
            out.push(format!(
                "{} ShroudForge warning(s): this start goes on without what does not run (a headless start asks \
                 nobody){}",
                self.warnings.len(),
                if save { "; do not save a world that used those mods until they run again" } else { "" }
            ));
        }
        out
    }

    /// Lets the cook lock go (a caller that starts nothing).
    pub fn release(&mut self) {
        self.lock = None;
    }
}

/// `s` with every control character, line separator and bidi control as `\u{..}` (console and log lines).
pub fn console_safe(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        let unsafe_char = c.is_control()
            || matches!(c, '\u{2028}' | '\u{2029}' | '\u{200e}' | '\u{200f}' | '\u{061c}' | '\u{202a}'..='\u{202e}' | '\u{2066}'..='\u{2069}');
        if unsafe_char {
            out.push_str(&format!("\\u{{{:x}}}", c as u32));
        } else {
            out.push(c);
        }
    }
    out
}

fn line(w: &Warning, advisory: bool) -> String {
    let level = if advisory {
        "note"
    } else {
        match w.severity.as_str() {
            "save-warning" => "SAVE WARNING",
            "infrastructure-error" => "ERROR",
            "info" => "note",
            _ => "WARNING",
        }
    };
    let mut s = match (w.kind.as_str(), &w.package) {
        ("package-skipped", Some(p)) => {
            let mut s = format!("ShroudForge package {} {} ({}) does not run this start: {}", p.id, p.version, p.root, w.message);
            let parts: Vec<String> = [(p.data, "startup data"), (p.lua, "Lua")]
                .iter()
                .filter(|(on, _)| *on)
                .map(|(_, n)| (*n).to_owned())
                .chain(p.dlls.iter().map(|d| format!("DLL {d}")))
                .collect();
            if !parts.is_empty() {
                s += &format!(" (its {} too)", parts.join(", "));
            }
            if p.save_sensitive || w.previous.is_some() {
                s += ". Its save data may change when a world is saved without it: do not save";
            }
            s
        }
        ("start-refused", _) => {
            let mut s = format!("no ShroudForge package runs this start ({}): {}", w.code, w.message);
            if !w.packages.is_empty() {
                s += &format!(" (switched on: {})", w.packages.join(", "));
            }
            s
        }
        _ => format!("{} ({}): {}", w.kind, w.code, w.message),
    };
    if !w.caused_by.is_empty() && w.kind != "start-refused" {
        s += &format!(" (because of {})", w.caused_by.join(" <- "));
    }
    console_safe(&format!("{level}: {s}"))
}

/// The warnings of a start of `request` in `install` as they are now, for a caller that wrote its profile file itself
/// (`kilnctl client activate` and `play-once`): read only. Empty without `sfi-scan`, and when kiln.json asks for no
/// Kiln interpreter (hosted, installed): those print what they printed.
pub fn headless_warnings(dirs: &DataDirs, install: &GameInstall, request: &LaunchRequest) -> Headless {
    if !interpreter_requested(install) {
        return Headless::default();
    }
    let a = assess(dirs, install, request);
    Headless { activation: None, warnings: a.warnings, advisories: a.advisories, lock: None }
}

/// Before a headless start of the active profile of `install` (a server folder, or a client): when kiln.json asks for
/// Kiln's interpreter, the active profile is written again the way Play writes it (`profile::refresh_active`: the plan
/// kept while current, made again when an input changed, a refusal recorded), under the game's cook lock, and the
/// warnings are what the check before Play would show. Ok(None) without `sfi-scan` and when kiln.json asks for no Kiln
/// interpreter: nothing is read but kiln.json, nothing is written (main's start). An error (the lock, a profile that
/// cannot be applied, the game folder) refuses the start; a package that does not run never does.
pub fn prepare_headless(dirs: &DataDirs, install: &GameInstall) -> Result<Option<Headless>> {
    if !interpreter_requested(install) {
        return Ok(None);
    }
    let lock = crate::state::lock_game(&install.path, Duration::from_secs(5), &|| false)?;
    let activation = crate::profile::refresh_active(dirs, install).map_err(|e| {
        let message = format!("the active profile cannot be applied, so the server was not started: {}", e.message);
        Error { message, ..e }
    })?;
    let request = LaunchRequest { mode: LaunchMode::Profile, profile_id: None, skip_backup: true };
    let a = assess(dirs, install, &request);
    Ok(Some(Headless { activation, warnings: a.warnings, advisories: a.advisories, lock }))
}

/// kiln.json asks for Kiln's interpreter (or its ShroudForge backend setting is invalid): the headless starts this prepares.
fn interpreter_requested(install: &GameInstall) -> bool {
    let side = match install.kind {
        crate::game::GameKind::Client => crate::manifest::RunSide::Client,
        crate::game::GameKind::Server => crate::manifest::RunSide::Server,
    };
    requests_interpreter(&install.path, side)
}

/// Whether kiln.json in `dir` asks for Kiln's interpreter on `side`, or its ShroudForge backend setting is invalid (read
/// only: kiln.json and the environment). A server start asks this before it makes an install record of its folder: with
/// a hosted or installed backend it does nothing more. False without `sfi-scan`.
pub fn requests_interpreter(dir: &Path, side: crate::manifest::RunSide) -> bool {
    #[cfg(feature = "sfi-scan")]
    {
        use crate::sfi_backend::{Backend, Refusal};
        let s = crate::sfi_backend::resolve_game(dir, side, &crate::sfi_backend::Env::from_process());
        s.requested == Some(Backend::Kiln) || s.refusal == Some(Refusal::SettingInvalid)
    }
    #[cfg(not(feature = "sfi-scan"))]
    {
        let _ = (dir, side);
        false
    }
}

// ---- the check ----

enum Purpose {
    Prepare,
    Interactive,
    Confirm(Box<Pending>),
}

struct Check<'b> {
    book: &'b Book,
    caller: Option<String>,
    purpose: Purpose,
    prepared: Option<Preflight>,
}

impl<'b> Check<'b> {
    fn new(book: &'b Book, caller: Option<&str>, purpose: Purpose) -> Self {
        Check {
            book,
            caller: caller.map(str::to_owned),
            purpose,
            prepared: None,
        }
    }
}

impl Gate for Check<'_> {
    fn once_file(
        &mut self,
        dirs: &DataDirs,
        profile_id: &str,
        install: &GameInstall,
    ) -> Result<ProfileFile> {
        if let Purpose::Confirm(p) = &self.purpose
            && let Some(s) = &p.once
            && s.still(dirs, install, profile_id)
            && let Ok(Some(file)) = crate::util::read_json::<ProfileFile>(
                &install.path.join(crate::paths::game::PROFILE_ONCE),
            )
        {
            return Ok(file);
        }
        crate::profile::write_once(dirs, profile_id, install)
    }

    fn check(&mut self, at: &GatePoint<'_>) -> Result<()> {
        let a = assess(at.dirs, at.install, at.request);
        match &self.purpose {
            Purpose::Interactive if a.warnings.is_empty() => return Ok(()),
            Purpose::Confirm(p) if a.warnings.is_empty() || a.binding == p.binding => return Ok(()),
            _ => {}
        }
        let earlier = match &self.purpose {
            Purpose::Confirm(p) => p.activation.clone(),
            _ => None,
        };
        let activation = at.activation.cloned().or(earlier);
        let once = match (at.request.mode, at.request.profile_id.as_deref()) {
            (LaunchMode::Once, Some(pid)) => OnceSnapshot::take(at.dirs, at.install, pid),
            _ => None,
        };
        let (token, expires) = self.book.issue(Pending {
            game_id: at.install.id.clone(),
            game: at.install.path.clone(),
            request: at.request.clone(),
            caller: self.caller.clone(),
            issued: Instant::now(),
            binding: a.binding,
            activation: activation.clone(),
            once,
        })?;
        let p = Preflight {
            game_id: at.install.id.clone(),
            request: at.request.clone(),
            token,
            expires,
            warnings: a.warnings,
            warnings_sha256: a.warnings_sha256,
            advisories: a.advisories,
            shroudforge: a.shroudforge,
            reinspects_every_play: a.reinspects,
            activation,
        };
        let e = match &self.purpose {
            Purpose::Prepare => {
                self.prepared = Some(p);
                return Err(Error::new(ErrorCode::Canceled, PREPARED));
            }
            Purpose::Interactive => pending_error(
                &p,
                "Some ShroudForge mods will not run this start. Read the warning and confirm it first; the game was \
                 not started.",
            ),
            Purpose::Confirm(_) => pending_error(
                &p,
                "What will not run this start changed since the warning was shown. Read it again and confirm it; the \
                 game was not started.",
            ),
        };
        Err(e)
    }
}

/// What a play-once start's prepare wrote and made it from: its confirm boots the same file and plan while all of it is
/// as it was and the plan is still exactly what the producer would write.
struct OnceSnapshot {
    once: String,
    active: Option<String>,
    saved: String,
}

fn file_sha(path: &Path) -> Option<String> {
    std::fs::read(path)
        .ok()
        .map(|b| crate::hash::sha256_hex(&b))
}

fn saved_sha(dirs: &DataDirs, profile_id: &str) -> Option<String> {
    let p = crate::profile::get(dirs, profile_id).ok()?;
    serde_json::to_vec(&p)
        .ok()
        .map(|b| crate::hash::sha256_hex(&b))
}

impl OnceSnapshot {
    /// Only a play-once file that pins a plan is reused (main's play-once start writes its files again every time).
    fn take(dirs: &DataDirs, install: &GameInstall, profile_id: &str) -> Option<OnceSnapshot> {
        let path = install.path.join(crate::paths::game::PROFILE_ONCE);
        let doc: Value = crate::util::read_json(&path).ok().flatten()?;
        doc.get("sfLaunchPlan")?;
        Some(OnceSnapshot {
            once: file_sha(&path)?,
            active: file_sha(&install.path.join(crate::paths::game::PROFILE)),
            saved: saved_sha(dirs, profile_id)?,
        })
    }

    fn still(&self, dirs: &DataDirs, install: &GameInstall, profile_id: &str) -> bool {
        file_sha(&install.path.join(crate::paths::game::PROFILE_ONCE)).as_deref()
            == Some(self.once.as_str())
            && file_sha(&install.path.join(crate::paths::game::PROFILE)) == self.active
            && saved_sha(dirs, profile_id).as_deref() == Some(self.saved.as_str())
            && plan_current(dirs, install)
    }
}

#[cfg(feature = "sfi-scan")]
fn plan_current(dirs: &DataDirs, install: &GameInstall) -> bool {
    crate::profile::sfi::plan_current(dirs, install, crate::sfi_plan::Mode::Once)
}

#[cfg(not(feature = "sfi-scan"))]
fn plan_current(_: &DataDirs, _: &GameInstall) -> bool {
    false
}

// ---- the warnings ----

/// What a start's check found, at the check.
struct Assessment {
    warnings: Vec<Warning>,
    warnings_sha256: String,
    advisories: Vec<Warning>,
    shroudforge: Option<Value>,
    reinspects: bool,
    /// SHA-256 over everything the warnings were made from (see the module docs).
    binding: String,
}

impl Assessment {
    /// No warnings: a start without mods, or a Kiln without `sfi-scan`.
    fn none(install: &GameInstall, request: &LaunchRequest) -> Assessment {
        let warnings_sha256 = sha_json(&json!([]));
        let binding = sha_json(
            &json!({ "install": install_key(install), "request": request, "warnings": warnings_sha256 }),
        );
        Assessment {
            warnings: Vec::new(),
            warnings_sha256,
            advisories: Vec::new(),
            shroudforge: None,
            reinspects: false,
            binding,
        }
    }
}

fn sha_json(v: &Value) -> String {
    crate::hash::sha256_hex(v.to_string().as_bytes())
}

fn install_key(install: &GameInstall) -> Value {
    json!({ "id": install.id, "path": install.path.to_string_lossy(), "exe": install.exe })
}

#[cfg(not(feature = "sfi-scan"))]
fn assess(_: &DataDirs, install: &GameInstall, request: &LaunchRequest) -> Assessment {
    Assessment::none(install, request)
}

/// The producer's refusals. `write-failed` is tried again at every Play; the others are answered again (no inspection)
/// until the game, the mods or Kiln change, or the profile is activated (`crate::profile::sfi`, the refusal's inputs).
#[cfg(feature = "sfi-scan")]
const PRODUCER_REFUSALS: [&str; 4] = [
    "no-archive-base",
    "input-unreadable",
    "plan-invalid",
    "write-failed",
];

#[cfg(feature = "sfi-scan")]
fn assess(dirs: &DataDirs, install: &GameInstall, request: &LaunchRequest) -> Assessment {
    use crate::sfi_plan::Mode;
    let mode = match request.mode {
        LaunchMode::Profile => Mode::Active,
        LaunchMode::Once => Mode::Once,
        // nothing of the ShroudForge family runs: only what was active before is said
        LaunchMode::Vanilla | LaunchMode::NoMods => return bare(install, request),
    };
    let game = &install.path;
    let s = crate::profile::sfi::status(dirs, install, mode);
    let rel = match mode {
        Mode::Active => crate::paths::game::PROFILE,
        Mode::Once => crate::paths::game::PROFILE_ONCE,
    };
    let doc: Value = crate::util::read_json(&game.join(rel))
        .ok()
        .flatten()
        .unwrap_or(Value::Null);
    let mut warnings = Vec::new();
    let mut advisories = Vec::new();
    let mut plan_bytes = None;
    let mut plan_doc = None;
    if s.requested {
        match (&s.refused, &s.plan) {
            (Some(r), _) => warnings.push(start_refused(&r.code, &r.reason, &doc)),
            (None, Some(pin)) => match read_plan(game, pin) {
                Ok((sha, plan)) => {
                    warnings.extend(skipped(&plan));
                    advisories.extend(keybinds_unsupported(game, &plan));
                    plan_bytes = Some(sha);
                    plan_doc = Some(plan);
                }
                Err(why) => warnings.push(start_refused("plan-missing", &why, &doc)),
            },
            // ShroudForge's own loader runs them (its own-loader note says so)
            (None, None) => {}
        }
        for n in &s.warnings {
            let w = Warning {
                kind: "plan-warning".into(),
                code: n.code.clone(),
                message: n.message.clone(),
                severity: n.severity.clone(),
                caused_by: n.caused_by.clone(),
                package: None,
                packages: Vec::new(),
                previous: None,
            };
            if n.severity == "info" {
                advisories.push(w);
            } else {
                warnings.push(w);
            }
        }
    }
    // the archive base the pinned plan was made on, in its exact words (shown, never acknowledged)
    if let Some(a) = plan_doc.as_ref().and_then(base_origin) {
        advisories.push(a);
    }
    // a refresh keeps the pinned plan when the installed inspector cannot be resolved now: say why, here
    if s.requested
        && s.route == "interpreter"
        && s.refused.is_none()
        && plan_doc.is_some()
        && let Some(why) = crate::profile::sfi::inspector_unresolved(game)
    {
        warnings.push(Warning {
            kind: "plan-warning".into(),
            code: "inspector-unresolved".into(),
            message: format!(
                "Kiln's archive inspector for this game cannot be used ({why}). This start keeps the launch plan made \
                 earlier; after a change to the profile or the game, Kiln cannot make a new plan until Kiln is \
                 installed again."
            ),
            severity: "warning".into(),
            caused_by: Vec::new(),
            package: None,
            packages: Vec::new(),
            previous: None,
        });
    }
    // save-sensitive packages active at an earlier start that do not run this one
    let running = running_ids(&s, plan_doc.as_ref(), &doc);
    previously_off(&mut warnings, &previously_active(game), &running);
    let refusals: Vec<&str> = s
        .refused
        .iter()
        .map(|r| r.code.as_str())
        .chain(s.warnings.iter().map(|n| n.code.as_str()))
        .filter(|c| PRODUCER_REFUSALS.contains(c))
        .collect();
    let reinspects = refusals.contains(&"write-failed");
    let note = |code: &str, message: &str| Warning {
        kind: "cost".into(),
        code: code.into(),
        message: message.into(),
        severity: "info".into(),
        caused_by: Vec::new(),
        package: None,
        packages: Vec::new(),
        previous: None,
    };
    if reinspects {
        advisories.push(note(
            "reinspects-every-play",
            "Kiln could not write a launch plan for this profile, so every Play tries again, and each try reads the \
             game's archive again (it can take a while).",
        ));
    } else if !refusals.is_empty() {
        advisories.push(note(
            "retries-on-change",
            "Kiln could not make a launch plan for this profile. Kiln tries again when the game, the mods or Kiln \
             change, or when you activate the profile.",
        ));
    }
    let warnings_sha256 = sha_json(&serde_json::to_value(&warnings).unwrap_or(Value::Null));
    let ident = crate::sfi_plan::ProfileIdentity::of(&doc, mode)
        .ok()
        .map(|i| json!({ "id": i.id, "generation": i.generation, "semantic": i.semantic_sha256 }));
    let env = crate::sfi_backend::Env::from_process();
    let binding = sha_json(&json!({
        "install": install_key(install),
        "request": request,
        "profile": ident,
        "plan": s.plan.as_ref().map(|p| json!({ "path": p.path, "sha256": p.sha256, "bytes": plan_bytes })),
        "kilnJson": file_sha(&game.join(crate::paths::game::KILN).join("kiln.json")),
        "requested": s.requested,
        "route": s.route,
        "refused": s.refused.as_ref().map(|r| r.code.clone()),
        "env": { "shroudforge": env.shroudforge, "runtime": env.runtime },
        "boot": boot_stamp(game),
        "sfiState": file_sha(&game.join(SFI_STATE)),
        "warnings": warnings_sha256,
    }));
    Assessment {
        warnings,
        warnings_sha256,
        advisories,
        shroudforge: serde_json::to_value(&s).ok(),
        reinspects,
        binding,
    }
}

/// kiln-sfi's record of the last start (kiln/state/sfi.json).
#[cfg(feature = "sfi-scan")]
const SFI_STATE: &str = "kiln/state/sfi.json";
/// The largest sfi.json read (kiln-sfi's own bound for the file it replaces).
#[cfg(feature = "sfi-scan")]
const SFI_STATE_MAX: u64 = 32 << 20;
/// At most this many earlier packages (sfi.json's own bound).
#[cfg(feature = "sfi-scan")]
const PREVIOUS_MAX: usize = 4096;

/// A vanilla or no-mods start: no plan, nothing of the ShroudForge family runs; only the save-sensitive packages active
/// at an earlier start are warned about.
#[cfg(feature = "sfi-scan")]
fn bare(install: &GameInstall, request: &LaunchRequest) -> Assessment {
    let game = &install.path;
    let mut warnings = Vec::new();
    previously_off(
        &mut warnings,
        &previously_active(game),
        &std::collections::HashSet::new(),
    );
    if warnings.is_empty() {
        return Assessment::none(install, request);
    }
    let warnings_sha256 = sha_json(&serde_json::to_value(&warnings).unwrap_or(Value::Null));
    let binding = sha_json(&json!({
        "install": install_key(install),
        "request": request,
        "boot": boot_stamp(game),
        "sfiState": file_sha(&game.join(SFI_STATE)),
        "warnings": warnings_sha256,
    }));
    Assessment {
        warnings,
        warnings_sha256,
        advisories: Vec::new(),
        shroudforge: None,
        reinspects: false,
        binding,
    }
}

/// The ShroudForge mods the profile file switches on.
#[cfg(feature = "sfi-scan")]
fn enabled_sf(doc: &Value) -> Vec<String> {
    doc["mods"]["shroudforge"]
        .as_array()
        .into_iter()
        .flatten()
        .filter(|m| m["enabled"].as_bool().unwrap_or(true))
        .filter_map(|m| m["id"].as_str().map(str::to_owned))
        .collect()
}

/// The ShroudForge packages that run this start: on Kiln's interpreter the plan's candidates (none when the start of
/// them is refused or the plan is gone); with ShroudForge's own loader or a hosted backend, the ones the profile file
/// switches on.
#[cfg(feature = "sfi-scan")]
fn running_ids(
    s: &crate::profile::sfi::Status,
    plan: Option<&Value>,
    doc: &Value,
) -> std::collections::HashSet<String> {
    if !s.requested || (s.refused.is_none() && s.plan.is_none()) {
        return enabled_sf(doc).into_iter().collect();
    }
    plan.map(|p| {
        p["packages"]
            .as_array()
            .into_iter()
            .flatten()
            .filter(|p| p["decision"] == "candidate")
            .filter_map(|p| p["id"].as_str().map(str::to_owned))
            .collect()
    })
    .unwrap_or_default()
}

/// The save-sensitive packages kiln/state/sfi.json lists as active at the last kiln start, read as kiln-sfi reads the
/// file it replaces: the record's selected save-sensitive packages that ran or whose data was in the game, and for an
/// aside or refused record the list it carried forward too; each id once, the record's own row first. Nothing for a
/// missing, oversized, unreadable or malformed file.
#[cfg(feature = "sfi-scan")]
fn previously_active(game: &Path) -> Vec<PreviousPackage> {
    use std::io::Read;
    let Ok(file) = std::fs::File::open(game.join(SFI_STATE)) else {
        return Vec::new();
    };
    let mut bytes = Vec::new();
    if file
        .take(SFI_STATE_MAX + 1)
        .read_to_end(&mut bytes)
        .is_err()
        || bytes.len() as u64 > SFI_STATE_MAX
    {
        return Vec::new();
    }
    let Ok(doc) = serde_json::from_slice::<Value>(&bytes) else {
        return Vec::new();
    };
    if doc["format"].as_u64() != Some(1)
        || !doc["packages"].is_array()
        || !doc["incomplete"].is_boolean()
    {
        return Vec::new();
    }
    let text = |v: &Value| v.as_str().map(str::to_owned);
    let launch = text(&doc["launch"]["launchId"]);
    let own = doc["packages"]
        .as_array()
        .into_iter()
        .flatten()
        .filter(|p| {
            p["intent"] == "selected"
                && p["saveSensitive"]["value"] == true
                && (p["runtimeActive"] == true || p["archivePresent"] == true)
        })
        .filter_map(|p| {
            Some(PreviousPackage {
                id: text(&p["id"])?,
                version: text(&p["version"]),
                name: text(&p["name"]),
                basis: basis(&p["saveSensitive"]["basis"]),
                from_launch_id: launch.clone(),
            })
        });
    let carried: Vec<PreviousPackage> =
        if matches!(doc["phase"].as_str(), Some("aside" | "refused")) {
            doc["carriedSaveSensitive"]
                .as_array()
                .into_iter()
                .flatten()
                .filter_map(|c| {
                    Some(PreviousPackage {
                        id: text(&c["id"])?,
                        version: text(&c["version"]),
                        name: None,
                        basis: basis(&c["basis"]),
                        from_launch_id: text(&c["fromLaunchId"]),
                    })
                })
                .collect()
        } else {
            Vec::new()
        };
    let mut seen = std::collections::HashSet::new();
    own.chain(carried)
        .filter(|p| !p.id.is_empty() && seen.insert(p.id.clone()))
        .take(PREVIOUS_MAX)
        .collect()
}

/// `policy` stays; any other basis is unknown-mixed (as kiln-sfi normalises a carried entry).
#[cfg(feature = "sfi-scan")]
fn basis(v: &Value) -> String {
    if v == "policy" {
        "policy"
    } else {
        "unknown-mixed"
    }
    .to_owned()
}

/// One warning per earlier save-sensitive package that does not run this start; a package the plan skips gets the
/// earlier start on its own warning (which becomes a save warning) instead of a second line.
#[cfg(feature = "sfi-scan")]
fn previously_off(
    warnings: &mut Vec<Warning>,
    previous: &[PreviousPackage],
    running: &std::collections::HashSet<String>,
) {
    for p in previous.iter().filter(|p| !running.contains(&p.id)) {
        if let Some(w) = warnings
            .iter_mut()
            .find(|w| w.package.as_ref().is_some_and(|k| k.id == p.id))
        {
            w.severity = "save-warning".into();
            w.previous = Some(p.clone());
            continue;
        }
        let label = match (&p.name, &p.version) {
            (Some(n), Some(v)) => format!("{n} ({} {v})", p.id),
            (Some(n), None) => format!("{n} ({})", p.id),
            (None, Some(v)) => format!("{} {v}", p.id),
            (None, None) => p.id.clone(),
        };
        warnings.push(Warning {
            kind: "previously-active-off".into(),
            code: "save-sensitive-now-off".into(),
            message: format!(
                "{label} was active at an earlier start and does not run this start. Its save data may change when a \
                 world is saved without it."
            ),
            severity: "save-warning".into(),
            caused_by: Vec::new(),
            package: None,
            packages: Vec::new(),
            previous: Some(p.clone()),
        });
    }
}

/// The archive base the plan was made on, in its exact label (an advisory: shown, never acknowledged).
#[cfg(feature = "sfi-scan")]
fn base_origin(plan: &Value) -> Option<Warning> {
    let token = plan["game"]["archiveBaseOrigin"].as_str()?;
    let &(token, label) = crate::sfi_inspection::ORIGINS
        .iter()
        .find(|(t, _)| *t == token)?;
    Some(Warning {
        kind: "base-origin".into(),
        code: token.to_owned(),
        message: label.to_owned(),
        severity: "info".into(),
        caused_by: Vec::new(),
        package: None,
        packages: Vec::new(),
        previous: None,
    })
}

/// The last boot of the game (boot.json's size and time): a run of the game between prepare and confirm changes it.
#[cfg(feature = "sfi-scan")]
fn boot_stamp(game: &Path) -> Value {
    let Ok(m) = std::fs::symlink_metadata(game.join(crate::paths::game::BOOT_JSON)) else {
        return Value::Null;
    };
    let at = m
        .modified()
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_nanos().to_string());
    json!({ "size": m.len(), "modified": at })
}

/// A start of none of the ShroudForge-format mods: why, and the ShroudForge mods the profile file switches on.
#[cfg(feature = "sfi-scan")]
fn start_refused(code: &str, reason: &str, doc: &Value) -> Warning {
    let packages = enabled_sf(doc);
    Warning {
        kind: "start-refused".into(),
        code: code.to_owned(),
        message: reason.to_owned(),
        severity: "warning".into(),
        caused_by: Vec::new(),
        package: None,
        packages,
        previous: None,
    }
}

/// The plan the profile file pins: its bytes' SHA-256 and its JSON, or why not (the core then starts none of them).
#[cfg(feature = "sfi-scan")]
fn read_plan(
    game: &Path,
    pin: &crate::sfi_plan::PlanRef,
) -> std::result::Result<(String, Value), String> {
    let gone = "the launch plan the profile names is missing or was changed since it was written, so the game starts \
                without ShroudForge packages; activate the profile again";
    let name = pin
        .path
        .strip_prefix(&format!("{}/", crate::sfi_plan::PLAN_FOLDER))
        .filter(|n| {
            !n.is_empty() && !n.contains(['/', '\\']) && n.ends_with(".json") && !n.starts_with('.')
        });
    let Some(name) = name else {
        return Err(gone.into());
    };
    let path = game
        .join(crate::paths::game::KILN)
        .join(crate::sfi_plan::PLAN_FOLDER)
        .join(name);
    if !std::fs::symlink_metadata(&path).is_ok_and(|m| m.is_file()) {
        return Err(gone.into());
    }
    let bytes = std::fs::read(&path).map_err(|_| gone.to_owned())?;
    let sha = crate::hash::sha256_hex(&bytes);
    if sha != pin.sha256 {
        return Err(gone.into());
    }
    let plan: Value = serde_json::from_slice(&bytes).map_err(|_| gone.to_owned())?;
    Ok((sha, plan))
}

/// The code of the advisory for a package that runs and declares a keyboard shortcut setting.
pub const KEYBINDS_UNSUPPORTED: &str = "keybinds-unsupported";
/// The largest extended.mod.json read for it.
#[cfg(feature = "sfi-scan")]
const EXTENDED_MAX: u64 = 1 << 20;

/// The packages that run this start and declare a keyboard shortcut (an extended.mod.json setting with control
/// `keybind`): kiln mode does not read shortcuts yet (a mod that asks whether its key is pressed is told it is not),
/// so each one gets an advisory: shown, never acknowledged, and no reason for a dialog by itself.
#[cfg(feature = "sfi-scan")]
fn keybinds_unsupported(game: &Path, plan: &Value) -> Vec<Warning> {
    plan["packages"]
        .as_array()
        .into_iter()
        .flatten()
        .filter(|p| p["decision"] == "candidate")
        .filter(|p| declares_keybind(game, p["root"].as_str().unwrap_or_default()))
        .map(|p| {
            let id = p["id"].as_str().unwrap_or_default();
            let label = match p["version"].as_str().unwrap_or_default() {
                "" => id.to_owned(),
                v => format!("{id} {v}"),
            };
            Warning {
                kind: "plan-warning".into(),
                code: KEYBINDS_UNSUPPORTED.into(),
                message: format!("{label}: its keyboard shortcuts do not work in kiln mode yet; the rest of the mod runs"),
                severity: "info".into(),
                caused_by: Vec::new(),
                package: None,
                packages: vec![label],
                previous: None,
            }
        })
        .collect()
}

/// Whether the package folder `root` (`mods/<folder>`, as the plan names it) has an extended.mod.json whose settings
/// hold a keybind. Read only, through no link, at most [`EXTENDED_MAX`] bytes; anything unreadable says no.
#[cfg(feature = "sfi-scan")]
fn declares_keybind(game: &Path, root: &str) -> bool {
    use std::io::Read;
    let Some(folder) = root
        .strip_prefix("mods/")
        .filter(|f| !f.is_empty() && *f != "." && *f != ".." && !f.contains(['/', '\\']))
    else {
        return false;
    };
    let dir = game.join("mods").join(folder);
    let path = dir.join("extended.mod.json");
    if !std::fs::symlink_metadata(&dir).is_ok_and(|m| m.is_dir())
        || !std::fs::symlink_metadata(&path).is_ok_and(|m| m.is_file())
    {
        return false;
    }
    let mut bytes = Vec::new();
    let read = std::fs::File::open(&path).and_then(|f| f.take(EXTENDED_MAX + 1).read_to_end(&mut bytes));
    if read.is_err() || bytes.len() as u64 > EXTENDED_MAX {
        return false;
    }
    let text = bytes.strip_prefix(b"\xEF\xBB\xBF").unwrap_or(&bytes);
    serde_json::from_slice::<Value>(text).is_ok_and(|doc| {
        doc["settings"]
            .as_object()
            .is_some_and(|s| s.values().any(|v| v["control"] == "keybind"))
    })
}

/// The packages the plan skips, one warning each.
#[cfg(feature = "sfi-scan")]
fn skipped(plan: &Value) -> Vec<Warning> {
    let rank = |s: &str| match s {
        "infrastructure-error" => 3,
        "save-warning" => 2,
        "warning" => 1,
        _ => 0,
    };
    let text = |v: &Value| v.as_str().unwrap_or_default().to_owned();
    let list = |v: &Value| -> Vec<String> {
        v.as_array()
            .into_iter()
            .flatten()
            .filter_map(|s| s.as_str().map(str::to_owned))
            .collect()
    };
    let mut out = Vec::new();
    for p in plan["packages"]
        .as_array()
        .into_iter()
        .flatten()
        .filter(|p| p["decision"] == "skip")
    {
        let reasons: Vec<Reason> = p["reasons"]
            .as_array()
            .into_iter()
            .flatten()
            .map(|r| Reason {
                code: text(&r["code"]),
                message: text(&r["message"]),
                caused_by: list(&r["causedBy"]),
                severity: text(&r["severity"]),
            })
            .collect();
        let save_sensitive = p["saveSensitive"].as_bool().unwrap_or(false);
        let severity = if save_sensitive {
            "save-warning".to_owned()
        } else {
            reasons
                .iter()
                .map(|r| r.severity.clone())
                .max_by_key(|s| rank(s))
                .unwrap_or_else(|| "warning".into())
        };
        let package = SkippedPackage {
            id: text(&p["id"]),
            version: text(&p["version"]),
            root: text(&p["root"]),
            save_sensitive,
            data: p["data"]["required"].as_bool().unwrap_or(false),
            lua: p["lua"]["required"].as_bool().unwrap_or(false),
            dlls: p["dlls"]
                .as_array()
                .into_iter()
                .flatten()
                .filter_map(|d| d["path"].as_str().map(str::to_owned))
                .collect(),
            reasons,
        };
        out.push(Warning {
            kind: "package-skipped".into(),
            code: package
                .reasons
                .first()
                .map(|r| r.code.clone())
                .unwrap_or_else(|| "skip".into()),
            message: package
                .reasons
                .iter()
                .map(|r| r.message.as_str())
                .collect::<Vec<_>>()
                .join("; "),
            severity,
            caused_by: package
                .reasons
                .iter()
                .find(|r| r.code == "dependency-skipped")
                .map(|r| r.caused_by.clone())
                .unwrap_or_default(),
            package: Some(package),
            packages: Vec::new(),
            previous: None,
        });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::ct_eq;

    /// A token compares whole: every byte, and its length.
    #[test]
    fn tokens_compare_whole() {
        assert!(ct_eq(b"0a1b", b"0a1b"));
        assert!(ct_eq(b"", b""));
        assert!(!ct_eq(b"0a1b", b"1a1b"));
        assert!(!ct_eq(b"0a1b", b"0a1c"));
        assert!(!ct_eq(b"0a1b", b"0a1"));
        assert!(!ct_eq(b"0a1", b"0a1b"));
    }
}
