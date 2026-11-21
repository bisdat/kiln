// SPDX-License-Identifier: GPL-3.0-or-later
//! The startup cache of the ShroudForge dialect (`kiln/eml/cache/sf-startup-<stem>.json`) and the startup controller's
//! kill points (with real process kills): a hit replays the outcome, the skip rows of a retried success
//! included; every key part misses on its own; a kill between the publication and the cache record means a miss and
//! the same rows next start, one after the record a hit; a kill in either attempt or between them publishes nothing and
//! the next start recovers.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;
#[path = "support/m6_startup/mod.rs"]
mod startup_support;

use fixture::archive::{ArchiveFixture, STEM};
use kiln_eml::{
    sf_pass::StartupDialect,
    sf_startup::{cache::Key, plan::Features},
    startup::{Attempt, Data, PlanFacts, StartupOutcome, State},
};
use mod_loader_lua::test_gate;
use startup_support::{Start, broken, content, native, outcome_json};
use std::{
    cell::RefCell,
    fs,
    path::{Path, PathBuf},
    process::{Command, Stdio},
    rc::Rc,
    time::{Duration, Instant},
};

/// A retried success: `bad` fails in its own entrypoint, `needs-bad` needs it, `ok` runs.
fn retried_packages(s: &Start) {
    s.patch("bad", &[], &[], &broken("bad fails"));
    s.patch("needs-bad", &["bad"], &[], &content("needs-bad blob"));
    s.patch("ok", &[], &[], &content("ok blob"));
}

fn rows(out: &StartupOutcome) -> Vec<(String, Data, Option<String>, Vec<String>)> {
    out.packages
        .iter()
        .map(|r| (r.id.clone(), r.data, r.code.clone(), r.chain.clone()))
        .collect()
}

// A hit: the same key and the generation it recorded still live: no attempt runs,
// and the outcome is the recorded one (attempt, both errors, warnings, the failed and dependency-skipped rows), the
// committed rows cached-verified, the same receipt granted.
#[test]
fn a_hit_replays_the_outcome_of_a_retried_success() {
    let s = Start::new("g13-hit");
    retried_packages(&s);
    let plan = s.plan(StartupDialect::Shroudforge, &[]);
    let (_, first) = s.start(&plan, &native());
    assert_eq!(
        (first.state, first.attempt),
        (State::Committed, Attempt::Retry),
        "{first:?}"
    );
    assert!(s.f.game.join(format!("kiln/eml/cache/sf-startup-{STEM}.json")).exists());
    let (_, hit) = s.start(&plan, &native());
    assert_eq!(hit.state, State::Cached, "{hit:?}");
    assert!(
        !s.points.borrow().iter().any(|p| p.starts_with("Attempt")),
        "{:?}",
        s.points.borrow()
    );
    assert_eq!(
        (hit.attempt, &hit.error, &hit.retry_error),
        (first.attempt, &first.error, &first.retry_error)
    );
    assert_eq!(hit.warnings, first.warnings, "the retry's warning repeats every start");
    let replayed: Vec<_> = rows(&first)
        .into_iter()
        .map(|(id, data, code, chain)| {
            (
                id,
                if data == Data::Committed {
                    Data::CachedVerified
                } else {
                    data
                },
                code,
                chain,
            )
        })
        .collect();
    assert_eq!(rows(&hit), replayed);
    assert_eq!(
        hit.live.as_ref().unwrap().receipt_sha256,
        first.live.as_ref().unwrap().receipt_sha256
    );
    assert_eq!(
        hit.live.as_ref().unwrap().archive_sha256,
        first.live.as_ref().unwrap().archive_sha256
    );
    let (a, b) = (outcome_json(&first, "cache-first"), outcome_json(&hit, "cache-hit"));
    assert_eq!(
        a["packages"].as_array().unwrap().len(),
        b["packages"].as_array().unwrap().len()
    );
    assert_eq!(b["state"], "cached");
}

