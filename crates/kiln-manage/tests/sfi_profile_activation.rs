// SPDX-License-Identifier: MIT
//! Kiln-mode activation and refresh through `profile::activate` / `profile::refresh_active`: the
//! journal wired in main's order, the launch plan published for exactly the profile it pins, one decision
//! of the route per operation, a plan produced only when its inputs changed, refusals said, main's routes as
//! they were.
//!
//! - **Real process kills** of the wired path: a child (this binary again) runs `profile::activate` or
//!   `profile::refresh_active` and kills itself at each boundary it passes (recorded on a dry run of the same
//!   operation). The parent recovers twice: the earlier state whole, the new one whole, or the planned start refused
//!   when the copies changed; the second recovery changes nothing; a profile never pins a plan of another state; the
//!   next activation commits whole and leaves exactly the plan it pins.
//! - The plan: published and pinned (profile.json `sfLaunchPlan`), kept by a refresh while every input is the same
//!   (no inspection), renewed when one changed (a copy's files, kiln.json, a DLL in the game folder, the game itself, the
//!   Library), refused with its reason (no inspection answer, no interpreter), earlier ones removed.
//!
//! Every package here is synthetic.

mod sfi_profile_common;
mod sfi_routing_common;

use kiln_manage::manifest::ModKind;
use kiln_manage::profile;
use kiln_manage::sfi_activation::{self, Boundary};
use kiln_manage::sfi_plan::Mode;
use serde_json::Value;
use sfi_profile_common::*;
use sfi_routing_common::*;

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
        "refresh" => drop(profile::refresh_active(&w.dirs, &w.inst)),
        other => panic!("{other}"),
    });
}

fn activate(w: &World) -> profile::Activation {
    profile::activate(&w.dirs, &w.profile, &w.inst).unwrap_or_else(|e| panic!("{e:?}"))
}

fn pos(seen: &[Boundary], b: Boundary) -> usize {
    seen.iter()
        .position(|x| *x == b)
        .unwrap_or_else(|| panic!("{b:?} not passed: {seen:?}"))
}

// ---- the plan ----

#[test]
fn a_kiln_activation_publishes_the_plan_its_profile_pins_in_mains_order() {
    let w = world("publish", true);
    let (a, seen) = passed(|| activate(&w));
    assert_eq!(a.generation, 1);
    let doc = w.doc(Mode::Active);
    let plan = pinned_plan(&w.game, &doc, Mode::Active);
    assert_eq!(inspections(&w.game), 1, "one inspection per plan");
    assert_eq!(sfi_activation::start_check(&w.game), None);
    assert_eq!(w.plans().len(), 1);
    // the journal first, then game_files, chain.ini, the copies, the plan, the profile: each staged before it changes
    assert_eq!(seen[0], Boundary::JournalBegun, "{seen:?}");
    assert!(
        pos(&seen, Boundary::GameFilesStaged) < pos(&seen, Boundary::ChainStaged),
        "{seen:?}"
    );
    assert!(
        pos(&seen, Boundary::ChainWritten) < pos(&seen, Boundary::PlacementsStaged),
        "{seen:?}"
    );
    assert!(
        pos(&seen, Boundary::PlacementsDone) < pos(&seen, Boundary::PlanStaged),
        "{seen:?}"
    );
    assert!(
        pos(&seen, Boundary::PlanWritten) < pos(&seen, Boundary::ProfileStaged),
        "{seen:?}"
    );
    assert_eq!(seen.last(), Some(&Boundary::Committed), "{seen:?}");
    // the plan's data writer is kiln-eml: an EML package that needs ShroudForge's runtime stays kiln-eml's to apply
    assert_eq!(plan["backend"]["dataWriter"], "kiln-eml");
    let eml = doc["mods"]["eml"].as_array().unwrap();
    // (profile.json leaves out "enabled" for an entry that is on)
    assert!(
        eml.iter()
            .any(|m| m["id"] == "wardrobe" && m.get("enabled") != Some(&Value::Bool(false))),
        "{doc:#}"
    );
    assert!(doc.get("emlRuntimeBlocked").is_none(), "{doc:#}");
    let ids: Vec<&str> = plan["packages"]
        .as_array()
        .unwrap()
        .iter()
        .map(|p| p["id"].as_str().unwrap())
        .collect();
    assert!(
        ids.contains(&"sfpack") && ids.contains(&"wardrobe"),
        "{ids:?}"
    );
    // what the launcher reads before Play
    let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
    assert_eq!(
        (s.requested, s.route, &s.refused),
        (true, "interpreter", &None),
        "{s:?}"
    );
    assert_eq!(
        s.plan.as_ref().map(|p| p.path.as_str()),
        doc["sfLaunchPlan"]["path"].as_str()
    );
    assert_eq!(
        s.decisions.get("sfpack").map(String::as_str),
        Some("candidate"),
        "{s:?}"
    );
    let rec = profile::sfi::record(&w.dirs, &w.inst);
    assert_eq!(
        rec.active.as_ref().map(|r| r.outcome.as_str()),
        Some("written"),
        "{rec:?}"
    );
    // a second activation: a new plan for the new generation, the earlier one gone
    let before = w.plans();
    let a2 = activate(&w);
    assert_eq!(a2.generation, 2);
    let doc2 = w.doc(Mode::Active);
    pinned_plan(&w.game, &doc2, Mode::Active);
    assert_eq!(w.plans().len(), 1, "{:?}", w.plans());
    assert!(w.plans().is_disjoint(&before));
    assert_eq!(inspections(&w.game), 2);
}

