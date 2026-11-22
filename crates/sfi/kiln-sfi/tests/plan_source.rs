// SPDX-License-Identifier: MIT
//! The production plan source. Under backend `kiln` the boundary reads the launch plan the core
//! froze once (early phase, else main), checks its hash against the record and parses the same bytes with
//! the strict reader; the worker's runtime follows that read ([`kiln_sfi::plan_source::KilnPlanSource`]),
//! re-hashes each package folder and discovers `<game>/mods`. Nothing below `<game>/shroudforge` is
//! read, never profile.json or launch.json from disk; a plan kiln-sfi cannot follow is a refused start, never
//! a hosted one. Tests 1-8 follow the plan source's rules; test 7's carry-forward half is in state.rs.
#[path = "support/fake_archive.rs"]
mod fake_archive;
#[path = "support/fake_startup.rs"]
mod fake_startup;
mod support;

use kiln_abi::services::kiln_launch::*;
use kiln_abi::*;
use kiln_sfi::lifecycle;
use kiln_sfi::plan_source::{
    self, Admission, KilnPlanSource, NativeVerdict, PlanRead, StartupVerdict, Verdicts,
};
use kiln_sfi::state::{Intent, Lua, Phase as RecordPhase, Trigger};
use kiln_sfi::worker::{Worker, WorkerPhase};
use kiln_sfi::{Boundary, InactiveReason, Phase};
use serde_json::{Value, json};
use sfi_policy::digest::package_content_sha256;
use sfi_policy::launch_plan::{LaunchPlan, ValidationContext};
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;
use support::*;

/// A liveness bound, not a latency claim: how long a wait for something that must happen holds before the test
/// reports a hang. A passing wait returns as soon as its condition holds, so this sits far above any delay that
/// scheduling alone can cause on a loaded machine (300 s, as worker.rs and the lifecycle bodies).
const WAIT: Duration = Duration::from_secs(300);
const KILN: (i32, i32) = (KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN);

// ---- packages and plans ----

/// A ShroudForge package folder `<game>/mods/<folder>` whose Lua runs `lua` at its top level (the tests log
/// `<id>:<word>` with shroudforge.log.info: print is DEBUG, below ShroudForge's default level);
/// returns its content digest.
fn put_package(game: &Path, folder: &str, id: &str, capabilities: &[&str], lua: &str) -> [u8; 32] {
    let dir = game.join("mods").join(folder);
    std::fs::create_dir_all(dir.join("src")).unwrap();
    std::fs::write(
        dir.join("mod.json"),
        json!({"id": id, "name": id, "version": "1.0.0", "capabilities": capabilities}).to_string(),
    )
    .unwrap();
    std::fs::write(dir.join("src").join("mod.lua"), lua).unwrap();
    package_content_sha256(&dir).unwrap()
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
/// A runtime candidate (Lua admitted, no data, no DLL) whose folder has `content`.
fn candidate(id: &str, folder: &str, content: [u8; 32]) -> Value {
    json!({"id":id,"version":"1.0.0","kind":"shroudforge","root":format!("mods/{folder}"),
      "content_sha256":hex(&content),"config_sha256":"d".repeat(64),"enabled":true,"targets":["client","server"],
      "capabilities":["runtime"],"dependencies":[],"requiredApis":[],"services":[],"requires180":false,
      "saveSensitive":false,"decision":"candidate","reasons":[],
      "data":{"required":false,"admission":"not-needed"},"lua":{"required":true,"admission":"admit"},"dlls":[]})
}
fn depends_on(mut package: Value, on: &[&str]) -> Value {
    package["dependencies"] = on
        .iter()
        .map(|id| json!({"id":id,"range":"^1.0","optional":false,"included":true}))
        .collect();
    package
}
fn with_dll(mut package: Value) -> Value {
    package["dlls"] = json!([{"path":"native/x.dll","sha256":"e".repeat(64),"mode":"library","source":"ini",
        "required":true,"gate":"pass","gateProof_sha256":"f".repeat(64),"bridgeAbi":0}]);
    package
}
fn with_patch(mut package: Value) -> Value {
    package["capabilities"] = json!(["runtime", "patch"]);
    package["data"] = json!({"required":true,"admission":"admit"});
    package
}
fn listed(mut package: Value, decision: &str) -> Value {
    package["decision"] = json!(decision);
    package["data"]["admission"] = json!(if package["data"]["required"] == true {
        "refuse"
    } else {
        "not-needed"
    });
    package["lua"]["admission"] = json!("refuse");
    match decision {
        "skip" => {
            package["reasons"] = json!([{"code":"x","message":"skipped by the planner","causedBy":[],"severity":"warning"}])
        }
        "disabled" => package["enabled"] = json!(false),
        "other-side" => package["targets"] = json!(["server"]),
        _ => {}
    }
    package
}
/// A plan document for this fake's executable on `side`.
fn plan(side: &str, packages: Vec<Value>, order: &[&str], dialect: &str) -> Value {
    let mut plan = plan_document(side);
    plan["packages"] = Value::Array(packages);
    plan["loadOrder"] = json!(order);
    plan["startupDialect"] = json!(dialect);
    plan
}
fn parsed(document: &Value) -> LaunchPlan {
    LaunchPlan::parse(
        &serde_json::to_vec(document).unwrap(),
        &ValidationContext {
            exe_sha256: FAKE_EXE.into(),
            side: document["game"]["side"].as_str().unwrap().into(),
            upstream_180_final: false,
        },
    )
    .unwrap_or_else(|e| panic!("the test plan must parse: {e}"))
}

// ---- a start ----

struct Boot {
    boundary: &'static Boundary,
    worker: &'static Worker,
    normal: KilnOwner,
    pool: Arc<FakePool>,
    info: KilnPluginInfo,
    state: PathBuf,
}
impl Boot {
    fn lines(&self) -> Vec<String> {
        logs(self.normal).into_iter().map(|(_, t)| t).collect()
    }
    fn has(&self, text: &str) -> bool {
        self.lines().iter().any(|l| l.contains(text))
    }
    fn running(&self) -> bool {
        eventually(WAIT, || {
            self.worker
                .shared()
                .is_some_and(|s| s.phase() == WorkerPhase::Running)
        })
    }
    /// An orderly stop, so the fake pool's thread ends.
    fn stop(&self) {
        if let Some(shared) = self.worker.shared() {
            shared.request_stop();
            assert!(self.pool.task(0).wait_finished(WAIT));
        }
    }
    fn record(&self) -> kiln_sfi::state::Reducer {
        self.boundary.status().record().expect("a record")
    }
}

#[derive(Default)]
struct Case {
    /// Run the early phase (a real start does).
    skip_early: bool,
    /// Runs between the early phase and main.
    between: Option<fn()>,
    state: Option<PathBuf>,
    /// Runs with kiln-sfi's owner and the API before the plugin step.
    prepare: Option<fn(KilnOwner, &mut KilnApi)>,
    /// kiln-eml has no startup seam: nothing is granted, so the runtime-create is refused.
    no_seam: bool,
}

/// The early phase, Kiln's plugin step and its on_ready, with the production worker factory.
fn boot(case: Case) -> Boot {
    set_launch(Some(KILN));
    let state = case.state.unwrap_or_else(|| fresh_dir("ps1-state"));
    set_state_dir(state.clone());
    let worker: &'static Worker =
        Box::leak(Box::new(Worker::new(lifecycle::factory::<KilnPlanSource>)));
    let boundary: &'static Boundary = Box::leak(Box::new(
        Boundary::with_worker(InactiveReason::NegotiationMissing, worker)
            .with_own_loader(|| None)
            .with_startup(if case.no_seam {
                fake_startup::none
            } else {
                fake_startup::resolve
            }),
    ));
    let (early, normal) = (next_owner(), next_owner());
    let pool = FakePool::new(normal, PoolConfig::default());
    let api = Box::leak(api(true));
    // kiln-eml's startup seam grants the plan this thread serves, and its archive serves that generation
    fake_archive::wrap(api);
    match std::fs::read(plan_path()) {
        Ok(_) if !case.no_seam => fake_startup::install(fake_startup::Config::granting_current()),
        _ => fake_startup::uninstall(),
    }
    if let Some(prepare) = case.prepare {
        prepare(normal, api);
    }
    let (mut early_info, mut info) = (plugin_info(), plugin_info());
    if !case.skip_early {
        assert_eq!(
            unsafe { boundary.enter(Phase::Early, &*api, early, &mut early_info) },
            KILN_OK
        );
        assert!(early_info.on_ready.is_none());
    }
    if let Some(between) = case.between {
        between();
    }
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*api, normal, &mut info) },
        KILN_OK
    );
    fake_startup::grant_archive(normal);
    if let Some(ready) = info.on_ready {
        unsafe { ready(info.user) };
    }
    Boot {
        boundary,
        worker,
        normal,
        pool,
        info,
        state,
    }
}

