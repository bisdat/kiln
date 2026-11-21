// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln-eml: EML (Enshrouded Mod Loader by Brabb3l, kfc-parser v0.2.0) as a Kiln plugin.
//!
//! - `discover`: which EML mods this start runs (Kiln profile, engine plugins, drop-in folders).
//! - `runner`: one EML run over them, failing soft (bad mods are switched off, a failed run restores vanilla data).
//! - `sf_pass`: the ShroudForge-dialect startup pass driver: one retry without the package that made it fail.
//! - `kiln_mode`: under Kiln's interpreter (kiln.launch backend `kiln`), its launch plan decides the
//!   packages that need ShroudForge's runtime or belong to its startup pass.
//! - `logging`: EML's tracing events as plain lines (kiln.log, kiln/logs/eml.log, stderr).
//! - `plugin` (Windows): the DLL's exports (kiln_plugin_early, kiln_plugin_main, kiln_eml_service).
//! - `archive_service`: kiln.eml.archive v1 (its export kiln_eml_archive_service), the metadata subset.
//! - `archive_values`: kiln.eml.values v1, the typed graph and its C table (normal service lookup only).
//!
//! GPL-3.0-or-later. Kiln (MIT) talks to it only through its exports and native/core/abi/services/kiln_eml.h.

pub mod discover;
// kiln-eml when kiln.launch froze backend `kiln` (kiln mode, with Kiln's interpreter).
pub mod kiln_mode;
pub mod logging;
pub mod modified;
pub mod progress;
pub mod runner;
// (one file or folder by any of its names, for the export root, the install lock and the
// running-game check)
mod same_path;
// The SF-dialect startup pass driver (one retry without a failing package), wired by kiln mode.
pub mod sf_pass;
// The SF-dialect startup pass's Lua state (type session, game.types); composed into the pass.
pub mod sf_startup;
// (the startup controller of a kiln-mode start: witness, admission, pass and retry, one
// publication, cache, outcome; a library, wired by the plugin wiring)
pub mod startup;
// kiln.eml.startup v1's provider (early export kiln_eml_startup_service), the early witness, the barrier, the
// grant
pub mod startup_seam;
// kiln.eml.archive v1's provider (early getter, metadata subset, error channel).
pub mod archive_service;
// kiln.eml.values v1's typed graph and its C table.
pub mod archive_values;
// The read-only archive inspector behind kiln-eml-inspect (never the patch runner).
pub mod inspection;
pub mod portraits;
pub mod portrait_export;

// Status codes of the bounded portrait export (not KilnResult).
pub const OK: i32 = 0;
pub const BAD_REQUEST: i32 = 2;
pub const RUN_FAILED: i32 = 3;
pub const ITEMS_FAILED: i32 = 4;
// Appended in dev.21: the portrait cache's causes, so a caller can tell a transient refusal from a lasting one. Every
// other BAD_REQUEST is lasting until the inputs change.
pub const CACHE_BUSY: i32 = 5; // another generator holds the cache lock: try again later
pub const CACHE_FULL: i32 = 6; // the cache is over its folder, entry or size budget: lasting until it is cleaned
// Appended in dev.21: reading or locking the cache failed with an I/O error (a sharing violation, a
// drive briefly gone, a permission problem). Transient: try again at a later start. A link, an invalid entry or a bad
// path in the cache stays BAD_REQUEST.
pub const CACHE_IO: i32 = 7;

#[cfg(any(windows, test))]
mod plugin;

// The test lock/spawn gate in this crate's unit tests (mod-loader-lua's test_gate.rs, a dev-dependency feature).
#[cfg(test)]
mod gate_tests;

/// kiln-eml's own version.
pub const VERSION: &str = env!("CARGO_PKG_VERSION");
/// The EML release eml/* was taken from.
pub const EML_VERSION: &str = "0.2.0";
