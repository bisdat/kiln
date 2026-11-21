// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (both pins): modules/mod-runtime/lua/bindings/mod.rs:88-115 (`register`) and
// lua/create_sandbox.rs:31-125 (the base environment LuaVm::add_package builds).
//! The one module composition a phase host installs into each package environment.
//!
//! `LuaVm::add_package` already builds SF's base environment (copied stdlib, `buffer`,
//! `hasher`, `game.guid`, `image` with its texture extension). [`install`] adds the rest
//! of SF's `register` in its order: `io`, `integer`, `game.types` (from the registry
//! port), `runtime` (the registrar), then the common `shroudforge`/`loader`/logging bindings.
//! Every host (the interpreter worker, the conformance host) calls this function; there
//! is no second composition.
//!
//! Without a [`GameTypes`] port there is no `game.types`, and without a runtime host no
//! `runtime` namespace: absence, never an empty stand-in table. A runtime host without
//! `game.types` is a setup error (the registrar installs the phase host's real table).
use crate::{
    bindings::{
        self, BindingTables, OwnerBindings,
        common::namespace,
        runtime::{RuntimeHost, register_runtime},
    },
    eml::{
        integer,
        io::{self, ExportConfig},
    },
    host::{HostContext, Package},
    lua::install_namespace,
};
use mlua::{Lua, Table, Value};
use std::rc::Rc;

/// The phase's registry port for `game.types` (the registrar). Production: the archive registry.
pub trait GameTypes {
    /// The real `game.types` table for `owner`, built on the worker during registration.
    fn types(&self, lua: &Lua, owner: &Package) -> mlua::Result<Table>;
}

/// Per-package inputs of the composition. Built by the host before any Lua runs.
pub struct Composition {
    pub owner: OwnerBindings,
    /// The global export switch and this package's export root.
    pub exports: ExportConfig,
    pub game_types: Option<Rc<dyn GameTypes>>,
    /// The package's runtime host; requires `game_types`.
    pub runtime: Option<Rc<dyn RuntimeHost>>,
}

/// The tables the composition installed, for family extensions and the lifecycle.
pub struct Composed {
    pub bindings: BindingTables,
    pub game: Table,
    pub runtime: Option<Table>,
}

/// Registrar body for `LuaVm::add_package`: runs before any package code executes.
pub fn install(
    lua: &Lua,
    env: &Table,
    package: &Package,
    context: &HostContext,
    composition: Composition,
) -> mlua::Result<Composed> {
    let Composition {
        owner,
        exports,
        game_types,
        runtime,
    } = composition;
    io::install(lua, env, package, context, exports)?;
    install_namespace(env, "integer", integer::create(lua)?)?;
    let game = namespace(lua, env, "game")?;
    if let Some(port) = game_types {
        if !game.raw_get::<Value>("types")?.is_nil() {
            return Err(mlua::Error::runtime("game.types is already installed"));
        }
        game.raw_set("types", port.types(lua, package)?)?;
    }
    let runtime = match runtime {
        Some(host) => Some(register_runtime(lua, env, package, context, &game, host)?),
        None => None,
    };
    let bindings = bindings::install(lua, env, package, context, owner)?;
    Ok(Composed {
        bindings,
        game,
        runtime,
    })
}
