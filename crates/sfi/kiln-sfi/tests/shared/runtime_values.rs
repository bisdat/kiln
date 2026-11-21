// SPDX-License-Identifier: MIT
// the interpreter's typed-value frontend, shared test bodies.
// Run by kiln-sfi's own tests (tests/values_frontend.rs) and by any other test package that
// includes them.
//! The typed-value frontend's own contract, over a scripted kiln.eml.values provider built from kiln-abi
//! types (tests/support/fake_values): the reader's protected requests and tokens, proxy interning and lease ownership,
//! finalizers inside reader callbacks, wake and collect, the drain before the VM and the worker go, packets and error
//! packets, and ShroudForge's userdata shapes. The provider's answers here are scripts; ShroudForge's behaviour through
//! the real kiln-eml provider is the consumer library's (examples/values_consumer.rs), run inside kiln-eml's own
//! C-consumer test.
#[allow(clippy::duplicate_mod)]
#[path = "../support/fake_values/mod.rs"]
mod fake;

use fake::*;
use kiln_abi::services::kiln_eml_values::*;
use kiln_abi::*;
use kiln_sfi::runtime::values::proxies::{
    ArrayValue, MappedArrayValue, MappedStructValue, MappedVariantValue, StructValue, VariantValue,
};
use kiln_sfi::runtime::values::{ArenaOptions, KnownUserdata, ValuesError, packets};
use mlua::{AnyUserData, UserData, Value};
use serde_json::json;
use std::any::TypeId;
use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

struct Foreign;
impl UserData for Foreign {}

const PERR: &str = r#"
function perr(f, ...)
  local ok, e = pcall(f, ...)
  assert(not ok, "expected an error")
  return (tostring(e):match("^[^\n]*"))
end
"#;

fn view(value: serde_json::Value) -> u64 {
    json(json!({"schema_version": 1, "packet": "view", "value": value}))
}
fn view_nil() -> u64 {
    view(json!({"tag": "nil"}))
}
fn i64_text(v: i64) -> String {
    format!("i64:{:016x}", v as u64)
}
fn length_packet(v: i64) -> u64 {
    json(json!({"schema_version": 1, "packet": "length", "value": i64_text(v)}))
}
fn error_packet(level: serde_json::Value) -> u64 {
    json(json!({"schema_version": 1, "packet": "error", "error": level}))
}
fn bytes_text(text: &[u8]) -> String {
    let mut out = String::from("bytes:");
    for b in text {
        out.push_str(&format!("{b:02x}"));
    }
    out
}

/// A VM with an open arena and `perr` defined.
fn setup(options: ArenaOptions) -> (TestVm, kiln_sfi::runtime::values::ValuesArena) {
    let t = vm();
    let arena = t.arena(options).unwrap();
    t.run::<()>(PERR).unwrap();
    (t, arena)
}

/// A proxy of `kind` on `node` made by the provider (MAKE_TYPED_PROXY inside a value_open), as a Lua value.
fn proxy(
    t: &TestVm,
    arena: &kiln_sfi::runtime::values::ValuesArena,
    kind: i32,
    node: u64,
) -> (Value, u64) {
    let provisional = Rc::new(RefCell::new(0));
    let p = provisional.clone();
    on("value_open", move |_| {
        let (lease, code, reply) = make_proxy(kind, node);
        assert_eq!(code, KILN_OK);
        assert_eq!(reply.lease_disposition, KILN_LUA_LEASE_ADOPTED as u32);
        *p.borrow_mut() = lease;
        // the root: another lease of the same node (the caller's item)
        handled(1, fake::lease(kind, node), 0)
    });
    let value = arena.open_default(t.lua(), "t.Any").unwrap();
    let lease = *provisional.borrow();
    (value, lease)
}

// ---- arena creation ----

// Red-first: the arena binds kiln.eml.values through the adapter and hands the provider a complete v1 reader table
// (56 bytes, version 1, every member, a user context) with kiln-sfi's owner, the view and the phase.
pub fn the_arena_hands_the_provider_a_complete_v1_reader() {
    let (_t, arena) = setup(ArenaOptions::default());
    assert_eq!(
        events()[0],
        Event::Create {
            owner: OWNER,
            view: VIEW,
            phase: KILN_VALUE_RUNTIME as u32
        }
    );
    let reader = reader();
    assert_eq!((reader.size as usize, reader.version), (56, 1));
    assert!(!reader.user.is_null());
    assert!(reader.access.is_some() && reader.copy.is_some() && reader.retain.is_some());
    assert!(reader.release.is_some() && reader.wake.is_some());
    assert_eq!(arena.control(), CONTROL);
}

pub fn a_refused_arena_or_a_short_table_is_kilns_error() {
    let t = vm();
    with(|f| f.create_answer = KILN_ERR_UNAVAILABLE);
    match t.arena(ArenaOptions::default()) {
        Err(ValuesError::Refused { member, code }) => {
            assert_eq!((member, code), ("arena_create", KILN_ERR_UNAVAILABLE))
        }
        _ => panic!("arena_create refused"),
    }
    with(|f| f.create_answer = KILN_OK);
    serve_short(true);
    let short = t.arena(ArenaOptions::default());
    serve_short(false);
    match short {
        Err(ValuesError::Unavailable(error)) => {
            assert!(error.to_string().contains("value_array_clear"), "{error}")
        }
        _ => panic!("a table without value_array_clear is unavailable"),
    }
}

// ---- the reader ----

#[derive(Default, Debug)]
struct Seen {
    codes: Vec<KilnResult>,
    kinds: Vec<u32>,
    texts: Vec<Result<Vec<u8>, KilnResult>>,
    bits: Vec<u64>,
    errors: Vec<bool>,
    typed: Vec<u64>,
}

