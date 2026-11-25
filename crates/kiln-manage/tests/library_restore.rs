//! An import that brings exactly the files a Library entry's name says (kind, id, version, kdh1) over that entry, whose
//! folder changed since its import (a player's Crucible pack, 0.2.0-dev.31.8: Loot Rarity Leveler, a dinput8.dll proxy
//! mod, writes its log and settings beside its DLL, and in the Z and native path modes the game runs it from its Library
//! folder). Before, the import skipped it ("existing library content does not match its hash") and the pack install
//! failed ("the library did not take native lootrarityleveler"). Now the import puts the original files back, keeps
//! the changed copy in `<library>/kiln-changed/`, and says where; every profile that lists the entry gets the original.
#![cfg(unix)]

use kiln_manage::game::{GameInstall, GameKind, PathMode, Platform};
use kiln_manage::library::{self, LibraryRef};
use kiln_manage::manifest::ModKind;
use kiln_manage::paths::DataDirs;
use kiln_manage::pe::build_test_dll;
use kiln_manage::profile::{self, ProfileMod};
use kiln_manage::task::NoProgress;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-library-restore-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p.canonicalize().unwrap())
}
fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}
fn append(file: &Path, line: &str) {
    use std::io::Write;
    writeln!(std::fs::File::options().append(true).open(file).unwrap(), "{line}").unwrap();
}

/// Every file below `dir` with its bytes (what a folder holds, to compare before and after).
fn tree(dir: &Path) -> BTreeMap<String, Vec<u8>> {
    fn walk(dir: &Path, prefix: &str, out: &mut BTreeMap<String, Vec<u8>>) {
        for e in std::fs::read_dir(dir).unwrap().flatten() {
            let name = format!("{prefix}{}", e.file_name().to_string_lossy());
            let meta = std::fs::symlink_metadata(e.path()).unwrap();
            if meta.is_dir() {
                walk(&e.path(), &format!("{name}/"), out);
            } else {
                out.insert(name, std::fs::read(e.path()).unwrap_or_default());
            }
        }
    }
    let mut out = BTreeMap::new();
    if dir.is_dir() {
        walk(dir, "", &mut out);
    }
    out
}

fn game_with_z(t: &Path) -> GameInstall {
    let game = t.join("game");
    put(&game, "enshrouded.exe", b"MZ");
    std::fs::create_dir_all(game.join("kiln")).unwrap();
    let pfx = t.join("pfx");
    std::fs::create_dir_all(pfx.join("dosdevices")).unwrap();
    std::os::unix::fs::symlink("/", pfx.join("dosdevices/z:")).unwrap();
    GameInstall {
        id: "g-restore".into(), kind: GameKind::Client, path: game, exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID, library: None, platform: Platform::Proton, prefix: Some(pfx), build_id: None,
        exe_sha256: None, steam_flavor: None, path_mode: PathMode::Z, source: "manual".into(),
    }
}

