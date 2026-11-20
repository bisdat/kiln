// SPDX-License-Identifier: MIT
//! `kiln debug`: attach a .NET debugger (netcoredbg, not bundled) to a local server's mods. Plain: says how
//! (`--json`: the proven target, wine, prefix, netcoredbg, env, command). `--dap`: a Debug Adapter Protocol proxy on
//! stdin/stdout around netcoredbg for editors (docs/kiln/modding/debugging.md).
//!
//! The rules that keep this from hurting a server:
//! - **Only the proven process.** The target is the one process whose Windows identity (PID, creation time, image and
//!   its digest, the session nonce its core wrote into `kiln/state/debug/<pid>.json` and answers on its control pipe)
//!   agrees with the install, its prefix and the runner (kiln_manage::debug). A PID is never compared alone, a Linux
//!   PID never decides anything, and the whole tuple is checked again right before netcoredbg starts and before the
//!   editor's attach is forwarded. A folder whose prefix is unknown is refused, and a server of a Steam library (where
//!   the user's live server lives) is always refused. Refusals happen before anything runs in a prefix.
//! - **The debuggee never ends with the session.** Every `disconnect` goes out with `terminateDebuggee: false`;
//!   `launch`, `terminate` and `restart` are refused and not advertised.
//! - **Stale modules go first.** Before attaching, the proxy asks the server to collect unloaded mod contexts; breakpoints
//!   otherwise can bind to the old copy of a reloaded mod (and sessions in that state have ended the server). While a
//!   session is attached, `kiln reload` refuses (kiln/state/debug/session.json).
//! - **The debugger is never killed.** A debugger dying while attached kills the server (measured), so
//!   when the client goes away (stdin EOF, SIGINT, SIGTERM, SIGHUP) the proxy disconnects cleanly and waits; if the
//!   adapter does not answer, the proxy closes its stdin, which also makes it detach, and leaves it alone.
//! - **A pause can be bounded.** `--max-pause-ms`: a stopped debuggee is continued by the proxy after that long (best
//!   effort: a pending evaluation can delay it). Every change of the session is written to the session file and sent
//!   to the editor as a `kiln/debugState` event.

use crate::out::{self, CliError};
use crate::project::Project;
use crate::server::{self, Server};
use anyhow::Result;
use kiln_manage::debug::resolver::{self, HostFacts, HostSide, HostSpec, ProcessSource, Resolved, WatchOutcome};
use kiln_manage::debug::{AttachPermit, DebugCode, DebugError, DebugSessionState, PrefixIdentity, RunnerContext, SessionMode, SessionState, TargetKind};
use serde_json::{Value, json};
use std::collections::BTreeMap;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

const SERVER_EXE: &str = "enshrouded_server.exe";
/// A proxy's own requests and errors, far from what a client or netcoredbg counts to.
const OWN_SEQ: i64 = 1_000_000_000;
const MAX_FRAME: usize = 256 << 20;
/// How long a client-less proxy waits for netcoredbg's disconnect response (a detach walks every module).
const DISCONNECT_WAIT: Duration = Duration::from_secs(30);
const EXIT_WAIT: Duration = Duration::from_secs(15);
/// Seconds to wait for the control command `gc` before attaching.
const GC_WAIT: u64 = 15;
/// How often an attached session asks the Windows side whether the debuggee still runs and is still itself.
const WATCH_EVERY: Duration = Duration::from_secs(2);
/// `--max-pause-ms` bounds (chapter 12a §4 step 8) and the default where a watchdog is on by default (clients).
const PAUSE_MIN_MS: u64 = 100;
const PAUSE_MAX_MS: u64 = 60_000;
const PAUSE_DEFAULT_MS: u64 = 5000;

#[derive(clap::Args, Default)]
pub struct DebugArgs {
    /// Server folder, lab name or server instance id (default: $KILN_SERVER, else the current project's KilnServer).
    #[arg(long, conflicts_with = "client")]
    pub server: Option<String>,
    /// A game client: its install id (as the launcher and kilnctl list it) or its folder. Started with developer
    /// debugging on; attaching needs --allow-client-pause, --observe needs nothing.
    #[arg(long, value_name = "INSTALL")]
    pub client: Option<String>,
    /// Consent to a debugger that stops the game client at every breakpoint (it can freeze or drop its connection).
    #[arg(long, requires = "client")]
    pub allow_client_pause: bool,
    /// The client's Kiln log and counters as they come, without a debugger: nothing pauses the game.
    #[arg(long, requires = "client", conflicts_with_all = ["dap", "allow_client_pause", "max_pause_ms"])]
    pub observe: bool,
    /// Speak the Debug Adapter Protocol on stdin/stdout (for editors); forces terminateDebuggee=false.
    #[arg(long)]
    pub dap: bool,
    /// Continue a stopped debuggee after this many milliseconds (100 to 60000; best effort). Clients: 5000 unless
    /// given; servers: off unless given.
    #[arg(long, value_name = "MS")]
    pub max_pause_ms: Option<u64>,
}

thread_local! {
    /// Adapters started by [`launch`] on this thread (the only place one starts): what the tests count.
    static ADAPTER_STARTS: std::cell::Cell<usize> = const { std::cell::Cell::new(0) };
}

fn cli_error(e: DebugError) -> anyhow::Error {
    let mut c = CliError::new(e.code.as_str(), e.message.clone());
    if let Some(d) = e.detail() {
        c = c.with_detail(d);
    }
    c.into()
}

/// The code and reason of an error, whichever type it is (for DAP error bodies and state events).
fn code_of(e: &anyhow::Error) -> (String, Option<String>, String) {
    if let Some(d) = e.downcast_ref::<DebugError>() {
        return (d.code.as_str().into(), d.reason.clone(), d.message.clone());
    }
    if let Some(c) = e.downcast_ref::<CliError>() {
        let reason = c.detail.as_ref().and_then(|d| d["reason"].as_str()).map(str::to_string);
        return (c.code.clone(), reason, c.message.clone());
    }
    ("error".into(), None, format!("{e:#}"))
}

/// What the debugger attaches to, and how to run it there.
struct Target {
    kind: TargetKind,
    /// The server (or game) folder.
    install: PathBuf,
    via: &'static str,
    lab: Option<String>,
    wine: Option<PathBuf>,
    prefix: Option<PathBuf>,
    netcoredbg: PathBuf,
    resolved: Resolved,
    max_pause_ms: Option<u64>,
    /// The consent given (`--allow-client-pause`); checked again by every permit.
    allow_client_pause: bool,
    /// A client's runner environment (the allowlist taken from the game's own runner context); None: the server's.
    runner_env: Option<Vec<(String, String)>>,
}

impl Target {
    fn env(&self) -> BTreeMap<&'static str, String> {
        let mut e = BTreeMap::new();
        // netcoredbg's bundled runtime looks for ICU and aborts: "Cannot get symbol u_charsToUChars from libicuuc"
        e.insert("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1".to_string());
        if let Some(env) = &self.runner_env {
            for (k, v) in env {
                let names = kiln_manage::debug::runner_env::ALLOWED.iter().copied().chain(["WINEDEBUG"]);
                if let Some(name) = names.into_iter().find(|a| *a == k.as_str()) {
                    e.insert(name, v.clone());
                }
            }
        } else if let Some(p) = &self.prefix {
            e.insert("WINEPREFIX", p.display().to_string());
            e.insert("WINEDEBUG", "-all".to_string());
            e.insert("WINEDLLOVERRIDES", kiln_manage::wine::DLL_OVERRIDES.to_string());
        }
        e
    }

    fn argv(&self) -> Vec<String> {
        let mut v = Vec::new();
        if let Some(w) = &self.wine {
            v.push(w.display().to_string());
        }
        v.push(self.netcoredbg.display().to_string());
        v.push("--interpreter=vscode".to_string());
        v
    }

    fn command(&self) -> Command {
        let argv = self.argv();
        let mut c = Command::new(&argv[0]);
        c.args(&argv[1..]);
        scrub(&mut c);
        c.envs(self.env());
        if let Some(d) = self.netcoredbg.parent() {
            c.current_dir(d);
        }
        c
    }

    fn windows_pid(&self) -> u32 {
        self.resolved.target.identity.windows_pid
    }

    fn what(&self) -> &'static str {
        match self.kind {
            TargetKind::Server => "server",
            TargetKind::Client => "game",
        }
    }
}

/// Inherited variables that would point a Wine call at another runner, prefix or display.
fn scrub(c: &mut Command) {
    for k in ["WINEPREFIX", "WINESERVER", "WINELOADER", "WINEDLLPATH", "WINEARCH", "WINEDLLOVERRIDES", "WINEDEBUG", "STEAM_COMPAT_DATA_PATH", "STEAM_COMPAT_CLIENT_INSTALL_PATH"] {
        c.env_remove(k);
    }
    kiln_manage::server::run::headless(c);
}

fn canon(p: &Path) -> PathBuf {
    p.canonicalize().unwrap_or_else(|_| p.to_path_buf())
}

/// $KILN_NETCOREDBG, else <data>/tools/netcoredbg/netcoredbg.exe. Not bundled (docs/kiln/modding/debugging.md).
fn netcoredbg() -> Result<PathBuf> {
    let (p, how) = match std::env::var_os("KILN_NETCOREDBG").filter(|v| !v.is_empty()) {
        Some(v) => (PathBuf::from(v), "KILN_NETCOREDBG"),
        None => (kiln_manage::paths::DataDirs::locate()?.root.join("tools/netcoredbg/netcoredbg.exe"), "the default place"),
    };
    if !p.is_file() {
        return Err(CliError::new(
            "tool_missing",
            format!("no netcoredbg at {} ({how}); get the win64 build (docs/kiln/modding/debugging.md) and put it there or set KILN_NETCOREDBG", p.display()),
        )
        .into());
    }
    Ok(canon(&p))
}

/// The server's Wine prefix: labs `<labs>/<name>.pfx`, instances their record. None on Windows.
fn prefix_of(s: &Server) -> Result<Option<PathBuf>> {
    if cfg!(windows) {
        return Ok(None);
    }
    if s.lab.is_some() {
        return Ok(Some(with_pfx(&s.dir)));
    }
    if let Some(i) = &s.instance {
        return match &i.prefix {
            Some(p) => Ok(Some(p.clone())),
            None => Err(CliError::new("not_found", format!("server instance {} has no Wine prefix on record", i.id)).into()),
        };
    }
    Err(CliError::new(
        "unsupported",
        format!("{} is neither a lab nor a known server instance, so its Wine prefix is unknown (kiln debug attaches only inside a server's own prefix)", s.dir.display()),
    )
    .into())
}

