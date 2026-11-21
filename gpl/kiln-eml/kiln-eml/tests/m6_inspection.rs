// SPDX-License-Identifier: GPL-3.0-or-later
//! The read-only archive inspector (kiln-eml-inspect). Synthetic archives only; no game bytes.
//! What is proven here: the BASE/input classification matrix, the role-framed digests (recomputed independently from
//! the framing spec below), the frozen BASE-origin tokens and labels, the closed schema, and that an inspection
//! creates, changes, opens for writing or locks nothing (tree snapshot with ctimes, inotify on Linux, a read-only
//! tree, and an ignored test on a read-only bind mount in a private namespace).
#[path = "support/m6_fixture/mod.rs"]
mod fixture;

use fixture::archive::{ArchiveFixture, STEM};
use kiln_eml::inspection::{self, Options, Side};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    fs,
    path::{Path, PathBuf},
    process::Command,
};

const PATCH: &str = r#"game.assets.create_content(buffer.wrap("m6 created content"))"#;
const UNCLASSIFIED: &str = "unclassified: game files as found when Kiln first managed this install";
const UNRECORDED: &str = "unrecorded: a working baseline from an earlier start; where its files came from was not recorded";
const REBASELINED: &str =
    "unclassified: game files as found when Kiln took a new baseline after the game changed";

