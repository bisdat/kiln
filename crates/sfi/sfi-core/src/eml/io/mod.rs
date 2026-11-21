// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (byte-identical bindings in 1.7.9 d8a394bc and 1.8.0 38ef448d):
// modules/mod-runtime/lua/bindings/io/{mod.rs:22-59,read_mod_files.rs:2-94,
// path_names.rs:2-52,manage_exports.rs:2-239}.
//! Owner-bound package IO and explicitly configured exports. No path discovery,
//! game services, process CWD fallback, archive reader or installation-tree access.
//!
//! Parity notes: reads return raw Lua bytes, not UTF-8 text; list_files preserves
//! source order; parent returns an empty string, not nil; export returns NO values.
//! Invalid host roots raise instead of panicking. Export containment is
//! checked before parent creation; pinned/no-follow operations refuse replacement
//! races and escaping links. Writes/copies and rename sources follow contained
//! links; rename destinations retain the final entry, as SF does. Copy/rename
//! commits are no-clobber, unlike SF's check-then-mutate race. A dangling rename
//! destination is privately staged and verified before replacement; a concurrent
//! restore conflict retains the displaced entry at the reported recovery path.
//! These changes only refuse escapes/races; stable in-sandbox paths are unchanged.
mod secure;
mod windows_path;

use super::buffer::{Buffer, ByteView};
use crate::{
    host::{HostContext, LogLevel, Package},
    lua::{FunctionArgs, LuaError, install_namespace},
};
use mlua::{Lua, Table};
use std::{
    io,
    path::{Path, PathBuf},
    rc::Rc,
};

/// What the export-root checks (secure.rs) ask the loader once per process, asked now: on Windows Wine's
/// `wine_get_unix_file_name`, looked up through GetModuleHandleW and GetProcAddress, which Wine's loader answers under
/// the loader lock (a package DLL's initializer can hold it). A host that runs this code while such an initializer may
/// still run (kiln-eml's startup pass in a kiln-mode start) calls it first, in its early phase. True under Wine or Proton;
/// false on native Windows and every other system. Idempotent.
pub fn prime_export_checks() -> bool {
    secure::prime()
}

/// Selected by the host before executing Lua. This is the GLOBAL export switch,
/// not loader.features.export (which additionally checks the package capability).
#[derive(Clone, Debug, Default)]
pub struct ExportConfig {
    pub enabled: bool,
    pub root: Option<PathBuf>,
}

