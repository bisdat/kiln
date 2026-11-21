// SPDX-License-Identifier: MIT
// the interpreter's ECS registry join.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (identical in both pins): modules/game-runtime/native/ecs/registry/refresh_component_registry.cpp:63-77
// (the parser type contract every registration must meet), :100-102 (the resolved storage types), :148-170 (the
// report), :243-258 (its unavailable form); modules/game-runtime/bridge/windows/ecs/read_component_registry.rs:3-13 and
// bridge/windows/provider/read_native_catalog.rs:3-7,14-41 (the provider's texts and its bounded re-reads).
//! The engine's component registrations, joined with the archive registry.
//!
//! ShroudForge's provider publishes one registry report (`runtime_provider::registry()`) and a map of resolved
//! storage types (`runtime_provider::resolve(name)`). Kiln has both facts in two places:
//! - kiln.types' `ecs_registry` (kiln_types.h): the engine's registration catalog as copied rows (registration
//!   index, component hash, reflected size, the type's identity in kiln.types, tracking flags);
//! - the archive registry ([`Registry`]): the reflected types, the authority for names, sizes and layouts.
//!
//! [`EcsRegistry::read`] joins them, with a proof per row: the row's type, named through kiln.types
//! (`type_at(type_identity)` + `type_info`), must be a `keen::ecs::` type of the archive registry with exactly that
//! qualified name, its qualified hash must be the row's component hash, and the archive's size, kiln.types' size and
//! the row's size must be one number. A row that fails it is ShroudForge's contract refusal: the registry report is
//! unavailable with `engine registration disagrees with the parser type contract: <name>`
//! ([`text::DISAGREES`]) and that row resolves nothing. Rows whose proof holds still resolve their storage type, as
//! ShroudForge's provider resolves its known layouts before (or without) a valid registry.
//!
//! What the rows cannot say yet (until `kiln.types.ecs_registration`): callbacks and their slots,
//! template (configuration) roles, the storage flags. Until then every joined row is a registration whose storage
//! is its own type (role `runtime`) and the catalog lists no template role ([`super::catalog`] fills the
//! registration detail fields as partial).
//!
//! Every call is a direct any-thread kiln.types member through the service adapter: no hop, no wait. A
//! catalog that changed during a read is read again, at most four times in all (ShroudForge's own bound). The joined
//! snapshot is kept while kiln.types reports the same layout stamp and count, so each metadata call costs one sizing
//! call once the catalog was read.

use super::super::services::ServiceAdapter;
use super::super::types::Registry;
use crate::service_member;
use kiln_abi::services::kiln_types::*;
use kiln_abi::services::kiln_world::{KILN_ECS_ERR_UNREADABLE, KILN_ECS_OK};
use kiln_abi::{KILN_ERR_BUFFER_TOO_SMALL, KILN_ERR_UNAVAILABLE, KILN_OK};
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;

/// ShroudForge's texts.
pub mod text {
    /// `runtime_provider::registry()` without the provider's registry extension (read_component_registry.rs:7-9).
    pub const LACKS: &str =
        "Native provider lacks the component registry extension. Install the matching provider";
    /// A report without a reason of its own (read_component_registry.rs:118-121).
    pub const UNAVAILABLE: &str = "component registry unavailable";
    /// read_native_catalog.rs:3-7.
    pub const INVALID_SIZE: &str = "invalid native catalog response size";
    pub const INVALID_RESPONSE: &str = "invalid component registry response";
    pub const CHANGED: &str = "component registry changed repeatedly during read";
    /// refresh_component_registry.cpp:70, followed by the type's qualified name.
    pub const DISAGREES: &str = "engine registration disagrees with the parser type contract: ";
}

/// The reflected type namespace every registration's type must be in (ShroudForge's provider takes no other name
/// into its type contract, refresh_component_registry.cpp:196-203).
pub const ECS_PREFIX: &str = "keen::ecs::";

/// The most rows read (ShroudForge reads at most 8 MiB of report; kiln.types lists about 700).
pub const MAX_ROWS: u32 = 1 << 16;

