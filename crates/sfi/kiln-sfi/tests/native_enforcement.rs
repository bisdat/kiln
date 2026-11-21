// SPDX-License-Identifier: MIT
use kiln_sfi::{
    native_contracts::NativeContracts,
    native_enforcement::{
        Backend, BackendLoadError, Breach, Enforcer, LoadOutcome, LoadPermit, Sink, Snapshot,
    },
};
use std::{
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
    },
};
#[derive(Default)]
struct State {
    existing: Vec<String>,
    events: Vec<Breach>,
    registered: usize,
    loads: usize,
    foreign: bool,
    fail_register: bool,
    post_load_banned: bool,
    dependency_collision: bool,
    load_fault: bool,
    clean_init_failure: bool,
    ordinary_failure: bool,
    unverifiable_mapping: bool,
    parked_receiver: Option<std::sync::mpsc::Receiver<kiln_sfi::native_enforcement::ParkedFault>>,
    notices: Vec<String>,
}
#[derive(Clone)]
struct Fake(Arc<Mutex<State>>);
struct ParkEvents(std::sync::mpsc::Receiver<kiln_sfi::native_enforcement::ParkedFault>);
impl kiln_sfi::native_enforcement::FaultSource for ParkEvents {
    fn wait(
        &mut self,
        budget: std::time::Duration,
    ) -> Result<Option<kiln_sfi::native_enforcement::ParkedFault>, String> {
        Ok(self.0.recv_timeout(budget).ok())
    }
}
impl Backend for Fake {
    fn fault_source(
        &mut self,
    ) -> Result<Option<Box<dyn kiln_sfi::native_enforcement::FaultSource>>, String> {
        Ok(self.0.lock().unwrap().parked_receiver.take().map(|rx| {
            Box::new(ParkEvents(rx)) as Box<dyn kiln_sfi::native_enforcement::FaultSource>
        }))
    }
    fn modules(&mut self) -> Result<Vec<String>, String> {
        Ok(self.0.lock().unwrap().existing.clone())
    }
    fn register(&mut self) -> Result<(), String> {
        let mut state = self.0.lock().unwrap();
        state.registered += 1;
        if state.fail_register {
            Err("registration unavailable".into())
        } else {
            Ok(())
        }
    }
    fn events(&mut self) -> Result<Vec<Breach>, String> {
        Ok(std::mem::take(&mut self.0.lock().unwrap().events))
    }
    fn first_loader(&mut self, image: &Snapshot) -> Result<Option<usize>, String> {
        let state = self.0.lock().unwrap();
        if state.dependency_collision && image.path().file_name().unwrap() == "helper.dll" {
            return Err("preloaded helper.dll backing content differs".into());
        }
        Ok(state.foreign.then_some(42))
    }
    fn load(&mut self, _: &Path, owner: u64) -> Result<usize, BackendLoadError> {
        let mut s = self.0.lock().unwrap();
        assert_eq!(s.registered, 1);
        s.loads += 1;
        if s.clean_init_failure {
            return Err(BackendLoadError::from_load_evidence(
                0,
                1114,
                true,
                0,
                "clean DLL init failure".into(),
            ));
        }
        if s.load_fault {
            return Err(BackendLoadError::NativeFault {
                exception: 0xc0000005,
                loader_error: 0,
                may_have_run: true,
                pin_status: 0,
            });
        }
        if s.ordinary_failure {
            return Err(BackendLoadError::LoaderFailure {
                code: 126,
                message: "missing dependency".into(),
            });
        }
        if s.post_load_banned {
            s.existing.push("shroudforge-runtime.dll".into());
            s.events.push(Breach {
                owner: Some(owner),
                module: "shroudforge-runtime.dll".into(),
                reason: "nested dependency load".into(),
            });
        }
        Ok(17)
    }
    fn validate_loaded(&mut self, _: usize, _: &Snapshot) -> Result<(), String> {
        if self.0.lock().unwrap().unverifiable_mapping {
            Err("mapped A is not replacement file B".into())
        } else {
            Ok(())
        }
    }
}
impl Sink for Fake {
    fn self_hosted(&mut self, reason: &str) {
        self.0
            .lock()
            .unwrap()
            .notices
            .push(format!("self-hosted:{reason}"));
    }
    fn quarantine(&mut self, owner: Option<u64>, reason: &str) {
        self.0
            .lock()
            .unwrap()
            .notices
            .push(format!("quarantine:{owner:?}:{reason}"));
    }
    fn record(&mut self, reason: &str) {
        self.0
            .lock()
            .unwrap()
            .notices
            .push(format!("record:{reason}"));
    }
    fn save_warning(&mut self, reason: &str) {
        self.0
            .lock()
            .unwrap()
            .notices
            .push(format!("warning:{reason}"));
    }
}
fn fixture() -> Vec<u8> {
    let mut b = vec![0; 0x400];
    b[..2].copy_from_slice(b"MZ");
    for (at, v) in [
        (0x3c, 0x80u32),
        (0x98 + 56, 0x2000),
        (0x98 + 60, 0x200),
        (0x188 + 8, 0x200),
        (0x188 + 12, 0x1000),
        (0x188 + 16, 0x200),
        (0x188 + 20, 0x200),
    ] {
        b[at..at + 4].copy_from_slice(&v.to_le_bytes());
    }
    b[0x80..0x84].copy_from_slice(b"PE\0\0");
    for (at, v) in [
        (0x84, 0x8664u16),
        (0x86, 1),
        (0x94, 240),
        (0x96, 0x2000),
        (0x98, 0x20b),
        (0x98 + 68, 3),
    ] {
        b[at..at + 2].copy_from_slice(&v.to_le_bytes());
    }
    b
}
struct Dir(PathBuf);
impl Drop for Dir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn directory() -> Dir {
    static N: AtomicUsize = AtomicUsize::new(0);
    let p = std::env::temp_dir().join(format!(
        "sfi-enforcement-{}-{}",
        std::process::id(),
        N.fetch_add(1, Ordering::SeqCst)
    ));
    std::fs::create_dir(&p).unwrap();
    Dir(p)
}
fn registry() -> NativeContracts {
    NativeContracts::shipped().unwrap()
}
#[test]
fn already_present_runtime_is_wrong_backend_not_quarantine_and_continue() {
    let state = Arc::new(Mutex::new(State {
        existing: vec!["shroudforge-runtime.dll".into()],
        ..State::default()
    }));
    assert!(Enforcer::early(Fake(state.clone()), Fake(state.clone())).is_err());
    let s = state.lock().unwrap();
    assert_eq!(s.registered, 0);
    assert_eq!(s.loads, 0);
    assert!(s.notices[0].starts_with("self-hosted:"));
    assert!(!s.notices.iter().any(|s| s.starts_with("quarantine:")));
}
#[test]
fn register_before_load_and_first_loader_wins() {
    let dir = directory();
    let path = dir.0.join("entry.dll");
    std::fs::write(&path, fixture()).unwrap();
    let snap = Snapshot::capture(&path, "entry.dll").unwrap();
    let permit = LoadPermit::inspect(&snap, &[], &registry()).unwrap();
    let state = Arc::new(Mutex::new(State::default()));
    let mut enforcer = Enforcer::early(Fake(state.clone()), Fake(state.clone())).unwrap();
    assert_eq!(enforcer.load(7, &permit).unwrap(), LoadOutcome::Loaded(17));
    state.lock().unwrap().foreign = true;
    assert_eq!(
        enforcer.load(8, &permit).unwrap(),
        LoadOutcome::ForeignOwned(42)
    );
    assert_eq!(state.lock().unwrap().loads, 1);
}
#[test]
fn late_breach_quarantines_attributable_owner_and_warns_without_unload() {
    let state = Arc::new(Mutex::new(State::default()));
    let mut e = Enforcer::early(Fake(state.clone()), Fake(state.clone())).unwrap();
    state.lock().unwrap().events.push(Breach {
        owner: Some(7),
        module: concat!("kf", "c-runtime.dll").into(),
        reason: "late banned load".into(),
    });
    e.poll().unwrap();
    assert!(e.is_quarantined(7));
    assert!(!e.is_quarantined(8));
    let s = state.lock().unwrap();
    assert!(
        s.notices
            .iter()
            .any(|s| s.starts_with("quarantine:Some(7)"))
    );
    assert!(s.notices.iter().any(|s| s.starts_with("record:")));
    assert!(s.notices.iter().any(|s| s.contains("Do not save")));
}
#[test]
fn unattributed_breach_or_notification_loss_closes_whole_runtime() {
    let state = Arc::new(Mutex::new(State::default()));
    let mut e = Enforcer::early(Fake(state.clone()), Fake(state.clone())).unwrap();
    state.lock().unwrap().events.push(Breach {
        owner: None,
        module: "unknown".into(),
        reason: "notification queue overflow".into(),
    });
    e.poll().unwrap();
    assert!(e.is_quarantined(7) && e.is_quarantined(8));
}
#[test]
fn missing_registration_never_reaches_load() {
    let state = Arc::new(Mutex::new(State {
        fail_register: true,
        ..State::default()
    }));
    assert!(Enforcer::early(Fake(state.clone()), Fake(state.clone())).is_err());
    assert_eq!(state.lock().unwrap().loads, 0);
}

