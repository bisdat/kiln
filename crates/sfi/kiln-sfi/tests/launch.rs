// SPDX-License-Identifier: MIT
//! kiln-sfi against the core's frozen ShroudForge backend (kiln.launch): the interpreter starts
//! only for `kiln`, at the plugin step's on_ready; every other record, and no record at all, starts nothing.
use kiln_abi::services::kiln_caps::*;
use kiln_abi::services::kiln_launch::*;
use kiln_abi::*;
use kiln_sfi::native_manager::selection::{NativePermission, NativePolicy};
use kiln_sfi::{Boundary, Launch, Phase};
use std::cell::{Cell, RefCell};

#[derive(Clone, Copy)]
enum Table {
    None,
    Record { backend: u32, code: u32 },
    Short,
    Version0,
    NoStatus,
    Unfrozen,
    SmallRecord,
    Misaligned,
}

thread_local! {
    static TABLE: Cell<Table> = const { Cell::new(Table::None) };
    static REPORTS: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
    static LOGS: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
    static SIZE_SEEN: Cell<u32> = const { Cell::new(0) };
    // The record's native_policy, how much of the record the fake core fills (200, or 192 for a core built
    // before native_policy), build_info's verified (None: the call fails), and the build_info calls seen
    static POLICY: Cell<u32> = const { Cell::new(0) };
    static FILLS: Cell<u32> = const { Cell::new(200) };
    static VERIFIED: Cell<Option<bool>> = const { Cell::new(Some(false)) };
    static BUILD_INFO_CALLS: Cell<u32> = const { Cell::new(0) };
    // The record's build_verified
    static BUILD_VERIFIED: Cell<u32> = const { Cell::new(0) };
    static WARNINGS: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
}

