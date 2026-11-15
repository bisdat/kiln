use kiln_manage::{library::{self, LibraryRef}, paths::DataDirs, profile::{self, ProfileMod}, task::NoProgress, pe::build_test_dll, manifest::ModKind};
use std::path::PathBuf;
struct World { tmp: library::TempDir, dirs: DataDirs, old: LibraryRef, pid: String }
fn world() -> World {
    let tmp = library::TempDir::new(&std::env::temp_dir(), "replace-dll-test").unwrap();
    let dirs = DataDirs::at(tmp.path().join("data"));
    let file = tmp.path().join("old.dll");
    std::fs::write(&file, build_test_dll(&[], b"old")).unwrap();
    let old = library::import(&dirs, &[file], &mut NoProgress).unwrap().imported.remove(0);
    let mut p = profile::create(&dirs, "test", None).unwrap();
    p.mods.list_mut(old.kind).push(ProfileMod { id: old.id.clone(), version: old.version.clone(), hash: old.hash.clone(), enabled: true });
    let pid = profile::save(&dirs, &p).unwrap().id;
    World { tmp, dirs, old, pid }
}
fn file(w: &World, name: &str, bytes: &[u8]) -> PathBuf { let p = w.tmp.path().join(name); std::fs::write(&p, bytes).unwrap(); p }
#[test]
fn replace_dll_matching_keeps_old_and_rollback_bytes() {
    let w = world(); let original = std::fs::read(library::path_of(&w.dirs, &w.old).unwrap().join("old.dll")).unwrap();
    let settings = w.dirs.profile(&w.pid).join("config/native/custom.ini");
    std::fs::create_dir_all(settings.parent().unwrap()).unwrap();
    std::fs::write(&settings, b"loot=custom").unwrap();
    let f = file(&w, "OLD.DLL", &build_test_dll(&[], b"new"));
    use kiln_manage::updates::{receipt, coordinator, status};
    let spec = kiln_manage::manifest::UpdatesSpec::Nexus { game_id: 5811, mod_id: 170, file_chain_id: "original-chain".into() };
    receipt::set_manual_source(&w.dirs, w.old.kind, &w.old.id, &spec, true, 1800000000).unwrap();
    receipt::capture_archive(&w.dirs, &[w.old.clone()], &"a".repeat(64)).unwrap();
    struct Provider { original: LibraryRef }
    impl coordinator::UpdateProvider for Provider {
        fn provider(&self) -> status::Provider { status::Provider::Nexus }
        fn check(&self, items: &[coordinator::CheckItem], _: &coordinator::CheckOptions, _: &mut dyn kiln_manage::task::Progress) -> kiln_manage::Result<Vec<status::ModUpdateStatus>> {
            assert_eq!(items[0].target.library_ref, self.original);
            assert_eq!(items[0].receipt.as_ref().unwrap().installed_version, self.original.version);
            assert_eq!(items[0].spec, kiln_manage::manifest::UpdatesSpec::Nexus { game_id: 5811, mod_id: 170, file_chain_id: "original-chain".into() });
            Ok(items.iter().map(|i| i.status(false).with_state(status::UpdateState::Current)).collect())
        }
    }
    let next = library::replace_dll(&w.dirs, &w.pid, &w.old, &f, false).unwrap().entry;
    assert_eq!(next.id, w.old.id); assert_eq!(next.version, w.old.version); assert_ne!(next.hash, w.old.hash);
    assert_eq!(library::record(&w.dirs, &next).unwrap().source.unwrap().kind, "replaced-dll");
    assert!(profile::get(&w.dirs, &w.pid).unwrap().mods.list(ModKind::Native)[0].enabled);
    assert_eq!(kiln_manage::updates::coordinator::targets(&w.dirs).unwrap()[0].library_ref, w.old);
    assert_eq!(std::fs::read(&settings).unwrap(), b"loot=custom");
    let opts = coordinator::CheckOptions { offline: false, refresh: true, automatic: false, now: 1800000000, api_version: "test".into() };
    coordinator::check(&w.dirs, &[&Provider { original: w.old.clone() }], &opts, &mut NoProgress).unwrap();
    assert!(receipt::load(&w.dirs).unwrap().receipt(&next).is_none()); // no fabricated provider provenance
    assert!(kiln_manage::kept::is_kept(&w.dirs, &w.old));
    kiln_manage::kept::rollback(&w.dirs, None, &w.pid, w.old.kind, &w.old.id, None).unwrap();
    assert_eq!(std::fs::read(library::path_of(&w.dirs, &w.old).unwrap().join("old.dll")).unwrap(), original);
    assert_eq!(profile::get(&w.dirs, &w.pid).unwrap().mods.list(w.old.kind)[0].hash, w.old.hash);
}
#[test]
fn replace_dll_rename_needs_confirmation_and_keeps_name() {
    let w = world(); let f = file(&w, "patched.dll", &build_test_dll(&[], b"new"));
    assert!(library::replace_dll(&w.dirs, &w.pid, &w.old, &f, false).unwrap_err().message.contains("old.dll"));
    let r = library::replace_dll(&w.dirs, &w.pid, &w.old, &f, true).unwrap().entry;
    let folder = library::path_of(&w.dirs, &r).unwrap(); assert!(folder.join("old.dll").exists()); assert!(!folder.join("patched.dll").exists());
}
#[test]
fn replace_dll_rejects_invalid_and_links() {
    let w = world();
    for bytes in [b"not a dll".to_vec(), { let mut b = build_test_dll(&[], b""); b[0x84..0x86].copy_from_slice(&0x14cu16.to_le_bytes()); b }] {
        let f = file(&w, "bad.dll", &bytes); assert!(library::replace_dll(&w.dirs, &w.pid, &w.old, &f, true).is_err());
    }
    let f = file(&w, "valid.txt", &build_test_dll(&[], b"valid"));
    assert!(library::replace_dll(&w.dirs, &w.pid, &w.old, &f, true).is_err());
    #[cfg(unix)] { let f = w.tmp.path().join("link.dll"); std::os::unix::fs::symlink(w.tmp.path().join("old.dll"), &f).unwrap(); assert!(library::replace_dll(&w.dirs, &w.pid, &w.old, &f, true).is_err()); }
}

