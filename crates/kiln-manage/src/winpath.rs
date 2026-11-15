//! Host <-> game path translation (contracts.md, "Path translation").
//! - Proton, Z mode: `/home/u/x` <-> `Z:\home\u\x` (valid only when `<pfx>/dosdevices/z:` resolves to `/`).
//! - Proton, other drives: `C:\...` <-> `<pfx>/drive_c/...`; other letters through `<pfx>/dosdevices/<l>:`.
//! - Windows: paths are used as they are.
//! - Copy mode: relative paths, resolved against `<game>/kiln/`.
//!
//! A host path with a component Windows cannot name (`<>:"|?*`, a trailing dot or space, a control character) does
//! not translate (Error::Unsupported): the caller falls back to copy mode.

use crate::{Error, ErrorCode, Result};
use std::path::{Path, PathBuf};

const INVALID: &[char] = &['<', '>', ':', '"', '|', '?', '*', '\\'];

/// `/a/b` -> `Z:\a\b`. Pure string translation; the caller checks the Z: drive.
pub fn unix_to_z(path: &Path) -> Result<String> {
    let s = path.to_str().ok_or_else(|| Error::new(ErrorCode::Unsupported, format!("{}: not UTF-8", path.display())))?;
    if !s.starts_with('/') {
        return Err(Error::invalid(format!("{s}: not an absolute path")));
    }
    let mut out = String::from("Z:");
    for part in s.split('/').filter(|p| !p.is_empty()) {
        if part.contains(INVALID) || part.ends_with('.') || part.ends_with(' ') || part.chars().any(|c| (c as u32) < 32) {
            return Err(Error::new(ErrorCode::Unsupported, format!("{s}: '{part}' cannot be a Windows name")));
        }
        out.push('\\');
        out.push_str(part);
    }
    if out.len() == 2 {
        out.push('\\');
    }
    Ok(out)
}

/// `Z:\a\b` (or `Z:/a/b`) -> `/a/b`. None for another drive or a relative path, and always None on Windows, where
/// `Z:` is a real drive (often a mapped network share): callers then use the path as it is.
pub fn z_to_unix(path: &str) -> Option<PathBuf> {
    if cfg!(windows) {
        return None;
    }
    let rest = path.strip_prefix("Z:").or_else(|| path.strip_prefix("z:"))?;
    if !rest.starts_with(['\\', '/']) {
        return None;
    }
    let parts: Vec<&str> = rest.split(['\\', '/']).filter(|p| !p.is_empty()).collect();
    Some(PathBuf::from(format!("/{}", parts.join("/"))))
}

/// True when `<prefix>/dosdevices/z:` resolves to `/`.
pub fn z_drive_ok(prefix: &Path) -> bool {
    std::fs::canonicalize(prefix.join("dosdevices").join("z:")).map(|p| p == Path::new("/")).unwrap_or(false)
}

/// `\\?\C:\x` -> `C:\x`, `\\?\UNC\server\share` -> `\\server\share` (Windows' verbatim prefix; game-side paths never
/// carry it).
pub(crate) fn strip_verbatim(s: &str) -> std::borrow::Cow<'_, str> {
    if let Some(rest) = s.strip_prefix(r"\\?\UNC\") {
        return format!(r"\\{rest}").into();
    }
    s.strip_prefix(r"\\?\").unwrap_or(s).into()
}

/// [`strip_verbatim`] where the path has a spelling without the prefix (a drive letter or a server): a verbatim path
/// without one (`\\?\Volume{…}\x`) stays as it is.
pub(crate) fn plain_path(s: &str) -> std::borrow::Cow<'_, str> {
    // Win32 trims these components without the verbatim prefix. Such a spelling would name
    // a different folder, so it must also stay verbatim in receipts and comparison keys.
    if s.starts_with(r"\\?\") && s.split('\\').skip(1).any(|c| c.ends_with(['.', ' '])) {
        return s.into();
    }
    let plain = strip_verbatim(s);
    if plain.len() == s.len() || is_absolute_game_path(&plain) { plain } else { s.into() }
}

