// SPDX-License-Identifier: GPL-3.0-or-later
//! Test-only child of the copied production runner. No production binding is replaced.
//! prepare.py installs this same child/hook in BOTH isolated copies, never in the worktree.
#[path = "m6_fixture/mod.rs"]
mod fixture;
use super::LuaModRunner;
use crate::{RunArgs, RunOptions, cache::CacheDiff, env::AppState};
use mlua::{Function, Table, Value};
use std::fs;

#[test]
fn m6_capture_dependency_bindings() {
    let f = fixture::archive::ArchiveFixture::new("dependency-bindings");
    let package = f.root.0.join("packages/dependency");
    fs::create_dir_all(package.join("src")).unwrap();
    fs::write(package.join("mod.json"), r#"{"id":"dependency","name":"dependency","version":"1.0.0","capabilities":["patch","export"]}"#).unwrap();
    // Obtain the real sandbox through the real production require cache, before instrumentation.
    fs::write(package.join("src/mod.lua"), "return _G").unwrap();
    fs::write(package.join("src/alias.lua"), include_str!("m6_alias.lua")).unwrap();
    let export = f.root.0.join("export");
    fs::create_dir_all(&export).unwrap();
    let environment = mod_loader::ModEnvironment::load_from(
        &f.game,
        f.game.join("kiln/eml/cache"),
        vec![package],
    )
    .unwrap();
    let app = AppState::new(
        environment.clone(),
        RunArgs {
            file_name: fixture::archive::STEM.into(),
            options: RunOptions {
                skip_cache: true,
                is_server: Some(false),
                patch: true,
                export: true,
                export_dir: Some(export.clone().into_std_path_buf()),
                ..Default::default()
            },
        },
        &CacheDiff::new_dirty(),
    )
    .unwrap();
    let runner = LuaModRunner::new(app).unwrap();
    runner.setup(environment.mod_registry()).unwrap();
    let env: Table = runner
        .state
        .require("mods.dependency", &runner.lua)
        .unwrap()
        .as_table()
        .unwrap()
        .clone();
    // The inspector is only in the host's globals, not the package environment. The shared
    // prelude captures it; no debug library or metatable inspector is exposed to fixture Lua.
    let inspector = runner
        .lua
        .create_function(|lua, value: Value| unsafe {
            // Same-VM protected raw access. lua_getmetatable returns the actual protected table;
            // no pointers/objects are shared between the two dependency graphs or VMs.
            lua.exec_raw::<Value>(value, |state| {
                if mlua::ffi::lua_getmetatable(state, 1) == 0 {
                    mlua::ffi::lua_pushnil(state);
                }
                mlua::ffi::lua_remove(state, 1);
            })
        })
        .unwrap();
    runner
        .lua
        .globals()
        .set("__sfi_getmetatable", inspector)
        .unwrap();
    let t: Table = runner
        .lua
        .load(include_str!("m6_prelude.lua"))
        .set_name("=prelude")
        .eval()
        .unwrap();
    runner
        .lua
        .globals()
        .raw_remove("__sfi_getmetatable")
        .unwrap();
    env.raw_set("T", t.clone()).unwrap();
    let result = runner
        .lua
        .load(include_str!("m6_bindings.lua"))
        .set_name("=fixture")
        .set_environment(env)
        .exec();
    if let Err(error) = &result {
        t.get::<Function>("fail_fixture")
            .unwrap()
            .call::<()>(error.clone())
            .unwrap();
    }
    let text: String = t.get::<Function>("transcript").unwrap().call(()).unwrap();
    let value: serde_json::Value = serde_json::from_str(&text).unwrap();
    fixture::save_observation("bindings-v2", &value);
    let mut raw = std::collections::BTreeMap::new();
    if export.join("raw").exists() {
        for file in fs::read_dir(export.join("raw")).unwrap() {
            let file = file.unwrap();
            raw.insert(
                file.file_name().to_string_lossy().into_owned(),
                fs::read_to_string(file.path()).unwrap(),
            );
        }
    }
    fixture::save_observation("bindings-raw-errors", &serde_json::to_value(raw).unwrap());
    assert!(result.is_ok(), "fixture execution failed: {result:?}");
    assert_eq!(value["version"], 2);
    assert!(value["records"].as_array().unwrap().len() > 80);
    assert_eq!(
        value["records"].as_array().unwrap().last().unwrap()["label"],
        "completed"
    );
    let missing = value["records"]
        .as_array()
        .unwrap()
        .iter()
        .find(|r| r["label"] == "types-argument-missing")
        .unwrap();
    assert!(
        missing["message"]
            .as_str()
            .unwrap()
            .contains("bad argument #1"),
        "trusted inspection must capture real mlua error text"
    );
}
