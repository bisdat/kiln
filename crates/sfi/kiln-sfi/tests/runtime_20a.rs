// SPDX-License-Identifier: MIT
//! The ownership store, `RuntimeWriteClaim`, the claim factories and the
//! generic override target, on fake claim authorities, a fake runtime host under the registrar's real
//! denial, and fake targets. Expected texts and rules are ShroudForge's (MIT, Copyright (c)
//! 2026 bonsaibauer), identical in both pins: modules/mod-runtime/lua/ingame/overrides/
//! claim_runtime_storage.rs:1-363, create_override_targets.lua, track_runtime_overrides.lua,
//! lua/ingame/check_ingame_access.rs:37-54,386-414. The Lua-visible claim bodies are in
//! shared/runtime_claims.rs.
#[path = "shared/runtime_claims.rs"]
mod bodies;

use bodies::*;
use kiln_sfi::runtime::overrides::*;
use mlua::{AnyUserData, Lua, Table, Value};
use serde_json::json;
use sfi_core::bindings::runtime::{
    Availability, EffectState, RuntimeHost, RuntimeOperation as Op, RuntimeOwnerContext,
    RuntimeProviderFacts,
};
use sfi_core::host::{HostContext, Phase, Side, SourceVersion};
use sfi_core::package;
use std::cell::{Cell, RefCell};
use std::rc::Rc;

#[test]
fn a_claim_acquires_and_releases_through_its_lua_methods() {
    bodies::a_claim_acquires_and_releases_through_its_lua_methods()
}

#[test]
fn storage_claims_cover_whole_storages_per_mod_and_resources_outlive_a_world() {
    bodies::storage_claims_cover_whole_storages_per_mod_and_resources_outlive_a_world()
}

/// A runtime host with settable answers; the registrar's real `runtime_denial_reason` reads it.
struct FakeHost {
    owner: RuntimeOwnerContext,
    active: Cell<bool>,
    fault: RefCell<Option<String>>,
    unsupported: RefCell<Vec<Op>>,
    can_write: Cell<bool>,
    resource_can_write: Cell<bool>,
}

const NOT_VERIFIED: &str = "operation runtime.ecs.write has not been verified for game build b1";

impl FakeHost {
    fn new(id: &str, capabilities: &[&str], phase: Phase) -> Rc<Self> {
        let manifest = package::Package::parse(
            json!({"id": id, "name": id, "version": "1.0.0", "capabilities": capabilities})
                .to_string()
                .as_bytes(),
            None,
            package::ParseOptions::default(),
        )
        .unwrap()
        .manifest;
        let context = HostContext::new(phase, Side::Server, SourceVersion::Sf179);
        Rc::new(Self {
            owner: RuntimeOwnerContext::new(&manifest, &context, true, true, "b1"),
            active: Cell::new(true),
            fault: RefCell::new(None),
            unsupported: RefCell::default(),
            can_write: Cell::new(true),
            resource_can_write: Cell::new(true),
        })
    }
}

impl RuntimeHost for FakeHost {
    fn owner_context(&self) -> &RuntimeOwnerContext {
        &self.owner
    }
    fn static_support(&self, operation: Op) -> Availability {
        if self.unsupported.borrow().contains(&operation) {
            Availability::Unavailable {
                reason: format!(
                    "operation {} has not been verified for game build b1",
                    operation.as_str()
                ),
            }
        } else {
            Availability::Available
        }
    }
    fn provider_facts(&self) -> RuntimeProviderFacts {
        RuntimeProviderFacts {
            ready: true,
            can_write: self.can_write.get(),
            resource_can_write: self.resource_can_write.get(),
            ..Default::default()
        }
    }
    fn lifecycle_active(&self) -> bool {
        self.active.get()
    }
    fn write_fault(&self) -> Option<String> {
        self.fault.borrow().clone()
    }
    fn report_effect(&self, _: EffectState, _: &str) {}
}

fn authority(host: &Rc<FakeHost>, session: &Rc<Cell<u64>>) -> Rc<dyn ClaimAuthority> {
    let session = session.clone();
    Rc::new(HostAuthority::new(host.clone(), move || session.get()))
}

