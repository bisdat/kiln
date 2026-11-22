// SPDX-License-Identifier: GPL-3.0-or-later
//! A working baseline that does not describe the game's data files: EML mode through the production runner, and the
//! archive inspector a kiln-mode start goes by. Synthetic archives only; no game bytes.
//!
//! The stale game folder (`stale`) is one a player can have: another mod loader replaced the backup pair with the
//! modded files it found, a later start took that pair up and sealed it again (it matched the live files then), the
//! game files were verified in Steam (the game's own header and resource file are back, the extra containers stay),
//! and a container the backup lists was rewritten since by another start. 0.2.0-dev.30.4 staged from that backup and
//! refused at the stage's read-back ("staged content hash mismatch"); without that read-back it would have published a
//! header listing bytes the container no longer holds. Kiln now looks at the backup before it stages anything.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;
#[path = "support/m6_startup/mod.rs"]
mod startup_support;

use fixture::archive::{ArchiveFixture, NEW_CONTENT, STEM};
use kfc::{
    container::{KFCFile, KFCWriteOptions, KFCWriter},
    guid::ContentHash,
};
use kiln_eml::inspection::{self, Options, Side};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::{Read, Seek, SeekFrom, Write},
    rc::Rc,
};

const PATCH: &str = r#"game.assets.create_content(buffer.wrap("m6 created content"))"#;
const REBASELINED: &str =
    "unclassified: game files as found when Kiln took a new baseline after the game changed";

fn name(ext: &str) -> String {
    format!("{STEM}.{ext}")
}
fn read(f: &ArchiveFixture, file: &str) -> Vec<u8> {
    fs::read(f.game.join(file)).unwrap()
}
fn inspect(f: &ArchiveFixture) -> Value {
    let options = Options { side: Side::Client, hash_containers: false };
    serde_json::to_value(inspection::inspect(f.game.as_std_path(), &options)).unwrap()
}

/// Every content the live header lists is, at its place in its container, the bytes its hash names (what the game
/// reads is what the header says). Err: the first one that is not.
fn live_contents_hold(f: &ArchiveFixture) -> Result<(), String> {
    let header = KFCFile::from_path(f.game.join(name("kfc")), false).map_err(|e| e.to_string())?;
    for (id, entry) in header.contents().iter() {
        let path = f.game.join(format!("{STEM}_{:03}.dat", entry.container_index));
        let mut file = fs::File::open(&path).map_err(|e| format!("{path}: {e}"))?;
        file.seek(SeekFrom::Start(entry.offset)).map_err(|e| format!("{path}: {e}"))?;
        let mut bytes = vec![0; id.size() as usize];
        file.read_exact(&mut bytes).map_err(|e| format!("{path}: {e}"))?;
        if ContentHash::from_data(&bytes) != *id {
            return Err(format!("{path} does not hold the content the header lists at offset {}", entry.offset));
        }
    }
    Ok(())
}

/// The stale game folder (module doc), and the game's own header and resource file.
fn stale(tag: &str) -> (ArchiveFixture, Vec<u8>, Vec<u8>) {
    stale_with(tag, true)
}

/// `stale`; `verified` false: the game files were not verified, and the live header is still the start's publication
/// built over the stale backup (as a start without a read-back of the game's containers left it).
fn stale_with(tag: &str, verified: bool) -> (ArchiveFixture, Vec<u8>, Vec<u8>) {
    let f = ArchiveFixture::new(tag);
    fs::write(f.game.join("enshrouded.exe"), b"synthetic client executable").unwrap();
    let (header, resources) = (read(&f, &name("kfc")), read(&f, &name("kfc_resources")));
    f.package("patch", PATCH, &["patch"]);
    let out = f.run_packages(&["patch"]);
    assert!(out.error.is_none() && f.receipt().exists(), "{out:?}");
    // another loader keeps the modded pair it found under the backup's names (Kiln's seal stays behind)
    for ext in ["kfc", "kfc_resources"] {
        fs::copy(f.game.join(name(ext)), f.game.join(name(&format!("{ext}.bak")))).unwrap();
    }
    // a later start takes it up: it is byte for byte the live pair, so it is sealed again and built over
    let out = f.run_packages(&["patch"]);
    assert!(out.error.is_none(), "{out:?}");
    // the game files are verified in Steam: its own header and resource file are back, the extra containers stay
    if verified {
        fs::write(f.game.join(name("kfc")), &header).unwrap();
        fs::write(f.game.join(name("kfc_resources")), &resources).unwrap();
    }
    // another start rewrote the created content's container since (other bytes, the same length)
    let backup = KFCFile::from_path(f.game.join(name("kfc.bak")), false).unwrap();
    let entry = backup.contents().get(&ContentHash::from_data(NEW_CONTENT)).expect("the backup lists the created content").clone();
    assert!(entry.container_index >= KFCFile::from_path(f.game.join(name("kfc.bak")), false).unwrap().containers().len() / 2);
    let mut file = fs::OpenOptions::new().write(true).open(f.game.join(format!("{STEM}_{:03}.dat", entry.container_index))).unwrap();
    file.seek(SeekFrom::Start(entry.offset)).unwrap();
    file.write_all(&NEW_CONTENT.to_ascii_uppercase()).unwrap();
    drop(file);
    (f, header, resources)
}