fn with_pfx(dir: &Path) -> PathBuf {
    let mut p = dir.as_os_str().to_owned();
    p.push(".pfx");
    PathBuf::from(p)
}

/// The host side of a local server: its folder, its prefix and the wine that runs it (Linux), read again at every
/// check. The Linux PID is display only.
struct ServerHost {
    spec: HostSpec,
    dir: PathBuf,
    wine: Option<PathBuf>,
}

impl HostSide for ServerHost {
    fn spec(&self) -> &HostSpec {
        &self.spec
    }
    fn facts(&mut self) -> std::result::Result<HostFacts, DebugError> {
        let Some(prefix) = self.spec.prefix.clone() else { return Ok(HostFacts::default()) };
        let host_pid = resolver::server_host_pid(&self.dir, &prefix)?;
        let runner = self.wine.as_deref().ok_or_else(|| DebugError::new(DebugCode::NotFound, format!("cannot tell which wine runs pid {host_pid}")))?;
        Ok(HostFacts {
            prefix_identity: Some(PrefixIdentity::of(&prefix)?),
            runner_sha256: Some(resolver::runner_sha256(runner)?),
            runner_context: RunnerContext::read(Path::new("/proc"), host_pid),
            host_pid: Some(host_pid),
        })
    }
}

/// How the Windows side of a Wine/Proton target is reached: its runner (the wine of the running process), the
/// environment that runner needs for that prefix (an allowlist, never inherited Wine variables), and a working folder.
pub(crate) struct RunnerLaunch {
    pub wine: PathBuf,
    pub env: Vec<(String, String)>,
    pub cwd: PathBuf,
}

impl RunnerLaunch {
    /// A dedicated server's runner: its wine, its own prefix.
    fn server(wine: &Path, prefix: &Path) -> RunnerLaunch {
        RunnerLaunch {
            wine: wine.to_path_buf(),
            env: vec![
                ("WINEPREFIX".into(), prefix.display().to_string()),
                ("WINEDEBUG".into(), "-all".into()),
                ("WINEDLLOVERRIDES".into(), format!("{};winedbg.exe=d", kiln_manage::wine::DLL_OVERRIDES)),
            ],
            cwd: prefix.to_path_buf(),
        }
    }

    /// `wine "<helper>" debug-inspect`, headless: the helper is an argument of wine, never a command of its own.
    fn helper_command(&self, helper: &Path) -> Command {
        let mut c = Command::new(&self.wine);
        c.arg(helper).arg("debug-inspect");
        scrub(&mut c);
        for (k, v) in &self.env {
            c.env(k, v);
        }
        c.current_dir(&self.cwd);
        c
    }
}

/// The Windows side for a target: in-process on native Windows (`NativeSource`), the hash-checked `kiln debug-inspect`
/// helper under the target's own runner otherwise. There is no fallback to a guessed PID: no helper is tool_missing.
pub(crate) fn open_source(runner: Option<&RunnerLaunch>) -> std::result::Result<Box<dyn ProcessSource + Send>, DebugError> {
    #[cfg(windows)]
    if runner.is_none() {
        return Ok(Box::new(kiln_manage::debug::windows::NativeSource::default()));
    }
    let Some(runner) = runner else {
        return Err(DebugError::new(DebugCode::Unsupported, "this target runs under Windows, but this kiln is not the Windows build").because("no_runner"));
    };
    let helper = crate::debug_inspect::find_helper()?;
    Ok(Box::new(crate::debug_inspect::HelperSource::spawn(runner.helper_command(&helper))?))
}

/// A resolved target with its host and Windows sides, ready for the proxy or for `--json`.
struct Planned {
    target: Target,
    host: Box<dyn HostSide + Send>,
    source: Box<dyn ProcessSource + Send>,
}

/// Consent first (nothing touches a process before it), then the install's one live descriptor, proven.
fn prepare(
    kind: TargetKind,
    allow_client_pause: bool,
    host: &mut dyn HostSide,
    source: &mut dyn ProcessSource,
    challenge: &mut dyn FnMut() -> std::result::Result<String, DebugError>,
) -> std::result::Result<Resolved, DebugError> {
    resolver::consent_gate(kind, allow_client_pause)?;
    resolver::resolve(host, source, challenge)
}

/// The order of every adapter start: the pre-attach collection, then consent and the whole tuple again
/// ([`resolver::permit`]), then the spawn, which only ever sees a permit.
fn launch<A>(
    r: &Resolved,
    allow_client_pause: bool,
    host: &mut dyn HostSide,
    source: &mut dyn ProcessSource,
    challenge: &mut dyn FnMut() -> std::result::Result<String, DebugError>,
    gc: impl FnOnce(),
    spawn: impl FnOnce(&AttachPermit) -> std::io::Result<A>,
) -> std::result::Result<(AttachPermit, A), DebugError> {
    gc();
    let permit = resolver::permit(r, allow_client_pause, host, source, challenge)?;
    ADAPTER_STARTS.with(|n| n.set(n.get() + 1));
    let adapter = spawn(&permit).map_err(|e| DebugError::new(DebugCode::ToolMissing, format!("cannot start the debugger: {e}")))?;
    Ok((permit, adapter))
}

fn max_pause(a: &DebugArgs, kind: TargetKind) -> Result<Option<u64>> {
    match a.max_pause_ms {
        Some(ms) if !(PAUSE_MIN_MS..=PAUSE_MAX_MS).contains(&ms) => {
            Err(CliError::usage(format!("--max-pause-ms is {PAUSE_MIN_MS} to {PAUSE_MAX_MS} (got {ms})")).into())
        }
        Some(ms) => Ok(Some(ms)),
        None => Ok(match kind {
            TargetKind::Server => None,
            TargetKind::Client => Some(PAUSE_DEFAULT_MS),
        }),
    }
}

/// `--client` or `--server`.
fn plan(a: &DebugArgs) -> Result<Planned> {
    match &a.client {
        Some(c) => plan_client(a, c),
        None => plan_server(a),
    }
}

/// `--server`: the guard, the server lookup, the prefix and the runner, then the proven target.
fn plan_server(a: &DebugArgs) -> Result<Planned> {
    // before anything looks at the folder or lists instances
    if let Some(raw) = a.server.as_deref().map(str::to_string).or_else(|| std::env::var("KILN_SERVER").ok()).filter(|v| !v.is_empty()) {
        resolver::server_steam_guard(&kiln_manage::util::expand(&raw, &kiln_manage::util::home())).map_err(cli_error)?;
    }
    let kind = TargetKind::Server;
    let max_pause_ms = max_pause(a, kind)?;
    let project = if a.server.is_none() { Project::find(None).ok() } else { None };
    let s = server::resolve(a.server.as_deref(), project.as_ref())?;
    // an instance id, a project's KilnServer or a symlink can still end up there
    let steam_runner = s.instance.as_ref().is_some_and(|i| matches!(i.runner, kiln_manage::server::instance::ServerRunner::Steam));
    if steam_runner {
        return Err(cli_error(DebugError::refused(
            format!("{} is inside a Steam library; kiln debug pauses the server for everyone on it, so it refuses", s.dir.display()),
            "steam_library",
        )));
    }
    resolver::server_steam_guard(&s.dir).map_err(cli_error)?;
    if s.instance.as_ref().is_some_and(|i| i.dir.is_none()) {
        return Err(CliError::new("unsupported", "that server is remote; kiln debug attaches to local servers").into());
    }
    let prefix = prefix_of(&s)?;
    if let Some(p) = &prefix {
        resolver::server_steam_guard(p).map_err(cli_error)?;
        if !p.is_dir() {
            return Err(CliError::new("not_found", format!("the server's Wine prefix {} does not exist", p.display())).into());
        }
    }
    let netcoredbg = netcoredbg()?;
    let wine = match &prefix {
        Some(p) => {
            let host = resolver::server_host_pid(&s.dir, p).map_err(cli_error)?;
            Some(resolver::wine_of(host).ok_or_else(|| CliError::new("not_found", format!("cannot tell which wine runs pid {host}")))?)
        }
        None => None,
    };
    let spec = HostSpec {
        kind,
        install_id: kiln_manage::game::install_id_for(&s.dir),
        state_dir: s.dir.join("kiln/state"),
        install_exe: s.dir.join(SERVER_EXE),
        prefix: prefix.clone(),
    };
    let runner = match (&wine, &prefix) {
        (Some(w), Some(p)) => Some(RunnerLaunch::server(w, p)),
        _ => None,
    };
    let mut host = ServerHost { spec, dir: s.dir.clone(), wine: wine.clone() };
    let mut source = open_source(runner.as_ref()).map_err(cli_error)?;
    let resolved = match prepare(kind, false, &mut host, source.as_mut(), &mut resolver::new_challenge) {
        Ok(r) => r,
        Err(e) => {
            source.close();
            return Err(cli_error(e));
        }
    };
    let target = Target {
        kind,
        install: s.dir.clone(),
        via: s.via,
        lab: s.lab.clone(),
        wine,
        prefix,
        netcoredbg,
        resolved,
        max_pause_ms,
        allow_client_pause: false,
        runner_env: None,
    };
    Ok(Planned { target, host: Box::new(host), source })
}

/// The host side of a game client: its install, and under Proton the runner every game process of its prefix agrees
/// on (kiln_manage::debug::proton), read again at every check. The Linux PID is display only.
struct ClientHost {
    spec: HostSpec,
    proc_root: PathBuf,
    install_dir: PathBuf,
    exe: String,
}

impl HostSide for ClientHost {
    fn spec(&self) -> &HostSpec {
        &self.spec
    }
    fn facts(&mut self) -> std::result::Result<HostFacts, DebugError> {
        let Some(prefix) = self.spec.prefix.clone() else { return Ok(HostFacts::default()) };
        let r = kiln_manage::debug::proton::find_runner(&self.proc_root, &self.install_dir, &self.exe, &prefix)?;
        let first = &r.contexts[0];
        Ok(HostFacts { prefix_identity: Some(r.prefix.clone()), runner_sha256: Some(r.runner_sha256.clone()), runner_context: Some(first.birth.clone()), host_pid: Some(first.pid) })
    }
}

/// The client install `sel` names: a folder, else an install id the launcher records or Steam's libraries show.
fn find_client(sel: &str) -> Result<kiln_manage::game::GameInstall> {
    let p = kiln_manage::util::expand(sel, &kiln_manage::util::home());
    let found = if p.is_dir() {
        Some(kiln_manage::game::inspect(&p)?)
    } else {
        let mut all = kiln_manage::paths::DataDirs::locate().ok().and_then(|d| kiln_manage::game::known(&d).ok()).unwrap_or_default();
        all.extend(kiln_manage::game::detect().unwrap_or_default());
        all.into_iter().find(|g| g.id == sel)
    };
    match found {
        Some(g) if g.kind == kiln_manage::game::GameKind::Client => Ok(g),
        Some(g) => Err(CliError::new("unsupported", format!("{} is a server install: use --server", g.path.display())).into()),
        None => Err(CliError::new("not_found", format!("no game install '{sel}' (an install id from the launcher or kilnctl, or a game folder)")).into()),
    }
}

