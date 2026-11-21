// SPDX-License-Identifier: MIT
//! The check before Play: `sfi_launch::prepare` / `confirm` around the start, and the launcher's own Play
//! (`play_interactive`). Nothing is opened or started (a spy counts Steam's opener and the direct starter, both) for a
//! start nobody confirmed, a cancel, a stale, replayed, expired or foreign token; a change between the dialog and the
//! confirm gives a new preflight; a confirmed start opens exactly once; no play-once flag is left by anything that does
//! not start; an acknowledgement changes nothing for the next start; a hosted start is the same with or without the
//! check (a twin of two game folders).
//!
//! Every package here is synthetic; nothing
//! here opens Steam or starts a process.

mod sfi_profile_common;
mod sfi_routing_common;

use kiln_manage::direct::DirectCommand;
use kiln_manage::launch::{self, LaunchMode, LaunchRequest, LaunchResult, Starter};
use kiln_manage::sfi_launch::{self, Book, Preflight, TOKEN_TTL};
use kiln_manage::{Error, ErrorCode, profile};
use serde_json::Value;
use sfi_profile_common::*;
use sfi_routing_common::{HOSTED, KILN_HELD, normal, put, select, snapshot, stable_json};
use std::cell::Cell;
use std::time::Duration;

/// The window that asks (the launcher's main window).
const MAIN: Option<&str> = Some("main");

/// What starts the game: Steam's opener, or the launcher's own direct start.
#[derive(Debug, Clone, Copy, PartialEq)]
enum Via {
    Steam,
    Direct,
}
const BOTH: [Via; 2] = [Via::Steam, Via::Direct];

/// Counts each time Steam's opener or the direct starter is asked.
#[derive(Default)]
struct Spy {
    opened: Cell<usize>,
}

