// SPDX-License-Identifier: MIT
//! kiln-sfi's sfi.json: the record's rules (incomplete until the end, committed data stays present, unknown
//! stays null, a loss is never cleared, the safety reserve, schema bounds), the startup pass's skip mapping
//! §7.1(2)), the save-sensitive policy (exact match, case-insensitive paths), the atomic write with launch.json's
//! retry, the previous-start read, and the hub's ordering and background writes.
//!
//! The writer's documents are compared with schema/fixtures/sfi/state/writer/, which sfi_review.mjs validates against
//! sfi-state.schema.json. `SFI_BLESS=1` rewrites them.
use kiln_sfi::state::reducer::sf_id;
use kiln_sfi::state::safety::{self, Now, PackageFiles, Policy, Verdict};
use kiln_sfi::state::writer::{self, WriteOps};
use kiln_sfi::state::*;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Barrier, Mutex};
use std::time::{Duration, Instant};

/// A liveness bound, not a latency claim: how long a wait for something that must happen holds before the test
/// reports a hang. A passing wait returns as soon as its condition holds, so this sits far above any delay that
/// scheduling alone can cause on a loaded machine (300 s, as worker.rs and the lifecycle bodies).
const WAIT: Duration = Duration::from_secs(300);

static DIRS: AtomicU64 = AtomicU64::new(0);
fn fresh(name: &str) -> PathBuf {
    let n = DIRS.fetch_add(1, Ordering::Relaxed);
    // the test target's tmp folder here; on another machine (a native Windows run) the system's temp folder
    let base = Path::new(env!("CARGO_TARGET_TMPDIR"));
    let base = if base.is_dir() {
        base.to_path_buf()
    } else {
        std::env::temp_dir()
    };
    let dir = base
        .join("kiln-sfi-state-unit")
        .join(format!("{name}-{}-{n}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}
fn read(dir: &Path) -> serde_json::Value {
    serde_json::from_slice(&std::fs::read(dir.join("sfi.json")).unwrap()).unwrap()
}
fn temps(dir: &Path) -> Vec<String> {
    std::fs::read_dir(dir)
        .unwrap()
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.ends_with(".tmp"))
        .collect()
}
fn eventually(limit: Duration, mut ok: impl FnMut() -> bool) -> bool {
    let end = Instant::now() + limit;
    while Instant::now() < end {
        if ok() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    ok()
}

const STARTED: u64 = 1_763_520_000_200;
const UPDATED: u64 = 1_763_520_004_000;
fn identity() -> Identity {
    Identity {
        launch_id: Some(0x3f2b_8c1e_5a7d_4e2f),
        plan_hash: Some([0xaa; 32]),
        pid: 4242,
        process_start_ms: 1_763_520_000_000,
        side: Side::Client,
        profile: Some(("main".into(), 7)),
        backend: Backend::Kiln,
        reason_code: "plan".into(),
        reason_text: Some("kiln.json shroudforge.backend = kiln".into()),
        frozen_at_ms: Some(1_763_520_000_100),
    }
}
fn previous_clean() -> PreviousStart {
    PreviousStart {
        disposition: Disposition::Clean,
        launch_id: Some(0x9a1c_7e5b_2d3f_4a6b),
        pid: Some(4100),
    }
}
fn starting() -> Reducer {
    Reducer::new(identity(), previous_clean(), STARTED, Phase::Starting, None)
}
fn selected(r: &mut Reducer, id: &str, data: Data, lua: Lua) {
    let mut init = PackageInit::selected(id, Some("1.2.0"), Some(&format!("Mod {id}")));
    init.data = data;
    init.lua = lua;
    r.add_package(init);
}
fn row<'a>(r: &'a Reducer, id: &str) -> &'a Package {
    r.package(id).unwrap_or_else(|| panic!("no row {id}"))
}
fn qs_policy_line() -> &'static str {
    "The game drops the quiver and spellbook ammo at the next save while this mod is off."
}

// ---- the record's rules ----

#[test]
fn incomplete_until_the_end_and_the_phase_only_moves_forward() {
    let mut r = starting();
    assert_eq!(r.to_value(UPDATED)["incomplete"], true);
    assert_eq!(r.to_value(UPDATED)["phase"], "starting");
    assert_eq!(r.set_phase(Phase::Running, None), Change::Phase);
    assert_eq!(
        r.set_phase(Phase::Starting, None),
        Change::None,
        "never back"
    );
    assert_eq!(
        r.set_phase(Phase::Refused, Some(Reason::new("x", "y"))),
        Change::None,
        "running is past refusing"
    );
    assert_eq!(r.to_value(UPDATED)["incomplete"], true);
    assert_eq!(r.set_phase(Phase::Ended, None), Change::Phase);
    let v = r.to_value(UPDATED);
    assert_eq!(
        (v["phase"].as_str(), v["incomplete"].as_bool()),
        (Some("ended"), Some(false))
    );
    assert!(v.get("reason").is_none(), "ended has no reason");
    assert!(r.sealed());
    // sealed: nothing after the end changes the record
    selected(&mut r, "late", Data::Committed, Lua::Active);
    assert_eq!(r.set_phase(Phase::Running, None), Change::None);
    assert!(r.package("late").is_none());
    // starting may end refused, with its reason
    let mut r = starting();
    assert_eq!(
        r.set_phase(
            Phase::Refused,
            Some(Reason::new("no-tasks", "kiln.tasks is not available"))
        ),
        Change::Phase
    );
    let v = r.to_value(UPDATED);
    assert_eq!(v["reason"]["code"], "no-tasks");
    assert_eq!(v["incomplete"], false);
}

#[test]
fn committed_data_stays_present_whatever_the_runtime_does() {
    let mut r = starting();
    selected(&mut r, "a", Data::Staged, Lua::Admitted);
    r.set_data("a", Data::Committed, Some(false));
    assert_eq!(
        row(&r, "a").archive_present,
        Some(true),
        "committed means present, whatever the producer said"
    );
    r.set_lua("a", Lua::Active);
    // later events cannot take the data back out
    assert_eq!(r.set_data("a", Data::Excluded, Some(false)), Change::None);
    assert_eq!(r.set_data("a", Data::Uncertain, None), Change::None);
    assert_eq!(
        r.late_loss("a", "runtime-error", "runtime error in on_update"),
        Change::Warning
    );
    let a = row(&r, "a");
    assert_eq!(
        (a.data, a.archive_present, a.lua, a.runtime_active),
        (Data::Committed, Some(true), Lua::Stopped, Some(false))
    );
    let w = a.warning.as_ref().unwrap();
    assert_eq!(w.presence, Presence::DataAppliedRuntimeStopped);
    assert!(w.text.contains(reducer::LATE_LOSS), "{}", w.text);
    // cached-verified is the same
    selected(&mut r, "b", Data::CachedVerified, Lua::Active);
    assert_eq!(row(&r, "b").archive_present, Some(true));
}

#[test]
fn unknown_stays_null_and_is_never_called_not_applied() {
    let mut r = starting();
    selected(&mut r, "u", Data::Staged, Lua::Admitted);
    r.set_data("u", Data::Uncertain, Some(false));
    assert_eq!(
        row(&r, "u").archive_present,
        None,
        "uncertain data is unknown, whatever the producer said"
    );
    r.skip(
        "u",
        Skip::new(
            "data-failure",
            Part::Data,
            "the archive could not be read back",
        ),
    );
    let w = row(&r, "u").warning.clone().unwrap();
    assert_eq!(w.presence, Presence::Unknown);
    assert!(
        w.text.contains("could not be verified") && !w.text.contains("not in the game"),
        "{}",
        w.text
    );
    // an unproven exclusion stays unknown too
    selected(&mut r, "v", Data::Staged, Lua::Admitted);
    r.set_data("v", Data::Excluded, None);
    r.skip("v", Skip::new("dll-gate", Part::Dll, "refused"));
    assert_eq!(
        row(&r, "v").warning.as_ref().unwrap().presence,
        Presence::Unknown
    );
    // proven absent: not applied
    selected(&mut r, "w", Data::Staged, Lua::Admitted);
    r.set_data("w", Data::Excluded, Some(false));
    r.skip("w", Skip::new("dll-gate", Part::Dll, "refused"));
    let w = row(&r, "w").warning.clone().unwrap();
    assert_eq!(w.presence, Presence::NotApplied);
    // not save-sensitive: safe to save without it, so the warning does not say otherwise
    assert!(
        w.text.contains("Its data is not in the game.") && !w.text.contains(reducer::DO_NOT_SAVE),
        "{}",
        w.text
    );
}