// Every key part misses on its own: the dialect, the archive, the executable, each accepted package's id, version,
// content and settings digests, their order, the native identity, the working baseline, the features.
#[test]
fn every_key_part_misses_on_its_own() {
    let s = Start::new("g13-key");
    s.patch("one", &[], &[], &content("one blob"));
    s.patch("two", &[], &[], &content("two blob"));
    let plan = s.plan(StartupDialect::Shroudforge, &[]);
    let w = s.witness(&plan).unwrap();
    let baseline = w.input.baseline().clone();
    let features = Features {
        patch: true,
        export: false,
    };
    let members = |p: &PlanFacts| -> Vec<kiln_eml::startup::PlanPackage> { p.packages.clone() };
    let key = |p: &PlanFacts,
               dialect,
               stem: &str,
               exe: &str,
               dlls: &std::collections::BTreeMap<String, Vec<String>>,
               f: Features,
               b: &mod_loader::lua::startup_host::BaselineIdentity| {
        let packages = members(p);
        let refs: Vec<&kiln_eml::startup::PlanPackage> = packages.iter().collect();
        Key::new(dialect, stem, exe, &refs, dlls, b, f, (None, "m6-g13-build"))
    };
    let none = std::collections::BTreeMap::new();
    let base = key(
        &plan,
        StartupDialect::Shroudforge,
        STEM,
        &plan.exe_sha256,
        &none,
        features,
        &baseline,
    );
    assert_eq!(
        base,
        key(
            &plan,
            StartupDialect::Shroudforge,
            STEM,
            &plan.exe_sha256,
            &none,
            features,
            &baseline
        )
    );
    let mut differ = Vec::new();
    differ.push((
        "dialect",
        key(
            &plan,
            StartupDialect::Eml,
            STEM,
            &plan.exe_sha256,
            &none,
            features,
            &baseline,
        ),
    ));
    differ.push((
        "stem",
        key(
            &plan,
            StartupDialect::Shroudforge,
            "enshrouded_server",
            &plan.exe_sha256,
            &none,
            features,
            &baseline,
        ),
    ));
    differ.push((
        "exe",
        key(
            &plan,
            StartupDialect::Shroudforge,
            STEM,
            &"00".repeat(32),
            &none,
            features,
            &baseline,
        ),
    ));
    for (part, edit) in [
        (
            "id",
            Box::new(|p: &mut PlanFacts| p.packages[0].id = "other".into()) as Box<dyn Fn(&mut PlanFacts)>,
        ),
        (
            "version",
            Box::new(|p: &mut PlanFacts| p.packages[0].version = "1.0.1".into()),
        ),
        (
            "content",
            Box::new(|p: &mut PlanFacts| p.packages[0].content_sha256[0] ^= 1),
        ),
        (
            "settings",
            Box::new(|p: &mut PlanFacts| p.packages[1].config_sha256[31] ^= 1),
        ),
        ("order", Box::new(|p: &mut PlanFacts| p.packages.reverse())),
    ] {
        let mut changed = plan.clone();
        edit(&mut changed);
        differ.push((
            part,
            key(
                &changed,
                StartupDialect::Shroudforge,
                STEM,
                &plan.exe_sha256,
                &none,
                features,
                &baseline,
            ),
        ));
    }
    let mut dlls = none.clone();
    dlls.insert("one".to_owned(), vec!["loaded".to_owned()]);
    differ.push((
        "native",
        key(
            &plan,
            StartupDialect::Shroudforge,
            STEM,
            &plan.exe_sha256,
            &dlls,
            features,
            &baseline,
        ),
    ));
    differ.push((
        "features",
        key(
            &plan,
            StartupDialect::Shroudforge,
            STEM,
            &plan.exe_sha256,
            &none,
            Features {
                patch: true,
                export: true,
            },
            &baseline,
        ),
    ));
    // the baseline: the same archive with another backup seal
    let seal = s.f.game.join(format!("{STEM}.kfc.bak.integrity"));
    let sealed = fs::read(&seal).unwrap();
    fs::write(&seal, b"{}").unwrap();
    let other = mod_loader::lua::startup_host::InputWitness::capture(s.game(), STEM, w.input.containers()).unwrap();
    fs::write(&seal, sealed).unwrap();
    differ.push((
        "baseline",
        key(
            &plan,
            StartupDialect::Shroudforge,
            STEM,
            &plan.exe_sha256,
            &none,
            features,
            other.baseline(),
        ),
    ));
    // the frozen locale (shroudforge.locale) and the runtime catalog's game build
    let refs: Vec<&kiln_eml::startup::PlanPackage> = plan.packages.iter().collect();
    for (part, frozen) in [
        ("locale", (Some("de"), "m6-g13-build")),
        ("build", (None, "another-build")),
    ] {
        differ.push((
            part,
            Key::new(
                StartupDialect::Shroudforge,
                STEM,
                &plan.exe_sha256,
                &refs,
                &none,
                &baseline,
                features,
                frozen,
            ),
        ));
    }
    for (part, k) in &differ {
        assert_ne!(*k, base, "{part}: the key did not change");
    }
    // end to end: a settings change, a native change and a receipt that is not the recorded one each run the pass
    let first = s.run(&w, &plan, &native());
    assert_eq!(first.state, State::Committed);
    let ran = |s: &Start| s.points.borrow().iter().any(|p| p == "Attempt(1)");
    let hit = s.run(&s.witness(&plan).unwrap(), &plan, &native());
    assert_eq!((hit.state, ran(&s)), (State::Cached, false));
    let mut settings = plan.clone();
    settings.packages[1].config_sha256[0] ^= 1;
    let miss = s.run(&s.witness(&settings).unwrap(), &settings, &native());
    assert_eq!((miss.state, ran(&s)), (State::Committed, true), "settings");
    let mut facts = native();
    facts.dlls.insert("one".into(), vec!["refused".into()]);
    let miss = s.run(&s.witness(&plan).unwrap(), &plan, &facts);
    assert_eq!((miss.state, ran(&s)), (State::Committed, true), "native");
    let hit = s.run(&s.witness(&plan).unwrap(), &plan, &facts);
    assert_eq!((hit.state, ran(&s)), (State::Cached, false));
    let receipt = s.f.game.join(format!("kiln/eml/ownership/{STEM}.json"));
    let mut bytes = fs::read(&receipt).unwrap();
    bytes.push(b'\n');
    fs::write(&receipt, &bytes).unwrap();
    let miss = s.run(&s.witness(&plan).unwrap(), &plan, &facts);
    assert_eq!((miss.state, ran(&s)), (State::Committed, true), "receipt");
}

