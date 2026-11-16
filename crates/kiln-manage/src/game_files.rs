// SPDX-License-Identifier: MIT
//! Native mods whose files must be in the game folder itself (docs/kiln/engines.md, "Files in the game folder").
//!
//! Two kinds of package only work from there:
//!
//! - **a plugin loader** under a proxy name the game loads itself (`powrprof.dll`, which enshrouded.exe imports, with
//!   the plugins it loads from `plugins\` beside the exe: "Autoloot and coop loot sharing", Nexus 266). Loaded by Kiln
//!   from its Library folder it finds no plugins;
//! - **Ultimate ASI Loader** (UAL) with `.asi` plugins (Smart Workshop, Nexus 361), or `.asi` plugins alone. UAL runs
//!   only under a name it knows and looks for the plugins next to itself, in `scripts\` and `plugins\`. Its own
//!   download is a `dinput8.dll`, which would replace Kiln's proxy.
//!
//! The Library keeps such a package as a native mod ([`import_asi`] makes one for an `.asi` package). [`plan`] tells
//! it by its files, so an entry an older launcher imported counts as well. Activation ([`place`]) copies its files into
//! the game folder, and kiln-core does not load it (profile::render leaves it out of `kiln/profile.json`): the game
//! loads the loader, Kiln's proxy loads UAL. UAL goes next to the game as `xinput9_1_0.dll`, the name Kiln's installer
//! chains it under, and `kiln/chain.ini` `[profile]` names it ([`ual_chain_entry`]); a UAL Kiln's installer chained
//! already, or one the player put there under that name, is used as it is.
//!
//! Ownership goes file by file, in [`RECEIPTS`]. Kiln writes a file only where there is none or where it still holds
//! the bytes Kiln wrote, and removes only such files ([`place`] for a mod that is off or gone, [`unplace`] when the
//! profile is deactivated or Kiln uninstalled). A code file (`.dll`, `.asi`) that is there already and that Kiln did not
//! place (another loader's `powrprof.dll`, a player's own copy) keeps the whole package out, with a sentence that says
//! so. A settings file the player or the mod changed stays where it is, also when the mod goes.

use crate::engine_mods::RetainedPlacement;
use crate::library::{Detected, DetectKind, ImportRecord, LibraryRef};
use crate::manifest::{Clients, ModKind, Side};
use crate::paths::DataDirs;
use crate::util::io_at;
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// Kiln's receipts for the files it placed in the game folder.
pub const RECEIPTS: &str = "kiln/state/game-files.json";
/// The name Ultimate ASI Loader runs under next to Kiln (install::ASI_LOADER_NAME): the game does not import it, so
/// only Kiln's proxy loads it.
pub const UAL_FILE: &str = crate::install::ASI_LOADER_NAME;
/// [`UAL_FILE`] as `kiln/chain.ini` names it (relative to `<game>/kiln/`).
pub const UAL_CHAIN_LINE: &str = "..\\xinput9_1_0.dll";
/// Where an `.asi` plugin that sits at the root of its package goes: UAL reads `scripts\` (and its own folder).
const ASI_FOLDER: &str = "scripts";
/// Kiln owns `dinput8.dll`; `steam_api64.dll` is the game's own Steam DLL; `xinput9_1_0.dll` is UAL's slot.
const NOT_FOR_LOADERS: [&str; 3] = ["dinput8.dll", "steam_api64.dll", UAL_FILE];
/// A package Kiln reads for its files: deeper or larger is not a package of this kind.
const MAX_FILES: usize = 4096;
const MAX_DEPTH: usize = 8;

/// What a package is ([`plan`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Shape {
    /// A loader DLL under a proxy name the game loads from its own folder, with the plugins it loads beside it.
    Loader,
    /// Ultimate ASI Loader and/or `.asi` plugins.
    Asi,
}

/// The files of a package as they go into the game folder.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GamePlan {
    pub shape: Shape,
    /// (file in the Library entry, file in the game folder), '/'-separated and relative. UAL is not among them.
    pub files: Vec<(String, String)>,
    /// The Library file that is Ultimate ASI Loader, when the package brings one (it goes to [`UAL_FILE`]).
    pub ual: Option<String>,
    /// The loader DLL of a [`Shape::Loader`] package (`powrprof.dll`).
    pub loader: Option<String>,
}

fn ext(rel: &str) -> String {
    rel.rsplit('/').next().and_then(|n| n.rsplit_once('.')).map(|(_, e)| e.to_ascii_lowercase()).unwrap_or_default()
}

/// Code: what a loader runs. Such a file Kiln did not place is never replaced, and one that changed is never removed.
fn is_code(rel: &str) -> bool {
    matches!(ext(rel).as_str(), "dll" | "asi" | "exe")
}

/// A document or an installer script of the package: it stays in the Library. A `.txt` other than a readme may be data
/// (a loot list), so it goes along.
fn is_document(rel: &str) -> bool {
    let name = rel.rsplit('/').next().unwrap_or(rel).to_ascii_lowercase();
    matches!(ext(&name).as_str(), "md" | "pdf" | "url" | "lnk" | "html" | "htm" | "rtf" | "nfo" | "bat" | "cmd" | "ps1" | "sh")
        || ["readme", "license", "licence", "changelog", "credits", "notice", "copying", "third_party"].iter().any(|w| name.starts_with(w))
}

