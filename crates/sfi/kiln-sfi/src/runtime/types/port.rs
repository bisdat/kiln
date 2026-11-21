// SPDX-License-Identifier: MIT
//! The two ports the type registry is read through.
//!
//! - [`TypeRegistryPort`]: the archive registry, the authority. It
//!   serves `registry_metadata` pages exactly as the archive schema defines them
//!   (`schema/sfi/eml-archive/registry.schema.json`, schema_version 1, with the shared
//!   `common.schema.json` type records): `page(first, limit)` is one call of
//!   `kiln.eml.archive.registry_metadata(view, first, limit, json, capacity, &required,
//!   &error)` and returns the page's JSON bytes, or why the call failed.
//! - [`NativeTypes`]: kiln.types, a cross-check only: its count, qualified names and
//!   sizes must agree with the archive's, or the registry is refused for live use. The
//!   production implementation is [`KilnTypes`], through the runtime's service adapter.
//!
//! The production [`TypeRegistryPort`] is [`super::archive::ArchiveRegistryPort`] (part B),
//! on the real `kiln.eml.archive` table through the service adapter.

use super::super::services::{ServiceAdapter, ServiceError};
use crate::service_member;
use kiln_abi::KilnResult;
use kiln_abi::services::kiln_types::*;

/// The launch identity a registry page is pinned to (`common.schema.json#/$defs/pin`).
/// The UUIDs and digests are kept as the schema's lowercase text.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RegistryPin {
    pub launch_id: String,
    pub plan_id: String,
    pub generation: u64,
    pub reload_epoch: u64,
    pub plan_sha256: String,
    pub archive_sha256: String,
    pub registry_id: u64,
    pub registry_sha256: String,
}

/// Why a registry page could not be read.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PortError {
    /// The service, or one of the members the read needs, is not there
    /// (the adapter's typed refusal).
    Unavailable(ServiceError),
    /// A member answered a non-OK result; `message` is the precise text its error-out
    /// record carried, when it carried one.
    Failed {
        member: &'static str,
        result: KilnResult,
        message: Option<String>,
    },
    /// A kiln.caps row the read needs is not available (`text`: what the row says, or
    /// that nobody reported it). A held, off or absent row refuses use.
    Withheld { row: &'static str, text: String },
    /// The view the archive granted is not pinned to the launch identity it was
    /// acquired with.
    IdentityMismatch,
}

impl std::fmt::Display for PortError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            PortError::Unavailable(error) => write!(f, "{error}"),
            PortError::Failed {
                member,
                result,
                message: Some(message),
            } => write!(f, "{member} failed ({result}): {message}"),
            PortError::Failed {
                member,
                result,
                message: None,
            } => write!(f, "{member} failed ({result})"),
            PortError::Withheld { row, text } => {
                write!(f, "capability {row} is not available: {text}")
            }
            PortError::IdentityMismatch => write!(
                f,
                "kiln.eml.archive.identity: the view is not pinned to this start's launch identity"
            ),
        }
    }
}

/// The archive registry, one `registry_metadata` page at a time.
pub trait TypeRegistryPort {
    /// One page: `limit` types from registry ordinal `first` (`limit` 0: the header
    /// only, `types: []`), as the schema's JSON bytes.
    fn page(&self, first: u64, limit: u32) -> Result<Vec<u8>, PortError>;
    /// The pin the frozen launch identity expects. `None`: every page must carry the
    /// same pin as the first, and nothing more is checked.
    fn expected_pin(&self) -> Option<RegistryPin> {
        None
    }
}

/// One type as kiln.types reports it, for the cross-check.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NativeType {
    pub qualified_name: Vec<u8>,
    pub size: u32,
}

/// kiln.types, the cross-check: every reflected type it found, or why it cannot say.
pub trait NativeTypes {
    /// `Err`: the native table cannot be read here (the check is skipped, never passed).
    fn types(&self) -> Result<Vec<NativeType>, String>;
}

/// [`NativeTypes`] on kiln.types v1 through the service adapter: `status` must report
/// the type table found and checked, then `type_count`, `type_at` and `type_info`
/// per index. Direct calls on the worker: no hop, no wait, no retry.
pub struct KilnTypes<'a> {
    services: &'a ServiceAdapter,
}

impl<'a> KilnTypes<'a> {
    pub fn new(services: &'a ServiceAdapter) -> Self {
        Self { services }
    }
}

impl NativeTypes for KilnTypes<'_> {
    fn types(&self) -> Result<Vec<NativeType>, String> {
        let services = self.services;
        let text = |error: ServiceError| error.to_string();
        let table = services
            .bind::<KilnTypesApi>(KILN_TYPES_SERVICE, KILN_TYPES_VERSION)
            .map_err(text)?;
        let status = service_member!(table, KilnTypesApi, status).map_err(text)?;
        let count = service_member!(table, KilnTypesApi, type_count).map_err(text)?;
        let at = service_member!(table, KilnTypesApi, type_at).map_err(text)?;
        let info = service_member!(table, KilnTypesApi, type_info).map_err(text)?;

        // SAFETY: an all-zero KilnTypesStatus is valid (integers and an empty KilnStr).
        let mut facts: KilnTypesStatus = unsafe { std::mem::zeroed() };
        facts.size = std::mem::size_of::<KilnTypesStatus>() as u32;
        // SAFETY: a caller record with its size set (kiln_types.h).
        let result = unsafe { services.call(status, (&raw mut facts,)) };
        if result != kiln_abi::KILN_OK || facts.types != 1 {
            return Err(format!(
                "kiln.types: the type table is not available (status {result}, types {})",
                facts.types
            ));
        }
        // SAFETY: no arguments.
        let total = unsafe { services.call(count, ()) };
        let mut types = Vec::with_capacity((total as usize).min(1 << 16));
        for index in 0..total {
            // SAFETY: an index below type_count.
            let handle = unsafe { services.call(at, (index,)) };
            if handle.is_null() {
                return Err(format!("kiln.types: no type at index {index}"));
            }
            // SAFETY: an all-zero KilnTypeInfo is valid (integers, empty strings, NULLs).
            let mut record: KilnTypeInfo = unsafe { std::mem::zeroed() };
            record.size = std::mem::size_of::<KilnTypeInfo>() as u32;
            // SAFETY: a live type handle from type_at and a sized caller record.
            let result = unsafe { services.call(info, (handle, &raw mut record)) };
            if result != kiln_abi::KILN_OK {
                return Err(format!(
                    "kiln.types: type_info failed for index {index} ({result})"
                ));
            }
            let name = record.qualified_name;
            let qualified_name = if name.len == 0 {
                Vec::new()
            } else if name.ptr.is_null() {
                return Err(format!("kiln.types: index {index} has no qualified name"));
            } else {
                // SAFETY: kiln.types names are readable for `len` bytes for the process.
                unsafe { std::slice::from_raw_parts(name.ptr.cast::<u8>(), name.len) }.to_vec()
            };
            types.push(NativeType {
                qualified_name,
                size: record.type_size,
            });
        }
        Ok(types)
    }
}
