// SPDX-License-Identifier: MIT
//! The runtime's service adapter, the fake services, the timed native-call
//! guard and the runtime installer, through real C-ABI fake tables
//! (`support/fake_services`) and, where the seam is the lifecycle host's, the real
//! worker. The hop refusal on a core without `world.lifecycle` is in `hops.rs`.
#[path = "support/fake_services/mod.rs"]
mod fake_services;
mod support;

use fake_services::{Knob, Shape};
use kiln_abi::services::kiln_world::*;
use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::runtime::RuntimeContext as Cx;
use kiln_sfi::runtime::services::{CallbackSink, ServiceAdapter, Unavailable};
use kiln_sfi::runtime::{Families, Family};
use kiln_sfi::service_member;
use kiln_sfi::worker::compose::{ComposePlan, RuntimeHosts};
use kiln_sfi::worker::*;
use mlua::{Lua, Table, Value};
use serde_json::json;
use sfi_core::bindings::runtime::{
    Availability, EffectState, RuntimeHost, RuntimeOperation, RuntimeOwnerContext,
    RuntimeProviderFacts,
};
use sfi_core::compose::{Composed, GameTypes};
use sfi_core::host::{HostContext, Package as RuntimePackage, PackageSource, Side, SourceVersion};
use sfi_core::lifecycle::{Cleanup, Monotonic};
use sfi_core::locales::LocaleSource;
use sfi_core::package::{
    self,
    discover::{PackageRecord, Registry},
};
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Barrier, LazyLock, Mutex};
use std::time::Duration;
use support::*;

const WAIT: Duration = Duration::from_secs(10);

fn adapter(lookup: bool) -> (ServiceAdapter, Box<KilnApi>) {
    let api = fake_services::api(lookup);
    let host = unsafe { Host::read(&*api) };
    let watchdog = Rc::new(Watchdog::new(process_clock()));
    (ServiceAdapter::new(host, watchdog), api)
}

/// A well-formed ref of a server-world entity opened under `owner`'s epoch.
fn server_ref(owner: KilnOwner, entity: KilnEntity) -> KilnObjectRef {
    KilnObjectRef {
        size: std::mem::size_of::<KilnObjectRef>() as u32,
        entity,
        generation: 1,
        world_kind: KILN_WORLD_SERVER as u32,
        session: fake_services::FAKE_SESSION,
        layout_stamp: 1,
        reload_epoch: owner,
    }
}

// ---- lookup, header and member checks ----

#[test]
fn a_table_is_bound_only_through_the_lookup_with_a_whole_header_and_its_version() {
    let (none, _api) = adapter(false);
    let error = none
        .bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)
        .err()
        .unwrap();
    assert_eq!(
        (error.service, error.why),
        ("kiln.world", Unavailable::NoLookup)
    );

    let (services, _api) = adapter(true);
    let why = |shape| {
        fake_services::set_shape("kiln.world", shape);
        let outcome = services.bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION);
        fake_services::set_shape("kiln.world", None);
        outcome.map(|t| (t.size(), t.version())).map_err(|e| e.why)
    };
    let full = std::mem::size_of::<KilnWorldApi>() as u32;
    assert_eq!(why(None), Ok((full, KILN_WORLD_VERSION)));
    assert_eq!(why(Some(Shape::Missing)), Err(Unavailable::Missing));
    assert_eq!(why(Some(Shape::Misaligned)), Err(Unavailable::Misaligned));
    assert_eq!(
        why(Some(Shape::Short(4))),
        Err(Unavailable::Header { size: 4 })
    );
    assert_eq!(why(Some(Shape::Short(8))), Ok((8, KILN_WORLD_VERSION)));
    assert_eq!(
        why(Some(Shape::Version(KILN_WORLD_VERSION - 1))),
        Err(Unavailable::Version { have: 3, need: 4 })
    );
    let unknown = services.bind::<KilnWorldApi>(b"kiln.nothing\0", 1);
    assert_eq!(unknown.err().unwrap().why, Unavailable::Missing);
    let counters = services.counters();
    assert_eq!(
        (counters.bound, counters.refused, counters.calls),
        (2, 5, 0)
    );
}

#[test]
fn a_member_past_the_table_s_size_is_never_read_and_a_null_one_never_called() {
    let (services, _api) = adapter(true);
    let owner = next_owner();
    let query = std::mem::offset_of!(KilnWorldApi, ecs_query);
    // A table that ends one byte before `ecs_query`'s slot does: an exact-size heap copy,
    // so a read past it is a heap overflow under ASan.
    for (size, readable) in [(query + 7, false), (query + 8, true)] {
        fake_services::set_shape("kiln.world", Some(Shape::Short(size as u32)));
        let table = services
            .bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)
            .unwrap();
        fake_services::set_shape("kiln.world", None);
        let member = service_member!(table, KilnWorldApi, ecs_query);
        if readable {
            let member = member.unwrap();
            assert_eq!(
                (member.service(), member.name()),
                ("kiln.world", "ecs_query")
            );
        } else {
            assert_eq!(
                member.err().unwrap().why,
                Unavailable::Short {
                    member: "ecs_query",
                    size: size as u32,
                    need: query + 8
                }
            );
        }
        // A member wholly inside the prefix still works.
        let status = service_member!(table, KilnWorldApi, status).unwrap();
        fake_services::bind_thread(owner);
        let mut out: KilnWorldStatus = unsafe { std::mem::zeroed() };
        assert_eq!(unsafe { services.call(status, (&raw mut out,)) }, KILN_OK);
    }
    // A NULL slot: the copy's `ecs_query` is zeroed, and the fake never sets
    // `register_program` at all.
    fake_services::set_shape("kiln.world", Some(Shape::NullAt(query)));
    let table = services
        .bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)
        .unwrap();
    fake_services::set_shape("kiln.world", None);
    assert_eq!(
        service_member!(table, KilnWorldApi, ecs_query)
            .err()
            .unwrap()
            .why,
        Unavailable::Null {
            member: "ecs_query"
        }
    );
    assert_eq!(
        service_member!(table, KilnWorldApi, register_program)
            .err()
            .unwrap()
            .why,
        Unavailable::Null {
            member: "register_program"
        }
    );
    assert_eq!(fake_services::counters(owner).calls("ecs_query"), 0);
    assert_eq!(fake_services::counters(owner).calls("status"), 2);
}

#[test]
fn an_older_table_version_is_unavailable_and_none_of_its_members_is_called() {
    let (services, _api) = adapter(true);
    let owner = next_owner();
    fake_services::bind_thread(owner);
    for (service, shape_name) in [
        (fake_services::FAKE_TIMED_SERVICE, "kiln.fake.timed"),
        (KILN_WORLD_SERVICE, "kiln.world"),
    ] {
        fake_services::set_shape(shape_name, Some(Shape::Version(0)));
        let error = services.bind::<KilnWorldApi>(service, 1).err().unwrap();
        fake_services::set_shape(shape_name, None);
        assert_eq!(error.why, Unavailable::Version { have: 0, need: 1 });
        assert_eq!(error.service, shape_name);
    }
    assert_eq!(fake_services::counters(owner).total_calls(), 0);
}

