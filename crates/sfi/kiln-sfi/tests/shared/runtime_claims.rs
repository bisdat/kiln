// SPDX-License-Identifier: MIT
// Shared override-claim bodies: run by kiln-sfi's own tests (tests/runtime_20a.rs) and by any
// other test package that includes them. Expected texts and rules
// are ShroudForge's (MIT, Copyright (c) 2026 bonsaibauer), identical in both pins:
// modules/mod-runtime/lua/ingame/overrides/claim_runtime_storage.rs:15-363,
// lua/ingame/check_ingame_access.rs:37-54,386-414.
//! The claim token in a real Lua VM, on a fake claim authority (the package's runtime host and
//! live session as plain values): `RuntimeWriteClaim:acquire` / `:release`, ownership refusals
//! with their labels, and the storage scope (whole storages per mod, several tokens of one mod,
//! session-bound component and service keys, session-independent resource keys).

use kiln_sfi::runtime::overrides::*;
use mlua::{Lua, Table};
use sfi_core::bindings::runtime::RuntimeOperation;
use std::cell::{Cell, RefCell};
use std::rc::Rc;

/// A claim authority with settable answers; it records what acquire asked, in order.
pub struct FakeAuthority {
    id: String,
    session: Cell<u64>,
    denials: RefCell<Vec<(RuntimeOperation, String)>>,
    unavailable: RefCell<Vec<RuntimeOperation>>,
    pub asked: RefCell<Vec<String>>,
}

impl FakeAuthority {
    pub fn new(id: &str, session: u64) -> Rc<Self> {
        Rc::new(Self {
            id: id.into(),
            session: Cell::new(session),
            denials: RefCell::default(),
            unavailable: RefCell::default(),
            asked: RefCell::default(),
        })
    }
    pub fn set_session(&self, session: u64) {
        self.session.set(session);
    }
    pub fn deny(&self, operation: RuntimeOperation, reason: &str) {
        self.denials.borrow_mut().push((operation, reason.into()));
    }
    pub fn make_unavailable(&self, operation: RuntimeOperation) {
        self.unavailable.borrow_mut().push(operation);
    }
    pub fn clear(&self) {
        self.denials.borrow_mut().clear();
        self.unavailable.borrow_mut().clear();
        self.asked.borrow_mut().clear();
    }
}

impl ClaimAuthority for FakeAuthority {
    fn owner_id(&self) -> &str {
        &self.id
    }
    fn world_session(&self) -> u64 {
        self.session.get()
    }
    fn denial(&self, operation: RuntimeOperation) -> Option<String> {
        self.asked
            .borrow_mut()
            .push(format!("denial {}", operation.as_str()));
        self.denials
            .borrow()
            .iter()
            .find(|(op, _)| *op == operation)
            .map(|(_, reason)| reason.clone())
    }
    fn available(&self, operation: RuntimeOperation) -> bool {
        self.asked
            .borrow_mut()
            .push(format!("available {}", operation.as_str()));
        !self.unavailable.borrow().contains(&operation)
    }
}

pub const STAMINA: &str = "keen::ecs::Stamina";

pub fn owned_by(label: &str, owner: &str) -> String {
    format!(
        "{label} is owned by runtime mod '{owner}'. Disable its setting and restore it before another mod writes this storage"
    )
}

pub fn resource(guid: &str) -> ResourceKey {
    ResourceKey {
        guid: guid.into(),
        type_hash: 0x1234_5678,
        part_index: 0,
    }
}