#[test]
fn a_loss_is_never_cleared_by_a_late_or_old_event() {
    let mut r = starting();
    selected(&mut r, "a", Data::Committed, Lua::Active);
    r.late_loss("a", "watchdog", "on_update ran too long");
    let revision = r.warning_revision();
    // a late "active" (an old event) does not bring the runtime back
    assert_eq!(r.set_lua("a", Lua::Active), Change::None);
    assert_eq!(
        (row(&r, "a").lua, row(&r, "a").runtime_active),
        (Lua::Stopped, Some(false))
    );
    // a second loss does not replace the first
    assert_eq!(
        r.late_loss("a", "runtime-error", "something else"),
        Change::None
    );
    assert_eq!(row(&r, "a").skip.as_ref().unwrap().code, "watchdog");
    assert_eq!(r.warning_revision(), revision);
    r.set_phase(Phase::Ended, None);
    // after the end: nothing
    selected(&mut r, "b", Data::Committed, Lua::Active);
    assert_eq!(r.late_loss("a", "x", "y"), Change::None);
    assert_eq!(r.set_lua("a", Lua::Active), Change::None);
    assert_eq!(r.warning_revision(), revision);
    assert!(row(&r, "a").warning.is_some(), "a warning is never removed");
}

#[test]
fn warning_revision_rises_with_each_new_or_changed_warning() {
    let mut r = starting();
    for id in ["a", "b"] {
        selected(&mut r, id, Data::Staged, Lua::Admitted);
        r.skip(
            id,
            Skip::new(
                "missing-dependency",
                Part::Package,
                "needs c, which is not installed",
            ),
        );
    }
    assert_eq!(r.warning_revision(), 2);
    assert_eq!(
        (
            row(&r, "a").warning.as_ref().unwrap().revision,
            row(&r, "b").warning.as_ref().unwrap().revision
        ),
        (1, 2)
    );
    // proving b's data absent changes its warning (presence): a new revision
    assert_eq!(
        r.set_data("b", Data::Excluded, Some(false)),
        Change::Warning
    );
    assert_eq!(r.warning_revision(), 3);
    assert_eq!(row(&r, "b").warning.as_ref().unwrap().revision, 3);
    // a change that leaves the warning as it is raises nothing
    r.set_dll(
        "b",
        Dll {
            name: "b.dll".into(),
            sha256: None,
            state: DllState::Refused,
        },
    );
    assert_eq!(r.warning_revision(), 3);
    for p in r.packages() {
        assert!(
            p.warning
                .as_ref()
                .is_none_or(|w| w.revision <= r.warning_revision())
        );
    }
}

#[test]
fn the_safety_reserve_keeps_every_warning_under_saturation() {
    let mut r = starting();
    for i in 0..reducer::MAX_PACKAGES {
        selected(&mut r, &format!("p{i}"), Data::Committed, Lua::Active);
    }
    assert_eq!(r.packages().len(), reducer::MAX_PACKAGES);
    // a plain row never pushes anything out
    selected(&mut r, "plain", Data::Committed, Lua::Active);
    assert!(r.package("plain").is_none());
    assert_eq!(r.dropped(), 1);
    // a skip for a package the full table does not hold still gets its row (a plain row makes room)
    assert_eq!(
        r.skip("late-comer", Skip::new("dll-gate", Part::Dll, "refused")),
        Change::Warning
    );
    assert_eq!(r.packages().len(), reducer::MAX_PACKAGES);
    let late = row(&r, "late-comer");
    assert!(
        late.warning.is_some() && late.archive_present.is_none(),
        "an unplanned row's presence is unknown"
    );
    // many simultaneous faults: every one stays
    for i in 0..100 {
        r.late_loss(&format!("p{i}"), "runtime-error", "boom");
    }
    for i in 0..100 {
        assert!(row(&r, &format!("p{i}")).warning.is_some(), "p{i}");
    }
    for i in 0..50 {
        r.skip(
            &format!("extra{i}"),
            Skip::new("dll-gate", Part::Dll, "refused"),
        );
        assert!(r.package(&format!("extra{i}")).is_some());
    }
    let warned = r.packages().iter().filter(|p| p.warning.is_some()).count();
    assert_eq!(warned, 151);
    assert_eq!(r.warning_revision(), 151);
    assert_eq!(r.packages().len(), reducer::MAX_PACKAGES);
}

#[test]
fn disabled_other_side_and_absent_rows_are_never_skips() {
    let mut r = starting();
    for (id, intent) in [
        ("d", Intent::Disabled),
        ("o", Intent::OtherSide),
        ("x", Intent::Absent),
    ] {
        let mut init = PackageInit::selected(id, None, None);
        init.intent = intent;
        r.add_package(init);
        assert_eq!(
            r.skip(id, Skip::new("dll-gate", Part::Dll, "refused")),
            Change::None,
            "{id}"
        );
        assert!(row(&r, id).skip.is_none() && row(&r, id).warning.is_none());
    }
    assert_eq!(r.warning_revision(), 0);
}

#[test]
fn the_startup_pass_record_maps_onto_skips() {
    // The failing package, the packages left out with it
    let mut r = starting();
    selected(&mut r, "base", Data::Staged, Lua::Admitted);
    let outcome = StartupOutcome {
        failed: Some(OutcomePackage {
            id: "base".into(),
            version: Some("1.0.0".into()),
            name: Some("Base".into()),
            text: "pregame phase failed: base/init.lua:3: attempt to index nil".into(),
            chain: vec![],
            absent_proven: true,
        }),
        dependents: vec![OutcomePackage {
            id: "addon".into(),
            version: Some("2.0.0".into()),
            name: None,
            text: "needs base, which failed in the pregame pass".into(),
            chain: vec![],
            absent_proven: false,
        }],
        first_error: Some("pregame phase failed: base/init.lua:3: attempt to index nil".into()),
        retry_error: None,
        packages: vec![],
    };
    assert_eq!(r.startup_outcome(&outcome), Change::Warning);
    let base = row(&r, "base");
    assert_eq!(
        (base.data, base.lua, base.archive_present),
        (Data::Failed, Lua::Excluded, Some(false))
    );
    let skip = base.skip.as_ref().unwrap();
    assert_eq!(
        (skip.code.as_str(), skip.part, skip.late, skip.chain.len()),
        ("startup-failed", Part::Data, false, 0)
    );
    assert_eq!(
        skip.text,
        outcome.failed.as_ref().unwrap().text,
        "the fork's text"
    );
    assert_eq!(
        base.warning.as_ref().unwrap().presence,
        Presence::NotApplied
    );
    let addon = row(&r, "addon");
    assert_eq!(
        (addon.data, addon.lua, addon.archive_present),
        (Data::Excluded, Lua::Excluded, None)
    );
    let skip = addon.skip.as_ref().unwrap();
    assert_eq!(
        (skip.code.as_str(), skip.chain.clone()),
        ("dependency-skipped", vec!["base".to_owned()])
    );
    assert_eq!(
        addon.version.as_deref(),
        Some("2.0.0"),
        "a row made from the record keeps its version"
    );
    let w = addon.warning.as_ref().unwrap();
    assert_eq!(
        (w.trigger, w.presence),
        (Trigger::Skipped, Presence::Unknown)
    );
    assert!(w.text.contains("It needs base."), "{}", w.text);
}

#[test]
fn a_failed_retry_excludes_every_package_of_the_pass_with_both_errors() {
    let mut r = starting();
    let p = |id: &str, absent| OutcomePackage {
        id: id.into(),
        version: Some("1.0.0".into()),
        name: None,
        text: String::new(),
        chain: vec![],
        absent_proven: absent,
    };
    let outcome = StartupOutcome {
        failed: Some(p("base", true)),
        dependents: vec![p("addon", true)],
        first_error: Some("pregame phase failed: first".into()),
        retry_error: Some("pregame phase failed: second".into()),
        packages: vec![p("base", true), p("other", false), p("third", true)],
    };
    r.startup_outcome(&outcome);
    for id in ["base", "addon", "other", "third"] {
        let row = row(&r, id);
        let skip = row.skip.as_ref().unwrap();
        assert_eq!(
            (row.data, skip.code.as_str()),
            (Data::Excluded, "startup-retry-failed"),
            "{id}"
        );
        assert!(
            skip.text.contains("first") && skip.text.contains("second"),
            "{id}: both errors: {}",
            skip.text
        );
        let presence = row.warning.as_ref().unwrap().presence;
        let expected = if id == "other" {
            Presence::Unknown
        } else {
            Presence::NotApplied
        };
        assert_eq!(
            presence, expected,
            "{id}: false only where the generation proves it absent"
        );
    }
}