/// A factory only needs the live session; the package's lifecycle, the write fault, static
/// support and the provider's write facts are each acquire's, through the registrar's denial.
#[test]
fn a_claim_made_while_inactive_or_faulted_is_refused_at_acquire_until_that_ends() {
    let store = Rc::new(OwnershipStore::new());
    let host = FakeHost::new("first-mod", &["runtime"], Phase::Runtime);
    let session = Rc::new(Cell::new(1));
    let auth = authority(&host, &session);
    assert_eq!(auth.owner_id(), "first-mod");

    host.active.set(false);
    *host.fault.borrow_mut() = Some("ecs-write-outcome-uncertain-rollback-unverified".into());
    host.can_write.set(false);
    host.unsupported.borrow_mut().push(Op::EcsWrite);
    let claim =
        component_claim(&store, auth.clone(), 7, STAMINA, None).expect("made while inactive");
    let asset = resource_claim(&store, auth.clone(), resource("0000-asset"), 1)
        .expect("made while faulted");
    let refused = |reason: &str| {
        assert_eq!(claim.acquire(), Err(reason.to_owned()));
        assert!(!claim.is_active() && store.is_empty());
    };
    refused("runtime API access is available only while this mod's lifecycle is active");
    host.active.set(true);
    refused("ecs-write-outcome-uncertain-rollback-unverified");
    *host.fault.borrow_mut() = None;
    refused(NOT_VERIFIED);
    host.unsupported.borrow_mut().clear();
    refused("runtime.ecs.write is currently unavailable");
    host.can_write.set(true);
    host.resource_can_write.set(false);
    assert_eq!(
        asset.acquire(),
        Err("runtime.assets.write is currently unavailable".into())
    );
    *host.fault.borrow_mut() = Some("ecs-write-outcome-uncertain-rollback-unverified".into());
    assert_eq!(
        asset.acquire(),
        Err("ecs-write-outcome-uncertain-rollback-unverified".into()),
        "the fault denies runtime.ecs.write first"
    );
    *host.fault.borrow_mut() = None;
    host.resource_can_write.set(true);
    claim.acquire().unwrap();
    asset.acquire().unwrap();
    assert!(claim.is_active() && asset.is_active());
    assert_eq!(store.len(), 2);
    // A later denial refuses again, and leaves the earlier acquire in place.
    host.active.set(false);
    assert!(claim.acquire().is_err());
    assert!(claim.is_active());
    assert_eq!(store.owner_of(claim.key()).as_deref(), Some("first-mod"));
    // Release needs no permission.
    claim.release();
    asset.release();
    assert!(store.is_empty());
}

/// Phase and permission come first, from the registrar's denial.
#[test]
fn outside_the_ingame_phase_or_without_runtime_the_denial_answers() {
    let store = Rc::new(OwnershipStore::new());
    let session = Rc::new(Cell::new(1));
    for (host, reason) in [
        (
            FakeHost::new("pre", &["runtime"], Phase::Startup),
            "runtime APIs are available only during the ingame phase".to_owned(),
        ),
        (
            FakeHost::new("plain", &[], Phase::Runtime),
            "mod 'plain' requires capabilities: [\"runtime\"]".to_owned(),
        ),
    ] {
        let claim = component_claim(&store, authority(&host, &session), 7, STAMINA, None).unwrap();
        assert_eq!(claim.acquire(), Err(reason));
    }
    assert!(store.is_empty());
}

