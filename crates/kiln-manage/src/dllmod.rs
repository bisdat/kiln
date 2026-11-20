//! Packages for AndyPimms' Enshrouded ModLoader (github.com/aebischer/Enshrouded-ModLoader, ABI 1: CarryOn, Belt
//! Lantern), the Library kind `dllmod` that kiln-dllmod runs (docs/kiln/engines/dllmod.md). A package is a 64-bit DLL
//! with a manifest named after it plus `.mod` beside it, whose first line is `ENSHROUDED_PACKAGE_V1`. Kiln's own reader of
//! that format, the same rules as native/plugins/dllmod/src/package.h (no code of the loader is used).
//!
//! The Library keeps a package folder as it is; activation copies the profile's enabled entries to
//! `<game>/Mods/EnshroudedModLoader/<library id>/` ([`RUN_FOLDER`]), where kiln-dllmod and that loader both read them.

use std::path::{Path, PathBuf};

/// Where Kiln's copies of the packages go in the game folder (engine_mods::run_folder): the folder Enshrouded
/// ModLoader 0.6.12 reads its packages from, so its own MODS page sees Kiln's copies when it is installed.
pub const RUN_FOLDER: &str = "Mods/EnshroudedModLoader";
pub const HEADER: &str = "ENSHROUDED_PACKAGE_V1";
pub const INITIALIZE: &str = "EnshroudedModInitialize";
pub const CONFIGURE: &str = "EnshroudedModConfigure";
const MANIFEST_MAX: u64 = 16 * 1024;