/// One registration whose row met the proof.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Registration {
    /// The engine registration index (the row's position in the catalog), not a type index.
    pub index: u32,
    /// The component hash: the qualified hash of its type.
    pub qualified_hash: u32,
    /// The archive registry index of its storage type.
    pub type_index: usize,
    /// The storage size in bytes.
    pub runtime_size: u32,
    /// kiln.types' raw row flags (`KILN_ECS_REGISTRY_*`), passed on unsigned.
    pub flags_bits: u32,
}

/// A storage type the join resolved: ShroudForge's `resolve(name)`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Resolved {
    pub type_index: usize,
    pub size: u32,
    pub component: u32,
}

/// One read of the catalog, joined.
#[derive(Debug)]
pub struct Snapshot {
    stamp: u64,
    count: u32,
    report: Result<Vec<Registration>, String>,
    resolved: HashMap<String, Resolved>,
}

impl Snapshot {
    fn unavailable(reason: &str) -> Self {
        Self {
            stamp: 0,
            count: 0,
            report: Err(reason.to_owned()),
            resolved: HashMap::new(),
        }
    }
    /// The registry report's rows, or why the report is unavailable.
    pub fn available(&self) -> Result<&[Registration], &str> {
        self.report.as_deref().map_err(String::as_str)
    }
    /// ShroudForge's `runtime_provider::resolve(name)`: the storage type of that exact name, when a row of that type
    /// met the proof (the first such row).
    pub fn resolve(&self, name: &str) -> Option<Resolved> {
        self.resolved.get(name).copied()
    }
    /// The number of distinct storage types resolved (the report's `runtime_type_count`).
    pub fn runtime_type_count(&self) -> usize {
        self.resolved.len()
    }
}

/// The catalog of one world kind, read on demand on the worker and kept per layout stamp.
pub struct EcsRegistry {
    world_kind: u32,
    cached: RefCell<Option<Rc<Snapshot>>>,
}

impl EcsRegistry {
    pub fn new(world_kind: u32) -> Self {
        Self {
            world_kind,
            cached: RefCell::new(None),
        }
    }

