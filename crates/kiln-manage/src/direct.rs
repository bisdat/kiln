//! Starting the game client directly instead of through Steam's Play (`steam://rungameid`), per install: a copy of the
//! game in a folder no Steam library knows (an older build kept for mods) cannot be started by Steam, which only ever
//! starts the copy in its own library.
//!
//! A direct start needs no Steam at all: not installed, not running, no library. Kiln starts the exe and nothing else;
//! whether a given copy of the game needs Steam to run is that copy's business.
//!
//! - **Windows**: `<game>\enshrouded.exe`, working directory the game folder. Kiln's proxy is `dinput8.dll` next to the
//!   exe, which Windows loads first: nothing else is set.
//! - **Linux**: through `umu-run` (umu-launcher: Proton outside Steam, with `WINEPREFIX`, `GAMEID=umu-default` and
//!   `PROTONPATH`), a Proton build (`proton run`, with `STEAM_COMPAT_DATA_PATH` and `STEAM_COMPAT_CLIENT_INSTALL_PATH`:
//!   Steam's folder when Steam is installed, else an empty folder of Kiln's own, [`compat_client_dir`]) or a Wine
//!   (`wine`, with `WINEPREFIX`) the player picks; by default ([`default_runner`]) umu-run when it is on PATH, else the
//!   newest Proton found, else Wine. The prefix is Kiln's own per install (`$XDG_DATA_HOME/kiln-prefixes/<installId>`,
//!   outside the data folder: a prefix links `z:` to `/` and holds the saves, so it never moves with the data folder)
//!   unless the player picks one. `WINEDLLOVERRIDES` loads `dinput8` native first, with `xinput9_1_0` when Ultimate ASI
//!   Loader is chained.
//!
//! Kiln never writes or changes `steam_appid.txt`, never sets `SteamAppId`/`SteamGameId` (nor a `GAMEID` from which umu
//! would derive them: `umu-<appid>` makes umu set `SteamAppId`), never touches `steam_api64.dll` or anything else of
//! the game's Steam or ownership checks (docs/kiln/launcher/custom-game-folders.md).
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
    /// `umu-run` (umu-launcher): Proton outside Steam, in its own runtime.
    Umu,
    Proton,
    Wine,
}

/// A umu-run, Proton or Wine the game can run under (Linux).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RunnerInfo {
    pub kind: RunnerKind,
    /// umu: the `umu-run` program; Proton: the folder with the `proton` script; Wine: the `wine` binary.
    pub path: PathBuf,
    /// `umu-run`, the folder name (`GE-Proton10-25`, `Proton 9.0`), or `Wine (<path>)`.
    pub label: String,
    /// `compat` (a compatibility-tools folder: GE-Proton and others), `steam` (Valve's Proton in a Steam library, found
    /// only when Steam is installed), `system` (umu-run or wine on PATH), `custom` (a path the player gave).
    pub source: String,
    /// umu: the Proton folder it runs (`PROTONPATH`, the newest found in a compatibility-tools folder); None: umu's
    /// own (UMU-Proton, which umu downloads at its first start).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub proton: Option<PathBuf>,
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

/// What a direct start looks at on this host (Linux): the home and data folders (Kiln's prefixes, the usual GE-Proton
/// folders) and PATH (umu-run, wine). Tests give fixture folders; [`Host::current`] is the process's.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Host {
    pub home: Option<PathBuf>,
    /// `XDG_DATA_HOME`, when absolute.
    pub xdg_data_home: Option<PathBuf>,
    /// `PATH`.
    pub path: Option<std::ffi::OsString>,
    /// Compatibility-tools folders of the system (`/usr/share/steam/compatibilitytools.d`, where distribution packages
    /// of GE-Proton go).
    pub shared: Vec<PathBuf>,
}

impl Host {
    pub fn current() -> Host {
        let e = crate::paths::DataEnv::from_process();
        Host {
            home: e.home.map(PathBuf::from),
            xdg_data_home: e.xdg_data_home.map(PathBuf::from).filter(|p| p.is_absolute()),
            path: std::env::var_os("PATH"),
            shared: vec![PathBuf::from("/usr/share/steam/compatibilitytools.d")],
        }
    }

