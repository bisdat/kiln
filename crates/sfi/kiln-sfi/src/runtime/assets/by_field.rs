// SPDX-License-Identifier: MIT
// the interpreter's game.assets loaded-resource index members.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/resources/index_loaded_resources.rs (ScalarField, request,
// prepare, get, tick), lua/ingame/resources/mod.rs:16-54, ingame/update_ingame_runtime.rs:48-50 (the runner),
// session/loaded_resources.rs:14-46 (the index invalidation of a write), session/record_runtime_state.rs:12-14 (a stopped
// owner leaves its jobs), game-runtime/bridge/windows/resources/
// read_resource_scalars.rs:6-89 (resource_scalar_batch and its full-read fallback).
//! `game.assets.prepare_loaded_index(type, field_path)` and `game.assets.get_loaded_resource_by_field(type,
//! field_path, value)` over the VM's [`super::index::Indexes`], and the runner [`tick`] that advances every job once
//! per worker turn (the family's runtime hook, [`super::index_family`]).
//!
//! The field is one aligned 32-bit scalar (an integer, a finite float or an enum, through typedefs) at a fixed offset
//! of the type's reflected root. The jobs read it through kiln.assets' `loaded_list` and `loaded_snapshot`: Kiln has
//! no scalar batch member, so every batch is ShroudForge's fallback, one complete snapshot per resource.

use super::index::{Lookup, Progress, Projection, ResourceId, Selector};
use super::resource::LoadedResource;
use super::{Assets, InlineField, Read};
use kiln_abi::services::kiln_assets::KilnAssetKey;
use mlua::{Lua, Table, Value};
use sfi_core::bindings::runtime::RuntimeHost;
use sfi_core::lua::FunctionArgs;
use std::collections::HashSet;
use std::rc::Rc;
use std::time::Instant;

/// ShroudForge's texts (index_loaded_resources.rs, read_resource_scalars.rs).
pub mod text {
    pub const PATH: &str = "indexed field path must contain 1 to 1024 bytes";
    pub const ALIGNED: &str = "loaded resource indexes require an aligned 32-bit scalar field";
    pub const CYCLIC: &str = "cyclic indexed scalar typedef";
    pub const UNRESOLVED: &str = "unresolved indexed scalar type";
    pub const STORAGE: &str = "indexed scalar typedef has an inconsistent storage size";
    pub const NO_INNER: &str = "indexed typedef has no value type";
    pub const KINDS: &str =
        "loaded resource indexes support 32-bit integers, finite floats and enums";
    pub const NO_ROOT: &str = "unresolved resource type";
    pub const CODEC: &str = "indexed scalar codec did not produce four bytes";
    pub const SMALL: &str = "indexed resource snapshot is smaller than its reflected root";
    pub const OUTSIDE: &str = "indexed scalar is outside resource snapshot";
    pub const WIDTH: &str = "indexed scalar has an invalid width";
    pub const SESSION: &str = "world session changed during loaded resource indexing";
    pub const PHASE: &str = "loaded ResourceService access is available only in the ingame phase";
    pub const NO_TYPE: &str = "indexed resource type is absent from the active reflection";
    pub const PREPARE_ARGS: &str =
        "prepare_loaded_index expects a type and fixed inline scalar field path";
    pub const GET_ARGS: &str =
        "get_loaded_resource_by_field expects a type, fixed inline scalar field path and value";
    pub const QUEUED: &str = "loaded resource index preparation is queued";
    // read_resource_scalars.rs
    pub const BATCH_SELECTION: &str = "invalid resource scalar batch selection";
    pub const BATCH_BEFORE: &str = "world session changed before resource scalar batch";
    pub const BATCH_SMALL: &str = "resource scalar snapshot is smaller than reflected root";
    pub const BATCH_OUTSIDE: &str = "resource scalar outside current snapshot";
    pub const BATCH_FALLBACK: &str = "world session changed during resource scalar fallback";
}

impl From<&KilnAssetKey> for ResourceId {
    fn from(key: &KilnAssetKey) -> Self {
        Self {
            guid: key.guid,
            type_hash: key.r#type,
            part: key.part,
        }
    }
}

impl ResourceId {
    pub fn key(&self) -> KilnAssetKey {
        KilnAssetKey {
            guid: self.guid,
            r#type: self.type_hash,
            part: self.part,
        }
    }
}

