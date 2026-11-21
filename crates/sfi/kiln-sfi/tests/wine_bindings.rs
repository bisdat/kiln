// SPDX-License-Identifier: MIT
//! Executed only by the explicit private-prefix Wine runner, one process per
//! case. Synthetic DLLs only; never game or corpus files.
#![cfg(windows)]
// The boundary cases enter kiln-sfi's early phase through the tests' fake host; the deadline cases also attach the
// fake kiln-eml startup seam.
#[path = "support/fake_archive.rs"]
mod fake_archive;
#[path = "support/fake_startup.rs"]
mod fake_startup;
mod support;
use core::ffi::{c_char, c_void};
use kiln_sfi::{
    native_contracts::NativeContracts,
    native_enforcement::{Backend, Enforcer, LoadOutcome, Sink, Snapshot, WindowsBackend},
    native_manager::{
        executor::{
            NativeRun, Outcome as NativeOutcome, STARTUP_NATIVE_DEADLINE, Witness, WitnessToken,
        },
        plan::{NativePlan, PlannedDll, PlannedPackage},
        selection::{Mode, NativePermission, NativePolicy},
    },
};
use std::{
    os::windows::ffi::OsStrExt,
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicUsize, Ordering},
    },
};
#[link(name = "kernel32")]
unsafe extern "system" {
    fn LoadLibraryExW(path: *const u16, file: *mut c_void, flags: u32) -> *mut c_void;
    fn FreeLibrary(module: *mut c_void) -> i32;
    fn GetModuleHandleW(name: *const u16) -> *mut c_void;
    fn GetProcAddress(module: *mut c_void, name: *const c_char) -> *mut c_void;
    fn GetLastError() -> u32;
    fn Sleep(milliseconds: u32);
    fn VirtualQuery(address: *const c_void, data: *mut c_void, length: usize) -> usize;
    fn SetEnvironmentVariableW(name: *const u16, value: *const u16) -> i32;
    fn GetEnvironmentVariableW(name: *const u16, value: *mut u16, capacity: u32) -> u32;
    fn MoveFileW(old: *const u16, new: *const u16) -> i32;
    fn CopyFileW(old: *const u16, new: *const u16, fail: i32) -> i32;
    fn VirtualAlloc(address: *mut c_void, size: usize, kind: u32, protect: u32) -> *mut c_void;
}
unsafe extern "C" {
    fn sfi_ring_publish(kind: u32, name: *const u16, length: u32, owner: u64);
    fn sfi_breach_flags() -> u32;
    fn sfi_monitor_current_owner() -> u64;
    fn sfi_mapped_header_matches(
        module: *mut c_void,
        disk: *const u8,
        length: usize,
        matches: *mut u32,
    ) -> u32;
}
fn wide(s: &std::ffi::OsStr) -> Vec<u16> {
    s.encode_wide().chain(Some(0)).collect()
}
fn text(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}
fn loaded(path: &Path) -> usize {
    let p = wide(path.as_os_str());
    let m = unsafe { LoadLibraryExW(p.as_ptr(), std::ptr::null_mut(), 0x1100) };
    assert!(
        !m.is_null(),
        "fixture load failed {}: {}",
        path.display(),
        unsafe { GetLastError() }
    );
    m as usize
}
fn handle(name: &str) -> usize {
    let n = text(name);
    unsafe { GetModuleHandleW(n.as_ptr()) as usize }
}
fn entry_ran() -> bool {
    let n = text("SFI_ENTRY_RAN");
    let mut value = [0u16; 16];
    unsafe { GetEnvironmentVariableW(n.as_ptr(), value.as_mut_ptr(), 16) > 0 }
}
fn clear_marker() {
    let n = text("SFI_ENTRY_RAN");
    unsafe {
        SetEnvironmentVariableW(n.as_ptr(), std::ptr::null());
    }
}
fn value(module: usize) -> i32 {
    let p = unsafe { GetProcAddress(module as *mut c_void, c"entry_value".as_ptr()) };
    assert!(!p.is_null());
    let f: unsafe extern "C" fn() -> i32 = unsafe { std::mem::transmute(p) };
    WindowsBackend::with_owner(7, || unsafe { f() }).unwrap()
}
#[derive(Clone, Default)]
struct Notices(Arc<Mutex<Vec<String>>>);
impl Sink for Notices {
    fn self_hosted(&mut self, s: &str) {
        self.0.lock().unwrap().push(format!("self:{s}"));
    }
    fn quarantine(&mut self, o: Option<u64>, s: &str) {
        self.0.lock().unwrap().push(format!("quarantine:{o:?}:{s}"));
    }
    fn record(&mut self, s: &str) {
        self.0.lock().unwrap().push(format!("record:{s}"));
    }
    fn save_warning(&mut self, s: &str) {
        self.0.lock().unwrap().push(format!("warning:{s}"));
    }
}
/// The loader's own report of where it mapped each fault fixture, from this test's LOADED/UNLOADED notification and
/// independent of kiln-sfi. Native Windows hides an image whose DllMain failed from GetModuleHandleW (its loader node
/// stays in the init-error state) even while the pin keeps it mapped, so the image is found by address, not by name.
const FAULT_FIXTURES: [&str; 4] = [
    "fault.dll",
    "fault-av.dll",
    "fault-false.dll",
    "fault-thread.dll",
];
static FAULT_BASE: [AtomicUsize; 4] = [const { AtomicUsize::new(0) }; 4];
static FAULT_UNLOADED: [AtomicBool; 4] = [const { AtomicBool::new(false) }; 4];
#[repr(C)]
struct LoaderName {
    length: u16,
    maximum: u16,
    buffer: *const u16,
}
#[repr(C)]
struct LoaderNotice {
    flags: u32,
    full: *const LoaderName,
    base: *const LoaderName,
    dll_base: *mut c_void,
    size: u32,
}
/// Runs under the loader lock: compares and stores only, never allocates or panics.
unsafe extern "system" fn fault_notice(reason: u32, data: *const LoaderNotice, _: *mut c_void) {
    let Some(name) = (unsafe { data.as_ref().and_then(|d| d.base.as_ref()) }) else {
        return;
    };
    if name.buffer.is_null() {
        return;
    }
    let name = unsafe { std::slice::from_raw_parts(name.buffer, name.length as usize / 2) };
    for (i, fixture) in FAULT_FIXTURES.iter().enumerate() {
        let same = name.len() == fixture.len()
            && name
                .iter()
                .zip(fixture.bytes())
                .all(|(&c, b)| c < 0x80 && (c as u8).to_ascii_lowercase() == b);
        if same && reason == 1 {
            FAULT_BASE[i].store(unsafe { (*data).dll_base } as usize, Ordering::SeqCst);
        } else if same && reason == 2 {
            FAULT_UNLOADED[i].store(true, Ordering::SeqCst);
        }
    }
}
fn watch_fault_fixtures() {
    type Notice = unsafe extern "system" fn(u32, *const LoaderNotice, *mut c_void);
    type Register = unsafe extern "system" fn(u32, Notice, *mut c_void, *mut *mut c_void) -> i32;
    let register = unsafe {
        GetProcAddress(
            handle("ntdll.dll") as *mut c_void,
            c"LdrRegisterDllNotification".as_ptr(),
        )
    };
    assert!(!register.is_null());
    let register: Register = unsafe { std::mem::transmute(register) };
    let mut cookie = std::ptr::null_mut();
    assert!(unsafe { register(0, fault_notice, std::ptr::null_mut(), &mut cookie) } >= 0);
}
fn registry() -> NativeContracts {
    NativeContracts::shipped().unwrap()
}
#[test]
fn synthetic_wine_native_bindings() {
    let Ok(case) = std::env::var("SFI_WINE_CASE") else {
        eprintln!("Wine fixture test requires explicit runner; no DLL loaded");
        return;
    };
    let root = PathBuf::from(std::env::var_os("SFI_WINE_FIXTURES").expect("explicit fixture root"));
    clear_marker();
    let notices = Notices::default();
    if case.starts_with("manager-") {
        return manager_case(&case, &root, notices);
    }
    if case.starts_with("boundary-") {
        return boundary_case(&case, &root);
    }
    if case.starts_with("deadline-") {
        return deadline_case(&case, &root);
    }
    if case == "owner-tls" {
        let barrier = Arc::new(std::sync::Barrier::new(4));
        let mut threads = Vec::new();
        for owner in 7..11u64 {
            let barrier = barrier.clone();
            threads.push(std::thread::spawn(move || {
                barrier.wait();
                for _ in 0..4096 {
                    WindowsBackend::with_owner(owner, || {
                        std::thread::yield_now();
                        assert_eq!(unsafe { sfi_monitor_current_owner() }, owner);
                        WindowsBackend::with_owner(owner + 100, || {
                            assert_eq!(unsafe { sfi_monitor_current_owner() }, owner + 100)
                        })
                        .unwrap();
                        assert_eq!(unsafe { sfi_monitor_current_owner() }, owner);
                    })
                    .unwrap();
                }
                assert_eq!(unsafe { sfi_monitor_current_owner() }, 0);
            }));
        }
        for thread in threads {
            thread.join().unwrap();
        }
        return;
    }
    if case == "mapping-replaced" || case == "unrelated-replaced" {
        let original = root.join("loaded.dll");
        loaded(&original);
        let renamed = root.join("renamed.dll");
        let old = wide(original.as_os_str());
        let new = wide(renamed.as_os_str());
        assert_ne!(unsafe { MoveFileW(old.as_ptr(), new.as_ptr()) }, 0);
        let replacement = wide(root.join("replacement.dll").as_os_str());
        assert_ne!(
            unsafe { CopyFileW(replacement.as_ptr(), old.as_ptr(), 0) },
            0
        );
        if case == "mapping-replaced" {
            let mut e = Enforcer::early(WindowsBackend::default(), notices.clone()).unwrap();
            let image = Snapshot::capture(&original, "loaded.dll").unwrap();
            let permit = e.inspect(&image, &[], &registry()).unwrap();
            let refused = e
                .load(7, &permit)
                .expect_err("replacement pathname was accepted as mapped ownership");
            eprintln!("replaced mapping refusal: {refused}");
            assert!(
                refused.contains("loaded.dll but its backing cannot be proved (loader pathname is not the backing file of mapped module"),
                "refused for another reason: {refused}"
            );
            return;
        }
    }
    // The shim reads module memory inline inside __try (the PE header prefilter on every loaded module, the loader-list
    // walk). A module another thread unloads meanwhile must be a contained fault, not the end of the process.
    if case == "seh-inline-fault" {
        let gone = unsafe { VirtualAlloc(std::ptr::null_mut(), 65536, 0x2000, 1) };
        assert!(!gone.is_null());
        let disk = std::fs::read(root.join("loaded.dll")).unwrap();
        let mut matches = 7;
        let fault =
            unsafe { sfi_mapped_header_matches(gone, disk.as_ptr(), disk.len(), &mut matches) };
        assert_eq!(fault, 0xc000_0005, "the access violation was not contained");
        assert_eq!(matches, 0);
        eprintln!("unmapped module header read contained: {fault:#x}");
        return;
    }
    if case == "diagnostic-burst" || case == "banned-burst" {
        let mut e = Enforcer::early(WindowsBackend::default(), notices.clone()).unwrap();
        let n = text("normal.dll");
        for i in 0..2048 {
            unsafe { sfi_ring_publish(7, n.as_ptr(), 10, i) };
        }
        if case == "banned-burst" {
            WindowsBackend::with_owner(7, || loaded(&root.join("shroudforge-runtime.dll")))
                .unwrap();
        }
        for i in 0..2048 {
            unsafe { sfi_ring_publish(7, n.as_ptr(), 10, i) };
        }
        e.poll().unwrap();
        assert_eq!(e.is_quarantined(7), case == "banned-burst");
        assert_eq!(
            notices
                .0
                .lock()
                .unwrap()
                .iter()
                .any(|s| s.starts_with("warning:")),
            case == "banned-burst"
        );
        return;
    }
    if case == "preloaded-collision" || case == "lazy-preloaded-collision" {
        loaded(&root.join("foreign/helper.dll"));
    }
    // Review of 9d5dead4 (a0-fix-3): another loader's identical helper.dll from a separate file is a verified copy;
    // the LOADED rescan must accept it as admission did (the generic path below expects a clean load).
    if case == "preloaded-identical-helper" {
        loaded(&root.join("copy/helper.dll"));
    }
    // Review of 9d5dead4 (a0-fix-2): another loader initialized an identical copy of the entry under alias.dll, whose
    // pathname was then replaced. Its backing cannot be proved, so the entry must not be initialized a second time.
    if case == "renamed-identical-copy" {
        let alias = root.join("alias.dll");
        loaded(&alias);
        let moved = wide(root.join("moved-alias.dll").as_os_str());
        let old = wide(alias.as_os_str());
        assert_ne!(unsafe { MoveFileW(old.as_ptr(), moved.as_ptr()) }, 0);
        let replacement = wide(root.join("replacement.dll").as_os_str());
        assert_ne!(
            unsafe { CopyFileW(replacement.as_ptr(), old.as_ptr(), 0) },
            0
        );
        let mut e = Enforcer::early(WindowsBackend::default(), notices.clone()).unwrap();
        let image = Snapshot::capture(&root.join("loaded.dll"), "loaded.dll").unwrap();
        let permit = e.inspect(&image, &[], &registry()).unwrap();
        let outcome = e.load(7, &permit);
        eprintln!("identical renamed copy outcome: {outcome:?}");
        let refused = outcome.expect_err("an unprovable identical copy was ignored");
        assert!(
            refused.contains("alias.dll matches loaded.dll but its backing cannot be proved (loader pathname is not the backing file of mapped module"),
            "refused for another reason: {refused}"
        );
        assert_eq!(
            handle("loaded.dll"),
            0,
            "the entry was initialized a second time"
        );
        return;
    }
    let mut e = Enforcer::early(WindowsBackend::default(), notices.clone()).unwrap();
    // Review of 9d5dead4 (a0-fix-1): package A delay-imports helper.dll (untriggered); package B then loads identical
    // helper.dll bytes as its entry and its DllMain returns FALSE. The pinned image must be FAILED and quarantined, so
    // no third owner can receive it as foreign-owned.
    if case == "lazy-watch-failed-entry" {
        let a_entry =
            Snapshot::capture(&root.join("falsy/entry-delay.dll"), "entry-delay.dll").unwrap();
        let a_deps = [Snapshot::capture(&root.join("falsy/helper.dll"), "helper.dll").unwrap()];
        let a = e.inspect(&a_entry, &a_deps, &registry()).unwrap();
        assert!(matches!(e.load(7, &a).unwrap(), LoadOutcome::Loaded(_)));
        assert_eq!(
            handle("helper.dll"),
            0,
            "delay dependency eagerly initialized"
        );
        let b_entry = Snapshot::capture(&root.join("b/helper.dll"), "helper.dll").unwrap();
        let b = e.inspect(&b_entry, &[], &registry()).unwrap();
        let failed = e.load(8, &b);
        eprintln!("failing entry outcome: {failed:?}");
        assert!(failed.is_err(), "a DllMain returning FALSE loaded");
        let mut checking = WindowsBackend::default();
        let reuse = checking.first_loader(&b_entry);
        eprintln!("third owner's first_loader: {reuse:?}");
        assert!(
            reuse.is_err(),
            "the failed image is not quarantined: {reuse:?}"
        );
        let third = e.load(9, &b);
        eprintln!("third owner outcome: {third:?}");
        assert!(
            third.is_err(),
            "a third owner received the failed image: {third:?}"
        );
        return;
    }
    // Another loader's thread (no package owner) loads a DLL with the name one package delay-imports.
    // Only the packages that delay-import that name stop; an unrelated package keeps running and later loads work.
    // lazy-owned-shadow: package 8's own thread loads the shadow; package 8 stops too (it is not a delay importer).
    if case == "lazy-unowned-shadow" || case == "lazy-system-shadow" || case == "lazy-owned-shadow"
    {
        let system = case == "lazy-system-shadow";
        let owned = case == "lazy-owned-shadow";
        let (entry_name, shadow, shadowed) = if system {
            (
                "sysdelay.dll",
                root.join("shadow/version.dll"),
                "version.dll",
            )
        } else {
            (
                "entry-delay.dll",
                root.join("foreign/helper.dll"),
                "helper.dll",
            )
        };
        assert_eq!(handle(shadowed), 0, "{shadowed} is loaded already");
        let a_entry = Snapshot::capture(&root.join(entry_name), entry_name).unwrap();
        let a_deps = if system {
            Vec::new()
        } else {
            vec![Snapshot::capture(&root.join("helper.dll"), "helper.dll").unwrap()]
        };
        let a = e.inspect(&a_entry, &a_deps, &registry()).unwrap();
        assert!(matches!(e.load(7, &a).unwrap(), LoadOutcome::Loaded(_)));
        assert_eq!(handle(shadowed), 0, "delay dependency eagerly initialized");
        let b_entry = Snapshot::capture(&root.join("loaded.dll"), "loaded.dll").unwrap();
        let b = e.inspect(&b_entry, &[], &registry()).unwrap();
        assert!(matches!(e.load(8, &b).unwrap(), LoadOutcome::Loaded(_)));
        if owned {
            WindowsBackend::with_owner(8, || loaded(&shadow)).unwrap();
        } else {
            loaded(&shadow);
        }
        e.poll().unwrap();
        eprintln!("after the shadow: {:?}", notices.0.lock().unwrap());
        assert!(
            e.is_quarantined(7),
            "the package that delay-imports {shadowed} kept running"
        );
        assert_eq!(
            e.is_quarantined(8),
            owned,
            "package 8 (its thread loaded the shadow: {owned}) stopped or kept running wrongly"
        );
        assert_eq!(
            unsafe { sfi_breach_flags() },
            0,
            "the shadow became a global breach"
        );
        let c_entry = Snapshot::capture(&root.join("api-entry.dll"), "api-entry.dll").unwrap();
        let c = e.inspect(&c_entry, &[], &registry()).unwrap();
        let later = e.load(9, &c);
        assert!(
            matches!(later, Ok(LoadOutcome::Loaded(_))),
            "loads stopped after the shadow: {later:?}"
        );
        return;
    }
    // An OS DLL name held by another loader's file (proxy/hijack DLLs are common in game folders): a package that imports
    // that name ordinarily is refused before it loads, natively too, for exactly that reason.
    if case == "system-preloaded-collision" {
        assert_eq!(handle("version.dll"), 0, "version.dll is loaded already");
        loaded(&root.join("shadow/version.dll"));
        let image = Snapshot::capture(&root.join("sysimport.dll"), "sysimport.dll").unwrap();
        let permit = e.inspect(&image, &[], &registry()).unwrap();
        let outcome = e.load(7, &permit);
        eprintln!("system collision outcome: {outcome:?}");
        let refused = outcome.expect_err("a package was bound to another loader's version.dll");
        assert_eq!(
            refused,
            "loader refusal 0x0: preloaded version.dll has different backing identity/content",
            "refused for another reason"
        );
        assert!(
            !entry_ran(),
            "the package ran against another loader's version.dll"
        );
        return;
    }
    // O3: another loader maps the entry with DONT_RESOLVE_DLL_REFERENCES. Windows and Wine list that module but never
    // resolve its imports or run its DllMain, and hand later ordinary loads the same module (measured on native
    // Windows). It is neither foreign-owned nor loadable: the package is refused.
    // unresolved-foreign-dependency: the same for a dependency. The scan refuses it on both loaders (Wine leaves ImageDll
    // set on such a module, so there it must not pass for another thread's load under way).
    if case == "unresolved-foreign" || case == "unresolved-foreign-dependency" {
        let unresolved = if case == "unresolved-foreign" {
            "entry.dll"
        } else {
            "helper.dll"
        };
        let path = wide(root.join(unresolved).as_os_str());
        let module = unsafe { LoadLibraryExW(path.as_ptr(), std::ptr::null_mut(), 1) };
        assert!(
            !module.is_null(),
            "DONT_RESOLVE_DLL_REFERENCES load failed: {}",
            unsafe { GetLastError() }
        );
        assert!(!entry_ran());
        let entry = Snapshot::capture(&root.join("entry.dll"), "entry.dll").unwrap();
        let deps = [Snapshot::capture(&root.join("helper.dll"), "helper.dll").unwrap()];
        let permit = e.inspect(&entry, &deps, &registry()).unwrap();
        let outcome = e.load(7, &permit);
        eprintln!("uninitialized foreign mapping outcome: {outcome:?}");
        let refused = outcome.expect_err("a module its loader never initialized was admitted");
        assert!(
            refused.contains(&format!(
                "{unresolved} is mapped but its loader never initialized it"
            )),
            "refused for another reason: {refused}"
        );
        assert!(
            !entry_ran(),
            "the uninitialized package was initialized after all"
        );
        assert_eq!(unsafe { sfi_breach_flags() }, 0);
        return;
    }
    // A watch holds at most 8 alternative files per name. Nine packages each ship their own
    // identical helper.dll; the first eight load (another loader's identical copy is the same DLL), and the ninth is
    // refused before any load with a reason that says so, not the generic inventory text.
    if case == "copies-over-capacity" {
        for k in 0..9u64 {
            let folder = root.join(format!("copies/p{k}"));
            let name = format!("entry-{k:02}.dll");
            let entry = Snapshot::capture(&folder.join(&name), &name).unwrap();
            let deps = [Snapshot::capture(&folder.join("helper.dll"), "helper.dll").unwrap()];
            let permit = e.inspect(&entry, &deps, &registry()).unwrap();
            let outcome = e.load(40 + k, &permit);
            eprintln!("package {k}: {outcome:?}");
            if k < 8 {
                assert!(
                    matches!(outcome, Ok(LoadOutcome::Loaded(_))),
                    "package {k}: {outcome:?}"
                );
                continue;
            }
            let refused = outcome.expect_err("a ninth identical copy was admitted");
            assert!(
                refused.contains("more than 8 identical copies of helper.dll"),
                "refused for another reason: {refused}"
            );
            assert_eq!(handle(&name), 0, "the ninth package's entry was loaded");
        }
        e.poll().unwrap();
        assert_eq!(unsafe { sfi_breach_flags() }, 0);
        assert!((40..48).all(|owner| !e.is_quarantined(owner)));
        return;
    }
    // O3: the module-list walk (check_active_namespace) under the loader lock while other threads load and free
    // unrelated DLLs. Every package must load with no poison, fault or breach.
    if case == "namespace-walk-stress" {
        let stop = Arc::new(AtomicBool::new(false));
        let mut churn = Vec::new();
        for t in 0..4usize {
            let (stop, root) = (stop.clone(), root.clone());
            churn.push(std::thread::spawn(move || {
                let paths: Vec<_> = (0..8)
                    .map(|i| {
                        wide(
                            root.join(format!("stress/s{:02}.dll", t * 8 + i))
                                .as_os_str(),
                        )
                    })
                    .collect();
                let mut cycles = 0usize;
                while !stop.load(Ordering::SeqCst) {
                    for p in &paths {
                        let m = unsafe { LoadLibraryExW(p.as_ptr(), std::ptr::null_mut(), 0x1100) };
                        assert!(!m.is_null());
                        assert_ne!(unsafe { FreeLibrary(m) }, 0);
                        cycles += 1;
                    }
                    // Wine's loader lock is unfair: four threads in a tight load/free loop starve the package loads
                    // (no progress in 180 s; Windows finished in 0.4 s). A 1 ms pause per 8 cycles keeps it fair.
                    unsafe { Sleep(1) };
                }
                cycles
            }));
        }
        let started = std::time::Instant::now();
        for k in 0..16u64 {
            let name = format!("entry-{k:02}.dll");
            let folder = root.join("walk");
            let entry = Snapshot::capture(&folder.join(&name), &name).unwrap();
            let deps = [Snapshot::capture(&folder.join("helper.dll"), "helper.dll").unwrap()];
            let permit = e.inspect(&entry, &deps, &registry()).unwrap();
            let outcome = e.load(20 + k, &permit);
            assert!(
                matches!(outcome, Ok(LoadOutcome::Loaded(_))),
                "package {k} under churn: {outcome:?}"
            );
        }
        let elapsed = started.elapsed();
        stop.store(true, Ordering::SeqCst);
        let cycles: usize = churn.into_iter().map(|t| t.join().unwrap()).sum();
        e.poll().unwrap();
        assert_eq!(unsafe { sfi_breach_flags() }, 0);
        assert!((20..36).all(|owner| !e.is_quarantined(owner)));
        eprintln!(
            "16 package loads in {elapsed:?} beside {cycles} unrelated load/free cycles: no poison, fault or breach"
        );
        return;
    }
    // Another loader (no package owner) loads the very file a package delay-imports, and its DllMain returns FALSE. The
    // watch pins that image outside any load window, so no failed transaction marks it. Native Windows still sets
    // ProcessAttachCalled for a FALSE return (with ProcessAttachFailed): a later
    // package whose entry is that file must be refused, never handed the failed image as foreign-owned.
    if case == "foreign-failed-copy" {
        let a_entry =
            Snapshot::capture(&root.join("falsy/entry-delay.dll"), "entry-delay.dll").unwrap();
        let a_deps = [Snapshot::capture(&root.join("falsy/helper.dll"), "helper.dll").unwrap()];
        let a = e.inspect(&a_entry, &a_deps, &registry()).unwrap();
        assert!(matches!(e.load(7, &a).unwrap(), LoadOutcome::Loaded(_)));
        let path = wide(root.join("falsy/helper.dll").as_os_str());
        let foreign = unsafe { LoadLibraryExW(path.as_ptr(), std::ptr::null_mut(), 0x1100) };
        assert!(foreign.is_null(), "a DllMain returning FALSE loaded");
        let c_entry = Snapshot::capture(&root.join("falsy/helper.dll"), "helper.dll").unwrap();
        let c = e.inspect(&c_entry, &[], &registry()).unwrap();
        let outcome = e.load(9, &c);
        eprintln!("another loader's failed image outcome: {outcome:?}");
        let refused = outcome.expect_err("another loader's failed image was admitted");
        assert!(
            refused.contains("helper.dll is mapped but its loader never initialized it"),
            "refused for another reason: {refused}"
        );
        return;
    }
    // The native loader drops trailing dots and spaces from path components and appends ".dll" to a name with no
    // extension, for the verbatim \\?\ spelling too. Inspection
    // refuses such a file name, but not a folder of a verbatim package root (dotted.\loaded.dll, whose dot a plain path
    // would lose to Win32 normalization at capture) nor a name with no extension (bare\loaded). Each sits beside a decoy
    // the loader would map instead (entry-delay.dll's bytes, whose DllMain sets SFI_ENTRY_RAN): the load must be refused
    // before any load. Where the file system cannot hold the respelled name apart, the layout is skipped.
    if case == "loader-respelled-name" {
        let verbatim = |p: &Path| PathBuf::from(format!(r"\\?\{}", p.display()));
        for (real, decoy, name) in [
            (r"dotted.\loaded.dll", r"dotted\loaded.dll", "loaded.dll"),
            (r"bare\loaded", r"bare\loaded.dll", "loaded"),
        ] {
            let (real_path, decoy_path) = (verbatim(&root.join(real)), verbatim(&root.join(decoy)));
            for path in [&real_path, &decoy_path] {
                std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            }
            std::fs::copy(root.join("loaded.dll"), &real_path).unwrap();
            std::fs::copy(root.join("entry-delay.dll"), &decoy_path).unwrap();
            if std::fs::read(&real_path).unwrap() != std::fs::read(root.join("loaded.dll")).unwrap()
            {
                eprintln!("{real}: the file system does not keep it apart from {decoy}; skipped");
                continue;
            }
            let image = Snapshot::capture(&real_path, name).unwrap();
            let permit = e.inspect(&image, &[], &registry()).unwrap();
            let outcome = e.load(7, &permit);
            eprintln!("{real} beside {decoy} outcome: {outcome:?}");
            let refused = outcome.expect_err("a path the loader respells was loaded");
            assert!(
                refused.contains("the Windows loader would not map"),
                "refused for another reason: {refused}"
            );
            assert!(!entry_ran(), "the decoy {decoy} ran in place of {real}");
        }
        return;
    }
    // Native Windows maps and snaps a DLL outside the loader lock, so the window scan meets other threads' loads and
    // unloads of a bound name mid-way: ImageDll set, not yet attached (about half of all sightings beside a load/free
    // loop on native Windows). The OS loader finishes those itself. Four threads load and free
    // version.dll while 16 packages that delay-import it load: every load must succeed, with no poison, fault or breach.
    if case == "midload-churn" {
        let stop = Arc::new(AtomicBool::new(false));
        let mut churn = Vec::new();
        for _ in 0..4 {
            let stop = stop.clone();
            churn.push(std::thread::spawn(move || {
                let name = text("version.dll");
                let mut cycles = 0usize;
                while !stop.load(Ordering::SeqCst) {
                    let m = unsafe { LoadLibraryExW(name.as_ptr(), std::ptr::null_mut(), 0x800) };
                    assert!(!m.is_null());
                    assert_ne!(unsafe { FreeLibrary(m) }, 0);
                    cycles += 1;
                    // Wine's loader lock is unfair (see namespace-walk-stress).
                    if cycles.is_multiple_of(8) {
                        unsafe { Sleep(1) };
                    }
                }
                cycles
            }));
        }
        let mut refused = Vec::new();
        for k in 0..16u64 {
            let name = format!("sysdelay-{k:02}.dll");
            let entry = Snapshot::capture(&root.join("churn").join(&name), &name).unwrap();
            let permit = e.inspect(&entry, &[], &registry()).unwrap();
            match e.load(40 + k, &permit) {
                Ok(LoadOutcome::Loaded(_)) => {}
                other => refused.push(format!("{name}: {other:?}")),
            }
        }
        stop.store(true, Ordering::SeqCst);
        let cycles: usize = churn.into_iter().map(|t| t.join().unwrap()).sum();
        e.poll().unwrap();
        assert!(
            refused.is_empty(),
            "packages refused beside another thread's loads of version.dll: {refused:?}"
        );
        assert_eq!(unsafe { sfi_breach_flags() }, 0);
        assert!((40..56).all(|owner| !e.is_quarantined(owner)));
        eprintln!("16 package loads beside {cycles} version.dll load/free cycles: all loaded");
        return;
    }
    if case.starts_with("dllmain-") {
        let filename = match case.as_str() {
            "dllmain-av" => "fault-av.dll",
            "dllmain-false" => "fault-false.dll",
            "dllmain-thread-fault" => "fault-thread.dll",
            _ => "fault.dll",
        };
        let path = root.join(filename);
        let image = Snapshot::capture(&path, filename).unwrap();
        let permit = e.inspect(&image, &[], &registry()).unwrap();
        watch_fault_fixtures();
        let outcome = e.load(7, &permit);
        eprintln!("injected DllMain outcome: {outcome:?}");
        assert!(outcome.is_err());
        assert!(e.is_quarantined(7));
        assert!(
            notices
                .0
                .lock()
                .unwrap()
                .iter()
                .any(|s| s.contains("Do not save"))
        );
        assert!(e.load(8, &permit).is_err());
        unsafe { Sleep(1000) };
        let fixture = FAULT_FIXTURES.iter().position(|f| *f == filename).unwrap();
        let module = FAULT_BASE[fixture].load(Ordering::SeqCst);
        assert_ne!(module, 0, "the loader never reported mapping {filename}");
        // Neither the name lookup nor the UNLOADED notice is asserted: Wine finds the failed image by name and also
        // notifies UNLOADED while the pin keeps it mapped; native Windows does neither (init-error state).
        eprintln!(
            "failed image at {module:#x}; GetModuleHandleW finds it at {:#x}; UNLOADED notified: {}",
            handle(filename),
            FAULT_UNLOADED[fixture].load(Ordering::SeqCst)
        );
        // MEMORY_BASIC_INFORMATION (x64): info[4] low half = State, info[5] low half = Type.
        let mut info = [0u64; 6];
        assert_ne!(
            unsafe { VirtualQuery(module as *const c_void, info.as_mut_ptr().cast(), 48) },
            0
        );
        assert_eq!(
            info[4] & 0xffffffff,
            0x1000,
            "failed package image was unmapped despite pin"
        );
        assert_eq!(
            info[5] & 0xffffffff,
            0x100_0000,
            "the failed package image's address is no longer an image mapping"
        );
        let mut checking = WindowsBackend::default();
        assert!(
            checking.first_loader(&image).is_err(),
            "pinned failure was treated as foreign-owned success"
        );
        if case == "dllmain-thread-fault" {
            let name = text("SFI_FAULT_THREAD_RAN");
            let mut value = [0u16; 16];
            assert!(
                unsafe { GetEnvironmentVariableW(name.as_ptr(), value.as_mut_ptr(), 16) } > 0,
                "published thread did not survive pinned rollback"
            );
        }
        let other = Snapshot::capture(&root.join("entry.dll"), "entry.dll").unwrap();
        let deps = [Snapshot::capture(&root.join("helper.dll"), "helper.dll").unwrap()];
        let next = e.inspect(&other, &deps, &registry()).unwrap();
        assert!(matches!(e.load(8, &next).unwrap(), LoadOutcome::Loaded(_)));
        assert!(
            !e.is_quarantined(8),
            "clean failed owner poisoned independent package"
        );
        eprintln!(
            "alive after1s; pinned failed image committed; no retry; identity is quarantined; independent package loaded"
        );
        return;
    }
    if case == "system-api-set" {
        let path = root.join("api-entry.dll");
        let image = Snapshot::capture(&path, "api-entry.dll").unwrap();
        let permit = e.inspect(&image, &[], &registry()).unwrap();
        let module = match e.load(7, &permit).unwrap() {
            LoadOutcome::Loaded(m) => m,
            other => panic!("unexpected OS fixture owner {other:?}"),
        };
        assert_eq!(value(module), 0);
        assert!(!e.is_quarantined(7));
        return;
    }
    let lazy = case.starts_with("lazy");
    let path = root.join(if lazy { "entry-delay.dll" } else { "entry.dll" });
    let name = path.file_name().unwrap().to_str().unwrap();
    let entry = Snapshot::capture(&path, name).unwrap();
    if case == "missing-dependency" {
        let refused = e
            .inspect(&entry, &[], &registry())
            .err()
            .expect("a package with a missing dependency was admitted");
        eprintln!("missing dependency refusal: {refused}");
        assert!(
            refused.starts_with("unresolved dependency helper.dll imported by entry.dll;"),
            "refused for another reason: {refused}"
        );
        assert!(!entry_ran());
        return;
    }
    let dependencies = [Snapshot::capture(&root.join("helper.dll"), "helper.dll").unwrap()];
    let permit = e.inspect(&entry, &dependencies, &registry()).unwrap();
    if case == "ordinary-correct" {
        std::fs::write(
            root.join("state-write-probe.txt"),
            b"ordinary file IO remains available",
        )
        .unwrap();
    }
    let outcome = e.load(7, &permit);
    if case.contains("preloaded-collision") {
        eprintln!("preloaded collision outcome: {outcome:?}");
        let refused = outcome.expect_err("a package bound to a mismatched preloaded helper");
        assert_eq!(
            refused,
            "loader refusal 0x0: preloaded helper.dll has different backing identity/content",
            "refused for another reason"
        );
        assert!(
            !entry_ran(),
            "entry ran against mismatched preloaded helper"
        );
        return;
    }
    let module = match outcome.unwrap() {
        LoadOutcome::Loaded(m) => m,
        other => panic!("unexpected fixture owner {other:?}"),
    };
    assert!(entry_ran());
    if lazy {
        assert_eq!(
            handle("helper.dll"),
            0,
            "delay dependency eagerly initialized"
        );
    }
    if case == "lazy-late-collision" {
        loaded(&root.join("foreign/helper.dll"));
        e.poll().unwrap();
        assert!(e.is_quarantined(7));
        assert!(
            notices
                .0
                .lock()
                .unwrap()
                .iter()
                .any(|s| s.contains("Do not save"))
        );
        return;
    }
    if case == "lazy-after-close" {
        drop(e);
        drop(permit);
        drop(dependencies);
        drop(entry);
        assert_eq!(value(module), 1);
        assert_eq!(
            unsafe { sfi_breach_flags() },
            0,
            "callback used destroyed consumer data"
        );
        return;
    }
    assert_eq!(value(module), 1);
    if lazy {
        assert_ne!(handle("helper.dll"), 0);
    }
    e.poll().unwrap();
    assert!(!e.is_quarantined(7));
    // No FreeLibrary: every fixture process ends with its mappings intact.
}