impl Spy {
    fn with<T>(&self, via: Via, f: impl FnOnce(Starter<'_>) -> T) -> T {
        let steam = |uri: &str| {
            assert!(uri.starts_with("steam://rungameid/"), "{uri}");
            self.opened.set(self.opened.get() + 1);
            Ok(())
        };
        let cmd = DirectCommand {
            program: "umu-run-never-started".into(),
            args: Vec::new(),
            cwd: ".".into(),
            env: Vec::new(),
            env_remove: Vec::new(),
            runner: None,
            prefix: None,
        };
        let direct = |_: &DirectCommand| {
            self.opened.set(self.opened.get() + 1);
            Ok(())
        };
        match via {
            Via::Steam => f(Starter::Steam(&steam)),
            Via::Direct => f(Starter::Direct(&cmd, &direct)),
        }
    }
    fn count(&self) -> usize {
        self.opened.get()
    }
}

fn req(mode: LaunchMode) -> LaunchRequest {
    LaunchRequest {
        mode,
        profile_id: None,
        skip_backup: true,
    }
}

fn once_req(w: &World) -> LaunchRequest {
    LaunchRequest {
        mode: LaunchMode::Once,
        profile_id: Some(w.profile.clone()),
        skip_backup: true,
    }
}

/// The world without the player's own ShroudForge folder: the plan has nothing to skip.
fn clean(tag: &str) -> World {
    let w = world(tag, false);
    std::fs::remove_dir_all(w.file("mods/players-own")).unwrap();
    w
}

/// A player's own ShroudForge package whose dependency is not installed: the plan skips it.
fn needy(w: &World) {
    put(
        &w.game,
        "mods/needy/mod.json",
        br#"{"id":"needy","name":"Needy","version":"1.2.0","capabilities":["runtime"],"dependencies":[{"id":"missing-lib","version":"^1"}]}"#,
    );
    put(
        &w.game,
        "mods/needy/extended.mod.json",
        br#"{"schemaVersion":1,"enabled":true,"targets":["client","server"]}"#,
    );
    put(&w.game, "mods/needy/src/mod.lua", b"return {}\n");
}

fn activate(w: &World) {
    profile::activate(&w.dirs, &w.profile, &w.inst).unwrap_or_else(|e| panic!("{e:?}"));
}

fn flag(w: &World) -> bool {
    w.file("kiln/play-once.ini").exists()
}

fn prepare(b: &Book, w: &World, r: &LaunchRequest) -> Preflight {
    sfi_launch::prepare(b, &w.dirs, &w.inst, r, MAIN).unwrap_or_else(|e| panic!("{e:?}"))
}

fn confirm(
    b: &Book,
    w: &World,
    spy: &Spy,
    via: Via,
    token: &str,
    r: &LaunchRequest,
) -> Result<LaunchResult, Error> {
    spy.with(via, |s| {
        sfi_launch::confirm(b, &w.dirs, &w.inst, token, r, MAIN, Some(&[]), s)
    })
}

fn interactive(
    b: &Book,
    w: &World,
    spy: &Spy,
    via: Via,
    r: &LaunchRequest,
) -> Result<LaunchResult, Error> {
    spy.with(via, |s| {
        sfi_launch::play_interactive(b, &w.dirs, &w.inst, r, MAIN, Some(&[]), s)
    })
}

/// The preflight a pending-warning error carries.
fn pending(e: &Error) -> Value {
    assert_eq!(e.code, ErrorCode::ConsentRequired, "{e:?}");
    sfi_launch::preflight_of(e)
        .cloned()
        .unwrap_or_else(|| panic!("no preflight: {e:?}"))
}

fn token_of(v: &Value) -> String {
    v["token"].as_str().unwrap().to_owned()
}

fn refusal(e: &Error) -> &str {
    assert_eq!(e.code, ErrorCode::Refused, "{e:?}");
    e.detail
        .as_ref()
        .and_then(|d| d["token"].as_str())
        .unwrap_or_else(|| panic!("{e:?}"))
}

fn skipped_ids(warnings: &[sfi_launch::Warning]) -> Vec<String> {
    warnings
        .iter()
        .filter_map(|w| w.package.as_ref().map(|p| p.id.clone()))
        .collect()
}

// ---- no warnings ----

/// Nothing skipped: the preflight has no warning (no dialog), its confirm opens exactly once, and the launcher's Play
/// starts without any token, as main's start does.
#[test]
fn a_start_with_nothing_to_warn_about_asks_nothing_and_opens_once() {
    for via in BOTH {
        let w = clean(&format!("v810-none-{via:?}"));
        activate(&w);
        let b = Book::new(TOKEN_TTL);
        let spy = Spy::default();
        let inspected = inspections(&w.game);
        let p = prepare(&b, &w, &req(LaunchMode::Profile));
        assert!(p.warnings.is_empty(), "{via:?}: {:?}", p.warnings);
        assert!(!p.reinspects_every_play);
        assert_eq!(
            p.shroudforge.as_ref().unwrap()["requested"],
            true,
            "{via:?}"
        );
        assert_eq!(p.token.len(), 64);
        assert_eq!(spy.count(), 0, "{via:?}: a prepare opens nothing");
        confirm(&b, &w, &spy, via, &p.token, &req(LaunchMode::Profile))
            .unwrap_or_else(|e| panic!("{via:?}: {e:?}"));
        assert_eq!(spy.count(), 1, "{via:?}");
        interactive(&b, &w, &spy, via, &req(LaunchMode::Profile))
            .unwrap_or_else(|e| panic!("{via:?}: {e:?}"));
        assert_eq!(spy.count(), 2, "{via:?}");
        assert!(!flag(&w));
        assert_eq!(b.waiting(), 0, "{via:?}: no token left");
        // the plan was current: neither the prepare nor the confirm read the archive again
        assert_eq!(inspections(&w.game), inspected, "{via:?}");
    }
}

// ---- warnings nobody acknowledged ----

/// A package the plan skips: the launcher's Play starts nothing and answers the warning as a preflight (its package,
/// version, folder and reasons); the preflight's own confirm then opens exactly once.
#[test]
fn an_unacknowledged_warning_never_opens() {
    for via in BOTH {
        let w = clean(&format!("v810-pending-{via:?}"));
        needy(&w);
        activate(&w);
        let b = Book::new(TOKEN_TTL);
        let spy = Spy::default();
        let e = interactive(&b, &w, &spy, via, &req(LaunchMode::Profile)).unwrap_err();
        let p = pending(&e);
        assert_eq!(spy.count(), 0, "{via:?}: nothing opened");
        assert!(!flag(&w));
        let ws = p["warnings"].as_array().unwrap();
        let needy = ws
            .iter()
            .find(|x| x["package"]["id"] == "needy")
            .unwrap_or_else(|| panic!("{p:#}"));
        assert_eq!(needy["kind"], "package-skipped");
        assert_eq!(needy["package"]["version"], "1.2.0");
        assert_eq!(needy["package"]["root"], "mods/needy");
        assert!(needy["package"]["lua"].as_bool().unwrap(), "{needy:#}");
        assert!(
            needy["message"].as_str().unwrap().contains("missing-lib"),
            "{needy:#}"
        );
        assert!(
            !ws.iter().any(|x| x["package"]["id"] == "sfpack"),
            "a candidate is no warning: {p:#}"
        );
        // the same warnings from a prepare (the launcher's dialog asks for them the same way)
        let again = prepare(&b, &w, &req(LaunchMode::Profile));
        assert_eq!(
            again.warnings_sha256,
            p["warningsSha256"].as_str().unwrap(),
            "{via:?}"
        );
        assert_eq!(skipped_ids(&again.warnings), ["needy"]);
        // the older token was replaced by the newer prepare of this game
        assert_eq!(
            refusal(
                &confirm(&b, &w, &spy, via, &token_of(&p), &req(LaunchMode::Profile)).unwrap_err()
            ),
            "unknown"
        );
        assert_eq!(spy.count(), 0);
        confirm(&b, &w, &spy, via, &again.token, &req(LaunchMode::Profile))
            .unwrap_or_else(|e| panic!("{via:?}: {e:?}"));
        assert_eq!(spy.count(), 1, "{via:?}: the acknowledged start opens once");
    }
}

/// Cancel: nothing opened, no play-once flag (the play-once profile it prepared stays unread without one), the token gone.
#[test]
fn a_cancel_opens_nothing_and_leaves_no_flag() {
    for via in BOTH {
        let w = clean(&format!("v810-cancel-{via:?}"));
        needy(&w);
        activate(&w);
        let b = Book::new(TOKEN_TTL);
        let spy = Spy::default();
        for r in [req(LaunchMode::Profile), once_req(&w)] {
            // a flag an earlier request left would name the files this prepare writes: it goes
            let stale = kiln_manage::playonce::PlayOnce {
                mode: kiln_manage::playonce::PlayOnceMode::NoMods,
                created: kiln_manage::util::now_unix(),
                nonce: "0123456789abcdef0123456789abcdef".into(),
                profile: None,
            };
            kiln_manage::playonce::write(&w.game, &stale).unwrap();
            let p = prepare(&b, &w, &r);
            assert!(!p.warnings.is_empty(), "{via:?} {r:?}");
            assert!(
                !flag(&w),
                "{via:?} {r:?}: no flag before the confirm, not even an earlier one"
            );
            assert!(
                !b.cancel("another-game", &p.token),
                "a token of another game stays"
            );
            assert!(b.cancel(&w.inst.id, &p.token));
            assert_eq!(b.waiting(), 0);
            assert_eq!(
                refusal(&confirm(&b, &w, &spy, via, &p.token, &r).unwrap_err()),
                "unknown"
            );
            assert_eq!(spy.count(), 0, "{via:?} {r:?}");
            assert!(!flag(&w), "{via:?} {r:?}: a cancel leaves no flag");
        }
    }
}

/// A prepare keeps no lock while its dialog is open: right after it, the game's cook lock and the kiln-mode activation's
/// lock are free (a start with the profile, and one with another profile for once).
#[test]
fn a_prepare_keeps_no_lock_for_the_dialog() {
    let w = clean("v810-locks");
    needy(&w);
    activate(&w);
    let b = Book::new(TOKEN_TTL);
    for r in [req(LaunchMode::Profile), once_req(&w)] {
        let p = prepare(&b, &w, &r);
        assert!(!p.warnings.is_empty(), "{r:?}");
        // (each waited for a moment: a child process of another test holds copies of every descriptor until its exec)
        let cook = kiln_manage::state::lock_game(&w.game, Duration::from_secs(3), &|| false)
            .unwrap_or_else(|e| panic!("{r:?}: the game's cook lock is still held: {e:?}"));
        assert!(cook.is_some(), "{r:?}");
        drop(cook);
        let journal = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(w.file("kiln/state/sfi-activation.lock"))
            .unwrap();
        let free = (0..30).any(|_| {
            journal.try_lock().is_ok() || {
                std::thread::sleep(Duration::from_millis(100));
                false
            }
        });
        assert!(free, "{r:?}: the activation's lock is still held");
        drop(journal);
    }
}

/// One token, one start: its replay is refused and opens nothing, also after the start it confirmed.
#[test]
fn a_token_confirms_one_start_and_never_another() {
    for via in BOTH {
        let w = clean(&format!("v810-replay-{via:?}"));
        needy(&w);
        activate(&w);
        let b = Book::new(TOKEN_TTL);
        let spy = Spy::default();
        let p = prepare(&b, &w, &req(LaunchMode::Profile));
        confirm(&b, &w, &spy, via, &p.token, &req(LaunchMode::Profile)).unwrap();
        assert_eq!(spy.count(), 1);
        let e = confirm(&b, &w, &spy, via, &p.token, &req(LaunchMode::Profile)).unwrap_err();
        assert_eq!(refusal(&e), "unknown", "{via:?}");
        assert_eq!(spy.count(), 1, "{via:?}: the replay opened nothing");
        // a launcher started again knows no earlier token
        let p = prepare(&b, &w, &req(LaunchMode::Profile));
        let restarted = Book::new(TOKEN_TTL);
        assert_eq!(
            refusal(
                &confirm(
                    &restarted,
                    &w,
                    &spy,
                    via,
                    &p.token,
                    &req(LaunchMode::Profile)
                )
                .unwrap_err()
            ),
            "unknown"
        );
        assert_eq!(spy.count(), 1);
    }
}

/// A token is good only for its game, its start and its window, and only for its time; each refusal takes it and opens
/// nothing.
#[test]
fn a_token_is_refused_for_another_game_start_window_or_after_its_time() {
    for via in BOTH {
        let a = clean(&format!("v810-own-a-{via:?}"));
        let other = clean(&format!("v810-own-b-{via:?}"));
        for w in [&a, &other] {
            needy(w);
            activate(w);
        }
        let b = Book::new(TOKEN_TTL);
        let spy = Spy::default();
        let p = prepare(&b, &a, &req(LaunchMode::Profile));
        let e = confirm(&b, &other, &spy, via, &p.token, &req(LaunchMode::Profile)).unwrap_err();
        assert_eq!(refusal(&e), "game");
        let p = prepare(&b, &a, &req(LaunchMode::Profile));
        assert_eq!(
            refusal(&confirm(&b, &a, &spy, via, &p.token, &req(LaunchMode::NoMods)).unwrap_err()),
            "request"
        );
        let p = prepare(&b, &a, &req(LaunchMode::Profile));
        let e = spy.with(via, |s| {
            sfi_launch::confirm(
                &b,
                &a.dirs,
                &a.inst,
                &p.token,
                &req(LaunchMode::Profile),
                Some("deck"),
                Some(&[]),
                s,
            )
        });
        assert_eq!(refusal(&e.unwrap_err()), "caller");
        // each refusal took its token
        assert_eq!(b.waiting(), 0);
        let short = Book::new(Duration::from_millis(200));
        let p = prepare(&short, &a, &req(LaunchMode::Profile));
        std::thread::sleep(Duration::from_millis(400));
        assert_eq!(
            refusal(
                &confirm(&short, &a, &spy, via, &p.token, &req(LaunchMode::Profile)).unwrap_err()
            ),
            "expired"
        );
        assert_eq!(spy.count(), 0, "{via:?}");
        assert!(!flag(&a) && !flag(&other));
    }
}

// ---- a change between the dialog and the confirm ----

/// Whatever the warnings were made from changes after the dialog showed them (the policy, a package's files, the
/// warnings themselves, a run of the game): the confirm starts nothing and answers a new preflight, whose own confirm
/// then opens once. A change that leaves nothing to warn about starts at once.
#[test]
fn a_change_between_the_dialog_and_the_confirm_asks_again() {
    type Change = fn(&World);
    let cases: [(&str, Change); 4] = [
        ("kiln.json's native policy", |w| {
            put(&w.game, "kiln/kiln.json", br#"{"preview": ["kiln-sfi.interpreter"], "shroudforge": {"backend": "kiln"}, "native": {"policy": "never"}}"#)
        }),
        ("a file of the skipped package", |w| {
            put(&w.game, "mods/needy/src/extra.lua", b"return 1\n")
        }),
        ("the warnings: a second package skipped", |w| {
            put(&w.game, "mods/needy2/mod.json", br#"{"id":"needy2","name":"Needy 2","version":"1.0.0","capabilities":["runtime"],"dependencies":[{"id":"missing-lib","version":"^1"}]}"#);
            put(
                &w.game,
                "mods/needy2/extended.mod.json",
                br#"{"schemaVersion":1,"enabled":true,"targets":["client","server"]}"#,
            );
            put(&w.game, "mods/needy2/src/mod.lua", b"return {}\n");
        }),
        ("a run of the game (its boot.json)", |w| {
            put(&w.game, "kiln/state/boot.json", b"{}")
        }),
    ];
    for via in BOTH {
        for (what, change) in cases {
            for once in [false, true] {
                // a world each: the change happens once
                let w = clean(&format!("v810-stale-{via:?}-{once}"));
                needy(&w);
                activate(&w);
                let b = Book::new(TOKEN_TTL);
                let spy = Spy::default();
                let r = if once {
                    once_req(&w)
                } else {
                    req(LaunchMode::Profile)
                };
                let p = prepare(&b, &w, &r);
                assert!(!p.warnings.is_empty(), "{what}");
                change(&w);
                let e = confirm(&b, &w, &spy, via, &p.token, &r).unwrap_err();
                let fresh = pending(&e);
                assert_ne!(token_of(&fresh), p.token, "{via:?} {what} {r:?}");
                assert_eq!(spy.count(), 0, "{via:?} {what} {r:?}: nothing opened");
                assert!(!flag(&w), "{via:?} {what} {r:?}: no flag");
                assert!(!fresh["warnings"].as_array().unwrap().is_empty());
                confirm(&b, &w, &spy, via, &token_of(&fresh), &r)
                    .unwrap_or_else(|e| panic!("{via:?} {what} {r:?}: {e:?}"));
                assert_eq!(
                    spy.count(),
                    1,
                    "{via:?} {what} {r:?}: the new acknowledgement opens once"
                );
                assert_eq!(flag(&w), once, "{via:?} {what} {r:?}");
            }
        }
    }
    // the package's dependency arrives during the dialog: nothing is left to acknowledge, so the start goes on
    for via in BOTH {
        let w = clean(&format!("v810-healed-{via:?}"));
        needy(&w);
        activate(&w);
        let b = Book::new(TOKEN_TTL);
        let spy = Spy::default();
        let p = prepare(&b, &w, &req(LaunchMode::Profile));
        std::fs::remove_dir_all(w.file("mods/needy")).unwrap();
        confirm(&b, &w, &spy, via, &p.token, &req(LaunchMode::Profile))
            .unwrap_or_else(|e| panic!("{via:?}: {e:?}"));
        assert_eq!(spy.count(), 1);
    }
}

// ---- play-once ----

/// A play-once start: its prepare writes the play-once profile and plan but no flag; its confirm boots exactly that
/// plan (no second archive inspection) and writes the flag right before it opens.
#[test]
fn a_play_once_start_flags_only_at_its_confirm_and_boots_the_plan_its_prepare_made() {
    for via in BOTH {
        let w = clean(&format!("v810-once-{via:?}"));
        needy(&w);
        activate(&w);
        let b = Book::new(TOKEN_TTL);
        let spy = Spy::default();
        let before = inspections(&w.game);
        let p = prepare(&b, &w, &once_req(&w));
        assert_eq!(
            inspections(&w.game),
            before + 1,
            "the play-once plan: one inspection"
        );
        assert!(!flag(&w));
        assert_eq!(skipped_ids(&p.warnings), ["needy"]);
        let once = std::fs::read(w.file("kiln/profile.once.json")).unwrap();
        pinned_plan(
            &w.game,
            &w.doc(kiln_manage::sfi_plan::Mode::Once),
            kiln_manage::sfi_plan::Mode::Once,
        );
        let r = confirm(&b, &w, &spy, via, &p.token, &once_req(&w))
            .unwrap_or_else(|e| panic!("{via:?}: {e:?}"));
        assert_eq!(spy.count(), 1);
        assert!(flag(&w), "{via:?}: the flag of the confirmed start");
        assert!(r.nonce.is_some());
        assert_eq!(
            std::fs::read(w.file("kiln/profile.once.json")).unwrap(),
            once,
            "{via:?}: the prepared file"
        );
        assert_eq!(
            inspections(&w.game),
            before + 1,
            "{via:?}: no second inspection"
        );
    }
}

/// A save backup that fails after the check: no flag left, nothing opened (main's rule, with the check before it).
#[test]
fn a_failed_backup_leaves_no_flag_and_opens_nothing() {
    for via in BOTH {
        let w = clean(&format!("v810-backup-{via:?}"));
        needy(&w);
        activate(&w);
        let b = Book::new(TOKEN_TTL);
        let spy = Spy::default();
        // a save folder that is a file: the backup cannot read it
        put(&w.root, "saves-file", b"not a folder");
        let saves = [kiln_manage::saves::SaveDir {
            label: "steam-1".into(),
            path: w.root.join("saves-file"),
            steam_id: Some("1".into()),
            recursive: false,
        }];
        let r = LaunchRequest {
            skip_backup: false,
            ..once_req(&w)
        };
        let p = prepare(&b, &w, &r);
        let e = spy
            .with(via, |s| {
                sfi_launch::confirm(&b, &w.dirs, &w.inst, &p.token, &r, MAIN, Some(&saves), s)
            })
            .unwrap_err();
        assert!(e.message.contains("save backup"), "{e:?}");
        assert_eq!(spy.count(), 0);
        assert!(!flag(&w), "{via:?}: the flag goes with the failed backup");
    }
}

// ---- what an acknowledgement does not do ----

/// An acknowledged start switches nothing on and changes no setting: kiln.json, the profile file and its plan are as they
/// were, and the next Play asks again with the same warnings.
#[test]
fn an_acknowledgement_changes_nothing_for_the_next_start() {
    let w = clean("v810-ack");
    needy(&w);
    activate(&w);
    let b = Book::new(TOKEN_TTL);
    let spy = Spy::default();
    let p = prepare(&b, &w, &req(LaunchMode::Profile));
    let kiln_json = std::fs::read(w.file("kiln/kiln.json")).unwrap();
    let profile_json = std::fs::read(w.file("kiln/profile.json")).unwrap();
    let plans = w.plans();
    confirm(
        &b,
        &w,
        &spy,
        Via::Steam,
        &p.token,
        &req(LaunchMode::Profile),
    )
    .unwrap();
    assert_eq!(std::fs::read(w.file("kiln/kiln.json")).unwrap(), kiln_json);
    assert_eq!(
        std::fs::read(w.file("kiln/profile.json")).unwrap(),
        profile_json
    );
    assert_eq!(w.plans(), plans);
    let e = interactive(&b, &w, &spy, Via::Steam, &req(LaunchMode::Profile)).unwrap_err();
    assert_eq!(
        pending(&e)["warningsSha256"].as_str(),
        Some(p.warnings_sha256.as_str()),
        "the same warnings again"
    );
    assert_eq!(spy.count(), 1);
}

// ---- what the warnings say ----

/// No plan could be made (the inspector refuses): no ShroudForge mod runs this start, the warning names them, and the
/// preflight says when Kiln tries again: the refusal is answered again (no archive read) until the game, the mods or
/// Kiln change, or the profile is activated.
#[test]
fn a_refused_plan_warns_for_every_shroudforge_mod_and_says_when_it_is_tried_again() {
    let w = clean("v810-refused");
    refuse_inspection(&w.game, true);
    activate(&w);
    let b = Book::new(TOKEN_TTL);
    let n = inspections(&w.game);
    let p = prepare(&b, &w, &req(LaunchMode::Profile));
    assert_eq!(
        inspections(&w.game),
        n,
        "the prepare's refresh answers the refusal again"
    );
    assert_eq!(p.warnings.len(), 1, "{:?}", p.warnings);
    let x = &p.warnings[0];
    assert_eq!(
        (x.kind.as_str(), x.code.as_str()),
        ("start-refused", "no-archive-base"),
        "{x:?}"
    );
    assert_eq!(x.packages, ["sfpack"]);
    assert!(!p.reinspects_every_play);
    let a = p
        .advisories
        .iter()
        .find(|a| a.code == "retries-on-change")
        .unwrap_or_else(|| panic!("{:?}", p.advisories));
    assert!(
        a.message.contains(
            "Kiln tries again when the game, the mods or Kiln change, or when you activate the profile"
        ),
        "{a:?}"
    );
    assert!(
        !p.advisories
            .iter()
            .any(|a| a.code == "reinspects-every-play")
    );
    refuse_inspection(&w.game, false);
}

/// A plan that could not be written: every Play tries again (and reads the archive), and the preflight says so.
#[test]
fn a_plan_that_cannot_be_written_says_every_play_inspects_again() {
    let w = clean("v810-write-failed");
    block_plans(&w);
    activate(&w);
    let b = Book::new(TOKEN_TTL);
    let n = inspections(&w.game);
    let p = prepare(&b, &w, &req(LaunchMode::Profile));
    assert!(
        inspections(&w.game) > n,
        "the prepare's refresh tried again"
    );
    assert!(p.reinspects_every_play);
    assert!(
        p.advisories
            .iter()
            .any(|a| a.code == "reinspects-every-play"),
        "{:?}",
        p.advisories
    );
    assert!(!p.advisories.iter().any(|a| a.code == "retries-on-change"));
    unblock_plans(&w);
}

/// kiln.json asks for the interpreter with no "preview". While this build's gate list holds it (a preview): the core
/// starts none of the ShroudForge mods, and Play says so. A build whose list has no row of it (a normal choice): the
/// start is a kiln start with nothing to warn about.
#[test]
fn the_interpreter_without_preview_follows_this_builds_gate_list() {
    let preview = kiln_manage::sfi_release::ReleasePolicy::shipped()
        .rows()
        .unwrap()
        .iter()
        .any(|r| r.name == kiln_manage::sfi_release::INTERPRETER);
    let w = clean("v810-held");
    select(&w.game, Some(KILN_HELD));
    activate(&w);
    let b = Book::new(TOKEN_TTL);
    let p = prepare(&b, &w, &req(LaunchMode::Profile));
    let sf = p.shroudforge.as_ref().unwrap();
    assert_eq!(sf["requested"], true, "{sf}");
    if preview {
        assert_eq!(p.warnings.len(), 1, "{:?}", p.warnings);
        assert_eq!(
            (p.warnings[0].kind.as_str(), p.warnings[0].code.as_str()),
            ("start-refused", "interpreter-held")
        );
        assert_eq!((sf["route"].as_str(), sf["refused"]["code"].as_str()), (Some("main"), Some("interpreter-held")), "{sf}");
    } else {
        assert!(p.warnings.is_empty(), "a normal choice: no held warning: {:?}", p.warnings);
        assert_eq!((sf["route"].as_str(), sf.get("refused")), (Some("interpreter"), None), "{sf}");
    }
    assert!(!p.reinspects_every_play);
}

/// The activation a prepare's refresh made (the profile changed since its activation) is the start's to show: the
/// preflight carries it, and so does the confirmed start's result.
#[test]
fn the_activation_a_prepare_made_reaches_the_confirmed_start() {
    let w = clean("v810-activation");
    needy(&w);
    activate(&w);
    w.select(1);
    let b = Book::new(TOKEN_TTL);
    let spy = Spy::default();
    let p = prepare(&b, &w, &req(LaunchMode::Profile));
    let a = p
        .activation
        .clone()
        .expect("the refresh activated the changed profile");
    assert_eq!(a.generation, 2);
    let r = confirm(
        &b,
        &w,
        &spy,
        Via::Steam,
        &p.token,
        &req(LaunchMode::Profile),
    )
    .unwrap();
    assert_eq!(r.activation.map(|x| x.generation), Some(2));
}

/// Starts without mods have no warning; their flag is written only by the confirm.
#[test]
fn starts_without_mods_have_no_warning_and_flag_only_at_the_confirm() {
    for via in BOTH {
        // not activated: no plugin loader in the game folder (a start without mods refuses one)
        let w = clean(&format!("v810-vanilla-{via:?}"));
        needy(&w);
        let b = Book::new(TOKEN_TTL);
        let spy = Spy::default();
        for mode in [LaunchMode::Vanilla, LaunchMode::NoMods] {
            let p = prepare(&b, &w, &req(mode));
            assert!(p.warnings.is_empty() && p.shroudforge.is_none(), "{mode:?}");
            assert!(!flag(&w), "{via:?} {mode:?}");
            confirm(&b, &w, &spy, via, &p.token, &req(mode)).unwrap();
            assert!(flag(&w), "{via:?} {mode:?}");
            std::fs::remove_file(w.file("kiln/play-once.ini")).unwrap();
        }
        assert_eq!(spy.count(), 2);
    }
}

// ---- what was active before, the inspector, the archive base ----

/// kiln/state/sfi.json of a kiln start that ran `sfpack` (a candidate here) and `ammo` (gone now), both save-sensitive,
/// and `plain` (not save-sensitive) and `chosen-off` (not selected): only those two count as active before.
fn ran_before(w: &World) {
    put(
        &w.game,
        "kiln/state/sfi.json",
        br#"{"format":1,"phase":"ended","incomplete":false,"launch":{"launchId":"00000000000000ab","pid":7},
        "packages":[
          {"id":"sfpack","version":"1.0.0","intent":"selected","runtimeActive":true,"archivePresent":true,
           "saveSensitive":{"value":true,"basis":"unknown-mixed"}},
          {"id":"ammo","version":"0.8.2","name":"Quiver","intent":"selected","runtimeActive":false,"archivePresent":true,
           "saveSensitive":{"value":true,"basis":"policy","policyRow":{"id":"ammo","version":"0.8.2"}}},
          {"id":"plain","version":"1.0.0","intent":"selected","runtimeActive":true,"archivePresent":true,
           "saveSensitive":{"value":false,"basis":"none"}},
          {"id":"chosen-off","version":"1.0.0","intent":"disabled","runtimeActive":false,"archivePresent":true,
           "saveSensitive":{"value":true,"basis":"policy"}}
        ]}"#,
    );
}

fn off_ids(warnings: &[sfi_launch::Warning]) -> Vec<String> {
    warnings
        .iter()
        .filter(|w| w.kind == "previously-active-off")
        .map(|w| w.previous.as_ref().unwrap().id.clone())
        .collect()
}

/// A save-sensitive package that was active at an earlier start and does not run this one is a save warning
/// before Play, on every kind of start (vanilla and without mods too, which have no plan); one that runs is not; the
/// launcher's Play opens nothing until it is acknowledged; a malformed record says nothing.
#[test]
fn a_save_sensitive_package_active_before_and_off_now_is_warned_on_every_start() {
    for via in BOTH {
        let w = clean(&format!("v813-before-{via:?}"));
        activate(&w);
        ran_before(&w);
        let b = Book::new(TOKEN_TTL);
        let spy = Spy::default();
        let p = prepare(&b, &w, &req(LaunchMode::Profile));
        assert_eq!(off_ids(&p.warnings), ["ammo"], "{:?}", p.warnings);
        let ammo = p
            .warnings
            .iter()
            .find(|x| x.kind == "previously-active-off")
            .unwrap();
        assert_eq!(ammo.severity, "save-warning");
        assert_eq!(ammo.code, "save-sensitive-now-off");
        assert!(ammo.message.contains("Quiver (ammo 0.8.2)"), "{ammo:?}");
        let prev = ammo.previous.as_ref().unwrap();
        assert_eq!(
            (prev.basis.as_str(), prev.from_launch_id.as_deref()),
            ("policy", Some("00000000000000ab"))
        );
        // starts without mods (a game folder with no plugin loader in it: not activated, as such a start needs)
        let bare = clean(&format!("v813-before-bare-{via:?}"));
        ran_before(&bare);
        let w = &bare;
        for mode in [LaunchMode::Vanilla, LaunchMode::NoMods] {
            let p = prepare(&b, &w, &req(mode));
            assert_eq!(off_ids(&p.warnings), ["sfpack", "ammo"], "{mode:?}");
            let e = interactive(&b, &w, &spy, via, &req(mode)).unwrap_err();
            let pf = pending(&e);
            let ids: Vec<&str> = pf["warnings"]
                .as_array()
                .unwrap()
                .iter()
                .filter(|x| x["kind"] == "previously-active-off")
                .filter_map(|x| x["previous"]["id"].as_str())
                .collect();
            assert_eq!(ids, ["sfpack", "ammo"], "{pf:#}");
            assert_eq!(
                spy.count(),
                0,
                "{via:?} {mode:?}: nothing opened before the acknowledgement"
            );
            assert!(!flag(&w));
            let p = prepare(&b, &w, &req(mode));
            confirm(&b, &w, &spy, via, &p.token, &req(mode))
                .unwrap_or_else(|e| panic!("{mode:?}: {e:?}"));
            assert_eq!(
                spy.count(),
                1,
                "{via:?} {mode:?}: the acknowledged start opens once"
            );
            spy.opened.set(0);
            std::fs::remove_file(w.file("kiln/play-once.ini")).unwrap();
        }
        let w = &w_profile_world(&format!("v813-before-aside-{via:?}"));
        // an aside record (a hosted start) hands the list on: still warned
        put(
            &w.game,
            "kiln/state/sfi.json",
            br#"{"format":1,"phase":"aside","incomplete":false,"launch":{"launchId":"00000000000000ac"},"packages":[],
            "carriedSaveSensitive":[{"id":"ammo","version":"0.8.2","basis":"policy","fromLaunchId":"00000000000000ab"}]}"#,
        );
        let p = prepare(&b, &w, &req(LaunchMode::Profile));
        assert_eq!(off_ids(&p.warnings), ["ammo"]);
        assert_eq!(
            p.warnings[0]
                .previous
                .as_ref()
                .unwrap()
                .from_launch_id
                .as_deref(),
            Some("00000000000000ab")
        );
        // a record kiln-sfi would not read (no incomplete member): nothing
        put(
            &w.game,
            "kiln/state/sfi.json",
            br#"{"format":1,"phase":"ended","packages":[{"id":"ammo","intent":"selected","archivePresent":true,
            "saveSensitive":{"value":true,"basis":"policy"}}]}"#,
        );
        assert!(
            prepare(&b, w, &req(LaunchMode::Profile))
                .warnings
                .is_empty()
        );
        std::fs::remove_file(w.file("kiln/state/sfi.json")).unwrap();
        assert!(
            prepare(&b, w, &req(LaunchMode::Profile))
                .warnings
                .is_empty()
        );
        // and the same malformed record on a start without mods
        put(
            &bare.game,
            "kiln/state/sfi.json",
            br#"{"format":1,"phase":"ended","packages":[{"id":"ammo","intent":"selected","archivePresent":true,
            "saveSensitive":{"value":true,"basis":"policy"}}]}"#,
        );
        assert!(
            prepare(&b, &bare, &req(LaunchMode::Vanilla))
                .warnings
                .is_empty()
        );
    }
}