/// ShroudForge's `ScalarField`: the inline field, its scalar kind after typedefs, and the root's size.
pub struct ScalarField {
    field: Rc<InlineField>,
    kind: &'static str,
    root_size: usize,
}

impl ScalarField {
    /// ShroudForge's `from_field` (index_loaded_resources.rs:27-71).
    pub fn from_field(
        assets: &Assets,
        root: usize,
        field: Rc<InlineField>,
    ) -> Result<Self, String> {
        let registry = assets.types();
        let range = field.byte_range();
        if range.len() != 4 || !range.start.is_multiple_of(4) {
            return Err(text::ALIGNED.into());
        }
        let mut index = field.type_index;
        let mut seen = HashSet::new();
        let kind = loop {
            if !seen.insert(index) || seen.len() > 64 {
                return Err(text::CYCLIC.into());
            }
            let ty = registry.get(index).ok_or(text::UNRESOLVED)?;
            if ty.size != 4 {
                return Err(text::STORAGE.into());
            }
            if ty.primitive_type != "Typedef" {
                break ty.primitive_type;
            }
            index = ty.inner_type.ok_or(text::NO_INNER)?;
        };
        if !matches!(kind, "UInt32" | "SInt32" | "Float32" | "Enum") {
            return Err(text::KINDS.into());
        }
        Ok(Self {
            field,
            kind,
            root_size: registry.get(root).ok_or(text::NO_ROOT)?.size as usize,
        })
    }

    /// ShroudForge's `wanted`: `value` encoded as the field's type (the VM's typed values), four bytes.
    pub fn wanted(&self, assets: &Assets, value: &Value) -> Result<[u8; 4], String> {
        let values = assets
            .values()
            .ok_or_else(|| crate::runtime::ecs::NO_VALUES.to_owned())?;
        let range = self.field.byte_range();
        let mut root = vec![0u8; self.root_size.max(range.end)];
        let mut mask = vec![0u8; root.len()];
        self.field.write_into(values, value, &mut root, &mut mask)?;
        let encoded = &root[range];
        super::index::validate_scalar(self.kind, encoded)?;
        encoded.try_into().map_err(|_| text::CODEC.into())
    }

    /// ShroudForge's `key`: the scalar of a complete snapshot.
    pub fn key(&self, bytes: &[u8]) -> Result<[u8; 4], String> {
        if bytes.len() < self.root_size {
            return Err(text::SMALL.into());
        }
        let scalar = bytes.get(self.field.byte_range()).ok_or(text::OUTSIDE)?;
        super::index::validate_scalar(self.kind, scalar)?;
        scalar.try_into().map_err(|_| text::WIDTH.into())
    }
}

/// Session zero remains a supported read-only menu snapshot.
fn require_session(assets: &Assets, expected: u64) -> Result<(), String> {
    if assets.world_session() != expected {
        return Err(text::SESSION.into());
    }
    Ok(())
}

/// ShroudForge's `request` (index_loaded_resources.rs:117-156).
fn request(
    assets: &Assets,
    host: &dyn RuntimeHost,
    args: &FunctionArgs,
) -> Result<(Selector, ScalarField, Projection), String> {
    if host.owner_context().phase() != sfi_core::host::Phase::Runtime {
        return Err(text::PHASE.into());
    }
    let selector = args.get::<Value>(0).map_err(|error| error.to_string())?;
    let root = crate::runtime::types::selectors::resolve(assets.types(), selector)?;
    let path = args.get::<String>(1).map_err(|error| error.to_string())?;
    if path.is_empty() || path.len() > 1024 {
        return Err(text::PATH.into());
    }
    let scalar = ScalarField::from_field(assets, root, assets.inline_field(root, &path)?)?;
    let type_hash = assets
        .types()
        .get(root)
        .map(|ty| ty.qualified_hash)
        .ok_or(text::NO_TYPE)?;
    let projection = Projection {
        type_hash,
        root_size: scalar.root_size,
        offset: scalar.field.byte_range().start,
        kind: scalar.kind,
    };
    Ok((Selector { root, path }, scalar, projection))
}

