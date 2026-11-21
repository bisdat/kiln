// SPDX-License-Identifier: MIT
//! Pure ShroudForge package facade: documents, normalized model, strict schemas,
//! settings/locale/editor policy, discovery, planning, profiles and revisions, plus the
//! source/descriptor/version types they need. Documents keep their key order through
//! [`json::Value`]; serde_json's `preserve_order` is never turned on here, so linking this
//! crate leaves a dependent's serde_json maps sorted. No Lua VM, interpreter, plugin, FFI or GPL
//! code; nothing here executes mod code. `sfi_core::package` and the source items of
//! `sfi_core::host` are re-exports of these exact definitions.
pub mod json;
pub mod package;
pub mod parity;
pub mod same_folder;
pub mod source;
