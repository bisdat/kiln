//! WebKit's process sandbox on Linux (0.2.0-dev.29.18): what WebKitGTK does with it in this start, the player's choice
//! to turn it off (never the launcher's own), and a probe of the bwrap WebKit would run.
//!
//! What WebKitGTK does (its source, 2.38 to 2.54; docs/kiln/launcher/app.md, "WebKit's sandbox (Linux)"):
//! - the sandbox starts the web process inside bubblewrap (`bwrap`, the path WebKit was built with). WebKitGTK runs it
//!   by default only for its GTK 4 API (webkitgtk-6.0). The launcher uses the GTK 3 API (webkit2gtk-4.1, through wry),
//!   which runs it only when the program asks (`webkit_web_context_set_sandbox_enabled`; wry and Kiln never do) or
//!   `WEBKIT_FORCE_SANDBOX=1` is set. So a default start runs its web process without bwrap;
//! - `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS` turns it off: since 2.40 with the value `1`, since 2.46 with any value
//!   but `0`. Before 2.40, `WEBKIT_FORCE_SANDBOX` set to anything but `1` did;
//! - when bwrap cannot start, WebKit ends the whole program (`g_error`: SIGTRAP), usually at "Failed to fully launch
//!   dbus-proxy": no blank window, no watchdog line;
//! - the AppImage's libwebkit2gtk has every "/usr" replaced by "././" (Tauri's bundler; AppRun changes into
//!   `$APPDIR/usr`), so its bwrap is `$APPDIR/usr/bin/bwrap`, which the AppImage does not have.
//!
//! So [`prepare`] (before GTK): the choice ([`decide`]: `--no-webkit-sandbox` / `--webkit-sandbox`, then
//! `KILN_LAUNCHER_WEBKIT_SANDBOX=off|on`, then a restart's flag, then `state/webkit-sandbox.json`; nothing: the
//! variable is never set), the variable ([`variable`]: a value the player set stays), what WebKit does ([`webkit_use`]),
//! the probe ([`probe_with`]: that bwrap, as WebKit runs it, a no-op), and the message when WebKit would use a sandbox
//! that cannot start (crate::dialog).

use kiln_manage::paths::DataDirs;
use kiln_manage::Error;
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::OnceLock;
use std::time::{Duration, Instant};

/// WebKitGTK's own switch (2.40 and later).
pub const VAR: &str = "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS";
/// Runs the sandbox for the GTK 3 API (WebKitGTK's own).
pub const FORCE_VAR: &str = "WEBKIT_FORCE_SANDBOX";
pub const ENV: &str = "KILN_LAUNCHER_WEBKIT_SANDBOX";
pub const ARG_OFF: &str = "--no-webkit-sandbox";
pub const ARG_ON: &str = "--webkit-sandbox";
/// Set only for the start a message's "off" button restarts into: `probe` (the start's message) or `late` (the page did
/// not start). It turns the sandbox off even when the marker could not be written, and writes it once the beta copy's
/// gate is open ([`after_gate`]).
pub(crate) const RESTARTED: &str = "KILN_LAUNCHER_WEBKIT_SANDBOX_RESTARTED";
/// The [`VAR`] this process set itself, so a launcher started from it (a restart) tells it from the player's own; a
/// program of the host never gets it (kiln_manage::hostenv).
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
const SET_BY_US: &str = kiln_manage::hostenv::SANDBOX_SET;
/// How long the probe may take.
pub const PROBE_TIMEOUT: Duration = Duration::from_secs(2);
/// Debug and QA builds (`dev-eval`) only: `works`, `fails`, `missing` or `unknown` replaces the probe's result, so the
/// messages can be tested on a machine where bwrap works (on a private Xvfb display).
#[cfg(any(debug_assertions, feature = "dev-eval"))]
pub const TEST_PROBE: &str = "KILN_LAUNCHER_TEST_SANDBOX_PROBE";

/// The player's choice for WebKit's sandbox.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Want {
    /// Nothing asked, or asked on: the launcher leaves WebKit as it is.
    On,
    /// The launcher sets [`VAR`].
    Off,
}

/// Why the choice is what it is.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Why {
    /// Nothing asked.
    Default,
    /// `state/webkit-sandbox.json` (Settings, or a message's button at an earlier start).
    Marker,
    /// This start is a message's restart.
    Restart,
    /// `KILN_LAUNCHER_WEBKIT_SANDBOX`.
    Environment,
    /// `--no-webkit-sandbox` / `--webkit-sandbox`.
    Argument,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Choice {
    pub want: Want,
    pub why: Why,
    /// A `KILN_LAUNCHER_WEBKIT_SANDBOX` value that is neither (said in the log).
    pub ignored: Option<String>,
}

/// The choice for this start. Precedence, as for the render mode: the arguments (the last of `--no-webkit-sandbox` /
/// `--webkit-sandbox`), then `KILN_LAUNCHER_WEBKIT_SANDBOX`, then a message's restart, then the marker, then on.
pub fn decide(args: &[String], env: Option<&str>, restarted: bool, marker: bool) -> Choice {
    let arg = args.iter().skip(1).rev().find_map(|a| match a.as_str() {
        ARG_OFF => Some(Want::Off),
        ARG_ON => Some(Want::On),
        _ => None,
    });
    let mut ignored = None;
    let env = match env.map(|v| v.trim().to_ascii_lowercase()) {
        Some(v) if v == "off" => Some(Want::Off),
        Some(v) if v == "on" => Some(Want::On),
        Some(v) if !v.is_empty() => {
            ignored = Some(format!("{ENV}={v} is neither off nor on: ignored"));
            None
        }
        _ => None,
    };
    let (want, why) = match (arg, env) {
        (Some(w), _) => (w, Why::Argument),
        (None, Some(w)) => (w, Why::Environment),
        _ if restarted => (Want::Off, Why::Restart),
        _ if marker => (Want::Off, Why::Marker),
        _ => (Want::On, Why::Default),
    };
    Choice { want, why, ignored }
}

/// What the launcher does with [`VAR`] for this start.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase", tag = "kind", content = "value")]
pub enum Var {
    /// Nothing: the choice is on (or nothing was asked).
    Untouched,
    /// The choice is off: the launcher sets `VAR=1` before GTK and WebKit start.
    Set,
    /// The player set it themselves (the value): left as it is, whatever the choice.
    Yours(String),
}

/// `yours`: [`VAR`] as the environment has it, after what an earlier Kiln set was taken back.
pub fn variable(c: &Choice, yours: Option<&str>) -> Var {
    match yours {
        Some(v) => Var::Yours(v.to_string()),
        None if c.want == Want::Off => Var::Set,
        None => Var::Untouched,
    }
}

/// Whether WebKit runs its sandbox in this start.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Webkit {
    /// It starts the web process inside bwrap.
    On,
    /// A variable turned it off.
    Off,
    /// The GTK 3 API without `WEBKIT_FORCE_SANDBOX=1`: WebKit does not run it (a default start).
    Unused,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Use {
    pub webkit: Webkit,
    /// Why, in words: "WEBKIT_FORCE_SANDBOX=1 is set".
    pub why: String,
    /// On because `WEBKIT_FORCE_SANDBOX=1` is set (the player's environment).
    pub forced: bool,
}

