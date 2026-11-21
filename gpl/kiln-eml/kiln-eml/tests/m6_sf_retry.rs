// SPDX-License-Identifier: GPL-3.0-or-later
//! The SF-dialect pass driver's one retry without the failing package, over real
//! StartupHost generations on the synthetic m6 archive. The pass itself is a synthetic stand-in (the pass composition brings the SF
//! startup VM): each pregame package's entrypoint is a Lua chunk named by its id, run in plan order, and a package that
//! ran writes one content blob into the generation's private stage; the pass then prepares and publishes.
//!
//! The cases mirror ShroudForge's own tests at Kiln's fork pin 2df1de68 (MIT, Copyright (c) 2026 bonsaibauer):
//! modules/entrypoints/loader/asset_preparation/kiln_retry_tests.rs and modules/mod-runtime/startup/name_failing_mod.rs.
//! Expected log lines are the fork's texts, byte for byte.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;

use camino::{Utf8Path, Utf8PathBuf};
use fixture::archive::{self, ArchiveFixture, STEM};
use kfc::{container::KFCFile, guid::ContentHash, reflection::TypeRegistry};
use kiln_eml::sf_pass::{
    PackageFailure, PassError, PassPlan, PlannedPackage, Retry, StartupDialect, run_pass,
};
use mod_loader::lua::startup_host::{PublishedArchive, StartupHost};
use std::{
    cell::Cell,
    fs,
    io::{Error, ErrorKind},
    process::{Command, Stdio},
    rc::Rc,
    time::{Duration, Instant},
};

const RETRY: &str = "running the pass once more without it";

/// A switched-on pregame package whose entrypoint is `lua` (None: no src/mod.lua), needing `needs`.
fn package(f: &ArchiveFixture, id: &str, lua: Option<&str>) {
    let path = f.root.0.join("packages").join(id).join("src");
    fs::create_dir_all(&path).unwrap();
    if let Some(lua) = lua {
        fs::write(path.join("mod.lua"), lua).unwrap();
    }
}
/// A package whose own check fails, as Wisp Light Timer's did (the fork's `broken`).
fn broken(f: &ArchiveFixture, id: &str, message: &str) {
    package(
        f,
        id,
        Some(&format!(
            "local function check()\n  error('{message}')\nend\ncheck()\nreturn {{}}"
        )),
    );
}
fn planned(id: &str, needs: &[&str]) -> PlannedPackage {
    PlannedPackage {
        id: id.into(),
        name: format!("The {id} mod"),
        version: "1.2.3".into(),
        requires: needs.iter().map(|s| s.to_string()).collect(),
        optional: Vec::new(),
        pregame: true,
    }
}
fn baseline(game: &Utf8Path) {
    for ext in ["kfc", "kfc_resources"] {
        fs::copy(
            game.join(format!("{STEM}.{ext}")),
            game.join(format!("{STEM}.{ext}.bak")),
        )
        .unwrap();
    }
}
fn blob(id: &str) -> Vec<u8> {
    format!("m6 sf pass content of {id}").into_bytes()
}

/// The install lock, taken once for the whole run and held across both attempts, as the plugin holds it (runner.rs
/// `lock_game` keeps it for the process's life). Never dropped and taken again for the retry: `flock` belongs to the
/// open file description, and a process another thread spawns holds a copy of every descriptor from its fork to its
/// exec, so a lock this thread dropped can still be held by that child when the retry asks for it (the kill test's
/// child did this to its neighbours in about 1 parallel run in 10, "lock acquisition failed because the operation
/// would block").
fn install_lock(root: &Utf8Path) -> fs::File {
    let guard = fs::OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(root.join("game/.kiln-eml.lock"))
        .unwrap();
    guard.try_lock().unwrap();
    guard
}

