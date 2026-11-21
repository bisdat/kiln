// SPDX-License-Identifier: MIT
//! Source-derived fixtures, not oracle observations. Both pins: modloader/
//! find_installed_mods.rs:10-159; plan_mod_load_order.rs:150-335;
//! read_mod_manifest.rs:15-126; mod_profiles.rs:34-174; calculate_*revision.rs;
//! calculate_preparation_fingerprint.rs:5-63. No game data or DLL execution.
#![allow(dead_code)]
use serde_json::{Value, json};
use sfi_package::{
    package::{
        Package as ParsedPackage, ParseOptions, Target, discover::*, plan::*, profiles::*,
        revisions::*,
    },
    source::{DiskSource, Package as RuntimePackage, PackageSource, SourceVersion},
};
use std::{
    fs,
    path::PathBuf,
    rc::Rc,
    sync::atomic::{AtomicU64, Ordering},
    time::{Duration, UNIX_EPOCH},
};
static SEQ: AtomicU64 = AtomicU64::new(0);
pub struct Tree(pub PathBuf);
impl Tree {
    pub fn new() -> Self {
        let p = std::env::temp_dir().join(format!(
            "c3-{}-{}",
            std::process::id(),
            SEQ.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(p.join("mods")).unwrap();
        fs::create_dir_all(p.join("profiles")).unwrap();
        Self(p)
    }
    pub fn package(&self, directory: &str, id: &str) {
        let p = self.0.join("mods").join(directory);
        fs::create_dir_all(&p).unwrap();
        fs::write(
            p.join("mod.json"),
            json!({"id":id,"name":id,"version":"1.0.0","capabilities":["runtime"]}).to_string(),
        )
        .unwrap();
        fs::write(
            p.join("extended.mod.json"),
            br#"{"schemaVersion":1,"enabled":true,"targets":["client","server"]}"#,
        )
        .unwrap();
    }
    pub fn roots(&self) -> PackageRoots {
        PackageRoots {
            mods: self.0.join("mods"),
            config: self.0.join("config"),
            profiles: self.0.join("profiles"),
        }
    }
}
impl Drop for Tree {
    fn drop(&mut self) {
        fs::remove_dir_all(&self.0).unwrap();
    }
}
fn parsed(id: &str, enabled: bool, caps: &[&str], deps: Value) -> ParsedPackage {
    ParsedPackage::parse(
        json!({"id":id,"name":id,"version":"1.0.0","capabilities":caps,"dependencies":deps})
            .to_string()
            .as_bytes(),
        Some(
            json!({"schemaVersion":1,"enabled":enabled,"targets":["client","server"]})
                .to_string()
                .as_bytes(),
        ),
        ParseOptions::default(),
    )
    .unwrap()
}
fn registry(packages: Vec<ParsedPackage>) -> Registry {
    let mut r = Registry::default();
    struct Empty;
    impl PackageSource for Empty {
        fn read_file(&self, _: &str) -> std::io::Result<Vec<u8>> {
            Err(std::io::ErrorKind::NotFound.into())
        }
    }
    for p in packages {
        let runtime = RuntimePackage::new(p.manifest.id.clone(), Rc::new(Empty));
        r.insert(
            PackageRecord::new(
                format!("folder-{}", p.manifest.id),
                p,
                runtime.clone(),
                runtime.source.clone(),
            )
            .unwrap(),
        )
        .unwrap();
    }
    r
}
fn dep(id: &str, version: &str, optional: bool) -> Value {
    json!({"id":id,"version":version,"optional":optional})
}
fn ids(plan: &Plan) -> Vec<&str> {
    plan.ids.iter().map(String::as_str).collect()
}

pub fn discovery_isolation_and_identity() {
    let t = Tree::new();
    t.package("different-folder", "a");
    t.package(".hidden", "hidden");
    t.package("dup1", "duplicate");
    t.package("dup2", "duplicate");
    t.package("dup3", "duplicate");
    t.package("bad", "bad");
    fs::write(t.0.join("mods/bad/mod.json"), b"{").unwrap();
    fs::create_dir(t.0.join("mods/no-manifest")).unwrap();
    fs::write(t.0.join("mods/archive.zip"), b"not a package directory").unwrap();
    let report = discover(&t.roots(), ParseOptions::default(), &[]).unwrap();
    assert_eq!(
        report.registry.snapshot().ids().collect::<Vec<_>>(),
        vec!["a"]
    );
    assert_eq!(report.errors.len(), 3); // one malformed + two duplicate reports
    let p = report.registry.get("a").unwrap();
    assert_eq!(p.directory(), "different-folder");
    assert_eq!(p.runtime().id(), "a");
    assert_eq!(
        p.runtime().source.read_file("mod.json").unwrap(),
        fs::read(t.0.join("mods/different-folder/mod.json")).unwrap()
    );
    let wrong = RuntimePackage::new("not-a", p.runtime().source.clone());
    assert!(
        PackageRecord::new(
            "folder".into(),
            p.parsed().clone(),
            wrong,
            p.raw_source().clone()
        )
        .is_err()
    );
    fs::write(t.0.join("not-directory"), b"x").unwrap();
    let mut roots = t.roots();
    roots.mods = t.0.join("not-directory");
    assert!(discover(&roots, ParseOptions::default(), &[]).is_err());
}

pub fn discovery_inference_and_locales() {
    let t = Tree::new();
    t.package("source", "a");
    let p = t.0.join("mods/source");
    fs::create_dir_all(p.join("src/nested")).unwrap();
    fs::create_dir(p.join("locales")).unwrap();
    fs::write(
        p.join("mod.json"),
        br#"{"id":"a","name":"A","version":"1.0.0","capabilities":["patch"]}"#,
    )
    .unwrap();
    fs::write(
        p.join("extended.mod.json"),
        br#"{"schemaVersion":1,"enabled":true,"settings":{"flag":{"value":true}}}"#,
    )
    .unwrap();
    fs::write(
        p.join("src/nested/feature.lua"),
        b"return {on_update=function() end}",
    )
    .unwrap();
    fs::write(
        p.join("locales/en.json"),
        br#"{"strings":{"hello":"Hello"}}"#,
    )
    .unwrap();
    fs::write(p.join("locales/ignored.JSON"), b"invalid").unwrap();
    fs::write(p.join("locales/.json"), b"not a locale extension").unwrap();
    fs::write(p.join("src/.lua"), b"\xff").unwrap(); // Path::extension is None.
    fs::create_dir(p.join("locales/nested")).unwrap();
    fs::write(p.join("locales/nested/not-a-locale.json"), b"invalid").unwrap();
    let report = discover(&t.roots(), ParseOptions::default(), &[]).unwrap();
    assert!(report.errors.is_empty(), "{:?}", report.errors);
    let m = &report.registry.get("a").unwrap().parsed().manifest;
    assert_eq!(
        m.capabilities,
        vec![sfi_package::package::manifest::Capability::Patch]
    ); // Inference never grants permissions.
    assert_eq!(m.settings[0].apply_at, "live");
    assert_eq!(m.locales["en"].strings["hello"], "Hello");
    fs::write(p.join("locales/en.json"), b"invalid").unwrap();
    assert_eq!(
        discover(&t.roots(), ParseOptions::default(), &[])
            .unwrap()
            .errors
            .len(),
        1
    );
}

pub fn dependency_matrix() {
    for optional in [false, true] {
        for installed in [false, true] {
            for enabled in [false, true] {
                for compatible in [false, true] {
                    let mut packages = vec![parsed(
                        "a",
                        true,
                        &["runtime"],
                        json!([dep("z", if compatible { "^1" } else { "^2" }, optional)]),
                    )];
                    if installed {
                        packages.push(parsed("z", enabled, &[], json!([])));
                    }
                    let r = registry(packages);
                    let p = startup_plan(&r, false, None);
                    let success = if optional && (!installed || !enabled) {
                        true
                    } else {
                        installed && enabled && compatible
                    };
                    assert_eq!(
                        p.ids.contains(&"a".into()),
                        success,
                        "optional={optional},installed={installed},enabled={enabled},compatible={compatible}: {:?}",
                        p.issues
                    );
                    if !success {
                        assert_eq!(p.issues[0].mod_id, "a");
                    }
                }
            }
        }
    }
    let r = registry(vec![
        parsed("z", true, &[], json!([])),
        parsed("a", true, &[], json!([dep("z", "*", false)])),
    ]);
    assert_eq!(ids(&startup_plan(&r, false, None)), vec!["z", "a"]);
    for optional in [false, true] {
        for range in ["^1", "^1.7.9", "=1.0.0", ">=2"] {
            let r = registry(vec![parsed(
                "a",
                true,
                &[],
                json!([dep("shroudforge-api", range, optional)]),
            )]);
            let p = startup_plan(&r, false, None);
            assert_eq!(p.issues.is_empty(), range == "^1" || range == "=1.0.0");
            if !p.issues.is_empty() {
                assert_eq!(p.issues[0].detail, "a: incompatible API dependency");
                assert_eq!(p.issues[0].code, "dependency-version");
            }
        }
    }
}

pub fn dependency_targets_cycles_and_retention() {
    let mut other = parsed("z", true, &[], json!([]));
    other.manifest.target = Target::Client;
    let r = registry(vec![
        parsed("a", true, &[], json!([dep("z", "*", false)])),
        other,
    ]);
    let p = startup_plan(&r, true, None);
    assert!(p.ids.is_empty());
    assert_eq!(p.issues.len(), 1);
    assert_eq!(p.issues[0].detail, "z: wrong process target");
    assert_eq!(p.issues[0].code, "wrong-target");
    let r = registry(vec![
        parsed("a", true, &[], json!([dep("b", "*", false)])),
        parsed("b", true, &[], json!([dep("a", "*", false)])),
    ]);
    let p = startup_plan(&r, false, None);
    assert!(p.ids.is_empty());
    assert_eq!(p.issues[0].detail, "dependency cycle: a");
    assert_eq!(p.issues[0].code, "dependency-cycle");
    let r = registry(vec![
        parsed(
            "a",
            true,
            &[],
            json!([dep("z", "*", false), dep("absent", "*", false)]),
        ),
        parsed("z", true, &[], json!([])),
    ]);
    let p = startup_plan(&r, false, None);
    assert_eq!(ids(&p), vec!["z"]);
    assert_eq!(p.issues[0].detail, "a: missing absent");
    assert_eq!(p.issues[0].to_string(), "a: a: missing absent");
    for (detail, code) in [
        ("missing thing incompatible", "missing-dependency"),
        ("disabled dependency: z", "dependency-disabled"),
        ("X incompatible", "dependency-version"),
        ("dependency cycle: a", "dependency-cycle"),
        ("z: wrong process target", "wrong-target"),
        ("conflict", "conflict-or-plan-error"),
    ] {
        assert_eq!(PlanIssue::new("a", detail.into()).code, code);
    }
}

pub fn runtime_candidates_and_membership() {
    let r = registry(vec![
        parsed(
            "a",
            false,
            &["runtime-register-dll"],
            json!([dep("z", "*", false)]),
        ),
        parsed("z", false, &["patch"], json!([])),
        parsed("export-only", true, &["export"], json!([])),
        parsed("asset-only", true, &["patch"], json!([])),
    ]);
    assert_eq!(ids(&runtime_candidates(&r, false, None)), vec!["z", "a"]);
    let snapshot = r.snapshot();
    assert!(snapshot.has_mod("a"));
    assert!(snapshot.has_mod("asset-only"));
    assert!(!snapshot.has_mod("absent"));
    assert!(!snapshot.get("a").unwrap().enabled);
    assert_eq!(
        ids(&startup_plan(&r, false, None)),
        vec!["asset-only", "export-only"]
    );
    let r = registry(vec![
        parsed("a", false, &["runtime"], json!([dep("z", "*", true)])),
        parsed("z", false, &[], json!([])),
    ]);
    assert_eq!(ids(&runtime_candidates(&r, false, None)), vec!["z", "a"]);
}

pub fn profile_value() -> Value {
    json!({"schemaVersion":1,"id":"adapter","packageDirectory":"Folder","name":"Adapter","files":[{"path":"nested/Example.DLL"}]})
}
pub fn profiles_validation() {
    let p = ProfileDocument::parse(profile_value().to_string().as_bytes()).unwrap();
    assert_eq!(p.profile.files[0].mode, DllMode::Library);
    for mode in ["library", "dinput8"] {
        let mut v = profile_value();
        v["files"][0]["mode"] = json!(mode);
        assert!(ProfileDocument::parse(v.to_string().as_bytes()).is_ok());
    }
    for path in [
        "../bad.dll",
        "/bad.dll",
        "a\\b.dll",
        "C:bad.dll",
        "./bad.dll",
        "bad.txt",
        "",
    ] {
        let mut v = profile_value();
        v["files"][0]["path"] = json!(path);
        assert!(
            ProfileDocument::parse(v.to_string().as_bytes()).is_err(),
            "{path}"
        );
    }
    for (key, value) in [
        ("schemaVersion", json!(2)),
        ("id", json!("none")),
        ("name", json!(" ")),
        ("packageDirectory", json!("a/b")),
        ("unexpected", json!(true)),
        ("modJson", json!({"id":"overwrite"})),
        ("extendedModJson", json!([])),
    ] {
        let mut v = profile_value();
        v[key] = value;
        assert!(
            ProfileDocument::parse(v.to_string().as_bytes()).is_err(),
            "{key}"
        );
    }
    let mut v = profile_value();
    v["files"] = json!([{"path":"a.dll"},{"path":"A.DLL"}]);
    assert!(ProfileDocument::parse(v.to_string().as_bytes()).is_err());
    v["files"] = json!(
        (0..17)
            .map(|i| json!({"path":format!("{i}.dll")}))
            .collect::<Vec<_>>()
    );
    assert!(ProfileDocument::parse(v.to_string().as_bytes()).is_err());
    v["files"] = json!(
        (0..16)
            .map(|i| json!({"path":format!("{i}.dll")}))
            .collect::<Vec<_>>()
    );
    assert!(ProfileDocument::parse(v.to_string().as_bytes()).is_ok());
    let mut v = profile_value();
    v["files"][0]["mode"] = json!("execute");
    assert!(ProfileDocument::parse(v.to_string().as_bytes()).is_err());
    for (key, value) in [
        ("packageFiles", json!({"mod.json":"{}"})),
        ("importFiles", json!({"safe":"../escape"})),
        ("modJson", json!({"capabilities":["unknown"]})),
    ] {
        let mut v = profile_value();
        v[key] = value;
        assert!(ProfileDocument::parse(v.to_string().as_bytes()).is_err());
    }
}

pub fn profiles_selection_overlay_and_catalog() {
    let target = json!({"scalar":7,"array":[1],"obj":{"old":1},"capabilities":["patch"]});
    let mut target = sfi_package::json::Value::from(target);
    merge_object(
        &mut target,
        &json!({"scalar":{"new":1},"array":{"new":1},"obj":{"new":2}}).into(),
    );
    assert_eq!(
        target,
        sfi_package::json::Value::from(
            json!({"scalar":7,"array":[1],"obj":{"old":1,"new":2},"capabilities":["patch"]})
        )
    );
    // Overlay keys are appended in the overlay's document order (SF preserve_order).
    let mut target: sfi_package::json::Value = serde_json::from_str(r#"{"z":{"k":1}}"#).unwrap();
    let overlay: sfi_package::json::Value =
        serde_json::from_str(r#"{"y":1,"z":{"b":2,"a":3},"x":4}"#).unwrap();
    merge_object(&mut target, &overlay);
    assert_eq!(
        target.to_string(),
        r#"{"z":{"k":1,"b":2,"a":3},"y":1,"x":4}"#
    );
    let t = Tree::new();
    t.package("Folder", "a");
    let mut v = profile_value();
    v["modId"] = json!("a");
    v["modJson"] = json!({"capabilities":["runtime-register-dll","runtime"]});
    v["extendedModJson"] = json!({"enabled":false,"description":"ignored"});
    v["packageFiles"] = json!({"src/mod.lua":"return {on_update=function() end}"});
    fs::write(t.0.join("profiles/z.JSON"), v.to_string()).unwrap();
    fs::write(t.0.join("profiles/a.json"), v.to_string()).unwrap();
    fs::write(t.0.join("profiles/b.json"), b"{").unwrap();
    fs::write(t.0.join("profiles/.json"), b"ignored: no extension").unwrap();
    let catalog = read_catalog(&DiskSource::new(t.0.join("profiles")).unwrap()).unwrap();
    assert_eq!(catalog.profiles.len(), 1);
    assert_eq!(catalog.errors.len(), 2);
    assert!(select_profile(&catalog.profiles, "folder", Some("a"), None).is_some());
    assert!(select_profile(&catalog.profiles, "folder", Some("different"), None).is_none());
    assert!(
        select_profile(
            &catalog.profiles,
            "different",
            Some("different"),
            Some("adapter")
        )
        .is_some()
    );
    assert!(select_profile(&catalog.profiles, "Folder", Some("a"), Some("none")).is_none());
    // Remove invalid extension overlay field to exercise effective parser and raw-byte separation.
    v["extendedModJson"] = json!({"enabled":false});
    fs::write(t.0.join("profiles/a.json"), v.to_string()).unwrap();
    let report = discover(&t.roots(), ParseOptions::default(), &[]).unwrap();
    assert!(report.errors.is_empty(), "{:?}", report.errors);
    let p = report.registry.get("a").unwrap();
    assert!(p.parsed().manifest.enabled);
    assert_eq!(p.parsed().manifest.capabilities.len(), 2);
    assert_eq!(
        p.parsed().documents.manifest.value()["capabilities"],
        sfi_package::json::Value::from(json!(["runtime"]))
    );
    assert_eq!(
        p.runtime().source.read_file("src/mod.lua").unwrap(),
        b"return {on_update=function() end}"
    );
    assert!(p.raw_source().read_file("src/mod.lua").is_err());
}

pub fn compatibility_rules_and_errors() {
    let r = registry(vec![
        parsed("a", true, &[], json!([])),
        parsed("b", true, &[], json!([])),
    ]);
    let v = json!({"schemaVersion":1,"conflicts":[{"first":{"id":"a","version":"^1"},"second":{"id":"b","version":"*"},"reason":"first"},{"first":{"id":"a","version":"*"},"second":{"id":"b","version":"*"},"reason":"last"}]});
    let rules = CompatibilityRules::parse(v.to_string().as_bytes()).unwrap();
    let p = startup_plan(&r, false, Some(&rules));
    assert!(p.ids.is_empty());
    assert_eq!(p.issues[0].detail, "a: last");
    for bad in [
        json!({"schemaVersion":2,"conflicts":[]}),
        json!({"schemaVersion":1,"conflicts":[],"extra":true}),
        json!({"schemaVersion":1,"conflicts":[{"first":{"id":"bad/id","version":"*"},"second":{"id":"b","version":"*"},"reason":"x"}]}),
    ] {
        assert!(CompatibilityRules::parse(bad.to_string().as_bytes()).is_err());
    }
    let mut bad = v.clone();
    bad["conflicts"][0]["first"]["version"] = json!("not semver");
    assert!(CompatibilityRules::parse(bad.to_string().as_bytes()).is_err());
    let r = registry(vec![
        parsed("a", true, &[], json!([])),
        parsed("b", false, &[], json!([])),
    ]);
    assert_eq!(ids(&startup_plan(&r, false, Some(&rules))), vec!["a"]);
    let p = Plan::compatibility_error("invalid".into());
    assert_eq!(p.issues[0].mod_id, "__loader__");
    assert_eq!(p.issues[0].code, "compatibility-rules");
    assert_eq!(p.issues[0].detail, "compatibility rules: invalid");
    assert_eq!(
        CompatibilityRules::parse(br#"{"schemaVersion":2,"conflicts":[]}"#).unwrap_err(),
        "rules: 1 was expected"
    );
    assert!(
        CompatibilityRules::parse(br#"{"schemaVersion":2,"schemaVersion":1,"conflicts":[]}"#)
            .is_ok()
    ); // SF reads a Value first.
    assert!(CompatibilityRules::parse(br#"{"schemaVersion":1.0,"conflicts":[]}"#).is_ok());
}

pub fn revision_bytes_and_save_proposal() {
    assert_eq!(
        revision(b""),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    );
    assert_eq!(
        revision(b"abc"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
    assert_eq!(
        revision(&vec![b'a'; 1000]),
        "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"
    );
    let p = parsed("a", true, &[], json!([]));
    assert_eq!(
        config_bytes(&p.manifest),
        br#"{"enabled":true,"settings":{}}"#
    );
    let mut m = p.manifest.clone();
    m.name = "Other".into();
    assert_eq!(config_revision(&m), config_revision(&p.manifest));
    assert_ne!(loaded_revision(&m), loaded_revision(&p.manifest));
    let expected = config_revision(&p.manifest);
    assert!(check_revision(&p.manifest, &expected).is_ok());
    assert_eq!(
        check_revision(&p.manifest, "stale").unwrap_err(),
        "Mod settings changed since they were displayed. Reload before saving"
    );
    let proposal = propose_edit(
        &p,
        &expected,
        sfi_package::package::edit::Edit::Enabled(false),
    )
    .unwrap();
    assert!(std::str::from_utf8(&proposal).unwrap().contains("false"));
    assert!(
        propose_edit(
            &p,
            "stale",
            sfi_package::package::edit::Edit::Enabled(false)
        )
        .is_err()
    );
    let bytes = loaded_bytes(&p.manifest);
    assert_eq!(bytes, br#"{"id":"a","name":"a","version":"1.0.0","enabled":true,"settingValues":{},"capabilities":[],"dependencies":[],"authors":[],"description":null,"license":null,"icon":null,"target":"client, server","changelog":[],"links":{},"groups":[],"actions":[],"settings":[],"locales":{}}"#);
    assert_eq!(
        loaded_revision(&p.manifest),
        "99e297dfa673af600b99fd0c76424ae56ff6d3e8fdbe61e543b783af9577cc4c"
    );
    // SF holds settingValues in insertion (settings) order: both revisions keep it whether
    // or not this build's serde_json keeps map order.
    let ordered = sfi_package::package::Package::parse(
        br#"{"id":"o","name":"o","version":"1.0.0"}"#,
        Some(br#"{"schemaVersion":1,"enabled":true,"settings":{"z":1.0,"a":1}}"#),
        ParseOptions::default(),
    )
    .unwrap();
    assert_eq!(
        config_bytes(&ordered.manifest),
        br#"{"enabled":true,"settings":{"z":1.0,"a":1}}"#
    );
    assert!(
        String::from_utf8(loaded_bytes(&ordered.manifest))
            .unwrap()
            .contains(r#","enabled":true,"settingValues":{"z":1.0,"a":1},"capabilities":"#)
    );
    for (length, expected) in [
        (
            55,
            "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318",
        ),
        (
            56,
            "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a",
        ),
        (
            63,
            "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34",
        ),
        (
            64,
            "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
        ),
    ] {
        assert_eq!(revision(&vec![b'a'; length]), expected);
    }
}

pub fn manifest_revision_raw_and_ordered() {
    let t = Tree::new();
    t.package("p", "a");
    let root = t.0.join("mods/p");
    fs::create_dir(root.join("locales")).unwrap();
    fs::write(root.join("locales/fr.json"), b"fr").unwrap();
    fs::write(root.join("locales/en.json"), b"en").unwrap();
    fs::write(root.join("locales/skip.JSON"), b"skip").unwrap();
    fs::write(root.join("locales/.json"), b"no extension").unwrap();
    let source = DiskSource::new(&root).unwrap();
    let mut expected = b"mod.json\0".to_vec();
    expected.extend(fs::read(root.join("mod.json")).unwrap());
    expected.extend(b"\0extended.mod.json\0");
    expected.extend(fs::read(root.join("extended.mod.json")).unwrap());
    expected.extend(b"\0locales/en.json\0en\0locales/fr.json\0fr\0");
    assert_eq!(manifest_bytes(&source).unwrap(), expected);
    let before = manifest_revision(&source).unwrap();
    fs::write(root.join("source.lua"), b"changed").unwrap();
    assert_eq!(manifest_revision(&source).unwrap(), before);
    fs::remove_file(root.join("extended.mod.json")).unwrap();
    assert!(
        manifest_bytes(&source)
            .unwrap()
            .windows(21)
            .any(|x| x == b"extended.mod.json\0\xff\0l")
    );
    fs::remove_dir_all(root.join("locales")).unwrap();
    fs::write(root.join("locales"), b"not dir").unwrap();
    assert!(
        manifest_bytes(&source)
            .unwrap_err()
            .contains("locales must be a directory")
    );
}

pub fn preparation_bytes_and_version_difference() {
    let t = Tree::new();
    t.package("p", "a");
    let root = t.0.join("mods/p");
    fs::write(root.join("z.LOG"), b"log").unwrap();
    fs::write(root.join("a.txt"), b"A").unwrap();
    fs::create_dir(root.join("a")).unwrap();
    fs::write(root.join("a/nested"), b"nested").unwrap();
    fs::write(root.join("a-early"), b"sibling").unwrap();
    fs::create_dir(root.join(".hidden")).unwrap();
    fs::write(root.join(".hidden/file"), b"hidden").unwrap();
    let source = DiskSource::new(&root).unwrap();
    let mut m = parsed("a", true, &["patch"], json!([])).manifest;
    let facts = ExecutableFacts {
        length: 0x0102030405060708,
        // A whole number of 100 ns (Windows file times), its bytes unlike in either order.
        modified: UNIX_EPOCH + Duration::from_nanos(258_300),
    };
    let packages = [PreparationPackage {
        manifest: &m,
        source: PreparationSource::Directory(&source),
    }];
    let actual = preparation_bytes(false, facts, &packages, SourceVersion::Sf180).unwrap();
    let mut expected = b"client".to_vec();
    expected.extend(facts.length.to_le_bytes());
    expected.extend(258_300u128.to_le_bytes());
    expected.extend(loaded_bytes(&m));
    // SF sorts PathBuf components, not raw strings: a/nested precedes a-early.
    for name in [
        "a/nested",
        "a-early",
        "a.txt",
        "extended.mod.json",
        "mod.json",
    ] {
        expected.extend(name.replace('/', std::path::MAIN_SEPARATOR_STR).as_bytes());
        expected.extend(digest(&fs::read(root.join(name)).unwrap()));
    }
    assert_eq!(actual, expected);
    assert_eq!(
        preparation_fingerprint(false, facts, &packages, SourceVersion::Sf180).unwrap(),
        revision(&expected)
    );
    assert_ne!(
        preparation_bytes(false, facts, &packages, SourceVersion::Sf179).unwrap(),
        expected
    );
    let pre = ExecutableFacts {
        length: 3,
        modified: UNIX_EPOCH - Duration::from_secs(1),
    };
    let archive = [PreparationPackage {
        manifest: &m,
        source: PreparationSource::Archive(b"archive bytes"),
    }];
    let bytes = preparation_bytes(true, pre, &archive, SourceVersion::Sf180).unwrap();
    assert_eq!(&bytes[..6], b"server");
    assert_eq!(&bytes[6..14], &3u64.to_le_bytes());
    assert_eq!(&bytes[14..30], &[0; 16]);
    assert_eq!(&bytes[bytes.len() - 32..], &digest(b"archive bytes"));
    m.capabilities = vec![sfi_package::package::manifest::Capability::Runtime];
    let packages = [PreparationPackage {
        manifest: &m,
        source: PreparationSource::Directory(&source),
    }];
    assert_eq!(
        preparation_bytes(false, facts, &packages, SourceVersion::Sf180)
            .unwrap()
            .len(),
        30
    );
}

pub fn profile_duplicate_fields_are_rejected() {
    // SF parses the closed record directly, not through a duplicate-collapsing Value.
    assert!(
        ProfileDocument::parse(
            br#"{"schemaVersion":1,"id":"first","id":"second","packageDirectory":"p","name":"P"}"#
        )
        .is_err()
    );
}

pub fn injected_source_traversal_order_and_failures() {
    use sfi_package::source::{SourceEntry, SourceKind, SourceMetadata};
    use std::{cell::RefCell, io};
    struct Ordered {
        reads: RefCell<Vec<String>>,
    }
    impl PackageSource for Ordered {
        fn read_file(&self, path: &str) -> io::Result<Vec<u8>> {
            self.reads.borrow_mut().push(path.into());
            match path {
                "src/z.lua" => Ok(b"z".to_vec()),
                "src/inner/a.lua" => Ok(b"a".to_vec()),
                "src/bad.lua" => Err(io::ErrorKind::PermissionDenied.into()),
                _ => Err(io::ErrorKind::NotFound.into()),
            }
        }
        fn metadata(&self, path: &str) -> io::Result<SourceMetadata> {
            Ok(SourceMetadata {
                kind: if path == "src/inner" {
                    SourceKind::Directory
                } else {
                    SourceKind::File
                },
            })
        }
        fn read_dir(&self, path: &str) -> io::Result<Vec<io::Result<SourceEntry>>> {
            let entries = match path {
                "src" => vec![
                    ("z.lua", SourceKind::File),
                    ("bad.lua", SourceKind::File),
                    ("inner", SourceKind::Directory),
                    ("ignored.LUA", SourceKind::File),
                ],
                "src/inner" => vec![("a.lua", SourceKind::File)],
                "missing" => return Err(io::ErrorKind::NotFound.into()),
                _ => return Err(io::ErrorKind::PermissionDenied.into()),
            };
            let mut result = vec![Err(io::ErrorKind::Other.into())];
            result.extend(entries.into_iter().map(|(name, kind)| {
                Ok(SourceEntry {
                    name: name.into(),
                    path: format!("{path}/{name}"),
                    kind,
                })
            }));
            Ok(result)
        }
    }
    let source = Ordered {
        reads: RefCell::default(),
    };
    let mut output = String::new();
    collect_lua_sources(&source, "src", &mut output).unwrap();
    assert_eq!(output, "z\na\n");
    assert_eq!(
        *source.reads.borrow(),
        vec!["src/z.lua", "src/bad.lua", "src/inner/a.lua"]
    );
    collect_lua_sources(&source, "missing", &mut output).unwrap();
    assert!(collect_lua_sources(&source, "denied", &mut output).is_err());
    let r = registry(vec![
        parsed(
            "a",
            true,
            &[],
            json!([dep("z", "*", false), dep("b", "*", false)]),
        ),
        parsed("b", true, &[], json!([])),
        parsed("z", true, &[], json!([])),
    ]);
    assert_eq!(ids(&startup_plan(&r, false, None)), vec!["z", "b", "a"]); // declared edges, not sorted edges
    for (range, version, ok) in [
        ("^1", "1.1.0-beta.1", false),
        ("^1.1.0-beta.1", "1.1.0-beta.2", true),
    ] {
        let mut z = parsed("z", true, &[], json!([]));
        z.manifest.version = version.parse().unwrap();
        let r = registry(vec![
            parsed("a", true, &[], json!([dep("z", range, false)])),
            z,
        ]);
        assert_eq!(startup_plan(&r, false, None).ids.contains(&"a".into()), ok);
    }
}

pub fn ordered_dll_selections_are_not_maps() {
    let config = json!({"modules":{"dllManager":{"profiles":[
        {"id":"first","packageDirectory":"folder","name":"First","target":"both","compatibilityProfile":"none","files":[{"path":"a.dll"}]},
        {"id":"second","packageDirectory":"Folder","name":"Second","target":"client","compatibilityProfile":"adapter"}
    ]}}});
    let selections = selections_from_config(&config);
    assert_eq!(selections.len(), 2);
    assert_eq!(selection_for_package(&selections, "FOLDER"), Some("none"));
    assert_eq!(selections[0].files[0].mode, DllMode::Library);
    assert!(!selections[0].enabled);
    assert!(!selections[0].approved);
    let tree = Tree::new();
    tree.package("Folder", "a");
    let mut profile = profile_value();
    profile["modJson"] = json!({"name":"Adapted"});
    fs::write(tree.0.join("profiles/a.json"), profile.to_string()).unwrap();
    let report = discover(&tree.roots(), ParseOptions::default(), &selections).unwrap();
    assert_eq!(
        report.registry.get("a").unwrap().parsed().manifest.name,
        "a"
    );
    let reversed: Vec<_> = selections.iter().rev().cloned().collect();
    let report = discover(&tree.roots(), ParseOptions::default(), &reversed).unwrap();
    assert_eq!(
        report.registry.get("a").unwrap().parsed().manifest.name,
        "Adapted"
    );
    assert!(
        selections_from_config(&json!({"modules":{"dllManager":{"profiles":[{"unknown":true}]}}}))
            .is_empty()
    );
    let mut selections = selections;
    selections[0].compatibility_profile = None;
    assert_eq!(selection_for_package(&selections, "folder"), None); // don't skip to a later match
}

// The read cutoff makes the pre-fix infinite recursion fail safely rather than
// overflowing a test thread's stack. The source and symlinks are otherwise real.
#[cfg(unix)]
fn discovery_cycle(link: &str, target: &str) {
    use sfi_package::source::{SourceEntry, SourceMetadata};
    use std::{cell::Cell, io, os::unix::fs::symlink};
    struct Bounded {
        source: DiskSource,
        lists: Cell<usize>,
    }
    impl PackageSource for Bounded {
        fn read_file(&self, path: &str) -> io::Result<Vec<u8>> {
            self.source.read_file(path)
        }
        fn metadata(&self, path: &str) -> io::Result<SourceMetadata> {
            self.source.metadata(path)
        }
        fn read_dir(&self, path: &str) -> io::Result<Vec<io::Result<SourceEntry>>> {
            self.lists.set(self.lists.get() + 1);
            if self.lists.get() > 12 {
                return Err(io::Error::other("test recursion cutoff"));
            }
            self.source.read_dir(path)
        }
    }
    for version in [SourceVersion::Sf179, SourceVersion::Sf180] {
        let tree = Tree::new();
        tree.package("cyclic", "cyclic");
        tree.package("neighbor", "neighbor");
        let root = tree.0.join("mods/cyclic");
        fs::create_dir_all(root.join("src/nested")).unwrap();
        symlink(target, root.join(link)).unwrap();
        let report = discover_source(
            &DiskSource::new(tree.0.join("mods")).unwrap(),
            |entry| {
                Ok(Rc::new(Bounded {
                    source: DiskSource::new(tree.0.join("mods").join(&entry.path))?,
                    lists: Cell::new(0),
                }) as Rc<dyn PackageSource>)
            },
            ParseOptions {
                version,
                ..ParseOptions::default()
            },
            &ProfileCatalog::default(),
            &[],
        )
        .unwrap();
        assert_eq!(
            report.registry.snapshot().ids().collect::<Vec<_>>(),
            ["neighbor"]
        );
        assert_eq!(report.errors.len(), 1);
        assert_eq!(report.errors[0].directory, "cyclic");
        assert!(
            report.errors[0]
                .detail
                .starts_with("source directory cycle:"),
            "{}",
            report.errors[0].detail
        );
        assert!(!report.errors[0].detail.contains("test recursion cutoff"));
    }
}
#[cfg(unix)]
pub fn discovery_self_cycle() {
    discovery_cycle("src/self", ".");
}
#[cfg(unix)]
pub fn discovery_ancestor_cycle() {
    discovery_cycle("src/nested/up", "..");
}
#[cfg(unix)]
pub fn discovery_contained_alias() {
    let tree = Tree::new();
    tree.package("alias", "a");
    let root = tree.0.join("mods/alias");
    fs::create_dir_all(root.join("src/real/nested")).unwrap();
    fs::write(
        root.join("src/real/nested/feature.lua"),
        b"on_update=function() end",
    )
    .unwrap();
    std::os::unix::fs::symlink("real", root.join("src/alias")).unwrap();
    let mut output = String::new();
    collect_lua_sources(&DiskSource::new(&root).unwrap(), "src", &mut output).unwrap();
    assert_eq!(
        output,
        "on_update=function() end\non_update=function() end\n"
    );
    let report = discover(&tree.roots(), ParseOptions::default(), &[]).unwrap();
    assert!(report.errors.is_empty(), "{:?}", report.errors);
    assert!(report.registry.get("a").is_some());
}

pub fn discovery_invalid_source_utf8() {
    for version in [SourceVersion::Sf179, SourceVersion::Sf180] {
        let tree = Tree::new();
        tree.package("invalid", "invalid");
        tree.package("neighbor", "neighbor");
        fs::create_dir(tree.0.join("mods/invalid/src")).unwrap();
        fs::write(tree.0.join("mods/invalid/src/bad.lua"), b"\xff").unwrap();
        let report = discover(
            &tree.roots(),
            ParseOptions {
                version,
                ..ParseOptions::default()
            },
            &[],
        )
        .unwrap();
        assert_eq!(
            report.registry.snapshot().ids().collect::<Vec<_>>(),
            ["neighbor"]
        );
        assert_eq!(report.errors.len(), 1);
        assert_eq!(
            report.errors[0].detail,
            "stream did not contain valid UTF-8"
        );
    }
}

pub fn profile_directory_listing() {
    use sfi_package::source::{SourceEntry, SourceKind, SourceMetadata};
    use std::io;
    struct Listing(DiskSource);
    impl PackageSource for Listing {
        fn read_file(&self, path: &str) -> io::Result<Vec<u8>> {
            self.0.read_file(path)
        }
        fn metadata(&self, path: &str) -> io::Result<SourceMetadata> {
            self.0.metadata(path)
        }
        fn read_dir(&self, path: &str) -> io::Result<Vec<io::Result<SourceEntry>>> {
            if std::path::Path::new(path) != std::path::Path::new("assets") {
                return self.0.read_dir(path);
            }
            Ok(vec![
                Ok(SourceEntry {
                    name: "z.txt".into(),
                    path: "assets/z.txt".into(),
                    kind: SourceKind::File,
                }),
                Err(io::Error::new(
                    io::ErrorKind::PermissionDenied,
                    "entry unavailable",
                )),
                Ok(SourceEntry {
                    name: "same.txt".into(),
                    path: "assets//same.txt".into(),
                    kind: SourceKind::File,
                }),
            ])
        }
    }
    let tree = Tree::new();
    tree.package("Folder", "a");
    let root = tree.0.join("mods/Folder");
    fs::create_dir(root.join("assets")).unwrap();
    fs::write(root.join("assets/z.txt"), b"z").unwrap();
    fs::write(root.join("assets/same.txt"), b"physical").unwrap();
    let raw: Rc<dyn PackageSource> = Rc::new(Listing(DiskSource::new(&root).unwrap()));
    let before = manifest_revision(raw.as_ref()).unwrap();
    let mut profile = profile_value();
    profile["packageFiles"] = json!({"assets//./a.txt":"a", "assets/same.txt":"virtual", "assets/b.txt":"b", "assets/nested/child.txt":"nested", "missing/child.txt":"absent parent"});
    let catalog = ProfileCatalog {
        profiles: vec![ProfileDocument::parse(profile.to_string().as_bytes()).unwrap()],
        errors: vec![],
    };
    let record = read_package(
        "Folder",
        raw.clone(),
        ParseOptions::default(),
        &catalog,
        None,
    )
    .unwrap();
    for path in ["assets", "assets/."] {
        let listed = record.runtime().source.read_dir(path).unwrap();
        assert_eq!(listed.len(), 5);
        assert_eq!(listed[0].as_ref().unwrap().path, "assets/z.txt");
        assert_eq!(
            listed[1].as_ref().unwrap_err().kind(),
            io::ErrorKind::PermissionDenied
        );
        assert_eq!(
            listed[1].as_ref().unwrap_err().to_string(),
            "entry unavailable"
        );
        assert_eq!(listed[2].as_ref().unwrap().path, "assets//same.txt");
        // SF appends original key spellings, in unspecified HashMap order.
        let mut appended: Vec<_> = listed[3..]
            .iter()
            .map(|entry| entry.as_ref().unwrap().clone())
            .collect();
        appended.sort_by(|a, b| a.path.cmp(&b.path));
        assert_eq!(
            appended[0],
            SourceEntry {
                name: "a.txt".into(),
                path: "assets//./a.txt".into(),
                kind: SourceKind::File
            }
        );
        assert_eq!(appended[1].path, "assets/b.txt");
    }
    assert_eq!(
        record
            .runtime()
            .source
            .read_file("assets/same.txt")
            .unwrap(),
        b"virtual"
    );
    assert_eq!(
        record
            .runtime()
            .source
            .read_dir("missing")
            .unwrap_err()
            .kind(),
        io::ErrorKind::NotFound
    );
    assert_eq!(
        manifest_revision(record.raw_source().as_ref()).unwrap(),
        before
    );
    assert_eq!(raw.read_file("assets/same.txt").unwrap(), b"physical");
}

pub fn profile_source_inference_directory_quirk() {
    // Both pins' is_directory uses starts_with INCLUDING equality for overrides.
    // A virtual Lua file is therefore recursed into, not read for inference.
    for version in [SourceVersion::Sf179, SourceVersion::Sf180] {
        let tree = Tree::new();
        tree.package("Folder", "a");
        let root = tree.0.join("mods/Folder");
        fs::create_dir(root.join("src")).unwrap();
        fs::write(
            root.join("extended.mod.json"),
            br#"{"schemaVersion":1,"enabled":true,"settings":{"flag":{"value":true}}}"#,
        )
        .unwrap();
        let mut profile = profile_value();
        profile["packageFiles"] = json!({"src//feature.lua":"on_update=function() end"});
        fs::write(tree.0.join("profiles/p.json"), profile.to_string()).unwrap();
        let options = ParseOptions {
            version,
            ..ParseOptions::default()
        };
        let report = discover(&tree.roots(), options, &[]).unwrap();
        assert!(report.errors.is_empty(), "{:?}", report.errors);
        assert_eq!(
            report.registry.get("a").unwrap().parsed().manifest.settings[0].apply_at,
            "restart"
        );
        fs::write(root.join("src/feature.lua"), b"physical file").unwrap();
        let report = discover(&tree.roots(), options, &[]).unwrap();
        assert!(report.registry.get("a").is_none());
        assert_eq!(report.errors.len(), 1);
        assert_eq!(
            report.errors[0].detail,
            DiskSource::new(root)
                .unwrap()
                .read_dir("src/feature.lua")
                .unwrap_err()
                .to_string()
        );
    }
}

pub fn profile_locale_directory_quirk() {
    // Both pins read_mod_locale_catalogs.rs:21-24 skip EVERY exact override:
    // is_directory includes Path::starts_with equality, even for a physical file.
    for version in [SourceVersion::Sf179, SourceVersion::Sf180] {
        for backed in [false, true] {
            for spelling in ["locales/en.json", "locales//en.json", "locales/./en.json"] {
                let tree = Tree::new();
                tree.package("Folder", "a");
                let root = tree.0.join("mods/Folder");
                fs::create_dir(root.join("locales")).unwrap();
                if backed {
                    fs::write(root.join("locales/en.json"), b"also-not-json").unwrap();
                }
                let mut profile = profile_value();
                profile["packageFiles"] = json!({spelling: "not-json"});
                fs::write(tree.0.join("profiles/p.json"), profile.to_string()).unwrap();
                let report = discover(
                    &tree.roots(),
                    ParseOptions {
                        version,
                        ..ParseOptions::default()
                    },
                    &[],
                )
                .unwrap();
                assert!(
                    report.errors.is_empty(),
                    "backed={backed}, {spelling}: {:?}",
                    report.errors
                );
                let record = report
                    .registry
                    .get("a")
                    .expect("locale overrides must not reject the package");
                assert!(record.parsed().manifest.locales.is_empty());
                let source = &record.runtime().source;
                assert_eq!(source.read_file("locales/en.json").unwrap(), b"not-json");
                assert!(source.exists("locales/en.json"));
                assert!(source.is_file("locales/en.json"));
                assert!(source.is_directory("locales/en.json"));
                // A non-overridden physical catalog is still selected and validated.
                fs::write(
                    root.join("locales/fr.json"),
                    br#"{"strings":{"hello":"Bonjour"}}"#,
                )
                .unwrap();
                let report = discover(
                    &tree.roots(),
                    ParseOptions {
                        version,
                        ..ParseOptions::default()
                    },
                    &[],
                )
                .unwrap();
                assert!(report.errors.is_empty(), "{:?}", report.errors);
                let locales = &report.registry.get("a").unwrap().parsed().manifest.locales;
                assert_eq!(locales.len(), 1);
                assert_eq!(locales["fr"].strings["hello"], "Bonjour");
            }
        }
    }
}

pub fn profile_source_predicates_in_public_traversal() {
    use sfi_package::source::ProfileOverlaySource;
    // The public traversal helper must use exactly the same predicates as discovery,
    // without an out-of-band override map that only one caller knows about.
    for backed in [false, true] {
        let tree = Tree::new();
        let root = tree.0.join("mods");
        fs::create_dir(root.join("src")).unwrap();
        if backed {
            fs::write(root.join("src/feature.lua"), b"physical Lua").unwrap();
        }
        let source = ProfileOverlaySource::new(
            Rc::new(DiskSource::new(&root).unwrap()),
            &std::collections::HashMap::from([("src//./feature.lua".into(), "virtual Lua".into())]),
        );
        let mut output = String::new();
        let result = collect_lua_sources(&source, "src", &mut output);
        if backed {
            assert_eq!(
                result.unwrap_err(),
                DiskSource::new(&root)
                    .unwrap()
                    .read_dir("src/feature.lua")
                    .unwrap_err()
                    .to_string()
            );
        } else {
            result.unwrap();
        }
        assert!(
            output.is_empty(),
            "exact overrides are recursed into, never read as Lua"
        );
    }
}

// Differential: config/compatibility/rules.json. tests/conformance/package-model/
// rules-cases.json; rules-sf-<pin>.json are ShroudForge's own check_mod_conflicts.rs:8-45
// (identical at both pins), run natively with validate_document(RULES) and an explicit
// {id: [enabled, version]} table in place of ModEnvironment (provenance in goldens.toml).
// Kiln: CompatibilityRules::parse, then startup_plan over the same packages; a blocked
// enabled root reports "<id>: <reason>".
fn rules_registry(mods: &Value) -> Registry {
    let packages = mods
        .as_object()
        .unwrap()
        .iter()
        .map(|(id, m)| {
            ParsedPackage::parse(
                json!({"id":id,"name":id,"version":m[1]})
                    .to_string()
                    .as_bytes(),
                Some(
                    json!({"schemaVersion":1,"enabled":m[0],"targets":["client","server"]})
                        .to_string()
                        .as_bytes(),
                ),
                ParseOptions::default(),
            )
            .unwrap()
        })
        .collect();
    registry(packages)
}
fn compatibility_rules_match_shroudforge(golden: &str) {
    let cases: Vec<Value> = serde_json::from_str(include_str!(
        "../conformance/package-model/rules-cases.json"
    ))
    .unwrap();
    let golden: Vec<Value> = serde_json::from_str(golden).unwrap();
    assert_eq!(cases.len(), golden.len());
    for (case, sf) in cases.iter().zip(&golden) {
        assert_eq!(case["id"], sf["id"]);
        let kiln = match CompatibilityRules::parse(case["rules"].as_str().unwrap().as_bytes()) {
            Err(e) => json!({"err": e}),
            Ok(rules) => {
                let r = rules_registry(&case["mods"]);
                let p = startup_plan(&r, false, Some(&rules));
                let mut blocked = serde_json::Map::new();
                for issue in &p.issues {
                    let reason = issue
                        .detail
                        .strip_prefix(&format!("{}: ", issue.mod_id))
                        .expect("only conflict issues in these registries");
                    blocked.insert(issue.mod_id.clone(), json!(reason));
                    assert_eq!(issue.code, "conflict-or-plan-error");
                }
                for id in &p.ids {
                    assert!(!blocked.contains_key(id));
                }
                json!({"blocked": blocked})
            }
        };
        assert_eq!(kiln, sf["result"], "{}", case["id"]);
    }
}
pub fn compatibility_rules_match_shroudforge_goldens() {
    compatibility_rules_match_shroudforge(include_str!(
        "../conformance/package-model/rules-sf-1.7.9.json"
    ));
    compatibility_rules_match_shroudforge(include_str!(
        "../conformance/package-model/rules-sf-1.8.0.json"
    ));
}

// Differential: the rules FILE. tests/conformance/package-model/rules-file-cases.json;
// rules-file-sf-<pin>.json are ShroudForge's own compatibility::conflicts
// (check_mod_conflicts.rs:5-45) and plan_report_detailed (plan_mod_load_order.rs:142-227),
// unchanged, over a scratch game directory whose config/compatibility/rules.json is set up
// as the case says (harness "rules-file" mode; provenance in goldens.toml). Kiln:
// rules_file::read_rules on a Kiln-chosen path set up the same way, then plan_with_rules
// over the same packages (client side).
fn rules_file_setup(case: &Value, path: &std::path::Path) {
    let parent = path.parent().unwrap();
    let bytes = || -> Vec<u8> {
        match case.get("hex").and_then(Value::as_str) {
            Some(s) => (0..s.len())
                .step_by(2)
                .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
                .collect(),
            None => case["text"].as_str().unwrap().as_bytes().to_vec(),
        }
    };
    match case["kind"].as_str().unwrap() {
        "missing" => fs::create_dir_all(parent).unwrap(),
        "missing-parent" => fs::create_dir_all(parent.parent().unwrap()).unwrap(),
        "directory" => fs::create_dir_all(path).unwrap(),
        "parent-is-file" => {
            fs::create_dir_all(parent.parent().unwrap()).unwrap();
            fs::write(parent, b"x").unwrap();
        }
        #[cfg(unix)]
        "dangling-symlink" => {
            fs::create_dir_all(parent).unwrap();
            std::os::unix::fs::symlink(parent.join("absent.json"), path).unwrap();
        }
        #[cfg(unix)]
        "unreadable" => {
            use std::os::unix::fs::PermissionsExt;
            fs::create_dir_all(parent).unwrap();
            fs::write(path, bytes()).unwrap();
            fs::set_permissions(path, fs::Permissions::from_mode(0o000)).unwrap();
            assert!(
                fs::read(path).is_err(),
                "run as a user that mode 000 locks out"
            );
        }
        "file" => {
            fs::create_dir_all(parent).unwrap();
            fs::write(path, bytes()).unwrap();
        }
        other => panic!("unknown kind {other}"),
    }
}
fn compatibility_rules_file_matches_shroudforge(golden: &str) {
    use sfi_package::package::rules_file::{RulesRead, plan_with_rules, read_rules};
    let cases: Vec<Value> = serde_json::from_str(include_str!(
        "../conformance/package-model/rules-file-cases.json"
    ))
    .unwrap();
    let golden: Vec<Value> = serde_json::from_str(golden).unwrap();
    assert_eq!(cases.len(), golden.len());
    let missing = &golden[0];
    assert_eq!(missing["id"], "file.missing");
    let mut skipped = vec![];
    for (case, sf) in cases.iter().zip(&golden) {
        let id = case["id"].as_str().unwrap();
        assert_eq!(case["id"], sf["id"]);
        if case["unix"] == true && cfg!(not(unix)) {
            skipped.push(id);
            continue;
        }
        let t = Tree::new();
        let path = t.0.join("kiln-owned").join("compat").join("rules.json");
        rules_file_setup(case, &path);
        let read = read_rules(&path, &t.0);
        #[cfg(unix)]
        if case["kind"] == "unreadable" {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&path, fs::Permissions::from_mode(0o644)).unwrap();
        }
        let r = rules_registry(&case["mods"]);
        let plan = plan_with_rules(&r, false, &read);
        let mut expected = sf.clone();
        // ShroudForge's IO error is std's text for the host; the goldens are Linux. On
        // Windows a directory is "Access is denied" (os error 5) and a file standing in for
        // the folder is NotFound (os error 3), which ShroudForge answers with its default.
        if cfg!(windows) && matches!(case["kind"].as_str(), Some("directory" | "parent-is-file")) {
            let error = fs::read(&path).unwrap_err();
            if case["kind"] == "directory" {
                assert_eq!(error.raw_os_error(), Some(5), "{id}");
                let text = error.to_string();
                expected["result"] = json!({"err": text});
                expected["plan"]["issues"][0]["detail"] =
                    json!(format!("compatibility rules: {text}"));
            } else {
                assert_eq!(error.kind(), std::io::ErrorKind::NotFound, "{id}");
                expected["result"] = missing["result"].clone();
                expected["plan"] = missing["plan"].clone();
            }
        }
        let result = match &read {
            RulesRead::Error(e) => json!({"err": e}),
            RulesRead::Embedded(_) | RulesRead::File { .. } => {
                let mut blocked = serde_json::Map::new();
                for issue in &plan.issues {
                    let reason = issue
                        .detail
                        .strip_prefix(&format!("{}: ", issue.mod_id))
                        .expect("only conflict issues in these registries");
                    blocked.insert(issue.mod_id.clone(), json!(reason));
                }
                json!({"blocked": blocked})
            }
        };
        assert_eq!(result, expected["result"], "{id}");
        assert_eq!(
            json!({"ids": plan.ids, "issues": plan.issues}),
            expected["plan"],
            "{id}"
        );
        // NotFound alone takes ShroudForge's default; a read file carries its digest.
        match &read {
            RulesRead::Embedded(rules) => {
                assert_eq!(expected["result"], missing["result"], "{id}");
                assert_eq!(
                    rules.document.bytes(),
                    sfi_package::package::rules_file::EMBEDDED_RULES.as_bytes()
                );
            }
            RulesRead::File { sha256, .. } => {
                assert_eq!(case["kind"], "file", "{id}");
                let hex: String = sha256.iter().map(|b| format!("{b:02x}")).collect();
                assert_eq!(hex, case["sha256"].as_str().unwrap(), "{id}");
            }
            RulesRead::Error(_) => assert!(expected["result"].get("err").is_some(), "{id}"),
        }
    }
    let windows_skips: &[&str] = &["file.dangling-symlink", "file.unreadable"];
    assert_eq!(skipped, if cfg!(unix) { &[][..] } else { windows_skips });
}
pub fn compatibility_rules_file_matches_shroudforge_goldens() {
    compatibility_rules_file_matches_shroudforge(include_str!(
        "../conformance/package-model/rules-file-sf-1.7.9.json"
    ));
    compatibility_rules_file_matches_shroudforge(include_str!(
        "../conformance/package-model/rules-file-sf-1.8.0.json"
    ));
}
// plan_mod_load_order.rs:216-227 (both pins): a rules error gives an empty plan whatever the
// packages and side, plus exactly one __loader__ issue, serialized as ShroudForge's
// ModPlanIssue. The embedded documents are ShroudForge's files byte for byte (SHA-256 of
// conflicts/rules.json and conflicts/rules.schema.json at both pins).
pub fn compatibility_rules_file_loader_issue() {
    use sfi_package::package::rules_file::{
        EMBEDDED_RULES, EMBEDDED_RULES_SCHEMA, RulesRead, plan_with_rules, read_rules,
    };
    assert_eq!(
        revision(EMBEDDED_RULES.as_bytes()),
        "f4f31e2f9e8858dd31011d529024e3040709e91c3f81e79a47c33921aa92a17e"
    );
    assert_eq!(
        revision(EMBEDDED_RULES_SCHEMA.as_bytes()),
        "9a00ec604abae803389ca17cbb3914f843382247ec6757c096dfcd0a31e7434a"
    );
    let r = rules_registry(&json!({"a": [true, "1.0.0"], "b": [false, "1.0.0"]}));
    for server in [false, true] {
        let plan = plan_with_rules(&r, server, &RulesRead::Error("boom".into()));
        assert!(plan.ids.is_empty());
        assert_eq!(
            serde_json::to_value(&plan.issues).unwrap(),
            json!([{"modId": "__loader__", "code": "compatibility-rules", "detail": "compatibility rules: boom"}])
        );
        assert_eq!(
            plan.issues[0].to_string(),
            "__loader__: compatibility rules: boom"
        );
    }
    let t = Tree::new();
    let RulesRead::Embedded(rules) = read_rules(&t.0.join("absent.json"), &t.0) else {
        panic!("a missing file uses the embedded default");
    };
    assert!(rules.conflicts.is_empty());
    assert_eq!(rules.schema_version, 1);
    let plan = plan_with_rules(&r, false, &RulesRead::Embedded(rules));
    assert_eq!(plan.ids, startup_plan(&r, false, None).ids);
    assert_eq!(plan.ids, vec!["a"]);
}
// Kiln mode never reads ShroudForge's own data (<game>/shroudforge, any letter case, also
// through a link or "..") or Kiln's compat-rules.json: the reader refuses such a path before
// it reads, so a valid file there blocks nothing and a missing one is not mistaken for the
// default. A Kiln path next to ShroudForge's rules never falls back to them, and a game that
// itself sits under a folder named shroudforge reads its rules normally. kiln-sfi never
// calls the reader.
pub fn compatibility_rules_file_never_reads_shroudforge_data() {
    use sfi_package::package::rules_file::{RulesRead, plan_with_rules, read_rules};
    let blocking = r#"{"schemaVersion":1,"conflicts":[{"first":{"id":"a","version":"*"},"second":{"id":"b","version":"*"},"reason":"clash"}]}"#;
    let write = |path: &std::path::Path| {
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, blocking).unwrap();
    };
    let t = Tree::new();
    let game = t.0.join("game");
    let sf = game.join("shroudforge/config/compatibility/rules.json");
    write(&sf);
    let r = rules_registry(&json!({"a": [true, "1.0.0"], "b": [true, "1.0.0"]}));
    assert!(matches!(
        read_rules(&game.join("kiln/rules.json"), &game),
        RulesRead::Embedded(_)
    ));
    let folder = "Kiln mode never reads files in the game's shroudforge folder";
    let mut refused = vec![
        sf.clone(),
        game.join("shroudforge"),
        game.join("ShroudForge/config/compatibility/rules.json"),
        game.join("SHROUDFORGE/absent.json"),
        game.join("kiln/../shroudforge/config/compatibility/rules.json"),
        game.join("absent/../shroudforge/config/compatibility/rules.json"),
        game.join("./shroudforge/absent/rules.json"),
    ];
    // A link into the folder (a file link and a folder link) is refused like the folder.
    #[cfg(unix)]
    {
        fs::create_dir_all(game.join("kiln")).unwrap();
        std::os::unix::fs::symlink(&sf, game.join("kiln/linked.json")).unwrap();
        std::os::unix::fs::symlink(game.join("shroudforge"), game.join("kiln/sf")).unwrap();
        std::os::unix::fs::symlink(&game, t.0.join("game-link")).unwrap();
        refused.push(game.join("kiln/linked.json"));
        refused.push(game.join("kiln/sf/config/compatibility/rules.json"));
        refused.push(game.join("kiln/sf/absent.json"));
        refused.push(t.0.join("game-link/shroudforge/config/compatibility/rules.json"));
    }
    // On Windows the root's spelling may differ in case, and links need a privilege a test
    // host may lack: then the link cases are reported and left out.
    #[cfg(windows)]
    {
        fs::create_dir_all(game.join("kiln")).unwrap();
        let upper = std::path::PathBuf::from(game.to_str().unwrap().to_uppercase());
        assert!(matches!(read_rules(&sf, &upper), RulesRead::Error(_)));
        let links = std::os::windows::fs::symlink_file(&sf, game.join("kiln/linked.json"))
            .and_then(|()| {
                std::os::windows::fs::symlink_dir(game.join("shroudforge"), game.join("kiln/sf"))
            });
        match links {
            Ok(()) => {
                refused.push(game.join("kiln/linked.json"));
                refused.push(game.join("kiln/sf/config/compatibility/rules.json"));
                refused.push(game.join("kiln/sf/absent.json"));
            }
            Err(error) => eprintln!("windows link cases not run: {error}"),
        }
        // Other names of the folder, which a name check misses: the 8.3 name (Wine keeps the
        // spelling it is given, as it does for another drive letter, Z:, and another case of a
        // non-ASCII game folder), a Unix link under Wine (which canonicalize keeps), a local share
        // natively. Each is still ShroudForge's folder, known by identity.
        refused.extend(windows_names::other_names(&game, &sf));
    }
    for path in refused {
        let read = read_rules(&path, &game);
        let RulesRead::Error(text) = &read else {
            panic!("{} was read: {read:?}", path.display());
        };
        assert_eq!(text, &format!("{} was not read: {folder}", path.display()));
        let plan = plan_with_rules(&r, false, &read);
        assert!(plan.ids.is_empty());
        assert_eq!(plan.issues[0].code, "compatibility-rules");
    }
    #[cfg(windows)]
    {
        windows_names::cleanup(&game);
        let (path, root) = windows_names::other_case(&t.0, &sf);
        let read = read_rules(&path, &root);
        let RulesRead::Error(text) = &read else {
            panic!(
                "{} under {} was read: {read:?}",
                path.display(),
                root.display()
            );
        };
        assert_eq!(text, &format!("{} was not read: {folder}", path.display()));
    }
    // The root may itself be given through a link.
    #[cfg(unix)]
    assert!(matches!(
        read_rules(&sf, &t.0.join("game-link")),
        RulesRead::Error(_)
    ));
    let compat = game.join("kiln/Compat-Rules.json");
    write(&compat);
    let RulesRead::Error(text) = read_rules(&compat, &game) else {
        panic!("Kiln's compat-rules.json was read");
    };
    assert_eq!(
        text,
        format!(
            "{} was not read: Kiln's compat-rules.json is not a ShroudForge rules document",
            compat.display()
        )
    );
    // Only the folder directly under the game root is ShroudForge's: a game under a folder
    // named shroudforge, a deeper folder of that name, a shroudforge folder outside the game
    // and a name that merely contains the word are all read.
    let outer = t.0.join("shroudforge/game");
    let readable = [
        (outer.join("kiln/rules.json"), outer.clone()),
        (game.join("kiln/shroudforge/rules.json"), game.clone()),
        (t.0.join("shroudforge/rules.json"), game.clone()),
        (
            game.join("kiln/shroudforge-rules/shroudforge"),
            game.clone(),
        ),
        (game.join("shroudforge-data/rules.json"), game.clone()),
    ];
    for (path, root) in &readable {
        write(path);
        let read = read_rules(path, root);
        assert!(
            matches!(read, RulesRead::File { .. }),
            "{} under {}: {read:?}",
            path.display(),
            root.display()
        );
        assert_eq!(plan_with_rules(&r, false, &read).issues.len(), 2);
    }
    // kiln-sfi (the run-time interpreter) never reads the rules; the launch-plan producer does.
    // It reaches the package model through sfi_core::package, so the scan looks for every
    // name a read would need: the reader, the document type and the file's location.
    #[cfg(not(windows))]
    {
        let crate_dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../kiln-sfi");
        let mut stack = vec![crate_dir.join("src")];
        let mut files = vec![crate_dir.join("build.rs")];
        while let Some(dir) = stack.pop() {
            for entry in fs::read_dir(&dir).unwrap() {
                let path = entry.unwrap().path();
                if path.is_dir() {
                    stack.push(path);
                } else if path.extension().is_some_and(|e| e == "rs") {
                    files.push(path);
                }
            }
        }
        assert!(
            files.len() > 10,
            "kiln-sfi sources at {}",
            crate_dir.display()
        );
        for path in &files {
            let text = fs::read_to_string(path).unwrap();
            for word in [
                "rules_file",
                "read_rules",
                "plan_with_rules",
                "CompatibilityRules",
                "compatibility/rules",
            ] {
                assert!(!text.contains(word), "{} names {word}", path.display());
            }
        }
    }
}

// Differential: runtime/profiles/mod-profiles/*.json and their overlays. Goldens are
// ShroudForge's own read_profiles (mod_profiles.rs:34-76, identical at both pins) and
// read_manifest_path with the catalog and loader dllManager selections in place
// (read_mod_manifest.rs:35-101, mod_profiles.rs:161-187). A skipped file's reason is SF's
// serde decode error or validate_profile message (mod_profiles.rs:78-147; SF only logs it).
fn expand_profile(v: &Value) -> String {
    match v.as_str() {
        Some(s) => s.to_owned(),
        None => v["template"].as_str().unwrap().replace(
            "@@",
            &v["fill"]
                .as_str()
                .unwrap()
                .repeat(v["count"].as_u64().unwrap() as usize),
        ),
    }
}
fn profiles_match_shroudforge(version: SourceVersion, golden: &str) {
    let cases: Vec<Value> = serde_json::from_str(include_str!(
        "../conformance/package-model/profile-cases.json"
    ))
    .unwrap();
    let golden: Vec<Value> = serde_json::from_str(golden).unwrap();
    assert_eq!(cases.len(), golden.len());
    for (case, sf) in cases.iter().zip(&golden) {
        let id = &case["id"];
        assert_eq!(id, &sf["id"]);
        let t = Tree::new();
        let mut reasons = serde_json::Map::new();
        let (mut skipped, mut considered) = (0, 0);
        for (name, text) in case["profiles"].as_object().unwrap() {
            let text = expand_profile(text);
            fs::write(t.0.join("profiles").join(name), &text).unwrap();
            let reason = serde_json::from_slice::<Profile>(text.as_bytes())
                .map_err(|e| e.to_string())
                .and_then(|p| p.validate());
            let listed = std::path::Path::new(name)
                .extension()
                .is_some_and(|e| e.eq_ignore_ascii_case("json"));
            considered += usize::from(listed);
            skipped += usize::from(listed && reason.is_err());
            reasons.insert(name.clone(), reason.err().map_or(Value::Null, Value::from));
        }
        assert_eq!(Value::Object(reasons), sf["reasons"], "{id}");
        let catalog = read_catalog(&DiskSource::new(t.0.join("profiles")).unwrap()).unwrap();
        let accepted: Vec<Value> = catalog
            .profiles
            .iter()
            .map(|p| {
                let mut v = serde_json::to_value(&p.profile).unwrap();
                // Goldens abbreviate virtual file contents over 256 bytes to length + SHA-256.
                for (_, text) in v["packageFiles"].as_object_mut().unwrap() {
                    let s = text.as_str().unwrap().to_owned();
                    if s.len() > 256 {
                        *text = json!({"len": s.len(), "sha256": revision(s.as_bytes())});
                    }
                }
                v
            })
            .collect();
        assert_eq!(Value::from(accepted.clone()), sf["accepted"], "{id}");
        let ids: std::collections::BTreeSet<_> =
            accepted.iter().map(|p| p["id"].as_str().unwrap()).collect();
        let duplicates = considered - skipped - accepted.len();
        assert_eq!(catalog.errors.len(), skipped + duplicates, "{id}");
        assert_eq!(ids.len(), accepted.len(), "{id}");
        let Some(files) = case.get("package").and_then(Value::as_object) else {
            assert!(sf.get("manifest").is_none());
            continue;
        };
        let directory = case["directory"].as_str().unwrap_or("pkg");
        let package = t.0.join("mods").join(directory);
        for (name, text) in files {
            let p = package.join(name);
            fs::create_dir_all(p.parent().unwrap()).unwrap();
            fs::write(p, text.as_str().unwrap()).unwrap();
        }
        let selections: Vec<DllSelection> = case
            .get("selections")
            .map(|s| serde_json::from_value(s.clone()).unwrap())
            .unwrap_or_default();
        let record = read_package(
            directory,
            Rc::new(DiskSource::new(&package).unwrap()),
            ParseOptions {
                version,
                ..Default::default()
            },
            &catalog,
            selection_for_package(&selections, directory),
        );
        // Revisions read the RAW package, never the profile overlay (calculate_manifest_revision.rs).
        let raw = DiskSource::new(&package).unwrap();
        let revision = match manifest_revision(&raw) {
            Ok(r) => json!({"Ok": r}),
            Err(e) => json!({"Err": e.replace(&package.display().to_string(), "<package>")}),
        };
        assert_eq!(revision, sf["revision"], "{id}");
        if let Ok(r) = &record {
            assert_eq!(
                manifest_revision(r.raw_source().as_ref()).ok(),
                revision["Ok"].as_str().map(str::to_owned)
            );
        }
        let kiln = match record {
            Ok(r) => json!({"ok": r.parsed().manifest}),
            Err(e) => json!({"err": e}),
        };
        assert_eq!(kiln, sf["manifest"], "{id}");
    }
}
pub fn profiles_match_shroudforge_goldens() {
    // The goldens were made on Linux: a missing file reads there as the system words it here.
    let native = |golden: &str| {
        golden.replace(
            "No such file or directory (os error 2)",
            &std::io::Error::from_raw_os_error(2).to_string(),
        )
    };
    profiles_match_shroudforge(
        SourceVersion::Sf179,
        &native(include_str!(
            "../conformance/package-model/profile-sf-1.7.9.json"
        )),
    );
    profiles_match_shroudforge(
        SourceVersion::Sf180,
        &native(include_str!(
            "../conformance/package-model/profile-sf-1.8.0.json"
        )),
    );
}

// Process targets in the planner (plan_mod_load_order.rs:150-200 both pins).
// A root whose normalized target excludes the process is skipped silently: no ID,
// no issue. A wrong-target DEPENDENCY blocks its consumer ("<id>: wrong process
// target", asserted in dependency_targets_cycles_and_retention).
pub fn wrong_target_roots_are_skipped_silently() {
    for (target, client, server) in [
        (Target::Client, true, false),
        (Target::Server, false, true),
        (Target::ClientServer, true, true),
    ] {
        let mut p = parsed("a", true, &["runtime"], json!([]));
        p.manifest.target = target;
        let r = registry(vec![p]);
        for (is_server, expected) in [(false, client), (true, server)] {
            for plan in [
                startup_plan(&r, is_server, None),
                runtime_candidates(&r, is_server, None),
            ] {
                assert!(plan.issues.is_empty(), "{target:?}");
                assert_eq!(plan.ids.is_empty(), !expected, "{target:?}/{is_server}");
            }
        }
    }
}

// mod-profiles target applicability (dll_manager.rs:36-43 applies_to, both pins:
// Both always, Client on the client, Server on the server; default Both) and a
// catalog directory that cannot be listed: SF profile_for_package suppresses the error
// (mod_profiles.rs:34-40,176-180), so discovery still reads the package, unprofiled.
pub fn profile_target_applicability_and_unlistable_catalog() {
    assert_eq!(DllTarget::default(), DllTarget::Both);
    for (target, client, server) in [
        (DllTarget::Both, true, true),
        (DllTarget::Client, true, false),
        (DllTarget::Server, false, true),
    ] {
        assert_eq!(target.supports_process(false), client);
        assert_eq!(target.supports_process(true), server);
    }
    let t = Tree::new();
    t.package("pkg", "example");
    fs::remove_dir(t.0.join("profiles")).unwrap();
    fs::write(t.0.join("profiles"), "not a directory").unwrap();
    let report = discover(&t.roots(), ParseOptions::default(), &[]).unwrap();
    assert!(report.registry.get("example").is_some());
    assert!(report.errors.is_empty());
    assert_eq!(report.profile_errors.len(), 1);
}

/// Other names of `<game>/shroudforge` on Windows and under Wine, each checked to reach the
/// real file before it is used (a name that does not reach it is reported and left out).
#[cfg(windows)]
mod windows_names {
    use std::{
        os::windows::ffi::{OsStrExt, OsStringExt},
        path::{Path, PathBuf},
    };
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetShortPathNameW(long: *const u16, short: *mut u16, size: u32) -> u32;
        fn GetModuleHandleW(name: *const u16) -> *mut core::ffi::c_void;
        fn GetProcAddress(
            module: *mut core::ffi::c_void,
            name: *const core::ffi::c_char,
        ) -> *mut core::ffi::c_void;
        fn GetFileAttributesW(name: *const u16) -> u32;
    }
    fn wide(path: &Path) -> Vec<u16> {
        path.as_os_str().encode_wide().chain([0]).collect()
    }
    /// Wine's Unix name of `path` (`wine_get_unix_file_name`), or None on native Windows.
    fn unix_name(path: &Path) -> Option<String> {
        let kernel32 = wide(Path::new("kernel32.dll"));
        // SAFETY: NUL-terminated names; kernel32 is loaded in every Win32 process.
        let f = unsafe {
            GetProcAddress(
                GetModuleHandleW(kernel32.as_ptr()),
                c"wine_get_unix_file_name".as_ptr(),
            )
        };
        if f.is_null() {
            return None;
        }
        // SAFETY: Wine's export: char *CDECL wine_get_unix_file_name(LPCWSTR). The test leaks it.
        let f: unsafe extern "C" fn(*const u16) -> *const core::ffi::c_char =
            unsafe { std::mem::transmute(f) };
        let name = wide(path);
        let result = unsafe { f(name.as_ptr()) };
        assert!(!result.is_null(), "no Unix name for {}", path.display());
        Some(
            unsafe { std::ffi::CStr::from_ptr(result) }
                .to_str()
                .unwrap()
                .to_owned(),
        )
    }
    pub fn other_names(game: &Path, sf: &Path) -> Vec<PathBuf> {
        let mut names = Vec::new();
        let keep = |names: &mut Vec<PathBuf>, what: &str, p: PathBuf| {
            if std::fs::read(&p).is_ok_and(|b| b == std::fs::read(sf).unwrap()) {
                names.push(p);
            } else {
                eprintln!("{what} not run: {} does not reach the file", p.display());
            }
        };
        let long = wide(sf);
        let mut short = vec![0u16; 32768];
        // SAFETY: a NUL-terminated name and an output buffer of the length given.
        let n = unsafe { GetShortPathNameW(long.as_ptr(), short.as_mut_ptr(), short.len() as u32) };
        let short = PathBuf::from(std::ffi::OsString::from_wide(&short[..n as usize]));
        if short
            .components()
            .any(|c| c.as_os_str().to_string_lossy().contains('~'))
        {
            keep(&mut names, "8.3 name", short);
        } else {
            eprintln!("8.3 name not run: this volume makes no short names");
        }
        if let Some(unix) = unix_name(game) {
            // Wine: Z: names every Unix path, so the game folder by another drive letter.
            keep(
                &mut names,
                "Z: name",
                PathBuf::from(format!("Z:{}", unix.replace('/', "\\")))
                    .join("shroudforge/config/compatibility/rules.json"),
            );
            // Wine: a Unix link the Linux side made, which Wine follows and canonicalize keeps.
            let link = game.join("kiln").join("unix-sf");
            let _ = std::process::Command::new("/usr/bin/ln")
                .arg("-sfn")
                .arg(unix_name(&game.join("shroudforge")).unwrap())
                .arg(format!(
                    "{}/unix-sf",
                    unix_name(&game.join("kiln")).unwrap()
                ))
                .spawn();
            let deadline = std::time::Instant::now() + std::time::Duration::from_secs(300);
            let name = wide(&link);
            // SAFETY: a NUL-terminated name. Wine shows a link to a folder as a reparse point.
            while unsafe { GetFileAttributesW(name.as_ptr()) } != 0x410 {
                assert!(std::time::Instant::now() < deadline, "ln made no link");
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
            keep(
                &mut names,
                "Unix link",
                link.join("config/compatibility/rules.json"),
            );
            names.push(link.join("absent.json"));
            // Unix links to a folder inside it and to one of its files: no folder on the way is
            // ShroudForge's own, by name or identity; only Wine knows where they lead.
            let kiln = unix_name(&game.join("kiln")).unwrap();
            for (target, name, shown) in [
                ("shroudforge/config", "unix-cfg", 0x410),
                (
                    "shroudforge/config/compatibility/rules.json",
                    "unix-rules.json",
                    0,
                ),
            ] {
                let _ = std::process::Command::new("/usr/bin/ln")
                    // -fn: a link a failed earlier run left is replaced, never followed.
                    .arg("-sfn")
                    .arg(unix_name(&game.join(target)).unwrap())
                    .arg(format!("{kiln}/{name}"))
                    .spawn();
                let deadline = std::time::Instant::now() + std::time::Duration::from_secs(300);
                let wide_name = wide(&game.join("kiln").join(name));
                // SAFETY: a NUL-terminated name. Wine shows a link to a folder as a reparse point
                // (0x410), a link to a file as the file (0: any attributes).
                let made = || {
                    let a = unsafe { GetFileAttributesW(wide_name.as_ptr()) };
                    a != u32::MAX && (shown == 0 || a == shown)
                };
                while !made() {
                    assert!(std::time::Instant::now() < deadline, "ln made no link");
                    std::thread::sleep(std::time::Duration::from_millis(10));
                }
            }
            keep(
                &mut names,
                "Unix link into it",
                game.join("kiln/unix-cfg/compatibility/rules.json"),
            );
            keep(
                &mut names,
                "Unix file link",
                game.join("kiln/unix-rules.json"),
            );
        } else {
            // Native: a local share of the drive.
            let text = game.to_str().unwrap();
            let (drive, rest) = text.trim_start_matches(r"\\?\").split_at(2);
            let share = PathBuf::from(format!(
                r"\\localhost\{}$\{}",
                &drive[..1],
                rest.trim_start_matches('\\')
            ))
            .join("shroudforge/config/compatibility/rules.json");
            keep(&mut names, "local share", share.clone());
            // Links to it through the share: canonicalize resolves them to the share's name,
            // which a name check misses (links need a privilege a test host may lack).
            let cfg = share.parent().unwrap().parent().unwrap().to_path_buf();
            match std::os::windows::fs::symlink_file(&share, game.join("kiln/share-rules.json"))
                .and_then(|()| std::os::windows::fs::symlink_dir(&cfg, game.join("kiln/share-cfg")))
            {
                Ok(()) => {
                    keep(
                        &mut names,
                        "file link through the share",
                        game.join("kiln/share-rules.json"),
                    );
                    keep(
                        &mut names,
                        "folder link through the share",
                        game.join("kiln/share-cfg/compatibility/rules.json"),
                    );
                }
                Err(error) => eprintln!("share link cases not run: {error}"),
            }
        }
        names
    }
    /// A game folder with a Greek name, the file given through another case of it (one name to
    /// NTFS and Wine; Wine's canonicalize keeps the case it is given): (that path, the root).
    pub fn other_case(scratch: &Path, sf: &Path) -> (PathBuf, PathBuf) {
        let greek = scratch.join("ΑΣgame");
        let greek_sf = greek.join("shroudforge/config/compatibility/rules.json");
        std::fs::create_dir_all(greek_sf.parent().unwrap()).unwrap();
        std::fs::copy(sf, &greek_sf).unwrap();
        let other_case = scratch.join("ασgame/shroudforge/config/compatibility/rules.json");
        assert_eq!(
            std::fs::read(&other_case).unwrap(),
            std::fs::read(sf).unwrap()
        );
        (other_case, greek)
    }
    /// Removes the links (never what they point at), so the tree's removal does not follow them.
    pub fn cleanup(game: &Path) {
        let Some(kiln) = unix_name(&game.join("kiln")) else {
            let _ = std::fs::remove_file(game.join("kiln/share-rules.json"));
            let _ = std::fs::remove_dir(game.join("kiln/share-cfg"));
            return;
        };
        for link in ["unix-sf", "unix-cfg", "unix-rules.json"] {
            let _ = std::process::Command::new("/usr/bin/rm")
                .arg(format!("{kiln}/{link}"))
                .spawn();
            let deadline = std::time::Instant::now() + std::time::Duration::from_secs(300);
            let name = wide(&game.join("kiln").join(link));
            // SAFETY: a NUL-terminated name.
            while unsafe { GetFileAttributesW(name.as_ptr()) } != u32::MAX {
                assert!(std::time::Instant::now() < deadline, "rm left the link");
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        }
    }
}

/// Links Windows and Wine make for a test (see `folder_link`).
#[cfg(windows)]
mod links {
    /// A folder link at `link` to `target`: a Unix symbolic link under Wine (made by ln, which Wine starts but gives
    /// no handle to wait on, so the link is waited for: a must-happen wait, 300 s bound), a junction natively.
    /// Returns the link's Unix name under Wine (remove it with `remove_link`), `None` natively.
    pub fn folder_link(target: &std::path::Path, link: &std::path::Path) -> Option<String> {
        use std::os::windows::ffi::OsStrExt;
        #[link(name = "kernel32")]
        unsafe extern "system" {
            fn GetModuleHandleW(name: *const u16) -> *mut core::ffi::c_void;
            fn GetProcAddress(
                module: *mut core::ffi::c_void,
                name: *const core::ffi::c_char,
            ) -> *mut core::ffi::c_void;
            fn GetFileAttributesW(name: *const u16) -> u32;
        }
        let wide =
            |p: &std::path::Path| -> Vec<u16> { p.as_os_str().encode_wide().chain([0]).collect() };
        let kernel32 = wide(std::path::Path::new("kernel32.dll"));
        // SAFETY: NUL-terminated names; kernel32 is loaded in every Win32 process.
        let f = unsafe {
            GetProcAddress(
                GetModuleHandleW(kernel32.as_ptr()),
                c"wine_get_unix_file_name".as_ptr(),
            )
        };
        if f.is_null() {
            // cmd takes no forward slashes in a path.
            let native = |p: &std::path::Path| {
                std::path::PathBuf::from(p.to_str().unwrap().replace('/', "\\"))
            };
            let status = std::process::Command::new("cmd")
                .arg("/C")
                .arg("mklink")
                .arg("/J")
                .arg(native(link))
                .arg(native(target))
                .status()
                .unwrap();
            assert!(
                status.success() && link.is_dir(),
                "junction {}",
                link.display()
            );
            return None;
        }
        // SAFETY: Wine's export: char *CDECL wine_get_unix_file_name(LPCWSTR). The test leaks the strings.
        let f: unsafe extern "C" fn(*const u16) -> *const core::ffi::c_char =
            unsafe { std::mem::transmute(f) };
        let unix = |p: &std::path::Path| -> String {
            let name = wide(p);
            let r = unsafe { f(name.as_ptr()) };
            assert!(!r.is_null(), "no Unix name for {}", p.display());
            unsafe { std::ffi::CStr::from_ptr(r) }
                .to_str()
                .unwrap()
                .to_owned()
        };
        let parent = unix(link.parent().unwrap());
        let name = link.file_name().unwrap().to_str().unwrap();
        let unix_link = format!("{}/{name}", parent.trim_end_matches('/'));
        let _ = std::process::Command::new("/usr/bin/ln")
            .arg("-s")
            .arg(unix(target))
            .arg(&unix_link)
            .spawn();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(300);
        let link_name = wide(link);
        // SAFETY: a NUL-terminated name. Wine shows a link to a folder as a reparse point by name.
        while unsafe { GetFileAttributesW(link_name.as_ptr()) } != 0x410 {
            assert!(std::time::Instant::now() < deadline, "ln made no link");
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        Some(unix_link)
    }
    /// Removes a link `folder_link` made: the link itself (by its Unix name under Wine), never what it points at.
    pub fn remove_link(link: &std::path::Path, unix_link: Option<String>) {
        match unix_link {
            Some(unix_link) => {
                let _ = std::process::Command::new("/usr/bin/rm")
                    .arg(unix_link)
                    .spawn();
                let deadline = std::time::Instant::now() + std::time::Duration::from_secs(300);
                while std::fs::symlink_metadata(link).is_ok() {
                    assert!(std::time::Instant::now() < deadline, "rm left the link");
                    std::thread::sleep(std::time::Duration::from_millis(10));
                }
            }
            None => std::fs::remove_dir(link).unwrap(),
        }
    }
}
// A disk source never reads through a folder link out of its package, natively (a junction, which
// canonicalize resolves) and under Wine (a Unix link, which canonicalize keeps in the path); a link
// that stays inside it still reads, as ShroudForge's own source does.
#[cfg(windows)]
pub fn disk_source_refuses_a_link_out_of_the_package() {
    let t = Tree::new();
    let package = t.0.join("mods/pack");
    let outside = t.0.join("outside");
    fs::create_dir_all(package.join("inner")).unwrap();
    fs::create_dir_all(&outside).unwrap();
    fs::write(package.join("inner/g.lua"), "in").unwrap();
    fs::write(outside.join("f.lua"), "out").unwrap();
    let out = links::folder_link(&outside, &package.join("out"));
    let alias = links::folder_link(&package.join("inner"), &package.join("alias"));
    assert_eq!(
        fs::read(package.join("out/f.lua")).unwrap(),
        b"out",
        "the link is real"
    );
    let source = DiskSource::new(&package).unwrap();
    assert_eq!(source.read_file("inner/g.lua").unwrap(), b"in");
    assert_eq!(source.read_file("alias/g.lua").unwrap(), b"in");
    for path in ["out/f.lua", "out"] {
        assert!(
            source.metadata(path).is_err(),
            "{path} wine: {}",
            out.is_some()
        );
    }
    assert_eq!(
        source.read_file("out/f.lua").unwrap_err().kind(),
        std::io::ErrorKind::PermissionDenied,
        "wine: {}",
        out.is_some()
    );
    assert!(source.read_dir("out").is_err());
    links::remove_link(&package.join("out"), out);
    links::remove_link(&package.join("alias"), alias);
    assert_eq!(fs::read(outside.join("f.lua")).unwrap(), b"out");
}