/// One setting a package declares (`setting=key|type|label|default...`).
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Setting {
    pub key: String,
    /// bool | choice | percent
    #[serde(rename = "type")]
    pub kind: String,
    pub label: String,
    pub default: String,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub choices: Vec<String>,
    /// percent: (min, max, step)
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub range: Option<(u32, u32, u32)>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Manifest {
    /// kind=mod (else kind=library: loads when a mod requires it)
    pub is_mod: bool,
    pub version: String,
    pub settings: Vec<Setting>,
    /// (package id, exact version)
    pub requires: Vec<(String, String)>,
}

fn only(s: &str, allowed: &str) -> bool {
    s.chars().all(|c| allowed.contains(c))
}

fn integer(s: &str) -> Option<u32> {
    if s.is_empty() || s.len() > 5 || (s.len() > 1 && s.starts_with('0')) || !s.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    s.parse::<u32>().ok().filter(|v| *v <= 10000)
}

fn field(s: &str) -> bool {
    s.len() <= 100 && !s.contains(['=', '\t', '\r', '\n', '\0'])
}

/// A package version: 1 to 64 of A-Z a-z 0-9 . _ -
pub fn valid_version(v: &str) -> bool {
    !v.is_empty() && v.len() <= 64 && v.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-'))
}

/// A package id: the DLL's path below the package root, lower case with '/'; None for a path that is no package id.
pub fn package_id(rel: &str) -> Option<String> {
    let id: String = rel.replace('\\', "/").to_ascii_lowercase();
    if id.len() < 5 || id.len() > 1024 || id.starts_with('/') || !id.ends_with(".dll") {
        return None;
    }
    for part in id.split('/') {
        if part.is_empty() || part == "." || part == ".." || part.ends_with(['.', ' '])
            || part.contains(['<', '>', '"', '|', '?', '*', ':', '\t', '\r', '\n', '\0'])
        {
            return None;
        }
    }
    Some(id)
}

/// Parses a manifest's text. Err with a short reason when the package format refuses it (the loader's own rules:
/// header first; kind, version and abi=1 once each; setting, text and require lines in their form; nothing else).
pub fn parse(text: &str) -> Result<Manifest, String> {
    if text.len() as u64 > MANIFEST_MAX {
        return Err("the manifest is larger than 16 KiB".into());
    }
    let text = text.strip_prefix('\u{feff}').unwrap_or(text);
    let mut lines = text.split('\n').map(|l| l.strip_suffix('\r').unwrap_or(l));
    if lines.next() != Some(HEADER) {
        return Err(format!("its first line is not {HEADER}"));
    }
    let mut m = Manifest::default();
    let (mut kind, mut version, mut abi) = (false, false, false);
    let mut texts: Vec<(String, String)> = Vec::new();
    for (n, line) in lines.enumerate() {
        if line.is_empty() {
            continue;
        }
        let at = format!(" (line {})", n + 2);
        let Some((k, v)) = line.split_once('=') else { return Err(format!("a line without '='{at}")) };
        match k {
            "kind" if !kind && (v == "mod" || v == "library") => {
                kind = true;
                m.is_mod = v == "mod";
            }
            "version" if !version && valid_version(v) => {
                version = true;
                m.version = v.to_string();
            }
            "abi" if !abi && v == "1" => abi = true,
            "kind" | "version" | "abi" => {
                return Err(format!("{k} must be given once{}{at}", if k == "abi" { ", and Kiln knows abi 1 only" } else { "" }));
            }
            "setting" => {
                if m.settings.len() >= 16 {
                    return Err(format!("more than 16 settings{at}"));
                }
                let parts: Vec<&str> = v.split('|').collect();
                if parts.len() < 4 || parts[0].is_empty() || parts[0].len() > 40 || !only(parts[0], "abcdefghijklmnopqrstuvwxyz0123456789_")
                    || parts[2].is_empty() || !parts.iter().all(|p| field(p))
                {
                    return Err(format!("a setting line is not key|type|label|default...{at}"));
                }
                if m.settings.iter().any(|s| s.key == parts[0]) {
                    return Err(format!("the setting {} is given twice{at}", parts[0]));
                }
                let mut s = Setting { key: parts[0].into(), kind: parts[1].into(), label: parts[2].into(), default: parts[3].into(), choices: vec![], range: None };
                match (parts[1], parts.len()) {
                    ("bool", 4) if s.default == "true" || s.default == "false" => {}
                    ("choice", 6..=12) => {
                        let choices: Vec<String> = parts[4..].iter().map(|c| c.to_string()).collect();
                        let mut seen = std::collections::HashSet::new();
                        if choices.iter().any(|c| c.is_empty() || !seen.insert(c.clone())) || !choices.contains(&s.default) {
                            return Err(format!("the choices of {} are empty, repeat or miss its default{at}", s.key));
                        }
                        s.choices = choices;
                    }
                    ("percent", 7) => {
                        let (Some(d), Some(lo), Some(hi), Some(step)) = (integer(parts[3]), integer(parts[4]), integer(parts[5]), integer(parts[6])) else {
                            return Err(format!("the percent setting {} has a bad number{at}", s.key));
                        };
                        if step < 1 || hi <= lo || (hi - lo) % step != 0 || (hi - lo) / step > 31 || d < lo || d > hi || (d - lo) % step != 0 {
                            return Err(format!("the percent setting {} has a bad default, range or step{at}", s.key));
                        }
                        s.range = Some((lo, hi, step));
                    }
                    _ => return Err(format!("the setting {} has an unknown type or the wrong number of fields{at}", s.key)),
                }
                m.settings.push(s);
            }
            "text" => {
                let parts: Vec<&str> = v.split('|').collect();
                if parts.len() != 3 || parts[0].is_empty() || parts[0].len() > 16 || !only(parts[0], "abcdefghijklmnopqrstuvwxyz0123456789-")
                    || parts[1].is_empty() || parts[1].len() > 100 || !only(parts[1], "abcdefghijklmnopqrstuvwxyz0123456789_.")
                    || parts[2].is_empty() || !field(parts[2])
                    || texts.iter().any(|(l, key)| l == parts[0] && key == parts[1])
                {
                    return Err(format!("a text line is not language|key|label, or repeats{at}"));
                }
                texts.push((parts[0].into(), parts[1].into()));
            }
            "require" => {
                let Some((path, ver)) = v.rsplit_once('@') else { return Err(format!("a require line is not <package>@<version>{at}")) };
                let (Some(id), true) = (package_id(path), valid_version(ver)) else {
                    return Err(format!("a require line names no package path or no version{at}"));
                };
                if m.requires.len() >= 64 || m.requires.iter().any(|(r, _)| *r == id) {
                    return Err(format!("more than 64 required packages, or one twice{at}"));
                }
                m.requires.push((id, ver.into()));
            }
            _ => return Err(format!("unknown key \"{k}\"{at}")),
        }
    }
    if !kind || !version || !abi {
        return Err("kind, version and abi are all needed".into());
    }
    if !m.is_mod && !m.settings.is_empty() {
        return Err("a library package has no settings".into());
    }
    for (lang, key) in &texts {
        let known = m.settings.iter().any(|s| *key == format!("setting.{}", s.key) || (0..s.choices.len()).any(|i| *key == format!("choice.{}.{i}", s.key)));
        if !known {
            return Err(format!("the text {key} ({lang}) names no setting"));
        }
    }
    Ok(m)
}

/// The manifest file of a DLL: the DLL's name plus `.mod` (`carryon.dll.mod`).
pub fn manifest_path(dll: &Path) -> PathBuf {
    let mut p = dll.as_os_str().to_owned();
    p.push(".mod");
    PathBuf::from(p)
}

/// Whether the file beside a DLL starts with the package header (the first line only: cheap, for detection).
pub fn has_header(dll: &Path) -> bool {
    use std::io::Read;
    let Ok(f) = std::fs::File::open(manifest_path(dll)) else { return false };
    let mut head = Vec::new();
    if f.take(64).read_to_end(&mut head).is_err() {
        return false;
    }
    let head = head.strip_prefix(b"\xef\xbb\xbf").unwrap_or(&head);
    head.starts_with(HEADER.as_bytes()) && matches!(head.get(HEADER.len()), None | Some(b'\r') | Some(b'\n'))
}

/// Reads and parses a DLL's manifest.
pub fn read(dll: &Path) -> Result<Manifest, String> {
    let path = manifest_path(dll);
    let meta = std::fs::symlink_metadata(&path).map_err(|_| format!("no {} beside it", file_name(&path)))?;
    if !meta.is_file() || meta.len() > MANIFEST_MAX {
        return Err(format!("{} is not a file of at most 16 KiB", file_name(&path)));
    }
    let bytes = std::fs::read(&path).map_err(|e| format!("{}: {e}", file_name(&path)))?;
    let text = String::from_utf8(bytes).map_err(|_| format!("{} is not UTF-8", file_name(&path)))?;
    parse(&text).map_err(|e| format!("{}: {e}", file_name(&path)))
}

fn file_name(p: &Path) -> String {
    p.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default()
}

/// The package DLLs directly in a folder (a DLL with a manifest whose first line is the header), by name.
pub fn packages_in(dir: &Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = std::fs::read_dir(dir).into_iter().flatten().flatten().map(|e| e.path())
        .filter(|p| p.is_file() && !p.is_symlink() && p.extension().is_some_and(|x| x.eq_ignore_ascii_case("dll")) && has_header(p))
        .collect();
    v.sort();
    v
}

/// What a package folder holds: its kind=mod packages and its kind=library ones (each DLL with its manifest).
pub struct Folder {
    pub mods: Vec<(PathBuf, Manifest)>,
    pub libraries: Vec<(PathBuf, Manifest)>,
}

/// The packages of a folder, or why Kiln cannot use it: a manifest the format refuses, a file that is no 64-bit DLL, a
/// mod without `EnshroudedModInitialize`, settings without `EnshroudedModConfigure`, a library that exports Initialize,
/// no package at all.
pub fn read_folder(dir: &Path) -> Result<Folder, String> {
    let mut f = Folder { mods: vec![], libraries: vec![] };
    for dll in packages_in(dir) {
        let shown = file_name(&dll);
        let m = read(&dll)?;
        let bytes = std::fs::read(&dll).map_err(|e| format!("{shown}: {e}"))?;
        if !crate::pe::is_pe64_dll(&bytes) {
            return Err(format!("{shown} is not a 64-bit Windows DLL"));
        }
        let exports = crate::pe::parse(&bytes).map(|i| i.exports).unwrap_or_default();
        let has = |e: &str| exports.iter().any(|x| x == e);
        if m.is_mod && !has(INITIALIZE) {
            return Err(format!("{shown} is a mod package without the export {INITIALIZE}"));
        }
        if m.is_mod && !m.settings.is_empty() && !has(CONFIGURE) {
            return Err(format!("{shown} has settings but no export {CONFIGURE}"));
        }
        if !m.is_mod && has(INITIALIZE) {
            return Err(format!("{shown} says it is a library but exports {INITIALIZE}"));
        }
        if m.is_mod { f.mods.push((dll, m)) } else { f.libraries.push((dll, m)) }
    }
    if f.mods.is_empty() && f.libraries.is_empty() {
        return Err("no DLL with its .dll.mod manifest in it".into());
    }
    Ok(f)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn manifests_of_the_two_public_mods() {
        let carry = parse("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=0.1.50-dev\nabi=1\nsetting=move_chests|bool|Chests|true\n\
            setting=move_objects|bool|Decor|true\nsetting=move_interactive|bool|Usable objects|true\nsetting=inspect_objects|bool|Debug (object IDs)|false\n").unwrap();
        assert!(carry.is_mod && carry.version == "0.1.50-dev" && carry.settings.len() == 4);
        let lantern = parse("\u{feff}ENSHROUDED_PACKAGE_V1\r\nkind=mod\r\nversion=0.1.29-dev\r\nabi=1\r\n").unwrap();
        assert!(lantern.is_mod && lantern.settings.is_empty());
        let example = parse("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1.0.0\nabi=1\nsetting=mode|choice|Mode|Normal|Calm|Normal|Fast\n\
            setting=intensity|percent|Intensity|100|0|200|25\ntext=fr|choice.mode.2|Rapide\ntext=en|setting.intensity|Intensity\n").unwrap();
        assert_eq!(example.settings[1].range, Some((0, 200, 25)));
        let lib = parse("ENSHROUDED_PACKAGE_V1\nkind=library\nversion=1\nabi=1\nrequire=Shared/Base.dll@2\n").unwrap();
        assert_eq!(lib.requires, vec![("shared/base.dll".to_string(), "2".to_string())]);
    }

    #[test]
    fn manifests_the_format_refuses() {
        for (text, why) in [
            ("kind=mod\nversion=1\nabi=1\n", "first line"),
            ("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\n", "all needed"),
            ("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=2\n", "abi"),
            ("ENSHROUDED_PACKAGE_V1\nkind=mod\nkind=library\nversion=1\nabi=1\n", "kind"),
            ("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nauthor=x\n", "unknown key"),
            ("ENSHROUDED_PACKAGE_V1\nkind=library\nversion=1\nabi=1\nsetting=a|bool|A|true\n", "library"),
            ("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nsetting=a|percent|A|30|0|100|25\n", "percent"),
            ("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nsetting=a|choice|A|x|y|z\n", "choices"),
            ("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\ntext=en|setting.b|B\n", "names no setting"),
            ("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nrequire=../x.dll@1\n", "require"),
        ] {
            let e = parse(text).unwrap_err();
            assert!(e.contains(why), "{text:?}: {e}");
        }
        assert_eq!(package_id("CarryOn\\CarryOn.dll").as_deref(), Some("carryon/carryon.dll"));
        assert!(package_id("../x.dll").is_none() && package_id("a/x.txt").is_none() && package_id("a./x.dll").is_none());
    }
}
