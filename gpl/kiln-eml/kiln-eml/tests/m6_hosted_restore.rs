// SPDX-License-Identifier: GPL-3.0-or-later
//! Kiln mode over Kiln's bundled ShroudForge's output (crate::startup on the synthetic m6 archive): a player who used
//! Kiln's bundled ShroudForge and then chose Kiln's interpreter finds that ShroudForge's prepared output live, no Kiln
//! receipt naming it (an older receipt of Kiln's own output, keyed to the same backups, may still be there), and its
//! record beside the backup (`<stem>.kfc.prepared.json`, the fork's record_prepared_game_files.rs shape). The verified
//! BASE goes back over that output only while the record proves it was made over exactly the backups present; a stale
//! record, unknown output, or a live set of another game build is refused with what the player does next, and nothing
//! is changed. A stage folder an earlier start left behind changes none of this.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;
#[path = "support/m6_startup/mod.rs"]
mod startup_support;

use fixture::archive::{ArchiveFixture, STEM, SemanticSet};
use kfc::reflection::LookupKey;
use kiln_eml::{
    sf_pass::StartupDialect,
    startup::{StartupOutcome, State},
};
use sha2::{Digest as _, Sha256};
use startup_support::{Start, native, outcome_json, shroudforge_decoy, tree};
use std::{
    collections::BTreeMap,
    fs,
    path::Path,
    rc::Rc,
    time::{Duration, UNIX_EPOCH},
};

/// A Lua body that only edits resource part `part` (`child.extra = value`): no content, no container.
fn edit_only(part: u32, value: u32) -> String {
    format!(
        r#"local r = game.assets.get_resource("01234567-89ab-cdef-0123-456789abcdef", "m6.Root", {part})
r.data.child.extra = {value}"#
    )
}

fn archive_file(name: &str) -> bool {
    name == format!("{STEM}.kfc")
        || name == format!("{STEM}.kfc_resources")
        || name
            .strip_prefix(&format!("{STEM}_"))
            .and_then(|s| s.strip_suffix(".dat"))
            .is_some_and(|n| !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()))
}

/// The live archive files (header, resource file, every container file), by name.
fn live_set(game: &Path) -> BTreeMap<String, Vec<u8>> {
    fs::read_dir(game)
        .unwrap()
        .flatten()
        .filter_map(|e| {
            let name = e.file_name().to_string_lossy().into_owned();
            archive_file(&name).then(|| (name, fs::read(e.path()).unwrap()))
        })
        .collect()
}

/// Makes `set` the live archive files exactly (another writer's publication).
fn put_live(game: &Path, set: &BTreeMap<String, Vec<u8>>) {
    for name in live_set(game).keys() {
        if !set.contains_key(name) {
            fs::remove_file(game.join(name)).unwrap();
        }
    }
    for (name, bytes) in set {
        fs::write(game.join(name), bytes).unwrap();
    }
}

fn modified(path: &Path) -> String {
    fs::metadata(path)
        .unwrap()
        .modified()
        .unwrap()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos()
        .to_string()
}

/// The record Kiln's bundled ShroudForge writes after its asset pass over the files as they are now (the fork's
/// capture: the header whole, the resource file at its ends, the backups by length and time).
fn sf_record(game: &Path, scheme: &str) -> Vec<u8> {
    let file = |ext: &str| game.join(format!("{STEM}.{ext}"));
    let edges = |path: &Path| {
        let bytes = fs::read(path).unwrap();
        let edge = 1usize << 20;
        let mut h = Sha256::new();
        if bytes.len() > 2 * edge {
            h.update(&bytes[..edge]);
            h.update(&bytes[bytes.len() - edge..]);
        } else {
            h.update(&bytes);
        }
        format!("{:x}", h.finalize())
    };
    let identity =
        |path: &Path| serde_json::json!({"size": fs::metadata(path).unwrap().len(), "modified": modified(path)});
    let record = serde_json::json!({
        "scheme": scheme,
        "target": STEM,
        "fingerprint": "c".repeat(64),
        "exportsEnabled": true,
        "build": null,
        "files": {
            "kfc": {"size": fs::metadata(file("kfc")).unwrap().len(), "modified": modified(&file("kfc")),
                "sha256": format!("{:x}", Sha256::digest(fs::read(file("kfc")).unwrap()))},
            "kfcResources": {"size": fs::metadata(file("kfc_resources")).unwrap().len(),
                "modified": modified(&file("kfc_resources")), "sha256": edges(&file("kfc_resources"))},
            "kfcBackup": identity(&file("kfc.bak")),
            "kfcResourcesBackup": identity(&file("kfc_resources.bak")),
        },
        "exports": null,
    });
    let bytes = serde_json::to_vec_pretty(&record).unwrap();
    fs::write(file("kfc.prepared.json"), &bytes).unwrap();
    bytes
}

