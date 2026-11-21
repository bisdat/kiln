// SPDX-License-Identifier: GPL-3.0-or-later
//! The startup controller end to end on the synthetic m6 archive (crate::startup): the early witness (first
//! managed capture, plan freshness, the install lock), the admission set under native refusals, the SF pass with its one
//! retry in the fork's order, the one Deferred publication (ShroudForge's folder untouched, the owed handoff recorded),
//! the verified BASE after a double failure, the receipt's BASE origin rules, a changed input, the EML and no-pass
//! dialects, and the outcome record. Expectations are ShroudForge 1.7.9's where the pass is its (sf_pass.rs,
//! plan_mod_load_order.rs), elsewhere.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;
#[path = "support/m6_startup/mod.rs"]
mod startup_support;

use fixture::archive::STEM;
use kiln_eml::{
    inspection,
    sf_pass::StartupDialect,
    startup::{self, Attempt, Data, NativeFacts, Origin, Refusal, State},
};
use mod_loader::lua::startup_host::{SfHandoff, StartupHost};
use startup_support::{Start, broken, content, edits, native, outcome_json, shroudforge_decoy, tree};
use std::{collections::BTreeMap, fs, rc::Rc};

/// Puts a path's Unix mode back when dropped, so a test that fails while a folder or file is unreadable leaves no
/// unreadable entry behind (the fixture's own clean-up could not remove a folder with mode 000).
#[cfg(unix)]
struct ModeGuard(std::path::PathBuf, u32);
#[cfg(unix)]
impl Drop for ModeGuard {
    fn drop(&mut self) {
        use std::os::unix::fs::PermissionsExt;
        let _ = fs::set_permissions(&self.0, fs::Permissions::from_mode(self.1));
    }
}

fn row<'a>(out: &'a startup::StartupOutcome, id: &str) -> &'a startup::Row {
    out.packages
        .iter()
        .find(|r| r.id == id)
        .unwrap_or_else(|| panic!("no row {id}: {:?}", out.packages))
}
fn ids(out: &startup::StartupOutcome) -> Vec<&str> {
    out.packages.iter().map(|r| r.id.as_str()).collect()
}
fn points(s: &Start, prefix: &str) -> Vec<String> {
    s.points
        .borrow()
        .iter()
        .filter(|p| p.starts_with(prefix))
        .cloned()
        .collect()
}
fn backups(s: &Start) -> [bool; 3] {
    ["kfc.bak", "kfc_resources.bak", "kfc.bak.integrity"].map(|e| s.f.game.join(format!("{STEM}.{e}")).exists())
}

// A first-managed kiln-mode start: the witness captures the working baseline (Deferred: ShroudForge's decoy folder is
// byte-identical, its applied record untouched, the owed handoff recorded instead), origin first-managed, the pass runs
// both packages, ONE publication; the receipt records the BASE origin keyed to the backups (the inspector reads it back),
// the outcome grants the published generation (its receipt, the generation digest) and its record is the schema's in
// field order. Step order: the stage before the reader, prepared before published, published before the cache record.
#[test]
fn a_first_managed_start_captures_its_baseline_and_publishes_once() {
    let s = Start::new("g13-first");
    s.patch("alpha", &[], &[], &edits(0, 41, "alpha blob"));
    s.patch("beta", &[], &[], &content("beta blob"));
    let sf = shroudforge_decoy(s.game());
    let plan = s.plan(StartupDialect::Shroudforge, &[]);
    assert_eq!(backups(&s), [false; 3], "nothing captured before the start");
    let w = s.witness(&plan).unwrap();
    assert_eq!(w.origin, Origin::FirstManaged);
    assert_eq!(backups(&s), [true; 3], "the witness captured the working baseline");
    let out = s.run(&w, &plan, &native());
    assert_eq!((out.state, out.attempt), (State::Committed, Attempt::First), "{out:?}");
    assert_eq!(ids(&out), ["alpha", "beta"]);
    for r in &out.packages {
        assert_eq!(
            (r.data, r.archive_present, r.code.as_deref()),
            (Data::Committed, Some(true), None)
        );
    }
    assert_eq!(s.extra(0), 41);
    assert!(s.contents().contains(&"alpha blob".to_owned()) && s.contents().contains(&"beta blob".to_owned()));
    // the grant inputs name the published generation
    let live = out.live.as_ref().unwrap();
    let receipt = s.receipt_sha256().unwrap();
    assert_eq!(live.receipt_sha256.map(|r| startup_hex(&r)), Some(receipt.clone()));
    assert_eq!(
        live.archive_sha256,
        startup::generation_digest(STEM, live.receipt_sha256.as_ref(), w.input.baseline())
    );
    assert_eq!(live.backing.game_version(), live.snapshot.game_version());
    // the grant inputs are what the archive service authorizes a PUBLISHED generation with
    let identity = kiln_eml::archive_service::authorize_with_archive(
        kiln_eml::archive_service::Grant {
            phase: kiln_eml::archive_service::Phase::Published,
            launch_id: [0; 16],
            plan_id: w.key.plan_id,
            generation: 0x6713_0001,
            reload_epoch: 1,
            plan_sha256: w.key.plan_sha256,
            archive_sha256: live.archive_sha256,
            prepared_owners: Vec::new(),
        },
        live.snapshot.clone(),
        live.registry.clone(),
        live.backing.clone(),
    )
    .unwrap();
    assert_eq!(identity.archive_sha256, live.archive_sha256);
    assert_eq!(
        outcome_json(&out, "first-managed-identity")["archiveSha256"],
        startup_hex(&identity.archive_sha256)
    );
    // the receipt's BASE origin, keyed to the backups, read back by the inspector
    let recorded = s.receipt().unwrap()["baseOrigin"].clone();
    assert_eq!(recorded["token"], inspection::FIRST_MANAGED);
    assert_eq!(recorded["label"], inspection::FIRST_MANAGED_LABEL);
    let inspected = inspection::inspect(
        s.game(),
        &inspection::Options {
            side: inspection::Side::Client,
            hash_containers: false,
        },
    );
    assert_eq!(inspected.base.as_ref().unwrap().origin.token, inspection::FIRST_MANAGED);
    assert_eq!(inspected.input.as_ref().unwrap().class, "owned-output");
    // ShroudForge's folder untouched; the handoff is owed instead
    assert_eq!(
        tree(&s.game().join("shroudforge")),
        sf,
        "the ShroudForge folder changed"
    );
    assert!(s.owed(), "no owed handoff recorded");
    // step order
    let all = s.points.borrow().clone();
    let at = |p: &str| {
        all.iter()
            .position(|q| q == p)
            .unwrap_or_else(|| panic!("{p} not reached: {all:?}"))
    };
    assert!(at("Step(StageBegun)") < at("Step(ReaderOpened)"));
    assert!(at("Step(Prepared)") < at("Published"));
    assert!(at("Published") < at("CacheWritten"));
    assert_eq!(points(&s, "Attempt"), ["Attempt(1)"]);
    // the record
    let json = outcome_json(&out, "first-managed");
    assert_eq!(json["state"], "committed");
    assert_eq!(json["grant"], true);
    assert_eq!(json["launchId"], "00000000000004d2");
    assert_eq!(json["receiptSha256"], receipt);
    assert_eq!(json["archiveSha256"], startup_hex(&live.archive_sha256));
    assert_eq!(json["baseOrigin"]["token"], inspection::FIRST_MANAGED);
    assert_eq!(json["native"], serde_json::json!({"received": true, "coherent": true}));
    assert_eq!(
        json["packages"][0],
        serde_json::json!({"id": "alpha", "version": "1.0.0", "pass": "shroudforge",
        "data": "committed", "archivePresent": true, "chain": []})
    );
}

