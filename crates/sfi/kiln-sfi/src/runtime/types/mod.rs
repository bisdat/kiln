// SPDX-License-Identifier: MIT
//! `game.types` / `runtime.types` and the Type userdata on a
//! registry port.
//!
//! - [`port`]: [`TypeRegistryPort`] (the archive registry's `registry_metadata` pages,
//!   the authority) and [`NativeTypes`] (kiln.types, a cross-check only;
//!   [`KilnTypes`] through the service adapter).
//! - [`registry`]: the pages read and checked into one [`Registry`], and the
//!   cross-check.
//! - [`selectors`]: hash domains, `resolve`, inheritance and effective fields.
//! - [`userdata`]: the per-VM session, the `game.types` table and the Type userdata.
//! - [`readonly`]: ShroudForge's read-only containers.
//!
//! **Wiring** ([`wire`]): one [`TypesWiring`] per runtime-create gives
//! - `game_types`, the composition's `GameTypes` port (`ComposePlan::game_types`): it
//!   reads the registry once, on the worker, at the first package's registration, and
//!   gives each package its own `game.types` table over one session; the registrar makes
//!   `runtime.types` the same table;
//! - `family`, the runtime family `types` (`PlanSource::families`), which claims
//!   `game.version`: at the first package it runs the kiln.types cross-check through
//!   the runtime context's adapter, then sets `game.version` to the archive's game
//!   version text in every package.
//!
//! Both must be wired. A registry that cannot be read, breaks a guarantee
//! ([`registry`]) or disagrees with kiln.types refuses runtime-create with
//! [`REGISTRY_FAILED`] and the reason, before any package code runs: ShroudForge's
//! session does not start without its registry either (`session/create_mod_session.rs:93`,
//! `reflection/load_type_registry.rs`), so there is no Lua-visible refusal text. A
//! kiln.types that cannot be read skips the cross-check (the archive registry is the
//! authority; see [`TypesHandle::cross_check`]) and writes one INFO line through the
//! wiring's log, [`SKIPPED`] and the reason.
//!
//! **Part B** ([`archive`]): [`ArchiveRegistryPort`] reads the pages from
//! `kiln.eml.archive` v1 with the archive generation granted to this start; the
//! production plan source ([`crate::plan_source::KilnPlanSource`]) wires it.

pub mod archive;
pub mod port;
pub mod readonly;
pub mod registry;
pub mod selectors;
pub mod userdata;

pub use archive::{ArchiveRegistryPort, GrantedView};
pub use port::{KilnTypes, NativeType, NativeTypes, PortError, RegistryPin, TypeRegistryPort};
pub use registry::{CrossCheck, LoadError, Registry};
pub use userdata::{Recognizer, Session, Type};

use super::install::Family;
use mlua::{Lua, Table};
use sfi_core::compose::GameTypes;
use sfi_core::host::Package;
use std::cell::RefCell;
use std::rc::{Rc, Weak};

/// The start of the runtime-create refusal when the registry is refused (ShroudForge
/// logs "Failed to load type registry" when its session cannot get one).
pub const REGISTRY_FAILED: &str = "Failed to load type registry";

/// The start of the one INFO line written when kiln.types cannot be read and the
/// cross-check is skipped.
pub const SKIPPED: &str = "kiln-sfi: type registry not cross-checked with kiln.types";

/// Where the wiring writes its log line (level, text): in production Kiln's log under
/// kiln-sfi's owner, which the plan source holds.
pub type TypesLog = Rc<dyn Fn(i32, &str)>;

struct Shared {
    port: Box<dyn TypeRegistryPort>,
    registry: RefCell<Option<Result<Rc<Registry>, LoadError>>>,
    checked: RefCell<Option<Result<CrossCheck, LoadError>>>,
    session: RefCell<Weak<Session>>,
    recognizers: Rc<RefCell<Vec<Recognizer>>>,
}