// Red-first: each request runs once, in a protected call, with Source's own reads: GET_FIELD_VALUE runs __index once,
// GET_FIELD_STRING is mlua's string conversion, LENGTH runs __len every time, RAW_SEQUENCE_INDEX reads raw,
// SOURCE_TO_STRING is mlua's Value::to_string (Rust number Display; non-UTF-8 is an error object), and a raising
// metamethod is an error object whose copy is the error's Display. DESCRIBE names every Lua kind, a proxy with its
// lease, a foreign userdata as OTHER_USERDATA.
pub fn reader_requests_run_once_with_sources_reads_in_a_protected_call() {
    let (t, arena) = setup(ArenaOptions::default());
    let (proxy, lease) = proxy(&t, &arena, KILN_VALUE_STRUCT_OWNED, 5);
    t.set("P", proxy);
    t.set("F", t.lua().create_userdata(Foreign).unwrap());
    let input: Value = t
        .run(
            r#"
        gets, lens = {}, 0
        local input = setmetatable({}, {
          __index = function(_, k)
            gets[#gets + 1] = k
            if k == "boom" then error("boom") end
            if k == "num" then return 1.0 end
            return k .. "!"
          end,
          __len = function() lens = lens + 1 return lens * 10 end,
        })
        rawset(input, 1, "raw1")
        rawset(input, "flag", false)
        rawset(input, "fn", function() end)
        rawset(input, "co", coroutine.create(function() end))
        rawset(input, "p", P)
        rawset(input, "f", F)
        rawset(input, "sum", 0.1 + 0.2)
        rawset(input, "int", 3)
        rawset(input, "one", 1.0)
        rawset(input, "tbl", setmetatable({}, {__tostring = function() return "custom" end}))
        rawset(input, "bad", setmetatable({}, {__tostring = function() return "\xff" end}))
        rawset(input, "nonutf8", "\xff")
        return input"#,
        )
        .unwrap();
    let seen = Rc::new(RefCell::new(Seen::default()));
    let s = seen.clone();
    on("value_open", move |call| {
        let mut seen = s.borrow_mut();
        let seen = &mut *seen;
        let object = call.object;
        let get = |seen: &mut Seen, op: i32, key: &[u8], index: i64| {
            let (code, reply) = access(op, object, key, index);
            seen.codes.push(code);
            seen.kinds.push(reply.kind);
            seen.bits.push(reply.bits);
            seen.typed.push(reply.typed_value);
            seen.errors.push(reply.error_object != 0);
            let text = if reply.error_object != 0 {
                copy(reply.error_object)
            } else {
                copy(reply.object)
            };
            seen.texts.push(text);
            reply
        };
        get(seen, KILN_LUA_DESCRIBE, b"", 0); // 0: the table
        get(seen, KILN_LUA_GET_FIELD_VALUE, b"a", 0); // 1: "a!"
        get(seen, KILN_LUA_GET_FIELD_VALUE, b"boom", 0); // 2: error object
        get(seen, KILN_LUA_GET_FIELD_STRING, b"num", 0); // 3: "1.0" (mlua's conversion of a float)
        get(seen, KILN_LUA_GET_FIELD_STRING, b"flag", 0); // 4: error object (a boolean is no string)
        for _ in 0..3 {
            get(seen, KILN_LUA_LENGTH, b"", 0); // 5, 6, 7: 10, 20, 30
        }
        get(seen, KILN_LUA_RAW_SEQUENCE_INDEX, b"", 1); // 8: "raw1"
        get(seen, KILN_LUA_RAW_SEQUENCE_INDEX, b"", 2); // 9: nil (raw: __index not consulted)
        for key in [
            &b"fn"[..],
            b"co",
            b"p",
            b"f",
            b"sum",
            b"int",
            b"one",
            b"tbl",
            b"bad",
            b"nonutf8",
        ] {
            let reply = get(seen, KILN_LUA_GET_FIELD_VALUE, key, 0); // 10..=19
            let (code, text) = access(KILN_LUA_SOURCE_TO_STRING, reply.object, b"", 0);
            seen.codes.push(code);
            seen.errors.push(text.error_object != 0);
            seen.texts.push(if text.error_object != 0 {
                copy(text.error_object)
            } else {
                copy(text.object)
            });
            seen.kinds.push(text.kind);
            seen.bits.push(0);
            seen.typed.push(0);
        }
        handled(1, 0, view_nil())
    });
    let result = arena.open_lua(t.lua(), "t.Pair", input).unwrap();
    assert!(result.is_nil());
    let seen = seen.borrow();
    assert!(seen.codes.iter().all(|c| *c == KILN_OK), "{:?}", seen.codes);
    let kind = |i: usize| seen.kinds[i] as i32;
    let text = |i: usize| String::from_utf8_lossy(seen.texts[i].as_ref().unwrap()).into_owned();
    assert_eq!(kind(0), KILN_LUA_TABLE);
    assert_eq!((kind(1), text(1)), (KILN_LUA_STRING, "a!".into()));
    assert!(seen.errors[2] && text(2).contains("boom"), "{}", text(2));
    assert_eq!(text(3), "1.0");
    assert!(seen.errors[4] && text(4).contains("boolean"), "{}", text(4));
    assert_eq!(
        [seen.bits[5], seen.bits[6], seen.bits[7]],
        [10, 20, 30],
        "__len runs every time"
    );
    assert_eq!((kind(8), text(8)), (KILN_LUA_STRING, "raw1".into()));
    assert_eq!(kind(9), KILN_LUA_NIL);
    // fields at 10, 12, ..., their SOURCE_TO_STRING right after
    let field = |n: usize| 10 + 2 * n;
    assert_eq!(kind(field(0)), KILN_LUA_OTHER, "a function");
    assert_eq!(kind(field(1)), KILN_LUA_OTHER, "a thread");
    assert_eq!(
        (kind(field(2)), seen.typed[field(2)]),
        (KILN_LUA_TYPED_VALUE, lease)
    );
    assert_eq!(kind(field(3)), KILN_LUA_OTHER_USERDATA);
    assert_eq!(text(field(4) + 1), "0.30000000000000004");
    assert_eq!(text(field(5) + 1), "3");
    assert_eq!(text(field(6) + 1), "1", "Rust's Display, not %.14g");
    assert_eq!(text(field(7) + 1), "custom");
    assert!(
        seen.errors[field(8) + 1],
        "a non-UTF-8 __tostring is an error"
    );
    assert!(seen.errors[field(9) + 1], "a non-UTF-8 string is an error");
    assert!(
        text(field(9) + 1).contains("error converting Lua string"),
        "{}",
        text(field(9) + 1)
    );
    // __index ran exactly once per GET_FIELD_VALUE/STRING it served (raw fields never reach it)
    let gets: Vec<String> = t.run("return gets").unwrap();
    assert_eq!(gets, ["a", "boom", "num"]);
    let counters = arena.reader_counters();
    assert_eq!(counters.by_request[KILN_LUA_LENGTH as usize], 3);
    assert!(counters.errors >= 4);
    assert_eq!(counters.panics, 0);
}

// Red-first: temporaries die with the frontend's outermost call, nested calls included; a retained token lives until
// its release; a release with no retain left is counted, never a double free.
pub fn temporaries_die_with_the_outermost_call_and_retained_tokens_with_their_release() {
    let (t, arena) = setup(ArenaOptions::default());
    let (proxy, _) = proxy(&t, &arena, KILN_VALUE_ARRAY_OWNED, 6);
    t.set("A", proxy);
    let tokens = Rc::new(RefCell::new(Vec::new()));
    let tk = tokens.clone();
    // a nested frontend call (#A inside __index) does not end the outer call's temporaries
    on("value_read", |_| handled(1, 0, length_packet(2)));
    on("value_open", move |call| {
        let (_, described) = access(KILN_LUA_DESCRIBE, call.object, b"", 0);
        let (_, kept) = access(KILN_LUA_GET_FIELD_VALUE, call.object, b"kept", 0);
        let (_, nested) = access(KILN_LUA_GET_FIELD_VALUE, call.object, b"nested", 0);
        let (after_nested, _) = access(KILN_LUA_DESCRIBE, described.object, b"", 0);
        assert_eq!(retain(kept.object), KILN_OK);
        tk.borrow_mut().extend([
            described.object,
            kept.object,
            nested.bits,
            after_nested as u64,
        ]);
        handled(1, 0, view_nil())
    });
    let input: Value = t
        .run(r#"return setmetatable({}, {__index = function(_, k) if k == "nested" then return #A end return {} end})"#)
        .unwrap();
    arena.open_lua(t.lua(), "t.Pair", input).unwrap();
    let [described, kept, nested_len, after_nested] = tokens.borrow()[..] else {
        panic!()
    };
    assert_eq!(nested_len, 2, "the nested call ran");
    assert_eq!(
        after_nested as KilnResult, KILN_OK,
        "a nested call keeps the outer temporaries"
    );
    let probes = Rc::new(RefCell::new(Vec::new()));
    let pr = probes.clone();
    on("value_read", move |_| {
        let mut probes = pr.borrow_mut();
        probes.push(access(KILN_LUA_DESCRIBE, described, b"", 0).0);
        probes.push(access(KILN_LUA_DESCRIBE, kept, b"", 0).0);
        release(kept);
        probes.push(access(KILN_LUA_DESCRIBE, kept, b"", 0).0);
        release(kept);
        handled(1, 0, length_packet(0))
    });
    t.run::<i64>("return #A").unwrap();
    assert_eq!(
        *probes.borrow(),
        [KILN_ERR_NOT_FOUND, KILN_OK, KILN_ERR_NOT_FOUND],
        "the temporary died, the retained token lived until its release"
    );
    let counters = arena.reader_counters();
    assert_eq!(
        (counters.retains, counters.releases, counters.stray_releases),
        (1, 2, 1)
    );
    assert_eq!(arena.live_tokens().1, 0);
}

// Red-first: MAKE_TYPED_PROXY interns weakly by node: a new node's proxy adopts the provisional lease (closed once,
// when Lua collects it), a live one is REUSED, the frontend's userdata has ShroudForge's class names, and an item
// lease for an interned node is closed (the proxy keeps its own).
pub fn proxies_are_interned_weakly_and_close_their_lease_once() {
    let (t, arena) = setup(ArenaOptions::default());
    let (root, adopted) = proxy(&t, &arena, KILN_VALUE_STRUCT_OWNED, 7);
    t.set("V", root);
    let items: Vec<u64> = events()
        .iter()
        .filter_map(|e| match e {
            Event::Close { lease, .. } => Some(*lease),
            _ => None,
        })
        .collect();
    assert_eq!(
        items.len(),
        1,
        "the root's item lease closed: the proxy keeps the adopted one"
    );
    assert_ne!(items[0], adopted);
    let reused = Rc::new(RefCell::new(None));
    let r = reused.clone();
    on("value_read", move |_| {
        let (provisional, code, reply) = make_proxy(KILN_VALUE_STRUCT_OWNED, 7);
        let (_, described) = access(KILN_LUA_DESCRIBE, reply.object, b"", 0);
        *r.borrow_mut() = Some((
            provisional,
            code,
            reply.lease_disposition,
            described.typed_value,
        ));
        handled(1, 0, length_packet(1))
    });
    t.run::<i64>("return #V").unwrap();
    let (provisional, code, disposition, typed) = reused.borrow().unwrap();
    assert_eq!((code, disposition), (KILN_OK, KILN_LUA_LEASE_REUSED as u32));
    assert_eq!(typed, adopted, "the live proxy keeps its adopted lease");
    assert_eq!(
        closes_of(provisional),
        0,
        "a REUSED provisional is the provider's to close"
    );
    assert_eq!(closes_of(adopted), 0);
    t.run::<()>("V = nil").unwrap();
    t.collect();
    assert_eq!(
        closes_of(adopted),
        1,
        "the finalizer closed the adopted lease once"
    );
    t.collect();
    assert_eq!(closes_of(adopted), 1);
    // a collected node gets a fresh proxy
    let (_, again) = proxy(&t, &arena, KILN_VALUE_STRUCT_OWNED, 7);
    assert_ne!(again, adopted);
    // ShroudForge's userdata names, one Rust type per class
    for (kind, name, node) in [
        (KILN_VALUE_STRUCT_OWNED, "StructValue", 70),
        (KILN_VALUE_STRUCT_MAPPED, "MappedStructValue", 71),
        (KILN_VALUE_ARRAY_OWNED, "ArrayValue", 72),
        (KILN_VALUE_ARRAY_MAPPED, "MappedArrayValue", 73),
        (KILN_VALUE_VARIANT_OWNED, "VariantValue", 74),
        (KILN_VALUE_VARIANT_MAPPED, "MappedVariantValue", 75),
    ] {
        let (value, _) = proxy(&t, &arena, kind, node);
        let Value::UserData(userdata) = &value else {
            panic!()
        };
        assert_eq!(userdata.type_name().unwrap().as_deref(), Some(name));
    }
}

// Red-first: a finalizer that runs inside a reader callback (a GC during the provider's request) closes its lease
// through value_close right there, and nothing else: the provider queues it (its duty); the request still answers.
pub fn a_finalizer_inside_a_reader_callback_only_closes_its_lease() {
    let (t, arena) = setup(ArenaOptions::default());
    let (dead, lease) = proxy(&t, &arena, KILN_VALUE_STRUCT_OWNED, 9);
    drop(dead);
    let input: Value = t
        .run(r#"return setmetatable({}, {__index = function() collectgarbage("collect") collectgarbage("collect") return 1 end})"#)
        .unwrap();
    let code = Rc::new(RefCell::new(None));
    let c = code.clone();
    on("value_open", move |call| {
        let (code, reply) = access(KILN_LUA_GET_FIELD_VALUE, call.object, b"x", 0);
        *c.borrow_mut() = Some((code, reply.kind, reply.bits));
        handled(1, 0, view_nil())
    });
    arena.open_lua(t.lua(), "t.Pair", input).unwrap();
    assert_eq!(*code.borrow(), Some((KILN_OK, KILN_LUA_INTEGER as u32, 1)));
    assert!(
        events().contains(&Event::Close {
            lease,
            in_access: true
        }),
        "{:?}",
        events()
    );
}

struct SendReader(KilnEmlLuaReaderV1);
unsafe impl Send for SendReader {}

// Red-first: wake runs on the closing thread, carries the control token and only signals (the host's notify, no Lua,
// no callback); the worker collects with that control after its next outermost call, or from its loop.
pub fn wake_signals_and_the_worker_collects_with_the_control() {
    let notified = Arc::new(AtomicUsize::new(0));
    let n = notified.clone();
    let worker = std::thread::current().id();
    let notify_thread = Arc::new(std::sync::Mutex::new(None));
    let nt = notify_thread.clone();
    let options = ArenaOptions {
        notify: Some(Arc::new(move || {
            n.fetch_add(1, Ordering::SeqCst);
            *nt.lock().unwrap() = Some(std::thread::current().id());
        })),
        ..ArenaOptions::default()
    };
    let (t, arena) = setup(options);
    let (array, _) = proxy(&t, &arena, KILN_VALUE_ARRAY_OWNED, 11);
    t.set("A", array);
    let wake_from_elsewhere = || {
        let reader = SendReader(reader());
        std::thread::spawn(move || {
            let reader = reader;
            unsafe { reader.0.wake.unwrap()(reader.0.user, CONTROL) };
        })
        .join()
        .unwrap();
    };
    let before = arena.reader_counters();
    wake_from_elsewhere();
    assert_eq!(notified.load(Ordering::SeqCst), 1);
    assert_ne!(
        notify_thread.lock().unwrap().unwrap(),
        worker,
        "notify runs on the closing thread"
    );
    assert_eq!(
        (arena.signal().wakes(), arena.signal().last_control()),
        (1, CONTROL)
    );
    assert_eq!(arena.reader_counters(), before, "wake touched nothing else");
    clear_events();
    on("value_read", |_| handled(1, 0, length_packet(3)));
    assert_eq!(t.run::<i64>("return #A").unwrap(), 3);
    let order: Vec<Event> = events()
        .into_iter()
        .filter(|e| !matches!(e, Event::Copy { .. } | Event::Close { .. }))
        .collect();
    assert_eq!(
        order,
        [
            Event::Member("value_read"),
            Event::Collect { control: CONTROL }
        ]
    );
    // nothing signalled: no collect
    clear_events();
    t.run::<i64>("return #A").unwrap();
    assert!(!events().iter().any(|e| matches!(e, Event::Collect { .. })));
    // idle: the worker's loop collects
    wake_from_elsewhere();
    assert_eq!(arena.collect_if_woken(), Some(0));
    assert_eq!(arena.collect_if_woken(), None);
}

// Stress: wakes from several threads at once while the worker keeps calling: every wake is counted with the control
// token and signalled, nothing else is touched off the worker, and every collect runs on the worker with that token.
pub fn wakes_from_many_threads_while_the_worker_calls() {
    let notified = Arc::new(AtomicUsize::new(0));
    let n = notified.clone();
    let options = ArenaOptions {
        notify: Some(Arc::new(move || {
            n.fetch_add(1, Ordering::SeqCst);
        })),
        ..ArenaOptions::default()
    };
    let (t, arena) = setup(options);
    let (array, _) = proxy(&t, &arena, KILN_VALUE_ARRAY_OWNED, 60);
    t.set("A", array);
    on("value_read", |_| handled(1, 0, length_packet(1)));
    clear_events();
    let before = arena.reader_counters();
    const THREADS: usize = 4;
    const WAKES: usize = 2000;
    let wakers: Vec<_> = (0..THREADS)
        .map(|_| {
            let reader = SendReader(reader());
            std::thread::spawn(move || {
                let reader = reader;
                for _ in 0..WAKES {
                    unsafe { reader.0.wake.unwrap()(reader.0.user, CONTROL) };
                }
            })
        })
        .collect();
    let mut calls = 0;
    while calls < 50 || wakers.iter().any(|w| !w.is_finished()) {
        assert_eq!(t.run::<i64>("return #A").unwrap(), 1);
        calls += 1;
    }
    for waker in wakers {
        waker.join().unwrap();
    }
    let _ = arena.collect_if_woken();
    assert_eq!(notified.load(Ordering::SeqCst), THREADS * WAKES);
    assert_eq!(arena.signal().wakes(), (THREADS * WAKES) as u64);
    assert_eq!(arena.signal().last_control(), CONTROL);
    let collects: Vec<Event> = events()
        .into_iter()
        .filter(|e| matches!(e, Event::Collect { .. }))
        .collect();
    assert!(
        !collects.is_empty() && collects.len() <= calls + 1,
        "{} collects for {calls} calls",
        collects.len()
    );
    assert!(
        collects
            .iter()
            .all(|e| *e == Event::Collect { control: CONTROL })
    );
    let after = arena.reader_counters();
    assert_eq!(
        (after.refused, after.panics),
        (before.refused, before.panics)
    );
    assert_eq!(arena.collect_if_woken(), None, "nothing left to collect");
}

// Red-first: a worker callback from another thread answers WRONG_THREAD and touches nothing.
pub fn callbacks_off_the_worker_answer_wrong_thread() {
    let (_t, arena) = setup(ArenaOptions::default());
    let reader = SendReader(reader());
    let codes = std::thread::spawn(move || {
        let reader = reader;
        let r = reader.0;
        let mut reply = empty_value();
        let request = KilnEmlLuaAccessV1 {
            size: std::mem::size_of::<KilnEmlLuaAccessV1>() as u32,
            operation: KILN_LUA_DESCRIBE as u32,
            object: 1,
            index: 0,
            key: KilnStr {
                ptr: std::ptr::null(),
                len: 0,
            },
            arena: 0,
            node_id: 0,
        };
        let mut required = 0usize;
        let codes = unsafe {
            [
                r.access.unwrap()(r.user, &request, &mut reply),
                r.copy.unwrap()(r.user, 1, std::ptr::null_mut(), 0, &mut required),
                r.retain.unwrap()(r.user, 1),
            ]
        };
        unsafe { r.release.unwrap()(r.user, 1) };
        codes
    })
    .join()
    .unwrap();
    assert_eq!(codes, [KILN_ERR_WRONG_THREAD; 3]);
    let counters = arena.reader_counters();
    assert_eq!(
        (
            counters.accesses,
            counters.copies,
            counters.retains,
            counters.releases
        ),
        (0, 0, 0, 0)
    );
}

// Red-first: closing the arena drains it while the VM lives and before the worker exits: arena_close, the provider's
// releases of what it retained (the frontend drops those values with the VM alive), arena_collect until pending is 0;
// afterwards no lease is closed again (the VM's teardown finalizes the proxies without calling the provider).
pub fn the_arena_closes_and_drains_before_the_vm_and_the_worker_go() {
    let log = std::thread::spawn(|| {
        let (t, arena) = setup(ArenaOptions::default());
        let (pinned, adopted) = proxy(&t, &arena, KILN_VALUE_STRUCT_OWNED, 12);
        t.set("P", pinned);
        // the provider pins the proxy (a slot), as kiln-eml does
        let pin = Rc::new(RefCell::new(0));
        let p = pin.clone();
        on("value_read", move |_| {
            let (_, code, reply) = make_proxy(KILN_VALUE_STRUCT_OWNED, 12);
            assert_eq!(code, KILN_OK);
            assert_eq!(retain(reply.object), KILN_OK);
            *p.borrow_mut() = reply.object;
            handled(1, 0, length_packet(0))
        });
        t.run::<i64>("return #P").unwrap();
        let pin = *pin.borrow();
        with(|f| f.pending.extend([2, 1, 0]));
        clear_events();
        // the host drops its VM first: the arena still holds it, so the drain runs with Lua alive
        let lua = t.lua().clone();
        drop(t);
        mark("vm-dropped-by-host");
        assert!(arena.close());
        assert!(arena.drained());
        assert!(arena.close(), "idempotent");
        mark("closed");
        drop(arena);
        drop(lua);
        mark("worker-end");
        (events(), pin, adopted)
    })
    .join()
    .unwrap();
    let (events, pin, adopted) = log;
    let interesting: Vec<Event> = events
        .into_iter()
        .filter(|e| !matches!(e, Event::Copy { .. }))
        .collect();
    assert_eq!(
        interesting,
        [
            Event::Marker("vm-dropped-by-host"),
            Event::ArenaClose,
            Event::Release { object: pin },
            Event::Collect { control: CONTROL },
            Event::Collect { control: CONTROL },
            Event::Marker("closed"),
            Event::Marker("worker-end"),
        ],
        "no lease ({adopted}) is closed after arena_close"
    );
}

// Red-first: a drain that never reaches 0 is reported and leaves the reader allocated (never freed under the
// provider); the provider can still call it.
pub fn an_undrained_arena_keeps_its_reader() {
    let faults = Rc::new(RefCell::new(Vec::new()));
    let f2 = faults.clone();
    let (_t, arena) = setup(ArenaOptions {
        on_fault: Some(Rc::new(move |e: &ValuesError| {
            f2.borrow_mut().push(e.to_string())
        })),
        ..ArenaOptions::default()
    });
    with(|f| f.pending.extend(std::iter::repeat_n(1, 100)));
    let reader = reader();
    assert!(!arena.close());
    assert!(!arena.drained());
    assert!(
        faults.borrow().iter().any(|f| f.contains("pending")),
        "{:?}",
        faults.borrow()
    );
    drop(arena);
    // still valid: a late release from the provider finds its user context
    unsafe { reader.release.unwrap()(reader.user, 999) };
}

// Review fix: an arena_create that admits the reader but returns no control token leaves the reader with the provider
// for good (nothing can close or drain that arena), so its user context is leaked, never freed under the provider.
pub fn an_arena_admitted_without_a_control_keeps_its_reader() {
    let t = vm();
    let notified = Arc::new(AtomicUsize::new(0));
    let n = notified.clone();
    with(|f| f.create_no_control = true);
    let created = t.arena(ArenaOptions {
        notify: Some(Arc::new(move || {
            n.fetch_add(1, Ordering::SeqCst);
        })),
        ..ArenaOptions::default()
    });
    match created {
        Err(ValuesError::Fault(why)) => assert!(why.contains("control"), "{why}"),
        Err(other) => panic!("{other}"),
        Ok(_) => panic!("an arena without a control token"),
    }
    // the provider still holds the reader: a wake (any thread) and a release (worker) find their user context
    let reader = reader();
    unsafe { reader.wake.unwrap()(reader.user, CONTROL) };
    unsafe { reader.release.unwrap()(reader.user, 999) };
    assert_eq!(notified.load(Ordering::SeqCst), 1);
}

struct Panicky;
impl KnownUserdata for Panicky {
    fn classify(&self, _: &AnyUserData) -> Option<(u32, Vec<u8>)> {
        panic!("a classifier that panics")
    }
}

// Review fix (test gap): a panic inside a callback is contained there: access and copy answer INTERNAL (a transport
// failure for the provider) and count it, the reader keeps answering, and wake returns normally. Nothing unwinds across
// the C boundary: an unwind out of an `extern "C"` callback would abort this test process.
pub fn callbacks_contain_panics_and_never_unwind_into_the_provider() {
    let notified = Arc::new(AtomicUsize::new(0));
    let n = notified.clone();
    let (t, arena) = setup(ArenaOptions {
        known: Some(Rc::new(Panicky)),
        notify: Some(Arc::new(move || {
            n.fetch_add(1, Ordering::SeqCst);
            panic!("a host signal that panics");
        })),
        ..ArenaOptions::default()
    });
    let (typed, _) = proxy(&t, &arena, KILN_VALUE_STRUCT_OWNED, 21);
    t.set("V", typed);
    t.set("F", t.lua().create_userdata(Foreign).unwrap());
    let codes = Rc::new(RefCell::new(Vec::new()));
    let c = codes.clone();
    on("value_set", move |call| {
        // a foreign userdata: the classifier runs while the reply is made, and panics
        let (foreign, _) = access(KILN_LUA_GET_FIELD_VALUE, call.object, b"f", 0);
        // a typed value needs no classifier; copying its token runs it, and it panics
        let (typed, reply) = access(KILN_LUA_GET_FIELD_VALUE, call.object, b"v", 0);
        let copied = copy(reply.object).err().unwrap_or(KILN_OK);
        // the reader still answers
        let (after, reply) = access(KILN_LUA_GET_FIELD_VALUE, call.object, b"n", 0);
        c.borrow_mut()
            .extend([foreign, typed, copied, after, reply.bits as KilnResult]);
        mutated(1)
    });
    let before = arena.reader_counters();
    t.run::<()>("V.x = {f = F, v = V, n = 7}").unwrap();
    assert_eq!(
        *codes.borrow(),
        [KILN_ERR_INTERNAL, KILN_OK, KILN_ERR_INTERNAL, KILN_OK, 7]
    );
    assert_eq!(arena.reader_counters().panics - before.panics, 2);
    let reader = reader();
    unsafe { reader.wake.unwrap()(reader.user, CONTROL) };
    assert_eq!(notified.load(Ordering::SeqCst), 1);
    assert_eq!(
        arena.signal().wakes(),
        1,
        "the wake was counted before the host's signal panicked"
    );
    assert_eq!(arena.collect_if_woken(), Some(0));
    assert!(arena.close());
}

// Review fix: a proxy the frontend cannot make (here: Lua out of memory) never takes the provisional lease: the
// request answers INTERNAL and the provisional stays the provider's to close (the frontend closed it before as well).
pub fn a_proxy_that_cannot_be_made_never_takes_the_provisional() {
    let (t, arena) = setup(ArenaOptions::default());
    let lua = t.lua().clone();
    let seen = Rc::new(RefCell::new(None));
    let s = seen.clone();
    on("value_open", move |_| {
        lua.gc_collect().unwrap();
        lua.gc_collect().unwrap();
        lua.set_memory_limit(1).unwrap();
        let (provisional, code, _) = make_proxy(KILN_VALUE_STRUCT_OWNED, 31);
        lua.set_memory_limit(0).unwrap();
        *s.borrow_mut() = Some((provisional, code, closes_of(provisional)));
        handled(1, 0, view_nil())
    });
    arena.open_default(t.lua(), "t.Any").unwrap();
    let (provisional, code, closes) = seen.borrow().unwrap();
    assert_eq!(code, KILN_ERR_INTERNAL);
    assert_eq!(
        closes, 0,
        "the frontend closed a provisional it never adopted"
    );
    t.collect();
    assert_eq!(closes_of(provisional), 0, "nor did a finalizer");
    assert_eq!(arena.reader_counters().adopted, 0);
}

// ---- packets and errors ----

// Red-first: every frozen fixture parses and every invalid one is refused (closed objects, tagged scalars, closed
// classes and kinds, root-only internal, found/found_userdata rules).
pub fn the_frozen_packets_parse_and_the_invalid_ones_are_refused() {
    let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../../schema/sfi/eml-values/fixtures");
    let valid: serde_json::Map<String, serde_json::Value> =
        serde_json::from_slice(&std::fs::read(dir.join("packets.json")).unwrap()).unwrap();
    assert!(valid.len() >= 40);
    for (name, packet) in &valid {
        let parsed = packets::parse(&serde_json::to_vec(packet).unwrap());
        assert!(parsed.is_ok(), "{name}: {parsed:?}");
    }
    let invalid: Vec<serde_json::Value> =
        serde_json::from_slice(&std::fs::read(dir.join("invalid.json")).unwrap()).unwrap();
    assert!(invalid.len() >= 30);
    for case in &invalid {
        let parsed = packets::parse(&serde_json::to_vec(&case["packet"]).unwrap());
        assert!(parsed.is_err(), "{}: accepted", case["why"]);
    }
    // the scalars decode losslessly
    assert_eq!(
        packets::parse(&serde_json::to_vec(&valid["view_i64_min"]).unwrap()).unwrap(),
        packets::Packet::View(packets::View::Scalar(packets::Scalar::Int(i64::MIN)))
    );
    assert_eq!(
        packets::parse(&serde_json::to_vec(&valid["view_f64_nan_payload"]).unwrap()).unwrap(),
        packets::Packet::View(packets::View::Scalar(packets::Scalar::Num(
            0x7ff8_0000_0000_0001
        )))
    );
    assert_eq!(
        packets::parse(&serde_json::to_vec(&valid["view_string_bytes"]).unwrap()).unwrap(),
        packets::Packet::View(packets::View::Scalar(packets::Scalar::Str(vec![
            0x61, 0, 0x62, 0xff
        ])))
    );
}

// Red-first: each error class is raised as Source raises it (RC-11): Source texts as Error::external (first line =
// the text), borrows as mlua's borrow errors, internal as Kiln's own text (reported, never Source's); the error packet
// is read first and its lease closed exactly once.
pub fn error_packets_raise_the_errors_source_raises() {
    let faults = Rc::new(RefCell::new(0));
    let f2 = faults.clone();
    let (t, arena) = setup(ArenaOptions {
        on_fault: Some(Rc::new(move |_: &ValuesError| *f2.borrow_mut() += 1)),
        ..ArenaOptions::default()
    });
    let (root, _) = proxy(&t, &arena, KILN_VALUE_STRUCT_OWNED, 13);
    t.set("V", root);
    let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../../schema/sfi/eml-values/fixtures");
    let valid: serde_json::Map<String, serde_json::Value> =
        serde_json::from_slice(&std::fs::read(dir.join("packets.json")).unwrap()).unwrap();
    let mut checked = 0;
    for (name, packet) in valid.iter().filter(|(_, p)| p["packet"] == "error") {
        let token = Rc::new(RefCell::new(0));
        let tk = token.clone();
        let packet = packet.clone();
        on("value_child", move |_| {
            let error = json(packet.clone());
            *tk.borrow_mut() = error;
            semantic(error)
        });
        let message: String = t.run("return perr(function() return V.x end)").unwrap();
        let token = *token.borrow();
        let level = &valid[name]["error"];
        let text = String::from_utf8_lossy(
            &(2..level["message"].as_str().unwrap().len())
                .step_by(2)
                .skip(2)
                .map(|i| {
                    u8::from_str_radix(&level["message"].as_str().unwrap()[i..i + 2], 16).unwrap()
                })
                .collect::<Vec<u8>>(),
        )
        .into_owned();
        let expected = match level["class"].as_str().unwrap() {
            "borrow" if level["mode"] == "mutable" => "error mutably borrowing userdata".to_owned(),
            "borrow" => "error borrowing userdata".to_owned(),
            "internal" => format!("runtime error: kiln: kiln.eml.values: provider fault: {text}"),
            _ => text.replace(
                "userdata `<frontend TypeId>`",
                &match level["found_userdata"]["value_kind"].as_str() {
                    Some("struct_owned") => format!("userdata `{:?}`", TypeId::of::<StructValue>()),
                    Some("array_mapped") => {
                        format!("userdata `{:?}`", TypeId::of::<MappedArrayValue>())
                    }
                    _ => "userdata `<frontend TypeId>`".to_owned(),
                },
            ),
        };
        assert_eq!(message, expected.lines().next().unwrap_or(""), "{name}");
        let copies = events()
            .iter()
            .filter(|e| matches!(e, Event::Copy { token: t, .. } if *t == token))
            .count();
        assert!(copies >= 1, "{name}: the packet was read");
        assert_eq!(closes_of(token), 1, "{name}: the error lease closed once");
        checked += 1;
    }
    assert!(checked >= 20, "{checked} error fixtures");
    assert_eq!(
        *faults.borrow(),
        1,
        "only the internal fixture is a Kiln fault"
    );
}

// Red-first: a root `lua` level re-raises the original mlua::Error the reader returned, looked up before the error
// lease closes (the lease's close releases the retain); without a retained host error, the text as Error::external.
pub fn a_lua_level_reraises_the_original_error() {
    let (t, arena) = setup(ArenaOptions::default());
    let (root, _) = proxy(&t, &arena, KILN_VALUE_STRUCT_OWNED, 14);
    t.set("V", root);
    let input: Value = t
        .run(r#"return setmetatable({}, {__index = function() error("original") end})"#)
        .unwrap();
    t.set("INPUT", input);
    // call 1: the reader's error, retained by the provider beyond the call
    let held = Rc::new(RefCell::new(0));
    let h = held.clone();
    on("value_set", move |call| {
        let (_, inner) = access(KILN_LUA_DESCRIBE, call.object, b"", 0);
        let (_, reply) = access(KILN_LUA_GET_FIELD_VALUE, inner.object, b"x", 0);
        assert_eq!(retain(reply.error_object), KILN_OK);
        *h.borrow_mut() = reply.error_object;
        mutated(1)
    });
    t.run::<()>("V.x = INPUT").unwrap();
    let held = *held.borrow();
    let original_text = String::from_utf8(copy_outside(&t, held)).unwrap();
    // call 2: an error tree whose root is that lua level; closing its lease releases the retain
    on("value_child", move |_| {
        let error = error_packet(json!({
            "class": "lua",
            "message": bytes_text(b"ignored text"),
            "host_error": format!("u64:{held:016x}"),
        }));
        with(|f| f.owns.insert(error, held));
        semantic(error)
    });
    let error = t.run::<Value>("return V.y").unwrap_err();
    let mlua::Error::CallbackError { cause, .. } = &error else {
        panic!("{error:?}")
    };
    assert!(
        matches!(&**cause, mlua::Error::RuntimeError(_)),
        "the original error, not a copy: {cause:?}"
    );
    assert_eq!(cause.to_string(), original_text);
    assert!(events().contains(&Event::Release { object: held }));
    assert_eq!(arena.live_tokens().1, 0);
    // no host error: the text as Error::external
    on("value_child", |_| {
        semantic(error_packet(
            json!({"class": "lua", "message": bytes_text(b"runtime error: lost")}),
        ))
    });
    let error = t.run::<Value>("return V.z").unwrap_err();
    let mlua::Error::CallbackError { cause, .. } = &error else {
        panic!()
    };
    assert!(matches!(&**cause, mlua::Error::ExternalError(_)));
    assert_eq!(cause.to_string(), "runtime error: lost");
}

/// The text of a reader object, copied from inside a provider call.
fn copy_outside(t: &TestVm, token: u64) -> Vec<u8> {
    let out = Rc::new(RefCell::new(Vec::new()));
    let o = out.clone();
    on("value_read", move |_| {
        *o.borrow_mut() = copy(token).unwrap();
        handled(1, 0, length_packet(0))
    });
    t.run::<i64>("return #V").unwrap();
    out.take()
}

// Red-first: Source's `userdata \`TypeId(..)\`` label is rendered from the found userdata's own Rust TypeId,
// and Source's `guid expected, got ..` type name as `{value:#?}` of a userdata or mlua's type_name of a function.
pub fn sources_typeid_label_and_type_name_are_rendered_from_the_found_value() {
    let (t, arena) = setup(ArenaOptions::default());
    let (root, _) = proxy(&t, &arena, KILN_VALUE_STRUCT_OWNED, 15);
    let (other, _) = proxy(&t, &arena, KILN_VALUE_VARIANT_MAPPED, 16);
    t.set("V", root);
    t.set("W", other.clone());
    t.set("F", t.lua().create_userdata(Foreign).unwrap());
    let label = |value_kind: Option<&str>, lua_kind: &str| {
        let mut found = json!({"lua_kind": lua_kind, "adapter": "none"});
        if let Some(kind) = value_kind {
            found["value_kind"] = json!(kind);
        }
        move |call: &Call| {
            access(KILN_LUA_DESCRIBE, call.object, b"", 0);
            semantic(error_packet(json!({
                "class": "validation",
                "message": bytes_text(b"at .: expected `t.List`, got `userdata `<frontend TypeId>``"),
                "path": ".",
                "kind": "incompatible_type",
                "expected": "t.List",
                "found_userdata": found.clone(),
            })))
        }
    };
    on("value_set", label(Some("variant_mapped"), "typed_value"));
    let message: String = t.run("return perr(function() V.list = W end)").unwrap();
    assert_eq!(
        message,
        format!(
            "at .: expected `t.List`, got `userdata `{:?}``",
            TypeId::of::<MappedVariantValue>()
        )
    );
    on("value_set", label(None, "other_userdata"));
    let message: String = t.run("return perr(function() V.list = F end)").unwrap();
    assert_eq!(
        message,
        format!(
            "at .: expected `t.List`, got `userdata `{:?}``",
            TypeId::of::<Foreign>()
        )
    );
    let guid = |call: &Call| {
        access(KILN_LUA_DESCRIBE, call.object, b"", 0);
        semantic(error_packet(json!({
            "class": "validation",
            "message": bytes_text(b"at guid: guid expected, got <frontend type name>"),
            "path": "guid",
            "kind": "lua",
            "cause": {"class": "runtime", "message": bytes_text(b"guid expected, got <frontend type name>")},
        })))
    };
    on("value_set", guid);
    let message: String = t
        .run("return perr(function() V.guid = function() end end)")
        .unwrap();
    assert_eq!(message, "at guid: guid expected, got function");
    let message: String = t.run("return perr(function() V.guid = W end)").unwrap();
    assert_eq!(message, format!("at guid: guid expected, got {other:#?}"));
    assert!(
        message.starts_with("at guid: guid expected, got MappedVariantValue: 0x"),
        "{message}"
    );
    // the classes' TypeIds are distinct (Source's label tells the classes apart)
    let ids = [
        TypeId::of::<StructValue>(),
        TypeId::of::<MappedStructValue>(),
        TypeId::of::<ArrayValue>(),
        TypeId::of::<MappedArrayValue>(),
        TypeId::of::<VariantValue>(),
        TypeId::of::<MappedVariantValue>(),
    ];
    for (i, a) in ids.iter().enumerate() {
        assert!(ids[i + 1..].iter().all(|b| b != a));
    }
}

// Red-first: a packet is copied with a probe and one exact retry, never a second operation; a size that changes
// between the two, or a refused copy, is Kiln's error; STRINGIFY keeps its bytes exactly.
pub fn packets_copy_with_a_probe_and_one_exact_retry() {
    let faults = Rc::new(RefCell::new(Vec::new()));
    let f2 = faults.clone();
    let (t, arena) = setup(ArenaOptions {
        on_fault: Some(Rc::new(move |e: &ValuesError| {
            f2.borrow_mut().push(e.to_string())
        })),
        ..ArenaOptions::default()
    });
    let (root, _) = proxy(&t, &arena, KILN_VALUE_STRUCT_OWNED, 17);
    t.set("V", root);
    let mut text = vec![b'x'; 999];
    text.push(0xff);
    let bytes = text.clone();
    let payload = Rc::new(RefCell::new(0));
    let p = payload.clone();
    on("value_read", move |call| {
        assert_eq!(call.query, KILN_VALUE_READ_STRINGIFY as u32);
        let token = packet(bytes.clone());
        *p.borrow_mut() = token;
        handled(1, 0, token)
    });
    clear_events();
    let got: mlua::String = t.run("return tostring(V)").unwrap();
    assert_eq!(got.as_bytes().to_vec(), text);
    let token = *payload.borrow();
    let shape: Vec<Event> = events()
        .into_iter()
        .filter(|e| {
            matches!(
                e,
                Event::Member(_) | Event::Copy { .. } | Event::Close { .. }
            )
        })
        .collect();
    assert_eq!(
        shape,
        [
            Event::Member("value_read"),
            Event::Copy {
                token,
                capacity: 256
            },
            Event::Copy {
                token,
                capacity: 1000
            },
            Event::Close {
                lease: token,
                in_access: false
            },
        ]
    );
    with(|f| f.copy_grow = 5);
    let message: String = t
        .run("return perr(function() return tostring(V) end)")
        .unwrap();
    assert!(
        message.contains("value_result_copy changed size"),
        "{message}"
    );
    assert!(message.starts_with("runtime error: kiln: "), "{message}");
    assert_eq!(closes_of(*payload.borrow()), 1, "the payload still closed");
    assert!(!faults.borrow().is_empty());
}

// Red-first: transport refusals, an unknown disposition, an unknown packet version and an UNSUPPORTED reply for a
// typed receiver are Kiln's errors (never Source texts), and every lease such a reply carried is closed.
pub fn refusals_and_unreadable_replies_are_kilns_errors() {
    let (t, arena) = setup(ArenaOptions::default());
    let (root, _) = proxy(&t, &arena, KILN_VALUE_STRUCT_OWNED, 18);
    t.set("V", root);
    on("value_child", |_| Answer::Refuse(KILN_ERR_REVOKED));
    let message: String = t.run("return perr(function() return V.x end)").unwrap();
    assert_eq!(
        message,
        "runtime error: kiln: kiln.eml.values.value_child refused (KilnResult 6)"
    );
    let item = Rc::new(RefCell::new(0));
    let i2 = item.clone();
    on("value_child", move |_| {
        let lease = fake::lease(KILN_VALUE_STRUCT_OWNED, 99);
        *i2.borrow_mut() = lease;
        Answer::Reply {
            disposition: 9,
            flags: 0,
            arity: 1,
            item: lease,
            payload: 0,
            error: 0,
            revision: 0,
        }
    });
    let message: String = t.run("return perr(function() return V.x end)").unwrap();
    assert_eq!(
        message,
        "runtime error: kiln: kiln.eml.values: unknown disposition 9"
    );
    assert_eq!(closes_of(*item.borrow()), 1);
    on("value_read", |_| {
        handled(
            1,
            0,
            json(json!({"schema_version": 2, "packet": "length", "value": i64_text(1)})),
        )
    });
    let message: String = t.run("return perr(function() return #V end)").unwrap();
    assert!(message.contains("schema_version 2"), "{message}");
    on("value_read", |_| unsupported());
    let message: String = t.run("return perr(function() return #V end)").unwrap();
    assert!(
        message.contains("UNSUPPORTED for a typed receiver"),
        "{message}"
    );
}

// Red-first (O1): a LOADED_SNAPSHOT root lives as long as the ResourceInfo that keeps its userdata: while it lives a
// re-open answers the same userdata (the new item lease closes); once it is collected the root's lease closes and a
// re-open is a fresh proxy.
pub fn a_loaded_root_lives_as_long_as_its_resource_info() {
    let (t, arena) = setup(ArenaOptions::default());
    let opened = Rc::new(RefCell::new(Vec::new()));
    let o = opened.clone();
    on("value_open", move |call| {
        assert_eq!(call.origin, KILN_VALUE_OPEN_LOADED_SNAPSHOT as u32);
        o.borrow_mut().push((call.instance, call.bytes.clone()));
        let item = fake::lease(KILN_VALUE_STRUCT_MAPPED, 30);
        handled(1, item, 0)
    });
    let root = arena
        .open_loaded(t.lua(), "t.Root", b"\x01\x02", 10, 1)
        .unwrap();
    let Value::UserData(first) = &root else {
        panic!()
    };
    let first_lease = kiln_sfi::runtime::values::proxies::lease_of(first).unwrap();
    // the ResourceInfo keeps the root (Source's value cache)
    let info = t.lua().create_table().unwrap();
    info.set("root", root.clone()).unwrap();
    t.set("Info", info);
    drop(root);
    let again = arena
        .open_loaded(t.lua(), "t.Root", b"\x01\x02", 10, 1)
        .unwrap();
    t.set("Again", again);
    assert!(t.run::<bool>("return rawequal(Again, Info.root)").unwrap());
    t.run::<()>("Again = nil").unwrap();
    assert_eq!(opened.borrow()[0], (10, vec![1, 2]));
    t.collect();
    assert_eq!(
        closes_of(first_lease),
        0,
        "alive while its ResourceInfo lives"
    );
    t.run::<()>("Info = nil").unwrap();
    t.collect();
    assert_eq!(closes_of(first_lease), 1, "collected with its ResourceInfo");
    let fresh = arena
        .open_loaded(t.lua(), "t.Root", b"\x01\x02", 10, 1)
        .unwrap();
    let Value::UserData(fresh) = &fresh else {
        panic!()
    };
    assert_ne!(
        kiln_sfi::runtime::values::proxies::lease_of(fresh),
        Some(first_lease)
    );
}
/// Every typed class has ShroudForge's registrations and each one goes to the provider as itself: a struct's or
/// an array's `__index` is `value_child` with the key's selector (a struct's field, an array's index), `__newindex`
/// `value_set`, `__len` a LENGTH read, `__tostring` a STRINGIFY read and `__pairs` an ITER_BEGIN read whose state is
/// the class's own iterator userdata (arity 3; each step an ITER_NEXT read); a mapped variant has the fields `type` and
/// `value` only. The answers are the provider's (scripted here; Source's through the real provider is the consumer
/// library's differential).
pub fn every_class_delegates_its_shroudforge_metamethods_to_the_provider() {
    let (t, arena) = setup(ArenaOptions::default());
    type Seen = (&'static str, u32, Option<(u32, u64, Vec<u8>)>);
    let calls: Rc<RefCell<Vec<Seen>>> = Rc::new(RefCell::new(Vec::new()));
    for member in ["value_child", "value_set", "value_read"] {
        let c = calls.clone();
        on(member, move |call| {
            c.borrow_mut()
                .push((call.member, call.query, call.selector.clone()));
            match (call.member, call.query as i32) {
                ("value_set", _) => mutated(9),
                ("value_child", _) => {
                    handled(1, 0, view(json!({"tag": "string", "value": "string:6f6b"})))
                }
                (_, KILN_VALUE_READ_LENGTH) => handled(1, 0, length_packet(3)),
                (_, KILN_VALUE_READ_STRINGIFY) => handled(1, 0, packet(b"shown".to_vec())),
                (_, KILN_VALUE_READ_ITER_BEGIN) => {
                    handled(3, fake::lease(KILN_VALUE_ITERATOR, 0), 0)
                }
                _ => handled(
                    2,
                    0,
                    json(json!({"schema_version": 1, "packet": "iter_next",
                        "key": {"tag": "nil"}, "value": {"tag": "nil"}})),
                ),
            }
        });
    }
    let field = |name: &[u8]| Some((KILN_VALUE_FIELD as u32, 0, name.to_vec()));
    let index = |i: u64| Some((KILN_VALUE_INDEX as u32, i, Vec::new()));
    let read = |query: i32| ("value_read", query as u32, None);
    for (node, (kind, iterator, key, selector)) in [
        (
            KILN_VALUE_STRUCT_OWNED,
            "StructValueIter",
            "X.f",
            field(b"f"),
        ),
        (
            KILN_VALUE_STRUCT_MAPPED,
            "MappedStructValueIter",
            "X.f",
            field(b"f"),
        ),
        (KILN_VALUE_ARRAY_OWNED, "ArrayValueIter", "X[2]", index(2)),
        (
            KILN_VALUE_ARRAY_MAPPED,
            "MappedArrayValueIter",
            "X[2]",
            index(2),
        ),
    ]
    .into_iter()
    .enumerate()
    {
        let (x, _) = proxy(&t, &arena, kind, 60 + node as u64);
        t.set("X", x);
        calls.borrow_mut().clear();
        let (got, n, shown, arity, state, steps): (String, i64, String, i64, AnyUserData, i64) = t
            .run(&format!(
                "local got = {key} {key} = 5 local n = #X local s = tostring(X) \
                 local arity = select('#', pairs(X)) local _, state = pairs(X) \
                 local steps = 0 for _ in pairs(X) do steps = steps + 1 end \
                 return got, n, s, arity, state, steps"
            ))
            .unwrap_or_else(|e| panic!("{iterator}: {e}"));
        assert_eq!(
            (got.as_str(), n, shown.as_str(), arity, steps),
            ("ok", 3, "shown", 3, 0),
            "{iterator}"
        );
        assert_eq!(state.type_name().unwrap().as_deref(), Some(iterator));
        drop(state);
        assert_eq!(
            calls.borrow().clone(),
            [
                ("value_child", 0, selector.clone()),
                ("value_set", 0, selector),
                read(KILN_VALUE_READ_LENGTH),
                read(KILN_VALUE_READ_STRINGIFY),
                read(KILN_VALUE_READ_ITER_BEGIN),
                read(KILN_VALUE_READ_ITER_BEGIN),
                read(KILN_VALUE_READ_ITER_BEGIN),
                read(KILN_VALUE_READ_ITER_NEXT),
            ],
            "{iterator}"
        );
    }
    // a mapped variant: the fields type and value (get), value (set), and nothing else
    let (v, _) = proxy(&t, &arena, KILN_VALUE_VARIANT_MAPPED, 70);
    t.set("V", v);
    calls.borrow_mut().clear();
    let (kind, value, length): (String, String, String) = t
        .run("local k, v = V.type, V.value V.value = 1 return k, v, perr(function() return #V end)")
        .unwrap();
    assert_eq!((kind.as_str(), value.as_str()), ("ok", "ok"));
    assert!(
        length.contains("attempt to get length of a MappedVariantValue value"),
        "{length}"
    );
    assert_eq!(
        calls.borrow().clone(),
        [
            ("value_child", 0, field(b"type")),
            ("value_child", 0, field(b"value")),
            ("value_set", 0, field(b"value")),
        ]
    );
}

// Red-first: ShroudForge's userdata shapes and the typed table port: receiver and key casts (sfi-core's MethodArgs
// texts, before any provider call), the struct key's String cast and the array's usize cast, variant fields only,
// pairs arity 3 with an iterator state, table.insert/remove/clear on arrays only, the port's mutable borrow seen by a
// re-entrant read, and one mutation per edit.
pub fn shapes_and_the_table_port_follow_shroudforge() {
    let (t, arena) = setup(ArenaOptions::default());
    let (s, _) = proxy(&t, &arena, KILN_VALUE_STRUCT_OWNED, 40);
    let (a, _) = proxy(&t, &arena, KILN_VALUE_ARRAY_MAPPED, 41);
    let (v, _) = proxy(&t, &arena, KILN_VALUE_VARIANT_OWNED, 42);
    t.set("S", s);
    t.set("A", a);
    t.set("Var", v);
    t.set("F", t.lua().create_userdata(Foreign).unwrap());
    let calls = Rc::new(RefCell::new(Vec::new()));
    for member in ["value_child", "value_set"] {
        let c = calls.clone();
        on(member, move |call| {
            c.borrow_mut().push((call.member, call.selector.clone()));
            if call.member == "value_set" {
                mutated(4)
            } else {
                handled(1, 0, view(json!({"tag": "string", "value": "string:6f6b"})))
            }
        });
    }
    clear_events();
    let texts: Vec<String> = t
        .run(
            r#"return {
          perr(function() return S[{}] end),
          perr(function() return A["x"] end),
          perr(function() return table.insert(F, 1) end),
          perr(function() return table.clear(F) end),
        }"#,
        )
        .unwrap();
    assert_eq!(texts[0], "bad argument #1 (string expected, got table)");
    assert_eq!(
        texts[1],
        "bad argument #1 (64-bit unsigned integer expected, got string)"
    );
    assert!(
        texts[2].contains("(table expected, got Foreign)"),
        "{}",
        texts[2]
    );
    assert!(
        texts[3].contains("'clear' is not supported for userdata"),
        "{}",
        texts[3]
    );
    // mlua protects the metatables (as in Source): a mod cannot reach the meta functions with another receiver
    assert!(!t.run::<bool>("return getmetatable(S) ~= false").unwrap());
    assert!(
        !events().iter().any(|e| matches!(e, Event::Member(_))),
        "no provider call: {:?}",
        events()
    );
    // a struct is no array for the port: Lua's own insert treats it as a table with __len and __newindex (Lua 5.4's
    // checktab), as it does Source's StructValue
    on("value_read", |_| handled(1, 0, length_packet(0)));
    calls.borrow_mut().clear();
    t.run::<()>("table.insert(S, 7)").unwrap();
    assert_eq!(
        calls.borrow().clone(),
        [(
            "value_set",
            Some((KILN_VALUE_FIELD as u32, 0, b"1".to_vec()))
        )]
    );
    assert!(
        !events()
            .iter()
            .any(|e| matches!(e, Event::Member(m) if m.starts_with("value_array")))
    );
    calls.borrow_mut().clear();
    clear_events();
    // key casts: an integer struct key is the field "1"; an array's numeric string is its index
    let results: Vec<String> = t.run(r#"return {S[1], A["2"], Var.type}"#).unwrap();
    assert_eq!(results, ["ok", "ok", "ok"]);
    t.run::<()>("Var.value = 5 S.f = 1").unwrap();
    assert_eq!(arena.last_mutation(), (true, 4));
    let selectors = calls.borrow().clone();
    assert_eq!(
        selectors,
        [
            (
                "value_child",
                Some((KILN_VALUE_FIELD as u32, 0, b"1".to_vec()))
            ),
            (
                "value_child",
                Some((KILN_VALUE_INDEX as u32, 2, Vec::new()))
            ),
            (
                "value_child",
                Some((KILN_VALUE_FIELD as u32, 0, b"type".to_vec()))
            ),
            (
                "value_set",
                Some((KILN_VALUE_FIELD as u32, 0, b"value".to_vec()))
            ),
            (
                "value_set",
                Some((KILN_VALUE_FIELD as u32, 0, b"f".to_vec()))
            ),
        ]
    );
    // variants have fields only: no __len, no __pairs, no setter for "type"
    let variant: Vec<String> = t
        .run(r#"return { perr(function() return #Var end), perr(function() Var.type = "x" end) }"#)
        .unwrap();
    assert!(
        variant[0].contains("attempt to get length of a VariantValue value"),
        "{}",
        variant[0]
    );
    assert!(!variant[1].is_empty());
    // pairs: arity 3, a StructValueIter state, scalar and item steps, the end
    let step = Rc::new(RefCell::new(0));
    let st = step.clone();
    on("value_read", move |call| match call.query as i32 {
        KILN_VALUE_READ_ITER_BEGIN => handled(3, fake::lease(KILN_VALUE_ITERATOR, 0), 0),
        KILN_VALUE_READ_ITER_NEXT => {
            *st.borrow_mut() += 1;
            match *st.borrow() {
                1 => handled(
                    2,
                    0,
                    json(json!({"schema_version": 1, "packet": "iter_next",
                    "key": {"tag": "string", "value": "string:61"}, "value": {"tag": "i64", "value": i64_text(1)}})),
                ),
                2 => handled(
                    2,
                    fake::lease(KILN_VALUE_STRUCT_OWNED, 43),
                    json(json!({"schema_version": 1, "packet": "iter_next",
                    "key": {"tag": "string", "value": "string:62"}, "value": {"tag": "item"}})),
                ),
                _ => handled(
                    2,
                    0,
                    json(json!({"schema_version": 1, "packet": "iter_next",
                    "key": {"tag": "nil"}, "value": {"tag": "nil"}})),
                ),
            }
        }
        _ => handled(1, 0, length_packet(0)),
    });
    let (arity, state, keys, item): (i64, AnyUserData, String, AnyUserData) = t
        .run(
            r#"local n = select('#', pairs(S))
            local _, state = pairs(S)
            local keys, item = {}, nil
            for k, v in pairs(S) do
              keys[#keys + 1] = k .. "=" .. type(v)
              if type(v) == "userdata" then item = v end
            end
            return n, state, table.concat(keys, ","), item"#,
        )
        .unwrap();
    assert_eq!((arity, keys.as_str()), (3, "a=number,b=userdata"));
    assert_eq!(
        state.type_name().unwrap().as_deref(),
        Some("StructValueIter")
    );
    assert_eq!(item.type_name().unwrap().as_deref(), Some("StructValue"));
    drop((state, item));
    // table operations on arrays go to the provider with the value's token; nothing removed is one nil
    let described = Rc::new(RefCell::new(Vec::new()));
    let d = described.clone();
    on("value_array_push", move |call| {
        d.borrow_mut()
            .push(access(KILN_LUA_DESCRIBE, call.object, b"", 0).1.bits);
        handled(1, 0, 0)
    });
    let d = described.clone();
    on("value_array_insert", move |call| {
        d.borrow_mut().push(call.index);
        handled(1, 0, 0)
    });
    on("value_array_pop", |_| {
        handled(
            1,
            fake::lease(KILN_VALUE_INTEGER, 50),
            view(json!({"tag": "i64", "value": i64_text(8)})),
        )
    });
    on("value_array_remove", |call| {
        if call.index == 9 {
            handled(1, 0, 0)
        } else {
            handled(1, fake::lease(KILN_VALUE_STRUCT_OWNED, 44), 0)
        }
    });
    on("value_array_clear", |_| handled(1, 0, 0));
    let (popped, removed, nothing, count): (i64, AnyUserData, Value, i64) = t
        .run(
            r#"table.insert(A, 7) table.insert(A, 2, 1) table.clear(A)
            return table.remove(A), table.remove(A, 1), table.remove(A, 9), select('#', table.remove(A, 9))"#,
        )
        .unwrap();
    assert_eq!((popped, nothing, count), (8, Value::Nil, 1));
    assert_eq!(removed.type_name().unwrap().as_deref(), Some("StructValue"));
    drop(removed);
    assert_eq!(*described.borrow(), [7, 2]);
    // the port holds the array mutably: a re-entrant #A from a validation callback sees mlua's borrow error
    let reentrant = Rc::new(RefCell::new(None));
    let r = reentrant.clone();
    on("value_array_push", move |call| {
        let (_, reply) = access(KILN_LUA_GET_FIELD_VALUE, call.object, b"x", 0);
        *r.borrow_mut() =
            Some(copy(reply.error_object).map(|b| String::from_utf8_lossy(&b).into_owned()));
        handled(1, 0, 0)
    });
    t.run::<()>(r#"table.insert(A, setmetatable({}, {__index = function() return #A end}))"#)
        .unwrap();
    let text = reentrant.borrow().clone().unwrap().unwrap();
    assert!(text.starts_with("error borrowing userdata"), "{text}");
}
