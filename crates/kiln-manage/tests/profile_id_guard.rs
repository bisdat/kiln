//! New profile IDs must not turn an ambiguous foreign game pointer into this data folder's profile.
//! All roots, install records and game files are disposable; no game or launcher process is started.

use kiln_manage::game::{self, GameInstall, GameKind, PathMode, Platform};
use kiln_manage::manifest::ModKind;
use kiln_manage::paths::DataDirs;
use kiln_manage::profile::{self, GameProfileOwner, ProfileFile};
use kiln_manage::util;
use std::path::PathBuf;

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn fixture(tag: &str) -> (Tmp, DataDirs, DataDirs, GameInstall) {
    let root = std::env::temp_dir().join(format!("km-profile-id-guard-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    std::fs::create_dir_all(&root).unwrap();
    let tmp = Tmp(kiln_manage::steam::strip_verbatim(root.canonicalize().unwrap()));
    let local = DataDirs::at(tmp.0.join("local"));
    let foreign = DataDirs::at(tmp.0.join("foreign"));
    std::fs::create_dir_all(&local.root).unwrap();
    assert_eq!(std::fs::read_dir(&local.root).unwrap().count(), 0);
    let install = GameInstall {
        id: "selected".into(), kind: GameKind::Client, path: tmp.0.join("game"), exe: "enshrouded.exe".into(),
        app_id: kiln_manage::CLIENT_APP_ID, library: None, platform: Platform::Proton, prefix: None,
        build_id: None, exe_sha256: None, steam_flavor: None, path_mode: PathMode::Copy, source: "manual".into(),
    };
    std::fs::create_dir_all(install.path.join("kiln")).unwrap();
    game::write_record(&local, &install, None).unwrap();
    util::write_json_atomic(&local.settings(), &serde_json::json!({ "defaultGameId": install.id })).unwrap();
    assert!(profile::list(&local).unwrap().is_empty());
    assert!(!activation_record(&local, &install).exists());
    (tmp, local, foreign, install)
}

fn activation_record(dirs: &DataDirs, install: &GameInstall) -> PathBuf {
    dirs.installs().join(format!("{}.active.json", install.id))
}

fn foreign_copy(foreign: &DataDirs, install: &GameInstall, name: &str) -> ProfileFile {
    let mut p = profile::create(foreign, name, None).unwrap();
    // Unlike a newly created empty local profile, so an accidental refresh really would reapply it.
    p.dropins = vec![ModKind::Kiln];
    p.dropins_engines = true;
    let p = profile::save(foreign, &p).unwrap();
    profile::render(foreign, &p, install, PathMode::Copy).unwrap()
}

fn write_pointer(install: &GameInstall, file: &ProfileFile) -> Vec<u8> {
    let path = install.path.join("kiln/profile.json");
    util::write_json_atomic(&path, file).unwrap();
    std::fs::read(path).unwrap()
}

fn assert_foreign_untouched(local: &DataDirs, install: &GameInstall, id: &str, before: &[u8]) {
    let owner = profile::game_profile_owner(local, install).unwrap();
    // Exercise the actual backend the launcher watcher calls, not a mocked ownership decision.
    let refreshed = profile::refresh_active(local, install).unwrap();
    let after = std::fs::read(install.path.join("kiln/profile.json")).unwrap();
    assert_eq!(
        (owner, refreshed.is_none(), after == before, activation_record(local, install).exists()),
        (GameProfileOwner::Elsewhere { id: id.into(), root: None }, true, true, false),
        "creation must leave ownership, refresh, exact game bytes and the absent activation record unchanged",
    );
}

#[test]
fn first_profile_does_not_adopt_a_foreign_copy_pointer() {
    let (_tmp, local, foreign, install) = fixture("first");
    let file = foreign_copy(&foreign, &install, "My mods");
    let before = write_pointer(&install, &file);
    assert_foreign_untouched(&local, &install, &file.id, &before);

    let p = profile::create(&local, "My mods", None).unwrap();
    assert_foreign_untouched(&local, &install, &file.id, &before);
    assert_eq!((p.id.as_str(), p.name.as_str()), ("my-mods-2", "My mods"));
    assert!(!local.profile(&file.id).exists());
}

#[test]
fn rootless_pointers_reserve_the_base_and_suffix_alongside_local_collisions() {
    let (_tmp, local, foreign, install) = fixture("suffix");
    let file = foreign_copy(&foreign, &install, "My mods");
    let before = write_pointer(&install, &file);
    let mut other = install.clone();
    other.id = "other".into();
    other.path = install.path.with_file_name("other-game");
    game::write_record(&local, &other, None).unwrap();
    let mut second = file.clone();
    second.id = "my-mods-2".into();
    second.config_dir = None; // legacy/rootless, not only an explicitly copy-mode pointer
    let other_before = write_pointer(&other, &second);
    // Existing local directories remain collisions even without a valid meta.json.
    std::fs::create_dir_all(local.profile("my-mods-3")).unwrap();

    let p = profile::create(&local, "My mods", None).unwrap();
    assert_eq!((p.id.as_str(), p.name.as_str()), ("my-mods-4", "My mods"));
    assert_foreign_untouched(&local, &install, &file.id, &before);
    assert_foreign_untouched(&local, &other, &second.id, &other_before);
}

#[test]
#[cfg(unix)] // An unmapped Wine drive; native Windows absolute paths already name a root.
fn unresolvable_absolute_pointer_reserves_its_id_without_guessing_a_root() {
    let (_tmp, local, foreign, install) = fixture("unresolved");
    let mut file = foreign_copy(&foreign, &install, "My mods");
    file.path_mode = Some(PathMode::Z);
    file.config_dir = Some(r"Q:\unmapped\profiles\my-mods\config".into());
    let before = write_pointer(&install, &file);

    let p = profile::create(&local, "My mods", None).unwrap();
    assert_eq!((p.id.as_str(), p.name.as_str()), ("my-mods-2", "My mods"));
    assert_foreign_untouched(&local, &install, &file.id, &before);
}

#[test]
fn local_copy_collision_and_copy_from_keep_existing_behavior() {
    let (_tmp, local, _foreign, install) = fixture("local");
    let mut first = profile::create(&local, "My mods", None).unwrap();
    assert_eq!(first.id, "my-mods"); // no game pointer: the ordinary first ID is unchanged
    first.notes = "copy these notes".into();
    let first = profile::save(&local, &first).unwrap();
    std::fs::write(local.profile(&first.id).join("config/test.json"), b"{\"local\":true}").unwrap();
    let file = profile::render(&local, &first, &install, PathMode::Copy).unwrap();
    let before = write_pointer(&install, &file);
    assert_eq!(profile::game_profile_owner(&local, &install).unwrap(), GameProfileOwner::Here { id: first.id.clone() });

    let copy = profile::create(&local, "My mods", Some(&first.id)).unwrap();
    assert_eq!((copy.id.as_str(), copy.name.as_str()), ("my-mods-2", "My mods"));
    assert_eq!(copy.notes, first.notes);
    assert_eq!(copy.mods, first.mods);
    assert_eq!(std::fs::read(local.profile(&copy.id).join("config/test.json")).unwrap(), b"{\"local\":true}");
    assert_eq!(profile::game_profile_owner(&local, &install).unwrap(), GameProfileOwner::Here { id: first.id });
    assert_eq!(std::fs::read(install.path.join("kiln/profile.json")).unwrap(), before);
    assert!(!activation_record(&local, &install).exists());
}

#[test]
fn rooted_same_id_still_uses_the_base_and_stays_foreign() {
    let (_tmp, local, foreign, install) = fixture("rooted");
    let mut file = foreign_copy(&foreign, &install, "My mods");
    file.path_mode = Some(PathMode::Native);
    file.config_dir = Some(foreign.profile(&file.id).join("config").display().to_string());
    let before = write_pointer(&install, &file);
    let p = profile::create(&local, "My mods", None).unwrap();
    assert_eq!((p.id.as_str(), p.name.as_str()), ("my-mods", "My mods"));
    assert_eq!(profile::game_profile_owner(&local, &install).unwrap(), GameProfileOwner::Elsewhere { id: file.id, root: Some(foreign.root) });
    assert!(profile::refresh_active(&local, &install).unwrap().is_none());
    assert_eq!(std::fs::read(install.path.join("kiln/profile.json")).unwrap(), before);
    assert!(!activation_record(&local, &install).exists());
}

#[test]
#[cfg(unix)]
fn z_rooted_same_id_still_uses_the_base_and_stays_foreign() {
    let (tmp, local, foreign, mut install) = fixture("z-rooted");
    let prefix = tmp.0.join("prefix");
    std::fs::create_dir_all(prefix.join("dosdevices")).unwrap();
    std::os::unix::fs::symlink("/", prefix.join("dosdevices/z:")).unwrap();
    install.prefix = Some(prefix);
    install.path_mode = PathMode::Z;
    game::write_record(&local, &install, None).unwrap();
    let mut file = foreign_copy(&foreign, &install, "My mods");
    file.path_mode = Some(PathMode::Z);
    file.config_dir = Some(kiln_manage::winpath::unix_to_z(&foreign.profile(&file.id).join("config")).unwrap());
    let before = write_pointer(&install, &file);

    let p = profile::create(&local, "My mods", None).unwrap();
    assert_eq!((p.id.as_str(), p.name.as_str()), ("my-mods", "My mods"));
    assert_eq!(profile::game_profile_owner(&local, &install).unwrap(), GameProfileOwner::Elsewhere { id: file.id, root: Some(foreign.root) });
    assert!(profile::refresh_active(&local, &install).unwrap().is_none());
    assert_eq!(std::fs::read(install.path.join("kiln/profile.json")).unwrap(), before);
    assert!(!activation_record(&local, &install).exists());
}

#[test]
fn unrelated_missing_invalid_and_unreadable_pointers_do_not_block_creation() {
    let (_tmp, local, _foreign, install) = fixture("invalid");
    // A missing pointer, invalid JSON, a future format and a path that is not a file are unrelated to ID allocation.
    for (n, bytes) in [None, Some(b"{ invalid".as_slice()), Some(br#"{"format":99,"id":"my-mods","generation":1,"mods":{}}"#.as_slice())].into_iter().enumerate() {
        let path = install.path.join("kiln/profile.json");
        if let Some(bytes) = bytes {
            std::fs::write(&path, bytes).unwrap();
        }
        let name = format!("Ordinary {n}");
        assert_eq!(profile::create(&local, &name, None).unwrap().id, format!("ordinary-{n}"));
    }
    let path = install.path.join("kiln/profile.json");
    std::fs::remove_file(&path).unwrap();
    std::fs::create_dir(&path).unwrap();
    assert_eq!(profile::create(&local, "My mods", None).unwrap().id, "my-mods");
    assert!(path.is_dir());
    assert!(!activation_record(&local, &install).exists());
}

#[test]
#[cfg(feature = "zip")]
fn pack_install_only_does_not_adopt_a_foreign_copy_pointer() {
    use kiln_manage::modpack::{ModpackSource, pack};
    use kiln_manage::task::NoProgress;
    let (tmp, local, foreign, install) = fixture("pack");
    let file = foreign_copy(&foreign, &install, "My mods");
    let before = write_pointer(&install, &file);
    let built = pack::build(&foreign, &pack::BuildRequest {
        profile: file.id.clone(), version: "1.0.0".into(), ..Default::default()
    }, &tmp.0.join("pack-out"), &mut NoProgress).unwrap();
    let result = pack::install(&local, Some(&install), &pack::InstallRequest {
        source: ModpackSource::Path { path: built.file, sha256: Some(built.sha256) },
        consent: true, allow_http: false, name: None, optional: Default::default(), activate: Some(false),
    }, &mut NoProgress).unwrap();

    assert!(result.activation.is_none());
    assert!(result.backup.is_none());
    assert_foreign_untouched(&local, &install, &file.id, &before);
    assert_eq!((result.profile.id.as_str(), result.profile.name.as_str()), ("my-mods-2", "My mods"));
    assert!(pack::instance(&local, &result.profile.id).unwrap().is_some());
}
