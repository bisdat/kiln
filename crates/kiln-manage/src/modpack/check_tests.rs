// Tests of the pack analysis (check.rs).
use super::*;
use crate::hash::Record;
use serde_json::json;
use std::io::Write;

struct Mod {
    kind: &'static str,
    id: String,
    files: Vec<(String, Vec<u8>)>,
}

fn kiln(id: &str, extra: &[(&str, &[u8])], manifest: Value) -> Mod {
    let mut m = json!({"id": id, "name": id, "version": "1.0.0", "apiVersion": "0.2", "side": "both", "clients": "mod", "authors": ["Someone"]});
    if let (Some(o), Some(e)) = (m.as_object_mut(), manifest.as_object()) {
        o.extend(e.clone());
    }
    let mut files = vec![("kiln.mod.json".to_string(), serde_json::to_vec(&m).unwrap())];
    files.extend(extra.iter().map(|(n, b)| (n.to_string(), b.to_vec())));
    Mod { kind: "kiln", id: id.into(), files }
}

fn hash_of(m: &Mod) -> String {
    let recs = m.files.iter().map(|(p, b)| Record { path: p.clone(), size: b.len() as u64, sha256: crate::hash::sha256_hex(b) });
    if m.kind == "plugin" { crate::hash::sha256_hex(&m.files[0].1) } else { crate::hash::of_mod_records(ModKind::parse(m.kind).unwrap(), recs).unwrap() }
}

fn path_of(m: &Mod) -> String {
    if m.kind == "plugin" { format!("mods/plugin/{}", m.files[0].0) } else { format!("mods/{}/{}", m.kind, m.id) }
}

fn index(mods: &[&Mod], layer: Option<&Mod>, extra: Value) -> Value {
    let mut ix = json!({
        "format": 2, "kind": "pack", "id": "test-pack", "name": "Test pack", "version": "1.0.0", "game": {"builds": [1]},
        "mods": mods.iter().map(|m| json!({"kind": m.kind, "id": m.id, "version": "1.0.0", "path": path_of(m), "hash": hash_of(m), "dep": "required"})).collect::<Vec<_>>(),
        "order": {"kiln": mods.iter().filter(|m| m.kind == "kiln").map(|m| m.id.clone()).chain(layer.map(|l| l.id.clone())).collect::<Vec<_>>()},
    });
    if let Some(l) = layer {
        ix["layer"] = json!({"id": l.id, "version": "1.0.0", "path": "layer", "hash": hash_of(l)});
    }
    if let (Some(o), Some(e)) = (ix.as_object_mut(), extra.as_object()) {
        o.extend(e.clone());
    }
    ix
}

fn zip_of(path: &Path, entries: &[(String, Vec<u8>)]) {
    let mut w = zip::ZipWriter::new(std::fs::File::create(path).unwrap());
    let o = zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
    for (n, b) in entries {
        w.start_file(n.as_str(), o).unwrap();
        w.write_all(b).unwrap();
    }
    w.finish().unwrap();
}

fn whole(path: &Path, ix: &Value, mods: &[&Mod], layer: Option<&Mod>, more: &[(String, Vec<u8>)]) {
    let mut e = vec![("kiln-modpack.json".to_string(), serde_json::to_vec(ix).unwrap())];
    for m in mods {
        for (n, b) in &m.files {
            e.push((if m.kind == "plugin" { path_of(m) } else { format!("{}/{n}", path_of(m)) }, b.clone()));
        }
    }
    if let Some(l) = layer {
        e.extend(l.files.iter().map(|(n, b)| (format!("layer/{n}"), b.clone())));
    }
    e.extend(more.iter().cloned());
    zip_of(path, &e);
}

fn tmp(tag: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("kiln-check-{tag}-{}-{}", std::process::id(), crate::hash::hex(&crate::util::random_bytes::<4>().unwrap())));
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn opts(work: &Path) -> CheckOptions {
    let mut o = CheckOptions { work: Some(work.to_path_buf()), ..Default::default() };
    o.lint.no_catalog = true;
    o
}

