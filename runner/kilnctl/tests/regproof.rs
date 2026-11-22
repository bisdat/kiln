//! `kilnctl regproof` end to end on synthetic files (invented types, ids and values; no game data, no Python): the
//! inputs given as files, the lab's outputs found in a Kiln data folder by content, the record written verified only
//! when every step is green, nothing written into the Kiln tree.
use serde_json::{Value, json};
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

const NEW: &str = "1111111111111111111111111111111111111111111111111111111111111111";
const OLD: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

fn scratch(name: &str) -> PathBuf {
    let t = std::env::temp_dir().join(format!("kilnctl-regproof-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&t);
    std::fs::create_dir_all(&t).unwrap();
    t
}

fn put(p: &Path, v: &Value) {
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, serde_json::to_string_pretty(v).unwrap()).unwrap();
}

fn types(sha: &str) -> Value {
    let prim = |n: &str, k: u32, s: u32| json!({"name": n, "kind": k, "size": s, "layout": 0, "inner": null});
    json!({"exe_sha256": sha, "types": [
        prim("demo::bool", 1, 1), prim("demo::u16", 4, 2), prim("demo::u32", 6, 4), prim("demo::f32", 10, 4), prim("demo::Guid", 29, 16),
        {"name": "demo::Gadget", "kind": 18, "size": 32, "layout": 7, "inner": null, "fields": [
            {"name": "stackLimit", "type": "demo::u16", "offset": 0}, {"name": "limit", "type": "demo::f32", "offset": 4},
            {"name": "isSet", "type": "demo::bool", "offset": 8}, {"name": "uid", "type": "demo::Guid", "offset": 12},
            {"name": "num", "type": "demo::u32", "offset": 28}]}]})
}

fn table(sha: &str) -> Value {
    json!({"format": 1, "side": "server", "exeSha256": sha, "registries": {"gadgets": {"g1": "game:a", "g2": "game:b"}}})
}

/// The regmapDigest contract (regmap/README.md): sha256 over "<file name>\n<sha256 of its bytes>\n" per regmap/common file.
fn digest(tree: &Path) -> String {
    let mut files: Vec<PathBuf> = std::fs::read_dir(tree.join("regmap/common")).unwrap().flatten().map(|e| e.path()).collect();
    files.sort();
    let mut text = String::new();
    for f in files {
        text.push_str(&format!("{}\n{}\n", f.file_name().unwrap().to_string_lossy(), kiln_manage::hash::sha256_hex(&std::fs::read(&f).unwrap())));
    }
    kiln_manage::hash::sha256_hex(text.as_bytes())
}

fn values(sha: &str, size: u32, digest: &str) -> Value {
    json!({"format": 1, "kind": "registry-values", "side": "server", "exeSha256": sha, "gameBuild": "100", "regmapDigest": digest,
           "base": {"origin": "live", "writer": "none"}, "registries": {"gadgets": {"game:a": {"size": size, "cap": 1.5, "label": true}}}})
}

fn sample(field: &str, extra: Value) -> Value {
    let b = "ab".repeat(32);
    let mut s = json!({"field": field, "entry": "game:a", "reverted": true, "bytesBefore": b, "bytesAfter": b});
    for (k, v) in extra.as_object().unwrap() {
        s[k] = v.clone();
    }
    s
}

/// A Kiln tree (regmap only), the inputs and a lab's data folder with every output.
fn setup(t: &Path) -> (PathBuf, PathBuf) {
    let tree = t.join("kiln");
    std::fs::create_dir_all(tree.join(".git")).unwrap();
    put(&tree.join("regmap/common/gadgets.json"), &json!({"regmap": 1, "registry": "gadgets",
        "entries": {"kind": "resource", "resourceType": "demo::Gadget", "guid": "uid", "id": "num"},
        "rows": {"size": {"paths": ["stackLimit"], "conv": "int", "read": "now", "backend": "live"},
                 "cap": {"paths": ["limit"], "conv": "float", "read": "restart", "backend": "boot"},
                 "label": {"paths": ["isSet"], "conv": "bool", "read": "now", "backend": "none"}}}));
    let inp = t.join("inputs");
    put(&inp.join("refl-new.json"), &types(NEW));
    put(&inp.join("refl-old.json"), &types(OLD));
    put(&inp.join("record.json"), &json!({"regmap": 1, "side": "server", "exeSha256": NEW, "gameBuild": "101", "steamBuild": "2",
        "verified": true, "proof": "regmap_check.py --exe", "layouts": {"demo::Gadget": "0x00000007"},
        "rows": {"gadgets.size": "ok", "gadgets.cap": "ok", "gadgets.label": "ok"}, "aliases": {"renames": [], "removals": [], "suffixes": []}}));
    put(&inp.join("ids-new.json"), &json!({"gadgets": {"g1": "game:a", "g2": "game:b"}}));
    // the lab's Kiln data folder: kiln.content's id tables, the proof producer's suites and values export
    let data = t.join("gate/kiln/data");
    put(&data.join(format!("content/registry/ids-server-{NEW}.json")), &table(NEW));
    put(&data.join(format!("content/registry/ids-server-{OLD}.json")), &table(OLD));
    let d = digest(&tree);
    let suite = |generation: &str, samples: Value| json!({"format": 1, "kind": "registry-roundtrip", "side": "server", "exeSha256": NEW,
                                                          "generation": generation, "regmapDigest": d, "samples": samples});
    put(&data.join("content/registry/proof/live.json"), &suite("live", json!([
        sample("gadgets.size", json!({"read": 20, "probe": 21, "readBack": 21, "rawBefore": [20], "rawAfter": [21]})),
        sample("gadgets.label", json!({"read": true, "rawBefore": [1]}))])));
    put(&data.join("content/registry/proof/boot.json"), &suite("boot", json!([
        sample("gadgets.cap", json!({"read": 1.5, "probe": 2.5, "readBack": 2.5, "rawBefore": [1.5], "rawAfter": [2.5]}))])));
    put(&data.join("content/registry/proof/values-new.json"), &values(NEW, 40, &d));
    put(&inp.join("values-old.json"), &values(OLD, 20, &d));
    (tree, data)
}

fn kilnctl(t: &Path, args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_kilnctl"))
        .env("HOME", t.join("home"))
        .env("XDG_STATE_HOME", t.join("xdg-state"))
        .env("XDG_CONFIG_HOME", t.join("xdg-config"))
        .env("XDG_DATA_HOME", t.join("xdg-data"))
        .env("KILN_DATA_DIR", t.join("data-root"))
        .env("KILN_REGMAP_IDS", t.join("store"))
        .args(args)
        .output()
        .unwrap()
}

fn text(o: &Output) -> String {
    format!("{}{}", String::from_utf8_lossy(&o.stdout), String::from_utf8_lossy(&o.stderr))
}

fn args<'a>(t: &'a Path, tree: &'a Path, data: &'a Path, out: &'a Path) -> Vec<String> {
    let i = t.join("inputs");
    let s = |p: PathBuf| p.to_string_lossy().into_owned();
    vec!["regproof".into(), "--tree".into(), s(tree.to_path_buf()), "--new-refl".into(), s(i.join("refl-new.json")),
         "--old-refl".into(), s(i.join("refl-old.json")), "--record".into(), s(i.join("record.json")), "--ids".into(), s(i.join("ids-new.json")),
         "--evidence".into(), s(data.to_path_buf()), "--evidence".into(), s(i.join("values-old.json")), "--out".into(), s(out.to_path_buf())]
}