/// Kiln's bundled mod loader's record of a startup pass naming the live header (its output; the rest is not checked).
fn shroudforge_record(f: &ArchiveFixture) {
    let header = read(f, &name("kfc"));
    let sha: String = Sha256::digest(&header).iter().map(|b| format!("{b:02x}")).collect();
    let record = json!({"scheme": "kiln-sf-prepared 1", "target": STEM, "files": {"kfc": {"size": header.len(), "sha256": sha}}});
    fs::write(f.game.join(name("kfc.prepared.json")), serde_json::to_vec(&record).unwrap()).unwrap();
}

// The backup was built over the game files and nothing claims them: a new backup of the game files is taken before
// anything is staged, and the start publishes a header whose every content is where it says.
#[test]
fn eml_mode_replaces_a_backup_built_over_the_game_files_before_staging() {
    let (f, header, resources) = stale("stale-retake");
    let out = f.run_packages(&["patch"]);
    assert!(out.error.is_none(), "{out:?}");
    live_contents_hold(&f).unwrap();
    assert_eq!(read(&f, &name("kfc.bak")), header, "the new backup is the game's own header");
    assert_eq!(read(&f, &name("kfc_resources.bak")), resources, "and its own resource file");
    // the next start goes by the new backup
    let again = f.run_packages(&["patch"]);
    assert!(again.error.is_none(), "{again:?}");
    assert_eq!(read(&f, &name("kfc.bak")), header);
    live_contents_hold(&f).unwrap();
}

// The same backup over game files Kiln's bundled mod loader claims (its record names the live header): refused with
// what to do, before anything is staged; the backup and the game files stay as they are.
#[test]
fn eml_mode_refuses_a_backup_built_over_files_another_loader_claims() {
    let (f, header, _) = stale("stale-claimed");
    shroudforge_record(&f);
    let backup = read(&f, &name("kfc.bak"));
    let out = f.run_packages(&["patch"]);
    let text = format!("{out:?}");
    for part in [
        "does not describe this game build's data files",
        "Kiln's bundled mod loader prepared",
        "Verify the game files in Steam",
        "out of the game folder",
    ] {
        assert!(text.contains(part), "{part:?} in {text}");
    }
    assert!(!text.contains("staged content hash mismatch"), "{text}");
    assert_eq!(read(&f, &name("kfc.bak")), backup, "the backup is left as it is");
    assert_eq!(read(&f, &name("kfc")), header, "the game files are left as they are");
}

// A backup that is neither the live header nor built under it (two publications over the same game files): refused,
// before anything is staged.
#[test]
fn eml_mode_refuses_a_backup_that_describes_other_data() {
    let (f, _, _) = stale("stale-other");
    // another loader's publication over the game's own files, whose content takes the same container
    let base = Rc::new(KFCFile::from_path(f.game.join(name("kfc")), false).unwrap());
    let mut writer = KFCWriter::new_incremental_with_options(&f.game, STEM, base, f.types.clone(),
        KFCWriteOptions { overwrite_containers: true, truncate_containers: true, ..Default::default() }).unwrap();
    writer.write_content(&ContentHash::from_data(b"another loader's content"), b"another loader's content").unwrap();
    writer.finalize().unwrap();
    let live = read(&f, &name("kfc"));
    let out = f.run_packages(&["patch"]);
    let text = format!("{out:?}");
    for part in ["does not describe this game build's data files", "other containers or contents", "Verify the game files in Steam"] {
        assert!(text.contains(part), "{part:?} in {text}");
    }
    assert_eq!(read(&f, &name("kfc")), live);
}