/// session 0 never makes or acquires a claim; a claim's session must stay the live one;
/// pruning happens on acquire (to the claim's session) and on check_write (to the live one).
#[test]
fn session_zero_and_the_prune_rules() {
    let store = Rc::new(OwnershipStore::new());
    let first = FakeAuthority::new("first-mod", 0);
    let second = FakeAuthority::new("second-mod", 1);
    assert_eq!(
        component_claim(&store, first.clone(), 7, STAMINA, None).err(),
        Some(SESSION_REFUSAL.into())
    );
    assert_eq!(
        service_claim(&store, second.clone(), "ecs::GameSettings", 0).err(),
        Some(SESSION_REFUSAL.into()),
        "a view from no session"
    );
    assert_eq!(
        template_claim(&store, second.clone(), resource("t"), 2).err(),
        Some(SESSION_REFUSAL.into()),
        "a template from another session"
    );
    first.set_session(1);
    let old = component_claim(&store, first.clone(), 7, STAMINA, None).unwrap();
    old.acquire().unwrap();
    // The world goes away (session 0): nothing is pruned, the claim cannot acquire.
    first.set_session(0);
    second.set_session(0);
    store.prune(0);
    assert_eq!(store.len(), 1);
    assert_eq!(old.acquire(), Err(SESSION_REFUSAL.into()));
    assert!(check_write(&store, second.as_ref(), old.key()).is_err());
    // A new session: the second mod's acquire there prunes the old record first.
    first.set_session(2);
    second.set_session(2);
    let new = component_claim(&store, second.clone(), 7, STAMINA, None).unwrap();
    assert_ne!(new.key(), old.key(), "the key carries its session");
    new.acquire().unwrap();
    assert_eq!(store.len(), 1);
    assert_eq!(store.owner_of(old.key()), None);
    assert_eq!(store.owner_of(new.key()).as_deref(), Some("second-mod"));
    // Releasing the old claim after the prune touches nothing.
    old.release();
    assert_eq!(store.owner_of(new.key()).as_deref(), Some("second-mod"));
}

/// The last token is handed out once; then every factory refuses, in Lua as `nil, reason`.
#[test]
fn token_exhaustion_refuses_every_later_claim() {
    let lua = Lua::new();
    let store = Rc::new(OwnershipStore::with_last_token(u64::MAX - 1));
    let first = FakeAuthority::new("first-mod", 1);
    let last = component_claim(&store, first.clone(), 7, STAMINA, None).unwrap();
    assert_eq!(last.token(), u64::MAX);
    for _ in 0..2 {
        assert_eq!(
            resource_claim(&store, first.clone(), resource("r"), 1).err(),
            Some(TOKEN_EXHAUSTED.into())
        );
    }
    let (claim, reason) = claim_result(
        &lua,
        service_claim(&store, first.clone(), "ecs::GameSettings", 1),
    )
    .unwrap();
    assert!(claim.is_none());
    assert_eq!(
        reason.as_deref(),
        Some("runtime claim token space exhausted")
    );
    // The session is checked before a token is taken.
    first.set_session(0);
    assert_eq!(
        component_claim(&store, first.clone(), 7, STAMINA, None).err(),
        Some(SESSION_REFUSAL.into())
    );
    // The last claim still works.
    first.set_session(1);
    last.acquire().unwrap();
    assert_eq!(store.tokens_on(last.key()), 1);
}

/// A claim the mod drops while active keeps its storage: no Drop releases it.
#[test]
fn a_leaked_active_claim_keeps_its_storage() {
    let lua = Lua::new();
    let store = Rc::new(OwnershipStore::new());
    let first = FakeAuthority::new("first-mod", 1);
    let second = FakeAuthority::new("second-mod", 1);
    let (claim, _) = claim_result(
        &lua,
        component_claim(&store, first.clone(), 7, STAMINA, None),
    )
    .unwrap();
    lua.globals().set("leaked", claim.unwrap()).unwrap();
    lua.load("assert(leaked:acquire()); leaked = nil")
        .exec()
        .unwrap();
    lua.gc_collect().unwrap();
    lua.gc_collect().unwrap();
    let key = component_key(first.as_ref(), 7, STAMINA);
    assert_eq!(store.owner_of(&key).as_deref(), Some("first-mod"));
    let label = format!("component {STAMINA} on entity 7");
    assert_eq!(
        component_claim(&store, second.clone(), 7, STAMINA, None)
            .unwrap()
            .acquire(),
        Err(owned_by(&label, "first-mod"))
    );
    assert_eq!(
        check_write(&store, second.as_ref(), &key),
        Err(owned_by(&label, "first-mod"))
    );
    // Dropping a Rust claim does not release either.
    let held = component_claim(&store, first.clone(), 9, STAMINA, None).unwrap();
    held.acquire().unwrap();
    drop(held);
    assert_eq!(store.len(), 2);
    // The claim's owner itself may keep writing and claiming.
    assert_eq!(check_write(&store, first.as_ref(), &key), Ok(()));
    let again = component_claim(&store, first.clone(), 7, STAMINA, None).unwrap();
    again.acquire().unwrap();
    again.release();
    assert_eq!(store.tokens_on(&key), 1, "the leaked token remains");
}