#[test]
fn a_complete_proof_writes_a_verified_record_outside_the_tree() {
    let t = scratch("green");
    let (tree, data) = setup(&t);
    let out = t.join("out");
    let a = args(&t, &tree, &data, &out);
    let o = kilnctl(&t, &a.iter().map(String::as_str).collect::<Vec<_>>());
    assert!(o.status.success(), "{}", text(&o));
    assert!(text(&o).contains("VERIFIED") || text(&o).contains("verified"), "{}", text(&o));
    let rec: Value = serde_json::from_str(&std::fs::read_to_string(out.join(format!("regmap-server-{NEW}.json"))).unwrap()).unwrap();
    assert_eq!(rec["verified"], true);
    assert_eq!(rec["rows"], json!({"gadgets.cap": "ok", "gadgets.label": "ok", "gadgets.size": "ok"}));
    assert!(rec["proof"].as_str().unwrap().contains("kilnctl regproof"), "{rec}");
    assert!(std::fs::read_to_string(out.join("values-changelog.txt")).unwrap().contains("game:a  size 20 -> 40"));
    assert!(std::fs::read_to_string(out.join("NEXT.txt")).unwrap().starts_with("nothing left to do"));
    // the new build's table and values are kept for the next update; the lab's tables were harvested
    assert!(t.join(format!("store/server/ids-server-{NEW}.json")).is_file());
    assert!(t.join(format!("store/server/ids-server-{OLD}.json")).is_file());
    assert!(t.join(format!("store/server/values-server-{NEW}.json")).is_file());
    // nothing was written into the Kiln tree
    let mut files: Vec<String> = Vec::new();
    let mut todo = vec![tree.clone()];
    while let Some(d) = todo.pop() {
        for e in std::fs::read_dir(&d).unwrap().flatten() {
            if e.file_type().unwrap().is_dir() { todo.push(e.path()) } else { files.push(e.path().to_string_lossy().into_owned()) }
        }
    }
    assert_eq!(files, [tree.join("regmap/common/gadgets.json").to_string_lossy().into_owned()]);
    let _ = std::fs::remove_dir_all(&t);
}