    fn env(&self) -> crate::paths::DataEnv {
        crate::paths::DataEnv {
            windows: cfg!(windows),
            home: self.home.clone().map(Into::into),
            xdg_data_home: self.xdg_data_home.clone().map(Into::into),
            ..Default::default()
        }
    }

    /// `$XDG_DATA_HOME`, else `~/.local/share`.
    pub fn data_home(&self) -> Option<PathBuf> {
        self.xdg_data_home.clone().or_else(|| self.home.as_ref().map(|h| h.join(".local/share")))
    }

    /// A program on this host's PATH.
    pub fn which(&self, cmd: &str) -> Option<PathBuf> {
        std::env::split_paths(self.path.as_ref()?).filter(|d| d.is_absolute()).map(|d| d.join(cmd))
            .find(|p| crate::util::is_executable(p))
    }
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

/// The folder a Proton start names in `STEAM_COMPAT_CLIENT_INSTALL_PATH` when no Steam is installed: an empty folder of
/// Kiln's own, `<kiln-prefixes>/_compat-client` (Proton only needs the variable to name a folder; the leading `_` is
/// never an install id). Made at the start ([`spawn`]).
pub fn compat_client_dir(env: &crate::paths::DataEnv) -> Option<PathBuf> {
    prefixes_root(env).map(|r| r.join("_compat-client"))
}

fn looks_like_prefix(p: &Path) -> bool {
    p.join("system.reg").is_file() || p.join("dosdevices").is_dir()
}

/// The compatdata folder (`STEAM_COMPAT_DATA_PATH`) and the Wine prefix (`WINEPREFIX`) for a runner kind, from the
/// player's folder or Kiln's own. A Proton build wants the folder that holds `pfx`; a picked `pfx` itself is taken as
/// its parent's. A Wine (and umu, which is given the prefix itself) wants the prefix; a picked compatdata folder means
/// the `pfx` in it.
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
                    "{} is a Wine prefix: Proton needs the folder that holds its pfx (a compatdata folder). Pick that folder, or run with Wine or umu-run.",
                    c.display()
                )))
            }
        }
        RunnerKind::Wine | RunnerKind::Umu => {
            if looks_like_prefix(&c.join("pfx")) {
                Ok((c.to_path_buf(), c.join("pfx")))
            } else {
                Ok((c.to_path_buf(), c.to_path_buf()))
            }
        }
    }
}

/// The platform an install runs as under a runner kind (saves, path mode and the report follow it).
pub fn platform_of(kind: RunnerKind) -> Platform {
    match kind {
        RunnerKind::Umu | RunnerKind::Proton => Platform::Proton,
        RunnerKind::Wine => Platform::Wine,
    }
}

// ---------------------------------------------------------------- runners (Linux)

/// The program name of umu-launcher's runner.
pub const UMU_RUN: &str = "umu-run";
/// The `GAMEID` a umu start gets: umu's own default (no game-specific fixes). Never `umu-1203620`: umu turns
/// `umu-<id>` into `SteamAppId`/`SteamGameId`, which Kiln never sets.
pub const UMU_GAMEID: &str = "umu-default";

/// What a runner path is: a folder with a `proton` script is a Proton build; a program named `umu-run` is umu; a `wine`
/// binary, or a folder with `bin/wine` or `wine` in it, is a Wine.
pub fn runner_at(path: &Path) -> Result<RunnerInfo> {
    if !path.is_absolute() {
        return Err(Error::invalid(format!("the umu-run, Proton or Wine must be an absolute path, not {}", path.display())));
    }
    let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let info = |kind, path: PathBuf, label: String| RunnerInfo { kind, path, label, source: "custom".into(), proton: None };
    if path.is_dir() {
        if path.join("proton").is_file() {
            return Ok(info(RunnerKind::Proton, path.to_path_buf(), name));
        }
        if crate::util::is_executable(&path.join(UMU_RUN)) {
            return Ok(info(RunnerKind::Umu, path.join(UMU_RUN), UMU_RUN.into()));
        }
        for wine in [path.join("bin/wine"), path.join("wine"), path.join("files/bin/wine")] {
            if crate::util::is_executable(&wine) {
                return Ok(info(RunnerKind::Wine, wine.clone(), format!("Wine ({})", wine.display())));
            }
        }
        return Err(Error::new(ErrorCode::NotFound, format!("{} has no proton script, no umu-run and no wine", path.display())));
    }
    if crate::util::is_executable(path) {
        if name == "proton" && let Some(dir) = path.parent() {
            return runner_at(dir);
        }
        if name == UMU_RUN {
            return Ok(info(RunnerKind::Umu, path.to_path_buf(), UMU_RUN.into()));
        }
        return Ok(info(RunnerKind::Wine, path.to_path_buf(), format!("Wine ({})", path.display())));
    }
    Err(Error::new(ErrorCode::NotFound, format!("{} is not a Proton folder, a umu-run or a wine program", path.display())))
}

