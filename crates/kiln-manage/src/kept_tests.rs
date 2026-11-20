//! Kept versions under stops and failures (kept.rs: the transaction, `reconcile`, `restore_after_place`), and restores
//! that belong to one profile and one game. A stop is a panic at a step (`hooks::STOP_AT`), as a crash would leave it; a
//! failed write is `hooks::FAIL_SAVE`. After each, "the next start" (what the launcher does: `recover`, the Library
//! list, the watcher's re-apply of the active profile) must leave the mod's settings all the old ones or all the new
//! ones, never a mix, the copy in the game the version the profile names, and nothing outside `kiln-kept/` touched by
//! the recovery itself.

use super::*;
use crate::game::{GameInstall, GameKind, PathMode, Platform};
use crate::task::NoProgress;
use std::panic::{catch_unwind, AssertUnwindSafe};

struct World {
    root: PathBuf,
    dirs: DataDirs,
    g: GameInstall,
}

impl Drop for World {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.root);
    }
}

fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}

fn install(root: &Path, name: &str, dirs: &DataDirs) -> GameInstall {
    let game = root.join(name);
    put(&game, "enshrouded.exe", b"MZ");
    std::fs::create_dir_all(game.join("kiln")).unwrap();
    let pfx = root.join(format!("{name}-pfx"));
    std::fs::create_dir_all(pfx.join("dosdevices")).unwrap();
    #[cfg(unix)]
    std::os::unix::fs::symlink("/", pfx.join("dosdevices/z:")).unwrap();
    let g = GameInstall {
        id: format!("g-{name}"), kind: GameKind::Client, path: game, exe: "enshrouded.exe".into(),
        app_id: crate::CLIENT_APP_ID, library: None, platform: Platform::Proton, prefix: Some(pfx), build_id: None,
        exe_sha256: None, steam_flavor: None, path_mode: PathMode::Z, source: "manual".into(),
    };
    crate::game::write_record(dirs, &g, None).unwrap();
    g
}

