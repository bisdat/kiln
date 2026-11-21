// SPDX-License-Identifier: MIT
//! A game switched between ShroudForge's backends and back: hosted or installed ShroudForge, then Kiln's
//! interpreter, then the backend the player had, on one game folder, through `profile::activate` as the launcher's
//! Play and the ShroudForge card do it.
//!
//! - **The round trip** (hosted, installed, and the older `useInstalled` setting): ShroudForge's own config names a mods
//!   folder that is not `<game>/mods`. Kiln's copies move between that folder and `<game>/mods` with the player's
//!   values of their settings; the player's own files (a DLL in the game folder, their own packages in either folder,
//!   their chain.ini line, their kiln.json notes and keys) are kept; a plan exists only while kiln.json selects the
//!   interpreter; nothing below `<game>/shroudforge` is written. The records a kiln start leaves (kiln-sfi's sfi.json,
//!   kiln-eml's ownership receipt and the handoff it owes ShroudForge) are left for the game: kiln-eml settles that
//!   handoff at the first hosted start (its own test, `a_hosted_stand_aside_settles_the_handoff_a_kiln_start_owes`), so
//!   nothing here may remove the marker or touch ShroudForge's record itself.
//! - **The switch to kiln stopped at each journal boundary** (a real kill of this test binary, as sfi_profile_activation
//!   does it), then the player goes back: main's activation settles the stopped switch first and the game is on its
//!   earlier backend whole; switching to kiln once more and back again is whole too.
//! - **In kiln mode the engine row and the support report never read ShroudForge's state** (an inotify tripwire over
//!   `<game>/shroudforge` on Linux): a failed startup pass ShroudForge recorded while it was hosted is no note and no
//!   finding of a kiln start.
//!
//! Every package and DLL here is synthetic; no
//! Steam, game, server or Wine runs.

mod sfi_profile_common;
mod sfi_routing_common;

use kiln_manage::engine_mods;
use kiln_manage::manifest::{ModKind, RunSide};
use kiln_manage::profile;
use kiln_manage::sfi_activation::{self, Boundary};
use kiln_manage::sfi_backend::{self, Backend};
use kiln_manage::sfi_plan::Mode;
use kiln_manage::shroudforge;
use serde_json::{Value, json};
use sfi_profile_common::*;
use sfi_routing_common::*;
use std::collections::BTreeMap;

const ALL: [Boundary; 22] = [
    Boundary::JournalBegun,
    Boundary::GameFilesStaged,
    Boundary::GameFilesIntent,
    Boundary::GameFilesOp,
    Boundary::GameFilesCommitted,
    Boundary::ChainStaged,
    Boundary::ChainWritten,
    Boundary::PlacementsStaged,
    Boundary::PlacementParked,
    Boundary::PlacementPublished,
    Boundary::PlacementReceipt,
    Boundary::PlacementsDone,
    Boundary::PlanStaged,
    Boundary::PlanWritten,
    Boundary::ProfileStaged,
    Boundary::ProfileWritten,
    Boundary::Committed,
    Boundary::Released,
    Boundary::RecoverPlanRemoved,
    Boundary::RecoverChainRestored,
    Boundary::RecoverGameFilesRestored,
    Boundary::RecoverSettled,
];

/// The child (a no-op in a normal run).
#[test]
fn kill_child() {
    child(&ALL, |w, what| match what {
        "activate" => drop(profile::activate(&w.dirs, &w.profile, &w.inst)),
        other => panic!("{other}"),
    });
}

/// ShroudForge's own config names this folder for its mods.
const SF_MODS: &str = "sfmods";

/// The backend the player had before Kiln's interpreter, as their kiln.json says it.
#[derive(Clone, Copy, Debug, PartialEq)]
enum Legacy {
    Hosted,
    Installed,
    /// The older setting (`useInstalled: true`), which the card replaces with `backend: "installed"` on the way back.
    UseInstalled,
}

impl Legacy {
    const ALL: [Legacy; 3] = [Legacy::Hosted, Legacy::Installed, Legacy::UseInstalled];

    fn setting(self) -> &'static str {
        match self {
            Legacy::Hosted => r#""backend": "hosted""#,
            Legacy::Installed => r#""backend": "installed""#,
            Legacy::UseInstalled => r#""useInstalled": true"#,
        }
    }

    fn back(self) -> Backend {
        match self {
            Legacy::Hosted => Backend::Hosted,
            Legacy::Installed | Legacy::UseInstalled => Backend::Installed,
        }
    }
}