fn proton_dirs_in(root: &Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = std::fs::read_dir(root).into_iter().flatten().flatten().map(|e| e.path())
        .filter(|d| d.join("proton").is_file()).collect();
    v.sort();
    v
}

/// The compatibility-tools folders a direct start looks in for GE-Proton and other Proton builds, none of which needs
/// Steam: those of the Steam roots found (when Steam is installed), GE-Proton's usual places (as `wine::ge_roots`:
/// `~/.local/share/Steam/compatibilitytools.d` whether or not Steam is there, where umu puts its downloads too, and the
/// system's), `$XDG_DATA_HOME/Steam/compatibilitytools.d`, and the Proton folders of Heroic and Lutris.
pub fn compat_roots(roots: &[SteamRoot], host: &Host) -> Vec<PathBuf> {
    let mut out: Vec<PathBuf> = roots.iter().map(|r| r.path.join("compatibilitytools.d")).collect();
    if let Some(home) = &host.home {
        for rel in [".local/share/Steam/compatibilitytools.d", ".steam/root/compatibilitytools.d", ".steam/steam/compatibilitytools.d",
                    ".config/heroic/tools/proton"] {
            out.push(home.join(rel));
        }
    }
    out.extend(host.shared.iter().cloned());
    if let Some(data) = host.data_home() {
        out.push(data.join("Steam/compatibilitytools.d"));
        out.push(data.join("lutris/runners/proton"));
    }
    let mut seen: Vec<PathBuf> = Vec::new();
    out.retain(|p| {
        let c = p.canonicalize().unwrap_or_else(|_| p.clone());
        let new = !seen.contains(&c);
        seen.push(c);
        new
    });
    out
}

/// Every umu-run, Proton and Wine found, in the order "automatic" takes them ([`default_runner`]): umu-run on PATH
/// (with the newest compatibility-tools Proton as its `PROTONPATH`), the Proton builds of the compatibility-tools
/// folders (GE-Proton newest first, then the others by name, newest first), Valve's Proton builds in the Steam
/// libraries (only when Steam is installed), then `wine` on PATH.
pub fn runners(roots: &[SteamRoot], host: &Host) -> Vec<RunnerInfo> {
    let mut out: Vec<RunnerInfo> = Vec::new();
    let mut push = |kind, path: PathBuf, label: String, source: &str| {
        let canon = path.canonicalize().unwrap_or(path);
        if !out.iter().any(|r: &RunnerInfo| r.path == canon) {
            out.push(RunnerInfo { kind, path: canon, label, source: source.into(), proton: None });
        }
    };
    let mut compat: Vec<PathBuf> = compat_roots(roots, host).iter().flat_map(|r| proton_dirs_in(r)).collect();
    let name = |p: &Path| p.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    compat.sort_by(|a, b| {
        let (na, nb) = (name(a), name(b));
        crate::wine::ge_version(&nb).cmp(&crate::wine::ge_version(&na)).then(nb.cmp(&na))
    });
    if let Some(u) = host.which(UMU_RUN) {
        push(RunnerKind::Umu, u, UMU_RUN.into(), "system");
    }
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
    if let Some(w) = host.which("wine") {
        let label = format!("Wine ({})", w.display());
        push(RunnerKind::Wine, w, label, "system");
    }
    let proton = umu_proton(&out);
    for r in out.iter_mut().filter(|r| r.kind == RunnerKind::Umu) {
        r.proton = proton.clone();
    }
    out
}

