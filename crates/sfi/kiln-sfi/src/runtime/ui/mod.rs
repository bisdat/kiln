// SPDX-License-Identifier: MIT
// the interpreter's UI bridge.
//! The runtime side of ShroudForge's UI API in kiln mode:
//! - [`transport`]: the files shared with Kiln.Ui (actions, notices) and the `ownerEpoch` values;
//! - [`actions`]: `shroudforge.ui.on_action` and the Lua side of the action drain;
//! - [`page`]: `shroudforge.ui.toggle_modloader` (ShroudForge's named event, verbatim; the UI side creates it);
//! - [`notices`]: `shroudforge.notifications.publish` and the notice files it writes.
//!
//! [`family`] installs the three members in every package's `shroudforge` table, as ShroudForge's create_mod_api
//! does for every runtime mod (both sides). The lifecycle host runs the drain at each turn start.

pub mod actions;
pub mod notices;
pub mod page;
pub mod transport;

use crate::runtime::install::Family;
use std::rc::Rc;
use transport::Transport;

/// The family's name in the runtime installer.
pub const FAMILY: &str = "ui";

/// The UI family: `shroudforge.ui` (on_action, toggle_modloader) and `shroudforge.notifications` (publish) for each
/// package, over this start's `transport` (`None`: no notice can be written). The VM's owner epoch is fixed at its first
/// package.
pub fn family(transport: Option<Transport>) -> Family {
    let transport = transport.map(Rc::new);
    Family::new(
        FAMILY,
        &["shroudforge.ui", "shroudforge.notifications"],
        move |lua, _env, package, composed, _cx| {
            let api = &composed.bindings.shroudforge;
            let ui = lua.create_table()?;
            actions::install(lua, &ui, package.id())?;
            page::install(lua, &ui)?;
            api.raw_set("ui", ui)?;
            notices::install(lua, api, package.id(), transport.clone())?;
            transport::owner_epoch(lua);
            Ok(())
        },
    )
}

/// This start's transport from its frozen plan (the game folder, the launch, the config folder); `None` without one.
pub fn production_transport(cx: &crate::worker::WorkerContext<'_>) -> Option<Transport> {
    let frozen = cx.shared().frozen_plan()?;
    Some(Transport {
        game: frozen.game.clone(),
        config: frozen.config.clone(),
        launch: transport::Launch {
            launch_id: frozen.launch_id,
            plan_hash: frozen.sha256,
        },
    })
}