/// The player's kiln.json: a note and a key of their own beside the backend (and the lift a preview build carries).
fn kiln_json(setting: &str) -> String {
    format!(
        "{{\n  // the player's own note\n  \"log_level\": \"debug\",\n  \"preview\": [\"kiln-sfi.interpreter\"],\n  \
         \"shroudforge\": {{{setting}}}\n}}\n"
    )
}

/// The backend switched as the ShroudForge card switches it (one key written, the player's text kept).
fn switch(w: &World, to: Backend) {
    sfi_backend::write_game(&w.game, Some(to), RunSide::Client)
        .unwrap_or_else(|e| panic!("{to:?}: {e:?}"));
}

fn activate(w: &World, what: &str) -> profile::Activation {
    profile::activate(&w.dirs, &w.profile, &w.inst).unwrap_or_else(|e| panic!("{what}: {e:?}"))
}

/// A world (sfi_profile_common) on a legacy backend: ShroudForge installed with its config naming [`SF_MODS`], the
/// player's kiln.json, the player's own package in ShroudForge's folder, the profile activated on main's route and the
/// player's value of the ShroudForge mod's setting in Kiln's copy there.
fn on_legacy(w: &World, legacy: Legacy) {
    sf_install(&w.game, Some(SF_MODS));
    put(
        &w.game,
        "kiln/kiln.json",
        kiln_json(legacy.setting()).as_bytes(),
    );
    put(
        &w.game,
        "sfmods/mine/mod.json",
        br#"{"id":"mine","name":"Mine","version":"1.0.0"}"#,
    );
    put(
        &w.game,
        "sfmods/mine/extended.mod.json",
        br#"{"schemaVersion":1,"launcher":"SF","enabled":true}"#,
    );
    activate(w, "on the legacy backend");
    shroudforge::set_setting(&w.file("sfmods/sfpack"), "speed", &json!(7)).unwrap();
}

fn speed(w: &World, dir: &str) -> Value {
    shroudforge::read_mod(&w.file(dir))
        .unwrap_or_else(|e| panic!("{dir}: {e:?}"))
        .settings
        .into_iter()
        .find(|s| s.key == "speed")
        .unwrap()
        .value
}

/// What must never change in any of these steps: the player's files and ShroudForge's own folder.
struct Kept {
    mine: BTreeMap<String, String>,
    shroudforge: BTreeMap<String, String>,
}

impl Kept {
    fn of(w: &World) -> Kept {
        Kept {
            mine: snapshot(&w.file("sfmods/mine")),
            shroudforge: snapshot(&w.file("shroudforge")),
        }
    }

    fn check(&self, w: &World, what: &str) {
        let s = GameState::of(w);
        assert_eq!(
            s.player,
            (PLAYER_DLL.to_vec(), PLAYER_SF.to_vec()),
            "{what}: the player's files"
        );
        assert_eq!(
            snapshot(&w.file("sfmods/mine")),
            self.mine,
            "{what}: the player's package in ShroudForge's folder"
        );
        assert_eq!(
            snapshot(&w.file("shroudforge")),
            self.shroudforge,
            "{what}: ShroudForge's own folder"
        );
        let chain = String::from_utf8(s.chain.clone().unwrap_or_default()).unwrap();
        assert!(
            chain.contains("player.dll"),
            "{what}: the player's chain.ini line: {chain}"
        );
        let text = std::fs::read_to_string(w.file("kiln/kiln.json")).unwrap();
        assert!(
            text.contains("// the player's own note") && text.contains("\"log_level\": \"debug\""),
            "{what}: the player's kiln.json: {text}"
        );
        assert!(!s.pending, "{what}: game_files settled");
    }
}