/// The executor on the real loader: it loads a frozen native plan through the native-load enforcement.
struct FileWitness {
    path: PathBuf,
    seen: Mutex<Option<String>>,
}
impl Witness for FileWitness {
    fn ready(&self) -> Result<WitnessToken, String> {
        let bytes = std::fs::read(&self.path).map_err(|e| e.to_string())?;
        *self.seen.lock().unwrap() = Some(NativeContracts::sha256(&bytes));
        Ok(WitnessToken {
            plan_id: [3; 16],
            revision: 1,
        })
    }
    fn recheck(&self, _: &WitnessToken) -> Result<(), String> {
        let bytes = std::fs::read(&self.path).map_err(|e| e.to_string())?;
        if self.seen.lock().unwrap().as_deref() == Some(NativeContracts::sha256(&bytes).as_str()) {
            Ok(())
        } else {
            Err("the witnessed archive input changed during the native loads".into())
        }
    }
}
fn planned(root: &Path, package: &str, file: &str) -> PlannedDll {
    let bytes = std::fs::read(root.join("game/mods").join(package).join(file)).unwrap();
    PlannedDll {
        status_id: format!("dll:{package}:{file}"),
        path: file.into(),
        sha256: NativeContracts::sha256(&bytes),
        mode: Mode::Library,
        source: "ini".into(),
        bridge_abi: 0,
    }
}
fn manager_case(case: &str, root: &Path, notices: Notices) {
    // The same plans under Kiln's native.policy refusing package DLLs ("never", or "verified-only" on a build
    // that is not verified): nothing may load, not even map.
    let (case, permission) = if let Some(base) = case.strip_suffix("-never") {
        (
            base,
            NativePermission {
                policy: NativePolicy::Never,
                verified: true,
            },
        )
    } else if let Some(base) = case.strip_suffix("-unverified") {
        (
            base,
            NativePermission {
                policy: NativePolicy::VerifiedOnly,
                verified: false,
            },
        )
    } else {
        (case, NativePermission::ALWAYS)
    };
    let refusal = permission.refusal();
    let game = root.join("game");
    let package = |id: &str, files: &[&str], deps: &[&str]| PlannedPackage {
        mod_id: id.into(),
        directory: id.into(),
        root: game.join("mods").join(id),
        dlls: files.iter().map(|f| planned(root, id, f)).collect(),
        dependencies: deps.iter().map(|d| (*d).to_owned()).collect(),
    };
    let mut packages = if case == "manager-order" {
        vec![
            package("a", &["entry.dll", "fault-false.dll", "third.dll"], &[]),
            package("b", &["solo.dll"], &[]),
            package("c", &["dep.dll"], &["a"]),
        ]
    } else {
        vec![package("t", &["touch.dll"], &[])]
    };
    if refusal.is_some() {
        // Lua-only packages: one needs a DLL package (through it the withdrawal travels), one needs nothing native.
        let first = packages[0].mod_id.clone();
        packages.push(PlannedPackage {
            mod_id: "lua".into(),
            directory: "lua".into(),
            root: game.join("mods").join("lua"),
            dlls: Vec::new(),
            dependencies: vec![first],
        });
        packages.push(PlannedPackage {
            mod_id: "free".into(),
            directory: "free".into(),
            root: game.join("mods").join("free"),
            dlls: Vec::new(),
            dependencies: Vec::new(),
        });
    }
    let witness = Arc::new(FileWitness {
        path: game.join("witness/input.bin"),
        seen: Mutex::new(None),
    });
    let run = NativeRun::new(
        NativePlan {
            plan_id: [3; 16],
            packages,
        },
        witness,
        permission,
    );
    let mut enforcer = Enforcer::early(WindowsBackend::default(), notices.clone()).unwrap();
    watch_fault_fixtures();
    run.execute(&mut enforcer, &registry());
    let terminal = run.wait(STARTUP_NATIVE_DEADLINE);
    for record in &terminal.records {
        eprintln!("{} -> {:?}", record.status_id, record.outcome);
    }
    if let Some(text) = refusal {
        assert!(
            terminal
                .records
                .iter()
                .all(|r| r.outcome == NativeOutcome::Withdrawn(text.into())
                    && r.actual_sha256.is_none()),
            "a DLL was attempted under the refusal"
        );
        let withdrawn: Vec<_> = terminal
            .withdrawn
            .iter()
            .map(|w| (w.mod_id.as_str(), w.reason.as_str()))
            .collect();
        let first = if case == "manager-order" { "a" } else { "t" };
        let dependent = format!("depends on withdrawn package {first}");
        let expected: Vec<(&str, &str)> = if case == "manager-order" {
            vec![("a", text), ("b", text), ("c", text), ("lua", &dependent)]
        } else {
            vec![("t", text), ("lua", &dependent)]
        };
        assert_eq!(withdrawn, expected);
        assert!(terminal.runs_whole("free"));
        // Nothing ran, so nothing changed the witnessed input (touch.dll would have).
        assert!(terminal.coherent.is_ok(), "{:?}", terminal.coherent);
        unsafe { Sleep(200) };
        for name in [
            "entry.dll",
            "helper.dll",
            "fault-false.dll",
            "third.dll",
            "solo.dll",
            "dep.dll",
            "touch.dll",
        ] {
            assert_eq!(handle(name), 0, "{name} was loaded under the refusal");
        }
        assert!(
            FAULT_BASE.iter().all(|b| b.load(Ordering::SeqCst) == 0),
            "the loader mapped a fixture under the refusal"
        );
        assert!(!entry_ran(), "a DllMain ran under the refusal");
        let notices = notices.0.lock().unwrap();
        assert!(
            !notices
                .iter()
                .any(|n| n.starts_with("quarantine:") || n.starts_with("warning:")),
            "{notices:?}"
        );
        eprintln!(
            "{text}: every DLL package withdrawn with its Lua-only dependent, zero loads, the witness holds"
        );
        return;
    }
    if case == "manager-witness-change" {
        assert!(terminal.records[0].outcome.admitted());
        assert!(
            terminal
                .coherent
                .as_ref()
                .is_err_and(|e| e.contains("witnessed archive input changed")),
            "{:?}",
            terminal.coherent
        );
        eprintln!("a DLL that changed the witnessed input made the run incoherent");
        return;
    }
    let kinds: Vec<_> = terminal
        .records
        .iter()
        .map(|r| match &r.outcome {
            NativeOutcome::Loaded { .. } => "loaded",
            NativeOutcome::Faulted(_) => "faulted",
            NativeOutcome::Withdrawn(_) => "withdrawn",
            other => panic!("{}: {other:?}", r.status_id),
        })
        .collect();
    assert_eq!(
        kinds,
        ["loaded", "faulted", "withdrawn", "loaded", "withdrawn"]
    );
    assert!(entry_ran(), "a's entry did not initialize");
    let withdrawn: Vec<_> = terminal
        .withdrawn
        .iter()
        .map(|w| w.mod_id.as_str())
        .collect();
    assert_eq!(withdrawn, ["a", "c"]);
    assert!(terminal.coherent.is_ok(), "{:?}", terminal.coherent);
    // Never unloaded: a's loaded entry and its pinned failed image stay mapped; withdrawn
    // DLLs were never loaded.
    unsafe { Sleep(500) };
    assert_ne!(
        handle("entry.dll"),
        0,
        "a loaded DLL of a withdrawn package was unmapped"
    );
    // By the loader's own LOADED address, not by name: native Windows hides an image whose DllMain failed from
    // GetModuleHandleW while the pin keeps it mapped (see the dllmain-* cases).
    let fixture = FAULT_FIXTURES
        .iter()
        .position(|f| *f == "fault-false.dll")
        .unwrap();
    let failed = FAULT_BASE[fixture].load(Ordering::SeqCst);
    assert_ne!(
        failed, 0,
        "the loader never reported mapping fault-false.dll"
    );
    let mut info = [0u64; 6];
    assert_ne!(
        unsafe { VirtualQuery(failed as *const c_void, info.as_mut_ptr().cast(), 48) },
        0
    );
    assert_eq!(
        (info[4] & 0xffffffff, info[5] & 0xffffffff),
        (0x1000, 0x100_0000),
        "the failed image was unmapped despite the pin"
    );
    assert_eq!(handle("third.dll"), 0, "a DLL after the failure was loaded");
    assert_eq!(handle("dep.dll"), 0, "a dependent's DLL was loaded");
    assert_ne!(handle("solo.dll"), 0);
    assert!(
        notices
            .0
            .lock()
            .unwrap()
            .iter()
            .any(|s| s.contains("Do not save"))
    );
    eprintln!(
        "ordered; failure withdrew a and its dependent c; b loaded; nothing unloaded or retried"
    );
}