fn refused_with(b: &Boot, code: &str, said: &str) {
    assert!(b.info.on_ready.is_none(), "{code}: no on_ready, no start");
    assert!(b.pool.submissions().is_empty(), "{code}: no submit");
    assert!(!b.boundary.worker_started(), "{code}");
    let record = b.record();
    assert_eq!(record.phase(), RecordPhase::Refused, "{code}");
    let v: Value =
        serde_json::from_slice(&std::fs::read(b.state.join("sfi.json")).unwrap()).unwrap();
    assert_eq!(v["reason"]["code"], code);
    let text = v["reason"]["text"].as_str().unwrap();
    assert!(text.contains(said), "{code}: {text}");
    // the loud warning, and both capability rows say it
    assert!(
        logs(b.normal)
            .iter()
            .any(|(level, l)| *level == KILN_LOG_ERROR
                && l.starts_with("kiln-sfi: WARNING:")
                && l.contains(said)),
        "{code}: {:?}",
        b.lines()
    );
    assert!(
        reports(b.normal).iter().any(|(_, row)| row.contains(said)),
        "{code}"
    );
}

// ---- 1. hash mismatch ----

#[test]
fn a_plan_whose_hash_is_not_the_frozen_one_is_refused_and_nothing_starts() {
    let game = game_dir();
    let content = put_package(
        &game,
        "a",
        "alpha",
        &["runtime"],
        "shroudforge.log.info('alpha:ran')",
    );
    set_plan(Some(
        serde_json::to_vec(&plan(
            "client",
            vec![candidate("alpha", "a", content)],
            &["alpha"],
            "none",
        ))
        .unwrap(),
    ));
    set_frozen_hash(Some([7; 32]));
    let b = boot(Case::default());
    refused_with(&b, "plan-changed", "changed since this start froze it");
    assert_eq!(b.boundary.plan_reads(), 1, "the early read, reused at main");
    assert!(!b.has("alpha:ran"), "no Lua");
    set_frozen_hash(None);
}

// ---- 2. strict refusal of a plan the core accepted ----

#[test]
fn a_plan_the_strict_reader_refuses_is_refused_never_hosted() {
    let game = game_dir();
    let content = put_package(
        &game,
        "a",
        "alpha",
        &["runtime"],
        "shroudforge.log.info('alpha:ran')",
    );
    let good = plan(
        "client",
        vec![candidate("alpha", "a", content)],
        &["alpha"],
        "none",
    );
    // Each variant passes the core's checks (launch.cpp check_plan reads only format, ids, game, backend,
    // contract and requires180/bridgeAbi of packages) and fails kiln-sfi's strict reader.
    let mut variants: Vec<(&str, Value)> = Vec::new();
    let mut v = good.clone();
    v["packages"][0]["unknown"] = json!(true);
    variants.push(("an unknown package key", v));
    let mut v = good.clone();
    v["packages"][0] = with_dll(candidate("alpha", "a", content));
    v["packages"][0]["dlls"][0]["gate"] = json!("unknown");
    variants.push(("a candidate DLL without a gate pass", v));
    let mut v = good.clone();
    v["startupDialect"] = json!("eml");
    variants.push(("a startup dialect the packages contradict", v));
    let mut v = good.clone();
    v["loadOrder"] = json!([]);
    variants.push(("a candidate missing from the load order", v));
    let mut v = good.clone();
    v["packages"][0]["root"] = json!("mods/a/b");
    variants.push(("a root of two folders", v));
    for (label, document) in variants {
        set_plan(Some(serde_json::to_vec(&document).unwrap()));
        let b = boot(Case::default());
        refused_with(
            &b,
            "plan-invalid",
            "is not a launch plan Kiln's interpreter reads",
        );
        assert!(!b.has("alpha:ran"), "{label}: no Lua");
        assert!(
            !reports(b.normal)
                .iter()
                .any(|(_, r)| r.contains("kiln-shroudforge has this start")),
            "{label}: never hosted"
        );
    }
    // a plan whose id is not the one its file name carries (the profile names the file by another id)
    let mut other = good.clone();
    other["plan_id"] = json!("0123abcd-0000-4000-8000-0000000000ff");
    let bytes = serde_json::to_vec(&other).unwrap();
    set_plan(Some(bytes.clone()));
    let named = game
        .join("kiln")
        .join("launch-plans")
        .join(format!("{DEFAULT_PLAN_ID}.json"));
    std::fs::write(&named, &bytes).unwrap();
    let hash: String = sfi_policy::digest::sha256(&bytes)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    set_profile(Some(format!(
        r#"{{"sfLaunchPlan":{{"path":"launch-plans/{DEFAULT_PLAN_ID}.json","sha256":"{hash}"}}}}"#
    )));
    let b = boot(Case::default());
    refused_with(
        &b,
        "plan-invalid",
        "has a plan id that is not the one its file name carries",
    );
    set_profile(None);
    // the profile's reference must be the core's shape too
    for profile in [
        r#"{"id":"test"}"#.to_owned(),
        r#"{"sfLaunchPlan":{"path":"launch-plans/../x.json","sha256":"00"}}"#.to_owned(),
        r#"{"sfLaunchPlan":{"path":"elsewhere/0123abcd-0000-4000-8000-000000000001.json","sha256":"aa"}}"#.to_owned(),
    ] {
        set_plan(Some(serde_json::to_vec(&good).unwrap()));
        set_profile(Some(profile.clone()));
        let b = boot(Case::default());
        refused_with(&b, "plan-unreadable", "");
        set_profile(None);
    }
}

/// Two packages whose roots are one folder under two names (here a link or junction; under Wine or on Windows also an
/// 8.3 name, sfi-policy's `two_roots_that_are_one_folder_are_refused`) are overlapping roots: no string comparison
/// sees them, so the plan is refused by identity before anything runs.
#[test]
fn two_packages_in_one_folder_under_two_names_are_refused() {
    let game = game_dir();
    let content = put_package(
        &game,
        "a",
        "alpha",
        &["runtime"],
        "shroudforge.log.info('alpha:ran')",
    );
    let other = game.join("mods").join("b");
    #[cfg(unix)]
    std::os::unix::fs::symlink(game.join("mods").join("a"), &other).unwrap();
    #[cfg(windows)]
    junction(&other, &game.join("mods").join("a"));
    let document = plan(
        "client",
        vec![
            candidate("alpha", "a", content),
            candidate("beta", "b", content),
        ],
        &["alpha", "beta"],
        "none",
    );
    set_plan(Some(serde_json::to_vec(&document).unwrap()));
    let b = boot(Case::default());
    refused_with(&b, "plan-invalid", "alpha and beta are one folder");
    assert!(!b.has("alpha:ran"), "no Lua");
    #[cfg(unix)]
    std::fs::remove_file(&other).unwrap();
    #[cfg(windows)]
    std::fs::remove_dir(&other).unwrap(); // removes the junction only
}

// ---- 3. tripwire: nothing below <game>/shroudforge, no profile.json or launch.json from disk ----