/// A package the plan skips that was active before carries that on its own warning, which becomes a save warning; no
/// second line for it. A change of the record between the dialog and the confirm asks again.
#[test]
fn a_skipped_package_active_before_is_one_save_warning_and_a_changed_record_asks_again() {
    let w = clean("v813-skipped-before");
    needy(&w);
    activate(&w);
    put(
        &w.game,
        "kiln/state/sfi.json",
        br#"{"format":1,"phase":"ended","incomplete":true,"launch":{"launchId":"00000000000000ad"},"packages":[
          {"id":"needy","version":"1.2.0","intent":"selected","runtimeActive":true,"archivePresent":false,
           "saveSensitive":{"value":true,"basis":"unknown-mixed"}}]}"#,
    );
    let b = Book::new(TOKEN_TTL);
    let spy = Spy::default();
    let p = prepare(&b, &w, &req(LaunchMode::Profile));
    assert_eq!(skipped_ids(&p.warnings), ["needy"]);
    assert!(off_ids(&p.warnings).is_empty(), "{:?}", p.warnings);
    let needy = p
        .warnings
        .iter()
        .find(|x| x.kind == "package-skipped")
        .unwrap();
    assert_eq!(needy.severity, "save-warning");
    assert_eq!(needy.previous.as_ref().unwrap().basis, "unknown-mixed");
    // the record changes (another package was active too) while the dialog is open: a new preflight, nothing opened
    ran_before(&w);
    let e = confirm(
        &b,
        &w,
        &spy,
        Via::Steam,
        &p.token,
        &req(LaunchMode::Profile),
    )
    .unwrap_err();
    let fresh = pending(&e);
    assert!(
        fresh["warnings"]
            .as_array()
            .unwrap()
            .iter()
            .any(|x| x["kind"] == "previously-active-off" && x["previous"]["id"] == "ammo"),
        "{fresh:#}"
    );
    assert_eq!(spy.count(), 0);
    confirm(
        &b,
        &w,
        &spy,
        Via::Steam,
        &token_of(&fresh),
        &req(LaunchMode::Profile),
    )
    .unwrap();
    assert_eq!(spy.count(), 1);
}