#[test]
fn a_member_slot_is_copied_once_at_bind_and_live_tokens_pass_per_call() {
    let (services, _api) = adapter(true);
    let owner = next_owner();
    fake_services::set_shape("kiln.world", Some(Shape::Copy));
    let table = services
        .bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)
        .unwrap();
    fake_services::set_shape("kiln.world", None);
    let validate = service_member!(table, KilnWorldApi, entity_validate).unwrap();
    // The table changes after the bind (it never does in Kiln; a torn or hostile one
    // might): the copied slot is what gets called, never a re-read of the table.
    static SWAPPED: AtomicU64 = AtomicU64::new(0);
    unsafe extern "C" fn swapped(
        _: KilnOwner,
        _: *const KilnObjectRef,
        _: *mut KilnObjectInfo,
    ) -> KilnResult {
        SWAPPED.fetch_add(1, Ordering::SeqCst);
        KILN_OK
    }
    let slot = std::mem::offset_of!(KilnWorldApi, entity_validate);
    type Validate =
        unsafe extern "C" fn(KilnOwner, *const KilnObjectRef, *mut KilnObjectInfo) -> KilnResult;
    unsafe {
        fake_services::last_copy()
            .add(slot)
            .cast::<Option<Validate>>()
            .write(Some(swapped))
    };
    // Two live tokens, each passed to its own call.
    for entity in [7u32, 8] {
        let token = KilnObjectRef {
            size: std::mem::size_of::<KilnObjectRef>() as u32,
            entity,
            generation: 1,
            world_kind: KILN_WORLD_SERVER as u32,
            session: 1,
            layout_stamp: 1,
            reload_epoch: owner,
        };
        let mut info: KilnObjectInfo = unsafe { std::mem::zeroed() };
        info.size = std::mem::size_of::<KilnObjectInfo>() as u32;
        assert_eq!(
            unsafe { services.call(validate, (owner, &raw const token, &raw mut info)) },
            KILN_OK
        );
        assert_eq!((info.entity, info.status), (entity, KILN_ECS_OK as u32));
    }
    assert_eq!(fake_services::counters(owner).calls("entity_validate"), 2);
    assert_eq!(SWAPPED.load(Ordering::SeqCst), 0);
    // A fresh bind reads the table again.
    let rebound = service_member!(
        services
            .bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)
            .unwrap(),
        KilnWorldApi,
        entity_validate
    )
    .unwrap();
    let mut info: KilnObjectInfo = unsafe { std::mem::zeroed() };
    let token: KilnObjectRef = unsafe { std::mem::zeroed() };
    unsafe { services.call(rebound, (owner, &raw const token, &raw mut info)) };
    assert_eq!(fake_services::counters(owner).calls("entity_validate"), 3);
}

#[test]
fn a_direct_call_is_one_call_with_no_hop_wait_or_retry_whatever_it_answers() {
    let (services, _api) = adapter(true);
    let table = services
        .bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)
        .unwrap();
    let cas = service_member!(table, KilnWorldApi, ecs_compare_exchange).unwrap();
    for knob in [
        Knob::Ok,
        Knob::Busy,
        Knob::Stale,
        Knob::Unreadable,
        Knob::Unavailable,
        Knob::Uncertain,
        Knob::Conflict,
    ] {
        let owner = next_owner();
        fake_services::set_knob(owner, knob);
        let object = server_ref(owner, 7);
        let mut reply: KilnMutationReply = unsafe { std::mem::zeroed() };
        reply.size = std::mem::size_of::<KilnMutationReply>() as u32;
        let bytes = [0u8; 4];
        let result = unsafe {
            services.call(
                cas,
                (
                    owner,
                    &raw const object,
                    1,
                    bytes.as_ptr().cast(),
                    bytes.as_ptr().cast(),
                    bytes.as_ptr().cast(),
                    4,
                    std::ptr::null_mut(),
                    &raw mut reply,
                ),
            )
        };
        // The member's own answer comes back untouched: the family maps it.
        assert_eq!(result, knob.ecs_result(), "{knob:?}");
        assert_eq!(reply.status, knob.ecs_status(), "{knob:?}");
        let counters = fake_services::counters(owner);
        assert_eq!(counters.calls("ecs_compare_exchange"), 1, "{knob:?}");
        assert_eq!((counters.retries, counters.waits, counters.hops), (0, 0, 0));
    }
    assert_eq!(services.counters().calls, 7);
    assert_eq!(services.counters().timed, 0);
}

#[test]
fn every_fake_table_binds_and_answers_its_knobs() {
    use kiln_abi::services::{
        kiln_assets::*, kiln_build::*, kiln_client::*, kiln_hooks::*, kiln_launch::*,
    };
    let (services, _api) = adapter(true);
    let owner = next_owner();
    fake_services::bind_thread(owner);
    let types = services
        .bind::<KilnTypesApi>(KILN_TYPES_SERVICE, KILN_TYPES_VERSION)
        .unwrap();
    let launch = services
        .bind::<KilnLaunchApi>(KILN_LAUNCH_SERVICE, KILN_LAUNCH_VERSION)
        .unwrap();
    let hooks = services
        .bind::<KilnHooksApi>(KILN_HOOKS_SERVICE, KILN_HOOKS_VERSION)
        .unwrap();
    let client = services
        .bind::<KilnClientApi>(KILN_CLIENT_SERVICE, KILN_CLIENT_VERSION)
        .unwrap();
    let assets = services
        .bind::<KilnAssetsApi>(KILN_ASSETS_SERVICE, KILN_ASSETS_VERSION)
        .unwrap();
    let build = services
        .bind::<KilnBuildApi>(KILN_BUILD_SERVICE, KILN_BUILD_VERSION)
        .unwrap();
    let count = service_member!(types, KilnTypesApi, type_count).unwrap();
    let notice = service_member!(launch, KilnLaunchApi, notice_copy).unwrap();
    let (base, size, read) = (
        service_member!(hooks, KilnHooksApi, image_base).unwrap(),
        service_member!(hooks, KilnHooksApi, image_size).unwrap(),
        service_member!(hooks, KilnHooksApi, read).unwrap(),
    );
    let on_client = service_member!(client, KilnClientApi, on_client_thread).unwrap();
    let assets_status = service_member!(assets, KilnAssetsApi, status).unwrap();
    let build_read = service_member!(build, KilnBuildApi, read).unwrap();
    for knob in [Knob::Ok, Knob::Unavailable, Knob::Busy] {
        fake_services::set_knob(owner, knob);
        let plain = knob.plain_result();
        assert_eq!(
            unsafe { services.call(count, ()) },
            if knob == Knob::Ok { 3 } else { 0 }
        );
        let mut len = 9usize;
        assert_eq!(
            unsafe { services.call(notice, (std::ptr::null_mut(), 0, &raw mut len)) },
            plain
        );
        let image = unsafe { services.call(base, ()) };
        assert_eq!(unsafe { services.call(size, ()) }, 64);
        let mut byte = [0u8; 2];
        assert_eq!(
            unsafe { services.call(read, (image + 3, byte.as_mut_ptr().cast(), 2)) },
            plain
        );
        assert_eq!(byte, if knob == Knob::Ok { [0xcc; 2] } else { [0; 2] });
        assert!(!unsafe { services.call(on_client, ()) });
        assert_eq!(
            unsafe { services.call(assets_status, (std::ptr::null_mut(), 0, &raw mut len)) },
            plain
        );
        assert_eq!(
            unsafe {
                services.call(
                    build_read,
                    (std::ptr::null(), std::ptr::null(), std::ptr::null_mut(), 0),
                )
            },
            plain
        );
    }
    let counters = fake_services::counters(owner);
    assert_eq!(counters.calls("hooks.read"), 3);
    assert_eq!(counters.retries, 0, "other members ran in between");
    // A refused call followed right away by another call of the same member counts as
    // a retry, so later families can assert there was none.
    fake_services::set_knob(owner, Knob::Busy);
    let image = fake_services::IMAGE.as_ptr() as usize;
    let mut byte = [0u8; 1];
    for _ in 0..2 {
        unsafe { services.call(read, (image, byte.as_mut_ptr().cast(), 1)) };
    }
    assert_eq!(fake_services::counters(owner).retries, 1);
}

