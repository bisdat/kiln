//! `DebugTargetResolver` (chapter 12a §4 steps 2, 5, 6, 8): from an install's descriptors to one proven target, and
//! the check that runs again right before an adapter may start.
//!
//! Two views are compared, never trusted alone:
//! - the **host side** ([`HostSide`]): the install, its prefix (canonical path, device, inode), the runner's digest,
//!   and for display the runner's Linux process;
//! - the **Windows side** ([`ProcessSource`]): the process the descriptor names, opened and held in its own namespace
//!   (native Windows: in this process; Wine/Proton: the `kiln debug-inspect` helper under the target's runner), its
//!   creation time, image path and digest, and the control pipe's answer to a fresh challenge.
//!
//! A target is the one live descriptor whose every field agrees with both views. [`permit`] repeats the check (and
//! the consent rule) right before the adapter starts; an adapter can only be started with the [`AttachPermit`] it
//! returns. Nothing here kills or terminates a process.

use super::errors::{DebugCode, DebugError};
use super::identity::*;
use serde::{Deserialize, Serialize};
use std::path::{Component, Path, PathBuf};

/// What the Windows side is asked to look at: the descriptor's identity and a fresh challenge.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Probe {
    pub windows_pid: u32,
    pub image: String,
    #[serde(rename = "creationTime100ns")]
    pub creation_time_100ns: String,
    pub image_sha256: String,
    pub session_nonce: String,
    pub pipe: String,
    pub challenge: String,
}

impl Probe {
    pub fn of(d: &Descriptor, challenge: String) -> Probe {
        Probe {
            windows_pid: d.windows_pid,
            image: d.image.clone(),
            creation_time_100ns: d.creation_time_100ns.clone(),
            image_sha256: d.image_sha256.clone(),
            session_nonce: d.session_nonce.clone(),
            pipe: d.pipe.clone(),
            challenge,
        }
    }
}

/// The control pipe's answer (the process's own view of itself).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PipeAnswer {
    pub session_nonce: String,
    /// The caller's challenge, echoed.
    pub challenge: String,
    pub windows_pid: u32,
    #[serde(rename = "creationTime100ns")]
    pub creation_time_100ns: String,
    pub image: String,
    pub image_sha256: String,
    pub managed_ready: bool,
    pub session_mode: SessionMode,
    pub runtime_version: String,
}

/// What the Windows side saw: the held process (OS view) and its pipe's answer.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Seen {
    pub windows_pid: u32,
    pub image: String,
    #[serde(rename = "creationTime100ns")]
    pub creation_time_100ns: String,
    /// SHA-256 of the image file, hashed by the Windows side.
    pub image_sha256: String,
    pub pipe: PipeAnswer,
}

/// The control pipe's request line (native/core/src/debug_descriptor.h), without its newline.
pub fn pipe_request(nonce: &str, challenge: &str) -> String {
    serde_json::json!({ "v": 1, "op": "identify", "nonce": nonce, "challenge": challenge }).to_string()
}

/// The control pipe's answer line. Its refusal of the nonce is identity_changed (`nonce`); an answer that is not the
/// protocol is identity_changed (`pipe_answer`): whatever serves that pipe is not a Kiln core of this protocol.
pub fn parse_pipe_answer(line: &str) -> Result<PipeAnswer, DebugError> {
    let bad = |why: &str| DebugError::identity_changed(format!("the control pipe's answer is not Kiln's ({why})"), "pipe_answer");
    let v: serde_json::Value = serde_json::from_str(line).map_err(|e| bad(&e.to_string()))?;
    if v["v"] != 1 {
        return Err(bad("protocol version"));
    }
    if v["ok"] != true {
        let code = v["code"].as_str().unwrap_or("?");
        let message = v["message"].as_str().unwrap_or("");
        return Err(match code {
            "identity_changed" => DebugError::identity_changed("the control pipe has another session nonce", "nonce"),
            _ => DebugError::new(DebugCode::UnsupportedIdentity, format!("the control pipe refused the request ({code}: {message})")).because("pipe_refused"),
        });
    }
    let id = &v["identity"];
    let text = |x: &serde_json::Value, what: &str| x.as_str().map(str::to_string).ok_or_else(|| bad(what));
    Ok(PipeAnswer {
        session_nonce: text(&id["sessionNonce"], "sessionNonce")?,
        challenge: text(&v["challenge"], "challenge")?,
        windows_pid: id["windowsPid"].as_u64().and_then(|p| u32::try_from(p).ok()).ok_or_else(|| bad("windowsPid"))?,
        creation_time_100ns: text(&id["creationTime100ns"], "creationTime100ns")?,
        image: text(&id["image"], "image")?,
        image_sha256: text(&id["imageSha256"], "imageSha256")?,
        managed_ready: v["managedReady"].as_bool().ok_or_else(|| bad("managedReady"))?,
        session_mode: SessionMode::parse(v["sessionMode"].as_str().unwrap_or("unknown")),
        runtime_version: v["runtimeVersion"].as_str().unwrap_or_default().to_string(),
    })
}

/// The time limits of the Windows side, in one place. Each one is longer than everything it waits for, so a slow step
/// times out where it happens and the step above it still answers in time. A watch poll that times out proves nothing
/// ([`WatchOutcome::Unknown`]): the session goes on and the next poll asks again.
///
/// | limit | value | what it bounds |
/// |---|---|---|
/// | [`PIPE_CONNECT`](limits::PIPE_CONNECT) | 0.5 s | waiting for the control pipe's one instance while it serves another client |
/// | [`PIPE_ANSWER`](limits::PIPE_ANSWER) | 1.5 s | the core's identify answer on a connected pipe (its control worker answers from snapshots) |
/// | [`HELPER_WATCH_ANSWER`](limits::HELPER_WATCH_ANSWER) | 2.5 s | the helper's answer to `watch_exit`: one pipe exchange (at most 2 s) and the process queries; the image digest is the held handle's, not hashed again |
/// | [`WATCH_EVERY`](limits::WATCH_EVERY) | 3 s | the watch poll period, from the start of one poll to the next: longer than a poll can take |
/// | [`HELPER_ANSWER`](limits::HELPER_ANSWER) | 5 s | the helper's answer to `inspect` and `prepare_attach`: one pipe exchange, the process queries and one image hash (chapter 12a §4 step 4: one response within 5 s) |
/// | [`HELPER_FIRST_ANSWER`](limits::HELPER_FIRST_ANSWER) | 15 s | the helper's first answer, which includes starting it under its runner |
///
/// So: pipe exchange (PIPE_CONNECT + PIPE_ANSWER) < the helper's limit for that request < the watch poll period. The
/// native Windows CLI asks in-process: the pipe limits are its only ones.
pub mod limits {
    use std::time::Duration;

    pub const PIPE_CONNECT: Duration = Duration::from_millis(500);
    pub const PIPE_ANSWER: Duration = Duration::from_millis(1500);
    pub const HELPER_WATCH_ANSWER: Duration = Duration::from_millis(2500);
    pub const WATCH_EVERY: Duration = Duration::from_secs(3);
    pub const HELPER_ANSWER: Duration = Duration::from_secs(5);
    pub const HELPER_FIRST_ANSWER: Duration = Duration::from_secs(15);

    const _: () = assert!(PIPE_CONNECT.as_millis() + PIPE_ANSWER.as_millis() < HELPER_WATCH_ANSWER.as_millis());
    const _: () = assert!(HELPER_WATCH_ANSWER.as_millis() < WATCH_EVERY.as_millis());
    const _: () = assert!(PIPE_CONNECT.as_millis() + PIPE_ANSWER.as_millis() < HELPER_ANSWER.as_millis());
    const _: () = assert!(HELPER_ANSWER.as_millis() < HELPER_FIRST_ANSWER.as_millis());
}

/// A watch poll: the held process ended, or it is still there (and answered again).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Watch {
    Exited,
    Alive(Seen),
}

/// The Windows side of one namespace (helper protocol v1: `inspect`, `prepare_attach`, `watch_exit`, `close`).
/// Implementations hold the inspected process's handle (query + synchronize) until `close` or the next `inspect`, and
/// never terminate anything.
pub trait ProcessSource {
    /// Whether this side runs elevated (native Windows token); None when it cannot tell (Wine).
    fn elevated(&mut self) -> Option<bool>;
    /// Opens and holds the probe's process, reads its identity and challenges its pipe.
    fn inspect(&mut self, probe: &Probe) -> Result<Seen, DebugError>;
    /// The same on the held process, right before the adapter starts.
    fn prepare_attach(&mut self, probe: &Probe) -> Result<Seen, DebugError>;
    /// Whether the held process ended; if not, its pipe answers the probe's challenge again.
    fn watch(&mut self, probe: &Probe) -> Result<Watch, DebugError>;
    /// Releases the handle (and ends a helper). Never a terminate call.
    fn close(&mut self);
}

