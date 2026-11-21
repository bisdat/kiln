// SPDX-License-Identifier: MIT
//! Kiln-mode play-once through `profile::write_once`: the start's own launch plan (`profile.mode`
//! `once`) pinned by `kiln/profile.once.json` through the activation journal of mode once, main's fixed-folder rules
//! kept, the active profile and its plan untouched, the plan removed with the play-once profile by the next
//! activation. Real process kills at each boundary of the once journal.
//!
//! Every package here is synthetic.

mod sfi_profile_common;
mod sfi_routing_common;

use kiln_manage::profile;
use kiln_manage::sfi_activation::{self, Boundary};
use kiln_manage::sfi_plan::Mode;
use sfi_profile_common::*;
use sfi_routing_common::{KILN, put, select};

const ALL: [Boundary; 9] = [
    Boundary::JournalBegun,
    Boundary::PlanStaged,
    Boundary::PlanWritten,
    Boundary::ProfileStaged,
    Boundary::ProfileWritten,
    Boundary::Committed,
    Boundary::PlacementsStaged,
    Boundary::RecoverPlanRemoved,
    Boundary::RecoverSettled,
];

/// The child (a no-op in a normal run).
#[test]
fn kill_child() {
    child(&ALL, |w, what| match what {
        "once" => drop(profile::write_once(&w.dirs, &w.profile, &w.inst)),
        "activate" => drop(profile::activate(&w.dirs, &w.profile, &w.inst)),
        other => panic!("{other}"),
    });
}

fn activate(w: &World) -> profile::Activation {
    profile::activate(&w.dirs, &w.profile, &w.inst).unwrap_or_else(|e| panic!("{e:?}"))
}

fn once(w: &World) -> profile::ProfileFile {
    profile::write_once(&w.dirs, &w.profile, &w.inst).unwrap_or_else(|e| panic!("{e:?}"))
}

#[test]
fn a_play_once_start_pins_its_own_plan_and_leaves_the_active_one() {
    let w = world("once", false);
    activate(&w);
    let active = std::fs::read(w.file("kiln/profile.json")).unwrap();
    let (file, seen) = passed(|| once(&w));
    let doc = w.doc(Mode::Once);
    let plan = pinned_plan(&w.game, &doc, Mode::Once);
    assert_eq!(plan["profile"]["mode"], "once");
    assert_eq!(
        file.sf_launch_plan.as_ref().map(|p| p.path.as_str()),
        doc["sfLaunchPlan"]["path"].as_str()
    );
    assert_eq!(
        std::fs::read(w.file("kiln/profile.json")).unwrap(),
        active,
        "the active profile is untouched"
    );
    pinned_plan(&w.game, &w.doc(Mode::Active), Mode::Active);
    assert_eq!(w.plans().len(), 2, "{:?}", w.plans());
    assert_eq!(inspections(&w.game), 2, "one inspection per plan");
    assert_eq!(sfi_activation::start_check_mode(&w.game, Mode::Once), None);
    assert_eq!(sfi_activation::start_check(&w.game), None);
    assert!(w.file("kiln/chain/once.ini").is_file(), "main's once.ini");
    // the journal of mode once: plan, then profile.once.json, committed last
    assert_eq!(seen.first(), Some(&Boundary::JournalBegun), "{seen:?}");
    let at = |b| {
        seen.iter()
            .position(|x| *x == b)
            .unwrap_or_else(|| panic!("{b:?}: {seen:?}"))
    };
    assert!(
        at(Boundary::PlanWritten) < at(Boundary::ProfileStaged)
            && at(Boundary::ProfileWritten) < at(Boundary::Committed)
    );
    assert!(
        !seen.contains(&Boundary::PlacementsStaged),
        "a play-once start changes no copy: {seen:?}"
    );
    let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Once);
    assert_eq!(
        (s.refused.clone(), s.plan.as_ref().map(|p| p.path.as_str())),
        (None, doc["sfLaunchPlan"]["path"].as_str()),
        "{s:?}"
    );
    // a second play-once replaces it; the next activation removes the play-once profile and its plan
    once(&w);
    assert_eq!(
        w.plans().len(),
        2,
        "the earlier play-once plan is gone: {:?}",
        w.plans()
    );
    activate(&w);
    assert!(!w.file("kiln/profile.once.json").exists());
    assert_eq!(w.plans().len(), 1, "{:?}", w.plans());
    pinned_plan(&w.game, &w.doc(Mode::Active), Mode::Active);
    assert!(profile::sfi::record(&w.dirs, &w.inst).once.is_none());
}