/// Where `path` is below `base`, compared as Windows compares folders: without Windows' verbatim prefix (`\\?\C:\x` is
/// `C:\x`, `\\?\UNC\s\x` is `\\s\x`; ShroudForge writes its `paths.mods` from a canonical path, which carries it, and
/// Kiln names the game folder without it), `/` and `\` alike, empty and `.` parts dropped, case ignored (each
/// character's simple upper case, as Windows' ordinal comparison of names). The parts of `path` below `base` (none: the
/// same folder), None when it is not there. Pure: a Linux build tests Windows' rules too.
pub(crate) fn windows_below(base: &str, path: &str) -> Option<Vec<String>> {
    // (how many separators lead: 2 a network path, 1 the root of a drive, 0 a drive letter or a relative path; the parts)
    fn parts(p: &str) -> (usize, Vec<String>) {
        let p = p.replace('/', "\\");
        let p = plain_path(&p);
        let lead = p.bytes().take_while(|&b| b == b'\\').count().min(2)
            + if p.as_bytes().get(1) == Some(&b':') && p.as_bytes().get(2) == Some(&b'\\') { 3 } else { 0 };
        (lead, p.split('\\').filter(|c| !c.is_empty() && *c != ".").map(str::to_owned).collect())
    }
    fn same_name(a: &str, b: &str) -> bool {
        let upper = |c: char| {
            let mut u = c.to_uppercase();
            match (u.next(), u.next()) { (Some(x), None) => x, _ => c }
        };
        a.chars().map(upper).eq(b.chars().map(upper))
    }
    let ((base_lead, base), (lead, path)) = (parts(base), parts(path));
    (base_lead == lead && path.len() >= base.len() && base.iter().zip(&path).all(|(a, b)| same_name(a, b)))
        .then(|| path[base.len()..].to_vec())
}

/// `a` and `b` name the same folder as Windows compares folders ([`windows_below`]).
pub(crate) fn windows_same_folder(a: &str, b: &str) -> bool {
    windows_below(a, b).is_some_and(|rest| rest.is_empty())
}

/// `a` and `b` are the same folder, compared as the system the launcher runs on compares folders: on Windows by
/// Windows' rules ([`windows_same_folder`]: the verbatim prefix and case do not matter), elsewhere exactly (a launcher
/// on Linux names a Proton game's folders by their host paths, which are case-sensitive).
pub(crate) fn same_folder(a: &Path, b: &Path) -> bool {
    if cfg!(windows) { windows_same_folder(&a.to_string_lossy(), &b.to_string_lossy()) } else { a == b }
}

/// `dir` relative to `base`, '/' separated ("" for `base` itself), compared as [`same_folder`] compares; None when
/// `dir` is not below `base`.
pub(crate) fn relative_folder(base: &Path, dir: &Path) -> Option<String> {
    if cfg!(windows) {
        return windows_below(&base.to_string_lossy(), &dir.to_string_lossy()).map(|rest| rest.join("/"));
    }
    dir.strip_prefix(base).ok().map(|p| p.to_string_lossy().replace('\\', "/"))
}

/// A host path as the game sees it, for an install's path mode.
/// - Z: `Z:\...` (the caller has checked the Z: drive: `game::path_mode`).
/// - Native: the path as it is (Windows; no `\\?\` prefix).
/// - Copy: Error::Unsupported (the caller copies the folder under `<game>/kiln/profiles/` and writes a relative path).
pub fn to_game(host: &Path, install: &crate::game::GameInstall) -> Result<String> {
    match install.path_mode {
        crate::game::PathMode::Z => unix_to_z(host),
        crate::game::PathMode::Native => {
            let s = host.to_str().ok_or_else(|| Error::new(ErrorCode::Unsupported, format!("{}: not UTF-8", host.display())))?;
            if !host.is_absolute() {
                return Err(Error::invalid(format!("{s}: not an absolute path")));
            }
            Ok(strip_verbatim(s).into_owned())
        }
        crate::game::PathMode::Copy => Err(Error::new(
            ErrorCode::Unsupported,
            format!("{}: copy mode has no game-side path for a host folder (copy it under kiln/profiles/)", host.display()),
        )),
    }
}