fn text(s: KilnStr) -> String {
    String::from_utf8_lossy(unsafe { std::slice::from_raw_parts(s.ptr.cast::<u8>(), s.len) })
        .into_owned()
}
unsafe extern "C" fn log(_: u64, level: i32, s: KilnStr) {
    if level == KILN_LOG_WARN {
        WARNINGS.with_borrow_mut(|l| l.push(text(s)));
    }
    LOGS.with_borrow_mut(|l| l.push(text(s)));
}
unsafe extern "C" fn build_info(out: *mut KilnBuildInfo) -> KilnResult {
    BUILD_INFO_CALLS.set(BUILD_INFO_CALLS.get() + 1);
    let out = unsafe { &mut *out };
    if (out.size as usize) < std::mem::size_of::<KilnBuildInfo>() {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    match VERIFIED.get() {
        Some(verified) => {
            out.verified = u8::from(verified);
            KILN_OK
        }
        None => KILN_ERR_UNAVAILABLE,
    }
}
unsafe extern "C" fn report(_: u64, name: KilnStr, state: u32, reason: u32, words: KilnStr) -> i32 {
    assert_eq!(text(name), "kiln-sfi.interpreter");
    assert_eq!(
        (state, reason),
        (KILN_CAP_UNAVAILABLE as u32, KILN_CAP_REASON_POLICY as u32)
    );
    REPORTS.with_borrow_mut(|r| r.push(text(words)));
    KILN_OK
}
static CAPS: KilnCapsApi = KilnCapsApi {
    header: KilnServiceHeader {
        size: std::mem::size_of::<KilnCapsApi>() as u32,
        version: 1,
    },
    count: None,
    at: None,
    find: None,
    report: Some(report),
    subscribe: None,
    unsubscribe: None,
    generation: None,
};

const REASON: &str = "the reason the core froze";
unsafe extern "C" fn status(out: *mut KilnLaunchStatus) -> KilnResult {
    let out = unsafe { &mut *out };
    SIZE_SEEN.set(out.size);
    if out.size < std::mem::size_of::<KilnLaunchStatus>() as u32 {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    let (backend, code) = match TABLE.get() {
        Table::Record { backend, code } => (backend, code),
        Table::Unfrozen => {
            out.frozen = 0;
            return KILN_ERR_UNAVAILABLE;
        }
        _ => (
            KILN_LAUNCH_BACKEND_KILN as u32,
            KILN_LAUNCH_REASON_PLAN as u32,
        ),
    };
    out.backend = backend;
    out.frozen = 1;
    out.reason_code = code;
    out.launch_id = 0x0123_4567_89ab_cdef;
    out.reason = [0; 128];
    for (i, b) in REASON.bytes().enumerate() {
        out.reason[i] = b as core::ffi::c_char;
    }
    // a core that fills less than the v1 record says so in size; one built before native_policy fills 192
    out.size = if matches!(TABLE.get(), Table::SmallRecord) {
        64
    } else {
        FILLS.get()
    };
    if FILLS.get() as usize >= std::mem::size_of::<KilnLaunchStatus>() {
        out.native_policy = POLICY.get();
        out.build_verified = BUILD_VERIFIED.get();
    }
    KILN_OK
}
#[repr(C, align(16))]
struct Aligned([u8; 64]);
static MISALIGNED: Aligned = Aligned([0; 64]);
thread_local! {
    // per test thread: the table service() hands out (a stable address for the thread's life)
    static LAUNCH: Cell<KilnLaunchApi> = const {
        Cell::new(KilnLaunchApi { header: KilnServiceHeader { size: 24, version: 1 }, status: None, notice_copy: None })
    };
}

unsafe extern "C" fn service(name: KilnStr, _: u32) -> *const core::ffi::c_void {
    match text(name).as_str() {
        "kiln.caps" => (&CAPS as *const KilnCapsApi).cast(),
        "kiln.launch" => {
            let mut table = KilnLaunchApi {
                header: KilnServiceHeader {
                    size: std::mem::size_of::<KilnLaunchApi>() as u32,
                    version: 1,
                },
                status: Some(status),
                notice_copy: None,
            };
            match TABLE.get() {
                Table::None => return std::ptr::null(),
                Table::Misaligned => {
                    return unsafe { (&raw const MISALIGNED).cast::<u8>().add(1).cast() };
                }
                Table::Short => table.header.size = 8,
                Table::Version0 => table.header.version = 0,
                Table::NoStatus => table.status = None,
                _ => {}
            }
            LAUNCH.with(|c| {
                c.set(table);
                c.as_ptr().cast_const().cast()
            })
        }
        _ => std::ptr::null(),
    }
}

fn api() -> Box<KilnApi> {
    let mut api: Box<KilnApi> = Box::new(unsafe { std::mem::zeroed() });
    api.size = std::mem::size_of::<KilnApi>() as u32;
    api.abi_version = 1;
    api.log = Some(log);
    api.service = Some(service);
    api.build_info = Some(build_info);
    api
}
fn info() -> KilnPluginInfo {
    let mut info: KilnPluginInfo = unsafe { std::mem::zeroed() };
    info.size = std::mem::size_of::<KilnPluginInfo>() as u32;
    info
}

struct Start {
    starts: u32,
    early_ready: bool,
    reports: Vec<String>,
    logs: Vec<String>,
    warnings: Vec<String>,
    native: Option<NativePermission>,
    early_build_info_calls: u32,
}

/// A real start's order for kiln-sfi: the early phase (owner 11), then Kiln's plugin step (owner 12) and its on_ready.
fn boot(table: Table) -> Start {
    TABLE.set(table);
    REPORTS.with_borrow_mut(Vec::clear);
    LOGS.with_borrow_mut(Vec::clear);
    WARNINGS.with_borrow_mut(Vec::clear);
    BUILD_INFO_CALLS.set(0);
    let boundary: &'static Boundary = Box::leak(Box::new(Boundary::new()));
    let api = api();
    let (mut early, mut normal) = (info(), info());
    assert_eq!(
        unsafe { boundary.enter(Phase::Early, &*api, 11, &mut early) },
        KILN_OK
    );
    assert!(
        early.on_ready.is_none(),
        "nothing starts in the early phase"
    );
    let early_build_info_calls = BUILD_INFO_CALLS.get();
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*api, 12, &mut normal) },
        KILN_OK
    );
    // a start is Kiln's plugin step's on_ready (this boundary has no worker, so it refuses there)
    let starts = u32::from(normal.on_ready.is_some());
    if let Some(ready) = normal.on_ready {
        unsafe { ready(normal.user) };
    }
    assert!(!boundary.worker_started(), "this boundary has no worker");
    Start {
        starts,
        early_ready: early.on_ready.is_some(),
        reports: REPORTS.with_borrow(Clone::clone),
        logs: LOGS.with_borrow(Clone::clone),
        warnings: WARNINGS.with_borrow(Clone::clone),
        native: boundary.native_permission(),
        early_build_info_calls,
    }
}

fn record(backend: i32, code: i32) -> Table {
    Table::Record {
        backend: backend as u32,
        code: code as u32,
    }
}

