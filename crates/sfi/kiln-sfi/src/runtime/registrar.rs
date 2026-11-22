// SPDX-License-Identifier: MIT
//! The production registrar, the one list of runtime families a Kiln start installs (runtime-create and every
//! live reload's rebuild), in this order:
//! 1. the types family (`game.version`, when the plan wired `game.types`) and the ECS family on it (the registry
//!    and catalog, the entity queries, the component reads and plain writes, the component claim and override
//!    target), then the attribute family on the ECS family's entities (the archive's attribute catalog and an
//!    entity's attribute storage), with the ECS family the named services on its shared ECS
//!    (`runtime.services.get`, the service claim and override target), then `game.assets`' loaded-resource family
//!    on the same wiring;
//! 2. the network family core with its maintenance hook;
//! 3. the UI family over this start's bridge files;
//! 4. the override tracker on the plan side's world, also the lifecycle's cleanup;
//! 5. the world context with its lifecycle hook;
//! 6. the world cursor, building input and props (the cursor and props on the ECS family's shared `Ecs`);
//! 7. the placement refusals (`runtime.world.entity.place`, `.finish_building`, the spatial `.destroy`), the voxel
//!    grid (`runtime.world.voxel.get_grid_spec`) on kiln.build's task-thread member, and `runtime.patch`, the
//!    placement and patch refusal-only on current profiles;
//! 8. the keybind members of `shroudforge.input` on the input feed, with its turn and stop hooks;
//! 9. last, [`unavailable::family`] over every ShroudForge member none of the families above claims, and
//!    [`unavailable::install_function_members`] for `runtime.functions` (the installer's step 5).
//!
//! A family that lands later is added here; the members it claims leave the unavailable family's list by
//! themselves.

use super::input;
use super::install::{Families, Family};
use super::named_services;
use super::patch;
use super::unavailable;
use super::world::{building_input, context, cursor, placement, props, voxels};

/// The families of a start that come from its plan, the rest being fixed.
pub struct Parts {
    /// The types family, when the plan wired `game.types`.
    pub types: Option<Family>,
    /// The ECS registry family on the same wiring.
    pub ecs: Option<Family>,
    /// The attribute family on the same wiring's archive generation (after the ECS family: it uses its entities).
    pub attributes: Option<Family>,
    /// `game.assets`' loaded-resource family on the same wiring.
    pub assets: Option<Family>,
    /// The network family with its maintenance (`network::production_family`).
    pub network: Family,
    /// The UI family over this start's transport.
    pub ui: Family,
    /// The override tracker family on the active ECS world.
    pub trackers: Option<Family>,
}

/// The production list: `parts`, the fixed families, then the unavailable family over what they leave.
pub fn production(parts: Parts) -> Result<Families, String> {
    // The named services ride on the ECS family's shared ECS: only with it, after the attribute family.
    let services = parts.ecs.as_ref().map(|_| named_services::family());
    let mut list: Vec<Family> = parts
        .types
        .into_iter()
        .chain(parts.ecs)
        .chain(parts.attributes)
        .chain(services)
        // the loaded-resource field indexes' runner beside the family whose VMs it drives
        .chain(
            parts
                .assets
                .into_iter()
                .flat_map(|assets| [assets, crate::runtime::assets::index_family()]),
        )
        .chain([parts.network, parts.ui])
        .chain(parts.trackers)
        .chain([
            context::family(),
            cursor::family(),
            building_input::family(),
            props::family(),
            placement::family(None),
            voxels::family(),
            patch::family(),
            input::production_family(),
        ])
        .collect();
    list.push(unavailable::family(unavailable::unclaimed(&list)));
    Ok(Families::new(list)?
        .with_function_members(std::rc::Rc::new(unavailable::install_function_members)))
}