/// What WebKitGTK `version` (the GTK 3 API) does with its sandbox, given [`FORCE_VAR`] and [`VAR`] as this start has them.
/// WebKitGTK's own rules (WebProcessPoolGLib.cpp since 2.46, ProcessLauncherGLib.cpp before).
pub fn webkit_use(version: (u32, u32, u32), force: Option<&str>, disable: Option<&str>) -> Use {
    let v = (version.0, version.1);
    let disabled = match disable {
        Some(d) if v >= (2, 46) => d != "0",
        Some(d) if v >= (2, 40) => d == "1",
        _ => false,
    };
    if disabled {
        return Use { webkit: Webkit::Off, why: format!("{VAR}={} is set", disable.unwrap_or_default()), forced: false };
    }
    match force {
        Some("1") => Use { webkit: Webkit::On, why: format!("{FORCE_VAR}=1 is set"), forced: true },
        Some(f) if v < (2, 40) => Use { webkit: Webkit::Off, why: format!("{FORCE_VAR}={f} is set (WebKitGTK before 2.40)"), forced: false },
        _ => Use {
            webkit: Webkit::Unused,
            why: format!("WebKitGTK's GTK 3 API, which the launcher uses, runs it only when {FORCE_VAR}=1 is set"),
            forced: false,
        },
    }
}

// ---------------------------------------------------------------- the probe

/// What running bwrap the way WebKit does said.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Probe {
    Works,
    /// It ran and failed: its own line ("bwrap: setting up uid map: Permission denied").
    Fails { line: String },
    /// The bwrap WebKit runs is not there.
    Missing { path: String },
    /// Could not tell (why).
    Unknown { why: String },
}

impl Probe {
    pub fn kind(&self) -> &'static str {
        match self {
            Probe::Works => "works",
            Probe::Fails { .. } => "fails",
            Probe::Missing { .. } => "missing",
            Probe::Unknown { .. } => "unknown",
        }
    }
    /// It cannot start: WebKit's sandbox would end the launcher.
    pub fn failed(&self) -> bool {
        matches!(self, Probe::Fails { .. } | Probe::Missing { .. })
    }
    /// "bwrap works", "bwrap fails: bwrap: setting up uid map: Permission denied", ...
    pub fn says(&self) -> String {
        match self {
            Probe::Works => "bwrap works".into(),
            Probe::Fails { line } => format!("bwrap fails: {line}"),
            Probe::Missing { path } => format!("bwrap is missing: {path} is not there"),
            Probe::Unknown { why } => format!("could not tell whether bwrap works: {why}"),
        }
    }
}

/// The arguments of the probe: the namespaces WebKit's web process gets (BubblewrapLauncher.cpp: uts, ipc, pid, net;
/// bwrap adds the user namespace itself when it is not setuid), /etc, /dev, /proc, a /tmp, the system's programs and
/// libraries read only, and a program that does nothing.
pub fn probe_args(no_op: &str) -> Vec<String> {
    let mut a: Vec<String> = [
        "--unshare-uts", "--unshare-ipc", "--unshare-pid", "--unshare-net", "--die-with-parent",
        "--ro-bind", "/etc", "/etc", "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp",
        "--ro-bind-try", "/usr", "/usr", "--ro-bind-try", "/lib", "/lib", "--ro-bind-try", "/lib64", "/lib64",
        "--ro-bind-try", "/bin", "/bin", "--",
    ].iter().map(|s| s.to_string()).collect();
    a.push(no_op.into());
    a
}

/// Runs `bwrap` as WebKit would, as a no-op, for at most `timeout` (then it is stopped by its own pid). It gets this
/// process's environment, as WebKit's does.
pub fn probe_with(bwrap: &Path, timeout: Duration) -> Probe {
    let no_op = ["/usr/bin/true", "/bin/true"].into_iter().find(|p| Path::new(p).is_file()).unwrap_or("true");
    let mut cmd = Command::new(bwrap);
    cmd.args(probe_args(no_op)).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::piped());
    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Probe::Missing { path: bwrap.display().to_string() },
        Err(e) => return Probe::Unknown { why: format!("could not run {}: {e}", bwrap.display()) },
    };
    let start = Instant::now();
    let status = loop {
        match child.try_wait() {
            Ok(Some(s)) => break s,
            Ok(None) if start.elapsed() < timeout => std::thread::sleep(Duration::from_millis(5)),
            Ok(None) => {
                let _ = child.kill();
                let _ = child.wait();
                return Probe::Unknown { why: format!("no answer within {} s", timeout.as_secs()) };
            }
            Err(e) => return Probe::Unknown { why: format!("waiting for bwrap: {e}") },
        }
    };
    if status.success() {
        return Probe::Works;
    }
    let mut err = String::new();
    if let Some(mut e) = child.stderr.take() {
        use std::io::Read;
        let _ = e.by_ref().take(8192).read_to_string(&mut err);
    }
    match status.code() {
        Some(code) => Probe::Fails { line: first_line(&err).unwrap_or_else(|| format!("exit status {code}")) },
        None => Probe::Unknown { why: format!("bwrap ended without an exit status ({status})") },
    }
}

/// bwrap's own line ("bwrap: ..."), else the first line that says anything.
pub fn first_line(stderr: &str) -> Option<String> {
    let lines = || stderr.lines().map(str::trim).filter(|l| !l.is_empty());
    lines().find(|l| l.starts_with("bwrap:")).or_else(|| lines().next()).map(|l| l.chars().take(300).collect())
}

/// The bwrap path in a libwebkit2gtk's read-only data: the string that ends in "/bwrap" (WebKit's `BWRAP_EXECUTABLE`;
/// one ending in "/bin/bwrap" first).
pub fn find_bwrap(rodata: &[u8]) -> Option<String> {
    let found: Vec<&str> = rodata.split(|b| *b == 0)
        .filter(|s| s.ends_with(b"/bwrap") && (s.starts_with(b"/") || s.starts_with(b".")) && s.iter().all(|c| c.is_ascii_graphic()))
        .filter_map(|s| std::str::from_utf8(s).ok()).collect();
    found.iter().find(|s| s.ends_with("/bin/bwrap")).or(found.first()).map(|s| s.to_string())
}

