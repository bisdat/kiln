//! The staging folders of a placement (`.kiln-place-<hex>`, [`super::sweep_staging`]): a placement stopped at any
//! step (before the copy, during it, after the old copy was parked, after the new one was published, after its receipt)
//! leaves none behind once the next placement or an uninstall ran, the publication journal stays as 0.2.0-dev.30.2
//! reads it, and a folder the record does not name, or names but is not of the shape Kiln makes, is never touched.

use super::*;
use crate::kept::Step;

/// A test game with an EMF Lua mod's first copy ready to place: (the temporary root, data dirs, game, profile).
fn world(tag: &str) -> (crate::library::TempDir, DataDirs, PathBuf, crate::profile::Profile) {
    let tmp = crate::library::TempDir::new(&std::env::temp_dir(), tag).unwrap();
    let dirs = DataDirs::at(tmp.path().join("data"));
    let game = tmp.path().join("game");
    std::fs::create_dir_all(game.join("kiln")).unwrap();
    let mut p = crate::profile::create(&dirs, "p", None).unwrap();
    p.mods.emf = vec![super::tests::emf_lua_mod(&dirs, tmp.path(), "1.0.0")];
    (tmp, dirs, game, p)
}

/// Runs `f` stopped at `step` as a crash would stop it (a panic: nothing is taken back). -> whether it got there.
fn stopped(step: Step, f: impl FnOnce()) -> bool {
    crate::kept::hooks::STOP_AT.with(|s| s.set(Some(step)));
    let stopped = std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)).is_err();
    crate::kept::hooks::STOP_AT.with(|s| s.set(None));
    stopped
}

/// The `.kiln-place-*` folders in the game's EMF mods folder.
fn leftovers(game: &Path) -> Vec<String> {
    std::fs::read_dir(game.join("emf/mods")).into_iter().flatten().flatten().map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.starts_with(".kiln-place-")).collect()
}

/// The publication journal as 0.2.0-dev.30.2 reads it: that release's `Publication`, which refuses a field it does not
/// know (the same struct as today's, byte for byte), over receipts of the same `Placement`.
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
#[allow(dead_code)]
struct Publication302 {
    path: PathBuf,
    #[serde(default)]
    from: Option<Marker>,
    to: Placement,
    #[serde(default)]
    parked: Option<PathBuf>,
}

fn readable_by_30_2(game: &Path, label: &str) {
    let journal = journal_file(game);
    if journal.exists() {
        serde_json::from_slice::<Vec<Publication302>>(&std::fs::read(&journal).unwrap())
            .unwrap_or_else(|e| panic!("{label}: 0.2.0-dev.30.2 would refuse the journal: {e}"));
    }
    let receipts = placements_file(game);
    if receipts.exists() {
        serde_json::from_slice::<Vec<Placement>>(&std::fs::read(&receipts).unwrap())
            .unwrap_or_else(|e| panic!("{label}: 0.2.0-dev.30.2 would refuse the receipts: {e}"));
    }
}

/// The one recorded staging folder after a stop: named before anything else, of Kiln's shape, directly in the mods folder.
fn the_recorded_folder(game: &Path, label: &str) -> Staging {
    let mut all = stagings(game).unwrap();
    assert_eq!(all.len(), 1, "{label}: {all:?}");
    let s = all.remove(0);
    let name = s.folder.file_name().unwrap().to_str().unwrap().to_string();
    let tag = name.strip_prefix(".kiln-place-").unwrap_or_else(|| panic!("{label}: {name}"));
    assert!(tag.len() == 12 && tag.bytes().all(|b| b.is_ascii_hexdigit()), "{label}: {name}");
    assert_eq!(s.folder.parent(), Some(game.join("emf/mods").as_path()), "{label}");
    assert_eq!(s.path, game.join("emf/mods/beastmaster"), "{label}");
    s
}

const STEPS: [(Step, bool); 7] = [
    (Step::StagingRecorded, false),
    (Step::StagingMade, false),
    (Step::StagingCopied, true), // stopped during the copy: part of the tree, no marker
    (Step::StagingCopied, false),
    (Step::Parked, false),
    (Step::Published, false),
    (Step::ReceiptSaved, false),
];