#[test]
fn the_fake_world_members_follow_kiln_world_h() {
    let (services, _api) = adapter(true);
    let table = services
        .bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)
        .unwrap();
    let open = service_member!(table, KilnWorldApi, entity_open).unwrap();
    let validate = service_member!(table, KilnWorldApi, entity_validate).unwrap();
    let query = service_member!(table, KilnWorldApi, ecs_query).unwrap();
    let snapshot = service_member!(table, KilnWorldApi, ecs_snapshot).unwrap();
    let policy = service_member!(table, KilnWorldApi, ecs_write_policy).unwrap();
    let cas = service_member!(table, KilnWorldApi, ecs_compare_exchange).unwrap();
    let context = service_member!(table, KilnWorldApi, ecs_context_snapshot).unwrap();
    let owner = next_owner();
    fake_services::bind_thread(owner);
    let server = KILN_WORLD_SERVER as u32;

    // entity_open(owner, reload_epoch, world_kind, expected_session, entity, out): the
    // epoch the caller passes is the ref's, the session is the live one.
    let mut opened: KilnObjectRef = unsafe { std::mem::zeroed() };
    let open_with = |epoch: u64, expected: u64, entity: KilnEntity, out: *mut KilnObjectRef| unsafe {
        services.call(open, (owner, epoch, server, expected, entity, out))
    };
    let fields = |r: &KilnObjectRef| {
        (
            r.size,
            r.entity,
            r.generation,
            r.world_kind,
            r.session,
            r.layout_stamp,
            r.reload_epoch,
        )
    };
    assert_eq!(open_with(owner, 0, 7, &raw mut opened), KILN_OK);
    assert_eq!(fields(&opened), fields(&server_ref(owner, 7)));
    let session = fake_services::FAKE_SESSION;
    assert_eq!(open_with(owner, session, 7, &raw mut opened), KILN_OK);
    assert_eq!(
        open_with(owner, session + 1, 7, &raw mut opened),
        KILN_ECS_ERR_STALE
    );
    assert_eq!(opened.size, 0, "the out record is cleared before a refusal");
    assert_eq!(
        open_with(owner, 0, 0, &raw mut opened),
        KILN_ERR_INVALID_ARGUMENT
    );
    let no_owner = unsafe { services.call(open, (0, owner, server, 0, 7, &raw mut opened)) };
    assert_eq!(no_owner, KILN_ERR_INVALID_ARGUMENT);

    // entity_validate: *out (size set) always gets the facts and the KILN_ECS_* status.
    let object = server_ref(owner, 7);
    let mut info: KilnObjectInfo = unsafe { std::mem::zeroed() };
    let validated = |info: *mut KilnObjectInfo| unsafe {
        services.call(validate, (owner, &raw const object, info))
    };
    assert_eq!(
        validated(&raw mut info),
        KILN_ERR_INVALID_ARGUMENT,
        "size not set"
    );
    info.size = std::mem::size_of::<KilnObjectInfo>() as u32;
    fake_services::set_knob(owner, Knob::Stale);
    assert_eq!(validated(&raw mut info), KILN_ECS_ERR_STALE);
    assert_eq!((info.status, info.entity), (KILN_ECS_STALE as u32, 7));

    // ecs_query: *required and *state are optional; *state mirrors the result; busy only
    // for a frame-quiet request.
    let mut request: KilnEcsQueryRequest = unsafe { std::mem::zeroed() };
    request.size = std::mem::size_of::<KilnEcsQueryRequest>() as u32;
    request.world_kind = server;
    let null = std::ptr::null_mut::<u32>();
    let (mut required, mut state) = (9u32, 9u32);
    let queried = |request: &KilnEcsQueryRequest, required: *mut u32, state: *mut u32| unsafe {
        services.call(
            query,
            (
                owner,
                &raw const *request,
                std::ptr::null_mut(),
                0,
                required,
                state,
            ),
        )
    };
    assert_eq!(queried(&request, null, null), KILN_ECS_ERR_STALE);
    assert_eq!(
        queried(&request, &raw mut required, &raw mut state),
        KILN_ECS_ERR_STALE
    );
    assert_eq!((required, state), (0, KILN_ECS_STALE as u32));
    fake_services::set_knob(owner, Knob::Busy);
    assert_eq!(queried(&request, null, &raw mut state), KILN_OK);
    assert_eq!(state, KILN_ECS_OK as u32);
    request.flags = KILN_ECS_QUERY_FRAME_QUIET as u32;
    assert_eq!(queried(&request, null, &raw mut state), KILN_ECS_ERR_BUSY);
    assert_eq!(state, KILN_ECS_BUSY as u32);
    state = 9;
    request.size = 0;
    assert_eq!(
        queried(&request, null, &raw mut state),
        KILN_ERR_INVALID_ARGUMENT
    );
    assert_eq!(state, 9, "an invalid argument leaves *state untouched");

    // ecs_snapshot: always a stable copy (never busy); a short buffer reports *required.
    let snap = |required: *mut u32| unsafe {
        services.call(
            snapshot,
            (
                owner,
                &raw const object,
                1,
                std::ptr::null_mut(),
                0,
                required,
            ),
        )
    };
    assert_eq!(snap(&raw mut required), KILN_ERR_BUFFER_TOO_SMALL);
    assert_eq!(required, fake_services::FAKE_COMPONENT_SIZE);
    assert_eq!(snap(null), KILN_ERR_BUFFER_TOO_SMALL);

    // ecs_write_policy answers OK or UNAVAILABLE only, with out->status.
    let mut written: KilnEcsWritePolicy = unsafe { std::mem::zeroed() };
    written.size = std::mem::size_of::<KilnEcsWritePolicy>() as u32;
    assert_eq!(
        unsafe { services.call(policy, (server, 1, &raw mut written)) },
        KILN_OK
    );
    assert_eq!(written.status, KILN_ECS_OK as u32);
    assert_eq!(written.component_size, fake_services::FAKE_COMPONENT_SIZE);
    fake_services::set_knob(owner, Knob::Unavailable);
    assert_eq!(
        unsafe { services.call(policy, (server, 1, &raw mut written)) },
        KILN_ERR_UNAVAILABLE
    );
    assert_eq!(written.status, KILN_ECS_UNAVAILABLE as u32);

    // ecs_context_snapshot needs its size set and carries the status.
    let mut ctx: KilnEcsContextInfo = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe { services.call(context, (&raw mut ctx,)) },
        KILN_ERR_INVALID_ARGUMENT
    );
    ctx.size = std::mem::size_of::<KilnEcsContextInfo>() as u32;
    assert_eq!(
        unsafe { services.call(context, (&raw mut ctx,)) },
        KILN_ERR_UNAVAILABLE
    );
    assert_eq!(ctx.status, KILN_ECS_UNAVAILABLE as u32);

    // ecs_compare_exchange: a reply whose size is not set is an invalid argument and is
    // left untouched.
    let mut reply: KilnMutationReply = unsafe { std::mem::zeroed() };
    reply.status = 77;
    let bytes = [0u8; 4];
    let result = unsafe {
        services.call(
            cas,
            (
                owner,
                &raw const object,
                1,
                bytes.as_ptr().cast(),
                std::ptr::null(),
                bytes.as_ptr().cast(),
                4,
                std::ptr::null_mut(),
                &raw mut reply,
            ),
        )
    };
    assert_eq!((result, reply.status), (KILN_ERR_INVALID_ARGUMENT, 77));
}