#[test]
fn nothing_below_the_shroudforge_folder_profile_json_or_launch_json_is_read() {
    let game = game_dir();
    let content = put_package(
        &game,
        "a",
        "alpha",
        &["runtime"],
        "shroudforge.log.info('alpha:right')",
    );
    let document = plan(
        "client",
        vec![candidate("alpha", "a", content)],
        &["alpha"],
        "none",
    );
    set_plan(Some(serde_json::to_vec(&document).unwrap()));
    // ShroudForge's own folder holds what would change the outcome if anything read it: its mods root with
    // another alpha, compatibility rules that block alpha, a profile overlay that replaces alpha's Lua.
    let sf = game.join("shroudforge");
    let other = sf.join("mods").join("a");
    std::fs::create_dir_all(other.join("src")).unwrap();
    std::fs::write(
        other.join("mod.json"),
        r#"{"id":"alpha","name":"alpha","version":"1.0.0","capabilities":["runtime"]}"#,
    )
    .unwrap();
    std::fs::write(
        other.join("src").join("mod.lua"),
        "shroudforge.log.info('alpha:WRONG mods root')",
    )
    .unwrap();
    std::fs::create_dir_all(sf.join("config").join("compatibility")).unwrap();
    std::fs::write(
        sf.join("config").join("compatibility").join("rules.json"),
        r#"{"schemaVersion":1,"conflicts":[{"first":{"id":"alpha"},"second":{"id":"alpha"},"reason":"WRONG rules"}]}"#,
    )
    .unwrap();
    std::fs::create_dir_all(sf.join("profiles")).unwrap();
    std::fs::write(
        sf.join("profiles").join("p.json"),
        r#"{"name":"p","packageFiles":{"src/mod.lua":"shroudforge.log.info('alpha:WRONG overlay')"}}"#,
    )
    .unwrap();
    // profile.json and launch.json on disk name another plan and another backend
    std::fs::write(
        game.join("kiln").join("profile.json"),
        r#"{"id":"disk","generation":9,"sfLaunchPlan":{"path":"launch-plans/ffffffff-0000-4000-8000-000000000009.json","sha256":"00"}}"#,
    )
    .unwrap();
    std::fs::create_dir_all(game.join("kiln").join("state")).unwrap();
    std::fs::write(
        game.join("kiln").join("state").join("launch.json"),
        r#"{"format":1,"backend":"hosted"}"#,
    )
    .unwrap();
    // First with every decoy readable: a read that changes the outcome shows "WRONG".
    let start = || {
        let b = boot(Case::default());
        assert!(b.running());
        assert!(
            eventually(WAIT, || b.has("[alpha] alpha:right")),
            "{:?}",
            b.lines()
        );
        assert!(!b.has("WRONG"), "{:?}", b.lines());
        b.stop();
    };
    start();
    // Then (Unix) with ShroudForge's folder unreadable: a read there that fails hard stops alpha. Readable again when
    // the test ends, passed or failed, so the folder can always be deleted.
    #[cfg(unix)]
    {
        struct Unreadable(PathBuf);
        impl Drop for Unreadable {
            fn drop(&mut self) {
                use std::os::unix::fs::PermissionsExt as _;
                let _ = std::fs::set_permissions(&self.0, std::fs::Permissions::from_mode(0o755));
            }
        }
        let _unreadable = {
            use std::os::unix::fs::PermissionsExt as _;
            std::fs::set_permissions(&sf, std::fs::Permissions::from_mode(0o000)).unwrap();
            Unreadable(sf.clone())
        };
        start();
    }
}

// ---- 4. the admission table ----

fn table_plan() -> Value {
    let c = [1u8; 32];
    plan(
        "client",
        vec![
            candidate("lib", "lib", c),
            depends_on(candidate("lua", "lua", c), &["lib"]),
            listed(candidate("skipped", "skipped", c), "skip"),
            listed(candidate("off", "off", c), "disabled"),
            listed(candidate("server-only", "server-only", c), "other-side"),
            with_dll(candidate("native", "native", c)),
            {
                let mut p = candidate("registers", "registers", c);
                p["capabilities"] = json!(["runtime", "runtime-register-dll"]);
                p
            },
            depends_on(candidate("over-native", "over-native", c), &["native"]),
            depends_on(candidate("over-over", "over-over", c), &["over-native"]),
            with_patch(candidate("patch", "patch", c)),
            depends_on(candidate("over-patch", "over-patch", c), &["patch"]),
            {
                // data-only dependency of an admitted package: registered, never run
                let mut p = candidate("data-free", "data-free", c);
                p["capabilities"] = json!([]);
                p["lua"] = json!({"required":false,"admission":"not-needed"});
                p
            },
            depends_on(
                candidate("uses-data-free", "uses-data-free", c),
                &["data-free"],
            ),
        ],
        &[
            "lib",
            "lua",
            "native",
            "registers",
            "over-native",
            "over-over",
            "patch",
            "over-patch",
            "data-free",
            "uses-data-free",
        ],
        "shroudforge",
    )
}

fn withdrawn(a: &Admission) -> BTreeMap<String, (&str, Vec<String>)> {
    a.withdrawn
        .iter()
        .map(|w| (w.id.clone(), (w.code.as_str(), w.chain.clone())))
        .collect()
}
fn set(ids: &[&str]) -> BTreeSet<String> {
    ids.iter().map(|s| s.to_string()).collect()
}

#[test]
fn admission_follows_the_plan_and_removes_what_cannot_run_whole() {
    let plan = parsed(&table_plan());
    let a = plan_source::admission(&plan, &Verdicts::default());
    assert_eq!(a.admitted, set(&["lib", "lua", "uses-data-free"]));
    assert_eq!(a.order, ["lib", "lua", "data-free", "uses-data-free"]);
    let w = withdrawn(&a);
    let expect = |code: &'static str, chain: &[&str]| {
        (
            code,
            chain.iter().map(|s| s.to_string()).collect::<Vec<_>>(),
        )
    };
    assert_eq!(w["native"], expect("native-not-wired", &[]));
    assert_eq!(w["registers"], expect("native-not-wired", &[]));
    assert_eq!(w["over-native"], expect("dependency-skipped", &["native"]));
    // through a Lua-only package
    assert_eq!(
        w["over-over"],
        expect("dependency-skipped", &["over-native", "native"])
    );
    assert_eq!(w["patch"], expect("startup-not-built", &[]));
    assert_eq!(w["over-patch"], expect("dependency-skipped", &["patch"]));
    // skip, disabled and other-side packages never register and are no withdrawal of this start
    for id in ["skipped", "off", "server-only"] {
        assert!(
            !a.order.contains(&id.to_owned()) && !a.admitted.contains(id) && !w.contains_key(id),
            "{id}"
        );
    }
    assert_eq!(w.len(), 6, "{w:?}");
    // the loud warnings name the cause chain
    let lines: Vec<String> = a
        .compose_withdrawn()
        .iter()
        .map(kiln_sfi::worker::compose::warning)
        .collect();
    assert!(
        lines
            .iter()
            .any(|l| l.contains("'over-over' will not run")
                && l.contains("(via over-native -> native)")),
        "{lines:?}"
    );
    assert!(
        lines.iter().any(|l| l
            .contains("'native' will not run: Kiln's native manager made no run for this start")),
        "{lines:?}"
    );
    assert!(
        lines.iter().any(|l| l.contains(
            "'patch' will not run: Kiln's EML loader (kiln-eml) offers no startup pass this start"
        )),
        "{lines:?}"
    );

    // The native manager's and the startup pass's reports: what they let run runs, never more than the plan admits
    let concluded = Verdicts {
        native: NativeVerdict::Concluded {
            refused: BTreeMap::from([("registers".to_owned(), "its DLL failed".to_owned())]),
        },
        startup: StartupVerdict::Committed {
            packages: set(&["patch"]),
            refused: BTreeMap::new(),
        },
    };
    let a = plan_source::admission(&plan, &concluded);
    assert_eq!(
        a.admitted,
        set(&[
            "lib",
            "lua",
            "native",
            "over-native",
            "over-over",
            "patch",
            "over-patch",
            "uses-data-free"
        ])
    );
    assert_eq!(
        withdrawn(&a).keys().cloned().collect::<Vec<_>>(),
        ["registers"]
    );
    assert_eq!(a.withdrawn[0].reason, "its DLL failed");
    let a = plan_source::admission(
        &plan,
        &Verdicts {
            native: NativeVerdict::Concluded {
                refused: BTreeMap::new(),
            },
            startup: StartupVerdict::Committed {
                packages: BTreeSet::new(),
                refused: BTreeMap::new(),
            },
        },
    );
    assert_eq!(withdrawn(&a)["patch"].0, "startup-not-committed");
    // every kept package is a candidate of the plan
    for id in a.order.iter().chain(&a.admitted) {
        assert!(plan.load_order().contains(&id.as_str()), "{id}");
    }

    // a withdrawal at runtime-create takes its dependents along
    let mut a = plan_source::admission(&plan, &Verdicts::default());
    a.withdraw(
        &plan,
        "lib",
        "package-changed",
        kiln_sfi::state::Part::Package,
        plan_source::CHANGED,
    );
    assert_eq!(a.admitted, set(&["uses-data-free"]));
    assert_eq!(withdrawn(&a)["lua"], expect("dependency-skipped", &["lib"]));
    // a package that is not a candidate is never withdrawn (nor admitted)
    a.withdraw(
        &plan,
        "skipped",
        "package-changed",
        kiln_sfi::state::Part::Package,
        "x",
    );
    assert!(!withdrawn(&a).contains_key("skipped"));
}