/// One whole synthetic pass from the clean baseline under the run's held install lock: a NEW StartupHost generation,
/// the plan's pregame packages in order, then the sole publisher. `stop` runs after the packages' writes, before
/// preparation (the kill point).
fn pass(
    root: &Utf8Path,
    types: &Rc<TypeRegistry>,
    plan: &PassPlan,
    guard: &fs::File,
    stop: &dyn Fn(),
) -> Result<PublishedArchive, PassError> {
    let game = root.join("game");
    let base = KFCFile::from_path(game.join(format!("{STEM}.kfc.bak")), false)
        .map_err(|e| PassError::Other(e.into()))?;
    let mut host = StartupHost::open(
        game.as_std_path(),
        STEM,
        types.clone(),
        Rc::new(base),
        guard,
    )
    .map_err(PassError::Other)?;
    let mut stage = host.begin_stage().map_err(PassError::Other)?;
    let lua = mlua::Lua::new();
    let failing_io = lua
        .create_function(|_, kind: String| -> mlua::Result<()> {
            Err(match kind.as_str() {
                "short" => {
                    Error::new(ErrorKind::UnexpectedEof, "failed to fill whole buffer").into()
                }
                "context" => mlua::Error::external(
                    anyhow::Error::from(Error::new(ErrorKind::InvalidData, "bad block"))
                        .context("could not read the resource"),
                ),
                _ => Error::from_raw_os_error(5).into(),
            })
        })
        .unwrap();
    lua.globals().set("failing_io", failing_io).unwrap();
    for p in plan.packages().iter().filter(|p| p.pregame) {
        let path = root.join("packages").join(&p.id).join("src/mod.lua");
        let Ok(code) = fs::read_to_string(&path) else {
            // SF's require raises a Lua error for a missing entry module: the package's failure, no I/O error.
            return Err(PassError::Package(PackageFailure {
                mod_id: p.id.clone(),
                message: format!("module 'mods.{}' not found", p.id),
                io: false,
            }));
        };
        if let Err(error) = lua.load(&code).set_name(p.id.as_str()).exec() {
            return Err(PassError::Package(PackageFailure::new(
                &p.id,
                error.to_string(),
                error.chain(),
            )));
        }
        let bytes = blob(&p.id);
        stage
            .write_content(&ContentHash::from_data(&bytes), &bytes)
            .map_err(|e| PassError::Other(e.into()))?;
    }
    stop();
    let prepared = stage.prepare().map_err(PassError::Other)?;
    prepared.publish().map_err(|e| PassError::Other(e.into()))
}

struct Run {
    result: Result<PublishedArchive, PassError>,
    retry: Retry,
    log: Vec<String>,
    attempts: usize,
}
impl Run {
    /// The pass succeeded; on failure shows the retry's own error too (the result keeps the first one).
    fn assert_ok(&self) {
        assert!(
            self.result.is_ok(),
            "{:?}; retry error: {:?}",
            self.result.as_ref().err(),
            self.retry.error
        );
    }
}
fn run(f: &ArchiveFixture, dialect: StartupDialect, plan: &PassPlan) -> Run {
    run_with(f, dialect, plan, &|_| {})
}
/// `run`, with `after` called after each attempt with the run's install lock.
fn run_with(
    f: &ArchiveFixture,
    dialect: StartupDialect,
    plan: &PassPlan,
    after: &dyn Fn(&fs::File),
) -> Run {
    let attempts = Cell::new(0);
    let mut log = Vec::new();
    let guard = install_lock(&f.root.0);
    let (result, retry) = run_pass(
        dialect,
        plan,
        |plan| {
            attempts.set(attempts.get() + 1);
            let out = pass(&f.root.0, &f.types, plan, &guard, &|| {});
            after(&guard);
            out
        },
        |line| log.push(line.to_owned()),
    );
    Run {
        result,
        retry,
        log,
        attempts: attempts.get(),
    }
}
fn ids(list: &serde_json::Value) -> Vec<&str> {
    list.as_array()
        .map(|items| items.iter().filter_map(|i| i["id"].as_str()).collect())
        .unwrap_or_default()
}
fn record(retry: &Retry) -> serde_json::Value {
    let mut record = serde_json::json!({"schemaVersion": 1, "status": "applied"});
    retry.record_into(&mut record);
    record
}
/// The package blobs the live archive holds.
fn loaded(f: &ArchiveFixture, plan_ids: &[&str]) -> Vec<String> {
    let contents = f.semantics().contents;
    plan_ids
        .iter()
        .filter(|id| contents.contains_key(&ContentHash::from_data(&blob(id)).to_string()))
        .map(|id| id.to_string())
        .collect()
}
fn fixture(tag: &str) -> ArchiveFixture {
    let f = ArchiveFixture::new(tag);
    baseline(&f.game);
    f
}