/// The owner's game folder in small: Kiln published its own output once (receipt, backups, seal), then Kiln's
/// bundled ShroudForge prepared its output from the same backups (here: another archive built from them, with another
/// edit) and recorded it; the receipt still names Kiln's earlier output. `record`: the scheme of ShroudForge's record,
/// None for no record (output nothing proves). Returns the start, ShroudForge's output and the BASE as the game reads it.
fn hosted_output(tag: &str, record: Option<&str>) -> (Start, BTreeMap<String, Vec<u8>>, SemanticSet) {
    let s = Start::new(tag);
    let vanilla = s.f.semantics();
    shroudforge_decoy(s.game());
    // ShroudForge's output: an archive built from the backups with its own edit (it stays live below)
    s.patch("alpha", &[], &[], &edit_only(0, 41));
    let (_, first) = s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native());
    assert_eq!(first.state, State::Committed, "{first:?}");
    let sf_output = live_set(s.game());
    // Kiln's own output, whose receipt stays behind
    s.patch("alpha", &[], &[], &edit_only(0, 77));
    let (_, second) = s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native());
    assert_eq!(second.state, State::Committed, "{second:?}");
    assert_eq!(s.extra(0), 77);
    // the bundled ShroudForge's start: its output goes live, Kiln's receipt is left naming other files, and the
    // handoff an earlier kiln-mode write owed was made by that start
    put_live(s.game(), &sf_output);
    let _ = fs::remove_file(s.game().join(format!("kiln/eml/ownership/{STEM}.sf-handoff-owed.json")));
    if let Some(scheme) = record {
        sf_record(s.game(), scheme);
    }
    assert_eq!(s.extra(0), 41);
    assert!(s.receipt().is_some(), "Kiln's older receipt is still there");
    // ShroudForge's mods leave kiln mode's pass: nothing is admitted, the start settles on the BASE (dialect none)
    fs::remove_dir_all(s.game().join("mods/alpha")).unwrap();
    (s, sf_output, vanilla)
}

fn base_warning(out: &StartupOutcome) -> String {
    out.warnings
        .iter()
        .find(|w| w.code == "base-not-restored")
        .map(|w| w.text.clone())
        .unwrap_or_else(|| panic!("no base-not-restored warning: {:?}", out.warnings))
}

const VERIFY: &str =
    "Verify the game files in Steam (Properties, Installed Files), then start the game again from the Kiln launcher";

