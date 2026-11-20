//! Crucible packs (format 2): build from a profile, verify, install as an instance, update with a three-way merge,
//! undo, uninstall, the server part, diff. On temp folders; nothing reaches a game or a server process.
#![cfg(feature = "zip")]

use kiln_manage::library::{self, LibraryRef};
use kiln_manage::modpack::merge::Pick;
use kiln_manage::modpack::pack::{self, BuildRequest, InstallRequest, OverrideSpec, PartSide, UpdateRequest};
use kiln_manage::modpack::{self, ModpackSource};
use kiln_manage::paths::DataDirs;
use kiln_manage::profile::{self, ProfileMod};
use kiln_manage::task::NoProgress;
use kiln_manage::{ErrorCode, jsonc};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-packs-{tag}-{}", std::process::id()));
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
    put(&d, "kiln.mod.json", format!(r#"{{ "id": "{id}", "version": "{version}", "apiVersion": "0.2", "entryAssembly": "M.dll", "side": "both" }}"#).as_bytes());
    put(&d, "M.dll", format!("{id} {version}").as_bytes());
    d
}

fn import(dirs: &DataDirs, paths: &[PathBuf]) -> Vec<LibraryRef> {
    let r = library::import(dirs, paths, &mut NoProgress).unwrap();
    assert!(r.skipped.is_empty(), "{:?}", r.skipped);
    r.imported.iter().chain(&r.existing).cloned().collect()
}

fn find(refs: &[LibraryRef], id: &str) -> LibraryRef {
    refs.iter().find(|r| r.id == id).cloned().unwrap_or_else(|| panic!("{id}: {refs:?}"))
}

fn pm(r: &LibraryRef, enabled: bool) -> ProfileMod {
    ProfileMod { id: r.id.clone(), version: r.version.clone(), hash: r.hash.clone(), enabled }
}

fn json_of(path: &Path) -> serde_json::Value {
    jsonc::parse_tree(&std::fs::read_to_string(path).unwrap()).unwrap().to_value()
}

/// The creator's profile: alpha (on), beta (switched off: optional, off), its config and a layer folder.
fn creator(dirs: &DataDirs, src: &Path) -> String {
    let refs = import(dirs, &[kiln_mod(src, "alpha", "1.0.0"), kiln_mod(src, "beta", "1.0.0")]);
    let mut p = profile::create(dirs, "Ember run", None).unwrap();
    p.mods.kiln = vec![pm(&find(&refs, "alpha"), true), pm(&find(&refs, "beta"), false)];
    profile::save(dirs, &p).unwrap();
    let cfg = dirs.profile(&p.id).join("config");
    put(&cfg, "alpha.json", br#"{ "range": 20, "smash": false, "hotkey": "F9" }"#);
    put(&cfg, "beta/settings.ini", b"[Loot]
chance = 5
");
    put(src, "layer/content/recipes.json", br#"{ "format": 1, "recipes": [] }"#);
    p.id
}

fn request(pid: &str, src: &Path, version: &str) -> BuildRequest {
    BuildRequest {
        profile: pid.into(),
        version: version.into(),
        layer: Some(src.join("layer")),
        overrides: vec![
            OverrideSpec { path: "alpha.json".into(), side: PartSide::Both, keys: vec!["range".into(), "smash".into()] },
            OverrideSpec { path: "beta/settings.ini".into(), side: PartSide::Client, keys: vec![] },
        ],
        server_settings: Some(serde_json::json!({ "gameSettings": { "playerHealthFactor": 1.5 } })),
        changes: Some("first".into()),
        ..Default::default()
    }
}

#[test]
fn a_pack_is_built_deterministically_from_a_profile_and_verifies() {
    let t = tmp("build");
    let dirs = DataDirs::at(t.0.join("data"));
    let pid = creator(&dirs, &t.0);
    let out = t.0.join("out");
    let a = pack::build(&dirs, &request(&pid, &t.0, "1.0.0"), &out, &mut NoProgress).unwrap();
    let first = std::fs::read(&a.file).unwrap();
    let b = pack::build(&dirs, &request(&pid, &t.0, "1.0.0"), &out, &mut NoProgress).unwrap();
    assert_eq!(a.sha256, b.sha256, "same input, same bytes");
    assert_eq!(first, std::fs::read(&b.file).unwrap());
    assert_eq!(a.file.file_name().unwrap(), "ember-run-1.0.0.kilnpack");
    let ix = pack::verify(&a.file, Some(&a.sha256)).unwrap();
    assert_eq!(ix, a.index);
    assert_eq!((ix.format, ix.kind.as_str(), ix.id.as_str()), (2, "pack", "ember-run"));
    // beta was switched off in the profile: optional, off by default
    let beta = ix.mods.iter().find(|m| m.id == "beta").unwrap();
    assert_eq!((beta.dep, beta.default), (pack::Dep::Optional, Some(false)));
    assert_eq!(ix.mods.iter().find(|m| m.id == "alpha").unwrap().dep, pack::Dep::Required);
    // the layer: data only, after every Kiln mod, last in the order
    let layer = ix.layer.as_ref().unwrap();
    assert_eq!((layer.id.as_str(), layer.version.as_str()), ("pack.ember-run", "1.0.0"));
    assert_eq!(ix.order.kiln, ["alpha", "beta", "pack.ember-run"]);
    // the override has only the chosen keys
    let rp = pack::read(&a.file, None, None).unwrap();
    let alpha = String::from_utf8(rp.overrides[&(PartSide::Both, "alpha.json".to_string())].clone()).unwrap();
    assert!(alpha.contains("range") && alpha.contains("smash") && !alpha.contains("hotkey"), "{alpha}");
    assert_eq!(rp.code.iter().cloned().collect::<Vec<_>>(), ["alpha", "beta"], "M.dll counts as code");
    // the layer manifest written by the build
    let st = t.0.join("unpacked");
    pack::read(&a.file, None, Some(&st)).unwrap();
    let lm = kiln_manage::manifest::read_kiln(&st.join("layer")).unwrap();
    assert!(lm.entry_assembly.is_empty());
    assert_eq!(lm.load_after, ["alpha", "beta"]);
    // a format-1 reader refuses a pack and says what it is
    let e = modpack::verify(&a.file, None).unwrap_err();
    assert_eq!(e.code, ErrorCode::Unsupported);
    assert!(e.message.contains("Crucible pack"), "{}", e.message);
    assert_eq!(pack::peek_format(&a.file).unwrap(), 2);
}

/// A copy of `file` with one more entry, or one entry's bytes changed.
fn rezip(file: &Path, to: &Path, change: Option<(&str, &[u8])>, add: Option<(&str, &[u8])>) {
    use std::io::{Read, Write};
    let mut src = zip::ZipArchive::new(std::fs::File::open(file).unwrap()).unwrap();
    let mut w = zip::ZipWriter::new(std::fs::File::create(to).unwrap());
    for i in 0..src.len() {
        let mut e = src.by_index(i).unwrap();
        let name = e.name().unwrap().to_string();
        let mut data = Vec::new();
        e.read_to_end(&mut data).unwrap();
        if let Some((n, b)) = change && n == name {
            data = b.to_vec();
        }
        w.start_file(name, zip::write::SimpleFileOptions::default()).unwrap();
        w.write_all(&data).unwrap();
    }
    if let Some((n, b)) = add {
        w.start_file(n, zip::write::SimpleFileOptions::default()).unwrap();
        w.write_all(b).unwrap();
    }
    w.finish().unwrap();
}

#[test]
fn verify_refuses_tampered_and_unlisted_entries_and_secrets_stay_out() {
    let t = tmp("tamper");
    let dirs = DataDirs::at(t.0.join("data"));
    let pid = creator(&dirs, &t.0);
    let built = pack::build(&dirs, &request(&pid, &t.0, "1.0.0"), &t.0.join("out"), &mut NoProgress).unwrap();
    assert_eq!(pack::verify(&built.file, Some(&"0".repeat(64))).unwrap_err().code, ErrorCode::HashMismatch);
    let bad = t.0.join("bad.kilnpack");
    rezip(&built.file, &bad, None, Some(("extra/run.dll", b"MZ")));
    assert_eq!(pack::verify(&bad, None).unwrap_err().code, ErrorCode::Refused);
    rezip(&built.file, &bad, Some(("mods/kiln/alpha/M.dll", b"changed")), None);
    assert_eq!(pack::verify(&bad, None).unwrap_err().code, ErrorCode::HashMismatch);
    rezip(&built.file, &bad, Some(("layer/content/recipes.json", b"{}")), None);
    assert_eq!(pack::verify(&bad, None).unwrap_err().code, ErrorCode::HashMismatch);
    rezip(&built.file, &bad, Some(("overrides/alpha.json", b"{}")), None);
    assert_eq!(pack::verify(&bad, None).unwrap_err().code, ErrorCode::HashMismatch);
    // a key that looks like a secret is refused, and so are files outside the allowlist
    put(&dirs.profile(&pid).join("config"), "alpha.json", br#"{ "range": 20, "apiToken": "abc123def" }"#);
    let mut req = request(&pid, &t.0, "1.0.0");
    req.overrides = vec![OverrideSpec { path: "alpha.json".into(), side: PartSide::Both, keys: vec![] }];
    let e = pack::build(&dirs, &req, &t.0.join("out"), &mut NoProgress).unwrap_err();
    assert_eq!(e.code, ErrorCode::Refused);
    assert!(e.message.contains("leave those keys out"), "{}", e.message);
    req.overrides[0].keys = vec!["range".into()];
    pack::build(&dirs, &req, &t.0.join("out"), &mut NoProgress).unwrap();
    for path in ["native.json", ".schema/alpha.json", "../x.json", "run.dll", "kiln.log"] {
        req.overrides = vec![OverrideSpec { path: path.into(), side: PartSide::Both, keys: vec![] }];
        assert_eq!(pack::build(&dirs, &req, &t.0.join("out"), &mut NoProgress).unwrap_err().code, ErrorCode::InvalidArgument, "{path}");
    }
    let found = pack::secret_findings("x.ini", "steam = 76561198000000001
maxTokens = 5
password = hunter2
");
    assert_eq!(found.len(), 2, "{found:?}");
}

#[test]
fn install_update_undo_uninstall_an_instance() {
    let t = tmp("instance");
    let creator_dirs = DataDirs::at(t.0.join("creator"));
    let pid = creator(&creator_dirs, &t.0);
    let out = t.0.join("out");
    let v1 = pack::build(&creator_dirs, &request(&pid, &t.0, "1.0.0"), &out, &mut NoProgress).unwrap();

    // a friend's launcher
    let dirs = DataDirs::at(t.0.join("friend"));
    let source = |b: &pack::BuiltPack| ModpackSource::Path { path: b.file.clone(), sha256: Some(b.sha256.clone()) };
    let mut req = InstallRequest { source: source(&v1), consent: false, allow_http: false, name: None, optional: BTreeMap::new(), activate: None };
    assert_eq!(pack::install(&dirs, None, &req, &mut NoProgress).unwrap_err().code, ErrorCode::ConsentRequired);
    req.consent = true;
    let preview = pack::inspect(&dirs, &v1.file, Some(&v1.sha256)).unwrap();
    assert_eq!(preview.missing.len(), 3, "alpha, beta and the layer");
    assert_eq!(preview.optional.get("beta"), Some(&false));
    let done = pack::install(&dirs, None, &req, &mut NoProgress).unwrap();
    let ip = done.profile.id.clone();
    let ids: Vec<(String, bool)> = done.profile.mods.kiln.iter().map(|m| (m.id.clone(), m.enabled)).collect();
    assert_eq!(ids, [("alpha".to_string(), true), ("beta".to_string(), false), ("pack.ember-run".to_string(), true)]);
    assert!(done.profile.dropins.is_empty() && done.profile.dropins_engines, "an instance loads no drop-in folders");
    let cfg = dirs.profile(&ip).join("config");
    assert_eq!(json_of(&cfg.join("alpha.json")), serde_json::json!({ "range": 20, "smash": false }));
    assert!(std::fs::read_to_string(cfg.join("beta/settings.ini")).unwrap().contains("chance = 5"));
    assert_eq!(pack::instance(&dirs, &ip).unwrap().unwrap().version, "1.0.0");
    assert_eq!(pack::instances(&dirs).unwrap().len(), 1);
    assert_eq!(pack::inspect(&dirs, &v1.file, None).unwrap().instance.as_deref(), Some(ip.as_str()));

    // the player: range 30 (their own change), a mod of their own, beta switched on
    put(&cfg, "alpha.json", br#"{
  // mine
  "range": 30, "smash": false }"#);
    let own = import(&dirs, &[kiln_mod(&t.0, "my-own", "1.0.0")]);
    let mut p = profile::get(&dirs, &ip).unwrap();
    p.mods.kiln.push(pm(&find(&own, "my-own"), true));
    p.mods.kiln.iter_mut().find(|m| m.id == "beta").unwrap().enabled = true;
    profile::save(&dirs, &p).unwrap();

    // the creator: range 25, smash true, a new mod gamma, a new layer file
    let g = import(&creator_dirs, &[kiln_mod(&t.0, "gamma", "2.0.0")]);
    let mut cp = profile::get(&creator_dirs, &pid).unwrap();
    cp.mods.kiln.push(pm(&find(&g, "gamma"), true));
    profile::save(&creator_dirs, &cp).unwrap();
    put(&creator_dirs.profile(&pid).join("config"), "alpha.json", br#"{ "range": 25, "smash": true }"#);
    put(&t.0, "layer/content/loot.json", br#"{ "format": 1 }"#);
    let v2 = pack::build(&creator_dirs, &request(&pid, &t.0, "1.1.0"), &out, &mut NoProgress).unwrap();
    let d = pack::diff(&v1.index, &v2.index);
    assert_eq!(d.added.iter().map(|m| m.id.as_str()).collect::<Vec<_>>(), ["gamma"]);
    assert!(d.layer_changed && d.overrides_changed == ["overrides/alpha.json"] && !d.server_settings_changed, "{d:?}");

    // the plan: gamma added, my-own kept, beta's switch kept, smash taken, range in conflict
    let plan = pack::plan_update(&dirs, &ip, &v2.file, None, &BTreeMap::new()).unwrap();
    assert_eq!(plan.mods.added.iter().map(|m| m.id.as_str()).collect::<Vec<_>>(), ["gamma"]);
    assert_eq!(plan.mods.yours.iter().map(|m| m.id.as_str()).collect::<Vec<_>>(), ["my-own"]);
    assert_eq!(plan.mods.kept_switches.len(), 1, "{:?}", plan.mods.kept_switches);
    assert!(plan.layer.changed);
    assert_eq!(plan.conflicts, 1);
    let f = plan.files.iter().find(|f| f.path == "alpha.json").unwrap();
    assert_eq!((f.taken.clone(), f.conflicts[0].key.as_str()), (vec!["smash".to_string()], "range"));
    assert_eq!(f.conflicts[0].mine, Some(serde_json::json!(30)));

    let mut up = UpdateRequest { profile: ip.clone(), source: source(&v2), consent: true, allow_http: false, optional: BTreeMap::new(),
        resolve: BTreeMap::new(), all: None };
    let e = pack::update(&dirs, &up, &mut NoProgress).unwrap_err();
    assert_eq!(e.code, ErrorCode::Conflict);
    assert!(e.message.contains("alpha.json#range"), "{}", e.message);
    assert_eq!(pack::instance(&dirs, &ip).unwrap().unwrap().version, "1.0.0", "nothing changed");
    up.resolve.insert("alpha.json#range".into(), Pick::Mine);
    let r = pack::update(&dirs, &up, &mut NoProgress).unwrap();
    let ids: Vec<(String, bool)> = r.profile.mods.kiln.iter().map(|m| (m.id.clone(), m.enabled)).collect();
    assert_eq!(ids, [("alpha".to_string(), true), ("beta".to_string(), true), ("gamma".to_string(), true), ("pack.ember-run".to_string(), true),
        ("my-own".to_string(), true)]);
    assert_eq!(r.profile.mods.kiln.iter().find(|m| m.id == "pack.ember-run").unwrap().version, "1.1.0");
    let text = std::fs::read_to_string(cfg.join("alpha.json")).unwrap();
    assert!(text.contains("// mine"), "comments stay: {text}");
    assert_eq!(json_of(&cfg.join("alpha.json")), serde_json::json!({ "range": 30, "smash": true }));
    let inst = pack::instance(&dirs, &ip).unwrap().unwrap();
    assert_eq!((inst.version.as_str(), inst.previous.as_deref()), ("1.1.0", Some("1.0.0")));

    // undo: the instance as it was before the update
    let back = pack::undo(&dirs, &ip).unwrap();
    assert!(!back.mods.kiln.iter().any(|m| m.id == "gamma"));
    assert_eq!(json_of(&cfg.join("alpha.json")), serde_json::json!({ "range": 30, "smash": false }));
    assert_eq!(pack::instance(&dirs, &ip).unwrap().unwrap().version, "1.0.0");
    assert_eq!(pack::undo(&dirs, &ip).unwrap_err().code, ErrorCode::NotFound);

    // another pack's file is refused as an update
    let mut other = request(&pid, &t.0, "1.0.0");
    other.id = Some("other-pack".into());
    std::fs::remove_file(t.0.join("layer/content/loot.json")).unwrap();
    let o = pack::build(&creator_dirs, &other, &t.0.join("out2"), &mut NoProgress).unwrap();
    assert_eq!(pack::plan_update(&dirs, &ip, &o.file, None, &BTreeMap::new()).unwrap_err().code, ErrorCode::Refused);

    // uninstall: the profile goes, the Library keeps the mods and names the ones nothing uses
    let u = pack::uninstall(&dirs, &ip).unwrap();
    assert!(profile::get(&dirs, &ip).is_err());
    let unused: Vec<&str> = u.unused.iter().map(|r| r.id.as_str()).collect();
    assert!(unused.contains(&"alpha") && unused.contains(&"pack.ember-run"), "{unused:?}");
    assert!(!unused.contains(&"my-own"), "not the pack's");
}

#[test]
fn the_server_part_goes_into_a_server_folder_and_builds_the_player_modpack() {
    let t = tmp("server");
    let dirs = DataDirs::at(t.0.join("data"));
    let pid = creator(&dirs, &t.0);
    let mut req = request(&pid, &t.0, "1.0.0");
    req.optional.insert("beta".into(), true);
    req.overrides[1].side = PartSide::Server;
    let built = pack::build(&dirs, &req, &t.0.join("out"), &mut NoProgress).unwrap();
    let server = t.0.join("server");
    put(&server, "enshrouded_server.json", br#"{ "name": "Isles", "queryPort": 15637, "gameSettings": { "playerHealthFactor": 1 } }"#);
    std::fs::create_dir_all(server.join("kiln/mods")).unwrap();
    let inst = kiln_manage::server::instance::steam_instance("pack-test", &server, None);
    let sreq = pack::ServerInstallRequest {
        source: ModpackSource::Path { path: built.file.clone(), sha256: None }, consent: true, allow_http: false,
        optional: BTreeMap::new(), settings: true, modpack_out: Some(t.0.join("players")),
    };
    let r = pack::install_server(&dirs, &inst, &sreq, &mut NoProgress).unwrap();
    for id in ["alpha", "beta", "pack.ember-run"] {
        assert!(server.join("kiln/mods").join(id).join("kiln.mod.json").exists(), "{id}");
    }
    assert_eq!(r.overrides, ["alpha.json", "beta/settings.ini"]);
    assert_eq!(json_of(&server.join("kiln/config/alpha.json")), serde_json::json!({ "range": 20, "smash": false }));
    assert!(r.settings);
    let s = json_of(&server.join("enshrouded_server.json"));
    assert_eq!(s["gameSettings"]["playerHealthFactor"], serde_json::json!(1.5));
    // the player modpack (format 1) carries what the server runs, the layer included
    let mp = r.modpack.unwrap();
    let ids: Vec<&str> = mp.manifest.mods.iter().map(|m| m.id.as_str()).collect();
    assert_eq!(ids, ["alpha", "beta", "pack.ember-run"]);
    modpack::verify(&mp.file, Some(&mp.sha256)).unwrap();
}

#[test]
fn overrides_carry_their_size() {
    let t = tmp("ovsize");
    let dirs = DataDirs::at(t.0.join("data"));
    let pid = creator(&dirs, &t.0);
    let built = pack::build(&dirs, &request(&pid, &t.0, "1.0.0"), &t.0.join("out"), &mut NoProgress).unwrap();
    let rp = pack::read(&built.file, None, None).unwrap();
    for o in &built.index.overrides {
        assert_eq!(o.size, Some(rp.overrides[&(o.side, o.path.clone())].len() as u64), "{}", o.path);
    }
    // an index whose size does not match the file is refused
    let mut ix = built.index.clone();
    ix.overrides[0].size = Some(1);
    let bad = t.0.join("bad.kilnpack");
    rezip(&built.file, &bad, Some(("kiln-modpack.json", &serde_json::to_vec_pretty(&ix).unwrap())), None);
    assert_eq!(pack::verify(&bad, None).unwrap_err().code, ErrorCode::HashMismatch);
}

#[test]
fn server_settings_carry_game_settings_only() {
    let t = tmp("srvset");
    let dirs = DataDirs::at(t.0.join("data"));
    let pid = creator(&dirs, &t.0);
    // a server's admin password, its name and bans never travel in a pack: an install would set them on someone's server
    let mut req = request(&pid, &t.0, "1.0.0");
    req.server_settings = Some(serde_json::json!({ "gameSettings": { "playerHealthFactor": 1.5 },
        "userGroups": [{ "name": "Admin", "password": "hunter2", "canKickBan": true }] }));
    let e = pack::build(&dirs, &req, &t.0.join("out"), &mut NoProgress).unwrap_err();
    assert_eq!(e.code, ErrorCode::Schema, "{}", e.message);
    assert!(e.message.contains("gameSettings"), "{}", e.message);
    let mut ix = pack::build(&dirs, &request(&pid, &t.0, "1.0.0"), &t.0.join("out"), &mut NoProgress).unwrap().index;
    ix.server = Some(pack::PackServer { settings: Some(serde_json::json!({ "gameSettings": {}, "name": "Mine now" })), notes: None });
    assert_eq!(pack::check_index(&ix).unwrap_err().code, ErrorCode::Schema);
    ix.server = Some(pack::PackServer { settings: Some(serde_json::json!({ "gameSettings": 3 })), notes: None });
    assert_eq!(pack::check_index(&ix).unwrap_err().code, ErrorCode::Schema);
}