// ---- callbacks: lent data, a producer function, publish against close ----

#[test]
fn callback_data_is_copied_out_before_the_callback_returns() {
    let (services, _api) = adapter(true);
    let owner = next_owner();
    let table = services
        .bind::<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION)
        .unwrap();
    let subscribe = service_member!(table, KilnWorldApi, ecs_write_subscribe).unwrap();
    let got: Arc<Mutex<Vec<KilnEcsWriteEvent>>> = Arc::default();
    let sink = {
        let got = got.clone();
        CallbackSink::new(move |event: KilnEcsWriteEvent| got.lock().unwrap().push(event))
    };
    let mut handle = 0;
    assert_eq!(
        unsafe {
            services.call(
                subscribe,
                (owner, Some(sink.function()), sink.user(), &raw mut handle),
            )
        },
        KILN_OK
    );
    // Kiln fires on its own thread with an event it reuses right after the callback.
    std::thread::spawn(move || {
        let mut event: KilnEcsWriteEvent = unsafe { std::mem::zeroed() };
        event.size = std::mem::size_of::<KilnEcsWriteEvent>() as u32;
        event.entity = 0x1234;
        event.component = 9;
        event.session = 5;
        assert_eq!(fake_services::fire_ecs_write(owner, &event), 1);
        // SAFETY: plain integers; the callback returned, so Kiln may reuse the event.
        unsafe { std::ptr::write_bytes(&mut event, 0xff, 1) };
        // An older core's shorter event: only its prefix is copied, the rest is zero.
        event.size = 16;
        event.entity = 0x55;
        fake_services::fire_ecs_write(owner, &event);
        unsafe { std::ptr::write_bytes(&mut event, 0xee, 1) };
        let _ = event;
    })
    .join()
    .unwrap();
    let got = got.lock().unwrap();
    assert_eq!(got.len(), 2);
    assert_eq!(
        (got[0].entity, got[0].component, got[0].session),
        (0x1234, 9, 5)
    );
    assert_eq!(
        (got[1].size, got[1].entity, got[1].component, got[1].session),
        (16, 0x55, 0, 0)
    );
    assert_eq!(sink.counts(), (2, 0));
}

#[test]
fn a_closed_sink_never_delivers_and_its_close_waits_for_deliveries_in_flight() {
    const PRODUCERS: usize = 4;
    for round in 0..40 {
        let closed = Arc::new(AtomicBool::new(false));
        let late = Arc::new(AtomicU64::new(0));
        let sink = {
            let (closed, late) = (closed.clone(), late.clone());
            CallbackSink::new(move |_: KilnEcsWriteEvent| {
                // A short delivery, long enough to be in flight at the close.
                let start = std::time::Instant::now();
                while start.elapsed() < Duration::from_micros(20) {
                    std::hint::spin_loop();
                }
                if closed.load(Ordering::SeqCst) {
                    late.fetch_add(1, Ordering::SeqCst);
                }
            })
        };
        let (function, user) = (sink.function(), sink.user() as usize);
        let start = Arc::new(Barrier::new(PRODUCERS + 1));
        let stop = Arc::new(AtomicBool::new(false));
        let attempts = Arc::new(AtomicU64::new(0));
        let producers: Vec<_> = (0..PRODUCERS)
            .map(|_| {
                let (start, stop, attempts) = (start.clone(), stop.clone(), attempts.clone());
                std::thread::spawn(move || {
                    let mut event: KilnEcsWriteEvent = unsafe { std::mem::zeroed() };
                    event.size = std::mem::size_of::<KilnEcsWriteEvent>() as u32;
                    start.wait();
                    while !stop.load(Ordering::SeqCst) {
                        unsafe { function(&event, user as *mut std::ffi::c_void) };
                        attempts.fetch_add(1, Ordering::SeqCst);
                    }
                })
            })
            .collect();
        start.wait();
        // Wait on what must happen, not on time: a delivery before the close and a
        // dropped call after it (a liveness bound only; a passing wait returns at once).
        let must = Duration::from_secs(300);
        assert!(
            eventually(must, || sink.counts().0 > 0),
            "round {round}: no delivery before the close"
        );
        sink.close();
        closed.store(true, Ordering::SeqCst);
        let at_close = sink.counts().0;
        assert!(
            eventually(must, || sink.counts().1 > 0),
            "round {round}: no call dropped after the close"
        );
        stop.store(true, Ordering::SeqCst);
        for producer in producers {
            producer.join().unwrap();
        }
        let (delivered, dropped) = sink.counts();
        assert_eq!(
            late.load(Ordering::SeqCst),
            0,
            "round {round}: a delivery ran after close"
        );
        assert_eq!(delivered, at_close, "round {round}");
        assert_eq!(
            delivered + dropped,
            attempts.load(Ordering::SeqCst),
            "round {round}"
        );
        assert!(
            dropped > 0,
            "round {round}: producers kept calling after the close"
        );
    }
}

#[test]
fn a_sink_closed_from_inside_its_own_delivery_returns_and_delivers_nothing_more() {
    let cell: Arc<std::sync::OnceLock<CallbackSink<KilnEcsWriteEvent>>> = Arc::default();
    let sink = {
        let cell = cell.clone();
        CallbackSink::new(move |_: KilnEcsWriteEvent| {
            // Kiln's own unsubscribe returns at once from inside a callback: so does this.
            cell.get().expect("set before the first event").close();
        })
    };
    let (function, user) = (sink.function(), sink.user() as usize);
    assert!(cell.set(sink).is_ok());
    let (done, finished) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let mut event: KilnEcsWriteEvent = unsafe { std::mem::zeroed() };
        event.size = std::mem::size_of::<KilnEcsWriteEvent>() as u32;
        for _ in 0..2 {
            unsafe { function(&event, user as *mut std::ffi::c_void) };
        }
        let _ = done.send(());
    });
    assert!(
        finished.recv_timeout(WAIT).is_ok(),
        "close from inside the delivery never returned"
    );
    assert_eq!(cell.get().unwrap().counts(), (1, 1));
}