fn startup_hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

// Mixed fixture: a package whose second DLL faulted after its first loaded (the terminal
// withdraws it) gets zero data, the package that needs it is left out with the chain naming it, and an independent
// patch survives in the same publication.
#[test]
fn a_dll_refusal_after_a_siblings_success_leaves_no_data_of_it_or_its_dependent() {
    let s = Start::new("g13-mixed");
    s.patch("pack-a", &[], &[], &edits(0, 11, "pack-a blob"));
    s.patch("needs-a", &["pack-a"], &[], &content("needs-a blob"));
    s.patch("indep", &[], &[], &edits(3, 33, "indep blob"));
    let plan = s.plan(StartupDialect::Shroudforge, &["pack-a"]);
    let mut facts = native();
    facts.withdrawn.insert(
        "pack-a".into(),
        Refusal {
            code: "dll-faulted".into(),
            text: "pack-a: its second DLL faulted while it loaded".into(),
            chain: vec![],
        },
    );
    facts
        .dlls
        .insert("pack-a".into(), vec!["loaded".into(), "faulted".into()]);
    let before = s.extra(0);
    let (_, out) = s.start(&plan, &facts);
    assert_eq!(out.state, State::Committed, "{out:?}");
    let a = row(&out, "pack-a");
    assert_eq!(
        (a.data, a.code.as_deref(), a.archive_present),
        (Data::Excluded, Some("native-refused"), Some(false))
    );
    assert_eq!(
        a.text.as_deref(),
        Some("pack-a: its second DLL faulted while it loaded")
    );
    let n = row(&out, "needs-a");
    assert_eq!(
        (n.data, n.code.as_deref(), n.chain.clone()),
        (Data::Excluded, Some("native-refused"), vec!["pack-a".to_owned()])
    );
    assert_eq!(row(&out, "indep").data, Data::Committed);
    assert_eq!(s.extra(0), before, "the refused package's edit is not live");
    assert_eq!(s.extra(3), 33, "the independent patch is live");
    let contents = s.contents();
    assert!(contents.contains(&"indep blob".to_owned()));
    assert!(!contents.contains(&"pack-a blob".to_owned()) && !contents.contains(&"needs-a blob".to_owned()));
    assert!(
        !s.vm_log
            .0
            .borrow()
            .iter()
            .any(|l| l.contains("pack-a blob ran") || l.contains("needs-a blob ran"))
    );
    let json = outcome_json(&out, "mixed");
    let needs = json["packages"]
        .as_array()
        .unwrap()
        .iter()
        .find(|r| r["id"] == "needs-a")
        .unwrap()
        .clone();
    assert_eq!(needs["chain"], serde_json::json!(["pack-a"]));
    assert_eq!(needs["archivePresent"], false);
}

// Retry order: a (failing) requires c and b is independent, so the first pass runs [c, a, b]; the
// fork plans its retry anew, [b, c], where keeping the first pass's order would run [c, b].
#[test]
fn the_retry_runs_the_packages_left_in_the_forks_order() {
    let s = Start::new("g13-order");
    s.patch("a", &["c"], &[], &broken("Missing base localization: keenglish"));
    s.patch("b", &[], &[], &content("b blob"));
    s.patch("c", &[], &[], &content("c blob"));
    let plan = s.plan(StartupDialect::Shroudforge, &[]);
    assert_eq!(plan.load_order, ["c", "a", "b"]);
    let (_, out) = s.start(&plan, &native());
    assert_eq!((out.state, out.attempt), (State::Committed, Attempt::Retry), "{out:?}");
    let registered: Vec<String> = s
        .points
        .borrow()
        .iter()
        .filter(|p| p.starts_with("Attempt") || p.starts_with("Step(Registered"))
        .cloned()
        .collect();
    assert_eq!(
        registered,
        [
            "Attempt(1)",
            "Step(Registered(\"c\"))",
            "Step(Registered(\"a\"))",
            "Step(Registered(\"b\"))",
            "Attempt(2)",
            "Step(Registered(\"b\"))",
            "Step(Registered(\"c\"))",
        ]
    );
    let a = row(&out, "a");
    assert_eq!(
        (a.data, a.code.as_deref(), a.archive_present),
        (Data::Failed, Some("startup-failed"), Some(false))
    );
    assert!(
        a.text
            .as_deref()
            .unwrap()
            .contains("Missing base localization: keenglish")
    );
    assert_eq!(
        (row(&out, "b").data, row(&out, "c").data),
        (Data::Committed, Data::Committed)
    );
    assert!(out.error.as_deref().unwrap().starts_with("pregame phase failed: "));
    assert!(
        out.error.as_deref().unwrap().contains("stack traceback:"),
        "the first error whole"
    );
    assert_eq!(out.retry_error, None);
    assert_eq!(out.warnings[0].code, "startup-retried");
    assert!(
        out.warnings[0].text.contains("started the game without it"),
        "{:?}",
        out.warnings
    );
    assert!(s.contents().contains(&"b blob".to_owned()) && s.contents().contains(&"c blob".to_owned()));
    let json = outcome_json(&out, "retried");
    assert_eq!(json["attempt"], "retry");
    assert_eq!(json["retryError"], serde_json::Value::Null);
}

// Optional-dependency closure through the startup controller: base fails; middle requires it; uses-middle
// optionally depends on middle (planned before it, middle drops out, so does it); uses-base optionally depends on base
// itself (skipped, as on a switched-off package: it stays); chains nearest first.
#[test]
fn an_optional_dependency_drops_a_package_only_with_a_dependent_of_the_failed_one() {
    let s = Start::new("g13-optional");
    s.patch("base", &[], &[], &broken("base fails"));
    s.patch("middle", &["base"], &[], &content("middle blob"));
    s.patch("uses-middle", &[], &["middle"], &content("uses-middle blob"));
    s.patch("uses-base", &[], &["base"], &content("uses-base blob"));
    s.patch("other", &[], &[], &content("other blob"));
    let plan = s.plan(StartupDialect::Shroudforge, &[]);
    let (_, out) = s.start(&plan, &native());
    assert_eq!((out.state, out.attempt), (State::Committed, Attempt::Retry), "{out:?}");
    assert_eq!(row(&out, "base").data, Data::Failed);
    let m = row(&out, "middle");
    assert_eq!(
        (m.code.as_deref(), m.chain.clone()),
        (Some("dependency-skipped"), vec!["base".to_owned()])
    );
    let um = row(&out, "uses-middle");
    assert_eq!(
        (um.code.as_deref(), um.chain.clone()),
        (Some("dependency-skipped"), vec!["middle".to_owned(), "base".to_owned()])
    );
    assert_eq!(um.text.as_deref(), Some("needs base, which failed in the pregame pass"));
    assert_eq!(
        (row(&out, "uses-base").data, row(&out, "other").data),
        (Data::Committed, Data::Committed)
    );
    let contents = s.contents();
    for (blob, live) in [
        ("middle blob", false),
        ("uses-middle blob", false),
        ("uses-base blob", true),
        ("other blob", true),
    ] {
        assert_eq!(contents.contains(&blob.to_owned()), live, "{blob}");
    }
    outcome_json(&out, "optional");
}

