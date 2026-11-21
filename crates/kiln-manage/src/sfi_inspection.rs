// SPDX-License-Identifier: MIT
//! kiln-eml's read-only archive inspector (`kiln-eml-inspect`) as the launch-plan producer runs it.
//! It serves the `kiln` launch plan.
//!
//! The inspector is GPL and stays a separate process: kiln-manage (MIT) never links it, and reads only its neutral
//! JSON (roles, file names, sizes, digests, classification, origin token). What a plan takes from it:
//! `game.exe_sha256` = `target.executable.sha256`, `game.archiveBase_sha256` = `base.sha256`,
//! `game.archiveInput_sha256` = `input.sha256`, `game.archiveBaseOrigin` = `base.origin.token`, and the rest
//! (evidence, input class, receipt, inventory) for the plan's fingerprints ([`Facts`]).
//!
//! **Running it** ([`Tool`]): only the helper this Kiln ships, never an executable named by package data:
//! - the path is absolute and its file name is exactly `kiln-eml-inspect` (`.exe` on Windows): the patch-capable
//!   `kiln-eml-run` and any other program are refused, whatever they would print;
//! - its SHA-256 is the one the caller was shipped with, read through the handle that is still open when the process
//!   starts: on Linux the process is started through that very handle (`/proc/self/fd/<n>`), so a file renamed or
//!   linked over the path after the check never starts; on Windows that handle shares reading only, so nobody can
//!   write, rename or delete the file in between; elsewhere (macOS) it starts by its path right after the check;
//! - an explicit argv, no shell: `--game <absolute game folder> --side client|server` and nothing else (no
//!   `--no-patch`, no `--mods`, no `--export`: the inspector's own entry is disjoint from the patch runner);
//! - stdin closed; stdout at most [`MAX_STDOUT`] bytes, stderr at most [`MAX_STDERR`]; at most [`TIMEOUT`], then the
//!   child (and only it) is stopped and reaped, and its output is not waited for; a child that ends while a process it
//!   left behind keeps its output open gives no answer either; exit code 0 or no inspection.
//!
//! **Reading it** ([`facts`]): the closed document of `gpl/kiln-eml/kiln-eml/schema/archive-inspection.schema.json`,
//! `schema_version` 1, `inspector.component` `kiln-eml-inspect` with the version shipped, framing
//! `kiln-eml-inspect archive-set v1`, the side and stem asked for, an origin token with its exact label, and the
//! schema's own inventory roles and note codes (the plan fingerprints them). A
//! `refused` inspection (missing, invalid, link, unreadable, changed during the read, an unknown receipt, a baseline
//! missing, incomplete or corrupt) gives no facts, with its reasons: a plan without a proven base is never written, and
//! never with a made-up digest. The base's origin must fit its evidence (the current set is first-managed or
//! rebaselined; a seal or an unsealed pair is unrecorded; a receipt baseline keeps what the receipt carried), and no
//! token is ever "vanilla": integrity is not provenance.
//!
//! **Where it is** ([`installed`], [`Installed`]): this Kiln installs it into each game folder as part of kiln-eml's
//! component `eml` (GPL-3, never the MIT `sfi` component), in its own folder
//! `kiln/tools/eml-inspect/<platform>/` with its licence, notice, schema and `version.json` (stage.sh). The
//! helper of a game is the one the Kiln installed there brought: its path, SHA-256 and version come from the install
//! receipt (`kiln/.install.json`, every file with its SHA-256 and component) and the `version.json` beside it, whose
//! own bytes must be the receipt's. The launcher and kilnctl set [`Installed`] at start
//! (`crate::profile::sfi::use_installed_inspector`); each kiln-mode operation resolves it for its game. A platform
//! with no helper (macOS), a game without one, or one whose files are not the receipt's has no inspector: no plan, with
//! that reason (never another helper, a copy elsewhere or another backend). On Linux the helper must be executable
//! where it was installed (the bundle installs it 0755, `crate::bundle::executable`); a folder mounted noexec or a
//! missing execute bit is said as such.
//!
//! **Cost and reuse** (the per-Play policy): the inspector reads the header, the resources, the backups and the
//! executable in full (1-2 GB on a client). The producer runs it once per plan it writes, and a plan is written only
//! when the launcher activates or re-prepares a profile; a Play that reuses the active plan runs no inspection (the
//! boot witness proves the archive again at start). No earlier inspection is ever reused for a new plan.
use crate::manifest::RunSide;
use serde_json::Value;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::mpsc::{Receiver, RecvTimeoutError};
use std::time::{Duration, Instant};

