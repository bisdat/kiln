// SPDX-License-Identifier: MIT
//! Save-sensitive packages: Kiln's shipped policy (`schema/sfi/sfi-safety-policy.json`, format 1) and the two rules
//! built on it.
//!
//! **Generic on purpose.** The save-sensitive warning is for "every backend": hosted and installed ShroudForge get the
//! save-sensitive warning too, through kiln-manage's activation. Nothing
//! in this file knows the interpreter, ShroudForge's runtime or sfi.json: it reads a policy, matches a package folder
//! against it, and compares two starts. It can move into a shared crate as it is; until then only kiln-sfi uses it.
//!
//! **Matching** is exact (the schema's rule): the manifest id byte for byte, the version byte for byte, and every
//! pinned file of one variant with its SHA-256 and size. Never a display name, a substring, a pattern or a range. A
//! pinned path is looked up on disk ignoring ASCII case, as Windows does: `Ammo-Slots.dll` is
//! `ammo-slots.dll`. On a case-sensitive disk with two names differing only in case, either one matching is enough
//! (the warning is the conservative side).

use sha2::{Digest, Sha256};
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};

/// One pinned file of a variant.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Pinned {
    /// Relative to the package folder, `/` separators.
    pub path: String,
    pub sha256: [u8; 32],
    pub size: u64,
}

/// One policy row.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Row {
    pub id: String,
    pub version: String,
    pub name: String,
    /// The line every warning about it adds.
    pub text: String,
    pub variants: Vec<Vec<Pinned>>,
}

/// The shipped policy.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Policy {
    pub revision: u64,
    /// `scope`: `every-backend` or `interpreter-only`.
    pub every_backend: bool,
    /// `unknownMixedDataNative: save-sensitive`: data plus native code and no row counts as save-sensitive.
    pub unknown_mixed_sensitive: bool,
    pub rows: Vec<Row>,
}

/// What the policy says about one package.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Verdict {
    /// An exact row matched.
    Policy {
        id: String,
        version: String,
        text: String,
    },
    /// No row; data plus native code, and the policy treats that as save-sensitive.
    UnknownMixed,
    NotSensitive,
}

/// A package as the matcher sees it.
pub struct PackageFiles<'a> {
    pub id: &'a str,
    pub version: &'a str,
    pub dir: &'a Path,
    /// It changes game data (startup data, an archive patch).
    pub has_data: bool,
    /// It brings native code (a DLL).
    pub has_native: bool,
}

fn hex32(text: &str) -> Option<[u8; 32]> {
    if text.len() != 64
        || !text
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
    {
        return None;
    }
    let mut out = [0u8; 32];
    for (i, byte) in out.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&text[i * 2..i * 2 + 2], 16).ok()?;
    }
    Some(out)
}
fn safe_path(path: &str) -> bool {
    !path.is_empty()
        && path.len() <= 260
        && path.split('/').all(|part| {
            !part.is_empty()
                && part != "."
                && part != ".."
                && part
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'_' || b == b'-')
        })
}

impl Policy {
    /// Reads a policy file. Refuses what the schema refuses that matters for matching (format, rows without a
    /// manifest pin, bad hashes or paths), and the reader rules the schema cannot say: one row per id+version, one
    /// entry per path in a variant (ASCII case ignored).
    pub fn parse(bytes: &[u8]) -> Result<Self, String> {
        let doc: serde_json::Value =
            serde_json::from_slice(bytes).map_err(|e| format!("not JSON: {e}"))?;
        if doc["format"].as_u64() != Some(1) {
            return Err("format is not 1".into());
        }
        let revision = doc["revision"]
            .as_u64()
            .filter(|&r| r >= 1)
            .ok_or("revision")?;
        let every_backend = match doc["scope"].as_str() {
            Some("every-backend") => true,
            Some("interpreter-only") => false,
            _ => return Err("scope".into()),
        };
        let unknown_mixed_sensitive = match doc["unknownMixedDataNative"].as_str() {
            Some("save-sensitive") => true,
            Some("listed-only") => false,
            _ => return Err("unknownMixedDataNative".into()),
        };
        let mut rows: Vec<Row> = Vec::new();
        for row in doc["packages"].as_array().ok_or("packages")? {
            let text = |key: &str| {
                row[key]
                    .as_str()
                    .filter(|s| !s.is_empty())
                    .map(str::to_owned)
            };
            let id = text("id").ok_or("a row without id")?;
            let version = text("version").ok_or("a row without version")?;
            if rows.iter().any(|r| r.id == id && r.version == version) {
                return Err(format!("{id} {version} listed twice"));
            }
            let mut variants = Vec::new();
            for variant in row["variants"]
                .as_array()
                .filter(|v| !v.is_empty())
                .ok_or("a row without variants")?
            {
                let mut files: Vec<Pinned> = Vec::new();
                for file in variant["files"]
                    .as_array()
                    .filter(|f| !f.is_empty())
                    .ok_or("a variant without files")?
                {
                    let path = file["path"]
                        .as_str()
                        .filter(|p| safe_path(p))
                        .ok_or("a bad pinned path")?;
                    if files.iter().any(|f| f.path.eq_ignore_ascii_case(path)) {
                        return Err(format!("{id}: {path} twice in one variant"));
                    }
                    files.push(Pinned {
                        path: path.to_owned(),
                        sha256: file["sha256"]
                            .as_str()
                            .and_then(hex32)
                            .ok_or("a bad sha256")?,
                        size: file["size"].as_u64().ok_or("a bad size")?,
                    });
                }
                if !files.iter().any(|f| f.path == "mod.json") {
                    return Err(format!("{id}: a variant that does not pin mod.json"));
                }
                variants.push(files);
            }
            rows.push(Row {
                name: text("name").unwrap_or_else(|| id.clone()),
                text: text("text").unwrap_or_default(),
                id,
                version,
                variants,
            });
        }
        Ok(Self {
            revision,
            every_backend,
            unknown_mixed_sensitive,
            rows,
        })
    }