// A normal backup (the live header is Kiln's publication over it) whose game container changed size since: refused with
// what to do, nothing staged, Kiln's output left live.
#[test]
fn eml_mode_refuses_a_backup_whose_containers_are_not_at_their_size() {
    let f = ArchiveFixture::new("stale-size");
    f.package("patch", PATCH, &["patch"]);
    assert!(f.run_packages(&["patch"]).error.is_none());
    let live = read(&f, &name("kfc"));
    let container = f.game.join(format!("{STEM}_001.dat"));
    fs::OpenOptions::new().append(true).open(&container).unwrap().write_all(b"x").unwrap();
    let text = format!("{:?}", f.run_packages(&["patch"]));
    for part in ["does not describe this game build's data files", "enshrouded_001.dat is", "Verify the game files in Steam"] {
        assert!(text.contains(part), "{part:?} in {text}");
    }
    assert_eq!(read(&f, &name("kfc")), live);
}

// The two layers: a stale backup the live header itself was built over (a start that did not read the game's containers
// back published from it) is not found before staging (the live index lists the same contents at the same places, and
// every container has its size); the stage's read-back of a few contents of each baseline container refuses it, and
// nothing is published.
#[test]
fn a_stale_backup_the_live_header_was_built_over_is_refused_by_the_stages_read_back() {
    let (f, _, _) = stale_with("stale-backstop", false);
    let live = read(&f, &name("kfc"));
    let out = f.run_packages(&["patch"]);
    let text = format!("{out:?}");
    assert!(text.contains("staged content hash mismatch"), "{text}");
    assert!(!text.contains("does not describe this game build's data files"), "not found before staging: {text}");
    for part in ["the vanilla backup enshrouded.kfc.bak no longer describes them", "verify the game files in Steam", "out of the game folder"] {
        assert!(text.contains(part), "the refusal says what to do: {part:?} in {text}");
    }
    // nothing new is published: the live header is as it was, or the backup the failed start put back over its own output
    let now = read(&f, &name("kfc"));
    assert!(now == live || now == read(&f, &name("kfc.bak")), "nothing new is published");
}

// The archive inspector (a kiln-mode start) agrees: a backup built over the live header is not the base, the live set is
// (note baseline-stale, a new baseline: the boot witness then takes it through the writer); one that describes other
// data is refused.
#[test]
fn the_inspector_does_not_take_a_stale_backup_for_the_base() {
    let (f, _, _) = stale("stale-inspect");
    let v = inspect(&f);
    assert_eq!(v["status"], "ok", "{v:#}");
    assert_eq!(v["base"]["evidence"], "current-set", "{v:#}");
    assert_eq!(v["base"]["origin"]["token"], "rebaselined-unclassified");
    assert_eq!(v["base"]["origin"]["label"], REBASELINED);
    let notes: Vec<_> = v["notes"].as_array().unwrap().iter().map(|n| n["code"].as_str().unwrap().to_owned()).collect();
    assert!(notes.iter().any(|n| n == "baseline-stale"), "{notes:?}");
    // another loader's publication over the game files: the backup describes other data
    let base = Rc::new(KFCFile::from_path(f.game.join(name("kfc")), false).unwrap());
    let mut writer = KFCWriter::new_incremental_with_options(&f.game, STEM, base, f.types.clone(),
        KFCWriteOptions { overwrite_containers: true, truncate_containers: true, ..Default::default() }).unwrap();
    writer.write_content(&ContentHash::from_data(b"another loader's content"), b"another loader's content").unwrap();
    writer.finalize().unwrap();
    let v = inspect(&f);
    assert_eq!(v["status"], "refused", "{v:#}");
    assert_eq!(v["reasons"][0]["code"], "baseline-corrupt", "{v:#}");
}