/// The inspector's `inspector.component`.
pub const COMPONENT: &str = "kiln-eml-inspect";
/// The only `schema_version` read.
pub const SCHEMA_VERSION: u64 = 1;
/// `proof.framing`: how the inspector frames its set digests (the boot witness frames them the same way).
pub const FRAMING: &str = "kiln-eml-inspect archive-set v1";
/// The origin tokens and their exact labels (final; never "vanilla").
pub const ORIGINS: [(&str, &str); 3] = [
    (
        "first-managed-unclassified",
        "unclassified: game files as found when Kiln first managed this install",
    ),
    (
        "rebaselined-unclassified",
        "unclassified: game files as found when Kiln took a new baseline after the game changed",
    ),
    (
        "unrecorded",
        "unrecorded: a working baseline from an earlier start; where its files came from was not recorded",
    ),
];
/// `inventory[].role` and `notes[].code` as the closed schema lists them (the plan fingerprints them).
const ROLES: [&str; 10] = [
    "executable",
    "header",
    "resources",
    "container",
    "retained-container",
    "container-unverified",
    "base-header",
    "base-resources",
    "base-seal",
    "receipt",
];
const NOTES: [&str; 7] = [
    "baseline-outdated",
    "seal-outdated",
    "receipt-baseline-mismatch",
    "receipt-origin-ignored",
    "receipt-stale",
    "pending-generation",
    "unreferenced-containers",
];
/// The most stdout read from the inspector.
pub const MAX_STDOUT: usize = 4 * 1024 * 1024;
/// The most stderr kept (for the reason when it fails).
pub const MAX_STDERR: usize = 64 * 1024;
/// How long one inspection may take (it reads 1-2 GB).
pub const TIMEOUT: Duration = Duration::from_secs(600);

/// Something that gives an inspection's stdout: the shipped helper ([`Tool`]) or a test's stand-in.
pub trait Inspect {
    /// The inspector's stdout for `game` on `side` (one JSON document), or why there is none.
    fn run(&self, game: &Path, side: RunSide) -> Result<Vec<u8>, String>;
    /// The inspector version this Kiln ships: the document must name exactly this one.
    fn version(&self) -> &str;
    /// The inspector to run for `game` when it depends on the game folder ([`Installed`]: the helper the Kiln
    /// installed there); None (the default): this one. Err: `game` has none, and why.
    fn for_game(&self, _game: &Path) -> Result<Option<Box<dyn Inspect>>, String> {
        Ok(None)
    }
}

/// The shipped helper: where it is, its SHA-256 and version as this Kiln shipped them. The launcher and kilnctl take
/// them from the game's install receipt and the `version.json` beside the helper, whose bytes the receipt binds
/// ([`installed`]); never from a mod package or a helper the receipt does not list.
#[derive(Debug, Clone)]
pub struct Tool {
    pub exe: PathBuf,
    /// Lower-case hex.
    pub sha256: String,
    pub version: String,
}

/// The file name the helper must have.
pub const EXE_NAME: &str = if cfg!(windows) {
    "kiln-eml-inspect.exe"
} else {
    "kiln-eml-inspect"
};

fn side_name(side: RunSide) -> &'static str {
    match side {
        RunSide::Client => "client",
        RunSide::Server => "server",
    }
}

impl Inspect for Tool {
    fn version(&self) -> &str {
        &self.version
    }

    fn run(&self, game: &Path, side: RunSide) -> Result<Vec<u8>, String> {
        self.run_with(game, side, TIMEOUT, &mut || {})
    }
}

