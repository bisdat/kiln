// SPDX-License-Identifier: MIT
//! Generated bindings to Kiln's public MIT C ABI.
//!
//! Service types live in `services::kiln_<name>`; shared types from `kiln_api.h`
//! are exported here exactly once. All function pointers are nullable. Read the
//! header size before reading an appended field, and never form a reference to
//! a full newer table when only its prefix is present. Layout assertions are
//! measured by clang targeting x86_64-pc-windows-msvc, not hand-calculated.
#![no_std]

// Keep compiler-generated files byte-exact for abigen --check; formatting the
// authored crate must not rewrite its generated modules or recurse into them.
#[rustfmt::skip]
mod bindings;
pub use bindings::*;
#[rustfmt::skip]
pub mod services;
#[rustfmt::skip]
mod layout;
