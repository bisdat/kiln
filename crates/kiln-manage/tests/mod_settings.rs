//! "Remove its settings too" (owner 2026-11-20): kiln_manage::mod_settings takes a removed mod's settings out of every
//! place Kiln keeps them (the game's settings stash and kept files, every config folder), only that mod's, after a
//! copy into the launcher's backups folder; library::remove_and_settings does it with the mod's last version only.

use kiln_manage::game::{self, GameInstall, GameKind, PathMode, Platform};
use kiln_manage::library::{self, LibraryRef};
use kiln_manage::manifest::ModKind;
use kiln_manage::mod_settings;
use kiln_manage::paths::DataDirs;
use kiln_manage::proc::ProcessInfo;
use kiln_manage::task::NoProgress;
use kiln_manage::{ErrorCode, Result};
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-modsettings-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p.canonicalize().unwrap())
}
fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}
fn read(root: &Path, rel: &str) -> String {
    std::fs::read_to_string(root.join(rel)).unwrap_or_else(|e| panic!("{rel}: {e}"))
}

fn import(dirs: &DataDirs, root: &Path, id: &str, version: &str, content: &str) -> LibraryRef {
    let d = root.join(format!("src/{id}-{version}-{content}"));
    put(&d, "kiln.mod.json", format!(r#"{{ "id": "{id}", "version": "{version}", "apiVersion": "0.2", "entryAssembly": "M.dll", "side": "both" }}"#).as_bytes());
    put(&d, "M.dll", content.as_bytes());
    let r = library::import(dirs, std::slice::from_ref(&d), &mut NoProgress).unwrap();
    r.imported.iter().chain(&r.existing).find(|x| x.id == id && x.version == version).cloned().unwrap()
}

fn client(dirs: &DataDirs, path: &Path) {
    std::fs::create_dir_all(path.join("kiln")).unwrap();
    let g = GameInstall {
        id: "g-client".into(), kind: GameKind::Client, path: path.to_path_buf(), exe: game::CLIENT_EXE.into(), app_id: kiln_manage::CLIENT_APP_ID,
        library: None, platform: Platform::Proton, prefix: None, build_id: None, exe_sha256: None, steam_flavor: None, path_mode: PathMode::Z,
        source: "manual".into(),
    };
    game::write_record(dirs, &g, None).unwrap();
}

fn nothing(_: &Path, _: &str) -> Result<Option<ProcessInfo>> {
    Ok(None)
}

const SNAP_NIGHT: &str = r#"{"format":1,"source":"kiln","file":"night-sky.json","mod":"night-sky","id":"night-sky"}"#;

/// Night Sky's settings everywhere Kiln keeps them, and other mods' files right next to them.
fn seed(game: &Path, data: &Path) {
    put(game, "kiln/state/placed-settings/kiln/night-sky/config.json", b"{\"stars\": 9}");
    put(game, "kiln/state/placed-settings/kiln/night-sky-plus/config.json", b"other");
    put(game, "kiln/kept-mod-files/kiln/night-sky/sky.ini", b"[sky]\nstars=9");
    put(game, "kiln/kept-mod-files/kiln/night-sky/logs/sky.log", b"a log");
    put(game, "kiln/kept-mod-files/kiln/night-sky/cache.bin", b"data");
    put(game, "kiln/config/night-sky.json", b"{ \"stars\": 9 }");
    put(game, "kiln/config/night-sky-client.json", b"{ \"glow\": true }");
    put(game, "kiln/config/.schema/night-sky.json", SNAP_NIGHT.as_bytes());
    put(game, "kiln/config/night-sky-moons.json", b"{ \"moons\": 2 }");
    put(game, "kiln/config/.schema/night-sky-moons.json", br#"{"format":1,"source":"kiln","file":"night-sky-moons.json","mod":"night-sky"}"#);
    put(game, "kiln/config/night-sky-plus.json", b"{ \"other\": 1 }");
    put(game, "kiln/config/.schema/night-sky-plus.json", br#"{"format":1,"source":"kiln","file":"night-sky-plus.json","mod":"night-sky-plus"}"#);
    put(game, "kiln/config/native.json", br#"{"format":1,"mods":{}}"#);
    put(game, "kiln/profiles/work/config/night-sky.json", b"{ \"stars\": 4 }");
    put(data, "profiles/work/config/night-sky.json", b"{ \"stars\": 4 }");
    put(data, "profiles/work/config/keybinds.json", b"{}");
    put(data, "profiles/play/config/night-sky-server.json", b"{ \"pvp\": false }");
}

#[test]
fn settings_stay_by_default_and_go_with_the_last_version_when_asked() {
    let t = tmp("last");
    let dirs = DataDirs::at(t.0.join("data"));
    let game = t.0.join("Enshrouded");
    client(&dirs, &game);
    let v1 = import(&dirs, &t.0, "night-sky", "1.0.0", "a");
    let v2 = import(&dirs, &t.0, "night-sky", "1.1.0", "b");
    seed(&game, &dirs.root);

    // the default: the version goes, every settings file stays
    assert_eq!(library::remove_and_settings_with(&dirs, &v1, false, false, &mut nothing).unwrap(), None);
    assert!(game.join("kiln/state/placed-settings/kiln/night-sky/config.json").is_file());
    // asked, but another version stays in the Library: so do the settings
    let v1 = import(&dirs, &t.0, "night-sky", "1.0.0", "a");
    assert_eq!(library::remove_and_settings_with(&dirs, &v1, false, true, &mut nothing).unwrap(), None);
    assert!(game.join("kiln/config/night-sky.json").is_file());

    // the last version: its settings go, everywhere
    let r = library::remove_and_settings_with(&dirs, &v2, false, true, &mut nothing).unwrap().expect("a settings removal");
    assert!(r.notes.is_empty(), "{:?}", r.notes);
    for gone in ["kiln/state/placed-settings/kiln/night-sky", "kiln/kept-mod-files/kiln/night-sky/sky.ini", "kiln/config/night-sky.json",
                 "kiln/config/night-sky-client.json", "kiln/config/.schema/night-sky.json", "kiln/config/night-sky-moons.json",
                 "kiln/config/.schema/night-sky-moons.json", "kiln/profiles/work/config/night-sky.json"] {
        assert!(!game.join(gone).exists(), "{gone} is still there");
    }
    for gone in ["profiles/work/config/night-sky.json", "profiles/play/config/night-sky-server.json"] {
        assert!(!dirs.root.join(gone).exists(), "{gone} is still there");
    }
    // only its own: other mods' files, Kiln's own files and what the mod wrote that is not a setting stay
    for stays in ["kiln/state/placed-settings/kiln/night-sky-plus/config.json", "kiln/config/night-sky-plus.json",
                  "kiln/config/.schema/night-sky-plus.json", "kiln/config/native.json", "kiln/kept-mod-files/kiln/night-sky/cache.bin",
                  "kiln/kept-mod-files/kiln/night-sky/logs/sky.log"] {
        assert!(game.join(stays).is_file(), "{stays} was removed");
    }
    assert!(dirs.root.join("profiles/work/config/keybinds.json").is_file());

    // the backup holds each file at a path that says where it was, and a record of it
    let backup = PathBuf::from(r.backup.expect("a backup"));
    assert!(backup.starts_with(dirs.backups().join(mod_settings::BACKUP_DIR)), "{}", backup.display());
    assert_eq!(read(&backup, "games/g-client/kiln/state/placed-settings/kiln/night-sky/config.json"), "{\"stars\": 9}");
    assert_eq!(read(&backup, "games/g-client/kiln/kept-mod-files/kiln/night-sky/sky.ini"), "[sky]\nstars=9");
    assert_eq!(read(&backup, "games/g-client/kiln/config/night-sky.json"), "{ \"stars\": 9 }");
    assert_eq!(read(&backup, "games/g-client/kiln/profiles/work/config/night-sky.json"), "{ \"stars\": 4 }");
    assert_eq!(read(&backup, "profiles/play/config/night-sky-server.json"), "{ \"pvp\": false }");
    let record: serde_json::Value = serde_json::from_str(&read(&backup, mod_settings::BACKUP_RECORD)).unwrap();
    assert_eq!(record["kind"], "kiln");
    assert_eq!(record["id"], "night-sky");
    assert_eq!(record["items"].as_array().unwrap().len(), r.removed.len());
    // "from" is where the file was, as this OS writes it (so it can be put back by hand): compared as a path
    assert!(record["items"].as_array().unwrap().iter().any(|i| Path::new(i["from"].as_str().unwrap()).ends_with("profiles/play/config/night-sky-server.json")));
}

#[test]
fn refused_while_the_library_holds_it_or_a_game_runs_and_nothing_to_remove_is_no_backup() {
    let t = tmp("refused");
    let dirs = DataDirs::at(t.0.join("data"));
    let game = t.0.join("Enshrouded");
    client(&dirs, &game);
    let v = import(&dirs, &t.0, "night-sky", "1.0.0", "a");
    seed(&game, &dirs.root);
    let e = mod_settings::remove_with(&dirs, ModKind::Kiln, "night-sky", &[], &mut nothing).unwrap_err();
    assert_eq!(e.code, ErrorCode::Conflict);
    library::remove_with(&dirs, &v, false, &mut nothing).unwrap();
    let mut running = |d: &Path, exe: &str| -> Result<Option<ProcessInfo>> {
        Ok((d == game && exe == game::CLIENT_EXE).then(|| ProcessInfo { pid: 7, name: "enshrouded.exe".into(), exe: None, cmdline: vec![] }))
    };
    let e = mod_settings::remove_with(&dirs, ModKind::Kiln, "night-sky", &[], &mut running).unwrap_err();
    assert_eq!(e.code, ErrorCode::GameRunning);
    assert!(game.join("kiln/config/night-sky.json").is_file(), "nothing removed while it runs");
    for bad in ["../x", "a/b", ".hidden", ""] {
        assert!(mod_settings::remove_with(&dirs, ModKind::Kiln, bad, &[], &mut nothing).is_err(), "{bad}");
    }
    // a mod with no settings anywhere: nothing removed, no backup folder
    let r = mod_settings::remove_with(&dirs, ModKind::Kiln, "no-settings", &[], &mut nothing).unwrap();
    assert!(r.removed.is_empty() && r.backup.is_none(), "{r:?}");
    assert!(!dirs.backups().join(mod_settings::BACKUP_DIR).exists());
}

#[test]
fn engine_files_go_by_kind_and_a_shared_file_loses_only_the_mods_entry() {
    let t = tmp("engines");
    let dirs = DataDirs::at(t.0.join("data"));
    let game = t.0.join("Enshrouded");
    client(&dirs, &game);
    let cfg = dirs.root.join("profiles/work/config");
    // EMF: its ini and schema; another EMF mod's stay
    put(&cfg, "emf/beastmaster.ini", b"[beastmaster]\nenabled=1\nlevel=3\n");
    put(&cfg, ".schema/emf/beastmaster.json", b"{}");
    put(&cfg, "emf/big-stacks.ini", b"[big-stacks]\n");
    put(&game, "kiln/state/placed-settings/emf/beastmaster/x.ini", b"x");
    let r = mod_settings::remove_with(&dirs, ModKind::Emf, "beastmaster", &[], &mut nothing).unwrap();
    assert!(!cfg.join("emf/beastmaster.ini").exists() && !cfg.join(".schema/emf/beastmaster.json").exists(), "{r:?}");
    assert!(cfg.join("emf/big-stacks.ini").is_file());
    assert!(!game.join("kiln/state/placed-settings/emf/beastmaster").exists());

    // Shroudtopia: one entry of the shared file, by the profile id or the mod.json name; comments and others stay
    let shared = "shroudtopia/shroudtopia.json";
    put(&cfg, shared, b"{\n  // every mod's settings\n  \"mods\": {\n    \"fly\": { \"speed\": 2 },\n    \"Flying Mod\": { \"speed\": 3 },\n    \"zoom\": { \"far\": true }\n  }\n}\n");
    let r = mod_settings::remove_with(&dirs, ModKind::Shroudtopia, "fly", &["Flying Mod".to_string()], &mut nothing).unwrap();
    let doc: serde_json::Value = kiln_manage::jsonc::parse(&read(&cfg, shared)).unwrap();
    assert_eq!(doc["mods"], serde_json::json!({ "zoom": { "far": true } }), "{r:?}");
    assert!(read(&cfg, shared).contains("// every mod's settings"));
    assert_eq!(r.removed.len(), 2, "{r:?}");
    let backup = PathBuf::from(r.backup.unwrap());
    assert!(read(&backup, "profiles/work/config/shroudtopia/shroudtopia.json").contains("Flying Mod"));
}

#[test]
fn a_bare_id_file_stays_while_a_mod_of_another_kind_has_the_same_id() {
    let t = tmp("same-id");
    let dirs = DataDirs::at(t.0.join("data"));
    let game = t.0.join("Enshrouded");
    client(&dirs, &game);
    import(&dirs, &t.0, "sky", "1.0.0", "a"); // a Kiln mod "sky" stays in the Library
    put(&game, "kiln/config/sky.json", b"{}");
    put(&game, "kiln/state/placed-settings/native/sky/sky.ini", b"x");
    let r = mod_settings::remove_with(&dirs, ModKind::Native, "sky", &[], &mut nothing).unwrap();
    assert!(game.join("kiln/config/sky.json").is_file(), "the Kiln mod's config stays");
    assert!(r.notes.iter().any(|n| n.contains("a kiln mod has the same id")), "{:?}", r.notes);
    assert!(!game.join("kiln/state/placed-settings/native/sky").exists(), "its own stash goes");
}