/// A 64-bit little-endian ELF file's `.rodata` (at most 64 MiB).
pub fn elf_rodata(path: &Path) -> Result<Vec<u8>, String> {
    use std::io::{Read, Seek, SeekFrom};
    let mut f = std::fs::File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut read_at = |off: u64, len: usize| -> Result<Vec<u8>, String> {
        let mut b = vec![0u8; len];
        f.seek(SeekFrom::Start(off)).and_then(|_| f.read_exact(&mut b)).map_err(|e| format!("{}: {e}", path.display()))?;
        Ok(b)
    };
    let h = read_at(0, 64)?;
    if &h[..4] != b"\x7fELF" || h[4] != 2 || h[5] != 1 {
        return Err(format!("{}: not a 64-bit little-endian ELF file", path.display()));
    }
    let u16_at = |b: &[u8], o: usize| u16::from_le_bytes([b[o], b[o + 1]]) as usize;
    let u32_at = |b: &[u8], o: usize| u32::from_le_bytes(b[o..o + 4].try_into().unwrap()) as usize;
    let u64_at = |b: &[u8], o: usize| u64::from_le_bytes(b[o..o + 8].try_into().unwrap());
    let (shoff, shentsize, shnum, shstrndx) = (u64_at(&h, 0x28), u16_at(&h, 0x3A), u16_at(&h, 0x3C), u16_at(&h, 0x3E));
    if shentsize < 64 || shnum == 0 || shnum > 1 << 16 || shstrndx >= shnum {
        return Err(format!("{}: no section table", path.display()));
    }
    let table = read_at(shoff, shentsize * shnum)?;
    let sect = |i: usize| (u32_at(&table, i * shentsize), u64_at(&table, i * shentsize + 0x18), u64_at(&table, i * shentsize + 0x20));
    let (_, str_off, str_size) = sect(shstrndx);
    let names = read_at(str_off, (str_size as usize).min(1 << 20))?;
    for i in 0..shnum {
        let (name, off, size) = sect(i);
        let n = names.get(name..).and_then(|r| r.split(|b| *b == 0).next()).unwrap_or_default();
        if n == b".rodata" {
            if size > 64 << 20 {
                return Err(format!("{}: .rodata is {size} bytes", path.display()));
            }
            return read_at(off, size as usize);
        }
    }
    Err(format!("{}: no .rodata", path.display()))
}

/// The libwebkit2gtk this process loaded (`/proc/self/maps`).
#[cfg(target_os = "linux")]
fn loaded_webkit() -> Option<PathBuf> {
    let maps = std::fs::read_to_string("/proc/self/maps").ok()?;
    maps.lines().filter_map(|l| l.split_whitespace().nth(5)).find(|p| {
        Path::new(p).file_name().is_some_and(|n| n.to_string_lossy().starts_with("libwebkit2gtk-4.1.so"))
    }).map(PathBuf::from)
}

/// The bwrap WebKit runs here: the path built into the loaded libwebkit2gtk, against the working directory when it is
/// relative, as the system starts it (the AppImage's: `$APPDIR/usr/././/bin/bwrap`).
#[cfg(target_os = "linux")]
pub fn bwrap_path() -> Result<(PathBuf, String), String> {
    let lib = loaded_webkit().ok_or_else(|| "no libwebkit2gtk-4.1 in this process".to_string())?;
    let built = find_bwrap(&elf_rodata(&lib)?).ok_or_else(|| format!("no bwrap path in {}", lib.display()))?;
    let p = Path::new(&built);
    let full = if p.is_absolute() { p.to_path_buf() } else { std::env::current_dir().map_err(|e| format!("the working directory: {e}"))?.join(p) };
    Ok((full, built))
}

/// `/proc/sys/kernel/apparmor_restrict_unprivileged_userns`, when the kernel has it.
pub fn apparmor_restrict() -> Option<String> {
    std::fs::read_to_string("/proc/sys/kernel/apparmor_restrict_unprivileged_userns").ok().map(|s| s.trim().to_string())
}

// ---------------------------------------------------------------- the marker

/// `<data>/state/webkit-sandbox.json`: there, a start without an override turns WebKit's sandbox off.
pub fn marker(d: &DataDirs) -> PathBuf {
    d.root.join("state").join("webkit-sandbox.json")
}

pub fn write_marker(d: &DataDirs, why: &str) -> Result<(), Error> {
    crate::render::write_marker_file(&marker(d), why)
}

pub fn clear_marker(d: &DataDirs) -> Result<(), Error> {
    crate::render::clear_marker_file(&marker(d))
}

fn marker_why(d: &DataDirs) -> Option<String> {
    crate::render::marker_file_why(&marker(d))
}

/// The marker's sentence for a message's restart (`probe` or `late`).
pub fn restart_why(reason: &str) -> &'static str {
    match reason {
        "probe" => "Turned off by the player in the launcher's message at the start: WebKit's sandbox could not start",
        _ => "Turned off by the player in the launcher's message after the page did not start",
    }
}

// ---------------------------------------------------------------- this start

/// What this start knows about WebKit's sandbox (Linux, after [`prepare`]).
#[derive(Debug, Clone)]
pub struct State {
    pub choice: Choice,
    pub var: Var,
    pub webkit_version: (u32, u32, u32),
    pub using: Use,
    pub probe: Probe,
    /// The bwrap WebKit runs, as built into it ("/usr/bin/bwrap", "././/bin/bwrap").
    pub bwrap: Option<String>,
    pub apparmor: Option<String>,
    /// Running as an AppImage.
    pub appimage: bool,
    /// The message's restart reason this start came from.
    pub restarted: Option<String>,
}

impl State {
    /// "WebKit's sandbox: not used (...); bwrap works", for the facts lines and the support report.
    pub fn says(&self) -> String {
        let w = match self.using.webkit {
            Webkit::On => "on",
            Webkit::Off => "off",
            Webkit::Unused => "not used",
        };
        let mut s = format!("{w} ({}); {}", self.using.why, self.probe_says());
        if let Some(a) = &self.apparmor {
            s += &format!("; apparmor_restrict_unprivileged_userns={a}");
        }
        s
    }
    /// The AppImage's WebKit has no bwrap (its path points into the AppImage): how every AppImage is, not the system's.
    pub fn appimage_without_bwrap(&self) -> bool {
        self.appimage && matches!(self.probe, Probe::Missing { .. })
    }
    /// The probe's sentence, the AppImage's own case in its words.
    pub fn probe_says(&self) -> String {
        match &self.probe {
            Probe::Missing { path } if self.appimage => format!(
                "this AppImage's WebKit cannot run its sandbox: it looks for bwrap inside the AppImage ({path}), where there is none"),
            p => p.says(),
        }
    }
    /// bwrap cannot start here, and it says something about this start or this system: the AppImage's own missing bwrap
    /// (so on every system) only when this start runs the sandbox.
    pub fn probe_is_news(&self) -> bool {
        self.probe.failed() && !(self.appimage_without_bwrap() && self.using.webkit != Webkit::On)
    }
    /// The player can still turn it off for this start's restart: WebKit runs it, and no variable of theirs decides.
    pub fn can_turn_off(&self) -> bool {
        self.using.webkit == Webkit::On && !matches!(self.var, Var::Yours(_))
    }
}

static STATE: OnceLock<State> = OnceLock::new();

pub fn state() -> Option<&'static State> {
    STATE.get()
}

/// A restart for another reason keeps a message's "off" whose marker may not be written yet.
pub fn keep(r: &mut crate::render::Relaunch) {
    if let Some(reason) = state().and_then(|s| s.restarted.clone()) {
        r.add(RESTARTED, &reason);
    }
}

/// A message's "off" button: the restart leaves the sandbox arguments and `KILN_LAUNCHER_WEBKIT_SANDBOX` out (the click
/// is newer) and says why (`probe` or `late`).
pub fn relaunch_off(reason: &str) -> crate::render::Relaunch {
    let r = crate::render::Relaunch { drop_args: vec![ARG_OFF, ARG_ON], unset: vec![ENV], set: vec![(RESTARTED, reason.to_string())] };
    r.keeping()
}

fn log(msg: &str) {
    crate::commands::app::launcher_log("sandbox", msg);
}