/// A client's host side and how its Windows side is reached (native Windows: in-process; Proton: its runner).
fn client_sides(g: &kiln_manage::game::GameInstall, proc_root: &Path) -> std::result::Result<(ClientHost, Option<RunnerLaunch>), DebugError> {
    let prefix = match g.platform {
        kiln_manage::game::Platform::Windows => None,
        _ => Some(g.prefix.clone().ok_or_else(|| DebugError::new(DebugCode::NotFound, format!("{} has no Wine/Proton prefix on record", g.path.display())))?),
    };
    let launch = match &prefix {
        Some(p) => {
            let r = kiln_manage::debug::proton::find_runner(proc_root, &g.path, &g.exe, p)?;
            Some(RunnerLaunch { wine: r.runner, env: kiln_manage::debug::runner_env::launch_env(&r.env), cwd: r.prefix.path })
        }
        None => None,
    };
    let spec = HostSpec {
        kind: TargetKind::Client,
        install_id: g.id.clone(),
        state_dir: g.path.join("kiln/state"),
        install_exe: g.path.join(&g.exe),
        prefix,
    };
    Ok((ClientHost { spec, proc_root: proc_root.to_path_buf(), install_dir: g.path.clone(), exe: g.exe.clone() }, launch))
}

/// `--client`: consent first (nothing is looked at without it), then the install, its runner and the proven target.
/// The server's Steam guard does not apply: a client is chosen explicitly, and attached only with consent.
fn plan_client(a: &DebugArgs, sel: &str) -> Result<Planned> {
    resolver::consent_gate(TargetKind::Client, a.allow_client_pause).map_err(cli_error)?;
    let kind = TargetKind::Client;
    let max_pause_ms = max_pause(a, kind)?;
    let g = find_client(sel)?;
    let netcoredbg = netcoredbg()?;
    let (mut host, launch) = client_sides(&g, Path::new("/proc")).map_err(cli_error)?;
    let mut source = open_source(launch.as_ref()).map_err(cli_error)?;
    let resolved = match prepare(kind, a.allow_client_pause, &mut host, source.as_mut(), &mut resolver::new_challenge) {
        Ok(r) => r,
        Err(e) => {
            source.close();
            return Err(cli_error(e));
        }
    };
    let target = Target {
        kind,
        install: g.path.clone(),
        via: "install",
        lab: None,
        wine: launch.as_ref().map(|l| l.wine.clone()),
        prefix: host.spec.prefix.clone(),
        netcoredbg,
        resolved,
        max_pause_ms,
        allow_client_pause: a.allow_client_pause,
        runner_env: launch.map(|l| l.env),
    };
    Ok(Planned { target, host: Box::new(host), source })
}

/// `--client <install> --observe`: the client's Kiln log (its counters are log lines) and its session mode, read
/// through the helper when the descriptor can be proven. No consent is needed because nothing can pause: no adapter
/// is started on this path, and none can be (it never reaches [`launch`]).
fn observe(a: &DebugArgs, json: bool) -> Result<i32> {
    let g = find_client(a.client.as_deref().unwrap_or_default())?;
    let sides = client_sides(&g, Path::new("/proc"));
    let mut watch = observe_target(sides, json);
    crate::logs::observe(&g.path.join("kiln/logs/kiln.log"), 20, json, &mut std::io::stdout(), &mut watch, None)?;
    Ok(0)
}

/// The observed target (best effort: logs are shown whether or not it can be proven), and a poll that reports its
/// session mode when it changes.
fn observe_target(
    sides: std::result::Result<(ClientHost, Option<RunnerLaunch>), DebugError>,
    json: bool,
) -> Box<dyn FnMut() -> Option<String>> {
    let header = move |text: String, v: Value| if json { v.to_string() } else { format!("[kiln debug] {text}") };
    let proven = sides.and_then(|(mut host, launch)| {
        let mut source = open_source(launch.as_ref())?;
        let r = resolver::resolve(&mut host, source.as_mut(), &mut resolver::new_challenge)?;
        Ok((r, source))
    });
    observe_poll(proven, header)
}

fn observe_poll(
    proven: std::result::Result<(Resolved, Box<dyn ProcessSource + Send>), DebugError>,
    header: impl Fn(String, Value) -> String + 'static,
) -> Box<dyn FnMut() -> Option<String>> {
    let mut first = Some(match &proven {
        Ok((r, _)) => header(
            format!(
                "observing PID {} (created {}), session mode {}, .NET {}: no debugger, nothing pauses the game",
                r.target.identity.windows_pid,
                r.target.identity.creation_time_100ns,
                json!(r.target.session_mode).as_str().unwrap_or("unknown"),
                if r.target.managed_ready { "running" } else { "not running" }
            ),
            json!({ "event": "target", "target": r.target }),
        ),
        Err(e) => header(format!("observing the log only: {} ({})", e.message, e.code), json!({ "event": "target", "error": e })),
    });
    let mut state = proven.ok();
    let mut mode = state.as_ref().map(|(r, _)| r.target.session_mode);
    let mut last = std::time::Instant::now();
    Box::new(move || {
        if let Some(f) = first.take() {
            return Some(f);
        }
        let (r, source) = state.as_mut()?;
        if last.elapsed() < WATCH_EVERY {
            return None;
        }
        last = std::time::Instant::now();
        match resolver::watch(r, source.as_mut(), &mut resolver::new_challenge) {
            WatchOutcome::Same(m) if Some(m) != mode => {
                mode = Some(m);
                let name = json!(m).as_str().unwrap_or("unknown").to_string();
                Some(header(format!("session mode: {name}"), json!({ "event": "sessionMode", "sessionMode": name })))
            }
            WatchOutcome::Same(_) => None,
            WatchOutcome::Gone => {
                source.close();
                state = None;
                Some(header("the game has exited".into(), json!({ "event": "exited" })))
            }
            WatchOutcome::Changed(why) => {
                source.close();
                state = None;
                Some(header(format!("the game no longer answers as the observed process ({why})"), json!({ "event": "identityChanged", "reason": why })))
            }
        }
    })
}

pub fn run(a: DebugArgs, json: bool) -> Result<i32> {
    if a.observe {
        return observe(&a, json);
    }
    if a.dap {
        return match plan(&a) {
            Ok(p) => proxy(p),
            Err(e) => refuse_all(&e),
        };
    }
    let mut p = plan(&a)?;
    p.source.close();
    let t = &p.target;
    let argv = t.argv();
    let env = t.env();
    let r = &t.resolved;
    if json {
        out::print_json(&json!({
            "ok": true, "kind": t.kind, "server": (t.kind == TargetKind::Server).then_some(&t.install), "install": t.install, "via": t.via, "lab": t.lab,
            "target": r.target, "pid": t.windows_pid(), "hostPid": r.target.host_pid,
            "wine": t.wine, "prefix": t.prefix, "netcoredbg": t.netcoredbg, "env": env, "command": argv,
            "maxPauseMs": t.max_pause_ms, "descriptor": r.descriptor_path, "notes": r.notes,
        }));
        return Ok(0);
    }
    let id = &r.target.identity;
    println!("{:<9} {}", t.what(), t.install.display());
    match r.target.host_pid {
        Some(h) => println!("pid       {} (Windows; created {}; host pid {h}, display only)", id.windows_pid, id.creation_time_100ns),
        None => println!("pid       {} (created {})", id.windows_pid, id.creation_time_100ns),
    }
    println!("session   {} (mode {:?}, .NET {})", id.session_nonce, r.target.session_mode, r.target.runtime_version);
    if let Some(p) = &t.prefix {
        println!("prefix    {}", p.display());
    }
    println!("debugger  {}", argv.join(" "));
    if let Some(ms) = t.max_pause_ms {
        println!("pauses    continued by kiln debug after {ms} ms (best effort)");
    }
    match t.kind {
        TargetKind::Server => {
            println!("\nEditors: run `kiln debug --server <server> --dap` as the debug adapter. Use only the DAP interpreter:");
            println!("a console session that ends while the server is paused can take the server down.");
        }
        TargetKind::Client => {
            println!("\nEditors: run `kiln debug --client <install> --allow-client-pause --dap` as the debug adapter. Every breakpoint");
            println!("stops the game; a stopped game can freeze or lose its connection. `--observe` shows logs without a debugger.");
        }
    }
    Ok(0)
}

// ---- Debug Adapter Protocol ----

/// One frame: `Content-Length: n\r\n\r\n<n bytes>`. None at end of input.
fn read_frame<R: BufRead>(r: &mut R) -> std::io::Result<Option<Vec<u8>>> {
    let mut len = None;
    let mut line = String::new();
    loop {
        line.clear();
        if r.read_line(&mut line)? == 0 {
            return Ok(None);
        }
        let l = line.trim_end();
        if l.is_empty() {
            if len.is_some() {
                break;
            }
            continue;
        }
        if let Some((k, v)) = l.split_once(':') {
            if k.trim().eq_ignore_ascii_case("content-length") {
                len = v.trim().parse::<usize>().ok();
            }
        }
    }
    let n = len.unwrap_or(0);
    if n > MAX_FRAME {
        return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, format!("a {n} byte DAP frame")));
    }
    let mut b = vec![0; n];
    r.read_exact(&mut b)?;
    Ok(Some(b))
}

fn write_frame<W: Write>(w: &mut W, body: &[u8]) -> std::io::Result<()> {
    write!(w, "Content-Length: {}\r\n\r\n", body.len())?;
    w.write_all(body)?;
    w.flush()
}

fn write_json<W: Write>(w: &mut W, v: &Value) -> std::io::Result<()> {
    write_frame(w, v.to_string().as_bytes())
}

fn error_response(req: &Value, seq: i64, message: &str) -> Value {
    json!({
        "seq": seq, "type": "response", "request_seq": req["seq"], "command": req["command"], "success": false,
        "message": message, "body": { "error": { "id": 1, "format": message, "showUser": true } },
    })
}

/// What happens to a message from the client.
#[derive(Debug, PartialEq)]
enum Policy {
    Forward,
    /// a disconnect, made safe
    Disconnect,
    Refuse(String),
}

