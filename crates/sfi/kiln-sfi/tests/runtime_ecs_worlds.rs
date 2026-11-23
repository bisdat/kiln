// SPDX-License-Identifier: MIT
// the interpreter's runtime.ecs world choice tests.
//! Which world `runtime.ecs` binds, on the production runtime facts with a live world context (the core's session
//! record, kiln.world's generations and the worker's lifecycle stream): a dedicated server and a hosting client
//! (single player included) bind the server world, read and write it; a joined client binds its client world, reads
//! it and refuses writes; a client that hosts, goes back to the menu and joins another world drops the hosted world's
//! handles; the session write latch is per world kind and session.
//! ShroudForge (MIT, Copyright (c) 2026 bonsaibauer) takes the hosted scene as the local authority on a hosting client,
//! 1.7.9 d8a394bc: modules/game-runtime/native/engine-context/resolve_engine_owners.cpp:65,133; the refusal texts are
//! those of lua/ingame/ecs/{access_component_fields,write_component}.rs and bridge/windows/ecs/update_component.rs.
#[path = "shared/ecs_fixture.rs"]
mod fixture;

use fixture::support::{session_lock, set_session, set_session_side};
use fixture::*;
use kiln_abi::services::kiln_caps::{KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE};
use kiln_sfi::runtime::ecs::cas::text as cas;
use kiln_sfi::runtime::ecs::catalog::text::NOT_READY;
use kiln_sfi::runtime::ecs::entities::text as q;
use kiln_sfi::runtime::unavailable::NO_RESOURCE_READ;
use kiln_sfi::runtime::world::context::WorldContext;
use kiln_sfi::worker::{LifecycleEvent, WorldLifecycle};
use sfi_core::host::Side;
use std::sync::{Mutex, MutexGuard};

const CLIENT: u32 = KILN_WORLD_CLIENT as u32;
const UNCERTAIN: &str = "ecs-write-outcome-uncertain-rollback-unverified";
const CAPS: [&str; 3] = ["world.lifecycle", "world.ecs_read", "world.ecs_write"];

// ---- one process: its side, its session record, its worlds ----

static SERIAL: Mutex<()> = Mutex::new(());
/// The session record and the capability rows are process-wide: the tests take turns and put them back.
struct Turn {
    _serial: MutexGuard<'static, ()>,
    _session: MutexGuard<'static, ()>,
}
impl Drop for Turn {
    fn drop(&mut self) {
        set_session(KILN_WORLD_SERVER, 0);
        set_session(KILN_WORLD_CLIENT, 0);
        set_session_side(true);
        for cap in CAPS {
            fake_services::set_cap(cap, None);
        }
    }
}
struct Process {
    s: Setup,
    context: Rc<WorldContext>,
    _turn: Turn,
}
/// A process on `side` over `world` (kiln.world's ECS fake), in the menu: the family follows the production facts.
fn process(side: Side, world: EcsWorld) -> Process {
    let turn = Turn {
        _serial: SERIAL.lock().unwrap_or_else(|e| e.into_inner()),
        _session: session_lock(),
    };
    set_session_side(side == Side::Server);
    for cap in CAPS {
        fake_services::set_cap(cap, Some((KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE)));
    }
    let s = setup(world);
    s.follow_facts.set(true);
    let context = Rc::new(WorldContext::new(s.services.clone(), side, Box::new(|_| 0)));
    s.facts.attach_world(context.clone());
    Process {
        s,
        context,
        _turn: turn,
    }
}
impl Process {
    /// World `kind` starts: the core's count `count`, kiln.world's generation `generation`, its STARTED drained.
    fn start(&self, kind: u32, count: u32, generation: u64) {
        set_session(kind as i32, count);
        fake_services::set_world_session(self.s.owner, kind, generation);
        self.context.apply(WorldLifecycle {
            world: kind,
            session: count,
            event: LifecycleEvent::Started,
        });
    }
    fn stop(&self, kind: u32, count: u32) {
        self.context.apply(WorldLifecycle {
            world: kind,
            session: count,
            event: LifecycleEvent::Stopping,
        });
        set_session(kind as i32, 0);
    }
    /// The scripted host takes the production facts' readiness and write context.
    fn sync_facts(&self) {
        let now = self.s.facts.provider_facts();
        self.s.facts(|f| {
            f.ready = now.ready;
            f.can_write = now.can_write;
        });
    }
}