/// The OS's creation time of the process the probe's PID names against the descriptor's: another one proves that PID is
/// another process now (the descriptor is stale, see `proves_stale`). The Windows side checks it right after
/// `GetProcessTimes`, before any image or pipe work (a reused PID serves no pipe and may have an unreadable image).
pub fn check_creation(p: &Probe, created: &str) -> Result<(), DebugError> {
    if created != p.creation_time_100ns {
        return Err(DebugError::identity_changed(
            format!(
                "PID {} is another process now (created {created}, the descriptor's was created {}): the game was restarted or the PID reused",
                p.windows_pid, p.creation_time_100ns
            ),
            "creation_time",
        ));
    }
    Ok(())
}

/// Every Windows-side fact against the probe (the descriptor): the OS view and the pipe's answer must both be that
/// process. Used by the resolver and by the helper (which refuses before answering).
pub fn check_seen(p: &Probe, s: &Seen) -> Result<(), DebugError> {
    let pid = p.windows_pid;
    if s.windows_pid != pid {
        return Err(DebugError::identity_changed(format!("the Windows side opened PID {} for PID {pid}", s.windows_pid), "pid"));
    }
    check_creation(p, &s.creation_time_100ns)?;
    if !same_windows_path(&s.image, &p.image) {
        return Err(DebugError::identity_changed(format!("PID {pid} runs {}, the descriptor names {}", s.image, p.image), "image_path"));
    }
    if !s.image_sha256.eq_ignore_ascii_case(&p.image_sha256) {
        return Err(DebugError::identity_changed(format!("the image of PID {pid} is not the file the descriptor hashed (another build?)"), "image_digest"));
    }
    let a = &s.pipe;
    if a.session_nonce != p.session_nonce {
        return Err(DebugError::identity_changed(format!("the control pipe of PID {pid} answered another session nonce"), "nonce"));
    }
    if a.challenge != p.challenge {
        return Err(DebugError::identity_changed(format!("the control pipe of PID {pid} did not echo the challenge"), "challenge"));
    }
    if a.windows_pid != pid || a.creation_time_100ns != p.creation_time_100ns || !same_windows_path(&a.image, &p.image) || !a.image_sha256.eq_ignore_ascii_case(&p.image_sha256) {
        return Err(DebugError::identity_changed(format!("the control pipe named in the descriptor of PID {pid} is served by another process"), "pipe_identity"));
    }
    Ok(())
}

/// The host-side facts of a target. `facts` is read again at every check: the prefix and runner are part of the tuple.
pub trait HostSide {
    fn spec(&self) -> &HostSpec;
    fn facts(&mut self) -> Result<HostFacts, DebugError>;
}

/// What does not change while a target is resolved.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HostSpec {
    pub kind: TargetKind,
    /// `kiln_manage::game::install_id_for(<install>)`.
    pub install_id: String,
    /// `<install>/kiln/state` (descriptors are in its `debug/`).
    pub state_dir: PathBuf,
    /// The host path of the install's executable (a descriptor of another folder's exe is not this install's).
    pub install_exe: PathBuf,
    /// The Wine/Proton prefix the target runs in; None on native Windows.
    pub prefix: Option<PathBuf>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct HostFacts {
    pub prefix_identity: Option<PrefixIdentity>,
    pub runner_sha256: Option<String>,
    pub runner_context: Option<RunnerContext>,
    /// Display only.
    pub host_pid: Option<u32>,
}

/// One proven target.
#[derive(Debug, Clone)]
pub struct Resolved {
    pub kind: TargetKind,
    pub target: DebugTarget,
    pub descriptor: Descriptor,
    pub descriptor_path: PathBuf,
    /// Descriptors that were not used, and why (stale, another install, unreadable).
    pub notes: Vec<String>,
}

/// A fresh 128-bit challenge, 32 hex digits.
pub fn new_challenge() -> Result<String, DebugError> {
    crate::util::random_bytes::<16>()
        .map(|b| crate::hash::hex(&b))
        .map_err(|e| DebugError::new(DebugCode::UnsupportedIdentity, format!("no random challenge: {}", e.message)))
}

fn what(kind: TargetKind) -> &'static str {
    match kind {
        TargetKind::Server => "server",
        TargetKind::Client => "game",
    }
}

fn identity_from(spec: &HostSpec, facts: &HostFacts, seen: &Seen) -> DebugIdentity {
    DebugIdentity {
        install_id: spec.install_id.clone(),
        prefix_identity: facts.prefix_identity.clone(),
        runner_sha256: facts.runner_sha256.clone(),
        windows_pid: seen.windows_pid,
        creation_time_100ns: seen.creation_time_100ns.clone(),
        image_sha256: seen.image_sha256.to_ascii_lowercase(),
        session_nonce: seen.pipe.session_nonce.clone(),
    }
}

fn inspect_checked(source: &mut dyn ProcessSource, probe: &Probe) -> Result<Seen, DebugError> {
    let seen = source.inspect(probe)?;
    check_seen(probe, &seen)?;
    Ok(seen)
}

/// Whether a refusal proves the descriptor stale: the Windows side found no running process of that PID, or the
/// process of that PID was created at another time. Nothing else is proof (a pipe that did not answer, a transport
/// error, another nonce, rights, an unsupported query). Even these two count only from the target's namespace (see
/// [`resolve`]).
fn proves_stale(e: &DebugError) -> bool {
    e.code == DebugCode::NotRunning || (e.code == DebugCode::IdentityChanged && e.reason.as_deref() == Some("creation_time"))
}

/// Removes a descriptor proven stale, if the file still holds that descriptor (a process that got the same PID since
/// writes its own file under the same name). Best effort.
fn remove_stale(path: &Path, d: &Descriptor) -> bool {
    let same = std::fs::read(path).ok().and_then(|b| serde_json::from_slice::<Descriptor>(&b).ok()).is_some_and(|now| now == *d);
    same && std::fs::remove_file(path).is_ok()
}

/// The install's one live, matching descriptor as a target. The source holds that process afterwards. Descriptors
/// this call proved stale (a crashed start leaves its file behind) are removed afterwards, whatever the outcome, and
/// named in `notes`; no other descriptor is ever removed.
///
/// "No process of that PID" and "another creation time" are proof only from the target's namespace: when this call
/// proved another descriptor of the install live, or the host side's facts hold (a process of this install runs in
/// its prefix, so the helper under that runner joined its wineserver; the client prefix's runner context exists;
/// native Windows has one namespace). A helper in another wineserver (wrong prefix or runner) sees an empty or
/// unrelated process table, where a fresh wineserver hands out the same low PIDs again: neither answer means anything
/// there, and then nothing is removed.
pub fn resolve(
    host: &mut dyn HostSide,
    source: &mut dyn ProcessSource,
    challenge: &mut dyn FnMut() -> Result<String, DebugError>,
) -> Result<Resolved, DebugError> {
    let mut proven = Vec::new();
    let mut r = resolve_inner(host, source, challenge, &mut proven);
    for (path, d) in &proven {
        if remove_stale(path, d)
            && let Ok(r) = r.as_mut()
        {
            r.notes.push(format!("{}: removed (its process ended)", path.display()));
        }
    }
    r
}