#[test]
fn post_load_audit_refuses_an_observed_nested_banned_dependency() {
    let dir = directory();
    let path = dir.0.join("entry.dll");
    std::fs::write(&path, fixture()).unwrap();
    let snapshot = Snapshot::capture(&path, "entry.dll").unwrap();
    let permit = LoadPermit::inspect(&snapshot, &[], &registry()).unwrap();
    let state = Arc::new(Mutex::new(State {
        post_load_banned: true,
        ..State::default()
    }));
    let mut enforcer = Enforcer::early(Fake(state.clone()), Fake(state.clone())).unwrap();
    assert!(enforcer.load(7, &permit).is_err());
    assert!(enforcer.is_quarantined(7));
    assert!(enforcer.load(8, &permit).is_err());
    assert_eq!(state.lock().unwrap().loads, 1);
    assert!(
        state
            .lock()
            .unwrap()
            .notices
            .iter()
            .any(|s| s.contains("Do not save"))
    );
}

fn importing(module: &str) -> Vec<u8> {
    let mut bytes = fixture();
    for (at, value) in [
        (0x98 + 108, 16u32),
        (0x98 + 120, 0x1000),
        (0x98 + 124, 40),
        (0x200, 0x1100),
        (0x20c, 0x1060),
        (0x210, 0x1100),
    ] {
        bytes[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }
    bytes[0x260..0x260 + module.len()].copy_from_slice(module.as_bytes());
    bytes[0x300..0x308].copy_from_slice(&((1u64 << 63) | 1).to_le_bytes());
    bytes
}

#[test]
fn missing_package_dependency_cannot_get_a_permit() {
    let dir = directory();
    let path = dir.0.join("entry.dll");
    std::fs::write(&path, importing("helper.dll")).unwrap();
    let entry = Snapshot::capture(&path, "entry.dll").unwrap();
    assert!(LoadPermit::inspect(&entry, &[], &registry()).is_err());
}

#[test]
fn preloaded_dependency_with_different_content_refuses_before_entry_runs() {
    let dir = directory();
    let path = dir.0.join("entry.dll");
    std::fs::write(&path, importing("helper.dll")).unwrap();
    let helper = dir.0.join("helper.dll");
    std::fs::write(&helper, fixture()).unwrap();
    let entry = Snapshot::capture(&path, "entry.dll").unwrap();
    let dependencies = [Snapshot::capture(&helper, "helper.dll").unwrap()];
    let permit = LoadPermit::inspect(&entry, &dependencies, &registry()).unwrap();
    let state = Arc::new(Mutex::new(State {
        dependency_collision: true,
        ..State::default()
    }));
    let mut e = Enforcer::early(Fake(state.clone()), Fake(state.clone())).unwrap();
    assert!(e.load(7, &permit).is_err());
    assert_eq!(state.lock().unwrap().loads, 0);
}

#[test]
fn native_load_fault_quarantines_warns_and_latches_without_retry() {
    let dir = directory();
    let path = dir.0.join("entry.dll");
    std::fs::write(&path, fixture()).unwrap();
    let entry = Snapshot::capture(&path, "entry.dll").unwrap();
    let permit = LoadPermit::inspect(&entry, &[], &registry()).unwrap();
    let state = Arc::new(Mutex::new(State {
        load_fault: true,
        ..State::default()
    }));
    let mut e = Enforcer::early(Fake(state.clone()), Fake(state.clone())).unwrap();
    assert!(e.load(7, &permit).is_err());
    assert!(e.is_quarantined(7));
    assert!(
        state
            .lock()
            .unwrap()
            .notices
            .iter()
            .any(|n| n.contains("Do not save"))
    );
    state.lock().unwrap().load_fault = false;
    assert!(e.load(8, &permit).is_err());
    assert_eq!(state.lock().unwrap().loads, 1);
}

#[test]
fn clean_pinned_init_failure_does_not_block_an_independent_package() {
    let dir = directory();
    let first = dir.0.join("first.dll");
    let second = dir.0.join("second.dll");
    std::fs::write(&first, fixture()).unwrap();
    let mut bytes = fixture();
    bytes[0x3ff] = 1;
    std::fs::write(&second, bytes).unwrap();
    let first = Snapshot::capture(&first, "first.dll").unwrap();
    let second = Snapshot::capture(&second, "second.dll").unwrap();
    let a = LoadPermit::inspect(&first, &[], &registry()).unwrap();
    let b = LoadPermit::inspect(&second, &[], &registry()).unwrap();
    let state = Arc::new(Mutex::new(State {
        clean_init_failure: true,
        ..State::default()
    }));
    let mut e = Enforcer::early(Fake(state.clone()), Fake(state.clone())).unwrap();
    assert!(e.load(7, &a).is_err());
    assert!(e.is_quarantined(7));
    assert!(!e.is_quarantined(8));
    state.lock().unwrap().clean_init_failure = false;
    assert_eq!(e.load(8, &b).unwrap(), LoadOutcome::Loaded(17));
    assert!(e.load(7, &a).is_err());
    assert_eq!(state.lock().unwrap().loads, 2);
}

#[test]
fn a_replaced_module_path_is_not_foreign_ownership_proof() {
    let dir = directory();
    let path = dir.0.join("entry.dll");
    std::fs::write(&path, fixture()).unwrap();
    let entry = Snapshot::capture(&path, "entry.dll").unwrap();
    let permit = LoadPermit::inspect(&entry, &[], &registry()).unwrap();
    let state = Arc::new(Mutex::new(State {
        foreign: true,
        unverifiable_mapping: true,
        ..State::default()
    }));
    let mut e = Enforcer::early(Fake(state.clone()), Fake(state.clone())).unwrap();
    assert!(e.load(7, &permit).is_err());
    assert_eq!(state.lock().unwrap().loads, 0);
}

#[test]
fn reset_failure_decision_and_healthy_supervisor_preserve_the_game() {
    use kiln_sfi::native_enforcement::ParkedFault;
    unsafe extern "C" {
        fn sfi_guard_decide(code: u32, reset_ok: u32, interpreter: u32, owner: u64) -> u32;
    }
    assert_eq!(unsafe { sfi_guard_decide(0x80000003, 0, 1, 7) }, 1);
    assert_eq!(unsafe { sfi_guard_decide(0x80000004, 0, 1, 7) }, 1);
    assert_eq!(unsafe { sfi_guard_decide(0xc00000fd, 1, 1, 7) }, 0);
    assert_eq!(unsafe { sfi_guard_decide(0xc00000fd, 0, 1, 7) }, 2);
    assert_eq!(unsafe { sfi_guard_decide(0xc00000fd, 0, 0, 7) }, 3);
    assert_eq!(unsafe { sfi_guard_decide(0xc00000fd, 0, 0, 0) }, 2);
    for all in [false, true] {
        let (tx, rx) = std::sync::mpsc::channel();
        let state = Arc::new(Mutex::new(State {
            parked_receiver: Some(rx),
            ..State::default()
        }));
        let enforcer = Enforcer::early(Fake(state.clone()), Fake(state.clone())).unwrap();
        tx.send(ParkedFault {
            owner: Some(7),
            all,
            reason: "injected _resetstkoflw failure; native thread parked".into(),
        })
        .unwrap();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
        loop {
            if state
                .lock()
                .unwrap()
                .notices
                .iter()
                .any(|s| s.starts_with("warning:") && s.contains("runtime stopped, do not save"))
            {
                break;
            }
            assert!(
                std::time::Instant::now() < deadline,
                "healthy-thread warning never delivered"
            );
            std::thread::yield_now();
        }
        assert!(enforcer.is_quarantined(7));
        assert_eq!(enforcer.is_quarantined(8), all);
        drop(enforcer);
        drop(tx);
    }
}

#[test]
fn os_converted_exceptions_are_faults_but_missing_modules_are_refusals() {
    for (exception, error) in [
        (0xe0424242, 0),
        (0, 0xe0424242),
        (0, 0xc0000005),
        (0, 1114),
        (0, 998),
        (0, 0xc0000142),
    ] {
        assert!(matches!(
            BackendLoadError::from_load_status(exception, error, "fixture".into()),
            BackendLoadError::NativeFault { .. }
        ));
    }
    assert!(matches!(
        BackendLoadError::from_load_evidence(0, 126, true, 0, "notification evidence".into()),
        BackendLoadError::NativeFault { .. }
    ));
    assert!(matches!(
        BackendLoadError::from_load_evidence(0, 0, true, 0xc0000001u32 as i32, "pin failed".into()),
        BackendLoadError::NativeFault { .. }
    ));
    for error in [2, 3, 126, 127, 193, 0xc0000135, 0xc0000139] {
        assert!(matches!(
            BackendLoadError::from_load_status(0, error, "fixture".into()),
            BackendLoadError::LoaderFailure { .. }
        ));
    }
}

#[test]
fn ordinary_loader_failure_is_a_refusal_without_runtime_quarantine() {
    let dir = directory();
    let path = dir.0.join("entry.dll");
    std::fs::write(&path, fixture()).unwrap();
    let entry = Snapshot::capture(&path, "entry.dll").unwrap();
    let permit = LoadPermit::inspect(&entry, &[], &registry()).unwrap();
    let state = Arc::new(Mutex::new(State {
        ordinary_failure: true,
        ..State::default()
    }));
    let mut e = Enforcer::early(Fake(state.clone()), Fake(state.clone())).unwrap();
    assert!(e.load(7, &permit).is_err());
    assert!(!e.is_quarantined(7));
    assert!(
        !state
            .lock()
            .unwrap()
            .notices
            .iter()
            .any(|n| n.contains("Do not save"))
    );
    state.lock().unwrap().ordinary_failure = false;
    assert!(e.load(8, &permit).is_ok());
}

#[cfg(unix)]
#[test]
fn changed_hash_or_identity_refuses_before_any_load() {
    let dir = directory();
    let path = dir.0.join("entry.dll");
    std::fs::write(&path, fixture()).unwrap();
    let snap = Snapshot::capture(&path, "entry.dll").unwrap();
    let permit = LoadPermit::inspect(&snap, &[], &registry()).unwrap();
    let state = Arc::new(Mutex::new(State::default()));
    let mut e = Enforcer::early(Fake(state.clone()), Fake(state.clone())).unwrap();
    let mut changed = fixture();
    changed[0x3ff] = 1;
    std::fs::write(&path, changed).unwrap();
    assert!(e.load(7, &permit).is_err());
    assert_eq!(state.lock().unwrap().loads, 0);
    std::fs::remove_file(&path).unwrap();
    std::fs::write(&path, fixture()).unwrap();
    assert!(e.load(7, &permit).is_err());
    assert_eq!(state.lock().unwrap().loads, 0);
}