/// The safety rules, applied to every client message before the adapter sees it.
fn apply_policy(msg: &mut Value, pid: u32) -> Policy {
    if msg["type"] != "request" {
        return Policy::Forward;
    }
    match msg["command"].as_str().unwrap_or_default() {
        "disconnect" => {
            make_safe_disconnect(msg);
            Policy::Disconnect
        }
        "launch" => Policy::Refuse("kiln debug only attaches to a running server; use an attach configuration".into()),
        "terminate" | "restart" | "terminateThreads" => Policy::Refuse("kiln debug never ends or restarts the server (use kiln run --restart); disconnect instead".into()),
        "attach" => {
            let asked = &msg["arguments"]["processId"];
            let n = match asked {
                Value::Null => Some(0),
                Value::Number(n) => n.as_u64(),
                Value::String(s) if s.trim().is_empty() => Some(0),
                Value::String(s) => s.trim().parse::<u64>().ok(),
                _ => None,
            };
            match n {
                Some(0) => {}
                Some(n) if n == u64::from(pid) => {}
                _ => return Policy::Refuse(format!("kiln debug attaches only to this server's process (pid {pid}), not to {asked}; omit processId")),
            }
            if !msg["arguments"].is_object() {
                msg["arguments"] = json!({});
            }
            msg["arguments"]["processId"] = json!(pid);
            Policy::Forward
        }
        _ => Policy::Forward,
    }
}

fn make_safe_disconnect(msg: &mut Value) {
    if !msg["arguments"].is_object() {
        msg["arguments"] = json!({});
    }
    let a = msg["arguments"].as_object_mut().expect("just made an object");
    a.insert("terminateDebuggee".into(), json!(false));
    a.insert("suspendDebuggee".into(), json!(false));
    a.remove("restart");
}

/// Don't offer the client what is refused anyway: VS Code's Stop then goes straight to disconnect.
fn patch_adapter_message(msg: &mut Value) {
    if msg["type"] == "response" && msg["command"] == "initialize" && msg["success"] == true {
        if !msg["body"].is_object() {
            msg["body"] = json!({});
        }
        for k in ["supportTerminateDebuggee", "supportSuspendDebuggee", "supportsTerminateRequest", "supportsRestartRequest", "supportsTerminateThreadsRequest"] {
            msg["body"][k] = json!(false);
        }
    }
}

/// Notices a mod reloaded under an attached session: its module comes a second time.
#[derive(Default)]
struct ModuleWatch {
    seen: std::collections::HashSet<String>,
}

impl ModuleWatch {
    fn reloaded(&mut self, m: &Value) -> Option<String> {
        if m["type"] != "event" || m["event"] != "module" || m["body"]["reason"] != "new" {
            return None;
        }
        let name = m["body"]["module"]["name"].as_str()?.to_string();
        (!self.seen.insert(name.clone())).then_some(name)
    }
}

/// netcoredbg keeps breakpoints on the first copy of a module, so they stop nowhere after a reload.
fn reload_notice(module: &str) -> Value {
    let text = format!("{module} was reloaded: breakpoints in it no longer stop. Disconnect and attach again.\n");
    json!({ "seq": OWN_SEQ, "type": "event", "event": "output", "body": { "category": "important", "output": text } })
}

/// Requests after which the debuggee runs again (the pause ends).
fn resumes(m: &Value) -> bool {
    m["type"] == "request" && matches!(m["command"].as_str(), Some("continue" | "next" | "stepIn" | "stepOut" | "stepBack" | "reverseContinue" | "goto"))
}

/// The pause watchdog (chapter 12a §4 step 8): a `stopped` event starts the clock, anything that resumes stops it, and
/// when the limit passes the proxy continues the debuggee itself. Best effort: a pending evaluation or a native lock
/// can delay the continue, and nothing here promises that a remote peer stays connected.
#[derive(Debug, Default)]
struct PauseWatch {
    max: Option<Duration>,
    since: Option<Instant>,
    since_utc: Option<String>,
    thread: Option<i64>,
}

#[derive(Debug, PartialEq)]
enum PauseChange {
    Paused,
    Resumed,
}

impl PauseWatch {
    fn new(max_ms: Option<u64>) -> PauseWatch {
        PauseWatch { max: max_ms.map(Duration::from_millis), ..Default::default() }
    }

    /// A message from the adapter.
    fn adapter(&mut self, m: &Value, now: Instant) -> Option<PauseChange> {
        if m["type"] != "event" {
            return None;
        }
        match m["event"].as_str() {
            Some("stopped") => {
                let first = self.since.is_none();
                self.thread = m["body"]["threadId"].as_i64().or(self.thread);
                if first {
                    self.since = Some(now);
                    self.since_utc = Some(kiln_manage::util::now_rfc3339());
                }
                Some(PauseChange::Paused)
            }
            Some("continued" | "terminated" | "exited") => self.resume(),
            _ => None,
        }
    }

    fn resume(&mut self) -> Option<PauseChange> {
        self.since_utc = None;
        self.since.take().map(|_| PauseChange::Resumed)
    }

    fn deadline(&self) -> Option<Instant> {
        Some(self.since? + self.max?)
    }

    /// The thread to continue when the limit has passed (the pause ends here).
    fn due(&mut self, now: Instant) -> Option<i64> {
        if self.deadline().is_some_and(|d| now >= d) {
            self.resume();
            return Some(self.thread.unwrap_or(0));
        }
        None
    }
}

enum Ev {
    /// the client is gone (stdin EOF or a signal)
    ClientGone(&'static str),
    /// the client asked to disconnect (request seq, as forwarded)
    ClientDisconnect,
    /// the client asked the debuggee to run on
    ClientResumed,
    /// the adapter answered this request
    AdapterResponse(i64),
    /// the adapter answered the client's attach
    Attached(bool),
    /// the adapter's stopped / continued / terminated events
    Pause(Value),
    AdapterEof,
    /// a watch poll of the Windows side
    Watch(WatchOutcome),
    /// the attach was not forwarded: the tuple no longer holds
    AttachRefused(String),
}

/// Blocks the signals that mean "the client is going away" and turns them into events. The mask is inherited, so this
/// runs after netcoredbg has been spawned.
#[cfg(unix)]
fn watch_signals(tx: mpsc::Sender<Ev>) {
    // SAFETY: sigset calls on a local set; sigwait only blocks this thread.
    unsafe {
        let mut set: libc::sigset_t = std::mem::zeroed();
        libc::sigemptyset(&mut set);
        for s in [libc::SIGINT, libc::SIGTERM, libc::SIGHUP] {
            libc::sigaddset(&mut set, s);
        }
        libc::pthread_sigmask(libc::SIG_BLOCK, &set, std::ptr::null_mut());
        std::thread::spawn(move || {
            loop {
                let mut sig = 0;
                if libc::sigwait(&set, &mut sig) == 0 {
                    let _ = tx.send(Ev::ClientGone(match sig {
                        libc::SIGINT => "SIGINT",
                        libc::SIGTERM => "SIGTERM",
                        _ => "SIGHUP",
                    }));
                }
            }
        });
    }
}

/// Windows: Ctrl-C, Ctrl-Break, closing the console, logoff and shutdown become the same event; on the last three
/// Windows ends this process soon after, so the handler holds it (about 4.5 s) for the safe detach.
#[cfg(windows)]
fn watch_signals(tx: mpsc::Sender<Ev>) {
    let tx = Mutex::new(tx);
    let ok = kiln_manage::debug::windows::watch_console(Box::new(move |what| {
        let _ = tx.lock().unwrap_or_else(|e| e.into_inner()).send(Ev::ClientGone(what));
    }));
    if !ok {
        eprintln!("kiln debug: console events cannot be watched; closing the console while attached may end the debugger");
    }
}

#[cfg(not(any(unix, windows)))]
fn watch_signals(_tx: mpsc::Sender<Ev>) {}

/// netcoredbg in its own process group: a Ctrl-C at the editor's terminal must not reach it while it is attached.
#[cfg(unix)]
fn own_group(c: &mut Command) -> Option<String> {
    use std::os::unix::process::CommandExt;
    c.process_group(0);
    None
}

/// Windows: no console (console events never reach it), its own process group, and out of a job that would end it
/// with the editor when the job allows that (kiln_manage::debug::windows::adapter_flags). Never a job of ours.
#[cfg(windows)]
fn own_group(c: &mut Command) -> Option<String> {
    use std::os::windows::process::CommandExt;
    let (flags, warning) = kiln_manage::debug::windows::adapter_flags(kiln_manage::debug::windows::job_facts());
    c.creation_flags(flags);
    warning
}

#[cfg(not(any(unix, windows)))]
fn own_group(_c: &mut Command) -> Option<String> {
    None
}

/// Windows: a console handler waiting for the safe detach may let the process end now.
fn detach_finished() {
    #[cfg(windows)]
    kiln_manage::debug::windows::detach_finished();
}

/// `gc` answers "<n> unloaded mod context(s) still alive": a mod that leaks its old copy keeps a stale module loaded, the
/// state in which breakpoints miss and disconnects have taken the server down (docs/kiln/modding/debugging.md).
fn stale_contexts_warning(gc: &Value) -> Option<String> {
    let message = gc["message"].as_str()?;
    let n: u64 = message.split_whitespace().next()?.parse().ok()?;
    (n > 0).then(|| format!("{message}: old copies of a reloaded mod are still loaded. Breakpoints may not stop, and a session in this state can take the server down. Restart the server before debugging."))
}

type ClientOut = Arc<Mutex<std::io::Stdout>>;

fn output_event(text: &str, category: &str) -> Value {
    json!({ "seq": OWN_SEQ, "type": "event", "event": "output", "body": { "category": category, "output": format!("{text}\n") } })
}

/// The session's state: the session file (so `kiln reload` and the editor know it is attached), a `kiln/debugState`
/// event to the client, and a line on stderr, at every change.
struct States {
    install: PathBuf,
    now: DebugSessionState,
    out: ClientOut,
    warned: bool,
}

impl States {
    fn set(&mut self, state: SessionState, reason: Option<&str>, paused_since: Option<String>) {
        self.now.state = state;
        self.now.reason = reason.map(str::to_string);
        self.now.paused_since = paused_since;
        self.publish();
    }

    fn mode(&mut self, mode: SessionMode) {
        if mode != self.now.session_mode {
            self.now.session_mode = mode;
            self.now.reason = Some("session_mode_changed".into());
            self.publish();
        }
    }

