// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (both pins, identical; 1.7.9 d8a394bc, 1.8.0 38ef448d):
// modules/modloader/check_mod_conflicts.rs:5-14 (the read), documents.rs:17-21 (the
// document), plan_mod_load_order.rs:216-227 (a failed read gives an empty plan), and
// conflicts/rules.json plus conflicts/rules.schema.json, copied byte for byte below.
//! The reader for ShroudForge's compatibility rules document. ShroudForge keeps it at
//! `<game>/shroudforge/config/compatibility/rules.json`; in Kiln mode its location is the
//! caller's choice (still open), so the reader takes the path and hard-codes none. It never
//! reads inside `<game>/shroudforge` (ShroudForge's own data, also through a link) and never
//! reads Kiln's `compat-rules.json`, which is a different format. The launch-plan producer
//! calls it; the interpreter never reads rules at run time.
use super::{
    discover::Registry,
    plan::{Plan, startup_plan},
    profiles::CompatibilityRules,
    revisions::digest,
};
use crate::same_folder::{self, Identity, identity, name_key};
use std::path::{Path, PathBuf};

/// ShroudForge's embedded default, used when the file does not exist
/// (`modules/modloader/conflicts/rules.json`).
pub const EMBEDDED_RULES: &str = "{\n  \"schemaVersion\": 1,\n  \"conflicts\": []\n}\n";
/// The schema every rules document is validated against
/// (`modules/modloader/conflicts/rules.schema.json`).
pub const EMBEDDED_RULES_SCHEMA: &str = r###"{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "required": ["schemaVersion", "conflicts"],
  "properties": {
    "schemaVersion": {"const":1},
    "conflicts": {"type":"array","items": {
      "type":"object","required":["first","second","reason"],
      "properties": {
        "first":{"$ref":"#/$defs/mod"},"second":{"$ref":"#/$defs/mod"},
        "reason":{"type":"string","minLength":1}
      },"additionalProperties":false
    }}
  },
  "$defs": {"mod":{"type":"object","required":["id","version"],"properties":{
    "id":{"type":"string","pattern":"^[A-Za-z0-9._-]+$"},"version":{"type":"string"}
  },"additionalProperties":false}},
  "additionalProperties":false
}
"###;

/// What reading the rules gave. `Error` holds ShroudForge's error text exactly; the plan
/// then is ShroudForge's (`plan_with_rules`).
#[derive(Clone, Debug)]
pub enum RulesRead {
    /// The file does not exist: ShroudForge's embedded default applies.
    Embedded(CompatibilityRules),
    /// The file was read and is valid; `sha256` is the digest of its bytes.
    File {
        rules: CompatibilityRules,
        sha256: [u8; 32],
    },
    /// Any other IO error, invalid JSON or a schema violation.
    Error(String),
}

/// Reads the rules document at `path` the way ShroudForge reads its own: NotFound uses the
/// embedded default; any other IO error is the error's own text; then strict JSON and the
/// embedded schema (`CompatibilityRules::parse`). `game_root` is the game folder whose
/// `shroudforge` folder is ShroudForge's own data: a path that resolves inside it is refused
/// (an `Error`) before it is read.
pub fn read_rules(path: &Path, game_root: &Path) -> RulesRead {
    if let Some(reason) = forbidden(path, game_root) {
        return RulesRead::Error(format!("{} was not read: {reason}", path.display()));
    }
    match std::fs::read(path) {
        Ok(bytes) => match CompatibilityRules::parse(&bytes) {
            Ok(rules) => RulesRead::File {
                rules,
                sha256: digest(&bytes),
            },
            Err(error) => RulesRead::Error(error),
        },
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            RulesRead::Embedded(embedded_rules())
        }
        Err(error) => RulesRead::Error(error.to_string()),
    }
}

/// ShroudForge's startup plan for this read: the rules applied, or, when they could not be
/// read, an empty plan with the single `__loader__` issue `compatibility-rules`.
pub fn plan_with_rules(registry: &Registry, server: bool, read: &RulesRead) -> Plan {
    match read {
        RulesRead::Embedded(rules) | RulesRead::File { rules, .. } => {
            startup_plan(registry, server, Some(rules))
        }
        RulesRead::Error(error) => Plan::compatibility_error(error.clone()),
    }
}

fn embedded_rules() -> CompatibilityRules {
    CompatibilityRules::parse(EMBEDDED_RULES.as_bytes()).expect("ShroudForge's embedded rules")
}