fn resolve_inner(
    host: &mut dyn HostSide,
    source: &mut dyn ProcessSource,
    challenge: &mut dyn FnMut() -> Result<String, DebugError>,
    proven: &mut Vec<(PathBuf, Descriptor)>,
) -> Result<Resolved, DebugError> {
    let spec = host.spec().clone();
    let kind = spec.kind;
    let dir = spec.state_dir.join(DESCRIPTOR_DIR);
    let files = read_descriptors(&spec.state_dir);
    if files.is_empty() {
        return Err(DebugError::refused(
            format!(
                "no debug descriptor in {}: start the {} with developer debugging on (kiln.json \"developer_debugging\": true, or the launcher's Developer debugging switch)",
                dir.display(),
                what(kind)
            ),
            "developer_debugging_off",
        ));
    }
    let mut notes = Vec::new();
    let mut candidates = Vec::new();
    for f in files {
        match f.read {
            Err(why) => notes.push(format!("{}: {why}", f.path.display())),
            Ok(d) if d.side != kind => notes.push(format!("{}: a {} descriptor", f.path.display(), d.side.as_str())),
            Ok(d) if !image_in_install(&d.image, &spec.install_exe, spec.prefix.as_deref()) => {
                notes.push(format!("{}: names {}, not this install's {}", f.path.display(), d.image, spec.install_exe.display()))
            }
            Ok(d) => candidates.push((f.path, d)),
        }
    }
    if candidates.is_empty() {
        return Err(DebugError::refused(format!("no usable debug descriptor in {} ({})", dir.display(), notes.join("; ")), "descriptor_invalid"));
    }
    let me_elevated = source.elevated();
    let mut live: Vec<(PathBuf, Descriptor, Seen)> = Vec::new();
    let mut stale = Vec::new();
    // descriptors that could not be checked (rights, an elevated target, a failed query, no answer): a note when another
    // one is the live target, the refusal when none is
    let mut unchecked: Vec<(String, DebugError)> = Vec::new();
    let mut last = None;
    for (i, (path, d)) in candidates.iter().enumerate() {
        if d.elevated && me_elevated == Some(false) {
            let e = DebugError::new(
                DebugCode::PrivilegeMismatch,
                format!("the {} (PID {}) runs elevated and this kiln does not; a debugger could not attach to it", what(kind), d.windows_pid),
            )
            .because("elevated_target");
            unchecked.push((format!("{}: {}", path.display(), e.message), e));
            continue;
        }
        let probe = Probe::of(d, challenge()?);
        last = Some(i);
        match inspect_checked(source, &probe) {
            Ok(seen) => live.push((path.clone(), d.clone(), seen)),
            Err(e) if matches!(e.code, DebugCode::NotRunning | DebugCode::IdentityChanged) => {
                if proves_stale(&e) {
                    proven.push((path.clone(), d.clone()));
                }
                stale.push(format!("{}: {}", path.display(), e.message))
            }
            Err(e) => unchecked.push((format!("{}: {}", path.display(), e.message), e)),
        }
    }
    notes.extend(stale.iter().cloned());
    notes.extend(unchecked.iter().map(|(n, _)| n.clone()));
    // no descriptor proved the namespace: the host side must (see resolve), or the stale answers prove nothing
    let mut kept = String::new();
    if !proven.is_empty()
        && live.is_empty()
        && let Err(e) = host.facts()
    {
        proven.clear();
        kept = format!("; their files are kept: nothing shows the {}'s Windows side alive ({}), so these answers prove nothing", what(kind), e.message);
    }
    let (path, d, mut seen) = match live.len() {
        // the first descriptor that could not be checked says why nothing could be proven ("not running" could be wrong)
        0 if !unchecked.is_empty() => return Err(unchecked.swap_remove(0).1),
        0 => {
            return Err(DebugError::new(
                DebugCode::NotRunning,
                format!("the {} is not running: the debug descriptors in {} belong to processes that ended or whose PID is another process now ({}){kept}", what(kind), dir.display(), stale.join("; ")),
            )
            .because("stale_descriptor"));
        }
        1 => live.pop().expect("one"),
        n => {
            let pids: Vec<u32> = live.iter().map(|(_, d, _)| d.windows_pid).collect();
            return Err(DebugError::refused(format!("{n} running processes of this install have a debug descriptor (Windows PIDs {pids:?}); stop the extra ones"), "several_descriptors"));
        }
    };
    // the source holds the process it inspected last: make that the chosen one
    let chosen = candidates.iter().position(|(p, _)| *p == path);
    if chosen != last {
        seen = inspect_checked(source, &Probe::of(&d, challenge()?))?;
    }
    if !seen.pipe.managed_ready {
        return Err(DebugError::new(
            DebugCode::RuntimeNotReady,
            format!("the .NET host of the {} (PID {}) is not running (kiln.log, [clr] lines; kiln.json \"clr\"): there are no managed mods to debug", what(kind), d.windows_pid),
        ));
    }
    let facts = host.facts()?;
    let identity = identity_from(&spec, &facts, &seen);
    let target = DebugTarget {
        identity,
        image: seen.image.clone(),
        runtime_version: seen.pipe.runtime_version.clone(),
        managed_ready: seen.pipe.managed_ready,
        session_mode: seen.pipe.session_mode,
        runner_context: facts.runner_context,
        host_pid: facts.host_pid,
    };
    Ok(Resolved { kind, target, descriptor: d, descriptor_path: path, notes })
}

/// The whole tuple again, from fresh host facts and a fresh `prepare_attach`: what was resolved must still be what
/// runs. Returns what the Windows side saw (its session mode is the current one).
pub fn revalidate(
    r: &Resolved,
    host: &mut dyn HostSide,
    source: &mut dyn ProcessSource,
    challenge: &mut dyn FnMut() -> Result<String, DebugError>,
) -> Result<Seen, DebugError> {
    let probe = Probe::of(&r.descriptor, challenge()?);
    let seen = source.prepare_attach(&probe)?;
    check_seen(&probe, &seen)?;
    let facts = host.facts()?;
    let now = identity_from(host.spec(), &facts, &seen);
    if let Some(field) = r.target.identity.first_difference(&now) {
        return Err(DebugError::identity_changed(format!("the {} is no longer the process that was resolved ({field} differs); resolve it again", what(r.kind)), field));
    }
    if !seen.pipe.managed_ready {
        return Err(DebugError::new(DebugCode::RuntimeNotReady, format!("the .NET host of the {} stopped answering as ready", what(r.kind))));
    }
    Ok(seen)
}

/// What a watch poll means for an attached session. Only `Gone` and `Changed` end it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum WatchOutcome {
    /// Still the resolved process; its current session mode.
    Same(SessionMode),
    /// It ended.
    Gone,
    /// A field of the resolved identity is proven different (the reason names it, see [`PROOF`]).
    Changed(String),
    /// This poll proved nothing either way (a timeout, a transport error, a pipe or query that failed): the session
    /// goes on and the next poll asks again (reason).
    Unknown(String),
}

/// The refusals that prove the process answering is not the resolved one, by the field that differs: the PID opened,
/// its creation time, image path or digest, the pipe's nonce, its echo of the challenge, the process its answer names
/// or the process serving it. Anything else (no answer in time, a pipe that is not there or busy, an answer that is not
/// the protocol, rights, a failed query) is not proof.
pub const PROOF: [&str; 8] = ["pid", "creation_time", "image_path", "image_digest", "nonce", "challenge", "pipe_identity", "pipe_server"];

fn proves_changed(e: &DebugError) -> bool {
    e.code == DebugCode::IdentityChanged && e.reason.as_deref().is_some_and(|r| PROOF.contains(&r))
}

/// One watch poll of the held process (helper `watch_exit`).
pub fn watch(r: &Resolved, source: &mut dyn ProcessSource, challenge: &mut dyn FnMut() -> Result<String, DebugError>) -> WatchOutcome {
    let probe = match challenge() {
        Ok(c) => Probe::of(&r.descriptor, c),
        Err(e) => return WatchOutcome::Unknown(e.reason.unwrap_or(e.message)),
    };
    let seen = source.watch(&probe).and_then(|w| match w {
        Watch::Exited => Ok(None),
        Watch::Alive(seen) => check_seen(&probe, &seen).map(|()| Some(seen.pipe.session_mode)),
    });
    match seen {
        Ok(Some(mode)) => WatchOutcome::Same(mode),
        Ok(None) => WatchOutcome::Gone,
        Err(e) if e.code == DebugCode::NotRunning => WatchOutcome::Gone,
        Err(e) if proves_changed(&e) => WatchOutcome::Changed(e.reason.unwrap_or(e.message)),
        Err(e) => WatchOutcome::Unknown(e.reason.unwrap_or(e.message)),
    }
}

/// The consent rule (chapter 12a §4 step 7, §5): every managed attach can stop the process, so a client is attached
/// only with explicit consent. Servers keep their own pause warning.
pub fn consent_gate(kind: TargetKind, allow_client_pause: bool) -> Result<(), DebugError> {
    if kind == TargetKind::Client && !allow_client_pause {
        return Err(DebugError::new(
            DebugCode::PauseConsentRequired,
            "a debugger stops the game at every breakpoint, and a stopped game can freeze or lose its connection: attach to a client only with --allow-client-pause (allowClientPause: true in the editor), or use --observe for logs and counters without a debugger",
        ));
    }
    Ok(())
}

/// Proof that consent and the whole tuple were checked right now. The only way to start an adapter.
#[derive(Debug)]
pub struct AttachPermit {
    identity: DebugIdentity,
    kind: TargetKind,
    session_mode: SessionMode,
}

impl AttachPermit {
    /// The PID the adapter attaches to (Windows numbering, in the target's namespace).
    pub fn windows_pid(&self) -> u32 {
        self.identity.windows_pid
    }
    pub fn identity(&self) -> &DebugIdentity {
        &self.identity
    }
    pub fn kind(&self) -> TargetKind {
        self.kind
    }
    pub fn session_mode(&self) -> SessionMode {
        self.session_mode
    }
}

/// Consent, then the tuple again ([`revalidate`]). Every path that starts netcoredbg goes through here.
pub fn permit(
    r: &Resolved,
    allow_client_pause: bool,
    host: &mut dyn HostSide,
    source: &mut dyn ProcessSource,
    challenge: &mut dyn FnMut() -> Result<String, DebugError>,
) -> Result<AttachPermit, DebugError> {
    consent_gate(r.kind, allow_client_pause)?;
    if host.spec().kind != r.kind {
        return Err(DebugError::refused("the target was resolved for another kind of process", "kind"));
    }
    let seen = revalidate(r, host, source, challenge)?;
    Ok(AttachPermit { identity: r.target.identity.clone(), kind: r.kind, session_mode: seen.pipe.session_mode })
}

// ---- server lookup (moved from kiln-cli's debug.rs, unchanged in behaviour) ----