#[test]
fn the_worker_withdraws_a_changed_or_missing_package_with_its_dependents() {
    let game = game_dir();
    let good = put_package(
        &game,
        "good",
        "good",
        &["runtime"],
        "shroudforge.log.info('good:ran')",
    );
    let changed = put_package(
        &game,
        "changed",
        "changed",
        &["runtime"],
        "shroudforge.log.info('changed:ran')",
    );
    // the folder changes after the launcher hashed it
    std::fs::write(
        game.join("mods")
            .join("changed")
            .join("src")
            .join("extra.lua"),
        "return 1",
    )
    .unwrap();
    let over = put_package(
        &game,
        "over",
        "over-changed",
        &["runtime"],
        "shroudforge.log.info('over-changed:ran')",
    );
    let document = plan(
        "client",
        vec![
            candidate("good", "good", good),
            candidate("changed", "changed", changed),
            depends_on(candidate("over-changed", "over", over), &["changed"]),
            candidate("missing", "missing", [3; 32]),
        ],
        &["good", "changed", "over-changed", "missing"],
        "none",
    );
    set_plan(Some(serde_json::to_vec(&document).unwrap()));
    let b = boot(Case::default());
    assert!(b.running());
    assert!(
        eventually(WAIT, || b.has("[good] good:ran")),
        "{:?}",
        b.lines()
    );
    assert!(!b.has("changed:ran"), "{:?}", b.lines());
    for (id, text) in [
        ("changed", "its files changed since the launcher wrote"),
        ("over-changed", "depends on withdrawn package changed"),
        (
            "missing",
            "its folder mods/missing cannot be read as the launch plan pinned it",
        ),
    ] {
        assert!(
            b.has(&format!(
                "WARNING: ShroudForge package '{id}' will not run: {text}"
            )),
            "{id}: {:?}",
            b.lines()
        );
    }
    // sfi.json: a row per listed package, a skip for each withdrawal, the ran one active
    assert!(eventually(WAIT, || b
        .record()
        .package("good")
        .is_some_and(|p| p.lua == Lua::Active)));
    let record = b.record();
    for (id, code) in [
        ("changed", "package-changed"),
        ("over-changed", "dependency-skipped"),
        ("missing", "package-unreadable"),
    ] {
        let row = record.package(id).unwrap_or_else(|| panic!("{id}"));
        assert_eq!(
            row.skip.as_ref().map(|s| s.code.as_str()),
            Some(code),
            "{id}"
        );
        assert_eq!(row.lua, Lua::Excluded, "{id}");
    }
    b.stop();
}

/// A package runs only from the folder whose digest was checked. Discovery finds packages by listing `<game>/mods`; a
/// found folder that is another folder than the planned one, though its name differs only in case (the Kelvin sign
/// U+212A lower-cases to `k`, but no filesystem joins the two), must not stand in for it: the planned folder holds
/// what the launcher hashed, the other one whatever was put there.
#[test]
fn a_package_never_runs_from_a_folder_other_than_the_one_whose_digest_was_checked() {
    let game = game_dir();
    let good = put_package(
        &game,
        "good",
        "good",
        &["runtime"],
        "shroudforge.log.info('good:ran')",
    );
    // The planned folder: what the launcher hashed, holding another id.
    let checked = put_package(
        &game,
        "k-pack",
        "other",
        &["runtime"],
        "shroudforge.log.info('other:ran')",
    );
    // The folder beside it, holding the planned id.
    put_package(
        &game,
        "\u{212A}-pack",
        "alpha",
        &["runtime"],
        "shroudforge.log.info('alpha:unchecked-ran')",
    );
    assert_ne!(
        std::fs::read(game.join("mods/k-pack/mod.json")).unwrap(),
        std::fs::read(game.join("mods/\u{212A}-pack/mod.json")).unwrap(),
        "two folders on this filesystem"
    );
    let document = plan(
        "client",
        vec![
            candidate("good", "good", good),
            candidate("alpha", "k-pack", checked),
        ],
        &["good", "alpha"],
        "none",
    );
    set_plan(Some(serde_json::to_vec(&document).unwrap()));
    let b = boot(Case::default());
    assert!(b.running(), "{:?}", b.lines());
    assert!(
        eventually(WAIT, || b.has("[good] good:ran")),
        "{:?}",
        b.lines()
    );
    assert!(
        eventually(WAIT, || b
            .record()
            .package("alpha")
            .is_some_and(|p| p.skip.is_some() || p.lua == Lua::Active)),
        "{:?}",
        b.lines()
    );
    assert!(!b.has("unchecked-ran"), "{:?}", b.lines());
    assert!(!b.has("other:ran"), "{:?}", b.lines());
    assert!(
        b.has("WARNING: ShroudForge package 'alpha' will not run: it is not found as ShroudForge package 'alpha' in mods/k-pack"),
        "{:?}",
        b.lines()
    );
    let record = b.record();
    let row = record.package("alpha").unwrap();
    assert_eq!(
        row.skip.as_ref().map(|s| s.code.as_str()),
        Some("package-not-found")
    );
    assert_eq!(row.lua, Lua::Excluded);
    b.stop();
}

/// An EML package with startup data and no Lua part (kiln-eml's own kind).
fn eml_data(id: &str, folder: &str) -> Value {
    let mut p = candidate(id, folder, [2; 32]);
    p["kind"] = json!("eml");
    p["capabilities"] = json!([]);
    p["data"] = json!({"required":true,"admission":"admit"});
    p["lua"] = json!({"required":false,"admission":"not-needed"});
    p
}