#[test]
fn a_save_sensitive_package_turned_off_warns() {
    let mut r = starting();
    let off = |now| TurnedOff {
        id: "ammo-slots".into(),
        version: Some("0.8.2".into()),
        name: Some("Quiver & Spellbook".into()),
        now,
        save_sensitive: SaveSensitive::policy("ammo-slots", "0.8.2", Some(qs_policy_line())),
        archive_present: None,
    };
    assert_eq!(
        r.turned_off(off(Intent::Selected)),
        Change::None,
        "still selected: not off"
    );
    assert_eq!(r.turned_off(off(Intent::OtherSide)), Change::None);
    assert_eq!(r.turned_off(off(Intent::Disabled)), Change::Warning);
    let p = row(&r, "ammo-slots");
    assert_eq!((p.intent, p.skip.is_none()), (Intent::Disabled, true));
    let w = p.warning.as_ref().unwrap();
    assert_eq!(
        (w.trigger, w.save_sensitive),
        (Trigger::SaveSensitiveOff, true)
    );
    assert!(
        w.text
            .contains("Quiver & Spellbook (ammo-slots 0.8.2) was active at the previous start"),
        "{}",
        w.text
    );
    assert!(
        w.text.contains(qs_policy_line()) && w.text.contains(reducer::DO_NOT_SAVE),
        "{}",
        w.text
    );
    // once only
    assert_eq!(r.turned_off(off(Intent::Disabled)), Change::None);
    // a package that is not save-sensitive never warns this way
    let mut plain = off(Intent::Absent);
    plain.id = "plain".into();
    plain.save_sensitive = SaveSensitive::none();
    assert_eq!(r.turned_off(plain), Change::None);
}

#[test]
fn every_value_fits_the_schema_bounds() {
    let mut r = starting();
    // not a ShroudForge identifier: left out (counted), never written
    selected(&mut r, "has space", Data::Staged, Lua::Admitted);
    selected(&mut r, &"x".repeat(81), Data::Staged, Lua::Admitted);
    assert_eq!((r.packages().len(), r.dropped()), (0, 2));
    let mut init = PackageInit::selected("ok", Some(&"9".repeat(80)), Some(&"é".repeat(300)));
    init.data = Data::Staged;
    r.add_package(init);
    let mut skip = Skip::new("Not A Code!", Part::Data, "ü".repeat(5000));
    skip.chain = (0..70)
        .map(|i| format!("c{i}"))
        .chain(["bad id".to_owned()])
        .collect();
    skip.loader = Some("XHL Core".into());
    r.skip("ok", skip);
    let p = row(&r, "ok");
    assert_eq!(p.version.as_ref().unwrap().chars().count(), 64);
    assert_eq!(p.name.as_ref().unwrap().chars().count(), 200);
    let s = p.skip.as_ref().unwrap();
    assert_eq!(
        (
            s.code.as_str(),
            s.text.chars().count(),
            s.chain.len(),
            s.loader.clone()
        ),
        ("unknown", 4096, 64, None)
    );
    assert!(p.warning.as_ref().unwrap().text.chars().count() <= 8192);
    let reason = Reason::new("", "   ");
    assert_eq!(
        (reason.code.as_str(), reason.text.is_empty()),
        ("unknown", false)
    );
}

// ---- the documents the writer produces, against the schema's fixtures ----

fn bless() -> bool {
    std::env::var_os("SFI_BLESS").is_some_and(|v| v == "1")
}
fn fixtures() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../schema/fixtures/sfi/state/writer")
}
fn qs_running() -> Reducer {
    let mut r = starting();
    r.set_menu(MenuBy::Kiln, None);
    selected(&mut r, "example-lua", Data::Committed, Lua::Active);
    let mut qs = PackageInit::selected("ammo-slots", Some("0.8.2"), Some("Quiver & Spellbook"));
    qs.save_sensitive = SaveSensitive::policy("ammo-slots", "0.8.2", Some(qs_policy_line()));
    r.add_package(qs);
    r.set_dll(
        "ammo-slots",
        Dll {
            name: "ammo-slots.dll".into(),
            sha256: Some([0x40; 32]),
            state: DllState::Refused,
        },
    );
    r.set_data("ammo-slots", Data::NotNeeded, Some(false));
    r.set_lua("ammo-slots", Lua::Excluded);
    let mut skip = Skip::new(
        "dll-gate",
        Part::Dll,
        "ammo-slots.dll was refused by the native load gate",
    );
    skip.loader = Some("xhl".into());
    r.skip("ammo-slots", skip);
    r.set_phase(Phase::Running, None);
    r
}
fn scenarios() -> Vec<(&'static str, Reducer)> {
    let mut out = Vec::new();
    out.push(("starting", starting()));
    out.push(("running-qs-skipped", qs_running()));
    let mut late = qs_running();
    late.late_loss(
        "example-lua",
        "runtime-error",
        "runtime error in on_update: attempt to call a nil value",
    );
    out.push(("running-late-loss", late));
    let mut ended = qs_running();
    ended.set_phase(Phase::Ended, None);
    out.push(("ended", ended));
    // The live runtime generation's owner epoch, on the active row only
    let mut epochs = qs_running();
    epochs.set_owner_epoch("example-lua", Some(3));
    epochs.set_owner_epoch("ammo-slots", Some(3));
    out.push(("running-owner-epoch", epochs));
    let mut startup = starting();
    selected(&mut startup, "base", Data::Staged, Lua::Admitted);
    startup.startup_outcome(&StartupOutcome {
        failed: Some(OutcomePackage {
            id: "base".into(),
            version: Some("1.0.0".into()),
            name: Some("Base".into()),
            text: "pregame phase failed: base/init.lua:3: attempt to index nil".into(),
            chain: vec![],
            absent_proven: true,
        }),
        dependents: vec![OutcomePackage {
            id: "addon".into(),
            version: Some("2.0.0".into()),
            name: None,
            text: "needs base, which failed in the pregame pass".into(),
            chain: vec![],
            absent_proven: false,
        }],
        first_error: Some("pregame phase failed: base/init.lua:3: attempt to index nil".into()),
        retry_error: None,
        packages: vec![],
    });
    out.push(("startup-failed", startup));
    let mut retry = starting();
    retry.startup_outcome(&StartupOutcome {
        failed: Some(OutcomePackage {
            id: "base".into(),
            version: Some("1.0.0".into()),
            name: None,
            text: "x".into(),
            chain: vec![],
            absent_proven: true,
        }),
        dependents: vec![],
        first_error: Some("pregame phase failed: first".into()),
        retry_error: Some("pregame phase failed: second".into()),
        packages: vec![OutcomePackage {
            id: "other".into(),
            version: Some("3.1.4".into()),
            name: Some("Other".into()),
            text: String::new(),
            chain: vec![],
            absent_proven: false,
        }],
    });
    out.push(("startup-retry-failed", retry));
    let mut off = starting();
    off.turned_off(TurnedOff {
        id: "ammo-slots".into(),
        version: Some("0.8.2".into()),
        name: Some("Quiver & Spellbook".into()),
        now: Intent::Absent,
        save_sensitive: SaveSensitive::policy("ammo-slots", "0.8.2", Some(qs_policy_line())),
        archive_present: Some(false),
    });
    off.turned_off(TurnedOff {
        id: "mixed".into(),
        version: None,
        name: None,
        now: Intent::Disabled,
        save_sensitive: SaveSensitive::unknown_mixed(),
        archive_present: None,
    });
    out.push(("save-sensitive-off", off));
    let mut hosted = identity();
    hosted.backend = Backend::Hosted;
    hosted.reason_code = "hosted-default".into();
    hosted.reason_text = None;
    hosted.plan_hash = None;
    out.push((
        "aside-hosted",
        Reducer::new(
            hosted,
            PreviousStart::NONE,
            STARTED,
            Phase::Aside,
            Some(Reason::new(
                "hosted-default",
                "backend hosted: kiln-shroudforge has this start (no setting)",
            )),
        ),
    ));
    let older = Identity {
        launch_id: None,
        plan_hash: None,
        profile: None,
        backend: Backend::None,
        reason_code: "no-kiln-launch".into(),
        reason_text: Some("this core has no kiln.launch record kiln-sfi can read".into()),
        frozen_at_ms: None,
        ..identity()
    };
    out.push((
        "aside-older-core",
        Reducer::new(
            older,
            PreviousStart {
                disposition: Disposition::Suspect,
                launch_id: None,
                pid: None,
            },
            STARTED,
            Phase::Aside,
            Some(Reason::new(
                "no-kiln-launch",
                "backend negotiation not integrated",
            )),
        ),
    ));
    let mut carried_hosted = identity();
    carried_hosted.backend = Backend::Hosted;
    carried_hosted.reason_code = "hosted-default".into();
    carried_hosted.reason_text = None;
    out.push((
        "aside-carried",
        Reducer::new(
            carried_hosted,
            previous_clean(),
            STARTED,
            Phase::Aside,
            Some(Reason::new(
                "hosted-default",
                "backend hosted: kiln-shroudforge has this start (no setting)",
            )),
        )
        .with_carried(&[
            PreviouslyActive {
                id: "ammo-slots".into(),
                version: Some("0.8.2".into()),
                name: Some("Quiver & Spellbook".into()),
                basis: "policy".into(),
                policy_row: Some(("ammo-slots".into(), "0.8.2".into())),
                from_launch_id: Some(0x9a1c_7e5b_2d3f_4a6b),
            },
            PreviouslyActive {
                id: "mixed".into(),
                version: None,
                name: None,
                basis: "unknown-mixed".into(),
                policy_row: None,
                from_launch_id: Some(0x9a1c_7e5b_2d3f_4a6b),
            },
        ]),
    ));
    out.push((
        "refused-env-off",
        Reducer::new(
            identity(),
            PreviousStart {
                disposition: Disposition::Incomplete,
                launch_id: Some(0x9a1c_7e5b_2d3f_4a6b),
                pid: Some(4100),
            },
            STARTED,
            Phase::Refused,
            Some(Reason::new("env-off", "KILN_SHROUDFORGE=0")),
        ),
    ));
    out
}