#[test]
fn one_failing_package_is_left_out_and_the_others_load() {
    let f = fixture("sf-retry-one");
    package(&f, "alpha", Some("return {}"));
    broken(&f, "broken", "Missing base localization: keenglish");
    package(&f, "needs-broken", Some("return {}"));
    package(&f, "zeta", Some("return {}"));
    let plan = PassPlan::new(vec![
        planned("alpha", &[]),
        planned("broken", &[]),
        planned("needs-broken", &["broken"]),
        planned("zeta", &[]),
    ]);
    let out = run(&f, StartupDialect::Shroudforge, &plan);
    out.assert_ok();
    assert_eq!(out.attempts, 2, "once more, once only");
    let all = ["alpha", "broken", "needs-broken", "zeta"];
    assert_eq!(loaded(&f, &all), ["alpha", "zeta"]);
    archive::validate(&f.game, STEM).unwrap();
    let record = record(&out.retry);
    assert_eq!(
        record["skipped"][0],
        serde_json::json!({"id": "broken", "name": "The broken mod", "version": "1.2.3",
            "error": "runtime error: [string \"broken\"]:2: Missing base localization: keenglish"}),
        "the package and its error on one line, without the traceback: {record}"
    );
    assert_eq!(
        record["skipped"][1],
        serde_json::json!({"id": "needs-broken", "name": "The needs-broken mod", "version": "1.2.3",
            "error": "needs broken, which failed in the pregame pass"}),
        "{record}"
    );
    assert_eq!(record.get("retryError"), None);
    assert_eq!(
        out.log,
        [
            "ShroudForge: The broken mod (broken 1.2.3) failed in the pregame pass (runtime error: \
             [string \"broken\"]:2: Missing base localization: keenglish); running the pass once more without it",
            "ShroudForge: The broken mod (broken 1.2.3) failed in the pregame pass (runtime error: \
             [string \"broken\"]:2: Missing base localization: keenglish); started the game without it and the mod \
             that needs it (needs-broken), the other 2 mods loaded",
        ]
    );
}

#[test]
fn a_second_failure_starts_without_the_pass_and_records_both_errors() {
    let f = fixture("sf-retry-two");
    package(&f, "alpha", Some("return {}"));
    broken(&f, "broken-a", "first check failed");
    broken(&f, "broken-b", "second check failed");
    let plan = PassPlan::new(vec![
        planned("alpha", &[]),
        planned("broken-a", &[]),
        planned("broken-b", &[]),
    ]);
    let out = run(&f, StartupDialect::Shroudforge, &plan);
    let error = out
        .result
        .err()
        .expect("a second failure fails the pass")
        .to_string();
    assert!(
        error.starts_with(
            "pregame phase failed: runtime error: [string \"broken-a\"]:2: first check failed\nstack traceback:"
        ),
        "the first failure's error, as the fork records it: {error}"
    );
    let record = record(&out.retry);
    assert_eq!(ids(&record["skipped"]), ["broken-a"], "{record}");
    let second = record["retryError"].as_str().unwrap();
    assert!(
        second.starts_with(
            "pregame phase failed: runtime error: [string \"broken-b\"]:2: second check failed"
        ),
        "{second}"
    );
    // The record keeps the second error whole (the fork's `second.to_string()`); only the log line is one line.
    assert!(
        second.contains("\nstack traceback:"),
        "retryError keeps the second error whole, with its traceback: {second}"
    );
    assert!(out.log[0].contains(RETRY));
    assert_eq!(
        out.log[1],
        "ShroudForge: the pregame pass failed again without The broken-a mod (broken-a 1.2.3) \
         (pregame phase failed: runtime error: [string \"broken-b\"]:2: second check failed); the game starts without \
         the mods of the pregame pass"
    );
    // No startup package loads: nothing was published by either attempt.
    assert_eq!(archive::validate(&f.game, STEM).unwrap(), f.original);
    assert!(!mod_loader::lua::owns_output(f.game.as_std_path(), STEM).unwrap());
}