#[test]
fn an_eml_dialect_plan_leaves_kiln_emls_own_data_packages_to_it() {
    let c = [1u8; 32];
    let mut sf_export = candidate("sf-export", "sf-export", c);
    sf_export["capabilities"] = json!(["runtime", "export"]);
    sf_export["data"] = json!({"required":true,"admission":"admit"});
    // a ShroudForge package with startup data and no Lua part: kiln-eml sets ShroudForge packages aside
    let mut sf_data = eml_data("sf-data", "sf-data");
    sf_data["kind"] = json!("shroudforge");
    sf_data["capabilities"] = json!(["export"]);
    let packages = vec![
        eml_data("eml-data", "eml-data"),
        sf_data,
        depends_on(candidate("uses-eml", "uses-eml", c), &["eml-data"]),
        depends_on(candidate("over-uses", "over-uses", c), &["uses-eml"]),
        candidate("free", "free", c),
        sf_export,
    ];
    let order = [
        "eml-data",
        "sf-data",
        "uses-eml",
        "over-uses",
        "free",
        "sf-export",
    ];
    let eml = parsed(&plan("client", packages.clone(), &order, "eml"));
    let a = plan_source::admission(&eml, &Verdicts::default());
    // kiln-eml's EML pass applies eml-data's startup data this start; it has no Lua part, so kiln-sfi
    // withdraws nothing of it and never says it does not run
    assert!(!a.is_withdrawn("eml-data"), "{:?}", a.withdrawn);
    let w = withdrawn(&a);
    let chain = |ids: &[&str]| ids.iter().map(|s| s.to_string()).collect::<Vec<_>>();
    // its Lua dependents wait for a startup outcome that names it (fail closed: none reaches kiln-sfi yet)
    assert_eq!(w["uses-eml"], ("startup-unwitnessed", chain(&["eml-data"])));
    assert_eq!(
        w["over-uses"],
        ("dependency-skipped", chain(&["uses-eml", "eml-data"]))
    );
    // a ShroudForge package's startup data is not kiln-eml's EML pass's: held as before
    assert_eq!(w["sf-export"], ("startup-not-built", chain(&[])));
    assert_eq!(w["sf-data"], ("startup-not-built", chain(&[])));
    assert_eq!(a.admitted, set(&["free"]));
    assert_eq!(a.order, ["free"]);
    // a startup outcome that names it lets its dependents run, never more than the plan admits
    let a = plan_source::admission(
        &eml,
        &Verdicts {
            native: NativeVerdict::NotWired,
            startup: StartupVerdict::Committed {
                packages: set(&["eml-data"]),
                refused: BTreeMap::new(),
            },
        },
    );
    assert_eq!(a.admitted, set(&["uses-eml", "over-uses", "free"]));
    assert_eq!(a.order, ["eml-data", "uses-eml", "over-uses", "free"]);
    assert_eq!(
        withdrawn(&a).keys().collect::<Vec<_>>(),
        ["sf-data", "sf-export"]
    );
    // in a ShroudForge-dialect plan kiln-eml skips that data (no startup pass yet): held like any other
    let mut packages = packages;
    packages.push(with_patch(candidate("patch", "patch", c)));
    let mut order = order.to_vec();
    order.push("patch");
    let sf = parsed(&plan("client", packages, &order, "shroudforge"));
    let a = plan_source::admission(&sf, &Verdicts::default());
    assert_eq!(withdrawn(&a)["eml-data"], ("startup-not-built", chain(&[])));
    assert_eq!(
        withdrawn(&a)["uses-eml"],
        ("dependency-skipped", chain(&["eml-data"]))
    );

    // through the worker: no skip, no warning and no "excluded" data for eml-data in an eml-dialect start
    let game = game_dir();
    let free = put_package(
        &game,
        "free",
        "free",
        &["runtime"],
        "shroudforge.log.info('free:ran')",
    );
    let document = plan(
        "client",
        vec![
            eml_data("eml-data", "eml-data"),
            candidate("free", "free", free),
        ],
        &["eml-data", "free"],
        "eml",
    );
    set_plan(Some(serde_json::to_vec(&document).unwrap()));
    let b = boot(Case::default());
    assert!(b.running());
    assert!(
        eventually(WAIT, || b.has("[free] free:ran")),
        "{:?}",
        b.lines()
    );
    assert!(!b.has("'eml-data' will not run"), "{:?}", b.lines());
    let record = b.record();
    let row = record.package("eml-data").expect("a row");
    assert!(row.skip.is_none() && row.warning.is_none(), "{row:?}");
    assert_eq!(row.lua, Lua::NotNeeded);
    // kiln-eml's startup outcome says its data is in the live generation
    assert_eq!(
        (row.data, row.archive_present),
        (kiln_sfi::state::Data::Committed, Some(true))
    );
    b.stop();
}

#[test]
fn a_package_the_plan_skips_is_recorded_with_the_plans_reason() {
    let game = game_dir();
    let alpha = put_package(
        &game,
        "a",
        "alpha",
        &["runtime"],
        "shroudforge.log.info('alpha:ran')",
    );
    let mut eml_skip = listed(eml_data("eml-skipped", "eml-skipped"), "skip");
    eml_skip["reasons"] = json!([{"code":"x","message":"an EML package the planner skipped","causedBy":[],"severity":"warning"}]);
    eml_skip["lua"]["admission"] = json!("not-needed");
    let document = plan(
        "client",
        vec![
            candidate("alpha", "a", alpha),
            listed(candidate("skipped", "skipped", [1; 32]), "skip"),
            eml_skip,
        ],
        &["alpha"],
        // an enabled EML package for this side makes it kiln-eml's dialect, whatever the plan decided for it
        "eml",
    );
    set_plan(Some(serde_json::to_vec(&document).unwrap()));
    let b = boot(Case::default());
    assert!(b.running());
    assert!(
        eventually(WAIT, || b.has("[alpha] alpha:ran")),
        "{:?}",
        b.lines()
    );
    // the loud warning for a ShroudForge package (kiln-eml says it for its own kind)
    assert!(
        b.has("WARNING: ShroudForge package 'skipped' will not run: this start's launch plan skips it: skipped by the planner"),
        "{:?}",
        b.lines()
    );
    assert!(!b.has("'eml-skipped' will not run"), "{:?}", b.lines());
    // sfi.json: a selected package that does not run has its reason, so its warning
    let record = b.record();
    for (id, said) in [
        ("skipped", "skipped by the planner"),
        ("eml-skipped", "an EML package the planner skipped"),
    ] {
        let row = record.package(id).unwrap_or_else(|| panic!("{id}"));
        assert_eq!(row.intent, Intent::Selected, "{id}");
        let skip = row.skip.as_ref().unwrap_or_else(|| panic!("{id}: {row:?}"));
        assert_eq!(skip.code, "plan-skip", "{id}");
        assert!(skip.text.contains(said), "{id}: {}", skip.text);
        assert_eq!(
            row.warning.as_ref().map(|w| w.trigger),
            Some(Trigger::Skipped),
            "{id}"
        );
    }
    b.stop();
}

/// Admission only removes: under every combination of native and startup verdicts (including verdicts that name
/// packages the plan does not admit, or does not list), and after any runtime-create withdrawal, every package it
/// keeps is a candidate of the plan, every package whose Lua runs has an admitted Lua part, the order is the plan's
/// with every included dependency before its dependent, and nothing runs that the most permissive verdict would not
/// run.
#[test]
fn admission_only_removes_under_every_verdict() {
    let plan = parsed(&table_plan());
    let load_order = plan.load_order();
    let lua_admitted: BTreeSet<String> = load_order
        .iter()
        .filter(|id| {
            plan.package(id)
                .is_some_and(|p| p.lua.admission == sfi_policy::launch_plan::PartAdmission::Admit)
        })
        .map(|id| id.to_string())
        .collect();
    let subsets = |pool: &[&str]| -> Vec<BTreeSet<String>> {
        (0..1u32 << pool.len())
            .map(|bits| {
                pool.iter()
                    .enumerate()
                    .filter(|(i, _)| bits & (1 << i) != 0)
                    .map(|(_, id)| id.to_string())
                    .collect()
            })
            .collect()
    };
    let mut natives = vec![NativeVerdict::NotWired];
    natives.extend(
        subsets(&["lib", "native", "registers", "skipped", "unknown"])
            .into_iter()
            .map(|ids| NativeVerdict::Concluded {
                refused: ids
                    .into_iter()
                    .map(|id| (id, "refused".to_owned()))
                    .collect(),
            }),
    );
    let mut startups = vec![StartupVerdict::NotBuilt];
    startups.extend(
        subsets(&["patch", "lua", "off", "unknown"])
            .into_iter()
            .map(|packages| StartupVerdict::Committed {
                packages,
                refused: BTreeMap::new(),
            }),
    );
    let permissive = plan_source::admission(
        &plan,
        &Verdicts {
            native: NativeVerdict::Concluded {
                refused: BTreeMap::new(),
            },
            startup: StartupVerdict::Committed {
                packages: load_order.iter().map(|id| id.to_string()).collect(),
                refused: BTreeMap::new(),
            },
        },
    );
    assert_eq!(
        permissive.admitted, lua_admitted,
        "nothing held, nothing refused"
    );
    let check = |a: &Admission, label: &dyn std::fmt::Debug| {
        assert!(a.admitted.is_subset(&lua_admitted), "{label:?}");
        assert!(a.admitted.is_subset(&permissive.admitted), "{label:?}");
        let mut at = 0;
        for id in &a.order {
            let found = load_order[at..].iter().position(|o| o == id);
            at += found.unwrap_or_else(|| panic!("{label:?}: {id} out of the plan's order")) + 1;
            assert!(!a.is_withdrawn(id), "{label:?}: {id}");
            for d in plan
                .package(id)
                .unwrap()
                .dependencies
                .iter()
                .filter(|d| d.included)
            {
                assert!(
                    a.order.iter().any(|o| o == d.id),
                    "{label:?}: {id} without {}",
                    d.id
                );
            }
        }
        for id in &a.admitted {
            assert!(a.order.contains(id), "{label:?}: {id}");
        }
        let mut seen = BTreeSet::new();
        for w in &a.withdrawn {
            assert!(
                load_order.contains(&w.id.as_str()),
                "{label:?}: {} is no candidate",
                w.id
            );
            assert!(seen.insert(w.id.clone()), "{label:?}: {} twice", w.id);
        }
    };
    let mut cases = 0;
    for native in &natives {
        for startup in &startups {
            let verdicts = Verdicts {
                native: native.clone(),
                startup: startup.clone(),
            };
            let a = plan_source::admission(&plan, &verdicts);
            check(&a, &verdicts);
            // a runtime-create withdrawal only removes too
            for id in ["lib", "data-free", "skipped"] {
                let mut after = a.clone();
                after.withdraw(
                    &plan,
                    id,
                    "package-changed",
                    kiln_sfi::state::Part::Package,
                    "x",
                );
                check(&after, &(&verdicts, id));
                assert!(after.admitted.is_subset(&a.admitted), "{verdicts:?} {id}");
                assert!(!after.order.iter().any(|o| o == id), "{verdicts:?} {id}");
            }
            cases += 1;
        }
    }
    assert_eq!(cases, natives.len() * startups.len());
}