fn codes(r: &CheckReport, level: Level) -> Vec<String> {
    r.findings.iter().filter(|f| f.level == level).map(|f| f.code.clone()).collect()
}

fn content(v: Value) -> Vec<u8> {
    serde_json::to_vec(&v).unwrap()
}

#[test]
fn a_good_pack_is_ok_with_its_conflicts_code_notes_and_credits() {
    let d = tmp("good");
    let a = kiln("big-stacks", &[("content/items.json", &content(json!({"format": 1, "items": {"Material_T1_Twigs": {"maxStackSize": 1000}}})))],
                 json!({"updates": {"provider": "github", "repository": "someone/big-stacks"}}));
    let b = kiln("kiln-auto-loot", &[("KilnAutoLoot.dll", &crate::pe::build_test_import_dll(&[("mscoree.dll", &["_CorDllMain"])], true))],
                 json!({"entryAssembly": "KilnAutoLoot.dll", "advanced": true}));
    let layer = kiln("pack.test-pack", &[("content/pack.json", &content(json!({"format": 1, "items": {"material_t1_twigs": {"maxStackSize": 600}}})))],
                     json!({"loadAfter": ["big-stacks", "kiln-auto-loot"]}));
    let ix = index(&[&a, &b], Some(&layer), json!({}));
    let p = d.join("p.kilnpack");
    whole(&p, &ix, &[&a, &b], Some(&layer), &[]);
    let mut o = opts(&d);
    o.own = Some(OwnMods { author: "frostdev".into(), mods: vec![OwnMod { kind: "kiln".into(), id: "kiln-auto-loot".into(), source: "https://github.com/frostdev-ops/kiln-auto-loot".into(), kdh1: vec![hash_of(&b)] }] });
    let r = check(Input::Pack(&p), &o).unwrap();
    assert!(r.ok, "{:#?}", r.findings);
    assert!(r.complete);
    assert_eq!(r.pack.as_ref().map(|p| p.id.as_str()), Some("test-pack"));
    assert_eq!(r.conflicts.len(), 1, "{:?}", r.conflicts);
    assert_eq!(r.conflicts[0].later.mod_id, "pack.test-pack");
    assert!(codes(&r, Level::Warning).contains(&"KL701".to_string()));
    let auto = r.mods.iter().find(|m| m.id == "kiln-auto-loot").unwrap();
    assert_eq!(auto.code.managed, ["KilnAutoLoot.dll"]);
    assert!(auto.capabilities.advanced);
    let credit = |id: &str| r.credits.iter().find(|c| c.id == id).unwrap().clone();
    assert_eq!((credit("kiln-auto-loot").match_kind.as_str(), credit("kiln-auto-loot").confidence.as_str()), ("kiln", "certain"));
    assert_eq!(credit("big-stacks").match_kind, "github");
    assert_eq!(credit("big-stacks").source.as_deref(), Some("https://github.com/someone/big-stacks"));
    assert_eq!(credit("big-stacks").author.as_deref(), Some("Someone"));
    // machine-readable: the shape Crucible stores
    let v = serde_json::to_value(&r).unwrap();
    assert_eq!(v["kind"], "kiln.pack-check");
    assert_eq!(v["credits"][0]["match"], "github");
    assert!(v["findings"].as_array().unwrap().iter().all(|f| f["level"].is_string() && f["code"].is_string()));
    std::fs::remove_dir_all(&d).ok();
}

#[test]
fn unsafe_paths_hash_mismatches_and_unlisted_entries_are_errors() {
    let d = tmp("unsafe");
    let a = kiln("a", &[], json!({}));
    let ix = index(&[&a], None, json!({}));
    for (tag, more) in [
        ("traversal", vec![("mods/kiln/a/../../../evil.txt".to_string(), b"x".to_vec())]),
        ("absolute", vec![("/etc/evil".to_string(), b"x".to_vec())]),
        ("unlisted", vec![("mods/kiln/b/x.txt".to_string(), b"x".to_vec())]),
        ("extra-in-mod", vec![("mods/kiln/a/extra.txt".to_string(), b"x".to_vec())]),
    ] {
        let p = d.join(format!("{tag}.kilnpack"));
        whole(&p, &ix, &[&a], None, &more);
        let r = check(Input::Pack(&p), &opts(&d)).unwrap();
        assert!(!r.ok, "{tag}: {:?}", r.findings);
        assert!(!r.complete, "{tag}");
        assert_eq!(r.credits.len(), 1, "{tag}: identification from the index alone still runs");
    }
    assert!(!d.join("evil.txt").exists() && !std::path::Path::new("/etc/evil").exists());
    std::fs::remove_dir_all(&d).ok();
}