#[test]
fn the_writer_produces_the_schema_fixtures() {
    let dir = fixtures();
    if bless() {
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
    }
    let mut names = Vec::new();
    for (name, record) in scenarios() {
        let file = dir.join(format!("{name}.json"));
        let bytes = record.to_bytes(UPDATED);
        if bless() {
            std::fs::write(&file, &bytes).unwrap();
        }
        let committed = std::fs::read(&file)
            .unwrap_or_else(|e| panic!("{}: {e} (SFI_BLESS=1 writes it)", file.display()));
        assert!(
            committed == bytes,
            "{name}: the writer's document changed; rerun with SFI_BLESS=1 and review it"
        );
        names.push(format!("{name}.json"));
    }
    let mut on_disk: Vec<String> = std::fs::read_dir(&dir)
        .unwrap()
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .collect();
    on_disk.sort();
    names.sort();
    assert_eq!(on_disk, names, "a fixture no scenario produces");
}

// ---- the save-sensitive policy (generic: other backends can use it) ----

fn shipped_policy() -> Policy {
    let file =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../schema/sfi/sfi-safety-policy.json");
    Policy::parse(&std::fs::read(file).unwrap()).unwrap()
}

#[test]
fn the_shipped_policy_reads_and_names_quiver_and_spellbook() {
    let policy = shipped_policy();
    assert!(policy.unknown_mixed_sensitive && policy.revision >= 1);
    let row = &policy.rows[0];
    assert_eq!(
        (row.id.as_str(), row.version.as_str(), row.name.as_str()),
        ("ammo-slots", "0.8.2", "Quiver & Spellbook")
    );
    assert!(row.text.contains("drops the quiver and spellbook ammo"));
    assert_eq!(row.variants[0].len(), 2);
}

/// A synthetic policy pinning two synthetic files (never a real mod's bytes).
fn synthetic(dir: &Path) -> Policy {
    std::fs::write(dir.join("mod.json"), b"{\"id\":\"syn\"}").unwrap();
    std::fs::write(dir.join("Syn-Native.DLL"), b"MZ synthetic").unwrap();
    let hash = |b: &[u8]| {
        use sha2::Digest;
        sha2::Sha256::digest(b)
            .iter()
            .map(|x| format!("{x:02x}"))
            .collect::<String>()
    };
    let doc = serde_json::json!({
        "format": 1, "revision": 3, "scope": "every-backend", "unknownMixedDataNative": "save-sensitive",
        "packages": [{
            "id": "syn", "version": "1.0.0", "name": "Synthetic", "risk": "save-data-loss", "text": "Synthetic line.",
            "evidence": "test", "variants": [{ "files": [
                { "path": "mod.json", "sha256": hash(b"{\"id\":\"syn\"}"), "size": 12 },
                { "path": "syn-native.dll", "sha256": hash(b"MZ synthetic"), "size": 12 }
            ] }]
        }]
    });
    Policy::parse(&serde_json::to_vec(&doc).unwrap()).unwrap()
}

#[test]
fn the_policy_matches_exactly_and_finds_files_ignoring_case() {
    let dir = fresh("policy");
    let policy = synthetic(&dir);
    assert!(policy.every_backend);
    let pkg = |id: &'static str, version: &'static str| PackageFiles {
        id,
        version,
        dir: &dir,
        has_data: true,
        has_native: true,
    };
    // Syn-Native.DLL on disk is the pinned syn-native.dll
    assert_eq!(
        policy.classify(&pkg("syn", "1.0.0")),
        Verdict::Policy {
            id: "syn".into(),
            version: "1.0.0".into(),
            text: "Synthetic line.".into()
        }
    );
    // exact id and version only
    for (id, version) in [
        ("Syn", "1.0.0"),
        ("syn-extra", "1.0.0"),
        ("syn", "1.0.1"),
        ("syn", "v1.0.0"),
    ] {
        assert_eq!(
            policy.classify(&pkg(id, version)),
            Verdict::UnknownMixed,
            "{id} {version}"
        );
    }
    // other bytes, same size: no match
    std::fs::write(dir.join("Syn-Native.DLL"), b"MZ synthetiC").unwrap();
    assert_eq!(policy.classify(&pkg("syn", "1.0.0")), Verdict::UnknownMixed);
    // a pinned file missing: no match; nothing native and no row: not sensitive
    std::fs::remove_file(dir.join("Syn-Native.DLL")).unwrap();
    assert!(policy.matching(&pkg("syn", "1.0.0")).is_none());
    assert_eq!(
        policy.classify(&PackageFiles {
            id: "syn",
            version: "1.0.0",
            dir: &dir,
            has_data: true,
            has_native: false
        }),
        Verdict::NotSensitive
    );
}

#[test]
fn the_policy_reader_refuses_ambiguous_rows() {
    let row = |files: serde_json::Value| {
        serde_json::json!({ "id": "a", "version": "1", "name": "A", "risk": "save-data-loss", "text": "t", "evidence": "e",
            "variants": [{ "files": files }] })
    };
    let doc = |rows: Vec<serde_json::Value>| {
        serde_json::to_vec(
            &serde_json::json!({ "format": 1, "revision": 1, "scope": "interpreter-only",
            "unknownMixedDataNative": "listed-only", "packages": rows }),
        )
        .unwrap()
    };
    let f = |path: &str| serde_json::json!({ "path": path, "sha256": "0".repeat(64), "size": 1 });
    assert!(Policy::parse(&doc(vec![row(serde_json::json!([f("mod.json")]))])).is_ok());
    for (what, bytes) in [
        (
            "listed twice",
            doc(vec![
                row(serde_json::json!([f("mod.json")])),
                row(serde_json::json!([f("mod.json")])),
            ]),
        ),
        (
            "a path twice ignoring case",
            doc(vec![row(serde_json::json!([f("mod.json"), f("MOD.json")]))]),
        ),
        (
            "no manifest pin",
            doc(vec![row(serde_json::json!([f("a.dll")]))]),
        ),
        (
            "a path escaping",
            doc(vec![row(serde_json::json!([f("mod.json"), f("../x.dll")]))]),
        ),
    ] {
        assert!(Policy::parse(&bytes).is_err(), "{what}");
    }
}

#[test]
fn turned_off_compares_two_starts() {
    let previous = vec![
        ("q", "0.8.2"),
        ("still", "1"),
        ("gone", "1"),
        ("other", "1"),
    ];
    let now = |id: &str| match id {
        "q" => Now::Disabled,
        "still" => Now::Selected,
        "other" => Now::OtherSide,
        _ => Now::Absent,
    };
    let off: Vec<(&str, Now)> = safety::turned_off(&previous, |p| p.0, now)
        .into_iter()
        .map(|(p, n)| (p.0, n))
        .collect();
    assert_eq!(off, vec![("q", Now::Disabled), ("gone", Now::Absent)]);
}

// ---- the file ----

static RENAMES: Mutex<Vec<PathBuf>> = Mutex::new(Vec::new());
fn recording_rename(from: &Path, to: &Path) -> std::io::Result<()> {
    RENAMES.lock().unwrap().push(from.to_path_buf());
    std::fs::rename(from, to)
}