/// What the stop left in the recorded folder, as each step leaves it.
fn check_stopped(step: Step, mid: bool, s: &Staging, update: bool, label: &str) {
    let new = s.folder.join("new");
    match step {
        Step::StagingRecorded => assert!(!s.folder.exists(), "{label}"),
        Step::StagingMade => assert!(s.folder.is_dir() && !new.exists(), "{label}"),
        Step::StagingCopied => {
            assert!(new.join("main.lua").is_file() && marker(&new).is_none(), "{label}");
            if mid { std::fs::remove_file(new.join("main.lua")).unwrap(); }
        }
        Step::Parked => {
            assert!(marker(&new).is_some(), "{label}");
            assert_eq!(s.folder.join("old").is_dir(), update, "{label}");
        }
        _ => assert!(!new.exists() && s.folder.join("old").is_dir() == update, "{label}"),
    }
}

/// A first copy (nothing was at its place) stopped at each step: the next start, or an uninstall, leaves no staging
/// folder and no record; the next start places the copy.
#[test]
fn a_first_copy_stopped_at_any_step_leaves_no_staging_folder() {
    for uninstall in [false, true] {
        for (step, mid) in STEPS {
            let label = format!("{step:?}{} then {}", if mid { " mid copy" } else { "" }, if uninstall { "uninstall" } else { "next start" });
            let (_tmp, dirs, game, p) = world(&format!("staging-first-{step:?}-{mid}-{uninstall}"));
            assert!(stopped(step, || { let _ = place(&dirs, &p, &game); }), "{label}: the placement did not reach the step");
            let s = the_recorded_folder(&game, &label);
            check_stopped(step, mid, &s, false, &label);
            readable_by_30_2(&game, &label);
            if uninstall {
                unplace(&dirs, &game).unwrap_or_else(|e| panic!("{label}: {e:?}"));
            } else {
                let rep = place(&dirs, &p, &game).unwrap_or_else(|e| panic!("{label}: {e:?}"));
                assert!(rep.warnings.is_empty(), "{label}: {rep:?}");
                assert!(game.join("emf/mods/beastmaster/main.lua").is_file(), "{label}: {rep:?}");
            }
            assert!(leftovers(&game).is_empty(), "{label}: {:?}", leftovers(&game));
            assert!(!staging_file(&game).exists(), "{label}: the record is not cleared");
            readable_by_30_2(&game, &label);
        }
    }
}

/// An update (Kiln's copy of 1.0.0 replaced by 2.0.0) stopped at each step: the same, and the next start places 2.0.0.
#[test]
fn an_update_stopped_at_any_step_leaves_no_staging_folder() {
    for uninstall in [false, true] {
        for (step, mid) in STEPS {
            let label = format!("update {step:?}{} then {}", if mid { " mid copy" } else { "" }, if uninstall { "uninstall" } else { "next start" });
            let (tmp, dirs, game, mut p) = world(&format!("staging-update-{step:?}-{mid}-{uninstall}"));
            place(&dirs, &p, &game).unwrap();
            assert!(!staging_file(&game).exists() && leftovers(&game).is_empty(), "{label}: a clean placement leaves its record");
            p.mods.emf = vec![super::tests::emf_lua_mod(&dirs, tmp.path(), "2.0.0")];
            assert!(stopped(step, || { let _ = place(&dirs, &p, &game); }), "{label}: the placement did not reach the step");
            let s = the_recorded_folder(&game, &label);
            check_stopped(step, mid, &s, true, &label);
            readable_by_30_2(&game, &label);
            let copy = game.join("emf/mods/beastmaster");
            if uninstall {
                unplace(&dirs, &game).unwrap_or_else(|e| panic!("{label}: {e:?}"));
                assert!(!copy.exists(), "{label}");
            } else {
                let rep = place(&dirs, &p, &game).unwrap_or_else(|e| panic!("{label}: {e:?}"));
                assert!(rep.warnings.is_empty(), "{label}: {rep:?}");
                assert_eq!(marker(&copy).map(|m| m.version).as_deref(), Some("2.0.0"), "{label}: {rep:?}");
            }
            assert!(leftovers(&game).is_empty(), "{label}: {:?}", leftovers(&game));
            assert!(!staging_file(&game).exists(), "{label}: the record is not cleared");
            readable_by_30_2(&game, &label);
        }
    }
}