/// `acquire` answers `true` / `false, reason`, `release` answers nothing, both from Lua.
pub fn a_claim_acquires_and_releases_through_its_lua_methods() {
    let lua = Lua::new();
    let store = Rc::new(OwnershipStore::new());
    let first = FakeAuthority::new("first-mod", 1);
    let second = FakeAuthority::new("second-mod", 1);
    let make = |authority: &Rc<FakeAuthority>| {
        claim_result(
            &lua,
            component_claim(&store, authority.clone(), 7, STAMINA, None),
        )
        .unwrap()
    };
    let (a, none_a) = make(&first);
    let (b, none_b) = make(&second);
    assert!(none_a.is_none() && none_b.is_none());
    let (asset, _) = claim_result(
        &lua,
        resource_claim(&store, first.clone(), resource("0000-asset"), 1),
    )
    .unwrap();
    let globals = lua.globals();
    globals.set("a", a.unwrap()).unwrap();
    globals.set("b", b.unwrap()).unwrap();
    globals.set("asset", asset.unwrap()).unwrap();
    let run = |code: &str| -> Table {
        lua.load(format!("return table.pack({code})"))
            .eval::<Table>()
            .unwrap()
    };
    let answer = |code: &str| -> (usize, Option<bool>, Option<String>) {
        let t = run(code);
        (t.get("n").unwrap(), t.get(1).unwrap(), t.get(2).unwrap())
    };
    let label = format!("component {STAMINA} on entity 7");
    assert_eq!(answer("a:acquire()"), (2, Some(true), None));
    assert_eq!(answer("a:acquire()"), (2, Some(true), None), "again: kept");
    assert_eq!(
        answer("b:acquire()"),
        (2, Some(false), Some(owned_by(&label, "first-mod")))
    );
    assert_eq!(answer("b:release()"), (0, None, None));
    assert_eq!(
        answer("b:acquire()"),
        (2, Some(false), Some(owned_by(&label, "first-mod"))),
        "another mod's release frees nothing"
    );
    assert_eq!(answer("a:release()"), (0, None, None));
    assert_eq!(answer("a:release()"), (0, None, None), "twice: nothing");
    assert_eq!(answer("b:acquire()"), (2, Some(true), None));
    assert_eq!(
        answer("a:acquire()"),
        (2, Some(false), Some(owned_by(&label, "second-mod")))
    );
    assert_eq!(answer("b:release()"), (0, None, None));

    // The package's denial and availability, in ShroudForge's order and texts.
    let inactive = "runtime API access is available only while this mod's lifecycle is active";
    first.deny(RuntimeOperation::EcsWrite, inactive);
    assert_eq!(
        answer("a:acquire()"),
        (2, Some(false), Some(inactive.into()))
    );
    first.clear();
    first.make_unavailable(RuntimeOperation::EcsWrite);
    assert_eq!(
        answer("a:acquire()"),
        (
            2,
            Some(false),
            Some("runtime.ecs.write is currently unavailable".into())
        )
    );
    first.clear();
    first.make_unavailable(RuntimeOperation::AssetsWrite);
    assert_eq!(
        answer("a:acquire()"),
        (2, Some(true), None),
        "a component asks for runtime.ecs.write only"
    );
    assert_eq!(
        answer("asset:acquire()"),
        (
            2,
            Some(false),
            Some("runtime.assets.write is currently unavailable".into())
        )
    );
    first.clear();
    assert_eq!(answer("asset:acquire()"), (2, Some(true), None));
    first.set_session(2);
    assert_eq!(
        answer("a:acquire()"),
        (2, Some(false), Some(SESSION_REFUSAL.into()))
    );
    assert_eq!(
        answer("asset:acquire()"),
        (2, Some(false), Some(SESSION_REFUSAL.into())),
        "a resource claim's token still belongs to its session"
    );
    // A factory refuses a stale or empty session with the same text.
    let (none, why) = claim_result(
        &lua,
        resource_claim(&store, first.clone(), resource("0000-asset"), 1),
    )
    .unwrap();
    assert!(none.is_none());
    assert_eq!(why.as_deref(), Some(SESSION_REFUSAL));
    first.set_session(0);
    assert_eq!(
        component_claim(&store, first.clone(), 7, STAMINA, None).err(),
        Some(SESSION_REFUSAL.into())
    );
}

/// Whole storages per mod; several tokens of one mod; session-bound component and service
/// keys pruned by a new nonzero session; resources kept; session 0 prunes nothing.
pub fn storage_claims_cover_whole_storages_per_mod_and_resources_outlive_a_world() {
    let store = Rc::new(OwnershipStore::new());
    let first = FakeAuthority::new("first-mod", 1);
    let second = FakeAuthority::new("second-mod", 1);
    let claim = |authority: &Rc<FakeAuthority>, key: &StorageKey| {
        let session = authority.world_session();
        get_claim(&store, authority.clone(), key.clone(), session, None).unwrap()
    };
    let component = component_key(first.as_ref(), 7, STAMINA);
    let service = StorageKey::Service {
        session: 1,
        key: "ecs::GameSettings".into(),
    };
    let asset = StorageKey::Resource(resource("8d0e5c62-0001"));
    assert_eq!(
        component,
        StorageKey::Component {
            session: 1,
            entity: 7,
            qualified_name: STAMINA.into()
        }
    );
    // Two tokens of one mod hold one storage until both are released.
    let (one, two) = (claim(&first, &component), claim(&first, &component));
    assert_ne!(one.token(), two.token());
    one.acquire().unwrap();
    two.acquire().unwrap();
    assert_eq!(store.tokens_on(&component), 2);
    one.release();
    assert_eq!(
        check_write(&store, second.as_ref(), &component),
        Err(owned_by(
            &format!("component {STAMINA} on entity 7"),
            "first-mod"
        ))
    );
    assert_eq!(check_write(&store, first.as_ref(), &component), Ok(()));
    two.release();
    assert!(store.is_empty());
    assert_eq!(check_write(&store, second.as_ref(), &component), Ok(()));

    // The whole storage: another entity or component is another storage.
    for key in [&component, &service, &asset] {
        claim(&first, key).acquire().unwrap();
    }
    claim(&second, &component_key(second.as_ref(), 8, STAMINA))
        .acquire()
        .unwrap();
    for (key, label) in [
        (&service, "named service ecs::GameSettings".to_owned()),
        (&asset, "loaded resource 8d0e5c62-0001".to_owned()),
    ] {
        assert_eq!(
            claim(&second, key).acquire(),
            Err(owned_by(&label, "first-mod"))
        );
    }
    // Same GUID, other part: another storage.
    let other_part = StorageKey::Resource(ResourceKey {
        part_index: 1,
        ..resource("8d0e5c62-0001")
    });
    claim(&second, &other_part).acquire().unwrap();

    // Session 0 is no evidence a world ended.
    first.set_session(0);
    second.set_session(0);
    store.prune(0);
    assert_eq!(store.len(), 5);
    assert!(check_write(&store, second.as_ref(), &service).is_err());
    // A new session ends component and service claims; resources stay.
    first.set_session(2);
    second.set_session(2);
    assert_eq!(check_write(&store, second.as_ref(), &component), Ok(()));
    assert_eq!(check_write(&store, second.as_ref(), &service), Ok(()));
    assert_eq!(
        check_write(&store, second.as_ref(), &asset),
        Err(owned_by("loaded resource 8d0e5c62-0001", "first-mod"))
    );
    assert_eq!(store.len(), 2);
    assert_eq!(store.owner_of(&asset).as_deref(), Some("first-mod"));
    assert_eq!(store.owner_of(&other_part).as_deref(), Some("second-mod"));
}