// ---- kiln-sfi's early phase arms the native loader only when the policy allows (boundary-* cases) ----

#[repr(C)]
struct ThreadEntry {
    size: u32,
    usage: u32,
    thread: u32,
    owner: u32,
    base_priority: i32,
    delta_priority: i32,
    flags: u32,
}
#[link(name = "kernel32")]
unsafe extern "system" {
    fn CreateToolhelp32Snapshot(flags: u32, process: u32) -> *mut c_void;
    fn Thread32First(snapshot: *mut c_void, entry: *mut ThreadEntry) -> i32;
    fn Thread32Next(snapshot: *mut c_void, entry: *mut ThreadEntry) -> i32;
    fn CloseHandle(handle: *mut c_void) -> i32;
    fn OpenThread(access: u32, inherit: i32, thread: u32) -> *mut c_void;
    fn GetThreadDescription(thread: *mut c_void, description: *mut *mut u16) -> i32;
    fn LocalFree(memory: *mut c_void) -> *mut c_void;
    fn GetCurrentProcessId() -> u32;
}
unsafe extern "C" {
    fn sfi_monitor_epoch() -> u64;
}
/// This process's threads: (id, description).
fn threads() -> Vec<(u32, String)> {
    let snapshot = unsafe { CreateToolhelp32Snapshot(4, 0) };
    assert!(
        !snapshot.is_null() && snapshot as isize != -1,
        "thread snapshot"
    );
    let me = unsafe { GetCurrentProcessId() };
    let mut entry = ThreadEntry {
        size: std::mem::size_of::<ThreadEntry>() as u32,
        usage: 0,
        thread: 0,
        owner: 0,
        base_priority: 0,
        delta_priority: 0,
        flags: 0,
    };
    let mut out = Vec::new();
    let mut more = unsafe { Thread32First(snapshot, &mut entry) } != 0;
    while more {
        if entry.owner == me {
            let mut name = String::new();
            let thread = unsafe { OpenThread(0x0800, 0, entry.thread) };
            if !thread.is_null() {
                let mut text: *mut u16 = std::ptr::null_mut();
                if unsafe { GetThreadDescription(thread, &mut text) } >= 0 && !text.is_null() {
                    let len = (0..).take_while(|&i| unsafe { *text.add(i) } != 0).count();
                    name =
                        String::from_utf16_lossy(unsafe { std::slice::from_raw_parts(text, len) });
                    unsafe { LocalFree(text.cast()) };
                }
                unsafe { CloseHandle(thread) };
            }
            out.push((entry.thread, name));
        }
        entry.size = std::mem::size_of::<ThreadEntry>() as u32;
        more = unsafe { Thread32Next(snapshot, &mut entry) } != 0;
    }
    unsafe { CloseHandle(snapshot) };
    out
}
/// The startup witness of the fake host's default plan (the boundary cases wire it, so only the policy decides).
struct PlanWitness;
impl Witness for PlanWitness {
    fn ready(&self) -> Result<WitnessToken, String> {
        let hex = support::DEFAULT_PLAN_ID.replace('-', "");
        let mut plan_id = [0u8; 16];
        for (i, byte) in plan_id.iter_mut().enumerate() {
            *byte = u8::from_str_radix(&hex[i * 2..i * 2 + 2], 16).unwrap();
        }
        Ok(WitnessToken {
            plan_id,
            revision: 1,
        })
    }
    fn recheck(&self, _: &WitnessToken) -> Result<(), String> {
        Ok(())
    }
}
fn plan_witness() -> kiln_sfi::native_manager::arming::WitnessPort {
    kiln_sfi::native_manager::arming::WitnessPort::Wired(Arc::new(PlanWitness))
}
fn boundary_case(case: &str, root: &Path) {
    use kiln_abi::services::kiln_launch::*;
    use kiln_abi::*;
    use kiln_sfi::native_manager::arming;
    use kiln_sfi::{Boundary, InactiveReason, NativeHooks, Phase};
    let (policy, build_verified, wired) = match case {
        "boundary-never" => (KILN_LAUNCH_NATIVE_POLICY_NEVER, 1, true),
        "boundary-unverified" => (KILN_LAUNCH_NATIVE_POLICY_VERIFIED_ONLY, 0, true),
        "boundary-not-wired" => (KILN_LAUNCH_NATIVE_POLICY_ALWAYS, 1, false),
        "boundary-armed" => (KILN_LAUNCH_NATIVE_POLICY_ALWAYS, 1, true),
        other => panic!("unknown boundary case {other}"),
    };
    // the plan: package t ships the case's game/mods/t/touch.dll (its real SHA-256), and a Lua-only package needs it;
    // the fake host names the case's game folder (an absolute Windows path)
    support::set_game_dir(root.join("game"));
    let dll = std::fs::read(root.join("game/mods/t/touch.dll")).unwrap();
    let mut plan = support::plan_document("client");
    let package = |id: &str, dlls: serde_json::Value, deps: &[&str]| {
        serde_json::json!({"id":id,"version":"1.0.0","kind":"shroudforge","root":format!("mods/{id}"),
          "content_sha256":"a".repeat(64),"config_sha256":"d".repeat(64),"enabled":true,"targets":["client","server"],
          "capabilities":["runtime"],
          "dependencies":deps.iter().map(|d| serde_json::json!({"id":d,"range":"^1.0","optional":false,"included":true})).collect::<Vec<_>>(),
          "requiredApis":[],"services":[],"requires180":false,"saveSensitive":false,"decision":"candidate","reasons":[],
          "data":{"required":false,"admission":"not-needed"},"lua":{"required":true,"admission":"admit"},"dlls":dlls})
    };
    plan["packages"] = serde_json::json!([
        package(
            "t",
            serde_json::json!([{"path":"touch.dll","sha256":NativeContracts::sha256(&dll),"mode":"library",
            "source":"ini","required":true,"gate":"pass","gateProof_sha256":"f".repeat(64),"bridgeAbi":0}]),
            &[]
        ),
        package("needs-t", serde_json::json!([]), &["t"]),
    ]);
    plan["loadOrder"] = serde_json::json!(["t", "needs-t"]);
    support::set_plan(Some(serde_json::to_vec(&plan).unwrap()));
    support::set_launch(Some((KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN)));
    support::set_native(policy as u32, build_verified, build_verified == 1);
    support::set_state_dir(support::fresh_dir("a4d-boundary"));
    let worker: &'static kiln_sfi::worker::Worker =
        Box::leak(Box::new(kiln_sfi::worker::Worker::new(
            kiln_sfi::lifecycle::factory::<kiln_sfi::plan_source::KilnPlanSource>,
        )));
    // the production loader (the Windows backend) and the production thread: only the witness is a test's
    let hooks = NativeHooks {
        witness: if wired {
            plan_witness
        } else {
            arming::not_wired
        },
        ..NativeHooks::PRODUCTION
    };
    let boundary: &'static Boundary = Box::leak(Box::new(
        Boundary::with_worker(InactiveReason::NegotiationMissing, worker)
            .with_own_loader(|| None)
            .with_native(hooks),
    ));
    let before = threads();
    assert_eq!(
        unsafe { sfi_monitor_epoch() },
        0,
        "no notification before the early phase"
    );
    let api = Box::leak(support::api(true));
    let mut info = support::plugin_info();
    assert_eq!(
        unsafe { boundary.enter(Phase::Early, &*api, support::next_owner(), &mut info) },
        KILN_OK
    );
    let run = boundary
        .native_run()
        .expect("the early phase made the native run")
        .clone();
    if case == "boundary-armed" {
        // the control: the same boundary arms with a policy that allows; the loader registers its notification
        // and loads touch.dll on its own thread, and its supervisor keeps running
        let terminal = run.wait(STARTUP_NATIVE_DEADLINE);
        for record in &terminal.records {
            eprintln!("{} -> {:?}", record.status_id, record.outcome);
        }
        assert!(terminal.not_armed.is_none());
        assert!(matches!(
            terminal.records[0].outcome,
            NativeOutcome::Loaded { .. }
        ));
        assert_ne!(handle("touch.dll"), 0);
        loaded(&root.join("loaded.dll"));
        assert!(
            unsafe { sfi_monitor_epoch() } > 0,
            "the loader notification is registered"
        );
        let after = threads();
        eprintln!("threads before {before:?} after {after:?}");
        assert!(
            after
                .iter()
                .any(|(_, name)| name == "kiln-sfi-fault-report"),
            "the armed loader's supervisor thread: {after:?}"
        );
        eprintln!(
            "armed: notification registered, touch.dll loaded on the loader thread, supervisor running"
        );
        return;
    }
    unsafe { Sleep(300) };
    let terminal = run.wait(std::time::Duration::ZERO);
    let reason = if wired {
        run.permission().refusal().unwrap().to_owned()
    } else {
        arming::WITNESS_NOT_WIRED.to_owned()
    };
    assert_eq!(terminal.not_armed.as_deref(), Some(reason.as_str()));
    assert!(
        terminal
            .records
            .iter()
            .all(|r| r.outcome == NativeOutcome::Refused(reason.clone())
                && r.actual_sha256.is_none())
    );
    // no loader notification: a DLL loaded now reaches no callback of kiln-sfi
    loaded(&root.join("loaded.dll"));
    assert_eq!(
        unsafe { sfi_monitor_epoch() },
        0,
        "a loader notification was registered"
    );
    assert_eq!(handle("touch.dll"), 0, "the package DLL was mapped");
    let after = threads();
    eprintln!("threads before {before:?} after {after:?}");
    assert!(
        !after
            .iter()
            .any(|(_, name)| name == arming::NATIVE_THREAD || name == "kiln-sfi-fault-report"),
        "a native thread started: {after:?}"
    );
    assert_eq!(
        after.len(),
        before.len(),
        "an extra thread: {before:?} -> {after:?}"
    );
    eprintln!("{case}: {reason}: no notification, no extra thread, nothing mapped");
}