#[test]
fn a_live_header_missing_its_resource_chunks_cannot_replace_a_good_backup() {
    let f = ArchiveFixture::new("stale-missing-chunks");
    f.package("patch", PATCH, &["patch"]);
    assert!(f.run_packages(&["patch"]).error.is_none());
    let backup = read(&f, &name("kfc.bak"));
    let resources = read(&f, &name("kfc_resources.bak"));
    let seal = read(&f, &name("kfc.bak.integrity"));
    let mut damaged = backup.clone();
    // KFCHeader: 16-byte prefix, then sixteen (offset, count) locations; resource_chunks is last.
    damaged[140..144].copy_from_slice(&0u32.to_le_bytes());
    fs::write(f.game.join(name("kfc")), &damaged).unwrap();
    let out = f.run_packages(&["patch"]);
    assert!(out.error.is_some(), "{out:?}");
    assert_eq!(read(&f, &name("kfc.bak")), backup, "the good backup must not be replaced by the damaged live index");
    assert_eq!(read(&f, &name("kfc_resources.bak")), resources);
    assert_eq!(read(&f, &name("kfc.bak.integrity")), seal);
    assert_eq!(read(&f, &name("kfc")), damaged);
    assert_eq!(inspect(&f)["status"], "refused");
}

#[test]
fn kiln_mode_captures_a_fresh_baseline_and_commits_after_a_stale_backup() {
    use kiln_eml::{sf_pass::StartupDialect, startup::State};
    let (source, header, resources) = stale("stale-kiln-source");
    // The EML runner keeps its install lock for process life. Copy the archive state to another synthetic install
    // before taking the kiln-mode host's lock, rather than trying to take that same lock twice.
    let f = ArchiveFixture::new("stale-kiln-commit");
    fn copy_install(source: &std::path::Path, target: &std::path::Path) {
        fs::create_dir_all(target).unwrap();
        for entry in fs::read_dir(source).unwrap() {
            let entry = entry.unwrap();
            if entry.file_name() == ".kiln-eml.lock" { continue; }
            let path = target.join(entry.file_name());
            if entry.file_type().unwrap().is_dir() {
                copy_install(&entry.path(), &path);
            } else {
                fs::copy(entry.path(), path).unwrap();
            }
        }
    }
    copy_install(source.game.as_std_path(), f.game.as_std_path());
    let start = startup_support::Start::over(f);
    start.patch("fresh", &[], &[], &startup_support::content("fresh content"));
    let (_, outcome) = start.start(&start.plan(StartupDialect::Shroudforge, &[]), &startup_support::native());
    assert_eq!(outcome.state, State::Committed, "{outcome:?}");
    assert_eq!(read(&start.f, &name("kfc.bak")), header);
    assert_eq!(read(&start.f, &name("kfc_resources.bak")), resources);
    live_contents_hold(&start.f).unwrap();
}

#[test]
fn another_loaders_appended_output_does_not_replace_the_original_backup() {
    let f = ArchiveFixture::new("stale-append-keeps-base");
    f.package("patch", PATCH, &["patch"]);
    assert!(f.run_packages(&["patch"]).error.is_none());
    let (backup, resources) = (read(&f, &name("kfc.bak")), read(&f, &name("kfc_resources.bak")));
    let seal = read(&f, &name("kfc.bak.integrity"));
    fs::write(f.game.join(name("kfc")), &backup).unwrap();
    fs::write(f.game.join(name("kfc_resources")), &resources).unwrap();
    fs::remove_file(f.receipt()).unwrap();
    let base = Rc::new(KFCFile::from_path(f.game.join(name("kfc")), false).unwrap());
    let mut writer = KFCWriter::new_incremental_with_options(&f.game, STEM, base, f.types.clone(),
        KFCWriteOptions { overwrite_containers: true, truncate_containers: true, ..Default::default() }).unwrap();
    writer.write_content(&ContentHash::from_data(b"other loader"), b"other loader").unwrap();
    writer.finalize().unwrap();
    let out = f.run_packages(&["patch"]);
    assert!(out.error.is_none(), "{out:?}");
    assert_eq!(read(&f, &name("kfc.bak")), backup);
    assert_eq!(read(&f, &name("kfc_resources.bak")), resources);
    assert_eq!(read(&f, &name("kfc.bak.integrity")), seal);
    live_contents_hold(&f).unwrap();
}
