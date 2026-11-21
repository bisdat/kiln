// SPDX-License-Identifier: MIT
//! The `runtime.world` families.
//!
//! - [`context`]: `runtime.world.operation_available`, `context_active`,
//!   `context_kind` and `session_id` on Kiln's own world facts and the worker's lifecycle stream.
//! - [`placement`]: `runtime.world.entity.place`, `.finish_building` and the
//!   overloaded `.destroy`'s spatial form, refusal-only on current profiles.

pub mod context;
pub mod placement;
