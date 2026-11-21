// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (both pins): modules/modloader/calculate_mod_config_revision.rs:1-3,
// read_mod_config_revision.rs:7-12, calculate_manifest_revision.rs:3-56,
// calculate_preparation_fingerprint.rs:5-63 (1.8 excludes .log at :48,76-79),
// update_mod_configuration.rs:6-18; mod-runtime/ingame/start_ingame_runtime.rs:76-84.
//! Pure identities/proposals. No executable discovery, SF state, cache or durable writer.
use super::{Package, discover::locale_files, edit, manifest::Manifest};
use crate::json::{self, Value};
use crate::source::{PackageSource, SourceKind, SourceVersion};
use std::{
    io,
    time::{SystemTime, UNIX_EPOCH},
};

pub fn config_bytes(manifest: &Manifest) -> Vec<u8> {
    let config = Value::Object(json::Map::from_iter([
        ("enabled".to_owned(), Value::Bool(manifest.enabled)),
        ("settings".to_owned(), setting_values(manifest)),
    ]));
    serde_json::to_vec(&config).expect("JSON value serializes")
}
/// `settingValues` in the order SF holds them (preserve_order: insertion order). A build
/// with that feature already has it. Without it serde_json sorted the keys, and a parsed
/// manifest's values were inserted in settings order: restore that, then any other keys.
fn setting_values(manifest: &Manifest) -> Value {
    let values = Value::from(&manifest.setting_values);
    let Value::Object(sorted) = &values else {
        return values;
    };
    if json::serde_maps_keep_order() {
        return values;
    }
    let mut ordered: json::Map = manifest
        .settings
        .iter()
        .filter_map(|s| Some((s.key.clone(), sorted.get(&s.key)?.clone())))
        .collect();
    for (key, value) in sorted {
        if !ordered.contains_key(key) {
            ordered.insert(key.clone(), value.clone());
        }
    }
    Value::Object(ordered)
}
pub fn config_revision(manifest: &Manifest) -> String {
    revision(&config_bytes(manifest))
}
pub fn loaded_bytes(manifest: &Manifest) -> Vec<u8> {
    if json::serde_maps_keep_order() {
        return serde_json::to_vec(manifest).expect("normalized manifest serializes");
    }
    // Struct fields and preset maps already serialize in order; only settingValues is a
    // sorted serde_json map here. Splice the ordered one into its place (no reparse, so no
    // float goes through text twice). Only id, name, version and enabled precede the field,
    // and a JSON string cannot hold an unescaped quote, so the first match is the field.
    let mut blank = manifest.clone();
    blank.setting_values = serde_json::Value::Null;
    let bytes = serde_json::to_vec(&blank).expect("normalized manifest serializes");
    let field = b",\"settingValues\":";
    let at = bytes
        .windows(field.len() + 4)
        .position(|w| w.starts_with(field) && w.ends_with(b"null"))
        .expect("settingValues follows enabled")
        + field.len();
    let mut out = bytes[..at].to_vec();
    out.extend(serde_json::to_vec(&setting_values(manifest)).expect("JSON value serializes"));
    out.extend(&bytes[at + 4..]);
    out
}
pub fn loaded_revision(manifest: &Manifest) -> String {
    revision(&loaded_bytes(manifest))
}
/// Read the ORIGINAL unprofiled source, not the effective runtime overlay source.
pub fn manifest_bytes(original: &dyn PackageSource) -> Result<Vec<u8>, String> {
    let mut bytes = Vec::new();
    for name in ["mod.json", "extended.mod.json"] {
        bytes.extend(name.as_bytes());
        bytes.push(0);
        match original.read_file(name) {
            Ok(b) => bytes.extend(b),
            Err(e) if e.kind() == io::ErrorKind::NotFound => bytes.push(0xff),
            Err(e) => return Err(e.to_string()),
        }
        bytes.push(0);
    }
    for entry in locale_files(original)? {
        bytes.extend(entry.path.as_bytes());
        bytes.push(0);
        bytes.extend(
            original
                .read_file(&entry.path)
                .map_err(|e| format!("{}: {e}", entry.path))?,
        );
        bytes.push(0);
    }
    Ok(bytes)
}
pub fn manifest_revision(original: &dyn PackageSource) -> Result<String, String> {
    manifest_bytes(original).map(|b| revision(&b))
}
pub fn check_revision(current: &Manifest, expected: &str) -> Result<(), String> {
    if config_revision(current) != expected {
        return Err("Mod settings changed since they were displayed. Reload before saving".into());
    }
    Ok(())
}
/// An optimistic comparison followed by the EXISTING byte-preserving editor.
/// The host must revalidate/serialize publication; this function writes nothing.
pub fn propose_edit(
    current: &Package,
    expected: &str,
    change: edit::Edit<'_>,
) -> Result<Vec<u8>, String> {
    check_revision(&current.manifest, expected)?;
    edit::apply(
        current.documents.manifest.bytes(),
        current.documents.extension.as_ref().map(|d| d.bytes()),
        change,
    )
}
#[derive(Clone, Copy, Debug)]
pub struct ExecutableFacts {
    pub length: u64,
    pub modified: SystemTime,
}
pub enum PreparationSource<'a> {
    Directory(&'a dyn PackageSource),
    Archive(&'a [u8]),
}
pub struct PreparationPackage<'a> {
    pub manifest: &'a Manifest,
    pub source: PreparationSource<'a>,
}
/// Caller supplies the enabled startup plan in dependency order and executable
/// facts. File digests are recomputed: SF's cache is infrastructure, not identity.
pub fn preparation_bytes(
    server: bool,
    executable: ExecutableFacts,
    plan: &[PreparationPackage<'_>],
    version: SourceVersion,
) -> Result<Vec<u8>, String> {
    let mut bytes = if server {
        b"server".to_vec()
    } else {
        b"client".to_vec()
    };
    bytes.extend(executable.length.to_le_bytes());
    bytes.extend(
        executable
            .modified
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos()
            .to_le_bytes(),
    );
    for package in plan {
        if !package.manifest.requires_pregame() {
            continue;
        }
        bytes.extend(loaded_bytes(package.manifest));
        match package.source {
            PreparationSource::Archive(raw) => bytes.extend(digest(raw)),
            PreparationSource::Directory(source) => {
                let mut files = Vec::new();
                walk(source, "", &mut files)?;
                // SF sorts std::path::PathBufs, whose ordering compares
                // components (a/file precedes a-sibling), not UTF-8 path strings.
                files.sort_by(|a, b| std::path::Path::new(a).cmp(std::path::Path::new(b)));
                for path in files {
                    if version == SourceVersion::Sf180
                        && path
                            .rsplit_once('.')
                            .is_some_and(|(_, ext)| ext.eq_ignore_ascii_case("log"))
                    {
                        continue;
                    }
                    // SF hashes std::path relative bytes, not camino package separators.
                    bytes.extend(path.replace('/', std::path::MAIN_SEPARATOR_STR).as_bytes());
                    bytes.extend(digest(&source.read_file(&path).map_err(|e| e.to_string())?));
                }
            }
        }
    }
    Ok(bytes)
}
fn walk(source: &dyn PackageSource, path: &str, files: &mut Vec<String>) -> Result<(), String> {
    for entry in source.read_dir(path).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        if entry.name.starts_with('.') {
            continue;
        }
        match entry.kind {
            SourceKind::Directory => walk(source, &entry.path, files)?,
            SourceKind::File => files.push(entry.path),
            _ => {}
        } // WalkDir does not follow links.
    }
    Ok(())
}
pub fn preparation_fingerprint(
    server: bool,
    executable: ExecutableFacts,
    plan: &[PreparationPackage<'_>],
    version: SourceVersion,
) -> Result<String, String> {
    preparation_bytes(server, executable, plan, version).map(|b| revision(&b))
}

// Independent SHA-256 (FIPS 180-4), not adapted from a game/parser or GPL code.
// Kept local to package identities except the byte/digest helpers so no shared
// dependency/lock changes are needed. Pure-module Lua hashing lives in sfi-core.
pub fn digest(bytes: &[u8]) -> [u8; 32] {
    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2,
    ];
    let mut h: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
        0x5be0cd19,
    ];
    let blocks = bytes.len() / 64 + if bytes.len() % 64 < 56 { 1 } else { 2 };
    for index in 0..blocks {
        let mut block = [0u8; 64];
        for (offset, byte) in block.iter_mut().enumerate() {
            let pos = index * 64 + offset;
            *byte = if pos < bytes.len() {
                bytes[pos]
            } else if pos == bytes.len() {
                0x80
            } else {
                0
            };
        }
        if index == blocks - 1 {
            block[56..].copy_from_slice(&(bytes.len() as u64).wrapping_mul(8).to_be_bytes());
        }
        let mut w = [0u32; 64];
        for (i, b) in block.as_chunks::<4>().0.iter().enumerate() {
            w[i] = u32::from_be_bytes(*b);
        }
        for i in 16..64 {
            let a = w[i - 15];
            let b = w[i - 2];
            let s0 = a.rotate_right(7) ^ a.rotate_right(18) ^ (a >> 3);
            let s1 = b.rotate_right(17) ^ b.rotate_right(19) ^ (b >> 10);
            w[i] = w[i - 16]
                .wrapping_add(s0)
                .wrapping_add(w[i - 7])
                .wrapping_add(s1);
        }
        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut hh] = h;
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let t1 = hh
                .wrapping_add(s1)
                .wrapping_add((e & f) ^ (!e & g))
                .wrapping_add(K[i])
                .wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let t2 = s0.wrapping_add((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
        }
        for (state, value) in h.iter_mut().zip([a, b, c, d, e, f, g, hh]) {
            *state = state.wrapping_add(value);
        }
    }
    let mut result = [0; 32];
    for (chunk, n) in result.as_chunks_mut::<4>().0.iter_mut().zip(h) {
        chunk.copy_from_slice(&n.to_be_bytes());
    }
    result
}
pub fn revision(bytes: &[u8]) -> String {
    digest(bytes).iter().map(|n| format!("{n:02x}")).collect()
}