// Kiln mode never reads ShroudForge's own files (everything inside <game>/shroudforge) or
// Kiln's own compat-rules.json. Checked before the read on the resolved path: links and ".."
// are followed (`resolved`), so a link into the folder is refused like the folder; the
// folder's name compares without case on every platform (Windows, and Wine/Proton on Linux,
// find it in any case). Only the folder directly under the game root is refused, so a
// game that itself sits below a folder named shroudforge reads normally. A link swapped in
// between this check and the read is not caught; the caller owns the path's folder.
//
// A name alone misses other names of the folder: under Wine (so Proton) canonicalize keeps the
// spelling it is given (an 8.3 name, Z: for the game's drive) and keeps a Unix link in the
// path, and natively a local share (\\localhost\C$) is another name. So the folder is also
// known by identity: a path refused when it, or a folder on its way, is a folder of the game
// root that is ShroudForge's by name (links followed); under Wine also when Wine's own Unix name
// of the path lies in such a folder (a Unix link to a folder or file inside it). Names fold as
// NTFS and Wine fold them (`same_folder::name_key`, never only ASCII). Any error but a missing
// entry refuses.
fn forbidden(path: &Path, game_root: &Path) -> Option<&'static str> {
    const FOLDER: &str = "Kiln mode never reads files in the game's shroudforge folder";
    if path
        .file_name()
        .and_then(|n| n.to_str())
        .is_some_and(|n| name_key(n) == name_key("compat-rules.json"))
    {
        return Some("Kiln's compat-rules.json is not a ShroudForge rules document");
    }
    let (Some(real), Some(root)) = (resolved(path), resolved(game_root)) else {
        return Some("its location could not be resolved");
    };
    let mut file = real.components();
    let inside_root = root.components().all(|r| {
        file.next()
            .is_some_and(|f| same_name(f.as_os_str(), r.as_os_str()))
    });
    let first = file.next().and_then(|c| c.as_os_str().to_str());
    if inside_root && first.is_some_and(|n| name_key(n) == name_key("shroudforge")) {
        return Some(FOLDER);
    }
    let Ok(protected) = shroudforge_folders(game_root) else {
        return Some("its location could not be resolved");
    };
    if protected.is_empty() {
        return None;
    }
    let Ok(absolute) = std::path::absolute(path) else {
        return Some("its location could not be resolved");
    };
    // Every folder on the way as given and as resolved: a link resolved there may name the folder
    // otherwise (a file link to it through a local share).
    for ancestor in absolute.ancestors().chain(real.ancestors()) {
        match identity(ancestor) {
            Ok(Some(id)) if protected.iter().any(|(_, p)| *p == id) => return Some(FOLDER),
            Ok(_) => {}
            Err(_) => return Some("its location could not be resolved"),
        }
    }
    // Under Wine neither path shows a Unix link (canonicalize keeps it), so a link to a folder
    // inside ShroudForge's, or to one of its files, has none of its folders on the way: where the
    // path really is comes from Wine, for its deepest existing folder or file.
    if same_folder::under_wine() {
        let mut existing = None;
        for ancestor in absolute.ancestors() {
            match ancestor.try_exists() {
                Ok(true) => {
                    existing = Some(ancestor);
                    break;
                }
                Ok(false) => {}
                Err(_) => return Some("its location could not be resolved"),
            }
        }
        let Some(existing) = existing else {
            return Some("its location could not be resolved");
        };
        for (folder, _) in &protected {
            match same_folder::wine_within(existing, folder) {
                Ok(false) => {}
                Ok(true) => return Some(FOLDER),
                Err(_) => return Some("its location could not be resolved"),
            }
        }
    }
    None
}

// The identities of the game root's folders that are ShroudForge's by name (any case the game,
// Windows or Wine finds it in), links followed. A missing game root has none.
fn shroudforge_folders(game_root: &Path) -> std::io::Result<Vec<(PathBuf, Identity)>> {
    let entries = match std::fs::read_dir(game_root) {
        Ok(entries) => entries,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(e),
    };
    let mut found = Vec::new();
    for entry in entries {
        let entry = entry?;
        if entry
            .file_name()
            .to_str()
            .is_some_and(|n| name_key(n) == name_key("shroudforge"))
            && let Some(id) = identity(&entry.path())?
        {
            found.push((entry.path(), id));
        }
    }
    Ok(found)
}

// Path components as the platform compares them: folded as NTFS and Wine fold names on
// Windows.
fn same_name(a: &std::ffi::OsStr, b: &std::ffi::OsStr) -> bool {
    if cfg!(windows) {
        match (a.to_str(), b.to_str()) {
            (Some(a), Some(b)) => name_key(a) == name_key(b),
            _ => a == b,
        }
    } else {
        a == b
    }
}

// The absolute path with every link and ".." resolved: the longest existing ancestor is
// canonicalized and the rest (which does not exist, so holds no link) is appended, its ".."
// taken lexically. None only when no ancestor can be canonicalized.
fn resolved(path: &Path) -> Option<std::path::PathBuf> {
    use std::path::Component;
    let absolute = std::path::absolute(path).ok()?;
    let mut existing = absolute.as_path();
    let mut rest = Vec::new();
    loop {
        if let Ok(mut base) = std::fs::canonicalize(existing) {
            for part in rest.iter().rev() {
                match part {
                    Component::ParentDir => {
                        base.pop();
                    }
                    Component::CurDir => {}
                    other => base.push(other.as_os_str()),
                }
            }
            return Some(base);
        }
        rest.push(existing.components().next_back()?);
        existing = existing.parent()?;
    }
}
