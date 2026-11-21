// SPDX-License-Identifier: MIT
//! kiln-sfi's typed-value frontend over kiln-eml's real kiln.eml.values provider, with a real mlua VM on
//! the calling thread (the worker).
//!
//! Built as a cdylib (`cargo build --example values_consumer`). kiln-eml's own C-consumer test
//! (`m6_value_abi` `c_consumer_drives_the_real_table`) loads the libraries named in `M6_VALUES_CONSUMERS` and calls
//! `m6_values_consumer_run` on a fresh thread with the table kiln-eml published and live archive views of generations
//! authorized over its synthetic m6 registry. This library is MIT and reaches the provider only through those C calls
//! (no GPL code is linked into it); it returns the number of failed checks and logs each failure.
//!
//! What it runs:
//! - arena refusals for a released view, a view whose generation keeps another registry and one that keeps none;
//! - the typed-value differential: `tests/fixtures/values/{typed,scalar}_values.lua` over owned roots and over mapped
//!   roots (the owned roots' own exports), compared line by line with the same scripts' output over kiln-eml's
//!   wrappers (ShroudForge's port), recorded in `*.{owned,mapped}.txt` (addresses and Rust TypeIds normalized);
//! - the texts only the frontend can render (Source's TypeId label and `guid expected, got ..` type name), exactly;
//! - reader request counts (user metatables read once per field, Source's LENGTH count), one mutation per edit;
//! - a lease closed from another thread: the provider's wake with the control token, the worker's collect;
//! - an arena closed from another thread while values are live, then the worker's close and drain;
//! - a removed child's lease and every retained token released by the end, and the drain before this thread returns.

use kiln_abi::services::kiln_eml_values::*;
use kiln_abi::*;
use kiln_sfi::lifecycle::{Watchdog, process_clock};
use kiln_sfi::runtime::ServiceAdapter;
use kiln_sfi::runtime::values::proxies::{self, MappedArrayValue, StructValue};
use kiln_sfi::runtime::values::{ArenaOptions, Values, ValuesArena, ValuesError, ValuesTables};
use kiln_sfi::worker::Host;
use mlua::{Lua, Table, UserData, Value};
use sfi_core::host::{HostContext, Package, PackageSource, Phase, Side, SourceVersion};
use sfi_core::lua::{LuaVm, VmAttachments};
use std::any::TypeId;
use std::ffi::c_void;
use std::fmt::Write as _;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicPtr, AtomicUsize, Ordering};

const ROOT_CHUNK: &str = include_str!("../tests/fixtures/values/root.lua");
const SCRIPT: &str = include_str!("../tests/fixtures/values/typed_values.lua");
const GOLDEN_OWNED: &str = include_str!("../tests/fixtures/values/typed_values.owned.txt");
const GOLDEN_MAPPED: &str = include_str!("../tests/fixtures/values/typed_values.mapped.txt");
const SCALARS_CHUNK: &str = include_str!("../tests/fixtures/values/scalars.lua");
const SCALAR_SCRIPT: &str = include_str!("../tests/fixtures/values/scalar_values.lua");
const SCALAR_OWNED: &str = include_str!("../tests/fixtures/values/scalar_values.owned.txt");
const SCALAR_MAPPED: &str = include_str!("../tests/fixtures/values/scalar_values.mapped.txt");

const PERR: &str = r#"
function perr(f, ...)
  local ok, e = pcall(f, ...)
  assert(not ok, "expected an error")
  return (tostring(e):match("^[^\n]*"))
end
"#;

/// The C consumer's argument record (kiln-eml's tests/c/m6_values_consumer.c `M6ConsumerArgs`).
#[repr(C)]
pub struct ConsumerArgs {
    size: u32,
    reserved: u32,
    api: *const KilnEmlValuesApiV1,
    view: KilnEmlArchiveView,
    closed_view: KilnEmlArchiveView,
    mismatch_view: KilnEmlArchiveView,
    bare_view: KilnEmlArchiveView,
    owner: KilnOwner,
    log: *mut u8,
    log_capacity: usize,
    checks: u32,
    failures: u32,
}

