// SPDX-License-Identifier: MIT
//! Shared, game-independent foundations for Kiln's interpreter of ShroudForge mods.

pub mod bindings;
pub mod compose;
pub mod conformance;
pub mod eml;
pub mod host;
pub mod lifecycle;
pub mod locales;
pub mod lua;
/// The pure package model: the one `sfi_package::package`, under its old path.
pub use sfi_package::package;
/// One folder by any of its names, and where Wine really has a path (`sfi_package::same_folder`).
pub use sfi_package::same_folder;
pub mod settings;