/// The record is a file of its own (older Kilns never read it), and the journal's entries keep exactly the fields
/// 0.2.0-dev.30.2 knows while a staging folder is recorded.
#[test]
fn the_staging_record_is_its_own_file_and_the_journal_keeps_its_fields() {
    let (tmp, dirs, game, mut p) = world("staging-downgrade");
    place(&dirs, &p, &game).unwrap();
    p.mods.emf = vec![super::tests::emf_lua_mod(&dirs, tmp.path(), "2.0.0")];
    assert!(stopped(Step::Parked, || { let _ = place(&dirs, &p, &game); }));
    let journal: serde_json::Value = serde_json::from_slice(&std::fs::read(journal_file(&game)).unwrap()).unwrap();
    for entry in journal.as_array().unwrap() {
        let keys: Vec<&str> = entry.as_object().unwrap().keys().map(String::as_str).collect();
        assert!(keys.iter().all(|k| ["path", "from", "to", "parked"].contains(k)), "{keys:?}");
    }
    let record: serde_json::Value = serde_json::from_slice(&std::fs::read(staging_file(&game)).unwrap()).unwrap();
    assert_eq!(record["format"], 1);
    assert_eq!(record["folders"].as_array().unwrap().len(), 1, "{record}");
    readable_by_30_2(&game, "parked update");
}

/// Copies a placed copy's files, marker included, as a staged copy would hold them.
fn staged_like(copy: &Path, to: &Path) {
    std::fs::create_dir_all(to).unwrap();
    for e in std::fs::read_dir(copy).unwrap().flatten() {
        if e.file_type().unwrap().is_file() { std::fs::copy(e.path(), to.join(e.file_name())).unwrap(); }
    }
}