/// The kill child's spec: the game folder, where to stop (a point's text and which occurrence), the marker to write.
fn spawn_child(game: &Path, park: &str, nth: usize, marker: &Path) -> std::process::Child {
    let spec = serde_json::json!({"game": game, "park": park, "nth": nth, "marker": marker});
    let mut command = Command::new(std::env::current_exe().unwrap());
    command
        .args(["--exact", "kill_child", "--ignored", "--test-threads=1"])
        .env("KILN_STARTUP_KILL_CHILD", spec.to_string())
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    test_gate::spawn(&mut command).unwrap()
}

/// The kill tests' child process: one kiln-mode start over the parent's game folder (its own install lock), stopped at
/// the `nth` (1-based) point `park` until it is killed.
#[test]
#[ignore = "child process of the startup kill tests; does nothing without KILN_STARTUP_KILL_CHILD"]
fn kill_child() {
    let Ok(spec) = std::env::var("KILN_STARTUP_KILL_CHILD") else {
        return;
    };
    let spec: serde_json::Value = serde_json::from_str(&spec).unwrap();
    let game = PathBuf::from(spec["game"].as_str().unwrap());
    let park = spec["park"].as_str().unwrap().to_owned();
    let nth = spec["nth"].as_u64().unwrap() as usize;
    let marker = PathBuf::from(spec["marker"].as_str().unwrap());
    let _hold = test_gate::lock_hold();
    let guard = fs::OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(game.join(".kiln-eml.lock"))
        .unwrap();
    guard.try_lock().unwrap();
    let plan = startup_support::plan_of(&game, StartupDialect::Shroudforge, &[]);
    let game8 = camino::Utf8PathBuf::from_path_buf(game.clone()).unwrap();
    let w = kiln_eml::startup::witness(&game8, STEM, &guard, &plan, startup_support::key()).unwrap();
    let seen = RefCell::new(0usize);
    let hook = |point: kiln_eml::startup::Point<'_>| {
        if format!("{point:?}") == park {
            *seen.borrow_mut() += 1;
            if *seen.borrow() == nth {
                let tmp = marker.with_extension("tmp");
                fs::write(&tmp, &park).unwrap();
                fs::rename(&tmp, &marker).unwrap();
                loop {
                    std::thread::sleep(Duration::from_secs(1));
                }
            }
        }
    };
    let types = Rc::new(fixture::types::registry());
    startup_support::run_over(
        &game,
        &types,
        &guard,
        &w,
        &plan,
        &native(),
        &|_| Ok(None),
        &hook,
        &RefCell::default(),
        &RefCell::default(),
        Rc::default(),
    );
    // reaching here: the point never came (the parent sees the exit without a marker)
}