fn health(hp: u32, max: u32) -> (u32, Vec<u8>) {
    (hash(HEALTH), words(&[hp, max]))
}
fn world_of(kind: u32, session: u64) -> EcsWorld {
    let mut world = EcsWorld::server().with(7, &[health(30, 100)]);
    world.world_kind = kind;
    world.session = session;
    world
}
fn write_hp(s: &Setup, h: i64, hp: u32) -> (bool, Option<String>) {
    let (ok, why, _): (bool, Option<String>, Value) = s.eval(&format!(
        "return ecs.write_fields({h}, 'keen::ecs::Health', {{hp = {hp}}})"
    ));
    (ok, why)
}

#[test]
fn a_hosting_client_reads_and_writes_the_hosted_server_world() {
    let p = process(Side::Client, world_of(SERVER, 5));
    let s = &p.s;
    // the menu: the plan side's world, no session
    assert_eq!(s.facts.ecs_world(), (CLIENT, 0));
    assert!(!s.facts.provider_facts().ready);
    // hosting (single player included): the server world runs here, then the client world
    p.start(SERVER, 1, 5);
    p.start(CLIENT, 1, 3);
    assert_eq!(s.facts.ecs_world(), (SERVER, 5));
    let facts = s.facts.provider_facts();
    assert!(facts.ready && facts.can_write);
    // ShroudForge's session marker of the hosted world, beside the client context's own
    assert_eq!(s.facts.live_session(SERVER), 5);
    assert_eq!(s.facts.live_session(CLIENT), 3);
    assert_eq!(s.ecs.world_session(), 5);
    p.sync_facts();
    let h = s.handle(7);
    assert_eq!(s.ecs.object(h as u32).unwrap().world_kind, SERVER);
    assert_eq!(write_hp(s, h, 42), (true, None));
    assert_eq!(
        s.world(|w| w.writes.last().cloned()).unwrap().2,
        words(&[42, 100])
    );
    // the claims and the template lookup have the hosted world's session
    s.world(|w| w.entities.get_mut(&7).unwrap().template = Some([1; 16]));
    assert_eq!(
        s.reason(&format!("ecs.get_template({h})")),
        NO_RESOURCE_READ
    );
}

#[test]
fn a_host_transition_does_not_bind_the_resident_client_world() {
    let p = process(Side::Client, world_of(CLIENT, 3));
    p.start(SERVER, 1, 5);
    p.start(CLIENT, 1, 3);
    assert_eq!(p.s.facts.ecs_world(), (SERVER, 5));

    // The hosted world has begun stopping, but the core still reports both worlds: the hosted scene is not ready,
    // so the provider binds no world (resolve_engine_owners.cpp:56) and nothing reads the resident client world.
    p.context.apply(WorldLifecycle {
        world: SERVER,
        session: 1,
        event: LifecycleEvent::Stopping,
    });
    assert_eq!(
        p.context.ecs_world(),
        Err("ecs-active-scene-is-transitioning")
    );
    assert_eq!(p.s.facts.ecs_world(), (SERVER, 0));
    let facts = p.s.facts.provider_facts();
    assert!(!facts.ready && !facts.can_write && !facts.resource_can_write);
    assert_eq!(p.s.ecs.open(7), Err(KILN_ERR_UNAVAILABLE));
    assert_eq!(p.s.ecs.query_refs(&[1]).err(), Some(q::QUERY_FAILED));
    // Mods get the provider's own answers, not the resolver's reason.
    p.sync_facts();
    assert_eq!(p.s.reason("ecs.query('keen::ecs::Health')"), NOT_READY);
    assert_eq!(
        p.s.reason("ecs.resolve(7)"),
        format!("{}7", q::ENTITY_UNAVAILABLE)
    );
}