// ---- 5. loader.is_client / is_server from the plan, on both sides ----

#[test]
fn loader_side_and_runtime_features_come_from_the_frozen_start_on_both_sides() {
    for (server, side) in [(false, "client"), (true, "server")] {
        set_side(server);
        let game = game_dir();
        let lua = "shroudforge.log.info('alpha:client=' .. tostring(loader.is_client) .. ' server=' .. tostring(loader.is_server) \
                   .. ' patch=' .. tostring(loader.features.patch) .. ' export=' .. tostring(loader.features.export))";
        let content = put_package(&game, "a", "alpha", &["runtime", "export"], lua);
        let mut package = candidate("alpha", "a", content);
        package["capabilities"] = json!(["runtime", "export"]);
        set_plan(Some(
            serde_json::to_vec(&plan(side, vec![package], &["alpha"], "none")).unwrap(),
        ));
        let b = boot(Case::default());
        assert!(b.running(), "{side}");
        let want = format!(
            "[alpha] alpha:client={} server={} patch=false export=false",
            !server, server
        );
        assert!(eventually(WAIT, || b.has(&want)), "{side}: {:?}", b.lines());
        match b.boundary.launch_plan() {
            Some(PlanRead::Frozen(frozen)) => assert_eq!(frozen.plan.game_side(), side),
            other => panic!("{other:?}"),
        }
        b.stop();
        // the plan for the other side: the strict reader refuses it (the record froze it for this side)
        set_plan(Some(
            serde_json::to_vec(&plan(
                if server { "client" } else { "server" },
                vec![],
                &[],
                "none",
            ))
            .unwrap(),
        ));
        let b = boot(Case::default());
        refused_with(
            &b,
            "plan-invalid",
            "is not a launch plan Kiln's interpreter reads",
        );
    }
    set_side(false);
}

// ---- 6. one read across early, main and the worker ----

#[test]
fn the_plan_is_read_once_across_early_main_and_the_worker() {
    let game = game_dir();
    let content = put_package(
        &game,
        "a",
        "alpha",
        &["runtime"],
        "shroudforge.log.info('alpha:ran')",
    );
    let document = plan(
        "client",
        vec![candidate("alpha", "a", content)],
        &["alpha"],
        "none",
    );
    set_plan(Some(serde_json::to_vec(&document).unwrap()));
    // After the early read the file is replaced (other bytes, other hash): a second read anywhere would
    // refuse the start; the worker runs alpha from the early read.
    fn replace() {
        let path = plan_path();
        let mut bytes = std::fs::read(&path).unwrap();
        bytes.extend_from_slice(b"\n\n");
        std::fs::write(&path, bytes).unwrap();
    }
    let b = boot(Case {
        between: Some(replace),
        ..Case::default()
    });
    assert!(b.running());
    assert!(
        eventually(WAIT, || b.has("[alpha] alpha:ran")),
        "{:?}",
        b.lines()
    );
    assert_eq!(b.boundary.plan_reads(), 1);
    match b.boundary.launch_plan() {
        Some(PlanRead::Frozen(frozen)) => {
            assert!(
                Arc::ptr_eq(frozen, b.worker.shared().unwrap().frozen_plan().unwrap()),
                "the worker holds the boundary's read"
            );
            assert_eq!(frozen.name, format!("launch-plans/{DEFAULT_PLAN_ID}.json"));
            assert_eq!(frozen.game, game);
        }
        other => panic!("{other:?}"),
    }
    b.stop();
    // no early phase: main reads it, once. Without kiln-sfi's early phase nothing attached to kiln-eml's startup seam,
    // so no archive generation is granted and the runtime-create is refused: no Lua
    set_plan(Some(serde_json::to_vec(&document).unwrap()));
    let b = boot(Case {
        skip_early: true,
        ..Case::default()
    });
    assert_eq!(b.boundary.plan_reads(), 1);
    assert!(
        eventually(WAIT, || {
            b.has(
            "kiln-sfi: WARNING: Kiln's interpreter does not run this start: kiln-sfi's early phase did not attach"
        )
        }),
        "{:?}",
        b.lines()
    );
    assert!(!b.has("alpha:ran"));
    b.stop();
    fake_startup::uninstall();
}

// ---- 7. the turned-off warning (the carry-forward half is in state.rs) ----

fn previous(state: &Path, packages: Value) {
    std::fs::create_dir_all(state).unwrap();
    std::fs::write(
        state.join("sfi.json"),
        json!({"format":1,"launch":{"pid":1,"launchId":"00000000000000aa"},"incomplete":false,"packages":packages}).to_string(),
    )
    .unwrap();
}
fn was_active(id: &str) -> Value {
    json!({"id":id,"version":"0.8.2","intent":"selected","runtimeActive":true,"archivePresent":null,
        "saveSensitive":{"value":true,"basis":"policy","policyRow":{"id":id,"version":"0.8.2"}}})
}

#[test]
fn a_save_sensitive_package_active_last_start_and_off_now_warns() {
    let game = game_dir();
    let content = put_package(
        &game,
        "q",
        "quiver",
        &["runtime"],
        "shroudforge.log.info('quiver:ran')",
    );
    let quiver = candidate("quiver", "q", content);
    for (label, packages, order, intent) in [
        (
            "disabled",
            vec![listed(quiver.clone(), "disabled")],
            vec![],
            Some(Intent::Disabled),
        ),
        ("gone", vec![], vec![], Some(Intent::Absent)),
        ("still selected", vec![quiver.clone()], vec!["quiver"], None),
        (
            "for the other side now",
            vec![listed(quiver.clone(), "other-side")],
            vec![],
            None,
        ),
    ] {
        let state = fresh_dir("ps1-turned-off");
        previous(&state, json!([was_active("quiver")]));
        set_plan(Some(
            serde_json::to_vec(&plan("client", packages, &order, "none")).unwrap(),
        ));
        let b = boot(Case {
            state: Some(state),
            ..Case::default()
        });
        assert!(b.running(), "{label}");
        let record = b.record();
        let row = record.package("quiver");
        match intent {
            Some(intent) => {
                let row = row.unwrap_or_else(|| panic!("{label}"));
                assert_eq!(row.intent, intent, "{label}");
                let warning = row.warning.as_ref().unwrap_or_else(|| panic!("{label}"));
                assert_eq!(warning.trigger, Trigger::SaveSensitiveOff, "{label}");
                assert!(row.save_sensitive.value, "{label}");
                assert_eq!(
                    row.save_sensitive.policy_row,
                    Some(("quiver".into(), "0.8.2".into())),
                    "{label}"
                );
            }
            None => assert!(row.is_none_or(|r| r.warning.is_none()), "{label}: {row:?}"),
        }
        b.stop();
    }
    // the plan's own save-warning (the launcher's finding) reaches kiln.log and sfi.json
    let state = fresh_dir("ps1-plan-warning");
    let mut document = plan(
        "client",
        vec![listed(quiver.clone(), "disabled")],
        &[],
        "none",
    );
    document["warnings"] = json!([{"code":"save-sensitive-off","message":"Quiver & Spellbook is off since the last activation",
        "causedBy":["quiver"],"severity":"save-warning"}]);
    set_plan(Some(serde_json::to_vec(&document).unwrap()));
    let b = boot(Case {
        state: Some(state),
        ..Case::default()
    });
    assert!(b.running());
    assert!(b.has("the launch plan warns (save-sensitive-off): Quiver & Spellbook is off since the last activation"));
    let record = b.record();
    let row = record.package("quiver").expect("a row");
    assert_eq!(
        (row.intent, row.warning.as_ref().map(|w| w.trigger)),
        (Intent::Disabled, Some(Trigger::SaveSensitiveOff))
    );
    b.stop();
}