#[test]
fn every_write_uses_its_own_temp_file_and_leaves_none() {
    let dir = fresh("atomic");
    let ops = WriteOps {
        rename: recording_rename,
        sleep: |_| {},
    };
    RENAMES.lock().unwrap().clear();
    writer::write_retrying(&dir, b"{\"one\":1}\n", &ops).unwrap();
    writer::write_retrying(&dir, b"{\"two\":2}\n", &ops).unwrap();
    let renames = RENAMES.lock().unwrap().clone();
    assert_eq!(renames.len(), 2);
    assert_ne!(renames[0], renames[1], "a temp name is never reused");
    for temp in &renames {
        let name = temp.file_name().unwrap().to_string_lossy();
        assert!(
            name.starts_with(&format!("sfi.json.{}-", std::process::id()))
                && name.ends_with(".tmp"),
            "{name}"
        );
        assert_eq!(
            temp.parent(),
            Some(dir.as_path()),
            "the temp file is in the same folder (same volume)"
        );
    }
    assert_eq!(
        std::fs::read(dir.join("sfi.json")).unwrap(),
        b"{\"two\":2}\n"
    );
    assert!(temps(&dir).is_empty());
    // many writers at once: each try has its own temp, the file is always one whole document
    let barrier = Arc::new(Barrier::new(8));
    let handles: Vec<_> = (0..8)
        .map(|i| {
            let (dir, barrier) = (dir.clone(), barrier.clone());
            std::thread::spawn(move || {
                barrier.wait();
                for j in 0..20 {
                    writer::write_retrying(
                        &dir,
                        format!("{{\"w\":{i},\"j\":{j}}}\n").as_bytes(),
                        &WriteOps::REAL,
                    )
                    .unwrap();
                    let v: serde_json::Value =
                        serde_json::from_slice(&std::fs::read(dir.join("sfi.json")).unwrap())
                            .unwrap();
                    assert!(v["w"].is_u64());
                }
            })
        })
        .collect();
    for h in handles {
        h.join().unwrap();
    }
    assert!(temps(&dir).is_empty());
}

static REFUSALS: AtomicU32 = AtomicU32::new(0);
static SLEPT: Mutex<Vec<Duration>> = Mutex::new(Vec::new());
fn refusing_rename(from: &Path, to: &Path) -> std::io::Result<()> {
    if REFUSALS.load(Ordering::SeqCst) > 0 {
        REFUSALS.fetch_sub(1, Ordering::SeqCst);
        return Err(std::io::Error::from(std::io::ErrorKind::PermissionDenied));
    }
    std::fs::rename(from, to)
}
fn recording_sleep(d: Duration) {
    SLEPT.lock().unwrap().push(d);
}

#[test]
fn a_refused_rename_is_tried_again_like_launch_json() {
    let ops = WriteOps {
        rename: refusing_rename,
        sleep: recording_sleep,
    };
    let dir = fresh("retry");
    std::fs::write(dir.join("sfi.json"), b"old\n").unwrap();
    // refused three times (a reader holds the old file open), then through
    REFUSALS.store(3, Ordering::SeqCst);
    SLEPT.lock().unwrap().clear();
    assert_eq!(writer::write_retrying(&dir, b"new\n", &ops).unwrap(), 4);
    assert_eq!(*SLEPT.lock().unwrap(), vec![writer::WAIT; 3]);
    assert_eq!(std::fs::read(dir.join("sfi.json")).unwrap(), b"new\n");
    assert!(temps(&dir).is_empty());
    // refused every time: 20 tries 25 ms apart, then the error; the old file stays whole, no temp left
    REFUSALS.store(1000, Ordering::SeqCst);
    SLEPT.lock().unwrap().clear();
    let error = writer::write_retrying(&dir, b"newer\n", &ops).unwrap_err();
    assert_eq!(error.kind(), std::io::ErrorKind::PermissionDenied);
    assert_eq!(1000 - REFUSALS.load(Ordering::SeqCst), writer::ATTEMPTS);
    assert_eq!(*SLEPT.lock().unwrap(), vec![Duration::from_millis(25); 19]);
    assert_eq!(std::fs::read(dir.join("sfi.json")).unwrap(), b"new\n");
    assert!(temps(&dir).is_empty());
    REFUSALS.store(0, Ordering::SeqCst);
}

#[test]
fn the_replaced_file_says_how_the_last_start_ended() {
    let dir = fresh("previous");
    let me = 777;
    assert_eq!(writer::read_previous(&dir, me).start, PreviousStart::NONE);
    let mut ended = qs_running();
    ended.set_phase(Phase::Ended, None);
    std::fs::write(dir.join("sfi.json"), ended.to_bytes(UPDATED)).unwrap();
    let previous = writer::read_previous(&dir, me);
    assert_eq!(
        previous.start,
        PreviousStart {
            disposition: Disposition::Clean,
            launch_id: Some(0x3f2b_8c1e_5a7d_4e2f),
            pid: Some(4242)
        }
    );
    // the save-sensitive package was selected but never active (skipped): not "active at the previous start"
    assert!(previous.save_sensitive_active.is_empty());
    let mut active = starting();
    let mut qs = PackageInit::selected("ammo-slots", Some("0.8.2"), Some("Quiver & Spellbook"));
    qs.save_sensitive = SaveSensitive::policy("ammo-slots", "0.8.2", None);
    qs.data = Data::NotNeeded;
    qs.lua = Lua::Active;
    active.add_package(qs);
    std::fs::write(dir.join("sfi.json"), active.to_bytes(UPDATED)).unwrap();
    let previous = writer::read_previous(&dir, me);
    assert_eq!(
        previous.start.disposition,
        Disposition::Incomplete,
        "a start that never wrote its end died"
    );
    assert_eq!(previous.save_sensitive_active.len(), 1);
    assert_eq!(
        previous.save_sensitive_active[0].policy_row,
        Some(("ammo-slots".into(), "0.8.2".into()))
    );
    // this very pid wrote it: a reused pid, nothing concluded
    let previous = writer::read_previous(&dir, 4242);
    assert_eq!(previous.start.disposition, Disposition::Suspect);
    assert!(previous.save_sensitive_active.is_empty());
    for garbage in [
        &b"{not json"[..],
        b"{\"format\":2,\"packages\":[],\"incomplete\":false}",
        b"[]",
        b"{\"format\":1,\"packages\":[]}",
    ] {
        std::fs::write(dir.join("sfi.json"), garbage).unwrap();
        assert_eq!(
            writer::read_previous(&dir, me).start.disposition,
            Disposition::Suspect,
            "{}",
            String::from_utf8_lossy(garbage)
        );
    }
}

#[test]
fn only_other_processes_temp_files_are_swept() {
    let dir = fresh("sweep");
    let mine = format!("sfi.json.{}-99.tmp", std::process::id());
    for name in [
        "sfi.json.1-0.tmp",
        "sfi.json.2-5.tmp",
        mine.as_str(),
        "other.tmp",
        "sfi.json",
    ] {
        std::fs::write(dir.join(name), b"x").unwrap();
    }
    writer::sweep_temps(&dir);
    let mut left: Vec<String> = std::fs::read_dir(&dir)
        .unwrap()
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .collect();
    left.sort();
    let mut expected = vec!["other.tmp".to_owned(), "sfi.json".to_owned(), mine];
    expected.sort();
    assert_eq!(left, expected);
}

// ---- the hub: who writes when ----

fn hub(dir: &Path) -> Arc<StateHub> {
    let hub = Arc::new(StateHub::new());
    hub.set_dir(Some(dir.to_path_buf()));
    hub
}

#[test]
fn starting_is_on_disk_when_begin_returns_and_background_writes_follow_phases() {
    let dir = fresh("hub");
    let hub = hub(&dir);
    assert_eq!(hub.begin(identity(), Phase::Starting, None), Begun::Written);
    let v = read(&dir);
    assert_eq!(
        (v["phase"].as_str(), v["incomplete"].as_bool()),
        (Some("starting"), Some(true))
    );
    assert_eq!(
        v["launch"]["launchId"], "3f2b8c1e5a7d4e2f",
        "kiln.launch's id as 16 hex digits"
    );
    assert_eq!(
        hub.begin(identity(), Phase::Starting, None),
        Begun::Already(Phase::Starting),
        "once per process"
    );
    hub.start_writer();
    hub.worker_running();
    // a phase change goes at once, not after the running gap that warning-only writes wait for
    assert!(
        eventually(RUNNING_GAP / 2, || read(&dir)["phase"] == "running"),
        "running reaches the file at once"
    );
    // the end is on disk when end() returns
    hub.end().unwrap();
    let v = read(&dir);
    assert_eq!(
        (v["phase"].as_str(), v["incomplete"].as_bool()),
        (Some("ended"), Some(false))
    );
    // after the end nothing changes, and the writer thread never puts an older record back
    assert_eq!(
        hub.update(|r| r.set_phase(Phase::Running, None)),
        Change::None
    );
    std::thread::sleep(Duration::from_millis(100));
    assert_eq!(read(&dir)["phase"], "ended");
    assert!(temps(&dir).is_empty());
}