#[test]
fn a_joined_client_reads_its_client_world_and_its_writes_are_refused() {
    let p = process(Side::Client, world_of(CLIENT, 11));
    let s = &p.s;
    p.start(CLIENT, 2, 11);
    assert_eq!(s.facts.ecs_world(), (CLIENT, 11));
    let facts = s.facts.provider_facts();
    assert!(facts.ready && !facts.can_write);
    assert_eq!(s.facts.live_session(SERVER), 0);
    p.sync_facts();
    let h = s.handle(7);
    assert_eq!(s.ecs.object(h as u32).unwrap().world_kind, CLIENT);
    let (hp,): (i64,) = s.eval(&format!(
        "return ecs.read_fields({h}, 'keen::ecs::Health', {{'hp'}}).hp"
    ));
    assert_eq!(hp, 30);
    // ShroudForge's write context: unavailable for write_fields, the provider's for write
    assert_eq!(
        write_hp(s, h, 1),
        (
            false,
            Some("runtime.ecs.write is currently unavailable".into())
        )
    );
    // kiln.world's policy for a world that is not the server's
    fake_services::set_write_policy(
        s.owner,
        hash(HEALTH),
        Some((
            KILN_ECS_UNAVAILABLE as u32,
            KILN_ECS_WRITE_WHY_NOT_SERVER_WORLD as u32,
        )),
    );
    s.eval::<()>(&format!("value = ecs.read({h}, 'keen::ecs::Health')"));
    let lease = *s.codec.borrow().sources.keys().next().unwrap();
    s.codec.borrow_mut().exports.insert(lease, words(&[1, 100]));
    assert_eq!(
        s.reason(&format!("ecs.write({h}, 'keen::ecs::Health', value)")),
        cas::NO_WRITE_CONTEXT
    );
    assert!(s.world(|w| w.writes.is_empty()));
}

#[test]
fn a_dedicated_server_binds_its_server_world() {
    let p = process(Side::Server, world_of(SERVER, 5));
    let s = &p.s;
    assert_eq!(s.facts.ecs_world(), (SERVER, 0));
    p.start(SERVER, 1, 5);
    assert_eq!(s.facts.ecs_world(), (SERVER, 5));
    let facts = s.facts.provider_facts();
    assert!(facts.ready && facts.can_write);
    assert_eq!(s.facts.live_session(SERVER), 5);
    p.sync_facts();
    let h = s.handle(7);
    assert_eq!(write_hp(s, h, 9), (true, None));
}

#[test]
fn hosting_then_the_menu_then_joining_drops_the_hosted_world_s_handles() {
    let p = process(Side::Client, world_of(SERVER, 5));
    let s = &p.s;
    p.start(SERVER, 1, 5);
    p.start(CLIENT, 1, 3);
    p.sync_facts();
    let hosted = s.handle(7);
    let read = |h: i64| -> (Value, Option<String>) {
        s.eval(&format!("return ecs.read_bytes({h}, 'keen::ecs::Health')"))
    };
    assert!(read(hosted).1.is_none());
    // back to the menu: both worlds stop, nothing is bound
    p.stop(CLIENT, 1);
    p.stop(SERVER, 1);
    assert_eq!(s.facts.ecs_world(), (CLIENT, 0));
    assert_eq!(s.facts.live_session(SERVER), 0);
    assert_eq!(
        read(hosted).1,
        Some(format!("component read failed: {HEALTH}"))
    );
    // joining another world: its client world, new handles, the hosted world's handles gone
    s.world(|w| {
        w.world_kind = CLIENT;
        w.session = 11;
    });
    p.start(CLIENT, 2, 11);
    assert_eq!(s.facts.ecs_world(), (CLIENT, 11));
    let joined = s.handle(7);
    assert_ne!(joined, hosted);
    assert!(read(joined).1.is_none());
    assert!(s.ecs.handles().get(hosted as u32).is_none());
    assert_eq!(
        read(hosted).1,
        Some(format!("component read failed: {HEALTH}"))
    );
}

#[test]
fn the_write_latch_is_per_world_kind_and_session() {
    let p = process(Side::Client, world_of(SERVER, 5));
    let s = &p.s;
    p.start(SERVER, 1, 5);
    p.start(CLIENT, 1, 3);
    p.sync_facts();
    let h = s.handle(7);
    s.world(|w| w.cas = [CasStep::Answer(Knob::Uncertain)].into());
    assert_eq!(write_hp(s, h, 41), (false, Some(UNCERTAIN.into())));
    // latched for the hosted server world's session
    assert_eq!(s.facts.write_fault().as_deref(), Some(UNCERTAIN));
    assert_eq!(
        s.facts
            .write_faults()
            .fault()
            .map(|f| (f.world_kind, f.session)),
        Some((SERVER, 5))
    );
    // the client world's sessions do not end it
    fake_services::set_world_session(s.owner, CLIENT, 9);
    assert_eq!(s.facts.write_fault().as_deref(), Some(UNCERTAIN));
    // a new session of the hosted world does
    fake_services::set_world_session(s.owner, SERVER, 6);
    assert_eq!(s.facts.write_fault(), None);
}
