// SPDX-License-Identifier: MIT
// the interpreter's UI bridge, the runtime's side of its files.
// Behaviour checked against ShroudForge (MIT, Copyright (c) 2026 bonsaibauer), 1.7.9 (the same in 1.8.0):
// modules/mod-runtime/lua/shroudforge/v1/dispatch_mod_ui_actions.rs:65-107 and its queue test (:185-235),
// create_mod_api.rs:232-263, modules/notifications/write_notification_event.rs:4-21.
//! kiln-sfi's end of the UI bridge against the fixtures Kiln.Ui's end shares (schema/fixtures/sfi/ui/): the
//! bounds, the exact bytes of every wire/ record, every drain case on real files (a real sharing violation on Windows),
//! a writer racing the drain, and the notice port (collisions, the cap, same-call errors, no link followed). Set
//! SFI_UI_FIXTURES to the fixtures folder when the test binary runs away from the source tree (on another machine).
use kiln_sfi::runtime::ui::transport::*;
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

fn fixtures() -> PathBuf {
    std::env::var_os("SFI_UI_FIXTURES").map_or_else(
        || Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../schema/fixtures/sfi/ui"),
        PathBuf::from,
    )
}
fn fixture(rel: &str) -> Vec<u8> {
    let path = fixtures().join(rel);
    std::fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}
fn fixture_json(rel: &str) -> Value {
    serde_json::from_slice(&fixture(rel)).unwrap()
}