// A double failure (F9): over a previous start's owned output, the pass fails in one package and the retry fails in
// another; nothing is published, the verified BASE is restored (Deferred, the owed handoff recorded), the receipt and
// its origin are gone, every row of the pass is left out with both errors, and the BASE is granted.
#[test]
fn a_double_failure_restores_the_verified_base() {
    let s = Start::new("g13-double");
    s.patch("good", &[], &[], &edits(0, 5, "good blob"));
    let (_, first) = s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native());
    assert_eq!(first.state, State::Committed);
    assert_eq!(s.extra(0), 5);
    let sf = shroudforge_decoy(s.game());
    s.patch("bad-one", &[], &[], &broken("first failure"));
    s.patch("bad-two", &[], &[], &broken("second failure"));
    let plan = s.plan(StartupDialect::Shroudforge, &[]);
    let (_, out) = s.start(&plan, &native());
    assert_eq!((out.state, out.attempt), (State::Restored, Attempt::Retry), "{out:?}");
    assert!(out.error.as_deref().unwrap().contains("first failure"));
    assert!(out.retry_error.as_deref().unwrap().contains("second failure"));
    for r in &out.packages {
        assert_eq!(
            (r.data, r.code.as_deref(), r.archive_present),
            (Data::Excluded, Some("startup-retry-failed"), Some(false)),
            "{r:?}"
        );
        assert!(r.chain.is_empty());
    }
    assert_eq!(
        fixture::archive::validate(&s.f.game, STEM).unwrap(),
        s.f.original,
        "the BASE is live, byte for byte"
    );
    assert!(s.receipt().is_none(), "the receipt went with the output it listed");
    assert!(s.owed());
    assert_eq!(tree(&s.game().join("shroudforge")), sf);
    let live = out.live.as_ref().unwrap();
    assert_eq!(live.receipt_sha256, None);
    assert_eq!(out.origin, Origin::Unrecorded);
    let json = outcome_json(&out, "restored");
    assert_eq!(
        (json["state"].as_str(), json["grant"].as_bool()),
        (Some("restored"), Some(true))
    );
    assert_eq!(json["receiptSha256"], serde_json::Value::Null);
    assert_eq!(json["baseOrigin"], serde_json::Value::Null);
    assert_eq!(json["warnings"][0]["code"], "startup-retry-failed");
}

fn lose_backups(s: &Start) {
    for ext in ["kfc.bak", "kfc_resources.bak", "kfc.bak.integrity"] {
        fs::remove_file(s.f.game.join(format!("{STEM}.{ext}"))).unwrap();
    }
}
/// The recovery copies of set-aside receipts (`kiln/eml/ownership/<stem>.stale-<uuid>.json`) and their bytes.
fn stale_copies(s: &Start) -> Vec<(String, Vec<u8>)> {
    let dir = s.f.game.join("kiln/eml/ownership");
    let mut out: Vec<_> = fs::read_dir(&dir)
        .unwrap()
        .flatten()
        .filter_map(|e| {
            let name = e.file_name().to_string_lossy().into_owned();
            let uuid = name.strip_prefix(&format!("{STEM}.stale-"))?.strip_suffix(".json")?;
            assert_eq!(uuid.len(), 36, "{name}");
            Some((name, fs::read(e.path()).unwrap()))
        })
        .collect();
    out.sort();
    out
}

// The backups a receipt names are gone. With Kiln's output live the witness stays refused and sets
// nothing aside; once the game files are the game's again (Steam's verification) it keeps the receipt as a recovery
// copy (renamed, byte-identical, never deleted), takes a new baseline through the GPL writer (Deferred: ShroudForge's
// folder untouched) with origin rebaselined, the plan's origin from the pre-Play inspection agrees, and the
// publication records that origin keyed to the new backups.
#[test]
fn lost_backups_are_rebaselined_only_over_game_files_that_are_not_kiln_output() {
    let s = Start::new("g17b-lost");
    s.patch("alpha", &[], &[], &edits(0, 41, "alpha blob"));
    let before = s.plan(StartupDialect::Shroudforge, &[]);
    let (_, first) = s.start(&before, &native());
    assert_eq!(first.state, State::Committed);
    let receipt_path = s.f.game.join(format!("kiln/eml/ownership/{STEM}.json"));
    let receipt = fs::read(&receipt_path).unwrap();
    lose_backups(&s);
    let refused = s.witness(&before).unwrap_err();
    assert_eq!(refused.code, "baseline-missing", "{refused:?}");
    assert!(refused.text.contains("still Kiln's output"), "{refused:?}");
    assert_eq!(fs::read(&receipt_path).unwrap(), receipt, "the receipt stays where it is");
    assert!(stale_copies(&s).is_empty());
    assert_eq!(backups(&s), [false; 3], "nothing captured over Kiln's output");
    // the game's files back
    for (name, bytes) in &s.f.original.0 {
        fs::write(s.f.game.join(name), bytes).unwrap();
    }
    let sf = shroudforge_decoy(s.game());
    let mut plan = s.plan(StartupDialect::Shroudforge, &[]);
    plan.archive_base_origin = Some(inspection::REBASELINED.into());
    let w = s.witness(&plan).unwrap_or_else(|r| panic!("{r:?}"));
    assert_eq!(w.origin, Origin::Rebaselined);
    assert!(s.receipt().is_none(), "the stale receipt was set aside");
    let copies = stale_copies(&s);
    assert_eq!(copies.len(), 1);
    assert_eq!(copies[0].1, receipt, "the recovery copy is the receipt, byte for byte");
    assert_eq!(backups(&s), [true; 3], "a new working baseline");
    assert_eq!(
        fs::read(s.f.game.join(format!("{STEM}.kfc.bak"))).unwrap(),
        s.f.original.0[&format!("{STEM}.kfc")],
        "the baseline is the verified game files"
    );
    let out = s.run(&w, &plan, &native());
    assert_eq!(out.state, State::Committed, "{out:?}");
    assert_eq!(out.origin, Origin::Rebaselined);
    let recorded = s.receipt().unwrap()["baseOrigin"].clone();
    assert_eq!(recorded["token"], inspection::REBASELINED);
    assert_eq!(recorded["label"], inspection::REBASELINED_LABEL);
    let inspected = inspection::inspect(
        s.game(),
        &inspection::Options {
            side: inspection::Side::Client,
            hash_containers: false,
        },
    );
    assert_eq!(inspected.input.as_ref().unwrap().class, "owned-output");
    assert_eq!(inspected.base.as_ref().unwrap().origin.token, inspection::REBASELINED);
    assert_eq!(tree(&s.game().join("shroudforge")), sf, "the ShroudForge folder changed");
    assert_eq!(stale_copies(&s), copies, "the recovery copy is kept");
    assert!(!outcome_json(&out, "rebaselined").to_string().to_lowercase().contains("vanilla"));
}