/// An installed inspector that cannot be resolved now leaves the pinned plan in use (a refresh keeps it), and
/// the check before Play says why; resolvable again, nothing is said.
#[test]
fn an_inspector_that_cannot_be_resolved_is_warned_with_its_reason() {
    let w = clean("v813-inspector");
    activate(&w);
    let b = Book::new(TOKEN_TTL);
    assert!(
        prepare(&b, &w, &req(LaunchMode::Profile))
            .warnings
            .is_empty()
    );
    unresolvable_inspector(&w.game, true);
    let p = prepare(&b, &w, &req(LaunchMode::Profile));
    unresolvable_inspector(&w.game, false);
    assert_eq!(p.warnings.len(), 1, "{:?}", p.warnings);
    let x = &p.warnings[0];
    assert_eq!(
        (x.kind.as_str(), x.code.as_str(), x.severity.as_str()),
        ("plan-warning", "inspector-unresolved", "warning")
    );
    assert!(x.message.contains(UNRESOLVED), "{x:?}");
    // the plan is still the one the start boots: no start-refused
    assert!(
        p.shroudforge.as_ref().unwrap().get("plan").is_some(),
        "{:?}",
        p.shroudforge
    );
    assert!(
        prepare(&b, &w, &req(LaunchMode::Profile))
            .warnings
            .is_empty()
    );
}