#[test]
fn a_refresh_keeps_a_current_plan_and_reads_no_archive() {
    let w = world("keep", true);
    activate(&w);
    let bytes = std::fs::read(w.file("kiln/profile.json")).unwrap();
    let plans = w.plans();
    for _ in 0..2 {
        let (r, seen) = passed(|| profile::refresh_active(&w.dirs, &w.inst).unwrap());
        assert!(r.is_none_or(|a| a.generation == 1), "nothing new to report");
        assert_eq!(
            std::fs::read(w.file("kiln/profile.json")).unwrap(),
            bytes,
            "the profile is as it was"
        );
        assert_eq!(w.plans(), plans);
        assert_eq!(
            inspections(&w.game),
            1,
            "no inspection: the plan's inputs are the same"
        );
        assert!(!w.file(sfi_activation::JOURNAL).exists());
        // the reconcile ran inside the journal, which it released
        assert_eq!(seen.first(), Some(&Boundary::JournalBegun), "{seen:?}");
        assert_eq!(seen.last(), Some(&Boundary::Released), "{seen:?}");
        assert!(!seen.contains(&Boundary::PlanStaged), "{seen:?}");
    }
}

/// Each input a plan depends on, changed after the activation: the next refresh makes a new plan (one inspection) for
/// the next generation and removes the earlier one; the one after keeps it.
#[test]
fn a_refresh_renews_the_plan_when_one_of_its_inputs_changed() {
    type Change = fn(&World);
    let cases: [(&str, Change); 6] = [
        ("a file the mod wrote into its copy", |w| {
            put(&w.game, "mods/sfpack/notes.txt", b"the mod wrote this")
        }),
        ("a setting of the copy", |w| {
            kiln_manage::shroudforge::set_setting(
                &w.file("mods/sfpack"),
                "speed",
                &serde_json::json!(7),
            )
            .unwrap();
        }),
        ("kiln.json's native policy", |w| {
            put(&w.game, "kiln/kiln.json", br#"{"preview": ["kiln-sfi.interpreter"], "shroudforge": {"backend": "kiln"}, "native": {"policy": "never"}}"#)
        }),
        ("a DLL in the game folder", |w| {
            put(
                &w.game,
                "winmm.dll",
                &kiln_manage::pe::build_test_dll(&[], b"a player's proxy"),
            )
        }),
        ("the game (its executable)", |w| {
            put(&w.game, "enshrouded.exe", b"MZ a newer build of the game")
        }),
        ("a player's line in chain.ini", |w| {
            let text = String::from_utf8(std::fs::read(w.file("kiln/chain.ini")).unwrap()).unwrap();
            assert!(text.contains("player.dll\r\n"), "{text}");
            put(
                &w.game,
                "kiln/chain.ini",
                text.replacen("player.dll\r\n", "player.dll\r\nplayer2.dll\r\n", 1)
                    .as_bytes(),
            );
        }),
    ];
    for (what, change) in cases {
        let w = world("renew", false);
        activate(&w);
        let before = w.plans();
        change(&w);
        let a = profile::refresh_active(&w.dirs, &w.inst)
            .unwrap_or_else(|e| panic!("{what}: {e:?}"))
            .unwrap_or_else(|| panic!("{what}: a new activation to report"));
        assert_eq!(a.generation, 2, "{what}");
        let doc = w.doc(Mode::Active);
        assert_eq!(doc["generation"], 2, "{what}");
        pinned_plan(&w.game, &doc, Mode::Active);
        assert_eq!(inspections(&w.game), 2, "{what}: one more inspection");
        assert_eq!(w.plans().len(), 1, "{what}: the earlier plan is gone");
        assert!(w.plans().is_disjoint(&before), "{what}");
        assert_eq!(sfi_activation::start_check(&w.game), None, "{what}");
        // the new plan is current: the next Play keeps it
        assert!(
            profile::refresh_active(&w.dirs, &w.inst)
                .unwrap()
                .is_none_or(|a| a.generation == 2),
            "{what}"
        );
        assert_eq!(inspections(&w.game), 2, "{what}: kept");
    }
}

/// The archive files the inspection read are watched by size and time: one rewritten in place with the same size (a newer
/// time, as a game update leaves it) makes a new plan.
#[test]
fn a_refresh_renews_the_plan_when_an_inspected_archive_file_was_rewritten() {
    let w = world("archive-time", false);
    put(&w.game, "enshrouded.kfc", b"kfc-1");
    activate(&w);
    let f = w.file("enshrouded.kfc");
    std::fs::write(&f, b"kfc-2").unwrap();
    let later =
        std::fs::metadata(&f).unwrap().modified().unwrap() + std::time::Duration::from_secs(5);
    std::fs::File::options()
        .write(true)
        .open(&f)
        .unwrap()
        .set_modified(later)
        .unwrap();
    let a = profile::refresh_active(&w.dirs, &w.inst)
        .unwrap()
        .expect("a new plan");
    assert_eq!(a.generation, 2);
    assert_eq!(inspections(&w.game), 2, "one more inspection");
    pinned_plan(&w.game, &w.doc(Mode::Active), Mode::Active);
}

/// Another version in the Library (the profile renders to another file): activated again, with a new plan.
#[test]
fn a_library_change_activates_again_with_a_new_plan() {
    let w = world("library", false);
    activate(&w);
    w.select(1);
    let a = profile::refresh_active(&w.dirs, &w.inst)
        .unwrap()
        .expect("activated again");
    assert_eq!(a.generation, 2);
    let doc = w.doc(Mode::Active);
    let plan = pinned_plan(&w.game, &doc, Mode::Active);
    let sf = plan["packages"]
        .as_array()
        .unwrap()
        .iter()
        .find(|p| p["id"] == "sfpack")
        .unwrap()
        .clone();
    assert_eq!(sf["version"], "2.0.0", "{sf:#}");
    assert_eq!(
        kiln_manage::engine_mods::marker(&w.file("mods/sfpack"))
            .unwrap()
            .version,
        "2.0.0"
    );
    assert_eq!(inspections(&w.game), 2);
    assert_eq!(w.plans().len(), 1);
}

/// An error inside the journaled activation (chain.ini cannot be read: a folder there) settles it at once: the earlier
/// game-folder files, copies, profile and plan, whole, no journal left, the planned start as before.
#[test]
fn an_error_inside_a_kiln_activation_rolls_it_back_whole() {
    let w = world("error", false);
    activate(&w);
    w.select(1);
    std::fs::remove_file(w.file("kiln/chain.ini")).unwrap();
    std::fs::create_dir_all(w.file("kiln/chain.ini")).unwrap();
    let before = GameState::of(&w);
    let (e, seen) = passed(|| profile::activate(&w.dirs, &w.profile, &w.inst).unwrap_err());
    assert!(
        seen.contains(&Boundary::GameFilesStaged) && !seen.contains(&Boundary::PlanStaged),
        "{e:?} {seen:?}"
    );
    let now = GameState::of(&w);
    assert_eq!(now, before, "{e:?}");
    assert_eq!(sfi_activation::start_check(&w.game), None);
    pinned_plan(&w.game, &w.doc(Mode::Active), Mode::Active);
    assert_eq!(
        kiln_manage::engine_mods::marker(&w.file("mods/sfpack"))
            .unwrap()
            .version,
        "1.0.0"
    );
}

// ---- refusals ----

/// An inspection that gives no answer: the profile is committed without a plan, the EML package that needs
/// ShroudForge's runtime switched off for kiln-eml as main switches it off while ShroudForge carries it (no data-only
/// half: the core refuses the start of ShroudForge packages without a plan), and the launcher reads why. Once the
/// inspector answers, the next Play makes the plan.
#[test]
fn a_refused_inspection_commits_the_profile_without_a_plan_and_says_why() {
    let w = world("refused", true);
    refuse_inspection(&w.game, true);
    let a = activate(&w);
    assert_eq!(a.generation, 1);
    let doc = w.doc(Mode::Active);
    assert!(doc.get("sfLaunchPlan").is_none(), "{doc:#}");
    assert!(w.plans().is_empty());
    let eml = doc["mods"]["eml"].as_array().unwrap();
    assert!(
        eml.iter()
            .any(|m| m["id"] == "wardrobe" && m["enabled"] == false),
        "{doc:#}"
    );
    let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
    let r = s.refused.clone().unwrap_or_else(|| panic!("{s:?}"));
    assert_eq!(r.code, "no-archive-base", "{s:?}");
    assert!(
        r.reason
            .contains("the stand-in inspector refuses this game"),
        "{s:?}"
    );
    // the inspector answers again and an input changed (the game's archive): the next Play makes the plan (no plan is no
    // current plan; with the same inputs the refusal is answered again: a_refused_plan_is_retried_only_when_...)
    refuse_inspection(&w.game, false);
    put(&w.game, "enshrouded.kfc", b"kfc-1");
    let a = profile::refresh_active(&w.dirs, &w.inst)
        .unwrap()
        .expect("a new activation");
    assert_eq!(a.generation, 2);
    pinned_plan(&w.game, &w.doc(Mode::Active), Mode::Active);
    assert_eq!(
        profile::sfi::status(&w.dirs, &w.inst, Mode::Active).refused,
        None
    );
    // an out-of-date plan with an inspector that refuses: the refresh keeps the plan the profile pins (kiln-sfi
    // withdraws a package whose files changed) and says why there is no newer one
    let pinned = w.doc(Mode::Active)["sfLaunchPlan"].clone();
    put(&w.game, "mods/sfpack/notes.txt", b"changed");
    refuse_inspection(&w.game, true);
    profile::refresh_active(&w.dirs, &w.inst).unwrap();
    assert_eq!(w.doc(Mode::Active)["sfLaunchPlan"], pinned, "kept");
    assert_eq!(w.doc(Mode::Active)["generation"], 2);
    assert!(!w.file(sfi_activation::JOURNAL).exists());
    let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
    assert!(
        s.refused.is_none() && s.warnings.iter().any(|n| n.code == "no-archive-base"),
        "{s:?}"
    );
    refuse_inspection(&w.game, false);
}

/// A refused plan is retried only when one of its inputs changed: with the same inputs a refresh answers the recorded
/// refusal and runs no inspection (the check before Play refreshes twice per Play), the profile file as it was and the
/// launcher told why; each input changed retries once; an explicit activation always produces again.
#[test]
fn a_refused_plan_is_retried_only_when_one_of_its_inputs_changed() {
    let w = world("refused-again", false);
    refuse_inspection(&w.game, true);
    activate(&w);
    assert_eq!(inspections(&w.game), 1);
    let refused = |what: &str| {
        let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
        let r = s.refused.clone().unwrap_or_else(|| panic!("{what}: {s:?}"));
        assert_eq!(r.code, "no-archive-base", "{what}: {s:?}");
        assert!(
            r.reason
                .contains("the stand-in inspector refuses this game"),
            "{what}: {s:?}"
        );
        assert!(!w.file(sfi_activation::JOURNAL).exists(), "{what}");
        assert!(w.doc(Mode::Active).get("sfLaunchPlan").is_none(), "{what}");
    };
    let bytes = std::fs::read(w.file("kiln/profile.json")).unwrap();
    for i in 0..3 {
        profile::refresh_active(&w.dirs, &w.inst).unwrap();
        assert_eq!(
            inspections(&w.game),
            1,
            "Play {i}: the refusal is answered again, nothing inspected"
        );
        assert_eq!(std::fs::read(w.file("kiln/profile.json")).unwrap(), bytes);
        refused("the same inputs");
    }
    type Change = fn(&World);
    let cases: [(&str, Change); 7] = [
        ("a file the mod wrote into its copy", |w| {
            put(&w.game, "mods/sfpack/notes.txt", b"the mod wrote this")
        }),
        ("kiln.json's native policy", |w| {
            put(&w.game, "kiln/kiln.json", br#"{"preview": ["kiln-sfi.interpreter"], "shroudforge": {"backend": "kiln"}, "native": {"policy": "never"}}"#)
        }),
        ("a DLL in the game folder", |w| {
            put(
                &w.game,
                "winmm.dll",
                &kiln_manage::pe::build_test_dll(&[], b"a player's proxy"),
            )
        }),
        ("the game's archive", |w| {
            put(&w.game, "enshrouded.kfc", b"kfc-2")
        }),
        ("kiln-eml's ownership receipt", |w| {
            put(&w.game, "kiln/eml/ownership/enshrouded.json", b"{}")
        }),
        ("the archive inspector this Kiln installed", |w| {
            put(
                &w.game,
                &format!(
                    "{}/windows-x64/kiln-eml-inspect.exe",
                    kiln_manage::sfi_inspection::INSTALL_DIR
                ),
                b"another build of the helper",
            )
        }),
        ("a player's line in chain.ini", |w| {
            let text = String::from_utf8(std::fs::read(w.file("kiln/chain.ini")).unwrap()).unwrap();
            assert!(text.contains("player.dll\r\n"), "{text}");
            put(
                &w.game,
                "kiln/chain.ini",
                text.replacen("player.dll\r\n", "player.dll\r\nplayer2.dll\r\n", 1)
                    .as_bytes(),
            );
        }),
    ];
    let mut n = 1;
    for (what, change) in cases {
        change(&w);
        profile::refresh_active(&w.dirs, &w.inst).unwrap_or_else(|e| panic!("{what}: {e:?}"));
        n += 1;
        assert_eq!(inspections(&w.game), n, "{what}: tried again once");
        refused(what);
        profile::refresh_active(&w.dirs, &w.inst).unwrap();
        assert_eq!(inspections(&w.game), n, "{what}: then answered again");
    }
    // an explicit activation always tries again, and its refusal is answered by the next Play
    activate(&w);
    assert_eq!(inspections(&w.game), n + 1, "an activation produces");
    profile::refresh_active(&w.dirs, &w.inst).unwrap();
    assert_eq!(inspections(&w.game), n + 1);
    refused("after the activation");
    // the inspector answers and an input changes: the plan is made, and kept
    refuse_inspection(&w.game, false);
    put(&w.game, "mods/sfpack/notes.txt", b"and this");
    profile::refresh_active(&w.dirs, &w.inst)
        .unwrap()
        .expect("a new activation");
    assert_eq!(inspections(&w.game), n + 2);
    pinned_plan(&w.game, &w.doc(Mode::Active), Mode::Active);
    profile::refresh_active(&w.dirs, &w.inst).unwrap();
    assert_eq!(inspections(&w.game), n + 2);
}

/// A refusal is answered again only for the profile file it was recorded for: a record older than the profile file (an
/// activation that made a plan and stopped before it wrote its record, which then still says refused) is no answer, and
/// the next Play produces the plan again instead of answering a refusal for good.
#[test]
fn a_refusal_recorded_before_the_profile_file_changed_is_not_answered_again() {
    let w = world("stale-refusal", false);
    refuse_inspection(&w.game, true);
    activate(&w);
    let rec = w.dirs.installs().join(format!("{}.sfi.json", w.inst.id));
    let refused = std::fs::read(&rec).unwrap();
    refuse_inspection(&w.game, false);
    activate(&w);
    assert_eq!(inspections(&w.game), 2);
    pinned_plan(&w.game, &w.doc(Mode::Active), Mode::Active);
    // the activation's own record lost: the one before it, a refusal of the same inputs, is what is there
    std::fs::write(&rec, &refused).unwrap();
    profile::refresh_active(&w.dirs, &w.inst).unwrap();
    assert_eq!(inspections(&w.game), 3, "produced, not answered");
    let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
    assert!(
        s.refused.is_none() && !s.warnings.iter().any(|n| n.code == "no-archive-base"),
        "{s:?}"
    );
    pinned_plan(&w.game, &w.doc(Mode::Active), Mode::Active);
    profile::refresh_active(&w.dirs, &w.inst).unwrap();
    assert_eq!(inspections(&w.game), 3, "and kept");
}

/// A plan that cannot be written (the plans folder is no folder) is refused like any other plan: the activation commits
/// the profile without one and the launcher reads `write-failed`. That refusal is no answer of the plan's inputs, which
/// did not change: every Play tries again, and once the folder is back the plan is made (and kept).
#[test]
fn a_plan_that_cannot_be_written_is_refused_and_tried_again_at_every_play() {
    let w = world("write-failed", false);
    block_plans(&w);
    activate(&w);
    assert_eq!(inspections(&w.game), 1);
    let write_failed = |what: &str| {
        let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
        assert_eq!(
            s.refused.as_ref().map(|r| r.code.as_str()),
            Some("write-failed"),
            "{what}: {s:?}"
        );
        assert!(w.doc(Mode::Active).get("sfLaunchPlan").is_none(), "{what}");
        assert!(!w.file(sfi_activation::JOURNAL).exists(), "{what}");
    };
    write_failed("the activation");
    for i in 0..2 {
        profile::refresh_active(&w.dirs, &w.inst).unwrap_or_else(|e| panic!("Play {i}: {e:?}"));
        assert_eq!(inspections(&w.game), 2 + i, "Play {i}: tried again");
        write_failed("a Play");
    }
    unblock_plans(&w);
    profile::refresh_active(&w.dirs, &w.inst)
        .unwrap()
        .expect("a new activation");
    assert_eq!(inspections(&w.game), 4);
    pinned_plan(&w.game, &w.doc(Mode::Active), Mode::Active);
    profile::refresh_active(&w.dirs, &w.inst).unwrap();
    assert_eq!(inspections(&w.game), 4, "and kept");
}

/// A refresh that must renew its plan and cannot write the new one does not fail Play ("an activation that published a
/// plan is committed, not released"): the journal forgets the plan it could not write, the refresh is released, and the
/// profile keeps what it pins. The launcher reads `write-failed` beside the kept plan; the next Play tries again.
#[test]
fn a_renewal_whose_plan_cannot_be_written_keeps_the_pinned_plan_and_says_why() {
    let w = world("renew-write-failed", false);
    activate(&w);
    let pinned = w.doc(Mode::Active)["sfLaunchPlan"].clone();
    let generation = w.doc(Mode::Active)["generation"].clone();
    put(&w.game, "mods/sfpack/notes.txt", b"changed");
    block_plans(&w);
    profile::refresh_active(&w.dirs, &w.inst).unwrap_or_else(|e| panic!("Play fails: {e:?}"));
    assert_eq!(inspections(&w.game), 2);
    assert!(!w.file(sfi_activation::JOURNAL).exists());
    assert_eq!(w.doc(Mode::Active)["sfLaunchPlan"], pinned, "kept");
    assert_eq!(w.doc(Mode::Active)["generation"], generation);
    let r = profile::sfi::record(&w.dirs, &w.inst).active.unwrap();
    assert_eq!(
        (r.outcome.as_str(), r.code.as_deref(), r.inputs.as_deref()),
        ("refused", Some("write-failed"), None),
        "{r:?}"
    );
    unblock_plans(&w);
    let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
    assert!(
        s.refused.is_none() && s.warnings.iter().any(|n| n.code == "write-failed"),
        "{s:?}"
    );
    // no input changed since, and the next Play renews the plan
    profile::refresh_active(&w.dirs, &w.inst)
        .unwrap()
        .expect("a new activation");
    assert_eq!(inspections(&w.game), 3);
    let doc = w.doc(Mode::Active);
    assert_ne!(doc["sfLaunchPlan"], pinned);
    pinned_plan(&w.game, &doc, Mode::Active);
}

/// A refused renewal of a pinned plan is answered again the same way: the profile keeps its pin, the launcher reads the
/// refusal as a warning, and only a changed input inspects again.
#[test]
fn a_refused_renewal_is_answered_again_until_an_input_changes() {
    let w = world("renew-refused-again", false);
    activate(&w);
    let pinned = w.doc(Mode::Active)["sfLaunchPlan"].clone();
    put(&w.game, "mods/sfpack/notes.txt", b"changed");
    refuse_inspection(&w.game, true);
    for i in 0..3 {
        profile::refresh_active(&w.dirs, &w.inst).unwrap();
        assert_eq!(inspections(&w.game), 2, "Play {i}: one inspection in all");
        assert_eq!(w.doc(Mode::Active)["sfLaunchPlan"], pinned, "kept");
        let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
        assert!(
            s.refused.is_none() && s.warnings.iter().any(|n| n.code == "no-archive-base"),
            "{s:?}"
        );
    }
    put(&w.game, "mods/sfpack/notes.txt", b"changed again");
    profile::refresh_active(&w.dirs, &w.inst).unwrap();
    assert_eq!(inspections(&w.game), 3);
    refuse_inspection(&w.game, false);
}

/// A refresh whose reconcile places a copy only now (the file names the Library entry, as an older Kiln's file does)
/// renders the profile otherwise. When no new plan can be made then, the profile is committed without one: never with the
/// plan the earlier file pinned, which was made for another state of the profile (the commit refuses that, and the
/// activation would be left refused with the copy placed).
#[test]
fn a_refresh_placing_a_copy_only_now_commits_without_the_earlier_plan_when_none_can_be_made() {
    let w = world("late-copy", false);
    // a Greeneye mod: it runs from Kiln's copy in <game>/greeneye/ once placed, else the file names the Library entry
    let ge = import(
        &w.dirs,
        &other_packages(&w.root)[0],
        ModKind::Greeneye,
        "worstmap",
    );
    let mut p = profile::get(&w.dirs, &w.profile).unwrap();
    p.mods.greeneye = vec![pm(&ge, true)];
    profile::save(&w.dirs, &p).unwrap();
    activate(&w);
    assert!(w.doc(Mode::Active)["sfLaunchPlan"].is_object());
    // the copy and its receipt gone, and the file naming what the profile renders to without them (the Library entry)
    std::fs::remove_dir_all(w.file("greeneye/worstmap")).unwrap();
    let receipts = w.file("kiln/state/library-placements.json");
    let mut r: Value = serde_json::from_slice(&std::fs::read(&receipts).unwrap()).unwrap();
    r.as_array_mut()
        .unwrap()
        .retain(|e| e["marker"]["id"] != "worstmap");
    std::fs::write(&receipts, serde_json::to_vec_pretty(&r).unwrap()).unwrap();
    let file = profile::read_game_profile(&w.game).unwrap().unwrap();
    let p = profile::get(&w.dirs, &w.profile).unwrap();
    let library = profile::render(&w.dirs, &p, &w.inst, file.path_mode.unwrap()).unwrap();
    assert_ne!(library.mods, file.mods, "the file named the copy");
    let mut doc = w.doc(Mode::Active);
    doc["mods"] = serde_json::to_value(&library.mods).unwrap();
    put(
        &w.game,
        "kiln/profile.json",
        serde_json::to_string_pretty(&doc).unwrap().as_bytes(),
    );
    refuse_inspection(&w.game, true);
    let r = profile::refresh_active(&w.dirs, &w.inst);
    refuse_inspection(&w.game, false);
    let a = r
        .unwrap_or_else(|e| panic!("{e:?}"))
        .expect("the profile renders otherwise now: activated");
    assert_eq!(a.generation, 2);
    let doc = w.doc(Mode::Active);
    assert_eq!(doc["generation"], 2, "{doc:#}");
    assert!(doc.get("sfLaunchPlan").is_none(), "{doc:#}");
    assert!(
        kiln_manage::engine_mods::marker(&w.file("greeneye/worstmap")).is_some(),
        "the copy is placed"
    );
    assert!(!w.file(sfi_activation::JOURNAL).exists());
    assert_eq!(sfi_activation::start_check(&w.game), None);
    assert!(
        w.plans().is_empty(),
        "the earlier plan is gone: {:?}",
        w.plans()
    );
    let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
    assert_eq!(
        s.refused.as_ref().map(|r| r.code.as_str()),
        Some("no-archive-base"),
        "{s:?}"
    );
}

/// Without kiln-sfi.dll the interpreter cannot run a start: no plan and no inspection; the EML package that needs
/// ShroudForge's runtime keeps main's blocked note; the launcher reads why.
#[test]
fn without_the_interpreter_no_plan_is_written_and_mains_note_stays() {
    let w = world("no-interpreter", true);
    std::fs::remove_file(w.file("kiln/plugins/kiln-sfi.dll")).unwrap();
    let a = activate(&w);
    assert_eq!(a.generation, 1);
    assert_eq!(inspections(&w.game), 0, "nothing to inspect for");
    let doc = w.doc(Mode::Active);
    assert!(
        doc.get("sfLaunchPlan").is_none() && w.plans().is_empty(),
        "{doc:#}"
    );
    let note = doc["emlRuntimeBlocked"]["wardrobe"]
        .as_str()
        .unwrap_or_else(|| panic!("{doc:#}"));
    assert!(
        note.starts_with("needs ShroudForge's runtime, which is not running"),
        "main's note: {note}"
    );
    let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
    assert_eq!(
        (s.route, s.refused.as_ref().map(|r| r.code.as_str())),
        ("interpreter-missing", Some("interpreter-missing")),
        "{s:?}"
    );
    let rec = profile::sfi::record(&w.dirs, &w.inst).active.unwrap();
    assert_eq!(rec.code.as_deref(), Some("interpreter-missing"), "{rec:?}");
}

/// A held `kiln` keeps main's route byte for byte (no journal, no lock file, no plan), and the launcher reads why. Held
/// only while this build's gate list holds the interpreter; without its row `kiln` with no "preview" is a normal choice.
#[test]
fn a_held_kiln_keeps_mains_route_and_says_why() {
    let w = world("held", false);
    select(&w.game, Some(KILN_HELD));
    activate(&w);
    if !interpreter_is_a_preview() {
        let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
        assert!(s.requested, "{s:?}");
        assert_ne!(s.refused.as_ref().map(|r| r.code.as_str()), Some("interpreter-held"), "{s:?}");
        return;
    }
    assert!(
        !w.file("kiln/state/sfi-activation.lock").exists(),
        "main's route takes no journal lock"
    );
    assert!(!w.file("kiln/launch-plans").exists());
    assert!(w.doc(Mode::Active).get("sfLaunchPlan").is_none());
    assert_eq!(inspections(&w.game), 0);
    let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Active);
    assert_eq!(
        (s.route, s.refused.as_ref().map(|r| r.code.as_str())),
        ("main", Some("interpreter-held")),
        "{s:?}"
    );
    select(&w.game, Some(HOSTED));
    assert_eq!(
        profile::sfi::status(&w.dirs, &w.inst, Mode::Active),
        profile::sfi::Status {
            requested: false,
            route: "main",
            refused: None,
            plan: None,
            warnings: Vec::new(),
            decisions: Default::default()
        }
    );
}

/// A hosted activation after kiln mode: profile.json pins no plan, the plans go, and a refused journal is superseded.
#[test]
fn a_hosted_activation_after_kiln_mode_removes_the_plans_and_a_refused_journal() {
    let w = world("to-hosted", false);
    activate(&w);
    w.select(1);
    assert!(run_child(&w, Boundary::PlanStaged, "activate"), "reached");
    assert_eq!(outcome(&recover_twice(&w, "to hosted")), "refused");
    assert!(sfi_activation::start_check(&w.game).is_some());
    select(&w.game, Some(HOSTED));
    let a = activate(&w);
    assert_eq!(a.generation, 2);
    assert!(w.doc(Mode::Active).get("sfLaunchPlan").is_none());
    assert!(w.plans().is_empty(), "{:?}", w.plans());
    assert!(!w.file(sfi_activation::JOURNAL).exists(), "superseded");
    assert_eq!(sfi_activation::start_check(&w.game), None);
    assert_eq!(profile::sfi::record(&w.dirs, &w.inst), Default::default());
}

/// A main-route activation settles an unfinished kiln-mode activation before it changes a copy: a first kiln activation
/// stopped right after it published a first copy (its receipt not written yet) is rolled back whole, that copy taken
/// out, before main's placement could take it for the player's own folder; no journal is left.
#[test]
fn a_hosted_activation_settles_an_unfinished_kiln_activation_first() {
    let w = world("hosted-settles", false);
    assert!(
        run_child(&w, Boundary::PlacementPublished, "activate"),
        "reached"
    );
    assert!(w.file(sfi_activation::JOURNAL).exists());
    select(&w.game, Some(HOSTED));
    let a = activate(&w);
    assert!(
        a.warnings.iter().any(|n| n.contains("did not finish")),
        "the recovery is said: {:#?}",
        a.warnings
    );
    assert!(!w.file(sfi_activation::JOURNAL).exists());
    assert!(w.plans().is_empty(), "{:?}", w.plans());
    // Kiln's copy, with its receipt: placed by the hosted activation, not left as a folder of the player's
    let receipts: Value = serde_json::from_slice(
        &std::fs::read(w.file("kiln/state/library-placements.json")).unwrap(),
    )
    .unwrap();
    assert!(
        receipts
            .as_array()
            .unwrap()
            .iter()
            .any(|r| r["marker"]["id"] == "sfpack"),
        "{receipts:#}"
    );
    assert!(a.retained.is_empty(), "{:#?}", a.retained);
}

/// A deactivation settles an unfinished kiln-mode activation before it removes a copy: the first copy published before
/// its receipt is taken out with the rest, not left behind as a folder of the player's; no journal and no plan stay.
#[test]
fn a_deactivation_settles_an_unfinished_kiln_activation_first() {
    let w = world("deactivate-settles", false);
    assert!(
        run_child(&w, Boundary::PlacementPublished, "activate"),
        "reached"
    );
    assert!(w.file("mods/sfpack").exists() && w.file(sfi_activation::JOURNAL).exists());
    profile::deactivate_report(&w.dirs, &w.inst).unwrap();
    assert!(!w.file("mods/sfpack").exists(), "Kiln's copy is gone");
    assert!(!w.file(sfi_activation::JOURNAL).exists());
    assert!(w.plans().is_empty(), "{:?}", w.plans());
    assert_eq!(
        std::fs::read(w.file("mods/players-own/mod.json")).unwrap(),
        PLAYER_SF
    );
}

/// main's safety notes of a reconcile (a player's own folder that still runs although its mod is off) come through a
/// kiln-mode refresh as through a hosted one.
#[test]
fn a_kiln_refresh_carries_the_safety_notes_of_its_reconcile() {
    let mut notes = Vec::new();
    for json in [Some(KILN), None] {
        let w = world("safety", false);
        select(&w.game, json);
        let own = import(
            &w.dirs,
            &{
                let d = w.root.join("src-own/players-own");
                put(&d, "mod.json", PLAYER_SF);
                put(
                    &d,
                    "extended.mod.json",
                    br#"{"schemaVersion":1,"launcher":"SF","enabled":false,"settings":{}}"#,
                );
                d
            },
            ModKind::ShroudForge,
            "players-own",
        );
        let mut p = profile::get(&w.dirs, &w.profile).unwrap();
        p.mods.shroudforge.push(pm(&own, false));
        profile::save(&w.dirs, &p).unwrap();
        activate(&w);
        let a = profile::refresh_active(&w.dirs, &w.inst)
            .unwrap()
            .expect("the safety note is reported");
        assert!(!a.safety_notes.is_empty(), "{a:#?}");
        notes.push(
            a.safety_notes
                .iter()
                .map(|n| normal(n, &w.root))
                .collect::<Vec<_>>(),
        );
    }
    assert_eq!(notes[0], notes[1], "kiln and hosted say the same");
}

// ---- real kills of the wired path ----

/// Every boundary `op` passes on a world `setup` makes, killed in turn: recovered whole, the next activation commits.
fn kill_matrix(tag: &str, setup: fn(&World), op: &str, at_least: usize) {
    let dry = world(&format!("{tag}-dry"), true);
    setup(&dry);
    let (_, seen) = passed(|| match op {
        "activate" => drop(profile::activate(&dry.dirs, &dry.profile, &dry.inst).unwrap()),
        _ => drop(profile::refresh_active(&dry.dirs, &dry.inst).unwrap()),
    });
    let order = first_passed(&seen);
    assert!(order.len() >= at_least, "{tag}: {order:?}");
    let mut outcomes = Vec::new();
    for b in order {
        let what = format!("{tag} killed at {b:?}");
        let w = world(&format!("{tag}-{b:?}"), true);
        setup(&w);
        let before = GameState::of(&w);
        assert!(run_child(&w, b, op), "{what}: never reached");
        let r = recover_twice(&w, &what);
        assert_settled(&w, &before, &r, Mode::Active, &what);
        match b {
            Boundary::ProfileWritten => assert_eq!(outcome(&r), "completed", "{what}"),
            Boundary::Committed | Boundary::Released => {
                assert_eq!(outcome(&r), "nothing", "{what}")
            }
            Boundary::JournalBegun | Boundary::GameFilesStaged => {
                assert_eq!(outcome(&r), "rolled-back", "{what}")
            }
            _ => assert_ne!(outcome(&r), "nothing", "{what}"),
        }
        outcomes.push(format!("{b:?}={}", outcome(&r)));
        // the next activation is the explicit recovery: whole, exactly the plan it pins
        let a = profile::activate(&w.dirs, &w.profile, &w.inst)
            .unwrap_or_else(|e| panic!("{what}: activating again: {e:?}"));
        assert_eq!(a.generation, GameState::of(&w).generation(), "{what}");
        assert_eq!(sfi_activation::start_check(&w.game), None, "{what}");
        pinned_plan(&w.game, &w.doc(Mode::Active), Mode::Active);
        assert_eq!(w.plans().len(), 1, "{what}: {:?}", w.plans());
        assert!(!w.file(sfi_activation::JOURNAL).exists(), "{what}");
    }
    eprintln!("{tag}: {}", outcomes.join(" "));
}

#[test]
fn a_first_kiln_activation_killed_at_every_boundary_recovers_whole() {
    kill_matrix("first", |_| {}, "activate", 15);
}

#[test]
fn a_replacement_kiln_activation_killed_at_every_boundary_recovers_whole() {
    kill_matrix(
        "repl",
        |w| {
            activate(w);
            w.select(1);
        },
        "activate",
        15,
    );
}

/// A refresh that renews an out-of-date plan (the copy's files changed), killed at each boundary.
#[test]
fn a_refresh_renewing_its_plan_killed_at_every_boundary_recovers_whole() {
    kill_matrix(
        "renew",
        |w| {
            activate(w);
            put(&w.game, "mods/sfpack/notes.txt", b"the mod wrote this");
        },
        "refresh",
        9,
    );
}

/// A refresh that keeps its plan (nothing changed), killed at each boundary: the earlier state, always.
#[test]
fn a_refresh_keeping_its_plan_killed_at_every_boundary_recovers_whole() {
    kill_matrix(
        "keep",
        |w| {
            activate(w);
        },
        "refresh",
        5,
    );
}

/// Kiln uninstalled over `op` stopped at each boundary it passes (on a world `setup` makes): the uninstall settles the
/// unfinished activation before it removes a copy, exactly as the next activation would (a twin world recovered first,
/// then uninstalled, is left the same), so no game file stays half placed.
fn uninstall_matrix(tag: &str, setup: fn(&World), op: &str, at_least: usize) {
    let dry = world(&format!("{tag}-udry"), true);
    setup(&dry);
    let (_, seen) = passed(|| match op {
        "activate" => drop(profile::activate(&dry.dirs, &dry.profile, &dry.inst).unwrap()),
        _ => drop(profile::refresh_active(&dry.dirs, &dry.inst).unwrap()),
    });
    let order = first_passed(&seen);
    assert!(order.len() >= at_least, "{tag}: {order:?}");
    let mut outcomes = Vec::new();
    for b in order {
        let what = format!("{tag} killed at {b:?}, then Kiln uninstalled");
        let [a, t] = ["u", "t"].map(|side| {
            let w = world(&format!("{tag}-{side}-{b:?}"), true);
            setup(&w);
            kiln_receipt(&w);
            w
        });
        let before = GameState::of(&a);
        assert!(run_child(&a, b, op), "{what}: never reached");
        assert!(run_child(&t, b, op), "{what}: never reached (twin)");
        let r = uninstall_settles_as_the_next_activation(&a, &t, &before, &what);
        outcomes.push(format!("{b:?}={}{:?}", outcome(&r), hidden(&a)));
    }
    eprintln!("uninstall {tag}: {}", outcomes.join(" "));
}

#[test]
fn uninstalling_over_a_first_kiln_activation_killed_at_every_boundary_settles_it_first() {
    uninstall_matrix("ufirst", |_| {}, "activate", 15);
}

#[test]
fn uninstalling_over_a_replacement_kiln_activation_killed_at_every_boundary_settles_it_first() {
    uninstall_matrix(
        "urepl",
        |w| {
            activate(w);
            w.select(1);
        },
        "activate",
        15,
    );
}

#[test]
fn uninstalling_over_a_renewing_refresh_killed_at_every_boundary_settles_it_first() {
    uninstall_matrix(
        "urenew",
        |w| {
            activate(w);
            // (an input of the plan that is not a file of the copy: unplace keeps a copy the mod wrote into)
            put(&w.game, "kiln/kiln.json", br#"{"preview": ["kiln-sfi.interpreter"], "shroudforge": {"backend": "kiln"}, "native": {"policy": "never"}}"#);
        },
        "refresh",
        9,
    );
}

/// An activation killed half-way is settled by the next refresh, which activates the profile again.
#[test]
fn an_interrupted_activation_is_settled_by_the_next_refresh() {
    let w = world("next-refresh", false);
    activate(&w);
    w.select(1);
    assert!(run_child(&w, Boundary::PlanWritten, "activate"));
    assert!(w.file(sfi_activation::JOURNAL).exists());
    let a = profile::refresh_active(&w.dirs, &w.inst)
        .unwrap()
        .expect("activated again");
    assert_eq!(sfi_activation::start_check(&w.game), None);
    let doc = w.doc(Mode::Active);
    assert_eq!(doc["generation"].as_u64(), Some(a.generation));
    pinned_plan(&w.game, &doc, Mode::Active);
    assert_eq!(
        w.plans().len(),
        1,
        "the interrupted activation's plan is gone: {:?}",
        w.plans()
    );
    assert!(
        a.warnings.iter().any(|n| n.contains("did not finish")),
        "the recovery is said: {:#?}",
        a.warnings
    );
}

/// profile.json written by main's code (no plan) is read and refreshed into kiln mode; one with a plan is read by
/// main's reader shape (the member is optional).
#[test]
fn an_older_profile_file_is_refreshed_into_kiln_mode() {
    let w = world("older", false);
    select(&w.game, Some(HOSTED));
    activate(&w);
    assert!(w.doc(Mode::Active).get("sfLaunchPlan").is_none());
    select(&w.game, Some(KILN));
    let a = profile::refresh_active(&w.dirs, &w.inst)
        .unwrap()
        .expect("a plan for kiln mode");
    assert_eq!(a.generation, 2);
    pinned_plan(&w.game, &w.doc(Mode::Active), Mode::Active);
    let file = profile::read_game_profile(&w.game).unwrap().unwrap();
    assert!(file.sf_launch_plan.is_some());
    let v: Value = serde_json::to_value(&file).unwrap();
    assert_eq!(v["sfLaunchPlan"], w.doc(Mode::Active)["sfLaunchPlan"]);
}
