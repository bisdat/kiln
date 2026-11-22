// SPDX-License-Identifier: MIT
//! The texture family's decode and encode fixtures (tests/conformance/texture) through sfi-core's own runner,
//! against their oracle goldens: the Lua side of the parity that Kiln.Api's GameTexture tests check from C#
//! (managed/tests/Kiln.Api.Tests/GameTextureConformanceTests.cs) against the same goldens.
use sfi_core::{
    conformance::{GoldenMatch, compare_golden, plain_std_environment, run_fixture},
    eml,
    lua::install_namespace,
};

fn environment(lua: &mlua::Lua) -> mlua::Result<mlua::Table> {
    let env = plain_std_environment(lua)?;
    env.raw_set("builtin", lua.create_table()?)?;
    install_namespace(&env, "buffer", eml::buffer::create(lua)?)?;
    install_namespace(&env, "hasher", eml::hasher::create(lua)?)?;
    eml::image::install(lua, &env)?;
    Ok(env)
}

fn check(family: &str, case: &str) {
    let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/conformance")
        .join(family);
    let read = |name: String| {
        std::fs::read_to_string(dir.join(&name)).unwrap_or_else(|e| panic!("{name}: {e}"))
    };
    let fixture = read(format!("{case}.lua"));
    let golden = read(format!("{case}.golden.json"));
    let actual = run_fixture(&fixture, environment).unwrap();
    assert_eq!(
        compare_golden(&golden, &actual).unwrap_or_else(|e| panic!("{family}/{case}: {e}")),
        GoldenMatch::MessageVerifiedV2,
        "{family}/{case}"
    );
}

#[test]
fn texture_decode_matches_its_golden() {
    check("texture", "decode");
}

#[test]
fn texture_encode_matches_its_golden() {
    check("texture", "encode");
}