/// Only a folder the record names, of Kiln's shape, beside the copy it stages, in its engine mod folder, holding nothing
/// but a staged copy of the recorded mod, is taken out. Everything else stays byte for byte, and so do its entries.
#[test]
fn a_staging_folder_not_in_the_record_or_not_kilns_is_never_touched() {
    let (tmp, dirs, game, p) = world("staging-untouched");
    place(&dirs, &p, &game).unwrap();
    let mods = game.join("emf/mods");
    let copy = mods.join("beastmaster");
    let m = marker(&copy).unwrap();
    let at = |folder: PathBuf, path: &Path| Staging { folder, path: path.to_path_buf(), marker: m.clone(), game_folder: false };
    // not in the record, though of Kiln's shape and holding a staged copy of this mod
    let unlisted = mods.join(".kiln-place-0123456789ab");
    staged_like(&copy, &unlisted.join("new"));
    // in the record, but not verified
    let parked = mods.join(".kiln-place-00000000000a");
    staged_like(&copy, &parked.join("new"));
    staged_like(&copy, &parked.join("old"));
    let extra = mods.join(".kiln-place-00000000000b");
    staged_like(&copy, &extra.join("new"));
    std::fs::write(extra.join("notes.txt"), b"player").unwrap();
    let other_marker = mods.join(".kiln-place-00000000000c");
    staged_like(&copy, &other_marker.join("new"));
    let mut other = m.clone();
    other.version = "9.9.9".into();
    crate::util::write_json_atomic(&other_marker.join("new").join(MARKER), &other).unwrap();
    let named = mods.join(".kiln-place-player");
    staged_like(&copy, &named.join("new"));
    let not_beside = game.join(".kiln-place-00000000000d");
    staged_like(&copy, &not_beside.join("new"));
    let elsewhere = game.join("other/.kiln-place-00000000000e");
    staged_like(&copy, &elsewhere.join("new"));
    let file_not_folder = mods.join(".kiln-place-00000000000f");
    std::fs::write(&file_not_folder, b"player").unwrap();
    #[cfg_attr(not(unix), allow(unused_mut))]
    let mut record = vec![
        at(parked.clone(), &copy), at(extra.clone(), &copy), at(other_marker.clone(), &copy), at(named.clone(), &copy),
        at(not_beside.clone(), &copy), at(elsewhere.clone(), &game.join("other/beastmaster")), at(file_not_folder.clone(), &copy),
    ];
    #[cfg(unix)]
    let outside = tmp.path().join("outside");
    #[cfg(unix)]
    let (linked, new_linked, link_inside) = {
        staged_like(&copy, &outside.join("new"));
        let linked = mods.join(".kiln-place-000000000010");
        std::os::unix::fs::symlink(&outside, &linked).unwrap();
        let new_linked = mods.join(".kiln-place-000000000011");
        std::fs::create_dir_all(&new_linked).unwrap();
        std::os::unix::fs::symlink(outside.join("new"), new_linked.join("new")).unwrap();
        let link_inside = mods.join(".kiln-place-000000000012");
        staged_like(&copy, &link_inside.join("new"));
        std::os::unix::fs::symlink(outside.join("new/main.lua"), link_inside.join("new/link.lua")).unwrap();
        record.extend([at(linked.clone(), &copy), at(new_linked.clone(), &copy), at(link_inside.clone(), &copy)]);
        (linked, new_linked, link_inside)
    };
    #[cfg(not(unix))]
    let _ = &tmp;
    save_stagings(&game, &record).unwrap();
    let snapshot = |dir: &Path| -> Vec<(String, Vec<u8>)> {
        let mut out = Vec::new();
        let mut stack = vec![dir.to_path_buf()];
        while let Some(d) = stack.pop() {
            for e in std::fs::read_dir(&d).unwrap().flatten() {
                let meta = std::fs::symlink_metadata(e.path()).unwrap();
                let rel = e.path().strip_prefix(dir).unwrap().to_string_lossy().replace('\\', "/");
                if meta.file_type().is_symlink() { out.push((rel, b"link".to_vec())); }
                else if meta.is_dir() { stack.push(e.path()); out.push((rel, Vec::new())); }
                else { out.push((rel, std::fs::read(e.path()).unwrap())); }
            }
        }
        out.sort();
        out
    };
    let before = snapshot(tmp.path());
    let rep = place(&dirs, &p, &game).unwrap();
    assert!(rep.warnings.is_empty(), "{rep:?}");
    unplace(&dirs, &game).unwrap();
    let after = snapshot(tmp.path());
    // the uninstall took out the copy and its receipt, nothing of the staging folders
    let gone: Vec<&String> = before.iter().filter(|b| !after.contains(b)).map(|(rel, _)| rel).collect();
    assert!(gone.iter().all(|rel| rel.starts_with("game/emf/mods/beastmaster") || rel.starts_with("game/kiln")), "{gone:?}");
    for dir in [&unlisted, &parked, &extra, &other_marker, &named, &not_beside, &elsewhere] {
        assert!(dir.join("new/main.lua").is_file(), "{}", dir.display());
    }
    assert!(parked.join("old/main.lua").is_file() && file_not_folder.is_file());
    #[cfg(unix)]
    {
        assert!(std::fs::symlink_metadata(&linked).unwrap().file_type().is_symlink() && outside.join("new/main.lua").is_file());
        assert!(std::fs::symlink_metadata(new_linked.join("new")).unwrap().file_type().is_symlink());
        assert!(link_inside.join("new/link.lua").exists() && link_inside.join("new/main.lua").is_file());
    }
    let kept: Vec<PathBuf> = stagings(&game).unwrap().into_iter().map(|s| s.folder).collect();
    assert_eq!(kept, record.iter().map(|s| s.folder.clone()).collect::<Vec<_>>(), "every entry whose folder stays is kept");
}

/// A recorded folder that is gone is dropped from the record, and a record Kiln cannot read is left as it is (it names
/// nothing, so nothing is removed for it).
#[test]
fn a_record_names_only_what_is_there_and_an_unreadable_one_is_left_alone() {
    let (_tmp, dirs, game, p) = world("staging-record");
    place(&dirs, &p, &game).unwrap();
    let copy = game.join("emf/mods/beastmaster");
    let gone = Staging { folder: game.join("emf/mods/.kiln-place-abcdefabcdef"), path: copy.clone(), marker: marker(&copy).unwrap(), game_folder: false };
    save_stagings(&game, &[gone]).unwrap();
    place(&dirs, &p, &game).unwrap();
    assert!(!staging_file(&game).exists());
    let stray = game.join("emf/mods/.kiln-place-abcdefabcdef");
    staged_like(&copy, &stray.join("new"));
    std::fs::write(staging_file(&game), b"{\"format\":1,").unwrap();
    place(&dirs, &p, &game).unwrap();
    assert_eq!(std::fs::read(staging_file(&game)).unwrap(), b"{\"format\":1,");
    assert!(stray.join("new/main.lua").is_file());
}

