// SPDX-License-Identifier: MIT
//! Pure policy for Kiln's interpreter of ShroudForge mods, shared by the kiln-sfi plugin and a
//! host-side consumer such as kiln-manage.
//!
//! - [`launch_plan`]: the strict launch-plan v1 reader (duplicate keys and noncanonical
//!   numbers refused before serde can overwrite a restriction), and its typed view.
//! - [`digest`]: a package folder's content digest, the `content_sha256` a plan pins (the
//!   plan's writer and kiln-sfi compute it with this one function).
//! - [`pe_gate`]: the read-only PE32+ import gate. It never calls the Windows loader.
//! - [`native_contracts`]: the host-owned exact-hash review registry and the ruling 10/11
//!   verdicts, plus [`native_contracts::AdvisoryInventory`] for host-side inventories.
//! - [`same_folder`]: one folder by any of its names (a Windows name key and a file identity).
//! - [`native_selection`]: ShroudForge's DLL Manager selection over Kiln-owned inputs,
//!   whole-package withdrawal and the sealed `register_dll` table. No filesystem, no load.
//!
//! Nothing here loads a DLL, constructs Lua, reads a ShroudForge folder, or builds a load
//! permit ([`digest`] reads the one package folder it is given, nothing else). kiln-sfi's
//! `native_enforcement` alone binds a verdict to the OS: it reopens the files, resolves
//! system images through the loader and owns the private `LoadPermit`.
//! The `os-binding` feature exposes only what that resolver needs; a host-side consumer
//! never enables it.
//!
//! `native_selection` adapts ShroudForge's DLL Manager (MIT notice in its header). The other
//! modules adapt no ShroudForge code; they cite its behaviour (both pins) as the reference for
//! validation rules only.

pub mod digest;
pub mod launch_plan;
pub mod native_contracts;
pub mod native_selection;
pub mod pe_gate;
pub mod same_folder;
mod strict_json;