// The receipt is set aside only as the inspection judged it: its bytes unchanged, no backup back in the folder,
// under the held install lock; a refusal leaves it where it is.
#[test]
fn a_stale_receipt_is_set_aside_only_as_it_was_judged() {
    let s = Start::new("g17b-aside");
    s.patch("alpha", &[], &[], &edits(0, 3, "alpha blob"));
    let (_, first) = s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native());
    assert_eq!(first.state, State::Committed);
    let receipt_path = s.f.game.join(format!("kiln/eml/ownership/{STEM}.json"));
    let receipt = fs::read(&receipt_path).unwrap();
    let judged = s.receipt_sha256().unwrap();
    lose_backups(&s);
    let aside = |sha: &str, guard: &fs::File| {
        mod_loader::lua::startup_host::set_aside_stale_receipt(s.game(), STEM, guard, sha)
    };
    aside(&"0".repeat(64), &s.guard).expect_err("another receipt's digest");
    let seal = s.f.game.join(format!("{STEM}.kfc.bak.integrity"));
    fs::write(&seal, b"{}").unwrap();
    aside(&judged, &s.guard).expect_err("a backup file is back");
    fs::remove_file(&seal).unwrap();
    let unheld = fs::File::open(s.game().join("mods/alpha/mod.json")).unwrap();
    aside(&judged, &unheld).expect_err("not the held install lock");
    assert_eq!(fs::read(&receipt_path).unwrap(), receipt);
    assert!(stale_copies(&s).is_empty());
    let journal = s.f.game.join(format!("kiln/eml/ownership/{STEM}.change-journal.json"));
    let path = aside(&judged, &s.guard).unwrap();
    assert!(!receipt_path.exists() && !journal.exists());
    assert_eq!(fs::read(&path).unwrap(), receipt);
    assert_eq!(stale_copies(&s).len(), 1);
}

// The witness: a plan made for other archive files, another executable or another BASE origin is stale; the install
// lock must be held by this start; a refusal's outcome grants nothing and carries the refusal on every row.
#[test]
fn the_witness_refuses_a_stale_plan_and_an_unheld_lock() {
    let s = Start::new("g13-stale");
    s.patch("alpha", &[], &[], &edits(0, 7, "alpha blob"));
    let plan = s.plan(StartupDialect::Shroudforge, &[]);
    for (what, edit) in [
        (
            "input",
            Box::new(|p: &mut startup::PlanFacts| p.archive_input_sha256 = "00".repeat(32))
                as Box<dyn Fn(&mut startup::PlanFacts)>,
        ),
        (
            "exe",
            Box::new(|p: &mut startup::PlanFacts| p.exe_sha256 = "11".repeat(32)),
        ),
        (
            "origin",
            Box::new(|p: &mut startup::PlanFacts| p.archive_base_origin = Some(inspection::REBASELINED.into())),
        ),
    ] {
        let mut stale = plan.clone();
        edit(&mut stale);
        let refused = s.witness(&stale).expect_err(what);
        assert_eq!(refused.code, "plan-stale", "{what}");
        assert_eq!(backups(&s), [false; 3], "{what}: a stale plan captures nothing");
        let out = startup::refused(&stale, startup_support::key(), &native(), &refused);
        assert_eq!((out.state, out.live.is_none()), (State::Refused, true));
        let json = outcome_json(&out, &format!("refused-{what}"));
        assert_eq!(json["grant"], false);
        assert_eq!(json["packages"][0]["code"], "plan-stale");
        assert_eq!(json["packages"][0]["archivePresent"], serde_json::Value::Null);
    }
    let unheld = fs::File::open(s.game().join("mods/alpha/mod.json")).unwrap();
    let refused = startup::witness(&s.f.game, STEM, &unheld, &plan, startup_support::key()).unwrap_err();
    assert_eq!(refused.code, "lock-busy");
    assert!(s.witness(&plan).is_ok());
}

// A reference index whose resource-type bundle points outside its index list (kfc-base slices it unchecked) is refused
// by the witness with a reason, before any DLL loads, instead of panicking in an attempt.
#[test]
fn a_malformed_resource_type_index_is_refused_by_the_witness() {
    let s = Start::new("g13-bundles");
    s.patch("alpha", &[], &[], &edits(0, 2, "alpha blob"));
    fixture::archive::change_table_u32(&s.f.game, 14, 8, 999);
    let plan = s.plan(StartupDialect::Shroudforge, &[]);
    let refused = s.witness(&plan).unwrap_err();
    assert_eq!(refused.code, "invalid", "{refused:?}");
    assert!(refused.text.contains("resource type index"), "{}", refused.text);
}

// A changed input (K7, K9; owner Q3): the terminal says a DLL changed the witnessed files, or the witness's own check
// sees it; nothing is published, retried or granted, every row says so and the warning names the DLL packages that
// loaded; the live archive and the receipt are as before.
#[test]
fn a_changed_input_publishes_nothing_and_grants_nothing() {
    let s = Start::new("g13-changed");
    s.patch("dll-pack", &[], &[], &edits(0, 9, "dll-pack blob"));
    let plan = s.plan(StartupDialect::Shroudforge, &["dll-pack"]);
    let w = s.witness(&plan).unwrap();
    let before = fixture::archive::validate(&s.f.game, STEM).unwrap();
    let mut facts = native();
    facts.coherent = Err("enshrouded.kfc_resources changed while package DLLs loaded".into());
    facts.dlls.insert("dll-pack".into(), vec!["loaded".into()]);
    let out = s.run(&w, &plan, &facts);
    assert_eq!(
        (out.state, out.attempt, out.live.is_none()),
        (State::Failed, Attempt::None, true)
    );
    assert_eq!(row(&out, "dll-pack").code.as_deref(), Some("startup-input-changed"));
    assert!(
        out.warnings[0].text.contains("DLL packages that loaded: dll-pack"),
        "{:?}",
        out.warnings
    );
    assert!(points(&s, "Attempt").is_empty(), "no attempt ran");
    assert_eq!(fixture::archive::validate(&s.f.game, STEM).unwrap(), before);
    assert!(s.receipt().is_none());
    let json = outcome_json(&out, "input-changed");
    assert_eq!(json["native"]["coherent"], false);
    // the witness's own check: the resource file rewritten with the same bytes after the witness
    let resources = s.game().join(format!("{STEM}.kfc_resources"));
    let bytes = fs::read(&resources).unwrap();
    std::thread::sleep(std::time::Duration::from_millis(20));
    fs::write(&resources, &bytes).unwrap();
    let out = s.run(&w, &plan, &native());
    assert_eq!((out.state, out.live.is_none()), (State::Failed, true));
    assert_eq!(row(&out, "dll-pack").code.as_deref(), Some("startup-input-changed"));
    assert!(s.receipt().is_none());
}

