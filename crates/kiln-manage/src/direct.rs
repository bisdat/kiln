//! Starting the game client directly instead of through Steam's Play (`steam://rungameid`), per install: a copy of the
//! game in a folder no Steam library knows (an older build kept for mods) cannot be started by Steam, which only ever
//! starts the copy in its own library.
//!
//! - **Windows**: `<game>\enshrouded.exe`, working directory the game folder. Kiln's proxy is `dinput8.dll` next to the
//!   exe, which Windows loads first: nothing else is set.
//! - **Linux**: through a Proton build (`proton run`, with `STEAM_COMPAT_DATA_PATH` and
//!   `STEAM_COMPAT_CLIENT_INSTALL_PATH`) or a Wine (`wine`, with `WINEPREFIX`) the player picks; by default the Proton
//!   Steam maps to Enshrouded, else the newest found. The prefix is Kiln's own per install
//!   (`$XDG_DATA_HOME/kiln-prefixes/<installId>`, outside the data folder: a prefix links `z:` to `/` and holds the
//!   saves, so it never moves with the data folder) unless the player picks one. `WINEDLLOVERRIDES` loads `dinput8`
//!   native first, as the Steam launch option Kiln advises does (`game::DLL_OVERRIDE_ADVICE`), with `xinput9_1_0`
//!   when Ultimate ASI Loader is chained.
//!
//! The game's own Steam API still talks to Steam: Steam must be running. Kiln never writes or changes
//! `steam_appid.txt`, never sets `SteamAppId`/`SteamGameId`, never touches `steam_api64.dll` or anything else of the
//! game's Steam or ownership checks. A copy that asks Steam to restart it is restarted by Steam (Steam's own copy),
//! which Kiln does not work around (docs/kiln/launcher/custom-game-folders.md).
//!
//! The settings live in the install's record (`InstallRecord.start`, `game::set_start`); [`command_for`] builds the
//! command without starting anything (tests), [`plan`] resolves the runner and prefix, [`spawn`] starts it.

use crate::game::{GameInstall, GameKind, Platform};
use crate::steam::SteamRoot;
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// How an install starts.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum StartMode {
    /// `steam://rungameid/1203620`: Steam starts the copy in its library (the default for a Steam install).
    Steam,
    /// The launcher starts this folder's `enshrouded.exe` itself (the default for a folder no Steam library knows).
    Direct,
}

/// `InstallRecord.start`: what the player chose for an install. A field left out is Kiln's default.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct StartSettings {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mode: Option<StartMode>,
    /// Linux: a Proton folder (with its `proton` script) or a Wine (the `wine` binary, or its `bin` folder). None: the
    /// Proton Steam uses for Enshrouded, else the newest found ([`default_runner`]).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub runner: Option<PathBuf>,
    /// Linux: the prefix (for Proton the compatdata folder that holds `pfx`, for Wine the prefix itself). None: Kiln's
    /// own for this install ([`own_prefix`]).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub prefix: Option<PathBuf>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum RunnerKind {
    Proton,
    Wine,
}

/// A Proton or Wine the game can run under (Linux).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RunnerInfo {
    pub kind: RunnerKind,
    /// Proton: the folder with the `proton` script; Wine: the `wine` binary.
    pub path: PathBuf,
    /// The folder name (`GE-Proton10-25`, `Proton 9.0`), or `Wine (<path>)`.
    pub label: String,
    /// `steam` (Valve's Proton in a Steam library), `compat` (a compatibility-tools folder: GE-Proton and others),
    /// `system` (wine on PATH), `custom` (a path the player gave).
    pub source: String,
}

/// A folder no Steam library knows: added by hand (`source` manual) and outside any library (`library` None).
pub fn is_custom(install: &GameInstall) -> bool {
    install.kind == GameKind::Client && install.source == "manual" && install.library.is_none()
}

/// The mode in force: the player's choice, else Direct for a custom folder and Steam for the rest. A server never
/// starts here (server_start).
pub fn effective_mode(install: &GameInstall, settings: Option<&StartSettings>) -> StartMode {
    if install.kind != GameKind::Client {
        return StartMode::Steam;
    }
    settings.and_then(|s| s.mode).unwrap_or(if is_custom(install) { StartMode::Direct } else { StartMode::Steam })
}