// ---- 8 (part): hosted, installed and idle never read a plan ----

#[test]
fn every_other_selection_reads_no_plan_and_starts_nothing() {
    for record in [
        (
            KILN_LAUNCH_BACKEND_HOSTED,
            KILN_LAUNCH_REASON_HOSTED_DEFAULT,
        ),
        (
            KILN_LAUNCH_BACKEND_INSTALLED,
            KILN_LAUNCH_REASON_USE_INSTALLED,
        ),
        (KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SAFE_IDLE),
        (
            KILN_LAUNCH_BACKEND_NONE,
            KILN_LAUNCH_REASON_INTERPRETER_HELD,
        ),
    ] {
        // a plan that would be refused if anyone read it
        set_frozen_hash(Some([9; 32]));
        set_launch(Some(record));
        let worker: &'static Worker =
            Box::leak(Box::new(Worker::new(lifecycle::factory::<KilnPlanSource>)));
        let boundary: &'static Boundary = Box::leak(Box::new(
            Boundary::with_worker(InactiveReason::NegotiationMissing, worker)
                .with_own_loader(|| None),
        ));
        let (early, normal) = (next_owner(), next_owner());
        let api = Box::leak(api(true));
        let (mut e, mut n) = (plugin_info(), plugin_info());
        assert_eq!(
            unsafe { boundary.enter(Phase::Early, &*api, early, &mut e) },
            KILN_OK
        );
        assert_eq!(
            unsafe { boundary.enter(Phase::Normal, &*api, normal, &mut n) },
            KILN_OK
        );
        assert!(e.on_ready.is_none() && n.on_ready.is_none(), "{record:?}");
        assert_eq!(boundary.plan_reads(), 0, "{record:?}");
        assert!(
            boundary.launch_plan().is_none() && !boundary.worker_started(),
            "{record:?}"
        );
        set_frozen_hash(None);
    }
}

// ---- native Windows: reparse refusal and case-folded roots (win1) ----

#[cfg(windows)]
fn junction(link: &Path, target: &Path) {
    // cmd reads a forward slash as a switch (a Linux CARGO_TARGET_TMPDIR under Wine has them).
    let native = |p: &Path| std::path::PathBuf::from(p.to_str().unwrap().replace('/', "\\"));
    let out = std::process::Command::new("cmd")
        .args(["/C", "mklink", "/J"])
        .arg(native(link))
        .arg(native(target))
        .output()
        .unwrap();
    assert!(out.status.success(), "{out:?}");
}

#[cfg(windows)]
#[test]
fn on_windows_a_junction_or_link_on_the_plan_path_is_refused() {
    let game = game_dir();
    let content = put_package(
        &game,
        "a",
        "alpha",
        &["runtime"],
        "shroudforge.log.info('alpha:ran')",
    );
    let document = plan(
        "client",
        vec![candidate("alpha", "a", content)],
        &["alpha"],
        "none",
    );
    set_plan(Some(serde_json::to_vec(&document).unwrap()));
    // the launch-plans folder a junction to a folder that holds the same plan
    let plans = game.join("kiln").join("launch-plans");
    let real = game.join("kiln").join("plans-real");
    std::fs::rename(&plans, &real).unwrap();
    junction(&plans, &real);
    let b = boot(Case::default());
    refused_with(
        &b,
        "plan-unreadable",
        "kiln/launch-plans is not a plain folder",
    );
    assert!(!b.has("alpha:ran"));
    std::fs::remove_dir(&plans).unwrap(); // removes the junction only
    std::fs::rename(&real, &plans).unwrap();
    // the plan file a symbolic link, where this account may create one
    let file = plan_path();
    let target = game.join("kiln").join("plan-target.json");
    std::fs::rename(&file, &target).unwrap();
    match std::os::windows::fs::symlink_file(&target, &file) {
        Ok(()) => {
            let b = boot(Case::default());
            refused_with(&b, "plan-unreadable", "it is not a plain file");
            std::fs::remove_file(&file).unwrap();
        }
        Err(e) => println!("SKIP file symlink: {e}"),
    }
    std::fs::rename(&target, &file).unwrap();
    // a package folder that is a junction: withdrawn, the others run
    let other = put_package(
        &game,
        "b-real",
        "beta",
        &["runtime"],
        "shroudforge.log.info('beta:ran')",
    );
    junction(
        &game.join("mods").join("b"),
        &game.join("mods").join("b-real"),
    );
    let document = plan(
        "client",
        vec![
            candidate("alpha", "a", content),
            candidate("beta", "b", other),
        ],
        &["alpha", "beta"],
        "none",
    );
    set_plan(Some(serde_json::to_vec(&document).unwrap()));
    let b = boot(Case::default());
    assert!(b.running());
    assert!(
        eventually(WAIT, || b.has("[alpha] alpha:ran")),
        "{:?}",
        b.lines()
    );
    assert!(
        b.has("ShroudForge package 'beta' will not run: its folder mods/b cannot be read"),
        "{:?}",
        b.lines()
    );
    assert!(!b.has("beta:ran"));
    b.stop();
}

#[cfg(windows)]
#[test]
fn on_windows_roots_and_the_plans_folder_are_found_ignoring_case() {
    let game = game_dir();
    // on disk "alpha-pack", in the plan "mods/Alpha-Pack"; the launch-plans folder spelled "Launch-Plans"
    let content = put_package(
        &game,
        "alpha-pack",
        "alpha",
        &["runtime"],
        "shroudforge.log.info('alpha:ran')",
    );
    let document = plan(
        "client",
        vec![candidate("alpha", "Alpha-Pack", content)],
        &["alpha"],
        "none",
    );
    set_plan(Some(serde_json::to_vec(&document).unwrap()));
    let plans = game.join("kiln").join("launch-plans");
    std::fs::rename(&plans, game.join("kiln").join("Launch-Plans")).unwrap();
    let b = boot(Case::default());
    assert!(b.running(), "{:?}", b.lines());
    assert!(
        eventually(WAIT, || b.has("[alpha] alpha:ran")),
        "{:?}",
        b.lines()
    );
    assert!(!b.has("will not run"), "{:?}", b.lines());
    b.stop();
    std::fs::rename(game.join("kiln").join("Launch-Plans"), &plans).unwrap();
}

// ---- game.types from the granted archive (the grant comes from kiln-eml's startup outcome) ----

fn typed_start(seam: bool) -> Boot {
    granted_start(
        seam,
        "shroudforge.log.info('typed:' .. (game.types and (tostring(game.types.count) .. '|' .. \
         game.types.version .. '|' .. game.version) or 'none'))",
    )
}

/// One runtime package `typed` running `lua`, with or without kiln-eml's startup seam (the archive grant).
fn granted_start(seam: bool, lua: &str) -> Boot {
    let game = game_dir();
    let typed = put_package(&game, "typed", "typed", &["runtime"], lua);
    let document = plan(
        "client",
        vec![candidate("typed", "typed", typed)],
        &["typed"],
        "none",
    );
    set_plan(Some(serde_json::to_vec(&document).unwrap()));
    boot(Case {
        no_seam: !seam,
        ..Case::default()
    })
}

