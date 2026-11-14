//! The boot snapshot's Play decision reads the profile of the start it is made for. At Play the launcher's `cook_run`
//! calls `cook::cook_due` (which writes the game's `kiln/profile.json` again when the active profile changed since its
//! activation: `profile::refresh_active`), then decides with `boot_snapshot::boot_mods` (launcher/src-tauri/src/
//! commands/cook.rs). The case that must not go wrong: the player switched an OnBoot mod on after the last start, so the
//! profile.json on disk is the old one without it; the first Play must still make the snapshot.
#![cfg(feature = "cooktools")]
use kiln_manage::boot_snapshot::{self, BootSnapshot};
use kiln_manage::cook;
use kiln_manage::cooktools::Locate;
use kiln_manage::game::{GameInstall, GameKind, PathMode, Platform};
use kiln_manage::library::{self, LibraryRef};
use kiln_manage::manifest::ModKind;
use kiln_manage::paths::DataDirs;
use kiln_manage::profile::{self, ProfileMod};
use kiln_manage::task::NoProgress;
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}

fn kiln_mod(root: &Path, id: &str, extra: &str) -> PathBuf {
    let d = root.join(format!("src/{id}"));
    put(&d, "kiln.mod.json", format!(r#"{{ "id": "{id}", "version": "1.0.0", "apiVersion": "0.2", "entryAssembly": "M.dll", "side": "both"{extra} }}"#).as_bytes());
    put(&d, "M.dll", id.as_bytes());
    d
}

fn pm(r: &library::ImportReport, id: &str, enabled: bool) -> ProfileMod {
    let x: &LibraryRef = r.imported.iter().chain(&r.existing).find(|x| x.kind == ModKind::Kiln && x.id == id).unwrap();
    ProfileMod { id: x.id.clone(), version: x.version.clone(), hash: x.hash.clone(), enabled }
}

#[test]
fn the_first_play_after_an_onboot_mod_is_switched_on_makes_the_snapshot() {
    let t = Tmp(std::env::temp_dir().join(format!("km-bootsnap-play-{}", std::process::id())));
    let _ = std::fs::remove_dir_all(&t.0);
    std::fs::create_dir_all(&t.0).unwrap();
    let root = t.0.canonicalize().unwrap();
    let dirs = DataDirs::at(root.join("data"));
    let game = root.join("game");
    put(&game, "enshrouded.exe", b"MZ");
    std::fs::create_dir_all(game.join("kiln")).unwrap();
    let inst = GameInstall {
        id: "g-bootsnap".into(), kind: GameKind::Client, path: game.clone(), exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID, library: None, platform: Platform::Proton, prefix: None, build_id: None,
        exe_sha256: None, steam_flavor: None, path_mode: PathMode::Copy, source: "manual".into(),
    };
    let r = library::import(&dirs, &[kiln_mod(&root, "plain", ""), kiln_mod(&root, "booter", r#", "boot": true"#)], &mut NoProgress).unwrap();
    // the last start: the OnBoot mod was off, and that is the profile.json in the game folder
    let mut p = profile::create(&dirs, "Boot", None).unwrap();
    p.mods.kiln = vec![pm(&r, "plain", true), pm(&r, "booter", false)];
    let p = profile::save(&dirs, &p).unwrap();
    profile::activate(&dirs, &p.id, &inst).unwrap();
    assert!(boot_snapshot::boot_mods(&inst).is_empty(), "the last start had no OnBoot mod");
    // the player switches it on in the launcher: the profile changes, the game folder's profile.json does not yet
    let mut p = profile::get(&dirs, &p.id).unwrap();
    p.mods.kiln = vec![pm(&r, "plain", true), pm(&r, "booter", true)];
    profile::save(&dirs, &p).unwrap();
    assert!(boot_snapshot::boot_mods(&inst).is_empty(), "read before Play's cook, the old profile.json would skip it");

    // Play, as cook_run runs it: the cook (which writes the active profile again first), then the snapshot step
    let locate = Locate::default();  // nothing to cook: no tool is located
    let run = cook::cook_due(&dirs, &inst, None, &locate, &mut |_, _, _, _| {}, &|| false).unwrap();
    assert!(run.activation.is_some(), "the cook wrote the changed profile");
    assert_eq!(boot_snapshot::boot_mods(&inst), ["booter"], "the step reads the profile of this start");
    let calls = std::cell::Cell::new(0);
    let s = boot_snapshot::make_if_needed(&boot_snapshot::boot_mods(&inst), || {
        calls.set(calls.get() + 1);
        BootSnapshot { status: "made".into(), ..Default::default() }
    });
    assert_eq!((s.status.as_str(), calls.get()), ("made", 1));
}

/// Play once with another profile (launch mode `once`): the active profile has no OnBoot mod, the one started once
/// has. The step decides from that profile before the start writes its file (kiln/profile.once.json, which kiln.boot
/// reads at that start), and it decides as kiln.boot then finds that file; the active profile stays as it was.
#[test]
fn a_single_start_with_another_profile_decides_from_that_profile() {
    let t = Tmp(std::env::temp_dir().join(format!("km-bootsnap-once-{}", std::process::id())));
    let _ = std::fs::remove_dir_all(&t.0);
    std::fs::create_dir_all(&t.0).unwrap();
    let root = t.0.canonicalize().unwrap();
    let dirs = DataDirs::at(root.join("data"));
    let game = root.join("game");
    put(&game, "enshrouded.exe", b"MZ");
    // a hand-installed drop-in that boots: loaded only by a profile that loads Kiln drop-ins
    put(&game, "kiln/mods/dropboot/kiln.mod.json", br#"{ "id": "dropboot", "version": "1.0.0", "apiVersion": "0.2", "entryAssembly": "D.dll", "boot": true }"#);
    let inst = GameInstall {
        id: "g-bootsnap-once".into(), kind: GameKind::Client, path: game.clone(), exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID, library: None, platform: Platform::Proton, prefix: None, build_id: None,
        exe_sha256: None, steam_flavor: None, path_mode: PathMode::Copy, source: "manual".into(),
    };
    let r = library::import(&dirs, &[kiln_mod(&root, "plain", ""), kiln_mod(&root, "booter", r#", "boot": true"#)], &mut NoProgress).unwrap();
    let mut main = profile::create(&dirs, "Main", None).unwrap();
    main.mods.kiln = vec![pm(&r, "plain", true), pm(&r, "booter", false)];
    main.dropins = vec![];
    let main = profile::save(&dirs, &main).unwrap();
    profile::activate(&dirs, &main.id, &inst).unwrap();
    let mut once = profile::create(&dirs, "Once", None).unwrap();
    once.mods.kiln = vec![pm(&r, "plain", true), pm(&r, "booter", true)];
    once.dropins = vec![ModKind::Kiln];
    let once = profile::save(&dirs, &once).unwrap();
    assert!(boot_snapshot::boot_mods(&inst).is_empty(), "Play's rule reads the active profile: no OnBoot mod there");
    assert_eq!(boot_snapshot::boot_mods_once(&dirs, &inst, &once.id).unwrap(), ["booter", "dropboot"]);
    // the drop-ins only by the profile's own rule; a switched-off OnBoot mod does not count
    let mut off = profile::get(&dirs, &once.id).unwrap();
    off.mods.kiln = vec![pm(&r, "plain", true), pm(&r, "booter", false)];
    off.dropins = vec![];
    profile::save(&dirs, &off).unwrap();
    assert!(boot_snapshot::boot_mods_once(&dirs, &inst, &once.id).unwrap().is_empty());
    let mut back = profile::get(&dirs, &once.id).unwrap();
    back.mods.kiln = vec![pm(&r, "plain", true), pm(&r, "booter", true)];
    back.dropins = vec![ModKind::Kiln];
    profile::save(&dirs, &back).unwrap();
    assert_eq!(boot_snapshot::boot_mods_once(&dirs, &inst, &once.id).unwrap(), ["booter", "dropboot"]);
    assert!(boot_snapshot::boot_mods_once(&dirs, &inst, "no-such-profile").is_err());

    // the start itself: kiln.boot reads kiln/profile.once.json, which names the same OnBoot mod, enabled
    let req = kiln_manage::launch::LaunchRequest { mode: kiln_manage::launch::LaunchMode::Once, profile_id: Some(once.id.clone()), skip_backup: true };
    kiln_manage::launch::play_with(&dirs, &inst, &req, Some(&[]), &|_| Ok(())).unwrap();
    let text = std::fs::read_to_string(game.join("kiln/profile.once.json")).unwrap();
    let file: profile::ProfileFile = serde_json::from_str(&text).unwrap();
    let booting: Vec<String> = kiln_manage::launch::kiln_folders(&inst, &file).into_iter()
        .filter(|(_, folder)| kiln_manage::manifest::read_kiln(folder).is_ok_and(|m| m.boot)).map(|(m, _)| m.id.clone()).collect();
    assert_eq!(booting, ["booter"]);
    assert!(file.dropins.names(ModKind::Kiln), "the once file loads Kiln drop-ins too");
    assert!(boot_snapshot::boot_mods(&inst).is_empty(), "a single start leaves the active profile as it was");
}