/// The settings of an install from its record (default when it has none).
pub fn settings(dirs: &crate::paths::DataDirs, install: &GameInstall) -> StartSettings {
    crate::game::record(dirs, &install.id).ok().flatten().and_then(|r| r.start).unwrap_or_default()
}

/// The mode in force for an install, from its record.
pub fn mode_of(dirs: &crate::paths::DataDirs, install: &GameInstall) -> StartMode {
    effective_mode(install, Some(&settings(dirs, install)))
}

/// Where Kiln keeps its own prefixes (Linux): `$XDG_DATA_HOME/kiln-prefixes`, else `~/.local/share/kiln-prefixes`.
/// None on Windows.
pub fn prefixes_root(env: &crate::paths::DataEnv) -> Option<PathBuf> {
    if env.windows {
        return None;
    }
    let base = match env.xdg_data_home.as_ref().filter(|d| Path::new(d).is_absolute()) {
        Some(x) => PathBuf::from(x),
        None => PathBuf::from(env.home.as_ref()?).join(".local/share"),
    };
    Some(base.join("kiln-prefixes"))
}

/// Kiln's own prefix of an install (its compatdata folder; the Wine prefix is `pfx` in it). None on Windows.
pub fn own_prefix(env: &crate::paths::DataEnv, install_id: &str) -> Option<PathBuf> {
    prefixes_root(env).map(|r| r.join(install_id))
}

fn looks_like_prefix(p: &Path) -> bool {
    p.join("system.reg").is_file() || p.join("dosdevices").is_dir()
}

/// The compatdata folder (`STEAM_COMPAT_DATA_PATH`) and the Wine prefix (`WINEPREFIX`) for a runner kind, from the
/// player's folder or Kiln's own. A Proton build wants the folder that holds `pfx`; a picked `pfx` itself is taken as
/// its parent's. A Wine wants the prefix; a picked compatdata folder means the `pfx` in it.
pub fn prefix_paths(kind: RunnerKind, chosen: Option<&Path>, own: &Path) -> Result<(PathBuf, PathBuf)> {
    let Some(c) = chosen else { return Ok((own.to_path_buf(), own.join("pfx"))) };
    if !c.is_absolute() {
        return Err(Error::invalid(format!("the prefix must be an absolute path, not {}", c.display())));
    }
    match kind {
        RunnerKind::Proton => {
            if c.join("pfx").exists() || !looks_like_prefix(c) {
                Ok((c.to_path_buf(), c.join("pfx")))
            } else if c.file_name().is_some_and(|n| n == "pfx") && let Some(parent) = c.parent() {
                Ok((parent.to_path_buf(), c.to_path_buf()))
            } else {
                Err(Error::invalid(format!(
                    "{} is a Wine prefix: Proton needs the folder that holds its pfx (a compatdata folder). Pick that folder, or run with Wine.",
                    c.display()
                )))
            }
        }
        RunnerKind::Wine => {
            if looks_like_prefix(&c.join("pfx")) {
                Ok((c.to_path_buf(), c.join("pfx")))
            } else {
                Ok((c.to_path_buf(), c.to_path_buf()))
            }
        }
    }
}

// ---------------------------------------------------------------- runners (Linux)

/// What a runner path is: a folder with a `proton` script is a Proton build; a `wine` binary, or a folder with
/// `bin/wine` or `wine` in it, is a Wine.
pub fn runner_at(path: &Path) -> Result<RunnerInfo> {
    if !path.is_absolute() {
        return Err(Error::invalid(format!("the Proton or Wine must be an absolute path, not {}", path.display())));
    }
    let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    if path.is_dir() {
        if path.join("proton").is_file() {
            return Ok(RunnerInfo { kind: RunnerKind::Proton, path: path.to_path_buf(), label: name, source: "custom".into() });
        }
        for wine in [path.join("bin/wine"), path.join("wine"), path.join("files/bin/wine")] {
            if crate::util::is_executable(&wine) {
                return Ok(RunnerInfo { kind: RunnerKind::Wine, label: format!("Wine ({})", wine.display()), path: wine, source: "custom".into() });
            }
        }
        return Err(Error::new(ErrorCode::NotFound, format!("{} has no proton script and no wine", path.display())));
    }
    if crate::util::is_executable(path) {
        if name == "proton" && let Some(dir) = path.parent() {
            return runner_at(dir);
        }
        return Ok(RunnerInfo { kind: RunnerKind::Wine, label: format!("Wine ({})", path.display()), path: path.to_path_buf(), source: "custom".into() });
    }
    Err(Error::new(ErrorCode::NotFound, format!("{} is not a Proton folder or a wine program", path.display())))
}

