// SPDX-License-Identifier: MIT
//! Red tests: sfi-package's NORMAL graph is pure Rust, and a foreign workspace (the shape
//! kiln-manage will have) can build and run against it without any Lua/native/plugin crate.
use serde_json::Value;
use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    path::{Path, PathBuf},
    process::Command,
};

const FORBIDDEN: &[&str] = &[
    "mlua",
    "mlua-sys",
    "mlua_derive",
    "lua-src",
    "luajit-src",
    "sfi-core",
    "kiln-sfi",
    "kiln-abi",
    "cc",
    "cmake",
    "bindgen",
    "pkg-config",
];

fn cargo() -> Command {
    Command::new(env!("CARGO"))
}
fn manifest_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}
fn metadata(manifest: &Path, locked: bool) -> Value {
    let mut command = cargo();
    command.args(["metadata", "--format-version", "1", "--offline"]);
    if locked {
        command.arg("--locked");
    }
    let out = command
        .arg("--manifest-path")
        .arg(manifest)
        .output()
        .unwrap();
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    serde_json::from_slice(&out.stdout).unwrap()
}
/// Names of every package compiled for `root`: normal and build edges from EVERY reached
/// package, dev edges never. A build-dependency's own normal dependencies are compiled into
/// its build script too, so a vendored C build behind one still shows up as `cc`/`cmake`.
fn closure(meta: &Value, root: &str) -> (BTreeSet<String>, BTreeMap<String, Value>) {
    let packages: BTreeMap<String, Value> = meta["packages"]
        .as_array()
        .unwrap()
        .iter()
        .map(|p| (p["id"].as_str().unwrap().to_owned(), p.clone()))
        .collect();
    let nodes: BTreeMap<String, Value> = meta["resolve"]["nodes"]
        .as_array()
        .unwrap()
        .iter()
        .map(|n| (n["id"].as_str().unwrap().to_owned(), n.clone()))
        .collect();
    let start = packages.values().find(|p| p["name"] == root).unwrap()["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let (mut seen, mut stack, mut names) = (BTreeSet::new(), vec![start], BTreeSet::new());
    while let Some(id) = stack.pop() {
        if !seen.insert(id.clone()) {
            continue;
        }
        names.insert(packages[&id]["name"].as_str().unwrap().to_owned());
        for dep in nodes[&id]["deps"].as_array().unwrap() {
            let compiled = dep["dep_kinds"]
                .as_array()
                .unwrap()
                .iter()
                .any(|k| k["kind"].is_null() || k["kind"] == "build");
            if compiled {
                stack.push(dep["pkg"].as_str().unwrap().to_owned());
            }
        }
    }
    let by_name = packages
        .into_values()
        .filter(|p| names.contains(p["name"].as_str().unwrap()))
        .map(|p| (p["name"].as_str().unwrap().to_owned(), p))
        .collect();
    (names, by_name)
}

#[test]
fn normal_graph_is_pure_rust_without_lua_native_or_test_support() {
    let workspace = manifest_dir().parent().unwrap().join("Cargo.toml");
    let meta = metadata(&workspace, true);
    let (names, packages) = closure(&meta, "sfi-package");
    for name in FORBIDDEN {
        assert!(
            !names.contains(*name),
            "{name} reached from sfi-package: {names:?}"
        );
    }
    for (name, package) in &packages {
        assert!(package["links"].is_null(), "{name} links a native library");
    }
    let own = &packages["sfi-package"];
    let kinds: BTreeSet<_> = own["dependencies"]
        .as_array()
        .unwrap()
        .iter()
        .map(|d| {
            (
                d["name"].as_str().unwrap(),
                d["kind"].as_str().unwrap_or("normal"),
            )
        })
        .collect();
    assert_eq!(
        kinds,
        BTreeSet::from([
            ("indexmap", "normal"),
            ("regex", "normal"),
            ("semver", "normal"),
            ("serde", "normal"),
            ("serde_json", "normal")
        ]),
        "no dev/test-support dependency; the integration tests are pure consumers"
    );
    eprintln!("sfi-package normal+build closure: {names:?}");
}

/// kiln-manage links sfi-package and sfi-policy. cargo unifies features across a build, so
/// a serde_json feature requested anywhere in their normal graph would change how every
/// serde_json map in kiln-manage orders its keys (its written JSON). Document order is
/// sfi-package's own job (json::Value). sfi-core keeps preserve_order: only the interpreter
/// build (sfi-core, kiln-sfi) links it, and it mirrors ShroudForge's own build.
#[test]
fn crates_kiln_manage_links_request_no_serde_json_order_feature() {
    let workspace = manifest_dir().parent().unwrap().join("Cargo.toml");
    let meta = metadata(&workspace, true);
    for root in ["sfi-package", "sfi-policy"] {
        let (names, packages) = closure(&meta, root);
        assert!(!names.contains("sfi-core"), "{root} reaches sfi-core");
        for (name, package) in &packages {
            // A feature of the crate itself can turn it on too (`serde_json/preserve_order`,
            // `serde_json?/preserve_order`); a workspace crate may not even offer one.
            if package["source"].is_null() {
                for (feature, enables) in package["features"].as_object().unwrap() {
                    assert!(
                        !enables.as_array().unwrap().iter().any(|f| {
                            f.as_str().is_some_and(|f| {
                                f.strip_prefix("serde_json")
                                    .and_then(|f| f.strip_prefix('?').or(Some(f)))
                                    == Some("/preserve_order")
                            })
                        }),
                        "{name} (linked by {root}) offers serde_json/preserve_order in feature {feature}"
                    );
                }
            }
            for dep in package["dependencies"].as_array().unwrap() {
                if dep["name"] != "serde_json" || !dep["kind"].is_null() {
                    continue;
                }
                let features = dep["features"].as_array().unwrap();
                assert!(
                    !features.iter().any(|f| f == "preserve_order"),
                    "{name} (linked by {root}) turns on serde_json/preserve_order: {features:?}"
                );
            }
        }
    }
}

#[test]
fn a_foreign_workspace_builds_and_runs_against_sfi_package_only() {
    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("sfi-package-consumer");
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(dir.join("src")).unwrap();
    let package = manifest_dir();
    fs::write(
        dir.join("Cargo.toml"),
        format!(
            "[package]\nname = \"sfi-package-consumer\"\nversion = \"0.0.0\"\nedition = \"2024\"\npublish = false\n\n\
             [workspace]\n\n[dependencies]\nsfi-package = {{ path = {:?} }}\nserde_json = \"1.0\"\n",
            package.to_str().unwrap()
        ),
    )
    .unwrap();
    fs::copy(
        package.join("tests/consumer/main.rs"),
        dir.join("src/main.rs"),
    )
    .unwrap();
    // The consumer also loads and queries a format-1 parity catalog.
    fs::copy(
        package.join("tests/consumer/sample-catalog-v1.json"),
        dir.join("src/sample-catalog-v1.json"),
    )
    .unwrap();
    // Start from the sfi lock so resolution stays offline and on the reviewed versions.
    fs::copy(
        package.parent().unwrap().join("Cargo.lock"),
        dir.join("Cargo.lock"),
    )
    .unwrap();
    let meta = metadata(&dir.join("Cargo.toml"), false);
    let (names, _) = closure(&meta, "sfi-package-consumer");
    for name in FORBIDDEN {
        assert!(
            !names.contains(*name),
            "{name} reached from the consumer: {names:?}"
        );
    }
    let run = cargo()
        .current_dir(&dir)
        .env("CARGO_TARGET_DIR", dir.join("target"))
        .env("CARGO_BUILD_BUILD_DIR", dir.join("target/build"))
        .args(["run", "--offline", "--quiet"])
        .output()
        .unwrap();
    assert!(
        run.status.success(),
        "{}",
        String::from_utf8_lossy(&run.stderr)
    );
    assert_eq!(String::from_utf8_lossy(&run.stdout).trim(), "consumer ok");
    fs::remove_dir_all(&dir).unwrap();
}