#[test]
fn access_denied_and_os_error_texts_are_not_retried() {
    for message in [
        "could not open enshrouded.kfc_resources: Access is denied. (os error 5)",
        "Permission denied",
    ] {
        let f = fixture("sf-retry-denied");
        package(&f, "alpha", Some("return {}"));
        broken(&f, "broken", message);
        let plan = PassPlan::new(vec![planned("alpha", &[]), planned("broken", &[])]);
        let out = run(&f, StartupDialect::Shroudforge, &plan);
        let error = out.result.err().unwrap().to_string();
        assert!(error.contains(message), "{error}");
        assert_eq!(
            (out.attempts, out.retry.clone(), out.log.len()),
            (1, Retry::default(), 0)
        );
        assert_eq!(record(&out.retry).get("skipped"), None);
    }
}

#[test]
fn an_io_error_in_the_chain_is_not_retried_whatever_its_text() {
    // name_failing_mod.rs: a short read (no OS code), access denied re-raised by pcall, an I/O error behind a context.
    for (code, expect_io) in [
        (
            "local function check() failing_io('short') end\ncheck()",
            true,
        ),
        (
            "local ok, e = pcall(failing_io, 'denied')\nif not ok then error(e) end",
            true,
        ),
        ("failing_io('context')", true),
        ("error('Missing base localization: keenglish')", false),
    ] {
        let f = fixture("sf-retry-io");
        package(&f, "alpha", Some("return {}"));
        package(&f, "wisp", Some(code));
        let plan = PassPlan::new(vec![planned("alpha", &[]), planned("wisp", &[])]);
        let out = run(&f, StartupDialect::Shroudforge, &plan);
        assert_eq!(out.attempts, if expect_io { 1 } else { 2 }, "{code}");
        assert_eq!(out.retry.skipped.is_empty(), expect_io, "{code}");
        assert_eq!(out.result.is_ok(), !expect_io, "{code}");
    }
    // A package that put the I/O error into its own words: only the text is left, and it says os error.
    let lua = mlua::Lua::new();
    let f = lua
        .create_function(|_, ()| -> mlua::Result<()> { Err(Error::from_raw_os_error(5).into()) })
        .unwrap();
    lua.globals().set("failing", f).unwrap();
    let error = lua
        .load("local ok, e = pcall(failing)\nif not ok then error('could not read: ' .. tostring(e)) end")
        .set_name("wisp")
        .exec()
        .unwrap_err();
    let failure = PackageFailure::new("wisp", error.to_string(), error.chain());
    assert!(
        !failure.io && failure.message.contains("(os error 5)"),
        "{failure:?}"
    );
    let raised = |failure: PackageFailure| {
        let attempts = Cell::new(0);
        let plan = PassPlan::new(vec![planned("wisp", &[])]);
        let failure = std::cell::RefCell::new(Some(failure));
        let _ = run_pass(
            StartupDialect::Shroudforge,
            &plan,
            |_| {
                attempts.set(attempts.get() + 1);
                Err::<(), _>(PassError::Package(failure.borrow_mut().take().unwrap_or(
                    PackageFailure {
                        mod_id: "wisp".into(),
                        message: "again".into(),
                        io: false,
                    },
                )))
            },
            |_| {},
        );
        attempts.get()
    };
    assert_eq!(
        raised(failure),
        1,
        "an os error text is the system's, not the package's"
    );
}