static DIRS: AtomicU64 = AtomicU64::new(0);
fn fresh(name: &str) -> PathBuf {
    let n = DIRS.fetch_add(1, Ordering::Relaxed);
    let base = Path::new(env!("CARGO_TARGET_TMPDIR"));
    let base = if base.is_dir() {
        base.to_path_buf()
    } else {
        std::env::temp_dir()
    };
    let dir = base
        .join("kiln-sfi-ui-transport")
        .join(format!("{name}-{}-{n}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// wire/launch.json's launch.
fn launch() -> Launch {
    Launch {
        launch_id: 0x3f2b_8c1e_5a7d_4e2f,
        plan_hash: [0xaa; 32],
    }
}

// ---- bounds and bytes

#[test]
fn the_bounds_are_bounds_json() {
    let b = fixture_json("bounds.json");
    assert_eq!(b["format"], 1);
    assert_eq!(b["recordBytes"], RECORD_BYTES);
    assert_eq!(b["pendingActionsPerOwner"], PENDING_ACTIONS_PER_OWNER);
    assert_eq!(b["noticeKeysPerProfile"], NOTICE_KEYS_PER_PROFILE);
    assert_eq!(b["entriesPerTurn"], ENTRIES_PER_TURN);
    assert_eq!(b["receiptBytes"], RECEIPT_BYTES);
    assert_eq!(b["keepEndedLaunchDays"], KEEP_ENDED_LAUNCH_DAYS);
}

fn action_of(v: &Value) -> ActionRecord {
    ActionRecord {
        request_id: v["requestId"].as_str().unwrap().into(),
        sequence: v["sequence"].as_u64().unwrap(),
        launch_id: v["launchId"].as_str().unwrap().into(),
        plan_hash: v["planHash"].as_str().unwrap().into(),
        owner_epoch: v["ownerEpoch"].as_u64().unwrap(),
        mod_id: v["modId"].as_str().unwrap().into(),
        action: v["action"].as_str().unwrap().into(),
        value: v.get("value").map(|x| x.as_str().unwrap().into()),
        created_ms: v["createdMs"].as_u64().unwrap(),
    }
}
fn notice_of(v: &Value) -> NoticeRecord {
    let n = &v["notice"];
    NoticeRecord {
        key: v["key"].as_str().unwrap().into(),
        launch_id: v["launchId"].as_str().unwrap().into(),
        owner_epoch: v["ownerEpoch"].as_u64().unwrap(),
        notice: Notice {
            id: n["id"].as_str().unwrap().into(),
            mod_id: n["modId"].as_str().unwrap().into(),
            title: n["title"].as_str().unwrap().into(),
            message: n["message"].as_str().unwrap().into(),
            level: n["level"].as_str().unwrap().into(),
            action_url: n["actionUrl"].as_str().map(str::to_owned),
            updated_at: n["updatedAt"].as_u64().unwrap(),
        },
    }
}

/// Every action and notice of wire/ comes out of the runtime's serializers byte for byte (members, order, escapes,
/// no whitespace, no newline), and each notice's keySha256 is its key's.
#[test]
fn the_records_are_byte_identical_to_wire() {
    let mut names: Vec<String> = std::fs::read_dir(fixtures().join("wire"))
        .unwrap()
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    let (mut actions, mut notices) = (0, 0);
    for name in &names {
        let bytes = fixture(&format!("wire/{name}"));
        let v: Value = serde_json::from_slice(&bytes).unwrap();
        match v["kind"].as_str() {
            Some("action") => {
                assert_eq!(action_of(&v).to_bytes(), bytes, "{name}");
                actions += 1;
            }
            Some("notice") => {
                assert_eq!(notice_of(&v).to_bytes(), bytes, "{name}");
                assert_eq!(
                    v["keySha256"],
                    key_sha256(v["key"].as_str().unwrap()),
                    "{name}"
                );
                notices += 1;
            }
            _ => assert_eq!(name, "launch.json", "the receipt is Kiln.Ui's alone"),
        }
    }
    assert_eq!((actions, notices), (4, 5), "{names:?}");
    let l = fixture_json("wire/launch.json");
    assert_eq!(l["launchId"], launch().launch_id_hex());
    assert_eq!(l["planHash"], launch().plan_hash_hex());
}

// ---- the drain cases (drain/cases.json), on real files

/// The cases' owners and callbacks: what the UI family's Lua side gives the drain.
#[derive(Default)]
struct Owners {
    epochs: BTreeMap<String, u64>,
    registered: BTreeSet<String>,
    failing: BTreeSet<String>,
    calls: Vec<(String, String, Option<String>)>,
}
impl ActionTarget for Owners {
    fn epoch(&self, mod_id: &str) -> Option<u64> {
        self.epochs.get(mod_id).copied()
    }
    fn invoke(&mut self, mod_id: &str, action: &str, value: Option<&str>) -> Invoke {
        let key = format!("{mod_id} {action}");
        if !self.registered.contains(&key) {
            return Invoke::Unregistered;
        }
        self.calls
            .push((mod_id.into(), action.into(), value.map(str::to_owned)));
        if self.failing.contains(&key) {
            Invoke::Called(Err("UI failure sentinel".into()))
        } else {
            Invoke::Called(Ok(()))
        }
    }
}

/// The real files, with a turn's removal failures, and its locked entries where a real lock is not possible.
struct CaseIo {
    remove_fails: BTreeSet<String>,
    locked: BTreeSet<String>,
}
fn name_of(path: &Path) -> String {
    path.file_name().unwrap().to_string_lossy().into_owned()
}
impl DrainIo for CaseIo {
    fn list(&mut self, dir: &Path, max: usize) -> Option<Vec<Entry>> {
        StdIo.list(dir, max)
    }
    fn read(&mut self, path: &Path, limit: u64) -> Opened {
        if !cfg!(windows) && self.locked.contains(&name_of(path)) {
            return Opened::Locked;
        }
        StdIo.read(path, limit)
    }
    fn remove(&mut self, path: &Path) -> std::io::Result<()> {
        if self.remove_fails.contains(&name_of(path)) {
            return Err(std::io::Error::other("removal fails this turn"));
        }
        if !cfg!(windows) && self.locked.contains(&name_of(path)) {
            return Err(std::io::Error::other("locked"));
        }
        StdIo.remove(path)
    }
}

/// Holds files open without any sharing, as another program does (Windows only: elsewhere CaseIo stands in).
struct Holds(#[allow(dead_code)] Vec<std::fs::File>);
fn hold(dir: &Path, names: &BTreeSet<String>) -> Holds {
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        Holds(
            names
                .iter()
                .map(|n| {
                    std::fs::OpenOptions::new()
                        .read(true)
                        .share_mode(0)
                        .open(dir.join(n))
                        .unwrap()
                })
                .collect(),
        )
    }
    #[cfg(not(windows))]
    {
        let _ = (dir, names);
        Holds(Vec::new())
    }
}

fn entry_bytes(entry: &Value) -> Vec<u8> {
    if let Some(text) = entry.get("text") {
        return text.as_str().unwrap().as_bytes().to_vec();
    }
    let wire = entry["wire"].as_str().unwrap();
    let base = fixture(&format!("wire/{wire}"));
    let (set, drop, pad) = (entry.get("set"), entry.get("drop"), entry.get("padTo"));
    let mut bytes = if set.is_none() && drop.is_none() {
        base
    } else {
        let mut v: Value = serde_json::from_slice(&base).unwrap();
        let object = v.as_object_mut().unwrap();
        for (k, x) in set.and_then(Value::as_object).into_iter().flatten() {
            object.insert(k.clone(), x.clone());
        }
        for k in drop.and_then(Value::as_array).into_iter().flatten() {
            object.shift_remove(k.as_str().unwrap());
        }
        serde_json::to_vec(&v).unwrap()
    };
    if let Some(pad) = pad.and_then(Value::as_u64) {
        bytes.resize(pad as usize, b' ');
    }
    bytes
}

fn strings(v: Option<&Value>) -> BTreeSet<String> {
    v.and_then(Value::as_array)
        .into_iter()
        .flatten()
        .map(|x| x.as_str().unwrap().to_owned())
        .collect()
}
fn calls_of(v: &Value) -> Vec<(String, String, Option<String>)> {
    let mut out: Vec<_> = v
        .as_array()
        .unwrap()
        .iter()
        .map(|c| {
            (
                c["modId"].as_str().unwrap().to_owned(),
                c["action"].as_str().unwrap().to_owned(),
                c["value"].as_str().map(str::to_owned),
            )
        })
        .collect();
    out.sort();
    out
}
fn remaining(dir: &Path) -> Vec<String> {
    let mut names: Vec<String> = std::fs::read_dir(dir)
        .unwrap()
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    names
}

fn run_case(case: &Value) {
    let name = case["name"].as_str().unwrap();
    let game = fresh(name);
    let launch = launch();
    let launch_dir = launch_dir(&game, &launch);
    let dir = actions_dir(&game, &launch);
    std::fs::create_dir_all(&dir).unwrap();
    // the receipt and a record in staging: never read, never touched
    std::fs::write(launch_dir.join("launch.json"), fixture("wire/launch.json")).unwrap();
    std::fs::create_dir_all(launch_dir.join("staging")).unwrap();
    std::fs::write(launch_dir.join("staging").join("x.tmp"), b"{\"format\":1,").unwrap();
    for entry in case["entries"].as_array().unwrap() {
        let path = dir.join(entry["name"].as_str().unwrap());
        if entry.get("folder").is_some() {
            std::fs::create_dir(&path).unwrap();
        } else {
            std::fs::write(&path, entry_bytes(entry)).unwrap();
        }
    }
    let mut owners = Owners {
        epochs: case["owners"]
            .as_object()
            .unwrap()
            .iter()
            .map(|(k, v)| (k.clone(), v.as_u64().unwrap()))
            .collect(),
        registered: strings(case.get("registered")),
        failing: strings(case.get("failing")),
        calls: Vec::new(),
    };
    let mut all = Vec::new();
    for (i, turn) in case["turns"].as_array().unwrap().iter().enumerate() {
        owners.registered.extend(strings(turn.get("register")));
        for id in strings(turn.get("deactivate")) {
            owners.epochs.remove(&id);
        }
        for (k, v) in turn
            .get("epochs")
            .and_then(Value::as_object)
            .into_iter()
            .flatten()
        {
            owners.epochs.insert(k.clone(), v.as_u64().unwrap());
        }
        let locked = strings(turn.get("locked"));
        let holds = hold(&dir, &locked);
        let mut io = CaseIo {
            remove_fails: strings(turn.get("removeFails")),
            locked,
        };
        owners.calls.clear();
        let report = drain(&dir, &launch, &mut io, &mut owners);
        drop(holds);
        assert!(report.entries.len() <= ENTRIES_PER_TURN, "{name} turn {i}");
        let mut calls = owners.calls.clone();
        calls.sort();
        all.extend(calls.clone());
        if let Some(expected) = turn.get("calls") {
            assert_eq!(calls, calls_of(expected), "{name} turn {i}: {report:#?}");
        }
        let left = remaining(&dir);
        if let Some(expected) = turn.get("remaining") {
            let expected: Vec<String> = strings(Some(expected)).into_iter().collect();
            assert_eq!(left, expected, "{name} turn {i}: {report:#?}");
        }
        if let Some(count) = turn.get("remainingCount") {
            assert_eq!(
                left.len() as u64,
                count.as_u64().unwrap(),
                "{name} turn {i}"
            );
        }
        // locked entries were never read: no outcome but Locked for them
        for (path, outcome) in &report.entries {
            if strings(turn.get("locked")).contains(&name_of(path)) {
                assert_eq!(*outcome, Outcome::Locked, "{name} turn {i}");
            }
        }
    }
    if let Some(expected) = case.get("allCalls") {
        all.sort();
        assert_eq!(all, calls_of(expected), "{name}: every call");
    }
    // the launch folder: the receipt and staging as they were, nothing new
    assert_eq!(
        std::fs::read(launch_dir.join("launch.json")).unwrap(),
        fixture("wire/launch.json")
    );
    assert_eq!(
        remaining(&launch_dir),
        ["actions", "launch.json", "staging"]
    );
    assert_eq!(remaining(&launch_dir.join("staging")), ["x.tmp"]);
    let _ = std::fs::remove_dir_all(&game);
}

#[test]
fn every_drain_case_passes_on_real_files() {
    let cases = fixture_json("drain/cases.json");
    assert_eq!(cases["format"], 1);
    let mut names = Vec::new();
    for case in cases["cases"].as_array().unwrap() {
        run_case(case);
        names.push(case["name"].as_str().unwrap().to_owned());
    }
    for must in [
        "source-parity",
        "stale-launch-plan-epoch",
        "values",
        "every-entry-and-the-record-bound",
        "thirty-two-entries-per-turn",
        "thirty-two-including-invalid",
        "delete-failure-replays",
        "waiting-then-disabled",
        "waiting-then-reloaded",
        "locked-entry-waits",
    ] {
        assert!(names.iter().any(|n| n == must), "case {must} missing");
    }
}

/// Without the launch's actions/ folder a turn reads nothing and creates nothing.
#[test]
fn a_turn_never_creates_the_actions_folder() {
    let game = fresh("no-actions");
    let mut owners = Owners::default();
    let report = drain(
        &actions_dir(&game, &launch()),
        &launch(),
        &mut StdIo,
        &mut owners,
    );
    assert!(report.entries.is_empty());
    assert_eq!(remaining(&game), Vec::<String>::new());
    let _ = std::fs::remove_dir_all(&game);
}

/// The size bound is decided from the metadata, before any open: an entry over it that cannot even be opened is
/// removed unread.
#[cfg(unix)]
#[test]
fn an_entry_over_the_bound_is_removed_without_being_opened() {
    use std::os::unix::fs::PermissionsExt;
    let game = fresh("bound-unread");
    let dir = actions_dir(&game, &launch());
    std::fs::create_dir_all(&dir).unwrap();
    let big = dir.join("big.json");
    std::fs::write(&big, vec![b' '; RECORD_BYTES as usize + 1]).unwrap();
    std::fs::set_permissions(&big, std::fs::Permissions::from_mode(0o000)).unwrap();
    let readable = std::fs::File::open(&big).is_ok(); // root reads anything
    let mut owners = Owners::default();
    let report = drain(&dir, &launch(), &mut StdIo, &mut owners);
    assert_eq!(
        report.entries[0].1,
        Outcome::Dropped {
            why: Dropped::TooLarge,
            removed: true
        },
        "readable by this user: {readable}"
    );
    assert!(!big.exists());
    let _ = std::fs::remove_dir_all(&game);
}

/// A link in actions/ is never followed: it is removed unread and its target is untouched.
#[cfg(unix)]
#[test]
fn a_link_is_removed_unread_and_its_target_kept() {
    let game = fresh("link");
    let dir = actions_dir(&game, &launch());
    std::fs::create_dir_all(&dir).unwrap();
    let outside = game.join("outside.json");
    std::fs::write(&outside, fixture("wire/action-with-value.json")).unwrap();
    std::os::unix::fs::symlink(&outside, dir.join("link.json")).unwrap();
    let mut owners = Owners::default();
    owners.epochs.insert("world-editor".into(), 3);
    owners.registered.insert("world-editor copy".into());
    let report = drain(&dir, &launch(), &mut StdIo, &mut owners);
    assert!(owners.calls.is_empty());
    assert_eq!(
        report.entries[0].1,
        Outcome::Dropped {
            why: Dropped::NotAFile,
            removed: true
        }
    );
    assert_eq!(
        std::fs::read(&outside).unwrap(),
        fixture("wire/action-with-value.json")
    );
    let _ = std::fs::remove_dir_all(&game);
}

/// The bridge's folders are never used through a link (Kiln.Ui's LinkOnPath rule: it never writes through one either):
/// with the ui root, the launch folder or actions/ a link, a turn reads, calls and removes nothing, and the link's
/// target keeps every file.
#[cfg(unix)]
#[test]
fn a_turn_never_reads_or_removes_through_a_linked_bridge_folder() {
    let id = launch().launch_id_hex();
    for (linked, below) in [
        ("kiln/sfi/ui".to_owned(), format!("{id}/actions")),
        (format!("kiln/sfi/ui/{id}"), "actions".to_owned()),
        (format!("kiln/sfi/ui/{id}/actions"), String::new()),
    ] {
        let root = fresh("linked-bridge");
        let t = Transport {
            game: root.join("game"),
            config: None,
            launch: launch(),
        };
        let elsewhere = root.join("elsewhere");
        let shown = elsewhere.join(&below);
        std::fs::create_dir_all(&shown).unwrap();
        std::fs::write(shown.join("r.json"), fixture("wire/action-with-value.json")).unwrap();
        std::fs::write(shown.join("keep.txt"), b"not JSON").unwrap();
        let link = t.game.join(&linked);
        std::fs::create_dir_all(link.parent().unwrap()).unwrap();
        std::os::unix::fs::symlink(&elsewhere, &link).unwrap();
        assert_eq!(
            link_on_path(&t.game, &t.launch),
            Some(link.clone()),
            "{linked}"
        );
        assert!(
            t.actions_dir().join("r.json").exists(),
            "{linked}: the link shows the files"
        );
        let mut owners = Owners::default();
        owners.epochs.insert("world-editor".into(), 3);
        owners.registered.insert("world-editor copy".into());
        let report = t.drain(&mut owners);
        assert!(report.entries.is_empty(), "{linked}: {report:#?}");
        assert!(owners.calls.is_empty(), "{linked}");
        assert_eq!(remaining(&shown), ["keep.txt", "r.json"], "{linked}");
        let _ = std::fs::remove_dir_all(&root);
    }
    // no link: the same transport drains
    let root = fresh("unlinked-bridge");
    let t = Transport {
        game: root.join("game"),
        config: None,
        launch: launch(),
    };
    std::fs::create_dir_all(t.actions_dir()).unwrap();
    std::fs::write(
        t.actions_dir().join("r.json"),
        fixture("wire/action-with-value.json"),
    )
    .unwrap();
    assert_eq!(link_on_path(&t.game, &t.launch), None);
    let mut owners = Owners::default();
    owners.epochs.insert("world-editor".into(), 3);
    owners.registered.insert("world-editor copy".into());
    t.drain(&mut owners);
    assert_eq!(owners.calls.len(), 1);
    assert_eq!(remaining(&t.actions_dir()), Vec::<String>::new());
    let _ = std::fs::remove_dir_all(&root);
}

/// Windows: actions/ as a junction (no privilege needed to make one) is a link too: nothing is read or removed through
/// it.
#[cfg(windows)]
#[test]
fn a_turn_never_reads_or_removes_through_a_junction() {
    let root = fresh("junction");
    let t = Transport {
        game: root.join("game"),
        config: None,
        launch: launch(),
    };
    let elsewhere = root.join("elsewhere");
    std::fs::create_dir_all(&elsewhere).unwrap();
    std::fs::write(
        elsewhere.join("r.json"),
        fixture("wire/action-with-value.json"),
    )
    .unwrap();
    std::fs::write(elsewhere.join("keep.txt"), b"not JSON").unwrap();
    let link = t.actions_dir();
    std::fs::create_dir_all(link.parent().unwrap()).unwrap();
    let made = std::process::Command::new("cmd")
        .arg("/C")
        .arg("mklink")
        .arg("/J")
        .arg(&link)
        .arg(&elsewhere)
        .output()
        .unwrap();
    assert!(made.status.success(), "{made:?}");
    assert!(link.join("r.json").exists(), "the junction shows the files");
    assert_eq!(link_on_path(&t.game, &t.launch), Some(link.clone()));
    let mut owners = Owners::default();
    owners.epochs.insert("world-editor".into(), 3);
    owners.registered.insert("world-editor copy".into());
    let report = t.drain(&mut owners);
    assert!(report.entries.is_empty(), "{report:#?}");
    assert!(owners.calls.is_empty());
    assert_eq!(remaining(&elsewhere), ["keep.txt", "r.json"]);
    // the junction itself goes, never its target's files
    std::fs::remove_dir(&link).unwrap();
    assert_eq!(remaining(&elsewhere), ["keep.txt", "r.json"]);
    let _ = std::fs::remove_dir_all(&root);
}

/// A real sharing violation (Windows): an entry another program holds without sharing, and one with a locked range,
/// stay unread and run once they are free. The classification is the raw error 32/33 only.
#[cfg(windows)]
#[test]
fn a_real_sharing_violation_leaves_the_entry_for_a_later_turn() {
    use std::os::windows::fs::OpenOptionsExt;
    use std::os::windows::io::AsRawHandle;
    #[repr(C)]
    struct Overlapped {
        internal: usize,
        internal_high: usize,
        offset: u32,
        offset_high: u32,
        event: *mut core::ffi::c_void,
    }
    unsafe extern "system" {
        fn LockFileEx(
            file: *mut core::ffi::c_void,
            flags: u32,
            reserved: u32,
            low: u32,
            high: u32,
            overlapped: *mut Overlapped,
        ) -> i32;
    }
    assert!(is_sharing_violation(&std::io::Error::from_raw_os_error(32)));
    assert!(is_sharing_violation(&std::io::Error::from_raw_os_error(33)));
    assert!(!is_sharing_violation(&std::io::Error::from_raw_os_error(5)));
    let game = fresh("sharing");
    let dir = actions_dir(&game, &launch());
    std::fs::create_dir_all(&dir).unwrap();
    std::fs::write(
        dir.join("held.json"),
        fixture("wire/action-with-value.json"),
    )
    .unwrap();
    std::fs::write(
        dir.join("ranged.json"),
        fixture("wire/action-without-value.json"),
    )
    .unwrap();
    let held = std::fs::OpenOptions::new()
        .read(true)
        .share_mode(0)
        .open(dir.join("held.json"))
        .unwrap();
    let ranged = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(dir.join("ranged.json"))
        .unwrap();
    let mut o = Overlapped {
        internal: 0,
        internal_high: 0,
        offset: 0,
        offset_high: 0,
        event: core::ptr::null_mut(),
    };
    // LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY over the whole file
    assert_ne!(
        unsafe { LockFileEx(ranged.as_raw_handle(), 3, 0, u32::MAX, u32::MAX, &mut o) },
        0
    );
    // the raw errors as the system gives them
    let e = std::fs::File::open(dir.join("held.json")).unwrap_err();
    assert_eq!(e.raw_os_error(), Some(32), "{e}");
    let mut owners = Owners::default();
    owners.epochs.insert("world-editor".into(), 3);
    owners.registered.insert("world-editor copy".into());
    let report = drain(&dir, &launch(), &mut StdIo, &mut owners);
    assert!(owners.calls.is_empty(), "{report:#?}");
    assert!(
        report.entries.iter().all(|(_, o)| *o == Outcome::Locked),
        "{report:#?}"
    );
    assert_eq!(remaining(&dir), ["held.json", "ranged.json"]);
    drop(held);
    drop(ranged);
    let report = drain(&dir, &launch(), &mut StdIo, &mut owners);
    assert_eq!(owners.calls.len(), 2, "{report:#?}");
    assert_eq!(remaining(&dir), Vec::<String>::new());
    let _ = std::fs::remove_dir_all(&game);
}

/// A writer that stages each record and renames it into actions/ (Kiln.Ui's way), racing turns of the drain: nothing
/// partial is ever parsed, nothing is removed before it is whole, and every request is called exactly once.
#[test]
fn a_writer_racing_the_drain_delivers_every_request_once_and_whole() {
    const N: usize = 400;
    let game = fresh("race");
    let launch = launch();
    let dir = actions_dir(&game, &launch);
    let staging = launch_dir(&game, &launch).join("staging");
    std::fs::create_dir_all(&dir).unwrap();
    std::fs::create_dir_all(&staging).unwrap();
    let writer = {
        let (dir, staging) = (dir.clone(), staging.clone());
        std::thread::spawn(move || {
            for i in 0..N {
                let record = ActionRecord {
                    request_id: format!("00000000-0000-4000-8000-{i:012}"),
                    sequence: i as u64,
                    launch_id: launch.launch_id_hex(),
                    plan_hash: launch.plan_hash_hex(),
                    owner_epoch: 3,
                    mod_id: "world-editor".into(),
                    action: "copy".into(),
                    // a large value, so a partial record is likely if one were ever seen
                    value: Some(format!("{i}:{}", "x".repeat(64 * 1024))),
                    created_ms: 1,
                };
                let temp = staging.join(format!("{i}.tmp"));
                {
                    use std::io::Write;
                    let mut f = std::fs::File::create(&temp).unwrap();
                    f.write_all(&record.to_bytes()).unwrap();
                    f.sync_all().unwrap();
                }
                std::fs::rename(&temp, dir.join(format!("{}.json", record.request_id))).unwrap();
            }
        })
    };
    let mut owners = Owners::default();
    owners.epochs.insert("world-editor".into(), 3);
    owners.registered.insert("world-editor copy".into());
    let mut dropped = Vec::new();
    let mut done = false;
    while !done {
        done = writer.is_finished();
        let report = drain(&dir, &launch, &mut StdIo, &mut owners);
        for (path, outcome) in report.entries {
            match outcome {
                Outcome::Called { removed: true, .. } => {}
                other => dropped.push((path, other)),
            }
        }
    }
    writer.join().unwrap();
    // what is left after the writer finished
    while !remaining(&dir).is_empty() {
        let report = drain(&dir, &launch, &mut StdIo, &mut owners);
        assert!(!report.entries.is_empty());
    }
    assert!(dropped.is_empty(), "{dropped:#?}");
    let mut seen: Vec<usize> = owners
        .calls
        .iter()
        .map(|(_, _, v)| {
            v.as_deref()
                .unwrap()
                .split(':')
                .next()
                .unwrap()
                .parse()
                .unwrap()
        })
        .collect();
    seen.sort();
    assert_eq!(seen, (0..N).collect::<Vec<_>>());
    let _ = std::fs::remove_dir_all(&game);
}

// ---- notices

fn notice(mod_id: &str, local: &str, title: &str, at: u64) -> (String, Notice) {
    (
        format!("{mod_id}-{local}"),
        Notice {
            id: format!("{mod_id}.{local}"),
            mod_id: mod_id.into(),
            title: title.into(),
            message: "Your layout was saved.".into(),
            level: "info".into(),
            action_url: None,
            updated_at: at,
        },
    )
}
fn transport(config: &Path) -> Transport {
    Transport {
        game: config.join("game"),
        config: Some(config.to_path_buf()),
        launch: launch(),
    }
}

/// A published notice is wire/notice.json's bytes, in its key's file; a later launch's transport sees the earlier
/// launch's notice still there and replaces it only by publishing the same key.
#[test]
fn a_notice_is_written_as_wire_and_survives_the_next_launch() {
    let config = fresh("notice");
    let (key, n) = notice("world-editor", "saved", "Saved", 1_763_520_011);
    transport(&config).publish(&key, 3, n).unwrap();
    let file = notices_dir(&config).join(format!("{}.json", key_sha256(&key)));
    assert_eq!(std::fs::read(&file).unwrap(), fixture("wire/notice.json"));
    // the next launch: the notice stays until its key is published again
    let next = Transport {
        launch: Launch {
            launch_id: 0x0000_0000_0000_0001,
            plan_hash: [0xbb; 32],
        },
        ..transport(&config)
    };
    let (key2, other) = notice("world-editor", "other", "Other", 5);
    next.publish(&key2, 1, other).unwrap();
    assert_eq!(std::fs::read(&file).unwrap(), fixture("wire/notice.json"));
    assert_eq!(remaining(&notices_dir(&config)).len(), 2);
    assert!(
        remaining(&notices_dir(&config))
            .iter()
            .all(|n| notice_file_name(n))
    );
    let _ = std::fs::remove_dir_all(&config);
}

/// ShroudForge's key collision: a-b/c and a/b-c are both "a-b-c", so they share one file and the later write wins
/// (wire/notice-collision-1.json, then -2).
#[test]
fn colliding_keys_share_one_file_and_the_later_write_wins() {
    let config = fresh("collision");
    let t = transport(&config);
    let first = notice_of(&fixture_json("wire/notice-collision-1.json"));
    let second = notice_of(&fixture_json("wire/notice-collision-2.json"));
    assert_eq!(first.key, second.key);
    t.publish(&first.key, first.owner_epoch, first.notice.clone())
        .unwrap();
    t.publish(&second.key, second.owner_epoch, second.notice.clone())
        .unwrap();
    let files = remaining(&notices_dir(&config));
    assert_eq!(files, [format!("{}.json", key_sha256("a-b-c"))]);
    assert_eq!(
        std::fs::read(notices_dir(&config).join(&files[0])).unwrap(),
        fixture("wire/notice-collision-2.json")
    );
    let _ = std::fs::remove_dir_all(&config);
}

/// The cap: with noticeKeysPerProfile keys stored, a new key is refused with an error for the same call and nothing
/// written, while an existing key (also at the cap) is replaced. Files that are not notices do not count.
#[test]
fn the_cap_refuses_a_new_key_and_still_replaces_an_existing_one() {
    let config = fresh("cap");
    let dir = notices_dir(&config);
    std::fs::create_dir_all(&dir).unwrap();
    for i in 0..NOTICE_KEYS_PER_PROFILE - 1 {
        std::fs::write(dir.join(format!("{:064x}.json", i)), b"{}").unwrap();
    }
    std::fs::write(dir.join("notes.txt"), b"not a notice").unwrap();
    std::fs::write(dir.join(".stale.tmp"), b"").unwrap();
    let t = transport(&config);
    // the 4096th key fits
    let (key, n) = notice("world-editor", "saved", "Saved", 1_763_520_011);
    t.publish(&key, 3, n).unwrap();
    // the 4097th does not
    let (new_key, new) = notice("world-editor", "another", "Another", 2);
    let error = t.publish(&new_key, 3, new).unwrap_err();
    assert!(error.starts_with("notification not written: "), "{error}");
    assert!(error.contains("4096"), "{error}");
    assert!(!dir.join(format!("{}.json", key_sha256(&new_key))).exists());
    // replacing an existing key at the cap works
    let (key, n) = notice("world-editor", "saved", "Saved again", 9);
    t.publish(&key, 3, n).unwrap();
    let v: Value = serde_json::from_slice(
        &std::fs::read(dir.join(format!("{}.json", key_sha256(&key)))).unwrap(),
    )
    .unwrap();
    assert_eq!(v["notice"]["title"], "Saved again");
    // no temporary file of ours is left
    assert!(
        remaining(&dir)
            .iter()
            .all(|n| notice_file_name(n) || n == "notes.txt" || n == ".stale.tmp")
    );
    let _ = std::fs::remove_dir_all(&config);
}

/// A write that cannot happen is an error for the same call, never a success: no config folder, a notices folder that
/// is a file, a record over the bound; nothing temporary is left.
#[test]
fn a_failed_write_is_the_same_call_s_error() {
    let config = fresh("io-error");
    let (key, n) = notice("world-editor", "saved", "Saved", 1);
    let none = Transport {
        config: None,
        ..transport(&config)
    };
    assert_eq!(
        none.publish(&key, 3, n.clone()).unwrap_err(),
        "notification not written: the core gives no config folder"
    );
    std::fs::create_dir_all(config.join("sfi")).unwrap();
    std::fs::write(notices_dir(&config), b"a file where the folder goes").unwrap();
    let error = transport(&config).publish(&key, 3, n.clone()).unwrap_err();
    assert!(error.starts_with("notification not written: "), "{error}");
    std::fs::remove_file(notices_dir(&config)).unwrap();
    let mut big = n;
    big.message = "x".repeat(RECORD_BYTES as usize);
    let error = transport(&config).publish(&key, 3, big).unwrap_err();
    assert!(error.contains("more than 2097152"), "{error}");
    if notices_dir(&config).exists() {
        assert_eq!(remaining(&notices_dir(&config)), Vec::<String>::new());
    }
    let _ = std::fs::remove_dir_all(&config);
}

/// A read-only notices folder: the same call fails and no temporary file is left.
#[cfg(unix)]
#[test]
fn a_refused_write_leaves_no_temporary_file() {
    use std::os::unix::fs::PermissionsExt;
    let config = fresh("readonly");
    let dir = notices_dir(&config);
    std::fs::create_dir_all(&dir).unwrap();
    std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o555)).unwrap();
    let probe = std::fs::write(dir.join("probe"), b"");
    let (key, n) = notice("world-editor", "saved", "Saved", 1);
    let result = transport(&config).publish(&key, 3, n);
    std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o755)).unwrap();
    if probe.is_ok() {
        // root writes anyway: nothing to prove here
        let _ = std::fs::remove_dir_all(&config);
        return;
    }
    assert!(
        result
            .unwrap_err()
            .starts_with("notification not written: ")
    );
    assert_eq!(remaining(&dir), Vec::<String>::new());
    let _ = std::fs::remove_dir_all(&config);
}