/// The game on its legacy backend: main's route whole, no plan, no journal, Kiln's copies (with the player's value)
/// in ShroudForge's folder and none in `<game>/mods`.
fn assert_legacy(w: &World, kept: &Kept, what: &str) {
    kept.check(w, what);
    let s = GameState::of(w);
    assert!(
        s.plans.is_empty(),
        "{what}: a plan on main's route: {:?}",
        s.plans
    );
    assert!(!s.journal, "{what}: a journal left");
    assert!(
        w.doc(Mode::Active).get("sfLaunchPlan").is_none(),
        "{what}: {}",
        w.doc(Mode::Active)
    );
    assert_eq!(sfi_activation::start_check(&w.game), None, "{what}");
    assert_eq!(
        speed(w, "sfmods/sfpack"),
        json!(7),
        "{what}: the player's value"
    );
    assert!(
        w.file("sfmods/wardrobe").is_dir(),
        "{what}: the EML package hosted ShroudForge runs"
    );
    assert_eq!(
        s.copies,
        BTreeMap::from([("players-own".to_owned(), None)]),
        "{what}: no copy of Kiln's left in <game>/mods"
    );
    let st = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
    assert_eq!(
        (st.requested, st.route, st.refused.is_none()),
        (false, "main", true),
        "{what}: {st:?}"
    );
    let row = sf_row(w);
    assert_ne!(row.plugin, "kiln-sfi", "{what}: the hosted row");
}

/// The game in kiln mode: the plan the profile pins, one, Kiln's copies (with the player's value) in `<game>/mods` and
/// none left in ShroudForge's folder.
fn assert_kiln(w: &World, kept: &Kept, what: &str) {
    kept.check(w, what);
    let s = GameState::of(w);
    let plan = pinned_plan(&w.game, &w.doc(Mode::Active), Mode::Active);
    assert_eq!(
        (
            plan["backend"]["selected"].as_str(),
            plan["backend"]["dataWriter"].as_str()
        ),
        (Some("kiln"), Some("kiln-eml")),
        "{what}"
    );
    assert_eq!(s.plans.len(), 1, "{what}: {:?}", s.plans);
    assert!(!s.journal, "{what}: a journal left");
    assert_eq!(sfi_activation::start_check(&w.game), None, "{what}");
    assert_eq!(
        speed(w, "mods/sfpack"),
        json!(7),
        "{what}: the player's value moved with the copy"
    );
    for id in ["sfpack", "wardrobe"] {
        assert!(
            !w.file(&format!("sfmods/{id}")).exists(),
            "{what}: {id} left in ShroudForge's folder"
        );
        assert!(s.copies.contains_key(id), "{what}: {id}: {:?}", s.copies);
    }
    let st = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
    assert_eq!(
        (st.requested, st.route, st.refused.is_none()),
        (true, "interpreter", true),
        "{what}: {st:?}"
    );
}

fn sf_row(w: &World) -> engine_mods::EngineStatus {
    engine_mods::status(&w.game)
        .into_iter()
        .find(|s| s.kind == ModKind::ShroudForge)
        .unwrap()
}

/// kiln-sfi's record of a kiln start that ran the ShroudForge mod.
// (ended: a running record while no game runs is unknown, sfi_state's stale rule)
const SFI_RUNNING: &[u8] = br#"{"format":1,"writer":"kiln-sfi","phase":"ended","launch":{"launchId":"00000000000000aa","pid":77,"backend":{"selected":"kiln"}},"updatedMs":5,"packages":[{"id":"sfpack","version":"1.0.0","intent":"selected","data":"committed","lua":"active","runtimeActive":true}]}"#;
const OWNERSHIP: &str = "kiln/eml/ownership/enshrouded.json";
const OWED: &str = "kiln/eml/ownership/enshrouded.sf-handoff-owed.json";

/// What a kiln start leaves in the game: kiln-sfi's record, kiln-eml's ownership receipt and the handoff it owes
/// ShroudForge (written ahead of its first change of the game data in kiln mode).
fn kiln_start_ran(w: &World) {
    put(&w.game, "kiln/state/sfi.json", SFI_RUNNING);
    put(
        &w.game,
        OWNERSHIP,
        br#"{"format":1,"stem":"enshrouded","note":"kiln-eml's receipt (synthetic)"}"#,
    );
    put(
        &w.game,
        OWED,
        br#"{"format":1,"stem":"enshrouded","text":"owed"}"#,
    );
}

fn bytes(w: &World, rel: &str) -> Option<Vec<u8>> {
    std::fs::read(w.file(rel)).ok()
}