#[test]
fn the_interpreter_starts_only_for_kiln() {
    let rows = [
        (
            record(
                KILN_LAUNCH_BACKEND_HOSTED,
                KILN_LAUNCH_REASON_HOSTED_DEFAULT,
            ),
            0,
            "backend hosted: kiln-shroudforge has this start",
        ),
        (
            record(KILN_LAUNCH_BACKEND_HOSTED, KILN_LAUNCH_REASON_ENV_RUNTIME),
            0,
            "backend hosted: kiln-shroudforge has this start",
        ),
        (
            record(
                KILN_LAUNCH_BACKEND_INSTALLED,
                KILN_LAUNCH_REASON_USE_INSTALLED,
            ),
            0,
            "backend installed: kiln-shroudforge has this start",
        ),
        (
            record(KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN),
            1,
            "Kiln's interpreter has this start",
        ),
        (
            record(
                KILN_LAUNCH_BACKEND_NONE,
                KILN_LAUNCH_REASON_INTERPRETER_HELD,
            ),
            0,
            "every ShroudForge backend stands aside this start",
        ),
        (
            record(KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SETTING_INVALID),
            0,
            "every ShroudForge backend stands aside this start",
        ),
        (
            record(KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SF18_VETO),
            0,
            "every ShroudForge backend stands aside this start",
        ),
        (
            record(KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SAFE_IDLE),
            0,
            "the core decided no ShroudForge backend for this start",
        ),
        (
            record(KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_PROFILE_INVALID),
            0,
            "the core decided no ShroudForge backend for this start",
        ),
        // KILN_LAUNCH_REASON_UNDECIDED (0x204): the idle class too
        (
            record(KILN_LAUNCH_BACKEND_NONE, 0x204),
            0,
            "the core decided no ShroudForge backend for this start",
        ),
    ];
    for (table, starts, said) in rows {
        let s = boot(table);
        assert_eq!(s.starts, starts, "{said}");
        assert!(
            !s.early_ready,
            "{said}: the early phase installs no on_ready"
        );
        assert_eq!(s.reports.len(), 2, "{said}: one capability row per phase");
        for r in &s.reports {
            assert!(r.starts_with(said) && r.contains(REASON), "{said}: {r}");
        }
        // the stub start is gone: a boundary without a worker refuses at on_ready
        assert!(
            !s.logs
                .iter()
                .any(|l| l.contains("interpreter start (stub)")),
            "{:?}",
            s.logs
        );
    }
}

#[test]
fn no_table_or_an_unreadable_one_keeps_todays_rule() {
    for table in [
        Table::None,
        Table::Short,
        Table::Version0,
        Table::NoStatus,
        Table::Unfrozen,
        Table::SmallRecord,
        Table::Misaligned,
        record(7, KILN_LAUNCH_REASON_PLAN),
        // a NONE record of neither class is not a decision either
        record(KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_HOSTED_DEFAULT),
        record(KILN_LAUNCH_BACKEND_NONE, 0x300),
    ] {
        let s = boot(table);
        assert_eq!(s.starts, 0);
        assert_eq!(s.reports, vec!["backend negotiation not integrated"; 2]);
    }
}

#[test]
fn the_record_is_asked_for_with_its_whole_size() {
    SIZE_SEEN.set(0);
    boot(record(KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN));
    assert_eq!(SIZE_SEEN.get(), 200);
    assert_eq!(std::mem::size_of::<KilnLaunchStatus>(), 200);
    assert_eq!(
        std::mem::offset_of!(KilnLaunchStatus, native_policy),
        KILN_LAUNCH_STATUS_V1_SIZE as usize
    );
}

#[test]
fn the_records_build_verified_is_read_as_the_core_froze_it() {
    // build_verified: 0 and 1 as they are; any other value is no answer (it counts as not verified)
    TABLE.set(record(KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN));
    for (value, read) in [
        (0, Some(false)),
        (1, Some(true)),
        (2, None),
        (u32::MAX, None),
    ] {
        BUILD_VERIFIED.set(value);
        let record = unsafe { kiln_sfi::read_launch_record(service) };
        assert_eq!(record.build_verified, read, "build_verified {value}");
        assert!(matches!(record.launch, Launch::Kiln { .. }));
    }
    // a core that fills only the first 192 bytes reported neither native_policy nor build_verified
    BUILD_VERIFIED.set(1);
    FILLS.set(192);
    let record = unsafe { kiln_sfi::read_launch_record(service) };
    FILLS.set(200);
    BUILD_VERIFIED.set(0);
    assert_eq!(
        (record.launch, record.build_verified),
        (Launch::Absent, None)
    );
    assert_eq!(
        std::mem::offset_of!(KilnLaunchStatus, build_verified),
        KILN_LAUNCH_STATUS_V1_SIZE as usize + 4
    );
    // unknown counts as not verified
    assert_eq!(
        kiln_sfi::native_permission(NativePolicy::VerifiedOnly, None).refusal(),
        kiln_sfi::native_permission(NativePolicy::VerifiedOnly, Some(false)).refusal()
    );
    assert!(
        kiln_sfi::native_permission(NativePolicy::VerifiedOnly, Some(true))
            .refusal()
            .is_none()
    );
}