// ---- the runtime installer and the timed guard, through the worker ----

static RECORDS: LazyLock<Mutex<HashMap<KilnOwner, Vec<String>>>> = LazyLock::new(Default::default);
fn note(owner: KilnOwner, text: impl Into<String>) {
    RECORDS
        .lock()
        .unwrap()
        .entry(owner)
        .or_default()
        .push(text.into());
}
fn records(owner: KilnOwner) -> Vec<String> {
    RECORDS
        .lock()
        .unwrap()
        .get(&owner)
        .cloned()
        .unwrap_or_default()
}

type FamiliesFn = fn(KilnOwner) -> Result<Families, String>;
#[derive(Clone, Copy)]
struct Scenario {
    source: &'static str,
    families: FamiliesFn,
}
static SCENARIOS: LazyLock<Mutex<HashMap<KilnOwner, Scenario>>> = LazyLock::new(Default::default);

struct TestClock(Cell<Duration>);
impl Monotonic for TestClock {
    fn now(&self) -> Duration {
        self.0.get()
    }
}
thread_local! {
    /// The worker thread's fake clock, made by the plan and advanced by the families.
    static CLOCK: RefCell<Option<Rc<TestClock>>> = const { RefCell::new(None) };
}
fn advance(ms: u64) {
    CLOCK.with_borrow(|clock| {
        let clock = clock.as_ref().expect("the plan made the clock");
        clock.0.set(clock.0.get() + Duration::from_millis(ms));
    });
}

struct Source(&'static str);
impl PackageSource for Source {
    fn read_file(&self, path: &str) -> std::io::Result<Vec<u8>> {
        if path == "src/mod.lua" {
            Ok(self.0.as_bytes().to_vec())
        } else {
            Err(std::io::ErrorKind::NotFound.into())
        }
    }
}
struct NoLocale;
impl LocaleSource for NoLocale {
    fn locale(&self) -> Option<String> {
        None
    }
}
struct NoCleanup;
impl Cleanup for NoCleanup {
    fn restore(&self, _: &str) -> Result<(), String> {
        Ok(())
    }
    fn release_input_reservations(&self, _: &str) {}
}
struct Types;
impl GameTypes for Types {
    fn types(&self, lua: &Lua, owner: &RuntimePackage) -> mlua::Result<Table> {
        let table = lua.create_table()?;
        table.raw_set("__owner", owner.id())?;
        Ok(table)
    }
}
struct Runtime(RuntimeOwnerContext);
impl RuntimeHost for Runtime {
    fn owner_context(&self) -> &RuntimeOwnerContext {
        &self.0
    }
    fn static_support(&self, _: RuntimeOperation) -> Availability {
        Availability::Available
    }
    fn provider_facts(&self) -> RuntimeProviderFacts {
        RuntimeProviderFacts::default()
    }
    fn lifecycle_active(&self) -> bool {
        true
    }
    fn write_fault(&self) -> Option<String> {
        None
    }
    fn report_effect(&self, _: EffectState, _: &str) {}
}

const IDS: [&str; 2] = ["a", "b"];
struct R00Plan;
impl PlanSource for R00Plan {
    fn plan(cx: &WorkerContext<'_>) -> Result<RuntimePlan, String> {
        let scenario = SCENARIOS.lock().unwrap()[&cx.owner()];
        let clock = Rc::new(TestClock(Cell::new(Duration::from_secs(100))));
        CLOCK.with_borrow_mut(|slot| *slot = Some(clock.clone()));
        let mut registry = Registry::default();
        for id in IDS {
            let source: Rc<dyn PackageSource> = Rc::new(Source(scenario.source));
            let mut parsed = package::Package::parse(
                json!({"id": id, "name": id, "version": "1.0.0", "capabilities": ["runtime"]})
                    .to_string()
                    .as_bytes(),
                None,
                package::ParseOptions::default(),
            )
            .unwrap();
            parsed.manifest.enabled = true;
            registry
                .insert(
                    PackageRecord::new(
                        id.into(),
                        parsed,
                        RuntimePackage::new(id.to_owned(), source.clone()),
                        source,
                    )
                    .unwrap(),
                )
                .unwrap();
        }
        let runtime: RuntimeHosts =
            Rc::new(|manifest: &package::Manifest, context: &HostContext| {
                Rc::new(Runtime(RuntimeOwnerContext::new(
                    manifest, context, false, false, "r00",
                ))) as Rc<dyn RuntimeHost>
            });
        Ok(RuntimePlan {
            side: Side::Server,
            version: SourceVersion::Sf179,
            registry,
            plan: IDS.map(String::from).to_vec(),
            admitted: IDS.map(String::from).into(),
            cleanup: Rc::new(NoCleanup),
            locale: Rc::new(NoLocale),
            clock,
            loader_patch: false,
            loader_export: false,
            attachments: None,
            register: None,
            compose: ComposePlan {
                game_types: Some(Rc::new(Types) as Rc<dyn GameTypes>),
                runtime: Some(runtime),
                withdrawn: Vec::new(),
            },
        })
    }
    fn families(cx: &WorkerContext<'_>) -> Result<Families, String> {
        let owner = cx.owner();
        (SCENARIOS.lock().unwrap()[&owner].families)(owner)
    }
}

struct Run {
    owner: KilnOwner,
    pool: Arc<FakePool>,
    shared: Arc<WorkerShared>,
}
impl Run {
    fn stop(self) {
        self.shared.request_stop();
        assert!(self.pool.task(0).wait_finished(WAIT));
    }
    fn wait_record(&self, prefix: &str) -> String {
        let mut found = None;
        assert!(
            eventually(WAIT, || {
                found = records(self.owner)
                    .into_iter()
                    .find(|r| r.starts_with(prefix));
                found.is_some()
            }),
            "never saw {prefix}: {:?} {:?}",
            records(self.owner),
            self.shared.status()
        );
        found.unwrap()
    }
}
fn start(scenario: Scenario) -> Run {
    let owner = next_owner();
    SCENARIOS.lock().unwrap().insert(owner, scenario);
    let pool = FakePool::new(owner, PoolConfig::default());
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(factory::<R00Plan>)));
    let api: &'static KilnApi = Box::leak(fake_services::api(true));
    assert!(worker.bind(unsafe { Host::read(api) }, owner));
    worker.start().expect("accepted");
    let shared = worker.shared().unwrap().clone();
    Run {
        owner,
        pool,
        shared,
    }
}

fn kind(value: &Value) -> &'static str {
    value.type_name()
}