#[test]
fn a_zip_bomb_and_too_many_entries_are_refused() {
    let d = tmp("bomb");
    let a = kiln("a", &[], json!({}));
    // one entry that unpacks past 512 MiB (a few hundred KB packed); the index lists it inside the mod
    let p = d.join("bomb.kilnpack");
    {
        let ix = index(&[&a], None, json!({}));
        let mut w = zip::ZipWriter::new(std::fs::File::create(&p).unwrap());
        let o = zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated).large_file(true);
        w.start_file("kiln-modpack.json", o).unwrap();
        w.write_all(&serde_json::to_vec(&ix).unwrap()).unwrap();
        w.start_file("mods/kiln/a/kiln.mod.json", o).unwrap();
        w.write_all(&a.files[0].1).unwrap();
        w.start_file("mods/kiln/a/zeros.bin", o).unwrap();
        let chunk = vec![0u8; 1 << 20];
        for _ in 0..513 {
            w.write_all(&chunk).unwrap();
        }
        w.finish().unwrap();
    }
    assert!(std::fs::metadata(&p).unwrap().len() < 4 << 20);
    let r = check(Input::Pack(&p), &opts(&d)).unwrap();
    assert!(!r.ok);
    assert!(r.findings.iter().any(|f| f.message.contains("512 MiB")), "{:?}", r.findings);
    std::fs::remove_file(&p).ok();
    // more entries than a pack may have: refused from the end record, before the zip's table is built
    let many = d.join("many.kilnpack");
    {
        let mut w = zip::ZipWriter::new(std::fs::File::create(&many).unwrap());
        let o = zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Stored);
        for i in 0..(MAX_ENTRIES + 1) {
            w.start_file(format!("mods/kiln/a/{i}"), o).unwrap();
        }
        w.finish().unwrap();
    }
    assert_eq!(claimed_entries(&many), Some(MAX_ENTRIES as u64 + 1));
    let r = check(Input::Pack(&many), &opts(&d)).unwrap();
    assert!(!r.ok);
    assert!(r.findings.iter().any(|f| f.code == "zip" && f.message.contains("entries")), "{:?}", r.findings);
    std::fs::remove_dir_all(&d).ok();
}

#[test]
fn blocked_hashes_secrets_and_missing_dependencies_are_errors() {
    let d = tmp("block");
    let a = kiln("a", &[], json!({"dependencies": [{"id": "not-in-pack", "version": ">=1.0.0"}]}));
    let secret = br#"{"webhook": "https://discord.com/api/webhooks/123456789012345678/abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJ"}"#.to_vec();
    let ix = index(&[&a], None, json!({"overrides": [{"path": "a.json", "mode": "merge-patch", "side": "both", "sha256": crate::hash::sha256_hex(&secret), "size": secret.len()}]}));
    let p = d.join("p.kilnpack");
    whole(&p, &ix, &[&a], None, &[("overrides/a.json".to_string(), secret)]);
    let mut o = opts(&d);
    o.blocked.insert(hash_of(&a).to_ascii_lowercase());
    let r = check(Input::Pack(&p), &o).unwrap();
    let errors = codes(&r, Level::Error);
    assert!(errors.contains(&"blocked".to_string()), "{:?}", r.findings);
    assert!(errors.contains(&"secret".to_string()), "{:?}", r.findings);
    assert!(errors.contains(&"resolve".to_string()), "{:?}", r.findings);
    assert!(r.mods[0].blocked);
    assert_eq!(CheckOptions::read_blocklist(&json!({"blocked": [{"sha256": "AB"}]})), BTreeSet::from(["ab".to_string()]));
    assert_eq!(CheckOptions::read_blocklist(&json!(["kdh1:X"])), BTreeSet::from(["kdh1:x".to_string()]));
    std::fs::remove_dir_all(&d).ok();
}