// The owner's case: the bundled ShroudForge's recorded output over the backups Kiln keeps goes back to the BASE, the
// start is granted (Kiln's interpreter starts), the stale receipt goes, ShroudForge's own folder and record are not
// touched and the handoff its folder is owed is recorded. The next start finds the BASE live: unchanged, granted.
#[test]
fn hosted_shroudforge_output_goes_back_to_the_base_and_kiln_mode_starts() {
    let (s, _, vanilla) = hosted_output("hsf-restore", Some("kiln-sf-prepared 1"));
    let sf_folder = tree(&s.game().join("shroudforge"));
    let record = fs::read(s.game().join(format!("{STEM}.kfc.prepared.json"))).unwrap();
    let (_, out) = s.start(&s.plan(StartupDialect::None, &[]), &native());
    assert_eq!((out.state, out.live.is_some()), (State::Restored, true), "{out:?}");
    assert_eq!(s.f.semantics(), vanilla, "the BASE as the game reads it");
    assert_eq!(
        fs::read(s.game().join(format!("{STEM}.kfc"))).unwrap(),
        fs::read(s.game().join(format!("{STEM}.kfc.bak"))).unwrap()
    );
    assert!(s.receipt().is_none(), "the stale receipt went with the restore");
    assert!(
        s.owed(),
        "ShroudForge's applied record is stale now: the handoff is owed"
    );
    assert_eq!(
        tree(&s.game().join("shroudforge")),
        sf_folder,
        "ShroudForge's folder changed"
    );
    assert_eq!(
        fs::read(s.game().join(format!("{STEM}.kfc.prepared.json"))).unwrap(),
        record,
        "ShroudForge's record is its own (it no longer matches, so ShroudForge prepares again when it runs)"
    );
    let json = outcome_json(&out, "hosted-restored");
    assert_eq!(
        (json["state"].as_str(), json["grant"].as_bool()),
        (Some("restored"), Some(true)),
        "{json}"
    );
    let (_, next) = s.start(&s.plan(StartupDialect::None, &[]), &native());
    assert_eq!((next.state, next.live.is_some()), (State::Unchanged, true), "{next:?}");
}

// The record names the live header, but the backups changed since that pass (here only their last-write time: their
// bytes, receipt and seal still match): Kiln cannot prove they are what ShroudForge built from, so nothing changes and
// the reason says to verify the game files.
#[test]
fn hosted_output_over_backups_it_did_not_start_from_is_refused_with_guidance() {
    let (s, sf_output, _) = hosted_output("hsf-stale", Some("kiln-sf-prepared 1"));
    let bak = s.game().join(format!("{STEM}.kfc.bak"));
    let later = fs::metadata(&bak).unwrap().modified().unwrap() + Duration::from_secs(5);
    fs::File::options()
        .write(true)
        .open(&bak)
        .unwrap()
        .set_modified(later)
        .unwrap();
    let receipt = s.receipt_sha256();
    let (_, out) = s.start(&s.plan(StartupDialect::None, &[]), &native());
    assert_eq!((out.state, out.live.is_none()), (State::Failed, true), "{out:?}");
    let text = base_warning(&out);
    assert!(
        text.contains("is what Kiln's bundled ShroudForge prepared, but the backup files changed since then")
            && text.ends_with(VERIFY),
        "{text}"
    );
    assert!(
        text.chars().count() <= 400,
        "the launcher shows the first 400 characters: {}",
        text.chars().count()
    );
    assert_eq!(live_set(s.game()), sf_output, "the live files were touched");
    assert_eq!(s.receipt_sha256(), receipt, "the receipt was touched");
    assert!(!s.owed(), "nothing changed, nothing is owed");
    assert_eq!(outcome_json(&out, "hosted-stale")["grant"], false);
}

// The bundled ShroudForge's output of another game build (the live header's version is not the backups'): the
// inspector would take the live set as a new BASE; the witness refuses instead (no DLL loads, nothing is captured), and
// says to verify the game files.
#[test]
fn hosted_output_of_another_game_build_is_never_taken_as_the_base() {
    let (s, _, _) = hosted_output("hsf-build", None);
    let mut types = (*s.f.types).clone();
    types.version = format!("{} (a later build)", types.version);
    let root = types.get_by_name(LookupKey::Qualified(fixture::types::ROOT)).unwrap();
    let bytes = fixture::types::root_value().to_bytes(&types, root).unwrap();
    let resources: Vec<_> = [9, 0, 3]
        .into_iter()
        .map(|part| (fixture::archive::resource_id(part), bytes.clone()))
        .collect();
    let later = ArchiveFixture::with_contents("hsf-build-later", Rc::new(types), &resources, &fixture::archive::BLOBS);
    put_live(s.game(), &live_set(later.game.as_std_path()));
    sf_record(s.game(), "kiln-sf-prepared 1");
    let backups: Vec<Vec<u8>> = ["kfc.bak", "kfc_resources.bak", "kfc.bak.integrity"]
        .iter()
        .map(|e| fs::read(s.game().join(format!("{STEM}.{e}"))).unwrap())
        .collect();
    let live = live_set(s.game());
    let refused = s
        .witness(&s.plan(StartupDialect::None, &[]))
        .expect_err("the witness takes SF's output as a BASE");
    assert_eq!(refused.code, "baseline-shroudforge-output", "{refused:?}");
    assert!(
        refused.text.contains("Kiln's bundled ShroudForge prepared") && refused.text.ends_with(VERIFY),
        "{refused:?}"
    );
    let after: Vec<Vec<u8>> = ["kfc.bak", "kfc_resources.bak", "kfc.bak.integrity"]
        .iter()
        .map(|e| fs::read(s.game().join(format!("{STEM}.{e}"))).unwrap())
        .collect();
    assert_eq!(after, backups, "no new baseline was taken");
    assert_eq!(live_set(s.game()), live);
}