    fn publish(&mut self) {
        if let Err(e) = kiln_manage::debug::write_session(&self.install, &self.now)
            && !std::mem::replace(&mut self.warned, true)
        {
            eprintln!("kiln debug: cannot write the session file ({}); kiln reload will not know a debugger is attached", e.message);
        }
        let n = &self.now;
        eprintln!("kiln debug: state {} ({}{})", json!(n.state).as_str().unwrap_or("?"), json!(n.session_mode).as_str().unwrap_or("?"), n.reason.as_deref().map(|r| format!(", {r}")).unwrap_or_default());
        let ev = json!({ "seq": OWN_SEQ, "type": "event", "event": "kiln/debugState", "body": self.now });
        let _ = write_json(&mut *self.out.lock().unwrap(), &ev);
    }
}

fn proxy(p: Planned) -> Result<i32> {
    let Planned { target: t, host, source } = p;
    let host = Arc::new(Mutex::new(host));
    let source = Arc::new(Mutex::new(source));
    // A reload leaves the old mod's module loaded until the next collection, and netcoredbg can bind a breakpoint to that
    // dead copy. Attaching after a reload stopped in 1 of 5 rounds without this, and the server died in both test loops;
    // with it, 6 of 6 twice and no deaths.
    let mut warning = None;
    let mut group_warning = None;
    let gc = || match crate::control::send(&t.install, "gc", GC_WAIT) {
        Err(e) => eprintln!("kiln debug: no collection before attaching ({e}); after a reload, breakpoints may not stop"),
        Ok(r) => warning = stale_contexts_warning(&r),
    };
    let spawn = |permit: &AttachPermit| {
        // the tuple was checked an instant ago; netcoredbg gets nothing else to attach to than this PID
        debug_assert_eq!(permit.windows_pid(), t.windows_pid());
        let mut cmd = t.command();
        cmd.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::inherit());
        group_warning = own_group(&mut cmd);
        cmd.spawn()
    };
    let launched = {
        let mut h = host.lock().unwrap();
        let mut s = source.lock().unwrap();
        launch(&t.resolved, t.allow_client_pause, h.as_mut(), s.as_mut(), &mut resolver::new_challenge, gc, spawn)
    };
    let (permit, mut child) = match launched {
        Ok(x) => x,
        Err(e) => {
            source.lock().unwrap().close();
            return refuse_all(&cli_error(e));
        }
    };
    let pid = permit.windows_pid();
    let adapter_in = Arc::new(Mutex::new(Some(child.stdin.take().expect("piped"))));
    let adapter_out = child.stdout.take().expect("piped");
    let client_out: ClientOut = Arc::new(Mutex::new(std::io::stdout()));
    let mut states = States {
        install: t.install.clone(),
        now: DebugSessionState {
            identity: permit.identity().clone(),
            state: SessionState::Attaching,
            session_mode: permit.session_mode(),
            reason: None,
            paused_since: None,
            max_pause_ms: t.max_pause_ms,
        },
        out: client_out.clone(),
        warned: false,
    };
    states.publish();
    for w in [warning, group_warning].into_iter().flatten() {
        eprintln!("kiln debug: {w}");
        let _ = write_json(&mut *client_out.lock().unwrap(), &output_event(&w, "important"));
    }
    let (tx, rx) = mpsc::channel::<Ev>();
    watch_signals(tx.clone());

    // adapter -> client
    {
        let tx = tx.clone();
        let client_out = client_out.clone();
        std::thread::spawn(move || {
            let mut r = BufReader::new(adapter_out);
            let mut modules = ModuleWatch::default();
            while let Ok(Some(body)) = read_frame(&mut r) {
                let mut out = client_out.lock().unwrap();
                match serde_json::from_slice::<Value>(&body) {
                    Ok(mut m) => {
                        patch_adapter_message(&mut m);
                        if let Some(name) = modules.reloaded(&m) {
                            let _ = write_json(&mut *out, &reload_notice(&name));
                        }
                        let request_seq = m["request_seq"].as_i64();
                        // the response to the proxy's own disconnect (and own continue) stays here
                        if request_seq.is_none_or(|s| s < OWN_SEQ) {
                            let _ = write_json(&mut *out, &m);
                        }
                        drop(out);
                        if m["type"] == "response" {
                            if m["command"] == "attach" && request_seq.is_some_and(|s| s < OWN_SEQ) {
                                let _ = tx.send(Ev::Attached(m["success"] == true));
                            }
                            let _ = tx.send(Ev::AdapterResponse(request_seq.unwrap_or(-1)));
                        } else if m["type"] == "event" && matches!(m["event"].as_str(), Some("stopped" | "continued" | "terminated" | "exited")) {
                            let _ = tx.send(Ev::Pause(m));
                        }
                    }
                    Err(_) => {
                        let _ = write_frame(&mut *out, &body);
                    }
                }
            }
            let _ = tx.send(Ev::AdapterEof);
        });
    }

    // client -> adapter
    let client_disconnect_seq = Arc::new(Mutex::new(None::<i64>));
    {
        let tx = tx.clone();
        let adapter_in = adapter_in.clone();
        let client_out = client_out.clone();
        let client_disconnect_seq = client_disconnect_seq.clone();
        let (host, source, resolved) = (host.clone(), source.clone(), t.resolved.clone());
        std::thread::spawn(move || {
            let mut r = BufReader::new(std::io::stdin());
            let mut own_seq = OWN_SEQ + 1_000_000;
            loop {
                let body = match read_frame(&mut r) {
                    Ok(Some(b)) => b,
                    _ => break,
                };
                let Ok(mut m) = serde_json::from_slice::<Value>(&body) else {
                    eprintln!("kiln debug: dropped a client message that is not JSON");
                    continue;
                };
                let mut policy = apply_policy(&mut m, pid);
                // the whole tuple again, immediately before the attach goes to netcoredbg (chapter 12a §4 step 2)
                if policy == Policy::Forward && m["type"] == "request" && m["command"] == "attach" {
                    let checked = {
                        let mut h = host.lock().unwrap();
                        let mut s = source.lock().unwrap();
                        resolver::revalidate(&resolved, h.as_mut(), s.as_mut(), &mut resolver::new_challenge)
                    };
                    if let Err(e) = checked {
                        let _ = tx.send(Ev::AttachRefused(e.reason.clone().unwrap_or_else(|| e.code.as_str().into())));
                        policy = Policy::Refuse(format!("{}: {}", e.code, e.message));
                    }
                }
                match policy {
                    Policy::Refuse(why) => {
                        own_seq += 1;
                        let _ = write_json(&mut *client_out.lock().unwrap(), &error_response(&m, own_seq, &why));
                    }
                    Policy::Forward => {
                        if resumes(&m) {
                            let _ = tx.send(Ev::ClientResumed);
                        }
                        if let Some(w) = adapter_in.lock().unwrap().as_mut() {
                            let _ = write_json(w, &m);
                        }
                    }
                    Policy::Disconnect => {
                        *client_disconnect_seq.lock().unwrap() = m["seq"].as_i64();
                        if let Some(w) = adapter_in.lock().unwrap().as_mut() {
                            let _ = write_json(w, &m);
                        }
                        let _ = tx.send(Ev::ClientDisconnect);
                    }
                }
            }
            let _ = tx.send(Ev::ClientGone("stdin closed"));
        });
    }

    // the Windows side: has the debuggee ended, is it still itself, which world mode
    {
        let tx = tx.clone();
        let (source, resolved) = (source.clone(), t.resolved.clone());
        std::thread::spawn(move || {
            loop {
                std::thread::sleep(WATCH_EVERY);
                let w = resolver::watch(&resolved, source.lock().unwrap().as_mut(), &mut resolver::new_challenge);
                let last = !matches!(w, WatchOutcome::Same(_));
                if tx.send(Ev::Watch(w)).is_err() || last {
                    break;
                }
            }
        });
    }