// Deferred mode never touches ShroudForge's folder: with it unreadable (and its applied record a decoy) a whole start,
// a publication and a verified restore succeed; the same folder makes the handoff that is owed fail (the control: it
// really is unreadable), and once it is readable again that handoff marks the applied record stale and the marker goes.
#[cfg(unix)]
#[test]
fn deferred_mode_never_opens_shroudforges_folder() {
    use std::os::unix::fs::PermissionsExt;
    let s = Start::new("g13-deferred");
    s.patch("alpha", &[], &[], &edits(0, 21, "alpha blob"));
    let sf_dir = s.game().join("shroudforge");
    let snapshot = shroudforge_decoy(s.game());
    let plan = s.plan(StartupDialect::Shroudforge, &[]);
    fs::set_permissions(&sf_dir, fs::Permissions::from_mode(0o000)).unwrap();
    let _restore = ModeGuard(sf_dir.clone(), 0o755);
    let readable = fs::read_dir(&sf_dir).is_ok(); // running as root: the folder stays readable, the control is skipped
    let (_, out) = s.start(&plan, &native());
    assert_eq!(out.state, State::Committed, "{out:?}");
    s.patch("bad-one", &[], &[], &broken("one"));
    s.patch("bad-two", &[], &[], &broken("two"));
    let (_, out) = s.start(&s_plan_without_inspect_change(&s), &native());
    assert_eq!(out.state, State::Restored, "{out:?}");
    assert!(s.owed());
    if !readable {
        assert!(
            mod_loader::lua::settle_owed_handoff(s.game(), STEM).is_err(),
            "the folder is unreadable"
        );
    }
    fs::set_permissions(&sf_dir, fs::Permissions::from_mode(0o755)).unwrap();
    assert_eq!(tree(&sf_dir), snapshot, "ShroudForge's folder changed");
    assert!(mod_loader::lua::settle_owed_handoff(s.game(), STEM).unwrap());
    assert!(!s.owed());
    let state: serde_json::Value = serde_json::from_slice(&fs::read(sf_dir.join("state.json")).unwrap()).unwrap();
    assert_eq!(
        state["assets"]["status"], "failed",
        "the owed handoff marks the stale applied record"
    );
}
#[cfg(unix)]
fn s_plan_without_inspect_change(s: &Start) -> startup::PlanFacts {
    s.plan(StartupDialect::Shroudforge, &[])
}

// The receipt's BASE origin: a Deferred publication records the host's (keyed to the backups); one
// without a recorded origin carries the earlier receipt's over while the backups are the same; an earlier one that is
// no recorded token ("vanilla") is never carried; a Live (EML-mode) publication never writes one.
#[test]
fn the_base_origin_is_recorded_carried_over_and_never_upgraded() {
    let s = Start::new("g13-origin");
    s.patch("alpha", &[], &[], &edits(0, 1, "alpha blob"));
    let (w, out) = s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native());
    assert_eq!((w.origin, out.state), (Origin::FirstManaged, State::Committed));
    let recorded = s.receipt().unwrap()["baseOrigin"].clone();
    assert_eq!(recorded["token"], inspection::FIRST_MANAGED);
    let publish = |handoff: SfHandoff, value: u32| {
        let base =
            Rc::new(kfc::container::KFCFile::from_path(s.f.game.join(format!("{STEM}.kfc.bak")), false).unwrap());
        let mut host = StartupHost::open(s.game(), STEM, s.f.types.clone(), base, &s.guard)
            .unwrap()
            .with_handoff(handoff);
        let lent = host.begin_stage().unwrap().lend();
        lent.writer()
            .write_content(
                &kfc::guid::ContentHash::from_data(&value.to_le_bytes()),
                &value.to_le_bytes(),
            )
            .unwrap();
        lent.prepare().unwrap().publish().unwrap();
    };
    // carried over: no origin given, the same backups
    publish(SfHandoff::Deferred, 2);
    assert_eq!(s.receipt().unwrap()["baseOrigin"], recorded);
    // never carried: an earlier origin that is no recorded token
    let path = s.f.game.join(format!("kiln/eml/ownership/{STEM}.json"));
    let mut receipt = s.receipt().unwrap();
    receipt["baseOrigin"]["token"] = "vanilla".into();
    fs::write(&path, serde_json::to_vec(&receipt).unwrap()).unwrap();
    publish(SfHandoff::Deferred, 3);
    assert_eq!(
        s.receipt().unwrap().get("baseOrigin"),
        None,
        "an unknown token was carried over"
    );
    // a kiln start over an unrecorded BASE records none (never upgraded); a Live publication writes none either
    let (_, out) = s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native());
    assert_eq!(
        out.origin,
        Origin::Unrecorded,
        "the receipt recorded none: unrecorded, never upgraded"
    );
    assert_eq!(s.receipt().unwrap().get("baseOrigin"), None);
    publish(SfHandoff::Live, 4);
    assert_eq!(s.receipt().unwrap().get("baseOrigin"), None);
    // the backups changed (another pair): an origin keyed to the old ones is not carried
    let mut receipt = s.receipt().unwrap();
    receipt["baseOrigin"] = recorded.clone();
    fs::write(&path, serde_json::to_vec(&receipt).unwrap()).unwrap();
    let seal = s.f.game.join(format!("{STEM}.kfc.bak.integrity"));
    let sealed = fs::read(&seal).unwrap();
    fs::remove_file(&seal).unwrap();
    publish(SfHandoff::Deferred, 5);
    assert_eq!(
        s.receipt().unwrap().get("baseOrigin"),
        None,
        "carried over to other backups"
    );
    fs::write(&seal, sealed).unwrap();
}