/// main's rule stays: a play-once profile whose ShroudForge mods differ from the active one's is refused (their folder
/// is fixed), and nothing is written for it: no journal, no plan, no play-once profile.
#[test]
fn play_once_keeps_mains_fixed_folder_refusal_and_writes_nothing() {
    let w = world("once-fixed", false);
    activate(&w);
    w.select(1);
    let plans = w.plans();
    let e = profile::write_once(&w.dirs, &w.profile, &w.inst).unwrap_err();
    assert_eq!(e.code, kiln_manage::ErrorCode::Unsupported, "{e:?}");
    assert!(e.message.contains("fixed game folder"), "{e:?}");
    assert_eq!(w.plans(), plans);
    assert!(
        !w.file("kiln/profile.once.json").exists() && !w.file(sfi_activation::JOURNAL).exists()
    );
}

/// A play-once start never supersedes a refused activation (the copies it refuses are the active profile's): it is
/// refused with that reason until the profile is activated again.
#[test]
fn play_once_is_refused_while_an_activation_is_refused() {
    let w = world("once-refused", false);
    activate(&w);
    w.select(1);
    assert!(run_child(&w, Boundary::PlanStaged, "activate"));
    assert_eq!(outcome(&recover_twice(&w, "once refused")), "refused");
    w.select(0);
    let e = profile::write_once(&w.dirs, &w.profile, &w.inst).unwrap_err();
    assert!(
        e.message
            .contains("activate the profile again before playing once"),
        "{e:?}"
    );
    assert!(
        sfi_activation::start_check(&w.game).is_some(),
        "still refused"
    );
    activate(&w);
    once(&w);
    assert_eq!(sfi_activation::start_check_mode(&w.game, Mode::Once), None);
}

/// No inspection answer: the play-once profile is written without a plan, and the launcher reads why.
#[test]
fn a_refused_once_plan_writes_the_play_once_profile_without_one() {
    let w = world("once-no-plan", false);
    activate(&w);
    refuse_inspection(&w.game, true);
    let file = once(&w);
    assert!(file.sf_launch_plan.is_none());
    let doc = w.doc(Mode::Once);
    assert!(doc.get("sfLaunchPlan").is_none(), "{doc:#}");
    let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Once);
    assert_eq!(
        s.refused.as_ref().map(|r| r.code.as_str()),
        Some("no-archive-base"),
        "{s:?}"
    );
    assert_eq!(w.plans().len(), 1, "the active plan only");
    refuse_inspection(&w.game, false);
}

/// A play-once start killed at each boundary of its journal: no play-once profile or the new one, whole; the plan it
/// published goes with a stop before its profile; the active profile and its plan never change.
#[test]
fn a_play_once_start_killed_at_every_boundary_recovers_whole() {
    let dry = world("once-dry", false);
    activate(&dry);
    let (_, seen) = passed(|| once(&dry));
    let order = first_passed(&seen);
    assert_eq!(
        order,
        [
            Boundary::JournalBegun,
            Boundary::PlanStaged,
            Boundary::PlanWritten,
            Boundary::ProfileStaged,
            Boundary::ProfileWritten,
            Boundary::Committed
        ],
        "{seen:?}"
    );
    for (i, b) in order.into_iter().enumerate() {
        let what = format!("play-once killed at {b:?}");
        let w = world(&format!("once-{b:?}"), false);
        activate(&w);
        if i % 2 == 1 {
            // over an earlier play-once profile too
            once(&w);
        }
        let before = GameState::of(&w);
        assert!(run_child(&w, b, "once"), "{what}: never reached");
        let r = recover_twice(&w, &what);
        let want = match b {
            Boundary::ProfileWritten => "completed",
            Boundary::Committed => "nothing",
            _ => "rolled-back",
        };
        assert_eq!(outcome(&r), want, "{what}: {r:?}");
        assert_settled(&w, &before, &r, Mode::Once, &what);
        let now = GameState::of(&w);
        assert_eq!(
            (now.profile, now.placements, now.copies),
            (before.profile, before.placements, before.copies),
            "{what}: active untouched"
        );
        assert_eq!(sfi_activation::start_check(&w.game), None, "{what}");
        once(&w);
        pinned_plan(&w.game, &w.doc(Mode::Once), Mode::Once);
        activate(&w);
        assert_eq!(w.plans().len(), 1, "{what}: {:?}", w.plans());
    }
}