#[test]
fn native_code_that_touches_the_network_or_starts_programs_is_a_warning_not_an_error() {
    let d = tmp("native");
    let dll = crate::pe::build_test_import_dll(&[("WS2_32.dll", &["connect"]), ("KERNEL32.dll", &["CreateProcessW", "GetTickCount"]), ("ADVAPI32.dll", &["RegSetValueExW"])], false);
    let plugin = Mod { kind: "plugin", id: "spy".into(), files: vec![("spy.dll".into(), dll)] };
    let ix = index(&[&plugin], None, json!({}));
    let p = d.join("p.kilnpack");
    whole(&p, &ix, &[&plugin], None, &[]);
    let r = check(Input::Pack(&p), &opts(&d)).unwrap();
    assert!(r.ok, "{:?}", r.findings);
    let w = codes(&r, Level::Warning);
    for c in ["code-network", "code-process", "code-registry"] {
        assert!(w.contains(&c.to_string()), "{c}: {:?}", r.findings);
    }
    assert_eq!(r.mods[0].code.native, ["spy.dll"]);
    assert!(r.findings.iter().any(|f| f.message.contains("ws2_32.dll")));
    std::fs::remove_dir_all(&d).ok();
}

/// Crucible's stored form: each archive by its sha256, assembled and checked like a whole pack.
#[test]
fn the_stored_form_is_assembled_and_checked() {
    let d = tmp("stored");
    let files = d.join("files");
    std::fs::create_dir_all(&files).unwrap();
    let a = kiln("a", &[("content/x.json", &content(json!({"items": {"X": {"weight": 1}}})))], json!({}));
    let layer = kiln("pack.test-pack", &[("content/y.json", &content(json!({"items": {"X": {"weight": 2}}})))], json!({"loadAfter": ["a"]}));
    let store = |m: &Mod| -> Value {
        let tmpz = files.join("tmp.zip");
        zip_of(&tmpz, &m.files);
        let (sha, size) = crate::hash::sha256_file(&tmpz).unwrap();
        std::fs::rename(&tmpz, files.join(&sha)).unwrap();
        json!({"sha256": sha, "size": size})
    };
    let ov = br#"{"range": 5}"#.to_vec();
    let ov_sha = crate::hash::sha256_hex(&ov);
    std::fs::write(files.join(&ov_sha), &ov).unwrap();
    let mut ix = index(&[&a], Some(&layer), json!({"overrides": [{"path": "a.json", "mode": "merge-patch", "side": "both", "sha256": ov_sha, "size": ov.len()}]}));
    ix["mods"][0]["file"] = store(&a);
    ix["layer"]["file"] = store(&layer);
    let ixp = d.join("index.json");
    std::fs::write(&ixp, serde_json::to_vec(&ix).unwrap()).unwrap();
    let r = check(Input::Stored { index: &ixp, files: &files }, &opts(&d)).unwrap();
    assert!(r.ok && r.complete, "{:?}", r.findings);
    assert_eq!(r.conflicts.len(), 1);
    assert_eq!(r.pack.as_ref().unwrap().sha256, None);
    // a file whose bytes are not what the index says; a missing one; a sha256 that is a path
    let mut bad = ix.clone();
    bad["mods"][0]["file"]["sha256"] = json!(ov_sha);
    bad["mods"][0]["file"]["size"] = json!(ov.len());
    std::fs::write(&ixp, serde_json::to_vec(&bad).unwrap()).unwrap();
    let r = check(Input::Stored { index: &ixp, files: &files }, &opts(&d)).unwrap();
    assert!(!r.ok, "a valid file that is not the mod's archive: {:?}", r.findings);
    for (sha, code) in [("0".repeat(64), "missing-file"), ("../../../../etc/passwd".to_string(), "index")] {
        let mut bad = ix.clone();
        bad["mods"][0]["file"]["sha256"] = json!(sha);
        std::fs::write(&ixp, serde_json::to_vec(&bad).unwrap()).unwrap();
        let r = check(Input::Stored { index: &ixp, files: &files }, &opts(&d)).unwrap();
        assert!(codes(&r, Level::Error).contains(&code.to_string()), "{sha}: {:?}", r.findings);
    }
    // an archive with a traversal entry: refused by the same rules as a whole pack
    let evil = files.join("evil.zip");
    zip_of(&evil, &[("kiln.mod.json".into(), a.files[0].1.clone()), ("../../escape.txt".into(), b"x".to_vec())]);
    let (sha, size) = crate::hash::sha256_file(&evil).unwrap();
    std::fs::rename(&evil, files.join(&sha)).unwrap();
    let mut bad = ix.clone();
    bad["mods"][0]["file"] = json!({"sha256": sha, "size": size});
    std::fs::write(&ixp, serde_json::to_vec(&bad).unwrap()).unwrap();
    let r = check(Input::Stored { index: &ixp, files: &files }, &opts(&d)).unwrap();
    assert!(!r.ok, "{:?}", r.findings);
    assert!(!d.join("escape.txt").exists());
    std::fs::remove_dir_all(&d).ok();
}