/// Tokens of one mod are counted; release is manual, once, and only for an acquired claim.
#[test]
fn refcounts_and_manual_release() {
    let store = Rc::new(OwnershipStore::new());
    let first = FakeAuthority::new("first-mod", 1);
    let key = StorageKey::Service {
        session: 1,
        key: "ecs::GameSettings".into(),
    };
    let claims: Vec<_> = (0..3)
        .map(|_| service_claim(&store, first.clone(), "ecs::GameSettings", 1).unwrap())
        .collect();
    let never = service_claim(&store, first.clone(), "ecs::GameSettings", 1).unwrap();
    assert!(claims.iter().all(|c| !c.is_active()));
    assert!(store.is_empty(), "making a claim records nothing");
    for (n, claim) in claims.iter().enumerate() {
        claim.acquire().unwrap();
        claim.acquire().unwrap();
        assert_eq!(store.tokens_on(&key), n + 1, "acquiring twice counts once");
    }
    never.release();
    assert_eq!(
        store.tokens_on(&key),
        3,
        "an inactive claim releases nothing"
    );
    claims[0].release();
    assert!(!claims[0].is_active(), "a released claim is inactive");
    claims[0].release();
    assert_eq!(store.tokens_on(&key), 2, "released once");
    claims[1].release();
    assert_eq!(store.owner_of(&key).as_deref(), Some("first-mod"));
    claims[2].release();
    assert!(store.is_empty());
    // Released claims can acquire again.
    claims[0].acquire().unwrap();
    assert_eq!(store.tokens_on(&key), 1);
    // The store's own rules: another owner's release of a token is ignored.
    store.release("second-mod", &key, claims[0].token());
    assert_eq!(store.tokens_on(&key), 1);
    store.release("first-mod", &key, 999);
    assert_eq!(store.tokens_on(&key), 1, "an unknown token changes nothing");
}

/// Two owners on one key, for each kind: the first keeps it, the second gets the label.
#[test]
fn two_owners_on_one_key() {
    let store = Rc::new(OwnershipStore::new());
    let first = FakeAuthority::new("first-mod", 4);
    let second = FakeAuthority::new("second-mod", 4);
    type Make =
        fn(&Rc<OwnershipStore>, Rc<dyn ClaimAuthority>) -> Result<RuntimeWriteClaim, String>;
    let cases: [(Make, &str); 4] = [
        (
            |s, a| component_claim(s, a, 42, "keen::ecs::Health", None),
            "component keen::ecs::Health on entity 42",
        ),
        (
            |s, a| resource_claim(s, a, resource("6f1c-guid"), 4),
            "loaded resource 6f1c-guid",
        ),
        (
            |s, a| service_claim(s, a, "ecs::GameSettings", 4),
            "named service ecs::GameSettings",
        ),
        (
            |s, a| template_claim(s, a, resource("6f1c-guid"), 4),
            "loaded resource 6f1c-guid",
        ),
    ];
    for (make, label) in cases {
        let mine = make(&store, first.clone()).unwrap();
        let theirs = make(&store, second.clone()).unwrap();
        mine.acquire().unwrap();
        assert_eq!(theirs.acquire(), Err(owned_by(label, "first-mod")));
        assert!(!theirs.is_active());
        assert_eq!(theirs.owner(), "second-mod");
        mine.release();
        theirs.acquire().unwrap();
        assert_eq!(mine.acquire(), Err(owned_by(label, "second-mod")));
        theirs.release();
        assert!(store.is_empty());
    }
    // A template shares its resource's ownership.
    let template = template_claim(&store, first.clone(), resource("shared"), 4).unwrap();
    template.acquire().unwrap();
    assert_eq!(
        resource_claim(&store, second.clone(), resource("shared"), 4)
            .unwrap()
            .acquire(),
        Err(owned_by("loaded resource shared", "first-mod"))
    );
}