/// A path inside a Steam library: `.../steamapps/common/<app>`, `.../steamapps/compatdata/...`.
pub fn in_steam_library(p: &Path) -> bool {
    p.components().any(|c| matches!(c, Component::Normal(n) if n.to_str().is_some_and(|s| s.eq_ignore_ascii_case("steamapps"))))
}

fn canon(p: &Path) -> PathBuf {
    p.canonicalize().unwrap_or_else(|_| p.to_path_buf())
}

/// The server guard (chapter 12a §4 step 6): a server inside a Steam library (where the user's live server lives) is
/// always refused, by the path as given and as it resolves. A client is never checked here.
pub fn server_steam_guard(p: &Path) -> Result<(), DebugError> {
    if in_steam_library(p) || in_steam_library(&canon(p)) {
        return Err(DebugError::refused(
            format!("{} is inside a Steam library; kiln debug pauses the server for everyone on it, so it refuses", p.display()),
            "steam_library",
        ));
    }
    Ok(())
}

/// The Linux PID of the server started from `dir` in its own `prefix`: the runner context of a Wine server, display
/// only. No wine runs until this found one, so a stopped server gets no stray wineserver.
pub fn server_host_pid(dir: &Path, prefix: &Path) -> Result<u32, DebugError> {
    let pids = crate::server::procs::find_servers(prefix);
    let want = canon(dir);
    let in_dir: Vec<u32> = pids.iter().copied().filter(|p| std::fs::read_link(format!("/proc/{p}/cwd")).is_ok_and(|c| canon(&c) == want)).collect();
    match in_dir.as_slice() {
        [one] => Ok(*one),
        [] if pids.is_empty() => Err(DebugError::new(DebugCode::NotRunning, format!("{} is not running; start it first (kiln run)", dir.display()))),
        [] => Err(DebugError::refused(format!("the server running in {} (pid {pids:?}) was not started from {}", prefix.display(), dir.display()), "other_folder")),
        more => Err(DebugError::refused(format!("{} server processes run in {} ({more:?}); stop the extra ones", more.len(), prefix.display()), "several_servers")),
    }
}

/// The running process's own wine: `/proc/<pid>/exe` is its preloader (`<GE>/files/lib/wine/i386-unix/wine64-preloader`,
/// `/usr/lib/wine/wine64-preloader`), below the `bin/wine` of that build. A different wine build could not talk to the
/// prefix's wineserver at all.
#[cfg(target_os = "linux")]
pub fn wine_of(pid: u32) -> Option<PathBuf> {
    let exe = std::fs::read_link(format!("/proc/{pid}/exe")).ok()?;
    wine_above(&exe)
}

#[cfg(not(target_os = "linux"))]
pub fn wine_of(_pid: u32) -> Option<PathBuf> {
    None
}

pub fn wine_above(exe: &Path) -> Option<PathBuf> {
    let usable = |w: &PathBuf| w.is_file() && crate::util::is_executable(w);
    // `bin/wine` first: GE-Proton also has a `lib/wine/i386-unix/wine` next to the preloader, which is not what started it
    exe.ancestors().skip(1).map(|a| a.join("bin/wine")).find(usable).or_else(|| exe.parent().map(|d| d.join("wine")).filter(usable))
}

/// SHA-256 of a runner's executable (the `runnerSha256` of the tuple).
pub fn runner_sha256(runner: &Path) -> Result<String, DebugError> {
    crate::hash::sha256_file(runner)
        .map(|(h, _)| h)
        .map_err(|e| DebugError::new(DebugCode::ToolMissing, format!("cannot hash the runner {}: {}", runner.display(), e.message)))
}

/// A fake Windows side and host side for tests (here and in kiln-cli): a process table the test edits between steps.
pub mod fake {
    use super::*;
    use crate::debug::windows::{DigestCache, HeldProcess, Hashing, look};

    #[derive(Debug, Clone)]
    pub struct FakePipe {
        pub name: String,
        pub session_nonce: String,
        pub managed_ready: bool,
        pub session_mode: SessionMode,
        pub runtime_version: String,
        /// false: answers with another challenge (a replayed answer).
        pub echo: bool,
    }

    #[derive(Debug, Clone)]
    pub struct FakeProc {
        pub windows_pid: u32,
        pub image: String,
        pub creation_time_100ns: String,
        pub image_sha256: String,
        pub pipe: Option<FakePipe>,
        /// OpenProcess is refused (another user, elevated).
        pub denied: bool,
        /// Its image file cannot be read (hashing it fails).
        pub unreadable: bool,
    }

    impl FakeProc {
        /// A Kiln process with developer debugging on, and its descriptor.
        pub fn kiln(windows_pid: u32, image: &str, creation: u64, sha: &str, nonce: &str, side: TargetKind) -> (FakeProc, Descriptor) {
            let pipe = pipe_name(windows_pid, nonce);
            let p = FakeProc {
                windows_pid,
                image: image.to_string(),
                creation_time_100ns: creation.to_string(),
                image_sha256: sha.to_string(),
                pipe: Some(FakePipe { name: pipe.clone(), session_nonce: nonce.into(), managed_ready: true, session_mode: SessionMode::Hosted, runtime_version: "8.0.11".into(), echo: true }),
                denied: false,
                unreadable: false,
            };
            let d = Descriptor {
                format: DESCRIPTOR_FORMAT,
                kiln: "0.2.0-dev".into(),
                side,
                session_nonce: nonce.into(),
                windows_pid,
                creation_time_100ns: creation.to_string(),
                image: image.to_string(),
                image_sha256: sha.to_string(),
                runtime_version: "8.0.11".into(),
                pipe,
                elevated: false,
                managed_ready: true,
                written: "2026-11-13T20:00:00Z".into(),
            };
            (p, d)
        }
    }

    type Hook = Box<dyn FnMut(&mut Vec<FakeProc>) + Send>;