#[test]
fn the_previous_starts_active_save_sensitive_packages_outlive_the_file_begin_replaces() {
    // the turned-off comparison needs them after begin, which replaces the only file that lists them
    let dir = fresh("previously-active");
    let mut previous = starting();
    let mut qs = PackageInit::selected("ammo-slots", Some("0.8.2"), Some("Quiver & Spellbook"));
    qs.save_sensitive = SaveSensitive::policy("ammo-slots", "0.8.2", None);
    qs.lua = Lua::Active;
    previous.add_package(qs);
    std::fs::write(dir.join("sfi.json"), previous.to_bytes(UPDATED)).unwrap();
    let hub = hub(&dir);
    assert!(hub.previously_active().is_empty(), "nothing before begin");
    // another process than the one that wrote it (pid 4242)
    let me = Identity {
        pid: 4243,
        ..identity()
    };
    assert_eq!(hub.begin(me, Phase::Starting, None), Begun::Written);
    assert_eq!(
        read(&dir)["packages"],
        serde_json::json!([]),
        "the file is this start's record now"
    );
    let active = hub.previously_active();
    assert_eq!(active.len(), 1, "{active:?}");
    assert_eq!(
        (
            active[0].id.as_str(),
            active[0].version.as_deref(),
            active[0].basis.as_str()
        ),
        ("ammo-slots", Some("0.8.2"), "policy")
    );
    assert_eq!(
        active[0].policy_row,
        Some(("ammo-slots".into(), "0.8.2".into()))
    );
    let off = safety::turned_off(active, |p| p.id.as_str(), |_| Now::Disabled);
    assert_eq!(off.len(), 1);
    // a second begin changes nothing, this list included
    hub.begin(identity(), Phase::Starting, None);
    assert_eq!(hub.previously_active().len(), 1);
}

// ---- aside and refused records carry the previous start's active save-sensitive list forward ----

const KILN_LAUNCH: &str = "3f2b8c1e5a7d4e2f";
/// A kiln record (launch 3f2b…, pid 4242) with two active save-sensitive packages and an active plain one.
fn kiln_record_with_qs_active(dir: &Path) {
    let mut kiln = starting();
    let mut qs = PackageInit::selected("ammo-slots", Some("0.8.2"), Some("Quiver & Spellbook"));
    qs.save_sensitive = SaveSensitive::policy("ammo-slots", "0.8.2", None);
    qs.lua = Lua::Active;
    kiln.add_package(qs);
    let mut mixed = PackageInit::selected("mixed", None, None);
    mixed.save_sensitive = SaveSensitive::unknown_mixed();
    kiln.add_package(mixed);
    kiln.set_data("mixed", Data::Committed, Some(true));
    selected(&mut kiln, "plain", Data::Committed, Lua::Active);
    std::fs::write(dir.join("sfi.json"), kiln.to_bytes(UPDATED)).unwrap();
}
/// Another start in `dir`: its own pid and launch id, the backend and first phase given.
fn next_start(dir: &Path, pid: u32, launch: u64, backend: Backend, phase: Phase) -> Arc<StateHub> {
    let hub = hub(dir);
    let me = Identity {
        pid,
        launch_id: Some(launch),
        backend,
        ..identity()
    };
    let reason = match phase {
        Phase::Aside => Some(Reason::new("hosted-default", "backend hosted")),
        Phase::Refused => Some(Reason::new("held", "kiln-sfi.interpreter is held")),
        _ => None,
    };
    assert_eq!(hub.begin(me, phase, reason), Begun::Written);
    hub
}
fn carried_qs_and_mixed() -> serde_json::Value {
    serde_json::json!([
        { "id": "ammo-slots", "version": "0.8.2", "basis": "policy",
          "policyRow": { "id": "ammo-slots", "version": "0.8.2" }, "fromLaunchId": KILN_LAUNCH },
        { "id": "mixed", "version": null, "basis": "unknown-mixed", "policyRow": null, "fromLaunchId": KILN_LAUNCH },
    ])
}
fn active_ids(hub: &StateHub) -> Vec<&str> {
    hub.previously_active()
        .iter()
        .map(|p| p.id.as_str())
        .collect()
}
/// The kiln start after the hops still finds both packages, with the start they were active in.
fn kiln_start_reports_qs_and_mixed(dir: &Path, pid: u32) {
    let kiln = next_start(
        dir,
        pid,
        0x3333_3333_3333_3333,
        Backend::Kiln,
        Phase::Starting,
    );
    assert_eq!(active_ids(&kiln), ["ammo-slots", "mixed"]);
    let active = kiln.previously_active();
    assert_eq!(
        (active[0].version.as_deref(), active[0].basis.as_str()),
        (Some("0.8.2"), "policy")
    );
    assert_eq!(
        active[0].policy_row,
        Some(("ammo-slots".into(), "0.8.2".into()))
    );
    assert_eq!(
        (
            active[1].version.as_deref(),
            active[1].basis.as_str(),
            &active[1].policy_row
        ),
        (None, "unknown-mixed", &None)
    );
    for p in active {
        assert_eq!(p.from_launch_id, Some(0x3f2b_8c1e_5a7d_4e2f), "{p:?}");
    }
    // Q&S switched off in this start: the comparison warns for it
    let off = safety::turned_off(
        active,
        |p| p.id.as_str(),
        |p| {
            if p == "ammo-slots" {
                Now::Disabled
            } else {
                Now::Selected
            }
        },
    );
    assert_eq!(off.len(), 1);
}

#[test]
fn kiln_then_hosted_then_kiln_still_knows_the_active_save_sensitive_packages() {
    let dir = fresh("carry-aside");
    kiln_record_with_qs_active(&dir);
    let aside = next_start(
        &dir,
        4301,
        0x1111_1111_1111_1111,
        Backend::Hosted,
        Phase::Aside,
    );
    let v = read(&dir);
    assert_eq!(v["phase"], "aside");
    assert_eq!(
        v["packages"],
        serde_json::json!([]),
        "aside lists no packages"
    );
    assert_eq!(v["carriedSaveSensitive"], carried_qs_and_mixed());
    assert_eq!(active_ids(&aside), ["ammo-slots", "mixed"]);
    kiln_start_reports_qs_and_mixed(&dir, 4302);
}

#[test]
fn two_hosted_starts_in_a_row_carry_the_list_through_both() {
    let dir = fresh("carry-two-hops");
    kiln_record_with_qs_active(&dir);
    next_start(
        &dir,
        4301,
        0x1111_1111_1111_1111,
        Backend::Hosted,
        Phase::Aside,
    );
    next_start(
        &dir,
        4302,
        0x2222_2222_2222_2222,
        Backend::Installed,
        Phase::Aside,
    );
    // unchanged: still the kiln start's launch id, not the first hop's
    assert_eq!(read(&dir)["carriedSaveSensitive"], carried_qs_and_mixed());
    kiln_start_reports_qs_and_mixed(&dir, 4303);
}

#[test]
fn a_kiln_start_lists_its_own_packages_and_carries_nothing() {
    let dir = fresh("carry-kiln-kiln");
    kiln_record_with_qs_active(&dir);
    let first = next_start(
        &dir,
        4301,
        0x1111_1111_1111_1111,
        Backend::Kiln,
        Phase::Starting,
    );
    assert_eq!(active_ids(&first), ["ammo-slots", "mixed"]);
    assert!(
        read(&dir).get("carriedSaveSensitive").is_none(),
        "a kiln record has no carried list"
    );
    // this start runs only the plain package, and ends
    first.update(|r| {
        r.add_package({
            let mut p = PackageInit::selected("plain", Some("1.0.0"), None);
            p.lua = Lua::Active;
            p
        })
    });
    first.end().unwrap();
    let v = read(&dir);
    assert_eq!(v["phase"], "ended");
    assert!(v.get("carriedSaveSensitive").is_none());
    let second = next_start(
        &dir,
        4302,
        0x2222_2222_2222_2222,
        Backend::Kiln,
        Phase::Starting,
    );
    assert!(
        second.previously_active().is_empty(),
        "the start before the last one is not carried by a kiln record: {:?}",
        second.previously_active()
    );
}