#[test]
fn identification_order_and_flags() {
    let mk = |id: &str, hash: &str, credit: Option<Value>| -> PackMod {
        let mut v = json!({"kind": "kiln", "id": id, "version": "1.0.0", "path": format!("mods/kiln/{id}"), "hash": hash, "dep": "required"});
        if let Some(c) = credit { v["credit"] = c; }
        serde_json::from_value(v).unwrap()
    };
    let ix: PackIndex = serde_json::from_value(json!({"format": 2, "kind": "pack", "id": "p", "name": "P", "version": "1.0.0", "mods": []})).unwrap();
    let ix = PackIndex { mods: vec![
        mk("seen", "kdh1:aa", Some(json!({"author": "Other", "source": "https://example.org/x"}))),
        mk("twice", "kdh1:bb", None),
        mk("nexus", "kdh1:cc", None),
        mk("mine", "kdh1:dd", Some(json!({"author": "Me", "source": "https://www.nexusmods.com/enshrouded/mods/42"}))),
        mk("nothing", "kdh1:ee", None),
    ], ..ix };
    let mut reports: Vec<ModReport> = ix.mods.iter().map(|m| ModReport { kind: m.kind, id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone(), name: None,
        authors: vec![], url: None, license: None, updates: None, code: CodeScan::default(), capabilities: Capabilities::default(), blocked: false }).collect();
    reports[2].updates = Some(UpdatesSpec::Nexus { game_id: 5811, mod_id: 310, file_chain_id: "x".into() });
    reports[2].authors = vec!["NexusAuthor".into()];
    let o = CheckOptions { known: vec![
        KnownCredit { kdh1: "kdh1:aa".into(), author: "Author".into(), source: "https://github.com/a/seen".into() },
        KnownCredit { kdh1: "kdh1:bb".into(), author: "One".into(), source: "https://github.com/one/x".into() },
        KnownCredit { kdh1: "kdh1:bb".into(), author: "Two".into(), source: "https://github.com/two/x".into() },
    ], ..Default::default() };
    let c = identify(&ix, &reports, &o);
    assert_eq!((c[0].match_kind.as_str(), c[0].confidence.as_str()), ("crucible", "likely"));
    assert!(c[0].flag.as_deref().is_some_and(|f| f.contains("Other")), "the creator's credit differs: {:?}", c[0].flag);
    assert_eq!((c[1].match_kind.as_str(), c[1].confidence.as_str()), ("crucible", "conflict"));
    assert_eq!((c[2].match_kind.as_str(), c[2].nexus_mod_id, c[2].author.as_deref()), ("nexus", Some(310), Some("NexusAuthor")));
    assert_eq!((c[3].match_kind.as_str(), c[3].confidence.as_str(), c[3].nexus_mod_id), ("manual", "creator", Some(42)));
    assert_eq!((c[4].match_kind.as_str(), c[4].confidence.as_str(), c[4].author.clone()), ("manual", "none", None));
}
