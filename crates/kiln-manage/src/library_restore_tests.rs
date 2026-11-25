//! [`super::restore_changed`]'s refusals: an import of an entry's original files over its changed folder moves nothing
//! while a game may be reading the entry (a native mod's DLL is loaded from it in the Z and native path modes), and the
//! import's other entries still go. The rest is tested end to end in `tests/library_restore.rs`.

use super::*;
use crate::game::{GameInstall, GameKind, PathMode, Platform};

fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}

fn tree(dir: &Path) -> Vec<(String, Vec<u8>)> {
    crate::hash::dir_records(dir).unwrap().into_iter().map(|r| { let b = std::fs::read(dir.join(&r.path)).unwrap(); (r.path, b) }).collect()
}

#[test]
fn an_entry_a_running_game_may_read_is_not_put_back() {
    let root = std::env::temp_dir().join(format!("km-library-restore-unit-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    let dirs = DataDirs::at(root.join("data"));
    let game = root.join("game");
    put(&game, "enshrouded.exe", b"MZ");
    let g = GameInstall {
        id: "g-unit".into(), kind: GameKind::Client, path: game.clone(), exe: "enshrouded.exe".into(), app_id: crate::CLIENT_APP_ID,
        library: None, platform: Platform::Windows, prefix: None, build_id: None, exe_sha256: None, steam_flavor: None,
        path_mode: PathMode::Native, source: "manual".into(),
    };
    crate::game::write_record(&dirs, &g, None).unwrap();
    let src = root.join("src/LootRarityLeveler");
    put(&src, "dinput8.dll", &crate::pe::build_test_dll(&["DirectInput8Create"], b"loot"));
    put(&src, "native.json", br#"{ "format": 1, "id": "lootrarityleveler", "name": "Loot Rarity Leveler", "version": "0.0.0", "dlls": ["dinput8.dll"], "phase": "early", "side": "both" }"#);
    put(&src, "LootRarityLeveler.log", b"started\n");
    let r = import(&dirs, std::slice::from_ref(&src), &mut crate::task::NoProgress).unwrap().imported[0].clone();
    let folder = path_of(&dirs, &r).unwrap();
    let rec = record(&dirs, &r).unwrap();
    put(&folder, "LootRarityLeveler.log", b"started\nleveled 12 items\n");
    let changed = tree(&folder);

    let prepare = || {
        let tmp = TempDir::new(&dirs.library(), "import").unwrap();
        copy_mod_tree(&src, &tmp.path().join("m")).unwrap();
        tmp
    };
    let mut running = |d: &Path, e: &str| Ok((d == game && e == "enshrouded.exe").then(|| crate::proc::ProcessInfo { pid: 4242, name: e.into(), exe: None, cmdline: vec![] }));
    let tmp = prepare();
    let mut report = ImportReport::default();
    let err = commit_folder_with(&dirs, &tmp.path().join("m"), rec.clone(), &mut report, &mut running).unwrap_err();
    assert_eq!(err.code, ErrorCode::GameRunning);
    assert!(err.message.starts_with("Loot Rarity Leveler was changed in its Library folder ("), "{}", err.message);
    assert!(err.message.contains("Close Enshrouded first"), "{}", err.message);
    assert_eq!(tree(&folder), changed, "nothing moved");
    assert!(!dirs.library().join(CHANGED_DIR).exists());
    assert!(report.existing.is_empty() && report.notes.is_empty());

    // the game closed: put back
    let tmp = prepare();
    commit_folder_with(&dirs, &tmp.path().join("m"), rec, &mut report, &mut |_, _| Ok(None)).unwrap();
    assert_eq!(report.existing, [r.clone()]);
    assert_eq!(crate::hash::dirhash(&folder).unwrap(), r.hash);
    assert!(!tmp.path().join("m").exists(), "the prepared copy is the entry now");
    let _ = std::fs::remove_dir_all(&root);
}

/// The copies' names: `<entry>.<time>[-n]`, a new one always after the highest n of its time, and only the newest
/// three of one entry stay (another entry's copies are never counted or deleted).
#[test]
fn changed_copies_are_pruned_to_the_newest_three_of_their_entry() {
    let dir = std::env::temp_dir().join(format!("km-library-restore-prune-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    for n in ["1.0.0+aaaaaaaa.20261124T100000Z", "1.0.0+aaaaaaaa.20261124T100000Z-2", "1.0.0+aaaaaaaa.20261125T090000Z",
              "1.0.0+aaaaaaaa.20261125T100000Z", "1.0.0+bbbbbbbb.20261101T000000Z", "1.0.0+aaaaaaaa.notes"] {
        std::fs::create_dir_all(dir.join(n)).unwrap();
    }
    prune_changed_copies(&dir, "1.0.0+aaaaaaaa", &dir.join("1.0.0+aaaaaaaa.20261125T100000Z"));
    let mut left: Vec<String> = std::fs::read_dir(&dir).unwrap().flatten().map(|e| e.file_name().into_string().unwrap()).collect();
    left.sort();
    assert_eq!(left, ["1.0.0+aaaaaaaa.20261124T100000Z-2", "1.0.0+aaaaaaaa.20261125T090000Z", "1.0.0+aaaaaaaa.20261125T100000Z",
        "1.0.0+aaaaaaaa.notes", "1.0.0+bbbbbbbb.20261101T000000Z"]);
    let _ = std::fs::remove_dir_all(&dir);
}