/// Every regular file below `root`, relative and '/'-separated, sorted; None for a link or a tree past the limits.
fn files_of(root: &Path) -> Option<Vec<String>> {
    fn walk(dir: &Path, rel: &str, depth: usize, out: &mut Vec<String>) -> Option<()> {
        if depth > MAX_DEPTH { return None; }
        for e in std::fs::read_dir(dir).ok()? {
            let e = e.ok()?;
            let meta = std::fs::symlink_metadata(e.path()).ok()?;
            let name = e.file_name().to_string_lossy().into_owned();
            let r = if rel.is_empty() { name.clone() } else { format!("{rel}/{name}") };
            if meta.file_type().is_symlink() { return None; }
            if meta.is_dir() {
                walk(&e.path(), &r, depth + 1, out)?;
            } else if meta.is_file() {
                out.push(r);
            }
            if out.len() > MAX_FILES { return None; }
        }
        Some(())
    }
    let mut out = Vec::new();
    walk(root, "", 0, &mut out)?;
    out.sort();
    Some(out)
}

/// Kiln's own files in a Library entry, never the mod's.
fn is_kiln_file(rel: &str) -> bool {
    rel.eq_ignore_ascii_case(crate::manifest::NATIVE_MANIFEST) || rel.rsplit('/').next().is_some_and(|n| n.starts_with('.'))
}

fn is_ual_at(path: &Path) -> bool {
    crate::pe::classify_proxy(path).is_ok_and(|c| c.kind == crate::pe::ProxyKind::UltimateAsiLoader)
}

/// A DLL directly in the folder that is Ultimate ASI Loader (any file name: its downloads call it `dinput8.dll`).
fn ual_in(folder: &Path, files: &[String]) -> Option<String> {
    files.iter().filter(|f| !f.contains('/') && ext(f) == "dll").find(|f| is_ual_at(&folder.join(f))).cloned()
}

/// The game-folder plan of the native Library entry `folder`, None for a native mod Kiln loads from its folder as
/// before. By its files (an older launcher's entry counts too):
///
/// - [`Shape::Asi`]: `.asi` plugins its native.json lists (`asi`, written by [`import_asi`]), or Ultimate ASI Loader
///   directly in it. UAL goes to [`UAL_FILE`]; the
///   files in a folder keep their place (`Scripts/SmartWorkshop.asi`), the other files at the top go to `scripts/` beside
///   the plugins; another proxy-named DLL at the top is left out (it is not UAL, and the slot is not its).
/// - [`Shape::Loader`]: its one DLL (native.json, no `init`) sits at the top under a proxy name other than Kiln's
///   `dinput8.dll`, the game's `steam_api64.dll` and UAL's `xinput9_1_0.dll`, it is no engine's own loader that Kiln runs
///   the mods of (SET's `powrprof.dll`), and a folder beside it holds a DLL (its plugins). Every file keeps its place.
///
/// Documents and installer scripts stay in the Library ([`is_document`]), and so do Kiln's own files.
pub fn plan(folder: &Path) -> Option<GamePlan> {
    let n = crate::manifest::read_native(folder).ok()?;
    let files: Vec<String> = files_of(folder)?.into_iter().filter(|f| !is_kiln_file(f)).collect();
    let keep = |f: &String| !is_document(f) && ext(f) != "exe";
    let ual = ual_in(folder, &files);
    // (an .asi package is the Library's own import, its native.json lists the plugins: a native DLL mod that also ships
    // an .asi build of itself stays a native mod Kiln loads)
    if ual.is_some() || (!n.asi.is_empty() && files.iter().any(|f| ext(f) == "asi")) {
        let mut out = Vec::new();
        for f in files.iter().filter(|f| keep(f) && Some(*f) != ual.as_ref()) {
            if f.contains('/') {
                out.push((f.clone(), f.clone()));
            } else if !(ext(f) == "dll" && crate::engines::is_proxy_name(f)) {
                out.push((f.clone(), format!("{ASI_FOLDER}/{f}")));
            }
        }
        return Some(GamePlan { shape: Shape::Asi, files: out, ual, loader: None });
    }
    let [dll] = n.dlls.as_slice() else { return None };
    if n.init.is_some() || dll.contains('/') || !crate::engines::is_proxy_name(dll) || NOT_FOR_LOADERS.iter().any(|x| x.eq_ignore_ascii_case(dll)) {
        return None;
    }
    if crate::engines::own_loader_in(folder).is_some() || !files.iter().any(|f| f.contains('/') && ext(f) == "dll") {
        return None;
    }
    let files = files.into_iter().filter(keep).map(|f| (f.clone(), f)).collect();
    Some(GamePlan { shape: Shape::Loader, files, ual: None, loader: Some(dll.clone()) })
}

/// The game-folder plan of a profile's native entry, from its Library folder.
pub fn plan_of(dirs: &DataDirs, m: &crate::profile::ProfileMod) -> Option<GamePlan> {
    let r = LibraryRef { kind: ModKind::Native, id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone() };
    plan(&crate::library::path_of(dirs, &r).ok()?)
}

/// Does this native entry run from the game folder (Kiln places it; kiln-core does not load it)?
pub fn runs_from_game_folder(dirs: &DataDirs, m: &crate::profile::ProfileMod) -> bool {
    plan_of(dirs, m).is_some()
}

/// One sentence for the launcher's native plan (profile::resolve_preview) about such an entry.
pub fn preview_reason(plan: &GamePlan) -> String {
    match plan.shape {
        Shape::Loader => format!("placed in the game folder at activation: the game loads {} itself, not Kiln",
            plan.loader.as_deref().unwrap_or("its loader")),
        Shape::Asi => format!("placed in the game folder at activation: Ultimate ASI Loader ({UAL_FILE}, chained by Kiln's proxy) loads its .asi plugins"),
    }
}