/// The WebKitGTK this process loaded.
#[cfg(target_os = "linux")]
pub fn webkit_version() -> (u32, u32, u32) {
    use webkit2gtk::ffi;
    // SAFETY: plain getters of the loaded library's version.
    unsafe { (ffi::webkit_get_major_version(), ffi::webkit_get_minor_version(), ffi::webkit_get_micro_version()) }
}

/// Linux, in `main` after crate::render's `prepare`, before any thread exists and before Tauri: the choice and the
/// variable, the probe, the log line, and the message when WebKit would run a sandbox that cannot start.
#[cfg(target_os = "linux")]
pub fn prepare(args: &[String]) {
    // a launcher started from another Kiln (a restart): the variable it set is not the player's
    let ours = std::env::var(SET_BY_US).is_ok_and(|l| l.split(',').any(|x| x.trim() == VAR));
    let restarted = std::env::var(RESTARTED).ok().filter(|r| !r.is_empty());
    // SAFETY: main, before any thread exists (the caller's contract).
    unsafe {
        if ours {
            std::env::remove_var(VAR);
        }
        std::env::remove_var(SET_BY_US);
        std::env::remove_var(RESTARTED);
    }
    let dirs = crate::folder::found_dirs();
    let has_marker = dirs.as_ref().is_some_and(|d| marker(d).is_file());
    let c = decide(args, std::env::var(ENV).ok().as_deref(), restarted.is_some(), has_marker);
    let var = variable(&c, std::env::var(VAR).ok().as_deref());
    if var == Var::Set {
        // SAFETY: as above.
        unsafe {
            std::env::set_var(VAR, "1");
            std::env::set_var(SET_BY_US, VAR);
        }
    }
    let version = webkit_version();
    let using = webkit_use(version, std::env::var(FORCE_VAR).ok().as_deref(), std::env::var(VAR).ok().as_deref());
    let t = Instant::now();
    let (bwrap, mut probe) = match bwrap_path() {
        Ok((full, built)) => (Some(built), probe_with(&full, PROBE_TIMEOUT)),
        Err(why) => (None, Probe::Unknown { why }),
    };
    let took = t.elapsed();
    #[cfg(any(debug_assertions, feature = "dev-eval"))]
    if let Ok(v) = std::env::var(TEST_PROBE) {
        probe = match v.as_str() {
            "works" => Probe::Works,
            "fails" => Probe::Fails { line: "bwrap: setting up uid map: Permission denied (test)".into() },
            "missing" => Probe::Missing { path: "/nonexistent/bwrap (test)".into() },
            _ => Probe::Unknown { why: "test".into() },
        };
        log(&format!("test: the probe's result is replaced ({TEST_PROBE}={v})"));
    }
    let s = State {
        choice: c,
        var,
        webkit_version: version,
        using,
        probe,
        bwrap,
        apparmor: apparmor_restrict(),
        appimage: std::env::var_os("APPIMAGE").is_some_and(|a| !a.is_empty()),
        restarted,
    };
    log(&start_line(&s, took));
    let _ = STATE.set(s.clone());
    if s.using.webkit == Webkit::On && s.probe.failed() {
        crate::dialog::at_start(&s);
    }
}

#[cfg(not(target_os = "linux"))]
pub fn prepare(_args: &[String]) {}

/// The start's log line: the choice and why, the variable, what WebKit does, the probe.
pub fn start_line(s: &State, took: Duration) -> String {
    let why = match s.choice.why {
        Why::Default => "nothing asked".to_string(),
        Why::Marker => "the marker of an earlier start".to_string(),
        Why::Restart => format!("restarted from the launcher's message ({})", s.restarted.as_deref().unwrap_or("?")),
        Why::Environment => format!("{ENV} is set"),
        Why::Argument => format!("{} was given", if s.choice.want == Want::Off { ARG_OFF } else { ARG_ON }),
    };
    let var = match &s.var {
        Var::Untouched => format!("{VAR} not set"),
        Var::Set => format!("{VAR}=1"),
        Var::Yours(v) => format!("{VAR}={v} (yours)"),
    };
    let (a, b, c) = s.webkit_version;
    let mut line = format!(
        "the player's choice: {} ({why}); {var}; WebKitGTK {a}.{b}.{c} {}; probe of {} ({} ms): {}",
        if s.choice.want == Want::Off { "off" } else { "on" },
        match s.using.webkit {
            Webkit::On => format!("runs its sandbox ({})", s.using.why),
            Webkit::Off => format!("has its sandbox off ({})", s.using.why),
            Webkit::Unused => format!("does not run its sandbox ({})", s.using.why),
        },
        s.bwrap.as_deref().unwrap_or("bwrap"),
        took.as_millis(),
        s.probe_says(),
    );
    line += &format!("; apparmor_restrict_unprivileged_userns: {}", s.apparmor.as_deref().unwrap_or("(not in this kernel)"));
    if let Some(i) = &s.choice.ignored {
        line += &format!("; {i}");
    }
    line
}

/// After the beta copy's gate opened (lib.rs `after_snapshot`): a message's "off" whose marker could not be written
/// before (the gate) is written now, so later starts keep it.
pub fn after_gate() {
    let Some(s) = state() else { return };
    let (Some(reason), Why::Restart) = (s.restarted.as_deref(), s.choice.why) else { return };
    let Ok(d) = crate::state::dirs() else { return };
    if marker(&d).is_file() {
        return;
    }
    match write_marker(&d, restart_why(reason)) {
        Ok(()) => log(&format!("wrote {} (the player's choice in the message)", marker(&d).display())),
        Err(e) => log(&format!("could not write {}: {}", marker(&d).display(), e.message)),
    }
}

// ---------------------------------------------------------------- what the UI and the report see

/// `app_info.sandbox` (Linux only).
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SandboxInfo {
    /// This start's choice and why.
    pub want: Want,
    pub why: Why,
    /// The marker is there: a start without an override turns the sandbox off.
    pub marker: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub marker_why: Option<String>,
    /// What WebKit does in this start, and why in words.
    pub webkit: Webkit,
    pub webkit_why: String,
    /// The player's own `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS` value (it decides; the switch does not).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub yours: Option<String>,
    /// "works", "fails", "missing" or "unknown", and the probe's sentence.
    pub probe: String,
    pub probe_says: String,
    /// bwrap cannot start here and that says something about this system (not the AppImage's own missing bwrap while
    /// the start does not run the sandbox): Settings shows the probe's sentence.
    #[serde(default)]
    pub probe_failed: bool,
}

pub fn info() -> Option<SandboxInfo> {
    let s = state()?;
    let d = crate::state::dirs().ok();
    Some(SandboxInfo {
        want: s.choice.want,
        why: s.choice.why,
        marker: d.as_ref().is_some_and(|d| marker(d).is_file()),
        marker_why: d.as_ref().and_then(marker_why),
        webkit: s.using.webkit,
        webkit_why: s.using.why.clone(),
        yours: match &s.var { Var::Yours(v) => Some(v.clone()), _ => None },
        probe: s.probe.kind().into(),
        probe_says: s.probe_says(),
        probe_failed: s.probe_is_news(),
    })
}