/// No link is followed: a notices folder (or its sfi parent) that is a link refuses the write and nothing reaches the
/// link's target.
#[cfg(unix)]
#[test]
fn a_linked_notices_folder_is_refused() {
    for linked in ["sfi", "sfi/notifications"] {
        let config = fresh("linked");
        let elsewhere = config.join("elsewhere");
        std::fs::create_dir_all(&elsewhere).unwrap();
        let link = config.join(linked);
        std::fs::create_dir_all(link.parent().unwrap()).unwrap();
        std::os::unix::fs::symlink(&elsewhere, &link).unwrap();
        let (key, n) = notice("world-editor", "saved", "Saved", 1);
        let error = transport(&config).publish(&key, 3, n).unwrap_err();
        assert!(error.contains("is a link"), "{linked}: {error}");
        assert_eq!(remaining(&elsewhere), Vec::<String>::new(), "{linked}");
        let _ = std::fs::remove_dir_all(&config);
    }
}

// ---- owner epochs

/// Every VM has one current epoch, given at the first ask and the same after; a renewal gives a new one; two VMs never
/// share one.
#[test]
fn owner_epochs_are_per_vm_and_renewed_never_reused() {
    let (a, b) = (mlua::Lua::new(), mlua::Lua::new());
    let ea = owner_epoch(&a);
    assert!(ea >= 1);
    assert_eq!(owner_epoch(&a), ea);
    let eb = owner_epoch(&b);
    assert_ne!(ea, eb);
    let renewed = renew_owner_epoch(&a);
    assert!(renewed != ea && renewed != eb);
    assert_eq!(owner_epoch(&a), renewed);
}

