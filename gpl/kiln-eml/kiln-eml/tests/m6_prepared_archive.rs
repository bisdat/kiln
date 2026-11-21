// SPDX-License-Identifier: GPL-3.0-or-later
//! the prepared archive physical seam, not SF Lua parity. Both SF pins' startup/commit_asset_changes.rs:35-36
//! finalize privately before committing. RULES 3 retains Kiln's safer header-last publisher.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;

use fixture::archive::{self, ArchiveFixture, STEM};
use kfc::{container::KFCFile, guid::ContentHash};
use mod_loader::lua::startup_host::StartupHost;
use std::{collections::BTreeMap, fs, path::Path, rc::Rc};

fn baseline(f: &ArchiveFixture) -> Rc<KFCFile> {
    for ext in ["kfc", "kfc_resources"] {
        fs::copy(
            f.game.join(format!("{STEM}.{ext}")),
            f.game.join(format!("{STEM}.{ext}.bak")),
        )
        .unwrap();
    }
    Rc::new(KFCFile::from_path(f.game.join(format!("{STEM}.kfc.bak")), false).unwrap())
}
fn guard(f: &ArchiveFixture) -> fs::File {
    let file = fs::File::create(f.game.join(".kiln-eml.lock")).unwrap();
    file.try_lock().unwrap();
    file
}
fn snapshot(root: &Path) -> BTreeMap<String, Vec<u8>> {
    let mut out = BTreeMap::new();
    fn visit(root: &Path, path: &Path, out: &mut BTreeMap<String, Vec<u8>>) {
        for e in fs::read_dir(path).unwrap() {
            let e = e.unwrap();
            let name = e.file_name().to_string_lossy().into_owned();
            // The install guard is the test's own lock, not game data; native Windows refuses to read a file whose byte
            // range another handle has locked (ERROR_LOCK_VIOLATION), unlike Wine and unix advisory locks.
            if name.starts_with(".kiln-eml-stage-") || name == ".kiln-eml.lock" {
                continue;
            }
            if e.file_type().unwrap().is_dir() {
                visit(root, &e.path(), out);
            } else {
                let key = e
                    .path()
                    .strip_prefix(root)
                    .unwrap()
                    .to_string_lossy()
                    .replace('\\', "/");
                // Write-ahead scratch proof is the only permitted prepare-time record.
                if !key.starts_with("kiln/eml/ownership/cleanup-") {
                    out.insert(key, fs::read(e.path()).unwrap());
                }
            }
        }
    }
    visit(root, root, &mut out);
    out
}
/// A cleaned stage: gone, or kept empty on non-Windows builds (no removal through a handle there, never by path).
fn reclaimed(stage: &Path) -> bool {
    !stage.exists() || (cfg!(not(windows)) && fs::read_dir(stage).is_ok_and(|mut d| d.next().is_none()))
}
fn stage_dir(f: &ArchiveFixture) -> std::path::PathBuf {
    fs::read_dir(&f.game)
        .unwrap()
        .flatten()
        .map(|e| e.path())
        .find(|p| {
            p.file_name()
                .unwrap()
                .to_string_lossy()
                .starts_with(".kiln-eml-stage-")
        })
        .unwrap()
}