/// Two families in order; each records what its registrar sees of the installer order.
fn ordered_families(_: KilnOwner) -> Result<Families, String> {
    let probe = |label: &'static str| {
        move |lua: &Lua,
              env: &Table,
              package: &RuntimePackage,
              composed: &Composed,
              cx: &Cx|
              -> mlua::Result<()> {
            let owner = cx.owner();
            let game: Table = env.raw_get("game")?;
            let types: Value = game.raw_get("types")?;
            let runtime = composed.runtime.clone().expect("the registrar ran");
            let runtime_types: Value = runtime.raw_get("types")?;
            let functions: Value = runtime.raw_get("functions")?;
            let earlier: Value = runtime.raw_get("first")?;
            note(
                owner,
                format!(
                    "{label}:{}:types={}:same={}:functions={}:first={}:worker={}:adapter={:p}",
                    package.id(),
                    kind(&types),
                    types == runtime_types,
                    kind(&functions),
                    kind(&earlier),
                    std::thread::current().id() == cx.worker(),
                    Rc::as_ptr(cx.services()),
                ),
            );
            runtime.raw_set(label, lua.create_table()?)?;
            Ok(())
        }
    };
    Families::new(vec![
        Family::new("first", &["runtime.first"], probe("first")),
        Family::new("second", &["runtime.second"], probe("second")),
    ])
}

#[test]
fn the_installer_runs_types_then_r_reg_then_families_then_functions_last() {
    let run = start(Scenario {
        source: "record(type(runtime.functions) .. ':' .. type(runtime.functions.get_operations) \
                 .. ':' .. type(runtime.first) .. ':' .. type(runtime.second)) return {}",
        families: |owner| {
            let families = ordered_families(owner)?;
            // The packages' own probe of the finished environment.
            let record = Family::new("record", &[], move |lua, env, _, _, cx| {
                let owner = cx.owner();
                env.raw_set(
                    "record",
                    lua.create_function(move |_, text: String| {
                        note(owner, format!("lua:{text}"));
                        Ok(())
                    })?,
                )
            });
            Families::new(families.iter().cloned().chain([record]).collect())
        },
    });
    // Both packages' top levels record, one after the other: wait for the second too.
    let lua = |owner| {
        records(owner)
            .iter()
            .filter(|r| r.starts_with("lua:"))
            .count()
    };
    assert!(
        eventually(WAIT, || lua(run.owner) == 2),
        "{:?}",
        records(run.owner)
    );
    let seen = records(run.owner);
    let adapters: Vec<&str> = seen
        .iter()
        .filter_map(|r| r.rsplit_once("adapter=").map(|(_, a)| a))
        .collect();
    assert_eq!(adapters.len(), 4, "{seen:?}");
    assert!(
        adapters.iter().all(|a| *a == adapters[0]),
        "one adapter per runtime-create: {seen:?}"
    );
    let shape: Vec<String> = seen
        .iter()
        .map(|r| r.split(":adapter=").next().unwrap().to_owned())
        .collect();
    assert_eq!(
        shape,
        [
            "first:a:types=table:same=true:functions=nil:first=nil:worker=true",
            "second:a:types=table:same=true:functions=nil:first=table:worker=true",
            "first:b:types=table:same=true:functions=nil:first=nil:worker=true",
            "second:b:types=table:same=true:functions=nil:first=table:worker=true",
            "lua:table:function:table:table",
            "lua:table:function:table:table",
        ]
    );
    run.stop();
}

#[test]
fn two_families_claiming_one_namespace_are_refused_before_any_lua() {
    let noop = |_: &Lua, _: &Table, _: &RuntimePackage, _: &Composed, _: &Cx| Ok(());
    let refused = |list: Vec<Family>| Families::new(list).err().expect("refused");
    assert!(
        refused(vec![
            Family::new("services", &["runtime.services"], noop),
            Family::new("services2", &["runtime.services"], noop),
        ])
        .contains("already owned by services")
    );
    assert!(
        refused(vec![
            Family::new("services", &["runtime.services"], noop),
            Family::new("get", &["runtime.services.get"], noop),
        ])
        .contains("already owned by services")
    );
    assert!(
        refused(vec![
            Family::new("get", &["runtime.services.get"], noop),
            Family::new("services", &["runtime.services"], noop),
        ])
        .contains("already owned by get")
    );
    assert!(
        refused(vec![
            Family::new("a", &["runtime.x"], noop),
            Family::new("a", &["runtime.y"], noop)
        ])
        .contains("registered twice")
    );
    for reserved in [
        "runtime",
        "runtime.functions",
        "runtime.functions.bind",
        "game.types",
        "runtime.types",
    ] {
        assert!(
            refused(vec![Family::new("a", &[reserved], noop)]).contains("installer owns"),
            "{reserved}"
        );
    }
    assert!(refused(vec![Family::new("a", &["runtime..x"], noop)]).contains("invalid"));
    // Disjoint neighbours are fine: `runtime.services` and `runtime.servicesx` are different paths.
    assert!(
        Families::new(vec![
            Family::new("services", &["runtime.services"], noop),
            Family::new("servicesx", &["runtime.servicesx", "game.items"], noop),
        ])
        .is_ok()
    );

    // Through the worker: runtime-create fails with the reason and no package runs.
    let run = start(Scenario {
        source: "record('ran') return {}",
        families: |_| {
            Families::new(vec![
                Family::new("services", &["runtime.services"], |_, _, _, _, _| Ok(())),
                Family::new("services2", &["runtime.services"], |_, _, _, _, _| Ok(())),
            ])
        },
    });
    assert!(run.pool.task(0).wait_finished(WAIT));
    let status = run.shared.status();
    assert_eq!(status.reason, StopReason::CreateFailed, "{status:?}");
    assert!(
        status
            .detail
            .unwrap_or_default()
            .contains("already owned by services")
    );
    assert!(records(run.owner).is_empty());
}