fn proton_dirs_in(root: &Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = std::fs::read_dir(root).into_iter().flatten().flatten().map(|e| e.path())
        .filter(|d| d.join("proton").is_file()).collect();
    v.sort();
    v
}

/// Every Proton and Wine found: the compatibility-tools folders of the Steam roots and GE-Proton's usual places
/// (`wine::ge_roots`), Valve's Proton builds in the Steam libraries, and `wine` on PATH. GE-Proton newest first, then
/// the others by name (newest first).
pub fn runners(roots: &[SteamRoot], home: &Path) -> Vec<RunnerInfo> {
    let mut out: Vec<RunnerInfo> = Vec::new();
    let mut push = |kind, path: PathBuf, label: String, source: &str| {
        let canon = path.canonicalize().unwrap_or(path);
        if !out.iter().any(|r: &RunnerInfo| r.path == canon) {
            out.push(RunnerInfo { kind, path: canon, label, source: source.into() });
        }
    };
    let mut compat_roots: Vec<PathBuf> = roots.iter().map(|r| r.path.join("compatibilitytools.d")).collect();
    compat_roots.extend(crate::wine::ge_roots(home));
    let mut compat: Vec<PathBuf> = compat_roots.iter().flat_map(|r| proton_dirs_in(r)).collect();
    let name = |p: &Path| p.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    compat.sort_by(|a, b| {
        let (na, nb) = (name(a), name(b));
        crate::wine::ge_version(&nb).cmp(&crate::wine::ge_version(&na)).then(nb.cmp(&na))
    });
    for d in compat {
        let n = name(&d);
        push(RunnerKind::Proton, d, n, "compat");
    }
    let mut valve: Vec<PathBuf> = Vec::new();
    for r in roots {
        for lib in crate::steam::libraries(r).unwrap_or_default() {
            valve.extend(proton_dirs_in(&lib.path.join("steamapps/common")).into_iter().filter(|d| name(d).starts_with("Proton")));
        }
    }
    valve.sort_by_key(|d| std::cmp::Reverse(name(d)));
    for d in valve {
        let n = name(&d);
        push(RunnerKind::Proton, d, n, "steam");
    }
    if let Some(w) = crate::util::which("wine") {
        let label = format!("Wine ({})", w.display());
        push(RunnerKind::Wine, w, label, "system");
    }
    out
}

/// The runner a direct start uses when the player chose none: the Proton Steam maps to Enshrouded (or Steam's
/// default mapping), else the first of `all` (GE-Proton newest first, Valve's Protons, then a system Wine).
pub fn default_runner(roots: &[SteamRoot], all: &[RunnerInfo]) -> Option<RunnerInfo> {
    let mut tool_roots: Vec<PathBuf> = roots.iter().map(|r| r.path.join("compatibilitytools.d")).collect();
    tool_roots.extend(crate::wine::ge_roots(&crate::util::home()));
    let libraries: Vec<PathBuf> = roots.iter().flat_map(|r| crate::steam::libraries(r).unwrap_or_default().into_iter().map(|l| l.path)).collect();
    for r in roots {
        for app in [crate::CLIENT_APP_ID, 0] {
            if let Some(name) = crate::steam::compat_tool(r, app).ok().flatten()
                && let Some(dir) = crate::server::proton::find_tool(&name, &tool_roots, &libraries)
                && dir.join("proton").is_file()
            {
                let canon = dir.canonicalize().unwrap_or(dir);
                return Some(all.iter().find(|x| x.path == canon).cloned().unwrap_or(RunnerInfo {
                    kind: RunnerKind::Proton, label: name, path: canon, source: "steam".into(),
                }));
            }
        }
    }
    all.first().cloned()
}

// ---------------------------------------------------------------- the command