#[test]
fn a_failure_no_package_raised_is_not_retried() {
    let f = fixture("sf-retry-nobody");
    package(&f, "alpha", Some("return {}"));
    broken(&f, "broken", "Missing base localization: keenglish");
    // another program changed a vanilla container: the pass refuses its baseline before any package runs
    let dat = f.game.join(format!("{STEM}_000.dat"));
    let mut bytes = fs::read(&dat).unwrap();
    bytes.push(0);
    fs::write(&dat, bytes).unwrap();
    let plan = PassPlan::new(vec![planned("alpha", &[]), planned("broken", &[])]);
    let out = run(&f, StartupDialect::Shroudforge, &plan);
    assert!(matches!(out.result, Err(PassError::Other(_))));
    assert_eq!(
        (out.attempts, out.retry, out.log.len()),
        (1, Retry::default(), 0)
    );
    // and a failing package that is not in this pass's plan names nothing
    let plan = PassPlan::new(vec![planned("alpha", &[])]);
    let (result, retry) = run_pass(
        StartupDialect::Shroudforge,
        &plan,
        |_| {
            Err::<(), _>(PassError::Package(PackageFailure {
                mod_id: "elsewhere".into(),
                message: "x".into(),
                io: false,
            }))
        },
        |_| panic!("no retry line"),
    );
    assert!(result.is_err() && retry == Retry::default());
}

#[test]
fn a_pass_without_failures_records_no_retry() {
    let f = fixture("sf-retry-good");
    package(&f, "alpha", Some("return {}"));
    package(&f, "beta", Some("return {}"));
    let plan = PassPlan::new(vec![planned("alpha", &[]), planned("beta", &["alpha"])]);
    let out = run(&f, StartupDialect::Shroudforge, &plan);
    out.assert_ok();
    assert_eq!((out.attempts, out.log.len()), (1, 0));
    assert_eq!(
        record(&out.retry),
        serde_json::json!({"schemaVersion": 1, "status": "applied"})
    );
    assert_eq!(loaded(&f, &["alpha", "beta"]), ["alpha", "beta"]);
}

#[test]
fn packages_that_need_the_failed_one_through_another_drop_out_too() {
    let f = fixture("sf-retry-chain");
    broken(&f, "broken", "check failed");
    package(&f, "middle", Some("return {}"));
    package(&f, "top", Some("return {}"));
    package(&f, "zeta", Some("return {}"));
    package(&f, "runtime-only", None);
    let mut runtime_only = planned("runtime-only", &["broken"]);
    runtime_only.pregame = false;
    let plan = PassPlan::new(vec![
        planned("broken", &[]),
        planned("middle", &["broken"]),
        planned("top", &["middle"]),
        planned("zeta", &[]),
        runtime_only,
    ]);
    let out = run(&f, StartupDialect::Shroudforge, &plan);
    out.assert_ok();
    assert_eq!(loaded(&f, &["broken", "middle", "top", "zeta"]), ["zeta"]);
    let record = record(&out.retry);
    assert_eq!(
        ids(&record["skipped"]),
        ["broken", "middle", "top"],
        "only pregame packages are listed: {record}"
    );
    assert!(
        out.log[1].ends_with(
            "started the game without it and the 2 mods that need it (middle, top), the other mod loaded"
        ),
        "{}",
        out.log[1]
    );
}