#[test]
fn a_family_that_touches_a_member_it_does_not_own_is_refused_before_any_lua() {
    for (families, reason) in [
        (
            (|_| {
                Families::new(vec![Family::new(
                    "status",
                    &["runtime.mine"],
                    |lua, _, _, composed, _| {
                        let runtime = composed.runtime.clone().unwrap();
                        runtime.raw_set("status", lua.create_function(|_, ()| Ok(()))?)
                    },
                )])
            }) as FamiliesFn,
            "runtime family status: replaced runtime.status, which it does not own",
        ),
        (
            (|_| {
                Families::new(vec![Family::new(
                    "extra",
                    &["runtime.mine"],
                    |lua, _, _, composed, _| {
                        composed
                            .runtime
                            .clone()
                            .unwrap()
                            .raw_set("extra", lua.create_table()?)
                    },
                )])
            }) as FamiliesFn,
            "runtime family extra: added runtime.extra without claiming it",
        ),
        (
            (|_| {
                Families::new(vec![Family::new(
                    "types",
                    &["game.items"],
                    |_, _, _, composed, _| composed.game.raw_set("types", mlua::Value::Nil),
                )])
            }) as FamiliesFn,
            "runtime family types: removed game.types, which it does not own",
        ),
        (
            (|_| {
                Families::new(vec![
                    Family::new("first", &["runtime.first"], |lua, _, _, composed, _| {
                        composed
                            .runtime
                            .clone()
                            .unwrap()
                            .raw_set("first", lua.create_table()?)
                    }),
                    Family::new("second", &["runtime.second"], |lua, _, _, composed, _| {
                        composed
                            .runtime
                            .clone()
                            .unwrap()
                            .raw_set("first", lua.create_table()?)
                    }),
                ])
            }) as FamiliesFn,
            "runtime family second: replaced runtime.first, which it does not own",
        ),
    ] {
        let run = start(Scenario {
            source: "record('ran') return {}",
            families,
        });
        assert!(run.pool.task(0).wait_finished(WAIT));
        let status = run.shared.status();
        assert_eq!(status.reason, StopReason::CreateFailed, "{status:?}");
        let detail = status.detail.unwrap_or_default();
        assert!(detail.contains(reason), "{detail}");
        assert!(records(run.owner).is_empty());
    }
    // A claim that nests a member lets its family add the member that leads to it.
    let run = start(Scenario {
        source: "record(type(runtime.deep.query)) return {}",
        families: |_| {
            Families::new(vec![
                Family::new("deep", &["runtime.deep.query"], |lua, _, _, composed, _| {
                    let deep = lua.create_table()?;
                    deep.raw_set("query", lua.create_table()?)?;
                    composed.runtime.clone().unwrap().raw_set("deep", deep)
                }),
                Family::new("record", &[], |lua, env, _, _, cx| {
                    let owner = cx.owner();
                    env.raw_set(
                        "record",
                        lua.create_function(move |_, text: String| {
                            note(owner, format!("lua:{text}"));
                            Ok(())
                        })?,
                    )
                }),
            ])
        },
    });
    assert_eq!(run.wait_record("lua:"), "lua:table");
    run.stop();
}