#[test]
fn prepare_only_is_readable_closed_and_has_no_live_ownership_effect() {
    let f = ArchiveFixture::new("prepare-only");
    let base = baseline(&f);
    fs::create_dir_all(f.game.join("shroudforge")).unwrap();
    fs::write(
        f.game.join("shroudforge/state.json"),
        b"deliberately unknown; preparation must not read this",
    )
    .unwrap();
    let guard = guard(&f);
    let before = snapshot(f.game.as_std_path());
    let mut host =
        StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    let mut stage = host.begin_stage().unwrap();
    assert!(
        host.begin_stage().is_err(),
        "one stage per captured host generation"
    );
    stage
        .write_content(
            &ContentHash::from_data(archive::NEW_CONTENT),
            archive::NEW_CONTENT,
        )
        .unwrap();
    let prepared = stage.prepare().unwrap();
    assert_eq!(snapshot(f.game.as_std_path()), before);
    assert!(!mod_loader::lua::owns_output(f.game.as_std_path(), STEM).unwrap());
    assert_eq!(prepared.index().resources().len(), 3);
    assert_eq!(
        prepared
            .read_content(&ContentHash::from_data(archive::NEW_CONTENT))
            .unwrap()
            .unwrap(),
        archive::NEW_CONTENT
    );
    assert_eq!(
        prepared
            .read_content(&ContentHash::from_data(archive::BLOBS[0]))
            .unwrap()
            .unwrap(),
        archive::BLOBS[0]
    );
    assert!(
        prepared
            .read_resource(&archive::resource_id(0))
            .unwrap()
            .is_some()
    );
    let scratch = stage_dir(&f);
    #[cfg(target_os = "linux")]
    for fd in fs::read_dir("/proc/self/fd").unwrap().flatten() {
        if let Ok(path) = fs::read_link(fd.path()) {
            // The prepared archive holds the stage directory itself until it is dropped; never a file in it.
            assert!(
                !path.starts_with(&scratch) || path == scratch,
                "stage reader/writer handle leaked: {path:?}"
            );
        }
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        for e in fs::read_dir(&scratch).unwrap().flatten() {
            fs::OpenOptions::new()
                .read(true)
                .write(true)
                .share_mode(0)
                .open(e.path())
                .unwrap();
        }
    }
    drop(prepared);
    assert!(reclaimed(&scratch));
    assert_eq!(snapshot(f.game.as_std_path()), before);
}

#[test]
fn prepared_publish_uses_the_existing_receipt_and_header_last_output() {
    let f = ArchiveFixture::new("prepare-publish");
    let base = baseline(&f);
    let guard = guard(&f);
    let mut host =
        StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    let mut stage = host.begin_stage().unwrap();
    stage
        .write_content(
            &ContentHash::from_data(archive::NEW_CONTENT),
            archive::NEW_CONTENT,
        )
        .unwrap();
    stage.prepare().unwrap().publish().unwrap();
    archive::validate(&f.game, STEM).unwrap();
    archive::assert_original_prefix(&f);
    assert_eq!(f.semantics().contents.len(), 4);
    assert!(mod_loader::lua::owns_output(f.game.as_std_path(), STEM).unwrap());
}

#[test]
fn corrupt_staged_reference_cannot_be_prepared() {
    for corrupt_dat in [false, true] {
        let f = ArchiveFixture::new("prepare-corrupt");
        let base = baseline(&f);
        let guard = guard(&f);
        let mut host =
            StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
        let stage = host.begin_stage().unwrap();
        let before = snapshot(f.game.as_std_path());
        if corrupt_dat {
            fs::write(f.game.join(format!("{STEM}_000.dat")), b"short").unwrap();
        } else {
            fs::OpenOptions::new()
                .write(true)
                .open(stage_dir(&f).join(format!("{STEM}.kfc_resources")))
                .unwrap()
                .set_len(1)
                .unwrap();
        }
        assert!(stage.prepare().is_err());
        if !corrupt_dat {
            assert_eq!(snapshot(f.game.as_std_path()), before);
        }
        assert!(!mod_loader::lua::owns_output(f.game.as_std_path(), STEM).unwrap());
    }
}

#[test]
fn a_prepared_token_refuses_changed_live_baseline_or_private_bytes() {
    for role in ["live", "baseline", "stage"] {
        let f = ArchiveFixture::new("prepare-stale");
        let base = baseline(&f);
        let guard = guard(&f);
        let mut host =
            StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
        let prepared = host.begin_stage().unwrap().prepare().unwrap();
        let path = match role {
            "live" => f.game.join(format!("{STEM}.kfc")).into_std_path_buf(),
            "baseline" => f.game.join(format!("{STEM}.kfc.bak")).into_std_path_buf(),
            _ => stage_dir(&f).join(format!("{STEM}.kfc_resources")),
        };
        fs::write(path, b"foreign changed bytes").unwrap();
        let before = snapshot(f.game.as_std_path());
        assert!(prepared.publish().is_err(), "stale {role} accepted");
        assert_eq!(snapshot(f.game.as_std_path()), before);
    }
}