/// What a direct start runs: built by [`command_for`] without starting anything.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DirectCommand {
    pub program: PathBuf,
    pub args: Vec<String>,
    /// The working directory: the game folder.
    pub cwd: PathBuf,
    /// Set for the game (in this order).
    pub env: Vec<(String, String)>,
    /// Inherited variables removed first (another runner, prefix or Steam app of the launcher's own environment).
    pub env_remove: Vec<String>,
    /// Linux: the runner and the Wine prefix the game runs in.
    pub runner: Option<RunnerInfo>,
    pub prefix: Option<PathBuf>,
}

/// Inherited variables a direct start never passes on (Linux): they would mix another Wine, prefix or Steam app into
/// this start. `SteamAppId`/`SteamGameId` are Steam's to set when Steam starts a game; Kiln never sets them.
pub const SCRUB: &[&str] = &[
    "WINEPREFIX", "WINESERVER", "WINELOADER", "WINEDLLPATH", "WINEARCH", "WINEDLLOVERRIDES",
    "STEAM_COMPAT_DATA_PATH", "STEAM_COMPAT_CLIENT_INSTALL_PATH", "STEAM_COMPAT_APP_ID", "SteamAppId", "SteamGameId",
    "LD_PRELOAD", "KILN_DISABLE",
];

/// The `WINEDLLOVERRIDES` of a direct start: Kiln's proxy native first (as the advised Steam launch option), with
/// `xinput9_1_0` when Ultimate ASI Loader is chained; for a plain Wine also `mshtml=` (no Gecko prompt; never
/// `mscoree`: wine.rs).
pub fn dll_overrides(kind: RunnerKind, chained_asi: bool) -> String {
    let proxy = if chained_asi { "dinput8,xinput9_1_0=n,b" } else { "dinput8=n,b" };
    match kind {
        RunnerKind::Proton => proxy.to_string(),
        RunnerKind::Wine => format!("mshtml=;{proxy}"),
    }
}

/// The command of a direct start, from facts only (no file is read, nothing starts). `windows`: the host's rules;
/// `runner`, `compat` (compatdata for Proton, the Wine prefix for Wine) and `steam_root` are needed on Linux only.
pub fn command_for(windows: bool, install: &GameInstall, runner: Option<&RunnerInfo>, compat: Option<&Path>,
                   steam_root: Option<&Path>, chained_asi: bool) -> Result<DirectCommand> {
    if install.kind != GameKind::Client {
        return Err(Error::invalid("a direct start is for the game client; servers start through server_start"));
    }
    let exe = install.path.join(&install.exe);
    if windows {
        return Ok(DirectCommand {
            program: exe, args: Vec::new(), cwd: install.path.clone(), env: Vec::new(),
            env_remove: vec!["KILN_DISABLE".into()], runner: None, prefix: None,
        });
    }
    let runner = runner.ok_or_else(|| Error::new(ErrorCode::NotFound,
        "No Proton or Wine was found to start the game with: install GE-Proton or Proton through Steam, or pick a Wine."))?;
    let compat = compat.ok_or_else(|| Error::invalid("a direct start under Proton or Wine needs a prefix"))?;
    let env_remove = SCRUB.iter().map(|s| s.to_string()).collect();
    let exe_arg = exe.to_string_lossy().into_owned();
    Ok(match runner.kind {
        RunnerKind::Proton => {
            let steam = steam_root.ok_or_else(|| Error::new(ErrorCode::NotFound,
                "Proton needs Steam's folder (STEAM_COMPAT_CLIENT_INSTALL_PATH) and no Steam install was found. Install Steam (it must run for the game anyway), or pick a Wine."))?;
            DirectCommand {
                program: runner.path.join("proton"),
                args: vec!["run".into(), exe_arg],
                cwd: install.path.clone(),
                env: vec![
                    ("STEAM_COMPAT_DATA_PATH".into(), compat.display().to_string()),
                    ("STEAM_COMPAT_CLIENT_INSTALL_PATH".into(), steam.display().to_string()),
                    ("WINEDLLOVERRIDES".into(), dll_overrides(RunnerKind::Proton, chained_asi)),
                ],
                env_remove,
                runner: Some(runner.clone()),
                prefix: Some(compat.join("pfx")),
            }
        }
        RunnerKind::Wine => DirectCommand {
            program: runner.path.clone(),
            args: vec![exe_arg],
            cwd: install.path.clone(),
            env: vec![
                ("WINEPREFIX".into(), compat.display().to_string()),
                ("WINEDLLOVERRIDES".into(), dll_overrides(RunnerKind::Wine, chained_asi)),
            ],
            env_remove,
            runner: Some(runner.clone()),
            prefix: Some(compat.to_path_buf()),
        },
    })
}