impl Tool {
    /// [`Inspect::run`] with another time limit, `before_start` called once the helper is checked and before it starts
    /// (tests: what a file swapped in at its path then would do).
    #[doc(hidden)]
    pub fn run_with(
        &self,
        game: &Path,
        side: RunSide,
        timeout: Duration,
        before_start: &mut dyn FnMut(),
    ) -> Result<Vec<u8>, String> {
        let who = format!("the archive inspector {}", self.exe.display());
        if !self.exe.is_absolute()
            || self.exe.file_name().and_then(|n| n.to_str()) != Some(EXE_NAME)
        {
            return Err(format!("{who} is not Kiln's {EXE_NAME}"));
        }
        if !game.is_absolute() {
            return Err(format!(
                "the game folder {} is not an absolute path",
                game.display()
            ));
        }
        // the file as checked is the file started: hashed through a handle that stays open until the process runs, and
        // started through that handle where the system allows it ([`start_command`])
        let mut held = open_held(&self.exe).map_err(|e| format!("{who}: {e}"))?;
        let (sha256, elf) = hash_reader(&mut held).map_err(|e| format!("{who}: {e}"))?;
        if sha256 != self.sha256 {
            return Err(format!(
                "{who} is not the one this Kiln ships (its SHA-256 is {sha256})"
            ));
        }
        before_start();
        let mut cmd = start_command(&self.exe, &held, elf);
        cmd.arg("--game")
            .arg(game)
            .arg("--side")
            .arg(side_name(side));
        cmd.stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt as _;
            cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: no console flashing up from the launcher
        }
        let mut child = cmd.spawn().map_err(|e| {
            if e.kind() == std::io::ErrorKind::PermissionDenied {
                // EACCES/EPERM: no execute bit, or a volume mounted noexec. Kiln runs it only where it was installed:
                // no copy elsewhere, no remount, no other helper
                format!(
                    "{who} cannot be started where it is installed ({e}): its execute permission is missing, or its \
                     folder is on a volume mounted without permission to run programs. Reinstall Kiln into a game \
                     folder that allows it"
                )
            } else {
                format!("{who} could not start: {e}")
            }
        })?;
        drop(held);
        let deadline = Instant::now() + timeout;
        let out = child.stdout.take().map(|s| read_bounded(s, MAX_STDOUT));
        let err = child.stderr.take().map(|s| read_bounded(s, MAX_STDERR));
        let status = loop {
            match child.try_wait() {
                Ok(Some(status)) => break status,
                Ok(None) if Instant::now() >= deadline => {
                    // only this child: its handle, never a name or a process search; reaped here. Its output is not
                    // waited for: a process it started may still hold the pipes (their readers end when they close)
                    let _ = child.kill();
                    let _ = child.wait();
                    return Err(format!(
                        "{who} took longer than {timeout:?} and was stopped"
                    ));
                }
                Ok(None) => std::thread::sleep(Duration::from_millis(20)),
                Err(e) => {
                    let _ = child.kill();
                    let _ = child.wait();
                    return Err(format!("{who}: {e}"));
                }
            }
        };
        // it has ended; its output is whole once the pipes close, which a process it left behind can delay: waited for
        // until the time limit (at least a second), never longer
        let until = deadline.max(Instant::now() + Duration::from_secs(1));
        let collect = |rx: Option<Receiver<Result<Vec<u8>, String>>>| {
            rx.map(
                |rx| match rx.recv_timeout(until.saturating_duration_since(Instant::now())) {
                    Ok(read) => read,
                    Err(RecvTimeoutError::Timeout) => Err(
                        "its output was still open after it ended (a process it started holds it)"
                            .into(),
                    ),
                    Err(RecvTimeoutError::Disconnected) => Err("reader failed".into()),
                },
            )
        };
        let stdout = collect(out);
        let stderr = collect(err);
        let stderr_text = match stderr {
            Some(Ok(bytes)) => String::from_utf8_lossy(&bytes).trim().to_owned(),
            _ => String::new(),
        };
        if !status.success() {
            let tail = if stderr_text.is_empty() {
                String::new()
            } else {
                format!(": {}", cut(&stderr_text, 512))
            };
            return Err(format!("{who} failed ({status}){tail}"));
        }
        match stdout {
            Some(Ok(bytes)) => Ok(bytes),
            Some(Err(e)) => Err(format!("{who}: {e}")),
            None => Err(format!("{who} printed nothing")),
        }
    }
}