#[test]
fn a_package_whose_switched_on_optional_dependency_drops_out_drops_out_too() {
    // The fork's planner (plan_mod_load_order.rs `visit`) skips an optional dependency that is switched off, as the
    // failed package is in the retry, but plans one that is switched on first: when that one drops out because it
    // needs the failed package, the package that optionally depends on it drops out too ("disabled dependency").
    let f = fixture("sf-retry-optional");
    broken(&f, "broken", "check failed");
    package(&f, "middle", Some("return {}"));
    package(&f, "uses-broken", Some("return {}"));
    package(&f, "uses-middle", Some("return {}"));
    package(&f, "zeta", Some("return {}"));
    let optional = |id: &str, on: &[&str]| {
        let mut p = planned(id, &[]);
        p.optional = on.iter().map(|s| s.to_string()).collect();
        p
    };
    // kept, but no pregame package: not one of "the other N mods" (the fork counts the pregame plan)
    let mut runtime_only = planned("runtime-only", &[]);
    runtime_only.pregame = false;
    let plan = PassPlan::new(vec![
        planned("broken", &[]),
        planned("middle", &["broken"]),
        optional("uses-broken", &["broken"]),
        optional("uses-middle", &["middle"]),
        planned("zeta", &[]),
        runtime_only,
    ]);
    let out = run(&f, StartupDialect::Shroudforge, &plan);
    out.assert_ok();
    assert_eq!(
        loaded(
            &f,
            &["broken", "middle", "uses-broken", "uses-middle", "zeta"]
        ),
        ["uses-broken", "zeta"]
    );
    assert_eq!(
        ids(&record(&out.retry)["skipped"]),
        ["broken", "middle", "uses-middle"]
    );
    assert!(
        out.log[1].ends_with(
            "started the game without it and the 2 mods that need it (middle, uses-middle), the other 2 mods loaded"
        ),
        "{}",
        out.log[1]
    );
}

#[test]
fn a_copy_of_the_install_lock_a_spawned_child_holds_does_not_fail_the_retry() {
    // A process another thread spawns holds a copy of every descriptor from its fork to its exec, the install lock's
    // too; `flock` belongs to the open file description, so the copy keeps the lock. try_clone makes that copy here
    // deterministically (the kill test's spawn made it by chance). The run holds its lock across both attempts, so the
    // copy changes nothing; a retry that took the lock again would find it held and fail.
    let f = fixture("sf-retry-inherited");
    package(&f, "alpha", Some("return {}"));
    broken(&f, "broken", "check failed");
    let plan = PassPlan::new(vec![planned("alpha", &[]), planned("broken", &[])]);
    let copy = std::cell::RefCell::new(None);
    let out = run_with(&f, StartupDialect::Shroudforge, &plan, &|guard| {
        copy.borrow_mut()
            .get_or_insert_with(|| guard.try_clone().unwrap());
    });
    out.assert_ok();
    assert_eq!(out.attempts, 2);
    assert_eq!(loaded(&f, &["alpha", "broken"]), ["alpha"]);
    assert_eq!(ids(&record(&out.retry)["skipped"]), ["broken"]);
    drop(copy);
}

#[test]
fn a_missing_entry_module_is_the_packages_failure() {
    let f = fixture("sf-retry-missing");
    package(&f, "alpha", Some("return {}"));
    package(&f, "gone", None);
    let plan = PassPlan::new(vec![planned("alpha", &[]), planned("gone", &[])]);
    let out = run(&f, StartupDialect::Shroudforge, &plan);
    out.assert_ok();
    assert_eq!(ids(&record(&out.retry)["skipped"]), ["gone"]);
    assert!(
        out.log[1].ends_with("started the game without it, the other mod loaded"),
        "{}",
        out.log[1]
    );
}

#[test]
fn the_eml_dialect_is_untouched() {
    for dialect in [StartupDialect::Eml, StartupDialect::None] {
        let f = fixture("sf-retry-eml");
        package(&f, "alpha", Some("return {}"));
        broken(&f, "broken", "Missing base localization: keenglish");
        let plan = PassPlan::new(vec![planned("alpha", &[]), planned("broken", &[])]);
        let out = run(&f, dialect, &plan);
        assert!(out.result.is_err());
        assert_eq!(
            (out.attempts, out.retry, out.log.len()),
            (1, Retry::default(), 0)
        );
    }
    assert_eq!(
        StartupDialect::parse("shroudforge"),
        Some(StartupDialect::Shroudforge)
    );
    assert_eq!(StartupDialect::parse("eml"), Some(StartupDialect::Eml));
    assert_eq!(StartupDialect::parse("none"), Some(StartupDialect::None));
    assert_eq!(StartupDialect::parse("ShroudForge"), None);
}