/// The runner of an install's settings (Linux): the player's path, else [`default_runner`].
pub fn resolve_runner(settings: &StartSettings, roots: &[SteamRoot]) -> Result<RunnerInfo> {
    match &settings.runner {
        Some(p) => runner_at(p),
        None => default_runner(roots, &runners(roots, &crate::util::home())).ok_or_else(|| Error::new(ErrorCode::NotFound,
            "No Proton or Wine was found to start the game with: install GE-Proton or Proton through Steam, or pick a Wine.")),
    }
}

/// The Steam folder Proton is told about: the root of the first Steam install found (a native one first).
pub fn steam_root_for(roots: &[SteamRoot]) -> Option<PathBuf> {
    roots.iter().find(|r| r.flavor == crate::steam::SteamFlavor::Native).or(roots.first()).map(|r| r.path.clone())
}

/// The command of a direct start of `install` on this host with its settings.
pub fn plan(install: &GameInstall, settings: &StartSettings, roots: &[SteamRoot]) -> Result<DirectCommand> {
    let chained = crate::game::asi_loader_chained(install);
    if cfg!(windows) {
        return command_for(true, install, None, None, None, chained);
    }
    let runner = resolve_runner(settings, roots)?;
    let own = own_prefix(&crate::paths::DataEnv::from_process(), &install.id)
        .ok_or_else(|| Error::new(ErrorCode::NotFound, "HOME is not set: Kiln has no place for the game's prefix"))?;
    let (compat, pfx) = prefix_paths(runner.kind, settings.prefix.as_deref(), &own)?;
    let target = if runner.kind == RunnerKind::Proton { compat } else { pfx };
    command_for(false, install, Some(&runner), Some(&target), steam_root_for(roots).as_deref(), chained)
}

/// The Wine prefix a direct start of `install` uses on this host (None on Windows, or when no runner is found): what
/// the install's `prefix` is while it starts directly (saves, path mode).
pub fn prefix_of(install: &GameInstall, settings: &StartSettings, roots: &[SteamRoot]) -> Option<(Platform, PathBuf)> {
    if cfg!(windows) {
        return None;
    }
    let kind = match &settings.runner {
        Some(p) => runner_at(p).ok()?.kind,
        None => resolve_runner(settings, roots).map(|r| r.kind).unwrap_or(RunnerKind::Proton),
    };
    let own = own_prefix(&crate::paths::DataEnv::from_process(), &install.id)?;
    let (_, pfx) = prefix_paths(kind, settings.prefix.as_deref(), &own).ok()?;
    Some((if kind == RunnerKind::Proton { Platform::Proton } else { Platform::Wine }, pfx))
}

/// Starts the command, detached: the game outlives the launcher. Linux: its own session, no AppImage environment
/// (`hostenv`), output into `log` (the Proton/Wine output; None: discarded). Windows: out of the launcher's job when it
/// may. The child is reaped on a thread. Returns the pid of the started program (Proton's script or wine on Linux, not
/// the game's: the watcher finds the game by its folder).
pub fn spawn(cmd: &DirectCommand, log: Option<&Path>) -> Result<u32> {
    let mut c = crate::hostenv::command(&cmd.program);
    for k in &cmd.env_remove {
        c.env_remove(k);
    }
    c.envs(cmd.env.iter().map(|(k, v)| (k.as_str(), v.as_str()))).args(&cmd.args).current_dir(&cmd.cwd);
    c.stdin(std::process::Stdio::null());
    match log.and_then(|p| {
        if let Some(d) = p.parent() {
            let _ = std::fs::create_dir_all(d);
        }
        std::fs::File::create(p).ok()
    }) {
        Some(f) => {
            let e = f.try_clone().map_err(Error::from)?;
            c.stdout(f).stderr(e);
        }
        None => {
            c.stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null());
        }
    }
    if let Some(prefix) = &cmd.prefix
        && let Some(parent) = prefix.parent()
    {
        // Proton makes `pfx` itself, in a compatdata folder that exists
        std::fs::create_dir_all(parent)?;
    }
    let child = spawn_detached(&mut c).map_err(|e| Error::new(ErrorCode::Unsupported, format!("starting {}: {e}", cmd.program.display())))?;
    let pid = child.id();
    let mut child = child;
    std::thread::spawn(move || {
        let _ = child.wait();
    });
    Ok(pid)
}