impl Shared {
    fn registry(&self) -> Result<Rc<Registry>, LoadError> {
        if let Some(outcome) = self.registry.borrow().as_ref() {
            return outcome.clone();
        }
        let outcome = Registry::load(self.port.as_ref()).map(Rc::new);
        *self.registry.borrow_mut() = Some(outcome.clone());
        outcome
    }

    fn session(&self, lua: &Lua) -> Result<Rc<Session>, LoadError> {
        if let Some(session) = self.session.borrow().upgrade()
            && session.belongs_to(lua)
        {
            return Ok(session);
        }
        let session = Session::new(lua, self.registry()?, self.recognizers.clone());
        *self.session.borrow_mut() = Rc::downgrade(&session);
        Ok(session)
    }
}

fn refused(error: LoadError) -> mlua::Error {
    mlua::Error::runtime(format!("{REGISTRY_FAILED}: {error}"))
}

/// The composition's `GameTypes` port on the archive registry.
struct RegistryTypes(Rc<Shared>);

impl GameTypes for RegistryTypes {
    fn types(&self, lua: &Lua, _owner: &Package) -> mlua::Result<Table> {
        let session = self.0.session(lua).map_err(refused)?;
        userdata::create(lua, &session)
    }
}

/// What the families and the plan source see of one wiring.
#[derive(Clone)]
pub struct TypesHandle(Rc<Shared>);

impl TypesHandle {
    /// The checked registry (read now if it was not yet).
    pub fn registry(&self) -> Result<Rc<Registry>, LoadError> {
        self.0.registry()
    }
    /// The cross-check's outcome once the family ran: `Agreed`, `Skipped` with the
    /// reason kiln.types could not be read, or the refusal.
    pub fn cross_check(&self) -> Option<Result<CrossCheck, LoadError>> {
        self.0.checked.borrow().clone()
    }
    /// `lua`'s session (the one `game.types` uses in that VM: its Types and the checked
    /// registry). It does not hold the port, so keeping it does not keep the registry's source open.
    pub fn session(&self, lua: &Lua) -> mlua::Result<Rc<Session>> {
        self.0.session(lua).map_err(refused)
    }
    /// Adds a recognizer for `game.types.of` (the value classes). The first that
    /// answers wins.
    pub fn add_recognizer(&self, recognizer: Recognizer) {
        self.0.recognizers.borrow_mut().push(recognizer);
    }
}

/// The two halves to wire for one runtime-create (module docs).
pub struct TypesWiring {
    pub game_types: Rc<dyn GameTypes>,
    pub family: Family,
    pub handle: TypesHandle,
}

/// The runtime family's name.
pub const FAMILY: &str = "types";

/// Wires `game.types`, `runtime.types` and `game.version` to `port`; `log` gets the
/// INFO line when the cross-check is skipped.
pub fn wire(
    port: impl TypeRegistryPort + 'static,
    log: impl Fn(i32, &str) + 'static,
) -> TypesWiring {
    let log: TypesLog = Rc::new(log);
    let shared = Rc::new(Shared {
        port: Box::new(port),
        registry: RefCell::new(None),
        checked: RefCell::new(None),
        session: RefCell::new(Weak::new()),
        recognizers: Rc::new(RefCell::new(Vec::new())),
    });
    let family = {
        let shared = shared.clone();
        Family::new(FAMILY, &["game.version"], move |_, _, _, composed, cx| {
            let registry = shared.registry().map_err(refused)?;
            let checked = shared.checked.borrow().clone();
            let checked = match checked {
                Some(outcome) => outcome,
                None => {
                    let outcome = registry.cross_check(&KilnTypes::new(cx.services()));
                    *shared.checked.borrow_mut() = Some(outcome.clone());
                    if let Ok(CrossCheck::Skipped(why)) = &outcome {
                        log(kiln_abi::KILN_LOG_INFO, &format!("{SKIPPED}: {why}"));
                    }
                    outcome
                }
            };
            checked.map_err(refused)?;
            composed.game.raw_set("version", registry.game_version())
        })
    };
    TypesWiring {
        game_types: Rc::new(RegistryTypes(shared.clone())),
        family,
        handle: TypesHandle(shared),
    }
}