#[test]
fn the_production_plan_source_reads_game_types_from_the_granted_archive() {
    let b = typed_start(true);
    assert!(b.running());
    let line = format!(
        "[typed] typed:0|{}|{}",
        fake_startup::REGISTRY,
        fake_startup::GAME
    );
    assert!(eventually(WAIT, || b.has(&line)), "{:?}", b.lines());
    // the types family ran its cross-check (this fake core has no kiln.types: skipped, one
    // INFO line under kiln-sfi's owner) and read the registry through one view
    let skipped: Vec<(i32, String)> = logs(b.normal)
        .into_iter()
        .filter(|(_, t)| t.starts_with(kiln_sfi::runtime::types::SKIPPED))
        .collect();
    assert_eq!(
        skipped,
        [(
            KILN_LOG_INFO,
            format!(
                "{}: kiln.types: not provided",
                kiln_sfi::runtime::types::SKIPPED
            )
        )]
    );
    let seen = fake_archive::seen(b.normal);
    // two views: the types port's (the first, read at runtime-create) and the VM's typed-value arena's
    assert_eq!(seen.views.len(), 2, "{:?}", seen.calls);
    assert_eq!(&seen.calls[..2], ["acquire", "identity"]);
    let types_view = seen.views[0];
    assert_ne!(fake_archive::arenas(b.normal)[0].view, types_view);
    // The registry is an owned copy once read: the view lives only through the
    // runtime-create that made the wiring (its family and the composition drop at the end
    // of create, on the worker thread), released exactly once and not again at the leave.
    assert_eq!(seen.released, [types_view], "{:?}", seen.calls);
    b.stop();
    let released = fake_archive::seen(b.normal).released;
    assert_eq!(
        released.iter().filter(|v| **v == types_view).count(),
        1,
        "{released:?}"
    );
}

/// The production plan source adds the runtime.ecs registry family on the granted registry. This fake core
/// has no kiln.types, so the registry is the provider's missing extension and the catalog lists the (empty)
/// registry's types with nothing resolved.
#[test]
fn the_production_plan_source_installs_the_ecs_registry_family_on_the_granted_registry() {
    let b = granted_start(
        true,
        "local c = runtime.ecs.get_catalog()
         local r, why = runtime.ecs.get_registry()
         local e, ewhy = runtime.ecs.get_component('keen::ecs::Health')
         shroudforge.log.info('ecs:' .. tostring(c.registry_available) .. '|' .. #c.entries .. '|' .. c.version
           .. '|' .. tostring(r) .. '|' .. why .. '|' .. tostring(e) .. '|' .. ewhy
           .. '|' .. type(runtime.ecs.get_components))",
    );
    assert!(b.running());
    let line = format!(
        "[typed] ecs:false|0|{}|nil|{}|nil|type not found: keen::ecs::Health|function",
        fake_startup::REGISTRY,
        kiln_sfi::runtime::ecs::registry::text::LACKS
    );
    assert!(eventually(WAIT, || b.has(&line)), "{:?}", b.lines());
    b.stop();
}

/// The production plan source installs the world context family. Without kiln.caps' `world.lifecycle` row
/// here, `runtime.world.context.active` is outside the contract; `runtime.ecs.query` waits for the ECS queries, so
/// `session_id` answers 0: ShroudForge's own mods, which call it unguarded, see "no world" and wait.
#[test]
fn the_production_plan_source_installs_the_world_context_family() {
    let b = granted_start(
        true,
        "return { on_load = function()
           local s = runtime.world.session_id()
           local a, why = runtime.world.context_active()
           shroudforge.log.info('world:' .. tostring(s) .. '|' .. math.type(s) .. '|' .. tostring(a) .. '|' .. why
             .. '|' .. runtime.world.context_kind() .. '|'
             .. tostring(runtime.world.operation_available('runtime.world.context.active')))
         end }",
    );
    assert!(b.running());
    let line = format!(
        "[typed] world:0|integer|false|operation runtime.world.context.active has not been verified for game build \
         {}|unknown|false",
        fake_startup::GAME
    );
    assert!(eventually(WAIT, || b.has(&line)), "{:?}", b.lines());
    b.stop();
}

/// Without an archive generation granted to this start there is no runtime (as
/// ShroudForge's does not start without its type registry): runtime-create is refused with the loud warning and a
/// refused record before any package code, and nothing is acquired.
#[test]
fn without_an_archive_grant_the_runtime_create_is_refused() {
    let b = typed_start(false);
    assert!(
        eventually(WAIT, || b.worker.shared().is_some_and(|s| matches!(
            s.phase(),
            WorkerPhase::Stopping | WorkerPhase::Stopped
        ))),
        "{:?}",
        b.lines()
    );
    assert!(
        logs(b.normal)
            .iter()
            .any(|(level, t)| *level == KILN_LOG_ERROR
                && t.starts_with("kiln-sfi: WARNING: Kiln's interpreter does not run this start")
                && t.contains("no startup seam")),
        "{:?}",
        b.lines()
    );
    assert!(!b.has("typed:"), "no Lua: {:?}", b.lines());
    // the hub's writer thread writes the refused record (the worker never waits for a file)
    let file = || -> Value {
        serde_json::from_slice(&std::fs::read(b.state.join("sfi.json")).unwrap()).unwrap()
    };
    assert!(
        eventually(WAIT, || file()["phase"] == "refused"),
        "{}",
        file()
    );
    assert_eq!(b.record().phase(), RecordPhase::Refused);
    assert_eq!(
        file()["reason"]["code"],
        "startup-grant-missing",
        "{}",
        file()
    );
    assert!(
        fake_archive::seen(b.normal).calls.is_empty(),
        "nothing acquired"
    );
    b.stop();
}

// ---- typed values: each VM's arena over its own view of the granted generation ----

/// The production plan source gives runtime-create's VM its kiln.eml.values arena over a view of the granted
/// generation of its own (acquired under kiln-sfi's owner, beside the types port's view, which create releases),
/// before any package code; the arena stays open while the runtime runs, and the orderly stop closes and drains it
/// before its view is released. `game.types.of` answers nil for what is not a typed value, and ShroudForge's
/// unverified-operation reason names the archive registry's game version.
#[test]
fn the_production_plan_source_gives_each_vm_typed_values_over_the_granted_generation() {
    let b = granted_start(
        true,
        "local s = runtime.status('runtime.ecs.write') \
         shroudforge.log.info('values:' .. tostring(game.types.of(1)) .. '|' .. tostring(game.types.of(game.types)) \
           .. '|' .. tostring(s.available) .. '|' .. tostring(s.reason))",
    );
    assert!(b.running());
    let line = format!(
        "[typed] values:nil|nil|false|operation runtime.ecs.write has not been verified for game build {}",
        fake_startup::GAME
    );
    assert!(eventually(WAIT, || b.has(&line)), "{:?}", b.lines());
    let arenas = fake_archive::arenas(b.normal);
    assert_eq!(arenas.len(), 1, "{arenas:?}");
    assert_eq!(
        arenas[0].phase,
        kiln_abi::services::kiln_eml_values::KILN_VALUE_RUNTIME as u32
    );
    assert!(!arenas[0].closed);
    let seen = fake_archive::seen(b.normal);
    assert_eq!(
        seen.views.len(),
        2,
        "the types port's view and the arena's: {:?}",
        seen.calls
    );
    assert!(seen.views.contains(&arenas[0].view));
    assert_eq!(
        seen.released,
        seen.views
            .iter()
            .copied()
            .filter(|v| *v != arenas[0].view)
            .collect::<Vec<_>>(),
        "the types port's view is released at the end of create, the arena's is held"
    );
    b.stop();
    assert_eq!(
        fake_archive::values_events(b.normal),
        [
            "arena1:create",
            "arena1:close",
            "arena1:drained",
            "arena1:view-released"
        ]
    );
    let seen = fake_archive::seen(b.normal);
    assert_eq!(seen.released.len(), 2);
}