/// The command that starts the checked helper.
/// - Linux: an ELF file is started through the very handle it was hashed by (`/proc/self/fd/<n>`, its own path as
///   argv\[0\]), so a file renamed or linked over its path after the check is never what starts. (Only a test's script
///   stand-in, whose hash the shipped helper never has, is not an ELF file; it starts by its path.)
/// - Windows: by its path, which the held handle (shared for reading only) keeps from being written, renamed or deleted
///   until the process has started.
/// - Elsewhere (macOS, where no game runs): by its path; the hash was checked just before.
fn start_command(exe: &Path, held: &std::fs::File, elf: bool) -> std::process::Command {
    #[cfg(target_os = "linux")]
    if elf {
        use std::os::fd::AsRawFd as _;
        use std::os::unix::process::CommandExt as _;
        let mut cmd = std::process::Command::new(format!("/proc/self/fd/{}", held.as_raw_fd()));
        cmd.arg0(exe);
        return cmd;
    }
    let _ = (held, elf);
    std::process::Command::new(exe)
}

/// Opens the helper for reading. Windows: shared for reading only, so it cannot be written, renamed or deleted while
/// it is held (the process start opens it for execution, which reading-only sharing allows).
fn open_held(path: &Path) -> std::io::Result<std::fs::File> {
    let meta = std::fs::symlink_metadata(path)?;
    if !meta.is_file() || reparse(&meta) {
        return Err(std::io::Error::other("not a plain file"));
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt as _;
        std::fs::OpenOptions::new()
            .read(true)
            .share_mode(0x1)
            .custom_flags(0x0020_0000)
            .open(path) // FILE_SHARE_READ, FILE_FLAG_OPEN_REPARSE_POINT
    }
    #[cfg(not(windows))]
    {
        std::fs::File::open(path)
    }
}

#[cfg(windows)]
fn reparse(meta: &std::fs::Metadata) -> bool {
    use std::os::windows::fs::MetadataExt as _;
    meta.file_type().is_symlink() || meta.file_attributes() & 0x400 != 0 // FILE_ATTRIBUTE_REPARSE_POINT
}
#[cfg(not(windows))]
fn reparse(meta: &std::fs::Metadata) -> bool {
    meta.file_type().is_symlink()
}

/// The SHA-256 of everything `reader` gives (lower-case hex), and whether it begins as an ELF file does.
fn hash_reader(reader: &mut impl Read) -> std::io::Result<(String, bool)> {
    use sha2::Digest as _;
    let mut h = sha2::Sha256::new();
    let mut buf = vec![0u8; 64 * 1024];
    let mut head = Vec::with_capacity(4);
    loop {
        let n = reader.read(&mut buf)?;
        if n == 0 {
            break;
        }
        if head.len() < 4 {
            let take = (4 - head.len()).min(n);
            head.extend_from_slice(&buf[..take]);
        }
        h.update(&buf[..n]);
    }
    Ok((crate::hash::hex(&h.finalize()), head == b"\x7fELF"))
}

/// Reads a pipe to its end on a thread, keeping at most `max` bytes; more is an error (the pipe is still drained, so
/// the child never blocks on a full pipe). The result arrives on the receiver when the pipe closes.
fn read_bounded(
    mut pipe: impl Read + Send + 'static,
    max: usize,
) -> Receiver<Result<Vec<u8>, String>> {
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let mut kept = Vec::new();
        let mut buf = [0u8; 16 * 1024];
        let mut over = false;
        let read = loop {
            match pipe.read(&mut buf) {
                Ok(0) => break Ok(()),
                Ok(n) => {
                    if kept.len() + n > max {
                        over = true;
                    } else {
                        kept.extend_from_slice(&buf[..n]);
                    }
                }
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                Err(e) => break Err(e.to_string()),
            }
        };
        let _ = tx.send(read.and_then(|()| {
            if over {
                Err(format!("it printed more than {max} bytes"))
            } else {
                Ok(kept)
            }
        }));
    });
    rx
}

fn cut(text: &str, max: usize) -> &str {
    if text.len() <= max {
        return text;
    }
    let mut n = max;
    while !text.is_char_boundary(n) {
        n -= 1;
    }
    &text[..n]
}

/// Where this Kiln installs the inspector in a game folder: one folder per platform the launcher and kilnctl run on.
pub const INSTALL_DIR: &str = "kiln/tools/eml-inspect";
/// The bundle component that brings it (kiln-eml's, GPL-3).
pub const INSTALL_COMPONENT: &str = "eml";
/// The file beside it that binds its version, protocol and SHA-256 ([`VersionFile`]).
pub const VERSION_FILE: &str = "version.json";
/// The schema it answers in, beside it.
pub const SCHEMA_FILE: &str = "archive-inspection.schema.json";
/// The platform folder of the helper this build runs; None: Kiln ships none for this platform (macOS).
pub const PLATFORM: Option<&str> = if cfg!(all(target_os = "linux", target_arch = "x86_64")) {
    Some("linux-x86_64")
} else if cfg!(all(windows, target_arch = "x86_64")) {
    Some("windows-x86_64")
} else {
    None
};
/// The most `version.json` read.
const MAX_VERSION_FILE: u64 = 64 * 1024;