// ---- receipts ----

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct Receipts {
    format: u32,
    #[serde(default)]
    mods: Vec<Placed>,
    /// Ultimate ASI Loader as [`UAL_FILE`], when Kiln placed it (from the package `from`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    ual: Option<PlacedUal>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct Placed {
    id: String,
    version: String,
    hash: String,
    shape: Shape,
    /// Game-relative file ('/'-separated) -> the sha256 Kiln wrote.
    files: BTreeMap<String, String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct PlacedUal {
    from: String,
    sha256: String,
}

fn receipts_path(game: &Path) -> PathBuf {
    RECEIPTS.split('/').fold(game.to_path_buf(), |p, c| p.join(c))
}

fn read_receipts(game: &Path) -> Result<Receipts> {
    let file = receipts_path(game);
    crate::engine_mods::refuse_symlink_path(game, &file)?;
    Ok(crate::util::read_json::<Receipts>(&file)?.unwrap_or_default())
}

fn save_receipts(game: &Path, r: &Receipts) -> Result<()> {
    let file = receipts_path(game);
    crate::engine_mods::refuse_symlink_path(game, &file)?;
    if r.mods.is_empty() && r.ual.is_none() {
        return match std::fs::remove_file(&file) {
            Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(io_at(&file, "removing")(e)),
            _ => Ok(()),
        };
    }
    crate::util::write_json_atomic(&file, &Receipts { format: 1, ..r.clone() })
}

/// sha256 of a regular file; None when there is none.
fn sha_of(path: &Path) -> Result<Option<String>> {
    match std::fs::symlink_metadata(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(io_at(path, "inspecting")(e)),
        Ok(m) if m.is_file() => Ok(Some(crate::hash::sha256_file(path)?.0)),
        Ok(_) => Err(Error::new(ErrorCode::Refused, format!("{}: not a regular file", path.display()))),
    }
}

/// `rel` below `game` with each component as the game folder already spells it (case ignored), so a package's
/// `Scripts/` lands in a `scripts/` that is there, as Windows would have it; the rest as `rel` spells it.
fn spelled(game: &Path, rel: &str) -> String {
    let mut at = game.to_path_buf();
    let mut out = Vec::new();
    for part in rel.split('/').filter(|p| !p.is_empty()) {
        let name = if at.join(part).symlink_metadata().is_ok() { part.to_string() } else {
            std::fs::read_dir(&at).into_iter().flatten().flatten().map(|e| e.file_name().to_string_lossy().into_owned())
                .find(|n| n.eq_ignore_ascii_case(part)).unwrap_or_else(|| part.to_string())
        };
        at.push(&name);
        out.push(name);
    }
    out.join("/")
}

fn host(game: &Path, rel: &str) -> PathBuf {
    rel.split('/').fold(game.to_path_buf(), |p, c| p.join(c))
}

/// Removes the folders between `path` and the game folder that are empty now (the ones Kiln made for a package).
fn remove_empty_parents(game: &Path, path: &Path) {
    for d in path.ancestors().skip(1) {
        if d == game || !d.starts_with(game) || std::fs::remove_dir(d).is_err() { break; }
    }
}

/// What [`place`] did.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GameFilesReport {
    /// `native:<id>` whose files Kiln wrote (new, or another version).
    pub placed: Vec<String>,
    /// `native:<id>` already in place.
    pub kept: Vec<String>,
    /// `native:<id>` whose files Kiln removed (off, gone from the profile).
    pub removed: Vec<String>,
    /// One sentence each: what Kiln placed or removed, and what it could not do and why (`native:<id>: ...`).
    pub notes: Vec<String>,
    /// Code files Kiln placed that changed since and therefore stay where they are: they may still run.
    pub retained: Vec<RetainedPlacement>,
}

fn tag(id: &str) -> String {
    format!("native:{id}")
}

fn names(files: &[String]) -> String {
    let mut v: Vec<&str> = files.iter().map(String::as_str).take(4).collect();
    if files.len() > 4 { v.push("..."); }
    v.join(", ")
}

/// Takes Kiln's files of `p` out of the game folder, except `keep` (game-relative paths a new version places again).
/// A file that still holds Kiln's bytes goes; a changed settings file stays (the player's now); a changed code file
/// stays and keeps its receipt line, as a [`RetainedPlacement`]. Returns what is left of the receipt.
fn take_out(game: &Path, p: &Placed, keep: &[String], rep: &mut GameFilesReport, off: bool) -> Result<BTreeMap<String, String>> {
    let mut left = BTreeMap::new();
    let mut kept_settings = Vec::new();
    for (rel, sha) in &p.files {
        if keep.iter().any(|k| k.eq_ignore_ascii_case(rel)) { continue; }
        crate::bundle::safe_rel_path(rel)?;
        let path = host(game, rel);
        crate::engine_mods::refuse_symlink_path(game, &path)?;
        match sha_of(&path)? {
            None => {}
            Some(have) if have == *sha => {
                std::fs::remove_file(&path).map_err(io_at(&path, "removing"))?;
                remove_empty_parents(game, &path);
            }
            Some(_) if is_code(rel) => {
                left.insert(rel.clone(), sha.clone());
                rep.retained.push(RetainedPlacement { path, may_run: true, off, reason: format!(
                    "{}: changed since Kiln placed it in the game folder; Kiln neither replaces nor removes it", tag(&p.id)) });
            }
            Some(_) => kept_settings.push(rel.clone()),
        }
    }
    if !kept_settings.is_empty() {
        rep.notes.push(format!("{}: {} changed since Kiln placed it and stay in the game folder (your settings): {}", tag(&p.id),
            if kept_settings.len() == 1 { "this file" } else { "these files" }, names(&kept_settings)));
    }
    Ok(left)
}