// The EML dialect: the plan's admitted EML package runs on the witnessed host (prepare_eml), one Deferred publication,
// rows with pass "eml"; the next start is EML's own change-cache hit (cached-verified).
#[test]
fn the_eml_dialect_publishes_once_and_hits_emls_cache() {
    let s = Start::new("g13-eml");
    let path = s.package(
        "eml-mod",
        serde_json::json!({"capabilities": ["patch"]}),
        Some(
            r#"local r = game.assets.get_resource("01234567-89ab-cdef-0123-456789abcdef", game.types.get("m6.Root"))
r.data.child.extra = 77"#,
        ),
    );
    let mut plan = s.plan(StartupDialect::Eml, &[]);
    plan.packages[0].kind = startup::PackageKind::Eml;
    let env = |sel: startup::EmlSelection<'_>| -> anyhow::Result<Option<mod_loader::ModEnvironment>> {
        assert_eq!(sel, startup::EmlSelection::Admitted(&["eml-mod".to_owned()]));
        Ok(Some(
            mod_loader::ModEnvironment::load_from(
                &s.f.game,
                s.f.game.join("kiln/eml/cache"),
                vec![camino::Utf8PathBuf::from_path_buf(path.clone()).unwrap()],
            )
            .map_err(|_| anyhow::anyhow!("load"))?,
        ))
    };
    let w = s.witness(&plan).unwrap();
    let out = s.run_with(&w, &plan, &native(), &env, &|_| {});
    assert_eq!(out.state, State::Committed, "{out:?}");
    assert_eq!(s.extra(0), 77);
    assert_eq!(row(&out, "eml-mod").data, Data::Committed);
    let json = outcome_json(&out, "eml");
    assert_eq!(json["packages"][0]["pass"], "eml");
    assert!(s.owed());
    let w = s.witness(&plan).unwrap();
    let again = s.run_with(&w, &plan, &native(), &env, &|_| {});
    assert_eq!(again.state, State::Cached, "{again:?}");
    assert_eq!(row(&again, "eml-mod").data, Data::CachedVerified);
    assert_eq!(
        again.live.as_ref().unwrap().receipt_sha256,
        out.live.as_ref().unwrap().receipt_sha256
    );
    outcome_json(&again, "eml-cached");
}

// No startup pass (dialect none) and no unlisted contributor: the owned output a previous start left is restored
// (verified BASE, no rows); with the BASE live already, the start is unchanged.
#[test]
fn no_pass_restores_owned_output_then_is_unchanged() {
    let s = Start::new("g13-none");
    s.patch("alpha", &[], &[], &edits(0, 3, "alpha blob"));
    let (_, first) = s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native());
    assert_eq!(first.state, State::Committed);
    fs::remove_dir_all(s.game().join("mods/alpha")).unwrap();
    let plan = s.plan(StartupDialect::None, &[]);
    let (_, out) = s.start(&plan, &native());
    assert_eq!((out.state, out.packages.len()), (State::Restored, 0), "{out:?}");
    assert_eq!(fixture::archive::validate(&s.f.game, STEM).unwrap(), s.f.original);
    let json = outcome_json(&out, "none-restored");
    assert_eq!(json["dialect"], "none");
    let (_, out) = s.start(&s.plan(StartupDialect::None, &[]), &native());
    assert_eq!(out.state, State::Unchanged, "{out:?}");
    outcome_json(&out, "none-unchanged");
}

// kiln-sfi never attached (K5): every package of the ShroudForge pass is left out (backend-unavailable) and the BASE is
// what is live; attached without a terminal (K6): the packages with native code and what needs them are left out.
#[test]
fn without_kiln_sfi_or_its_terminal_the_affected_packages_are_left_out() {
    let s = Start::new("g13-native");
    s.patch("dll-pack", &[], &[], &edits(0, 13, "dll blob"));
    s.patch("needs-dll", &["dll-pack"], &[], &content("needs blob"));
    s.patch("lua-only", &[], &[], &content("lua blob"));
    let plan = s.plan(StartupDialect::Shroudforge, &["dll-pack"]);
    let absent = NativeFacts {
        attached: false,
        received: false,
        withdrawn: BTreeMap::new(),
        coherent: Ok(()),
        dlls: BTreeMap::new(),
    };
    let (_, out) = s.start(&plan, &absent);
    assert_eq!(out.state, State::Unchanged, "{out:?}");
    assert!(
        out.packages
            .iter()
            .all(|r| r.code.as_deref() == Some("backend-unavailable"))
    );
    assert!(points(&s, "Attempt").is_empty());
    let json = outcome_json(&out, "backend-unavailable");
    assert_eq!(json["native"]["coherent"], false);
    let silent = NativeFacts {
        attached: true,
        received: false,
        ..absent
    };
    let (_, out) = s.start(&plan, &silent);
    assert_eq!(out.state, State::Committed, "{out:?}");
    assert_eq!(row(&out, "dll-pack").code.as_deref(), Some("native-terminal-missing"));
    assert_eq!(row(&out, "needs-dll").chain, ["dll-pack"]);
    assert_eq!(row(&out, "lua-only").data, Data::Committed);
}

// The outcome record byte for byte (a typed serializer: schema field order, clamped free text, an identity's digest).
#[test]
fn the_outcome_record_is_written_byte_for_byte_in_the_schemas_order() {
    let out = startup::StartupOutcome {
        key: startup_support::key(),
        state: State::Refused,
        dialect: StartupDialect::Shroudforge,
        attempt: Attempt::None,
        error: None,
        retry_error: None,
        packages: vec![startup::Row {
            id: "pkg".into(),
            version: Some("1.0.0".into()),
            data: Data::Excluded,
            archive_present: Some(false),
            code: Some("plan-stale".into()),
            text: Some("x".repeat(5000)),
            chain: (0..70).map(|i| format!("d{i}")).collect(),
        }],
        warnings: vec![startup::Warning {
            code: "plan-stale".into(),
            text: "stale \"plan\"".into(),
        }],
        native: startup::NativeSummary {
            received: false,
            incoherent: Some("none".into()),
        },
        live: None,
        origin: Origin::FirstManaged,
    };
    let text = out.json(None);
    // a record without a grant names no BASE origin, whatever the witness found
    let chain: Vec<String> = (0..64).map(|i| format!("\"d{i}\"")).collect();
    let expected = format!(
        "{{\"format\":1,\"launchId\":\"00000000000004d2\",\"planId\":\"6d362d67-3133-2d70-6c61-6e2d69642d30\",\
         \"planSha256\":\"{}\",\"dialect\":\"shroudforge\",\"state\":\"refused\",\"grant\":false,\"receiptSha256\":null,\
         \"archiveSha256\":null,\"baseOrigin\":null,\"native\":{{\"received\":false,\"coherent\":false,\
         \"text\":\"none\"}},\"attempt\":\"none\",\"error\":null,\"retryError\":null,\"packages\":[{{\"id\":\"pkg\",\
         \"version\":\"1.0.0\",\"pass\":\"shroudforge\",\"data\":\"excluded\",\"archivePresent\":null,\"code\":\"plan-stale\",\
         \"text\":\"{}\",\"chain\":[{}]}}],\"warnings\":[{{\"code\":\"plan-stale\",\"text\":\"stale \\\"plan\\\"\"}}]}}",
        "aa".repeat(32),
        "x".repeat(4096),
        chain.join(",")
    );
    assert_eq!(text, expected);
    outcome_json(&out, "byte-exact");
}