#[test]
fn a_refused_record_carries_the_list() {
    // refused at the plugin step
    let dir = fresh("carry-refused");
    kiln_record_with_qs_active(&dir);
    next_start(
        &dir,
        4301,
        0x1111_1111_1111_1111,
        Backend::Kiln,
        Phase::Refused,
    );
    let v = read(&dir);
    assert_eq!(v["phase"], "refused");
    assert_eq!(v["carriedSaveSensitive"], carried_qs_and_mixed());
    kiln_start_reports_qs_and_mixed(&dir, 4302);
    // refused at on_ready, after starting was written
    let dir = fresh("carry-refused-late");
    kiln_record_with_qs_active(&dir);
    let late = next_start(
        &dir,
        4301,
        0x1111_1111_1111_1111,
        Backend::Kiln,
        Phase::Starting,
    );
    late.refuse(Reason::new(
        "policy-veto",
        "the safety policy vetoes this start",
    ))
    .unwrap();
    let v = read(&dir);
    assert_eq!(v["phase"], "refused");
    assert_eq!(v["carriedSaveSensitive"], carried_qs_and_mixed());
    kiln_start_reports_qs_and_mixed(&dir, 4302);
}

#[test]
fn a_refused_record_after_starting_hands_on_its_own_active_packages_with_the_carried_list() {
    // a start refused at on_ready may already have startup data in the game: the next kiln start compares against
    // those packages too (read from packages), and against the carried list, each id once with packages first
    let dir = fresh("carry-refused-own");
    kiln_record_with_qs_active(&dir);
    let late = next_start(
        &dir,
        4301,
        0x1111_1111_1111_1111,
        Backend::Kiln,
        Phase::Starting,
    );
    late.update(|r| {
        let mut qs = PackageInit::selected("ammo-slots", Some("0.9.0"), None);
        qs.save_sensitive = SaveSensitive::policy("ammo-slots", "0.9.0", None);
        r.add_package(qs);
        r.set_data("ammo-slots", Data::Committed, Some(true));
        let mut fresh = PackageInit::selected("fresh", Some("2.0.0"), None);
        fresh.save_sensitive = SaveSensitive::unknown_mixed();
        r.add_package(fresh);
        r.set_data("fresh", Data::Committed, Some(true));
        // switched off in this start: not active here, but still carried from the kiln start before
        r.turned_off(TurnedOff {
            id: "mixed".into(),
            version: None,
            name: None,
            now: Intent::Disabled,
            save_sensitive: SaveSensitive::unknown_mixed(),
            archive_present: None,
        })
    });
    late.refuse(Reason::new(
        "policy-veto",
        "the safety policy vetoes this start",
    ))
    .unwrap();
    let v = read(&dir);
    assert_eq!(v["phase"], "refused");
    assert_eq!(
        v["carriedSaveSensitive"],
        carried_qs_and_mixed(),
        "the carried list is written unchanged"
    );
    let kiln = next_start(
        &dir,
        4302,
        0x3333_3333_3333_3333,
        Backend::Kiln,
        Phase::Starting,
    );
    let mut ids = active_ids(&kiln);
    ids.sort_unstable();
    assert_eq!(ids, ["ammo-slots", "fresh", "mixed"]);
    let by_id = |id: &str| {
        kiln.previously_active()
            .iter()
            .find(|p| p.id == id)
            .unwrap()
            .clone()
    };
    let qs = by_id("ammo-slots");
    assert_eq!(
        (qs.version.as_deref(), qs.from_launch_id),
        (Some("0.9.0"), Some(0x1111_1111_1111_1111)),
        "the refused start's own row wins over the carried one"
    );
    assert_eq!(by_id("fresh").from_launch_id, Some(0x1111_1111_1111_1111));
    assert_eq!(by_id("mixed").from_launch_id, Some(0x3f2b_8c1e_5a7d_4e2f));
}

#[test]
fn a_suspect_previous_file_carries_nothing() {
    // the hosted start's own pid comes back: a reused pid, nothing concluded from its file
    let dir = fresh("carry-suspect-pid");
    kiln_record_with_qs_active(&dir);
    next_start(
        &dir,
        4301,
        0x1111_1111_1111_1111,
        Backend::Hosted,
        Phase::Aside,
    );
    let reused = next_start(
        &dir,
        4301,
        0x2222_2222_2222_2222,
        Backend::Hosted,
        Phase::Aside,
    );
    assert!(reused.previously_active().is_empty());
    let v = read(&dir);
    assert_eq!(v["previousStart"]["disposition"], "suspect");
    assert_eq!(v["carriedSaveSensitive"], serde_json::json!([]));
    let kiln = next_start(
        &dir,
        4302,
        0x3333_3333_3333_3333,
        Backend::Kiln,
        Phase::Starting,
    );
    assert!(kiln.previously_active().is_empty());
    // an unreadable file
    for garbage in [
        &b"{not json"[..],
        b"{\"format\":1,\"phase\":\"aside\",\"packages\":[]}",
    ] {
        let dir = fresh("carry-suspect-garbage");
        std::fs::write(dir.join("sfi.json"), garbage).unwrap();
        let aside = next_start(
            &dir,
            4301,
            0x1111_1111_1111_1111,
            Backend::Hosted,
            Phase::Aside,
        );
        assert!(aside.previously_active().is_empty());
        assert_eq!(read(&dir)["carriedSaveSensitive"], serde_json::json!([]));
    }
}

#[test]
fn a_carried_list_is_bounded_and_fits_the_schema_whatever_the_file_said() {
    let dir = fresh("carry-clamp");
    kiln_record_with_qs_active(&dir);
    next_start(
        &dir,
        4301,
        0x1111_1111_1111_1111,
        Backend::Hosted,
        Phase::Aside,
    );
    let mut doc = read(&dir);
    let long = "9".repeat(100);
    let mut junk = vec![
        serde_json::json!({ "id": "bad id", "version": "1", "basis": "policy", "policyRow": null, "fromLaunchId": KILN_LAUNCH }),
        serde_json::json!({ "id": "said-none", "version": "1", "basis": "none", "policyRow": null, "fromLaunchId": KILN_LAUNCH }),
        serde_json::json!({ "id": "rowless", "version": "", "basis": "policy", "policyRow": null, "fromLaunchId": "not-an-id" }),
        serde_json::json!({ "id": "long", "version": long, "basis": "unknown-mixed", "policyRow": { "id": "long", "version": "1" } }),
        serde_json::json!({ "id": "said-none", "version": "2", "basis": "policy", "policyRow": { "id": "said-none", "version": "2" } }),
        serde_json::json!({ "version": "1", "basis": "policy" }),
    ];
    for i in 0..5000 {
        junk.push(serde_json::json!({ "id": format!("p{i}"), "version": "1.0.0", "basis": "policy",
            "policyRow": { "id": format!("p{i}"), "version": "1.0.0" }, "fromLaunchId": KILN_LAUNCH }));
    }
    doc["carriedSaveSensitive"] = serde_json::Value::Array(junk);
    std::fs::write(dir.join("sfi.json"), serde_json::to_vec(&doc).unwrap()).unwrap();
    let aside = next_start(
        &dir,
        4302,
        0x2222_2222_2222_2222,
        Backend::Hosted,
        Phase::Aside,
    );
    assert!(aside.previously_active().len() <= 4096);
    let v = read(&dir);
    let carried = v["carriedSaveSensitive"].as_array().unwrap();
    assert_eq!(carried.len(), 4096, "the schema's bound, as packages");
    let mut seen = std::collections::HashSet::new();
    for c in carried {
        let id = c["id"].as_str().unwrap();
        assert!(sf_id(id), "{c}");
        assert!(seen.insert(id.to_owned()), "{id} twice");
        assert!(
            c["version"].is_null()
                || (1..=64).contains(&c["version"].as_str().unwrap().chars().count()),
            "{c}"
        );
        match c["basis"].as_str().unwrap() {
            "policy" => assert!(c["policyRow"].is_object(), "{c}"),
            "unknown-mixed" => assert!(c["policyRow"].is_null(), "{c}"),
            other => panic!("basis {other}: a carried package is save-sensitive"),
        }
        assert!(
            c["fromLaunchId"].is_null()
                || writer::parse_launch_id(c["fromLaunchId"].as_str().unwrap()).is_some(),
            "{c}"
        );
        assert_eq!(c.as_object().unwrap().len(), 5, "{c}");
    }
    let by_id = |id: &str| carried.iter().find(|c| c["id"] == id).cloned();
    assert!(by_id("bad id").is_none());
    assert_eq!(
        by_id("said-none").unwrap()["basis"],
        "unknown-mixed",
        "said save-sensitive by being listed: the safe side"
    );
    assert_eq!(
        by_id("said-none").unwrap()["version"],
        "1",
        "the first of two rows with one id stays"
    );
    let rowless = by_id("rowless").unwrap();
    assert_eq!(
        (
            rowless["basis"].as_str(),
            rowless["version"].is_null(),
            rowless["fromLaunchId"].is_null()
        ),
        (Some("unknown-mixed"), true, true)
    );
    let long = by_id("long").unwrap();
    assert_eq!(
        (
            long["version"].as_str().unwrap().len(),
            long["policyRow"].is_null()
        ),
        (64, true)
    );
}