/// Where Ultimate ASI Loader comes from for this activation.
enum Ual {
    /// A UAL Kiln did not place is at [`UAL_FILE`] (Kiln's installer chained it, or the player put it there).
    There,
    /// Kiln's copy (already current, or written now).
    Kilns,
    /// None, with the reason.
    Missing(String),
}

/// Places the files of the profile's enabled native mods that run from the game folder ([`plan`]) and takes out the
/// ones of Kiln's earlier placements that the profile no longer has on. Called by profile activation before chain.ini
/// is written (that names UAL, [`ual_chain_entry`]). Writes no state when there is nothing to place and nothing placed.
pub fn place(dirs: &DataDirs, profile: &crate::profile::Profile, game: &Path) -> Result<GameFilesReport> {
    let mut rep = GameFilesReport::default();
    let wanted: Vec<(&crate::profile::ProfileMod, PathBuf, GamePlan)> = profile.mods.native.iter().filter(|m| m.enabled)
        .filter_map(|m| {
            let r = LibraryRef { kind: ModKind::Native, id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone() };
            let folder = crate::library::path_of(dirs, &r).ok()?;
            let plan = plan(&folder)?;
            Some((m, folder, plan))
        }).collect();
    let file = receipts_path(game);
    if wanted.is_empty() && std::fs::symlink_metadata(&file).is_err() {
        return Ok(rep);
    }
    let mut rc = read_receipts(game)?;

    // Ultimate ASI Loader for the .asi packages
    let need_ual = wanted.iter().any(|(_, _, p)| p.shape == Shape::Asi);
    let ual_rel = spelled(game, UAL_FILE);
    let ual_path = host(game, &ual_rel);
    crate::engine_mods::refuse_symlink_path(game, &ual_path)?;
    let at_slot = sha_of(&ual_path)?;
    let ours = rc.ual.as_ref().is_some_and(|u| at_slot.as_deref() == Some(u.sha256.as_str()));
    let brought = wanted.iter().find_map(|(m, folder, p)| p.ual.as_ref().map(|u| (m.id.clone(), folder.join(u))));
    let ual = if !need_ual {
        Ual::Missing(String::new())
    } else {
        match (&at_slot, ours, &brought) {
            (Some(_), false, _) if is_ual_at(&ual_path) => Ual::There,
            (Some(_), false, _) => Ual::Missing(format!("{UAL_FILE} next to the game is not Ultimate ASI Loader and Kiln did not place it \
                ({}); Kiln leaves it as it is", crate::pe::classify_proxy(&ual_path).ok().and_then(|c| c.detail).unwrap_or_else(|| "another DLL".into()))),
            (_, _, Some((from, src))) => {
                let bytes = std::fs::read(src).map_err(io_at(src, "reading"))?;
                let sha = crate::hash::sha256_hex(&bytes);
                if at_slot.as_deref() != Some(sha.as_str()) {
                    crate::util::write_atomic_exclusive(&ual_path, &bytes)?;
                    rep.notes.push(format!("{}: Kiln placed its Ultimate ASI Loader next to the game as {UAL_FILE} (Kiln's own \
                        dinput8.dll stays); Kiln's proxy loads it from kiln/chain.ini, and Kiln removes it again when no mod of the \
                        profile needs it. Under Proton it needs the Steam launch option {}", tag(from), crate::game::CHAINED_DLL_OVERRIDE_ADVICE));
                }
                rc.ual = Some(PlacedUal { from: from.clone(), sha256: sha });
                save_receipts(game, &rc)?;
                Ual::Kilns
            }
            (Some(_), true, None) => Ual::Missing("no package of this profile that is on brings Ultimate ASI Loader any more".into()),
            (None, _, None) => Ual::Missing(String::new()),
        }
    };
    // Kiln's UAL that no package of the profile brings now goes (when it is unchanged)
    if !matches!(ual, Ual::Kilns) && let Some(u) = rc.ual.clone() {
        if ours {
            std::fs::remove_file(&ual_path).map_err(io_at(&ual_path, "removing"))?;
        } else if at_slot.is_some() {
            rep.retained.push(RetainedPlacement { path: ual_path.clone(), may_run: true, off: true, reason: format!(
                "Ultimate ASI Loader ({UAL_FILE}) changed since Kiln placed it for {}; Kiln neither replaces nor removes it", tag(&u.from)) });
        }
        rc.ual = None;
        save_receipts(game, &rc)?;
    }
    let ual_ok = matches!(ual, Ual::There | Ual::Kilns);
    let placeable = |p: &GamePlan| p.shape == Shape::Loader || ual_ok;
    for (m, _, p) in wanted.iter().filter(|(_, _, p)| !placeable(p)) {
        let why = match &ual { Ual::Missing(w) if !w.is_empty() => format!(" ({w})"), _ => String::new() };
        rep.notes.push(format!("{}: its .asi plugins need Ultimate ASI Loader, which is not in this game{why}: add Ultimate ASI Loader's \
            download (its dinput8.dll) to the Library and switch it on in this profile, then activate the profile again. \
            Kiln placed none of its {} file(s)", tag(&m.id), p.files.len()));
    }

    // 1. Kiln's placements the profile does not want (off, gone, not placeable now); another version's files a new
    // version does not place again
    let current: Vec<(&crate::profile::ProfileMod, &PathBuf, &GamePlan)> = wanted.iter().filter(|(_, _, p)| placeable(p)).map(|(m, f, p)| (*m, f, p)).collect();
    let mut next: Vec<Placed> = Vec::new();
    for old in std::mem::take(&mut rc.mods) {
        let new = current.iter().find(|(m, _, _)| m.id == old.id);
        let keep: Vec<String> = new.map(|(_, _, p)| p.files.iter().map(|(_, g)| spelled(game, g)).collect()).unwrap_or_default();
        // (an error stops here with the receipts file as it was: a file it names that is gone is skipped next time)
        let mut files = take_out(game, &old, &keep, &mut rep, new.is_none())?;
        if new.is_none() {
            rep.removed.push(tag(&old.id));
        }
        // what the new version places again stays in its receipt until it is written over
        for (rel, sha) in old.files.iter().filter(|(r, _)| keep.iter().any(|k| k.eq_ignore_ascii_case(r))) {
            files.insert(rel.clone(), sha.clone());
        }
        if !files.is_empty() || new.is_some() {
            next.push(Placed { files, ..old });
        }
    }
    rc.mods = next;
    save_receipts(game, &rc)?;

    // 2. the profile's packages
    for (m, folder, p) in current {
        let t = tag(&m.id);
        // this version was placed before: what is said about it was said then
        let had = rc.mods.iter().any(|x| x.id == m.id && x.version == m.version && x.hash == m.hash);
        let prev: BTreeMap<String, String> = rc.mods.iter().filter(|x| x.id == m.id).flat_map(|x| x.files.clone()).collect();
        let ours_now = |rel: &str, have: &str| prev.iter().any(|(r, s)| r.eq_ignore_ascii_case(rel) && s == have);
        let mut write: Vec<(String, Vec<u8>)> = Vec::new();
        let mut owned: BTreeMap<String, String> = BTreeMap::new();
        let mut blocked = None;
        let mut theirs = Vec::new();
        let mut same = Vec::new();
        for (src, rel) in &p.files {
            let rel = spelled(game, rel);
            let dst = host(game, &rel);
            if let Err(e) = crate::bundle::safe_rel_path(&rel).and_then(|_| crate::engine_mods::refuse_symlink_path(game, &dst)) {
                blocked = Some(format!("{rel}: {}", e.message));
                break;
            }
            let from = folder.join(src.replace('/', std::path::MAIN_SEPARATOR_STR));
            let bytes = std::fs::read(&from).map_err(io_at(&from, "reading"))?;
            let sha = crate::hash::sha256_hex(&bytes);
            match sha_of(&dst)? {
                None => { write.push((rel.clone(), bytes)); owned.insert(rel, sha); }
                Some(have) if ours_now(&rel, &have) => {
                    if have != sha { write.push((rel.clone(), bytes)); }
                    owned.insert(rel, sha);
                }
                // the same bytes, put there by someone else (the package installed by hand): they serve, and stay theirs
                Some(have) if have == sha => { if is_code(&rel) { same.push(rel); } }
                Some(_) if is_code(&rel) => {
                    let what = crate::pe::classify_proxy(&dst).ok().and_then(|c| c.detail).filter(|_| ext(&rel) == "dll")
                        .map(|d| format!(" ({d})")).unwrap_or_default();
                    blocked = Some(format!("{rel} is in the game folder already{what} and Kiln did not place it; Kiln leaves it as it is"));
                    break;
                }
                Some(_) => theirs.push(rel),
            }
        }
        if let Some(why) = blocked {
            rep.notes.push(format!("{t}: not placed in the game folder: {why}, so none of this mod's files are placed. Remove that file \
                (or the other mod it belongs to) to use this one"));
            continue;
        }
        let changed = !write.is_empty();
        for (i, (rel, bytes)) in write.iter().enumerate() {
            if let Err(e) = crate::util::write_atomic_exclusive(&host(game, rel), bytes) {
                // what is written so far is Kiln's: the receipt says so before the error goes up
                let mut files = prev.clone();
                files.extend(write[..i].iter().map(|(r, _)| (r.clone(), owned[r].clone())));
                rc.mods.retain(|x| x.id != m.id);
                rc.mods.push(Placed { id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone(), shape: p.shape, files });
                let _ = save_receipts(game, &rc);
                return Err(e);
            }
        }
        // (a file of the earlier receipt that is not Kiln's now, as the player changed it, is the player's from here on;
        // the earlier version's other files went in step 1)
        rc.mods.retain(|x| x.id != m.id);
        rc.mods.push(Placed { id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone(), shape: p.shape, files: owned });
        save_receipts(game, &rc)?;
        if !theirs.is_empty() && (changed || !had) {
            rep.notes.push(format!("{t}: {} in the game folder already and differ from the package's; Kiln leaves them as they are \
                (your settings): {}", if theirs.len() == 1 { "this file is" } else { "these files are" }, names(&theirs)));
        }
        if !same.is_empty() && (changed || !had) {
            rep.notes.push(format!("{t}: {} in the game folder already with the same content, put there by hand: {} yours, \
                so Kiln does not remove {} when the mod is off ({})", if same.len() == 1 { "this file is" } else { "these files are" },
                if same.len() == 1 { "it stays" } else { "they stay" }, if same.len() == 1 { "it" } else { "them" }, names(&same)));
        }
        if changed {
            let written: Vec<String> = write.iter().map(|(rel, _)| rel.clone()).collect();
            rep.placed.push(t.clone());
            rep.notes.push(match p.shape {
                Shape::Loader => {
                    let loader = p.loader.as_deref().unwrap_or("its loader");
                    let stem = loader.strip_suffix(".dll").or_else(|| loader.strip_suffix(".DLL")).unwrap_or(loader).to_ascii_lowercase();
                    format!("{t}: Kiln placed {} file(s) of it in the game folder ({}): the game loads {loader} itself; Kiln removes \
                        them again when the mod is off. Under Wine or Proton the game takes it only with a DLL override (Steam launch \
                        option WINEDLLOVERRIDES=\"dinput8,{stem}=n,b\" %command%)", written.len(), names(&written))
                }
                Shape::Asi => format!("{t}: Kiln placed {} file(s) of it in the game folder ({}), for Ultimate ASI Loader ({UAL_FILE}, \
                    chained by Kiln's proxy); Kiln removes them again when the mod is off", written.len(), names(&written)),
            });
        } else {
            rep.kept.push(t);
        }
    }
    Ok(rep)
}