/// Settings, "WebKit's sandbox": the marker for the next start (on: removed; off: written).
pub fn set_next(on: bool) -> Result<SandboxInfo, Error> {
    if state().is_none() {
        return Err(Error::new(kiln_manage::ErrorCode::Unsupported, "Only the Linux launcher has this."));
    }
    let d = crate::state::dirs()?;
    if on {
        clear_marker(&d)?;
    } else {
        write_marker(&d, "Turned off in Settings")?;
    }
    log(&format!("Settings: the next start {}", if on { "leaves WebKit's sandbox on" } else { "turns WebKit's sandbox off" }));
    info().ok_or_else(|| Error::new(kiln_manage::ErrorCode::Internal, "no sandbox state"))
}

/// The support report's finding (`webkit-sandbox`) when the probe found that bwrap cannot start: a problem when WebKit
/// runs its sandbox in this start, a note when it does not (then it is not why a window stays empty).
pub fn finding(s: &State) -> Option<kiln_manage::report::Finding> {
    use kiln_manage::report::{Finding, Level};
    // the AppImage's missing bwrap is how every AppImage is: news only when this start runs the sandbox
    if !s.probe_is_news() {
        return None;
    }
    let aa = s.apparmor.as_deref().filter(|a| *a == "1").map(|_| " AppArmor restricts user namespaces here (kernel.apparmor_restrict_unprivileged_userns = 1).").unwrap_or_default();
    let (level, text) = if s.using.webkit == Webkit::On {
        (Level::Problem, format!("WebKit's sandbox cannot start on this system ({}), and this start uses it ({}): the launcher's page cannot open.{aa} \
            Turn it off in the launcher's message or with {ARG_OFF}; app.md, \"WebKit's sandbox (Linux)\", has the proper fix.", s.probe_says(), s.using.why))
    } else {
        (Level::Info, format!("bwrap cannot set up WebKit's sandbox on this system ({}).{aa} This start does not use the sandbox ({}), so this is not why a launcher window stays empty.",
            s.probe_says(), s.using.why))
    };
    Some(Finding { id: "webkit-sandbox".into(), level, text })
}

// ---------------------------------------------------------------- the messages' words

/// What a message's button does.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Action {
    /// Write the marker and restart without WebKit's sandbox.
    SandboxOff,
    /// Go on as if nothing was said.
    StartAnyway,
    /// Write the render marker and restart without the GPU.
    NoGpu,
    /// Open the folder of launcher.log (the message stays).
    OpenLogs,
    Quit,
}

/// A message: its title, its text, its buttons (the first is the default).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Message {
    pub title: String,
    pub body: String,
    pub buttons: Vec<(Action, &'static str)>,
}

/// Where the proper fix is written down.
pub const FIX_URL: &str = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/launcher/app.md#webkits-sandbox-linux";

/// What turning the sandbox off means (the launcher's page and what it loads: tauri.conf.json's CSP).
pub const OFF_MEANS: &str = "Turning the sandbox off: the launcher's page then runs without WebKit's process sandbox. It shows \
only the launcher's own pages and, from the internet, only images from Nexus Mods. The launcher keeps this choice; \
Settings > Appearance > \"WebKit's sandbox\" turns the sandbox back on.";

/// The proper fix for what the probe found.
pub fn proper_fix(s: &State) -> String {
    let unforce = if s.using.forced {
        format!("\nOr start the launcher without {FORCE_VAR}=1: WebKitGTK then does not run its sandbox for the launcher at all.")
    } else {
        String::new()
    };
    match &s.probe {
        Probe::Missing { .. } if s.appimage => format!(
            "The proper fix: start the launcher without {FORCE_VAR}=1. This AppImage's WebKit looks for bwrap inside the AppImage, \
            where there is none, so it cannot run its sandbox on any system; Kiln's .deb uses the system's WebKit and bwrap."),
        Probe::Missing { path } => format!(
            "The proper fix: install bubblewrap (your system's package of that name has {path}).{unforce}"),
        _ if s.apparmor.as_deref() == Some("1") => format!(
            "The proper fix, on Ubuntu 24.04 and systems built on it: an AppArmor profile that lets the launcher use user namespaces, \
            like the ones Ubuntu ships for its own WebKitGTK apps (GNOME Web, Geary). The steps:\n{FIX_URL}{unforce}"),
        _ => format!(
            "The proper fix: let programs set up user namespaces for bwrap (its line above says what failed). The steps for common \
            systems:\n{FIX_URL}{unforce}"),
    }
}

/// The message at the start (before the window): WebKit would run its sandbox and bwrap cannot start it.
pub fn start_message(s: &State) -> Message {
    let what = match &s.probe {
        Probe::Missing { path } => format!(
            "WebKit's sandbox needs bwrap, which WebKit looks for at {path}, and it is not there. So the launcher's page would not open: \
            WebKit would end the launcher."),
        p => {
            let mut w = format!("This system does not let WebKit's sandbox start, so the launcher's page would not open: WebKit would end \
                the launcher.\nbwrap says: {}", match p { Probe::Fails { line } => line.clone(), other => other.says() });
            if s.apparmor.as_deref() == Some("1") {
                w += "\nAppArmor restricts user namespaces here (kernel.apparmor_restrict_unprivileged_userns = 1).";
            }
            w
        }
    };
    Message {
        title: "Kiln Launcher: WebKit's sandbox cannot start".into(),
        body: format!("{what}\nWebKit runs its sandbox in this start because {}.\n\n{OFF_MEANS}\n\n{}", s.using.why, proper_fix(s)),
        buttons: vec![(Action::SandboxOff, "Turn the sandbox off and start"), (Action::StartAnyway, "Start anyway"), (Action::Quit, "Quit")],
    }
}

/// What the launcher tried, from its render choice: "drawing with the GPU, then without it (it started itself again)".
pub fn tried(c: &crate::render::Choice) -> String {
    use crate::render::{Mode, Why as R};
    match (c.mode, c.why) {
        (Mode::Software, R::Restart) => "drawing with the GPU, then without it (it started itself again)".into(),
        (Mode::Software, R::Marker) => "drawing without the GPU (the setting from an earlier start)".into(),
        (Mode::Software, R::Argument) => format!("drawing without the GPU ({} was given)", crate::render::ARG_SOFTWARE),
        (Mode::Software, _) => format!("drawing without the GPU ({}=software)", crate::render::ENV),
        (Mode::Gpu, R::Argument) => format!("drawing with the GPU only ({} was given)", crate::render::ARG_GPU),
        (Mode::Gpu, R::Environment) => format!("drawing with the GPU only ({}=gpu)", crate::render::ENV),
        (Mode::Gpu, _) => "drawing with the GPU; starting again without it did not work".into(),
    }
}