#[cfg(unix)]
fn spawn_detached(cmd: &mut std::process::Command) -> std::io::Result<std::process::Child> {
    use std::os::unix::process::CommandExt;
    // SAFETY: setsid is async-signal-safe; the game gets its own session and outlives the launcher.
    unsafe {
        cmd.pre_exec(|| {
            if libc::setsid() < 0 {
                return Err(std::io::Error::last_os_error());
            }
            Ok(())
        });
    }
    cmd.spawn()
}

#[cfg(windows)]
fn spawn_detached(cmd: &mut std::process::Command) -> std::io::Result<std::process::Child> {
    use std::os::windows::process::CommandExt;
    const CREATE_BREAKAWAY_FROM_JOB: u32 = 0x0100_0000;
    match cmd.creation_flags(CREATE_BREAKAWAY_FROM_JOB).spawn() {
        Ok(c) => Ok(c),
        Err(_) => cmd.creation_flags(0).spawn(),
    }
}

/// `game_start_get`: everything the launcher shows and offers for an install's start.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StartSetup {
    /// The mode in force ([`effective_mode`]).
    pub mode: StartMode,
    /// What the player chose (fields left out: Kiln's defaults).
    pub settings: StartSettings,
    /// A folder no Steam library knows ([`is_custom`]).
    pub custom: bool,
    /// The launcher runs on Windows (no Proton, Wine or prefix to pick).
    pub windows: bool,
    /// Linux: every Proton and Wine found ([`runners`]).
    pub runners: Vec<RunnerInfo>,
    /// Linux: the runner a direct start uses now (the player's, else [`default_runner`]); None when there is none.
    pub runner: Option<RunnerInfo>,
    /// Linux: the runner "automatic" means ([`default_runner`]).
    pub default_runner: Option<RunnerInfo>,
    /// Why there is no runner, or why the player's choice cannot be used.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub problem: Option<String>,
    /// Linux: Kiln's own prefix (compatdata) for this install.
    pub own_prefix: Option<PathBuf>,
    /// The command a direct start runs now (None when it cannot be built: `problem`).
    pub command: Option<DirectCommand>,
    /// A Steam client runs now: the game's Steam API needs it.
    pub steam_running: bool,
}

/// [`StartSetup`] of an install.
pub fn setup(dirs: &crate::paths::DataDirs, install: &GameInstall, roots: &[SteamRoot]) -> StartSetup {
    let settings = settings(dirs, install);
    let windows = cfg!(windows);
    let all = if windows { Vec::new() } else { runners(roots, &crate::util::home()) };
    let auto = if windows { None } else { default_runner(roots, &all) };
    let (runner, mut problem) = if windows {
        (None, None)
    } else {
        match &settings.runner {
            Some(p) => match runner_at(p) {
                Ok(r) => (Some(all.iter().find(|x| x.path == r.path).cloned().unwrap_or(r)), None),
                Err(e) => (None, Some(e.message)),
            },
            None => match auto.clone() {
                Some(r) => (Some(r), None),
                None => (None, Some("No Proton or Wine was found to start the game with: install GE-Proton or Proton through Steam, or pick a Wine.".into())),
            },
        }
    };
    let command = match plan(install, &settings, roots) {
        Ok(c) => Some(c),
        Err(e) => {
            problem.get_or_insert(e.message);
            None
        }
    };
    StartSetup {
        mode: effective_mode(install, Some(&settings)),
        custom: is_custom(install),
        windows,
        runners: all,
        runner,
        default_runner: auto,
        problem,
        own_prefix: own_prefix(&crate::paths::DataEnv::from_process(), &install.id),
        command,
        steam_running: steam_running(),
        settings,
    }
}

/// Whether a Steam client runs on this PC (`steam` / `steam.exe`): a direct start needs it for the game's Steam API.
pub fn steam_running() -> bool {
    crate::proc::list().unwrap_or_default().iter().any(|p| {
        let n = p.name.to_ascii_lowercase();
        n == "steam" || n == "steam.exe"
    })
}

