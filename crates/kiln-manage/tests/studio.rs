//! Pack Studio (DESIGN.md section 3): pack sources made from a profile, settings captured (never hand-written), the
//! pack layer's files and lint, credits, the build, the test instance, the load order. On temp folders; nothing
//! reaches a game or a server process.
#![cfg(feature = "zip")]

use kiln_manage::library::{self, LibraryRef};
use kiln_manage::modpack::pack::{self, Credit, Dep, Env, Need, PartSide};
use kiln_manage::modpack::studio::{self, CaptureChoice, NewPack, Visibility};
use kiln_manage::paths::DataDirs;
use kiln_manage::profile::{self, ProfileMod};
use kiln_manage::task::NoProgress;
use kiln_manage::ErrorCode;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-studio-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p.canonicalize().unwrap())
}
fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}
fn kiln_mod(root: &Path, id: &str, version: &str) -> PathBuf {
    let d = root.join(format!("src/{id}-{version}"));
    put(&d, "kiln.mod.json", format!(r#"{{ "id": "{id}", "name": "Mod {id}", "version": "{version}", "apiVersion": "0.2", "entryAssembly": "M.dll", "side": "both" }}"#).as_bytes());
    put(&d, "M.dll", format!("{id} {version}").as_bytes());
    d
}
fn import(dirs: &DataDirs, paths: &[PathBuf]) -> Vec<LibraryRef> {
    let r = library::import(dirs, paths, &mut NoProgress).unwrap();
    assert!(r.skipped.is_empty(), "{:?}", r.skipped);
    r.imported.iter().chain(&r.existing).cloned().collect()
}
fn pm(refs: &[LibraryRef], id: &str, enabled: bool) -> ProfileMod {
    let r = refs.iter().find(|r| r.id == id).unwrap();
    ProfileMod { id: r.id.clone(), version: r.version.clone(), hash: r.hash.clone(), enabled }
}

/// The creator's profile: alpha (on), beta (off), alpha's settings with a .schema snapshot of its defaults.
fn creator(dirs: &DataDirs, src: &Path) -> String {
    let refs = import(dirs, &[kiln_mod(src, "alpha", "1.0.0"), kiln_mod(src, "beta", "1.0.0")]);
    let mut p = profile::create(dirs, "Ember run", None).unwrap();
    p.mods.kiln = vec![pm(&refs, "alpha", true), pm(&refs, "beta", false)];
    profile::save(dirs, &p).unwrap();
    let cfg = dirs.profile(&p.id).join("config");
    put(&cfg, "alpha.json", br#"{ "range": 20, "smash": false, "hotkey": "F9", "loot": { "chance": 5 }, "discord": { "token": "abc" } }"#);
    put(&cfg, ".schema/alpha.json", br#"{ "format": 1, "source": "kiln", "file": "alpha.json", "id": "alpha", "mod": "alpha",
        "entries": [ { "path": "range", "type": "int", "default": 12 }, { "path": "smash", "type": "bool", "default": true },
                     { "path": "hotkey", "type": "string", "default": "F1" }, { "path": "loot.chance", "type": "int", "default": 5 },
                     { "path": "discord.token", "type": "string", "default": "" } ] }"#);
    put(&cfg, "beta/settings.ini", b"[Loot]\nchance = 7\n");
    put(&cfg, "notes.txt", b"hello\n");
    p.id
}

fn studio_of(dirs: &DataDirs, src: &Path) -> studio::Studio {
    let pid = creator(dirs, src);
    studio::create(dirs, &NewPack { name: "Ember run".into(), from: Some(pid), id: None }).unwrap()
}

#[test]
fn a_pack_starts_from_a_profile_or_empty_and_ids_stay_unique() {
    let t = tmp("create");
    let dirs = DataDirs::at(t.0.join("data"));
    let st = studio_of(&dirs, &t.0);
    assert_eq!(st.source.id, "ember-run");
    assert_eq!(st.source.version, "1.0.0");
    assert_eq!(st.profile_name.as_deref(), Some("Ember run"));
    let ids: Vec<(&str, Dep, Option<bool>)> = st.mods.iter().map(|m| (m.id.as_str(), m.dep, m.default)).collect();
    // beta is switched off in the profile: optional, off
    assert_eq!(ids, [("alpha", Dep::Required, None), ("beta", Dep::Optional, Some(false))]);
    assert_eq!(st.mods[0].name, "Mod alpha");
    assert!(st.dir.join("layer/kiln.mod.json").exists(), "the layer's manifest is there for lint");
    // the same name again: another id; an empty pack makes its own profile
    let again = studio::create(&dirs, &NewPack { name: "Ember run".into(), from: None, id: None }).unwrap();
    assert_eq!(again.source.id, "ember-run-2");
    assert_ne!(again.source.profile, st.source.profile);
    assert!(again.mods.is_empty());
    let list = studio::list(&dirs).unwrap();
    assert_eq!(list.len(), 2);
    assert!(list.iter().any(|s| s.id == "ember-run" && s.mods == 2));
    // init refuses a folder that holds a pack
    let e = studio::init(&dirs, &st.dir, &NewPack { name: "X".into(), ..Default::default() }).unwrap_err();
    assert_eq!(e.code, ErrorCode::Conflict);
    // delete moves it to backups and keeps the profile
    studio::delete(&dirs, &again.dir).unwrap();
    assert!(!again.dir.exists());
    assert!(profile::get(&dirs, &again.source.profile).is_ok());
}

#[test]
fn settings_are_captured_from_the_profile_against_the_mods_defaults() {
    let t = tmp("capture");
    let dirs = DataDirs::at(t.0.join("data"));
    let st = studio_of(&dirs, &t.0);
    let files = studio::capture(&dirs, &st.dir).unwrap();
    let alpha = files.iter().find(|f| f.path == "alpha.json").unwrap();
    assert!(alpha.defaults);
    let keys: Vec<&str> = alpha.keys.iter().map(|k| k.key.as_str()).collect();
    // loot.chance equals its default: not offered
    assert_eq!(keys, ["discord.token", "hotkey", "range", "smash"]);
    let k = |n: &str| alpha.keys.iter().find(|k| k.key == n).unwrap();
    assert_eq!(k("range").default, Some(serde_json::json!(12)));
    assert!(k("hotkey").personal && !k("range").personal);
    assert!(k("discord.token").secret.is_some(), "a token never travels");
    // no snapshot: every key is offered (INI), a .txt travels whole
    let beta = files.iter().find(|f| f.path == "beta/settings.ini").unwrap();
    assert!(!beta.defaults);
    assert_eq!(beta.keys.iter().map(|k| k.key.as_str()).collect::<Vec<_>>(), ["Loot.chance"]);
    let notes = files.iter().find(|f| f.path == "notes.txt").unwrap();
    assert!(notes.keys.is_empty());
    assert!(!files.iter().any(|f| f.path.starts_with(".schema")), "snapshots are not settings");

    // chosen keys become overrides; a secret, a key that is not offered and the whole file with a secret are refused
    let ch = |path: &str, keys: &[&str], whole: bool| CaptureChoice { path: path.into(), side: PartSide::Both, keys: keys.iter().map(|s| s.to_string()).collect(), whole };
    assert_eq!(studio::set_capture(&dirs, &st.dir, &[ch("alpha.json", &["discord.token"], false)]).unwrap_err().code, ErrorCode::Refused);
    assert_eq!(studio::set_capture(&dirs, &st.dir, &[ch("alpha.json", &["loot.chance"], false)]).unwrap_err().code, ErrorCode::NotFound);
    assert_eq!(studio::set_capture(&dirs, &st.dir, &[ch("alpha.json", &[], true)]).unwrap_err().code, ErrorCode::Refused);
    assert_eq!(studio::set_capture(&dirs, &st.dir, &[ch("../x.json", &[], true)]).unwrap_err().code, ErrorCode::NotFound);
    let st2 = studio::set_capture(&dirs, &st.dir, &[ch("alpha.json", &["range", "smash"], false), ch("notes.txt", &[], false), ch("beta/settings.ini", &["Loot.chance"], false)]).unwrap();
    let ov: Vec<(&str, Vec<String>)> = st2.source.overrides.iter().map(|o| (o.path.as_str(), o.keys.clone())).collect();
    assert_eq!(ov, [("alpha.json", vec!["range".to_string(), "smash".to_string()]), ("notes.txt", vec![]), ("beta/settings.ini", vec!["Loot.chance".to_string()])]);
    let again = studio::capture(&dirs, &st.dir).unwrap();
    let a = again.iter().find(|f| f.path == "alpha.json").unwrap();
    assert!(a.keys.iter().find(|k| k.key == "range").unwrap().chosen);
    assert!(!a.keys.iter().find(|k| k.key == "hotkey").unwrap().chosen);
    assert!(again.iter().find(|f| f.path == "notes.txt").unwrap().whole);

    // the build carries exactly those keys
    let built = studio::build(&dirs, &st.dir, None, &mut NoProgress).unwrap();
    let rp = pack::read(&built.file, None, None).unwrap();
    let text = String::from_utf8(rp.overrides[&(PartSide::Both, "alpha.json".to_string())].clone()).unwrap();
    assert!(text.contains("range") && text.contains("smash") && !text.contains("hotkey") && !text.contains("token"), "{text}");
}

#[test]
fn the_source_builds_with_its_choices_layer_and_credits() {
    let t = tmp("build");
    let dirs = DataDirs::at(t.0.join("data"));
    let st = studio_of(&dirs, &t.0);
    let mut s = st.source.clone();
    s.mods.insert("beta".into(), studio::ModChoice { dep: Dep::Optional, default: Some(true), env: Some(Env { client: Need::Optional, server: Need::Unsupported }) });
    s.mods.insert("gone".into(), studio::ModChoice::default());
    s.version = "1.2.0".into();
    s.summary = Some("A loot run".into());
    s.changes = Some("Rebalanced loot".into());
    s.server_settings = Some(serde_json::json!({ "gameSettings": { "playerHealthFactor": 1.5 } }));
    let st = studio::save(&dirs, &st.dir, s.clone()).unwrap();
    assert!(!st.source.mods.contains_key("gone"), "choices of mods the profile does not have are dropped");
    // what save keeps fixed, and what it refuses
    let mut bad = st.source.clone();
    bad.id = "other".into();
    bad.profile = "nope".into();
    let kept = studio::save(&dirs, &st.dir, bad).unwrap();
    assert_eq!((kept.source.id.as_str(), kept.source.profile.as_str()), ("ember-run", st.source.profile.as_str()));
    let mut bad = st.source.clone();
    bad.version = "one".into();
    assert_eq!(studio::save(&dirs, &st.dir, bad).unwrap_err().code, ErrorCode::InvalidArgument);
    let mut bad = st.source.clone();
    bad.server_settings = Some(serde_json::json!({ "name": "Mine", "gameSettings": {} }));
    assert_eq!(studio::save(&dirs, &st.dir, bad).unwrap_err().code, ErrorCode::InvalidArgument);

    // the layer: files below content/ only, no secrets; Set in the pack keeps the file's comments
    for bad in ["../x.json", "content/a/b.json", "content/.x.json", "content/aux.json", "layer.json", "content/x.txt"] {
        assert!(studio::layer_write(&st.dir, bad, "{}").is_err(), "{bad}");
    }
    assert_eq!(studio::layer_write(&st.dir, "content/bot.json", r#"{ "token": "abc" }"#).unwrap_err().code, ErrorCode::Refused);
    studio::layer_write(&st.dir, "content/items.json", "{ \"format\": 1, \"items\": { \"Material_T1_Wood_Logs\": { \"maxStackSize\": 500 } } }").unwrap();
    studio::layer_set(&st.dir, "items", "Material_T1_Wood_Logs", "maxStackSize", &serde_json::json!(1000)).unwrap();
    let pj = studio::layer_read(&st.dir, studio::STUDIO_CONTENT).unwrap();
    assert!(pj.contains("// Set by Pack Studio") && pj.contains("1000"), "{pj}");
    let files: Vec<String> = studio::layer_files(&st.dir).unwrap().into_iter().map(|f| f.path).collect();
    assert_eq!(files, ["content/items.json", "content/pack.json"]);

    // credits: an http source is refused; only complete rows go into the index
    let mut c = BTreeMap::new();
    c.insert("alpha".to_string(), Credit { author: "frostdev".into(), source: "http://x".into(), match_kind: None });
    assert_eq!(studio::set_credits(&dirs, &st.dir, &c).unwrap_err().code, ErrorCode::InvalidArgument);
    c.insert("alpha".to_string(), Credit { author: "frostdev".into(), source: "https://github.com/frostdev-ops/alpha".into(), match_kind: Some("kiln".into()) });
    c.insert("beta".to_string(), Credit { author: "someone".into(), source: "".into(), match_kind: None });
    let st = studio::set_credits(&dirs, &st.dir, &c).unwrap();
    assert_eq!(studio::public_needs(&st), ["credits for Mod beta"]);

    let a = studio::build(&dirs, &st.dir, None, &mut NoProgress).unwrap();
    let b = studio::build(&dirs, &st.dir, None, &mut NoProgress).unwrap();
    assert_eq!(a.sha256, b.sha256, "same source, same bytes");
    let ix = &a.index;
    assert_eq!((ix.id.as_str(), ix.version.as_str(), ix.changes.as_deref()), ("ember-run", "1.2.0", Some("Rebalanced loot")));
    let beta = ix.mods.iter().find(|m| m.id == "beta").unwrap();
    assert_eq!((beta.dep, beta.default, beta.env), (Dep::Optional, Some(true), Some(Env { client: Need::Optional, server: Need::Unsupported })));
    assert_eq!(ix.mods.iter().find(|m| m.id == "alpha").unwrap().credit.as_ref().unwrap().author, "frostdev");
    assert!(beta.credit.is_none(), "an incomplete credit is left out");
    assert_eq!(ix.layer.as_ref().unwrap().id, "pack.ember-run");
    assert_eq!(ix.server.as_ref().unwrap().settings.as_ref().unwrap()["gameSettings"]["playerHealthFactor"], serde_json::json!(1.5));
    let unpacked = t.0.join("unpacked");
    pack::read(&a.file, None, Some(&unpacked)).unwrap();
    assert!(std::fs::read_to_string(unpacked.join("layer/content/pack.json")).unwrap().contains("1000"));
    let lm = kiln_manage::manifest::read_kiln(&unpacked.join("layer")).unwrap();
    assert_eq!(lm.version, "1.2.0");
    assert_eq!(lm.load_after, ["alpha", "beta"]);

    // a kept version and the publish record move the next version on
    let name = studio::keep_version(&st.dir, &a).unwrap();
    assert_eq!(name, "ember-run-1.2.0.kilnpack");
    let s = studio::record_published(&st.dir, studio::Published { pack: "pck_1".into(), visibility: Visibility::Link, versions: vec![studio::PublishedVersion { version: "1.2.0".into(), id: "pv_1".into(), published: "now".into(), visibility: Visibility::Link, file: Some(name) }], ..Default::default() }).unwrap();
    assert_eq!(s.version, "1.2.1");
    assert!(s.changes.is_none());
    assert_eq!(studio::version_file(&st.dir, "1.2.0").unwrap(), st.dir.join("versions/ember-run-1.2.0.kilnpack"));
    assert_eq!(studio::version_file(&st.dir, "9.9.9").unwrap_err().code, ErrorCode::NotFound);
}

#[test]
fn the_order_puts_the_layer_last_and_picks_include_what_mods_add() {
    let t = tmp("order");
    let dirs = DataDirs::at(t.0.join("data"));
    let st = studio_of(&dirs, &t.0);
    let o = studio::order(&dirs, &st.dir).unwrap();
    let loads: Vec<&str> = o.kiln.iter().filter(|r| r.loads).map(|r| r.id.as_str()).collect();
    assert_eq!(loads, ["alpha", "pack.ember-run"], "beta is off: not in the order");
    assert!(o.kiln.iter().find(|r| r.id == "pack.ember-run").unwrap().layer);
    studio::layer_write(&st.dir, "content/new.json", r#"{ "format": 1, "items": { "add": [ { "name": "Cinderbrand_Sword", "displayName": "Cinderbrand", "base": "Weapon_T1_1H_Sword_Craft" } ] } }"#).unwrap();
    let picks = studio::pick(&dirs, &st.dir, "items", "cinder", 10).unwrap();
    // the pack's own first; the rest (if this PC has a game-data catalog) are the game's
    assert_eq!((picks[0].name.as_str(), picks[0].from.as_deref()), ("Cinderbrand_Sword", Some("pack.ember-run")));
    assert!(picks[1..].iter().all(|p| p.from.is_none()));
    assert!(studio::pick(&dirs, &st.dir, "recipes", "cinder", 10).unwrap().iter().all(|p| p.from.is_none()));
    // lint runs over the layer with the profile as the other mods
    let r = studio::layer_lint(&dirs, &st.dir, None).unwrap();
    let _ = r.summary;
}

#[test]
fn a_test_instance_is_the_built_pack_installed_and_replaced_by_the_next() {
    let t = tmp("test");
    let dirs = DataDirs::at(t.0.join("data"));
    let st = studio_of(&dirs, &t.0);
    let first = studio::test(&dirs, &st.dir, None, &mut NoProgress).unwrap();
    assert_eq!(first.profile.name, "Ember run (test)");
    assert!(pack::instance(&dirs, &first.profile.id).unwrap().is_some());
    let s = studio::read_source(&st.dir).unwrap();
    assert_eq!(s.test_profile.as_deref(), Some(first.profile.id.as_str()));
    assert!(s.test_started.is_some());
    let second = studio::test(&dirs, &st.dir, None, &mut NoProgress).unwrap();
    let tests: Vec<String> = profile::list(&dirs).unwrap().into_iter().filter(|p| p.name.ends_with("(test)")).map(|p| p.id).collect();
    assert_eq!(tests, [second.profile.id.clone()], "the last test instance goes");
    assert!(profile::get(&dirs, &st.source.profile).is_ok(), "the creator's profile stays");

    // the game's log after the test: conflicts and problems
    let game = t.0.join("game");
    put(&game, "kiln/logs/kiln.log", b"[I 11:00:00.000] [kiln] Kiln 0.2.0 starting\n[E 11:00:01.000] [kiln] an older boot\n[I 12:00:00.000] [kiln] Kiln 0.2.0 starting\n[I 12:00:00.100] [kiln] content: 3 packs\n[W 12:00:00.200] [kiln] conflict: items Material_T1_Twigs.maxStackSize: alpha sets 600, pack.ember-run sets 1000 (the later one wins: pack.ember-run)\n[E 12:00:00.300] [mod:alpha] something broke\n");
    let r = studio::test_report(&st.dir, Some(&game)).unwrap();
    assert!(r.ran, "{r:?}");
    assert_eq!(r.conflicts.len(), 1);
    assert_eq!(r.problems.len(), 1);
    assert!(r.problems[0].contains("something broke"));

    studio::test_end(&dirs, &st.dir).unwrap();
    assert_eq!(profile::get(&dirs, &second.profile.id).unwrap_err().code, ErrorCode::NotFound);
    assert!(studio::read_source(&st.dir).unwrap().test_profile.is_none());
}