/// A refused play-once plan is retried only when one of its inputs changed: the check before Play writes the play-once
/// profile at prepare and again at confirm, and with the same inputs the second answers the recorded refusal (no
/// inspection).
#[test]
fn a_refused_once_plan_is_retried_only_when_one_of_its_inputs_changed() {
    let w = world("once-refused-again", false);
    activate(&w);
    refuse_inspection(&w.game, true);
    for i in 0..3 {
        let file = once(&w);
        assert!(file.sf_launch_plan.is_none());
        assert_eq!(inspections(&w.game), 2, "start {i}: one inspection in all");
        let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Once);
        assert_eq!(
            s.refused.as_ref().map(|r| r.code.as_str()),
            Some("no-archive-base"),
            "{s:?}"
        );
    }
    put(&w.game, "enshrouded.kfc", b"kfc-2");
    once(&w);
    assert_eq!(inspections(&w.game), 3, "the archive changed: tried again");
    once(&w);
    assert_eq!(inspections(&w.game), 3);
    refuse_inspection(&w.game, false);
    put(&w.game, "enshrouded.kfc", b"kfc-3");
    once(&w);
    assert_eq!(inspections(&w.game), 4);
    pinned_plan(&w.game, &w.doc(Mode::Once), Mode::Once);
    // the active profile's plan stayed current all along
    profile::refresh_active(&w.dirs, &w.inst).unwrap();
    pinned_plan(&w.game, &w.doc(Mode::Active), Mode::Active);
}

/// A refusal of a play-once plan is answered again only for the play-once profile it was recorded for: a record older
/// than profile.once.json (a start that pinned a plan and stopped before it wrote its record) is no answer.
#[test]
fn a_once_refusal_recorded_before_the_play_once_profile_changed_is_not_answered_again() {
    let w = world("once-stale-refusal", false);
    activate(&w);
    refuse_inspection(&w.game, true);
    once(&w);
    let rec = w.dirs.installs().join(format!("{}.sfi.json", w.inst.id));
    let refused = std::fs::read(&rec).unwrap();
    refuse_inspection(&w.game, false);
    put(&w.game, "enshrouded.kfc", b"kfc-2");
    once(&w);
    assert_eq!(inspections(&w.game), 3);
    pinned_plan(&w.game, &w.doc(Mode::Once), Mode::Once);
    // that start's own record lost, and the input it changed back: the refusal before it is what is there, of the same
    // inputs
    std::fs::write(&rec, &refused).unwrap();
    std::fs::remove_file(w.file("enshrouded.kfc")).unwrap();
    let file = once(&w);
    assert_eq!(inspections(&w.game), 4, "produced, not answered");
    assert!(file.sf_launch_plan.is_some());
    pinned_plan(&w.game, &w.doc(Mode::Once), Mode::Once);
}

/// A play-once plan that cannot be written: the play-once profile is written without one and the launcher reads
/// `write-failed`; the next play-once start tries again (no input of the plan changed), and pins its plan once the plans
/// folder is back. The active profile's plan stays current.
#[test]
fn a_once_plan_that_cannot_be_written_is_refused_and_tried_again() {
    let w = world("once-write-failed", false);
    activate(&w);
    block_plans(&w);
    for i in 0..2 {
        let file = once(&w);
        assert!(file.sf_launch_plan.is_none());
        assert_eq!(inspections(&w.game), 2 + i, "start {i}: tried again");
        let s = profile::sfi::status(&w.dirs, &w.inst, Mode::Once);
        assert_eq!(
            s.refused.as_ref().map(|r| r.code.as_str()),
            Some("write-failed"),
            "{s:?}"
        );
        assert!(!w.file(sfi_activation::JOURNAL).exists());
    }
    unblock_plans(&w);
    once(&w);
    assert_eq!(inspections(&w.game), 4);
    pinned_plan(&w.game, &w.doc(Mode::Once), Mode::Once);
    profile::refresh_active(&w.dirs, &w.inst).unwrap();
    assert_eq!(inspections(&w.game), 4, "the active plan is current");
    pinned_plan(&w.game, &w.doc(Mode::Active), Mode::Active);
}