#[test]
fn a_missing_boot_generation_leaves_the_record_unverified() {
    let t = scratch("red");
    let (tree, data) = setup(&t);
    std::fs::remove_file(data.join("content/registry/proof/boot.json")).unwrap();
    let out = t.join("out");
    let a = args(&t, &tree, &data, &out);
    let o = kilnctl(&t, &a.iter().map(String::as_str).collect::<Vec<_>>());
    assert_eq!(o.status.code(), Some(1), "{}", text(&o));
    let rec: Value = serde_json::from_str(&std::fs::read_to_string(out.join(format!("regmap-server-{NEW}.json"))).unwrap()).unwrap();
    assert_eq!(rec["verified"], false);
    // no sample in a boot generation: unproven (kept off, with its reason), not "changed"
    assert_eq!(rec["rows"]["gadgets.cap"], "unproven");
    assert!(rec["reasons"]["gadgets.cap"].as_str().is_some_and(|r| r.contains("lab round trip")), "{rec}");
    assert_eq!(rec["rows"]["gadgets.size"], "ok");
    let next = std::fs::read_to_string(out.join("NEXT.txt")).unwrap();
    assert!(next.contains("boot generation"), "{next}");
    // --mark needs an instance (the record goes into its Kiln folder)
    let mut m = a.clone();
    m.push("--mark".into());
    assert!(!kilnctl(&t, &m.iter().map(String::as_str).collect::<Vec<_>>()).status.success());
    let _ = std::fs::remove_dir_all(&t);
}

#[test]
fn outputs_inside_the_kiln_tree_are_refused() {
    let t = scratch("guard");
    let (tree, data) = setup(&t);
    let out = tree.join("out");
    let a = args(&t, &tree, &data, &out);
    let o = kilnctl(&t, &a.iter().map(String::as_str).collect::<Vec<_>>());
    assert!(!o.status.success());
    assert!(text(&o).contains("never go into the repository"), "{}", text(&o));
    assert!(!out.exists());
    let _ = std::fs::remove_dir_all(&t);
}

/// The maintainers' guide names the command, the maintainer folder, the statuses and both lab file contracts.
#[test]
fn the_docs_describe_regproof_and_its_contracts() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let read = |p: &str| std::fs::read_to_string(root.join(p)).unwrap();
    let check = |file: &str, words: &[&str]| {
        let text = read(file);
        for w in words {
            assert!(text.contains(w), "{file} does not mention {w:?}");
        }
    };
    check("docs/kiln/kilnctl.md", &["`regproof", "KILN_REGMAP_IDS", "registry-roundtrip", "registry-values", "--accept", "--first-build",
                                    "regmap-ids", "regproof/<name>"]);
    check("docs/kiln/sigport.md", &["kilnctl regproof", "registry proof", "unproven", "boot generation", "content/registry/proof"]);
    check("regmap/README.md", &["kilnctl regproof", "KILN_REGMAP_IDS", "registry-roundtrip", "registry-values", "unproven", "\"reasons\"",
                                "rawBefore", "bytesBefore", "\"base\"", "content/registry/proof"]);
}
