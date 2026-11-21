// SPDX-License-Identifier: MIT
//! Pure owner-bound bindings. Native, runtime, input and UI families extend the returned
//! ordinary tables; their absence is not successful admission or native support.
pub mod clock;
pub mod common;
pub mod loader;
pub mod logging;
pub mod runtime;

pub use common::{BindingTables, OwnerBindings, install};