/// Kiln uninstalled over a play-once start stopped at each boundary of its journal: the uninstall settles it first,
/// exactly as the next activation would (a twin recovered first, then uninstalled, is left the same).
#[test]
fn uninstalling_over_a_play_once_start_killed_at_every_boundary_settles_it_first() {
    let dry = world("once-udry", false);
    activate(&dry);
    let (_, seen) = passed(|| once(&dry));
    let mut outcomes = Vec::new();
    for b in first_passed(&seen) {
        let what = format!("play-once killed at {b:?}, then Kiln uninstalled");
        let [a, t] = ["u", "t"].map(|side| {
            let w = world(&format!("once-{side}-{b:?}"), false);
            activate(&w);
            kiln_receipt(&w);
            w
        });
        let before = GameState::of(&a);
        assert!(run_child(&a, b, "once"), "{what}: never reached");
        assert!(run_child(&t, b, "once"), "{what}: never reached (twin)");
        let r = uninstall_settles_as_the_next_activation(&a, &t, &before, &what);
        outcomes.push(format!("{b:?}={}{:?}", outcome(&r), hidden(&a)));
    }
    eprintln!("uninstall once: {}", outcomes.join(" "));
}

/// A play-once start writes kiln/chain/once.ini and kiln/play-once.ini, which only a play-once plan fingerprints (the
/// chain fingerprint follows the plan's mode), so the next normal Play keeps the active plan: no archive inspection, the
/// same generation. An activation makes its plan before it removes an earlier play-once start's files: the next Play
/// keeps that plan too.
#[test]
fn a_play_once_start_leaves_the_active_plan_current() {
    let w = world("once-current", false);
    activate(&w);
    once(&w);
    // the start's flag as the launcher writes it, then consumed by the start
    std::fs::write(
        w.file("kiln/play-once.ini"),
        "[play-once]\r\nmode=profile\r\n",
    )
    .unwrap();
    profile::refresh_active(&w.dirs, &w.inst).unwrap();
    assert_eq!(
        inspections(&w.game),
        2,
        "a Play with the flag there keeps the active plan"
    );
    std::fs::remove_file(w.file("kiln/play-once.ini")).unwrap();
    profile::refresh_active(&w.dirs, &w.inst).unwrap();
    assert_eq!(
        (
            inspections(&w.game),
            w.doc(Mode::Active)["generation"].as_u64()
        ),
        (2, Some(1)),
        "a Play after a play-once start keeps the active plan"
    );
    activate(&w);
    let n = inspections(&w.game);
    profile::refresh_active(&w.dirs, &w.inst).unwrap();
    assert_eq!(
        inspections(&w.game),
        n,
        "a Play keeps the plan of an activation that followed a play-once start"
    );
}

/// A play-once start of a profile holding an EML package that needs ShroudForge's runtime: since dev.30 main no
/// longer counts the ShroudForge entry render adds for such a package as the player's selection
/// (without_runtime_eml_signals), so the start is not refused for it. Kiln mode answers as main does: both start, or
/// both refuse with main's words and write nothing.
#[test]
fn play_once_with_an_eml_runtime_package_answers_as_main_does() {
    let mut outcomes = Vec::new();
    for json in [None, Some(KILN)] {
        let w = world("once-eml", true);
        select(&w.game, json);
        activate(&w);
        let plans = w.plans();
        match profile::write_once(&w.dirs, &w.profile, &w.inst) {
            Ok(_) => outcomes.push(Ok(())),
            Err(e) => {
                assert!(
                    !w.file("kiln/profile.once.json").exists() && !w.file(sfi_activation::JOURNAL).exists()
                );
                assert_eq!(w.plans(), plans);
                outcomes.push(Err((e.code, e.message)));
            }
        }
    }
    assert_eq!(outcomes[0], outcomes[1], "hosted and kiln mode answer alike");
}
