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
    fn outside_kept(&self) -> BTreeMap<String, Vec<u8>> {
        let mut out = BTreeMap::new();
        for root in [self.g.path.clone(), self.dirs.library().join("greeneye")] {
            let mut stack = vec![root.clone()];
            while let Some(d) = stack.pop() {
                for e in std::fs::read_dir(&d).into_iter().flatten().flatten() {
                    let p = e.path();
                    if p.is_dir() { stack.push(p); } else { out.insert(p.display().to_string(), std::fs::read(&p).unwrap_or_default()); }
                }
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
fn a_restore_whose_record_cannot_be_cleared_is_done_again_without_harm() {
    let w = World::updated("restore-fail");
    w.rollback().unwrap();
    fail_nth_save(1);
    let a = crate::profile::refresh_active(&w.dirs, &w.g).unwrap();
    assert!(a.is_some_and(|a| a.kept_files.iter().any(|l| l.contains("tries again at the next start"))));
    assert_settings(&w, "1.0.0", OLD);
    // the player changes a setting before the next start: the restore, done again, puts back the same old bytes
    w.next_start();
    assert_settings(&w, "1.0.0", OLD);
    assert!(pending_restores(&w.dirs, ModKind::Greeneye, "worstmap").is_empty());
    w.tidy_on_disk();
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
/// cannot cut the power on Linux: this is what makes a power cut leave either the old or the new state.
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
