// SPDX-License-Identifier: MIT
// the interpreter's UI bridge, ShroudForge's ui and notifications members.
// Behaviour checked against ShroudForge (MIT, Copyright (c) 2026 bonsaibauer), 1.7.9 (the same in 1.8.0):
// modules/mod-runtime/lua/shroudforge/v1/create_mod_api.rs:191-206,232-268,343-389, dispatch_mod_ui_actions.rs:21-107
// and its tests (:110-235), modules/notifications/write_notification_event.rs:4-15, event-schema.json.
//! `shroudforge.ui.on_action`, `shroudforge.ui.toggle_modloader` and `shroudforge.notifications.publish` in a real VM,
//! over the UI transport on real files: ShroudForge's dotted registry and its collisions, the requested owner and
//! epoch checks, the port bracket, the toggle's event (a real one on Windows), publish's checks and texts against the
//! parity fixtures (tests/fixtures/r35/notifications.json), and errors raised by the same call.
use kiln_sfi::runtime::ui::{self, actions, notices, page, transport::*};
use mlua::{Function, Lua, MultiValue, Table, Value};
use sfi_core::lifecycle::CallbackPort;
use std::cell::{Cell, RefCell};
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::atomic::{AtomicU64, Ordering};

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
        .join("kiln-sfi-r35")
        .join(format!("{name}-{}-{n}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}
fn files(root: &Path) -> Transport {
    Transport {
        game: root.join("game"),
        config: Some(root.join("config")),
        launch: Launch {
            launch_id: 0x3f2b_8c1e_5a7d_4e2f,
            plan_hash: [0xaa; 32],
        },
    }
}

/// One package's `shroudforge` table with the three members, as the UI family installs them.
fn api(lua: &Lua, mod_id: &str, transport: Option<Rc<Transport>>) -> Table {
    let api = lua.create_table().unwrap();
    let ui = lua.create_table().unwrap();
    actions::install(lua, &ui, mod_id).unwrap();
    page::install(lua, &ui).unwrap();
    api.raw_set("ui", ui).unwrap();
    notices::install(lua, &api, mod_id, transport).unwrap();
    api
}
fn member(api: &Table, table: &str, name: &str) -> Function {
    api.get::<Table>(table).unwrap().get(name).unwrap()
}

/// The error a Rust callback raised, without mlua's callback wrapper.
fn cause(error: &mlua::Error) -> &mlua::Error {
    match error {
        mlua::Error::CallbackError { cause, .. } | mlua::Error::BadArgument { cause, .. } => {
            self::cause(cause)
        }
        other => other,
    }
}
/// (kind, text): runtime / external / conversion, as the parity fixtures name them.
fn shape(error: &mlua::Error) -> (&'static str, String) {
    match cause(error) {
        mlua::Error::RuntimeError(text) => ("runtime", text.clone()),
        mlua::Error::ExternalError(e) => ("external", e.to_string()),
        mlua::Error::FromLuaConversionError { .. } => ("conversion", String::new()),
        other => ("other", other.to_string()),
    }
}

fn write_action(
    t: &Transport,
    name: &str,
    mod_id: &str,
    action: &str,
    epoch: u64,
    value: Option<&str>,
) {
    let dir = t.actions_dir();
    std::fs::create_dir_all(&dir).unwrap();
    let record = ActionRecord {
        request_id: name.into(),
        sequence: 0,
        launch_id: t.launch.launch_id_hex(),
        plan_hash: t.launch.plan_hash_hex(),
        owner_epoch: epoch,
        mod_id: mod_id.into(),
        action: action.into(),
        value: value.map(str::to_owned),
        created_ms: 0,
    };
    std::fs::write(dir.join(name), record.to_bytes()).unwrap();
}
fn left(t: &Transport) -> Vec<String> {
    let mut names: Vec<String> = std::fs::read_dir(t.actions_dir())
        .unwrap()
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    names
}

// ---- ui.on_action and the dispatch

/// on_action stores the callback under ShroudForge's dotted key and returns nothing; an invalid action id raises
/// ShroudForge's runtime error; a non-function callback is mlua's conversion error (verbatim arguments).
#[test]
fn on_action_registers_under_the_dotted_key() {
    let lua = Lua::new();
    let api = api(&lua, "world-editor", None);
    let on_action = member(&api, "ui", "on_action");
    let callback: Function = lua.load("return function(v) seen = v end").eval().unwrap();
    let returned: MultiValue = on_action.call(("copy", callback)).unwrap();
    assert!(returned.is_empty());
    let stored: Function = lua
        .named_registry_value(&actions::action_registry_key("world-editor", "copy"))
        .unwrap();
    stored.call::<()>("x").unwrap();
    assert_eq!(lua.globals().get::<String>("seen").unwrap(), "x");
    assert_eq!(
        actions::action_registry_key("world-editor", "copy"),
        "shroudforge.ui.action.world-editor.copy"
    );
    for bad in ["../copy", "", &"x".repeat(81)] {
        let callback: Function = lua.load("return function() end").eval().unwrap();
        let e = on_action.call::<()>((bad, callback)).unwrap_err();
        assert_eq!(
            shape(&e),
            ("runtime", "invalid ui action".into()),
            "{bad:?}"
        );
    }
    let e = on_action.call::<()>(("copy", 5)).unwrap_err();
    assert_eq!(shape(&e).0, "conversion");
}

/// ShroudForge's own queue test on Kiln records, through the Lua side: a value or nil reaches the callback, a failing
/// callback is logged with the action and removed, an unregistered one waits and runs once registered.
#[test]
fn the_dispatch_calls_lua_callbacks_and_keeps_waiting_requests() {
    let root = fresh("dispatch");
    let t = files(&root);
    let lua = Lua::new();
    let api = api(&lua, "world-editor", None);
    let on_action = member(&api, "ui", "on_action");
    on_action
        .call::<()>((
            "copy",
            lua.load("return function(v) received = v; calls = (calls or 0) + 1 end")
                .eval::<Function>()
                .unwrap(),
        ))
        .unwrap();
    on_action
        .call::<()>((
            "failure",
            lua.load("return function() error('UI failure sentinel') end")
                .eval::<Function>()
                .unwrap(),
        ))
        .unwrap();
    let epoch = owner_epoch(&lua);
    write_action(
        &t,
        "1.json",
        "world-editor",
        "copy",
        epoch,
        Some("Block äß"),
    );
    write_action(&t, "2.json", "world-editor", "failure", epoch, None);
    write_action(&t, "3.json", "world-editor", "later", epoch, Some("late"));
    let active = vec!["world-editor".to_owned()];
    let logged = RefCell::new(Vec::new());
    actions::dispatch(&lua, &active, &t, None, |l| {
        logged.borrow_mut().push(l.to_owned())
    });
    assert_eq!(lua.globals().get::<String>("received").unwrap(), "Block äß");
    assert_eq!(left(&t), ["3.json"]);
    let logged = logged.into_inner();
    assert_eq!(logged.len(), 1, "{logged:?}");
    assert!(
        logged[0].starts_with("shroudforge::mod: ui action failed: ")
            && logged[0].contains("UI failure sentinel")
            && logged[0].ends_with(" (mod_id=world-editor, action=failure)"),
        "{logged:?}"
    );
    // nil for no value
    write_action(&t, "4.json", "world-editor", "copy", epoch, None);
    actions::dispatch(&lua, &active, &t, None, |_| {});
    assert!(lua.globals().get::<Value>("received").unwrap().is_nil());
    on_action
        .call::<()>((
            "later",
            lua.load("return function(v) late = v end")
                .eval::<Function>()
                .unwrap(),
        ))
        .unwrap();
    actions::dispatch(&lua, &active, &t, None, |_| {});
    assert_eq!(lua.globals().get::<String>("late").unwrap(), "late");
    assert_eq!(left(&t), Vec::<String>::new());
    let _ = std::fs::remove_dir_all(&root);
}

/// The dotted collision: foo.bar/baz and foo/bar.baz share a key and the later registration wins. A request is checked
/// against its requested owner only: foo.bar (active) is answered by the callback foo registered, even though foo is
/// not active; a request for foo itself is dropped; a callback is never removed because its registrant is inactive.
#[test]
fn dotted_collisions_answer_the_requested_active_owner_with_the_later_callback() {
    let root = fresh("collision");
    let t = files(&root);
    let lua = Lua::new();
    let first = api(&lua, "foo.bar", None);
    let second = api(&lua, "foo", None);
    member(&first, "ui", "on_action")
        .call::<()>((
            "baz",
            lua.load("return function(v) hit = 'foo.bar:' .. v end")
                .eval::<Function>()
                .unwrap(),
        ))
        .unwrap();
    member(&second, "ui", "on_action")
        .call::<()>((
            "bar.baz",
            lua.load("return function(v) hit = 'foo:' .. v end")
                .eval::<Function>()
                .unwrap(),
        ))
        .unwrap();
    let epoch = owner_epoch(&lua);
    write_action(&t, "a.json", "foo.bar", "baz", epoch, Some("1"));
    write_action(&t, "b.json", "foo", "bar.baz", epoch, Some("2"));
    let active = vec!["foo.bar".to_owned()];
    actions::dispatch(&lua, &active, &t, None, |_| {});
    assert_eq!(lua.globals().get::<String>("hit").unwrap(), "foo:1");
    assert_eq!(left(&t), Vec::<String>::new());
    // the callback is still there for the next request
    write_action(&t, "c.json", "foo.bar", "baz", epoch, Some("3"));
    actions::dispatch(&lua, &active, &t, None, |_| {});
    assert_eq!(lua.globals().get::<String>("hit").unwrap(), "foo:3");
    let _ = std::fs::remove_dir_all(&root);
}

/// The VM's owner epoch decides: a request of an older generation (before a renewal) is dropped, never called; one
/// for an inactive owner too.
#[test]
fn a_request_of_an_earlier_generation_or_an_inactive_owner_is_never_called() {
    let root = fresh("epochs");
    let t = files(&root);
    let lua = Lua::new();
    let api = api(&lua, "world-editor", None);
    member(&api, "ui", "on_action")
        .call::<()>((
            "copy",
            lua.load("return function(v) calls = (calls or 0) + 1 end")
                .eval::<Function>()
                .unwrap(),
        ))
        .unwrap();
    let old = owner_epoch(&lua);
    let now = renew_owner_epoch(&lua);
    write_action(&t, "old.json", "world-editor", "copy", old, None);
    write_action(&t, "other.json", "other-mod", "copy", now, None);
    actions::dispatch(&lua, &["world-editor".to_owned()], &t, None, |_| {});
    assert!(lua.globals().get::<Value>("calls").unwrap().is_nil());
    assert_eq!(left(&t), Vec::<String>::new());
    write_action(&t, "now.json", "world-editor", "copy", now, None);
    actions::dispatch(&lua, &["world-editor".to_owned()], &t, None, |_| {});
    assert_eq!(lua.globals().get::<i64>("calls").unwrap(), 1);
    let _ = std::fs::remove_dir_all(&root);
}

/// The lifecycle port: each callback is bracketed (enter with the requested owner, exit after), a watchdog failure at
/// exit is the callback's error, and while the host stops no callback runs and the request stays.
#[test]
fn callbacks_run_inside_the_port_and_wait_while_stopping() {
    #[derive(Default)]
    struct Port {
        entered: RefCell<Vec<String>>,
        exits: Cell<u32>,
        fail: Cell<bool>,
        stopping: Cell<bool>,
    }
    impl CallbackPort for Port {
        fn enter(&self, owner: &str) {
            self.entered.borrow_mut().push(owner.to_owned());
        }
        fn exit(&self) -> Option<String> {
            self.exits.set(self.exits.get() + 1);
            self.fail.get().then(|| "watchdog".to_owned())
        }
        fn interrupted(&self) -> bool {
            self.stopping.get()
        }
    }
    let root = fresh("port");
    let t = files(&root);
    let lua = Lua::new();
    let api = api(&lua, "world-editor", None);
    member(&api, "ui", "on_action")
        .call::<()>((
            "copy",
            lua.load("return function() calls = (calls or 0) + 1 end")
                .eval::<Function>()
                .unwrap(),
        ))
        .unwrap();
    let epoch = owner_epoch(&lua);
    let active = vec!["world-editor".to_owned()];
    let port = Port::default();
    write_action(&t, "1.json", "world-editor", "copy", epoch, None);
    actions::dispatch(&lua, &active, &t, Some(&port), |_| {});
    assert_eq!(
        (port.entered.borrow().clone(), port.exits.get()),
        (vec!["world-editor".to_owned()], 1)
    );
    // a watchdog failure at exit: the callback's error, logged
    port.fail.set(true);
    write_action(&t, "2.json", "world-editor", "copy", epoch, None);
    let logged = RefCell::new(Vec::new());
    actions::dispatch(&lua, &active, &t, Some(&port), |l| {
        logged.borrow_mut().push(l.to_owned())
    });
    assert_eq!(
        logged.borrow().clone(),
        ["shroudforge::mod: ui action failed: watchdog (mod_id=world-editor, action=copy)"]
    );
    // stopping: nothing runs, the request stays
    port.stopping.set(true);
    write_action(&t, "3.json", "world-editor", "copy", epoch, None);
    actions::dispatch(&lua, &active, &t, Some(&port), |_| {});
    assert_eq!(lua.globals().get::<i64>("calls").unwrap(), 2);
    assert_eq!(left(&t), ["3.json"]);
    assert_eq!(port.exits.get(), 2);
    let _ = std::fs::remove_dir_all(&root);
}

// ---- ui.toggle_modloader

/// Off Windows: ShroudForge's own text, as a runtime error.
#[cfg(not(windows))]
#[test]
fn toggle_modloader_raises_shroudforge_s_text_off_windows() {
    let lua = Lua::new();
    let api = api(&lua, "world-editor", None);
    let e = member(&api, "ui", "toggle_modloader")
        .call::<()>(())
        .unwrap_err();
    assert_eq!(
        shape(&e),
        (
            "runtime",
            "Modloader UI toggle is only available on Windows clients".into()
        )
    );
    assert_eq!(
        page::event_name(4242),
        "Local\\ShroudForge.ModMenu.Open.4242"
    );
}

/// Windows: without the event the call raises ShroudForge's unavailable text with the system's error; with it (an
/// auto-reset event of this process's name, as the UI side creates it) the call returns no values and the event is signalled.
#[cfg(windows)]
#[test]
fn toggle_modloader_signals_the_named_event_and_returns_nothing() {
    type Handle = *mut core::ffi::c_void;
    unsafe extern "system" {
        fn CreateEventW(
            attributes: *const core::ffi::c_void,
            manual: i32,
            initial: i32,
            name: *const u16,
        ) -> Handle;
        fn WaitForSingleObject(handle: Handle, ms: u32) -> u32;
        fn CloseHandle(handle: Handle) -> i32;
    }
    let lua = Lua::new();
    let api = api(&lua, "world-editor", None);
    let toggle = member(&api, "ui", "toggle_modloader");
    let e = toggle.call::<()>(()).unwrap_err();
    let (kind, text) = shape(&e);
    assert_eq!(kind, "runtime");
    assert!(
        text.starts_with("Modloader UI toggle bridge is unavailable: "),
        "{text}"
    );
    assert!(text.contains("os error 2"), "{text}");
    let name: Vec<u16> = page::event_name(std::process::id())
        .encode_utf16()
        .chain(Some(0))
        .collect();
    let event = unsafe { CreateEventW(core::ptr::null(), 0, 0, name.as_ptr()) };
    assert!(!event.is_null());
    assert_eq!(
        unsafe { WaitForSingleObject(event, 0) },
        0x102,
        "not signalled yet"
    );
    let returned: MultiValue = toggle.call(()).unwrap();
    assert!(returned.is_empty());
    assert_eq!(unsafe { WaitForSingleObject(event, 0) }, 0, "signalled");
    unsafe { CloseHandle(event) };
}

// ---- notifications.publish

fn fixtures() -> PathBuf {
    std::env::var_os("SFI_R35_FIXTURES").map_or_else(
        || Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/r35"),
        PathBuf::from,
    )
}

/// Every parity fixture: ShroudForge's reads, checks, order and texts, and what is stored for an accepted notice.
#[test]
fn publish_matches_the_shroudforge_parity_fixtures() {
    let doc: serde_json::Value =
        serde_json::from_slice(&std::fs::read(fixtures().join("notifications.json")).unwrap())
            .unwrap();
    let mod_id = doc["modId"].as_str().unwrap();
    let root = fresh("parity");
    let t = Rc::new(files(&root));
    let lua = Lua::new();
    let api = api(&lua, mod_id, Some(t.clone()));
    let publish = member(&api, "notifications", "publish");
    let mut checked = 0;
    for case in doc["cases"].as_array().unwrap() {
        let name = case["name"].as_str().unwrap();
        let notice = lua.create_table().unwrap();
        for (k, v) in case["notice"].as_object().unwrap() {
            let value = match v {
                serde_json::Value::String(s) => Value::String(lua.create_string(s).unwrap()),
                serde_json::Value::Object(o) if o.contains_key("lua") => lua
                    .load(o["lua"].as_str().unwrap())
                    .eval::<Value>()
                    .unwrap(),
                other => panic!("{name}: {other}"),
            };
            notice.raw_set(k.as_str(), value).unwrap();
        }
        let result = publish.call::<MultiValue>(notice);
        if let Some(ok) = case.get("ok") {
            let returned = result.unwrap_or_else(|e| panic!("{name}: {e}"));
            assert!(returned.is_empty(), "{name}");
            let local = case["notice"]["id"].as_str().unwrap();
            let key = format!("{mod_id}-{local}");
            let file =
                notices_dir(t.config.as_ref().unwrap()).join(format!("{}.json", key_sha256(&key)));
            let stored: serde_json::Value =
                serde_json::from_slice(&std::fs::read(&file).unwrap()).unwrap();
            assert_eq!(stored["key"], key, "{name}");
            assert_eq!(
                stored["notice"]["id"],
                format!("{mod_id}.{local}"),
                "{name}"
            );
            assert_eq!(stored["notice"]["modId"], mod_id, "{name}");
            assert_eq!(stored["notice"]["level"], ok["level"], "{name}");
            assert_eq!(stored["notice"]["actionUrl"], ok["actionUrl"], "{name}");
            assert_eq!(stored["notice"]["title"], case["notice"]["title"], "{name}");
            assert_eq!(stored["ownerEpoch"], owner_epoch(&lua), "{name}");
            assert_eq!(stored["launchId"], "3f2b8c1e5a7d4e2f", "{name}");
        } else {
            let e = result.expect_err(name);
            let (kind, text) = shape(&e);
            assert_eq!(kind, case["error"]["kind"], "{name}: {e}");
            if let Some(expected) = case["error"].get("text") {
                assert_eq!(text, expected.as_str().unwrap(), "{name}");
            }
        }
        checked += 1;
    }
    assert!(checked >= 19);
    // refused publishes wrote nothing: only the accepted ones' files
    let stored = std::fs::read_dir(notices_dir(t.config.as_ref().unwrap()))
        .unwrap()
        .count();
    let accepted = doc["cases"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|c| c.get("ok").is_some())
        .count();
    assert_eq!(stored, accepted);
    let _ = std::fs::remove_dir_all(&root);
}

/// The schema message follows jsonschema 0.33.0's enum format for the five schema levels.
#[test]
fn the_schema_level_message_is_jsonschema_s() {
    assert_eq!(notices::schema_level_error("info"), None);
    assert_eq!(notices::schema_level_error("error"), None);
    assert_eq!(
        notices::schema_level_error("success").unwrap(),
        "\"success\" is not one of \"trace\", \"debug\" or 3 other candidates"
    );
    assert!(notices::valid_event_filename("world-editor-saved"));
    assert!(!notices::valid_event_filename("a/b"));
    assert!(!notices::valid_event_filename(&"a".repeat(241)));
}

/// ShroudForge's collision through Lua: a-b publishing c and a publishing b-c share the key a-b-c, one file, and the
/// later write wins.
#[test]
fn colliding_notice_keys_share_one_file_through_lua() {
    let root = fresh("lua-collision");
    let t = Rc::new(files(&root));
    let lua = Lua::new();
    let ab = api(&lua, "a-b", Some(t.clone()));
    let a = api(&lua, "a", Some(t.clone()));
    let publish = |api: &Table, id: &str, title: &str| {
        let notice = lua.create_table().unwrap();
        notice.raw_set("id", id).unwrap();
        notice.raw_set("title", title).unwrap();
        notice.raw_set("message", "m").unwrap();
        member(api, "notifications", "publish")
            .call::<()>(notice)
            .unwrap();
    };
    publish(&ab, "c", "first");
    publish(&a, "b-c", "second");
    let dir = notices_dir(t.config.as_ref().unwrap());
    let names: Vec<_> = std::fs::read_dir(&dir)
        .unwrap()
        .flatten()
        .map(|e| e.file_name())
        .collect();
    assert_eq!(names.len(), 1);
    let stored: serde_json::Value =
        serde_json::from_slice(&std::fs::read(dir.join(&names[0])).unwrap()).unwrap();
    assert_eq!(stored["key"], "a-b-c");
    assert_eq!(stored["notice"]["id"], "a.b-c");
    assert_eq!(stored["notice"]["title"], "second");
    let _ = std::fs::remove_dir_all(&root);
}

/// A write that fails is the same call's external error (nothing queued, nothing written): a notices folder that is a
/// file, the cap for a new key (an existing key still replaced at the cap), no transport at all. The epoch stored is
/// the VM's current one.
#[test]
fn a_failed_write_raises_from_the_same_publish_call() {
    let root = fresh("same-call");
    let t = Rc::new(files(&root));
    let lua = Lua::new();
    let api = api(&lua, "world-editor", Some(t.clone()));
    let publish = member(&api, "notifications", "publish");
    let notice = |id: &str, title: &str| {
        let n = lua.create_table().unwrap();
        n.raw_set("id", id).unwrap();
        n.raw_set("title", title).unwrap();
        n.raw_set("message", "m").unwrap();
        n
    };
    let config = t.config.clone().unwrap();
    std::fs::create_dir_all(config.join("sfi")).unwrap();
    std::fs::write(notices_dir(&config), b"a file").unwrap();
    let e = publish.call::<()>(notice("saved", "t")).unwrap_err();
    let (kind, text) = shape(&e);
    assert_eq!(kind, "external");
    assert!(text.starts_with("notification not written: "), "{text}");
    std::fs::remove_file(notices_dir(&config)).unwrap();
    // the cap
    let dir = notices_dir(&config);
    std::fs::create_dir_all(&dir).unwrap();
    for i in 0..NOTICE_KEYS_PER_PROFILE - 1 {
        std::fs::write(dir.join(format!("{i:064x}.json")), b"{}").unwrap();
    }
    publish.call::<()>(notice("saved", "first")).unwrap();
    let e = publish.call::<()>(notice("another", "t")).unwrap_err();
    let (kind, text) = shape(&e);
    assert_eq!(kind, "external");
    assert!(text.contains("4096"), "{text}");
    // an existing key at the cap, at a renewed epoch
    let renewed = renew_owner_epoch(&lua);
    publish.call::<()>(notice("saved", "second")).unwrap();
    let stored: serde_json::Value = serde_json::from_slice(
        &std::fs::read(dir.join(format!("{}.json", key_sha256("world-editor-saved")))).unwrap(),
    )
    .unwrap();
    assert_eq!(stored["notice"]["title"], "second");
    assert_eq!(stored["ownerEpoch"], renewed);
    // no transport
    let bare = Lua::new();
    let none = self::api(&bare, "world-editor", None);
    let n = bare.create_table().unwrap();
    n.raw_set("id", "saved").unwrap();
    n.raw_set("title", "t").unwrap();
    n.raw_set("message", "m").unwrap();
    let e = member(&none, "notifications", "publish")
        .call::<()>(n)
        .unwrap_err();
    assert_eq!(
        shape(&e),
        (
            "external",
            "notification not written: this start has no notices folder".into()
        )
    );
    let _ = std::fs::remove_dir_all(&root);
}

/// The family: its two namespaces, accepted by the installer's checks next to the network family.
#[test]
fn the_ui_family_claims_shroudforge_ui_and_notifications() {
    let family = ui::family(None);
    assert_eq!(family.name(), "ui");
    assert_eq!(
        family.claims(),
        ["shroudforge.ui", "shroudforge.notifications"]
    );
    assert!(
        kiln_sfi::runtime::Families::new(vec![
            kiln_sfi::runtime::network::production_family(),
            family
        ])
        .is_ok()
    );
}