/// The Proton umu runs (`PROTONPATH`): the first compatibility-tools Proton of `all` (GE-Proton newest first), so umu
/// downloads nothing. Valve's builds are left out: they expect Steam's runtime container, which umu replaces with
/// its own. None: umu's own Proton.
pub fn umu_proton(all: &[RunnerInfo]) -> Option<PathBuf> {
    all.iter().find(|r| r.kind == RunnerKind::Proton && r.source == "compat").map(|r| r.path.clone())
}

/// The runner a direct start uses when the player chose none ("automatic"): umu-run when it is on PATH, else the newest
/// Proton found (a compatibility-tools build before Valve's), else a Wine on PATH. Nothing of Steam's config is read.
pub fn default_runner(all: &[RunnerInfo]) -> Option<RunnerInfo> {
    [RunnerKind::Umu, RunnerKind::Proton, RunnerKind::Wine].into_iter().find_map(|k| all.iter().find(|r| r.kind == k).cloned())
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

/// Inherited variables a direct start never passes on (Linux): they would mix another Wine, prefix, umu game or Steam
/// app into this start. `SteamAppId`/`SteamGameId` are Steam's to set when Steam starts a game; Kiln never sets them.
pub const SCRUB: &[&str] = &[
    "WINEPREFIX", "WINESERVER", "WINELOADER", "WINEDLLPATH", "WINEARCH", "WINEDLLOVERRIDES",
    "STEAM_COMPAT_DATA_PATH", "STEAM_COMPAT_CLIENT_INSTALL_PATH", "STEAM_COMPAT_APP_ID", "SteamAppId", "SteamGameId",
    "GAMEID", "UMU_ID", "PROTONPATH", "STORE", "PROTON_VERB", "UMU_NO_PROTON",
    "LD_PRELOAD", "KILN_DISABLE",
];

/// The `WINEDLLOVERRIDES` of a direct start: Kiln's proxy native first, with `xinput9_1_0` when Ultimate ASI Loader is
/// chained; for a plain Wine also `mshtml=` (no Gecko prompt; never `mscoree`: wine.rs).
pub fn dll_overrides(kind: RunnerKind, chained_asi: bool) -> String {
    let proxy = if chained_asi { "dinput8,xinput9_1_0=n,b" } else { "dinput8=n,b" };
    match kind {
        RunnerKind::Umu | RunnerKind::Proton => proxy.to_string(),
        RunnerKind::Wine => format!("mshtml=;{proxy}"),
    }
}

/// Why nothing can start the game (Linux), said the same way everywhere.
pub const NO_RUNNER: &str = "No umu-run, Proton or Wine was found to start the game with: install umu-launcher, GE-Proton or Wine, or pick one.";

/// The command of a direct start, from facts only (no file is read, nothing starts). `windows`: the host's rules;
/// on Linux `runner` and `prefix` (the compatdata folder for Proton, the Wine prefix for Wine and umu) are needed, and
/// for Proton `client`, the folder `STEAM_COMPAT_CLIENT_INSTALL_PATH` names (Steam's, or [`compat_client_dir`]).
pub fn command_for(windows: bool, install: &GameInstall, runner: Option<&RunnerInfo>, prefix: Option<&Path>,
                   client: Option<&Path>, chained_asi: bool) -> Result<DirectCommand> {
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
    let runner = runner.ok_or_else(|| Error::new(ErrorCode::NotFound, NO_RUNNER))?;
    let prefix = prefix.ok_or_else(|| Error::invalid("a direct start under umu-run, Proton or Wine needs a prefix"))?;
    let env_remove = SCRUB.iter().map(|s| s.to_string()).collect();
    let exe_arg = exe.to_string_lossy().into_owned();
    let overrides = ("WINEDLLOVERRIDES".to_string(), dll_overrides(runner.kind, chained_asi));
    Ok(match runner.kind {
        RunnerKind::Umu => {
            let mut env = vec![
                ("WINEPREFIX".into(), prefix.display().to_string()),
                ("GAMEID".into(), UMU_GAMEID.into()),
            ];
            if let Some(p) = &runner.proton {
                env.push(("PROTONPATH".into(), p.display().to_string()));
            }
            env.push(overrides);
            DirectCommand {
                program: runner.path.clone(), args: vec![exe_arg], cwd: install.path.clone(), env, env_remove,
                runner: Some(runner.clone()), prefix: Some(prefix.to_path_buf()),
            }
        }
        RunnerKind::Proton => {
            let client = client.ok_or_else(|| Error::new(ErrorCode::NotFound,
                "Proton needs a folder for STEAM_COMPAT_CLIENT_INSTALL_PATH and none was given: HOME is not set"))?;
            DirectCommand {
                program: runner.path.join("proton"),
                args: vec!["run".into(), exe_arg],
                cwd: install.path.clone(),
                env: vec![
                    ("STEAM_COMPAT_DATA_PATH".into(), prefix.display().to_string()),
                    ("STEAM_COMPAT_CLIENT_INSTALL_PATH".into(), client.display().to_string()),
                    overrides,
                ],
                env_remove,
                runner: Some(runner.clone()),
                prefix: Some(prefix.join("pfx")),
            }
        }
        RunnerKind::Wine => DirectCommand {
            program: runner.path.clone(),
            args: vec![exe_arg],
            cwd: install.path.clone(),
            env: vec![("WINEPREFIX".into(), prefix.display().to_string()), overrides],
            env_remove,
            runner: Some(runner.clone()),
            prefix: Some(prefix.to_path_buf()),
        },
    })
}

/// The runner of an install's settings (Linux): the player's path, else [`default_runner`]. A umu-run gets the Proton
/// it runs (`all`'s newest compatibility-tools build).
pub fn resolve_runner(settings: &StartSettings, all: &[RunnerInfo]) -> Result<RunnerInfo> {
    match &settings.runner {
        Some(p) => {
            let mut r = runner_at(p)?;
            if let Some(found) = all.iter().find(|x| x.path == r.path) {
                r = found.clone();
            } else if r.kind == RunnerKind::Umu {
                r.proton = umu_proton(all);
            }
            Ok(r)
        }
        None => default_runner(all).ok_or_else(|| Error::new(ErrorCode::NotFound, NO_RUNNER)),
    }
}

/// The Steam folder Proton is told about when Steam is installed: the root of the first Steam install found (a
/// native one first). None without Steam ([`compat_client_dir`] then).
pub fn steam_root_for(roots: &[SteamRoot]) -> Option<PathBuf> {
    roots.iter().find(|r| r.flavor == crate::steam::SteamFlavor::Native).or(roots.first()).map(|r| r.path.clone())
}

/// The command of a direct start of `install` on this host with its settings.
pub fn plan(install: &GameInstall, settings: &StartSettings, roots: &[SteamRoot]) -> Result<DirectCommand> {
    plan_on(install, settings, roots, &Host::current())
}

/// [`plan`] on a given host (tests: fixture home, data folder and PATH; never the process's).
pub fn plan_on(install: &GameInstall, settings: &StartSettings, roots: &[SteamRoot], host: &Host) -> Result<DirectCommand> {
    let chained = crate::game::asi_loader_chained(install);
    if cfg!(windows) {
        return command_for(true, install, None, None, None, chained);
    }
    let runner = resolve_runner(settings, &runners(roots, host))?;
    let env = host.env();
    let own = own_prefix(&env, &install.id)
        .ok_or_else(|| Error::new(ErrorCode::NotFound, "HOME is not set: Kiln has no place for the game's prefix"))?;
    let (compat, pfx) = prefix_paths(runner.kind, settings.prefix.as_deref(), &own)?;
    let target = if runner.kind == RunnerKind::Proton { compat } else { pfx };
    let client = steam_root_for(roots).or_else(|| compat_client_dir(&env));
    command_for(false, install, Some(&runner), Some(&target), client.as_deref(), chained)
}

/// The Wine prefix a direct start of `install` uses on this host (None on Windows): what the install's `prefix` is
/// while it starts directly (saves, path mode). With no runner found it is the prefix a Proton or umu start would use.
pub fn prefix_of(install: &GameInstall, settings: &StartSettings, roots: &[SteamRoot]) -> Option<(Platform, PathBuf)> {
    prefix_on(install, settings, roots, &Host::current())
}

/// [`prefix_of`] on a given host.
pub fn prefix_on(install: &GameInstall, settings: &StartSettings, roots: &[SteamRoot], host: &Host) -> Option<(Platform, PathBuf)> {
    if cfg!(windows) {
        return None;
    }
    let kind = match &settings.runner {
        Some(p) => runner_at(p).ok()?.kind,
        None => default_runner(&runners(roots, host)).map(|r| r.kind).unwrap_or(RunnerKind::Proton),
    };
    let own = own_prefix(&host.env(), &install.id)?;
    let (_, pfx) = prefix_paths(kind, settings.prefix.as_deref(), &own).ok()?;
    Some((platform_of(kind), pfx))
}

/// Starts the command, detached: the game outlives the launcher. Linux: its own session, no AppImage environment
/// (`hostenv`), output into `log` (the umu, Proton or Wine output; None: discarded); the prefix's parent and the
/// folder `STEAM_COMPAT_CLIENT_INSTALL_PATH` names are made first. Windows: out of the launcher's job when it may.
/// The child is reaped on a thread. Returns the pid of the started program (umu-run, Proton's script or wine on Linux,
/// not the game's: the watcher finds the game by its folder).
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
        // Proton makes `pfx` itself, in a compatdata folder that exists; Wine and umu make the prefix in its parent
        std::fs::create_dir_all(parent)?;
    }
    if let Some((_, client)) = cmd.env.iter().find(|(k, _)| k == "STEAM_COMPAT_CLIENT_INSTALL_PATH") {
        // Kiln's own empty folder when Steam is not installed (Steam's exists)
        std::fs::create_dir_all(client)?;
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
    /// The launcher runs on Windows (no runner or prefix to pick).
    pub windows: bool,
    /// A Steam install was found on this PC: without one the launcher does not offer "through Steam" for an install
    /// that starts directly. Nothing of a direct start depends on it.
    pub steam_found: bool,
    /// Linux: every umu-run, Proton and Wine found ([`runners`]), in the order "automatic" takes them.
    pub runners: Vec<RunnerInfo>,
    /// Linux: the runner a direct start uses now (the player's, else [`default_runner`]); None when there is none.
    pub runner: Option<RunnerInfo>,
    /// Linux: the runner "automatic" means ([`default_runner`]: umu-run, else the newest Proton, else Wine).
    pub default_runner: Option<RunnerInfo>,
    /// Why there is no runner, or why the player's choice cannot be used.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub problem: Option<String>,
    /// Linux: Kiln's own prefix (compatdata) for this install.
    pub own_prefix: Option<PathBuf>,
    /// The command a direct start runs now (None when it cannot be built: `problem`).
    pub command: Option<DirectCommand>,
}

/// [`StartSetup`] of an install.
pub fn setup(dirs: &crate::paths::DataDirs, install: &GameInstall, roots: &[SteamRoot]) -> StartSetup {
    setup_on(dirs, install, roots, &Host::current())
}

/// [`setup`] on a given host.
pub fn setup_on(dirs: &crate::paths::DataDirs, install: &GameInstall, roots: &[SteamRoot], host: &Host) -> StartSetup {
    let settings = settings(dirs, install);
    let windows = cfg!(windows);
    let all = if windows { Vec::new() } else { runners(roots, host) };
    let auto = if windows { None } else { default_runner(&all) };
    let (runner, mut problem) = if windows {
        (None, None)
    } else {
        match resolve_runner(&settings, &all) {
            Ok(r) => (Some(r), None),
            Err(e) => (None, Some(e.message)),
        }
    };
    let command = match plan_on(install, &settings, roots, host) {
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
        steam_found: !roots.is_empty(),
        runners: all,
        runner,
        default_runner: auto,
        problem,
        own_prefix: own_prefix(&host.env(), &install.id),
        command,
        settings,
    }
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