    /// The exact row for `package`, if one matches.
    pub fn matching(&self, package: &PackageFiles) -> Option<&Row> {
        self.rows.iter().find(|row| {
            row.id == package.id
                && row.version == package.version
                && row
                    .variants
                    .iter()
                    .any(|files| files.iter().all(|f| on_disk(package.dir, f)))
        })
    }

    pub fn classify(&self, package: &PackageFiles) -> Verdict {
        if let Some(row) = self.matching(package) {
            return Verdict::Policy {
                id: row.id.clone(),
                version: row.version.clone(),
                text: row.text.clone(),
            };
        }
        if self.unknown_mixed_sensitive && package.has_data && package.has_native {
            Verdict::UnknownMixed
        } else {
            Verdict::NotSensitive
        }
    }
}

/// Every file under `dir` whose relative path equals `path` ignoring ASCII case.
pub fn find_ignoring_case(dir: &Path, path: &str) -> Vec<PathBuf> {
    let mut found = vec![dir.to_path_buf()];
    for part in path.split('/') {
        let mut next = Vec::new();
        for parent in &found {
            let Ok(entries) = fs::read_dir(parent) else {
                continue;
            };
            for entry in entries.flatten() {
                if entry
                    .file_name()
                    .to_str()
                    .is_some_and(|name| name.eq_ignore_ascii_case(part))
                {
                    next.push(entry.path());
                }
            }
        }
        found = next;
        if found.is_empty() {
            break;
        }
    }
    found
}

/// One of the files at `pinned.path` (case ignored) is a plain file with its size and SHA-256.
fn on_disk(dir: &Path, pinned: &Pinned) -> bool {
    find_ignoring_case(dir, &pinned.path).iter().any(|file| {
        fs::metadata(file).is_ok_and(|m| m.is_file() && m.len() == pinned.size)
            && sha256(file) == Some(pinned.sha256)
    })
}

fn sha256(file: &Path) -> Option<[u8; 32]> {
    let mut reader = fs::File::open(file).ok()?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0u8; 64 * 1024];
    loop {
        let n = reader.read(&mut buffer).ok()?;
        if n == 0 {
            break;
        }
        hasher.update(&buffer[..n]);
    }
    Some(hasher.finalize().into())
}

/// What a package is in this start compared with the last: the second warning trigger.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Now {
    Selected,
    Disabled,
    OtherSide,
    Absent,
}

/// The save-sensitive packages active at the previous start that are now disabled or gone (the second warning
/// trigger). `previous` holds one entry per such package (`id_of` gives its manifest id); `now` says what each id is
/// in this start. Other-side and still-selected packages are not turned off.
pub fn turned_off<T>(
    previous: &[T],
    id_of: impl Fn(&T) -> &str,
    now: impl Fn(&str) -> Now,
) -> Vec<(&T, Now)> {
    previous
        .iter()
        .filter_map(|p| match now(id_of(p)) {
            state @ (Now::Disabled | Now::Absent) => Some((p, state)),
            Now::Selected | Now::OtherSide => None,
        })
        .collect()
}