fn kill_plan() -> PassPlan {
    PassPlan::new(vec![
        planned("alpha", &[]),
        planned("broken", &[]),
        planned("needs-broken", &["broken"]),
        planned("zeta", &[]),
    ])
}

/// The child process of `a_kill_during_the_retry_publishes_nothing_and_recovers`: stops the retry after its writes.
#[test]
#[ignore = "child process of the retry kill test; does nothing without KILN_RETRY_KILL_CHILD"]
fn kill_child() {
    let Ok(root) = std::env::var("KILN_RETRY_KILL_CHILD") else {
        return;
    };
    let root = Utf8PathBuf::from(root);
    let types = Rc::new(fixture::types::registry());
    let attempts = Cell::new(0);
    let guard = install_lock(&root);
    let _ = run_pass(
        StartupDialect::Shroudforge,
        &kill_plan(),
        |plan| {
            attempts.set(attempts.get() + 1);
            let retry = attempts.get() == 2;
            pass(&root, &types, plan, &guard, &|| {
                if retry {
                    fs::write(root.join("killed.tmp"), b"retry staged").unwrap();
                    fs::rename(root.join("killed.tmp"), root.join("killed")).unwrap();
                    loop {
                        std::thread::sleep(Duration::from_secs(1));
                    }
                }
            })
        },
        |_| {},
    );
}

#[test]
fn a_kill_during_the_retry_publishes_nothing_and_recovers() {
    let f = fixture("sf-retry-kill");
    package(&f, "alpha", Some("return {}"));
    broken(&f, "broken", "Missing base localization: keenglish");
    package(&f, "needs-broken", Some("return {}"));
    package(&f, "zeta", Some("return {}"));
    let marker = f.root.0.join("killed");
    let mut child = Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "kill_child", "--ignored", "--test-threads=1"])
        .env("KILN_RETRY_KILL_CHILD", f.root.0.as_str())
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let started = Instant::now();
    while !marker.exists() {
        if let Some(status) = child.try_wait().unwrap() {
            panic!("the child exited ({status}) before the retry staged its writes");
        }
        assert!(
            started.elapsed() < Duration::from_secs(120),
            "the child never reached the retry"
        );
        std::thread::sleep(Duration::from_millis(5));
    }
    child.kill().unwrap();
    child.wait().unwrap();
    // Killed during the retry: neither attempt published; the live set, receipt and backups are as before.
    assert_eq!(archive::validate(&f.game, STEM).unwrap(), f.original);
    assert!(!mod_loader::lua::owns_output(f.game.as_std_path(), STEM).unwrap());
    assert!(!f.receipt().exists());
    let stages = || -> Vec<std::path::PathBuf> {
        fs::read_dir(&f.game)
            .unwrap()
            .flatten()
            .map(|e| e.path())
            .filter(|p| {
                p.file_name()
                    .unwrap()
                    .to_string_lossy()
                    .starts_with(".kiln-eml-stage-")
            })
            .collect()
    };
    assert!(
        stages()
            .iter()
            .any(|s| fs::read_dir(s).unwrap().next().is_some()),
        "the killed retry left its staged bytes"
    );
    // The next start: cleanup twice under the install lock, then the pass again (with its one retry).
    for _ in 0..2 {
        mod_loader::lua::prune(f.game.as_std_path(), STEM);
        for stage in stages() {
            assert!(
                fs::read_dir(&stage).unwrap().next().is_none(),
                "a recorded stage kept bytes after cleanup"
            );
        }
        assert_eq!(archive::validate(&f.game, STEM).unwrap(), f.original);
    }
    let out = run(&f, StartupDialect::Shroudforge, &kill_plan());
    out.assert_ok();
    assert_eq!(
        loaded(&f, &["alpha", "broken", "needs-broken", "zeta"]),
        ["alpha", "zeta"]
    );
    assert_eq!(
        ids(&record(&out.retry)["skipped"]),
        ["broken", "needs-broken"]
    );
    assert!(mod_loader::lua::owns_output(f.game.as_std_path(), STEM).unwrap());
}