static TABLE: AtomicPtr<KilnEmlValuesApiV1> = AtomicPtr::new(std::ptr::null_mut());

unsafe extern "C" fn service(name: KilnStr, _min: u32) -> *const c_void {
    // SAFETY: the adapter passes a name valid for the call.
    let name = unsafe { std::slice::from_raw_parts(name.ptr.cast::<u8>(), name.len) };
    if name == b"kiln.eml.values" {
        return TABLE.load(Ordering::SeqCst).cast_const().cast();
    }
    std::ptr::null()
}

struct Checks {
    checks: u32,
    failures: u32,
    log: String,
}

impl Checks {
    fn check(&mut self, ok: bool, what: impl std::fmt::Display) {
        self.checks += 1;
        if !ok {
            self.failures += 1;
            let _ = writeln!(self.log, "FAIL {what}");
        }
    }
    fn equal<T: PartialEq + std::fmt::Debug>(&mut self, got: T, want: T, what: &str) {
        let ok = got == want;
        self.check(ok, format_args!("{what}: got {got:?}, want {want:?}"));
    }
}

struct NoFiles;
impl PackageSource for NoFiles {
    fn read_file(&self, path: &str) -> std::io::Result<Vec<u8>> {
        Err(std::io::Error::new(
            std::io::ErrorKind::NotFound,
            path.to_owned(),
        ))
    }
}

/// A userdata neither side knows.
struct Foreign;
impl UserData for Foreign {}

/// Addresses and Rust TypeIds differ per process and build: `0x<hex>` becomes `0x..`.
fn normalize(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let bytes = text.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'0'
            && bytes.get(i + 1) == Some(&b'x')
            && bytes.get(i + 2).is_some_and(u8::is_ascii_hexdigit)
        {
            out.push_str("0x..");
            i += 2;
            while bytes.get(i).is_some_and(u8::is_ascii_hexdigit) {
                i += 1;
            }
        } else {
            let ch = text[i..].chars().next().unwrap();
            out.push(ch);
            i += ch.len_utf8();
        }
    }
    out
}

struct Vm {
    vm: LuaVm,
    env: Table,
}

impl Vm {
    fn new(name: &str) -> Self {
        let vm = LuaVm::new(HostContext::new(
            Phase::Startup,
            Side::Client,
            SourceVersion::Sf179,
        ))
        .unwrap();
        vm.attach(VmAttachments {
            typed_tables: Some(Rc::new(ValuesTables)),
            buffer_resources: None,
        })
        .unwrap();
        let env = vm
            .add_package(Package::new(name, Rc::new(NoFiles)), |_, _, _| Ok(()))
            .unwrap();
        Self { vm, env }
    }
    fn lua(&self) -> &Lua {
        self.vm.lua()
    }
    fn run<R: mlua::FromLuaMulti>(&self, chunk: &str) -> mlua::Result<R> {
        self.lua()
            .load(chunk)
            .set_name("=diff")
            .set_environment(self.env.clone())
            .eval()
    }
    fn collect(&self) {
        let _ = self.lua().gc_collect();
        let _ = self.lua().gc_collect();
    }
}

fn lease(value: &Value) -> u64 {
    match value {
        Value::UserData(userdata) => proxies::lease_of(userdata).unwrap_or(0),
        _ => 0,
    }
}

/// Compares a script's output with its kiln-eml transcript, line by line.
fn differential(c: &mut Checks, label: &str, got: &str, golden: &str) {
    let got = normalize(got);
    let golden: Vec<&str> = golden.lines().collect();
    let lines: Vec<&str> = got.lines().collect();
    c.equal(lines.len(), golden.len(), &format!("{label}: line count"));
    for (i, (a, b)) in lines.iter().zip(golden.iter()).enumerate() {
        c.check(
            a == b,
            format_args!("{label} line {}: got {a:?}, kiln-eml {b:?}", i + 1),
        );
    }
}