/// True for `X:\...` / `X:/...` and UNC `\\server\...`.
pub fn is_absolute_game_path(p: &str) -> bool {
    let b = p.as_bytes();
    (b.len() >= 3 && b[0].is_ascii_alphabetic() && b[1] == b':' && (b[2] == b'\\' || b[2] == b'/'))
        || p.starts_with("\\\\")
        || p.starts_with("//")
}

/// Drops `.` and resolves `..` without touching the file system.
fn lexical(p: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for c in p.components() {
        match c {
            std::path::Component::CurDir => {}
            std::path::Component::ParentDir => {
                out.pop();
            }
            other => out.push(other),
        }
    }
    out
}

/// Where a Wine drive letter points: `<prefix>/dosdevices/<l>:` (a symlink, relative ones resolved against
/// dosdevices/); C: falls back to `<prefix>/drive_c`, Z: to `/`.
pub fn drive_target(prefix: Option<&Path>, letter: char) -> Option<PathBuf> {
    let l = letter.to_ascii_lowercase();
    if let Some(pfx) = prefix {
        let dd = pfx.join("dosdevices");
        if let Ok(t) = std::fs::read_link(dd.join(format!("{l}:"))) {
            return Some(if t.is_absolute() { t } else { lexical(&dd.join(t)) });
        }
        if l == 'c' {
            return Some(pfx.join("drive_c"));
        }
    }
    (l == 'z').then(|| PathBuf::from("/"))
}

