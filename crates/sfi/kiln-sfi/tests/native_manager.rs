// SPDX-License-Identifier: MIT
//! Guarded, ordered native preparation with exactly one terminal record per DLL, on the native-load
//! enforcement with a fake loader backend and a fake startup witness.
use kiln_sfi::{
    launch_plan::{LaunchPlan, ValidationContext},
    native_contracts::NativeContracts,
    native_enforcement::{Backend, BackendLoadError, Breach, Enforcer, Sink, Snapshot},
    native_manager::{
        arming::{self, Armed, Deadline, LoaderBody, WitnessPort},
        executor::{
            NativeRun, NativeTerminal, Outcome, RECHECK_ALLOWANCE, RECHECK_UNANSWERED,
            STARTUP_NATIVE_DEADLINE, Witness, WitnessToken,
        },
        plan::{NativePlan, PlannedDll, PlannedPackage},
        selection::{Mode, NativePermission, NativePolicy},
    },
};
use serde_json::json;
use std::{
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicUsize, Ordering},
        mpsc,
    },
    time::Duration,
};

/// A liveness bound, not a latency claim: how long a wait for something that must happen holds before the test
/// reports a hang. A passing wait returns as soon as its condition holds, so this sits far above any delay that
/// scheduling alone can cause on a loaded machine (300 s, as worker.rs and the lifecycle bodies).
const WAIT: Duration = Duration::from_secs(300);
const PLAN: [u8; 16] = [7; 16];