#[test]
fn launch_read_classifies_every_record() {
    let read = |table| {
        TABLE.set(table);
        unsafe { kiln_sfi::read_launch(service) }
    };
    assert_eq!(read(Table::None), Launch::Absent);
    assert_eq!(
        read(record(KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN)),
        Launch::Kiln {
            code: 5,
            reason: REASON.into(),
            native_policy: NativePolicy::Always,
        }
    );
    assert_eq!(
        read(record(
            KILN_LAUNCH_BACKEND_INSTALLED,
            KILN_LAUNCH_REASON_SETTING
        )),
        Launch::Shroudforge {
            installed: true,
            code: 2,
            reason: REASON.into()
        }
    );
    assert_eq!(
        read(record(
            KILN_LAUNCH_BACKEND_HOSTED,
            KILN_LAUNCH_REASON_HOSTED_DEFAULT
        )),
        Launch::Shroudforge {
            installed: false,
            code: 1,
            reason: REASON.into()
        }
    );
    assert_eq!(
        read(record(KILN_LAUNCH_BACKEND_NONE, 0x1ff)),
        Launch::Refused {
            code: 0x1ff,
            reason: REASON.into()
        }
    );
    assert_eq!(
        read(record(KILN_LAUNCH_BACKEND_NONE, 0x2ff)),
        Launch::Idle {
            code: 0x2ff,
            reason: REASON.into()
        }
    );
    assert_eq!(
        read(record(KILN_LAUNCH_BACKEND_NONE, 0x0ff)),
        Launch::Absent
    );
    assert_eq!(read(record(4, KILN_LAUNCH_REASON_PLAN)), Launch::Absent);
}

#[test]
fn the_stub_ready_with_no_boundary_does_nothing() {
    let s = boot(record(KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN));
    assert_eq!(s.starts, 1);
    // the core never calls on_ready with another user; a null one is ignored, not a crash
    let boundary: &'static Boundary = Box::leak(Box::new(Boundary::new()));
    let api = api();
    let mut normal = info();
    TABLE.set(record(KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN));
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*api, 12, &mut normal) },
        KILN_OK
    );
    unsafe { normal.on_ready.unwrap()(std::ptr::null_mut()) };
    assert_eq!(boundary.status().phase(), None, "nothing began");
}

// ---- kiln.json native.policy in kiln mode ----

const NEVER: &str = r#"kiln.json native.policy is "never""#;
const UNVERIFIED: &str =
    r#"kiln.json native.policy is "verified-only" and this game build is not verified"#;
fn warning(text: &str) -> String {
    format!(
        "kiln-sfi: {text}: no ShroudForge package DLL loads this start; every package that ships one is withdrawn, with every package that depends on it"
    )
}