#[test]
fn the_tick_and_the_worker_never_wait_for_the_file() {
    // The writer thread's rename is held until this test lets it go; updates from another thread still return. Proven
    // by what happened, not by how long it took (a wall-clock bound fails whenever the test thread is descheduled):
    // nothing lets the rename go before the updates have returned, so an update that waited for the file would never
    // return. They run on their own thread, so that shows as a failure after WAIT rather than as a hang.
    static HOLD: AtomicU32 = AtomicU32::new(0);
    static INSIDE: AtomicU32 = AtomicU32::new(0);
    fn held_rename(from: &Path, to: &Path) -> std::io::Result<()> {
        if HOLD.load(Ordering::SeqCst) == 1 {
            INSIDE.store(1, Ordering::SeqCst);
            while HOLD.load(Ordering::SeqCst) == 1 {
                std::thread::sleep(Duration::from_millis(1));
            }
        }
        std::fs::rename(from, to)
    }
    let dir = fresh("nowait");
    let hub = Arc::new(StateHub::with_ops(WriteOps {
        rename: held_rename,
        sleep: |_| {},
    }));
    hub.set_dir(Some(dir.clone()));
    assert_eq!(hub.begin(identity(), Phase::Starting, None), Begun::Written);
    hub.start_writer();
    HOLD.store(1, Ordering::SeqCst);
    hub.update(|r| {
        selected(r, "a", Data::Committed, Lua::Active);
        r.set_phase(Phase::Running, None)
    });
    assert!(
        eventually(WAIT, || INSIDE.load(Ordering::SeqCst) == 1),
        "the writer thread never reached its rename"
    );
    // the writer thread is now inside its held rename; producers keep going
    let (sent, returned) = std::sync::mpsc::channel();
    {
        let hub = hub.clone();
        std::thread::spawn(move || {
            for i in 0..200 {
                hub.update(|r| r.late_loss("a", "runtime-error", format!("boom {i}")));
                hub.update(|r| {
                    r.skip(
                        &format!("p{i}"),
                        Skip::new("dll-gate", Part::Dll, "refused"),
                    )
                });
            }
            let _ = sent.send(());
        });
    }
    let returned = returned.recv_timeout(WAIT).is_ok();
    HOLD.store(0, Ordering::SeqCst);
    assert!(returned, "updates waited for IO");
    hub.end().unwrap();
    let v = read(&dir);
    assert_eq!(v["phase"], "ended");
    // coalescing dropped no warning: every skip is in the final document
    assert_eq!(
        v["packages"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|p| p["warning"].is_object())
            .count(),
        201
    );
    assert_eq!(v["warningRevision"], 201);
}

#[test]
fn a_warning_while_running_waits_out_the_gap_and_still_arrives() {
    let dir = fresh("gap");
    let hub = hub(&dir);
    hub.begin(identity(), Phase::Starting, None);
    hub.start_writer();
    hub.update(|r| {
        selected(r, "a", Data::Committed, Lua::Active);
        r.set_phase(Phase::Running, None)
    });
    assert!(eventually(WAIT, || read(&dir)["phase"] == "running"));
    let running_at = Instant::now();
    hub.update(|r| r.late_loss("a", "runtime-error", "boom"));
    std::thread::sleep(Duration::from_millis(500));
    assert_eq!(
        read(&dir)["warningRevision"],
        0,
        "a warning-only write while running waits for the gap"
    );
    assert!(
        eventually(WAIT, || read(&dir)["warningRevision"] == 1),
        "but it arrives"
    );
    assert!(
        running_at.elapsed() >= RUNNING_GAP - Duration::from_millis(600),
        "{:?}",
        running_at.elapsed()
    );
    // a phase change goes at once (and carries the warning anyway)
    hub.update(|r| r.late_loss("a", "x", "y"));
    hub.end().unwrap();
    assert_eq!(read(&dir)["phase"], "ended");
}

/// An `ownerEpoch` change while running is written at once, not RUNNING_GAP after the last write. Kiln.Ui
/// stamps a UI action with the epoch it last read here, and the runtime drops an action of an earlier epoch unread.
#[test]
fn an_owner_epoch_change_while_running_is_written_at_once() {
    let dir = fresh("epoch-now");
    let hub = hub(&dir);
    hub.begin(identity(), Phase::Starting, None);
    hub.start_writer();
    hub.update(|r| {
        selected(r, "a", Data::Committed, Lua::Active);
        r.set_lua("a", Lua::Active);
        r.set_phase(Phase::Running, None)
    });
    assert!(eventually(WAIT, || read(&dir)["phase"] == "running"));
    // right after a write, where a warning-only change waits out the gap
    assert_eq!(
        hub.update(|r| r.set_owner_epoch("a", Some(5))),
        Change::Urgent
    );
    assert!(
        eventually(RUNNING_GAP / 3, || read(&dir)["packages"][0]["ownerEpoch"]
            == 5),
        "the first epoch reaches the file at once"
    );
    // a reload's new generation, again right after a write
    hub.update(|r| r.set_owner_epoch("a", Some(6)));
    assert!(
        eventually(RUNNING_GAP / 3, || read(&dir)["packages"][0]["ownerEpoch"]
            == 6),
        "the renewed epoch reaches the file at once"
    );
    // and so does its removal (the package stopped)
    hub.update(|r| r.set_owner_epoch("a", None));
    assert!(
        eventually(RUNNING_GAP / 3, || read(&dir)["packages"][0]
            .get("ownerEpoch")
            .is_none()),
        "the cleared epoch reaches the file at once"
    );
    hub.end().unwrap();
    assert_eq!(read(&dir)["phase"], "ended");
}

#[test]
fn a_failed_background_write_is_tried_again() {
    static FAIL: AtomicU32 = AtomicU32::new(0);
    static CALLS: Mutex<Vec<Instant>> = Mutex::new(Vec::new());
    fn failing_rename(from: &Path, to: &Path) -> std::io::Result<()> {
        CALLS.lock().unwrap().push(Instant::now());
        if FAIL.load(Ordering::SeqCst) > 0 {
            FAIL.fetch_sub(1, Ordering::SeqCst);
            return Err(std::io::Error::from(std::io::ErrorKind::PermissionDenied));
        }
        std::fs::rename(from, to)
    }
    let dir = fresh("again");
    let hub = Arc::new(StateHub::with_ops(WriteOps {
        rename: failing_rename,
        sleep: |_| {},
    }));
    hub.set_dir(Some(dir.clone()));
    hub.begin(identity(), Phase::Starting, None);
    hub.start_writer();
    // more refusals than one write's 20 tries: the first background write fails whole
    CALLS.lock().unwrap().clear();
    FAIL.store(25, Ordering::SeqCst);
    // running is a phase change (urgent); its failed write still waits out the retry wait
    hub.worker_running();
    std::thread::sleep(Duration::from_millis(300));
    assert_eq!(read(&dir)["phase"], "starting");
    assert!(
        eventually(WAIT, || read(&dir)["phase"] == "running"),
        "tried again after the wait"
    );
    assert_eq!(FAIL.load(Ordering::SeqCst), 0);
    let calls = CALLS.lock().unwrap().clone();
    assert_eq!(
        calls.len(),
        26,
        "20 refused, then 5 refused and the one that got through"
    );
    assert!(
        calls[20] - calls[19] >= BACKGROUND_RETRY - Duration::from_millis(100),
        "the second write waited for the retry wait: {:?}",
        calls[20] - calls[19]
    );
    assert!(temps(&dir).is_empty());
    hub.end().unwrap();
}

#[test]
fn without_a_state_folder_begin_says_so_and_nothing_is_written() {
    let hub = StateHub::new();
    hub.set_dir(None);
    assert_eq!(
        hub.begin(identity(), Phase::Starting, None),
        Begun::NotWritten(WriteError::NoFolder)
    );
    // a folder that cannot hold files: the error, the old record in memory only
    let dir = fresh("blocked");
    let file = dir.join("not-a-folder");
    std::fs::write(&file, b"x").unwrap();
    let hub = StateHub::new();
    hub.set_dir(Some(file));
    assert!(matches!(
        hub.begin(identity(), Phase::Starting, None),
        Begun::NotWritten(WriteError::Io(_))
    ));
    assert_eq!(hub.phase(), Some(Phase::Starting));
}