/// ShroudForge's 15 s startup deadline in the early window, through the production boundary (production threads,
/// The Windows loader, the real deadline) with the fake kiln-eml startup seam (READY witness, terminal recorded).
/// Package hl/hs ships sleep.dll, whose DllMain holds the loader lock for 19 s (deadline-hung) or 300 ms
/// (deadline-in-time); a Lua-only package needs it; package x ships x.dll after it.
/// - deadline-hung: the deadline waiter pushes the terminal 15 s after the arming (inside kiln-eml's attach + 20 s)
///   while sleep.dll's DllMain still sleeps: sleep.dll timed-out, x.dll not started; when the DllMain returns its load is
///   only logged, x.dll never loads, nothing is pushed again.
/// - deadline-in-time: the loader thread pushes once (everything loaded), and the waiter ends long before its deadline.
fn deadline_case(case: &str, root: &Path) {
    use kiln_abi::services::kiln_launch::*;
    use kiln_sfi::native_manager::arming;
    use kiln_sfi::{Boundary, InactiveReason, NativeHooks, Phase};
    use std::time::{Duration, Instant};
    let hung = match case {
        "deadline-hung" => true,
        "deadline-in-time" => false,
        other => panic!("unknown deadline case {other}"),
    };
    let sleeper = if hung { "hl" } else { "hs" };
    support::set_game_dir(root.join("game"));
    let dll = |package: &str, file: &str| {
        let bytes = std::fs::read(root.join("game/mods").join(package).join(file)).unwrap();
        serde_json::json!([{"path":file,"sha256":NativeContracts::sha256(&bytes),"mode":"library","source":"ini",
            "required":true,"gate":"pass","gateProof_sha256":"f".repeat(64),"bridgeAbi":0}])
    };
    let package = |id: &str, dlls: serde_json::Value, deps: &[&str]| {
        serde_json::json!({"id":id,"version":"1.0.0","kind":"shroudforge","root":format!("mods/{id}"),
          "content_sha256":"a".repeat(64),"config_sha256":"d".repeat(64),"enabled":true,"targets":["client","server"],
          "capabilities":["runtime"],
          "dependencies":deps.iter().map(|d| serde_json::json!({"id":d,"range":"^1.0","optional":false,"included":true})).collect::<Vec<_>>(),
          "requiredApis":[],"services":[],"requires180":false,"saveSensitive":false,"decision":"candidate","reasons":[],
          "data":{"required":false,"admission":"not-needed"},"lua":{"required":true,"admission":"admit"},"dlls":dlls})
    };
    let mut plan = support::plan_document("client");
    plan["packages"] = serde_json::json!([
        package(sleeper, dll(sleeper, "sleep.dll"), &[]),
        package("needs-sleeper", serde_json::json!([]), &[sleeper]),
        package("x", dll("x", "x.dll"), &[]),
    ]);
    plan["loadOrder"] = serde_json::json!([sleeper, "needs-sleeper", "x"]);
    support::set_plan(Some(serde_json::to_vec(&plan).unwrap()));
    support::set_launch(Some((KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN)));
    support::set_native(KILN_LAUNCH_NATIVE_POLICY_ALWAYS as u32, 1, true);
    support::set_state_dir(support::fresh_dir("a4e-deadline"));
    fake_startup::install(fake_startup::Config::granting(&plan));
    let worker: &'static kiln_sfi::worker::Worker =
        Box::leak(Box::new(kiln_sfi::worker::Worker::new(
            kiln_sfi::lifecycle::factory::<kiln_sfi::plan_source::KilnPlanSource>,
        )));
    // production in full: the seam's witness, the loader and waiter threads, the Windows loader, the 15 s deadline
    assert_eq!(NativeHooks::PRODUCTION.deadline, STARTUP_NATIVE_DEADLINE);
    let boundary: &'static Boundary = Box::leak(Box::new(
        Boundary::with_worker(InactiveReason::NegotiationMissing, worker)
            .with_own_loader(|| None)
            .with_native(NativeHooks::PRODUCTION)
            .with_startup(fake_startup::resolve),
    ));
    let api = Box::leak(support::api(true));
    let mut info = support::plugin_info();
    let owner = support::next_owner();
    assert_eq!(
        unsafe { boundary.enter(Phase::Early, &*api, owner, &mut info) },
        kiln_abi::KILN_OK
    );
    let run = boundary
        .native_run()
        .expect("the early phase made the native run")
        .clone();
    // the armed run's two threads, named (a thread that cannot start shows no name yet), checked after the timing
    let names: Vec<String> = threads().into_iter().map(|(_, n)| n).collect();
    let eventually = |bound: Duration, condition: &dyn Fn() -> bool| {
        let started = Instant::now();
        while !condition() {
            if started.elapsed() > bound {
                return false;
            }
            unsafe { Sleep(10) };
        }
        true
    };
    let pushed = || fake_startup::seen().is_some_and(|s| s.terminal.is_some());
    let sleep_done = || {
        let n = text("SFI_SLEEP_DONE");
        let mut value = [0u16; 8];
        unsafe { GetEnvironmentVariableW(n.as_ptr(), value.as_mut_ptr(), 8) > 0 }
    };
    assert!(
        eventually(Duration::from_secs(60), &pushed),
        "no native terminal"
    );
    let seen = fake_startup::seen().unwrap();
    let after = seen.pushed_at.unwrap() - seen.attached_at.unwrap();
    let v = seen.terminal_json().unwrap();
    eprintln!(
        "{case}: terminal pushed {after:?} after the attach on {:?}: {v}",
        seen.pushed_on
    );
    let withdrawn: Vec<(String, String)> = v["withdrawn"]
        .as_array()
        .unwrap()
        .iter()
        .map(|w| {
            (
                w["id"].as_str().unwrap().to_owned(),
                w["code"].as_str().unwrap().to_owned(),
            )
        })
        .collect();
    let outcomes: Vec<&str> = v["dlls"]
        .as_array()
        .unwrap()
        .iter()
        .map(|d| d["outcome"].as_str().unwrap())
        .collect();
    if hung {
        // ShroudForge's deadline from the arming, and inside kiln-eml's wait (attach + 20 s)
        assert!(
            after >= STARTUP_NATIVE_DEADLINE,
            "pushed too early: {after:?}"
        );
        assert!(
            after < Duration::from_secs(20),
            "pushed after kiln-eml's wait: {after:?}"
        );
        assert_eq!(seen.pushed_on.as_deref(), Some(arming::DEADLINE_THREAD));
        // pushed while sleep.dll's DllMain still sleeps inside the loader lock
        assert!(!sleep_done(), "the DllMain had already returned");
        assert!(run.late_results().is_empty());
        assert_eq!(v["deadlinePassed"], true, "{v}");
        assert_eq!(outcomes, ["timed-out", "timed-out"], "{v}");
        assert_eq!(
            withdrawn,
            [
                (sleeper.to_owned(), "dll-timed-out".to_owned()),
                (
                    "needs-sleeper".to_owned(),
                    "dependency-withdrawn".to_owned()
                ),
                ("x".to_owned(), "dll-timed-out".to_owned()),
            ],
            "{v}"
        );
        // the DllMain returns: its load is only logged, x.dll never loads, nothing is pushed again
        assert!(
            eventually(Duration::from_secs(60), &sleep_done),
            "the DllMain never returned"
        );
        assert!(
            eventually(Duration::from_secs(60), &|| run.late_results().len() == 1),
            "the loader thread never finished sleep.dll's load"
        );
        assert!(run.late_results()[0].1.admitted());
        // review: the late load is a kiln.log warning of the early owner, from the loader thread's enforcement
        assert!(
            eventually(Duration::from_secs(60), &|| {
                support::logs(owner).iter().any(
                |(level, line)| *level == kiln_abi::KILN_LOG_WARN
                    && line == "kiln-sfi: native DLL 'sleep.dll' (dll:hl:sleep.dll) returned from the loader after the native startup deadline had concluded this start's native run (loaded: its code runs in the game process); the result is never admitted and its package stays withdrawn this start"
            )
            }),
            "the late load was not logged: {:?}",
            support::logs(owner)
        );
        unsafe { Sleep(300) };
        assert_eq!(handle("x.dll"), 0, "x.dll loaded after the deadline");
        assert_eq!(fake_startup::seen().unwrap().count("native_terminal"), 1);
        assert!(
            names.iter().any(|n| n == arming::DEADLINE_THREAD)
                && names.iter().any(|n| n == arming::NATIVE_THREAD),
            "the armed run's two threads: {names:?}"
        );
        eprintln!(
            "deadline-hung: pushed by the deadline waiter {after:?} after the attach while sleep.dll's DllMain held the loader lock; its late load only logged; x.dll never loaded; one push"
        );
    } else {
        assert!(after < STARTUP_NATIVE_DEADLINE, "{after:?}");
        assert_eq!(seen.pushed_on.as_deref(), Some(arming::NATIVE_THREAD));
        assert!(sleep_done());
        assert_eq!(v["deadlinePassed"], false, "{v}");
        assert_eq!(outcomes, ["loaded", "loaded"], "{v}");
        assert!(withdrawn.is_empty(), "{v}");
        assert_ne!(handle("x.dll"), 0);
        // the waiter ended when the run was done, long before its 15 s
        assert!(
            eventually(Duration::from_secs(8), &|| !threads()
                .iter()
                .any(|(_, n)| n == arming::DEADLINE_THREAD)),
            "the deadline waiter is still there"
        );
        assert_eq!(fake_startup::seen().unwrap().count("native_terminal"), 1);
        assert!(
            names.iter().any(|n| n == arming::DEADLINE_THREAD)
                && names.iter().any(|n| n == arming::NATIVE_THREAD),
            "the armed run's two threads: {names:?}"
        );
        eprintln!(
            "deadline-in-time: pushed by the loader thread {after:?} after the attach; the waiter ended without pushing; one push"
        );
    }
}