/// Acquire's order: session, then for each operation denial before availability, then the
/// family's check, then the store; a refusal stops the rest.
#[test]
fn acquire_checks_in_shroudforges_order() {
    let store = Rc::new(OwnershipStore::new());
    let first = FakeAuthority::new("first-mod", 1);
    let checks = Rc::new(Cell::new(0));
    let failing = Rc::new(Cell::new(true));
    let (c, f) = (checks.clone(), failing.clone());
    let check: Rc<dyn AcquireCheck> = Rc::new(move || {
        c.set(c.get() + 1);
        if f.get() {
            Err("claimed entity generation or component write authority is unavailable".to_owned())
        } else {
            Ok(())
        }
    });
    let claim = component_claim(&store, first.clone(), 7, STAMINA, Some(check)).unwrap();
    first.deny(Op::EcsWrite, "denied");
    assert_eq!(claim.acquire(), Err("denied".into()));
    assert_eq!(*first.asked.borrow(), ["denial runtime.ecs.write"]);
    assert_eq!(checks.get(), 0, "no family check after a denial");
    first.clear();
    first.make_unavailable(Op::EcsWrite);
    assert!(claim.acquire().is_err());
    assert_eq!(
        *first.asked.borrow(),
        ["denial runtime.ecs.write", "available runtime.ecs.write"]
    );
    assert_eq!(checks.get(), 0);
    first.clear();
    assert_eq!(
        claim.acquire(),
        Err("claimed entity generation or component write authority is unavailable".into())
    );
    assert_eq!(checks.get(), 1);
    assert!(store.is_empty(), "the family's refusal records nothing");
    failing.set(false);
    claim.acquire().unwrap();
    assert_eq!(checks.get(), 2, "checked again on every acquire");
    // A resource asks both operations, ecs first.
    first.clear();
    let asset = resource_claim(&store, first.clone(), resource("r"), 1).unwrap();
    asset.acquire().unwrap();
    assert_eq!(
        *first.asked.borrow(),
        [
            "denial runtime.ecs.write",
            "available runtime.ecs.write",
            "denial runtime.assets.write",
            "available runtime.assets.write"
        ]
    );
    // Session first: a stale claim asks nothing.
    first.clear();
    first.set_session(3);
    assert_eq!(asset.acquire(), Err(SESSION_REFUSAL.into()));
    assert!(first.asked.borrow().is_empty());
}

/// A fake target over one Lua global.
struct GlobalTarget {
    claim: AnyUserData,
    shared: bool,
    normalizes: bool,
    canonical: bool,
}

impl ClaimTarget for GlobalTarget {
    fn key(&self) -> String {
        "view/1/ecs::GameSettings/speed".into()
    }
    fn claim(&self) -> AnyUserData {
        self.claim.clone()
    }
    fn shared(&self) -> bool {
        self.shared
    }
    fn read(&self, lua: &Lua) -> mlua::Result<Result<Table, String>> {
        let value: Value = lua.globals().get("current")?;
        if value.is_nil() {
            return Ok(Err("Field is not a finite scalar: speed".into()));
        }
        if let Value::String(s) = &value
            && s.to_str()? == "raise"
        {
            return Err(mlua::Error::runtime("read raised"));
        }
        let snapshot = lua.create_table()?;
        snapshot.raw_set("value", value)?;
        Ok(Ok(snapshot))
    }
    fn write(
        &self,
        lua: &Lua,
        snapshot: Table,
        value: Value,
    ) -> mlua::Result<Result<Written, String>> {
        if lua.globals().get::<bool>("busy")? {
            return Ok(Err("Scene gate is busy".into()));
        }
        assert_eq!(
            snapshot.raw_get::<Value>("value")?,
            lua.globals().get::<Value>("current")?
        );
        lua.globals().set("current", value.clone())?;
        Ok(Ok(Written {
            actual: value.clone(),
            canonical: self.canonical.then_some(value),
        }))
    }
    fn normalizes(&self) -> bool {
        self.normalizes
    }
    fn normalize(&self, _: &Lua, _: Table, value: Value) -> mlua::Result<Result<Value, String>> {
        Ok(match value {
            Value::Integer(n) if n >= 0 => Ok(Value::Integer(n.min(10))),
            _ => Err("value out of range".into()),
        })
    }
}