    // Until the session ends: a disconnect is sent exactly once, and its response awaited.
    let mut pause = PauseWatch::new(t.max_pause_ms);
    let mut disconnect_sent = false;
    let mut awaiting: Option<i64> = None;
    let mut deadline = None;
    let mut own = OWN_SEQ + 1;
    let mut code = 0;
    let mut end: (SessionState, Option<String>) = (SessionState::Detached, None);
    let send_disconnect = |adapter_in: &Mutex<Option<std::process::ChildStdin>>| {
        let m = json!({ "seq": OWN_SEQ, "type": "request", "command": "disconnect", "arguments": { "terminateDebuggee": false, "suspendDebuggee": false } });
        if let Some(w) = adapter_in.lock().unwrap().as_mut() {
            let _ = write_json(w, &m);
        }
    };
    loop {
        let wake = [deadline, pause.deadline()].into_iter().flatten().min();
        let ev = match wake {
            Some(d) => match rx.recv_timeout(d.saturating_duration_since(Instant::now())) {
                Ok(e) => e,
                Err(mpsc::RecvTimeoutError::Timeout) => {
                    if let Some(thread) = pause.due(Instant::now()) {
                        own += 1;
                        let c = json!({ "seq": own, "type": "request", "command": "continue", "arguments": { "threadId": thread } });
                        if let Some(w) = adapter_in.lock().unwrap().as_mut() {
                            let _ = write_json(w, &c);
                        }
                        let ms = t.max_pause_ms.unwrap_or_default();
                        let text = format!("kiln debug: the {} was paused for {ms} ms (maxPauseMs) and was continued. A pending evaluation can delay this; a peer may still have timed out.", t.what());
                        eprintln!("{text}");
                        {
                            let mut out = client_out.lock().unwrap();
                            let _ = write_json(&mut *out, &json!({ "seq": OWN_SEQ, "type": "event", "event": "continued", "body": { "threadId": thread, "allThreadsContinued": true } }));
                            let _ = write_json(&mut *out, &output_event(&text, "important"));
                        }
                        states.set(SessionState::Running, Some("timeout_continue"), None);
                        continue;
                    }
                    if deadline.is_some_and(|d| Instant::now() >= d) {
                        eprintln!("kiln debug: netcoredbg has not finished after a disconnect; leaving it to detach (its stdin is closed)");
                        break;
                    }
                    continue;
                }
                Err(mpsc::RecvTimeoutError::Disconnected) => break,
            },
            None => match rx.recv() {
                Ok(e) => e,
                Err(_) => break,
            },
        };
        match ev {
            Ev::ClientGone(why) => {
                if !disconnect_sent {
                    eprintln!("kiln debug: {why}; detaching from the {}", t.what());
                    disconnect_sent = true;
                    awaiting = Some(OWN_SEQ);
                    send_disconnect(&adapter_in);
                    deadline = Some(Instant::now() + DISCONNECT_WAIT);
                    end.1.get_or_insert_with(|| "client_gone".into());
                    states.set(SessionState::Detaching, Some("client_gone"), None);
                }
            }
            Ev::ClientDisconnect => {
                disconnect_sent = true;
                awaiting = *client_disconnect_seq.lock().unwrap();
                deadline = Some(Instant::now() + DISCONNECT_WAIT);
                end.1.get_or_insert_with(|| "client_disconnect".into());
                states.set(SessionState::Detaching, Some("client_disconnect"), None);
            }
            Ev::ClientResumed => {
                if pause.resume() == Some(PauseChange::Resumed) {
                    states.set(SessionState::Running, None, None);
                }
            }
            Ev::Attached(ok) => {
                if ok {
                    states.set(SessionState::Running, None, None);
                } else {
                    end = (SessionState::Ended, Some("attach_failed".into()));
                    states.set(SessionState::Ended, Some("attach_failed"), None);
                }
            }
            Ev::AttachRefused(reason) => {
                end = (SessionState::Ended, Some(reason.clone()));
                states.set(SessionState::Ended, Some(&reason), None);
            }
            Ev::Pause(m) => match pause.adapter(&m, Instant::now()) {
                Some(PauseChange::Paused) => {
                    let mode = json!(states.now.session_mode);
                    let mode = mode.as_str().unwrap_or("unknown");
                    let text = match (t.kind, t.max_pause_ms) {
                        (TargetKind::Server, None) => format!("[kiln] the server is paused for everyone on it (session mode {mode})"),
                        (_, Some(ms)) => format!("[kiln] the {} is paused (session mode {mode}); it continues by itself after {ms} ms", t.what()),
                        (TargetKind::Client, None) => format!("[kiln] the game is paused (session mode {mode})"),
                    };
                    let _ = write_json(&mut *client_out.lock().unwrap(), &output_event(&text, "console"));
                    states.set(SessionState::Paused, None, pause.since_utc.clone());
                }
                Some(PauseChange::Resumed) if !disconnect_sent => states.set(SessionState::Running, None, None),
                _ => {}
            },
            Ev::Watch(w) => match w {
                WatchOutcome::Same(mode) => states.mode(mode),
                WatchOutcome::Gone | WatchOutcome::Changed(_) => {
                    let (reason, text) = match &w {
                        WatchOutcome::Gone => ("debuggee_exited".to_string(), format!("kiln debug: the {} (PID {pid}) has exited; the session ends.", t.what())),
                        WatchOutcome::Changed(r) => (
                            "identity_changed".to_string(),
                            format!("kiln debug: the {} answering for PID {pid} is no longer the attached process ({r}); detaching.", t.what()),
                        ),
                        WatchOutcome::Same(_) => unreachable!(),
                    };
                    eprintln!("{text}");
                    let _ = write_json(&mut *client_out.lock().unwrap(), &output_event(&text, "important"));
                    end = (SessionState::Ended, Some(reason.clone()));
                    if !disconnect_sent {
                        // a safe detach, never a kill: the adapter detaches (or finds nothing left to detach from)
                        disconnect_sent = true;
                        awaiting = Some(OWN_SEQ);
                        send_disconnect(&adapter_in);
                        deadline = Some(Instant::now() + DISCONNECT_WAIT);
                        let _ = write_json(&mut *client_out.lock().unwrap(), &json!({ "seq": OWN_SEQ + 1, "type": "event", "event": "terminated", "body": {} }));
                    }
                    states.set(SessionState::Detaching, Some(&reason), None);
                }
            },
            Ev::AdapterResponse(seq) => {
                if awaiting == Some(seq) {
                    awaiting = None;
                    // nothing more to say to it, and netcoredbg can sit on a read until its stdin closes
                    adapter_in.lock().unwrap().take();
                    deadline = Some(Instant::now() + EXIT_WAIT);
                }
            }
            Ev::AdapterEof => {
                if !disconnect_sent {
                    code = 1;
                    let gone = tell_client_adapter_died(&t, source.lock().unwrap().as_mut(), &client_out);
                    end = (SessionState::Ended, Some(if gone { "debuggee_exited" } else { "adapter_exited" }.into()));
                }
                break;
            }
        }
    }
    // never kill: closing stdin is what a clean netcoredbg exits on, and the child outlives us if it hangs
    adapter_in.lock().unwrap().take();
    let t0 = Instant::now();
    while t0.elapsed() < Duration::from_secs(5) {
        match child.try_wait() {
            Ok(Some(_)) | Err(_) => break,
            Ok(None) => std::thread::sleep(Duration::from_millis(50)),
        }
    }
    states.set(end.0, end.1.as_deref(), None);
    source.lock().unwrap().close();
    detach_finished();
    Ok(code)
}

/// netcoredbg ended while attached. Whether the debuggee went with it is the Windows side's answer (the held process),
/// never a Linux PID's. True when it is gone.
fn tell_client_adapter_died(t: &Target, source: &mut dyn ProcessSource, client_out: &Mutex<std::io::Stdout>) -> bool {
    let w = resolver::watch(&t.resolved, source, &mut resolver::new_challenge);
    let what = t.what();
    let (gone, text) = match w {
        WatchOutcome::Same(_) => (false, format!("netcoredbg exited while attached; the {what} is still running. Check that it still responds before trusting it.\n")),
        WatchOutcome::Gone => (true, format!("netcoredbg exited while attached and the {what} is gone with it; see kiln/logs/kiln.log and restart it.\n")),
        WatchOutcome::Changed(r) => (true, format!("netcoredbg exited while attached, and the {what} no longer answers as the attached process ({r}); see kiln/logs/kiln.log.\n")),
    };
    eprintln!("kiln debug: {}", text.trim_end());
    let mut out = client_out.lock().unwrap();
    let _ = write_json(&mut *out, &json!({ "seq": OWN_SEQ, "type": "event", "event": "output", "body": { "category": "important", "output": text } }));
    let _ = write_json(&mut *out, &json!({ "seq": OWN_SEQ + 1, "type": "event", "event": "terminated", "body": {} }));
    gone
}

/// `--dap` when there is nothing to attach to: an editor shows a response's message, but only "the adapter exited" for
/// a process that just quits. Every request fails with the reason (and the stable code in `body.error.kilnCode`) until
/// the client disconnects; that one succeeds and exits 0, because nothing is attached and a failure there would only
/// stack a second error on the refusal.
fn refuse_all(e: &anyhow::Error) -> Result<i32> {
    let (code, reason, why) = code_of(e);
    eprintln!("kiln debug: {why}");
    let mut r = BufReader::new(std::io::stdin());
    let mut w = std::io::stdout();
    let mut seq = OWN_SEQ;
    while let Ok(Some(body)) = read_frame(&mut r) {
        let Ok(m) = serde_json::from_slice::<Value>(&body) else { continue };
        if m["type"] != "request" {
            continue;
        }
        seq += 1;
        if m["command"] == "disconnect" {
            let _ = write_json(&mut w, &json!({ "seq": seq, "type": "response", "request_seq": m["seq"], "command": "disconnect", "success": true }));
            return Ok(0);
        }
        let mut resp = error_response(&m, seq, &why);
        resp["body"]["error"]["kilnCode"] = json!(code);
        resp["body"]["error"]["kilnReason"] = json!(reason);
        let _ = write_json(&mut w, &resp);
    }
    Ok(1)
}

/// The fake DAP adapter suite (`PASS debug.detach-policy`, `PASS debug.pause-watchdog`): Linux test builds only.
#[cfg(all(test, target_os = "linux"))]
#[path = "../tests/debug_dap/suite.rs"]
mod dap_suite;

#[cfg(test)]
mod tests {
    use super::*;
    use kiln_manage::debug::{Descriptor, PrefixIdentity};
    use kiln_manage::debug::resolver::fake::{FakeHost, FakeProc, FakeSource, counter};

    fn req(command: &str, arguments: Value) -> Value {
        json!({ "seq": 7, "type": "request", "command": command, "arguments": arguments })
    }

    #[test]
    fn every_disconnect_keeps_the_debuggee() {
        for args in [json!({ "terminateDebuggee": true }), json!({ "terminateDebuggee": false }), json!({ "restart": true, "terminateDebuggee": true }), json!({}), Value::Null] {
            let mut m = req("disconnect", args);
            assert_eq!(apply_policy(&mut m, 32), Policy::Disconnect);
            assert_eq!(m["arguments"]["terminateDebuggee"], json!(false));
            assert_eq!(m["arguments"]["suspendDebuggee"], json!(false));
            assert!(m["arguments"].get("restart").is_none());
        }
        let mut m = json!({ "seq": 1, "type": "request", "command": "disconnect" });
        assert_eq!(apply_policy(&mut m, 32), Policy::Disconnect);
        assert_eq!(m["arguments"]["terminateDebuggee"], json!(false));
    }

    #[test]
    fn what_could_end_or_start_a_process_is_refused() {
        for c in ["launch", "terminate", "restart", "terminateThreads"] {
            assert!(matches!(apply_policy(&mut req(c, json!({})), 32), Policy::Refuse(_)), "{c}");
        }
    }

    #[test]
    fn attach_only_to_the_resolved_pid() {
        for ok in [json!({}), json!({ "processId": 0 }), json!({ "processId": 32 }), json!({ "processId": "32" }), json!({ "processId": "" }), json!({ "processId": null }), Value::Null] {
            let mut m = req("attach", ok.clone());
            assert_eq!(apply_policy(&mut m, 32), Policy::Forward, "{ok}");
            assert_eq!(m["arguments"]["processId"], json!(32), "{ok}");
        }
        for bad in [json!({ "processId": 33 }), json!({ "processId": "33" }), json!({ "processId": -1 }), json!({ "processId": "${command:pickProcess}" }), json!({ "processId": [32] })] {
            assert!(matches!(apply_policy(&mut req("attach", bad.clone()), 32), Policy::Refuse(_)), "{bad}");
        }
    }

    #[test]
    fn others_pass_untouched() {
        let mut m = req("setBreakpoints", json!({ "source": { "path": "/a.cs" } }));
        let before = m.clone();
        assert_eq!(apply_policy(&mut m, 32), Policy::Forward);
        assert_eq!(m, before);
        // a client's response to a reverse request is not a request
        let mut r = json!({ "seq": 2, "type": "response", "command": "disconnect", "request_seq": 1, "success": true });
        let before = r.clone();
        assert_eq!(apply_policy(&mut r, 32), Policy::Forward);
        assert_eq!(r, before);
    }

    #[test]
    fn capabilities_are_not_offered() {
        let mut m = json!({ "seq": 1, "type": "response", "command": "initialize", "request_seq": 1, "success": true,
                            "body": { "supportsTerminateRequest": true, "supportTerminateDebuggee": true, "supportsConfigurationDoneRequest": true } });
        patch_adapter_message(&mut m);
        assert_eq!(m["body"]["supportsTerminateRequest"], json!(false));
        assert_eq!(m["body"]["supportTerminateDebuggee"], json!(false));
        assert_eq!(m["body"]["supportsRestartRequest"], json!(false));
        assert_eq!(m["body"]["supportsConfigurationDoneRequest"], json!(true));
    }

