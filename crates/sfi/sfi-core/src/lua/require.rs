// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (both pins): modules/mod-runtime/lua/parse_require_path.rs:25-75;
// modules/mod-runtime/lua/require_mod.rs:16-220; lua/mod_runner.rs:121-134.
use super::{FunctionArgs, LuaError};
use crate::host::Package;
use mlua::{Lua, Table, Value};
use std::{
    cell::RefCell,
    collections::{HashMap, HashSet},
    rc::Rc,
};

struct PackageState {
    package: Package,
    env: Table,
}
#[derive(Default)]
pub(crate) struct State {
    packages: RefCell<HashMap<String, PackageState>>,
    loaded: RefCell<HashMap<String, Value>>,
    loading: RefCell<HashSet<String>>,
}
struct LoadingGuard<'a> {
    loading: &'a RefCell<HashSet<String>>,
    key: String,
}
impl Drop for LoadingGuard<'_> {
    fn drop(&mut self) {
        self.loading.borrow_mut().remove(&self.key);
    }
}

impl State {
    pub fn contains(&self, id: &str) -> bool {
        self.packages.borrow().contains_key(id)
    }
    pub fn add(&self, package: Package, env: Table) {
        self.packages
            .borrow_mut()
            .insert(package.id().into(), PackageState { package, env });
    }
    pub fn environment(&self, id: &str) -> Option<Table> {
        self.packages
            .borrow()
            .get(id)
            .map(|state| state.env.clone())
    }
    fn source(&self, id: &str) -> mlua::Result<(Package, Table)> {
        let packages = self.packages.borrow();
        let state = packages
            .get(id)
            .ok_or_else(|| LuaError::mod_not_found(id))?;
        Ok((state.package.clone(), state.env.clone()))
    }
    pub fn bind_local(self: &Rc<Self>, lua: &Lua, env: &Table, owner: &str) -> mlua::Result<()> {
        let local = LocalState {
            owner: owner.into(),
            loaded: RefCell::default(),
            global: self.clone(),
        };
        env.raw_set(
            "require",
            lua.create_function(move |lua, args: FunctionArgs| {
                local.require(lua, &args.get::<String>(0)?)
            })?,
        )
    }
    fn require_qualified(&self, lua: &Lua, key: &str) -> mlua::Result<Value> {
        if let Some(value) = self.loaded.borrow().get(key) {
            return Ok(value.clone());
        }
        if !self.loading.borrow_mut().insert(key.into()) {
            return Err(LuaError::circular_dependency(key));
        }
        let _loading = LoadingGuard {
            loading: &self.loading,
            key: key.into(),
        };
        let path = RequirePath::qualified(key)?;
        let (package, env) = self.source(path.owner)?;
        let file = if path.path.is_empty() {
            String::new()
        } else {
            format!("src/{}.lua", path.path.replace('.', "/"))
        };
        let function = load(lua, &package, &env, &file, key)?;
        let result = function.call::<Value>(())?;
        self.loaded.borrow_mut().insert(key.into(), result.clone());
        if path.path == "mod" {
            self.loaded
                .borrow_mut()
                .insert(format!("mods.{}", path.owner), result.clone());
        }
        Ok(result)
    }
    pub fn entrypoint(&self, lua: &Lua, id: &str) -> mlua::Result<Value> {
        let key = format!("mods:{id}");
        if let Some(value) = self.loaded.borrow().get(&key) {
            return Ok(value.clone());
        }
        if !self.loading.borrow_mut().insert(key.clone()) {
            return Err(LuaError::circular_dependency(id));
        }
        let _loading = LoadingGuard {
            loading: &self.loading,
            key: key.clone(),
        };
        let (package, env) = self.source(id)?;
        let value = load(lua, &package, &env, "src/mod.lua", id)?.call::<Value>(())?;
        self.loaded.borrow_mut().insert(key, value.clone());
        Ok(value)
    }
    pub fn require(&self, owner: &str, id: &str) -> mlua::Result<Value> {
        let env = self
            .environment(owner)
            .ok_or_else(|| LuaError::mod_not_found(owner))?;
        env.raw_get::<mlua::Function>("require")?.call(id)
    }
}

fn load(
    lua: &Lua,
    package: &Package,
    env: &Table,
    path: &str,
    chunk_name: &str,
) -> mlua::Result<mlua::Function> {
    let bytes = package
        .source
        .read_file(path)
        .map_err(|error| LuaError::module_load(path, error))?;
    // read_to_string uses this I/O error, not FromUtf8Error's different text.
    let source = std::str::from_utf8(&bytes).map_err(|_| {
        LuaError::module_load(
            path,
            std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                "stream did not contain valid UTF-8",
            ),
        )
    })?;
    lua.load(source)
        .set_name(chunk_name)
        .set_environment(env.clone())
        .into_function()
}

struct LocalState {
    owner: String,
    loaded: RefCell<HashMap<String, Value>>,
    global: Rc<State>,
}
impl LocalState {
    fn require(&self, lua: &Lua, id: &str) -> mlua::Result<Value> {
        if let Some(value) = self.loaded.borrow().get(id) {
            return Ok(value.clone());
        }
        if let Some(value) = self.global.loaded.borrow().get(id) {
            return Ok(value.clone());
        }
        let path = RequirePath::parse(id, &self.owner);
        let value = self
            .global
            .require_qualified(lua, &format!("mods.{}.{}", path.owner, path.path))?;
        if path.local {
            self.loaded
                .borrow_mut()
                .insert(path.path.into(), value.clone());
        }
        Ok(value)
    }
}
struct RequirePath<'a> {
    owner: &'a str,
    path: &'a str,
    local: bool,
}
impl<'a> RequirePath<'a> {
    fn parse(id: &'a str, fallback: &'a str) -> Self {
        if let Some(rest) = id.strip_prefix("mods.") {
            let (owner, path) = rest.split_once('.').unwrap_or((rest, "mod"));
            Self {
                owner,
                path,
                local: false,
            }
        } else {
            Self {
                owner: fallback,
                path: id,
                local: true,
            }
        }
    }
    fn qualified(id: &'a str) -> mlua::Result<Self> {
        if id.starts_with("mods.") {
            Ok(Self::parse(id, ""))
        } else {
            Err(LuaError::module_not_found(id))
        }
    }
}