/// `kiln/tools/eml-inspect/<platform>/version.json` as tools/package/stage.sh writes it (tools/sfi/sfi_stage.py): what
/// the helper beside it is. Closed: an unknown key is no version file this Kiln reads.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct VersionFile {
    /// 1.
    pub format: u32,
    /// [`COMPONENT`].
    pub tool: String,
    /// The version its answers name (`inspector.version`).
    pub version: String,
    /// Its platform folder.
    pub platform: String,
    /// Its file name ([`EXE_NAME`] on that platform).
    pub executable: String,
    /// Its SHA-256 (lower-case hex).
    pub sha256: String,
    /// The `schema_version` it answers in ([`SCHEMA_VERSION`]).
    pub schema_version: u64,
    /// The schema's file name beside it ([`SCHEMA_FILE`]) and SHA-256.
    pub schema: String,
    pub schema_sha256: String,
    /// Its digest framing ([`FRAMING`]).
    pub framing: String,
    /// The Kiln it was built with (the VERSION file), and the commit when the build knew it.
    pub kiln: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub git: Option<String>,
}

fn is_sha256(s: &str) -> bool {
    s.len() == 64
        && s.bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// The game-relative path of `name` in this platform's helper folder.
fn installed_rel(platform: &str, name: &str) -> String {
    format!("{INSTALL_DIR}/{platform}/{name}")
}

/// `rel` ('/' separated) below `game`.
fn below(game: &Path, rel: &str) -> PathBuf {
    rel.split('/').fold(game.to_path_buf(), |p, c| p.join(c))
}

/// The inspector the Kiln installed in `game` brought, for this platform: its path, and its SHA-256 and version as
/// that install recorded them. The receipt (`kiln/.install.json`) must list the helper, its `version.json` and its
/// schema in component [`INSTALL_COMPONENT`]; the `version.json` on disk must be the receipt's bytes and name that
/// helper (its SHA-256, this platform, [`COMPONENT`], [`SCHEMA_VERSION`], [`FRAMING`], the receipt's schema). The
/// helper itself is hashed again when it starts ([`Tool`]). Err: no inspector for this game, and why.
pub fn installed(game: &Path) -> Result<Tool, String> {
    let Some(platform) = PLATFORM else {
        return Err(format!(
            "Kiln ships no archive inspector ({COMPONENT}) for this platform ({} {})",
            std::env::consts::OS,
            std::env::consts::ARCH
        ));
    };
    let receipt = crate::install::read_receipt(game)
        .map_err(|e| {
            format!(
                "the install receipt of {} cannot be read ({e})",
                game.display()
            )
        })?
        .ok_or_else(|| format!("Kiln is not installed in {}", game.display()))?;
    let listed = |name: &str| {
        let rel = installed_rel(platform, name);
        match receipt.files.iter().find(|f| f.path == rel) {
            Some(f)
                if f.component.as_deref() == Some(INSTALL_COMPONENT)
                    && !f.config
                    && is_sha256(&f.sha256) =>
            {
                Ok((rel, f.sha256.clone()))
            }
            Some(_) => Err(format!(
                "the install receipt lists {rel} other than as a file of component {INSTALL_COMPONENT}"
            )),
            None => Err(format!(
                "Kiln's archive inspector ({COMPONENT}) is not installed with this Kiln in {} (no {rel}; it comes \
                 with the {INSTALL_COMPONENT} component)",
                game.display()
            )),
        }
    };
    let (exe_rel, exe_sha) = listed(EXE_NAME)?;
    let (version_rel, version_sha) = listed(VERSION_FILE)?;
    let (_, schema_sha) = listed(SCHEMA_FILE)?;
    let version_path = below(game, &version_rel);
    let bytes = read_plain(&version_path, MAX_VERSION_FILE)
        .map_err(|e| format!("{version_rel} cannot be read ({e})"))?;
    if crate::hash::sha256_hex(&bytes) != version_sha {
        return Err(format!(
            "{version_rel} is not the one this Kiln installed (its SHA-256 differs from the install receipt's)"
        ));
    }
    let v: VersionFile = serde_json::from_slice(&bytes)
        .map_err(|e| format!("{version_rel} is not a version file this Kiln reads ({e})"))?;
    let problems: Vec<&str> = [
        (v.format != 1, "format"),
        (v.tool != COMPONENT, "tool"),
        (v.version.is_empty() || v.version.len() > 64, "version"),
        (v.platform != platform, "platform"),
        (v.executable != EXE_NAME, "executable"),
        (v.sha256 != exe_sha, "sha256 (not the receipt's helper)"),
        (v.schema_version != SCHEMA_VERSION, "schemaVersion"),
        (v.schema != SCHEMA_FILE, "schema"),
        (
            v.schema_sha256 != schema_sha,
            "schemaSha256 (not the receipt's schema)",
        ),
        (v.framing != FRAMING, "framing"),
    ]
    .into_iter()
    .filter_map(|(bad, what)| bad.then_some(what))
    .collect();
    if !problems.is_empty() {
        return Err(format!(
            "{version_rel} does not describe the helper this Kiln installed: {}",
            problems.join(", ")
        ));
    }
    Ok(Tool {
        exe: below(game, &exe_rel),
        sha256: exe_sha,
        version: v.version,
    })
}

/// A plain file's bytes (no link, at most `max`).
fn read_plain(path: &Path, max: u64) -> std::io::Result<Vec<u8>> {
    let meta = std::fs::symlink_metadata(path)?;
    if !meta.is_file() || reparse(&meta) {
        return Err(std::io::Error::other("not a plain file"));
    }
    let mut out = Vec::new();
    std::fs::File::open(path)?
        .take(max + 1)
        .read_to_end(&mut out)?;
    if out.len() as u64 > max {
        return Err(std::io::Error::other("too large"));
    }
    Ok(out)
}

/// The inspector installed with each game's Kiln ([`installed`]), resolved for the game it inspects
/// ([`Inspect::for_game`]): what the launcher and kilnctl run (`crate::profile::sfi::use_installed_inspector`). Its
/// own [`Inspect::version`] is empty, so an answer read without resolving it first is refused.
#[derive(Debug, Clone, Copy, Default)]
pub struct Installed;

impl Inspect for Installed {
    fn run(&self, game: &Path, side: RunSide) -> Result<Vec<u8>, String> {
        installed(game)?.run(game, side)
    }
    fn version(&self) -> &str {
        ""
    }
    fn for_game(&self, game: &Path) -> Result<Option<Box<dyn Inspect>>, String> {
        installed(game).map(|t| Some(Box::new(t) as Box<dyn Inspect>))
    }
}

/// What a launch plan takes from one inspection.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Facts {
    pub side: RunSide,
    pub inspector_version: String,
    /// `target.executable.sha256`: the game executable's file digest (the core compares it at boot).
    pub exe_sha256: String,
    /// `base.sha256`, `input.sha256`.
    pub base_sha256: String,
    pub input_sha256: String,
    /// `base.origin.token`, one of [`ORIGINS`].
    pub origin: &'static str,
    /// `base.evidence`: receipt-baseline, sealed-baseline, unsealed-baseline or current-set.
    pub evidence: String,
    /// `input.class`: owned-output, base-copy or unclassified.
    pub input_class: String,
    /// `proof.containers`: size or sha256.
    pub containers: String,
    /// `receipt`: state, file, size and SHA-256 as the inspector read them ("" when absent).
    pub receipt: [String; 4],
    /// `inventory`: (role, file, size), as listed.
    pub inventory: Vec<(String, String, u64)>,
    /// `notes`' codes, as listed.
    pub notes: Vec<String>,
}

