//! The Wine/Proton environment a helper or debugger needs to run beside a game in its prefix (chapter 12a §4 step 3):
//! an allowlist read from the game's own environment. Nothing else is copied: not `LD_PRELOAD` (the Steam overlay,
//! anything injected), not a command, not a path the descriptor names.

use std::collections::BTreeMap;

/// What is taken from a runner context's environment: the prefix, Steam's compat data and client paths, the library
/// paths and DLL overrides the runner was started with, and the sync mode its wineserver uses (a client of another
/// sync mode cannot share that wineserver).
pub const ALLOWED: &[&str] = &[
    "WINEPREFIX",
    "STEAM_COMPAT_DATA_PATH",
    "STEAM_COMPAT_CLIENT_INSTALL_PATH",
    "LD_LIBRARY_PATH",
    "WINEDLLPATH",
    "WINEDLLOVERRIDES",
    "WINEESYNC",
    "WINEFSYNC",
    "WINENTSYNC",
];

/// The allowlisted variables of a NUL-separated environment block (`/proc/<pid>/environ`), by name. A name that
/// appears twice keeps its first value, as getenv does.
pub fn allowlisted(block: &[u8]) -> BTreeMap<String, String> {
    let mut out = BTreeMap::new();
    for kv in block.split(|&b| b == 0) {
        let Some(eq) = kv.iter().position(|&b| b == b'=') else { continue };
        let (k, v) = (String::from_utf8_lossy(&kv[..eq]).into_owned(), String::from_utf8_lossy(&kv[eq + 1..]).into_owned());
        if ALLOWED.contains(&k.as_str()) {
            out.entry(k).or_insert(v);
        }
    }
    out
}

/// The environment the helper and the debugger run with: the captured allowlist, Wine's debug output off, and the
/// crash dialog (winedbg) off so no window ever reaches the desktop.
pub fn launch_env(captured: &BTreeMap<String, String>) -> Vec<(String, String)> {
    let mut env: Vec<(String, String)> = captured.iter().filter(|(k, _)| k.as_str() != "WINEDLLOVERRIDES").map(|(k, v)| (k.clone(), v.clone())).collect();
    let overrides = match captured.get("WINEDLLOVERRIDES").map(|s| s.trim_matches(';')).filter(|s| !s.is_empty()) {
        Some(o) => format!("{o};winedbg.exe=d"),
        None => "winedbg.exe=d".to_string(),
    };
    env.push(("WINEDLLOVERRIDES".into(), overrides));
    env.push(("WINEDEBUG".into(), "-all".into()));
    env
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_the_allowlist_is_taken() {
        let block = b"HOME=/home/u\0WINEPREFIX=/c/pfx\0LD_PRELOAD=/steam/gameoverlayrenderer.so\0STEAM_COMPAT_DATA_PATH=/c\0WINEDLLOVERRIDES=dinput8=n,b\0WINEPREFIX=/other\0SteamAppId=1203620\0WINEFSYNC=1\0garbage\0";
        let got = allowlisted(block);
        assert_eq!(got.get("WINEPREFIX").map(String::as_str), Some("/c/pfx"), "the first value wins");
        assert!(!got.contains_key("LD_PRELOAD"));
        assert!(!got.contains_key("HOME") && !got.contains_key("SteamAppId"));
        assert_eq!(got.len(), 4);
        let env = launch_env(&got);
        assert!(!env.iter().any(|(k, _)| k == "LD_PRELOAD"));
        assert!(env.contains(&("WINEDLLOVERRIDES".into(), "dinput8=n,b;winedbg.exe=d".into())));
        assert!(env.contains(&("WINEDEBUG".into(), "-all".into())));
        assert!(launch_env(&BTreeMap::new()).contains(&("WINEDLLOVERRIDES".into(), "winedbg.exe=d".into())));
    }
}