#[test]
fn dropped_stage_retains_foreign_children() {
    for finalized in [false, true] {
        let f = ArchiveFixture::new("prepare-foreign");
        let base = baseline(&f);
        let guard = guard(&f);
        let mut host =
            StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
        let stage = host.begin_stage().unwrap();
        let path = stage_dir(&f);
        if finalized {
            let prepared = stage.prepare().unwrap();
            fs::write(path.join("foreign.txt"), b"keep").unwrap();
            drop(prepared);
        } else {
            fs::write(path.join("foreign.txt"), b"keep").unwrap();
            drop(stage);
        }
        assert_eq!(fs::read(path.join("foreign.txt")).unwrap(), b"keep");
        mod_loader::lua::prune(f.game.as_std_path(), STEM);
        mod_loader::lua::prune(f.game.as_std_path(), STEM);
        assert_eq!(fs::read(path.join("foreign.txt")).unwrap(), b"keep");
    }
}

#[test]
fn finalization_failure_discards_scratch_without_claiming_output() {
    let f = ArchiveFixture::new("prepare-finalize-error");
    let base = baseline(&f);
    let guard = guard(&f);
    let before = snapshot(f.game.as_std_path());
    let mut host =
        StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    let mut stage = host.begin_stage().unwrap();
    let invalid = kfc::guid::ResourceId::new(
        kfc::guid::Guid::parse(archive::CREATED_GUID).unwrap(),
        0xdeadbeef,
        0,
    );
    stage.write_resource(&invalid, &[1, 2, 3]).unwrap();
    let scratch = stage_dir(&f);
    assert!(
        stage.prepare().is_err(),
        "unknown resource type must fail writer finalization"
    );
    assert!(reclaimed(&scratch));
    assert_eq!(snapshot(f.game.as_std_path()), before);
}

#[test]
fn facade_requires_exact_baseline_and_the_original_held_guard() {
    let f = ArchiveFixture::new("prepare-guard");
    let base = baseline(&f);
    let guard = guard(&f);
    let unrelated = fs::File::create(f.root.0.join("other-lock")).unwrap();
    unrelated.try_lock().unwrap();
    assert!(
        StartupHost::open(
            f.game.as_std_path(),
            STEM,
            f.types.clone(),
            base.clone(),
            &unrelated
        )
        .is_err()
    );
    let path = f.game.join(format!("{STEM}_000.dat"));
    let bytes = fs::read(&path).unwrap();
    // write, not append: an append-only handle may not set the length on native Windows (no FILE_WRITE_DATA).
    fs::OpenOptions::new()
        .write(true)
        .open(&path)
        .unwrap()
        .set_len(bytes.len() as u64 + 1)
        .unwrap();
    assert!(
        StartupHost::open(
            f.game.as_std_path(),
            STEM,
            f.types.clone(),
            base.clone(),
            &guard
        )
        .is_err(),
        "new facade requires an exact baseline set"
    );
    fs::write(path, bytes).unwrap();
    let mut host =
        StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    let prepared = host.begin_stage().unwrap().prepare().unwrap();
    guard.unlock().unwrap();
    let before = snapshot(f.game.as_std_path());
    assert!(
        prepared.publish().is_err(),
        "an explicitly released borrowed guard is not authority"
    );
    assert_eq!(snapshot(f.game.as_std_path()), before);
}

#[cfg(unix)]
#[test]
fn replaced_root_and_stage_directories_are_not_publish_or_cleanup_authority() {
    for replace_root in [false, true] {
        let f = ArchiveFixture::new("prepare-path-identity");
        let base = baseline(&f);
        let guard = guard(&f);
        let mut host =
            StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
        let prepared = host.begin_stage().unwrap().prepare().unwrap();
        let path = if replace_root {
            f.game.as_std_path().to_path_buf()
        } else {
            stage_dir(&f)
        };
        let moved = f.root.0.join("saved-directory");
        fs::rename(&path, &moved).unwrap();
        fs::create_dir(&path).unwrap();
        fs::write(
            path.join(format!("{STEM}.kfc")),
            b"unrelated same-name child",
        )
        .unwrap();
        assert!(prepared.publish().is_err());
        assert_eq!(
            fs::read(path.join(format!("{STEM}.kfc"))).unwrap(),
            b"unrelated same-name child"
        );
        // The write-ahead cleanup record left behind is no deletion authority over the replacement on later starts.
        for _ in 0..2 {
            mod_loader::lua::prune(f.game.as_std_path(), STEM);
            assert_eq!(
                fs::read(path.join(format!("{STEM}.kfc"))).unwrap(),
                b"unrelated same-name child",
                "recovery deleted a file in a replaced directory"
            );
        }
    }
}