/// The player's mod as the pack holds it: a dinput8.dll proxy with Kiln's native.json, its settings and its log.
fn loot_rarity_leveler(root: &Path) -> PathBuf {
    let d = root.join("src/LootRarityLeveler");
    put(&d, "dinput8.dll", &build_test_dll(&["DirectInput8Create"], b"loot rarity leveler"));
    put(&d, "native.json", br#"{ "format": 1, "id": "lootrarityleveler", "name": "Loot Rarity Leveler", "version": "0.0.0", "dlls": ["dinput8.dll"], "phase": "early", "side": "both" }"#);
    put(&d, "LootRarityLeveler.ini", b"[General]\nLevel=1\n");
    put(&d, "LootRarityLeveler.log", b"[LootRarityLeveler] started\n");
    d
}

fn import(dirs: &DataDirs, paths: &[PathBuf]) -> library::ImportReport {
    library::import(dirs, paths, &mut NoProgress).unwrap()
}

fn profile_with(dirs: &DataDirs, name: &str, r: &LibraryRef) -> String {
    let mut p = profile::create(dirs, name, None).unwrap();
    p.mods.list_mut(r.kind).push(ProfileMod { id: r.id.clone(), version: r.version.clone(), hash: r.hash.clone(), enabled: true });
    profile::save(dirs, &p).unwrap().id
}

/// `<library>/kiln-changed/<kind>/<id>/`: the changed copies kept of one mod's entries, by folder name.
fn kept_copies(dirs: &DataDirs, r: &LibraryRef) -> Vec<PathBuf> {
    let dir = dirs.library().join("kiln-changed").join(r.kind.as_str()).join(&r.id);
    let mut v: Vec<PathBuf> = std::fs::read_dir(dir).into_iter().flatten().flatten().map(|e| e.path()).collect();
    v.sort();
    v
}

/// The player's case: imported from the pack, the mod wrote into its Library folder while the game ran it from there,
/// the same pack's mod imported again. The entry gets its original files back, the profiles that list it are as they
/// were (and now name the original files again), the game folder is not touched, and the changed copy is kept and named.
#[test]
fn an_import_of_the_original_files_puts_a_changed_entry_back_and_keeps_the_changed_copy() {
    let t = tmp("player");
    let dirs = DataDirs::at(t.0.join("data"));
    let src = loot_rarity_leveler(&t.0);
    let r = import(&dirs, std::slice::from_ref(&src)).imported[0].clone();
    assert_eq!((r.kind, r.id.as_str(), r.version.as_str()), (ModKind::Native, "lootrarityleveler", "0.0.0"));
    let entry = library::path_of(&dirs, &r).unwrap();
    let original = tree(&entry);
    let record_before = std::fs::read(format!("{}.json", entry.display())).unwrap();
    let pids = [profile_with(&dirs, "Pack", &r), profile_with(&dirs, "My own", &r)];
    let g = game_with_z(&t.0);
    profile::activate(&dirs, &pids[0], &g).unwrap();
    let game_before = tree(&g.path);

    // the game ran it from the Library: its log grew, its settings were saved
    append(&entry.join("LootRarityLeveler.log"), "[LootRarityLeveler] leveled 12 items");
    put(&entry, "LootRarityLeveler.ini", b"[General]\nLevel=3\n");
    let changed = tree(&entry);
    assert_ne!(kiln_manage::hash::dirhash(&entry).unwrap(), r.hash);

    let again = import(&dirs, std::slice::from_ref(&src));
    assert!(again.skipped.is_empty(), "{:?}", again.skipped);
    assert_eq!((again.imported.len(), again.existing.as_slice()), (0, std::slice::from_ref(&r)), "the entry its name says");
    assert_eq!(tree(&entry), original, "the original files are back");
    assert_eq!(kiln_manage::hash::dirhash(&entry).unwrap(), r.hash);
    assert_eq!(std::fs::read(format!("{}.json", entry.display())).unwrap(), record_before, "the record is the import's, as it was");
    // the changed copy, kept and named in the import's notes
    let kept = kept_copies(&dirs, &r);
    assert_eq!(kept.len(), 1, "{kept:?}");
    assert!(kept[0].file_name().unwrap().to_string_lossy().starts_with(&format!("{}.", entry.file_name().unwrap().to_string_lossy())), "{kept:?}");
    assert_eq!(tree(&kept[0]), changed, "the changed files as they were");
    let note = again.notes.iter().find(|n| n.text.contains(&kept[0].display().to_string())).unwrap_or_else(|| panic!("{:?}", again.notes));
    assert!(note.text.starts_with("Loot Rarity Leveler was changed in its Library folder"), "{}", note.text);
    // and logged in the Library (a pack install keeps no import notes)
    let log = std::fs::read_to_string(dirs.library().join("kiln-changed/changes.log")).unwrap();
    assert!(log.lines().count() == 1 && log.contains("native/lootrarityleveler 0.0.0: ") && log.contains(&kept[0].display().to_string()), "{log}");
    // the profiles list the entry they listed, now its original files; the game folder waits for the next activation
    for pid in &pids {
        let p = profile::get(&dirs, pid).unwrap();
        assert_eq!(p.mods.native.iter().map(|m| (m.version.as_str(), m.hash.as_str())).collect::<Vec<_>>(), [("0.0.0", r.hash.as_str())]);
    }
    assert_eq!(tree(&g.path), game_before, "nothing in the game folder changed");
    // the Library lists one version, not changed; the kept copy is no entry, so Clean up never offers it
    let list = library::list(&dirs, Some(ModKind::Native)).unwrap();
    assert_eq!(list.len(), 1);
    assert_eq!(list[0].versions.len(), 1, "{:?}", list[0].versions);
    assert!(!list[0].versions[0].changed);
    assert!(library::unused(&dirs).unwrap().is_empty());
    // and the profile activates with the original files
    profile::activate(&dirs, &pids[0], &g).unwrap();

    // the same import once more: nothing changed, nothing set aside
    let third = import(&dirs, &[src]);
    assert_eq!((third.existing.len(), third.skipped.len()), (1, 0));
    assert_eq!(kept_copies(&dirs, &r).len(), 1);
}

/// A mod that changes its folder at every run: each import over it keeps one more copy, the newest three in all (as
/// the game folder keeps a profile's changed copies). Clean up removes no copy and the Library lists none.
#[test]
fn the_newest_three_changed_copies_of_an_entry_are_kept() {
    let t = tmp("three");
    let dirs = DataDirs::at(t.0.join("data"));
    let src = loot_rarity_leveler(&t.0);
    let r = import(&dirs, std::slice::from_ref(&src)).imported[0].clone();
    let entry = library::path_of(&dirs, &r).unwrap();
    profile_with(&dirs, "Pack", &r);
    for run in 1..=5 {
        append(&entry.join("LootRarityLeveler.log"), &format!("run {run}"));
        let again = import(&dirs, std::slice::from_ref(&src));
        assert!(again.skipped.is_empty(), "run {run}: {:?}", again.skipped);
    }
    let kept = kept_copies(&dirs, &r);
    assert_eq!(kept.len(), 3, "{kept:?}");
    let logs: Vec<String> = kept.iter().map(|k| std::fs::read_to_string(k.join("LootRarityLeveler.log")).unwrap()).collect();
    for run in 3..=5 {
        assert!(logs.iter().any(|l| l.ends_with(&format!("run {run}\n"))), "run {run} is kept: {logs:?}");
    }
    let report = library::remove_many(&dirs, &library::unused(&dirs).unwrap(), false).unwrap();
    assert!(report.removed.is_empty() && report.failed.is_empty(), "{report:?}");
    assert_eq!(kept_copies(&dirs, &r).len(), 3, "Clean up leaves them");
    assert!(library::list(&dirs, None).unwrap().iter().all(|e| e.versions.len() == 1));
}

/// Other files than the name says are another entry: the changed one stays as it is, nothing is set aside. A folder
/// that is gone (its record left) is imported again, as before.
#[test]
fn other_files_or_a_missing_folder_leave_nothing_aside() {
    let t = tmp("strict");
    let dirs = DataDirs::at(t.0.join("data"));
    let src = loot_rarity_leveler(&t.0);
    let r = import(&dirs, std::slice::from_ref(&src)).imported[0].clone();
    let entry = library::path_of(&dirs, &r).unwrap();
    append(&entry.join("LootRarityLeveler.log"), "changed in the Library");
    let changed = tree(&entry);
    // the source changed too, in another way: a new version of its files, a new entry
    append(&src.join("LootRarityLeveler.ini"), "Other=1");
    let other = import(&dirs, std::slice::from_ref(&src));
    assert_eq!(other.imported.len(), 1, "{other:?}");
    assert_ne!(other.imported[0].hash, r.hash);
    assert_eq!(tree(&entry), changed, "the changed entry is left as it is");
    assert!(kept_copies(&dirs, &r).is_empty());

    // a folder that is gone: imported again from the files its record names
    let fresh = t.0.join("fresh");
    let r2 = import(&dirs, &[loot_rarity_leveler(&fresh)]);
    assert_eq!(r2.existing, [r.clone()], "the changed entry put back: {r2:?}");
    std::fs::remove_dir_all(&entry).unwrap();
    let r3 = import(&dirs, &[loot_rarity_leveler(&t.0.join("again"))]);
    assert_eq!(r3.imported, [r.clone()], "{r3:?}");
    assert_eq!(kept_copies(&dirs, &r).len(), 1, "only the copy the put back kept");
}

/// What a mod only wrote beside its own files (a new log, Item Skill Descriptions' item-skill-text.log) is no change
/// of the entry anywhere else (the list, activation, sharing): an import over it takes the entry as it is and keeps
/// the file.
#[test]
fn a_new_output_file_beside_the_entry_is_no_change() {
    let t = tmp("output");
    let dirs = DataDirs::at(t.0.join("data"));
    let src = loot_rarity_leveler(&t.0);
    let r = import(&dirs, std::slice::from_ref(&src)).imported[0].clone();
    let entry = library::path_of(&dirs, &r).unwrap();
    put(&entry, "LootRarityLeveler-crash.log", b"a log of its own\n");
    let again = import(&dirs, &[src]);
    assert!(again.skipped.is_empty(), "{:?}", again.skipped);
    assert_eq!(again.existing, [r.clone()]);
    assert!(entry.join("LootRarityLeveler-crash.log").is_file(), "the mod's output stays");
    assert!(kept_copies(&dirs, &r).is_empty());
}

/// A take-over makes a new entry (a new hash) of the changed files and moves the profiles to it: an import of the
/// original files later is the old entry again, and never touches the taken-over one or the profiles.
#[test]
fn a_taken_over_entry_is_not_touched_by_an_import_of_the_original() {
    let t = tmp("take-over");
    let dirs = DataDirs::at(t.0.join("data"));
    let src = loot_rarity_leveler(&t.0);
    let r = import(&dirs, std::slice::from_ref(&src)).imported[0].clone();
    let pid = profile_with(&dirs, "Mine", &r);
    put(&library::path_of(&dirs, &r).unwrap(), "LootRarityLeveler.ini", b"[General]\nLevel=9\n");
    let taken = library::take_over_with(&dirs, &r, &mut |_, _| Ok(None)).unwrap().to;
    assert_ne!(taken.hash, r.hash);
    let taken_dir = library::path_of(&dirs, &taken).unwrap();
    let taken_files = tree(&taken_dir);

    let again = import(&dirs, &[src]);
    assert!(again.skipped.is_empty(), "{:?}", again.skipped);
    assert!(again.imported.iter().chain(&again.existing).any(|x| *x == r), "{again:?}");
    assert_eq!(tree(&taken_dir), taken_files, "the taken-over entry is as it was");
    assert_eq!(profile::get(&dirs, &pid).unwrap().mods.native[0].hash, taken.hash, "the profile keeps the player's files");
    assert!(kept_copies(&dirs, &r).is_empty(), "nothing was changed to keep");
}

/// Every kind the Library holds is put back the same way: a Kiln mod (by its identity), an EML mod, a plugin DLL.
#[test]
fn every_kind_is_put_back() {
    let t = tmp("kinds");
    let dirs = DataDirs::at(t.0.join("data"));
    let s = t.0.join("src");
    put(&s.join("blades"), "kiln.mod.json", br#"{ "id": "blades", "name": "Blades", "version": "1.0.0", "apiVersion": "0.2", "entryAssembly": "M.dll", "side": "both" }"#);
    put(&s.join("blades"), "M.dll", b"blades");
    put(&s.join("stacks"), "mod.json", br#"{"id":"stacks","name":"Stacks","version":"1.2.0","capabilities":["patch"]}"#);
    put(&s.join("stacks"), "src/mod.lua", b"return {}");
    put(&s, "hello.dll", &build_test_dll(&["kiln_plugin_main"], b"v1"));
    let paths = vec![s.join("blades"), s.join("stacks"), s.join("hello.dll")];
    let r = import(&dirs, &paths);
    assert!(r.skipped.is_empty(), "{:?}", r.skipped);
    let cases = [(ModKind::Kiln, "M.dll"), (ModKind::Eml, "src/mod.lua"), (ModKind::Plugin, "hello.dll")];
    let mut refs = Vec::new();
    for (kind, file) in cases {
        let lr = r.imported.iter().find(|x| x.kind == kind).cloned().unwrap_or_else(|| panic!("{kind:?}: {r:?}"));
        let folder = library::path_of(&dirs, &lr).unwrap();
        let folder = if kind == ModKind::Plugin { folder.parent().unwrap().to_path_buf() } else { folder };
        put(&folder, file, b"edited in the Library");
        refs.push((lr, folder, file));
    }
    let again = import(&dirs, &paths);
    assert!(again.skipped.is_empty(), "{:?}", again.skipped);
    for (lr, folder, file) in refs {
        assert!(again.existing.contains(&lr), "{lr:?}: {again:?}");
        assert_ne!(std::fs::read(folder.join(file)).unwrap(), b"edited in the Library", "{lr:?}: put back");
        let kept = kept_copies(&dirs, &lr);
        assert_eq!(kept.len(), 1, "{lr:?}: {kept:?}");
        assert_eq!(std::fs::read(kept[0].join(file)).unwrap(), b"edited in the Library", "{lr:?}");
    }
}
