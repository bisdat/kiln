//! The pinned netcoredbg (chapter 12a §2: one release, not repinned without the acceptance tests; never vsdbg).
//! `kiln debug` checks the folder of the netcoredbg.exe it is about to start against these per-file SHA-256s: a copy at
//! the default place (or one the editor says it took from its bundle) must match, an explicit override that differs
//! starts with a warning. tools/vscode/scripts/fetch-netcoredbg.mjs pins the same release by its zip.

use super::errors::{DebugCode, DebugError};
use std::path::Path;

/// The pinned release.
pub const VERSION: &str = "3.2.0-1092";
/// SHA-256 of the release's `netcoredbg-win64.zip` (fetch-netcoredbg.mjs checks it).
pub const ZIP_SHA256: &str = "3c410a45fa502415203a94fcb88654af65bf8e3dac158a5527a722e7a6b9274a";
/// Every file of the release's `netcoredbg/` folder and its SHA-256.
pub const FILES: &[(&str, &str)] = &[
    ("ManagedPart.dll", "bf6c824357cded65cc6dc0a2467104a835b1f79274e742c0655c263bc906efee"),
    ("Microsoft.CodeAnalysis.CSharp.Scripting.dll", "64050c89e7bf7905f6c7ec9eb1594dcea5fbbd2868341f9ef99b58e9dea01605"),
    ("Microsoft.CodeAnalysis.CSharp.dll", "e9cc57c55a95146f98b55dd46fdd881b4be48925e3773ef4aa5ac622b76903a6"),
    ("Microsoft.CodeAnalysis.Scripting.dll", "066f1d39b5e211e343c8c79f374438884b1684fa27dc8c29a9000f88615bf764"),
    ("Microsoft.CodeAnalysis.dll", "97553a05b1f8aad2f29349f0fc7893ceb6dd199bd769d3e1092839957c7c0cdb"),
    ("dbgshim.dll", "4b5d67e6325322e863e5edee33d2f94b83ef1178885824b65c57f7917d4970f3"),
    ("netcoredbg.exe", "23fc802866a03c5aa02484937d61ecbccf0ca731718d1840b0871f153340a823"),
];

/// Where the netcoredbg to check came from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Origin {
    /// The default place (`<data>/tools/netcoredbg/`), or a copy the editor took from its own bundle: must be the pin.
    Pinned,
    /// An explicit choice of the user (`KILN_NETCOREDBG`, the editor's setting): a mismatch is a warning.
    Override,
}

/// The files of `table` that are missing or differ in the folder of `exe` (names, with the reason).
pub fn mismatches_with(exe: &Path, table: &[(&str, &str)]) -> Vec<String> {
    let dir = exe.parent().unwrap_or(Path::new("."));
    let mut out = Vec::new();
    for (name, want) in table {
        let p = dir.join(name);
        match crate::hash::sha256_file(&p) {
            Ok((got, _)) if got.eq_ignore_ascii_case(want) => {}
            Ok(_) => out.push(format!("{name} differs")),
            Err(_) => out.push(format!("{name} is missing")),
        }
    }
    out
}

/// Checks the netcoredbg next to `exe` against the pin. Ok(None): it is the pinned release. Ok(Some(warning)): an
/// override that is not; it may start. Err (tool_missing, reason `netcoredbg_unpinned`): a pinned origin that is not.
pub fn check(exe: &Path, origin: Origin) -> Result<Option<String>, DebugError> {
    check_with(exe, origin, FILES)
}

pub fn check_with(exe: &Path, origin: Origin, table: &[(&str, &str)]) -> Result<Option<String>, DebugError> {
    let bad = mismatches_with(exe, table);
    if bad.is_empty() {
        return Ok(None);
    }
    let what = format!("{} is not netcoredbg {VERSION}, the release Kiln is tested with ({})", exe.display(), bad.join(", "));
    match origin {
        Origin::Pinned => Err(DebugError::new(
            DebugCode::ToolMissing,
            format!("{what}: put the win64 build of netcoredbg {VERSION} there (docs/kiln/modding/debugging.md, Set up), or name another copy with KILN_NETCOREDBG"),
        )
        .because("netcoredbg_unpinned")),
        Origin::Override => Ok(Some(format!("{what}; it was chosen explicitly, so it starts, but detaching safely is only tested with {VERSION}"))),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn folder(files: &[(&str, &[u8])]) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("kiln-ncd-{}-{}", std::process::id(), files.len()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        for (n, b) in files {
            std::fs::write(d.join(n), b).unwrap();
        }
        d
    }

    #[test]
    fn the_pin_is_the_zip_scripts_and_names_every_file() {
        let script = include_str!("../../../../tools/vscode/scripts/fetch-netcoredbg.mjs");
        assert!(script.contains(&format!("const VERSION = \"{VERSION}\"")), "fetch-netcoredbg.mjs pins another release");
        assert!(script.contains(ZIP_SHA256));
        for (name, sha) in FILES {
            assert!(script.contains(&format!("\"{name}\"")), "{name} is not in fetch-netcoredbg.mjs's FILES");
            assert_eq!(sha.len(), 64);
        }
        assert_eq!(FILES.len(), 7);
    }

    #[test]
    fn a_pinned_place_must_match_and_an_override_only_warns() {
        let d = folder(&[("netcoredbg.exe", b"exe"), ("dbgshim.dll", b"shim")]);
        let exe = d.join("netcoredbg.exe");
        let sha = crate::hash::sha256_hex;
        let (e, s) = (sha(b"exe"), sha(b"shim"));
        let table = [("netcoredbg.exe", e.as_str()), ("dbgshim.dll", s.as_str())];
        assert_eq!(check_with(&exe, Origin::Pinned, &table).unwrap(), None);
        // the real pin: these files are not netcoredbg 3.2.0-1092
        let err = check(&exe, Origin::Pinned).unwrap_err();
        assert_eq!((err.code, err.reason.as_deref()), (DebugCode::ToolMissing, Some("netcoredbg_unpinned")));
        assert!(err.message.contains("netcoredbg.exe differs") && err.message.contains("ManagedPart.dll is missing"), "{}", err.message);
        let warn = check(&exe, Origin::Override).unwrap().expect("a warning");
        assert!(warn.contains("chosen explicitly"), "{warn}");
        std::fs::write(&exe, b"changed").unwrap();
        assert!(check_with(&exe, Origin::Pinned, &table).is_err());
        let _ = std::fs::remove_dir_all(&d);
    }
}