/// The target table has ShroudForge's shape, and the claim it carries is the claim.
#[test]
fn a_target_has_shroudforges_lua_shape() {
    let lua = Lua::new();
    let store = Rc::new(OwnershipStore::new());
    let first = FakeAuthority::new("first-mod", 1);
    let make = |shared, normalizes, canonical| {
        let (claim, _) = claim_result(
            &lua,
            service_claim(&store, first.clone(), "ecs::GameSettings", 1),
        )
        .unwrap();
        target_table(
            &lua,
            Rc::new(GlobalTarget {
                claim: claim.unwrap(),
                shared,
                normalizes,
                canonical,
            }),
        )
        .unwrap()
    };
    lua.globals()
        .set("plain", make(false, false, false))
        .unwrap();
    lua.globals().set("view", make(true, true, true)).unwrap();
    lua.globals().set("busy", false).unwrap();
    lua.load(
        r#"
        local function pack(...) return table.pack(...) end
        assert(plain.key == "view/1/ecs::GameSettings/speed")
        assert(plain.shared == nil and plain.normalize == nil)
        assert(view.shared == true and type(view.normalize) == "function")
        assert(rawequal(plain.claim, plain.claim) and type(plain.claim) == "userdata")
        -- read: snapshot, or nil + reason; a raised error propagates
        current = nil
        local r = pack(plain.read())
        assert(r.n == 2 and r[1] == nil and r[2] == "Field is not a finite scalar: speed")
        current = 3
        r = pack(plain.read())
        assert(r.n == 1 and r[1].value == 3)
        current = "raise"
        local ok, err = pcall(plain.read)
        assert(not ok and tostring(err):find("read raised", 1, true))
        -- write: true, actual / true, actual, nil, canonical / false, nil, reason
        current = 3
        local snap = plain.read()
        r = pack(plain.write(snap, 5))
        assert(r.n == 2 and r[1] == true and r[2] == 5)
        snap = view.read()
        r = pack(view.write(snap, 6))
        assert(r.n == 4 and r[1] == true and r[2] == 6 and r[3] == nil and r[4] == 6)
        busy = true
        r = pack(view.write(view.read(), 7))
        assert(r.n == 3 and r[1] == false and r[2] == nil and r[3] == "Scene gate is busy")
        busy = false
        -- normalize: value, or nil + reason
        r = pack(view.normalize(snap, 99))
        assert(r.n == 1 and r[1] == 10)
        r = pack(view.normalize(snap, -1))
        assert(r.n == 2 and r[1] == nil and r[2] == "value out of range")
        -- the tracker's use of the claim
        assert(plain.claim:acquire())
        plain.claim:release()
        "#,
    )
    .exec()
    .unwrap();
    assert!(store.is_empty());
    // Anything but a RuntimeWriteClaim as the claim is refused.
    let bogus = lua.create_any_userdata(5u32).unwrap();
    let err = target_table(
        &lua,
        Rc::new(GlobalTarget {
            claim: bogus,
            shared: false,
            normalizes: false,
            canonical: false,
        }),
    )
    .unwrap_err();
    assert!(
        err.to_string().contains("must be a RuntimeWriteClaim"),
        "{err}"
    );
}

/// A claim authority whose live session moves on after its first read, as a world change
/// between two reads of the any-thread session would.
struct MovingSession {
    reads: Cell<u32>,
}

impl ClaimAuthority for MovingSession {
    fn owner_id(&self) -> &str {
        "first-mod"
    }
    fn world_session(&self) -> u64 {
        let reads = self.reads.get();
        self.reads.set(reads + 1);
        if reads == 0 { 1 } else { 2 }
    }
    fn denial(&self, _: Op) -> Option<String> {
        None
    }
    fn available(&self, _: Op) -> bool {
        true
    }
}

/// A component claim reads the live session once, as ShroudForge's get_component_claim does:
/// a world change after that read refuses the claim instead of pairing a key of one session
/// with a token of the next (whose record the next prune would drop under an active claim).
#[test]
fn a_component_claim_reads_the_session_once() {
    let store = Rc::new(OwnershipStore::new());
    let moving = Rc::new(MovingSession {
        reads: Cell::new(0),
    });
    match component_claim(&store, moving, 7, STAMINA, None) {
        Err(reason) => assert_eq!(reason, SESSION_REFUSAL),
        Ok(claim) => panic!(
            "claim made across a world change: key {:?}, token session {}",
            claim.key(),
            claim.session()
        ),
    }
}