fn progress_table(lua: &Lua, progress: Progress) -> mlua::Result<Table> {
    let result = lua.create_table()?;
    result.raw_set("session_id", progress.session_id)?;
    result.raw_set("scanned", progress.scanned)?;
    result.raw_set("total", progress.total)?;
    result.raw_set("payload_reads", progress.payload_reads)?;
    result.raw_set("complete", progress.complete)?;
    result.raw_set("phase", progress.phase)?;
    Ok(result)
}

/// `game.assets.prepare_loaded_index(type, field_path)` (index_loaded_resources.rs:160-206): `ready, reason,
/// progress`, or `false, reason, nil`. Idempotent preparation declaration. It survives a later nonzero world
/// session and is removed with its owner, not after a particular gameplay action.
pub fn prepare(
    lua: &Lua,
    assets: &Assets,
    package_id: &str,
    host: Option<&Rc<dyn RuntimeHost>>,
    args: &FunctionArgs,
) -> mlua::Result<(bool, Option<String>, Option<Table>)> {
    let prepare = || -> Result<(Progress, Option<String>), String> {
        if let Some(reason) = super::resource::read_denial(package_id, host) {
            return Err(reason);
        }
        if args.len() != 2 {
            return Err(text::PREPARE_ARGS.into());
        }
        let host = host.expect("a package without a host is denied above");
        let (selector, _, projection) = request(assets, host.as_ref(), args)?;
        let mut indexes = assets.indexes();
        let position = indexes.register(
            assets.world_session(),
            package_id,
            selector,
            projection,
            Instant::now(),
        )?;
        assets.index_owner(package_id, host);
        let progress = indexes.progress(position);
        let reason = if progress.complete {
            None
        } else {
            Some(
                indexes
                    .pending_reason(position)
                    .unwrap_or_else(|| text::QUEUED.into()),
            )
        };
        Ok((progress, reason))
    };
    Ok(match prepare() {
        Ok((progress, reason)) => (
            progress.complete,
            reason,
            Some(progress_table(lua, progress)?),
        ),
        Err(reason) => (false, Some(reason), None),
    })
}

/// `game.assets.get_loaded_resource_by_field(type, field_path, value)` (index_loaded_resources.rs:208-275):
/// `resource, nil, progress`, `nil, reason, progress` (still scanning, not found, ambiguous) or `nil, reason, nil`.
pub fn get(
    lua: &Lua,
    assets: &Rc<Assets>,
    package_id: &str,
    host: Option<&Rc<dyn RuntimeHost>>,
    args: &FunctionArgs,
) -> mlua::Result<(Option<LoadedResource>, Option<String>, Option<Table>)> {
    let mut root = 0;
    let mut lookup = || -> Result<Lookup, String> {
        if let Some(reason) = super::resource::read_denial(package_id, host) {
            return Err(reason);
        }
        if args.len() != 3 {
            return Err(text::GET_ARGS.into());
        }
        let host = host.expect("a package without a host is denied above");
        let (selector, scalar, projection) = request(assets, host.as_ref(), args)?;
        root = selector.root;
        let value = args.get::<Value>(2).map_err(|error| error.to_string())?;
        let wanted = scalar.wanted(assets, value)?;
        let session = assets.world_session();
        let mut indexes = assets.indexes();
        let position =
            indexes.register(session, package_id, selector, projection, Instant::now())?;
        assets.index_owner(package_id, host);
        let result = indexes.lookup(
            position,
            wanted,
            Instant::now(),
            |id| {
                require_session(assets, session)?;
                let bytes = match assets.read(&id.key())? {
                    Read::Bytes(bytes) => bytes,
                    // the bridge's read of a GUID it cannot find
                    Read::Missing => return Err(super::text::RESOURCE_CHANGED.into()),
                };
                require_session(assets, session)?;
                Ok(bytes)
            },
            |bytes| scalar.key(bytes),
        );
        if let Err(reason) = require_session(assets, session) {
            indexes.observe_session(assets.world_session(), Instant::now());
            return Err(reason);
        }
        result
    };
    Ok(match lookup() {
        Ok(result) => {
            let resource = result.resource.map(|(id, bytes)| {
                LoadedResource::from_snapshot(
                    assets.clone(),
                    id.key(),
                    root,
                    result.progress.session_id,
                    bytes,
                )
            });
            (
                resource,
                result.reason,
                Some(progress_table(lua, result.progress)?),
            )
        }
        Err(reason) => (None, Some(reason), None),
    })
}