/// Takes every file Kiln placed in the game folder out again (profile deactivated, Kiln uninstalled), as [`place`]
/// does for a mod that is off: Kiln's unchanged files go, changed code files stay as retained placements, changed
/// settings files stay. Writes nothing in a game Kiln placed nothing in.
pub fn unplace(game: &Path) -> Result<crate::engine_mods::PlacementCleanup> {
    let mut out = crate::engine_mods::PlacementCleanup::default();
    if std::fs::symlink_metadata(receipts_path(game)).is_err() {
        return Ok(out);
    }
    let mut rc = read_receipts(game)?;
    let mut rep = GameFilesReport::default();
    let mut left = Vec::new();
    for p in rc.mods.clone() {
        let before: Vec<PathBuf> = p.files.keys().map(|r| host(game, r)).collect();
        let files = take_out(game, &p, &[], &mut rep, true)?;
        out.removed.extend(before.into_iter().filter(|f| !f.exists()));
        if !files.is_empty() { left.push(Placed { files, ..p }); }
    }
    rc.mods = left;
    if let Some(u) = rc.ual.take() {
        let path = host(game, &spelled(game, UAL_FILE));
        match sha_of(&path)? {
            Some(have) if have == u.sha256 => {
                std::fs::remove_file(&path).map_err(io_at(&path, "removing"))?;
                out.removed.push(path);
            }
            Some(_) => out.retained.push(RetainedPlacement { path, may_run: true, off: true, reason: format!(
                "Ultimate ASI Loader ({UAL_FILE}) changed since Kiln placed it; Kiln neither replaces nor removes it") }),
            None => {}
        }
    }
    save_receipts(game, &rc)?;
    out.retained.extend(rep.retained);
    out.kept_files.extend(rep.notes);
    Ok(out)
}