pub fn install(
    lua: &Lua,
    env: &Table,
    package: &Package,
    context: &HostContext,
    exports: ExportConfig,
) -> mlua::Result<()> {
    install_namespace(env, "io", create(lua, package, context, exports)?)
}
pub fn create(
    lua: &Lua,
    package: &Package,
    context: &HostContext,
    exports: ExportConfig,
) -> mlua::Result<Table> {
    let table = lua.create_table()?;
    for (name, as_string) in [("read", false), ("read_to_string", true)] {
        let source = package.source.clone();
        table.raw_set(
            name,
            lua.create_function(move |lua, args: FunctionArgs| {
                let path = args.get::<String>(0)?;
                let bytes = source.read_file(&path).map_err(mlua::Error::external)?;
                if as_string {
                    Ok(mlua::Value::String(lua.create_string(bytes)?))
                } else {
                    Ok(mlua::Value::UserData(
                        lua.create_userdata(Buffer::wrap(bytes))?,
                    ))
                }
            })?,
        )?;
    }
    let source = package.source.clone();
    let log = context.log.clone();
    let owner = package.id().to_string();
    table.raw_set(
        "list_files",
        lua.create_function(move |_, args: FunctionArgs| {
            let path = args.get::<String>(0)?;
            let mut entries = Vec::new();
            for entry in source.read_dir(&path).map_err(mlua::Error::external)? {
                match entry {
                    Ok(entry) => entries.push(entry.path.replace('\\', "/")),
                    Err(error) => {
                        if let Some(log) = &log
                            && log.enabled(LogLevel::Warn)
                        {
                            log.write(
                                &owner,
                                LogLevel::Warn,
                                format!("failed to read directory: {error}; path={path}")
                                    .as_bytes(),
                            );
                        }
                    }
                }
            }
            Ok(entries)
        })?,
    )?;
    for name in ["exists", "is_file", "is_directory"] {
        let source = package.source.clone();
        table.raw_set(
            name,
            lua.create_function(move |_, args: FunctionArgs| {
                let path = args.get::<String>(0)?;
                Ok(match name {
                    "exists" => source.exists(&path),
                    "is_file" => source.is_file(&path),
                    _ => source.is_directory(&path),
                })
            })?,
        )?;
    }
    for name in ["name", "name_without_extension", "extension", "parent"] {
        table.raw_set(
            name,
            lua.create_function(move |_, args: FunctionArgs| {
                let path = args.get::<String>(0)?;
                Ok(match name {
                    "name" => windows_path::name(&path).to_string(),
                    "name_without_extension" => windows_path::stem(&path).to_string(),
                    "extension" => windows_path::extension(&path).to_string(),
                    _ => windows_path::parent(&path),
                })
            })?,
        )?;
    }
    table.raw_set(
        "join",
        lua.create_function(|_, args: FunctionArgs| {
            let parts = (0..args.len())
                .map(|i| args.get::<String>(i))
                .collect::<mlua::Result<Vec<_>>>()?;
            Ok(windows_path::join(&parts))
        })?,
    )?;
    let exports = Rc::new(SelectedExports {
        enabled: exports.enabled,
        root: exports
            .root
            .map(|path| secure::select_root(&path).map_err(|e| e.to_string()))
            .unwrap_or_else(|| Err("export root is not configured".into())),
    });
    let config = exports.clone();
    table.raw_set(
        "export",
        lua.create_function(move |_, args: FunctionArgs| {
            config.gate(true)?;
            let name = args.get::<String>(0)?;
            let bytes = args.get::<ByteView>(1)?;
            let configured = config.path()?;
            let relative = sanitize(&name, "invalid path")?;
            let original = configured.join(&relative);
            if original.exists() && !original.is_file() {
                return Err(LuaError::generic("path exists but is not a file"));
            }
            if relative.as_os_str().is_empty() {
                // SF joins an empty path with a trailing separator. Windows refuses
                // this write, creating only the configured root's PARENTS, not root.
                let parent = configured
                    .parent()
                    .ok_or_else(|| LuaError::generic("export path has no parent directory"))?;
                let anchor = secure::Root::anchor(parent)
                    .map_err(|e| error("failed to create parent directories", e))?;
                let name = PathBuf::from(
                    configured
                        .file_name()
                        .ok_or_else(|| LuaError::generic("export path has no parent directory"))?,
                )
                .join("");
                return anchor
                    .write_name(&name, bytes.as_bytes()?)
                    .map_err(|e| error("failed to write file", e));
            }
            // Opening an existing root first permits full inspection before creating anything.
            let root = secure::Root::open(config.path()?, true)
                .map_err(|e| error("failed to create parent directories", e))?;
            let path = resolve(&root, &relative, "failed to write file")?;
            root.parent(&path, true)
                .map_err(|e| error("failed to create parent directories", e))?;
            root.write(&path, bytes.as_bytes()?)
                .map_err(|e| error("failed to write file", e))
        })?,
    )?;
    let config = exports.clone();
    table.raw_set(
        "export_exists",
        lua.create_function(move |_, args: FunctionArgs| {
            config.gate(false)?;
            let relative = sanitize(&args.get::<String>(0)?, "invalid path")?;
            if !config.path()?.join(&relative).exists() {
                return Ok(false);
            }
            let root = config.open(false)?;
            resolve(&root, &relative, "export path could not be inspected")?;
            Ok(true)
        })?,
    )?;
    let config = exports.clone();
    table.raw_set(
        "read_export_to_string",
        lua.create_function(move |lua, args: FunctionArgs| {
            use io::Read;
            config.gate(false)?;
            let relative = sanitize(&args.get::<String>(0)?, "invalid export path")?;
            let root = config.open(false)?;
            let path = resolve(&root, &relative, "export file could not be opened")?;
            let path = path
                .canonicalize()
                .map_err(|e| error("export file could not be opened", e))?;
            require_file(&root, &path)?;
            let mut file = root
                .read(&path)
                .map_err(|e| error("export file could not be read", e))?;
            let mut bytes = Vec::new();
            file.read_to_end(&mut bytes)
                .map_err(|e| error("export file could not be read", e))?;
            lua.create_string(bytes)
        })?,
    )?;
    let config = exports.clone();
    table.raw_set(
        "export_list",
        lua.create_function(move |_, args: FunctionArgs| {
            config.gate(false)?;
            let root = config.open(true)?;
            let relative = sanitize(&args.get::<String>(0)?, "invalid export path")?;
            let original = root.path.join(&relative);
            if !original.exists() {
                return Ok(Vec::new());
            }
            let directory = resolve(&root, &relative, "export directory could not be inspected")?;
            if !directory.is_dir() {
                return Err(LuaError::generic(
                    "export path must name a directory inside the configured export directory",
                ));
            }
            let mut entries = Vec::new();
            let (_directory_guard, listing) = root
                .entries(&directory)
                .map_err(|e| error("export directory could not be listed", e))?;
            for entry in listing {
                let entry = entry.map_err(|e| error("export entry could not be read", e))?;
                let Ok(resolved) = entry.path().canonicalize() else {
                    continue;
                };
                if let Ok(relative) = resolved.strip_prefix(&root.path) {
                    entries.push(relative.to_string_lossy().replace('\\', "/"));
                }
            }
            entries.sort();
            Ok(entries)
        })?,
    )?;
    for name in ["export_copy", "export_rename", "export_delete"] {
        let config = exports.clone();
        table.raw_set(
            name,
            lua.create_function(move |_, args: FunctionArgs| {
                config.gate(false)?;
                let root = config.open(true)?;
                let relative = sanitize(&args.get::<String>(0)?, "invalid export path")?;
                let path = resolve(&root, &relative, "export file could not be inspected")?;
                let source = path
                    .canonicalize()
                    .map_err(|e| error("export file could not be inspected", e))?;
                require_file(&root, &source)?;
                if name == "export_delete" {
                    return root
                        .delete(&source)
                        .map_err(|e| error("export file could not be deleted", e));
                }
                let destination = sanitize(&args.get::<String>(1)?, "invalid export path")?;
                if destination.as_os_str().is_empty() {
                    return Err(LuaError::generic(if root.path.parent().is_none() {
                        "export path has no parent directory"
                    } else {
                        "export path resolves outside the configured export directory"
                    }));
                }
                // Rename replaces the leaf entry; copy follows it. Both inspect
                // existing parents before creating any directories.
                let destination = if name == "export_rename" {
                    root.rename_destination(&destination)
                        .map_err(|e| resolve_error("export directory could not be inspected", e))?
                } else {
                    resolve(
                        &root,
                        &destination,
                        "export directory could not be inspected",
                    )?
                };
                root.parent(&destination, true)
                    .map_err(|e| error("export directory could not be created", e))?;
                if destination.exists() {
                    return Err(LuaError::generic("destination export file already exists"));
                }
                if name == "export_copy" {
                    root.copy(&source, &destination)
                        .map_err(|e| error("export file could not be copied", e))
                } else {
                    root.rename(&source, &destination)
                        .map_err(|e| error("export file could not be renamed", e))
                }
            })?,
        )?;
    }
    Ok(table)
}
struct SelectedExports {
    enabled: bool,
    root: Result<PathBuf, String>,
}
impl SelectedExports {
    fn gate(&self, write: bool) -> mlua::Result<()> {
        if self.enabled {
            Ok(())
        } else {
            Err(LuaError::generic(if write {
                "exporting is disabled"
            } else {
                "export access is disabled"
            }))
        }
    }
    fn path(&self) -> mlua::Result<&Path> {
        self.root
            .as_ref()
            .map(|path| path.as_path())
            .map_err(|e| LuaError::generic(format!("export directory is unavailable: {e}")))
    }
    fn open(&self, create: bool) -> mlua::Result<secure::Root> {
        secure::Root::open(self.path()?, create)
            .map_err(|e| error("export directory is unavailable", e))
    }
}
fn error(prefix: &str, e: io::Error) -> mlua::Error {
    LuaError::generic(format!("{prefix}: {e}"))
}
fn sanitize(path: &str, prefix: &str) -> mlua::Result<PathBuf> {
    windows_path::sanitize(path).map_err(|e| error(prefix, e))
}
fn resolve(root: &secure::Root, path: &Path, prefix: &str) -> mlua::Result<PathBuf> {
    root.resolve(path).map_err(|e| resolve_error(prefix, e))
}
fn resolve_error(prefix: &str, e: io::Error) -> mlua::Error {
    if e.to_string() == "export path resolves outside the configured export directory" {
        LuaError::generic(e.to_string())
    } else {
        error(prefix, e)
    }
}
fn require_file(root: &secure::Root, path: &Path) -> mlua::Result<()> {
    if !path.starts_with(&root.path) || !path.is_file() {
        Err(LuaError::generic(
            "export path must name a file inside the configured export directory",
        ))
    } else {
        Ok(())
    }
}
