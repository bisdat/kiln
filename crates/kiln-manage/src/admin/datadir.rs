//! Where a server's mods keep their data. By default `<server>/kiln/data/<mod id>`; a profile with `dataDir: profile`
//! ([`crate::profile::DataDirMode::Profile`]) moves it to `<data>/profiles/<pid>/data/<mod id>` (or the copy-mode
//! folder in the game), and Kiln.Admin and Kilnwarden then write endpoint.json, admin-token and
//! launcher-endpoint.json there. kiln-core says which folder the running server booted with in
//! `kiln/state/boot.json` `profile.dataDir` (the profile's game-side path: absolute Windows, `Z:` under Wine, or
//! relative to `<server>/kiln/`).
//!
//! The server's data root is that folder when boot.json names one this launcher can map, else `<server>/kiln/data`:
//! one folder, never a mix. Every mod of a running server writes under the same root, so a reader that needs files of
//! two mods (Open web panel: Kilnwarden's listener and Kiln.Admin's token) resolves the root once and reads both
//! from it. (Before the review of 0.2.0-dev.31.12 each file fell back to `kiln/data` on its own when the booted folder
//! lacked it, so a server could pair its own listener with another folder's Kiln.Admin.)

use crate::server::remote::RemoteSpec;
use std::path::{Path, PathBuf};

/// `profile.dataDir` from boot.json text; None when absent, null, empty or unreadable.
pub fn booted_data_dir(boot_json: &[u8]) -> Option<String> {
    let v: serde_json::Value = serde_json::from_slice(boot_json.strip_prefix(b"\xef\xbb\xbf").unwrap_or(boot_json)).ok()?;
    let d = v.get("profile")?.get("dataDir")?.as_str()?.trim();
    (!d.is_empty() && d.len() <= 32767 && !d.contains('\0')).then(|| d.to_string())
}

/// The components of a relative game-side path; None for `..` or a component with a drive or stream colon (`C:` pushed
/// onto a Windows path replaces it: `a\\C:\\x` would leave the server folder).
fn relative_parts(p: &str) -> Option<Vec<&str>> {
    let parts: Vec<&str> = p.split(['\\', '/']).filter(|c| !c.is_empty() && *c != ".").collect();
    (!parts.is_empty() && !parts.iter().any(|c| *c == ".." || c.contains(':'))).then_some(parts)
}

fn is_drive_path(p: &str) -> bool {
    let b = p.as_bytes();
    b.len() >= 3 && b[0].is_ascii_alphabetic() && b[1] == b':' && (b[2] == b'\\' || b[2] == b'/')
}

/// A game-side data folder as a host path for a server on this machine (`prefix`: its Wine prefix, for drive letters
/// other than Z:). None when it can't be mapped (a UNC path under Wine, an unmapped drive).
pub fn local_host_path(server_dir: &Path, prefix: Option<&Path>, game_path: &str) -> Option<PathBuf> {
    let p = game_path.trim();
    if p.starts_with("\\\\") || p.starts_with("//") {
        return cfg!(windows).then(|| PathBuf::from(p));
    }
    if is_drive_path(p) {
        if cfg!(windows) {
            return Some(PathBuf::from(p));
        }
        let letter = p.as_bytes()[0] as char;
        let mut out = crate::winpath::drive_target(prefix, letter)?;
        for c in relative_parts(&p[3..])? {
            out.push(c);
        }
        return Some(out);
    }
    if p.starts_with('/') {
        return (!cfg!(windows)).then(|| PathBuf::from(p));
    }
    let mut out = server_dir.join("kiln");
    for c in relative_parts(p)? {
        out.push(c);
    }
    Some(out)
}

/// The same for a remote (Linux) server: an absolute POSIX path on that machine. `Z:` maps to `/`; other drive letters
/// and UNC paths can't be mapped from here.
pub fn remote_host_path(remote: &RemoteSpec, game_path: &str) -> Option<String> {
    let p = game_path.trim();
    if p.contains('\n') || p.contains('\0') {
        return None;
    }
    if is_drive_path(p) {
        if !p[..1].eq_ignore_ascii_case("z") {
            return None;
        }
        return Some(format!("/{}", relative_parts(&p[3..])?.join("/")));
    }
    if p.starts_with('/') && !p.starts_with("//") {
        return Some(format!("/{}", relative_parts(p)?.join("/")));
    }
    if p.starts_with('\\') {
        return None;
    }
    Some(format!("{}/kiln/{}", remote.dir.trim_end_matches('/'), relative_parts(p)?.join("/")))
}

/// The folder holding the per-mod data folders of the server in `server_dir`, as it booted (`prefix`: its Wine
/// prefix, for drive letters other than Z:).
pub fn local_data_root(server_dir: &Path, prefix: Option<&Path>) -> PathBuf {
    std::fs::read(server_dir.join(crate::paths::game::BOOT_JSON))
        .ok()
        .and_then(|b| booted_data_dir(&b))
        .and_then(|d| local_host_path(server_dir, prefix, &d))
        .unwrap_or_else(|| server_dir.join("kiln").join("data"))
}