/// Kills a child stopped at `park` (its `nth` occurrence) over `f`'s game folder; returns the fixture with the install
/// lock taken again.
fn killed_at(f: ArchiveFixture, park: &str, nth: usize) -> Start {
    let marker = f.root.0.join("killed").into_std_path_buf();
    let mut child = spawn_child(f.game.as_std_path(), park, nth, &marker);
    let started = Instant::now();
    while !marker.exists() {
        if let Some(status) = child.try_wait().unwrap() {
            panic!("{park}: the child exited ({status}) without reaching it");
        }
        if started.elapsed() > Duration::from_secs(120) {
            let _ = child.kill();
            let _ = child.wait();
            panic!("{park}: the child did not reach it in time");
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    child.kill().unwrap();
    child.wait().unwrap();
    assert_eq!(fs::read_to_string(&marker).unwrap(), park);
    Start::over(f)
}

/// The outcome of the same start without a kill, on a fresh fixture.
fn reference(packages: fn(&Start)) -> StartupOutcome {
    let s = Start::new("g13-kill-ref");
    packages(&s);
    s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native()).1
}

// Killed after the one publication, before the cache record: next start the cache misses (no record of this
// receipt), the pass runs again and the rows are the uninterrupted start's.
#[test]
fn a_kill_between_publication_and_cache_record_misses_next_start_with_the_same_rows() {
    let expected = reference(retried_packages);
    let s = Start::new("g13-k12");
    retried_packages(&s);
    let s = killed_at(s.into_fixture(), "Published", 1);
    assert!(
        !s.f.game.join(format!("kiln/eml/cache/sf-startup-{STEM}.json")).exists(),
        "no record yet"
    );
    assert!(
        mod_loader::lua::owns_output(s.game(), STEM).unwrap(),
        "the publication completed"
    );
    mod_loader::lua::prune(s.game(), STEM);
    let (_, out) = s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native());
    assert!(
        s.points.borrow().iter().any(|p| p == "Attempt(1)"),
        "a miss runs the pass"
    );
    assert_eq!(
        (out.state, rows(&out), out.attempt),
        (State::Committed, rows(&expected), expected.attempt)
    );
}

// Killed after the cache record (before kiln-sfi could read the outcome): next start hits, with the identical
// outcome (the skip rows included).
#[test]
fn a_kill_after_the_cache_record_hits_next_start_with_the_identical_outcome() {
    let expected = reference(retried_packages);
    let s = Start::new("g13-k13");
    retried_packages(&s);
    let s = killed_at(s.into_fixture(), "CacheWritten", 1);
    let (_, out) = s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native());
    assert_eq!(out.state, State::Cached, "{out:?}");
    assert!(!s.points.borrow().iter().any(|p| p.starts_with("Attempt")));
    let replayed: Vec<_> = rows(&expected)
        .into_iter()
        .map(|(id, data, code, chain)| {
            (
                id,
                if data == Data::Committed {
                    Data::CachedVerified
                } else {
                    data
                },
                code,
                chain,
            )
        })
        .collect();
    assert_eq!(rows(&out), replayed);
    assert_eq!((out.attempt, &out.warnings), (expected.attempt, &expected.warnings));
}

// Killed in the first attempt, between the attempts, or in the retry (after its stage was prepared): nothing of
// the start is live (the old set), the recorded stage is reclaimed by the next start's clean-up, which then runs the
// whole pass and publishes; the install lock was never released in between (the child held it until it died).
#[test]
fn a_kill_in_either_attempt_or_between_them_publishes_nothing_and_recovers() {
    let expected = reference(retried_packages);
    for (park, nth) in [
        ("Step(Registered(\"ok\"))", 1),
        ("Attempt(2)", 1),
        ("Step(Prepared)", 1),
    ] {
        let s = Start::new("g13-k8");
        retried_packages(&s);
        let original = fixture::archive::validate(&s.f.game, STEM).unwrap();
        let f = s.into_fixture();
        let s = killed_at(f, park, nth);
        // the witness captured the baseline before the kill; the live set is the one before the start
        assert_eq!(
            fixture::archive::validate(&s.f.game, STEM).unwrap(),
            original,
            "{park}: something was published"
        );
        assert!(!mod_loader::lua::owns_output(s.game(), STEM).unwrap(), "{park}");
        mod_loader::lua::prune(s.game(), STEM);
        let leftovers: Vec<_> = fs::read_dir(s.game())
            .unwrap()
            .flatten()
            .filter(|e| e.file_name().to_string_lossy().starts_with(".kiln-eml-stage-"))
            .filter(|e| fs::read_dir(e.path()).unwrap().next().is_some())
            .collect();
        assert!(leftovers.is_empty(), "{park}: a stage kept Kiln's bytes after clean-up");
        let (_, out) = s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native());
        assert_eq!((out.state, rows(&out)), (State::Committed, rows(&expected)), "{park}");
    }
}
