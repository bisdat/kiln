// SPDX-License-Identifier: MIT
use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    path::Path,
};

#[test]
fn image_default_features_and_entire_locked_closure_match_both_shroudforge_pins() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR"));
    let manifest: toml::Value =
        toml::from_str(&fs::read_to_string(root.join("Cargo.toml")).unwrap()).unwrap();
    // A plain dependency enables DEFAULT features, including all default codecs.
    assert_eq!(manifest["dependencies"]["image"].as_str(), Some("=0.25.10"));
    assert_eq!(manifest["dependencies"]["half"].as_str(), Some("=2.7.1"));
    let lock: toml::Value =
        toml::from_str(&fs::read_to_string(root.join("../Cargo.lock")).unwrap()).unwrap();
    let packages = lock["package"].as_array().unwrap();
    let find = |dependency: &str| {
        let parts: Vec<_> = dependency.split_whitespace().collect();
        let matches: Vec<_> = packages
            .iter()
            .filter(|p| {
                p["name"].as_str() == Some(parts[0])
                    && (parts.len() == 1 || p["version"].as_str() == Some(parts[1]))
            })
            .collect();
        assert_eq!(
            matches.len(),
            1,
            "ambiguous/missing locked dependency {dependency}"
        );
        matches[0]
    };
    let mut pending = vec![find("image")];
    let mut actual = BTreeMap::new();
    while let Some(package) = pending.pop() {
        let key = (
            package["name"].as_str().unwrap().to_owned(),
            package["version"].as_str().unwrap().to_owned(),
        );
        if actual
            .insert(key, package["checksum"].as_str().unwrap().to_owned())
            .is_some()
        {
            continue;
        }
        if let Some(dependencies) = package.get("dependencies").and_then(toml::Value::as_array) {
            pending.extend(dependencies.iter().map(|d| find(d.as_str().unwrap())));
        }
    }
    let snapshot: serde_json::Value =
        serde_json::from_str(include_str!("fixtures/c7e/image-lock.json")).unwrap();
    let expected: BTreeMap<_, _> = snapshot["packages"]
        .as_array()
        .unwrap()
        .iter()
        .map(|p| {
            (
                (
                    p["name"].as_str().unwrap().to_owned(),
                    p["version"].as_str().unwrap().to_owned(),
                ),
                p["checksum"].as_str().unwrap().to_owned(),
            )
        })
        .collect();
    // 120 with the image codecs plus bytemuck_derive once the block codec enables bytemuck's derive.
    assert_eq!(expected.len(), 121);
    assert_eq!(
        actual, expected,
        "image transitive resolution drifted from both SF pins"
    );
    let half_versions: BTreeSet<_> = packages
        .iter()
        .filter(|p| p["name"].as_str() == Some("half"))
        .map(|p| p["version"].as_str().unwrap())
        .collect();
    assert_eq!(half_versions, BTreeSet::from(["2.7.1"]));
}

#[test]
fn lua_binding_lock_matches_shroudforge() {
    let workspace = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
    let source = fs::read_to_string(workspace.join("Cargo.lock"))
        .expect("read the committed sfi Cargo.lock");
    let lock: toml::Value = toml::from_str(&source).expect("parse the committed sfi Cargo.lock");
    let packages = lock["package"].as_array().unwrap();

    // Both SF 1.7.9 (d8a394bc) and 1.8.0 (38ef448d) resolve these versions:
    // Cargo.lock:2286-2287 (mlua), :2305-2306 (mlua-sys), :2182-2183 (lua-src).
    for (name, expected) in [
        ("mlua", "0.11.6"),
        ("mlua-sys", "0.10.0"),
        ("lua-src", "550.0.0"),
    ] {
        let versions: Vec<_> = packages
            .iter()
            .filter(|package| package["name"].as_str() == Some(name))
            .map(|package| package["version"].as_str().unwrap())
            .collect();
        assert_eq!(versions, [expected], "ShroudForge lock drift for {name}");
    }
}

#[test]
fn block_codec_is_shroudforges_locked_cpu_crate_without_a_gpu_backend() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR"));
    let manifest: toml::Value =
        toml::from_str(&fs::read_to_string(root.join("Cargo.toml")).unwrap()).unwrap();
    let block = &manifest["dependencies"]["block_compression"];
    assert_eq!(block["version"].as_str(), Some("=0.6.0"));
    assert_eq!(block["default-features"].as_bool(), Some(false));
    let features: Vec<_> = block["features"]
        .as_array()
        .unwrap()
        .iter()
        .map(|f| f.as_str().unwrap())
        .collect();
    assert_eq!(features, ["bc15", "bc6h", "bc7"]);
    let lock: toml::Value =
        toml::from_str(&fs::read_to_string(root.join("../Cargo.lock")).unwrap()).unwrap();
    let packages = lock["package"].as_array().unwrap();
    let find = |name: &str| {
        let found: Vec<_> = packages
            .iter()
            .filter(|p| p["name"].as_str() == Some(name))
            .collect();
        assert_eq!(found.len(), 1, "{name}");
        found[0]
    };
    // Same versions and checksums as both SF pins' Cargo.lock (block_compression :288-296,
    // bytemuck :332-338, bytemuck_derive :341-349).
    for (name, version, checksum) in [
        (
            "block_compression",
            "0.6.0",
            "50ab69e224693b5d879ae105255f92540ed9357ff4503d089e67063da3e703b3",
        ),
        (
            "bytemuck",
            "1.25.2",
            "95832e849adfb21180ccb6826a99da14e5d266ae5c2e668e1602cf234f153797",
        ),
        (
            "bytemuck_derive",
            "1.12.1",
            "6a1f896587b6f2c069c73d2f0913e2d590c3990285cd2f0b6aa02b786b4c679c",
        ),
    ] {
        let package = find(name);
        assert_eq!(package["version"].as_str(), Some(version), "{name}");
        assert_eq!(package["checksum"].as_str(), Some(checksum), "{name}");
    }
    let deps: Vec<_> = find("block_compression")["dependencies"]
        .as_array()
        .unwrap()
        .iter()
        .map(|d| d.as_str().unwrap())
        .collect();
    assert_eq!(deps, ["bytemuck", "half"]);
    assert!(
        packages.iter().all(|p| p["name"].as_str() != Some("wgpu")),
        "the GPU backend must stay out of the lock"
    );
}