#[test]
fn hosted_and_installed_to_kiln_and_back_keep_the_players_values_and_files() {
    for legacy in Legacy::ALL {
        let tag = format!("rt-{legacy:?}");
        let w = world(&tag, true);
        on_legacy(&w, legacy);
        let kept = Kept::of(&w);
        assert_legacy(&w, &kept, &format!("{tag}: before"));
        for round in 1..=2 {
            let what = format!("{tag} round {round}");
            // to Kiln's interpreter
            switch(&w, Backend::Kiln);
            let n = inspections(&w.game);
            let mut wire = Tripwire::arm(&w.file("shroudforge"));
            activate(&w, &what);
            assert_eq!(
                wire.events(),
                Vec::<String>::new(),
                "{what}: the switch to kiln read ShroudForge's folder"
            );
            assert_eq!(inspections(&w.game), n + 1, "{what}: one inspection");
            assert_kiln(&w, &kept, &format!("{what}: kiln"));
            wire.events(); // (the checks' own snapshot of ShroudForge's folder)
            // a Play keeps the plan and reads nothing below <game>/shroudforge
            profile::refresh_active(&w.dirs, &w.inst).unwrap();
            assert_eq!(
                inspections(&w.game),
                n + 1,
                "{what}: the Play kept the plan"
            );
            assert_eq!(
                wire.events(),
                Vec::<String>::new(),
                "{what}: the Play read ShroudForge's folder"
            );
            kiln_start_ran(&w);
            let row = sf_row(&w);
            assert_eq!(
                (
                    row.plugin.as_str(),
                    row.state.as_ref().map(|s| s["status"].clone())
                ),
                ("kiln-sfi", Some(json!("ended"))),
                "{what}: {row:?}"
            );
            assert_kiln(&w, &kept, &format!("{what}: kiln, after its start"));
            let (receipt, owed) = (bytes(&w, OWNERSHIP), bytes(&w, OWED));
            // back to the backend the player had
            switch(&w, legacy.back());
            let a = activate(&w, &what);
            assert_legacy(&w, &kept, &format!("{what}: back"));
            // ShroudForge's stale record is the game's to settle at its first hosted start (kiln-eml's stand_aside):
            // Kiln leaves the handoff it owes and kiln-eml's receipt as they are, and ShroudForge's record too
            assert_eq!(
                (bytes(&w, OWNERSHIP), bytes(&w, OWED)),
                (receipt.clone(), owed.clone()),
                "{what}: {a:?}"
            );
            profile::refresh_active(&w.dirs, &w.inst).unwrap();
            profile::deactivate(&w.dirs, &w.inst).unwrap();
            assert_eq!(
                (bytes(&w, OWNERSHIP), bytes(&w, OWED)),
                (receipt, owed),
                "{what}: a Play and a deactivation"
            );
            assert!(GameState::of(&w).plans.is_empty(), "{what}");
            kept.check(&w, &format!("{what}: deactivated"));
            activate(&w, &what);
            assert_legacy(&w, &kept, &format!("{what}: activated again"));
            let text = std::fs::read_to_string(w.file("kiln/kiln.json")).unwrap();
            assert!(!text.contains("useInstalled"), "{what}: one key: {text}");
        }
    }
}

/// The switch to kiln stopped at each boundary it passes, on each legacy backend; then the player goes back.
fn switch_matrix(legacy: Legacy) {
    let tag = format!("sw-{legacy:?}");
    let dry = world(&format!("{tag}-dry"), true);
    on_legacy(&dry, legacy);
    switch(&dry, Backend::Kiln);
    let (_, seen) = passed(|| activate(&dry, "dry run"));
    // (the copies move between the two folders inside the placements stage: the move has no boundary of its own)
    let order = first_passed(&seen);
    assert!(order.len() >= 9, "{tag}: {order:?}");
    let mut outcomes = Vec::new();
    for b in order {
        let what = format!("{tag} killed at {b:?}");
        let w = world(&format!("{tag}-{b:?}"), true);
        on_legacy(&w, legacy);
        let kept = Kept::of(&w);
        switch(&w, Backend::Kiln);
        assert!(run_child(&w, b, "activate"), "{what}: never reached");
        outcomes.push(format!(
            "{b:?}:{}",
            if w.file(sfi_activation::JOURNAL).exists() {
                "journal"
            } else {
                "none"
            }
        ));
        // the player goes back: main's activation settles the stopped switch before it changes a copy
        switch(&w, legacy.back());
        activate(&w, &what);
        assert_legacy(&w, &kept, &format!("{what}, then back"));
        assert_eq!(
            sfi_activation::recover(&w.dirs, &w.game)
                .map(|r| outcome(&r))
                .ok(),
            Some("nothing"),
            "{what}"
        );
        // kiln once more, and back again: whole each time
        switch(&w, Backend::Kiln);
        activate(&w, &what);
        assert_kiln(&w, &kept, &format!("{what}, then kiln again"));
        switch(&w, legacy.back());
        activate(&w, &what);
        assert_legacy(&w, &kept, &format!("{what}, then back again"));
    }
    eprintln!("{tag}: {}", outcomes.join(" "));
}