/// The archive base the pinned plan was made on is an advisory in its exact words: shown, never a reason for the dialog.
#[test]
fn the_archive_base_origin_is_an_advisory_in_its_exact_words() {
    let w = clean("v813-origin");
    activate(&w);
    let b = Book::new(TOKEN_TTL);
    for r in [req(LaunchMode::Profile), once_req(&w)] {
        let p = prepare(&b, &w, &r);
        assert!(p.warnings.is_empty(), "{r:?}: {:?}", p.warnings);
        let a = p
            .advisories
            .iter()
            .find(|a| a.kind == "base-origin")
            .unwrap_or_else(|| panic!("{r:?}: {:?}", p.advisories));
        assert_eq!(a.code, "first-managed-unclassified");
        assert_eq!(
            a.message,
            "unclassified: game files as found when Kiln first managed this install"
        );
        assert_eq!(a.severity, "info");
    }
    // vanilla (a game folder not activated, as such a start needs): no plan, no base
    let bare = clean("v813-origin-bare");
    assert!(
        prepare(&b, &bare, &req(LaunchMode::Vanilla))
            .advisories
            .is_empty()
    );
}

/// A package that runs and declares a keyboard shortcut setting gets an advisory (kiln mode does not read shortcuts
/// yet): shown, never a reason for the dialog by itself, and printed by a headless start. A package without one
/// (`sfpack`'s slider) and a package that does not run get none.
#[test]
fn a_running_package_with_a_keyboard_shortcut_is_an_advisory_only() {
    const LINE: &str = "hotkeys 1.0.0: its keyboard shortcuts do not work in kiln mode yet; the rest of the mod runs";
    let keys = |a: &[sfi_launch::Warning]| -> Vec<sfi_launch::Warning> {
        a.iter().filter(|a| a.code == sfi_launch::KEYBINDS_UNSUPPORTED).cloned().collect()
    };
    let w = clean("keybinds");
    put(
        &w.game,
        "mods/hotkeys/mod.json",
        br#"{"id":"hotkeys","name":"Hotkeys","version":"1.0.0","capabilities":["runtime"]}"#,
    );
    put(
        &w.game,
        "mods/hotkeys/extended.mod.json",
        br#"{"schemaVersion":1,"enabled":true,"targets":["client","server"],
        "settings":{"speed":3,"open":{"label":"Open","control":"keybind","value":"SDL:59"}}}"#,
    );
    put(&w.game, "mods/hotkeys/src/mod.lua", b"return {}\n");
    // a package the plan skips declares one too: it does not run, so it gets no advisory
    needy(&w);
    put(
        &w.game,
        "mods/needy/extended.mod.json",
        br#"{"schemaVersion":1,"enabled":true,"targets":["client","server"],
        "settings":{"open":{"label":"Open","control":"keybind","value":"SDL:60"}}}"#,
    );
    activate(&w);
    let b = Book::new(TOKEN_TTL);
    let r = req(LaunchMode::Profile);
    let p = prepare(&b, &w, &r);
    let a = keys(&p.advisories);
    assert_eq!(a.len(), 1, "{:?}", p.advisories);
    assert_eq!(a[0].message, LINE);
    assert_eq!(a[0].severity, "info");
    assert_eq!(a[0].packages, ["hotkeys 1.0.0"]);
    assert!(keys(&p.warnings).is_empty(), "{:?}", p.warnings);
    assert_eq!(skipped_ids(&p.warnings), ["needy"]);
    // a headless start prints it as a note
    let lines = sfi_launch::headless_warnings(&w.dirs, &w.inst, &r).lines();
    assert!(
        lines.contains(&format!("note: plan-warning ({}): {LINE}", sfi_launch::KEYBINDS_UNSUPPORTED)),
        "{lines:?}"
    );
    // by itself it asks nothing: without the skipped package there is no warning, and the advisory stays
    std::fs::remove_dir_all(w.file("mods/needy")).unwrap();
    activate(&w);
    let p = prepare(&b, &w, &r);
    assert!(p.warnings.is_empty(), "{:?}", p.warnings);
    assert_eq!(keys(&p.advisories).len(), 1, "{:?}", p.advisories);
}

