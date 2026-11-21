// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (byte-identical in both pins): modules/mod-runtime/lua/ingame/
// set_profile_patch_enabled.rs:3-55 (runtime.patch), check_ingame_access.rs:51-56
// (runtime.gameplay.patch), lua/ingame/mod.rs:78; game-runtime/bridge/windows/patches/
// check_runtime_patch.rs, set_runtime_patch_enabled.rs; native/patches/set_runtime_patch.cpp:
// 204-211; native/profiles/read_game_profile.cpp:207-240 (runtimePatches).
//! `runtime.patch.available` / `.set_enabled` and the `runtime.gameplay.patch`
//! operation, refusal-only on every current profile.
//!
//! **Why refusal-only.** A profile patch exists only where the build profile has a
//! `runtimePatches` section; no profile of a build from 25966634 on has one, in either pin.
//! So `runtime_patch_available(name)` is false for every name (and for
//! `runtime.gameplay.patch`, which asks whether any patch is available), and ShroudForge's
//! `set_enabled` stops at `build profile does not resolve <name>` before its native call.
//! Kiln answers the same after the same checks, in ShroudForge's order, with no Kiln member.
//!
//! **The catalog**: `runtime.gameplay.patch` is in ShroudForge's contract on every build,
//! so its row is [`Backing::Interpreter`](super::operation_catalog::Backing::Interpreter);
//! its provider fact has no row (false): `status` says available, `has` says no and
//! `get_operations` says the provider is not ready, as ShroudForge does on a current build.

use super::install::Family;
use mlua::Table;
use sfi_core::bindings::runtime::{RuntimeHost, RuntimeOperation as Op, runtime_denial_reason};
use sfi_core::lua::{FunctionArgs, LuaError};

/// The runtime family's name.
pub const FAMILY: &str = "patch";

/// It owns ShroudForge's whole `runtime.patch` table.
pub const CLAIMS: [&str; 1] = ["runtime.patch"];

/// `runtime.patch.available(name)`: the name is converted first; denied or not, no current
/// profile resolves a patch.
pub fn available(host: &dyn RuntimeHost, args: &FunctionArgs) -> mlua::Result<bool> {
    args.get::<String>(0)?;
    // Denied: false before the provider is asked. Not denied: the provider answers false for
    // every name on a current profile.
    let _denied = runtime_denial_reason(host, Op::GameplayPatch).is_some();
    Ok(false)
}

/// `runtime.patch.set_enabled(name, enabled)`: denial, arity, name, enabled, then
/// ShroudForge's unresolved-patch reason.
pub fn set_enabled(
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> mlua::Result<(bool, Option<String>)> {
    if let Some(reason) = runtime_denial_reason(host, Op::GameplayPatch) {
        return Ok((false, Some(reason)));
    }
    if args.len() != 2 {
        return Err(LuaError::generic(
            "runtime.patch.set_enabled expects profile patch name and enabled:boolean",
        ));
    }
    let name = args.get::<String>(0)?;
    args.get::<bool>(1)?;
    Ok((
        false,
        Some(format!("build profile does not resolve {name}")),
    ))
}

/// The family: each package's `runtime.patch` table over its own registrar host.
pub fn family() -> Family {
    Family::new(FAMILY, &CLAIMS, |lua, _, _, composed, cx| {
        let Some(runtime) = &composed.runtime else {
            return Ok(());
        };
        let host = cx.package_host().ok_or_else(|| {
            mlua::Error::runtime("runtime.patch needs the package's runtime host")
        })?;
        let patch: Table = lua.create_table()?;
        let h = host.clone();
        patch.raw_set(
            "available",
            lua.create_function(move |_, args: FunctionArgs| available(h.as_ref(), &args))?,
        )?;
        patch.raw_set(
            "set_enabled",
            lua.create_function(move |_, args: FunctionArgs| set_enabled(host.as_ref(), &args))?,
        )?;
        runtime.raw_set("patch", patch)
    })
}