fn run(args: &mut ConsumerArgs, c: &mut Checks) {
    TABLE.store(args.api.cast_mut(), Ordering::SeqCst);
    // SAFETY: a zeroed KilnApi with its size and the lookup is a valid (older-core-shaped) table.
    let mut api: Box<KilnApi> = Box::new(unsafe { std::mem::zeroed() });
    api.size = std::mem::size_of::<KilnApi>() as u32;
    api.service = Some(service);
    // SAFETY: `api` is readable for its size.
    let host = unsafe { Host::read(&*api) };
    let services = Rc::new(ServiceAdapter::new(
        host,
        Rc::new(Watchdog::new(process_clock())),
    ));
    let owner = args.owner;

    // ---- arena refusals ----
    for (view, code, what) in [
        (args.closed_view, KILN_ERR_REVOKED, "a released view"),
        (args.mismatch_view, KILN_ERR_UNAVAILABLE, "another registry"),
        (args.bare_view, KILN_ERR_UNAVAILABLE, "no registry kept"),
    ] {
        let lua = Lua::new();
        match ValuesArena::create(&services, &lua, owner, view, ArenaOptions::default()) {
            Err(ValuesError::Refused { member, code: got }) => {
                c.equal((member, got), ("arena_create", code), what)
            }
            Err(other) => c.check(false, format_args!("{what}: {other}")),
            Ok(_) => c.check(false, format_args!("{what}: an arena was created")),
        }
    }

    // ---- the main VM and its arena ----
    let vm = Vm::new("m02");
    let woken = Arc::new(AtomicUsize::new(0));
    let w = woken.clone();
    let arena = match ValuesArena::create(
        &services,
        vm.lua(),
        owner,
        args.view,
        ArenaOptions {
            phase: KILN_VALUE_RUNTIME as u32,
            notify: Some(Arc::new(move || {
                w.fetch_add(1, Ordering::SeqCst);
            })),
            ..ArenaOptions::default()
        },
    ) {
        Ok(arena) => arena,
        Err(error) => {
            c.check(false, format_args!("arena_create: {error}"));
            return;
        }
    };
    c.check(true, "arena_create over the live view");
    let values = arena.values();
    vm.run::<()>(PERR).unwrap();
    vm.env
        .set("FOREIGN", vm.lua().create_userdata(Foreign).unwrap())
        .unwrap();
    let root_value = || -> Value { vm.lua().load(ROOT_CHUNK).set_name("=root").eval().unwrap() };

    // ---- the differential: each script over fresh owned roots, then fresh mapped roots ----
    let roots = |c: &mut Checks, mapped: bool| -> Option<(Value, Value)> {
        let mut out = Vec::new();
        for (chunk, ty) in [(ROOT_CHUNK, "m6.Root"), (SCALARS_CHUNK, "m6.Scalars")] {
            let input: Value = vm.lua().load(chunk).set_name("=root").eval().unwrap();
            let owned = match values.open_lua(vm.lua(), ty, input) {
                Ok(owned) => owned,
                Err(error) => {
                    c.check(false, format_args!("open LUA_INPUT {ty}: {error}"));
                    return None;
                }
            };
            if !mapped {
                out.push(owned);
                continue;
            }
            let bytes = values.export(lease(&owned), "").unwrap_or_default();
            c.check(
                !bytes.is_empty(),
                format_args!("{ty} exports its resource bytes"),
            );
            match values.open_bytes(vm.lua(), ty, &bytes) {
                Ok(root) => {
                    c.check(
                        matches!(&root, Value::UserData(u) if u.type_name().ok().flatten().as_deref() == Some("MappedStructValue")),
                        format_args!("a BYTES {ty} is a MappedStructValue"),
                    );
                    out.push(root);
                }
                Err(error) => {
                    c.check(false, format_args!("open BYTES {ty}: {error}"));
                    return None;
                }
            }
        }
        let scalars = out.pop()?;
        Some((out.pop()?, scalars))
    };
    for (script, label, owned_golden, mapped_golden) in [
        (SCRIPT, "typed_values", GOLDEN_OWNED, GOLDEN_MAPPED),
        (SCALAR_SCRIPT, "scalar_values", SCALAR_OWNED, SCALAR_MAPPED),
    ] {
        for (mapped, golden) in [(false, owned_golden), (true, mapped_golden)] {
            let Some((root, scalars)) = roots(c, mapped) else {
                continue;
            };
            vm.env.set("ROOT", root).unwrap();
            vm.env.set("SCALARS", scalars).unwrap();
            let storage = if mapped { "mapped" } else { "owned" };
            match vm.run::<String>(script) {
                Ok(out) => differential(c, &format!("{label}.{storage}"), &out, golden),
                Err(error) => c.check(false, format_args!("{label}.{storage}: {error}")),
            }
            vm.env.set("ROOT", Value::Nil).unwrap();
            vm.env.set("SCALARS", Value::Nil).unwrap();
        }
    }
    let bytes = {
        let input: Value = root_value();
        let owned = values.open_lua(vm.lua(), "m6.Root", input).unwrap();
        values.export(lease(&owned), "").unwrap_or_default()
    };

    // ---- the frontend's own texts, exactly ----
    let fresh = values.open_lua(vm.lua(), "m6.Root", root_value()).unwrap();
    vm.env.set("R", fresh).unwrap();
    let label: String = vm
        .run("return perr(function() R.children = R.child end)")
        .unwrap();
    c.equal(
        label,
        format!(
            "at .: expected `m6.Children`, got `userdata `{:?}``",
            TypeId::of::<StructValue>()
        ),
        "TypeId label of a struct",
    );
    let mapped_children = values.open_bytes(vm.lua(), "m6.Root", &bytes).unwrap();
    vm.env.set("M", mapped_children).unwrap();
    let label: String = vm
        .run("return perr(function() R.child = M.children end)")
        .unwrap();
    c.equal(
        label,
        format!(
            "at .: expected `m6.Child`, got `userdata `{:?}``",
            TypeId::of::<MappedArrayValue>()
        ),
        "TypeId label of a mapped array",
    );
    let scalars_input: Value = vm.lua().load(SCALARS_CHUNK).eval().unwrap();
    match values.open_lua(vm.lua(), "m6.Scalars", scalars_input) {
        Ok(scalars) => {
            vm.env.set("S", scalars).unwrap();
            let text: String = vm
                .run("return perr(function() S.guid = function() end end)")
                .unwrap();
            c.equal(
                text.as_str(),
                "at .: guid expected, got function",
                "guid type name of a function",
            );
            let child: Value = vm.run("return R.child").unwrap();
            let text: String = vm
                .run("return perr(function() S.guid = R.child end)")
                .unwrap();
            c.equal(
                text,
                format!("at .: guid expected, got {child:#?}"),
                "guid type name of a userdata",
            );
            let text: String = vm
                .run("return perr(function() S.guid = FOREIGN end)")
                .unwrap();
            c.check(
                text.starts_with("at .: guid expected, got "),
                format_args!("guid of a foreign userdata: {text}"),
            );
            let ok: bool = vm
                .run(r#"S.guid = "00000000-0000-0000-0000-000000000001" return S.guid == "00000000-0000-0000-0000-000000000001""#)
                .unwrap_or(false);
            c.check(ok, "a GUID string is accepted");
        }
        Err(error) => c.check(false, format_args!("open LUA_INPUT m6.Scalars: {error}")),
    }

    // ---- reader requests: user metatables once per field, Source's LENGTH count; one mutation per edit ----
    let before = values.reader_counters();
    let reads: String = vm
        .run(
            r#"local reads = {}
            R.child = setmetatable({}, {__index = function(_, k) reads[#reads + 1] = k return 5 end})
            return table.concat(reads, ",")"#,
        )
        .unwrap();
    let after = values.reader_counters();
    c.equal(
        reads.as_str(),
        "base,extra",
        "a metatable field is read once, in the type's order",
    );
    c.equal(
        after.by_request[KILN_LUA_GET_FIELD_VALUE as usize]
            - before.by_request[KILN_LUA_GET_FIELD_VALUE as usize],
        2,
        "two GET_FIELD_VALUE requests",
    );
    let before = values.reader_counters();
    vm.run::<()>("R.fixed = setmetatable({1, 2}, {__len = function() return 2 end})")
        .unwrap();
    let after = values.reader_counters();
    c.equal(
        after.by_request[KILN_LUA_LENGTH as usize] - before.by_request[KILN_LUA_LENGTH as usize],
        2,
        "a static array's length is asked twice",
    );
    vm.run::<()>("R.child.base = 1").unwrap();
    let (mutated, first) = values.last_mutation();
    vm.run::<()>("R.child.base = 2").unwrap();
    let (again, second) = values.last_mutation();
    c.check(
        mutated && again && second == first + 1,
        format_args!("one revision per edit ({first} then {second})"),
    );
    let length: i64 = vm
        .run("for i = 1, 40 do table.insert(R.children, {base = i, extra = i}) end return #tostring(R.children)")
        .unwrap_or(0);
    c.check(
        length > 256,
        format_args!("a packet larger than the probe is copied whole ({length} bytes)"),
    );

    // ---- a creator package's value, first read by another package ----
    let shared = vm.lua().create_table().unwrap();
    vm.env.set("SHARED", shared.clone()).unwrap();
    let other = vm
        .vm
        .add_package(Package::new("m02-b", Rc::new(NoFiles)), |_, _, _| Ok(()))
        .unwrap();
    other.set("SHARED", shared).unwrap();
    vm.run::<()>("SHARED.v = R.maybe").unwrap();
    vm.env.set("R", Value::Nil).unwrap();
    vm.collect();
    let base: mlua::Result<i64> = vm
        .lua()
        .load("return SHARED.v.base")
        .set_environment(other.clone())
        .eval();
    c.equal(
        base.ok(),
        Some(13),
        "another package's first access to a value it was handed",
    );

    // ---- a lease closed from another thread: wake with the control, collect on the worker ----
    let tmp: Value = vm
        .lua()
        .load("return SHARED.v")
        .set_environment(other.clone())
        .eval()
        .unwrap();
    let tmp_lease = lease(&tmp);
    let api_table = args.api as usize;
    let closed = std::thread::spawn(move || {
        // SAFETY: the provider's value_close is any-thread.
        unsafe {
            (*(api_table as *const KilnEmlValuesApiV1))
                .value_close
                .unwrap()(owner, tmp_lease)
        }
    })
    .join()
    .unwrap();
    c.equal(closed, KILN_OK, "value_close from another thread");
    c.equal(
        woken.load(Ordering::SeqCst),
        1,
        "the provider woke the host once",
    );
    c.equal(
        values.signal().last_control(),
        values.control(),
        "wake carried the control token",
    );
    // pending counts the queued closes and the host objects the provider retains: after the collect only the latter,
    // which must be exactly the retains this frontend still holds for it
    let pending = arena.collect_if_woken();
    c.equal(
        pending.map(u64::from),
        Some(values.outstanding_retains()),
        "the worker collected; pending = live retains",
    );
    vm.env.set("TMP", tmp).unwrap();
    let text: String = vm
        .run("return perr(function() return TMP.base end)")
        .unwrap();
    c.equal(
        text.as_str(),
        "runtime error: kiln: kiln.eml.values.value_child refused (KilnResult 6)",
        "a revoked lease is Kiln's error",
    );
    vm.env.set("TMP", Value::Nil).unwrap();
    other.set("SHARED", Value::Nil).unwrap();
    vm.env.set("SHARED", Value::Nil).unwrap();
    vm.env.set("M", Value::Nil).unwrap();
    vm.env.set("S", Value::Nil).unwrap();

    // ---- an arena closed from another thread while its values live, then the worker's drain ----
    {
        let second = Vm::new("m02-c");
        match ValuesArena::create(
            &services,
            second.lua(),
            owner,
            args.view,
            ArenaOptions::default(),
        ) {
            Ok(b) => {
                let root = b
                    .open_lua(
                        second.lua(),
                        "m6.Root",
                        second.lua().load(ROOT_CHUNK).eval().unwrap(),
                    )
                    .unwrap();
                second.env.set("B", root).unwrap();
                second
                    .run::<i64>("return B.child.base + #B.children")
                    .unwrap();
                let control = b.control();
                let code = std::thread::spawn(move || {
                    let mut pending = 0u32;
                    // SAFETY: arena_close is any-thread.
                    unsafe {
                        (*(api_table as *const KilnEmlValuesApiV1))
                            .arena_close
                            .unwrap()(owner, control, &mut pending)
                    }
                })
                .join()
                .unwrap();
                c.equal(code, KILN_OK, "arena_close from another thread");
                let text: String = second
                    .run(r#"local ok, e = pcall(function() return B.child end) return (tostring(e):match("^[^\n]*"))"#)
                    .unwrap();
                c.check(
                    text.contains("refused (KilnResult 6)"),
                    format_args!("a closed arena refuses: {text}"),
                );
                second.env.set("B", Value::Nil).unwrap();
                second.collect();
                c.check(
                    b.close(),
                    "the worker's close drained the arena closed elsewhere",
                );
                c.equal(b.live_tokens().1, 0, "nothing retained after that drain");
            }
            Err(error) => c.check(false, format_args!("second arena: {error}")),
        }
    }

    // ---- the end: every lease and retained token released, the drain before this thread returns ----
    vm.collect();
    c.equal(
        values.collect().ok(),
        Some(0),
        "nothing pending after the collections",
    );
    c.equal(
        values.live_tokens().1,
        0,
        "no token retained once every value is gone",
    );
    c.check(arena.close(), "close and drain on the worker");
    let counters = values.reader_counters();
    c.equal(
        counters.retains,
        counters.releases,
        "every retain released once",
    );
    c.equal(
        (counters.stray_releases, counters.panics),
        (0, 0),
        "no stray release or panic",
    );
    c.check(
        counters.adopted > 0,
        format_args!(
            "proxies adopted ({}), reused ({})",
            counters.adopted, counters.reused
        ),
    );
    let _keep: &Values = &values;
    drop(arena);
    drop(vm);
    drop(api);
}

/// Runs the checks; returns the number of failures (each one a line in `args.log`).
///
/// # Safety
/// `args` points to a live `M6ConsumerArgs` whose `api` is kiln-eml's kiln.eml.values table and whose `log` is writable
/// for `log_capacity` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn m6_values_consumer_run(args: *mut ConsumerArgs) -> i32 {
    // SAFETY: the caller's contract.
    let args = unsafe { &mut *args };
    if (args.size as usize) < std::mem::size_of::<ConsumerArgs>() || args.api.is_null() {
        return 1;
    }
    let mut checks = Checks {
        checks: 0,
        failures: 0,
        log: String::new(),
    };
    if let Err(panic) = catch_unwind(AssertUnwindSafe(|| run(args, &mut checks))) {
        let text = panic
            .downcast_ref::<&str>()
            .map(|s| s.to_string())
            .or_else(|| panic.downcast_ref::<String>().cloned())
            .unwrap_or_default();
        checks.check(false, format_args!("panic: {text}"));
    }
    let _ = writeln!(
        checks.log,
        "values_consumer: {} checks, {} failures",
        checks.checks, checks.failures
    );
    args.checks += checks.checks;
    args.failures += checks.failures;
    if !args.log.is_null() && args.log_capacity > 0 {
        let bytes = checks.log.as_bytes();
        let n = bytes.len().min(args.log_capacity - 1);
        // SAFETY: the log is writable for log_capacity bytes.
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), args.log, n);
            args.log.add(n).write(0);
        }
    }
    checks.failures as i32
}