/// An activated world for the profile start (its plan pins `sfpack`, a candidate).
fn w_profile_world(tag: &str) -> World {
    let w = clean(tag);
    activate(&w);
    w
}

// ---- hosted: the same start with or without the check ----

/// Two hosted game folders: one starts through main's `play_via`, the other through prepare and confirm (and the
/// launcher's Play), with a package the plan would skip in kiln mode. No warning, and the game folders end the same.
#[test]
fn a_hosted_start_is_the_same_with_or_without_the_check() {
    for via in BOTH {
        let twins: Vec<World> = ["main", "checked"]
            .iter()
            .map(|t| clean(&format!("v810-twin-{t}-{via:?}")))
            .collect();
        for w in &twins {
            select(&w.game, Some(HOSTED));
            needy(w);
            activate(w);
        }
        let spy = Spy::default();
        let (main, checked) = (&twins[0], &twins[1]);
        for r in [req(LaunchMode::Profile), once_req(main)] {
            spy.with(via, |s| {
                launch::play_via(&main.dirs, &main.inst, &r, Some(&[]), s)
            })
            .unwrap();
        }
        let b = Book::new(TOKEN_TTL);
        for r in [req(LaunchMode::Profile), once_req(checked)] {
            let p = prepare(&b, checked, &r);
            assert!(p.warnings.is_empty(), "{via:?}: hosted: {:?}", p.warnings);
            assert_eq!(p.shroudforge.as_ref().unwrap()["requested"], false);
            confirm(&b, checked, &spy, via, &p.token, &r).unwrap();
        }
        interactive(&b, checked, &spy, via, &req(LaunchMode::Profile)).unwrap();
        spy.with(via, |s| {
            launch::play_via(
                &main.dirs,
                &main.inst,
                &req(LaunchMode::Profile),
                Some(&[]),
                s,
            )
        })
        .unwrap();
        assert_eq!(spy.count(), 6);
        assert_eq!(twin_tree(main), twin_tree(checked), "{via:?}");
        assert!(!w_has_plans(main) && !w_has_plans(checked));
    }
}

