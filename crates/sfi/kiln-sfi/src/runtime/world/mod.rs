// SPDX-License-Identifier: MIT
//! The `runtime.world` families.
//!
//! - [`context`]: `runtime.world.operation_available`, `context_active`,
//!   `context_kind` and `session_id` on Kiln's own world facts and the worker's lifecycle stream.
//! - [`placement`]: `runtime.world.entity.place`, `.finish_building` and the
//!   overloaded `.destroy`'s spatial form, refusal-only on current profiles.
//! - [`cursor`]: `runtime.world.cursor.get`, the local player's `ClientPlayerInput` through kiln.world's reads.
//! - [`building_input`]: `runtime.world.building.submit`, `status` and `cancel` on kiln.client's building input.
//! - [`props`]: `runtime.world.entity.query_props`, `query_props_in_bounds`, `get_transform` and `set_scale`
//!   on kiln.world's reads and compare-exchange, with the shared entity handles; [`recipes`] is
//!   `register_prop_recipes` and its catalog.
//! - [`voxels`]: `runtime.world.voxel.get_grid_spec` on kiln.build's task-thread grid member.

/// The VM's shared [`Ecs`](crate::runtime::ecs::Ecs) at a call (production: [`crate::runtime::ecs::of_vm`]; None in a
/// VM without the ECS family).
pub type EcsSource =
    std::rc::Rc<dyn Fn(&mlua::Lua) -> Option<std::rc::Rc<crate::runtime::ecs::Ecs>>>;

pub mod building_input;
pub mod context;
pub mod cursor;
pub mod placement;
pub mod props;
pub mod recipes;
pub mod voxels;