#[derive(Default)]
struct State {
    loads: Vec<String>,
    foreign: Vec<String>,
    clean_failure: Vec<String>,
    global_fault: Vec<String>,
    refuse: Vec<String>,
    unattributed_breach_after: Vec<String>,
    mutates_input: Vec<String>,
    block: Option<(String, mpsc::Receiver<()>)>,
    events: Vec<Breach>,
    /// Calls to `owner_failures` (Enforcer::early polls once, every load polls before it binds).
    polls: usize,
    /// Block the poll with this call number: inside the load, before anything is committed.
    block_poll: Option<(usize, mpsc::Receiver<()>)>,
    /// Loader notifications registered, fault sources asked for, and the order of registrations and loads.
    registers: usize,
    fault_sources: usize,
    calls: Vec<String>,
    /// This load returns only once this instant has passed (a load that finishes at the deadline).
    finish_at: Option<(String, std::time::Instant)>,
    /// Review: the enforcement's record lines (kiln.log in production).
    records: Vec<String>,
}
#[derive(Clone, Default)]
struct Fake(Arc<Mutex<State>>, Arc<AtomicBool>);
fn name(path: &Path) -> String {
    path.file_name().unwrap().to_str().unwrap().to_owned()
}
impl Backend for Fake {
    fn modules(&mut self) -> Result<Vec<String>, String> {
        Ok(Vec::new())
    }
    fn register(&mut self) -> Result<(), String> {
        let mut state = self.0.lock().unwrap();
        state.registers += 1;
        state.calls.push("register".into());
        Ok(())
    }
    fn fault_source(
        &mut self,
    ) -> Result<Option<Box<dyn kiln_sfi::native_enforcement::FaultSource>>, String> {
        self.0.lock().unwrap().fault_sources += 1;
        Ok(None)
    }
    fn events(&mut self) -> Result<Vec<Breach>, String> {
        Ok(std::mem::take(&mut self.0.lock().unwrap().events))
    }
    fn owner_failures(&mut self) -> Result<Vec<Breach>, String> {
        let blocker = {
            let mut state = self.0.lock().unwrap();
            state.polls += 1;
            match &state.block_poll {
                Some((at, _)) if *at == state.polls => state.block_poll.take().map(|(_, rx)| rx),
                _ => None,
            }
        };
        if let Some(rx) = blocker {
            rx.recv().unwrap();
        }
        Ok(Vec::new())
    }
    fn first_loader(&mut self, image: &Snapshot) -> Result<Option<usize>, String> {
        let state = self.0.lock().unwrap();
        Ok(state.foreign.contains(&name(image.path())).then_some(42))
    }
    fn load(&mut self, path: &Path, owner: u64) -> Result<usize, BackendLoadError> {
        let file = name(path);
        let blocker = {
            let mut state = self.0.lock().unwrap();
            state.loads.push(file.clone());
            state.calls.push(format!("load:{file}"));
            match &state.block {
                Some((blocked, _)) if *blocked == file => state.block.take().map(|(_, rx)| rx),
                _ => None,
            }
        };
        if let Some(rx) = blocker {
            rx.recv().unwrap();
        }
        let finish_at = {
            let state = self.0.lock().unwrap();
            state
                .finish_at
                .as_ref()
                .filter(|(at, _)| *at == file)
                .map(|(_, when)| *when)
        };
        if let Some(when) = finish_at {
            while std::time::Instant::now() < when {
                std::hint::spin_loop();
            }
        }
        let mut state = self.0.lock().unwrap();
        if state.mutates_input.contains(&file) {
            self.1.store(true, Ordering::SeqCst);
        }
        if state.unattributed_breach_after.contains(&file) {
            state.events.push(Breach {
                owner: None,
                module: concat!("kf", "c-runtime.dll").into(),
                reason: "late banned load".into(),
            });
        }
        if state.clean_failure.contains(&file) {
            return Err(BackendLoadError::from_load_evidence(
                0,
                1114,
                true,
                0,
                "DllMain returned FALSE".into(),
            ));
        }
        if state.global_fault.contains(&file) {
            return Err(BackendLoadError::NativeFault {
                exception: 0xc0000005,
                loader_error: 0,
                may_have_run: true,
                pin_status: 0,
            });
        }
        if state.refuse.contains(&file) {
            return Err(BackendLoadError::LoaderFailure {
                code: 126,
                message: "missing dependency".into(),
            });
        }
        let _ = owner;
        Ok(100 + state.loads.len())
    }
    fn validate_loaded(&mut self, _: usize, _: &Snapshot) -> Result<(), String> {
        Ok(())
    }
}
impl Sink for Fake {
    fn self_hosted(&mut self, _: &str) {}
    fn quarantine(&mut self, _: Option<u64>, _: &str) {}
    fn record(&mut self, reason: &str) {
        self.0.lock().unwrap().records.push(reason.to_owned());
    }
    fn save_warning(&mut self, _: &str) {}
}
struct FakeWitness {
    ready: Result<WitnessToken, String>,
    changed: Arc<AtomicBool>,
    readies: AtomicUsize,
}
impl Witness for FakeWitness {
    fn ready(&self) -> Result<WitnessToken, String> {
        self.readies.fetch_add(1, Ordering::SeqCst);
        self.ready.clone()
    }
    fn recheck(&self, token: &WitnessToken) -> Result<(), String> {
        assert_eq!(token.plan_id, PLAN);
        if self.changed.load(Ordering::SeqCst) {
            Err("witnessed archive input changed after the native loads".into())
        } else {
            Ok(())
        }
    }
}
fn witness(changed: &Arc<AtomicBool>) -> Arc<FakeWitness> {
    Arc::new(FakeWitness {
        ready: Ok(WitnessToken {
            plan_id: PLAN,
            revision: 1,
        }),
        changed: changed.clone(),
        readies: AtomicUsize::new(0),
    })
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
struct Dir(PathBuf);
impl Drop for Dir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn directory() -> Dir {
    static N: AtomicUsize = AtomicUsize::new(0);
    let p = std::env::temp_dir().join(format!(
        "sfi-native-manager-{}-{}",
        std::process::id(),
        N.fetch_add(1, Ordering::SeqCst)
    ));
    std::fs::create_dir_all(&p).unwrap();
    Dir(p)
}
/// Writes `mods/<dir>/<file>` with distinct bytes (a marker byte past the PE data) and plans it.
fn planned(game: &Path, dir: &str, file: &str, bytes: Vec<u8>) -> PlannedDll {
    let path = game.join("mods").join(dir).join(file);
    std::fs::create_dir_all(path.parent().unwrap()).unwrap();
    let mut bytes = bytes;
    let tag = format!("{dir}/{file}");
    bytes[0x3a0..0x3a0 + tag.len()].copy_from_slice(tag.as_bytes());
    std::fs::write(&path, &bytes).unwrap();
    PlannedDll {
        status_id: format!("dll:{dir}:{file}"),
        path: file.into(),
        sha256: NativeContracts::sha256(&bytes),
        mode: Mode::Library,
        source: "ini".into(),
        bridge_abi: 0,
    }
}
fn package(game: &Path, id: &str, dlls: Vec<PlannedDll>, deps: &[&str]) -> PlannedPackage {
    PlannedPackage {
        mod_id: id.into(),
        directory: id.into(),
        root: game.join("mods").join(id),
        dlls,
        dependencies: deps.iter().map(|d| (*d).to_owned()).collect(),
    }
}
fn run(
    plan: NativePlan,
    fake: &Fake,
    witness: Arc<FakeWitness>,
) -> (NativeTerminal, Arc<NativeRun>) {
    run_with(plan, fake, witness, NativePermission::ALWAYS)
}
fn run_with(
    plan: NativePlan,
    fake: &Fake,
    witness: Arc<FakeWitness>,
    permission: NativePermission,
) -> (NativeTerminal, Arc<NativeRun>) {
    let run = NativeRun::new(plan, witness, permission);
    let mut enforcer = Enforcer::early(fake.clone(), fake.clone()).unwrap();
    run.execute(&mut enforcer, &NativeContracts::shipped().unwrap());
    (run.wait(STARTUP_NATIVE_DEADLINE), run)
}
fn outcomes(terminal: &NativeTerminal) -> Vec<(String, String)> {
    terminal
        .records
        .iter()
        .map(|r| {
            let kind = match &r.outcome {
                Outcome::Loaded { .. } => "loaded",
                Outcome::ForeignOwned { .. } => "foreign",
                Outcome::Refused(_) => "refused",
                Outcome::Faulted(_) => "faulted",
                Outcome::TimedOut(_) => "timed-out",
                Outcome::Withdrawn(_) => "withdrawn",
            };
            (r.status_id.clone(), kind.to_owned())
        })
        .collect()
}
fn pairs(list: &[(&str, &str)]) -> Vec<(String, String)> {
    list.iter()
        .map(|(a, b)| ((*a).to_owned(), (*b).to_owned()))
        .collect()
}

#[test]
fn no_witness_means_every_dll_refused_and_no_backend_call() {
    let game = directory();
    let fake = Fake::default();
    for ready in [
        Err("guard not taken".to_owned()),
        Ok(WitnessToken {
            plan_id: [9; 16],
            revision: 1,
        }),
    ] {
        let plan = NativePlan {
            plan_id: PLAN,
            packages: vec![package(
                &game.0,
                "a",
                vec![planned(&game.0, "a", "a.dll", fixture())],
                &[],
            )],
        };
        let w = Arc::new(FakeWitness {
            ready,
            changed: Arc::new(AtomicBool::new(false)),
            readies: AtomicUsize::new(0),
        });
        let (terminal, _) = run(plan, &fake, w);
        assert_eq!(outcomes(&terminal), pairs(&[("dll:a:a.dll", "refused")]));
        assert!(terminal.coherent.is_err());
        assert!(!terminal.runs_whole("a"));
    }
    assert!(fake.0.lock().unwrap().loads.is_empty());
}

#[test]
fn ordered_loads_capture_the_package_closure_and_a_failure_withdraws_package_and_dependents() {
    let game = directory();
    let fake = Fake::default();
    fake.0.lock().unwrap().clean_failure = vec!["bad.dll".into()];
    let mut a_entry = planned(&game.0, "a", "entry.dll", importing("helper.dll"));
    a_entry.source = "selection".into();
    planned(&game.0, "a", "helper.dll", fixture());
    let plan = NativePlan {
        plan_id: PLAN,
        packages: vec![
            package(
                &game.0,
                "a",
                vec![
                    a_entry,
                    planned(&game.0, "a", "bad.dll", fixture()),
                    planned(&game.0, "a", "third.dll", fixture()),
                ],
                &[],
            ),
            package(
                &game.0,
                "b",
                vec![planned(&game.0, "b", "b.dll", fixture())],
                &[],
            ),
            package(
                &game.0,
                "c",
                vec![planned(&game.0, "c", "c.dll", fixture())],
                &["a"],
            ),
            // Lua only: it has no DLL, but it depends on a withdrawn package.
            package(&game.0, "d", Vec::new(), &["c"]),
        ],
    };
    let (terminal, run) = run(plan, &fake, witness(&Arc::new(AtomicBool::new(false))));
    assert_eq!(
        outcomes(&terminal),
        pairs(&[
            ("dll:a:entry.dll", "loaded"),
            ("dll:a:bad.dll", "faulted"),
            ("dll:a:third.dll", "withdrawn"),
            ("dll:b:b.dll", "loaded"),
            ("dll:c:c.dll", "withdrawn"),
        ])
    );
    // One attempt each, in plan order; the withdrawn ones never reach the loader.
    assert_eq!(
        fake.0.lock().unwrap().loads,
        ["entry.dll", "bad.dll", "b.dll"]
    );
    let withdrawn: Vec<_> = terminal
        .withdrawn
        .iter()
        .map(|w| (w.mod_id.as_str(), w.caused_by.clone()))
        .collect();
    assert_eq!(
        withdrawn,
        [
            ("a", vec![]),
            ("c", vec!["a".to_owned()]),
            ("d", vec!["c".to_owned(), "a".to_owned()])
        ]
    );
    assert!(terminal.runs_whole("b"));
    assert!(terminal.coherent.is_ok());
    assert!(!terminal.deadline_passed);
    assert_eq!(
        terminal.records[0].actual_sha256.as_deref(),
        Some(terminal.records[0].planned_sha256.as_str())
    );
    // register_dll reuse looks the record up by its process key.
    assert!(
        terminal
            .record_for_key("A/Entry.DLL")
            .is_some_and(|r| r.outcome.admitted())
    );
    // A second execute is a no-op: nothing is retried or initialized again.
    let mut enforcer = Enforcer::early(fake.clone(), fake.clone()).unwrap();
    run.execute(&mut enforcer, &NativeContracts::shipped().unwrap());
    assert_eq!(fake.0.lock().unwrap().loads.len(), 3);
    assert_eq!(run.wait(Duration::ZERO), terminal);
}

#[test]
fn a_missing_package_dependency_is_refused_before_the_loader() {
    let game = directory();
    let fake = Fake::default();
    let plan = NativePlan {
        plan_id: PLAN,
        packages: vec![package(
            &game.0,
            "a",
            vec![planned(&game.0, "a", "entry.dll", importing("helper.dll"))],
            &[],
        )],
    };
    let (terminal, _) = run(plan, &fake, witness(&Arc::new(AtomicBool::new(false))));
    assert!(
        matches!(&terminal.records[0].outcome, Outcome::Refused(r) if r.contains("helper.dll"))
    );
    assert!(fake.0.lock().unwrap().loads.is_empty());
}

#[test]
fn identity_mode_source_and_bridge_refuse_before_any_load() {
    let game = directory();
    let fake = Fake::default();
    let mut changed = planned(&game.0, "a", "a.dll", fixture());
    changed.sha256 = "0".repeat(64);
    let mut dinput = planned(&game.0, "b", "b.dll", fixture());
    dinput.mode = Mode::Dinput8;
    let mut chain = planned(&game.0, "c", "c.dll", fixture());
    chain.source = "chain".into();
    let mut bridge = planned(&game.0, "d", "d.dll", fixture());
    bridge.bridge_abi = 2;
    let mut missing = planned(&game.0, "e", "e.dll", fixture());
    missing.path = "gone.dll".into();
    let plan = NativePlan {
        plan_id: PLAN,
        packages: ["a", "b", "c", "d", "e"]
            .iter()
            .zip([changed, dinput, chain, bridge, missing])
            .map(|(id, dll)| package(&game.0, id, vec![dll], &[]))
            .collect(),
    };
    let (terminal, _) = run(plan, &fake, witness(&Arc::new(AtomicBool::new(false))));
    let reasons: Vec<_> = terminal
        .records
        .iter()
        .map(|r| match &r.outcome {
            Outcome::Refused(reason) => reason.clone(),
            other => panic!("{other:?}"),
        })
        .collect();
    assert!(reasons[0].contains("changed since the launch plan pinned it"));
    assert!(reasons[1].contains("provider (kiln.proxy), which"));
    assert!(reasons[2].contains("Kiln's proxy route"));
    assert!(reasons[3].contains("kiln-sfi.sf18"));
    assert!(reasons[4].contains("gone.dll"));
    assert!(terminal.records[0].actual_sha256.is_some());
    assert!(fake.0.lock().unwrap().loads.is_empty());
    assert_eq!(terminal.withdrawn.len(), 5);
}

#[test]
fn foreign_owned_is_admitted_without_a_second_initialization() {
    let game = directory();
    let fake = Fake::default();
    fake.0.lock().unwrap().foreign = vec!["shared.dll".into()];
    let plan = NativePlan {
        plan_id: PLAN,
        packages: vec![package(
            &game.0,
            "a",
            vec![planned(&game.0, "a", "shared.dll", fixture())],
            &[],
        )],
    };
    let (terminal, _) = run(plan, &fake, witness(&Arc::new(AtomicBool::new(false))));
    assert_eq!(
        terminal.records[0].outcome,
        Outcome::ForeignOwned { module: 42 }
    );
    assert!(terminal.runs_whole("a"));
    assert!(fake.0.lock().unwrap().loads.is_empty());
}

#[test]
fn an_ordinary_refusal_withdraws_only_its_package_and_a_global_breach_stops_the_rest() {
    let game = directory();
    let fake = Fake::default();
    fake.0.lock().unwrap().refuse = vec!["a.dll".into()];
    fake.0.lock().unwrap().unattributed_breach_after = vec!["b.dll".into()];
    let plan = NativePlan {
        plan_id: PLAN,
        packages: vec![
            package(
                &game.0,
                "a",
                vec![planned(&game.0, "a", "a.dll", fixture())],
                &[],
            ),
            package(
                &game.0,
                "b",
                vec![planned(&game.0, "b", "b.dll", fixture())],
                &[],
            ),
            package(
                &game.0,
                "c",
                vec![planned(&game.0, "c", "c.dll", fixture())],
                &[],
            ),
        ],
    };
    let (terminal, _) = run(plan, &fake, witness(&Arc::new(AtomicBool::new(false))));
    assert_eq!(
        outcomes(&terminal),
        pairs(&[
            ("dll:a:a.dll", "refused"),
            ("dll:b:b.dll", "faulted"),
            ("dll:c:c.dll", "refused"),
        ])
    );
    assert!(matches!(&terminal.records[2].outcome, Outcome::Refused(r) if r.contains("breached")));
    assert_eq!(fake.0.lock().unwrap().loads, ["a.dll", "b.dll"]);
}

#[test]
fn the_deadline_makes_unfinished_dlls_terminal_and_a_late_load_is_never_admitted() {
    let game = directory();
    let fake = Fake::default();
    let (release, gate) = mpsc::channel();
    fake.0.lock().unwrap().block = Some(("slow.dll".into(), gate));
    let plan = NativePlan {
        plan_id: PLAN,
        packages: vec![
            package(
                &game.0,
                "a",
                vec![planned(&game.0, "a", "slow.dll", fixture())],
                &[],
            ),
            package(
                &game.0,
                "b",
                vec![planned(&game.0, "b", "b.dll", fixture())],
                &[],
            ),
        ],
    };
    let w = witness(&Arc::new(AtomicBool::new(false)));
    let native = NativeRun::new(plan, w, NativePermission::ALWAYS);
    // An open run may start a load any moment
    assert!(native.may_be_loading());
    let worker = {
        let native = native.clone();
        let fake = fake.clone();
        std::thread::spawn(move || {
            let mut enforcer = Enforcer::early(fake.clone(), fake).unwrap();
            native.execute(&mut enforcer, &NativeContracts::shipped().unwrap());
        })
    };
    let started = std::time::Instant::now();
    while fake.0.lock().unwrap().loads.is_empty() {
        assert!(
            started.elapsed() < WAIT,
            "the loader never reached slow.dll"
        );
        std::thread::yield_now();
    }
    assert!(native.may_be_loading(), "inside slow.dll's load");
    let terminal = native.wait(Duration::from_millis(150));
    assert!(terminal.deadline_passed);
    // Concluded at the deadline with slow.dll's load still running: still loading (its initializer may hold the
    // loader lock), until that load returns
    assert!(
        native.may_be_loading(),
        "the deadline left slow.dll loading"
    );
    assert_eq!(
        terminal.records[0].outcome,
        Outcome::TimedOut("Native DLL 'slow.dll' did not finish initialization within 15 seconds. Disable it in DLL Manager and restart the game".into())
    );
    assert!(
        matches!(&terminal.records[1].outcome, Outcome::TimedOut(r) if r.starts_with("not started"))
    );
    assert!(!terminal.runs_whole("a") && !terminal.runs_whole("b"));
    release.send(()).unwrap();
    worker.join().unwrap();
    assert!(
        !native.may_be_loading(),
        "the late load returned and nothing else starts"
    );
    // The slow load finished after the deadline: logged, never admitted, and nothing new started.
    assert_eq!(fake.0.lock().unwrap().loads, ["slow.dll"]);
    let late = native.late_results();
    assert_eq!(late.len(), 1);
    assert_eq!(late[0].0, "dll:a:slow.dll");
    assert!(late[0].1.admitted());
    assert_eq!(native.wait(STARTUP_NATIVE_DEADLINE), terminal);
}

#[test]
fn a_dll_that_changes_the_witnessed_input_makes_the_run_incoherent() {
    let game = directory();
    for mutate in [false, true] {
        let fake = Fake::default();
        if mutate {
            fake.0.lock().unwrap().mutates_input = vec!["a.dll".into()];
        }
        let plan = NativePlan {
            plan_id: PLAN,
            packages: vec![package(
                &game.0,
                "a",
                vec![planned(&game.0, "a", "a.dll", fixture())],
                &[],
            )],
        };
        let (terminal, _) = run(plan, &fake, witness(&fake.1));
        assert!(terminal.records[0].outcome.admitted());
        assert_eq!(terminal.coherent.is_err(), mutate);
    }
}

#[test]
fn the_native_plan_follows_the_frozen_launch_plan() {
    let h = "a".repeat(64);
    let dll = |path: &str, mode: &str, source: &str| json!({"path":path,"sha256":"e".repeat(64),"mode":mode,"source":source,"required":true,"gate":"pass","gateProof_sha256":h,"bridgeAbi":0});
    let package = |id: &str, root: &str, deps: serde_json::Value, dlls: serde_json::Value| {
        json!({"id":id,"version":"1.0.0","kind":"shroudforge","root":root,
            "content_sha256":h,"config_sha256":h,"enabled":true,"targets":["client"],"capabilities":["runtime"],
            "dependencies":deps,"requiredApis":[],"services":[],"requires180":false,"saveSensitive":false,"decision":"candidate","reasons":[],
            "data":{"required":false,"admission":"not-needed"},"lua":{"required":true,"admission":"admit"},"dlls":dlls})
    };
    let plan = json!({"format":1,"minReader":1,"plan_id":"07070707-0707-0707-0707-070707070707","launch_id":null,
        "startupDialect":"none","writer":{"component":"kiln-manage","version":"0.1.0"},
        "profile":{"id":"test","generation":1,"mode":"active","semantic_sha256":h},
        "game":{"side":"client","exe_sha256":h,"archiveBase_sha256":h,"archiveInput_sha256":h},
        "backend":{"requested":"kiln","selected":"kiln","fallbackOrder":[],"dataWriter":"kiln-eml","selectionReason":"test"},
        "contract":{"apiVersion":"1.7.9","sourceCommit":"b".repeat(40),"upstream180Final":false,"inventory_sha256":h,"parity_sha256":h,"serviceContracts_sha256":h},
        "fingerprints":{"packages_sha256":h,"settings_sha256":h,"chain_sha256":h,"loaderInventory_sha256":h,"compat_sha256":h,"releasePolicy_sha256":h,"placementReceipts_sha256":h},
        "packages":[
            package("late","mods/Late",json!([{"id":"early","range":"^1","optional":false,"included":true},{"id":"opt","range":"*","optional":true,"included":false}]),
                json!([dll("Bin/X.dll","library","selection"),dll("y.dll","dinput8","lua-registration")])),
            package("early","mods/early",json!([]),json!([dll("e.dll","library","ini")]))
        ],
        "loadOrder":["early","late"],"warnings":[]});
    let context = ValidationContext {
        exe_sha256: h.clone(),
        side: "client".into(),
        upstream_180_final: false,
    };
    let plan = LaunchPlan::parse(&serde_json::to_vec(&plan).unwrap(), &context).unwrap();
    let native = NativePlan::from_launch_plan(&plan, Path::new("/game")).unwrap();
    assert_eq!(native.plan_id, PLAN);
    let order: Vec<_> = native.packages.iter().map(|p| p.mod_id.as_str()).collect();
    assert_eq!(order, ["early", "late"]);
    let late = &native.packages[1];
    assert_eq!(late.directory, "Late");
    assert_eq!(late.root, Path::new("/game/mods/Late"));
    assert_eq!(late.dependencies, ["early"]);
    assert_eq!(
        late.dlls
            .iter()
            .map(|d| (d.status_id.as_str(), d.mode, d.source.as_str()))
            .collect::<Vec<_>>(),
        [
            ("dll:late:bin/x.dll", Mode::Library, "selection"),
            ("dll:late:y.dll", Mode::Dinput8, "lua-registration")
        ]
    );
}

#[test]
fn execute_runs_once_even_before_the_wait() {
    let game = directory();
    let fake = Fake::default();
    let plan = NativePlan {
        plan_id: PLAN,
        packages: vec![package(
            &game.0,
            "a",
            vec![planned(&game.0, "a", "a.dll", fixture())],
            &[],
        )],
    };
    let w = witness(&Arc::new(AtomicBool::new(false)));
    let native = NativeRun::new(plan, w.clone(), NativePermission::ALWAYS);
    let mut enforcer = Enforcer::early(fake.clone(), fake.clone()).unwrap();
    let registry = NativeContracts::shipped().unwrap();
    native.execute(&mut enforcer, &registry);
    native.execute(&mut enforcer, &registry);
    assert_eq!(fake.0.lock().unwrap().loads, ["a.dll"]);
    assert_eq!(w.readies.load(Ordering::SeqCst), 1);
    assert!(
        native.wait(STARTUP_NATIVE_DEADLINE).records[0]
            .outcome
            .admitted()
    );
}

#[test]
fn a_lua_only_package_carries_a_withdrawal_to_its_dependents_before_their_dlls_load() {
    // b fails; a has no DLL but depends on b; c's DLL needs a. c must not initialize.
    let game = directory();
    let fake = Fake::default();
    fake.0.lock().unwrap().refuse = vec!["b.dll".into()];
    let plan = NativePlan {
        plan_id: PLAN,
        packages: vec![
            package(
                &game.0,
                "b",
                vec![planned(&game.0, "b", "b.dll", fixture())],
                &[],
            ),
            package(&game.0, "a", Vec::new(), &["b"]),
            package(
                &game.0,
                "c",
                vec![planned(&game.0, "c", "c.dll", fixture())],
                &["a"],
            ),
        ],
    };
    let (terminal, _) = run(plan, &fake, witness(&Arc::new(AtomicBool::new(false))));
    assert_eq!(
        outcomes(&terminal),
        pairs(&[("dll:b:b.dll", "refused"), ("dll:c:c.dll", "withdrawn")])
    );
    assert!(
        matches!(&terminal.records[1].outcome, Outcome::Withdrawn(r) if r.contains("depends on withdrawn package a"))
    );
    assert_eq!(fake.0.lock().unwrap().loads, ["b.dll"]);
    let withdrawn: Vec<_> = terminal
        .withdrawn
        .iter()
        .map(|w| (w.mod_id.as_str(), w.caused_by.clone()))
        .collect();
    assert_eq!(
        withdrawn,
        [
            ("b", vec![]),
            ("a", vec!["b".to_owned()]),
            ("c", vec!["a".to_owned(), "b".to_owned()])
        ]
    );
}

#[test]
fn a_dll_still_being_prepared_at_the_deadline_never_starts_loading() {
    let game = directory();
    let fake = Fake::default();
    let (release, gate) = mpsc::channel();
    // Poll 1 is Enforcer::early; poll 2 is a.dll's own load, before it binds or calls the loader.
    fake.0.lock().unwrap().block_poll = Some((2, gate));
    let plan = NativePlan {
        plan_id: PLAN,
        packages: vec![package(
            &game.0,
            "a",
            vec![planned(&game.0, "a", "a.dll", fixture())],
            &[],
        )],
    };
    let native = NativeRun::new(
        plan,
        witness(&Arc::new(AtomicBool::new(false))),
        NativePermission::ALWAYS,
    );
    let worker = {
        let native = native.clone();
        let fake = fake.clone();
        std::thread::spawn(move || {
            let mut enforcer = Enforcer::early(fake.clone(), fake).unwrap();
            native.execute(&mut enforcer, &NativeContracts::shipped().unwrap());
        })
    };
    let started = std::time::Instant::now();
    while fake.0.lock().unwrap().polls < 2 {
        assert!(
            started.elapsed() < WAIT,
            "the worker never reached a.dll's load"
        );
        std::thread::yield_now();
    }
    let terminal = native.wait(Duration::from_millis(150));
    assert!(terminal.deadline_passed);
    assert_eq!(
        terminal.records[0].outcome,
        Outcome::TimedOut("not started: the native startup deadline passed".into())
    );
    release.send(()).unwrap();
    worker.join().unwrap();
    // The game was released at the deadline: no DLL initializer may start after it.
    assert!(
        fake.0.lock().unwrap().loads.is_empty(),
        "a load started after the deadline"
    );
    assert!(native.late_results().iter().all(|(_, o)| !o.admitted()));
    // review: the loader was never called for it, so there is no late load to log
    assert_eq!(native.late_results().len(), 1);
    assert!(fake.0.lock().unwrap().records.is_empty());
    assert_eq!(native.wait(STARTUP_NATIVE_DEADLINE), terminal);
}

#[cfg(unix)]
#[test]
fn a_dll_that_resolves_outside_its_package_folder_is_refused() {
    // ShroudForge's resolve_package_dll_path: "DLL is outside its installed mod folder".
    let game = directory();
    let fake = Fake::default();
    let mut dll = planned(&game.0, "elsewhere", "x.dll", fixture());
    std::fs::create_dir_all(game.0.join("mods/a")).unwrap();
    std::os::unix::fs::symlink(game.0.join("mods/elsewhere"), game.0.join("mods/a/bin")).unwrap();
    dll.path = "bin/x.dll".into();
    dll.status_id = "dll:a:bin/x.dll".into();
    let plan = NativePlan {
        plan_id: PLAN,
        packages: vec![package(&game.0, "a", vec![dll], &[])],
    };
    let (terminal, _) = run(plan, &fake, witness(&Arc::new(AtomicBool::new(false))));
    assert!(
        matches!(&terminal.records[0].outcome, Outcome::Refused(r) if r.contains("outside its installed mod folder")),
        "{:?}",
        terminal.records[0].outcome
    );
    assert!(fake.0.lock().unwrap().loads.is_empty());
    assert!(!terminal.runs_whole("a"));
}

/// Links Windows and Wine make for a test (see `folder_link`).
#[cfg(windows)]
mod links {
    /// A folder link at `link` to `target`: a Unix symbolic link under Wine (made by ln, which Wine starts but gives
    /// no handle to wait on, so the link is waited for: a must-happen wait, 300 s bound), a junction natively.
    /// Returns the link's Unix name under Wine (remove it with `remove_link`), `None` natively.
    pub fn folder_link(target: &std::path::Path, link: &std::path::Path) -> Option<String> {
        use std::os::windows::ffi::OsStrExt;
        #[link(name = "kernel32")]
        unsafe extern "system" {
            fn GetModuleHandleW(name: *const u16) -> *mut core::ffi::c_void;
            fn GetProcAddress(
                module: *mut core::ffi::c_void,
                name: *const core::ffi::c_char,
            ) -> *mut core::ffi::c_void;
            fn GetFileAttributesW(name: *const u16) -> u32;
        }
        let wide =
            |p: &std::path::Path| -> Vec<u16> { p.as_os_str().encode_wide().chain([0]).collect() };
        let kernel32 = wide(std::path::Path::new("kernel32.dll"));
        // SAFETY: NUL-terminated names; kernel32 is loaded in every Win32 process.
        let f = unsafe {
            GetProcAddress(
                GetModuleHandleW(kernel32.as_ptr()),
                c"wine_get_unix_file_name".as_ptr(),
            )
        };
        if f.is_null() {
            // cmd takes no forward slashes in a path.
            let native = |p: &std::path::Path| {
                std::path::PathBuf::from(p.to_str().unwrap().replace('/', "\\"))
            };
            let status = std::process::Command::new("cmd")
                .arg("/C")
                .arg("mklink")
                .arg("/J")
                .arg(native(link))
                .arg(native(target))
                .status()
                .unwrap();
            assert!(
                status.success() && link.is_dir(),
                "junction {}",
                link.display()
            );
            return None;
        }
        // SAFETY: Wine's export: char *CDECL wine_get_unix_file_name(LPCWSTR). The test leaks the strings.
        let f: unsafe extern "C" fn(*const u16) -> *const core::ffi::c_char =
            unsafe { std::mem::transmute(f) };
        let unix = |p: &std::path::Path| -> String {
            let name = wide(p);
            let r = unsafe { f(name.as_ptr()) };
            assert!(!r.is_null(), "no Unix name for {}", p.display());
            unsafe { std::ffi::CStr::from_ptr(r) }
                .to_str()
                .unwrap()
                .to_owned()
        };
        let parent = unix(link.parent().unwrap());
        let name = link.file_name().unwrap().to_str().unwrap();
        let unix_link = format!("{}/{name}", parent.trim_end_matches('/'));
        let _ = std::process::Command::new("/usr/bin/ln")
            .arg("-s")
            .arg(unix(target))
            .arg(&unix_link)
            .spawn();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(300);
        let link_name = wide(link);
        // SAFETY: a NUL-terminated name. Wine shows a link to a folder as a reparse point by name.
        while unsafe { GetFileAttributesW(link_name.as_ptr()) } != 0x410 {
            assert!(std::time::Instant::now() < deadline, "ln made no link");
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        Some(unix_link)
    }
    /// Removes a link `folder_link` made: the link itself (by its Unix name under Wine), never what it points at.
    pub fn remove_link(link: &std::path::Path, unix_link: Option<String>) {
        match unix_link {
            Some(unix_link) => {
                let _ = std::process::Command::new("/usr/bin/rm")
                    .arg(unix_link)
                    .spawn();
                let deadline = std::time::Instant::now() + std::time::Duration::from_secs(300);
                while std::fs::symlink_metadata(link).is_ok() {
                    assert!(std::time::Instant::now() < deadline, "rm left the link");
                    std::thread::sleep(std::time::Duration::from_millis(10));
                }
            }
            None => std::fs::remove_dir(link).unwrap(),
        }
    }
}

// The same on Windows: a junction (which canonicalize resolves) natively, a Unix link (which Wine follows and
// canonicalize keeps in the path) under Wine.
#[cfg(windows)]
#[test]
fn on_windows_a_dll_that_resolves_outside_its_package_folder_is_refused() {
    let game = directory();
    let fake = Fake::default();
    let mut dll = planned(&game.0, "elsewhere", "x.dll", fixture());
    std::fs::create_dir_all(game.0.join("mods/a")).unwrap();
    let link = links::folder_link(&game.0.join("mods/elsewhere"), &game.0.join("mods/a/bin"));
    assert!(
        game.0.join("mods/a/bin/x.dll").is_file(),
        "the link is real"
    );
    dll.path = "bin/x.dll".into();
    dll.status_id = "dll:a:bin/x.dll".into();
    let plan = NativePlan {
        plan_id: PLAN,
        packages: vec![package(&game.0, "a", vec![dll], &[])],
    };
    let (terminal, _) = run(plan, &fake, witness(&Arc::new(AtomicBool::new(false))));
    links::remove_link(&game.0.join("mods/a/bin"), link);
    assert!(
        matches!(&terminal.records[0].outcome, Outcome::Refused(r) if r.contains("outside its installed mod folder")),
        "{:?}",
        terminal.records[0].outcome
    );
    assert!(fake.0.lock().unwrap().loads.is_empty());
    assert!(!terminal.runs_whole("a"));
}

// ---- Kiln's native.policy at boot ----

/// a (DLL) <- l (Lua only) <- z (Lua only) <- c (DLL); b (DLL) alone; free (Lua only) alone.
fn policy_plan(game: &Path) -> NativePlan {
    NativePlan {
        plan_id: PLAN,
        packages: vec![
            package(game, "a", vec![planned(game, "a", "a.dll", fixture())], &[]),
            package(game, "l", Vec::new(), &["a"]),
            package(game, "z", Vec::new(), &["l"]),
            package(game, "b", vec![planned(game, "b", "b.dll", fixture())], &[]),
            package(
                game,
                "c",
                vec![planned(game, "c", "c.dll", fixture())],
                &["z"],
            ),
            package(game, "free", Vec::new(), &[]),
        ],
    }
}

#[test]
fn native_policy_never_withdraws_every_dll_package_and_its_dependents_with_zero_loads() {
    for (permission, text) in [
        (
            NativePermission {
                policy: NativePolicy::Never,
                verified: true,
            },
            r#"kiln.json native.policy is "never""#,
        ),
        (
            NativePermission {
                policy: NativePolicy::VerifiedOnly,
                verified: false,
            },
            r#"kiln.json native.policy is "verified-only" and this game build is not verified"#,
        ),
    ] {
        let game = directory();
        let fake = Fake::default();
        let (terminal, _) = run_with(
            policy_plan(&game.0),
            &fake,
            witness(&Arc::new(AtomicBool::new(false))),
            permission,
        );
        // Not one load, not one capture: the loader is never asked.
        assert!(fake.0.lock().unwrap().loads.is_empty(), "{permission:?}");
        assert_eq!(
            terminal
                .records
                .iter()
                .map(|r| (
                    r.status_id.as_str(),
                    r.outcome.clone(),
                    r.actual_sha256.clone()
                ))
                .collect::<Vec<_>>(),
            ["dll:a:a.dll", "dll:b:b.dll", "dll:c:c.dll"].map(|id| (
                id,
                Outcome::Withdrawn(text.into()),
                None
            )),
            "{permission:?}"
        );
        let withdrawn: Vec<_> = terminal
            .withdrawn
            .iter()
            .map(|w| (w.mod_id.as_str(), w.reason.as_str(), w.caused_by.clone()))
            .collect();
        assert_eq!(
            withdrawn,
            [
                ("a", text, vec![]),
                ("l", "depends on withdrawn package a", vec!["a".to_owned()]),
                (
                    "z",
                    "depends on withdrawn package l",
                    vec!["l".to_owned(), "a".to_owned()]
                ),
                ("b", text, vec![]),
                ("c", text, vec![]),
            ],
            "{permission:?}"
        );
        // A Lua-only package that needs no withdrawn one still runs, and nothing loaded, so the witness holds.
        assert!(terminal.runs_whole("free"));
        assert!(terminal.coherent.is_ok(), "{permission:?}");
        assert!(!terminal.deadline_passed);
    }
}

#[test]
fn native_policy_always_and_verified_only_on_a_verified_build_load_as_before() {
    for permission in [
        NativePermission::ALWAYS,
        NativePermission {
            policy: NativePolicy::Always,
            verified: true,
        },
        NativePermission {
            policy: NativePolicy::VerifiedOnly,
            verified: true,
        },
    ] {
        let game = directory();
        let fake = Fake::default();
        let (terminal, _) = run_with(
            policy_plan(&game.0),
            &fake,
            witness(&Arc::new(AtomicBool::new(false))),
            permission,
        );
        assert_eq!(fake.0.lock().unwrap().loads, ["a.dll", "b.dll", "c.dll"]);
        assert_eq!(
            outcomes(&terminal),
            pairs(&[
                ("dll:a:a.dll", "loaded"),
                ("dll:b:b.dll", "loaded"),
                ("dll:c:c.dll", "loaded"),
            ]),
            "{permission:?}"
        );
        assert!(terminal.withdrawn.is_empty() && terminal.coherent.is_ok());
    }
}

// ---- when the loader is armed, and a run concluded without it ----

/// One spawn of the loader thread, counted; the thread is joined by the caller.
#[derive(Default)]
struct Spawned {
    spawns: usize,
    bodies: usize,
    thread: Option<std::thread::JoinHandle<()>>,
}

/// Every combination of the three arming terms (the permission, at least one planned DLL, a wired witness): only all
/// three together arm the loader, which registers its notification before the first load; every other combination
/// registers nothing, asks for no fault source, starts no thread, loads nothing and concludes at once with the first
/// reason in the order policy, plan, witness.
#[test]
fn the_loader_is_armed_only_when_the_policy_allows_the_plan_has_a_dll_and_the_witness_is_wired() {
    let never = NativePermission {
        policy: NativePolicy::Never,
        verified: true,
    };
    let unverified = NativePermission {
        policy: NativePolicy::VerifiedOnly,
        verified: false,
    };
    let verified = NativePermission {
        policy: NativePolicy::VerifiedOnly,
        verified: true,
    };
    for permission in [never, unverified, NativePermission::ALWAYS, verified] {
        for dlls in [false, true] {
            for wired in [false, true] {
                let what = format!("{permission:?}, dlls {dlls}, wired {wired}");
                let game = directory();
                let fake = Fake::default();
                let w = witness(&Arc::new(AtomicBool::new(false)));
                let plan = if dlls {
                    policy_plan(&game.0)
                } else {
                    NativePlan {
                        plan_id: PLAN,
                        packages: vec![
                            package(&game.0, "l", Vec::new(), &[]),
                            package(&game.0, "m", Vec::new(), &["l"]),
                        ],
                    }
                };
                let port = if wired {
                    WitnessPort::Wired(w.clone())
                } else {
                    WitnessPort::NotWired
                };
                let run = NativeRun::new(plan, port.witness(), permission);
                let mut spawned = Spawned::default();
                let armed = {
                    let bodies = &mut spawned.bodies;
                    let (spawns, thread) = (&mut spawned.spawns, &mut spawned.thread);
                    arming::arm(
                        &run,
                        &port,
                        |body: LoaderBody| {
                            *spawns += 1;
                            *thread = Some(std::thread::spawn(body));
                            Ok(())
                        },
                        || {
                            *bodies += 1;
                            let fake = fake.clone();
                            arming::loader_body(Arc::clone(&run), move || {
                                Ok((fake.clone(), fake, NativeContracts::shipped()?))
                            })
                        },
                    )
                };
                let terminal = run.wait(STARTUP_NATIVE_DEADLINE);
                if let Some(thread) = spawned.thread.take() {
                    thread.join().unwrap();
                }
                let state = fake.0.lock().unwrap();
                if permission.refusal().is_none() && dlls && wired {
                    assert_eq!(armed, Armed::Started, "{what}");
                    assert_eq!((spawned.spawns, spawned.bodies), (1, 1), "{what}");
                    assert_eq!((state.registers, state.fault_sources), (1, 1), "{what}");
                    // the notification is live before the first package DLL loads
                    assert_eq!(
                        state.calls,
                        ["register", "load:a.dll", "load:b.dll", "load:c.dll"],
                        "{what}"
                    );
                    assert!(terminal.not_armed.is_none(), "{what}");
                    assert!(
                        terminal.records.iter().all(|r| r.outcome.admitted()),
                        "{what}"
                    );
                    assert_eq!(w.readies.load(Ordering::SeqCst), 1, "{what}");
                    continue;
                }
                let reason = permission.refusal().map(str::to_owned).unwrap_or_else(|| {
                    if dlls {
                        arming::WITNESS_NOT_WIRED.to_owned()
                    } else {
                        arming::NO_DLLS.to_owned()
                    }
                });
                assert_eq!(armed, Armed::NotArmed(reason.clone()), "{what}");
                assert_eq!(
                    arming::not_armed(&run, &port),
                    Some(reason.clone()),
                    "{what}"
                );
                assert_eq!(
                    (
                        spawned.spawns,
                        spawned.bodies,
                        state.registers,
                        state.fault_sources
                    ),
                    (0, 0, 0, 0),
                    "{what}: no thread, no notifier, no fault source"
                );
                assert!(state.calls.is_empty() && state.loads.is_empty(), "{what}");
                assert_eq!(
                    w.readies.load(Ordering::SeqCst),
                    0,
                    "{what}: the witness is not even asked"
                );
                assert_eq!(
                    terminal.not_armed.as_deref(),
                    Some(reason.as_str()),
                    "{what}"
                );
                assert!(
                    terminal
                        .records
                        .iter()
                        .all(|r| r.outcome == Outcome::Refused(reason.clone())
                            && r.actual_sha256.is_none()),
                    "{what}"
                );
            }
        }
    }
}

/// A thread that cannot start concludes the run with that reason: still nothing registered or loaded.
#[test]
fn a_loader_thread_that_cannot_start_concludes_the_run() {
    let game = directory();
    let fake = Fake::default();
    let port = WitnessPort::Wired(witness(&Arc::new(AtomicBool::new(false))));
    let run = NativeRun::new(
        policy_plan(&game.0),
        port.witness(),
        NativePermission::ALWAYS,
    );
    let armed = arming::arm(
        &run,
        &port,
        |_| Err("no more threads".to_owned()),
        || {
            let fake = fake.clone();
            arming::loader_body(Arc::clone(&run), move || {
                Ok((fake.clone(), fake, NativeContracts::shipped()?))
            })
        },
    );
    let Armed::NotArmed(reason) = armed else {
        panic!("{armed:?}");
    };
    assert!(reason.contains("no more threads"), "{reason}");
    let terminal = run.wait(Duration::ZERO);
    assert_eq!(terminal.not_armed.as_deref(), Some(reason.as_str()));
    let state = fake.0.lock().unwrap();
    assert!(state.registers == 0 && state.loads.is_empty());
}

/// The loader thread's backend refusing (Enforcer::early fails: ShroudForge's own runtime is in the process) loads
/// nothing and leaves every DLL refused with that reason; the run is never executed afterwards.
#[test]
fn an_early_enforcement_that_refuses_loads_nothing() {
    #[derive(Clone, Default)]
    struct Hosted(Fake);
    impl Backend for Hosted {
        fn modules(&mut self) -> Result<Vec<String>, String> {
            Ok(vec!["shroudforge-runtime.dll".into()])
        }
        fn register(&mut self) -> Result<(), String> {
            self.0.register()
        }
        fn events(&mut self) -> Result<Vec<Breach>, String> {
            self.0.events()
        }
        fn first_loader(&mut self, image: &Snapshot) -> Result<Option<usize>, String> {
            self.0.first_loader(image)
        }
        fn load(&mut self, path: &Path, owner: u64) -> Result<usize, BackendLoadError> {
            self.0.load(path, owner)
        }
        fn validate_loaded(&mut self, module: usize, image: &Snapshot) -> Result<(), String> {
            self.0.validate_loaded(module, image)
        }
    }
    let game = directory();
    let fake = Fake::default();
    let port = WitnessPort::Wired(witness(&Arc::new(AtomicBool::new(false))));
    let run = NativeRun::new(
        policy_plan(&game.0),
        port.witness(),
        NativePermission::ALWAYS,
    );
    let body = {
        let fake = fake.clone();
        arming::loader_body(Arc::clone(&run), move || {
            Ok((Hosted(fake.clone()), fake, NativeContracts::shipped()?))
        })
    };
    body();
    let terminal = run.wait(Duration::ZERO);
    assert!(
        terminal.records.iter().all(|r| matches!(&r.outcome, Outcome::Refused(why) if why.contains("ShroudForge's own loader"))),
        "{:?}",
        terminal.records
    );
    let mut enforcer = Enforcer::early(fake.clone(), fake.clone()).unwrap();
    run.execute(&mut enforcer, &NativeContracts::shipped().unwrap());
    let state = fake.0.lock().unwrap();
    assert!(
        state.registers <= 1 && state.loads.is_empty(),
        "never executed after"
    );
}

/// conclude_without_loader: every planned DLL is Refused with the reason, nothing captured; packages with a DLL are
/// withdrawn with it and their dependents through Lua-only packages; the terminal is there at once and stays; a later
/// execute loads nothing; a second conclusion (or one after execute started) changes nothing.
#[test]
fn a_run_concluded_without_the_loader_refuses_every_dll_and_withdraws_through_lua_only_packages() {
    let game = directory();
    let fake = Fake::default();
    let w = witness(&Arc::new(AtomicBool::new(false)));
    let run = NativeRun::new(policy_plan(&game.0), w.clone(), NativePermission::ALWAYS);
    let why = "no startup witness in this test";
    assert!(run.may_be_loading(), "open until it is concluded");
    assert!(run.conclude_without_loader(why));
    assert!(
        !run.may_be_loading(),
        "concluded without the loader: nothing loads"
    );
    assert!(!run.conclude_without_loader("a second reason"));
    let terminal = run.wait(Duration::ZERO);
    assert_eq!(
        terminal
            .records
            .iter()
            .map(|r| (
                r.status_id.as_str(),
                r.outcome.clone(),
                r.actual_sha256.clone()
            ))
            .collect::<Vec<_>>(),
        ["dll:a:a.dll", "dll:b:b.dll", "dll:c:c.dll"].map(|id| (
            id,
            Outcome::Refused(why.into()),
            None
        ))
    );
    let withdrawn: Vec<_> = terminal
        .withdrawn
        .iter()
        .map(|w| (w.mod_id.as_str(), w.reason.as_str(), w.caused_by.clone()))
        .collect();
    assert_eq!(
        withdrawn,
        [
            ("a", why, vec![]),
            ("l", "depends on withdrawn package a", vec!["a".to_owned()]),
            (
                "z",
                "depends on withdrawn package l",
                vec!["l".to_owned(), "a".to_owned()]
            ),
            ("b", why, vec![]),
            ("c", why, vec![]),
        ]
    );
    assert!(terminal.runs_whole("free"));
    assert_eq!(terminal.not_armed.as_deref(), Some(why));
    assert!(terminal.coherent.is_ok() && !terminal.deadline_passed);
    // nothing reaches the loader afterwards, and the witness was never asked
    let mut enforcer = Enforcer::early(fake.clone(), fake.clone()).unwrap();
    run.execute(&mut enforcer, &NativeContracts::shipped().unwrap());
    assert!(fake.0.lock().unwrap().loads.is_empty());
    assert_eq!(w.readies.load(Ordering::SeqCst), 0);
    assert_eq!(run.wait(STARTUP_NATIVE_DEADLINE), terminal);

    // a run that already started is not concluded afterwards
    let game = directory();
    let fake = Fake::default();
    let run = NativeRun::new(policy_plan(&game.0), w.clone(), NativePermission::ALWAYS);
    let mut enforcer = Enforcer::early(fake.clone(), fake.clone()).unwrap();
    run.execute(&mut enforcer, &NativeContracts::shipped().unwrap());
    assert!(!run.conclude_without_loader(why));
    let terminal = run.wait(STARTUP_NATIVE_DEADLINE);
    assert!(terminal.not_armed.is_none());
    assert_eq!(fake.0.lock().unwrap().loads, ["a.dll", "b.dll", "c.dll"]);
}

// ---- ShroudForge's startup deadline in the early window ----

/// Every push an armed run made: the thread it ran on, and the terminal.
#[derive(Clone, Default)]
struct Pushes(Arc<Mutex<Vec<Pushed>>>);
/// One push: the thread it ran on, and the terminal.
type Pushed = (Option<String>, NativeTerminal);
impl Pushes {
    fn push(&self) -> arming::TerminalPush {
        let pushes = self.0.clone();
        Box::new(move |terminal: &NativeTerminal| {
            pushes.lock().unwrap().push((
                std::thread::current().name().map(str::to_owned),
                terminal.clone(),
            ));
        })
    }
    fn all(&self) -> Vec<Pushed> {
        self.0.lock().unwrap().clone()
    }
}
/// The threads a test spawn started, named as the production spawns name them; the test joins them.
#[derive(Clone, Default)]
struct Threads(Arc<Mutex<Vec<Named>>>);
/// A thread a test spawn started: its name and handle.
type Named = (String, std::thread::JoinHandle<()>);
impl Threads {
    /// A spawn that names its thread `name` and runs `before` on it ahead of the body.
    fn spawn_with(
        &self,
        name: &'static str,
        before: impl FnOnce() + Send + 'static,
    ) -> Box<dyn FnOnce(LoaderBody) -> Result<(), String>> {
        let threads = self.clone();
        Box::new(move |body: LoaderBody| {
            let handle = std::thread::Builder::new()
                .name(name.into())
                .spawn(move || {
                    before();
                    body()
                })
                .map_err(|e| e.to_string())?;
            threads.0.lock().unwrap().push((name.to_owned(), handle));
            Ok(())
        })
    }
    fn spawn(&self, name: &'static str) -> Box<dyn FnOnce(LoaderBody) -> Result<(), String>> {
        self.spawn_with(name, || {})
    }
    fn names(&self) -> Vec<String> {
        self.0
            .lock()
            .unwrap()
            .iter()
            .map(|(n, _)| n.clone())
            .collect()
    }
    fn finished(&self, name: &str) -> bool {
        self.0
            .lock()
            .unwrap()
            .iter()
            .filter(|(n, _)| n == name)
            .all(|(_, h)| h.is_finished())
    }
    /// Joins every thread; false when one is still running after `bound`.
    fn join(&self, bound: Duration) -> bool {
        let started = std::time::Instant::now();
        while !self.0.lock().unwrap().iter().all(|(_, h)| h.is_finished()) {
            if started.elapsed() > bound {
                return false;
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        for (_, handle) in self.0.lock().unwrap().drain(..) {
            handle.join().unwrap();
        }
        true
    }
}
/// True as soon as `condition` holds; false when `bound` passed first.
fn eventually(bound: Duration, mut condition: impl FnMut() -> bool) -> bool {
    let started = std::time::Instant::now();
    while !condition() {
        if started.elapsed() > bound {
            return false;
        }
        std::thread::sleep(Duration::from_millis(1));
    }
    true
}
/// A loader body over the fake backend.
fn fake_body(run: &Arc<NativeRun>, fake: &Fake) -> impl FnOnce() -> LoaderBody + use<> {
    let (run, fake) = (Arc::clone(run), fake.clone());
    move || {
        arming::loader_body(run, move || {
            Ok((fake.clone(), fake, NativeContracts::shipped()?))
        })
    }
}
/// arm_and_push with the test's threads (named as production names them), `deadline` and `body`.
fn arm_push(
    run: &Arc<NativeRun>,
    port: &WitnessPort,
    threads: &Threads,
    deadline: impl Into<Deadline>,
    body: impl FnOnce() -> LoaderBody,
    pushes: &Pushes,
) -> Armed {
    arming::arm_and_push(
        run,
        port,
        threads.spawn(arming::NATIVE_THREAD),
        threads.spawn(arming::DEADLINE_THREAD),
        deadline,
        body,
        pushes.push(),
    )
}
/// The terminal's withdrawn packages as kiln-eml reads them: (id, code, chain).
fn codes(terminal: &NativeTerminal) -> Vec<(String, String, Vec<String>)> {
    let v: serde_json::Value = serde_json::from_str(&kiln_sfi::startup_seam::terminal_json(
        PLAN, terminal, None, false,
    ))
    .unwrap();
    v["withdrawn"]
        .as_array()
        .unwrap()
        .iter()
        .map(|w| {
            (
                w["id"].as_str().unwrap().to_owned(),
                w["code"].as_str().unwrap().to_owned(),
                w["chain"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .map(|c| c.as_str().unwrap().to_owned())
                    .collect(),
            )
        })
        .collect()
}
const TIMED_OUT: &str = "Native DLL 'slow.dll' did not finish initialization within 15 seconds. Disable it in DLL Manager and restart the game";
/// Review: the record line for slow.dll's load returning, loaded, after the deadline concluded the run.
const LATE_SLOW_LOADED: &str = "native DLL 'slow.dll' (dll:h:slow.dll) returned from the loader after the native startup deadline had concluded this start's native run (loaded: its code runs in the game process); the result is never admitted and its package stays withdrawn this start";

/// A DLL whose initializer never returns in time: the deadline waiter (a thread of its own, started before the loader
/// thread) pushes the terminal at the deadline while the loader thread is still inside the load (the deadline is
/// signalled once the loader is inside it: a clock deadline could pass before a loaded machine lets the loader get
/// there, and the DLL would then be `timed-out` as not started): the DLL `timed-out`,
/// the DLLs after it not started (`timed-out` too), their packages withdrawn (`dll-timed-out`) and the Lua-only
/// dependent with them (`dependency-withdrawn`). The loader thread is not stopped: when the initializer returns, its
/// result is only logged, nothing else loads, and nothing is pushed again.
#[test]
fn a_hung_load_gets_its_terminal_from_the_deadline_waiter_once_while_the_loader_is_still_inside_it()
{
    let game = directory();
    let fake = Fake::default();
    let (release, gate) = mpsc::channel();
    fake.0.lock().unwrap().block = Some(("slow.dll".into(), gate));
    let plan = NativePlan {
        plan_id: PLAN,
        packages: vec![
            package(
                &game.0,
                "h",
                vec![planned(&game.0, "h", "slow.dll", fixture())],
                &[],
            ),
            package(&game.0, "d", Vec::new(), &["h"]),
            package(
                &game.0,
                "x",
                vec![planned(&game.0, "x", "x.dll", fixture())],
                &[],
            ),
            package(&game.0, "e", Vec::new(), &["x"]),
        ],
    };
    let w = witness(&Arc::new(AtomicBool::new(false)));
    let port = WitnessPort::Wired(w.clone());
    let run = NativeRun::new(plan, port.witness(), NativePermission::ALWAYS);
    let (threads, pushes) = (Threads::default(), Pushes::default());
    let (pass, deadline) = mpsc::channel();
    let armed = arm_push(
        &run,
        &port,
        &threads,
        Deadline::Signalled(deadline),
        fake_body(&run, &fake),
        &pushes,
    );
    assert_eq!(armed, Armed::Started);
    assert_eq!(
        threads.names(),
        [arming::DEADLINE_THREAD, arming::NATIVE_THREAD]
    );
    assert!(
        eventually(WAIT, || fake.0.lock().unwrap().loads == ["slow.dll"]),
        "the loader never reached slow.dll"
    );
    assert!(pushes.all().is_empty(), "pushed before the deadline");
    pass.send(()).unwrap();
    assert!(
        eventually(WAIT, || !pushes.all().is_empty()),
        "no terminal while the load hangs"
    );
    let (on, terminal) = pushes.all()[0].clone();
    assert_eq!(on.as_deref(), Some(arming::DEADLINE_THREAD));
    assert!(terminal.deadline_passed && terminal.not_armed.is_none());
    assert_eq!(
        outcomes(&terminal),
        pairs(&[
            ("dll:h:slow.dll", "timed-out"),
            ("dll:x:x.dll", "timed-out")
        ])
    );
    assert_eq!(
        terminal.records[0].outcome,
        Outcome::TimedOut(TIMED_OUT.into())
    );
    assert_eq!(
        terminal.records[1].outcome,
        Outcome::TimedOut("not started: the native startup deadline passed".into())
    );
    let s = |v: &[&str]| v.iter().map(|x| (*x).to_owned()).collect::<Vec<_>>();
    assert_eq!(
        codes(&terminal),
        [
            ("h".to_owned(), "dll-timed-out".to_owned(), s(&[])),
            ("d".to_owned(), "dependency-withdrawn".to_owned(), s(&["h"])),
            ("x".to_owned(), "dll-timed-out".to_owned(), s(&[])),
            ("e".to_owned(), "dependency-withdrawn".to_owned(), s(&["x"])),
        ]
    );
    // the waiter did not stop the loader thread: it is still inside slow.dll's initializer
    assert_eq!(fake.0.lock().unwrap().loads, ["slow.dll"]);
    assert!(run.late_results().is_empty());
    assert!(!threads.finished(arming::NATIVE_THREAD));
    // the initializer returns: only logged, never admitted, nothing new loads, no second push
    release.send(()).unwrap();
    assert!(threads.join(WAIT), "a thread of the run never ended");
    assert_eq!(fake.0.lock().unwrap().loads, ["slow.dll"]);
    let late = run.late_results();
    assert_eq!(late.len(), 1);
    assert_eq!(late[0].0, "dll:h:slow.dll");
    assert!(late[0].1.admitted());
    // review: the late load is in the enforcement's record (kiln.log), once, and only that one
    let records = fake.0.lock().unwrap().records.clone();
    assert_eq!(records, [LATE_SLOW_LOADED], "the late load was not logged");
    assert_eq!(pushes.all().len(), 1, "pushed twice");
    assert_eq!(run.wait(Duration::ZERO), terminal);
}

/// A run whose every DLL is terminal before the deadline is pushed once, by the loader thread, as without a deadline waiter; the
/// waiter wakes when the run is done and ends long before its deadline, pushing nothing.
#[test]
fn a_run_done_before_the_deadline_is_pushed_by_the_loader_thread_and_the_waiter_ends_without_pushing()
 {
    let game = directory();
    let fake = Fake::default();
    let (release, gate) = mpsc::channel();
    fake.0.lock().unwrap().block = Some(("slow.dll".into(), gate));
    let plan = NativePlan {
        plan_id: PLAN,
        packages: vec![
            package(
                &game.0,
                "a",
                vec![planned(&game.0, "a", "a.dll", fixture())],
                &[],
            ),
            package(
                &game.0,
                "h",
                vec![planned(&game.0, "h", "slow.dll", fixture())],
                &[],
            ),
        ],
    };
    let port = WitnessPort::Wired(witness(&Arc::new(AtomicBool::new(false))));
    let run = NativeRun::new(plan, port.witness(), NativePermission::ALWAYS);
    let (threads, pushes) = (Threads::default(), Pushes::default());
    // a deadline far beyond the test's own liveness bound: the waiter can only end because the run is done
    let armed = arm_push(
        &run,
        &port,
        &threads,
        WAIT * 4,
        fake_body(&run, &fake),
        &pushes,
    );
    assert_eq!(armed, Armed::Started);
    assert!(eventually(WAIT, || fake.0.lock().unwrap().loads.len() == 2));
    assert!(pushes.all().is_empty(), "pushed before the run was done");
    release.send(()).unwrap();
    assert!(eventually(WAIT, || !pushes.all().is_empty()));
    assert!(threads.join(WAIT), "the waiter waited for its deadline");
    let all = pushes.all();
    assert_eq!(all.len(), 1, "pushed twice");
    let (on, terminal) = &all[0];
    assert_eq!(on.as_deref(), Some(arming::NATIVE_THREAD));
    assert!(!terminal.deadline_passed && terminal.coherent.is_ok());
    assert!(terminal.records.iter().all(|r| r.outcome.admitted()));
    assert!(terminal.withdrawn.is_empty());
    assert_eq!(&run.wait(Duration::ZERO), terminal);
}

/// The clock starts when the run is armed (ShroudForge starts it once the whole plan is handed to its worker), not at
/// the first load: a witness that is not ready before the deadline leaves every DLL `timed-out` (not started), and the
/// waiter pushes; the loader, once the witness answers, finds the run closed and loads nothing. Once with a signalled
/// deadline that passes while the loader thread is inside the witness wait (the outcome, whatever the scheduling), and
/// once with a clock deadline while the witness stays outstanding until the push (no load can start, so the push
/// proves the clock counts from the arming); a clock round whose waiter a loaded machine did not run by the deadline
/// (the loader then stops with WAITER_NEVER_RAN before asking the witness) is tried again.
#[test]
fn the_deadline_counts_from_the_arming_not_from_the_first_load() {
    struct Late {
        gate: Mutex<Option<mpsc::Receiver<()>>>,
        asked: AtomicUsize,
    }
    impl Witness for Late {
        fn ready(&self) -> Result<WitnessToken, String> {
            self.asked.fetch_add(1, Ordering::SeqCst);
            if let Some(gate) = self.gate.lock().unwrap().take() {
                gate.recv().unwrap();
            }
            Ok(WitnessToken {
                plan_id: PLAN,
                revision: 1,
            })
        }
        fn recheck(&self, _: &WitnessToken) -> Result<(), String> {
            Ok(())
        }
    }
    const CLOCK: Duration = Duration::from_millis(500);
    // one round; false for a clock round the waiter did not reach by the deadline
    let round = |clocked: bool| -> bool {
        let game = directory();
        let fake = Fake::default();
        let (release, gate) = mpsc::channel();
        let late = Arc::new(Late {
            gate: Mutex::new(Some(gate)),
            asked: AtomicUsize::new(0),
        });
        let port = WitnessPort::Wired(late.clone());
        let run = NativeRun::new(
            policy_plan(&game.0),
            port.witness(),
            NativePermission::ALWAYS,
        );
        let (threads, pushes) = (Threads::default(), Pushes::default());
        let (pass, signal) = mpsc::channel();
        let armed_at = std::time::Instant::now();
        let armed = arm_push(
            &run,
            &port,
            &threads,
            if clocked {
                Deadline::After(CLOCK)
            } else {
                Deadline::Signalled(signal)
            },
            fake_body(&run, &fake),
            &pushes,
        );
        assert_eq!(armed, Armed::Started);
        if clocked {
            // the witness answers only after the push: nothing loads before it, so only a clock that counts from the
            // arming can pass
            assert!(
                eventually(WAIT, || !pushes.all().is_empty()),
                "no push while the witness was outstanding: the clock does not count from the arming"
            );
            let (on, terminal) = pushes.all()[0].clone();
            if on.as_deref() == Some(arming::NATIVE_THREAD) {
                assert!(
                    terminal
                        .records
                        .iter()
                        .all(|r| r.outcome == Outcome::Refused(arming::WAITER_NEVER_RAN.into())),
                    "{terminal:?}"
                );
                assert_eq!(late.asked.load(Ordering::SeqCst), 0);
                assert!(threads.join(WAIT));
                assert_eq!(pushes.all().len(), 1);
                return false;
            }
            assert!(armed_at.elapsed() >= CLOCK, "{:?}", armed_at.elapsed());
        } else {
            assert!(
                eventually(WAIT, || late.asked.load(Ordering::SeqCst) == 1),
                "the witness was never asked"
            );
            assert!(pushes.all().is_empty(), "pushed before the deadline");
            pass.send(()).unwrap();
            assert!(eventually(WAIT, || !pushes.all().is_empty()));
        }
        let (on, terminal) = pushes.all()[0].clone();
        assert_eq!(on.as_deref(), Some(arming::DEADLINE_THREAD));
        // a clock deadline can also pass before the loader thread got to the witness
        let asked = late.asked.load(Ordering::SeqCst);
        assert!(
            asked == 1 || (clocked && asked == 0),
            "the witness was asked {asked} times"
        );
        assert!(terminal.deadline_passed);
        assert!(terminal.records.iter().all(|r| r.outcome
            == Outcome::TimedOut("not started: the native startup deadline passed".into())));
        assert_eq!(
            terminal.coherent,
            Err("no startup witness was issued".to_owned())
        );
        release.send(()).unwrap();
        assert!(threads.join(WAIT));
        assert!(
            fake.0.lock().unwrap().loads.is_empty(),
            "a load after the deadline"
        );
        assert_eq!(pushes.all().len(), 1);
        true
    };
    assert!(round(false));
    assert!(
        (0..20).any(|_| round(true)),
        "the deadline waiter never ran by a {CLOCK:?} clock deadline in 20 tries"
    );
}

/// A loader body that panics is still pushed once: before the deadline by the loader thread (every DLL refused with
/// LOADER_STOPPED; the waiter ends without pushing); after the deadline the waiter has already pushed and the loader
/// thread's panic changes nothing and pushes nothing (that deadline is signalled while the body is running: a clock
/// deadline could pass before a loaded machine ran the waiter, and the loader would then stop with WAITER_NEVER_RAN).
#[test]
fn a_panicking_loader_is_pushed_exactly_once() {
    for late in [false, true] {
        let game = directory();
        let port = WitnessPort::Wired(witness(&Arc::new(AtomicBool::new(false))));
        let run = NativeRun::new(
            policy_plan(&game.0),
            port.witness(),
            NativePermission::ALWAYS,
        );
        let (threads, pushes) = (Threads::default(), Pushes::default());
        let (release, gate) = mpsc::channel::<()>();
        let (entered, running) = mpsc::channel::<()>();
        let (pass, signal) = mpsc::channel::<()>();
        let deadline = if late {
            Deadline::Signalled(signal)
        } else {
            Deadline::After(WAIT * 4)
        };
        let armed = arm_push(
            &run,
            &port,
            &threads,
            deadline,
            move || -> LoaderBody {
                Box::new(move || {
                    if late {
                        entered.send(()).unwrap();
                        gate.recv().unwrap();
                    }
                    std::panic::resume_unwind(Box::new("the test loader fails"))
                })
            },
            &pushes,
        );
        assert_eq!(armed, Armed::Started, "late {late}");
        if late {
            running
                .recv_timeout(WAIT)
                .expect("the loader body never ran");
            assert!(pushes.all().is_empty(), "pushed before the deadline");
            pass.send(()).unwrap();
        }
        assert!(eventually(WAIT, || !pushes.all().is_empty()), "late {late}");
        if late {
            release.send(()).unwrap();
        }
        assert!(threads.join(WAIT), "late {late}");
        let all = pushes.all();
        assert_eq!(all.len(), 1, "late {late}: pushed twice");
        let (on, terminal) = &all[0];
        if late {
            assert_eq!(on.as_deref(), Some(arming::DEADLINE_THREAD));
            assert!(terminal.deadline_passed);
            assert!(
                terminal
                    .records
                    .iter()
                    .all(|r| matches!(r.outcome, Outcome::TimedOut(_)))
            );
        } else {
            assert_eq!(on.as_deref(), Some(arming::NATIVE_THREAD));
            assert!(!terminal.deadline_passed);
            assert!(
                terminal
                    .records
                    .iter()
                    .all(|r| r.outcome == Outcome::Refused(arming::LOADER_STOPPED.into()))
            );
        }
        assert_eq!(&run.wait(Duration::ZERO), terminal, "late {late}");
    }
}

/// The waiter exists and runs before the loader thread does anything: with the waiter's start held, the loader thread
/// registers nothing, asks no witness and loads nothing; once the waiter runs, the run goes on. A waiter that does not
/// run before the deadline leaves the run with nothing loaded (pushed once, by the loader thread), and the waiter, when
/// it runs at last, pushes nothing.
#[test]
fn the_loader_does_nothing_before_the_deadline_waiter_runs() {
    for runs_in_time in [true, false] {
        let game = directory();
        let fake = Fake::default();
        let w = witness(&Arc::new(AtomicBool::new(false)));
        let port = WitnessPort::Wired(w.clone());
        let run = NativeRun::new(
            policy_plan(&game.0),
            port.witness(),
            NativePermission::ALWAYS,
        );
        let (threads, pushes) = (Threads::default(), Pushes::default());
        let (start, held) = mpsc::channel::<()>();
        let deadline = if runs_in_time {
            WAIT * 4
        } else {
            Duration::from_millis(100)
        };
        let armed = arming::arm_and_push(
            &run,
            &port,
            threads.spawn(arming::NATIVE_THREAD),
            threads.spawn_with(arming::DEADLINE_THREAD, move || held.recv().unwrap()),
            deadline,
            fake_body(&run, &fake),
            pushes.push(),
        );
        assert_eq!(armed, Armed::Started);
        if runs_in_time {
            std::thread::sleep(Duration::from_millis(200));
            assert!(
                fake.0.lock().unwrap().calls.is_empty() && w.readies.load(Ordering::SeqCst) == 0,
                "the loader went on before its deadline waiter ran"
            );
            assert!(pushes.all().is_empty());
            start.send(()).unwrap();
            assert!(eventually(WAIT, || !pushes.all().is_empty()));
            assert_eq!(
                fake.0.lock().unwrap().calls,
                ["register", "load:a.dll", "load:b.dll", "load:c.dll"]
            );
            let (on, terminal) = pushes.all()[0].clone();
            assert_eq!(on.as_deref(), Some(arming::NATIVE_THREAD));
            assert!(terminal.records.iter().all(|r| r.outcome.admitted()));
        } else {
            assert!(eventually(WAIT, || !pushes.all().is_empty()));
            let (on, terminal) = pushes.all()[0].clone();
            assert_eq!(on.as_deref(), Some(arming::NATIVE_THREAD));
            assert!(
                terminal
                    .records
                    .iter()
                    .all(|r| r.outcome == Outcome::Refused(arming::WAITER_NEVER_RAN.into()))
            );
            let state = fake.0.lock().unwrap();
            assert!(
                state.calls.is_empty() && state.registers == 0,
                "{:?}",
                state.calls
            );
            drop(state);
            start.send(()).unwrap();
        }
        assert!(threads.join(WAIT));
        assert_eq!(pushes.all().len(), 1, "pushed twice");
    }
}

/// A waiter that cannot start leaves the run unarmed (no loader thread, nothing registered, one push at once); a loader
/// thread that cannot start after the waiter did concludes the run without the loader, pushed once by the caller, and
/// the waiter ends without pushing.
#[test]
fn a_waiter_or_loader_that_cannot_start_concludes_the_run_once() {
    for waiter_fails in [true, false] {
        let game = directory();
        let fake = Fake::default();
        let port = WitnessPort::Wired(witness(&Arc::new(AtomicBool::new(false))));
        let run = NativeRun::new(
            policy_plan(&game.0),
            port.witness(),
            NativePermission::ALWAYS,
        );
        let (threads, pushes) = (Threads::default(), Pushes::default());
        let armed = if waiter_fails {
            arming::arm_and_push(
                &run,
                &port,
                threads.spawn(arming::NATIVE_THREAD),
                |_| Err("no more threads".to_owned()),
                WAIT * 4,
                fake_body(&run, &fake),
                pushes.push(),
            )
        } else {
            arming::arm_and_push(
                &run,
                &port,
                |_| Err("no more threads".to_owned()),
                threads.spawn(arming::DEADLINE_THREAD),
                WAIT * 4,
                fake_body(&run, &fake),
                pushes.push(),
            )
        };
        let Armed::NotArmed(reason) = armed else {
            panic!("{armed:?}");
        };
        assert!(reason.contains("no more threads"), "{reason}");
        assert_eq!(reason.contains("deadline waiter"), waiter_fails, "{reason}");
        assert_eq!(
            threads.names(),
            if waiter_fails {
                Vec::<String>::new()
            } else {
                vec![arming::DEADLINE_THREAD.to_owned()]
            }
        );
        assert!(threads.join(WAIT), "the waiter waited for its deadline");
        let all = pushes.all();
        assert_eq!(all.len(), 1);
        assert_eq!(all[0].1.not_armed.as_deref(), Some(reason.as_str()));
        let state = fake.0.lock().unwrap();
        assert!(state.registers == 0 && state.loads.is_empty());
    }
}

/// Nothing changes for a run that is not armed: no deadline waiter is started, the terminal is pushed once, at once.
#[test]
fn a_run_that_is_not_armed_starts_no_deadline_waiter() {
    for (permission, dlls, wired) in [
        (
            NativePermission {
                policy: NativePolicy::Never,
                verified: true,
            },
            true,
            true,
        ),
        (NativePermission::ALWAYS, false, true),
        (NativePermission::ALWAYS, true, false),
    ] {
        let game = directory();
        let plan = if dlls {
            policy_plan(&game.0)
        } else {
            NativePlan {
                plan_id: PLAN,
                packages: vec![package(&game.0, "l", Vec::new(), &[])],
            }
        };
        let port = if wired {
            WitnessPort::Wired(witness(&Arc::new(AtomicBool::new(false))))
        } else {
            WitnessPort::NotWired
        };
        let run = NativeRun::new(plan, port.witness(), permission);
        let pushes = Pushes::default();
        let armed = arming::arm_and_push(
            &run,
            &port,
            |_| -> Result<(), String> { panic!("no loader thread for a run that is not armed") },
            |_| -> Result<(), String> { panic!("no deadline waiter for a run that is not armed") },
            Duration::ZERO,
            || -> LoaderBody { panic!("no loader body for a run that is not armed") },
            pushes.push(),
        );
        assert!(matches!(armed, Armed::NotArmed(_)), "{armed:?}");
        let all = pushes.all();
        assert_eq!(all.len(), 1);
        assert_eq!(all[0].0, std::thread::current().name().map(str::to_owned));
        assert!(all[0].1.not_armed.is_some() && !all[0].1.deadline_passed);
    }
}

/// The run concludes once, whoever comes first, deterministically in each order: the loader's last result before the
/// deadline (the waiter then has nothing to push), the deadline before the loader's result (the late result is only
/// logged; every later caller gets the waiter's terminal), and a waiter that comes while another caller is still
/// building the terminal (it waits for that one; the witness is rechecked once).
#[test]
fn the_run_concludes_once_whoever_comes_first() {
    struct Counting {
        rechecks: AtomicUsize,
        hold: Mutex<Option<(mpsc::Sender<()>, mpsc::Receiver<()>)>>,
    }
    impl Witness for Counting {
        fn ready(&self) -> Result<WitnessToken, String> {
            Ok(WitnessToken {
                plan_id: PLAN,
                revision: 1,
            })
        }
        fn recheck(&self, _: &WitnessToken) -> Result<(), String> {
            self.rechecks.fetch_add(1, Ordering::SeqCst);
            if let Some((inside, release)) = self.hold.lock().unwrap().take() {
                inside.send(()).unwrap();
                release.recv().unwrap();
            }
            Ok(())
        }
    }
    let one = |game: &Path| NativePlan {
        plan_id: PLAN,
        packages: vec![package(
            game,
            "h",
            vec![planned(game, "h", "slow.dll", fixture())],
            &[],
        )],
    };
    let executing = |run: &Arc<NativeRun>, fake: &Fake| {
        let (run, fake) = (Arc::clone(run), fake.clone());
        std::thread::spawn(move || {
            let mut enforcer = Enforcer::early(fake.clone(), fake).unwrap();
            run.execute(&mut enforcer, &NativeContracts::shipped().unwrap());
        })
    };
    let counting = || {
        Arc::new(Counting {
            rechecks: AtomicUsize::new(0),
            hold: Mutex::new(None),
        })
    };

    // 1. every DLL terminal before the deadline: the waiter, even past its deadline, concludes nothing
    let game = directory();
    let fake = Fake::default();
    let w = counting();
    let run = NativeRun::new(one(&game.0), w.clone(), NativePermission::ALWAYS);
    executing(&run, &fake).join().unwrap();
    assert_eq!(run.expire(std::time::Instant::now()), None);
    let terminal = run.wait(Duration::ZERO);
    assert!(!terminal.deadline_passed && terminal.records[0].outcome.admitted());
    assert_eq!(
        run.expire(std::time::Instant::now()),
        None,
        "an in-time terminal is never the waiter's"
    );
    assert_eq!(w.rechecks.load(Ordering::SeqCst), 1);

    // 2. the deadline before the loader's result: the waiter's terminal is the run's
    let game = directory();
    let fake = Fake::default();
    let (release, gate) = mpsc::channel();
    fake.0.lock().unwrap().block = Some(("slow.dll".into(), gate));
    let w = counting();
    let run = NativeRun::new(one(&game.0), w.clone(), NativePermission::ALWAYS);
    let loader = executing(&run, &fake);
    assert!(eventually(WAIT, || fake.0.lock().unwrap().loads.len() == 1));
    let expired = run
        .expire(std::time::Instant::now())
        .expect("the deadline passed");
    assert!(expired.deadline_passed);
    assert_eq!(
        expired.records[0].outcome,
        Outcome::TimedOut(TIMED_OUT.into())
    );
    release.send(()).unwrap();
    loader.join().unwrap();
    assert_eq!(run.wait(WAIT), expired);
    assert_eq!(run.expire(std::time::Instant::now()), Some(expired));
    assert_eq!(run.late_results().len(), 1);
    assert_eq!(w.rechecks.load(Ordering::SeqCst), 1);

    // 3. the waiter comes while another caller builds the terminal: it gets that terminal, the witness is rechecked once
    let game = directory();
    let fake = Fake::default();
    let (release, gate) = mpsc::channel();
    fake.0.lock().unwrap().block = Some(("slow.dll".into(), gate));
    let w = counting();
    let (inside_tx, inside) = mpsc::channel();
    let (go, held) = mpsc::channel();
    *w.hold.lock().unwrap() = Some((inside_tx, held));
    let run = NativeRun::new(one(&game.0), w.clone(), NativePermission::ALWAYS);
    let loader = executing(&run, &fake);
    assert!(eventually(WAIT, || fake.0.lock().unwrap().loads.len() == 1));
    let first = {
        let run = Arc::clone(&run);
        std::thread::spawn(move || run.wait(Duration::ZERO))
    };
    inside.recv().unwrap();
    let second = {
        let run = Arc::clone(&run);
        std::thread::spawn(move || run.expire(std::time::Instant::now()))
    };
    std::thread::sleep(Duration::from_millis(100));
    assert!(
        !second.is_finished(),
        "the waiter built a terminal of its own"
    );
    go.send(()).unwrap();
    let first = first.join().unwrap();
    let second = second.join().unwrap();
    assert!(first.deadline_passed);
    assert_eq!(second, Some(first.clone()));
    assert_eq!(w.rechecks.load(Ordering::SeqCst), 1, "rechecked twice");
    release.send(()).unwrap();
    loader.join().unwrap();
    assert_eq!(run.wait(Duration::ZERO), first);
}

/// Stress of the race the waiter and the loader run at the deadline: in every round the load finishes when the
/// deadline passes (give or take 1.5 ms). Whoever wins, exactly one terminal is pushed, it is the run's terminal, and it
/// is consistent: `deadlinePassed` exactly when the DLL timed out, a DLL the loader was called for times out with the
/// initialization text and its late load is only logged, and one it was not called for times out as not started. On
/// a loaded machine the clock can also pass before the loader thread reaches the DLL (not started, maybe with no late
/// result at all) or before the waiter ran (the loader then loads nothing: WAITER_NEVER_RAN); those rounds must be just
/// as consistent.
#[test]
fn a_load_finishing_at_the_deadline_still_gives_exactly_one_terminal() {
    const ROUNDS: usize = 240;
    let deadline = Duration::from_millis(40);
    let (mut in_time, mut late_loaded, mut late_not_started, mut not_reached, mut waiter_late) =
        (0, 0, 0, 0, 0);
    for round in 0..ROUNDS {
        let game = directory();
        let fake = Fake::default();
        let plan = NativePlan {
            plan_id: PLAN,
            packages: vec![package(
                &game.0,
                "h",
                vec![planned(&game.0, "h", "slow.dll", fixture())],
                &[],
            )],
        };
        let port = WitnessPort::Wired(witness(&Arc::new(AtomicBool::new(false))));
        let run = NativeRun::new(plan, port.witness(), NativePermission::ALWAYS);
        let (threads, pushes) = (Threads::default(), Pushes::default());
        // -1.5 ms .. +1.5 ms around the deadline, in 0.25 ms steps
        let offset = (round % 13) as i64 * 250 - 1500;
        let now = std::time::Instant::now();
        let finish = if offset < 0 {
            now + deadline - Duration::from_micros(offset.unsigned_abs())
        } else {
            now + deadline + Duration::from_micros(offset as u64)
        };
        fake.0.lock().unwrap().finish_at = Some(("slow.dll".into(), finish));
        let armed = arm_push(
            &run,
            &port,
            &threads,
            deadline,
            fake_body(&run, &fake),
            &pushes,
        );
        assert_eq!(armed, Armed::Started);
        assert!(threads.join(WAIT), "round {round}: a thread never ended");
        let all = pushes.all();
        assert_eq!(all.len(), 1, "round {round}: {} pushes", all.len());
        let terminal = &all[0].1;
        assert_eq!(&run.wait(Duration::ZERO), terminal, "round {round}");
        let late = run.late_results();
        let loads = fake.0.lock().unwrap().loads.len();
        assert!(loads <= 1, "round {round}");
        match &terminal.records[0].outcome {
            Outcome::Loaded { .. } => {
                assert!(
                    !terminal.deadline_passed && late.is_empty() && loads == 1,
                    "round {round}"
                );
                assert_eq!(all[0].0.as_deref(), Some(arming::NATIVE_THREAD));
                in_time += 1;
            }
            Outcome::TimedOut(text) if loads == 1 => {
                // the loader was called for slow.dll before the run closed
                assert!(terminal.deadline_passed, "round {round}");
                assert_eq!(text, TIMED_OUT, "round {round}");
                assert!(
                    late.len() == 1 && late[0].1.admitted(),
                    "round {round}: {late:?}"
                );
                assert!(terminal.withdrawn.iter().any(|w| w.mod_id == "h"));
                late_loaded += 1;
            }
            Outcome::TimedOut(text) => {
                // the run closed before the loader was called for slow.dll: at its commit point (a late result that
                // ran nothing) or before the loader thread got that far (no late result)
                assert!(terminal.deadline_passed, "round {round}");
                assert_eq!(
                    text, "not started: the native startup deadline passed",
                    "round {round}"
                );
                assert!(
                    late.len() <= 1 && late.iter().all(|(_, o)| !o.admitted()),
                    "round {round}: {late:?}"
                );
                assert!(terminal.withdrawn.iter().any(|w| w.mod_id == "h"));
                if late.is_empty() {
                    not_reached += 1;
                } else {
                    late_not_started += 1;
                }
            }
            Outcome::Refused(reason) if reason == arming::WAITER_NEVER_RAN => {
                // the clock passed before the waiter ran: the loader loaded nothing and pushed
                assert!(
                    !terminal.deadline_passed && late.is_empty() && loads == 0,
                    "round {round}"
                );
                assert_eq!(all[0].0.as_deref(), Some(arming::NATIVE_THREAD));
                waiter_late += 1;
            }
            other => panic!("round {round}: {other:?}"),
        }
    }
    eprintln!(
        "{ROUNDS} rounds: in time {in_time}, timed out while loading {late_loaded}, timed out at the commit point {late_not_started}, timed out before the loader reached the DLL {not_reached}, waiter not run by the deadline {waiter_late}"
    );
}

// ---- review: no caller waits past a bound on a conclusion whose recheck does not answer ----

/// A witness whose first recheck holds until released, after saying it is inside.
struct Holding {
    rechecks: AtomicUsize,
    hold: Mutex<Option<(mpsc::Sender<()>, mpsc::Receiver<()>)>>,
}
impl Witness for Holding {
    fn ready(&self) -> Result<WitnessToken, String> {
        Ok(WitnessToken {
            plan_id: PLAN,
            revision: 1,
        })
    }
    fn recheck(&self, _: &WitnessToken) -> Result<(), String> {
        self.rechecks.fetch_add(1, Ordering::SeqCst);
        if let Some((inside, release)) = self.hold.lock().unwrap().take() {
            inside.send(()).unwrap();
            release.recv().unwrap();
        }
        Ok(())
    }
}
/// A [`Holding`] witness, the channel that says its recheck is inside, and the sender that lets it answer.
fn holding() -> (Arc<Holding>, mpsc::Receiver<()>, mpsc::Sender<()>) {
    let (inside_tx, inside) = mpsc::channel();
    let (release, held) = mpsc::channel();
    let witness = Arc::new(Holding {
        rechecks: AtomicUsize::new(0),
        hold: Mutex::new(Some((inside_tx, held))),
    });
    (witness, inside, release)
}
/// How far past [`RECHECK_ALLOWANCE`] a bounded wait may end on a loaded machine before the test calls it unbounded.
const SLACK: Duration = Duration::from_secs(20);
/// One package, h, with slow.dll.
fn slow_plan(game: &Path) -> NativePlan {
    NativePlan {
        plan_id: PLAN,
        packages: vec![package(
            game,
            "h",
            vec![planned(game, "h", "slow.dll", fixture())],
            &[],
        )],
    }
}

/// A caller that comes while another caller's witness recheck runs waits for it (a slow recheck still gets its say),
/// but at most RECHECK_ALLOWANCE after the conclusion began: kiln-eml's recheck is a call nothing here can stop. Then
/// the run's terminal is published not coherent (RECHECK_UNANSWERED); every waiting caller gets it, the deadline waiter
/// too (so it pushes it), and the caller still inside the recheck gets that same terminal when it answers at last. One
/// terminal, one recheck.
#[test]
fn no_caller_waits_past_the_allowance_for_a_recheck_that_does_not_answer() {
    let game = directory();
    let fake = Fake::default();
    let (w, inside, release) = holding();
    let run = NativeRun::new(slow_plan(&game.0), w.clone(), NativePermission::ALWAYS);
    {
        let mut enforcer = Enforcer::early(fake.clone(), fake.clone()).unwrap();
        run.execute(&mut enforcer, &NativeContracts::shipped().unwrap());
    }
    // the loader's side concludes the run (every DLL terminal in time) and its recheck does not answer
    let began = std::time::Instant::now();
    let first = {
        let run = Arc::clone(&run);
        std::thread::spawn(move || run.wait(Duration::ZERO))
    };
    inside.recv().unwrap();
    let asked = std::time::Instant::now();
    let second = {
        let run = Arc::clone(&run);
        std::thread::spawn(move || (run.wait(STARTUP_NATIVE_DEADLINE), std::time::Instant::now()))
    };
    let waiter = {
        let run = Arc::clone(&run);
        std::thread::spawn(move || {
            (
                run.expire(std::time::Instant::now()),
                std::time::Instant::now(),
            )
        })
    };
    if !eventually(RECHECK_ALLOWANCE + SLACK, || {
        second.is_finished() && waiter.is_finished()
    }) {
        release.send(()).unwrap();
        panic!(
            "a caller still waits {:?} after the recheck began",
            asked.elapsed()
        );
    }
    // the conclusion began after `began`, and neither caller may stop waiting for its recheck before the allowance
    // after that (a loaded machine only makes them later)
    let (second, second_at) = second.join().unwrap();
    let (expired, waiter_at) = waiter.join().unwrap();
    for at in [second_at, waiter_at] {
        assert!(
            at - began >= RECHECK_ALLOWANCE,
            "a caller did not wait for the recheck: it gave up {:?} after the run was concluded",
            at - began
        );
    }
    assert_eq!(second.coherent, Err(RECHECK_UNANSWERED.to_owned()));
    assert!(!second.deadline_passed && second.records[0].outcome.admitted());
    assert_eq!(
        expired,
        Some(second.clone()),
        "the deadline waiter did not get the terminal to push"
    );
    // the recheck answers at last: its caller gets the published terminal, not one of its own
    release.send(()).unwrap();
    assert_eq!(first.join().unwrap(), second);
    assert_eq!(run.wait(Duration::ZERO), second);
    assert_eq!(run.expire(std::time::Instant::now()), Some(second));
    assert_eq!(w.rechecks.load(Ordering::SeqCst), 1, "rechecked twice");
}

/// The deadline waiter concludes a hung run at the deadline and its witness recheck does not answer (say kiln-eml's
/// recheck needs the loader lock the hung initializer holds): once the load returns, the loader thread waits for that
/// conclusion at most RECHECK_ALLOWANCE and then pushes the run's terminal, timed out and not coherent; the late load is
/// logged; when the recheck answers at last, nothing is pushed again. The deadline is signalled once the loader is
/// inside the load (a clock deadline could pass before a loaded machine lets the loader get there).
#[test]
fn a_deadline_conclusion_stuck_in_its_recheck_is_still_pushed_once() {
    let game = directory();
    let fake = Fake::default();
    let (release_load, gate) = mpsc::channel();
    fake.0.lock().unwrap().block = Some(("slow.dll".into(), gate));
    let (w, inside, release) = holding();
    let port = WitnessPort::Wired(w.clone());
    let run = NativeRun::new(slow_plan(&game.0), port.witness(), NativePermission::ALWAYS);
    let (threads, pushes) = (Threads::default(), Pushes::default());
    let (pass, deadline) = mpsc::channel();
    let armed = arm_push(
        &run,
        &port,
        &threads,
        Deadline::Signalled(deadline),
        fake_body(&run, &fake),
        &pushes,
    );
    assert_eq!(armed, Armed::Started);
    assert!(
        eventually(WAIT, || fake.0.lock().unwrap().loads == ["slow.dll"]),
        "the loader never reached slow.dll"
    );
    pass.send(()).unwrap();
    // the waiter concluded at the deadline and is inside the recheck; the load still hangs
    inside.recv().unwrap();
    assert!(pushes.all().is_empty());
    release_load.send(()).unwrap();
    if !eventually(RECHECK_ALLOWANCE + SLACK, || !pushes.all().is_empty()) {
        release.send(()).unwrap();
        panic!("nothing pushed while the deadline's recheck does not answer");
    }
    let (on, terminal) = pushes.all()[0].clone();
    assert_eq!(on.as_deref(), Some(arming::NATIVE_THREAD));
    assert!(terminal.deadline_passed);
    assert_eq!(terminal.coherent, Err(RECHECK_UNANSWERED.to_owned()));
    assert_eq!(
        terminal.records[0].outcome,
        Outcome::TimedOut(TIMED_OUT.into())
    );
    assert_eq!(fake.0.lock().unwrap().records, [LATE_SLOW_LOADED]);
    release.send(()).unwrap();
    assert!(threads.join(WAIT), "a thread of the run never ended");
    assert_eq!(pushes.all().len(), 1, "pushed twice");
    assert_eq!(run.wait(Duration::ZERO), terminal);
    assert_eq!(w.rechecks.load(Ordering::SeqCst), 1, "rechecked twice");
}

/// A witness recheck that panics confirms nothing: the run's one terminal is not coherent (an internal error), the
/// caller that came while it ran gets that same terminal (nobody is left waiting) and the panic never reaches a caller.
#[test]
fn a_recheck_that_panics_leaves_one_terminal_not_coherent() {
    struct Panicking {
        rechecks: AtomicUsize,
        hold: Mutex<Option<(mpsc::Sender<()>, mpsc::Receiver<()>)>>,
    }
    impl Witness for Panicking {
        fn ready(&self) -> Result<WitnessToken, String> {
            Ok(WitnessToken {
                plan_id: PLAN,
                revision: 1,
            })
        }
        fn recheck(&self, _: &WitnessToken) -> Result<(), String> {
            self.rechecks.fetch_add(1, Ordering::SeqCst);
            if let Some((inside, release)) = self.hold.lock().unwrap().take() {
                inside.send(()).unwrap();
                release.recv().unwrap();
            }
            // resume_unwind, not panic!: it skips the panic hook. With RUST_BACKTRACE set the hook prints a backtrace,
            // reading this test binary's debug info inside the 5 s RECHECK_ALLOWANCE the other caller waits; on a
            // loaded machine that outlasted the allowance, and the run's terminal was then RECHECK_UNANSWERED.
            std::panic::resume_unwind(Box::new("the test recheck fails"))
        }
    }
    let game = directory();
    let fake = Fake::default();
    let (inside_tx, inside) = mpsc::channel();
    let (release, held) = mpsc::channel();
    let w = Arc::new(Panicking {
        rechecks: AtomicUsize::new(0),
        hold: Mutex::new(Some((inside_tx, held))),
    });
    let run = NativeRun::new(slow_plan(&game.0), w.clone(), NativePermission::ALWAYS);
    {
        let mut enforcer = Enforcer::early(fake.clone(), fake.clone()).unwrap();
        run.execute(&mut enforcer, &NativeContracts::shipped().unwrap());
    }
    let first = {
        let run = Arc::clone(&run);
        std::thread::spawn(move || run.wait(Duration::ZERO))
    };
    inside.recv().unwrap();
    let second = {
        let run = Arc::clone(&run);
        std::thread::spawn(move || run.wait(Duration::ZERO))
    };
    std::thread::sleep(Duration::from_millis(100));
    assert!(
        !second.is_finished(),
        "a caller did not wait for the recheck"
    );
    release.send(()).unwrap();
    let first = first
        .join()
        .expect("the recheck's panic reached its caller");
    let second = second.join().unwrap();
    assert_eq!(first, second);
    assert!(
        first
            .coherent
            .as_ref()
            .is_err_and(|e| e.contains("internal error")),
        "{:?}",
        first.coherent
    );
    assert!(!first.deadline_passed && first.records[0].outcome.admitted());
    assert_eq!(run.wait(Duration::ZERO), first);
    assert_eq!(w.rechecks.load(Ordering::SeqCst), 1, "rechecked twice");
}