/// Reads one inspection's stdout into [`Facts`] (see the module docs); Err says why there is no proven base.
pub fn facts(stdout: &[u8], side: RunSide, version: &str) -> Result<Facts, String> {
    let bad = |what: &str| format!("the archive inspector's answer {what}");
    if stdout.len() > MAX_STDOUT {
        return Err(bad("is too large"));
    }
    let text = std::str::from_utf8(stdout).map_err(|_| bad("is not UTF-8"))?;
    let doc: Value = serde_json::from_str(text.strip_suffix('\n').unwrap_or(text))
        .map_err(|e| bad(&format!("is not one JSON document ({e})")))?;
    let top = keys(
        &doc,
        &[
            "schema_version",
            "inspector",
            "status",
            "target",
            "proof",
            "input",
            "base",
            "receipt",
            "inventory",
            "reasons",
            "notes",
            "cost",
        ],
        &[],
    )
    .ok_or_else(|| bad("is not an archive inspection of schema 1 (a key is missing or unknown)"))?;
    if top["schema_version"].as_u64() != Some(SCHEMA_VERSION) {
        return Err(bad(&format!(
            "has schema_version {} (this Kiln reads 1)",
            top["schema_version"]
        )));
    }
    let inspector = keys(&top["inspector"], &["component", "version"], &[])
        .ok_or_else(|| bad("names no inspector"))?;
    if inspector["component"].as_str() != Some(COMPONENT) {
        return Err(bad(&format!(
            "comes from {} rather than {COMPONENT}",
            inspector["component"]
        )));
    }
    if inspector["version"].as_str() != Some(version) {
        return Err(bad(&format!(
            "comes from inspector version {} rather than {version}, the one this Kiln ships",
            inspector["version"]
        )));
    }
    let reasons: Vec<String> = top["reasons"]
        .as_array()
        .ok_or_else(|| bad("has no reason list"))?
        .iter()
        .map(|r| {
            let code = r["code"].as_str().unwrap_or("?");
            let text = r["text"].as_str().unwrap_or("");
            match r["file"].as_str() {
                Some(file) => format!("{code} ({file}): {text}"),
                None => format!("{code}: {text}"),
            }
        })
        .collect();
    match top["status"].as_str() {
        Some("ok") if reasons.is_empty() => {}
        Some("ok") => return Err(bad("says ok and gives refusal reasons")),
        Some("refused") => {
            return Err(format!(
                "the archive inspector found no proven game archive base: {}",
                if reasons.is_empty() {
                    "no reason given".to_owned()
                } else {
                    reasons.join("; ")
                }
            ));
        }
        _ => return Err(bad("has an unknown status")),
    }
    let target = keys(&top["target"], &["side", "stem", "executable"], &[])
        .ok_or_else(|| bad("names no target"))?;
    let stem = match side {
        RunSide::Client => "enshrouded",
        RunSide::Server => "enshrouded_server",
    };
    if target["side"].as_str() != Some(side_name(side)) || target["stem"].as_str() != Some(stem) {
        return Err(bad(&format!(
            "is for {} {}, not this {}",
            target["side"],
            target["stem"],
            side_name(side)
        )));
    }
    let exe = keys(&target["executable"], &["file", "size", "sha256"], &[])
        .ok_or_else(|| bad("names no executable digest"))?;
    let exe_sha256 = hash(&exe["sha256"]).ok_or_else(|| bad("has no executable SHA-256"))?;
    let proof = keys(&top["proof"], &["framing", "containers"], &[])
        .ok_or_else(|| bad("names no proof"))?;
    if proof["framing"].as_str() != Some(FRAMING) {
        return Err(bad(&format!(
            "frames its digests as {} rather than {FRAMING}",
            proof["framing"]
        )));
    }
    let containers = one_of(&proof["containers"], &["size", "sha256"])
        .ok_or_else(|| bad("has an unknown container proof"))?;
    let input = keys(&top["input"], &["class", "sha256", "files"], &[])
        .ok_or_else(|| bad("has no input set"))?;
    let input_class = one_of(
        &input["class"],
        &["owned-output", "base-copy", "unclassified"],
    )
    .ok_or_else(|| bad("has an unknown input class"))?;
    let input_sha256 = hash(&input["sha256"]).ok_or_else(|| bad("has no input digest"))?;
    let base = keys(
        &top["base"],
        &["evidence", "sha256", "origin", "baseline", "files"],
        &[],
    )
    .ok_or_else(|| bad("has no base set"))?;
    let evidence = one_of(
        &base["evidence"],
        &[
            "receipt-baseline",
            "sealed-baseline",
            "unsealed-baseline",
            "current-set",
        ],
    )
    .ok_or_else(|| bad("has unknown base evidence"))?;
    let base_sha256 = hash(&base["sha256"]).ok_or_else(|| bad("has no base digest"))?;
    let origin =
        keys(&base["origin"], &["token", "label"], &[]).ok_or_else(|| bad("has no base origin"))?;
    let &(token, _) = ORIGINS
        .iter()
        .find(|(t, l)| origin["token"].as_str() == Some(*t) && origin["label"].as_str() == Some(*l))
        .ok_or_else(|| {
            bad(&format!(
                "gives the base origin {} with a label that is not its own",
                origin["token"]
            ))
        })?;
    // the origin fits the evidence it rests on (inspection.rs: the current set is first-managed or rebaselined, a seal
    // or an unsealed pair is unrecorded, a receipt baseline keeps the origin its receipt carried for these backups)
    let fits = match evidence {
        "current-set" => token != "unrecorded",
        "sealed-baseline" | "unsealed-baseline" => token == "unrecorded",
        _ => true,
    };
    if !fits {
        return Err(bad(&format!(
            "gives the origin {token} to a base proven by {evidence}"
        )));
    }
    let receipt = keys(&top["receipt"], &["state", "file"], &["size", "sha256"])
        .ok_or_else(|| bad("has no receipt section"))?;
    let state = one_of(&receipt["state"], &["absent", "understood", "unknown"])
        .ok_or_else(|| bad("has an unknown receipt state"))?;
    if state == "unknown" {
        return Err(bad(
            "says ok over an ownership receipt it does not understand",
        ));
    }
    let receipt = [
        state.to_owned(),
        receipt["file"]
            .as_str()
            .ok_or_else(|| bad("names no receipt file"))?
            .to_owned(),
        receipt
            .get("size")
            .and_then(Value::as_u64)
            .map(|n| n.to_string())
            .unwrap_or_default(),
        receipt
            .get("sha256")
            .map(|v| hash(v).ok_or_else(|| bad("has a receipt digest that is not one")))
            .transpose()?
            .unwrap_or_default(),
    ];
    let mut inventory = Vec::new();
    for item in top["inventory"]
        .as_array()
        .ok_or_else(|| bad("has no inventory"))?
    {
        let item = keys(item, &["role", "file", "size"], &[])
            .ok_or_else(|| bad("has an inventory entry that is not one"))?;
        match (
            one_of(&item["role"], &ROLES),
            item["file"].as_str(),
            item["size"].as_u64(),
        ) {
            (Some(role), Some(file), Some(size)) => {
                inventory.push((role.to_owned(), file.to_owned(), size))
            }
            _ => return Err(bad("has an inventory entry that is not one")),
        }
    }
    let notes = top["notes"]
        .as_array()
        .ok_or_else(|| bad("has no note list"))?
        .iter()
        .map(|n| {
            keys(n, &["code", "file", "text"], &[])
                .and_then(|n| one_of(&n["code"], &NOTES))
                .map(str::to_owned)
                .ok_or_else(|| bad("has a note that is not one"))
        })
        .collect::<Result<Vec<_>, _>>()?;
    Ok(Facts {
        side,
        inspector_version: version.to_owned(),
        exe_sha256,
        base_sha256,
        input_sha256,
        origin: token,
        evidence: evidence.to_owned(),
        input_class: input_class.to_owned(),
        containers: containers.to_owned(),
        receipt,
        inventory,
        notes,
    })
}

/// Runs `inspector` and reads its answer: the facts a plan needs, or why there is no proven base.
pub fn inspect(inspector: &dyn Inspect, game: &Path, side: RunSide) -> Result<Facts, String> {
    if let Some(resolved) = inspector.for_game(game)? {
        return inspect(resolved.as_ref(), game, side);
    }
    let stdout = inspector.run(game, side)?;
    facts(&stdout, side, inspector.version())
}

/// `v` as an object with exactly the `required` keys and any of the `optional` ones.
fn keys<'a>(
    v: &'a Value,
    required: &[&str],
    optional: &[&str],
) -> Option<&'a serde_json::Map<String, Value>> {
    let o = v.as_object()?;
    (required.iter().all(|k| o.contains_key(*k))
        && o.keys()
            .all(|k| required.contains(&k.as_str()) || optional.contains(&k.as_str())))
    .then_some(o)
}
fn one_of<'a>(v: &'a Value, words: &[&str]) -> Option<&'a str> {
    v.as_str().filter(|s| words.contains(s))
}
fn hash(v: &Value) -> Option<String> {
    v.as_str()
        .filter(|s| {
            s.len() == 64
                && s.bytes()
                    .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
        })
        .map(str::to_owned)
}