/// A game-side path (absolute, or relative to `<game>/kiln/`) as a host path.
pub fn to_host(game_path: &str, install: &crate::game::GameInstall) -> Result<PathBuf> {
    let p = game_path.trim();
    if p.is_empty() {
        return Err(Error::invalid("empty game-side path"));
    }
    if !is_absolute_game_path(p) && !p.starts_with('/') {
        let rel: Vec<&str> = p.split(['\\', '/']).filter(|c| !c.is_empty() && *c != ".").collect();
        // Copy mode's ways out of kiln/, under the native core's folder name rule: `../mods/<folder>`, a folder in the
        // game's own mods folder (an EML mod a native DLL reads there, an XHL companion), and `../greeneye/<id>`, Kiln's
        // copy of a Greeneye mod
        if let ["..", root @ ("mods" | "greeneye"), name] = rel.as_slice()
            && crate::engine_mods::is_game_folder_name(name)
        {
            return Ok(install.path.join(root).join(name));
        }
        if rel.contains(&"..") {
            return Err(Error::invalid(format!("{p}: '..' is not allowed in a relative game path")));
        }
        let mut out = install.path.join(crate::paths::game::KILN);
        for c in rel {
            out.push(c);
        }
        return Ok(out);
    }
    if cfg!(windows) || install.platform == crate::game::Platform::Windows {
        return Ok(PathBuf::from(p));
    }
    if p.starts_with('/') && !p.starts_with("//") {
        return Ok(PathBuf::from(p)); // already a host path
    }
    if p.starts_with("\\\\") || p.starts_with("//") {
        return Err(Error::new(ErrorCode::Unsupported, format!("{p}: UNC paths have no host path under Wine")));
    }
    let letter = p.chars().next().unwrap_or('?');
    let base = drive_target(install.prefix.as_deref(), letter)
        .ok_or_else(|| Error::not_found(format!("{p}: drive {letter}: is not mapped in the Wine prefix")))?;
    let mut out = base;
    for c in p[2..].split(['\\', '/']).filter(|c| !c.is_empty() && *c != ".") {
        out.push(c);
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn z_round_trip() {
        let z = unix_to_z(Path::new("/home/user/.local/share/kiln/library")).unwrap();
        assert_eq!(z, "Z:\\home\\user\\.local\\share\\kiln\\library");
        assert_eq!(unix_to_z(Path::new("/")).unwrap(), "Z:\\");
        assert!(unix_to_z(Path::new("/a/b:c")).is_err());
        assert!(unix_to_z(Path::new("/a/trailing.")).is_err());
        assert!(z_to_unix("C:\\x").is_none());
        if cfg!(windows) {
            // Z: is a real drive there: no translation
            assert!(z_to_unix(&z).is_none());
        } else {
            assert_eq!(z_to_unix(&z).unwrap(), PathBuf::from("/home/user/.local/share/kiln/library"));
        }
    }

    /// ShroudForge's `paths.mods` on Windows is `\\?\C:\…\mods` (written from a canonical path at every start), Kiln's
    /// game folder `C:\…` (Nexus report, 2026-11-14: Auto Loot Plus stayed off). Windows' rules, tested on every system.
    #[test]
    fn windows_compares_folders_without_the_verbatim_prefix_and_case() {
        let game = r"C:\Program Files (x86)\Steam\steamapps\common\Enshrouded";
        let mods = format!(r"{game}\mods");
        let sf = format!(r"\\?\{mods}");
        assert!(windows_same_folder(&sf, &mods) && windows_same_folder(&mods, &sf), "the verbatim prefix");
        assert!(windows_same_folder(&mods.to_lowercase(), &mods) && windows_same_folder(&sf.to_uppercase(), &mods), "case");
        assert!(windows_same_folder(&format!("{sf}\\"), &mods.replace('\\', "/")), "separators");
        assert!(windows_same_folder(r"\\?\UNC\nas\games\Enshrouded\mods", r"\\NAS\Games\Enshrouded\mods\."), "a network path");
        assert!(windows_same_folder(r"C:\Spiele\ÄÖÜ", r"c:\spiele\äöü"), "non-ASCII case pairs");
        assert!(!windows_same_folder(&mods, &format!("{mods}-other")), "a sibling with a longer name");
        assert!(!windows_same_folder(&mods, &format!(r"D:{}", &mods[2..])), "another drive");
        assert!(!windows_same_folder(r"\mods", "mods"), "a root-relative path is not a relative one");
        assert!(!windows_same_folder(r"\\?\Volume{1b3}\mods", r"Volume{1b3}\mods"), "a volume path has no other spelling");
        assert_eq!(windows_below(game, &format!(r"{}\auto-loot-plus", sf.to_lowercase())), Some(vec!["mods".into(), "auto-loot-plus".into()]));
        assert_eq!(windows_below(&sf, game), None, "a parent is not below");
        // the spelling a receipt keeps: the plain one where there is one
        assert_eq!(plain_path(&format!(r"{sf}\auto-loot-plus")), format!(r"{mods}\auto-loot-plus"));
        assert_eq!(plain_path(r"\\?\UNC\nas\games\mods"), r"\\nas\games\mods");
        assert_eq!(plain_path(r"\\?\Volume{1b3}\mods"), r"\\?\Volume{1b3}\mods");
        assert_eq!(plain_path("/home/u/game/mods"), "/home/u/game/mods");
    }

    #[test]
    fn verbatim_names_that_need_the_prefix_keep_it() {
        for p in [r"\\?\C:\Game.\mods", r"\\?\C:\Game \mods", r"\\?\UNC\nas\share\Game.\mods"] {
            assert_eq!(plain_path(p), p, "removing the prefix changes Win32 name resolution");
            assert!(!windows_same_folder(p, &strip_verbatim(p)));
        }
        assert!(!windows_same_folder("C:", r"C:\"), "drive current directory is not its root");
    }

    /// The launcher on Linux keeps comparing host paths exactly: `/game/Mods` and `/game/mods` are two folders there.
    #[test]
    fn this_systems_comparison_is_exact_on_linux() {
        let (a, b) = (Path::new("/srv/game/mods"), Path::new("/srv/game/Mods"));
        assert!(same_folder(a, a));
        assert_eq!(same_folder(a, b), cfg!(windows));
        assert_eq!(relative_folder(Path::new("/srv/game"), a).as_deref(), Some("mods"));
        assert_eq!(relative_folder(Path::new("/srv/Game"), a).is_some(), cfg!(windows));
    }
}