    #[test]
    fn leaked_contexts_warn() {
        assert!(stale_contexts_warning(&json!({ "message": "0 unloaded mod context(s) still alive" })).is_none());
        assert!(stale_contexts_warning(&json!({ "message": "2 unloaded mod context(s) still alive" })).unwrap().contains("Restart the server"));
        assert!(stale_contexts_warning(&json!({ "ok": true })).is_none());
    }

    #[test]
    fn a_module_that_comes_twice_is_a_reload() {
        let new = |n: &str| json!({ "seq": 1, "type": "event", "event": "module", "body": { "reason": "new", "module": { "name": n } } });
        let mut w = ModuleWatch::default();
        assert_eq!(w.reloaded(&new("TplCode")), None);
        assert_eq!(w.reloaded(&new("Other")), None);
        assert_eq!(w.reloaded(&new("TplCode")), Some("TplCode".to_string()));
        assert_eq!(w.reloaded(&json!({ "type": "event", "event": "output", "body": {} })), None);
        let gone = json!({ "type": "event", "event": "module", "body": { "reason": "removed", "module": { "name": "Other" } } });
        assert_eq!(w.reloaded(&gone), None);
    }

    #[test]
    fn frames_round_trip() {
        let mut b = Vec::new();
        write_frame(&mut b, b"{\"a\":1}").unwrap();
        write_frame(&mut b, b"{}").unwrap();
        let mut r = std::io::Cursor::new(b);
        assert_eq!(read_frame(&mut r).unwrap().unwrap(), b"{\"a\":1}");
        assert_eq!(read_frame(&mut r).unwrap().unwrap(), b"{}");
        assert!(read_frame(&mut r).unwrap().is_none());
        let mut r = std::io::Cursor::new(b"content-length:2\r\nContent-Type: x\r\n\r\n{}".to_vec());
        assert_eq!(read_frame(&mut r).unwrap().unwrap(), b"{}");
    }

    #[test]
    fn lab_prefix_is_a_sibling() {
        assert_eq!(with_pfx(Path::new("/srv/labs/mylab")), PathBuf::from("/srv/labs/mylab.pfx"));
    }

    #[test]
    fn pause_watch_continues_after_the_limit_only() {
        let t0 = Instant::now();
        let stopped = |tid: i64| json!({ "type": "event", "event": "stopped", "body": { "reason": "breakpoint", "threadId": tid } });
        let mut w = PauseWatch::new(Some(5000));
        assert_eq!(w.deadline(), None);
        assert_eq!(w.adapter(&stopped(7), t0), Some(PauseChange::Paused));
        assert!(w.since_utc.is_some());
        // a second stop (another thread) does not restart the clock
        assert_eq!(w.adapter(&stopped(9), t0 + Duration::from_millis(3000)), Some(PauseChange::Paused));
        assert_eq!(w.deadline(), Some(t0 + Duration::from_millis(5000)));
        assert_eq!(w.due(t0 + Duration::from_millis(4999)), None);
        assert_eq!(w.due(t0 + Duration::from_millis(5000)), Some(9));
        assert_eq!(w.deadline(), None, "the pause ended with the continue");
        assert_eq!(w.due(t0 + Duration::from_millis(9000)), None, "continued once");
        // the client resumes in time: no continue from the proxy
        assert_eq!(w.adapter(&stopped(7), t0), Some(PauseChange::Paused));
        assert!(resumes(&req("next", json!({ "threadId": 7 }))));
        assert!(!resumes(&req("evaluate", json!({ "expression": "x" }))));
        assert_eq!(w.resume(), Some(PauseChange::Resumed));
        assert_eq!(w.due(t0 + Duration::from_secs(60)), None);
        // the adapter says it runs again
        assert_eq!(w.adapter(&stopped(7), t0), Some(PauseChange::Paused));
        assert_eq!(w.adapter(&json!({ "type": "event", "event": "continued", "body": { "threadId": 7 } }), t0), Some(PauseChange::Resumed));
        assert_eq!(w.adapter(&json!({ "type": "event", "event": "output", "body": {} }), t0), None);
        // no limit (a server without --max-pause-ms): never continued by the proxy
        let mut off = PauseWatch::new(None);
        off.adapter(&stopped(1), t0);
        assert_eq!(off.deadline(), None);
        assert_eq!(off.due(t0 + Duration::from_secs(3600)), None);
    }

    #[test]
    fn max_pause_bounds() {
        let a = |ms: Option<u64>| DebugArgs { max_pause_ms: ms, ..Default::default() };
        assert_eq!(max_pause(&a(None), TargetKind::Server).unwrap(), None);
        assert_eq!(max_pause(&a(None), TargetKind::Client).unwrap(), Some(5000));
        assert_eq!(max_pause(&a(Some(100)), TargetKind::Server).unwrap(), Some(100));
        assert_eq!(max_pause(&a(Some(60_000)), TargetKind::Client).unwrap(), Some(60_000));
        for bad in [0, 99, 60_001] {
            let e = max_pause(&a(Some(bad)), TargetKind::Server).unwrap_err();
            assert_eq!(e.downcast_ref::<CliError>().unwrap().code, "usage");
        }
    }

    // ---- PASS debug.identity: every refusal starts no adapter ----

    const SHA: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const N1: &str = "11111111111111111111111111111111";
    const N2: &str = "22222222222222222222222222222222";

    struct World {
        root: PathBuf,
    }