fn fixture(tag: &str) -> ArchiveFixture {
    let f = ArchiveFixture::new(tag);
    // a stand-in executable: hashed, never run
    fs::write(
        f.game.join("enshrouded.exe"),
        b"synthetic client executable",
    )
    .unwrap();
    f
}
fn options(side: Side) -> Options {
    Options {
        side,
        hash_containers: false,
    }
}
fn inspect_at(game: &Path, side: Side) -> Value {
    serde_json::to_value(inspection::inspect(game, &options(side))).unwrap()
}
fn inspect(f: &ArchiveFixture) -> Value {
    inspect_at(f.game.as_std_path(), Side::Client)
}
fn published(tag: &str) -> ArchiveFixture {
    let f = fixture(tag);
    f.package("patch", PATCH, &["patch"]);
    let out = f.run_packages(&["patch"]);
    assert!(f.receipt().exists() && f.backup_seal().exists(), "{out:?}");
    f
}
fn sha(bytes: &[u8]) -> [u8; 32] {
    Sha256::digest(bytes).into()
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
/// The framing, written from its spec (README "Archive inspector"), not from the implementation.
fn frame(entries: &[(String, u64, Option<[u8; 32]>)]) -> String {
    let mut sorted: Vec<_> = entries.to_vec();
    sorted.sort_by(|a, b| a.0.as_bytes().cmp(b.0.as_bytes()));
    let mut h = Sha256::new();
    h.update(b"kiln-eml-inspect archive-set v1\n");
    h.update((sorted.len() as u64).to_le_bytes());
    for (member, size, digest) in &sorted {
        h.update((member.len() as u64).to_le_bytes());
        h.update(member.as_bytes());
        h.update([if digest.is_some() { 1u8 } else { 2u8 }]);
        h.update(size.to_le_bytes());
        if let Some(d) = digest {
            h.update(d);
        }
    }
    hex(&h.finalize())
}
/// The expected framed set: header and resources from \`files\` (member -> physical file), containers by size.
fn expected(game: &Path, header: &str, resources: &str, containers: usize) -> String {
    let mut entries = Vec::new();
    for (member, file) in [
        (format!("{STEM}.kfc"), header),
        (format!("{STEM}.kfc_resources"), resources),
    ] {
        let bytes = fs::read(game.join(file)).unwrap();
        entries.push((member, bytes.len() as u64, Some(sha(&bytes))));
    }
    for i in 0..containers {
        let name = format!("{STEM}_{i:03}.dat");
        entries.push((
            name.clone(),
            fs::metadata(game.join(&name)).unwrap().len(),
            None,
        ));
    }
    frame(&entries)
}
fn containers(f: &ArchiveFixture, header: &str) -> usize {
    kfc::container::KFCFile::from_path(f.game.join(header), false)
        .unwrap()
        .containers()
        .len()
}
fn file_digest(game: &Path, name: &str) -> Value {
    let bytes = fs::read(game.join(name)).unwrap();
    json!({"size": bytes.len(), "sha256": hex(&sha(&bytes))})
}
fn ok(v: &Value) {
    assert_eq!(v["status"], "ok", "{v:#}");
    assert!(v["reasons"].as_array().unwrap().is_empty(), "{v:#}");
    assert!(
        !v.to_string().to_lowercase().contains("vanilla"),
        "never called vanilla: {v:#}"
    );
}
fn refused(v: &Value, code: &str) {
    assert_eq!(v["status"], "refused", "{v:#}");
    assert!(
        v["input"].is_null() && v["base"].is_null(),
        "a refusal carries no digest: {v:#}"
    );
    let codes: Vec<_> = v["reasons"]
        .as_array()
        .unwrap()
        .iter()
        .map(|r| r["code"].as_str().unwrap())
        .collect();
    assert_eq!(codes, [code], "{v:#}");
    assert!(!v["reasons"][0]["text"].as_str().unwrap().is_empty());
}
fn notes(v: &Value) -> Vec<String> {
    v["notes"]
        .as_array()
        .unwrap()
        .iter()
        .map(|n| n["code"].as_str().unwrap().to_owned())
        .collect()
}
fn origin(v: &Value) -> (&str, &str) {
    (
        v["base"]["origin"]["token"].as_str().unwrap(),
        v["base"]["origin"]["label"].as_str().unwrap(),
    )
}
fn edit_receipt(f: &ArchiveFixture, change: impl FnOnce(&mut Value)) {
    let mut receipt: Value = serde_json::from_slice(&fs::read(f.receipt()).unwrap()).unwrap();
    change(&mut receipt);
    fs::write(f.receipt(), serde_json::to_vec(&receipt).unwrap()).unwrap();
}

#[test]
fn a_first_managed_valid_set_is_the_base_with_the_unclassified_origin() {
    let f = fixture("inspect-first");
    let v = inspect(&f);
    ok(&v);
    assert_eq!(v["schema_version"], 1);
    assert_eq!(v["target"]["side"], "client");
    assert_eq!(v["target"]["stem"], STEM);
    assert_eq!(
        v["target"]["executable"]["sha256"],
        hex(&sha(b"synthetic client executable"))
    );
    assert_eq!(v["input"]["class"], "unclassified");
    assert_eq!(v["base"]["evidence"], "current-set");
    assert_eq!(origin(&v), ("first-managed-unclassified", UNCLASSIFIED));
    assert!(
        v["base"]["baseline"].is_null(),
        "no working baseline exists yet"
    );
    let n = containers(&f, "enshrouded.kfc");
    let want = expected(
        f.game.as_std_path(),
        "enshrouded.kfc",
        "enshrouded.kfc_resources",
        n,
    );
    // equal digests are legitimate here: both sets are the same validated live files, each framed on its own
    assert_eq!(v["input"]["sha256"], want);
    assert_eq!(v["base"]["sha256"], want);
    assert_eq!(v["input"]["files"].as_array().unwrap().len(), 2 + n);
    assert_eq!(v["base"]["files"], v["input"]["files"]);
    assert_eq!(
        v["proof"],
        json!({"framing": "kiln-eml-inspect archive-set v1", "containers": "size"})
    );
    assert!(notes(&v).is_empty(), "{v:#}");
}

#[test]
fn hashed_containers_change_the_proof_kind_and_digest() {
    let f = fixture("inspect-hashed");
    let sized = inspect(&f);
    let hashed = serde_json::to_value(inspection::inspect(
        f.game.as_std_path(),
        &Options {
            side: Side::Client,
            hash_containers: true,
        },
    ))
    .unwrap();
    ok(&hashed);
    assert_eq!(hashed["proof"]["containers"], "sha256");
    assert_ne!(hashed["input"]["sha256"], sized["input"]["sha256"]);
    let mut entries = Vec::new();
    for e in hashed["input"]["files"].as_array().unwrap() {
        let bytes = fs::read(f.game.join(e["file"].as_str().unwrap())).unwrap();
        assert_eq!(e["proof"], "sha256");
        entries.push((
            e["member"].as_str().unwrap().to_owned(),
            bytes.len() as u64,
            Some(sha(&bytes)),
        ));
    }
    assert_eq!(hashed["input"]["sha256"], frame(&entries));
}

#[test]
fn exact_owned_output_over_its_receipt_baseline() {
    let f = published("inspect-owned");
    let v = inspect(&f);
    ok(&v);
    assert_eq!(v["input"]["class"], "owned-output");
    assert_eq!(v["base"]["evidence"], "receipt-baseline");
    // the EML-mode writer records no origin: unrecorded, never upgraded
    assert_eq!(origin(&v), ("unrecorded", UNRECORDED));
    let game = f.game.as_std_path();
    assert_eq!(
        v["input"]["sha256"],
        expected(
            game,
            "enshrouded.kfc",
            "enshrouded.kfc_resources",
            containers(&f, "enshrouded.kfc")
        )
    );
    assert_eq!(
        v["base"]["sha256"],
        expected(
            game,
            "enshrouded.kfc.bak",
            "enshrouded.kfc_resources.bak",
            containers(&f, "enshrouded.kfc.bak")
        )
    );
    assert_ne!(v["input"]["sha256"], v["base"]["sha256"]);
    let mut baseline = serde_json::Map::new();
    for ext in ["kfc.bak", "kfc_resources.bak", "kfc.bak.integrity"] {
        let name = format!("{STEM}.{ext}");
        baseline.insert(name.clone(), file_digest(game, &name));
    }
    assert_eq!(v["base"]["baseline"], Value::Object(baseline));
    let receipt = fs::read(f.receipt()).unwrap();
    assert_eq!(
        v["receipt"],
        json!({"state": "understood", "file": format!("kiln/eml/ownership/{STEM}.json"),
        "size": receipt.len(), "sha256": hex(&sha(&receipt))})
    );
}

#[test]
fn first_capture_then_the_writers_baseline_keeps_the_same_bytes_and_no_upgrade() {
    let f = fixture("inspect-capture");
    let first = inspect(&f);
    ok(&first);
    f.package("patch", PATCH, &["patch"]);
    f.run_packages(&["patch"]);
    let next = inspect(&f);
    ok(&next);
    assert_eq!(next["base"]["evidence"], "receipt-baseline");
    // the backups the writer made are the bytes the first inspection framed as BASE
    assert_eq!(next["base"]["sha256"], first["base"]["sha256"]);
    assert_eq!(
        next["base"]["files"]
            .as_array()
            .unwrap()
            .iter()
            .map(|e| (&e["member"], &e["sha256"]))
            .collect::<Vec<_>>(),
        first["base"]["files"]
            .as_array()
            .unwrap()
            .iter()
            .map(|e| (&e["member"], &e["sha256"]))
            .collect::<Vec<_>>()
    );
    assert_ne!(origin(&next).0, "vanilla");
}

#[test]
fn a_receipt_origin_keyed_to_this_baseline_is_kept_and_never_upgraded() {
    let f = published("inspect-origin");
    let baseline = inspect(&f)["base"]["baseline"].clone();
    edit_receipt(
        &f,
        |r| r["baseOrigin"] = json!({"token": "first-managed-unclassified", "label": UNCLASSIFIED, "baseline": baseline}),
    );
    let v = inspect(&f);
    ok(&v);
    assert_eq!(origin(&v), ("first-managed-unclassified", UNCLASSIFIED));
    assert!(notes(&v).is_empty());
    // the rebaselined origin (written after a game update re-baselined the install) is kept the same way
    edit_receipt(
        &f,
        |r| r["baseOrigin"] = json!({"token": "rebaselined-unclassified", "label": REBASELINED, "baseline": baseline}),
    );
    let v = inspect(&f);
    ok(&v);
    assert_eq!(origin(&v), ("rebaselined-unclassified", REBASELINED));
    assert!(notes(&v).is_empty());
    // keyed to another baseline, an unknown or an upgraded token, or another label: ignored, unrecorded
    let mut other = baseline.clone();
    other[format!("{STEM}.kfc.bak")]["size"] = json!(1);
    for bad in [
        json!({"token": "first-managed-unclassified", "label": UNCLASSIFIED, "baseline": other}),
        json!({"token": "vanilla", "label": "vanilla", "baseline": baseline}),
        json!({"token": "first-managed-unclassified", "label": "game files", "baseline": baseline}),
        json!({"token": "rebaselined-unclassified", "label": UNCLASSIFIED, "baseline": baseline}),
        json!({"token": "first-managed-unclassified", "label": REBASELINED, "baseline": baseline}),
        json!({"token": "rebaselined-unclassified", "label": REBASELINED, "baseline": other}),
        json!({"token": "unrecorded", "label": UNRECORDED, "baseline": baseline, "extra": 1}),
        json!("first-managed-unclassified"),
    ] {
        edit_receipt(&f, |r| r["baseOrigin"] = bad.clone());
        let v = inspect(&f);
        ok(&v);
        assert_eq!(origin(&v), ("unrecorded", UNRECORDED), "{bad}");
        assert_eq!(notes(&v), ["receipt-origin-ignored"], "{bad}");
    }
}

#[test]
fn a_sealed_working_base_without_a_receipt() {
    let f = published("inspect-sealed");
    f.forget_ownership();
    let v = inspect(&f);
    ok(&v);
    assert_eq!(v["base"]["evidence"], "sealed-baseline");
    assert_eq!(
        v["input"]["class"], "unclassified",
        "patched live files without a receipt prove nothing"
    );
    assert_eq!(origin(&v), ("unrecorded", UNRECORDED));
    assert_eq!(
        v["receipt"],
        json!({"state": "absent", "file": format!("kiln/eml/ownership/{STEM}.json")})
    );
}

#[test]
fn a_restored_install_has_equal_input_and_base_digests() {
    let f = published("inspect-restored");
    assert!(mod_loader::lua::restore(&f.game, STEM));
    let v = inspect(&f);
    ok(&v);
    assert_eq!(v["input"]["class"], "base-copy");
    assert_eq!(v["base"]["evidence"], "receipt-baseline");
    let game = f.game.as_std_path();
    let n = containers(&f, "enshrouded.kfc");
    assert_eq!(
        v["input"]["sha256"],
        expected(game, "enshrouded.kfc", "enshrouded.kfc_resources", n)
    );
    assert_eq!(
        v["base"]["sha256"],
        expected(
            game,
            "enshrouded.kfc.bak",
            "enshrouded.kfc_resources.bak",
            n
        )
    );
    assert_eq!(v["input"]["sha256"], v["base"]["sha256"]);
}

#[test]
fn a_legacy_unsealed_pair_is_checked_against_the_live_resource_prefix() {
    let f = fixture("inspect-unsealed");
    for ext in ["kfc", "kfc_resources"] {
        fs::copy(
            f.game.join(format!("{STEM}.{ext}")),
            f.game.join(format!("{STEM}.{ext}.bak")),
        )
        .unwrap();
    }
    let v = inspect(&f);
    ok(&v);
    assert_eq!(v["base"]["evidence"], "unsealed-baseline");
    assert_eq!(v["input"]["class"], "base-copy");
    assert_eq!(origin(&v), ("unrecorded", UNRECORDED));
    assert!(v["base"]["baseline"][format!("{STEM}.kfc.bak.integrity")].is_null());
    fixture::archive::corrupt_first_byte(&f.game.join(format!("{STEM}.kfc_resources.bak")));
    refused(&inspect(&f), "baseline-corrupt");
}

#[test]
fn a_corrupt_sealed_pair_is_refused() {
    let f = published("inspect-corrupt");
    fixture::archive::corrupt_first_byte(&f.game.join(format!("{STEM}.kfc_resources.bak")));
    refused(&inspect(&f), "baseline-corrupt");
    let f = published("inspect-corrupt-seal");
    fs::write(f.backup_seal(), b"{\"header\":[1,2]}").unwrap();
    refused(&inspect(&f), "baseline-corrupt");
}

#[test]
fn a_seal_left_behind_by_a_replaced_pair_is_checked_like_an_unsealed_pair() {
    let f = fixture("inspect-replaced");
    for ext in ["kfc", "kfc_resources"] {
        fs::copy(
            f.game.join(format!("{STEM}.{ext}")),
            f.game.join(format!("{STEM}.{ext}.bak")),
        )
        .unwrap();
    }
    fs::write(f.backup_seal(), b"{\"header\":[1,2],\"resources\":[3,4]}").unwrap();
    let v = inspect(&f);
    ok(&v);
    assert_eq!(v["base"]["evidence"], "unsealed-baseline");
    assert_eq!(notes(&v), ["seal-outdated"]);
}

#[test]
fn a_backup_of_another_build_leaves_the_current_set_as_the_base() {
    let f = fixture("inspect-old-build");
    let mut header = fs::read(f.game.join("enshrouded.kfc")).unwrap();
    let version = fixture::types::VERSION.as_bytes();
    let at = header
        .windows(version.len())
        .position(|w| w == version)
        .expect("version string in the header");
    header[at] ^= 1;
    fs::write(f.game.join("enshrouded.kfc.bak"), &header).unwrap();
    fs::copy(
        f.game.join("enshrouded.kfc_resources"),
        f.game.join("enshrouded.kfc_resources.bak"),
    )
    .unwrap();
    let v = inspect(&f);
    ok(&v);
    assert_eq!(v["base"]["evidence"], "current-set");
    // not "first managed": Kiln (or another tool) managed this install before the game changed
    assert_eq!(origin(&v), ("rebaselined-unclassified", REBASELINED));
    assert_eq!(notes(&v), ["baseline-outdated"]);
    assert_eq!(v["base"]["sha256"], v["input"]["sha256"]);
}

#[test]
fn incomplete_or_missing_baselines_are_refused() {
    // a header-only backup (an older EML's) stays refused; the text says what the user can do about it
    let f = fixture("inspect-header-only");
    fs::copy(
        f.game.join("enshrouded.kfc"),
        f.game.join("enshrouded.kfc.bak"),
    )
    .unwrap();
    let v = inspect(&f);
    refused(&v, "baseline-incomplete");
    assert_eq!(v["reasons"][0]["file"], "enshrouded.kfc_resources.bak");
    let text = v["reasons"][0]["text"].as_str().unwrap();
    for part in [
        "enshrouded.kfc_resources.bak is missing",
        "verify the game files in Steam",
        "move enshrouded.kfc.bak out of the game folder",
        "new baseline",
    ] {
        assert!(text.contains(part), "{part:?} in {text:?}");
    }
    let f = fixture("inspect-resources-only");
    fs::copy(
        f.game.join("enshrouded.kfc_resources"),
        f.game.join("enshrouded.kfc_resources.bak"),
    )
    .unwrap();
    fs::write(f.backup_seal(), b"{\"header\":[1,2],\"resources\":[3,4]}").unwrap();
    let v = inspect(&f);
    refused(&v, "baseline-incomplete");
    let text = v["reasons"][0]["text"].as_str().unwrap();
    assert!(
        text.contains("enshrouded.kfc.bak is missing")
            && text
                .contains("move enshrouded.kfc_resources.bak and enshrouded.kfc.bak.integrity out"),
        "{text:?}"
    );
    // The receipt names a baseline that is gone and Kiln's output is live: never a base (the rebaseline needs files
    // that are not Kiln's output; the text names the file and what to do)
    let f = published("inspect-baseline-gone");
    lose_backups(&f);
    let v = inspect(&f);
    refused(&v, "baseline-missing");
    assert_eq!(v["reasons"][0]["file"], "enshrouded.kfc");
    let text = v["reasons"][0]["text"].as_str().unwrap();
    for part in [
        "enshrouded.kfc is still Kiln's output",
        "verify the game files in Steam",
        "new baseline",
    ] {
        assert!(text.contains(part), "{part:?} in {text:?}");
    }
}

/// The three backup files a receipt names, gone (Steam's verification, a manual delete, a game update).
fn lose_backups(f: &ArchiveFixture) {
    for ext in ["kfc.bak", "kfc_resources.bak", "kfc.bak.integrity"] {
        fs::remove_file(f.game.join(format!("{STEM}.{ext}"))).unwrap();
    }
}
/// The game's own files back, byte for byte (what Steam's verification does; files it does not know stay).
fn verify_in_steam(f: &ArchiveFixture) {
    for (name, bytes) in &f.original.0 {
        fs::write(f.game.join(name), bytes).unwrap();
    }
}

// The receipt's working baseline is gone and none of the live files is output the receipt lists (the
// game files are the game's again): the live set is the BASE, rebaselined (never "first managed", never vanilla), with
// note receipt-stale; the receipt is only read. The pre-Play inspection and the boot witness share this verdict.
#[test]
fn lost_backups_over_files_that_are_not_kiln_output_are_a_rebaseline() {
    let f = published("inspect-lost-backups");
    let receipt = fs::read(f.receipt()).unwrap();
    lose_backups(&f);
    verify_in_steam(&f);
    let v = inspect(&f);
    ok(&v);
    assert_eq!(v["base"]["evidence"], "current-set");
    assert_eq!(origin(&v), ("rebaselined-unclassified", REBASELINED));
    assert_eq!(notes(&v)[0], "receipt-stale", "{v:#}");
    let text = v["notes"][0]["text"].as_str().unwrap();
    for part in [
        "no longer in the game folder",
        "recovery copy",
        "new baseline",
    ] {
        assert!(text.contains(part), "{part:?} in {text:?}");
    }
    assert_eq!(v["input"]["class"], "unclassified");
    assert!(v["base"]["baseline"].is_null());
    let n = containers(&f, "enshrouded.kfc");
    let want = expected(
        f.game.as_std_path(),
        "enshrouded.kfc",
        "enshrouded.kfc_resources",
        n,
    );
    assert_eq!(
        (&v["base"]["sha256"], &v["input"]["sha256"]),
        (&json!(want), &json!(want))
    );
    assert_eq!(v["receipt"]["state"], "understood");
    assert_eq!(
        fs::read(f.receipt()).unwrap(),
        receipt,
        "the inspector only reads the receipt"
    );
    check(&schema(), &schema(), &v, "$").unwrap_or_else(|e| panic!("{e}\n{v:#}"));
}

// After lost backups a live file that is still Kiln's output (the receipt's current or previous generation, size
// first, then SHA-256) keeps the start refused, naming it; a listed output file that is the receipt's own backup of
// that member (an output Kiln left as the game's) is no such file.
#[test]
fn lost_backups_under_any_kiln_output_file_stay_refused() {
    // the header verified, the resource file still Kiln's
    let f = published("inspect-lost-resources");
    lose_backups(&f);
    let resources = fs::read(f.game.join("enshrouded.kfc_resources")).unwrap();
    verify_in_steam(&f);
    let res_name = format!("{STEM}.kfc_resources");
    let listed = |f: &ArchiveFixture, name: &str| -> Value {
        serde_json::from_slice::<Value>(&fs::read(f.receipt()).unwrap()).unwrap()["files"][name]
            .clone()
    };
    if listed(&f, &res_name) != file_digest(f.game.as_std_path(), &res_name) {
        fs::write(f.game.join(&res_name), &resources).unwrap();
        let v = inspect(&f);
        refused(&v, "baseline-missing");
        assert_eq!(v["reasons"][0]["file"], res_name.as_str());
    }
    // the output Kiln published is only the receipt's previous generation: still Kiln's output
    let f = published("inspect-lost-previous");
    edit_receipt(&f, |r| {
        r["previous"] = r["files"].clone();
        r["files"][format!("{STEM}.kfc")]["sha256"] = json!("0".repeat(64));
    });
    lose_backups(&f);
    let v = inspect(&f);
    refused(&v, "baseline-missing");
    assert_eq!(v["reasons"][0]["file"], "enshrouded.kfc");
    // the game's files are back but the receipt lists the verified header as its own backup's bytes (an output it
    // left as the game's): not Kiln's output, a rebaseline
    let f = published("inspect-lost-unchanged-member");
    lose_backups(&f);
    verify_in_steam(&f);
    let header = file_digest(f.game.as_std_path(), "enshrouded.kfc");
    edit_receipt(&f, |r| {
        r["files"][format!("{STEM}.kfc")] = header.clone();
        r["backups"][format!("{STEM}.kfc.bak")] = header.clone();
    });
    let v = inspect(&f);
    ok(&v);
    assert_eq!(notes(&v)[0], "receipt-stale");
    // the same header listed as output without being the receipt's backup of it: Kiln's output, refused
    edit_receipt(&f, |r| {
        r["backups"][format!("{STEM}.kfc.bak")]["sha256"] = json!("1".repeat(64))
    });
    refused(&inspect(&f), "baseline-missing");
    // a leftover backup file is no lost baseline: still the incomplete-baseline refusal
    let f = published("inspect-lost-one-left");
    lose_backups(&f);
    verify_in_steam(&f);
    fs::write(f.backup_seal(), b"{}").unwrap();
    refused(&inspect(&f), "baseline-incomplete");
}

#[test]
fn an_unknown_receipt_is_refused_and_kept() {
    let f = published("inspect-unknown-receipt");
    edit_receipt(&f, |r| r["format"] = json!(2));
    let before = fs::read(f.receipt()).unwrap();
    let v = inspect(&f);
    refused(&v, "receipt-unknown");
    assert_eq!(v["receipt"]["state"], "unknown");
    assert_eq!(fs::read(f.receipt()).unwrap(), before);
}

#[test]
fn a_pending_generation_is_not_owned_output() {
    let f = published("inspect-pending");
    edit_receipt(&f, |r| r["unproven"] = json!([format!("{STEM}.kfc")]));
    let v = inspect(&f);
    ok(&v);
    assert_eq!(v["input"]["class"], "unclassified");
    assert_eq!(v["base"]["evidence"], "receipt-baseline");
    assert_eq!(notes(&v), ["pending-generation"]);
}

#[test]
fn backups_another_tool_replaced_fall_back_to_their_own_proof() {
    let f = published("inspect-receipt-mismatch");
    let base = inspect(&f)["base"]["sha256"].clone();
    edit_receipt(&f, |r| {
        let name = format!("{STEM}.kfc.bak");
        r["backups"][&name]["sha256"] = json!("0".repeat(64));
    });
    let v = inspect(&f);
    ok(&v);
    assert_eq!(v["base"]["evidence"], "sealed-baseline");
    assert_eq!(v["input"]["class"], "unclassified");
    assert_eq!(notes(&v), ["receipt-baseline-mismatch"]);
    assert_eq!(v["base"]["sha256"], base, "same bytes, weaker evidence");
}

#[test]
fn absent_or_invalid_input_files_are_refused() {
    let cases: [(&str, &str, &str); 4] = [
        ("inspect-no-exe", "enshrouded.exe", "missing"),
        (
            "inspect-no-resources",
            "enshrouded.kfc_resources",
            "missing",
        ),
        ("inspect-no-container", "enshrouded_001.dat", "missing"),
        ("inspect-no-header", "enshrouded.kfc", "missing"),
    ];
    for (tag, name, code) in cases {
        let f = fixture(tag);
        fs::remove_file(f.game.join(name)).unwrap();
        let v = inspect(&f);
        refused(&v, code);
        assert_eq!(v["reasons"][0]["file"], name);
    }
    let f = fixture("inspect-garbage");
    fs::write(f.game.join("enshrouded.kfc"), b"not an archive").unwrap();
    refused(&inspect(&f), "invalid");
    let f = fixture("inspect-short-resources");
    let resources = f.game.join("enshrouded.kfc_resources");
    let len = fs::metadata(&resources).unwrap().len();
    fs::OpenOptions::new()
        .write(true)
        .open(&resources)
        .unwrap()
        .set_len(len - 1)
        .unwrap();
    refused(&inspect(&f), "invalid");
    let f = fixture("inspect-wrong-container");
    fs::write(f.game.join("enshrouded_001.dat"), b"x").unwrap();
    refused(&inspect(&f), "invalid");
}

#[test]
fn unreferenced_containers_are_listed_and_do_not_block() {
    let f = fixture("inspect-extra-dat");
    fs::write(f.game.join("enshrouded_009.dat"), b"a player's file").unwrap();
    let v = inspect(&f);
    ok(&v);
    assert_eq!(notes(&v), ["unreferenced-containers"]);
    let retained: Vec<_> = v["inventory"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|e| e["role"] == "retained-container")
        .collect();
    assert_eq!(retained.len(), 1);
    assert_eq!(retained[0]["file"], "enshrouded_009.dat");
    assert!(
        v["input"]["files"]
            .as_array()
            .unwrap()
            .iter()
            .all(|e| e["file"] != "enshrouded_009.dat")
    );
}

#[cfg(unix)]
#[test]
fn links_are_refused() {
    let f = fixture("inspect-link");
    let header = f.game.join("enshrouded.kfc");
    let moved = f.root.0.join("elsewhere.kfc");
    fs::rename(&header, &moved).unwrap();
    std::os::unix::fs::symlink(&moved, &header).unwrap();
    refused(&inspect(&f), "link");
    let f = published("inspect-link-receipt");
    let kiln = f.game.join("kiln");
    let moved = f.root.0.join("kiln-elsewhere");
    fs::rename(&kiln, &moved).unwrap();
    std::os::unix::fs::symlink(&moved, &kiln).unwrap();
    refused(&inspect(&f), "link");
}

/// Windows: a junction on the way to the receipt (no privilege needed to make one) is a reparse point, refused.
#[cfg(windows)]
#[test]
fn a_junction_is_refused() {
    let f = published("inspect-junction");
    let kiln = f.game.join("kiln");
    let moved = f.root.0.join("kiln-elsewhere");
    fs::rename(&kiln, &moved).unwrap();
    let made = Command::new("cmd")
        .args(["/C", "mklink", "/J", kiln.as_str(), moved.as_str()])
        .output()
        .unwrap();
    assert!(
        made.status.success(),
        "{}",
        String::from_utf8_lossy(&made.stderr)
    );
    let v = inspect(&f);
    let _ = fs::remove_dir(&kiln);
    refused(&v, "link");
}

#[cfg(unix)]
#[test]
fn unreadable_input_is_refused() {
    use std::os::unix::fs::PermissionsExt;
    let f = fixture("inspect-unreadable");
    let resources = f.game.join("enshrouded.kfc_resources");
    fs::set_permissions(&resources, fs::Permissions::from_mode(0)).unwrap();
    if fs::File::open(&resources).is_ok() {
        fs::set_permissions(&resources, fs::Permissions::from_mode(0o644)).unwrap();
        panic!("running with privileges that ignore file modes: this check cannot run here");
    }
    let v = inspect(&f);
    fs::set_permissions(&resources, fs::Permissions::from_mode(0o644)).unwrap();
    refused(&v, "unreadable");
}

#[test]
fn client_and_server_files_side_by_side_are_inspected_apart() {
    let f = fixture("inspect-colocated");
    let n = containers(&f, "enshrouded.kfc");
    for (from, to) in [
        ("enshrouded.kfc", "enshrouded_server.kfc"),
        (
            "enshrouded.kfc_resources",
            "enshrouded_server.kfc_resources",
        ),
    ] {
        fs::copy(f.game.join(from), f.game.join(to)).unwrap();
    }
    for i in 0..n {
        fs::copy(
            f.game.join(format!("{STEM}_{i:03}.dat")),
            f.game.join(format!("enshrouded_server_{i:03}.dat")),
        )
        .unwrap();
    }
    refused(&inspect_at(f.game.as_std_path(), Side::Server), "missing");
    fs::write(
        f.game.join("enshrouded_server.exe"),
        b"synthetic server executable",
    )
    .unwrap();
    let client = inspect(&f);
    let server = inspect_at(f.game.as_std_path(), Side::Server);
    ok(&client);
    ok(&server);
    assert_eq!(
        (
            client["target"]["stem"].as_str(),
            server["target"]["stem"].as_str()
        ),
        (Some(STEM), Some("enshrouded_server"))
    );
    assert_eq!(server["target"]["side"], "server");
    assert_ne!(
        client["input"]["sha256"], server["input"]["sha256"],
        "members are named by stem"
    );
    for (v, stem) in [(&client, STEM), (&server, "enshrouded_server")] {
        for e in v["inventory"].as_array().unwrap() {
            let file = e["file"].as_str().unwrap();
            let base = file.rsplit('/').next().unwrap();
            let other = if stem == STEM {
                "enshrouded_server"
            } else {
                "enshrouded."
            };
            assert!(!base.starts_with(other), "{stem} lists {file}");
        }
    }
    assert!(
        notes(&client).is_empty(),
        "the server's DATs are not the client's retained containers: {client:#}"
    );
}

#[test]
fn a_file_changed_or_replaced_while_inspected_is_refused() {
    for (tag, replace) in [("inspect-changed", false), ("inspect-replaced-file", true)] {
        let f = fixture(tag);
        let game = f.game.clone().into_std_path_buf();
        let mut done = false;
        let v = inspection::inspect_observed(
            game.as_path(),
            &options(Side::Client),
            &mut |file: &str| {
                if file != "enshrouded.kfc_resources" || done {
                    return;
                }
                done = true;
                let path = game.join(file);
                let mut bytes = fs::read(&path).unwrap();
                if replace {
                    let copy = game.join("replacement.tmp");
                    fs::write(&copy, &bytes).unwrap();
                    fs::rename(&copy, &path).unwrap();
                } else {
                    bytes.push(0);
                    fs::write(&path, &bytes).unwrap();
                }
            },
        );
        assert!(done);
        let v = serde_json::to_value(v).unwrap();
        refused(&v, "changed");
        assert_eq!(v["reasons"][0]["file"], "enshrouded.kfc_resources");
    }
}

/// A same-length rewrite in place whose modification time is put back: the change time still moves (Unix ctime,
/// Windows ChangeTime), so the final check refuses it, for a file read in full (the header, also hashed again) and
/// for one checked only by metadata afterwards (the resource file).
#[test]
fn a_same_length_rewrite_with_its_time_put_back_is_refused() {
    for (tag, target) in [
        ("inspect-rewrite-header", "enshrouded.kfc"),
        ("inspect-rewrite-resources", "enshrouded.kfc_resources"),
    ] {
        let f = fixture(tag);
        let game = f.game.clone().into_std_path_buf();
        // clear of the timestamp tick the fixture's own writes fell in
        std::thread::sleep(std::time::Duration::from_millis(50));
        let mut done = false;
        let v = inspection::inspect_observed(
            game.as_path(),
            &options(Side::Client),
            &mut |file: &str| {
                if file != target || done {
                    return;
                }
                done = true;
                use std::io::{Seek, Write};
                let path = game.join(file);
                let modified = fs::metadata(&path).unwrap().modified().unwrap();
                let len = fs::metadata(&path).unwrap().len();
                let mut last = [0u8; 1];
                {
                    use std::io::Read;
                    let mut h = fs::File::open(&path).unwrap();
                    h.seek(std::io::SeekFrom::Start(len - 1)).unwrap();
                    h.read_exact(&mut last).unwrap();
                }
                let mut h = fs::OpenOptions::new().write(true).open(&path).unwrap();
                h.seek(std::io::SeekFrom::Start(len - 1)).unwrap();
                h.write_all(&[last[0] ^ 0xff]).unwrap();
                h.set_modified(modified).unwrap();
                drop(h);
                let m = fs::metadata(&path).unwrap();
                assert_eq!((m.len(), m.modified().unwrap()), (len, modified));
            },
        );
        assert!(done);
        let v = serde_json::to_value(v).unwrap();
        refused(&v, "changed");
        assert_eq!(v["reasons"][0]["file"], target);
    }
}

/// A receipt path that holds no receipt this build can read (too large for the writer's cap, or a folder) is an
/// unknown receipt, kept as it is, not a link.
#[test]
fn an_oversized_or_non_file_receipt_is_an_unknown_receipt() {
    let f = published("inspect-receipt-huge");
    let receipt = f.receipt();
    let file = fs::OpenOptions::new().write(true).open(&receipt).unwrap();
    file.set_len((16 << 20) + 1).unwrap();
    drop(file);
    let v = inspect(&f);
    refused(&v, "receipt-unknown");
    assert_eq!(v["receipt"]["state"], "unknown");
    assert_eq!(fs::metadata(&receipt).unwrap().len(), (16 << 20) + 1);
    let f = published("inspect-receipt-dir");
    let receipt = f.receipt();
    fs::remove_file(&receipt).unwrap();
    fs::create_dir(&receipt).unwrap();
    let v = inspect(&f);
    refused(&v, "receipt-unknown");
    assert!(receipt.is_dir());
}

// ---- read-only proof ----

/// Every entry below \`root\`: kind, bytes, mode, mtime and ctime (ctime moves on any write or metadata change).
fn tree(root: &Path) -> BTreeMap<PathBuf, (bool, Vec<u8>, String)> {
    let mut out = BTreeMap::new();
    fn visit(root: &Path, dir: &Path, out: &mut BTreeMap<PathBuf, (bool, Vec<u8>, String)>) {
        for e in fs::read_dir(dir).unwrap() {
            let path = e.unwrap().path();
            let m = fs::symlink_metadata(&path).unwrap();
            #[cfg(unix)]
            let stamp = {
                use std::os::unix::fs::MetadataExt;
                format!(
                    "{:o} {} {} {}.{} {}.{}",
                    m.mode(),
                    m.ino(),
                    m.len(),
                    m.mtime(),
                    m.mtime_nsec(),
                    m.ctime(),
                    m.ctime_nsec()
                )
            };
            #[cfg(not(unix))]
            let stamp = format!(
                "{} {:?} {:?}",
                m.len(),
                m.modified().ok(),
                m.permissions().readonly()
            );
            let rel = path.strip_prefix(root).unwrap().to_path_buf();
            if m.is_dir() {
                out.insert(rel, (true, Vec::new(), stamp));
                visit(root, &path, out);
            } else {
                out.insert(rel, (false, fs::read(&path).unwrap_or_default(), stamp));
            }
        }
    }
    visit(root, root, &mut out);
    out
}

/// The helper binary; KILN_EML_INSPECT_BIN names it where the test exe runs away from its build (win1).
fn bin() -> Command {
    Command::new(
        std::env::var_os("KILN_EML_INSPECT_BIN")
            .unwrap_or_else(|| env!("CARGO_BIN_EXE_kiln-eml-inspect").into()),
    )
}
fn run_bin(game: &Path, side: &str) -> Value {
    let out = bin()
        .args(["--game", game.to_str().unwrap(), "--side", side])
        .output()
        .unwrap();
    assert_eq!(
        out.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    serde_json::from_slice(&out.stdout).unwrap()
}

#[cfg(target_os = "linux")]
mod watch {
    use std::{ffi::CString, path::Path};
    unsafe extern "C" {
        fn inotify_init1(flags: i32) -> i32;
        fn inotify_add_watch(fd: i32, path: *const std::ffi::c_char, mask: u32) -> i32;
        fn read(fd: i32, buf: *mut std::ffi::c_void, count: usize) -> isize;
        fn close(fd: i32) -> i32;
    }
    pub const OPEN: u32 = 0x20;
    /// modify, attrib, close-after-write, moves, create, delete, delete/move self
    pub const WRITE: u32 = 0x2 | 0x4 | 0x8 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800;
    pub struct Watch {
        fd: i32,
        dirs: Vec<(i32, std::path::PathBuf)>,
    }
    impl Watch {
        pub fn new(root: &Path) -> Self {
            let fd = unsafe { inotify_init1(0o4000) }; // IN_NONBLOCK
            assert!(fd >= 0);
            let mut w = Self {
                fd,
                dirs: Vec::new(),
            };
            w.add(root);
            w
        }
        fn add(&mut self, dir: &Path) {
            let c = CString::new(dir.as_os_str().as_encoded_bytes()).unwrap();
            let wd = unsafe { inotify_add_watch(self.fd, c.as_ptr(), 0xfff) };
            assert!(wd >= 0, "watch {}", dir.display());
            self.dirs.push((wd, dir.to_path_buf()));
            for e in std::fs::read_dir(dir).unwrap() {
                let e = e.unwrap();
                if e.file_type().unwrap().is_dir() {
                    self.add(&e.path());
                }
            }
        }
        /// (mask, path) of every event since the last call.
        pub fn events(&self) -> Vec<(u32, std::path::PathBuf)> {
            let mut out = Vec::new();
            let mut buf = vec![0u8; 1 << 16];
            loop {
                let n = unsafe { read(self.fd, buf.as_mut_ptr().cast(), buf.len()) };
                if n <= 0 {
                    return out;
                }
                let mut at = 0;
                while at < n as usize {
                    let i32_at = |o: usize| i32::from_ne_bytes(buf[o..o + 4].try_into().unwrap());
                    let u32_at = |o: usize| u32::from_ne_bytes(buf[o..o + 4].try_into().unwrap());
                    let (wd, mask, len) = (i32_at(at), u32_at(at + 4), u32_at(at + 12) as usize);
                    let name = &buf[at + 16..at + 16 + len];
                    let name = String::from_utf8_lossy(name)
                        .trim_end_matches('\0')
                        .to_owned();
                    let dir = self
                        .dirs
                        .iter()
                        .find(|(w, _)| *w == wd)
                        .map(|(_, d)| d.clone())
                        .unwrap_or_default();
                    out.push((mask, if name.is_empty() { dir } else { dir.join(name) }));
                    at += 16 + len;
                }
            }
        }
    }
    impl Drop for Watch {
        fn drop(&mut self) {
            unsafe { close(self.fd) };
        }
    }
}

#[test]
fn inspection_creates_changes_and_write_opens_nothing() {
    // a first-managed install without kiln/ (no ownership, cache or lock folders), and an owned one beside SF state
    let bare = fixture("inspect-readonly-bare");
    fs::remove_dir_all(bare.game.join("kiln")).unwrap();
    let owned = published("inspect-readonly-owned");
    owned.sf_applied();
    fs::write(owned.game.join(".kiln-eml.lock"), b"").unwrap();
    for f in [&bare, &owned] {
        let root = f.root.0.as_std_path();
        let game = f.game.as_std_path();
        let before = tree(root);
        #[cfg(target_os = "linux")]
        let watch = watch::Watch::new(root);
        #[cfg(target_os = "linux")]
        watch.events(); // setting the watches up lists every folder
        let lib = inspect(f);
        let cli = run_bin(game, "client");
        ok(&lib);
        let strip = |mut v: Value| {
            v.as_object_mut().unwrap().remove("cost");
            v
        };
        assert_eq!(
            strip(lib),
            strip(cli),
            "the CLI prints the library's inspection"
        );
        #[cfg(target_os = "linux")]
        {
            let events = watch.events();
            let writes: Vec<_> = events
                .iter()
                .filter(|(m, _)| m & watch::WRITE != 0)
                .collect();
            assert!(writes.is_empty(), "write events: {writes:?}");
            let opened: Vec<_> = events
                .iter()
                .filter(|(m, _)| m & watch::OPEN != 0)
                .map(|(_, p)| p.clone())
                .collect();
            assert!(
                opened
                    .iter()
                    .any(|p| p.ends_with("enshrouded.kfc_resources")),
                "the watch saw the reads: {events:?}"
            );
            for p in &opened {
                let rel = p.strip_prefix(root).unwrap();
                assert!(
                    !rel.starts_with("game/shroudforge"),
                    "opened ShroudForge state: {}",
                    p.display()
                );
                assert!(!rel.ends_with(".kiln-eml.lock"), "opened the install lock");
            }
        }
        assert_eq!(tree(root), before, "the tree changed");
    }
    assert!(!bare.game.join("kiln").exists());
}

/// Windows: no open of the inspector asks for write or delete access, and every open shares reading, writing and
/// deleting. First every file of the install carries the read-only attribute and is held open by another handle that
/// shares reading only (an open asking for write or delete access fails then); then every file is held open with write
/// and delete access, sharing everything (an open that does not share writing and deleting fails then). The library
/// and the helper inspect it both times and nothing changes.
#[cfg(windows)]
#[test]
fn opens_read_only_and_share_everything_on_windows() {
    use std::os::windows::fs::OpenOptionsExt;
    const SHARE_READ: u32 = 0x1;
    const SHARE_ALL: u32 = 0x1 | 0x2 | 0x4;
    const GENERIC_READ: u32 = 0x8000_0000;
    const GENERIC_WRITE: u32 = 0x4000_0000;
    const DELETE: u32 = 0x0001_0000;
    struct Writable(Vec<PathBuf>);
    impl Drop for Writable {
        fn drop(&mut self) {
            for p in &self.0 {
                if let Ok(m) = fs::metadata(p) {
                    let mut perm = m.permissions();
                    perm.set_readonly(false);
                    let _ = fs::set_permissions(p, perm);
                }
            }
        }
    }
    let f = published("inspect-win-share");
    let root = f.root.0.as_std_path();
    let game = f.game.as_std_path();
    // every file the inspector may open: the side's files in the game folder and the receipt (the install lock and
    // the fixture's own state are held by this process and are never opened by an inspection; Linux's inotify proof)
    let mut files: Vec<PathBuf> = fs::read_dir(game)
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| p.is_file() && !p.file_name().unwrap().to_string_lossy().starts_with('.'))
        .collect();
    files.push(f.receipt().into_std_path_buf());
    assert!(files.len() >= 8, "{files:?}");
    assert!(files.iter().any(|p| p.ends_with("enshrouded.json")));
    let header = game.join("enshrouded.kfc");
    let check = |phase: &str| {
        let before = tree(root);
        let lib = inspect(&f);
        let cli = run_bin(game, "client");
        ok(&lib);
        ok(&cli);
        assert_eq!(lib["input"]["class"], "owned-output", "{phase}");
        assert_eq!(cli["input"]["sha256"], lib["input"]["sha256"], "{phase}");
        assert_eq!(tree(root), before, "{phase}: the tree changed");
    };
    {
        let restore = Writable(files.clone());
        for p in &files {
            let mut perm = fs::metadata(p).unwrap().permissions();
            perm.set_readonly(true);
            fs::set_permissions(p, perm).unwrap();
        }
        let held: Vec<fs::File> = files
            .iter()
            .map(|p| {
                fs::OpenOptions::new()
                    .read(true)
                    .share_mode(SHARE_READ)
                    .open(p)
                    .unwrap_or_else(|e| panic!("hold {}: {e}", p.display()))
            })
            .collect();
        assert!(
            fs::OpenOptions::new()
                .append(true)
                .share_mode(SHARE_ALL)
                .open(&header)
                .is_err(),
            "control: a write open must fail here"
        );
        assert!(
            fs::OpenOptions::new()
                .read(true)
                .access_mode(GENERIC_READ | DELETE)
                .share_mode(SHARE_ALL)
                .open(&header)
                .is_err(),
            "control: a delete-access open must fail here"
        );
        check("read-only, shared for reading only");
        drop(held);
        drop(restore);
    }
    let held: Vec<fs::File> = files
        .iter()
        .map(|p| {
            fs::OpenOptions::new()
                .access_mode(GENERIC_READ | GENERIC_WRITE | DELETE)
                .share_mode(SHARE_ALL)
                .open(p)
                .unwrap()
        })
        .collect();
    assert!(
        fs::OpenOptions::new()
            .read(true)
            .share_mode(SHARE_READ)
            .open(&header)
            .is_err(),
        "control: an open that shares reading only must fail here"
    );
    assert!(
        fs::OpenOptions::new()
            .read(true)
            .share_mode(0x1 | 0x2)
            .open(&header)
            .is_err(),
        "control: an open that does not share deleting must fail here"
    );
    check("held for writing and deleting");
    drop(held);
}

#[cfg(unix)]
#[test]
fn a_read_only_tree_is_inspected() {
    use std::os::unix::fs::PermissionsExt;
    /// Puts the modes back even when an assertion fails, so the fixture root can still be removed.
    struct Restore(Vec<(PathBuf, u32)>);
    impl Drop for Restore {
        fn drop(&mut self) {
            for (p, mode) in self.0.iter().rev() {
                let _ = fs::set_permissions(p, fs::Permissions::from_mode(*mode));
            }
        }
    }
    let f = published("inspect-ro-tree");
    let root = f.root.0.as_std_path();
    let mut all: Vec<PathBuf> = tree(root).keys().map(|p| root.join(p)).collect();
    all.push(root.to_path_buf());
    let restore = Restore(
        all.iter()
            .map(|p| (p.clone(), fs::metadata(p).unwrap().permissions().mode()))
            .collect(),
    );
    for (p, mode) in &restore.0 {
        fs::set_permissions(p, fs::Permissions::from_mode(mode & !0o222)).unwrap();
    }
    let blocked = fs::File::create(f.game.join("probe")).is_err();
    let v = run_bin(f.game.as_std_path(), "client");
    drop(restore);
    assert!(
        blocked,
        "the tree was writable: privileges ignore file modes here"
    );
    ok(&v);
    assert_eq!(v["input"]["class"], "owned-output");
}

/// A real read-only bind mount in a private user+mount namespace (no host mount changes). Needs unprivileged user
/// namespaces and util-linux unshare: run with --ignored; where it cannot run, the result is unverified, not a pass.
#[cfg(target_os = "linux")]
#[test]
#[ignore = "needs unprivileged user and mount namespaces (unshare -rm); run with --ignored"]
fn a_read_only_mount_is_inspected() {
    let f = published("inspect-ro-mount");
    let game = f.game.as_std_path();
    let before = tree(f.root.0.as_std_path());
    let script = r#"mount --bind "$1" "$1" && mount -o remount,bind,ro "$1" && ! touch "$1/probe" 2>/dev/null && exec "$2" --game "$1" --side client"#;
    let out = Command::new("unshare")
        .args([
            "-rm",
            "sh",
            "-c",
            script,
            "sh",
            game.to_str().unwrap(),
            env!("CARGO_BIN_EXE_kiln-eml-inspect"),
        ])
        .output()
        .unwrap();
    assert_eq!(
        out.status.code(),
        Some(0),
        "unshare/mount failed (unverified here): {}",
        String::from_utf8_lossy(&out.stderr)
    );
    let v: Value = serde_json::from_slice(&out.stdout).unwrap();
    ok(&v);
    assert_eq!(v["input"]["class"], "owned-output");
    assert_eq!(tree(f.root.0.as_std_path()), before);
}

#[test]
fn the_cli_has_no_patch_runner_path_and_rejects_bad_arguments() {
    let f = fixture("inspect-cli");
    let game = f.game.as_str();
    let before = tree(f.root.0.as_std_path());
    for args in [
        vec!["--game", game],
        vec!["--side", "client"],
        vec!["--game", game, "--side", "both"],
        vec!["--game", game, "--side", "client", "--no-patch"],
        vec!["--game", game, "--side", "client", "--mods", game],
        vec!["--game", game, "--side", "client", "--export"],
        vec![],
    ] {
        let out = bin().args(&args).output().unwrap();
        assert_eq!(out.status.code(), Some(2), "{args:?}");
        assert!(out.stdout.is_empty(), "{args:?}");
    }
    assert_eq!(tree(f.root.0.as_std_path()), before);
}

// ---- schema agreement ----

fn schema() -> Value {
    serde_json::from_str(include_str!("../schema/archive-inspection.schema.json")).unwrap()
}
/// The subset of JSON Schema the inspection schema uses; anything else in it fails this test.
fn check(schema: &Value, root: &Value, v: &Value, at: &str) -> Result<(), String> {
    let s = schema.as_object().unwrap();
    for key in s.keys() {
        let known = [
            "$schema",
            "$id",
            "title",
            "description",
            "$defs",
            "$ref",
            "type",
            "properties",
            "required",
            "additionalProperties",
            "enum",
            "const",
            "items",
            "minimum",
            "pattern",
            "oneOf",
        ];
        if !known.contains(&key.as_str()) {
            return Err(format!("{at}: unsupported schema keyword {key}"));
        }
    }
    if let Some(r) = s.get("$ref") {
        let name = r.as_str().unwrap().strip_prefix("#/$defs/").unwrap();
        return check(&root["$defs"][name], root, v, at);
    }
    if let Some(options) = s.get("oneOf") {
        let n = options
            .as_array()
            .unwrap()
            .iter()
            .filter(|o| check(o, root, v, at).is_ok())
            .count();
        return if n == 1 {
            Ok(())
        } else {
            Err(format!("{at}: {n} oneOf branches match {v}"))
        };
    }
    if let Some(t) = s.get("type") {
        let types: Vec<&str> = match t {
            Value::Array(a) => a.iter().map(|x| x.as_str().unwrap()).collect(),
            x => vec![x.as_str().unwrap()],
        };
        let fits = |t: &str| match t {
            "null" => v.is_null(),
            "string" => v.is_string(),
            "boolean" => v.is_boolean(),
            "object" => v.is_object(),
            "array" => v.is_array(),
            "integer" => v.is_u64() || v.is_i64(),
            _ => false,
        };
        if !types.iter().any(|t| fits(t)) {
            return Err(format!("{at}: {v} is not {types:?}"));
        }
    }
    if let Some(c) = s.get("const")
        && c != v
    {
        return Err(format!("{at}: {v} != {c}"));
    }
    if let Some(e) = s.get("enum")
        && !e.as_array().unwrap().contains(v)
    {
        return Err(format!("{at}: {v} not in {e}"));
    }
    if let Some(m) = s.get("minimum")
        && v.as_u64().is_some_and(|x| x < m.as_u64().unwrap())
    {
        return Err(format!("{at}: {v} < {m}"));
    }
    if let Some(p) = s.get("pattern") {
        assert_eq!(p, "^[0-9a-f]{64}$", "the only pattern the schema uses");
        if let Some(text) = v.as_str()
            && !(text.len() == 64 && text.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f')))
        {
            return Err(format!("{at}: {text} is not sha256 hex"));
        }
    }
    if let (Some(o), Some(props)) = (v.as_object(), s.get("properties")) {
        let props = props.as_object().unwrap();
        for r in s
            .get("required")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
        {
            if !o.contains_key(r.as_str().unwrap()) {
                return Err(format!("{at}: missing {r}"));
            }
        }
        match s.get("additionalProperties") {
            Some(Value::Bool(false)) => {
                if let Some(k) = o.keys().find(|k| !props.contains_key(*k)) {
                    return Err(format!("{at}: unknown key {k}"));
                }
            }
            Some(extra) => {
                for (k, x) in o.iter().filter(|(k, _)| !props.contains_key(*k)) {
                    check(extra, root, x, &format!("{at}.{k}"))?;
                }
            }
            None => return Err(format!("{at}: object schema without additionalProperties")),
        }
        for (k, x) in o {
            if let Some(p) = props.get(k) {
                check(p, root, x, &format!("{at}.{k}"))?;
            }
        }
    } else if let (Some(o), Some(extra)) = (v.as_object(), s.get("additionalProperties")) {
        for (k, x) in o {
            check(extra, root, x, &format!("{at}.{k}"))?;
        }
    }
    if let (Some(a), Some(items)) = (v.as_array(), s.get("items")) {
        for (i, x) in a.iter().enumerate() {
            check(items, root, x, &format!("{at}[{i}]"))?;
        }
    }
    Ok(())
}

#[test]
fn every_inspection_matches_the_closed_schema_and_the_frozen_tokens() {
    let schema = schema();
    let mut outputs = Vec::new();
    let f = fixture("inspect-schema-first");
    outputs.push(inspect(&f));
    let f = published("inspect-schema-owned");
    outputs.push(inspect(&f));
    fs::write(f.game.join("enshrouded_009.dat"), b"x").unwrap();
    outputs.push(inspect(&f));
    f.forget_ownership();
    outputs.push(inspect(&f));
    fixture::archive::corrupt_first_byte(&f.game.join(format!("{STEM}.kfc_resources.bak")));
    outputs.push(inspect(&f));
    let f = fixture("inspect-schema-missing");
    fs::remove_file(f.game.join("enshrouded.exe")).unwrap();
    outputs.push(inspect(&f));
    let f = fixture("inspect-schema-rebaselined");
    let mut header = fs::read(f.game.join("enshrouded.kfc")).unwrap();
    let version = fixture::types::VERSION.as_bytes();
    let at = header
        .windows(version.len())
        .position(|w| w == version)
        .unwrap();
    header[at] ^= 1;
    fs::write(f.game.join("enshrouded.kfc.bak"), &header).unwrap();
    let v = inspect(&f);
    assert_eq!(v["base"]["origin"]["token"], "rebaselined-unclassified");
    outputs.push(v);
    let f = fixture("inspect-schema-incomplete");
    fs::copy(
        f.game.join("enshrouded.kfc"),
        f.game.join("enshrouded.kfc.bak"),
    )
    .unwrap();
    outputs.push(inspect(&f));
    for v in &outputs {
        check(&schema, &schema, v, "$").unwrap_or_else(|e| panic!("{e}\n{v:#}"));
    }
    // the schema's origin table is exactly the frozen token/label pairs, and nothing is called vanilla
    let origin = &schema["$defs"]["origin"]["oneOf"];
    let pairs: Vec<_> = origin
        .as_array()
        .unwrap()
        .iter()
        .map(|o| {
            (
                o["properties"]["token"]["const"].clone(),
                o["properties"]["label"]["const"].clone(),
            )
        })
        .collect();
    assert_eq!(
        pairs,
        [
            (json!("first-managed-unclassified"), json!(UNCLASSIFIED)),
            (json!("rebaselined-unclassified"), json!(REBASELINED)),
            (json!("unrecorded"), json!(UNRECORDED))
        ]
    );
    assert_eq!(
        inspection::ORIGINS.as_slice(),
        [
            ("first-managed-unclassified", UNCLASSIFIED),
            ("rebaselined-unclassified", REBASELINED),
            ("unrecorded", UNRECORDED)
        ]
        .as_slice()
    );
    assert!(!schema.to_string().to_lowercase().contains("vanilla"));
    // closed: a value with an unknown key or a token outside the table is refused
    let mut extra = outputs[0].clone();
    extra["base"]["origin"]["token"] = json!("vanilla");
    assert!(check(&schema, &schema, &extra, "$").is_err());
    let mut extra = outputs[0].clone();
    extra["base"]["note"] = json!(1);
    assert!(check(&schema, &schema, &extra, "$").is_err());
}