#[test]
fn launch_read_carries_the_frozen_native_policy_for_kiln() {
    let read = |table, policy, fills| {
        TABLE.set(table);
        POLICY.set(policy);
        FILLS.set(fills);
        unsafe { kiln_sfi::read_launch(service) }
    };
    let kiln = record(KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN);
    for (value, policy) in [
        (KILN_LAUNCH_NATIVE_POLICY_ALWAYS, NativePolicy::Always),
        (
            KILN_LAUNCH_NATIVE_POLICY_VERIFIED_ONLY,
            NativePolicy::VerifiedOnly,
        ),
        (KILN_LAUNCH_NATIVE_POLICY_NEVER, NativePolicy::Never),
    ] {
        assert_eq!(
            read(kiln, value as u32, 200),
            Launch::Kiln {
                code: 5,
                reason: REASON.into(),
                native_policy: policy,
            }
        );
    }
    // a kiln record with a policy this build does not know cannot be read: the rule from before the table
    assert_eq!(read(kiln, 3, 200), Launch::Absent);
    assert_eq!(read(kiln, u32::MAX, 200), Launch::Absent);
    // a core built before native_policy (a 192-byte record) never decided it: no kiln start either
    assert_eq!(read(kiln, 2, 192), Launch::Absent);
    // hosted, installed, refused and idle do not read native_policy, whatever it says
    for value in [2, 3, u32::MAX] {
        assert!(matches!(
            read(
                record(KILN_LAUNCH_BACKEND_HOSTED, KILN_LAUNCH_REASON_SETTING),
                value,
                200
            ),
            Launch::Shroudforge {
                installed: false,
                ..
            }
        ));
        assert!(matches!(
            read(
                record(KILN_LAUNCH_BACKEND_INSTALLED, KILN_LAUNCH_REASON_SETTING),
                value,
                200
            ),
            Launch::Shroudforge {
                installed: true,
                ..
            }
        ));
        assert!(matches!(
            read(record(KILN_LAUNCH_BACKEND_NONE, 0x101), value, 200),
            Launch::Refused { .. }
        ));
        assert!(matches!(
            read(record(KILN_LAUNCH_BACKEND_NONE, 0x200), value, 200),
            Launch::Idle { .. }
        ));
    }
}

#[test]
fn kiln_reads_the_native_permission_at_its_plugin_step_and_warns_when_it_refuses() {
    for (policy, verified, permission, refusal) in [
        (0, Some(false), (NativePolicy::Always, false), None),
        (0, None, (NativePolicy::Always, false), None),
        (1, Some(true), (NativePolicy::VerifiedOnly, true), None),
        (
            1,
            Some(false),
            (NativePolicy::VerifiedOnly, false),
            Some(UNVERIFIED),
        ),
        // the core cannot say: not verified
        (
            1,
            None,
            (NativePolicy::VerifiedOnly, false),
            Some(UNVERIFIED),
        ),
        (2, Some(true), (NativePolicy::Never, true), Some(NEVER)),
        (2, Some(false), (NativePolicy::Never, false), Some(NEVER)),
    ] {
        POLICY.set(policy);
        VERIFIED.set(verified);
        let s = boot(record(KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN));
        let case = format!("policy {policy}, verified {verified:?}");
        // The interpreter still starts: Lua-only packages run; the DLL packages are withdrawn by the native manager.
        assert_eq!(s.starts, 1, "{case}");
        assert_eq!(
            s.native,
            Some(NativePermission {
                policy: permission.0,
                verified: permission.1,
            }),
            "{case}"
        );
        // build_info is asked at Kiln's plugin step only: the core decides verified after the early plugins ran.
        assert_eq!(s.early_build_info_calls, 0, "{case}");
        assert_eq!(BUILD_INFO_CALLS.get(), 1, "{case}");
        assert_eq!(
            s.warnings,
            refusal.map(warning).into_iter().collect::<Vec<_>>(),
            "{case}"
        );
        // The capability rows are the same as under "always".
        for r in &s.reports {
            assert!(
                r.starts_with("Kiln's interpreter has this start"),
                "{case}: {r}"
            );
        }
    }
}

#[test]
fn hosted_installed_refused_and_idle_never_read_or_apply_the_native_policy() {
    for table in [
        record(
            KILN_LAUNCH_BACKEND_HOSTED,
            KILN_LAUNCH_REASON_HOSTED_DEFAULT,
        ),
        record(
            KILN_LAUNCH_BACKEND_INSTALLED,
            KILN_LAUNCH_REASON_USE_INSTALLED,
        ),
        record(
            KILN_LAUNCH_BACKEND_NONE,
            KILN_LAUNCH_REASON_INTERPRETER_HELD,
        ),
        record(KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SAFE_IDLE),
    ] {
        POLICY.set(KILN_LAUNCH_NATIVE_POLICY_ALWAYS as u32);
        VERIFIED.set(Some(true));
        let always = boot(table);
        POLICY.set(KILN_LAUNCH_NATIVE_POLICY_NEVER as u32);
        VERIFIED.set(None);
        let never = boot(table);
        assert_eq!(BUILD_INFO_CALLS.get(), 0);
        assert_eq!(never.native, None);
        assert!(never.warnings.is_empty());
        assert_eq!(
            (never.starts, never.reports, never.logs),
            (always.starts, always.reports, always.logs)
        );
    }
}