// Recovery without Drop (a killed run): a stage replaced at the same path is never cleaned by its old record, and the
// stage the run really created is still reclaimed.
#[cfg(unix)]
#[test]
fn recovery_after_a_killed_run_cleans_only_the_recorded_stage_itself() {
    for replace_stage in [true, false] {
        let f = ArchiveFixture::new("prepare-killed-identity");
        let base = baseline(&f);
        let guard = guard(&f);
        let mut host =
            StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
        let prepared = host.begin_stage().unwrap().prepare().unwrap();
        std::mem::forget(prepared); // the process died: no Drop, the record stays
        let stage = stage_dir(&f);
        if replace_stage {
            fs::rename(&stage, f.root.0.join("saved-stage")).unwrap();
            fs::create_dir(&stage).unwrap();
            fs::write(stage.join(format!("{STEM}.kfc")), b"unrelated same-name child").unwrap();
        }
        for _ in 0..2 {
            mod_loader::lua::prune(f.game.as_std_path(), STEM);
        }
        if replace_stage {
            assert_eq!(
                fs::read(stage.join(format!("{STEM}.kfc"))).unwrap(),
                b"unrelated same-name child",
                "recovery deleted a file in a replaced stage"
            );
        } else {
            assert!(reclaimed(&stage), "the recorded stage itself was not reclaimed after a killed run");
        }
    }
}

#[test]
fn resources_and_contents_share_one_read_validated_private_generation() {
    let f = ArchiveFixture::new("prepare-resource");
    let base = baseline(&f);
    let guard = guard(&f);
    let mut host =
        StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    let mut stage = host.begin_stage().unwrap();
    let id = archive::resource_id(0);
    let ty = f
        .types
        .get_by_hash(kfc::reflection::LookupKey::Qualified(id.type_hash()))
        .unwrap();
    let value = fixture::types::root_value();
    let bytes = value.to_bytes(&f.types, ty).unwrap();
    stage.write_resource(&id, &bytes).unwrap();
    stage
        .write_content(
            &ContentHash::from_data(archive::NEW_CONTENT),
            archive::NEW_CONTENT,
        )
        .unwrap();
    let prepared = stage.prepare().unwrap();
    assert_eq!(
        kfc::resource::value::Value::from_bytes(
            &f.types,
            ty,
            prepared.read_resource(&id).unwrap().unwrap()
        )
        .unwrap(),
        value
    );
    let before = f.semantics();
    assert_eq!(before.contents.len(), 3);
    prepared.publish().unwrap();
    archive::validate(&f.game, STEM).unwrap();
    assert_eq!(f.resource_value(0), value);
    assert_eq!(f.semantics().contents.len(), 4);
    archive::assert_original_prefix(&f);
}

#[cfg(windows)]
#[test]
fn sharing_failure_preserves_previous_live_target() {
    use std::os::windows::fs::OpenOptionsExt;
    let f = ArchiveFixture::new("prepare-sharing");
    let base = baseline(&f);
    let guard = guard(&f);
    let mut host =
        StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    let prepared = host.begin_stage().unwrap().prepare().unwrap();
    let path = f.game.join(format!("{STEM}.kfc"));
    let before = fs::read(&path).unwrap();
    let held = fs::OpenOptions::new()
        .read(true)
        .share_mode(1)
        .open(&path)
        .unwrap();
    assert!(prepared.publish().is_err());
    assert_eq!(
        fs::read(&path).unwrap(),
        before,
        "replacement must not remove first"
    );
    drop(held);
}