#[test]
fn a_switch_from_hosted_to_kiln_stopped_at_every_boundary_goes_back_to_hosted_whole() {
    switch_matrix(Legacy::Hosted);
}

#[test]
fn a_switch_from_installed_to_kiln_stopped_at_every_boundary_goes_back_to_installed_whole() {
    switch_matrix(Legacy::Installed);
}

// ---- the engine row and the support report in kiln mode ----

/// What a hosted start left: kiln-shroudforge's word (it hosted ShroudForge) and ShroudForge's own record of a failed
/// startup pass, with no later start recorded (no boot.json): under hosted ShroudForge that is the last start's.
fn hosted_failure_left(w: &World) {
    put(
        &w.game,
        "kiln/state/shroudforge.json",
        br#"{"mode":"hosted","source":"bundled","runtime":"running","pid":4242,"updated":1794308400}"#,
    );
    put(
        &w.game,
        "shroudforge/state.json",
        br#"{"assets":{"status":"failed","phase":"startup","target":"enshrouded","error":"stale failure of the hosted start","completedAt":1794308401,"mods":[]},"runtime":{"running":false,"updatedAt":1,"active":[]}}"#,
    );
}

fn report(w: &World) -> kiln_manage::report::Report {
    kiln_manage::report::build(&kiln_manage::report::ReportInput {
        install: Some(w.inst.clone()),
        dirs: Some(w.dirs.clone()),
        tool: "kiln-launcher test".into(),
        roots: Some(Vec::new()),
        deck: Some(false),
        redactor: Some(kiln_manage::report::Redactor::new(None, None, None)),
        ..Default::default()
    })
}

const STARTUP_FINDINGS: [&str; 2] = ["sf-startup-failed", "sf-startup-skipped"];

#[test]
fn in_kiln_mode_the_engine_rows_and_the_support_report_never_read_shroudforges_state() {
    let w = world("rows", true);
    on_legacy(&w, Legacy::Hosted);
    hosted_failure_left(&w);
    // under hosted ShroudForge the failed pass is said (the fixture is one the readers take)
    assert!(
        sf_row(&w).startup_failed.is_some(),
        "the control: {:?}",
        sf_row(&w)
    );
    let r = report(&w);
    assert!(
        r.findings.iter().any(|f| f.id == "sf-startup-failed"),
        "the control: {:?}",
        r.findings
    );
    // kiln mode, after a kiln start
    switch(&w, Backend::Kiln);
    activate(&w, "kiln");
    kiln_start_ran(&w);
    let mut wire = Tripwire::arm(&w.file("shroudforge"));
    let rows = engine_mods::status(&w.game);
    assert_eq!(
        wire.events(),
        Vec::<String>::new(),
        "the engine rows read ShroudForge's folder"
    );
    let r = report(&w);
    assert_eq!(
        wire.events(),
        Vec::<String>::new(),
        "the support report read ShroudForge's folder"
    );
    let sf = rows
        .iter()
        .find(|s| s.kind == ModKind::ShroudForge)
        .unwrap();
    assert_eq!(sf.plugin, "kiln-sfi", "{sf:?}");
    for row in &rows {
        assert!(
            row.startup_failed.is_none() && row.startup_skipped.is_none(),
            "{:?}: a hosted start's record on a kiln start's row: {row:?}",
            row.kind
        );
    }
    assert!(
        !r.findings
            .iter()
            .any(|f| STARTUP_FINDINGS.contains(&f.id.as_str())),
        "a hosted start's record in a kiln start's report: {:?}",
        r.findings
    );
    assert!(
        !r.summary
            .contains("ShroudForge's record of its startup pass"),
        "{}",
        r.summary
    );
    // back on hosted, ShroudForge's own record is read again (it is the hosted start's to settle)
    switch(&w, Backend::Hosted);
    activate(&w, "back");
    assert!(
        sf_row(&w).startup_failed.is_some(),
        "hosted again: {:?}",
        sf_row(&w)
    );
}
