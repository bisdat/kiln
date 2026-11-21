// SPDX-License-Identifier: MIT
//! A root-style consumer (like kiln-manage) of sfi-package alone: parse, discover, plan,
//! profiles, revisions and a serialized snapshot, run from a foreign workspace and CWD.
use sfi_package::{
    package::{
        Package, ParseOptions, discover::PackageRoots, discover::discover, edit,
        plan::startup_plan, profiles::selections_from_config, revisions,
    },
    source::{DiskSource, PackageSource, ProfileOverlaySource, SourceVersion},
};
use std::{collections::HashMap, fs, rc::Rc};

/// This crate links sfi-package and nothing else that touches serde_json's features, so its
/// own maps must stay sorted: sfi-package must not turn preserve_order on for dependents.
fn dependents_keep_sorted_maps() {
    let map: serde_json::Map<String, serde_json::Value> =
        serde_json::from_str(r#"{"b":1,"a":2}"#).unwrap();
    assert_eq!(
        map.keys().map(String::as_str).collect::<Vec<_>>(),
        ["a", "b"],
        "sfi-package turned on serde_json's preserve_order for its dependents"
    );
}

/// ShroudForge reads documents with preserve_order. In this sorted build sfi-package still
/// keeps document order wherever SF's results show it.
fn document_order_holds_without_preserve_order(root: &std::path::Path) {
    let manifest = br#"{"id":"o","name":"o","version":"1.0.0","capabilities":["runtime"]}"#;
    let extension = br#"{"schemaVersion":1,"enabled":true,"settings":{"z":{"value":"b","options":{"b":"B","a":"A"}},"a":true,"m":{"value":2,"options":{"2":"Two","1":"One"}}},"presets":{"z":{"custom":"b","settings":["m","a"],"values":{"a":{"m":1,"a":false}}}}}"#;
    let parsed = Package::parse(manifest, Some(extension), ParseOptions::default()).unwrap();
    let m = &parsed.manifest;
    let keys: Vec<_> = m.settings.iter().map(|s| s.key.as_str()).collect();
    assert_eq!(keys, ["z", "a", "m"], "settings in document order");
    let options: Vec<_> = m.settings[0]
        .options
        .iter()
        .map(|o| o.label.as_str())
        .collect();
    assert_eq!(options, ["B", "A"], "options in document order");
    assert_eq!(
        String::from_utf8(revisions::config_bytes(m)).unwrap(),
        r#"{"enabled":true,"settings":{"z":"b","a":true,"m":2}}"#
    );
    let loaded = String::from_utf8(revisions::loaded_bytes(m)).unwrap();
    assert!(
        loaded.contains(r#""settingValues":{"z":"b","a":true,"m":2},"#),
        "{loaded}"
    );
    assert!(
        loaded.contains(r#""values":{"a":{"m":1,"a":false}}"#),
        "{loaded}"
    );
    // Refusals name the first offender in document order.
    let strict = ParseOptions {
        tolerate_unknown_fields: false,
        ..ParseOptions::default()
    };
    assert_eq!(
        Package::parse(
            br#"{"id":"o","name":"o","version":"1.0.0","z":1,"a":2}"#,
            None,
            strict
        )
        .unwrap_err(),
        "mod: unknown fields: z, a. Enable Mod Detection to ignore unknown top-level fields."
    );
    assert_eq!(
        Package::parse(
            manifest,
            Some(br#"{"schemaVersion":1,"enabled":true,"groups":[{"label":"G","z":true,"a":false}]}"#),
            ParseOptions::default(),
        )
        .unwrap_err(),
        "extended-mod: Additional properties are not allowed ('z' was unexpected)"
    );
    assert_eq!(
        Package::parse(
            manifest,
            Some(br#"{"schemaVersion":1,"enabled":true,"settings":{"x":{"value":1,"z":0,"a":0}}}"#),
            ParseOptions::default(),
        )
        .unwrap_err(),
        r#"extended-mod: {"value":1,"z":0,"a":0} is not valid under any of the schemas listed in the 'oneOf' keyword"#
    );
    // Kiln's editor lists settings and choices as the mod declares them.
    let editor = edit::read(manifest, Some(extension)).unwrap();
    let keys: Vec<_> = editor.settings.iter().map(|s| s.key.as_str()).collect();
    assert_eq!(keys, ["z", "a", "m"]);
    let choices: Vec<_> = edit::option_keys(&editor.settings[2])
        .into_iter()
        .map(String::as_str)
        .collect();
    assert_eq!(choices, ["2", "1"]);
    // A compatibility profile appends the settings it adds in its own order.
    let dir = root.join("mods/ordered");
    fs::create_dir_all(&dir).unwrap();
    fs::write(dir.join("mod.json"), manifest).unwrap();
    fs::write(
        dir.join("extended.mod.json"),
        br#"{"schemaVersion":1,"enabled":true,"targets":["client","server"],"settings":{"z":1}}"#,
    )
    .unwrap();
    fs::write(
        root.join("profiles/ordered.json"),
        br#"{"schemaVersion":1,"id":"ordered-profile","packageDirectory":"ordered","name":"Ordered","extendedModJson":{"settings":{"y":2,"x":3}}}"#,
    )
    .unwrap();
}

fn main() {
    dependents_keep_sorted_maps();
    let root = std::env::current_dir().unwrap().join("tree");
    let _ = fs::remove_dir_all(&root);
    for (folder, id, deps) in [
        ("one", "a", "[]"),
        ("two", "b", r#"[{"id":"a","version":"^1"}]"#),
    ] {
        let dir = root.join("mods").join(folder);
        fs::create_dir_all(dir.join("src")).unwrap();
        fs::write(
            dir.join("mod.json"),
            format!(r#"{{"id":"{id}","name":"{id}","version":"1.0.0","capabilities":["runtime"],"dependencies":{deps}}}"#),
        )
        .unwrap();
        fs::write(
            dir.join("extended.mod.json"),
            br#"{"schemaVersion":1,"enabled":true,"targets":["client","server"]}"#,
        )
        .unwrap();
        fs::write(dir.join("src/mod.lua"), b"return {}").unwrap();
    }
    fs::create_dir_all(root.join("profiles")).unwrap();
    document_order_holds_without_preserve_order(&root);
    let roots = PackageRoots {
        mods: root.join("mods"),
        config: root.join("config"),
        profiles: root.join("profiles"),
    };
    // Embedded schemas resolve without any manifest-relative path (both dialects).
    for version in [SourceVersion::Sf179, SourceVersion::Sf180] {
        let options = ParseOptions {
            version,
            ..ParseOptions::default()
        };
        let report = discover(&roots, options, &[]).unwrap();
        assert!(report.errors.is_empty(), "{:?}", report.errors);
        let ids: Vec<_> = report
            .registry
            .snapshot()
            .ids()
            .map(str::to_owned)
            .collect();
        assert_eq!(ids, ["a", "b", "o"]);
        let ordered = report.registry.get("o").unwrap();
        let keys: Vec<_> = ordered
            .parsed()
            .manifest
            .settings
            .iter()
            .map(|s| s.key.as_str())
            .collect();
        assert_eq!(keys, ["z", "y", "x"], "profile additions in their order");
        let plan = startup_plan(&report.registry, true, None);
        let order: Vec<_> = plan.ids.iter().map(String::as_str).collect();
        assert_eq!(order, ["a", "b", "o"]);
        let record = report.registry.get("b").unwrap();
        let raw = revisions::manifest_revision(record.raw_source().as_ref()).unwrap();
        assert_eq!(raw.len(), 64);
        revisions::check_revision(
            &record.parsed().manifest,
            &revisions::config_revision(&record.parsed().manifest),
        )
        .unwrap();
        // Pure model snapshots serialize without an interpreter.
        let snapshot = serde_json::to_string(&record.parsed().manifest).unwrap();
        assert!(snapshot.contains("\"b\""));
    }
    let disk: Rc<dyn PackageSource> = Rc::new(DiskSource::new(root.join("mods/one")).unwrap());
    let overlay = ProfileOverlaySource::new(
        disk,
        &HashMap::from([("src/extra.lua".to_owned(), "x".to_owned())]),
    );
    assert!(overlay.is_file("src/extra.lua") && overlay.is_directory("src/extra.lua"));
    assert!(selections_from_config(&serde_json::json!({})).is_empty());
    let parsed = Package::parse(
        &fs::read(root.join("mods/one/mod.json")).unwrap(),
        None,
        ParseOptions::default(),
    )
    .unwrap();
    assert_eq!(parsed.manifest.id, "a");
    fs::remove_dir_all(&root).unwrap();
    // format-1 parity catalogs load and answer queries with sfi-package alone.
    use sfi_package::parity::{Catalog, Outcome, RevisionId};
    let catalog = Catalog::from_json(include_bytes!("sample-catalog-v1.json")).unwrap();
    assert_eq!(
        catalog
            .query(RevisionId::Sf179, "integer.u8.neg", None)
            .outcome,
        Outcome::Absent
    );
    assert_eq!(
        catalog
            .query(RevisionId::Sf180, "runtime.types", None)
            .resolved
            .as_deref(),
        Some("game.types")
    );
    assert_eq!(
        catalog
            .query(RevisionId::Sf180, "game.nothing", None)
            .outcome,
        Outcome::Unknown
    );
    println!("consumer ok");
}