/// The `kiln/chain.ini` `[profile]` line that loads Ultimate ASI Loader for the profile's `.asi` packages: (the id of
/// the package it is for, [`UAL_CHAIN_LINE`]). Some when an enabled `.asi` package of the profile is placed
/// ([`RECEIPTS`]), UAL is at [`UAL_FILE`], and `[chain]` does not load it already (Kiln's installer chained it there).
pub fn ual_chain_entry(dirs: &DataDirs, profile: &crate::profile::Profile, game: &Path) -> Option<(String, String)> {
    let rc = read_receipts(game).ok()?;
    let placed = |id: &str| rc.mods.iter().any(|p| p.id == id && p.shape == Shape::Asi);
    let asi: Vec<(&crate::profile::ProfileMod, GamePlan)> = profile.mods.native.iter().filter(|m| m.enabled && placed(&m.id))
        .filter_map(|m| plan_of(dirs, m).filter(|p| p.shape == Shape::Asi).map(|p| (m, p))).collect();
    let (first, _) = asi.first()?;
    if !is_ual_at(&host(game, &spelled(game, UAL_FILE))) { return None; }
    let chained = crate::chain::read(game).is_ok_and(|ini| ini.chain.iter()
        .any(|e| e.status == crate::chain::EntryStatus::Ok && e.path.eq_ignore_ascii_case(UAL_CHAIN_LINE)));
    if chained { return None; }
    // the line goes with the package that brings UAL, else the first .asi package
    let owner = asi.iter().find(|(_, p)| p.ual.is_some()).map(|(m, _)| *m).unwrap_or(first);
    Some((owner.id.clone(), UAL_CHAIN_LINE.to_string()))
}

/// The chain.ini `[profile]` line Kiln writes for Ultimate ASI Loader ([`ual_chain_entry`]), with UAL at [`UAL_FILE`]: the
/// launch options check asks for the `xinput9_1_0` override for it too (game::launch_options_info). A `[profile]` line
/// for a file of that name that is not UAL is not this.
pub fn ual_in_profile_section(ini: &crate::chain::ChainIni, game: &Path) -> bool {
    ini.profile.iter().any(|e| e.status == crate::chain::EntryStatus::Ok && e.path.eq_ignore_ascii_case(UAL_CHAIN_LINE))
        && is_ual_at(&host(game, &spelled(game, UAL_FILE)))
}

// ---- the Library: .asi packages ----

/// An `.asi` package for the Library ([`DetectKind::Asi`], imported by [`import_asi`] as a native mod): a folder that is
/// no other kind of mod, with Ultimate ASI Loader directly in it and/or `.asi` plugins in it or in a folder of it
/// (Smart Workshop's client file is a `Scripts` folder). `top_name`: what the package is called when `dir` is an
/// unpacked archive at depth 0.
pub fn detect_asi_package(dir: &Path, depth: usize, top_name: Option<&str>) -> Option<Detected> {
    if crate::engines::is_container(dir) { return None; }
    let entries: Vec<PathBuf> = std::fs::read_dir(dir).ok()?.flatten().map(|e| e.path()).filter(|p| !p.is_symlink()).collect();
    let is_asi = |p: &Path| p.is_file() && p.extension().is_some_and(|x| x.eq_ignore_ascii_case("asi"));
    let asi = entries.iter().any(|p| is_asi(p) || (p.is_dir() && std::fs::read_dir(p).into_iter().flatten().flatten().any(|e| is_asi(&e.path()))));
    let ual = crate::engines::dlls_in(dir).into_iter().any(|d| is_ual_at(&d));
    if !asi && !ual { return None; }
    let name = match (depth, top_name) {
        (0, Some(n)) => crate::library::package_stem(n).to_string(),
        _ => dir.file_name()?.to_string_lossy().into_owned(),
    };
    let id = if asi { crate::library::id_from_name(&name) } else { UAL_ID.to_string() };
    Some(Detected { kind: DetectKind::Asi, path: dir.display().to_string(), id: Some(id), version: Some("0.0.0".into()),
        name: Some(if asi { name } else { UAL_NAME.to_string() }), reason: ASI_REASON.into(), items: vec![],
        engine: Some(crate::engines::Engine::UltimateAsiLoader) })
}