/// The bridge's `resource_scalar_batch` (read_resource_scalars.rs:6-89) without a provider batch: its selection
/// checks, then one complete snapshot per resource and the scalar at `offset`.
pub fn scalar_batch(
    assets: &Assets,
    ids: &[ResourceId],
    type_hash: u32,
    root_size: usize,
    offset: usize,
    session: u64,
) -> Result<Vec<[u8; 4]>, String> {
    if ids.is_empty()
        || ids.len() > 64
        || !offset.is_multiple_of(4)
        || offset.checked_add(4).is_none_or(|end| end > root_size)
        || ids.iter().any(|id| id.type_hash() != type_hash)
    {
        return Err(text::BATCH_SELECTION.into());
    }
    if assets.world_session() != session {
        return Err(text::BATCH_BEFORE.into());
    }
    let mut values = Vec::with_capacity(ids.len());
    for id in ids {
        let bytes = match assets.read(&id.key())? {
            Read::Bytes(bytes) => bytes,
            Read::Missing => return Err(super::text::RESOURCE_CHANGED.into()),
        };
        if bytes.len() < root_size {
            return Err(text::BATCH_SMALL.into());
        }
        values.push(
            bytes
                .get(offset..offset + 4)
                .ok_or(text::BATCH_OUTSIDE)?
                .try_into()
                .expect("four bytes"),
        );
    }
    if assets.world_session() != session {
        return Err(text::BATCH_FALLBACK.into());
    }
    Ok(values)
}

impl Assets {
    /// Remembers that `owner` declared an index job, with its host.
    fn index_owner(&self, owner: &str, host: &Rc<dyn RuntimeHost>) {
        let mut owners = self.index_owners.borrow_mut();
        if !owners.iter().any(|(id, _)| id == owner) {
            owners.push((owner.to_owned(), host.clone()));
        }
    }
    /// The index owners whose lifecycle is no longer active, forgotten here.
    fn stopped_index_owners(&self) -> Vec<String> {
        let mut stopped = Vec::new();
        self.index_owners.borrow_mut().retain(|(id, host)| {
            let active = host.lifecycle_active();
            if !active {
                stopped.push(id.clone());
            }
            active
        });
        stopped
    }
}

/// ShroudForge's `tick` (index_loaded_resources.rs:279-313): the single runtime owner advances every registered
/// job. No Lua callback or repeated mod lookup is needed to finish its initial index. First a package whose lifecycle
/// stopped leaves its jobs, as ShroudForge's `set_runtime_mod_active(false)` removes the owner
/// (record_runtime_state.rs:12-14): a job no active package declared goes.
pub fn tick(assets: &Assets) {
    let stopped = assets.stopped_index_owners();
    let mut indexes = assets.indexes();
    for owner in &stopped {
        indexes.remove_owner(owner);
    }
    if indexes.is_empty() {
        return;
    }
    let session = assets.world_session();
    indexes.advance(
        session,
        Instant::now(),
        |type_hash| {
            require_session(assets, session)?;
            let resources = assets.list(type_hash)?;
            require_session(assets, session)?;
            Ok(resources.iter().map(ResourceId::from).collect())
        },
        |ids, projection| {
            require_session(assets, session)?;
            let keys = scalar_batch(
                assets,
                ids,
                projection.type_hash,
                projection.root_size,
                projection.offset,
                session,
            )?;
            require_session(assets, session)?;
            Ok(keys)
        },
    );
    if assets.world_session() != session {
        indexes.observe_session(assets.world_session(), Instant::now());
    }
}

/// The two members into one package's `game.assets`.
pub fn install(
    lua: &Lua,
    table: &Table,
    package_id: &str,
    host: Option<Rc<dyn RuntimeHost>>,
    assets: Rc<Assets>,
) -> mlua::Result<()> {
    let (a, id, h) = (assets.clone(), package_id.to_owned(), host.clone());
    table.raw_set(
        "get_loaded_resource_by_field",
        lua.create_function(move |lua, args: FunctionArgs| get(lua, &a, &id, h.as_ref(), &args))?,
    )?;
    let (id, h) = (package_id.to_owned(), host);
    table.raw_set(
        "prepare_loaded_index",
        lua.create_function(move |lua, args: FunctionArgs| {
            prepare(lua, &assets, &id, h.as_ref(), &args)
        })?,
    )?;
    Ok(())
}