/// A twin's game folder for the comparison: each path (with '/') and its text, the times of JSON files taken out, the
/// twin's own temporary folder written `<T>` however a file spells it (`normal`, and its folder name wherever it
/// stands), the play-once flag (its time and nonce) left out.
fn twin_tree(w: &World) -> std::collections::BTreeMap<String, String> {
    let own = w.root.file_name().unwrap().to_string_lossy().into_owned();
    snapshot(&w.game)
        .into_iter()
        .map(|(rel, v)| (rel.replace('\\', "/"), v))
        .filter(|(rel, _)| rel != "kiln/play-once.ini")
        .map(|(rel, v)| {
            let text = if rel.ends_with('/') || v.starts_with("-> ") {
                v
            } else {
                let bytes = std::fs::read(w.game.join(&rel)).unwrap();
                match serde_json::from_slice::<Value>(&bytes) {
                    Ok(j) if rel.ends_with(".json") => stable_json(&j, &w.root),
                    _ => match std::str::from_utf8(&bytes) {
                        Ok(s) => normal(s, &w.root),
                        Err(_) => kiln_manage::hash::sha256_hex(&bytes),
                    },
                }
            };
            (rel, text.replace(&own, "<T>"))
        })
        .collect()
}

fn w_has_plans(w: &World) -> bool {
    !w.plans().is_empty()
}