/// sfi.json's ownerEpoch (kiln-sfi's writer): only an active row holds one; a skip, a non-active lua and the end clear
/// it; 0 is never written.
#[test]
fn sfi_json_holds_an_owner_epoch_only_for_a_live_runtime() {
    use kiln_sfi::state::*;
    let identity = Identity {
        launch_id: Some(0x3f2b_8c1e_5a7d_4e2f),
        plan_hash: Some([0xaa; 32]),
        pid: 4242,
        process_start_ms: 1,
        side: Side::Client,
        profile: None,
        backend: Backend::Kiln,
        reason_code: "plan".into(),
        reason_text: None,
        frozen_at_ms: Some(1),
    };
    let mut r = Reducer::new(identity, PreviousStart::NONE, 1, Phase::Starting, None);
    for id in ["a", "b"] {
        let mut init = PackageInit::selected(id, Some("1.0.0"), None);
        init.lua = Lua::Admitted;
        r.add_package(init);
    }
    let epoch = |r: &Reducer, id: &str| r.package(id).unwrap().owner_epoch;
    let doc = |r: &Reducer| -> Value { serde_json::from_slice(&r.to_bytes(2)).unwrap() };
    // not active yet: refused
    assert_eq!(r.set_owner_epoch("a", Some(7)), Change::None);
    r.set_lua("a", Lua::Active);
    r.set_lua("b", Lua::Active);
    assert_eq!(r.set_owner_epoch("a", Some(0)), Change::None);
    assert_eq!(r.set_owner_epoch("a", Some(7)), Change::Urgent);
    assert_eq!(r.set_owner_epoch("a", Some(7)), Change::None);
    r.set_owner_epoch("b", Some(7));
    assert_eq!(doc(&r)["packages"][0]["ownerEpoch"], json!(7));
    // a reload's new generation
    r.set_owner_epoch("a", Some(8));
    assert_eq!(epoch(&r, "a"), Some(8));
    // a late loss clears it; so does a lua that is no longer active
    r.late_loss("b", "runtime-error", "boom");
    assert_eq!(epoch(&r, "b"), None);
    assert!(doc(&r)["packages"][1].get("ownerEpoch").is_none());
    r.set_lua("a", Lua::Stopped);
    assert_eq!(epoch(&r, "a"), None);
    r.set_lua("a", Lua::Active);
    r.set_owner_epoch("a", Some(9));
    // the end: no owner epoch is current
    r.set_phase(Phase::Running, None);
    r.set_phase(Phase::Ended, None);
    assert_eq!(epoch(&r, "a"), None);
    assert!(doc(&r)["packages"][0].get("ownerEpoch").is_none());
}
