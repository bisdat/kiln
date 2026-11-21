// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (both pins): modules/modloader/mod_package_filesystem.rs:69-87,121-225,
// 235-326,379-413,504-517. The pinned filesystem modules are byte-identical.
//! Explicit host inputs. No installation-path discovery or implicit configuration reads.
//!
//! SF provenance (both pins): modules/mod-runtime/lua/mod_runner.rs:40-46,111-134;
//! modules/modloader/mod_package_filesystem.rs:99-135,300-308,392-400,504-517.
//! The host selects the package registry before execution; inclusion is not activation.

use std::rc::Rc;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Phase {
    Startup,
    Runtime,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Side {
    Client,
    Server,
}
pub use sfi_package::source::{
    DiskSource, Package, PackageSource, ProfileOverlaySource, SourceEntry, SourceKind,
    SourceMetadata, SourceVersion,
};

/// Process-origin monotonic seconds. The host owns the origin across VM replacements.
/// SF: modules/mod-runtime/lua/shroudforge/v1/read_monotonic_clock.rs:6-10 (both pins).
pub trait Clock {
    fn seconds(&self) -> f64;
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
}
/// Counted payload, before subscriber framing/CR-LF escaping. Embedded NUL is not a terminator.
/// SF: modules/mod-runtime/lua/bindings/write_mod_log.rs:8-37 (both pins).
pub trait LogSink {
    fn enabled(&self, level: LogLevel) -> bool;
    fn write(&self, owner: &str, level: LogLevel, payload: &[u8]);
}

/// Phase/side and source version are independent of package ownership and reload epoch.
/// Clock and logging are explicitly optional until a binding requiring them is installed;
/// there is no invented clock origin, log delivery, or successful no-op implementation.
#[derive(Clone)]
pub struct HostContext {
    pub phase: Phase,
    pub side: Side,
    pub version: SourceVersion,
    pub clock: Option<Rc<dyn Clock>>,
    pub log: Option<Rc<dyn LogSink>>,
}
impl HostContext {
    pub fn new(phase: Phase, side: Side, version: SourceVersion) -> Self {
        Self {
            phase,
            side,
            version,
            clock: None,
            log: None,
        }
    }
}