#[test]
fn replace_dll_ambiguous_lists_dlls_and_kind_change_requires_confirmation() {
    let w = world();
    let package = w.tmp.path().join("multi"); std::fs::create_dir_all(&package).unwrap();
    std::fs::write(package.join("native.json"), br#"{"id":"multi","version":"1.0.0","dlls":["one.dll","two.dll"]}"#).unwrap();
    for name in ["one.dll", "two.dll"] { std::fs::write(package.join(name), build_test_dll(&[], b"old")).unwrap(); }
    let old = library::import(&w.dirs, &[package], &mut NoProgress).unwrap().imported.remove(0);
    let mut p = profile::get(&w.dirs, &w.pid).unwrap();
    p.mods.native.push(ProfileMod { id: old.id.clone(), version: old.version.clone(), hash: old.hash.clone(), enabled: false });
    profile::save(&w.dirs, &p).unwrap();
    let f = file(&w, "patch.dll", &build_test_dll(&[], b"new"));
    let e = library::replace_dll(&w.dirs, &w.pid, &old, &f, true).unwrap_err();
    assert!(e.message.contains("one.dll") && e.message.contains("two.dll"));
    let f = file(&w, "ONE.DLL", &build_test_dll(&[], b"new"));
    let new = library::replace_dll(&w.dirs, &w.pid, &old, &f, false).unwrap().entry;
    assert_eq!(std::fs::read(library::path_of(&w.dirs, &new).unwrap().join("two.dll")).unwrap(), build_test_dll(&[], b"old"));
    let saved = profile::get(&w.dirs, &w.pid).unwrap();
    assert_eq!(saved.mods.native[0].id, w.old.id); assert!(!saved.mods.native[1].enabled);
    let f = file(&w, "old.dll", &build_test_dll(&["kiln_plugin_main"], b"plugin"));
    let e = library::replace_dll(&w.dirs, &w.pid, &w.old, &f, false).unwrap_err();
    assert!(e.message.contains("different kind"));
    library::replace_dll(&w.dirs, &w.pid, &w.old, &f, true).unwrap().entry;
}