/// The Library id of Ultimate ASI Loader on its own: one entry whatever the download is called, so a newer UAL replaces
/// the one in the profile.
pub const UAL_ID: &str = "ultimate-asi-loader";
const UAL_NAME: &str = "Ultimate ASI Loader";
/// Why an `.asi` package or Ultimate ASI Loader is a Library mod.
pub const ASI_REASON: &str = "Ultimate ASI Loader or its .asi plugins: added as a native mod, placed in the game folder while it \
    is on (Ultimate ASI Loader as xinput9_1_0.dll next to Kiln's dinput8.dll, the plugins in scripts)";

/// Imports an [`DetectKind::Asi`] item (a package folder, a lone `.asi` file, or a lone Ultimate ASI Loader DLL) as a
/// native mod: its files as they are, and a generated native.json that lists Ultimate ASI Loader under `dlls` when the
/// package brings it and the plugins under `asi`. kiln-core never loads it ([`plan`]): activation places it.
pub fn import_asi(ctx: &mut crate::engines::ImportCtx) -> Result<()> {
    let (dirs, d, src) = (ctx.dirs, ctx.detected, ctx.src);
    let tmp = crate::library::TempDir::new(&dirs.library(), "import")?;
    let work = tmp.path().join("m");
    let file_name = src.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let mut size = if src.is_dir() {
        let size = crate::library::copy_mod_tree(src, &work)?;
        if crate::hash::dirhash(&work)? != crate::hash::dirhash(src)? {
            return Err(Error::new(ErrorCode::HashMismatch, "It changed while it was imported."));
        }
        size
    } else {
        std::fs::create_dir_all(&work).map_err(io_at(&work, "creating"))?;
        std::fs::copy(src, work.join(&file_name)).map_err(io_at(src, "copying"))?
    };
    let files = files_of(&work).ok_or_else(|| Error::new(ErrorCode::Refused, "links or too many files in the package"))?;
    let ual = ual_in(&work, &files);
    let asi: Vec<String> = files.iter().filter(|f| ext(f) == "asi").cloned().collect();
    if ual.is_none() && asi.is_empty() {
        ctx.report.skipped.push(crate::library::Skipped { path: d.path.clone(), reason: "no .asi plugin and no Ultimate ASI Loader in it".into() });
        return Ok(());
    }
    let stem = |n: &str| Path::new(n).file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_else(|| n.to_string());
    let (id, name) = match (src.is_dir(), asi.is_empty()) {
        (_, true) => (UAL_ID.to_string(), UAL_NAME.to_string()),
        (false, false) => (crate::library::id_from_name(&stem(&file_name)), stem(&file_name)),
        (true, false) => (d.id.clone().unwrap_or_else(|| crate::library::id_from_name(&file_name)), d.name.clone().unwrap_or(file_name.clone())),
    };
    let version = ual.as_ref().and_then(|u| std::fs::read(work.join(u)).ok()).filter(|_| asi.is_empty())
        .and_then(|b| crate::library::dll_file_version(&b)).unwrap_or_else(|| "0.0.0".into());
    let manifest = serde_json::json!({
        "format": 1, "id": id, "name": name, "version": version,
        "description": format!("Generated by Kiln: {}. Kiln places it in the game folder while it is on; kiln-core does not load it",
            match (&ual, asi.len()) {
                (Some(_), 0) => "Ultimate ASI Loader".to_string(),
                (Some(_), n) => format!("Ultimate ASI Loader and {n} .asi plugin(s)"),
                (None, n) => format!("{n} .asi plugin(s) for Ultimate ASI Loader"),
            }),
        "side": "client", "dlls": ual.iter().collect::<Vec<_>>(), "asi": asi,
    });
    let mut body = serde_json::to_string_pretty(&manifest)?;
    body.push('\n');
    size += body.len() as u64;
    std::fs::write(work.join(crate::manifest::NATIVE_MANIFEST), body).map_err(io_at(&work, "writing"))?;
    crate::manifest::read_native(&work)?;
    let hash = crate::hash::dirhash(&work)?;
    let rec = ImportRecord { format: 1, kind: ModKind::Native, id, version, hash, imported: crate::util::now_rfc3339(),
        source: Some(ctx.from.clone()), size, name: Some(name), side: Some(Side::Client), clients: Some(Clients::Mod), file: None,
        manifest_id: None, game_folder: None };
    crate::library::commit_folder(dirs, &work, rec, ctx.report)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pe::build_test_dll;

    fn put(root: &Path, rel: &str, data: &[u8]) {
        let p = root.join(rel);
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(p, data).unwrap();
    }

    struct Gone(PathBuf);
    impl Drop for Gone { fn drop(&mut self) { let _ = std::fs::remove_dir_all(&self.0); } }

    fn entry(tag: &str, native: &str, files: &[(&str, Vec<u8>)]) -> (PathBuf, Gone) {
        let d = std::env::temp_dir().join(format!("km-gf-unit-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        put(&d, "native.json", native.as_bytes());
        for (rel, b) in files { put(&d, rel, b); }
        (d.clone(), Gone(d))
    }

    const NATIVE: &str = r#"{"format":1,"id":"m","version":"0.0.0","dlls":["powrprof.dll"]}"#;

    #[test]
    fn a_loader_is_told_by_a_proxy_name_and_a_folder_of_dlls() {
        let dll = || build_test_dll(&[], b"loader");
        let (d, _g) = entry("loader", NATIVE, &[("powrprof.dll", dll()), ("plugins/x.dll", dll()), ("x.ini", b"a".to_vec()),
            ("README.md", b"r".to_vec()), ("plugins/loot.txt", b"l".to_vec()), (".kiln-library.json", b"{}".to_vec())]);
        let p = plan(&d).unwrap();
        assert_eq!(p.shape, Shape::Loader);
        let want: Vec<(String, String)> = ["plugins/loot.txt", "plugins/x.dll", "powrprof.dll", "x.ini"].iter().map(|f| (f.to_string(), f.to_string())).collect();
        assert_eq!(p.files, want);
        // no folder of DLLs: a DLL mod with its files, loaded from the Library as before
        let (d, _g) = entry("plain", NATIVE, &[("powrprof.dll", dll()), ("x.ini", b"a".to_vec()), ("data/x.dat", b"d".to_vec())]);
        assert!(plan(&d).is_none());
        // Kiln's proxy name, the game's Steam DLL: never placed
        for name in ["dinput8.dll", "steam_api64.dll", "DINPUT8.DLL"] {
            let (d, _g) = entry("names", &NATIVE.replace("powrprof.dll", name), &[(name, dll()), ("plugins/x.dll", dll())]);
            assert!(plan(&d).is_none(), "{name}");
        }
        // any other proxy name the game loads from its folder: by the files, not by one name
        let (d, _g) = entry("version", &NATIVE.replace("powrprof.dll", "version.dll"), &[("version.dll", dll()), ("mods/y/y.dll", dll())]);
        assert_eq!(plan(&d).unwrap().loader.as_deref(), Some("version.dll"));
        // SET's own loader (an older launcher imported it as a native mod) is no package to place
        let (d, _g) = entry("set", NATIVE, &[("powrprof.dll", build_test_dll(&[], b"shrouded_engine_tweaks")), ("x/y.dll", dll())]);
        assert!(plan(&d).is_none());
        // a Kiln-aware native mod (init export) is Kiln's to load
        let (d, _g) = entry("init", &NATIVE.replace("}", r#","init":"go"}"#), &[("powrprof.dll", dll()), ("plugins/x.dll", dll())]);
        assert!(plan(&d).is_none());
    }

    #[test]
    fn an_asi_package_maps_its_files_where_ultimate_asi_loader_looks() {
        let ual = build_test_dll(&["DirectInput8Create", "IsUltimateASILoader"], b"");
        let n = r#"{"format":1,"id":"m","version":"0.0.0","dlls":["dinput8.dll"],"asi":["a.asi"]}"#;
        let (d, _g) = entry("asi", n, &[("dinput8.dll", ual.clone()), ("a.asi", b"MZ".to_vec()), ("a.ini", b"i".to_vec()),
            ("Scripts/b.asi", b"MZ".to_vec()), ("version.dll", build_test_dll(&[], b"x")), ("readme.txt", b"r".to_vec())]);
        let p = plan(&d).unwrap();
        assert_eq!((p.shape, p.ual.as_deref()), (Shape::Asi, Some("dinput8.dll")));
        let got: Vec<(&str, &str)> = p.files.iter().map(|(a, b)| (a.as_str(), b.as_str())).collect();
        assert_eq!(got, [("Scripts/b.asi", "Scripts/b.asi"), ("a.asi", "scripts/a.asi"), ("a.ini", "scripts/a.ini")]);
        // a native DLL mod that also ships an .asi build of itself (no `asi` in its native.json) stays Kiln's to load
        let (d, _g) = entry("both", r#"{"format":1,"id":"m","version":"0.0.0","dlls":["m.dll"]}"#,
            &[("m.dll", build_test_dll(&[], b"m")), ("m.asi", b"MZ".to_vec())]);
        assert!(plan(&d).is_none());
    }

    #[test]
    fn a_package_folder_lands_in_the_game_folders_own_spelling() {
        let g = std::env::temp_dir().join(format!("km-gf-spell-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&g);
        std::fs::create_dir_all(g.join("scripts")).unwrap();
        assert_eq!(spelled(&g, "Scripts/SmartWorkshop.asi"), "scripts/SmartWorkshop.asi");
        assert_eq!(spelled(&g, "plugins/x.dll"), "plugins/x.dll");
        let _ = std::fs::remove_dir_all(&g);
    }

    #[test]
    fn an_asi_manifest_may_have_no_dlls() {
        assert!(crate::manifest::parse_native(r#"{"id":"m","version":"0.0.0","dlls":[],"asi":["scripts/a.asi"]}"#).is_ok());
        for bad in [r#""asi":[]"#, r#""asi":["a.dll"]"#, r#""asi":["../a.asi"]"#, r#""asi":["a.asi","a.asi"]"#, r#""asi":"a.asi""#] {
            let t = format!(r#"{{"id":"m","version":"0.0.0","dlls":[],{bad}}}"#);
            assert!(crate::manifest::parse_native(&t).is_err(), "{bad}");
        }
    }
}