    /// ShroudForge's `runtime_provider::registry()` and `resolve`: `Err` (with ShroudForge's text) when kiln.types or
    /// a member the read needs is not there, or the read failed (an invalid size or response, a catalog that kept
    /// changing: ShroudForge's fetch errors); otherwise the joined snapshot, whose report may be unavailable.
    pub fn read(
        &self,
        services: &ServiceAdapter,
        archive: &Registry,
    ) -> Result<Rc<Snapshot>, String> {
        let lacks = |_| text::LACKS.to_owned();
        let table = services
            .bind::<KilnTypesApi>(KILN_TYPES_SERVICE, KILN_TYPES_VERSION)
            .map_err(lacks)?;
        let ecs = service_member!(table, KilnTypesApi, ecs_registry).map_err(lacks)?;
        let at = service_member!(table, KilnTypesApi, type_at).map_err(lacks)?;
        let info_of = service_member!(table, KilnTypesApi, type_info).map_err(lacks)?;
        // The provider answered "unavailable" (ShroudForge's report with `available = false`): a snapshot whose report
        // gives the reason and has no entries.
        let unavailable = |reason: &str| Ok(Rc::new(Snapshot::unavailable(reason)));
        // The read itself failed (ShroudForge's fetch_json errors, read_native_catalog.rs:3-7): `registry()`'s Err, which
        // get_component and get_registry give as their reason.
        let failed = |reason: &str| Err(reason.to_owned());

        let mut rows: Vec<KilnEcsRegistryRow> = Vec::new();
        let mut first = true;
        let mut read = None;
        for _attempt in 0..4 {
            // SAFETY: all-zero records are valid (integers only).
            let mut info: KilnEcsRegistryInfo = unsafe { std::mem::zeroed() };
            info.size = std::mem::size_of::<KilnEcsRegistryInfo>() as u32;
            let mut required = 0u32;
            let capacity = rows.len() as u32;
            let out = if rows.is_empty() {
                std::ptr::null_mut()
            } else {
                rows.as_mut_ptr()
            };
            // SAFETY: a sized info record; `out` holds `capacity` rows (or is NULL with 0).
            let result = unsafe {
                services.call(
                    ecs,
                    (
                        self.world_kind,
                        &raw mut info,
                        out,
                        capacity,
                        &raw mut required,
                    ),
                )
            };
            if first {
                first = false;
                // The catalog is unchanged since the last read: keep its join.
                if (result == KILN_OK || result == KILN_ERR_BUFFER_TOO_SMALL)
                    && info.status == KILN_ECS_OK as u32
                    && info.layout_stamp != 0
                    && let Some(cached) = self.cached.borrow().as_ref()
                    && cached.stamp == info.layout_stamp
                    && cached.count == info.count
                {
                    return Ok(cached.clone());
                }
            }
            match result {
                KILN_OK => {
                    read = Some((info, required));
                    break;
                }
                KILN_ERR_BUFFER_TOO_SMALL => {
                    if required > MAX_ROWS {
                        return failed(text::INVALID_SIZE);
                    }
                    // SAFETY: as above.
                    rows = vec![unsafe { std::mem::zeroed() }; required as usize];
                }
                result if result == KILN_ECS_ERR_UNREADABLE => {}
                KILN_ERR_UNAVAILABLE => return unavailable(text::UNAVAILABLE),
                _ => return failed(text::INVALID_RESPONSE),
            }
        }
        let Some((info, count)) = read else {
            return failed(text::CHANGED);
        };
        if info.status != KILN_ECS_OK as u32 || info.count != count || count as usize > rows.len() {
            return failed(text::INVALID_RESPONSE);
        }
        rows.truncate(count as usize);
        if rows.iter().enumerate().any(|(position, row)| {
            (row.size as usize) < std::mem::size_of::<KilnEcsRegistryRow>()
                || row.index as usize != position
        }) {
            return failed(text::INVALID_RESPONSE);
        }

        let mut registrations = Vec::with_capacity(rows.len());
        let mut resolved = HashMap::new();
        let mut refused: Option<String> = None;
        for row in &rows {
            let Ok(identity) = u32::try_from(row.type_identity) else {
                return failed(text::INVALID_RESPONSE);
            };
            // SAFETY: plain value argument.
            let handle = unsafe { services.call(at, (identity,)) };
            if handle.is_null() {
                return failed(text::INVALID_RESPONSE);
            }
            // SAFETY: an all-zero KilnTypeInfo is valid (integers, empty strings, NULLs).
            let mut native: KilnTypeInfo = unsafe { std::mem::zeroed() };
            native.size = std::mem::size_of::<KilnTypeInfo>() as u32;
            // SAFETY: a live type handle from type_at and a sized caller record.
            if unsafe { services.call(info_of, (handle, &raw mut native)) } != KILN_OK {
                return failed(text::INVALID_RESPONSE);
            }
            let name = native.qualified_name;
            let name = if name.len == 0 {
                Vec::new()
            } else if name.ptr.is_null() {
                return failed(text::INVALID_RESPONSE);
            } else {
                // SAFETY: kiln.types names are readable for `len` bytes for the process.
                unsafe { std::slice::from_raw_parts(name.ptr.cast::<u8>(), name.len) }.to_vec()
            };
            let proven = std::str::from_utf8(&name)
                .ok()
                .filter(|name| name.starts_with(ECS_PREFIX))
                .and_then(|name| archive.by_qualified_name(name))
                .filter(|ty| {
                    ty.qualified_hash == row.component
                        && ty.size == row.type_size
                        && native.type_size == row.type_size
                });
            let Some(ty) = proven else {
                refused.get_or_insert_with(|| {
                    format!("{}{}", text::DISAGREES, String::from_utf8_lossy(&name))
                });
                continue;
            };
            registrations.push(Registration {
                index: row.index,
                qualified_hash: row.component,
                type_index: ty.index,
                runtime_size: row.type_size,
                flags_bits: row.flags,
            });
            resolved
                .entry(ty.qualified_name.clone())
                .or_insert(Resolved {
                    type_index: ty.index,
                    size: row.type_size,
                    component: row.component,
                });
        }
        let snapshot = Rc::new(Snapshot {
            stamp: info.layout_stamp,
            count,
            report: match refused {
                Some(reason) => Err(reason),
                None => Ok(registrations),
            },
            resolved,
        });
        *self.cached.borrow_mut() = (info.layout_stamp != 0).then(|| snapshot.clone());
        Ok(snapshot)
    }
}