/// An entry goes only once its folder's removal is on disk (its parent folder flushed, Unix), never before: a power cut
/// cannot leave a staging folder the record no longer names.
#[test]
fn an_entry_ends_only_after_its_folders_removal_is_flushed() {
    use crate::durable::Event;
    let (_tmp, dirs, game, p) = world("staging-flush");
    place(&dirs, &p, &game).unwrap();
    let mods = game.join("emf/mods");
    let copy = mods.join("beastmaster");
    let m = marker(&copy).unwrap();
    // an entry that stays (its folder is not of Kiln's shape), so the end of the next one rewrites the record
    let stays = Staging { folder: mods.join(".kiln-place-player"), path: copy.clone(), marker: m.clone(), game_folder: false };
    std::fs::create_dir_all(stays.folder.join("new")).unwrap();
    save_stagings(&game, std::slice::from_ref(&stays)).unwrap();
    let (folder, events) = crate::durable::recording(|| {
        let guard = StagingGuard::begin(&game, &copy, &m, false).unwrap();
        drop(guard.create().unwrap());
        let folder = guard.folder.clone();
        drop(guard);
        folder
    });
    assert!(!folder.exists());
    let record = staging_file(&game);
    let writes: Vec<usize> = events.iter().enumerate()
        .filter(|(_, e)| matches!(e, Event::Journal { path, .. } if *path == record)).map(|(i, _)| i).collect();
    assert_eq!(writes.len(), 2, "{events:?}");
    assert!(events[writes[0]..writes[1]].contains(&Event::Dir(mods.clone())), "the entry ended before the folder's removal was flushed: {events:?}");
    assert_eq!(stagings(&game).unwrap().into_iter().map(|s| s.folder).collect::<Vec<_>>(), vec![stays.folder]);
}

/// A record larger than Kiln ever writes is never read whole: it names none (nothing is removed for it), and it never
/// stops a placement, whose staging folder's entry replaces it.
#[test]
fn an_oversized_record_names_nothing_and_never_stops_a_placement() {
    let (tmp, dirs, game, mut p) = world("staging-oversized");
    place(&dirs, &p, &game).unwrap();
    let copy = game.join("emf/mods/beastmaster");
    let stray = game.join("emf/mods/.kiln-place-abcdefabcdef");
    staged_like(&copy, &stray.join("new"));
    // a valid record naming a folder Kiln would take out, padded past the bound
    let entry = Staging { folder: stray.clone(), path: copy.clone(), marker: marker(&copy).unwrap(), game_folder: false };
    let mut text = serde_json::to_vec(&StagingRecord { format: 1, folders: vec![entry] }).unwrap();
    text.resize(usize::try_from(MAX_STAGING_RECORD).unwrap() + 1, b' ');
    std::fs::write(staging_file(&game), &text).unwrap();
    let rep = place(&dirs, &p, &game).unwrap();
    assert!(rep.warnings.is_empty(), "{rep:?}");
    assert!(stray.join("new/main.lua").is_file(), "a folder named only by an oversized record was removed");
    p.mods.emf = vec![super::tests::emf_lua_mod(&dirs, tmp.path(), "2.0.0")];
    let rep = place(&dirs, &p, &game).unwrap();
    assert!(rep.warnings.is_empty(), "{rep:?}");
    assert_eq!(marker(&copy).map(|m| m.version).as_deref(), Some("2.0.0"), "{rep:?}");
    assert!(!staging_file(&game).exists(), "the update's own entry replaced the oversized record and ended");
    assert!(stray.join("new/main.lua").is_file());
}