/// The newest runtime bundle of the data folder for `side` (what `build_check` reads when Kiln is not in the game yet).
pub fn newest_bundle(dirs: &crate::paths::DataDirs, side: crate::manifest::RunSide) -> Option<PathBuf> {
    let mut list = crate::bundle::list(dirs).ok()?;
    list.retain(|b| b.sides.contains(&side));
    list.sort_by(crate::bundle::compare);
    list.pop().map(|b| b.file)
}

// ---------------------------------------------------------------- the game build

/// Whether Kiln knows the build of an install's exe (sigdb's build records, by the exe's sha256).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BuildCheck {
    pub sha256: String,
    /// The first 12 hex digits (what the player sees).
    pub short: String,
    /// A verified build record exists: Kiln's hooks run on it. False: Kiln starts with the game but stays idle.
    pub known: bool,
    /// Where the records were read: `game` (`kiln/sigdb/builds/<side>/` of the installed runtime) or `bundle` (the
    /// runtime the launcher would install).
    pub from: String,
}

/// The sentence for a build Kiln does not know (the launcher's i18n `play.unknownBuild.text` says the same).
pub fn unknown_build_sentence(short: &str) -> String {
    format!("Kiln doesn't know this game build ({short}). Kiln starts with the game but stays idle: no mods load.")
}

/// sha256 → whether its build record is verified, from a runtime manifest's `builds` index.
pub fn known_in_manifest(m: &crate::bundle::RuntimeManifest, side: crate::manifest::RunSide, sha: &str) -> bool {
    let list = match side {
        crate::manifest::RunSide::Client => &m.builds.client,
        crate::manifest::RunSide::Server => &m.builds.server,
    };
    list.iter().any(|b| b.sha256.eq_ignore_ascii_case(sha) && b.verified)
}

/// Whether the record `kiln/sigdb/builds/<side>/<sha>.json` in `game` says `"verified": true` (what the runtime reads).
pub fn known_in_game(game: &Path, side: &str, sha: &str) -> Option<bool> {
    let dir = game.join("kiln/sigdb/builds").join(side);
    if !dir.is_dir() {
        return None;
    }
    let rec = std::fs::read(dir.join(format!("{}.json", sha.to_ascii_lowercase()))).ok()
        .and_then(|b| serde_json::from_slice::<serde_json::Value>(&b).ok());
    Some(rec.and_then(|v| v.get("verified").and_then(|x| x.as_bool())).unwrap_or(false))
}

/// The build check of an install: the installed runtime's records when Kiln is in the game, else the `builds` index of
/// the runtime zip `bundle` gives (the one the launcher would install; asked only then). None when neither is there or the exe cannot be read. The exe's
/// sha256 is remembered while the file is unchanged (`filecache`).
pub fn build_check(install: &GameInstall, bundle: impl FnOnce() -> Option<PathBuf>) -> Option<BuildCheck> {
    let (side, run_side) = match install.kind {
        GameKind::Client => ("client", crate::manifest::RunSide::Client),
        GameKind::Server => ("server", crate::manifest::RunSide::Server),
    };
    let sha = crate::filecache::sha256(&install.path.join(&install.exe)).ok()?.to_ascii_lowercase();
    let short = sha.chars().take(12).collect::<String>();
    if let Some(known) = known_in_game(&install.path, side, &sha) {
        return Some(BuildCheck { sha256: sha, short, known, from: "game".into() });
    }
    let known = bundle_known(&bundle()?, run_side, &sha)?;
    Some(BuildCheck { sha256: sha, short, known, from: "bundle".into() })
}

#[cfg(feature = "zip")]
fn bundle_known(zip: &Path, side: crate::manifest::RunSide, sha: &str) -> Option<bool> {
    use std::sync::{Arc, LazyLock};
    static MANIFESTS: LazyLock<crate::filecache::Memo<Arc<crate::bundle::RuntimeManifest>>> = LazyLock::new(|| crate::filecache::Memo::new(8));
    let m = MANIFESTS.get(zip, |p| crate::bundle::read(p).map(Arc::new)).ok()?;
    Some(known_in_manifest(&m, side, sha))
}

#[cfg(not(feature = "zip"))]
fn bundle_known(_zip: &Path, _side: crate::manifest::RunSide, _sha: &str) -> Option<bool> {
    None
}