/// The table at a dotted path below the registrar's runtime table.
fn below(runtime: &Table, dotted: &str) -> mlua::Result<Table> {
    let mut table = runtime.clone();
    for name in dotted.split('.') {
        table = table.raw_get(name)?;
    }
    Ok(table)
}
/// A family that records each package's top level (both packages run the same source).
fn record_family(owner: KilnOwner) -> Family {
    Family::new("record", &[], move |lua, env, _, _, _| {
        env.raw_set(
            "record",
            lua.create_function(move |_, text: String| {
                note(owner, format!("lua:{text}"));
                Ok(())
            })?,
        )
    })
}
/// Each container's type and member count, as a package sees it.
const CONTAINER_PROBE: &str = r#"
local out = {}
for _, path in ipairs({'ecs', 'network', 'world', 'world.building', 'world.cursor', 'world.entity', 'world.voxel'}) do
  local t = runtime
  for name in path:gmatch('[^.]+') do t = t[name] end
  local n = 0
  for _ in pairs(t) do n = n + 1 end
  out[#out + 1] = path .. '=' .. type(t) .. '/' .. n
end
record(table.concat(out, ','))
return {}
"#;

#[test]
fn the_installer_owns_the_container_namespaces_and_families_claim_inside_them() {
    use kiln_sfi::runtime::install::CONTAINERS;
    // ShroudForge's runtime tables the 31 operations pass through (1.7.9: lua/ingame/mod.rs:63-77,
    // world/mod.rs:14-87, player_input/submit_building_input.rs:171); runtime.player is the registrar's.
    assert_eq!(
        CONTAINERS,
        [
            "runtime.ecs",
            "runtime.network",
            "runtime.overrides",
            "runtime.world",
            "runtime.world.building",
            "runtime.world.cursor",
            "runtime.world.entity",
            "runtime.world.voxel",
        ]
    );
    let noop = |_: &Lua, _: &Table, _: &RuntimePackage, _: &Composed, _: &Cx| Ok(());
    let refused = |list: Vec<Family>| Families::new(list).err().expect("refused");
    for container in CONTAINERS {
        let reason = refused(vec![Family::new("a", &[container], noop)]);
        assert!(reason.contains("installer owns"), "{container}: {reason}");
    }
    // Names inside a container: one owner each, neighbours may belong to other families.
    assert!(
        Families::new(vec![
            Family::new(
                "context",
                &["runtime.world.context_active", "runtime.world.session_id"],
                noop
            ),
            Family::new("props", &["runtime.world.entity.query_props"], noop),
            Family::new("spawn", &["runtime.world.entity.spawn"], noop),
            Family::new("network", &["runtime.network.status"], noop),
            Family::new("query", &["runtime.ecs.query"], noop),
        ])
        .is_ok()
    );
    assert!(
        refused(vec![
            Family::new("spawn", &["runtime.world.entity.spawn"], noop),
            Family::new("spawn2", &["runtime.world.entity.spawn"], noop),
        ])
        .contains("already owned by spawn")
    );

    // Through the worker: the containers are there, empty, before the first family runs,
    // and each family adds only the names it claims.
    let run = start(Scenario {
        source: CONTAINER_PROBE,
        families: |owner| {
            Families::new(vec![
                Family::new(
                    "context",
                    &["runtime.world.context_active"],
                    move |lua, _, _, composed, _| {
                        let runtime = composed.runtime.clone().unwrap();
                        let mut seen = Vec::new();
                        for path in ["ecs", "network", "world", "world.entity"] {
                            let table = below(&runtime, path)?;
                            seen.push(format!("{path}/{}", table.pairs::<Value, Value>().count()));
                        }
                        note(owner, format!("context:{}", seen.join(",")));
                        below(&runtime, "world")?
                            .raw_set("context_active", lua.create_function(|_, ()| Ok(true))?)
                    },
                ),
                Family::new(
                    "spawn",
                    &["runtime.world.entity.spawn"],
                    |lua, _, _, composed, _| {
                        below(&composed.runtime.clone().unwrap(), "world.entity")?
                            .raw_set("spawn", lua.create_function(|_, ()| Ok(()))?)
                    },
                ),
                record_family(owner),
            ])
        },
    });
    let lua = |owner| {
        records(owner)
            .iter()
            .filter(|r| r.starts_with("lua:"))
            .count()
    };
    assert!(
        eventually(WAIT, || lua(run.owner) == 2),
        "{:?} / {:?}",
        records(run.owner),
        run.shared.status()
    );
    let seen = records(run.owner);
    let probe = "lua:ecs=table/0,network=table/0,world=table/5,world.building=table/0,\
                 world.cursor=table/0,world.entity=table/1,world.voxel=table/0";
    assert_eq!(seen.iter().filter(|r| *r == probe).count(), 2, "{seen:?}");
    assert_eq!(
        seen.iter()
            .filter(|r| *r == "context:ecs/0,network/0,world/4,world.entity/0")
            .count(),
        2,
        "{seen:?}"
    );
    run.stop();

    // Adding an unclaimed child, or replacing or removing a container or another family's
    // child, refuses runtime-create before any Lua.
    for (families, reason) in [
        (
            (|_| {
                Families::new(vec![Family::new(
                    "context",
                    &["runtime.world.context_active"],
                    |lua, _, _, composed, _| {
                        below(&composed.runtime.clone().unwrap(), "world")?
                            .raw_set("extra", lua.create_table()?)
                    },
                )])
            }) as FamiliesFn,
            "runtime family context: added runtime.world.extra without claiming it",
        ),
        (
            (|_| {
                Families::new(vec![Family::new(
                    "spawn",
                    &["runtime.world.entity.spawn"],
                    |lua, _, _, composed, _| {
                        below(&composed.runtime.clone().unwrap(), "world.entity")?
                            .raw_set("place", lua.create_table()?)
                    },
                )])
            }) as FamiliesFn,
            "runtime family spawn: added runtime.world.entity.place without claiming it",
        ),
        (
            (|_| {
                Families::new(vec![Family::new(
                    "context",
                    &["runtime.world.context_active"],
                    |lua, _, _, composed, _| {
                        let world = lua.create_table()?;
                        world.raw_set("context_active", lua.create_table()?)?;
                        composed.runtime.clone().unwrap().raw_set("world", world)
                    },
                )])
            }) as FamiliesFn,
            "runtime family context: replaced runtime.world, which it does not own",
        ),
        (
            (|_| {
                Families::new(vec![Family::new(
                    "network",
                    &["runtime.network.status"],
                    |_, _, _, composed, _| {
                        composed
                            .runtime
                            .clone()
                            .unwrap()
                            .raw_set("network", Value::Nil)
                    },
                )])
            }) as FamiliesFn,
            "runtime family network: removed runtime.network, which it does not own",
        ),
        (
            (|_| {
                Families::new(vec![Family::new(
                    "spawn",
                    &["runtime.world.entity.spawn"],
                    |lua, _, _, composed, _| {
                        let entity = lua.create_table()?;
                        entity.raw_set("spawn", lua.create_table()?)?;
                        below(&composed.runtime.clone().unwrap(), "world")?
                            .raw_set("entity", entity)
                    },
                )])
            }) as FamiliesFn,
            "runtime family spawn: replaced runtime.world.entity, which it does not own",
        ),
        (
            (|_| {
                Families::new(vec![
                    Family::new(
                        "first",
                        &["runtime.world.entity.spawn"],
                        |lua, _, _, composed, _| {
                            below(&composed.runtime.clone().unwrap(), "world.entity")?
                                .raw_set("spawn", lua.create_table()?)
                        },
                    ),
                    Family::new(
                        "second",
                        &["runtime.world.entity.place"],
                        |lua, _, _, composed, _| {
                            below(&composed.runtime.clone().unwrap(), "world.entity")?
                                .raw_set("spawn", lua.create_table()?)
                        },
                    ),
                ])
            }) as FamiliesFn,
            "runtime family second: replaced runtime.world.entity.spawn, which it does not own",
        ),
    ] {
        let run = start(Scenario {
            source: "record('ran') return {}",
            families,
        });
        assert!(run.pool.task(0).wait_finished(WAIT));
        let status = run.shared.status();
        assert_eq!(status.reason, StopReason::CreateFailed, "{status:?}");
        let detail = status.detail.unwrap_or_default();
        assert!(detail.contains(reason), "{reason}: {detail}");
        assert!(records(run.owner).is_empty());
    }
}

/// `timed()`: one call of `kiln.fake.timed` through the adapter's timed path. The call
/// itself takes 1 ms of the fake clock and reports 10 s of source wait; it answers the
/// open call's Lua time afterwards, in ms.
fn timed_family(owner: KilnOwner) -> Result<Families, String> {
    let _ = owner;
    Families::new(vec![Family::new(
        "timed",
        &["runtime.r00"],
        |lua, env, _, _, cx| {
            let owner = cx.owner();
            let services = cx.services().clone();
            let table = services
                .bind::<fake_services::FakeTimedApi>(fake_services::FAKE_TIMED_SERVICE, 1)
                .map_err(mlua::Error::external)?;
            let call = service_member!(table, fake_services::FakeTimedApi, call, waits)
                .map_err(mlua::Error::external)?;
            fake_services::set_reported_wait(owner, 10_000_000_000);
            fake_services::on_timed_call(|| advance(1));
            env.raw_set(
                "advance",
                lua.create_function(|_, ms: u64| {
                    advance(ms);
                    Ok(())
                })?,
            )?;
            env.raw_set(
                "record",
                lua.create_function(move |_, text: String| {
                    note(owner, text);
                    Ok(())
                })?,
            )?;
            let services_for_lua = services.clone();
            env.raw_set(
                "timed",
                lua.create_function(move |_, ()| {
                    let mut reply = fake_services::FakeTimedReply::default();
                    let timed = unsafe {
                        services_for_lua.call_reporting_wait(call, (owner, &raw mut reply))
                    };
                    assert_eq!(timed.measured(), Duration::from_millis(1));
                    let result = timed.finish(reply.source_wait_ns);
                    assert_eq!((result, reply.source_wait_ns), (KILN_OK, 10_000_000_000));
                    let excluded = services_for_lua.counters().excluded_ns / 1_000_000;
                    Ok(format!(
                        "{}:{excluded}",
                        services_for_lua.watchdog().execution().as_millis()
                    ))
                })?,
            )?;
            Ok(())
        },
    )])
}

#[test]
fn the_timed_guard_never_excludes_more_than_the_measured_call() {
    let run = start(Scenario {
        source: "local done = false return { update_interval_ms = 8, on_update = function() \
                 if not done then done = true advance(9900) record('lua:' .. timed()) end end }",
        families: timed_family,
    });
    // Both packages call once: 9.9 s of Lua, then a 1 ms call reporting 10 s. Only the
    // 1 ms is forgiven, so the account still shows the 9.9 s from before the call.
    let first = run.wait_record("lua:");
    assert_eq!(first, "lua:9900:1");
    assert!(eventually(WAIT, || records(run.owner).len() == 2));
    assert_eq!(records(run.owner)[1], "lua:9900:2");
    let counters = fake_services::counters(run.owner);
    assert_eq!(counters.calls("timed.call"), 2);
    assert_eq!((counters.waits, counters.retries, counters.hops), (2, 0, 0));
    run.stop();
}

// ---- structure: the adapter is the only path ----

#[test]
fn no_runtime_module_reaches_kiln_but_through_the_adapter() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("src/runtime");
    let mut files = vec![root.clone()];
    let mut checked = 0;
    while let Some(path) = files.pop() {
        if path.is_dir() {
            files.extend(std::fs::read_dir(&path).unwrap().map(|e| e.unwrap().path()));
            continue;
        }
        if path.extension().is_none_or(|e| e != "rs") || path == root.join("services.rs") {
            continue;
        }
        let text = std::fs::read_to_string(&path).unwrap();
        for forbidden in [
            ".service(",
            "Host::read",
            "KilnApi",
            ".host()",
            "member_at",
            "post_world",
            // The timed path is the adapter's; a family never times or pauses the watchdog
            // itself (it could forgive its own Lua time).
            "native_call",
            "WatchdogPause",
            ".pause()",
        ] {
            assert!(
                !text.contains(forbidden),
                "{} uses {forbidden}: Kiln's services go through runtime::services only",
                path.display()
            );
        }
        checked += 1;
    }
    assert!(checked >= 3, "checked {checked} files");
}