/// The same for a remote server, as an absolute path on it (boot.json is read over ssh).
pub fn remote_data_root(remote: &RemoteSpec) -> String {
    crate::server::remote::read_file(remote, crate::paths::game::BOOT_JSON)
        .ok()
        .and_then(|b| booted_data_dir(&b))
        .and_then(|d| remote_host_path(remote, &d))
        .map(|p| p.trim_end_matches('/').to_string())
        .filter(|p| !p.is_empty())
        .unwrap_or_else(|| format!("{}/kiln/data", remote.dir.trim_end_matches('/')))
}

/// A server's data root, resolved once: every file one operation reads comes from it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DataRoot {
    Local(PathBuf),
    /// An absolute path on the remote machine.
    Remote(String),
}

impl DataRoot {
    /// Mod `id`'s folder on this machine (None for a remote root).
    pub fn local_mod(&self, id: &str) -> Option<PathBuf> {
        match self {
            DataRoot::Local(p) => Some(p.join(id)),
            DataRoot::Remote(_) => None,
        }
    }

    /// Mod `id`'s folder on the remote machine (None for a local root).
    pub fn remote_mod(&self, id: &str) -> Option<String> {
        match self {
            DataRoot::Remote(p) => Some(format!("{}/{id}", p.trim_end_matches('/'))),
            DataRoot::Local(_) => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_the_booted_profiles_data_dir() {
        assert_eq!(booted_data_dir(br#"{"profile":{"mode":"active","dataDir":"Z:\\home\\me\\kiln\\profiles\\p1\\data"}}"#).as_deref(), Some("Z:\\home\\me\\kiln\\profiles\\p1\\data"));
        for none in [&br#"{"profile":{"dataDir":null}}"#[..], br#"{"profile":{"dataDir":""}}"#, br#"{"profile":{}}"#, b"{}", b"not json"] {
            assert_eq!(booted_data_dir(none), None);
        }
    }

    #[test]
    fn maps_game_side_paths() {
        let server = Path::new("/srv/lab");
        assert_eq!(local_host_path(server, None, "profiles-copy/p1/data"), Some(PathBuf::from("/srv/lab/kiln/profiles-copy/p1/data")));
        assert_eq!(local_host_path(server, None, "a/../../etc"), None, "no way out of the server folder");
        if !cfg!(windows) {
            assert_eq!(local_host_path(server, None, "Z:\\home\\me\\kiln\\profiles\\p1\\data"), Some(PathBuf::from("/home/me/kiln/profiles/p1/data")));
            assert_eq!(local_host_path(server, None, "\\\\nas\\share"), None);
            let pfx = std::env::temp_dir().join(format!("kiln-pfx-{}", std::process::id()));
            std::fs::create_dir_all(pfx.join("drive_c")).unwrap();
            assert_eq!(local_host_path(server, Some(&pfx), "C:\\data\\x"), Some(pfx.join("drive_c").join("data").join("x")));
            let _ = std::fs::remove_dir_all(&pfx);
        }
        let r = RemoteSpec { host: "box".into(), port: None, dir: "/srv/e/".into(), instance: None };
        assert_eq!(remote_host_path(&r, "Z:\\srv\\kiln\\profiles\\p\\data").as_deref(), Some("/srv/kiln/profiles/p/data"));
        assert_eq!(remote_host_path(&r, "profiles-copy\\p\\data").as_deref(), Some("/srv/e/kiln/profiles-copy/p/data"));
        assert_eq!(remote_host_path(&r, "D:\\x"), None);
        assert_eq!(remote_host_path(&r, "/a/../b"), None);
    }

    #[test]
    fn a_drive_inside_a_relative_path_is_refused() {
        // review of 0.2.0-dev.31.12: on Windows, PathBuf::push("C:") replaces the server folder
        let server = Path::new("/srv/lab");
        assert_eq!(local_host_path(server, None, "a\\C:\\Windows"), None);
        assert_eq!(local_host_path(server, None, "copy/p1:stream/data"), None);
        let r = RemoteSpec { host: "box".into(), port: None, dir: "/srv/e".into(), instance: None };
        assert_eq!(remote_host_path(&r, "a\\C:\\x"), None);
    }

    #[test]
    fn the_booted_folder_is_the_only_root() {
        let dir = std::env::temp_dir().join(format!("kiln-datadir-{}-{}", std::process::id(), line!()));
        let state = dir.join("kiln/state");
        std::fs::create_dir_all(&state).unwrap();
        assert_eq!(local_data_root(&dir, None), dir.join("kiln/data"));
        std::fs::write(state.join("boot.json"), br#"{"profile":{"dataDir":"copy/p1/data"}}"#).unwrap();
        assert_eq!(local_data_root(&dir, None), dir.join("kiln/copy/p1/data"), "no kiln/data fallback beside it");
        // a folder this launcher can't map (C: without the prefix): the default
        std::fs::write(state.join("boot.json"), br#"{"profile":{"dataDir":"C:\\data"}}"#).unwrap();
        if !cfg!(windows) {
            assert_eq!(local_data_root(&dir, None), dir.join("kiln/data"));
        }
        let _ = std::fs::remove_dir_all(&dir);
    }
}