/// Windows (NTFS): a file of the staged copy held open without delete sharing (an antivirus scan, an editor) keeps the
/// folder and its entry, the placement goes on and says so; once the file is free the next start takes the folder out,
/// a read-only file too.
#[cfg(windows)]
#[test]
fn a_staging_folder_windows_holds_stays_until_it_is_free() {
    use std::os::windows::fs::OpenOptionsExt;
    let (_tmp, dirs, game, p) = world("staging-held");
    assert!(stopped(Step::StagingCopied, || { let _ = place(&dirs, &p, &game); }));
    let s = the_recorded_folder(&game, "held");
    let ro = s.folder.join("new/mod.json");
    let mut perm = std::fs::metadata(&ro).unwrap().permissions();
    perm.set_readonly(true);
    std::fs::set_permissions(&ro, perm).unwrap();
    // FILE_SHARE_READ only: no FILE_SHARE_DELETE
    let held = std::fs::OpenOptions::new().read(true).share_mode(1).open(s.folder.join("new/main.lua")).unwrap();
    let rep = place(&dirs, &p, &game).unwrap();
    assert!(rep.warnings.iter().any(|w| w.contains("could not be removed yet")), "{rep:?}");
    assert!(s.folder.exists() && stagings(&game).unwrap().iter().any(|x| x.folder == s.folder));
    assert!(game.join("emf/mods/beastmaster/main.lua").is_file(), "the placement goes on: {rep:?}");
    drop(held);
    let rep = place(&dirs, &p, &game).unwrap();
    assert!(rep.warnings.is_empty(), "{rep:?}");
    assert!(leftovers(&game).is_empty() && !staging_file(&game).exists(), "{:?}", leftovers(&game));
}

/// Windows (NTFS): an update whose old copy has a file held open without delete sharing. However the park of the old
/// copy goes (refused, or allowed), once the file is free the next start ends with the new version and no staging folder.
#[cfg(windows)]
#[test]
fn an_update_with_a_held_old_copy_leaves_no_staging_folder_once_free() {
    use std::os::windows::fs::OpenOptionsExt;
    let (tmp, dirs, game, mut p) = world("staging-held-update");
    place(&dirs, &p, &game).unwrap();
    p.mods.emf = vec![super::tests::emf_lua_mod(&dirs, tmp.path(), "2.0.0")];
    let copy = game.join("emf/mods/beastmaster");
    let held = std::fs::OpenOptions::new().read(true).share_mode(1).open(copy.join("main.lua")).unwrap();
    let first = place(&dirs, &p, &game);
    eprintln!("with the old copy held: {first:?}; staging folders {:?}", leftovers(&game));
    drop(held);
    let rep = place(&dirs, &p, &game).unwrap();
    assert_eq!(marker(&copy).map(|m| m.version).as_deref(), Some("2.0.0"), "{rep:?}");
    assert!(leftovers(&game).is_empty() && !staging_file(&game).exists(), "{:?}", leftovers(&game));
}

/// Windows (NTFS): a recorded staging folder that is a junction is never followed: its target and the junction stay.
#[cfg(windows)]
#[test]
fn a_recorded_staging_folder_that_is_a_junction_is_never_followed() {
    let (tmp, dirs, game, p) = world("staging-junction");
    place(&dirs, &p, &game).unwrap();
    let copy = game.join("emf/mods/beastmaster");
    let outside = tmp.path().join("outside");
    staged_like(&copy, &outside.join("new"));
    let folder = game.join("emf/mods/.kiln-place-0123456789ab");
    let win = |p: &Path| p.to_string_lossy().replace('/', "\\");
    let made = std::process::Command::new("cmd").args(["/C", "mklink", "/J"]).arg(win(&folder)).arg(win(&outside)).output().unwrap();
    assert!(made.status.success(), "{made:?}");
    save_stagings(&game, &[Staging { folder: folder.clone(), path: copy.clone(), marker: marker(&copy).unwrap(), game_folder: false }]).unwrap();
    place(&dirs, &p, &game).unwrap();
    unplace(&dirs, &game).unwrap();
    assert!(outside.join("new/main.lua").is_file(), "the junction's target was removed");
    assert!(std::fs::symlink_metadata(&folder).is_ok(), "the junction was removed");
    assert_eq!(stagings(&game).unwrap().len(), 1);
    std::fs::remove_dir(&folder).unwrap();
}