/// The message when the page did not start and the launcher has nothing left to try by itself. `what`: the watchdog's
/// words ("the page did not start within 15 s of the window"); `log`: launcher.log's path.
pub fn late_message(s: Option<&State>, render: &crate::render::Choice, what: &str, log: &str) -> Message {
    let sandbox = match s {
        None => "unknown".to_string(),
        Some(s) => {
            let mut w = match (s.using.webkit, &s.var) {
                (Webkit::Unused, _) => format!("not in use in this start ({}), so it is not the cause", s.using.why),
                (Webkit::Off, Var::Yours(_)) => format!("off ({}, in your environment)", s.using.why),
                (Webkit::Off, _) => format!("off ({})", match s.choice.why {
                    Why::Argument => format!("{ARG_OFF} was given"),
                    Why::Environment => format!("{ENV}=off"),
                    Why::Restart => "your choice in the message before this start".into(),
                    _ => "your choice from an earlier start".into(),
                }),
                (Webkit::On, _) => format!("on ({})", s.using.why),
            };
            if s.using.webkit == Webkit::On || s.probe_is_news() {
                w += &format!("; {}", s.probe_says());
            }
            w
        }
    };
    let mut buttons = Vec::new();
    if s.is_some_and(State::can_turn_off) {
        buttons.push((Action::SandboxOff, "Start without WebKit's sandbox"));
    }
    if render.mode == crate::render::Mode::Gpu {
        buttons.push((Action::NoGpu, "Start without the GPU"));
    }
    buttons.push((Action::OpenLogs, "Open the log folder"));
    buttons.push((Action::Quit, "Quit"));
    let tries = if buttons.len() > 2 {
        "The cause is not known, so each button below is a try."
    } else {
        "The cause is not known, and the launcher has nothing left to try."
    };
    let head = match what.strip_prefix("the page ") {
        Some(rest) => format!("The launcher's page {rest}, so the window stays empty."),
        None => format!("The launcher's window did not come up ({what})."),
    };
    Message {
        title: "Kiln Launcher: the window did not start".into(),
        body: format!(
            "{head}\n\nTried: {}.\nWebKit's sandbox: {sandbox}.\n\n{tries} launcher.log has the details:\n{log}\nPlease send it when you \
            report this.",
            tried(render)),
        buttons,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(tail: &[&str]) -> Vec<String> {
        std::iter::once("kiln-launcher").chain(tail.iter().copied()).map(str::to_string).collect()
    }
    fn c(want: Want, why: Why) -> Choice {
        Choice { want, why, ignored: None }
    }
    fn state(using: Use, probe: Probe) -> State {
        State {
            choice: c(Want::On, Why::Default),
            var: Var::Untouched,
            webkit_version: (2, 50, 4),
            using,
            probe,
            bwrap: Some("/usr/bin/bwrap".into()),
            apparmor: Some("1".into()),
            appimage: false,
            restarted: None,
        }
    }
    fn forced() -> Use {
        webkit_use((2, 50, 4), Some("1"), None)
    }
    fn uid_map() -> Probe {
        Probe::Fails { line: "bwrap: setting up uid map: Permission denied".into() }
    }

    /// Without a click, a flag, the environment setting or the marker, the launcher never sets the variable.
    #[test]
    fn nothing_asked_sets_nothing() {
        let first = decide(&args(&[]), None, false, false);
        assert_eq!(first, c(Want::On, Why::Default));
        assert_eq!(variable(&first, None), Var::Untouched);
        // other arguments, an empty value, the render flags: nothing either
        for a in [&["--deck", "--software-render", "--no-webkit"][..], &["--gpu-render"]] {
            let x = decide(&args(a), Some(""), false, false);
            assert_eq!(variable(&x, None), Var::Untouched, "{a:?}");
        }
        // an unknown value is said and ignored, and sets nothing
        let odd = decide(&args(&[]), Some("disabled"), false, false);
        assert_eq!(variable(&odd, None), Var::Untouched);
        assert!(odd.ignored.as_deref().is_some_and(|i| i.contains("KILN_LAUNCHER_WEBKIT_SANDBOX=disabled")), "{odd:?}");
        // even when the probe failed and WebKit would run its sandbox: the decision does not look at them
        assert_eq!(decide(&args(&[]), None, false, false).want, Want::On);
    }

    #[test]
    fn the_arguments_win_then_the_environment_then_a_restart_then_the_marker() {
        // the marker alone
        assert_eq!(decide(&args(&[]), None, false, true), c(Want::Off, Why::Marker));
        // a message's restart (its marker may not be written yet)
        assert_eq!(decide(&args(&[]), None, true, false), c(Want::Off, Why::Restart));
        // the environment over a restart and the marker, both ways
        assert_eq!(decide(&args(&[]), Some("on"), true, true), c(Want::On, Why::Environment));
        assert_eq!(decide(&args(&[]), Some(" OFF "), false, false), c(Want::Off, Why::Environment));
        // the arguments over everything; the last one given wins
        assert_eq!(decide(&args(&[ARG_ON]), Some("off"), true, true), c(Want::On, Why::Argument));
        assert_eq!(decide(&args(&[ARG_OFF]), Some("on"), false, false), c(Want::Off, Why::Argument));
        assert_eq!(decide(&args(&[ARG_OFF, "--deck", ARG_ON]), None, false, true), c(Want::On, Why::Argument));
        assert_eq!(decide(&args(&[ARG_ON, ARG_OFF]), None, false, false), c(Want::Off, Why::Argument));
    }

    #[test]
    fn the_players_own_variable_stays_whatever_the_choice() {
        let off = decide(&args(&[ARG_OFF]), None, false, false);
        assert_eq!(variable(&off, None), Var::Set);
        assert_eq!(variable(&off, Some("0")), Var::Yours("0".into()), "theirs stays, even against the flag");
        let on = decide(&args(&[]), None, false, false);
        assert_eq!(variable(&on, Some("1")), Var::Yours("1".into()));
        // the log says so
        let mut s = state(webkit_use((2, 50, 4), None, Some("1")), Probe::Works);
        s.var = Var::Yours("1".into());
        assert!(start_line(&s, Duration::from_millis(3)).contains("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 (yours)"));
        assert!(!s.can_turn_off());
    }

    /// WebKitGTK's own rules for the GTK 3 API (its source, 2.38 to 2.54).
    #[test]
    fn what_webkit_does_with_its_sandbox_by_version() {
        // a default start: not run at all
        for v in [(2, 38, 6), (2, 44, 3), (2, 50, 4), (2, 54, 1)] {
            assert_eq!(webkit_use(v, None, None).webkit, Webkit::Unused, "{v:?}");
        }
        // WEBKIT_FORCE_SANDBOX=1 runs it
        assert_eq!(webkit_use((2, 50, 4), Some("1"), None), Use { webkit: Webkit::On, why: "WEBKIT_FORCE_SANDBOX=1 is set".into(), forced: true });
        // the switch: since 2.46 any value but 0, 2.40 to 2.44 only 1, and it wins over WEBKIT_FORCE_SANDBOX=1
        assert_eq!(webkit_use((2, 50, 4), Some("1"), Some("1")).webkit, Webkit::Off);
        assert_eq!(webkit_use((2, 50, 4), Some("1"), Some("yes")).webkit, Webkit::Off);
        assert_eq!(webkit_use((2, 50, 4), Some("1"), Some("0")).webkit, Webkit::On);
        assert_eq!(webkit_use((2, 44, 3), Some("1"), Some("yes")).webkit, Webkit::On, "2.44 reads only 1");
        assert_eq!(webkit_use((2, 42, 0), Some("1"), Some("1")).webkit, Webkit::Off);
        // before 2.40 there is no switch; WEBKIT_FORCE_SANDBOX=0 turned it off there
        assert_eq!(webkit_use((2, 38, 6), Some("1"), Some("1")).webkit, Webkit::On);
        assert_eq!(webkit_use((2, 38, 6), Some("0"), None).webkit, Webkit::Off);
        assert_eq!(webkit_use((2, 50, 4), Some("0"), None).webkit, Webkit::Unused, "since 2.40 FORCE=0 only warns");
    }

    #[test]
    fn the_bwrap_path_is_read_from_webkits_own_strings() {
        let rodata = b"\0bwrapinfo.json\0Failed to fully launch dbus-proxy: %s\0/usr/bin/xdg-dbus-proxy\0/usr/bin/bwrap\0--seccomp\0";
        assert_eq!(find_bwrap(rodata).as_deref(), Some("/usr/bin/bwrap"));
        // the AppImage's, after Tauri's bundler replaced "/usr" with "././"
        assert_eq!(find_bwrap(b"x\0././/bin/xdg-dbus-proxy\0././/bin/bwrap\0").as_deref(), Some("././/bin/bwrap"));
        // a Nix store path, at the very start
        assert_eq!(find_bwrap(b"/nix/store/abc-bubblewrap-0.11/bin/bwrap\0").as_deref(), Some("/nix/store/abc-bubblewrap-0.11/bin/bwrap"));
        assert_eq!(find_bwrap(b"bwrap\0no path here\0"), None);
    }

    /// The real library, where this machine has one (the system's WebKitGTK; `KILN_TEST_WEBKIT_LIB` names another, such
    /// as an AppImage's extracted libwebkit2gtk-4.1.so.0).
    #[test]
    fn the_bwrap_path_of_an_installed_webkitgtk() {
        let lib = std::env::var_os("KILN_TEST_WEBKIT_LIB").filter(|v| !v.is_empty()).map(PathBuf::from)
            .or_else(|| ["/usr/lib/libwebkit2gtk-4.1.so.0", "/usr/lib/x86_64-linux-gnu/libwebkit2gtk-4.1.so.0", "/usr/lib64/libwebkit2gtk-4.1.so.0"]
                .into_iter().map(PathBuf::from).find(|p| p.is_file()));
        let Some(lib) = lib else { return };
        let path = find_bwrap(&elf_rodata(&lib).unwrap());
        eprintln!("{}: {path:?}", lib.display());
        assert!(path.is_some_and(|p| p.ends_with("/bin/bwrap")));
        assert!(elf_rodata(Path::new("/nonexistent/libwebkit2gtk-4.1.so.0")).is_err());
    }

    #[test]
    fn the_probe_tells_works_fails_missing_and_unknown_apart() {
        assert_eq!(probe_with(Path::new("/nonexistent/bin/bwrap"), PROBE_TIMEOUT), Probe::Missing { path: "/nonexistent/bin/bwrap".into() });
        let t = std::env::temp_dir().join(format!("kl-sandbox-probe-{}", std::process::id()));
        std::fs::create_dir_all(&t).unwrap();
        let script = |name: &str, body: &str| {
            let p = t.join(name);
            std::fs::write(&p, format!("#!/bin/sh\n{body}\n")).unwrap();
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o755)).unwrap();
            }
            p
        };
        // a fake bwrap that checks it is asked like WebKit asks, then works
        let ok = script("ok", r#"case "$*" in *--unshare-pid*--proc\ /proc*--\ */true) exit 0;; esac; exit 9"#);
        assert_eq!(probe_with(&ok, PROBE_TIMEOUT), Probe::Works);
        let uid = script("uid", "echo 'some noise' >&2; echo 'bwrap: setting up uid map: Permission denied' >&2; exit 1");
        assert_eq!(probe_with(&uid, PROBE_TIMEOUT), uid_map());
        let silent = script("silent", "exit 3");
        assert_eq!(probe_with(&silent, PROBE_TIMEOUT), Probe::Fails { line: "exit status 3".into() });
        let hangs = script("hangs", "exec sleep 30");
        let t0 = Instant::now();
        assert!(matches!(probe_with(&hangs, Duration::from_millis(200)), Probe::Unknown { why } if why.contains("no answer")));
        assert!(t0.elapsed() < Duration::from_secs(5), "stopped at the timeout");
        let dir = script("not-a-program", "").with_file_name("");
        assert!(matches!(probe_with(&dir, PROBE_TIMEOUT), Probe::Unknown { .. }));
        let _ = std::fs::remove_dir_all(&t);
        assert_eq!(first_line("\n  \nbwrap: Creating new namespace failed: Permission denied\n").as_deref(), Some("bwrap: Creating new namespace failed: Permission denied"));
        assert_eq!(first_line("").as_deref(), None);
    }

    #[test]
    fn the_marker_is_kept_and_settings_turns_the_sandbox_back_on() {
        let t = std::env::temp_dir().join(format!("kl-sandbox-marker-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        let d = DataDirs::at(&t);
        let start = |restarted: bool| decide(&args(&["--deck"]), None, restarted, marker(&d).is_file());
        assert_eq!(start(false), c(Want::On, Why::Default));
        assert_eq!(marker(&d), t.join("state").join("webkit-sandbox.json"));
        write_marker(&d, restart_why("probe")).unwrap();
        assert_eq!(start(false), c(Want::Off, Why::Marker));
        assert_eq!(variable(&start(false), None), Var::Set);
        let v: serde_json::Value = serde_json::from_slice(&std::fs::read(marker(&d)).unwrap()).unwrap();
        assert_eq!(v["format"], 1);
        assert!(v["why"].as_str().unwrap().starts_with("Turned off by the player in the launcher's message at the start"));
        assert!(v["at"].is_string() && v["version"].is_string());
        assert!(marker_why(&d).is_some_and(|w| w.contains("WebKit's sandbox could not start")));
        // a flag on wins for one start; the marker stays
        assert_eq!(decide(&args(&[ARG_ON]), None, false, true).want, Want::On);
        clear_marker(&d).unwrap();
        clear_marker(&d).unwrap();
        assert_eq!(start(false), c(Want::On, Why::Default));
        let _ = std::fs::remove_dir_all(&t);
    }

    #[test]
    fn the_start_message_says_what_is_wrong_what_off_means_and_the_proper_fix() {
        let s = state(forced(), uid_map());
        let m = start_message(&s);
        assert_eq!(m.title, "Kiln Launcher: WebKit's sandbox cannot start");
        for want in [
            "This system does not let WebKit's sandbox start",
            "bwrap says: bwrap: setting up uid map: Permission denied",
            "kernel.apparmor_restrict_unprivileged_userns = 1",
            "because WEBKIT_FORCE_SANDBOX=1 is set",
            "runs without WebKit's process sandbox",
            "only the launcher's own pages and, from the internet, only images from Nexus Mods",
            "Settings > Appearance > \"WebKit's sandbox\" turns the sandbox back on",
            "an AppArmor profile that lets the launcher use user namespaces",
            FIX_URL,
            "Or start the launcher without WEBKIT_FORCE_SANDBOX=1",
        ] {
            assert!(m.body.contains(want), "{want}\n{}", m.body);
        }
        assert_eq!(m.buttons, vec![(Action::SandboxOff, "Turn the sandbox off and start"), (Action::StartAnyway, "Start anyway"), (Action::Quit, "Quit")]);
        // no AppArmor restriction: the general fix
        let mut s2 = state(forced(), Probe::Fails { line: "bwrap: No permissions to creating new namespace".into() });
        s2.apparmor = None;
        let m2 = start_message(&s2);
        assert!(!m2.body.contains("AppArmor") && m2.body.contains("let programs set up user namespaces"), "{}", m2.body);
        // the AppImage's bwrap is not there
        let mut s3 = state(forced(), Probe::Missing { path: "/tmp/.mount_Kiln/usr/././/bin/bwrap".into() });
        s3.appimage = true;
        let m3 = start_message(&s3);
        assert!(m3.body.contains("WebKit looks for at /tmp/.mount_Kiln/usr/././/bin/bwrap, and it is not there"), "{}", m3.body);
        assert!(m3.body.contains("cannot run its sandbox on any system") && m3.body.contains(".deb"), "{}", m3.body);
        // a .deb without bubblewrap
        let s4 = state(forced(), Probe::Missing { path: "/usr/bin/bwrap".into() });
        assert!(start_message(&s4).body.contains("install bubblewrap"));
    }

    #[test]
    fn the_late_message_offers_only_what_is_left_and_says_it_is_a_try() {
        use crate::render::{Choice as R, Mode, Why as RW};
        let soft = R { mode: Mode::Software, why: RW::Restart, ignored: None };
        let gpu = R { mode: Mode::Gpu, why: RW::Argument, ignored: None };
        let what = "the page did not start within 15 s of the window";
        let log = "/home/player/.local/share/kiln/logs/launcher.log";
        // the player's case: software mode tried, the sandbox not in use, bwrap fails on the system: the log and Quit
        let unused = state(webkit_use((2, 50, 4), None, None), uid_map());
        let m = late_message(Some(&unused), &soft, what, log);
        assert_eq!(m.title, "Kiln Launcher: the window did not start");
        assert_eq!(m.buttons, vec![(Action::OpenLogs, "Open the log folder"), (Action::Quit, "Quit")]);
        for want in ["The launcher's page did not start within 15 s of the window, so the window stays empty.",
            "Tried: drawing with the GPU, then without it (it started itself again).",
            "WebKit's sandbox: not in use in this start (", "so it is not the cause; bwrap fails: bwrap: setting up uid map",
            "the launcher has nothing left to try", log] {
            assert!(m.body.contains(want), "{want}\n{}", m.body);
        }
        // the sandbox on and the GPU asked for: both are offered, and the message says each is a try
        let on = state(forced(), Probe::Works);
        let m = late_message(Some(&on), &gpu, what, log);
        assert_eq!(m.buttons.iter().map(|b| b.0).collect::<Vec<_>>(), [Action::SandboxOff, Action::NoGpu, Action::OpenLogs, Action::Quit]);
        assert_eq!(m.buttons[0].1, "Start without WebKit's sandbox");
        assert_eq!(m.buttons[1].1, "Start without the GPU");
        assert!(m.body.contains("each button below is a try") && m.body.contains("on (WEBKIT_FORCE_SANDBOX=1 is set); bwrap works"), "{}", m.body);
        assert!(m.body.contains("Tried: drawing with the GPU only (--gpu-render was given)."), "{}", m.body);
        // off already (the player's choice): not offered again
        let mut off = state(webkit_use((2, 50, 4), Some("1"), Some("1")), Probe::Works);
        off.choice = c(Want::Off, Why::Marker);
        off.var = Var::Set;
        let m = late_message(Some(&off), &soft, what, log);
        assert!(!m.buttons.iter().any(|b| b.0 == Action::SandboxOff));
        assert!(m.body.contains("WebKit's sandbox: off (your choice from an earlier start)"), "{}", m.body);
        // the player's own variable keeps it on: not offered either
        let mut yours = state(forced(), Probe::Works);
        yours.var = Var::Yours("0".into());
        assert!(!late_message(Some(&yours), &soft, what, log).buttons.iter().any(|b| b.0 == Action::SandboxOff));
    }

    #[test]
    fn the_report_finding_is_a_problem_only_when_the_start_uses_the_sandbox() {
        use kiln_manage::report::Level;
        assert!(finding(&state(forced(), Probe::Works)).is_none());
        assert!(finding(&state(forced(), Probe::Unknown { why: "x".into() })).is_none());
        let p = finding(&state(forced(), uid_map())).unwrap();
        assert_eq!((p.id.as_str(), p.level), ("webkit-sandbox", Level::Problem));
        assert!(p.text.contains("setting up uid map") && p.text.contains("--no-webkit-sandbox"), "{}", p.text);
        let n = finding(&state(webkit_use((2, 50, 4), None, None), uid_map())).unwrap();
        assert_eq!(n.level, Level::Info);
        assert!(n.text.contains("not why a launcher window stays empty"), "{}", n.text);
        // the facts line
        assert!(state(webkit_use((2, 50, 4), None, None), Probe::Works).says().starts_with("not used (WebKitGTK's GTK 3 API"));
    }

    /// The AppImage's WebKit has no bwrap on any system: no finding, no line in the late message, no "on this system" in
    /// Settings while the start does not run the sandbox; said in the AppImage's words when it does.
    #[test]
    fn the_appimages_missing_bwrap_is_said_as_the_appimages() {
        use crate::render::{Choice as R, Mode, Why as RW};
        let missing = Probe::Missing { path: "/tmp/.mount_Kiln/usr/././/bin/bwrap".into() };
        let mut unused = state(webkit_use((2, 50, 4), None, None), missing.clone());
        unused.appimage = true;
        assert!(finding(&unused).is_none() && !unused.probe_is_news());
        let soft = R { mode: Mode::Software, why: RW::Argument, ignored: None };
        let m = late_message(Some(&unused), &soft, "the page did not start within 15 s of the window", "/x/launcher.log");
        assert!(!m.body.contains("bwrap"), "{}", m.body);
        assert!(unused.says().contains("this AppImage's WebKit cannot run its sandbox"), "{}", unused.says());
        let mut forced_ai = state(forced(), missing);
        forced_ai.appimage = true;
        let f = finding(&forced_ai).unwrap();
        assert_eq!(f.level, kiln_manage::report::Level::Problem);
        assert!(f.text.contains("this AppImage's WebKit cannot run its sandbox"), "{}", f.text);
    }

    #[test]
    fn a_restart_for_the_sandbox_replaces_its_flags_and_keeps_the_rest() {
        let r = relaunch_off("late");
        let os = |s: &str| std::ffi::OsString::from(s);
        let cmd = crate::render::restart_command(Path::new("/usr/bin/kiln-launcher"), false, &[os("--deck"), os(ARG_ON), os("--software-render")], Vec::new(), &r);
        assert_eq!(cmd.get_args().collect::<Vec<_>>(), ["--deck", "--software-render"]);
        let envs: Vec<_> = cmd.get_envs().map(|(k, v)| (k.to_os_string(), v.map(|v| v.to_os_string()))).collect();
        assert!(envs.contains(&(os(ENV), None)) && envs.contains(&(os(RESTARTED), Some(os("late")))), "{envs:?}");
        // the new start: off, from the restart
        assert_eq!(decide(&args(&["--deck", "--software-render"]), None, true, false), c(Want::Off, Why::Restart));
    }
}