    /// One held row of the table, as the OS calls of [`crate::debug::windows::look`] see it.
    struct FakeHeld<'a> {
        p: &'a FakeProc,
        hashes: &'a mut u32,
    }

    impl HeldProcess for FakeHeld<'_> {
        fn pid(&self) -> u32 {
            self.p.windows_pid
        }
        fn exited(&mut self) -> bool {
            false // an exit is the row leaving the table
        }
        fn creation_time_100ns(&mut self) -> Result<String, DebugError> {
            Ok(self.p.creation_time_100ns.clone())
        }
        fn image(&mut self) -> Result<String, DebugError> {
            Ok(self.p.image.clone())
        }
        fn hash_image(&mut self, image: &str) -> Result<String, DebugError> {
            *self.hashes += 1;
            if self.p.unreadable {
                return Err(DebugError::new(DebugCode::UnsupportedIdentity, format!("the image of PID {} ({image}) cannot be read", self.p.windows_pid)).because("image_read"));
            }
            Ok(self.p.image_sha256.clone())
        }
        fn ask_pipe(&mut self, probe: &Probe) -> Result<PipeAnswer, DebugError> {
            let p = self.p;
            let pid = p.windows_pid;
            let pipe = p.pipe.as_ref().filter(|x| x.name == probe.pipe).ok_or_else(|| {
                DebugError::identity_changed(format!("PID {pid} serves no pipe {}", probe.pipe), "pipe_missing")
            })?;
            Ok(PipeAnswer {
                session_nonce: pipe.session_nonce.clone(),
                challenge: if pipe.echo { probe.challenge.clone() } else { "0".repeat(32) },
                windows_pid: pid,
                creation_time_100ns: p.creation_time_100ns.clone(),
                image: p.image.clone(),
                image_sha256: p.image_sha256.clone(),
                managed_ready: pipe.managed_ready,
                session_mode: pipe.session_mode,
                runtime_version: pipe.runtime_version.clone(),
            })
        }
    }

    /// The Windows side of one namespace, from a table. Records every call; never kills anything (it cannot).
    #[derive(Default)]
    pub struct FakeSource {
        pub procs: Vec<FakeProc>,
        pub elevated: Option<bool>,
        /// Every call answers this error (a source that cannot work: unsupported_identity, tool_missing).
        pub fail: Option<DebugError>,
        /// Runs before every `prepare_attach`: the table changes between resolve and attach.
        pub before_prepare: Option<Hook>,
        pub calls: Vec<String>,
        pub held: Option<u32>,
        /// How often an image was hashed.
        pub hashes: u32,
        /// The image digest of the held row.
        digest: DigestCache,
    }

    impl FakeSource {
        pub fn new(procs: Vec<FakeProc>) -> FakeSource {
            FakeSource { procs, ..Default::default() }
        }

        fn look(&mut self, probe: &Probe, hashing: Hashing) -> Result<Seen, DebugError> {
            if let Some(e) = &self.fail {
                return Err(e.clone());
            }
            let pid = probe.windows_pid;
            let Some(p) = self.procs.iter().find(|p| p.windows_pid == pid) else {
                self.held = None;
                return Err(DebugError::new(DebugCode::NotRunning, format!("no process {pid}")));
            };
            if p.denied {
                return Err(DebugError::new(DebugCode::PrivilegeMismatch, format!("OpenProcess({pid}): access denied")).because("open_denied"));
            }
            if self.held != Some(pid) {
                self.digest = DigestCache::default(); // another handle
            }
            self.held = Some(pid);
            look(&mut FakeHeld { p, hashes: &mut self.hashes }, probe, &mut self.digest, hashing)
        }
    }

    impl ProcessSource for FakeSource {
        fn elevated(&mut self) -> Option<bool> {
            self.elevated
        }
        fn inspect(&mut self, probe: &Probe) -> Result<Seen, DebugError> {
            self.calls.push(format!("inspect {}", probe.windows_pid));
            self.digest = DigestCache::default(); // inspect opens a new handle
            self.look(probe, Hashing::Fresh)
        }
        fn prepare_attach(&mut self, probe: &Probe) -> Result<Seen, DebugError> {
            self.calls.push(format!("prepare_attach {}", probe.windows_pid));
            if let Some(h) = self.before_prepare.as_mut() {
                h(&mut self.procs);
            }
            if self.held != Some(probe.windows_pid) {
                return Err(DebugError::identity_changed(format!("PID {} is not the held process", probe.windows_pid), "not_held"));
            }
            self.look(probe, Hashing::Fresh)
        }
        fn watch(&mut self, probe: &Probe) -> Result<Watch, DebugError> {
            self.calls.push(format!("watch {}", probe.windows_pid));
            if !self.procs.iter().any(|p| p.windows_pid == probe.windows_pid && Some(p.windows_pid) == self.held) {
                return Ok(Watch::Exited);
            }
            self.look(probe, Hashing::Cached).map(Watch::Alive)
        }
        fn close(&mut self) {
            self.calls.push("close".into());
            self.held = None;
            self.digest = DigestCache::default();
        }
    }

    /// A host side whose facts a test can change between resolve and attach.
    pub struct FakeHost {
        pub spec: HostSpec,
        pub facts: HostFacts,
        pub calls: usize,
        /// Facts from the second call on (the prefix replaced, another runner).
        pub later: Option<HostFacts>,
    }

    impl HostSide for FakeHost {
        fn spec(&self) -> &HostSpec {
            &self.spec
        }
        fn facts(&mut self) -> Result<HostFacts, DebugError> {
            self.calls += 1;
            Ok(match (&self.later, self.calls) {
                (Some(l), n) if n > 1 => l.clone(),
                _ => self.facts.clone(),
            })
        }
    }

    /// Challenges `c-1`, `c-2`, ... (distinct, predictable for tests).
    pub fn counter() -> impl FnMut() -> Result<String, DebugError> {
        let mut n = 0;
        move || {
            n += 1;
            Ok(format!("c-{n}"))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::fake::*;
    use super::*;

    const SHA_A: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const N1: &str = "11111111111111111111111111111111";
    const N2: &str = "22222222222222222222222222222222";

    /// Two installs `a` and `b` with the same exe name, each with its kiln/state.
    struct World {
        root: PathBuf,
    }

    impl World {
        fn new(tag: &str) -> World {
            let root = std::env::temp_dir().join(format!("kiln-debug-res-{tag}-{}", std::process::id()));
            let _ = std::fs::remove_dir_all(&root);
            for d in ["a", "b"] {
                std::fs::create_dir_all(root.join(d).join("kiln/state/debug")).unwrap();
                std::fs::write(root.join(d).join("enshrouded_server.exe"), b"MZ").unwrap();
            }
            World { root }
        }
        fn image(&self, d: &str) -> String {
            crate::winpath::unix_to_z(&self.root.join(d).join("enshrouded_server.exe")).unwrap()
        }
        fn put(&self, d: &str, desc: &Descriptor) {
            let f = self.root.join(d).join("kiln/state/debug").join(format!("{}.json", desc.windows_pid));
            std::fs::write(f, serde_json::to_vec(desc).unwrap()).unwrap();
        }
        fn host(&self, d: &str, kind: TargetKind) -> FakeHost {
            FakeHost {
                spec: HostSpec {
                    kind,
                    install_id: format!("install-{d}"),
                    state_dir: self.root.join(d).join("kiln/state"),
                    install_exe: self.root.join(d).join("enshrouded_server.exe"),
                    prefix: None,
                },
                facts: HostFacts {
                    prefix_identity: Some(PrefixIdentity { path: format!("/pfx/{d}").into(), device: 1, inode: 10 }),
                    runner_sha256: Some("r".repeat(64)),
                    runner_context: Some(RunnerContext { linux_boot_id: "boot".into(), pid: 4000, start_ticks: 7 }),
                    host_pid: Some(4001),
                },
                calls: 0,
                later: None,
            }
        }
        fn kiln(&self, d: &str, pid: u32, creation: u64, nonce: &str) -> (FakeProc, Descriptor) {
            FakeProc::kiln(pid, &self.image(d), creation, SHA_A, nonce, TargetKind::Server)
        }
    }

    impl Drop for World {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.root);
        }
    }

    fn run(host: &mut FakeHost, src: &mut FakeSource) -> Result<Resolved, DebugError> {
        resolve(host, src, &mut counter())
    }

    #[test]
    fn the_one_live_descriptor_is_the_target() {
        let w = World::new("one");
        let (p, d) = w.kiln("a", 32, 134_000, N1);
        w.put("a", &d);
        let mut src = FakeSource::new(vec![p]);
        let mut host = w.host("a", TargetKind::Server);
        let r = run(&mut host, &mut src).unwrap();
        let id = &r.target.identity;
        assert_eq!((id.windows_pid, id.creation_time_100ns.as_str(), id.session_nonce.as_str(), id.install_id.as_str()), (32, "134000", N1, "install-a"));
        assert_eq!(r.target.host_pid, Some(4001));
        assert_eq!(r.target.session_mode, SessionMode::Hosted);
        assert_eq!(src.held, Some(32));
        let p = permit(&r, false, &mut host, &mut src, &mut counter()).unwrap();
        assert_eq!(p.windows_pid(), 32);
        assert_eq!(src.calls, vec!["inspect 32", "prepare_attach 32"]);
    }

    #[test]
    fn two_same_name_processes_in_different_folders() {
        let w = World::new("two");
        let (pa, da) = w.kiln("a", 32, 100, N1);
        let (pb, db) = w.kiln("b", 64, 200, N2);
        w.put("a", &da);
        w.put("b", &db);
        let mut src = FakeSource::new(vec![pa, pb]);
        assert_eq!(run(&mut w.host("a", TargetKind::Server), &mut src).unwrap().target.identity.windows_pid, 32);
        assert_eq!(run(&mut w.host("b", TargetKind::Server), &mut src).unwrap().target.identity.windows_pid, 64);
        // b's descriptor copied into a's state folder names b's exe: not a's process, never attached as a's
        let w2 = World::new("two-copy");
        let (pb, db) = w2.kiln("b", 64, 200, N2);
        w2.put("a", &db);
        let mut src = FakeSource::new(vec![pb]);
        let e = run(&mut w2.host("a", TargetKind::Server), &mut src).unwrap_err();
        assert_eq!((e.code, e.reason.as_deref()), (DebugCode::Refused, Some("descriptor_invalid")));
        assert!(src.calls.is_empty(), "b's process was not even inspected: {:?}", src.calls);
    }

    #[test]
    fn pid_reuse_is_a_stale_descriptor() {
        let w = World::new("reuse");
        let (_, d) = w.kiln("a", 32, 100, N1);
        w.put("a", &d);
        // PID 32 is another process now: another creation time, no pipe of that name
        let (mut other, _) = w.kiln("a", 32, 999, N2);
        other.pipe = None;
        let e = run(&mut w.host("a", TargetKind::Server), &mut FakeSource::new(vec![other.clone()])).unwrap_err();
        assert_eq!((e.code, e.reason.as_deref()), (DebugCode::NotRunning, Some("stale_descriptor")));
        assert!(e.message.contains("created 999"), "told by its creation time, not by the missing pipe: {}", e.message);
        // even when the new process is a Kiln with the same image: its creation time differs
        w.put("a", &d);
        let (k, _) = w.kiln("a", 32, 999, N1);
        let e = run(&mut w.host("a", TargetKind::Server), &mut FakeSource::new(vec![k])).unwrap_err();
        assert_eq!(e.code, DebugCode::NotRunning);
        assert!(e.message.contains("created 999"), "{}", e.message);
    }

    #[test]
    fn exit_between_resolve_and_attach() {
        let w = World::new("exit");
        let (p, d) = w.kiln("a", 32, 100, N1);
        w.put("a", &d);
        let mut src = FakeSource::new(vec![p]);
        let mut host = w.host("a", TargetKind::Server);
        let r = run(&mut host, &mut src).unwrap();
        src.before_prepare = Some(Box::new(|t| t.clear()));
        let e = permit(&r, false, &mut host, &mut src, &mut counter()).unwrap_err();
        assert_eq!(e.code, DebugCode::NotRunning);
        // and a restart with the same PID before attach
        let (p, _) = w.kiln("a", 32, 100, N1);
        let mut src = FakeSource::new(vec![p]);
        let r = run(&mut host, &mut src).unwrap();
        src.before_prepare = Some(Box::new(|t| t[0].creation_time_100ns = "101".into()));
        let e = permit(&r, false, &mut host, &mut src, &mut counter()).unwrap_err();
        assert_eq!((e.code, e.reason.as_deref()), (DebugCode::IdentityChanged, Some("creation_time")));
    }

    #[test]
    fn stale_descriptor_and_switch_off() {
        let w = World::new("stale");
        let e = run(&mut w.host("a", TargetKind::Server), &mut FakeSource::default()).unwrap_err();
        assert_eq!((e.code, e.reason.as_deref()), (DebugCode::Refused, Some("developer_debugging_off")));
        let (_, d) = w.kiln("a", 32, 100, N1);
        w.put("a", &d);
        let e = run(&mut w.host("a", TargetKind::Server), &mut FakeSource::default()).unwrap_err();
        assert_eq!((e.code, e.reason.as_deref()), (DebugCode::NotRunning, Some("stale_descriptor")));
    }

    #[test]
    fn proven_stale_descriptors_are_removed_and_nothing_else() {
        let w = World::new("prune");
        let file = |pid: u32| w.root.join("a/kiln/state/debug").join(format!("{pid}.json"));
        // 32 crashed (no process), 40's PID is another process now (other creation time), 48 is live
        let (_, d32) = w.kiln("a", 32, 100, N1);
        let (mut p40, d40) = w.kiln("a", 40, 200, N1);
        p40.creation_time_100ns = "201".into();
        let (p48, d48) = w.kiln("a", 48, 300, N2);
        for d in [&d32, &d40, &d48] {
            w.put("a", d);
        }
        let r = run(&mut w.host("a", TargetKind::Server), &mut FakeSource::new(vec![p40, p48.clone()])).unwrap();
        assert_eq!(r.target.identity.windows_pid, 48);
        assert!(!file(32).exists() && !file(40).exists(), "the proven-stale descriptors are gone");
        assert!(file(48).exists(), "the live one stays");
        assert_eq!(r.notes.iter().filter(|n| n.contains("removed")).count(), 2, "{:?}", r.notes);
        // also when the resolve itself fails (all stale) while a process of this install runs in its prefix (the host
        // facts hold): a crashed start's descriptor goes; with nothing running nothing is removed (see
        // a_no_process_answer_is_proof_only_while_the_targets_namespace_is_alive)
        std::fs::remove_file(file(48)).unwrap();
        w.put("a", &d32);
        let e = run(&mut w.host("a", TargetKind::Server), &mut FakeSource::default()).unwrap_err();
        assert_eq!(e.code, DebugCode::NotRunning);
        assert!(!file(32).exists());
        // unproven failures keep their files: no pipe answer, another nonce, rights, unsupported, another install
        type Change = Box<dyn Fn(&mut FakeProc, &mut FakeSource)>;
        let unproven: Vec<(&str, Change)> = vec![
            ("pipe_missing", Box::new(|p, _| p.pipe = None)),
            ("nonce", Box::new(|p, _| p.pipe.as_mut().unwrap().session_nonce = N2.into())),
            ("challenge", Box::new(|p, _| p.pipe.as_mut().unwrap().echo = false)),
            ("image_digest", Box::new(|p, _| p.image_sha256 = "b".repeat(64))),
            ("denied", Box::new(|p, _| p.denied = true)),
            ("unsupported", Box::new(|_, s| s.fail = Some(DebugError::new(DebugCode::UnsupportedIdentity, "no query")))),
            ("transport", Box::new(|_, s| s.fail = Some(DebugError::new(DebugCode::Timeout, "the helper did not answer")))),
        ];
        for (what, change) in unproven {
            let (mut p, d) = w.kiln("a", 32, 100, N1);
            w.put("a", &d);
            let mut src = FakeSource::default();
            change(&mut p, &mut src);
            src.procs = vec![p];
            assert!(run(&mut w.host("a", TargetKind::Server), &mut src).is_err(), "{what}");
            assert!(file(32).exists(), "{what}: not proof, the file stays");
        }
        // another install's descriptor is never touched
        std::fs::remove_file(file(32)).unwrap();
        let (_, db) = w.kiln("b", 64, 100, N1);
        w.put("a", &db);
        let _ = run(&mut w.host("a", TargetKind::Server), &mut FakeSource::default());
        assert!(file(64).exists());
        // a file rewritten by a new process under the same PID after the probe is not removed
        std::fs::remove_file(file(64)).unwrap();
        let (_, old) = w.kiln("a", 32, 100, N1);
        w.put("a", &old);
        let (_, newer) = w.kiln("a", 32, 500, N2);
        let mut src = FakeSource::default();
        let root = w.root.clone();
        let rewrite = newer.clone();
        src.fail = None;
        let mut host = w.host("a", TargetKind::Server);
        let mut ch = counter();
        let mut first = true;
        let mut rewrite_then = move || {
            if std::mem::take(&mut first) {
                std::fs::write(root.join("a/kiln/state/debug/32.json"), serde_json::to_vec(&rewrite).unwrap()).unwrap();
            }
            ch()
        };
        let _ = resolve(&mut host, &mut src, &mut rewrite_then);
        assert!(file(32).exists(), "the newer process's descriptor stays");
    }

    #[test]
    fn elevation_mismatch() {
        let w = World::new("elev");
        let (p, mut d) = w.kiln("a", 32, 100, N1);
        d.elevated = true;
        w.put("a", &d);
        let mut src = FakeSource::new(vec![p.clone()]);
        src.elevated = Some(false);
        let e = run(&mut w.host("a", TargetKind::Server), &mut src).unwrap_err();
        assert_eq!(e.code, DebugCode::PrivilegeMismatch);
        assert!(src.calls.is_empty());
        // a process this user cannot open
        let (mut p, d) = w.kiln("a", 32, 100, N1);
        w.put("a", &d);
        p.denied = true;
        let e = run(&mut w.host("a", TargetKind::Server), &mut FakeSource::new(vec![p])).unwrap_err();
        assert_eq!(e.code, DebugCode::PrivilegeMismatch);
    }

    #[test]
    fn several_live_descriptors_are_refused() {
        let w = World::new("several");
        let (p1, d1) = w.kiln("a", 32, 100, N1);
        let (p2, d2) = w.kiln("a", 48, 300, N2);
        w.put("a", &d1);
        w.put("a", &d2);
        let e = run(&mut w.host("a", TargetKind::Server), &mut FakeSource::new(vec![p1.clone(), p2])).unwrap_err();
        assert_eq!((e.code, e.reason.as_deref()), (DebugCode::Refused, Some("several_descriptors")));
        // with one of them stale the other is the target, and it is the held one even though it was inspected first
        let mut src = FakeSource::new(vec![p1]);
        let r = run(&mut w.host("a", TargetKind::Server), &mut src).unwrap();
        assert_eq!(r.target.identity.windows_pid, 32);
        assert_eq!(src.held, Some(32));
        assert_eq!(r.notes.len(), 2, "{:?}", r.notes);
        assert!(r.notes[0].contains("no process 48") && r.notes[1].contains("48.json: removed"), "{:?}", r.notes);
    }

    #[test]
    fn every_windows_fact_is_checked() {
        let w = World::new("facts");
        type Change = Box<dyn Fn(&mut FakeProc) + Send + Sync>;
        let cases: Vec<(&str, Change)> = vec![
            ("nonce", Box::new(|p| p.pipe.as_mut().unwrap().session_nonce = N2.into())),
            ("challenge", Box::new(|p| p.pipe.as_mut().unwrap().echo = false)),
            ("pipe_missing", Box::new(|p| p.pipe.as_mut().unwrap().name.push('x'))),
            ("image_digest", Box::new(|p| p.image_sha256 = "b".repeat(64))),
            ("image_path", Box::new(|p| p.image = r"Z:\elsewhere\enshrouded_server.exe".into())),
        ];
        for (reason, change) in cases {
            let (p, d) = w.kiln("a", 32, 100, N1);
            w.put("a", &d);
            // at resolve: the descriptor is stale (nothing attaches)
            let mut bad = p.clone();
            change(&mut bad);
            let e = run(&mut w.host("a", TargetKind::Server), &mut FakeSource::new(vec![bad])).unwrap_err();
            assert_eq!(e.code, DebugCode::NotRunning, "{reason}");
            // between resolve and attach: identity_changed with the reason
            let mut src = FakeSource::new(vec![p]);
            let mut host = w.host("a", TargetKind::Server);
            let r = run(&mut host, &mut src).unwrap();
            let change = std::sync::Arc::new(change);
            let c2 = change.clone();
            let mut once = Some(());
            src.before_prepare = Some(Box::new(move |t| {
                if once.take().is_some() {
                    c2(&mut t[0]);
                }
            }));
            let e = permit(&r, false, &mut host, &mut src, &mut counter()).unwrap_err();
            assert_eq!((e.code, e.reason.as_deref()), (DebugCode::IdentityChanged, Some(reason)));
        }
    }

    #[test]
    fn host_facts_are_part_of_the_tuple() {
        let w = World::new("hostfacts");
        let (p, d) = w.kiln("a", 32, 100, N1);
        w.put("a", &d);
        for (field, later) in [
            ("prefixIdentity", HostFacts { prefix_identity: Some(PrefixIdentity { path: "/pfx/a".into(), device: 1, inode: 11 }), ..w.host("a", TargetKind::Server).facts }),
            ("runnerSha256", HostFacts { runner_sha256: Some("s".repeat(64)), ..w.host("a", TargetKind::Server).facts }),
        ] {
            let mut host = w.host("a", TargetKind::Server);
            host.later = Some(later);
            let mut src = FakeSource::new(vec![p.clone()]);
            let r = run(&mut host, &mut src).unwrap();
            let e = permit(&r, false, &mut host, &mut src, &mut counter()).unwrap_err();
            assert_eq!((e.code, e.reason.as_deref()), (DebugCode::IdentityChanged, Some(field)));
        }
        // the display-only host PID changing is not a reason to refuse
        let mut host = w.host("a", TargetKind::Server);
        host.later = Some(HostFacts { host_pid: Some(9999), runner_context: None, ..host.facts.clone() });
        let mut src = FakeSource::new(vec![p]);
        let r = run(&mut host, &mut src).unwrap();
        assert!(permit(&r, false, &mut host, &mut src, &mut counter()).is_ok());
    }

    #[test]
    fn runtime_not_ready_and_unsupported() {
        let w = World::new("rt");
        let (mut p, d) = w.kiln("a", 32, 100, N1);
        w.put("a", &d);
        p.pipe.as_mut().unwrap().managed_ready = false;
        let e = run(&mut w.host("a", TargetKind::Server), &mut FakeSource::new(vec![p.clone()])).unwrap_err();
        assert_eq!(e.code, DebugCode::RuntimeNotReady);
        let mut src = FakeSource::new(vec![p]);
        src.fail = Some(DebugError::new(DebugCode::UnsupportedIdentity, "GetProcessTimes is missing").because("GetProcessTimes"));
        let e = run(&mut w.host("a", TargetKind::Server), &mut src).unwrap_err();
        assert_eq!(e.code, DebugCode::UnsupportedIdentity);
    }

    #[test]
    fn a_client_needs_consent_before_anything_is_asked() {
        assert_eq!(consent_gate(TargetKind::Client, false).unwrap_err().code, DebugCode::PauseConsentRequired);
        assert!(consent_gate(TargetKind::Client, true).is_ok());
        assert!(consent_gate(TargetKind::Server, false).is_ok());
        let w = World::new("consent");
        let (p, mut d) = w.kiln("a", 32, 100, N1);
        d.side = TargetKind::Client;
        w.put("a", &d);
        let mut src = FakeSource::new(vec![p]);
        let mut host = w.host("a", TargetKind::Client);
        let r = run(&mut host, &mut src).unwrap();
        let before = src.calls.len();
        let e = permit(&r, false, &mut host, &mut src, &mut counter()).unwrap_err();
        assert_eq!(e.code, DebugCode::PauseConsentRequired);
        assert_eq!(src.calls.len(), before, "no prepare_attach without consent");
        assert!(permit(&r, true, &mut host, &mut src, &mut counter()).is_ok());
        // a server descriptor is not a client target and the other way round
        let e = run(&mut w.host("a", TargetKind::Server), &mut FakeSource::default()).unwrap_err();
        assert_eq!(e.reason.as_deref(), Some("descriptor_invalid"));
    }

    #[test]
    fn watch_outcomes() {
        let w = World::new("watch");
        let (p, d) = w.kiln("a", 32, 100, N1);
        w.put("a", &d);
        let mut src = FakeSource::new(vec![p]);
        let r = run(&mut w.host("a", TargetKind::Server), &mut src).unwrap();
        assert_eq!(watch(&r, &mut src, &mut counter()), WatchOutcome::Same(SessionMode::Hosted));
        src.procs[0].pipe.as_mut().unwrap().session_mode = SessionMode::Menu;
        assert_eq!(watch(&r, &mut src, &mut counter()), WatchOutcome::Same(SessionMode::Menu));
        src.procs[0].pipe.as_mut().unwrap().session_nonce = N2.into();
        assert_eq!(watch(&r, &mut src, &mut counter()), WatchOutcome::Changed("nonce".into()));
        src.procs.clear();
        assert_eq!(watch(&r, &mut src, &mut counter()), WatchOutcome::Gone);
        assert!(!src.calls.iter().any(|c| c.contains("kill") || c.contains("terminate")));
    }

    /// Review item 2: one slow or failed poll is not a reason to end a session; only a field proven different is.
    #[test]
    fn a_watch_poll_that_proves_nothing_keeps_the_session() {
        let w = World::new("unknown");
        let (p, d) = w.kiln("a", 32, 100, N1);
        w.put("a", &d);
        let mut src = FakeSource::new(vec![p]);
        let r = run(&mut w.host("a", TargetKind::Server), &mut src).unwrap();
        // a slow helper or pipe, a busy pipe, a pipe the core closed while its process exits, an answer that is not the
        // protocol, a lost handle, rights, a failed query: none of them shows another process
        let transient = [
            DebugError::new(DebugCode::Timeout, "the debug-inspect helper did not answer watch_exit").because("helper_no_answer"),
            DebugError::new(DebugCode::Timeout, "the debug-inspect helper has ended").because("helper_gone"),
            DebugError::new(DebugCode::Timeout, "the control pipe did not answer").because("pipe_no_answer"),
            DebugError::new(DebugCode::Timeout, "the control pipe stayed busy").because("pipe_busy"),
            DebugError::identity_changed("PID 32 serves no control pipe", "pipe_missing"),
            DebugError::identity_changed("the control pipe's answer is not Kiln's", "pipe_answer"),
            DebugError::identity_changed("the helper's answer does not echo this request's challenge", "helper_challenge"),
            DebugError::identity_changed("PID 32 is not the held process", "not_held"),
            DebugError::new(DebugCode::PrivilegeMismatch, "the control pipe refuses this user").because("pipe_denied"),
            DebugError::new(DebugCode::UnsupportedIdentity, "the image cannot be read").because("image_read"),
        ];
        for e in transient {
            src.fail = Some(e.clone());
            let got = watch(&r, &mut src, &mut counter());
            assert!(!matches!(got, WatchOutcome::Changed(_) | WatchOutcome::Gone), "{:?} ended the session: {got:?}", e.reason);
            assert_eq!(got, WatchOutcome::Unknown(e.reason.clone().unwrap()));
        }
        src.fail = None;
        let got = watch(&r, &mut src, &mut || Err(DebugError::new(DebugCode::UnsupportedIdentity, "no random challenge")));
        assert!(!matches!(got, WatchOutcome::Changed(_) | WatchOutcome::Gone), "no challenge ended the session: {got:?}");
        assert_eq!(got, WatchOutcome::Unknown("no random challenge".into()));
        // the next poll that answers is the session again
        assert_eq!(watch(&r, &mut src, &mut counter()), WatchOutcome::Same(SessionMode::Hosted));
        // a field proven different ends it
        for reason in ["pid", "creation_time", "image_path", "image_digest", "nonce", "challenge", "pipe_identity", "pipe_server"] {
            src.fail = Some(DebugError::identity_changed("differs", reason));
            assert_eq!(watch(&r, &mut src, &mut counter()), WatchOutcome::Changed(reason.into()), "{reason}");
        }
    }

    /// Review item 2: the held handle's image is hashed once; a watch poll does not read the whole image again.
    #[test]
    fn watch_polls_reuse_the_image_digest_of_the_held_handle() {
        let w = World::new("digest");
        let (p, d) = w.kiln("a", 32, 100, N1);
        w.put("a", &d);
        let mut src = FakeSource::new(vec![p]);
        let mut host = w.host("a", TargetKind::Server);
        let r = run(&mut host, &mut src).unwrap();
        assert_eq!(src.hashes, 1, "inspect hashes");
        permit(&r, false, &mut host, &mut src, &mut counter()).unwrap();
        assert_eq!(src.hashes, 2, "prepare_attach hashes again (the last check before an attach)");
        for _ in 0..3 {
            assert_eq!(watch(&r, &mut src, &mut counter()), WatchOutcome::Same(SessionMode::Hosted));
        }
        assert_eq!(src.hashes, 2, "watch polls take the digest of the held handle");
        // another handle (a new inspect) hashes again
        run(&mut host, &mut src).unwrap();
        assert_eq!(src.hashes, 3);
    }

    /// Review item 3: a reused PID is proven stale by its creation time even when its new process serves no pipe or
    /// cannot be read, and a descriptor that cannot be checked is a note when another one is the live target.
    #[test]
    fn a_reused_or_unreadable_pid_does_not_hide_the_live_descriptor() {
        let w = World::new("reuse-order");
        let file = |pid: u32| w.root.join("a/kiln/state/debug").join(format!("{pid}.json"));
        let mut host = w.host("a", TargetKind::Server);
        // PID 32 is another process now: no control pipe, an image this user cannot read
        let (_, d32) = w.kiln("a", 32, 100, N1);
        let (mut other, _) = w.kiln("a", 32, 999, N2);
        other.pipe = None;
        other.unreadable = true;
        let (p48, d48) = w.kiln("a", 48, 300, N2);
        w.put("a", &d32);
        w.put("a", &d48);
        let r = run(&mut host, &mut FakeSource::new(vec![other, p48.clone()])).expect("the live one is the target");
        assert_eq!(r.target.identity.windows_pid, 48);
        assert!(!file(32).exists(), "its creation time proves 32's descriptor stale: removed");
        assert!(r.notes.iter().any(|n| n.contains("created 999")), "{:?}", r.notes);
        // a PID this user cannot open now (an elevated or another user's process took it), a descriptor of an elevated
        // process, and one whose check failed: notes, and the live one is still the target
        let (mut denied, d40) = w.kiln("a", 40, 200, N1);
        denied.denied = true;
        let (_, mut d44) = w.kiln("a", 44, 250, N1);
        d44.elevated = true;
        w.put("a", &d40);
        w.put("a", &d44);
        let mut src = FakeSource::new(vec![denied, p48.clone()]);
        src.elevated = Some(false);
        let r = run(&mut host, &mut src).expect("the live one is the target");
        assert_eq!((r.target.identity.windows_pid, src.held), (48, Some(48)));
        assert!(r.notes.iter().any(|n| n.contains("40.json") && n.contains("access denied")), "{:?}", r.notes);
        assert!(r.notes.iter().any(|n| n.contains("44.json") && n.contains("runs elevated")), "{:?}", r.notes);
        assert!(!src.calls.contains(&"inspect 44".to_string()), "an elevated target is not opened: {:?}", src.calls);
        assert!(file(40).exists() && file(44).exists(), "not proof: their files stay");
        // with none live the refusal is the one that says why it could not be checked, not "not running"
        std::fs::remove_file(file(48)).unwrap();
        let mut src = FakeSource::new(vec![]);
        src.procs = vec![{
            let (mut p, _) = w.kiln("a", 40, 200, N1);
            p.denied = true;
            p
        }];
        let e = run(&mut host, &mut src).unwrap_err();
        assert_eq!(e.code, DebugCode::PrivilegeMismatch, "{}", e.message);
    }

    /// A host side whose facts fail: nothing shows the target's namespace alive (no process of this install runs in its
    /// prefix, no runner context of the client's prefix).
    struct Down(FakeHost);

    impl HostSide for Down {
        fn spec(&self) -> &HostSpec {
            &self.0.spec
        }
        fn facts(&mut self) -> Result<HostFacts, DebugError> {
            Err(DebugError::new(DebugCode::NotRunning, "no process of this install runs in its prefix"))
        }
    }

    /// Review nit (item C): a Windows side in another wineserver (wrong prefix or runner) sees no process of that PID,
    /// or another process under it. Such answers prove a descriptor stale only while the host side shows the target's
    /// namespace alive in the same resolve, or another descriptor of this install was proven live there.
    #[test]
    fn a_no_process_answer_is_proof_only_while_the_targets_namespace_is_alive() {
        let w = World::new("namespace");
        let file = |pid: u32| w.root.join("a/kiln/state/debug").join(format!("{pid}.json"));
        // no process 32, and PID 40 is another process (another creation time), as a fresh wineserver would show them
        let (_, d32) = w.kiln("a", 32, 100, N1);
        let (mut p40, d40) = w.kiln("a", 40, 200, N1);
        p40.creation_time_100ns = "7".into();
        w.put("a", &d32);
        w.put("a", &d40);
        let mut down = Down(w.host("a", TargetKind::Server));
        let e = resolve(&mut down, &mut FakeSource::new(vec![p40.clone()]), &mut counter()).unwrap_err();
        assert_eq!((e.code, e.reason.as_deref()), (DebugCode::NotRunning, Some("stale_descriptor")));
        assert!(file(32).exists() && file(40).exists(), "nothing shows the namespace alive: nothing is removed");
        assert!(e.message.contains("kept"), "{}", e.message);
        // the same answers while a process of this install runs in that prefix (the host facts hold): a crashed start's
        // descriptors go
        let e = run(&mut w.host("a", TargetKind::Server), &mut FakeSource::new(vec![p40.clone()])).unwrap_err();
        assert_eq!(e.code, DebugCode::NotRunning);
        assert!(!file(32).exists() && !file(40).exists(), "removed while the namespace is alive");
        // or when the same source proved another descriptor of this install live (here the host facts fail afterwards)
        w.put("a", &d32);
        let (p48, d48) = w.kiln("a", 48, 300, N2);
        w.put("a", &d48);
        assert!(resolve(&mut down, &mut FakeSource::new(vec![p48]), &mut counter()).is_err(), "the facts of the live one fail");
        assert!(!file(32).exists(), "48 proved the namespace: 32 is removed");
        assert!(file(48).exists(), "the live one stays");
    }

    /// The answer lines exactly as native/core/src/debug_descriptor.cpp writes them.
    #[test]
    fn the_cores_pipe_answers() {
        let ok = r#"{"v":1,"ok":true,"challenge":"challenge-0001","identity":{"windowsPid":252,"creationTime100ns":"134390977514834040","image":"Z:\\srv\\enshrouded_server.exe","imageSha256":"f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2","sessionNonce":"dbfc61b7726e7dd1b89ebe665c73f4d7"},"managedReady":true,"sessionMode":"hosted","runtimeVersion":"10.0.14","side":"server","kiln":"0.2.0-dev"}"#;
        let a = parse_pipe_answer(ok).unwrap();
        assert_eq!((a.windows_pid, a.creation_time_100ns.as_str(), a.challenge.as_str()), (252, "134390977514834040", "challenge-0001"));
        assert_eq!((a.image.as_str(), a.session_mode, a.managed_ready), (r"Z:\srv\enshrouded_server.exe", SessionMode::Hosted, true));
        let wrong = r#"{"v":1,"ok":false,"code":"identity_changed","message":"this process has another session nonce"}"#;
        assert_eq!(parse_pipe_answer(wrong).unwrap_err().reason.as_deref(), Some("nonce"));
        let big = r#"{"v":1,"ok":false,"code":"frame_too_large","message":"a request line is at most 16384 bytes"}"#;
        assert_eq!(parse_pipe_answer(big).unwrap_err().code, DebugCode::UnsupportedIdentity);
        for junk in ["", "hello", r#"{"v":2,"ok":true}"#, r#"{"v":1,"ok":true,"identity":{"windowsPid":252}}"#] {
            assert_eq!(parse_pipe_answer(junk).unwrap_err().reason.as_deref(), Some("pipe_answer"), "{junk}");
        }
        // a number where the core writes a decimal string is not the protocol
        let num = ok.replace(r#""creationTime100ns":"134390977514834040""#, r#""creationTime100ns":134390977514834040"#);
        assert_eq!(parse_pipe_answer(&num).unwrap_err().reason.as_deref(), Some("pipe_answer"));
        let req: serde_json::Value = serde_json::from_str(&pipe_request("n", "c")).unwrap();
        assert_eq!(req, serde_json::json!({ "v": 1, "op": "identify", "nonce": "n", "challenge": "c" }));
    }

    #[test]
    fn steam_paths() {
        assert!(in_steam_library(Path::new("/home/u/.local/share/Steam/steamapps/common/EnshroudedServer")));
        assert!(in_steam_library(Path::new("/mnt/games/SteamLibrary/steamapps/compatdata/2278520/pfx")));
        assert!(!in_steam_library(Path::new("/home/u/srv/enshrouded/labs/mylab")));
        assert!(!in_steam_library(Path::new("/home/u/steamapps-backup/server")));
        let e = server_steam_guard(Path::new("/home/u/.local/share/Steam/steamapps/common/EnshroudedServer")).unwrap_err();
        assert_eq!((e.code, e.reason.as_deref()), (DebugCode::Refused, Some("steam_library")));
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn wine_is_found_above_the_preloader() {
        let t = std::env::temp_dir().join(format!("kiln-debug-wine-{}", std::process::id()));
        let lib = t.join("files/lib/wine/i386-unix");
        std::fs::create_dir_all(&lib).unwrap();
        std::fs::create_dir_all(t.join("files/bin")).unwrap();
        let wine = t.join("files/bin/wine");
        let decoy = lib.join("wine");
        for w in [&wine, &decoy] {
            std::fs::write(w, "").unwrap();
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(w, std::fs::Permissions::from_mode(0o755)).unwrap();
        }
        assert_eq!(wine_above(&lib.join("wine64-preloader")), Some(wine));
        std::fs::remove_dir_all(&t).unwrap();
    }
}