// Output nothing proves (no record, or a record of another scheme naming it) is refused as before, now with what to
// do; nothing changes.
#[test]
fn unknown_modified_output_is_refused_as_before_with_guidance() {
    for (tag, record) in [("hsf-unknown", None), ("hsf-scheme", Some("kiln-sf-prepared 0"))] {
        let (s, sf_output, _) = hosted_output(tag, record);
        let receipt = s.receipt_sha256();
        let (_, out) = s.start(&s.plan(StartupDialect::None, &[]), &native());
        assert_eq!((out.state, out.live.is_none()), (State::Failed, true), "{tag}: {out:?}");
        let text = base_warning(&out);
        assert!(
            text.contains("is neither the BASE nor output Kiln can prove it published; left as it is")
                && text.ends_with(VERIFY),
            "{tag}: {text}"
        );
        assert_eq!(live_set(s.game()), sf_output, "{tag}: the live files were touched");
        assert_eq!(s.receipt_sha256(), receipt, "{tag}: the receipt was touched");
    }
}

// A stage folder and its unbound clean-up record, as a start whose stage could not be listed leaves them (an empty
// `.kiln-eml-stage-<uuid>`): the next kiln-mode start stages anew and publishes over the bundled ShroudForge's
// recorded output (ShroudForge's pass of a ShroudForge mod), and the leftover is kept as it is (an unbound record is no
// proof to delete anything).
#[test]
fn a_leftover_stage_does_not_stop_the_next_kiln_mode_start() {
    let (s, _, _) = hosted_output("hsf-stage", Some("kiln-sf-prepared 1"));
    let id = "6b9c0eee-3e9f-4fea-8986-01d59f617891";
    let stage = s.game().join(format!(".kiln-eml-stage-{id}"));
    fs::create_dir(&stage).unwrap();
    let cleanup = s.game().join(format!("kiln/eml/ownership/cleanup-{id}.json"));
    let unbound = serde_json::to_vec(
        &serde_json::json!({"format": 2, "stem": STEM, "name": format!(".kiln-eml-stage-{id}"),
        "root": "a game folder identity"}),
    )
    .unwrap();
    fs::write(&cleanup, &unbound).unwrap();
    s.patch("beta", &[], &[], &edit_only(3, 5));
    let (_, out) = s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native());
    assert_eq!((out.state, out.live.is_some()), (State::Committed, true), "{out:?}");
    assert_eq!(s.extra(3), serde_json::json!(5));
    assert_ne!(
        s.extra(0),
        serde_json::json!(41),
        "built from the BASE, not over ShroudForge's output"
    );
    assert!(
        stage.is_dir() && fs::read_dir(&stage).unwrap().next().is_none(),
        "the leftover stage was changed"
    );
    assert_eq!(fs::read(&cleanup).unwrap(), unbound, "the unbound record was changed");
    let leftovers: Vec<String> = fs::read_dir(s.game())
        .unwrap()
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.starts_with(".kiln-eml-stage-"))
        .collect();
    assert!(leftovers.contains(&format!(".kiln-eml-stage-{id}")), "{leftovers:?}");
    // Windows removes this start's emptied stage through its handle; host builds keep it empty (cleanup.rs)
    if cfg!(windows) {
        assert_eq!(
            leftovers.len(),
            1,
            "this start's own stage was not removed: {leftovers:?}"
        );
    }
}
