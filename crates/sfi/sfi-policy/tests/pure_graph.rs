// SPDX-License-Identifier: MIT
//! Red tests: sfi-policy's normal graph is pure Rust, a foreign workspace (the shape
//! kiln-manage will have) builds and runs against it with no plugin, cc, Lua, GPL or test
//! support crate, and the OS-bound evaluation stays out of reach without `os-binding`.
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
    "sfi-package",
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
fn resolved_features(meta: &Value, name: &str) -> Vec<String> {
    let id = meta["packages"]
        .as_array()
        .unwrap()
        .iter()
        .find(|p| p["name"] == name)
        .unwrap()["id"]
        .clone();
    meta["resolve"]["nodes"]
        .as_array()
        .unwrap()
        .iter()
        .find(|n| n["id"] == id)
        .unwrap()["features"]
        .as_array()
        .unwrap()
        .iter()
        .map(|f| f.as_str().unwrap().to_owned())
        .collect()
}

#[test]
fn normal_graph_is_pure_rust_without_plugin_cc_lua_or_test_support() {
    let workspace = manifest_dir().parent().unwrap().join("Cargo.toml");
    let meta = metadata(&workspace, true);
    let (names, packages) = closure(&meta, "sfi-policy");
    for name in FORBIDDEN {
        assert!(
            !names.contains(*name),
            "{name} reached from sfi-policy: {names:?}"
        );
    }
    for (name, package) in &packages {
        assert!(package["links"].is_null(), "{name} links a native library");
    }
    let own = &packages["sfi-policy"];
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
        BTreeSet::from([("serde_json", "normal"), ("sha2", "normal")]),
        "no dev/test-support dependency; the integration tests are pure consumers"
    );
    // A member of the guarded sfi workspace: licence_guard's package and source scans cover it.
    let members: Vec<_> = meta["workspace_members"].as_array().unwrap().clone();
    assert!(
        members
            .iter()
            .any(|m| m.as_str().is_some_and(|m| m.contains("sfi-policy"))),
        "sfi-policy must stay inside the guarded workspace: {members:?}"
    );
    eprintln!("sfi-policy normal+build closure: {names:?}");
}

#[test]
fn a_foreign_workspace_builds_and_runs_against_sfi_policy_only() {
    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("sfi-policy-consumer");
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(dir.join("src")).unwrap();
    let package = manifest_dir();
    fs::write(
        dir.join("Cargo.toml"),
        format!(
            "[package]\nname = \"sfi-policy-consumer\"\nversion = \"0.0.0\"\nedition = \"2024\"\npublish = false\n\n\
             [workspace]\n\n[features]\nbind = [\"sfi-policy/os-binding\"]\n\n\
             [[bin]]\nname = \"consumer\"\npath = \"src/consumer.rs\"\n\n[[bin]]\nname = \"bound\"\npath = \"src/bound.rs\"\n\n\
             [dependencies]\nsfi-policy = {{ path = {:?} }}\n",
            package.to_str().unwrap()
        ),
    )
    .unwrap();
    for file in ["consumer.rs", "bound.rs"] {
        fs::copy(
            package.join("tests/consumer").join(file),
            dir.join("src").join(file),
        )
        .unwrap();
    }
    // Start from the sfi lock so resolution stays offline and on the reviewed versions.
    fs::copy(
        package.parent().unwrap().join("Cargo.lock"),
        dir.join("Cargo.lock"),
    )
    .unwrap();
    let meta = metadata(&dir.join("Cargo.toml"), false);
    let (names, _) = closure(&meta, "sfi-policy-consumer");
    for name in FORBIDDEN {
        assert!(
            !names.contains(*name),
            "{name} reached from the consumer: {names:?}"
        );
    }
    assert!(
        resolved_features(&meta, "sfi-policy").is_empty(),
        "a default consumer resolves sfi-policy without os-binding"
    );
    let run = |args: &[&str]| {
        cargo()
            .current_dir(&dir)
            .env("CARGO_TARGET_DIR", dir.join("target"))
            .env("CARGO_BUILD_BUILD_DIR", dir.join("target/build"))
            .args(args)
            .output()
            .unwrap()
    };
    let consumer = run(&["run", "--offline", "--quiet", "--bin", "consumer"]);
    assert!(
        consumer.status.success(),
        "{}",
        String::from_utf8_lossy(&consumer.stderr)
    );
    assert_eq!(
        String::from_utf8_lossy(&consumer.stdout).trim(),
        "consumer ok"
    );
    // The OS-bound members are not part of the default surface ...
    let refused = run(&["check", "--offline", "--quiet", "--bin", "bound"]);
    let stderr = String::from_utf8_lossy(&refused.stderr);
    assert!(
        !refused.status.success(),
        "bound.rs compiled without os-binding"
    );
    for member in ["evaluate_bound", "parse_system"] {
        assert!(stderr.contains(member), "{member} not refused: {stderr}");
    }
    // ... and the same source compiles once the feature is named, so the refusal is the gate.
    let bound = run(&[
        "check",
        "--offline",
        "--quiet",
        "--bin",
        "bound",
        "--features",
        "bind",
    ]);
    assert!(
        bound.status.success(),
        "{}",
        String::from_utf8_lossy(&bound.stderr)
    );
    fs::remove_dir_all(&dir).unwrap();
}