// a package whose own entry module is missing fails as itself although the
// core reports it with an OS error code, so the pass runs once more without it; an entry module that exists but cannot
// be read stays the system's failure (never retried).
#[test]
fn a_missing_entry_module_is_retried_an_unreadable_one_is_not() {
    let s = Start::new("g13-entry");
    s.package("gone", serde_json::json!({"capabilities": ["patch"]}), None);
    s.patch("kept", &[], &[], &content("kept blob"));
    let (_, out) = s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native());
    assert_eq!((out.state, out.attempt), (State::Committed, Attempt::Retry), "{out:?}");
    let gone = row(&out, "gone");
    assert_eq!(
        (gone.data, gone.code.as_deref()),
        (Data::Failed, Some("startup-failed"))
    );
    assert!(
        gone.text
            .as_deref()
            .unwrap()
            .contains("failed to load module: src/mod.lua"),
        "{gone:?}"
    );
    assert!(s.contents().contains(&"kept blob".to_owned()));
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let s = Start::new("g13-entry-denied");
        let denied = s.patch("denied", &[], &[], &content("denied blob"));
        s.patch("kept", &[], &[], &content("kept blob"));
        let plan = s.plan(StartupDialect::Shroudforge, &[]);
        let entry = denied.join("src/mod.lua");
        fs::set_permissions(&entry, fs::Permissions::from_mode(0o000)).unwrap();
        let _restore = ModeGuard(entry.clone(), 0o644);
        if fs::read(&entry).is_err() {
            // the digest port reads every file: give it the plan's digest so admission lets the package through
            let w = s.witness(&plan).unwrap();
            let out = startup::run(
                &w,
                &plan,
                &native(),
                &s.guard,
                startup::Ports {
                    types: s.f.types.clone(),
                    registry: s.registry(),
                    catalog: Rc::new(startup_support::Catalog),
                    locale: None,
                    vm: kiln_eml::sf_startup::runner::Ports {
                        clock: Rc::new(Clock),
                        log: s.vm_log.clone(),
                    },
                    digest: &|root: &std::path::Path| {
                        let id = root.file_name().unwrap().to_string_lossy().into_owned();
                        Ok(plan.package(&id).unwrap().content_sha256)
                    },
                    eml: &|_| Ok(None),
                    log: &|_| {},
                    observe: &|_| {},
                },
            );
            assert_eq!((out.state, out.attempt), (State::Unchanged, Attempt::First), "{out:?}");
            assert!(
                out.packages
                    .iter()
                    .all(|r| r.code.as_deref() == Some("startup-pass-failed"))
            );
            assert!(
                out.error
                    .as_deref()
                    .unwrap()
                    .to_lowercase()
                    .contains("permission denied"),
                "{:?}",
                out.error
            );
        }
        fs::set_permissions(&entry, fs::Permissions::from_mode(0o644)).unwrap();
    }
}
struct Clock;
impl sfi_core::host::Clock for Clock {
    fn seconds(&self) -> f64 {
        1.0
    }
}

// A panic inside an attempt (here: the caller's observer at the VM step) is contained: a failure no package raised,
// never retried; the pass ends on the verified BASE, every row startup-pass-failed.
#[test]
fn a_panic_in_an_attempt_is_contained_and_never_retried() {
    let s = Start::new("g13-panic");
    s.patch("alpha", &[], &[], &content("alpha blob"));
    let plan = s.plan(StartupDialect::Shroudforge, &[]);
    let w = s.witness(&plan).unwrap();
    let out = s.run_with(&w, &plan, &native(), &|_| Ok(None), &|point| {
        if format!("{point:?}") == "Step(VmReady)" {
            panic!("an observer that panics");
        }
    });
    assert_eq!((out.state, out.attempt), (State::Unchanged, Attempt::First), "{out:?}");
    assert_eq!(points(&s, "Attempt"), ["Attempt(1)"]);
    assert!(out.error.as_deref().unwrap().contains("panic: an observer that panics"));
    assert_eq!(row(&out, "alpha").code.as_deref(), Some("startup-pass-failed"));
    assert!(!s.contents().contains(&"alpha blob".to_owned()));
    outcome_json(&out, "panicked");
}

// EML's change cache is completed only while the install guard is held: once it is not, the
// completion is refused and writes nothing; the publication stands.
#[test]
fn the_change_cache_is_never_completed_after_the_guard_is_gone() {
    let s = Start::new("g13-commit-guard");
    let path = s.package(
        "eml-mod",
        serde_json::json!({"capabilities": ["patch"]}),
        Some(
            r#"local r = game.assets.get_resource("01234567-89ab-cdef-0123-456789abcdef", game.types.get("m6.Root"))
r.data.child.extra = 55"#,
        ),
    );
    let plan = s.plan(StartupDialect::Shroudforge, &[]);
    let w = s.witness(&plan).unwrap();
    let base = Rc::new(kfc::container::KFCFile::from_path(s.f.game.join(format!("{STEM}.kfc.bak")), false).unwrap());
    let mut host = StartupHost::open_matching(s.game(), STEM, s.f.types.clone(), base, &s.guard, &w.input).unwrap();
    let env = mod_loader::ModEnvironment::load_from(
        &s.f.game,
        s.f.game.join("kiln/eml/cache"),
        vec![camino::Utf8PathBuf::from_path_buf(path).unwrap()],
    )
    .map_err(|_| ())
    .unwrap();
    let args = mod_loader::lua::RunArgs {
        file_name: STEM.into(),
        options: mod_loader::lua::RunOptions {
            patch: true,
            ..Default::default()
        },
    };
    let mod_loader::lua::startup_host::PreparedEml::Staged(run) = host
        .prepare_eml(&env, args, &mod_loader::lua::Progress::none())
        .unwrap()
    else {
        panic!("expected a staged run");
    };
    let (archive, commit) = run.into_parts();
    let published = archive.publish().unwrap();
    s.guard.unlock().unwrap();
    let refused = commit.finish_after_publish(&published).unwrap_err();
    assert!(
        refused.to_string().contains("install guard is no longer held"),
        "{refused}"
    );
    assert!(
        !s.f.game.join("kiln/eml/cache/files.json").exists(),
        "the change cache was saved without the guard"
    );
    assert_eq!(s.extra(0), 55, "the publication stands");
    s.guard.try_lock().unwrap();
}

// A Live (EML-mode) write makes the handoff a kiln-mode write owes: ShroudForge's applied record is marked stale under
// its lock and the marker goes.
#[test]
fn a_live_write_makes_the_handoff_kiln_mode_owes() {
    let s = Start::new("g13-owed-live");
    s.patch("alpha", &[], &[], &edits(0, 8, "alpha blob"));
    shroudforge_decoy(s.game());
    let (_, out) = s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native());
    assert_eq!(out.state, State::Committed);
    assert!(s.owed());
    let base = Rc::new(kfc::container::KFCFile::from_path(s.f.game.join(format!("{STEM}.kfc.bak")), false).unwrap());
    let mut host = StartupHost::open(s.game(), STEM, s.f.types.clone(), base, &s.guard).unwrap();
    let lent = host.begin_stage().unwrap().lend();
    lent.writer()
        .write_content(&kfc::guid::ContentHash::from_data(b"live"), b"live")
        .unwrap();
    lent.prepare().unwrap().publish().unwrap();
    assert!(!s.owed(), "the Live publication did not make the owed handoff");
    let state: serde_json::Value =
        serde_json::from_slice(&fs::read(s.game().join("shroudforge/state.json")).unwrap()).unwrap();
    assert_eq!(state["assets"]["status"], "failed");
}