    impl World {
        fn new(tag: &str) -> World {
            let root = std::env::temp_dir().join(format!("kiln-cli-debug-{tag}-{}", std::process::id()));
            let _ = std::fs::remove_dir_all(&root);
            for d in ["a", "b"] {
                std::fs::create_dir_all(root.join(d).join("kiln/state/debug")).unwrap();
                std::fs::write(root.join(d).join(SERVER_EXE), b"MZ").unwrap();
            }
            World { root }
        }
        fn kiln(&self, d: &str, pid: u32, creation: u64, nonce: &str, kind: TargetKind) -> (FakeProc, Descriptor) {
            let image = kiln_manage::winpath::unix_to_z(&self.root.join(d).join(SERVER_EXE)).unwrap();
            FakeProc::kiln(pid, &image, creation, SHA, nonce, kind)
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
                    install_exe: self.root.join(d).join(SERVER_EXE),
                    prefix: None,
                },
                facts: HostFacts { prefix_identity: None, runner_sha256: Some("r".repeat(64)), runner_context: None, host_pid: Some(4001) },
                calls: 0,
                later: None,
            }
        }
    }

    impl Drop for World {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.root);
        }
    }

    /// One attempt through the real order (consent, resolve, gc, permit, spawn), counting adapter starts.
    fn attempt(kind: TargetKind, allow: bool, host: &mut FakeHost, src: &mut FakeSource, starts: &mut u32) -> std::result::Result<u32, DebugError> {
        let mut ch = counter();
        let r = prepare(kind, allow, host, src, &mut ch)?;
        let (p, ()) = launch(&r, allow, host, src, &mut ch, || {}, |p: &AttachPermit| {
            assert_eq!(p.windows_pid(), r.target.identity.windows_pid);
            *starts += 1;
            Ok(())
        })?;
        Ok(p.windows_pid())
    }

    #[test]
    fn identity_refusals_start_no_adapter() {
        let w = World::new("identity");
        type Case = (&'static str, DebugCode, Box<dyn Fn(&World) -> (TargetKind, bool, FakeHost, FakeSource)>);
        let server = TargetKind::Server;
        let cases: Vec<Case> = vec![
            ("two same-name processes in different folders (b's descriptor in a)", DebugCode::Refused, Box::new(move |w| {
                let (pb, db) = w.kiln("b", 64, 200, N2, server);
                w.put("a", &db);
                (server, false, w.host("a", server), FakeSource::new(vec![pb]))
            })),
            ("PID reuse", DebugCode::NotRunning, Box::new(move |w| {
                let (_, d) = w.kiln("a", 32, 100, N1, server);
                w.put("a", &d);
                let (other, _) = w.kiln("a", 32, 999, N1, server);
                (server, false, w.host("a", server), FakeSource::new(vec![other]))
            })),
            ("exit between resolve and attach", DebugCode::NotRunning, Box::new(move |w| {
                let (p, d) = w.kiln("a", 32, 100, N1, server);
                w.put("a", &d);
                let mut s = FakeSource::new(vec![p]);
                s.before_prepare = Some(Box::new(|t| t.clear()));
                (server, false, w.host("a", server), s)
            })),
            ("PID reused between resolve and attach", DebugCode::IdentityChanged, Box::new(move |w| {
                let (p, d) = w.kiln("a", 32, 100, N1, server);
                w.put("a", &d);
                let mut s = FakeSource::new(vec![p]);
                s.before_prepare = Some(Box::new(|t| t[0].creation_time_100ns = "101".into()));
                (server, false, w.host("a", server), s)
            })),
            ("stale descriptor", DebugCode::NotRunning, Box::new(move |w| {
                let (_, d) = w.kiln("a", 32, 100, N1, server);
                w.put("a", &d);
                (server, false, w.host("a", server), FakeSource::default())
            })),
            ("elevation mismatch", DebugCode::PrivilegeMismatch, Box::new(move |w| {
                let (p, mut d) = w.kiln("a", 32, 100, N1, server);
                d.elevated = true;
                w.put("a", &d);
                let mut s = FakeSource::new(vec![p]);
                s.elevated = Some(false);
                (server, false, w.host("a", server), s)
            })),
            ("another user's process", DebugCode::PrivilegeMismatch, Box::new(move |w| {
                let (mut p, d) = w.kiln("a", 32, 100, N1, server);
                p.denied = true;
                w.put("a", &d);
                (server, false, w.host("a", server), FakeSource::new(vec![p]))
            })),
            ("wrong nonce on the pipe", DebugCode::NotRunning, Box::new(move |w| {
                let (mut p, d) = w.kiln("a", 32, 100, N1, server);
                p.pipe.as_mut().unwrap().session_nonce = N2.into();
                w.put("a", &d);
                (server, false, w.host("a", server), FakeSource::new(vec![p]))
            })),
            ("challenge not echoed", DebugCode::NotRunning, Box::new(move |w| {
                let (mut p, d) = w.kiln("a", 32, 100, N1, server);
                p.pipe.as_mut().unwrap().echo = false;
                w.put("a", &d);
                (server, false, w.host("a", server), FakeSource::new(vec![p]))
            })),
            ("prefix replaced between resolve and attach", DebugCode::IdentityChanged, Box::new(move |w| {
                let (p, d) = w.kiln("a", 32, 100, N1, server);
                w.put("a", &d);
                let mut h = w.host("a", server);
                h.later = Some(HostFacts { runner_sha256: Some("s".repeat(64)), ..h.facts.clone() });
                (server, false, h, FakeSource::new(vec![p]))
            })),
            ("several processes with a descriptor", DebugCode::Refused, Box::new(move |w| {
                let (p1, d1) = w.kiln("a", 32, 100, N1, server);
                let (p2, d2) = w.kiln("a", 48, 300, N2, server);
                w.put("a", &d1);
                w.put("a", &d2);
                (server, false, w.host("a", server), FakeSource::new(vec![p1, p2]))
            })),
            ("developer debugging off", DebugCode::Refused, Box::new(move |w| (server, false, w.host("a", server), FakeSource::default()))),
            (".NET not running", DebugCode::RuntimeNotReady, Box::new(move |w| {
                let (mut p, d) = w.kiln("a", 32, 100, N1, server);
                p.pipe.as_mut().unwrap().managed_ready = false;
                w.put("a", &d);
                (server, false, w.host("a", server), FakeSource::new(vec![p]))
            })),
            ("identity not provable (Wine lacks a query)", DebugCode::UnsupportedIdentity, Box::new(move |w| {
                let (p, d) = w.kiln("a", 32, 100, N1, server);
                w.put("a", &d);
                let mut s = FakeSource::new(vec![p]);
                s.fail = Some(DebugError::new(DebugCode::UnsupportedIdentity, "no GetProcessTimes"));
                (server, false, w.host("a", server), s)
            })),
            ("client without consent", DebugCode::PauseConsentRequired, Box::new(move |w| {
                let (p, d) = w.kiln("a", 32, 100, N1, TargetKind::Client);
                w.put("a", &d);
                (TargetKind::Client, false, w.host("a", TargetKind::Client), FakeSource::new(vec![p]))
            })),
        ];
        let mut starts = 0;
        for (name, want, setup) in &cases {
            for d in ["a", "b"] {
                for e in std::fs::read_dir(w.root.join(d).join("kiln/state/debug")).unwrap().flatten() {
                    std::fs::remove_file(e.path()).unwrap();
                }
            }
            let (kind, allow, mut host, mut src) = setup(&w);
            let before = starts;
            let e = attempt(kind, allow, &mut host, &mut src, &mut starts).expect_err(name);
            assert_eq!(e.code, *want, "{name}: {}", e.message);
            assert_eq!(starts, before, "{name}: an adapter was started");
            if *want == DebugCode::PauseConsentRequired {
                assert!(src.calls.is_empty(), "{name}: the Windows side was asked before consent: {:?}", src.calls);
            }
        }
        // the Steam guard refuses before any lookup (nothing exists at that path, and nothing is looked at)
        let steam = DebugArgs { server: Some("/nowhere/SteamLibrary/steamapps/common/EnshroudedServer".into()), ..Default::default() };
        let e = plan(&steam).err().expect("a Steam server is refused");
        assert_eq!(code_of(&e).0, "refused");
        assert_eq!(code_of(&e).1.as_deref(), Some("steam_library"));
        // the control: the same path does start an adapter when everything agrees (the counter can count)
        let (p, d) = w.kiln("a", 32, 100, N1, server);
        w.put("a", &d);
        let pid = attempt(server, false, &mut w.host("a", server), &mut FakeSource::new(vec![p.clone()]), &mut starts).unwrap();
        assert_eq!((pid, starts), (32, 1));
        // and a client with consent
        let (pc, mut dc) = (p, d);
        dc.side = TargetKind::Client;
        std::fs::remove_file(w.root.join("a/kiln/state/debug/32.json")).unwrap();
        w.put("a", &dc);
        attempt(TargetKind::Client, true, &mut w.host("a", TargetKind::Client), &mut FakeSource::new(vec![pc]), &mut starts).unwrap();
        assert_eq!(starts, 2);
        println!("PASS debug.identity ({} refusal cases and the Steam server guard: 0 adapter starts; 2 control cases: 2 starts)", cases.len());
    }

    #[test]
    fn a_client_without_consent_starts_nothing() {
        let before = ADAPTER_STARTS.with(|n| n.get());
        for dap in [true, false] {
            let a = DebugArgs { client: Some("no-such-install-id".into()), dap, ..Default::default() };
            let e = plan(&a).err().expect("refused");
            assert_eq!(code_of(&e).0, "pause_consent_required", "dap={dap}: {}", code_of(&e).2);
        }
        assert_eq!(ADAPTER_STARTS.with(|n| n.get()), before);
    }

    #[test]
    fn client_flags_and_their_conflicts() {
        use clap::Parser;
        #[derive(clap::Parser)]
        struct T {
            #[command(flatten)]
            a: DebugArgs,
        }
        let ok = |v: &[&str]| T::try_parse_from(std::iter::once("t").chain(v.iter().copied())).is_ok();
        assert!(ok(&["--client", "x", "--observe"]));
        assert!(ok(&["--client", "x", "--allow-client-pause", "--dap"]));
        assert!(ok(&["--client", "x", "--allow-client-pause", "--dap", "--max-pause-ms", "2000"]));
        assert!(!ok(&["--client", "x", "--observe", "--dap"]), "observe and DAP are exclusive");
        assert!(!ok(&["--client", "x", "--observe", "--allow-client-pause"]));
        assert!(!ok(&["--client", "x", "--observe", "--max-pause-ms", "200"]));
        assert!(!ok(&["--allow-client-pause"]), "consent belongs to a client");
        assert!(!ok(&["--observe"]));
        assert!(!ok(&["--client", "x", "--server", "y"]));
    }

    #[test]
    fn two_prefixes_with_the_same_windows_pid_never_mix() {
        let w = World::new("prefixes");
        let client = TargetKind::Client;
        let (pa, da) = w.kiln("a", 32, 100, N1, client);
        let (pb, db) = w.kiln("b", 32, 900, N2, client);
        w.put("a", &da);
        w.put("b", &db);
        let pfx = |p: &str, inode: u64| Some(PrefixIdentity { path: p.into(), device: 1, inode });
        let mut ha = w.host("a", client);
        ha.facts.prefix_identity = pfx("/c/a/pfx", 1);
        let mut hb = w.host("b", client);
        hb.facts.prefix_identity = pfx("/c/b/pfx", 2);
        let (mut sa, mut sb) = (FakeSource::new(vec![pa]), FakeSource::new(vec![pb]));
        let ra = prepare(client, true, &mut ha, &mut sa, &mut counter()).unwrap();
        let rb = prepare(client, true, &mut hb, &mut sb, &mut counter()).unwrap();
        let (ia, ib) = (&ra.target.identity, &rb.target.identity);
        assert_eq!((ia.windows_pid, ib.windows_pid), (32, 32));
        assert_eq!(ia.first_difference(ib), Some("installId"));
        assert_ne!(ia.prefix_identity, ib.prefix_identity);
        // a's target against b's namespace and host: refused, nothing started
        let mut starts = 0;
        let e = launch(&ra, true, &mut hb, &mut sb, &mut counter(), || {}, |_: &AttachPermit| {
            starts += 1;
            Ok(())
        })
        .unwrap_err();
        assert_eq!(e.code, DebugCode::IdentityChanged);
        assert_eq!(starts, 0);
        // a's own namespace: started once
        launch(&ra, true, &mut ha, &mut sa, &mut counter(), || {}, |p: &AttachPermit| {
            assert_eq!(p.identity().prefix_identity, pfx("/c/a/pfx", 1));
            starts += 1;
            Ok(())
        })
        .unwrap();
        assert_eq!(starts, 1);
    }

    #[test]
    fn observe_shows_the_log_and_starts_no_adapter() {
        let before = ADAPTER_STARTS.with(|n| n.get());
        let w = World::new("observe");
        let (p, d) = w.kiln("a", 32, 100, N1, TargetKind::Client);
        w.put("a", &d);
        let log = w.root.join("a/kiln/logs/kiln.log");
        std::fs::create_dir_all(log.parent().unwrap()).unwrap();
        std::fs::write(&log, "[I 12:00:00.000] [mod:counter] counter 1\n").unwrap();
        // no consent asked: observing resolves, it never permits or launches
        let mut host = w.host("a", TargetKind::Client);
        let mut src = FakeSource::new(vec![p]);
        let r = resolver::resolve(&mut host, &mut src, &mut counter()).unwrap();
        src.procs.clear(); // the game exits while it is observed
        let mut poll = observe_poll(Ok((r, Box::new(src))), |t, _| format!("[kiln debug] {t}"));
        let appender = {
            let log = log.clone();
            std::thread::spawn(move || {
                std::thread::sleep(Duration::from_millis(600));
                use std::io::Write;
                let mut f = std::fs::OpenOptions::new().append(true).open(&log).unwrap();
                writeln!(f, "[I 12:00:01.000] [mod:counter] counter 2").unwrap();
            })
        };
        let mut out = Vec::new();
        crate::logs::observe(&log, 20, false, &mut out, &mut poll, Some(14)).unwrap();
        appender.join().unwrap();
        let text = String::from_utf8(out).unwrap();
        assert!(text.starts_with("[kiln debug] observing PID 32 (created 100), session mode hosted"), "{text}");
        assert!(text.contains("counter 1") && text.contains("counter 2"), "{text}");
        assert!(text.contains("[kiln debug] the game has exited"), "{text}");
        // a target that cannot be proven: the log alone
        let mut poll = observe_poll(Err(DebugError::refused("no debug descriptor", "developer_debugging_off")), |t, _| t);
        let mut out = Vec::new();
        crate::logs::observe(&log, 1, true, &mut out, &mut poll, Some(1)).unwrap();
        let text = String::from_utf8(out).unwrap();
        assert!(text.starts_with("observing the log only: no debug descriptor (refused)"), "{text}");
        assert!(text.contains(r#""text":"counter 2""#), "{text}");
        assert_eq!(ADAPTER_STARTS.with(|n| n.get()), before, "observing started an adapter");
    }

    #[test]
    fn refusal_bodies_carry_the_code() {
        let e = cli_error(DebugError::identity_changed("gone", "creation_time"));
        let (code, reason, msg) = code_of(&e);
        assert_eq!((code.as_str(), reason.as_deref(), msg.as_str()), ("identity_changed", Some("creation_time"), "gone"));
        let c = e.downcast_ref::<CliError>().unwrap();
        assert_eq!(c.detail.as_ref().unwrap()["reason"], "creation_time");
    }
}
