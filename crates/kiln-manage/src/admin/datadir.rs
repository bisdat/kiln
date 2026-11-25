//! Where a server's mods keep their data. By default `<server>/kiln/data/<mod id>`; a profile with `dataDir: profile`
//! ([`crate::profile::DataDirMode::Profile`]) moves it to `<data>/profiles/<pid>/data/<mod id>` (or the copy-mode
//! folder in the game), and Kiln.Admin and Kiln Web Admin then write endpoint.json, admin-token and
//! launcher-endpoint.json there. kiln-core says which folder the running server booted with in
//! `kiln/state/boot.json` `profile.dataDir` (the profile's game-side path: absolute Windows, `Z:` under Wine, or
//! relative to `<server>/kiln/`). The candidates come in order: the booted profile's folder first, the default after,
//! so a stale file left in `kiln/data` by an earlier start never shadows the live one.

use crate::server::remote::RemoteSpec;
use std::path::{Path, PathBuf};

/// `profile.dataDir` from boot.json text; None when absent, null, empty or unreadable.
pub fn booted_data_dir(boot_json: &[u8]) -> Option<String> {
    let v: serde_json::Value = serde_json::from_slice(boot_json.strip_prefix(b"\xef\xbb\xbf").unwrap_or(boot_json)).ok()?;
    let d = v.get("profile")?.get("dataDir")?.as_str()?.trim();
    (!d.is_empty() && d.len() <= 32767 && !d.contains('\0')).then(|| d.to_string())
}

fn relative_parts(p: &str) -> Option<Vec<&str>> {
    let parts: Vec<&str> = p.split(['\\', '/']).filter(|c| !c.is_empty() && *c != ".").collect();
    (!parts.is_empty() && !parts.contains(&"..")).then_some(parts)
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

/// The folders where mod `id` of the server in `server_dir` may keep its data, most likely first.
pub fn local_mod_dirs(server_dir: &Path, prefix: Option<&Path>, id: &str) -> Vec<PathBuf> {
    let default = server_dir.join("kiln").join("data").join(id);
    let mut out = Vec::new();
    if let Ok(b) = std::fs::read(server_dir.join(crate::paths::game::BOOT_JSON))
        && let Some(d) = booted_data_dir(&b)
        && let Some(p) = local_host_path(server_dir, prefix, &d)
    {
        out.push(p.join(id));
    }
    if !out.contains(&default) {
        out.push(default);
    }
    out
}

/// The same for a remote server, as absolute paths on it (boot.json is read over ssh).
pub fn remote_mod_dirs(remote: &RemoteSpec, id: &str) -> Vec<String> {
    let default = format!("{}/kiln/data/{id}", remote.dir.trim_end_matches('/'));
    let mut out = Vec::new();
    if let Ok(b) = crate::server::remote::read_file(remote, crate::paths::game::BOOT_JSON)
        && let Some(d) = booted_data_dir(&b)
        && let Some(p) = remote_host_path(remote, &d)
    {
        out.push(format!("{}/{id}", p.trim_end_matches('/')));
    }
    if !out.contains(&default) {
        out.push(default);
    }
    out
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
    fn the_booted_folder_comes_before_the_default() {
        let dir = std::env::temp_dir().join(format!("kiln-datadir-{}-{}", std::process::id(), line!()));
        let state = dir.join("kiln/state");
        std::fs::create_dir_all(&state).unwrap();
        assert_eq!(local_mod_dirs(&dir, None, "kiln-webadmin"), vec![dir.join("kiln/data/kiln-webadmin")]);
        std::fs::write(state.join("boot.json"), br#"{"profile":{"dataDir":"copy/p1/data"}}"#).unwrap();
        assert_eq!(local_mod_dirs(&dir, None, "kiln-webadmin"), vec![dir.join("kiln/copy/p1/data/kiln-webadmin"), dir.join("kiln/data/kiln-webadmin")]);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