// The receipt records only the inspector's unclassified tokens, with their exact labels; the witness is Send + Sync.
#[test]
fn the_recorded_origins_are_the_inspectors_and_the_witness_is_shared_read_only() {
    assert_eq!(
        mod_loader::lua::RECORDED_ORIGINS.to_vec(),
        inspection::ORIGINS[..2].to_vec()
    );
    fn shared<T: Send + Sync>() {}
    shared::<startup::Witnessed>();
}

// A failed pass over output Kiln cannot prove it published: the BASE cannot be verified (the live header is not the
// backup's), so nothing is granted and no row claims anything about the live archive (archivePresent null).
#[test]
fn a_failed_pass_over_unproven_output_grants_nothing_and_proves_nothing() {
    let s = Start::new("g13-unproven");
    s.patch("good", &[], &[], &edits(0, 6, "good blob"));
    let (_, first) = s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native());
    assert_eq!(first.state, State::Committed);
    fs::remove_file(s.f.game.join(format!("kiln/eml/ownership/{STEM}.json"))).unwrap();
    s.patch("bad-one", &[], &[], &broken("one"));
    s.patch("bad-two", &[], &[], &broken("two"));
    let (_, out) = s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native());
    assert_eq!((out.state, out.live.is_none()), (State::Failed, true), "{out:?}");
    assert!(
        out.warnings.iter().any(|w| w.code == "base-not-restored"),
        "{:?}",
        out.warnings
    );
    assert_eq!(s.extra(0), 6, "unproven output is never touched");
    let json = outcome_json(&out, "unproven");
    assert_eq!(json["grant"], false);
    for row in json["packages"].as_array().unwrap() {
        assert_eq!(row["archivePresent"], serde_json::Value::Null, "{row}");
    }
}

// A publication that fails after a retried pass (here ShroudForge's own unfinished publication appears once the retry
// prepared): nothing is published or granted, and the outcome still names the first attempt's error (the schema: an
// outcome of a retry has one).
#[test]
fn a_publication_failure_after_a_retry_keeps_the_first_attempts_error() {
    let s = Start::new("g13-retry-publish");
    s.patch("bad", &[], &[], &broken("bad fails"));
    s.patch("ok", &[], &[], &content("ok blob"));
    let plan = s.plan(StartupDialect::Shroudforge, &[]);
    let w = s.witness(&plan).unwrap();
    let journal = s.game().join(format!(".{STEM}.shroudforge-stage"));
    let out = s.run_with(&w, &plan, &native(), &|_| Ok(None), &|point| {
        if format!("{point:?}") == "Step(Prepared)" {
            fs::create_dir_all(&journal).unwrap();
            fs::write(journal.join("pending.json"), b"[]").unwrap();
        }
    });
    assert_eq!(
        (out.state, out.attempt, out.live.is_none()),
        (State::Failed, Attempt::Retry, true),
        "{out:?}"
    );
    assert!(
        out.error.as_deref().is_some_and(|e| e.contains("bad fails")),
        "the first attempt's error: {:?}",
        out.error
    );
    assert!(
        out.warnings.iter().any(|w| w.code == "startup-publication-failed"),
        "{:?}",
        out.warnings
    );
    assert!(s.receipt().is_none(), "nothing was published");
    assert!(!s.contents().contains(&"ok blob".to_owned()));
    let json = outcome_json(&out, "retry-publication-failed");
    assert!(json["error"].is_string(), "{json}");
    assert_eq!(json["grant"], false);
}

// restore_base accepts the BASE only as the game reads it: the live header byte for byte the
// backup's, and the live resource file holding the whole resource backup from its start. A torn (short), foreign
// (other bytes, also at the published file's length) or empty resource file under the BASE header is never accepted:
// refused, the resource file and the receipt left as they are. What a restore interrupted between the header and the
// resource file leaves (the BASE header over the published resource file, whose appended chunks the BASE header never
// references) is accepted, and the receipt goes.
#[test]
fn restore_base_never_accepts_a_torn_or_foreign_resource_file() {
    let s = Start::new("g13-torn");
    let vanilla = s.f.semantics();
    s.patch("alpha", &[], &[], &edits(0, 4, "alpha blob"));
    let (_, first) = s.start(&s.plan(StartupDialect::Shroudforge, &[]), &native());
    assert_eq!(first.state, State::Committed, "{first:?}");
    let receipt = s.receipt_sha256().unwrap();
    let file = |ext: &str| s.game().join(format!("{STEM}.{ext}"));
    let base = fs::read(file("kfc_resources.bak")).unwrap();
    let published = fs::read(file("kfc_resources")).unwrap();
    assert!(
        published.len() > base.len() && published.starts_with(&base),
        "the published resource file starts with the BASE's"
    );
    // the header back as a restore interrupted before the resource file leaves it
    fs::copy(file("kfc.bak"), file("kfc")).unwrap();
    let restore = || {
        let reference = Rc::new(kfc::container::KFCFile::from_path(file("kfc.bak"), false).unwrap());
        StartupHost::open(s.game(), STEM, s.f.types.clone(), reference, &s.guard)
            .and_then(|host| host.with_handoff(SfHandoff::Deferred).restore_base())
    };
    let torn = base[..base.len() - 1].to_vec();
    let mut foreign = base.clone();
    foreign[base.len() / 2] ^= 0xff;
    let mut foreign_published = published.clone();
    foreign_published[0] ^= 0xff;
    for (what, bytes) in [
        ("torn", &torn),
        ("foreign", &foreign),
        ("foreign-published", &foreign_published),
        ("empty", &Vec::new()),
    ] {
        fs::write(file("kfc_resources"), bytes).unwrap();
        let refused = restore().expect_err(what);
        assert!(
            format!("{refused:#}").contains("neither the BASE"),
            "{what}: {refused:#}"
        );
        assert_eq!(
            &fs::read(file("kfc_resources")).unwrap(),
            bytes,
            "{what}: the resource file was touched"
        );
        assert_eq!(
            s.receipt_sha256().as_deref(),
            Some(receipt.as_str()),
            "{what}: the receipt went"
        );
    }
    fs::write(file("kfc_resources"), &published).unwrap();
    let restored = restore().unwrap();
    assert_eq!((restored.copied, restored.receipt_removed), (false, true));
    assert_eq!(s.f.semantics(), vanilla, "the BASE as the game reads it");
    assert_ne!(
        fixture::archive::validate(&s.f.game, STEM).unwrap(),
        s.f.original,
        "the published resource file stays (the next publication replaces it)"
    );
}