fn world(tag: &str) -> World {
    let root = std::env::temp_dir().join(format!("km-kept-crash-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    std::fs::create_dir_all(&root).unwrap();
    let root = root.canonicalize().unwrap();
    let dirs = DataDirs::at(root.join("data"));
    let g = install(&root, "game", &dirs);
    World { root, dirs, g }
}

const MAP: &str = "greeneye/worstmap";
const A: &str = "greeneye/worstmap/minimap_config.ini";
const B: &str = "greeneye/worstmap/looted_chests.ini";

impl World {
    fn greeneye(&self, version: &str) -> PathBuf {
        let d = self.root.join("src/worstmap");
        put(&d, "mod.json", format!(r#"{{"id":"worstmap","name":"The Worst Map Mod","version":"{version}","dll":"map.dll","side":"client"}}"#).as_bytes());
        put(&d, "map.dll", &crate::pe::build_test_dll(&["GreeneyeModStart"], version.as_bytes()));
        d
    }
    fn drop_in(&self, g: &GameInstall, version: &str) -> crate::quick::AddReport {
        crate::quick::add_with(&self.dirs, g, &[self.greeneye(version)], false, &mut NoProgress).unwrap()
    }
    fn pid(&self, g: &GameInstall) -> String {
        crate::profile::read_game_profile(&g.path).unwrap().expect("profile.json").id
    }
    fn version_in(&self, g: &GameInstall) -> String {
        crate::profile::get(&self.dirs, &self.pid(g)).unwrap().mods.greeneye[0].version.clone()
    }
    fn write(&self, g: &GameInstall, value: &str) {
        put(&g.path, A, format!("zoom={value}").as_bytes());
        put(&g.path, B, format!("chests={value}").as_bytes());
    }
    /// The settings in the game's copy, and the version the copy is.
    fn settings(&self, g: &GameInstall) -> (String, String, String) {
        let read = |rel: &str| std::fs::read_to_string(g.path.join(rel)).unwrap_or_default();
        (crate::engine_mods::marker(&g.path.join(MAP)).map(|m| m.version).unwrap_or_default(), read(A), read(B))
    }
    /// v1 with settings 1, then the update to v2, whose settings are 9.
    fn updated(tag: &str) -> World {
        let w = world(tag);
        w.drop_in(&w.g, "1.0.0");
        w.write(&w.g, "1");
        w.drop_in(&w.g, "2.0.0");
        w.write(&w.g, "9");
        w
    }
    fn rollback(&self) -> Result<RollbackReport> {
        rollback(&self.dirs, Some(&self.g), &self.pid(&self.g), ModKind::Greeneye, "worstmap", None)
    }
    /// The next start: the recovery, the Library list, the re-apply of the active profile (the watcher's first pass).
    fn next_start(&self) -> Vec<String> {
        let notes = recover(&self.dirs);
        crate::library::list(&self.dirs, None).unwrap();
        crate::profile::refresh_active(&self.dirs, &self.g).unwrap();
        notes
    }
    fn kept(&self) -> Vec<String> {
        kept_versions(&self.dirs, ModKind::Greeneye, "worstmap").into_iter().map(|v| v.version).collect()
    }
    /// Every file below the game folder and the Library entries, with its bytes.
    /// Every file and folder of the test's world (the data folder, the games, the sources) but the kept versions' own
    /// (`library/kiln-kept/`, `kiln-kept.json` and its lock), with each file's bytes; links are listed, never followed.
    fn outside_kept(&self) -> BTreeMap<String, Vec<u8>> {
        let lib = self.dirs.library();
        let skip = [lib.join(SETTINGS_DIR), lib.join(STATE_FILE), lib.join(LOCK_FILE)];
        let mut out = BTreeMap::new();
        let mut stack = vec![self.root.clone()];
        while let Some(d) = stack.pop() {
            for e in std::fs::read_dir(&d).into_iter().flatten().flatten() {
                let p = e.path();
                if skip.contains(&p) { continue; }
                let meta = std::fs::symlink_metadata(&p).unwrap();
                let key = p.display().to_string();
                if meta.file_type().is_symlink() { out.insert(key, std::fs::read_link(&p).unwrap().display().to_string().into_bytes()); }
                else if meta.is_dir() { out.insert(format!("{key}/"), Vec::new()); stack.push(p); }
                else { out.insert(key, std::fs::read(&p).unwrap_or_default()); }
            }
        }
        out
    }
    /// Nothing below `kiln-kept/` that the state does not name, and no staged leftover.
    fn tidy_on_disk(&self) {
        let st = load(&self.dirs).unwrap();
        assert!(st.pending.is_empty(), "no change left unfinished: {:?}", st.pending);
        let named: Vec<String> = st.mods.iter().flat_map(|m| m.kept.iter().map(|v| v.settings.dir.clone()).chain(m.restores.iter().map(|r| r.settings.dir.clone())))
            .flatten().collect();
        let dir = self.dirs.library().join(SETTINGS_DIR).join("greeneye/worstmap");
        let on_disk: Vec<String> = std::fs::read_dir(&dir).into_iter().flatten().flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect();
        for name in &on_disk {
            assert!(named.contains(name), "{name} is in kiln-kept but the state does not name it ({named:?})");
        }
    }
}

/// Runs `f` and stops it at `step`, as a crash there would.
fn crash_at(step: Step, f: impl FnOnce()) {
    hooks::STOP_AT.with(|s| s.set(Some(step)));
    let stopped = catch_unwind(AssertUnwindSafe(f)).is_err();
    hooks::STOP_AT.with(|s| s.set(None));
    assert!(stopped, "the change did not reach {step:?}");
}

fn fail_nth_save(n: u32) {
    hooks::FAIL_SAVE.with(|f| f.set(n));
}

/// What the recovery itself writes is the state file and kiln-kept/ only.
fn recover_touches_only_kept(w: &World) -> Vec<String> {
    let before = w.outside_kept();
    let notes = recover(&w.dirs);
    assert_eq!(w.outside_kept(), before, "the recovery touched files outside kiln-kept");
    notes
}

const OLD: (&str, &str) = ("zoom=1", "chests=1");
const NEW: (&str, &str) = ("zoom=9", "chests=9");

fn assert_settings(w: &World, version: &str, values: (&str, &str)) {
    let (copy, a, b) = w.settings(&w.g);
    assert_eq!(w.version_in(&w.g), version, "the profile's version");
    assert_eq!((copy.as_str(), a.as_str(), b.as_str()), (version, values.0, values.1), "the copy's version and settings");
}

#[test]
fn a_roll_back_stopped_while_its_copy_is_staged_or_renamed_changes_nothing() {
    for step in [Step::SnapshotCopied, Step::SnapshotRenamed] {
        let w = World::updated(&format!("stage-{step:?}"));
        crash_at(step, || { let _ = w.rollback(); });
        recover_touches_only_kept(&w);
        w.next_start();
        assert_settings(&w, "2.0.0", NEW);
        assert_eq!(w.kept(), ["1.0.0"], "{step:?}: the old version is still kept");
        w.tidy_on_disk();
        // and the roll back still works, with the old settings
        w.rollback().unwrap();
        w.next_start();
        assert_settings(&w, "1.0.0", OLD);
    }
}

#[test]
fn a_roll_back_stopped_after_its_record_before_the_profile_save_is_undone() {
    let w = World::updated("index");
    crash_at(Step::IndexWritten, || { let _ = w.rollback(); });
    assert!(!load(&w.dirs).unwrap().pending.is_empty(), "the record is on disk");
    recover_touches_only_kept(&w);
    w.next_start();
    assert_settings(&w, "2.0.0", NEW);
    assert_eq!(w.kept(), ["1.0.0"]);
    assert!(pending_restores(&w.dirs, ModKind::Greeneye, "worstmap").is_empty());
    w.tidy_on_disk();
}

#[test]
fn a_roll_back_stopped_after_the_profile_save_is_finished_at_the_next_start() {
    let w = World::updated("commit");
    crash_at(Step::BeforeCommit, || { let _ = w.rollback(); });
    assert_eq!(w.version_in(&w.g), "1.0.0", "the profile was saved");
    recover_touches_only_kept(&w);
    w.next_start();
    assert_settings(&w, "1.0.0", OLD);
    assert_eq!(w.kept(), ["2.0.0"]);
    w.tidy_on_disk();
    // roll back twice still returns to the newer version with its settings
    w.rollback().unwrap();
    w.next_start();
    assert_settings(&w, "2.0.0", NEW);
}

#[test]
fn a_roll_back_whose_record_cannot_be_written_changes_nothing() {
    let w = World::updated("fail-prepare");
    fail_nth_save(1);
    let e = w.rollback().unwrap_err();
    assert!(e.message.contains("was not rolled back") && e.message.contains("Nothing was changed"), "{e:?}");
    w.next_start();
    assert_settings(&w, "2.0.0", NEW);
    assert_eq!(w.kept(), ["1.0.0"]);
    w.tidy_on_disk();
}

#[test]
fn a_roll_back_whose_commit_cannot_be_written_is_finished_at_the_next_start() {
    let w = World::updated("fail-commit");
    fail_nth_save(2); // the record's write goes through, the commit's fails
    let r = w.rollback().unwrap();
    assert!(r.notices.iter().any(|n| n.contains("finishes keeping the earlier versions at its next start")), "{:?}", r.notices);
    w.next_start();
    assert_settings(&w, "1.0.0", OLD);
    w.tidy_on_disk();
}

#[cfg(unix)]
#[test]
fn a_roll_back_whose_profile_save_fails_is_undone_at_once() {
    use std::os::unix::fs::PermissionsExt;
    let w = World::updated("fail-profile");
    let pdir = w.dirs.profile(&w.pid(&w.g));
    std::fs::set_permissions(&pdir, std::fs::Permissions::from_mode(0o555)).unwrap();
    let r = w.rollback();
    std::fs::set_permissions(&pdir, std::fs::Permissions::from_mode(0o755)).unwrap();
    assert!(r.is_err(), "the profile could not be written: {r:?}");
    assert!(load(&w.dirs).unwrap().pending.is_empty(), "undone at once, no record left");
    w.next_start();
    assert_settings(&w, "2.0.0", NEW);
    assert_eq!(w.kept(), ["1.0.0"]);
    w.tidy_on_disk();
}

#[test]
fn a_restore_stopped_half_way_is_finished_by_the_next_placement() {
    let w = World::updated("restore");
    w.rollback().unwrap();
    crash_at(Step::RestoreFile, || { let _ = crate::profile::refresh_active(&w.dirs, &w.g); });
    // right after the stop the copy holds one old file and one carried over: the restore is still recorded
    assert_eq!(pending_restores(&w.dirs, ModKind::Greeneye, "worstmap").len(), 1);
    w.next_start();
    assert_settings(&w, "1.0.0", OLD);
    assert!(pending_restores(&w.dirs, ModKind::Greeneye, "worstmap").is_empty());
    w.tidy_on_disk();
}

#[test]
fn a_restore_whose_done_mark_cannot_be_written_is_done_again_without_harm() {
    let w = World::updated("restore-fail");
    w.rollback().unwrap();
    fail_nth_save(1); // the write that marks the restore done
    let a = crate::profile::refresh_active(&w.dirs, &w.g).unwrap();
    assert!(a.is_some_and(|a| a.kept_files.iter().any(|l| l.contains("tries again at the next start"))));
    assert_settings(&w, "1.0.0", OLD);
    assert_eq!(pending_restores(&w.dirs, ModKind::Greeneye, "worstmap").iter().filter(|r| !r.applied).count(), 1, "not marked done");
    // done again at the next placement: the same old bytes, nothing else
    w.next_start();
    assert_settings(&w, "1.0.0", OLD);
    assert!(pending_restores(&w.dirs, ModKind::Greeneye, "worstmap").is_empty());
    w.tidy_on_disk();
}

/// The restore is marked done, and dropping its record fails: the player changes a setting before the next start,
/// and the next start drops the record without putting the old settings back over that change.
#[test]
fn a_restore_whose_record_cannot_be_dropped_keeps_the_players_later_change() {
    let w = World::updated("restore-drop-fail");
    w.rollback().unwrap();
    fail_nth_save(2); // the done mark is written, the drop fails
    let a = crate::profile::refresh_active(&w.dirs, &w.g).unwrap();
    assert_settings(&w, "1.0.0", OLD);
    put(&w.g.path, A, b"zoom=7"); // the player's change
    w.next_start();
    assert_settings(&w, "1.0.0", ("zoom=7", "chests=1"));
    assert!(pending_restores(&w.dirs, ModKind::Greeneye, "worstmap").is_empty());
    w.tidy_on_disk();
    assert!(a.is_some_and(|a| a.kept_files.iter().any(|l| l.contains("tidies the settings it put back at its next start"))));
}

#[test]
fn an_update_stopped_before_or_after_its_profile_save_ends_whole() {
    // before the save: the update did not happen
    let w = world("update-index");
    w.drop_in(&w.g, "1.0.0");
    w.write(&w.g, "1");
    crash_at(Step::IndexWritten, || { w.drop_in(&w.g, "2.0.0"); });
    recover_touches_only_kept(&w);
    w.next_start();
    assert_settings(&w, "1.0.0", OLD);
    assert!(w.kept().is_empty());
    w.tidy_on_disk();
    // after the save: the update stands, the old version is kept with its settings
    let w = world("update-commit");
    w.drop_in(&w.g, "1.0.0");
    w.write(&w.g, "1");
    crash_at(Step::BeforeCommit, || { w.drop_in(&w.g, "2.0.0"); });
    recover_touches_only_kept(&w);
    w.next_start();
    assert_settings(&w, "2.0.0", OLD); // carried over, as every update does
    assert_eq!(w.kept(), ["1.0.0"]);
    w.write(&w.g, "9");
    w.rollback().unwrap();
    w.next_start();
    assert_settings(&w, "1.0.0", OLD);
    w.tidy_on_disk();
}

#[test]
fn the_next_start_repairs_what_the_state_and_the_disk_disagree_on() {
    // a settings copy without an entry in the state, and a staged leftover: removed
    let w = World::updated("orphans");
    let dir = w.dirs.library().join(SETTINGS_DIR).join("greeneye/worstmap");
    put(&dir, "1.0.0+deadbeef.0123456789ab/greeneye/worstmap/minimap_config.ini", b"zoom=5");
    put(&dir, ".stage-0123456789ab/greeneye/worstmap/minimap_config.ini", b"zoom=5");
    recover_touches_only_kept(&w);
    w.tidy_on_disk();
    assert_eq!(w.kept(), ["1.0.0"]);
    // an entry whose settings copy is gone: dropped with a note
    let st = load(&w.dirs).unwrap();
    let gone = dir.join(st.mods[0].kept[0].settings.dir.as_deref().unwrap());
    std::fs::remove_dir_all(&gone).unwrap();
    let notes = recover_touches_only_kept(&w);
    assert!(notes.iter().any(|n| n.contains("1.0.0") && n.contains("are gone")), "{notes:?}");
    assert!(w.kept().is_empty());
    // a kept version the profile that left it uses again (an older launcher's "Use 1.0.0"): no longer kept
    let w = World::updated("used-again");
    let mut p = crate::profile::get(&w.dirs, &w.pid(&w.g)).unwrap();
    let one = crate::library::list(&w.dirs, Some(ModKind::Greeneye)).unwrap()[0].versions.iter().find(|v| v.version == "1.0.0").cloned().unwrap();
    p.mods.greeneye[0].version = one.version.clone();
    p.mods.greeneye[0].hash = one.hash.clone();
    crate::profile::save(&w.dirs, &p).unwrap();
    let notes = recover_touches_only_kept(&w);
    assert!(notes.iter().any(|n| n.contains("uses it again")), "{notes:?}");
    assert!(w.kept().is_empty());
    w.tidy_on_disk();
}

/// Two profiles in two games, the same mod: a roll back's restore belongs to its profile and to the game its settings
/// came from. The other profile's placement, change and update leave it alone; the same profile placed in the other
/// game does not apply it either.
#[test]
fn a_restore_belongs_to_its_profile_and_its_game() {
    let w = World::updated("two");
    let p = w.pid(&w.g);
    let g2 = install(&w.root, "game2", &w.dirs);
    let q = crate::profile::create(&w.dirs, "Second", None).unwrap();
    let mut q2 = q.clone();
    let one = crate::library::list(&w.dirs, Some(ModKind::Greeneye)).unwrap()[0].versions.iter().find(|v| v.version == "1.0.0").cloned().unwrap();
    q2.mods.greeneye.push(ProfileMod { id: "worstmap".into(), version: one.version.clone(), hash: one.hash.clone(), enabled: true });
    let q = crate::profile::save(&w.dirs, &q2).unwrap();
    crate::profile::activate(&w.dirs, &q.id, &g2).unwrap();
    w.write(&g2, "5");
    // P rolls back in the first game: its restore waits for P's placement there
    w.rollback().unwrap();
    assert_eq!(pending_restores(&w.dirs, ModKind::Greeneye, "worstmap").iter().map(|r| r.profile.as_str()).collect::<Vec<_>>(), [p.as_str()]);
    // Q, which uses the same version in the second game, is placed: P's restore is not applied there, nor taken
    crate::profile::refresh_active(&w.dirs, &g2).unwrap();
    assert_eq!(w.settings(&g2), ("1.0.0".into(), "zoom=5".into(), "chests=5".into()));
    assert_eq!(pending_restores(&w.dirs, ModKind::Greeneye, "worstmap").len(), 1);
    // Q updates to 2.0.0 in the second game: its own change keeps its 1.0.0, and P's restore is still there
    w.drop_in(&g2, "2.0.0");
    assert_eq!(w.version_in(&g2), "2.0.0");
    assert_eq!(pending_restores(&w.dirs, ModKind::Greeneye, "worstmap").iter().map(|r| r.profile.as_str()).collect::<Vec<_>>(), [p.as_str()]);
    // P's kept 2.0.0 stays P's (Q's change to that version does not take it), and Q's 1.0.0 is Q's own
    let kept: Vec<(String, Option<String>)> = kept_versions(&w.dirs, ModKind::Greeneye, "worstmap").into_iter().map(|v| (v.version, v.profile)).collect();
    assert!(kept.contains(&("2.0.0".into(), Some(p.clone()))) && kept.contains(&("1.0.0".into(), Some(q.id.clone()))), "{kept:?}");
    // P placed in the second game (it is active there now too): not the game its settings came from, not applied
    crate::profile::activate(&w.dirs, &p, &g2).unwrap();
    assert_eq!(w.settings(&g2).0, "1.0.0");
    assert_ne!(w.settings(&g2).1, "zoom=1", "P's restore came from the first game");
    assert_eq!(pending_restores(&w.dirs, ModKind::Greeneye, "worstmap").len(), 1);
    // P placed in its own game: applied there
    crate::profile::refresh_active(&w.dirs, &w.g).unwrap();
    assert_settings(&w, "1.0.0", OLD);
    assert!(pending_restores(&w.dirs, ModKind::Greeneye, "worstmap").is_empty());
    w.tidy_on_disk();
}

#[test]
fn review_corrupt_state_never_sweeps_settings_copies() {
    for (tag, bytes) in [("truncated", "{\"format\":1,"), ("invalid-shape", "{\"format\":1,\"mods\":false}")] {
        let w = World::updated(tag);
        let before = load(&w.dirs).unwrap();
        let copy = copy_dir(&w.dirs, ModKind::Greeneye, "worstmap", before.mods[0].kept[0].settings.dir.as_deref().unwrap()).unwrap();
        std::fs::write(state_path(&w.dirs), bytes).unwrap();
        recover(&w.dirs);
        assert!(copy.join(A).is_file(), "{tag}: recovery deleted a backup it could not read");
        assert_eq!(std::fs::read_to_string(state_path(&w.dirs)).unwrap(), bytes);
    }
}

#[cfg(unix)]
#[test]
fn review_state_save_never_follows_its_temporary_link() {
    let w = World::updated("state-link");
    let outside = w.root.join("precious");
    std::fs::write(&outside, b"precious").unwrap();
    std::os::unix::fs::symlink(&outside, w.dirs.library().join("kiln-kept.json.tmp")).unwrap();
    save(&w.dirs, &load(&w.dirs).unwrap()).unwrap();
    assert_eq!(std::fs::read(&outside).unwrap(), b"precious");
}

#[test]
fn review_canceling_a_restore_before_profile_save_recovers_the_restore() {
    let w = World::updated("cancel-restore");
    w.rollback().unwrap();
    let before = crate::profile::get(&w.dirs, &w.pid(&w.g)).unwrap();
    let mut after = before.clone();
    after.mods.greeneye.clear();
    crash_at(Step::IndexWritten, || { let _ = prepare_saved(&w.dirs, &before, &after); });
    w.next_start();
    assert_settings(&w, "1.0.0", OLD);
}

#[test]
fn review_deleted_profiles_do_not_keep_unreachable_snapshots_forever() {
    let w = World::updated("deleted-profile");
    let pid = w.pid(&w.g);
    w.rollback().unwrap(); // both a kept version and a waiting restore
    crate::profile::delete(&w.dirs, &pid).unwrap();
    recover(&w.dirs);
    let st = load(&w.dirs).unwrap();
    assert!(st.mods.is_empty(), "deleted profile retains unreachable backups: {st:?}");
}

/// Disk state at a process stop after copy_in_with publishes the replacement folder but before
/// persist publishes its receipt. Keep the parked old tree as a real process stop would.
#[test]
fn review_stop_between_placement_and_receipt_must_finish_the_restore() {
    let w = World::updated("placement-receipt-gap");
    w.rollback().unwrap();
    let receipt_path = w.g.path.join("kiln/state/library-placements.json");
    let old_receipt = std::fs::read(&receipt_path).unwrap();
    let old_tree = w.g.path.join("greeneye/.kiln-place-review/old");
    crate::library::copy_mod_tree(&w.g.path.join(MAP), &old_tree).unwrap();
    let p = crate::profile::get(&w.dirs, &w.pid(&w.g)).unwrap();
    crate::engine_mods::place(&w.dirs, &p, &w.g.path).unwrap();
    // No kept restore has run. The new tree is published, but the on-disk receipt is still the old one.
    std::fs::write(&receipt_path, old_receipt).unwrap();
    w.next_start();
    assert_settings(&w, "1.0.0", OLD);
}

/// Kiln's temporary folders of a placement beside the mod's copy (`.kiln-place-<hex>`).
fn place_leftovers(w: &World) -> Vec<String> {
    std::fs::read_dir(w.g.path.join("greeneye")).into_iter().flatten().flatten().map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.starts_with(".kiln-place-")).collect()
}

/// The placement that follows a roll back, stopped after the old copy is parked, after the new one is published, after
/// its receipt, and after the game's profile.json: the next start finishes it (or puts the parked copy back and places
/// again), and the rolled back version has the settings it had, never the newer version's.
#[test]
fn a_placement_stopped_at_any_step_ends_with_the_rolled_back_settings() {
    for step in [Step::Parked, Step::Published, Step::ReceiptSaved, Step::GameProfileWritten] {
        let w = World::updated(&format!("place-{step:?}"));
        w.rollback().unwrap();
        crash_at(step, || { let _ = crate::profile::refresh_active(&w.dirs, &w.g); });
        recover_touches_only_kept(&w);
        w.next_start();
        assert_settings(&w, "1.0.0", OLD);
        assert!(place_leftovers(&w).is_empty(), "{step:?}: {:?}", place_leftovers(&w));
        assert!(pending_restores(&w.dirs, ModKind::Greeneye, "worstmap").is_empty(), "{step:?}");
        w.tidy_on_disk();
    }
}

/// An update's placement stopped after the new copy is published: the next start gives it its receipt, and the new
/// version runs with the settings carried over (as every update does), the old one kept with them.
#[test]
fn an_update_placement_stopped_after_its_publish_is_finished() {
    let w = world("update-publish");
    w.drop_in(&w.g, "1.0.0");
    w.write(&w.g, "1");
    crash_at(Step::Published, || { w.drop_in(&w.g, "2.0.0"); });
    w.next_start();
    assert_settings(&w, "2.0.0", OLD);
    assert_eq!(w.kept(), ["1.0.0"]);
    assert!(place_leftovers(&w).is_empty());
    w.tidy_on_disk();
}

/// The order of what an update, a roll back and its placement make durable (the seam `crate::durable`, recorded):
/// a kept-state record is written only after every settings folder it names (and the folder holding that name) was
/// flushed; the record that drops a change's pending entry only after the profile's meta.json and its folder were
/// flushed; the record that marks a restore done only after the restored files and their folders were flushed. A test
/// cannot cut the power: it checks the production call order, not hardware persistence.
#[test]
fn every_record_is_written_after_what_it_names_is_flushed() {
    use crate::durable::Event;
    let w = world("order");
    let ((), events) = crate::durable::recording(|| {
        w.drop_in(&w.g, "1.0.0");
        w.write(&w.g, "1");
        w.drop_in(&w.g, "2.0.0");
        w.write(&w.g, "9");
        w.rollback().unwrap();
        crate::profile::refresh_active(&w.dirs, &w.g).unwrap();
    });
    assert_settings(&w, "1.0.0", OLD);
    let state = state_path(&w.dirs);
    let meta = w.dirs.profile(&w.pid(&w.g)).join("meta.json");
    let flushed = |upto: usize, e: &Event| events[..upto].contains(e);
    let journals: Vec<(usize, &Vec<PathBuf>, usize, usize)> = events.iter().enumerate()
        .filter_map(|(i, e)| match e { Event::Journal { path, names, pending, restores } if *path == state => Some((i, names, *pending, *restores)), _ => None })
        .collect();
    assert!(journals.len() >= 4, "the update, the roll back and the restore write the state: {events:#?}");
    let mut named = 0;
    for (i, names, _, _) in &journals {
        for n in names.iter() {
            named += 1;
            assert!(flushed(*i, &Event::Dir(n.clone())), "{} named before its folder was flushed", n.display());
            assert!(flushed(*i, &Event::Dir(n.parent().unwrap().to_path_buf())), "{} named before its name was flushed", n.display());
        }
    }
    assert!(named > 0, "the records name settings folders");
    let mut commits = 0;
    let mut cleared = 0;
    for pair in journals.windows(2) {
        let ((a, _, pending_a, restores_a), (b, _, pending_b, restores_b)) = (pair[0].clone(), pair[1].clone());
        let between = &events[a..b];
        if pending_a > 0 && pending_b == 0 {
            commits += 1;
            assert!(between.contains(&Event::File(meta.clone())) && between.contains(&Event::Dir(meta.parent().unwrap().to_path_buf())),
                "a pending change dropped before its profile was flushed: {between:#?}");
        }
        if restores_b < restores_a {
            cleared += 1;
            for rel in [A, B] {
                let f = w.g.path.join(rel);
                assert!(between.contains(&Event::File(f.clone())) && between.contains(&Event::Dir(f.parent().unwrap().to_path_buf())),
                    "a restore marked done before {rel} was flushed: {between:#?}");
            }
        }
    }
    assert!(commits >= 2, "the update and the roll back commit after their profile save ({commits})");
    assert_eq!(cleared, 1, "the restore is marked done once");
}

/// The game folder moved (a Steam library moved) between the update and the roll back: the roll back says that the
/// settings wait for the folder they were kept in, and the placement in the new folder says it too, and leaves them.
#[test]
fn a_roll_back_after_the_game_folder_moved_says_where_its_settings_wait() {
    let w = World::updated("moved");
    let old = w.g.path.clone();
    let moved = w.root.join("moved-game");
    std::fs::rename(&old, &moved).unwrap();
    let g = GameInstall { path: moved.clone(), ..w.g.clone() };
    crate::game::write_record(&w.dirs, &g, None).unwrap();
    let r = rollback(&w.dirs, Some(&g), &w.pid(&g), ModKind::Greeneye, "worstmap", None).unwrap();
    assert!(r.notices.iter().any(|n| n.contains(&old.display().to_string()) && n.contains("go back only there")), "{:?}", r.notices);
    let a = crate::profile::refresh_active(&w.dirs, &g).unwrap().unwrap();
    assert!(a.kept_files.iter().any(|l| l.contains(&old.display().to_string()) && l.contains("not a game folder Kiln knows")), "{:?}", a.kept_files);
    assert_eq!(std::fs::read_to_string(moved.join(A)).unwrap(), "zoom=9", "the settings it has stay");
    assert_eq!(pending_restores(&w.dirs, ModKind::Greeneye, "worstmap").len(), 1, "they wait for their folder");
}

/// A settings copy's tag comes from the OS's random source only: when it fails, the version is not kept (no name made
/// of anything guessable), and the update goes on.
#[test]
fn a_settings_copy_is_never_named_by_a_guessable_tag() {
    let w = world("tags-rng");
    w.drop_in(&w.g, "1.0.0");
    w.write(&w.g, "1");
    hooks::TAGS.with(|t| *t.borrow_mut() = vec![None]);
    let up = w.drop_in(&w.g, "2.0.0");
    hooks::TAGS.with(|t| t.borrow_mut().clear());
    assert!(up.notices.iter().any(|n| n.contains("was not kept for a roll back") && n.contains("random source")), "{:?}", up.notices);
    assert_eq!(w.version_in(&w.g), "2.0.0", "the update itself goes on");
    assert!(w.kept().is_empty());
    w.tidy_on_disk();
}

/// A tag already taken (by a staging folder, or by a copy) is never reused: the staging folder is made exclusively,
/// and a new tag is drawn.
#[test]
fn a_settings_copy_never_takes_a_taken_name() {
    let w = World::updated("tags-taken");
    let dir = w.dirs.library().join(SETTINGS_DIR).join("greeneye/worstmap");
    std::fs::create_dir_all(dir.join(".stage-aaaaaaaaaaaa")).unwrap();
    let two = crate::library::list(&w.dirs, Some(ModKind::Greeneye)).unwrap()[0].versions.iter().find(|v| v.version == "2.0.0").cloned().unwrap();
    std::fs::create_dir_all(dir.join(format!("2.0.0+{}.bbbbbbbbbbbb", crate::hash::hash8(&two.hash).unwrap()))).unwrap();
    // drawn in this order: aaaa (its staging folder is taken), bbbb (its copy is taken), cccc, then the change's id
    hooks::TAGS.with(|t| *t.borrow_mut() = vec![Some("0123456789ab".into()), Some("cccccccccccc".into()), Some("bbbbbbbbbbbb".into()), Some("aaaaaaaaaaaa".into())]);
    w.rollback().unwrap();
    hooks::TAGS.with(|t| t.borrow_mut().clear());
    let st = load(&w.dirs).unwrap();
    let kept_two = st.mods[0].kept.iter().find(|v| v.version == "2.0.0").expect("2.0.0 kept");
    assert!(kept_two.settings.dir.as_deref().is_some_and(|d| d.ends_with(".cccccccccccc")), "{:?}", kept_two.settings.dir);
    w.next_start();
    assert_settings(&w, "1.0.0", OLD);
    w.tidy_on_disk();
}

#[test]
fn review_publication_never_moves_a_parked_path_outside_the_game() {
    let w = World::updated("journal-outside");
    let receipt: serde_json::Value = crate::util::read_json::<serde_json::Value>(&w.g.path.join("kiln/state/library-placements.json")).unwrap().unwrap();
    let external = w.root.join("player-backup");
    std::fs::rename(w.g.path.join(MAP), &external).unwrap();
    let mut to = receipt[0].clone();
    to["marker"]["version"] = "3.0.0".into();
    let journal = serde_json::json!([{"path": w.g.path.join(MAP), "from": receipt[0]["marker"], "to": to, "parked": external}]);
    crate::util::write_json_atomic(&w.g.path.join("kiln/state/placement-journal.json"), &journal).unwrap();
    let _ = crate::engine_mods::place(&w.dirs, &crate::profile::get(&w.dirs, &w.pid(&w.g)).unwrap(), &w.g.path);
    assert!(external.join("map.dll").is_file(), "recovery moved a player folder outside the game");
}

#[test]
fn review_publication_never_deletes_an_unverified_sibling() {
    let w = World::updated("journal-delete");
    let receipts: serde_json::Value = crate::util::read_json(&w.g.path.join("kiln/state/library-placements.json")).unwrap().unwrap();
    let sibling = w.g.path.join("greeneye/.kiln-place-player");
    put(&sibling, "precious.txt", b"player data");
    let journal = serde_json::json!([{"path": w.g.path.join(MAP), "from": receipts[0]["marker"], "to": receipts[0], "parked": sibling.join("old")}]);
    crate::util::write_json_atomic(&w.g.path.join("kiln/state/placement-journal.json"), &journal).unwrap();
    let p = crate::profile::get(&w.dirs, &w.pid(&w.g)).unwrap();
    assert!(crate::engine_mods::place(&w.dirs, &p, &w.g.path).is_err());
    assert!(sibling.join("precious.txt").is_file(), "a journal is not authority to delete a sibling folder");
}

#[test]
fn review_publication_keeps_the_only_remaining_old_copy() {
    let w = World::updated("journal-only-copy");
    w.rollback().unwrap();
    crash_at(Step::ReceiptSaved, || { let _ = crate::profile::refresh_active(&w.dirs, &w.g); });
    let journal: serde_json::Value = crate::util::read_json(&w.g.path.join("kiln/state/placement-journal.json")).unwrap().unwrap();
    let parked = PathBuf::from(journal[0]["parked"].as_str().unwrap());
    std::fs::remove_dir_all(w.g.path.join(MAP)).unwrap();
    let p = crate::profile::get(&w.dirs, &w.pid(&w.g)).unwrap();
    let _ = crate::engine_mods::place(&w.dirs, &p, &w.g.path);
    assert!(parked.join("map.dll").is_file(), "recovery deleted the only remaining old copy");
}

#[test]
fn review_unreadable_publication_journal_blocks_mutation_and_is_reported() {
    let w = World::updated("journal-corrupt");
    let journal = w.g.path.join("kiln/state/placement-journal.json");
    std::fs::write(&journal, b"[{").unwrap();
    let p = crate::profile::get(&w.dirs, &w.pid(&w.g)).unwrap();
    assert!(crate::engine_mods::place(&w.dirs, &p, &w.g.path).is_err());
    assert_eq!(std::fs::read(&journal).unwrap(), b"[{");
}

#[test]
fn review_equal_restore_bytes_are_flushed_before_the_done_mark() {
    let w = World::updated("restore-equal-durability");
    w.rollback().unwrap();
    let p = crate::profile::get(&w.dirs, &w.pid(&w.g)).unwrap();
    crate::engine_mods::place(&w.dirs, &p, &w.g.path).unwrap();
    w.write(&w.g, "1"); // matching bytes can have been written without a directory flush
    let (_, events) = crate::durable::recording(|| restore_after_place(&w.dirs, &p, &w.g.path));
    for rel in [A, B] {
        assert!(events.contains(&crate::durable::Event::File(w.g.path.join(rel))), "matching {rel} was marked done without flushing: {events:?}");
    }
}

#[test]
fn review_shroudforge_values_are_flushed_before_the_done_mark() {
    let w = world("restore-sf-durability");
    let copy = w.g.path.join("mods/sfpack");
    put(&copy, "mod.json", br#"{"id":"sfpack","name":"SF Pack","version":"1.0.0"}"#);
    put(&copy, "extended.mod.json", br#"{"schemaVersion":1,"launcher":"SF","enabled":true,"targets":["client","server"],"settings":{"speed":{"label":"Speed","control":"slider","min":1,"max":10,"value":3}}}"#);
    let mut r: PendingRestore = serde_json::from_value(serde_json::json!({"version":"1.0.0","hash":format!("kdh1:{}", "0".repeat(64)),"profile":"p","settings":{"size":0,"values":{"mods/sfpack":{"speed":7}}}})).unwrap();
    r.preimages = Some(capture_preimages(&w.dirs, ModKind::ShroudForge, "sfpack", &KeptSettings { game: Some(w.g.path.display().to_string()), ..r.settings.clone() }).unwrap());
    let placed = PlacedCopy { path: copy.clone(), marker: crate::engine_mods::Marker { format:1, kind: ModKind::ShroudForge, id:"sfpack".into(), version:r.version.clone(), hash:r.hash.clone() }, files: BTreeMap::new() };
    let (done, events) = crate::durable::recording(|| apply_restore(&w.dirs, &w.g.path, ModKind::ShroudForge, "sfpack", &r, &[(placed, "mods/sfpack".into())], &mut Vec::new()));
    assert!(done);
    assert_eq!(crate::shroudforge::read_mod(&copy).unwrap().settings[0].value, 7);
    assert!(events.contains(&crate::durable::Event::File(copy.join("extended.mod.json"))), "SF values were marked done without a flush: {events:?}");
}

#[test]
fn review_failed_done_mark_must_not_erase_a_later_player_edit() {
    let w = World::updated("restore-mark-later-edit");
    w.rollback().unwrap();
    fail_nth_save(1);
    crate::profile::refresh_active(&w.dirs, &w.g).unwrap();
    assert_settings(&w, "1.0.0", OLD);
    put(&w.g.path, A, b"zoom=7");
    w.next_start();
    assert_settings(&w, "1.0.0", ("zoom=7", "chests=1"));
}

#[test]
fn review_replay_preserves_an_edit_to_a_file_not_yet_restored() {
    let w = World::updated("preimage-unwritten-edit");
    w.rollback().unwrap();
    crash_at(Step::RestoreFile, || { let _ = crate::profile::refresh_active(&w.dirs, &w.g); });
    // The first file in snapshot order is B; A has not been restored yet.
    assert_eq!(std::fs::read_to_string(w.g.path.join(A)).unwrap(), "zoom=9");
    put(&w.g.path, A, b"zoom=7");
    let p = crate::profile::get(&w.dirs, &w.pid(&w.g)).unwrap();
    let notes = restore_after_place(&w.dirs, &p, &w.g.path);
    assert_settings(&w, "1.0.0", ("zoom=7", "chests=1"));
    assert!(notes.iter().any(|n| n.contains(A) && n.contains("later change")), "{notes:?}");
}

#[test]
fn review_replay_preserves_a_players_deletion_after_restore() {
    let w = World::updated("preimage-deletion");
    w.rollback().unwrap();
    fail_nth_save(1);
    crate::profile::refresh_active(&w.dirs, &w.g).unwrap();
    std::fs::remove_file(w.g.path.join(A)).unwrap();
    w.next_start();
    assert!(!w.g.path.join(A).exists(), "a later deletion must stay deleted");
}

#[test]
fn review_durable_records_follow_import_placement_and_game_records() {
    use crate::durable::Event;
    let w = world("all-record-order");
    let (_, events) = crate::durable::recording(|| { w.drop_in(&w.g, "1.0.0"); });
    let rec = crate::library::list(&w.dirs, Some(ModKind::Greeneye)).unwrap()[0].versions[0].clone();
    let published = |target: &Path| events.iter().enumerate().find_map(|(i, e)| match e {
        Event::Rename { from, to } if to == target => Some((i, from.clone())), _ => None,
    }).expect("publication must use durable rename");
    let (import_at, import_stage) = published(&rec.folder);
    assert!(events[..import_at].contains(&Event::File(import_stage.join("map.dll"))), "Library bytes must be flushed before their durable rename");
    let record = PathBuf::from(format!("{}.json", rec.folder.display()));
    let imported_record_at = events.iter().position(|e| matches!(e, Event::Journal { path, .. } if *path == record)).expect("durable import record");
    assert!(imported_record_at > import_at);
    let (place_at, place_stage) = published(&w.g.path.join(MAP));
    assert!(events[..place_at].contains(&Event::File(place_stage.join("map.dll"))), "placed bytes must be flushed before publication");
    let mut previous = place_at;
    for record in [w.g.path.join("kiln/state/library-placements.json"), w.g.path.join("kiln/profile.json"), w.dirs.root.join(format!("installs/{}.active.json", w.g.id))] {
        let at = events.iter().enumerate().skip(previous + 1).find_map(|(i, e)| matches!(e, Event::Journal { path, .. } if *path == record).then_some(i)).expect("record must use durability seam after the files it names");
        assert!(at > previous, "receipt, profile and activation records must follow publication in order");
        previous = at;
    }

}

#[test]
fn review_replayed_shroudforge_value_preserves_a_later_edit() {
    let w = world("preimage-sf");
    let src = w.root.join("src/sfpack");
    let manifest = |v: &str| format!(r#"{{"id":"sfpack","name":"SF Pack","version":"{v}"}}"#);
    put(&src, "mod.json", manifest("1.0.0").as_bytes());
    put(&src, "extended.mod.json", br#"{"schemaVersion":1,"launcher":"SF","enabled":true,"targets":["client","server"],"settings":{"speed":{"label":"Speed","control":"slider","min":1,"max":10,"value":3}}}"#);
    put(&src, "src/mod.lua", b"return {}");
    let add = || crate::quick::add_with(&w.dirs, &w.g, &[src.clone()], false, &mut NoProgress).unwrap();
    add();
    let copy = crate::shroudforge::mods_dir(&w.g.path).join("sfpack");
    crate::shroudforge::set_setting(&copy, "speed", &serde_json::json!(7)).unwrap();
    put(&src, "mod.json", manifest("2.0.0").as_bytes());
    add();
    crate::shroudforge::set_setting(&copy, "speed", &serde_json::json!(9)).unwrap();
    rollback(&w.dirs, Some(&w.g), &w.pid(&w.g), ModKind::ShroudForge, "sfpack", None).unwrap();
    fail_nth_save(1);
    crate::profile::refresh_active(&w.dirs, &w.g).unwrap();
    assert_eq!(crate::shroudforge::read_mod(&copy).unwrap().settings[0].value, 7);
    crate::shroudforge::set_setting(&copy, "speed", &serde_json::json!(5)).unwrap();
    w.next_start();
    assert_eq!(crate::shroudforge::read_mod(&copy).unwrap().settings[0].value, 5);
}

#[test]
fn review_legacy_restore_preserves_differing_files_without_guessing() {
    let w = World::updated("legacy-preimages");
    w.rollback().unwrap();
    let mut st = load(&w.dirs).unwrap();
    st.mods[0].restores[0].preimages = None;
    save(&w.dirs, &st).unwrap();
    let p = crate::profile::get(&w.dirs, &w.pid(&w.g)).unwrap();
    crate::engine_mods::place(&w.dirs, &p, &w.g.path).unwrap();
    let notes = restore_after_place(&w.dirs, &p, &w.g.path);
    assert_settings(&w, "1.0.0", NEW);
    assert!(notes.iter().any(|n| n.contains("legacy restore") && n.contains(A)), "{notes:?}");
    assert!(pending_restores(&w.dirs, ModKind::Greeneye, "worstmap").is_empty());
}

#[test]
fn review_absent_preimage_permits_first_restore_and_an_unacknowledged_retry() {
    let w = World::updated("absent-preimage");
    std::fs::remove_file(w.g.path.join(A)).unwrap();
    w.rollback().unwrap();
    let pending = pending_restores(&w.dirs, ModKind::Greeneye, "worstmap");
    assert_eq!(pending[0].preimages.as_ref().unwrap().files[A].before, None);
    fail_nth_save(1);
    crate::profile::refresh_active(&w.dirs, &w.g).unwrap();
    assert_eq!(std::fs::read_to_string(w.g.path.join(A)).unwrap(), "zoom=1");
    std::fs::remove_file(w.g.path.join(A)).unwrap();
    w.next_start();
    assert_eq!(std::fs::read_to_string(w.g.path.join(A)).unwrap(), "zoom=1", "absence equals the recorded preimage, so the required rule treats it as not yet restored");
}

#[cfg(unix)]
#[test]
fn review_half_restore_replay_does_not_replace_the_completed_file() {
    use std::os::unix::fs::MetadataExt;
    let w = World::updated("half-restore-inode");
    w.rollback().unwrap();
    crash_at(Step::RestoreFile, || { let _ = crate::profile::refresh_active(&w.dirs, &w.g); });
    let completed = std::fs::metadata(w.g.path.join(B)).unwrap().ino();
    w.next_start();
    assert_settings(&w, "1.0.0", OLD);
    assert_eq!(std::fs::metadata(w.g.path.join(B)).unwrap().ino(), completed);
}

#[test]
fn review_park_and_recovery_put_back_use_the_durable_seam() {
    use crate::durable::Event;
    let w = World::updated("park-order");
    w.rollback().unwrap();
    let (_, park_events) = crate::durable::recording(|| crash_at(Step::Parked, || { let _ = crate::profile::refresh_active(&w.dirs, &w.g); }));
    let parked = park_events.iter().find_map(|e| match e {
        Event::Rename { from, to } if *from == w.g.path.join(MAP) && to.file_name().is_some_and(|n| n == "old") => Some(to.clone()), _ => None,
    }).expect("the park must be durable");
    let (_, recovery_events) = crate::durable::recording(|| w.next_start());
    assert!(recovery_events.contains(&Event::Rename { from: parked, to: w.g.path.join(MAP) }), "put-back must be durable: {recovery_events:?}");
    assert_settings(&w, "1.0.0", OLD);
}
