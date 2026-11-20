//! Crucible instances: the player layer (local.<pid>) kept across updates with its data overlaps and "Drop my change",
//! undo bringing it back; renamed settings files carrying the player's values; a key new to the pack that the player
//! changed since the install; an override the pack stops shipping; a pack put together for game clients without the
//! server's mods; the save backup an update takes for a game the instance is active on. Temp folders only.
#![cfg(feature = "zip")]

mod fixture;

use kiln_manage::library::{self, LibraryRef};
use kiln_manage::modpack::layers;
use kiln_manage::modpack::merge::Pick;
use kiln_manage::modpack::pack::{self, BuildRequest, Env, InstallRequest, Need, OverrideSpec, PartSide, UpdateRequest};
use kiln_manage::modpack::ModpackSource;
use kiln_manage::paths::DataDirs;
use kiln_manage::profile::{self, ProfileMod};
use kiln_manage::task::NoProgress;
use kiln_manage::{ErrorCode, jsonc};
use serde_json::json;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use fixture::{put, scratch, Scratch};

fn kiln_mod(root: &Path, id: &str, version: &str) -> PathBuf {
    let d = root.join(format!("src/{id}-{version}"));
    put(&d, "kiln.mod.json", format!(r#"{{ "id": "{id}", "version": "{version}", "apiVersion": "0.2", "entryAssembly": "M.dll", "side": "both" }}"#).as_bytes());
    put(&d, "M.dll", format!("{id} {version}").as_bytes());
    d
}

fn import(dirs: &DataDirs, paths: &[PathBuf]) -> Vec<LibraryRef> {
    let r = library::import(dirs, paths, &mut NoProgress).unwrap();
    assert!(r.skipped.is_empty(), "{:?}", r.skipped);
    r.imported.iter().chain(&r.existing).cloned().collect()
}

fn pm(r: &LibraryRef) -> ProfileMod {
    ProfileMod { id: r.id.clone(), version: r.version.clone(), hash: r.hash.clone(), enabled: true }
}

fn json_of(path: &Path) -> serde_json::Value {
    jsonc::parse_tree(&std::fs::read_to_string(path).unwrap()).unwrap().to_value()
}

/// A creator with mods `ids`, a config folder and a layer folder; returns (creator data, profile id).
fn creator(t: &Scratch, ids: &[&str]) -> (DataDirs, String) {
    let dirs = DataDirs::at(t.0.join("creator"));
    let refs = import(&dirs, &ids.iter().map(|id| kiln_mod(&t.0, id, "1.0.0")).collect::<Vec<_>>());
    let mut p = profile::create(&dirs, "Ember run", None).unwrap();
    p.mods.kiln = refs.iter().map(pm).collect();
    profile::save(&dirs, &p).unwrap();
    put(&dirs.profile(&p.id).join("config"), "alpha.json", br#"{ "range": 20, "smash": false, "hotkey": "F9" }"#);
    put(&t.0, "layer/content/items.json", br#"{ "format": 1, "items": { "Logs": { "maxStackSize": 500, "rarity": "Rare" } } }"#);
    (dirs, p.id)
}

fn build(t: &Scratch, cd: &DataDirs, pid: &str, version: &str, overrides: Vec<OverrideSpec>) -> pack::BuiltPack {
    let req = BuildRequest { profile: pid.into(), version: version.into(), layer: Some(t.0.join("layer")), overrides, ..Default::default() };
    pack::build(cd, &req, &t.0.join(format!("out-{version}")), &mut NoProgress).unwrap()
}

fn alpha(keys: &[&str]) -> Vec<OverrideSpec> {
    vec![OverrideSpec { path: "alpha.json".into(), side: PartSide::Both, keys: keys.iter().map(|k| k.to_string()).collect() }]
}

fn install(dirs: &DataDirs, b: &pack::BuiltPack) -> String {
    let req = InstallRequest { source: ModpackSource::Path { path: b.file.clone(), sha256: Some(b.sha256.clone()) }, consent: true, allow_http: false,
        name: None, optional: BTreeMap::new(), activate: None };
    pack::install(dirs, None, &req, &mut NoProgress).unwrap().profile.id
}

fn up(ip: &str, b: &pack::BuiltPack) -> UpdateRequest {
    UpdateRequest { profile: ip.into(), source: ModpackSource::Path { path: b.file.clone(), sha256: Some(b.sha256.clone()) }, consent: true,
        allow_http: false, optional: BTreeMap::new(), resolve: BTreeMap::new(), all: None, drop: vec![] }
}

fn kiln_ids(dirs: &DataDirs, ip: &str) -> Vec<String> {
    profile::get(dirs, ip).unwrap().mods.kiln.iter().map(|m| m.id.clone()).collect()
}

fn local_entry(dirs: &DataDirs, ip: &str) -> ProfileMod {
    profile::get(dirs, ip).unwrap().mods.kiln.into_iter().find(|m| m.id == layers::local_id(ip)).expect("the player layer is in the profile")
}

#[test]
fn the_player_layer_stays_last_across_an_update_with_drop_my_change_and_undo() {
    let t = scratch("layers-local");
    let (cd, cpid) = creator(&t, &["alpha"]);
    let v1 = build(&t, &cd, &cpid, "1.0.0", alpha(&["range"]));
    let dirs = DataDirs::at(t.0.join("friend"));
    let ip = install(&dirs, &v1);

    // the player's own data: a stack size the pack sets too, and one the pack does not touch
    assert!(layers::files(&dirs, &ip).unwrap().is_empty());
    assert_eq!(layers::write(&dirs, &ip, "kiln.mod.json", Some("{}")).unwrap_err().code, ErrorCode::InvalidArgument);
    assert_eq!(layers::write(&dirs, &ip, "content/mine.json", Some("[1]")).unwrap_err().code, ErrorCode::Schema);
    layers::write(&dirs, &ip, "content/mine.json", Some(r#"{
  // mine
  "format": 1, "items": { "Logs": { "maxStackSize": 999 }, "Twigs": { "maxStackSize": 50 } } }"#)).unwrap();
    let p = layers::save(&dirs, &ip).unwrap();
    let ids: Vec<&str> = p.mods.kiln.iter().map(|m| m.id.as_str()).collect();
    assert_eq!(ids, ["alpha", "pack.ember-run", &*layers::local_id(&ip)], "the player layer loads last");
    let first = local_entry(&dirs, &ip);
    let lm = kiln_manage::manifest::read_kiln(&library::path_of(&dirs, &LibraryRef { kind: kiln_manage::manifest::ModKind::Kiln,
        id: first.id.clone(), version: first.version.clone(), hash: first.hash.clone() }).unwrap()).unwrap();
    assert!(lm.entry_assembly.is_empty());
    assert_eq!(lm.load_after, ["alpha", "pack.ember-run"]);

    // the creator: a new mod, and the layer's file renamed with Logs at 800
    import(&cd, &[kiln_mod(&t.0, "gamma", "1.0.0")]);
    let g = library::list(&cd, None).unwrap().into_iter().find(|e| e.id == "gamma").unwrap();
    let mut cp = profile::get(&cd, &cpid).unwrap();
    cp.mods.kiln.push(ProfileMod { id: "gamma".into(), version: "1.0.0".into(), hash: g.versions[0].hash.clone(), enabled: true });
    profile::save(&cd, &cp).unwrap();
    std::fs::remove_file(t.0.join("layer/content/items.json")).unwrap();
    put(&t.0, "layer/content/stacks.json", br#"{ "format": 1, "items": { "Logs": { "maxStackSize": 800, "rarity": "Rare" } } }"#);
    let v2 = build(&t, &cd, &cpid, "1.1.0", alpha(&["range"]));

    let plan = pack::plan_update(&dirs, &ip, &v2.file, None, &BTreeMap::new()).unwrap();
    assert!(plan.mods.yours.is_empty(), "the player layer is no 'mod of yours': {:?}", plan.mods.yours);
    let local = plan.local.as_ref().expect("the plan shows the player layer");
    assert_eq!((local.id.clone(), local.files), (layers::local_id(&ip), 1));
    assert_eq!(local.overlaps.len(), 1, "{:?}", local.overlaps);
    let o = &local.overlaps[0];
    assert_eq!((o.section.as_str(), o.target.as_str(), o.field.as_str()), ("items", "Logs", "maxStackSize"));
    assert_eq!((o.pack.clone(), o.base.clone(), o.mine.clone(), o.changed, o.file.as_str()), (json!(800), Some(json!(500)), json!(999), true, "content/mine.json"));

    // a field nobody overlaps cannot be dropped
    let mut bad = up(&ip, &v2);
    bad.drop = vec![r#"["items","Twigs","maxStackSize"]"#.into()];
    assert_eq!(pack::update(&dirs, &bad, &mut NoProgress).unwrap_err().code, ErrorCode::InvalidArgument);

    // update keeping the player's change: it stays last, after gamma, and its manifest names gamma now
    let r = pack::update(&dirs, &up(&ip, &v2), &mut NoProgress).unwrap();
    assert_eq!(kiln_ids(&dirs, &ip), ["alpha", "gamma", "pack.ember-run", &*layers::local_id(&ip)]);
    let second = local_entry(&dirs, &ip);
    assert_ne!(second.hash, first.hash, "the manifest names the new mods to load after");
    let lm = kiln_manage::manifest::read_kiln(&library::path_of(&dirs, &LibraryRef { kind: kiln_manage::manifest::ModKind::Kiln,
        id: second.id.clone(), version: second.version.clone(), hash: second.hash.clone() }).unwrap()).unwrap();
    assert!(lm.load_after.contains(&"gamma".to_string()), "{:?}", lm.load_after);
    assert!(r.backups.is_empty(), "no game: no save backup");
    assert!(json_of(&layers::local_dir(&dirs, &ip).join("content/mine.json"))["items"]["Logs"]["maxStackSize"] == 999);

    // undo: the profile's player layer entry as before
    pack::undo(&dirs, &ip).unwrap();
    assert_eq!(local_entry(&dirs, &ip).hash, first.hash);
    assert_eq!(kiln_ids(&dirs, &ip), ["alpha", "pack.ember-run", &*layers::local_id(&ip)]);

    // update again, letting go of the overlap: the pack's 800 counts, the player's other change stays
    let mut drop = up(&ip, &v2);
    drop.drop = vec![o.key.clone()];
    pack::update(&dirs, &drop, &mut NoProgress).unwrap();
    let mine = std::fs::read_to_string(layers::local_dir(&dirs, &ip).join("content/mine.json")).unwrap();
    assert!(mine.contains("// mine"), "the file keeps its comments: {mine}");
    let v = jsonc::parse(&mine).unwrap();
    assert_eq!(v["items"], json!({ "Twigs": { "maxStackSize": 50 } }));
    assert_ne!(local_entry(&dirs, &ip).hash, second.hash);
    assert!(pack::plan_update(&dirs, &ip, &v2.file, None, &BTreeMap::new()).unwrap().local.unwrap().overlaps.is_empty());

    // undo brings the dropped change back (working copy and Library entry)
    pack::undo(&dirs, &ip).unwrap();
    assert_eq!(json_of(&layers::local_dir(&dirs, &ip).join("content/mine.json"))["items"]["Logs"]["maxStackSize"], 999);
    assert_eq!(local_entry(&dirs, &ip).hash, first.hash);

    // a working copy that went missing comes back from the Library entry the profile names
    std::fs::remove_dir_all(layers::local_dir(&dirs, &ip)).unwrap();
    layers::save(&dirs, &ip).unwrap();
    assert_eq!(local_entry(&dirs, &ip).hash, first.hash);
    assert!(layers::local_dir(&dirs, &ip).join("content/mine.json").is_file());
    // a player layer emptied leaves the profile
    layers::write(&dirs, &ip, "content/mine.json", None).unwrap();
    layers::save(&dirs, &ip).unwrap();
    assert!(!kiln_ids(&dirs, &ip).contains(&layers::local_id(&ip)));
}

#[test]
fn a_renamed_settings_file_carries_the_players_values() {
    let t = scratch("layers-rename");
    let (cd, cpid) = creator(&t, &["alpha"]);
    let v1 = build(&t, &cd, &cpid, "1.0.0", alpha(&["range", "smash"]));
    let dirs = DataDirs::at(t.0.join("friend"));
    let ip = install(&dirs, &v1);
    let cfg = dirs.profile(&ip).join("config");
    put(&cfg, "alpha.json", br#"{ "range": 30, "smash": false, "own": "yes" }"#);

    // v2: the mod's settings moved to alpha/settings.json, same values
    let ccfg = cd.profile(&cpid).join("config");
    put(&ccfg, "alpha/settings.json", br#"{ "range": 20, "smash": false }"#);
    let moved = vec![OverrideSpec { path: "alpha/settings.json".into(), side: PartSide::Both, keys: vec!["range".into(), "smash".into()] }];
    let v2 = build(&t, &cd, &cpid, "1.1.0", moved.clone());
    let plan = pack::plan_update(&dirs, &ip, &v2.file, None, &BTreeMap::new()).unwrap();
    let f = plan.files.iter().find(|f| f.path == "alpha/settings.json").expect("the new file is in the plan");
    assert_eq!(f.renamed_from.as_deref(), Some("alpha.json"));
    assert!(f.conflicts.is_empty() && f.kept == ["range"], "{f:?}");
    assert!(!plan.files.iter().any(|f| f.path == "alpha.json"), "the old name is no file of its own: {:?}", plan.files);
    pack::update(&dirs, &up(&ip, &v2), &mut NoProgress).unwrap();
    assert_eq!(json_of(&cfg.join("alpha/settings.json")), json!({ "range": 30, "smash": false }), "the player's values of the pack's keys move along");
    assert_eq!(json_of(&cfg.join("alpha.json"))["range"], 30, "the old file stays as it was");

    // another player whose mod made the new file already, and a pack that also changed range: one question, the
    // player's answer lands in the new file
    let dirs2 = DataDirs::at(t.0.join("friend2"));
    let ip2 = install(&dirs2, &v1);
    let cfg2 = dirs2.profile(&ip2).join("config");
    put(&cfg2, "alpha.json", br#"{ "range": 30, "smash": false }"#);
    put(&cfg2, "alpha/settings.json", br#"{ "range": 1, "smash": true, "fromMod": 1 }"#);
    put(&ccfg, "alpha/settings.json", br#"{ "range": 25, "smash": false }"#);
    let v3 = build(&t, &cd, &cpid, "1.2.0", moved);
    let plan = pack::plan_update(&dirs2, &ip2, &v3.file, None, &BTreeMap::new()).unwrap();
    assert_eq!(plan.conflicts, 1);
    let mut u = up(&ip2, &v3);
    u.resolve.insert("alpha/settings.json#range".into(), Pick::Mine);
    pack::update(&dirs2, &u, &mut NoProgress).unwrap();
    assert_eq!(json_of(&cfg2.join("alpha/settings.json")), json!({ "range": 30, "smash": false, "fromMod": 1 }));
}

#[test]
fn a_key_new_to_the_pack_asks_when_the_player_changed_it_and_a_dropped_override_keeps_the_file() {
    let t = scratch("layers-since");
    let (cd, cpid) = creator(&t, &["alpha"]);
    let v1 = build(&t, &cd, &cpid, "1.0.0", alpha(&["range"]));
    let dirs = DataDirs::at(t.0.join("friend"));
    let ip = install(&dirs, &v1);
    let cfg = dirs.profile(&ip).join("config");
    // the mod writes its defaults at its first start
    put(&cfg, "alpha.json", br#"{ "range": 20, "hotkey": "F9" }"#);
    let ccfg = cd.profile(&cpid).join("config");
    put(&ccfg, "alpha.json", br#"{ "range": 21, "smash": false, "hotkey": "F9" }"#);
    let v2 = build(&t, &cd, &cpid, "1.1.0", alpha(&["range"]));
    pack::update(&dirs, &up(&ip, &v2), &mut NoProgress).unwrap();
    assert_eq!(json_of(&cfg.join("alpha.json")), json!({ "range": 21, "hotkey": "F9" }));

    // the player picks F5; the pack starts setting the hotkey (F10): a question, not an overwrite
    put(&cfg, "alpha.json", br#"{ "range": 21, "hotkey": "F5" }"#);
    put(&ccfg, "alpha.json", br#"{ "range": 21, "smash": false, "hotkey": "F10" }"#);
    let v3 = build(&t, &cd, &cpid, "1.2.0", alpha(&["range", "hotkey"]));
    let plan = pack::plan_update(&dirs, &ip, &v3.file, None, &BTreeMap::new()).unwrap();
    let f = plan.files.iter().find(|f| f.path == "alpha.json").unwrap();
    assert_eq!(f.conflicts.len(), 1, "{f:?}");
    assert_eq!((f.conflicts[0].key.as_str(), f.conflicts[0].base.clone(), f.conflicts[0].pack.clone(), f.conflicts[0].mine.clone()),
        ("hotkey", None, Some(json!("F10")), Some(json!("F5"))));
    // the mod's own default (F9, never written by Kiln) is asked about too: Kiln cannot tell it from a player's value
    put(&cfg, "alpha.json", br#"{ "range": 21, "hotkey": "F9" }"#);
    let plan = pack::plan_update(&dirs, &ip, &v3.file, None, &BTreeMap::new()).unwrap();
    assert_eq!(plan.files.iter().find(|f| f.path == "alpha.json").unwrap().conflicts.len(), 1);
    // a file without the key: simply taken
    put(&cfg, "alpha.json", br#"{ "range": 21 }"#);
    let plan = pack::plan_update(&dirs, &ip, &v3.file, None, &BTreeMap::new()).unwrap();
    let f = plan.files.iter().find(|f| f.path == "alpha.json").unwrap();
    assert!(f.conflicts.is_empty() && f.taken == ["hotkey"], "{f:?}");
    pack::update(&dirs, &up(&ip, &v3), &mut NoProgress).unwrap();
    // now Kiln wrote F10: the player's later change of a key the pack stops and starts managing again is theirs
    put(&cfg, "alpha.json", br#"{ "range": 21, "hotkey": "F10" }"#);
    let plan = pack::plan_update(&dirs, &ip, &v3.file, None, &BTreeMap::new()).unwrap();
    assert_eq!(plan.conflicts, 0, "as Kiln left it");

    // the pack stops shipping alpha.json: the player's file and values stay as they are
    put(&cfg, "alpha.json", br#"{ "range": 99, "hotkey": "F5" }"#);
    let v4 = build(&t, &cd, &cpid, "1.3.0", vec![]);
    let plan = pack::plan_update(&dirs, &ip, &v4.file, None, &BTreeMap::new()).unwrap();
    assert_eq!(plan.conflicts, 0);
    pack::update(&dirs, &up(&ip, &v4), &mut NoProgress).unwrap();
    assert_eq!(json_of(&cfg.join("alpha.json")), json!({ "range": 99, "hotkey": "F5" }));
}

/// `file` without the entries under `prefix`.
fn strip(file: &Path, to: &Path, prefix: &str) {
    use std::io::{Read, Write};
    let mut src = zip::ZipArchive::new(std::fs::File::open(file).unwrap()).unwrap();
    let mut w = zip::ZipWriter::new(std::fs::File::create(to).unwrap());
    for i in 0..src.len() {
        let mut e = src.by_index(i).unwrap();
        let name = e.name().unwrap().to_string();
        if name.starts_with(prefix) {
            continue;
        }
        let mut data = Vec::new();
        e.read_to_end(&mut data).unwrap();
        w.start_file(name.as_str(), zip::write::SimpleFileOptions::default()).unwrap();
        w.write_all(&data).unwrap();
    }
    w.finish().unwrap();
}

#[test]
fn a_pack_for_game_clients_leaves_the_server_only_mods_out() {
    let t = scratch("layers-client");
    let (cd, cpid) = creator(&t, &["alpha", "srv"]);
    let mut req = BuildRequest { profile: cpid.clone(), version: "1.0.0".into(), layer: Some(t.0.join("layer")), ..Default::default() };
    req.env.insert("srv".into(), Env { client: Need::Unsupported, server: Need::Required });
    let full = pack::build(&cd, &req, &t.0.join("out"), &mut NoProgress).unwrap();
    let client = t.0.join("client.kilnpack");
    strip(&full.file, &client, "mods/kiln/srv/");
    // a whole read refuses it, and so does a server's
    assert_eq!(pack::read(&client, None, None).unwrap_err().code, ErrorCode::Refused);
    assert!(pack::read_partial(&client, None, None, &pack::server_skips).is_err());
    let r = pack::read_partial(&client, None, None, &pack::client_skips).unwrap();
    assert_eq!(r.absent.iter().cloned().collect::<Vec<_>>(), ["srv"]);
    // what a client still lacks never names it
    let dirs = DataDirs::at(t.0.join("friend"));
    let ix = &full.index;
    let mut ix_files = ix.clone();
    for m in ix_files.mods.iter_mut() {
        m.file = Some(pack::StoredFile { sha256: "0".repeat(64), size: 1, url: None });
    }
    ix_files.layer.as_mut().unwrap().file = Some(pack::StoredFile { sha256: "0".repeat(64), size: 1, url: None });
    let lacking: Vec<String> = pack::lacking_for(&dirs, &ix_files, &pack::client_skips).unwrap().into_iter().map(|(id, _)| id).collect();
    assert_eq!(lacking, ["alpha", "pack.ember-run"]);
    // a client installs and updates from it
    let preview = pack::inspect(&dirs, &client, None).unwrap();
    assert!(!preview.missing.iter().any(|m| m.id == "srv"));
    let ip = install(&dirs, &pack::BuiltPack { file: client.clone(), sha256: kiln_manage::hash::sha256_file(&client).unwrap().0, size: 0, index: full.index.clone(), notes: vec![] });
    assert_eq!(kiln_ids(&dirs, &ip), ["alpha", "pack.ember-run"]);
    assert!(pack::plan_update(&dirs, &ip, &client, None, &BTreeMap::new()).is_ok());
}

#[test]
fn an_update_backs_up_the_saves_of_the_game_the_instance_is_active_on() {
    let f = fixture::Fixture::build("layers-saves");
    let t = scratch("layers-saves-src");
    let (cd, cpid) = creator(&t, &["alpha"]);
    let v1 = build(&t, &cd, &cpid, "1.0.0", alpha(&["range"]));
    let req = InstallRequest { source: ModpackSource::Path { path: v1.file.clone(), sha256: None }, consent: true, allow_http: false,
        name: None, optional: BTreeMap::new(), activate: None };
    let done = pack::install(&f.dirs, Some(&f.game_z), &req, &mut NoProgress).unwrap();
    assert!(done.backup.is_some() && done.activation.is_some());
    let ip = done.profile.id;
    put(&cd.profile(&cpid).join("config"), "alpha.json", br#"{ "range": 22 }"#);
    let v2 = build(&t, &cd, &cpid, "1.1.0", alpha(&["range"]));
    let before = kiln_manage::saves::backups(&f.dirs, &f.game_z).unwrap().len();
    let r = pack::update(&f.dirs, &up(&ip, &v2), &mut NoProgress).unwrap();
    assert_eq!(r.backups.len(), 1, "one game has the instance active");
    assert_eq!(kiln_manage::saves::backups(&f.dirs, &f.game_z).unwrap().len(), before + 1);
    pack::undo(&f.dirs, &ip).unwrap();
    assert_eq!(kiln_manage::saves::backups(&f.dirs, &f.game_z).unwrap().len(), before + 2, "undo backs up too");
}
