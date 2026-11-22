// SPDX-License-Identifier: MIT
// Behaviour checked against ShroudForge (MIT, Copyright (c) 2026 bonsaibauer), 1.7.9:
// modules/mod-runtime/lua/shroudforge/v1/create_mod_api.rs:75-91,117-189,273-311; modules/modloader/
// validate_mod_settings.rs:44-72; modules/hotkeys/parse_hotkey.rs:109-180.
//! The keybind members of `shroudforge.input` (kiln-sfi src/runtime/input.rs) in a real VM, over the settings
//! registry sfi-core installs from the package model (the values a package's extended.mod.json holds, Kiln's mod page
//! writes them there) and a key source the test steers: ShroudForge's `read_keybind` order and texts, the
//! `enabledWhen` chain, live values, the press memory per package and per key, rebinding, the start rules (client,
//! a declared keybind, once) and the stop. The real input feed under these members is tests/input_feed.rs.
use kiln_sfi::input::Binding;
use kiln_sfi::runtime::input::{self, InputState, KeySource};
use kiln_sfi::runtime::{network, registrar, ui, unavailable};
use mlua::{Lua, Table};
use serde_json::{Value as Json, json};
use sfi_core::host::Side;
use sfi_core::package::{self, ParseOptions};
use sfi_core::settings::{self, Settings};
use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::rc::Rc;

// ---- a key source the test steers ----

#[derive(Default)]
struct Keys {
    focused: bool,
    /// Canonical binding texts held now.
    held: HashSet<String>,
    /// Press revision per canonical binding text.
    revisions: HashMap<String, u64>,
    names: HashMap<u16, String>,
    pumps: usize,
    stops: usize,
}
#[derive(Clone, Default)]
struct Fake(Rc<RefCell<Keys>>);
impl Fake {
    fn focused(self) -> Self {
        self.0.borrow_mut().focused = true;
        self
    }
    fn hold(&self, binding: &str, held: bool) {
        let mut k = self.0.borrow_mut();
        if held {
            k.held.insert(binding.into());
        } else {
            k.held.remove(binding);
        }
    }
    fn press(&self, binding: &str) {
        let mut k = self.0.borrow_mut();
        let next = k.revisions.values().max().copied().unwrap_or(0) + 1;
        k.revisions.insert(binding.into(), next);
    }
    fn set_focus(&self, focused: bool) {
        self.0.borrow_mut().focused = focused;
    }
}
impl KeySource for Fake {
    fn pump(&mut self) {
        self.0.borrow_mut().pumps += 1;
    }
    fn focused(&self) -> bool {
        self.0.borrow().focused
    }
    fn is_down(&self, binding: &Binding) -> bool {
        let k = self.0.borrow();
        k.focused && k.held.contains(&binding.normalized())
    }
    fn press_revision(&self, binding: &Binding) -> u64 {
        self.0
            .borrow()
            .revisions
            .get(&binding.normalized())
            .copied()
            .unwrap_or(0)
    }
    fn label(&mut self, binding: &Binding) -> String {
        let names = self.0.borrow().names.clone();
        struct Names(HashMap<u16, String>);
        impl kiln_sfi::input::KeyNames for Names {
            fn key_name(&self, code: u16) -> Option<&str> {
                self.0.get(&code).map(String::as_str)
            }
        }
        binding.label(&Names(names))
    }
    fn stop(&mut self) -> Option<String> {
        self.0.borrow_mut().stops += 1;
        None
    }
}

// ---- a package: its model, its settings registry and the members ----

fn model(settings: Json) -> package::Manifest {
    let manifest =
        json!({"id": "keys-mod", "name": "Keys", "version": "1.0.0", "capabilities": ["runtime"]});
    let extension = json!({"schemaVersion": 1, "enabled": true, "settings": settings});
    package::Package::parse(
        manifest.to_string().as_bytes(),
        Some(extension.to_string().as_bytes()),
        ParseOptions::default(),
    )
    .unwrap()
    .manifest
}

struct Pkg {
    api: Table,
    settings: Settings,
}
fn package(lua: &Lua, id: &str, model: &package::Manifest, state: &Rc<InputState>) -> Pkg {
    let api = lua.create_table().unwrap();
    let settings =
        settings::install(lua, &api, id, &model.setting_values, &model.settings).unwrap();
    input::install_members(lua, &api, &settings, state).unwrap();
    lua.globals().raw_set(id.replace('-', "_"), &api).unwrap();
    Pkg { api, settings }
}
fn live(state: &Rc<InputState>, fake: &Fake) {
    state.set_side(Side::Client);
    state.opened(Ok(Box::new(fake.clone())));
}

fn eval(lua: &Lua, code: &str) -> Result<String, String> {
    lua.load(format!("return tostring({code})"))
        .eval::<String>()
        .map_err(|e| root(&e))
}
fn ok(lua: &Lua, code: &str) -> String {
    eval(lua, code).unwrap_or_else(|e| panic!("{code}: {e}"))
}
fn err(lua: &Lua, code: &str) -> String {
    match eval(lua, code) {
        Err(e) => e,
        Ok(v) => panic!("{code}: returned {v}"),
    }
}
fn root(error: &mlua::Error) -> String {
    match error {
        mlua::Error::CallbackError { cause, .. } => root(cause),
        mlua::Error::RuntimeError(text) => text.clone(),
        other => other.to_string(),
    }
}

const F9: &str = "SDL:66";

/// read_keybind's first check: a key that is not a declared keybind setting raises ShroudForge's text in all three
/// members (a declared setting of another control too, and an unknown key); a non-string key converts as a string
/// (a number) or raises mlua's conversion error (nil, a table).
#[test]
fn only_declared_keybind_settings_answer() {
    let lua = Lua::new();
    let m = model(json!({
        "toggleKey": {"value": F9, "control": "keybind"},
        "title": {"value": "x", "control": "text"},
        "7": {"value": "SDL:4", "control": "keybind"},
    }));
    let state = InputState::new();
    let fake = Fake::default().focused();
    live(&state, &fake);
    package(&lua, "keys-mod", &m, &state);
    for member in ["is_keybind_down", "was_keybind_pressed", "keybind_label"] {
        for key in ["'nope'", "'title'", "'togglekey'"] {
            assert_eq!(
                err(&lua, &format!("keys_mod.input.{member}({key})")),
                input::NOT_DECLARED,
                "{member}({key})"
            );
        }
        assert!(
            err(&lua, &format!("keys_mod.input.{member}(nil)")).contains("nil"),
            "{member}(nil) is mlua's conversion error"
        );
    }
    // a number names the setting "7" (mlua converts it as ShroudForge's String argument does)
    assert_eq!(ok(&lua, "keys_mod.input.keybind_label(7)"), "SDL:4");
    assert_eq!(
        ok(&lua, "keys_mod.input.is_keybind_down('toggleKey')"),
        "false"
    );
}

/// The value comes from the settings registry as sfi-core installed it from the package model (the value its
/// extended.mod.json holds, plain or as { "value": ... }), and a live settings change is read at the next call.
/// "" is no shortcut; a value that is no string, or not canonical, raises the parser's text.
#[test]
fn values_come_from_the_settings_registry_and_are_read_per_call() {
    let lua = Lua::new();
    let m = model(json!({
        "toggleKey": {"value": "Ctrl+SDL:66", "control": "keybind", "applyAt": "live"},
        "other": {"value": "", "control": "keybind"},
    }));
    assert_eq!(
        m.setting_values["toggleKey"], "Ctrl+SDL:66",
        "the persisted value is the model's"
    );
    let state = InputState::new();
    let fake = Fake::default().focused();
    fake.0.borrow_mut().names.insert(66, "F9".into());
    live(&state, &fake);
    let p = package(&lua, "keys-mod", &m, &state);
    assert_eq!(
        ok(&lua, "keys_mod.settings.get('toggleKey')"),
        "Ctrl+SDL:66"
    );
    assert_eq!(
        ok(&lua, "keys_mod.input.keybind_label('toggleKey')"),
        "Ctrl+F9"
    );
    assert_eq!(
        ok(&lua, "keys_mod.input.keybind_label('other')"),
        "nil",
        "'' is no shortcut"
    );
    assert_eq!(ok(&lua, "keys_mod.input.is_keybind_down('other')"), "false");
    fake.hold("Ctrl+SDL:66", true);
    assert_eq!(
        ok(&lua, "keys_mod.input.is_keybind_down('toggleKey')"),
        "true"
    );
    fake.hold("SDL:66", true);
    fake.hold("Ctrl+SDL:66", false);
    assert_eq!(
        ok(&lua, "keys_mod.input.is_keybind_down('toggleKey')"),
        "false",
        "F9 alone is not Ctrl+F9"
    );

    // a live settings change replaces the registry: the next call reads the new binding
    p.settings
        .replace(&lua, &json!({"toggleKey": F9, "other": ""}))
        .unwrap();
    assert_eq!(
        ok(&lua, "keys_mod.input.is_keybind_down('toggleKey')"),
        "true"
    );
    assert_eq!(ok(&lua, "keys_mod.input.keybind_label('toggleKey')"), "F9");
    for (value, text) in [
        (json!(66), input::NOT_A_BINDING.to_owned()),
        (json!(true), input::NOT_A_BINDING.to_owned()),
        (json!(null), input::NOT_A_BINDING.to_owned()),
        (json!(["SDL:66"]), input::NOT_A_BINDING.to_owned()),
        (
            json!("Shift+Ctrl+SDL:66"),
            "Use the canonical hotkey 'Ctrl+Shift+SDL:66'".to_owned(),
        ),
        (
            json!("SDL:0"),
            "Hotkey requires a valid SDL scancode".to_owned(),
        ),
        (
            json!("F9"),
            "Hotkey requires an SDL scancode or a mouse button".to_owned(),
        ),
    ] {
        p.settings
            .replace(&lua, &json!({"toggleKey": value.clone(), "other": ""}))
            .unwrap();
        for member in ["is_keybind_down", "was_keybind_pressed", "keybind_label"] {
            assert_eq!(
                err(&lua, &format!("keys_mod.input.{member}('toggleKey')")),
                text,
                "{member} {value}"
            );
        }
    }
    let _ = p.api;
}

/// ShroudForge's enabledWhen chain: a keybind whose controller does not hold its value (or whose chain is broken or
/// circular) counts as no shortcut: not held, no press, no label. JSON numbers compare as numbers.
#[test]
fn a_keybind_switched_off_by_its_enabled_when_chain_is_no_shortcut() {
    let lua = Lua::new();
    let m = model(json!({
        "useKeys": false,
        "mode": {"value": 1, "options": {"1": "One", "2": "Two"}},
        "toggleKey": {"value": F9, "control": "keybind", "enabledWhen": {"setting": "useKeys", "equals": true}},
        "modeKey": {"value": "SDL:67", "control": "keybind", "enabledWhen": {"setting": "mode", "equals": 1}},
        "chained": {"value": "SDL:68", "control": "keybind", "enabledWhen": {"setting": "toggleKey", "equals": F9}},
    }));
    let state = InputState::new();
    let fake = Fake::default().focused();
    live(&state, &fake);
    let p = package(&lua, "keys-mod", &m, &state);
    for key in ["toggleKey", "chained"] {
        fake.hold(m.setting_values[key].as_str().unwrap(), true);
        assert_eq!(
            ok(&lua, &format!("keys_mod.input.is_keybind_down('{key}')")),
            "false",
            "{key}"
        );
        assert_eq!(
            ok(&lua, &format!("keys_mod.input.keybind_label('{key}')")),
            "nil",
            "{key}"
        );
        assert_eq!(
            ok(
                &lua,
                &format!("keys_mod.input.was_keybind_pressed('{key}')")
            ),
            "false",
            "{key}"
        );
    }
    fake.hold("SDL:67", true);
    assert_eq!(
        ok(&lua, "keys_mod.input.is_keybind_down('modeKey')"),
        "true"
    );
    // the controller switched on (a live value): both keys of the chain answer
    p.settings
        .replace(&lua, &json!({"useKeys": true, "mode": 2, "toggleKey": F9, "modeKey": "SDL:67", "chained": "SDL:68"}))
        .unwrap();
    assert_eq!(
        ok(&lua, "keys_mod.input.is_keybind_down('toggleKey')"),
        "true"
    );
    assert_eq!(
        ok(&lua, "keys_mod.input.is_keybind_down('chained')"),
        "true"
    );
    assert_eq!(
        ok(&lua, "keys_mod.input.is_keybind_down('modeKey')"),
        "false",
        "mode 2"
    );
    assert_eq!(
        ok(&lua, "keys_mod.input.keybind_label('chained')"),
        "SDL:68"
    );
    // a controller value that became a float (a live value) still equals the integer
    p.settings
        .replace(&lua, &json!({"useKeys": true, "mode": 1.0, "toggleKey": F9, "modeKey": "SDL:67", "chained": "SDL:68"}))
        .unwrap();
    assert_eq!(
        ok(&lua, "keys_mod.input.is_keybind_down('modeKey')"),
        "true",
        "1.0 equals 1"
    );
}

/// was_keybind_pressed (create_mod_api.rs:149-181): the first call for a key only remembers; a newer press while
/// focused is reported once; a press while unfocused is used up unreported; a rebinding starts over; a key switched
/// off forgets; each package and each key has its own memory.
#[test]
fn presses_are_reported_once_per_package_and_key() {
    let lua = Lua::new();
    let m = model(json!({
        "a": {"value": F9, "control": "keybind"},
        "b": {"value": F9, "control": "keybind"},
    }));
    let state = InputState::new();
    let fake = Fake::default().focused();
    live(&state, &fake);
    let p = package(&lua, "keys-mod", &m, &state);
    package(&lua, "other-mod", &m, &state);
    let pressed =
        |pkg: &str, key: &str| ok(&lua, &format!("{pkg}.input.was_keybind_pressed('{key}')"));
    fake.press(F9);
    assert_eq!(
        pressed("keys_mod", "a"),
        "false",
        "a press from before the first call is never reported"
    );
    assert_eq!(pressed("keys_mod", "a"), "false");
    fake.press(F9);
    assert_eq!(pressed("keys_mod", "a"), "true");
    assert_eq!(pressed("keys_mod", "a"), "false", "reported once");
    assert_eq!(
        pressed("keys_mod", "b"),
        "false",
        "key b's first call only remembers"
    );
    assert_eq!(
        pressed("other_mod", "a"),
        "false",
        "the other package's first call only remembers"
    );
    fake.press(F9);
    assert_eq!(pressed("keys_mod", "a"), "true");
    assert_eq!(pressed("keys_mod", "b"), "true");
    assert_eq!(
        pressed("other_mod", "a"),
        "true",
        "each package sees the press"
    );
    fake.set_focus(false);
    fake.press(F9);
    assert_eq!(pressed("keys_mod", "a"), "false", "unfocused");
    fake.set_focus(true);
    assert_eq!(
        pressed("keys_mod", "a"),
        "false",
        "the unfocused press is used up"
    );
    // rebinding: the next call only remembers, then the new binding's presses count
    p.settings
        .replace(&lua, &json!({"a": "SDL:67", "b": F9}))
        .unwrap();
    fake.press("SDL:67");
    assert_eq!(
        pressed("keys_mod", "a"),
        "false",
        "first call after the rebinding"
    );
    fake.press("SDL:67");
    assert_eq!(pressed("keys_mod", "a"), "true");
    // switched off (""), then back: the memory starts over
    p.settings
        .replace(&lua, &json!({"a": "", "b": F9}))
        .unwrap();
    assert_eq!(pressed("keys_mod", "a"), "false");
    p.settings
        .replace(&lua, &json!({"a": "SDL:67", "b": F9}))
        .unwrap();
    fake.press("SDL:67");
    assert_eq!(pressed("keys_mod", "a"), "false", "back on: only remembers");
}

/// The key source starts once per process, in a game client, for the first package that declares a keybind setting
/// (create_mod_api.rs:75-91). Without one (a server, a package without keybinds, a start that failed) every query
/// answers as with no key: not held, never pressed, the label with no key names (SDL:<n>, the mouse names).
#[test]
fn the_key_source_starts_for_a_client_package_with_a_keybind_and_once() {
    let lua = Lua::new();
    let with = model(json!({"toggleKey": {"value": "SDL:66|Mouse:4", "control": "keybind"}}));
    let without = model(json!({"title": "x"}));
    let api = lua.create_table().unwrap();
    let s_with =
        settings::install(&lua, &api, "keys-mod", &with.setting_values, &with.settings).unwrap();
    let s_without = settings::install(
        &lua,
        &api,
        "keys-mod",
        &without.setting_values,
        &without.settings,
    )
    .unwrap();

    let server = InputState::new();
    assert!(!server.wants(&s_with), "no side yet");
    server.set_side(Side::Server);
    assert!(!server.wants(&s_with), "a server never starts it");

    let client = InputState::new();
    client.set_side(Side::Client);
    assert!(!client.wants(&s_without), "no keybind setting");
    assert!(client.wants(&s_with));
    client.opened(Err("kiln.ui is not there".into()));
    assert!(!client.wants(&s_with), "one try per process");
    assert!(!client.is_live());
    assert_eq!(
        client.take_lines(),
        vec![(
            kiln_abi::KILN_LOG_WARN,
            format!("{}kiln.ui is not there", input::UNAVAILABLE)
        )]
    );
    let fake = Fake::default().focused();
    fake.hold("SDL:66|Mouse:4", true);
    fake.press("SDL:66|Mouse:4");
    client.opened(Ok(Box::new(fake.clone())));
    assert!(
        !client.is_live(),
        "a source after the one try is stopped, not used"
    );
    assert_eq!(fake.0.borrow().stops, 1);
    package(&lua, "keys-mod", &with, &client);
    assert_eq!(
        ok(&lua, "keys_mod.input.is_keybind_down('toggleKey')"),
        "false"
    );
    assert_eq!(
        ok(&lua, "keys_mod.input.was_keybind_pressed('toggleKey')"),
        "false"
    );
    assert_eq!(
        ok(&lua, "keys_mod.input.was_keybind_pressed('toggleKey')"),
        "false"
    );
    assert_eq!(
        ok(&lua, "keys_mod.input.keybind_label('toggleKey')"),
        "SDL:66 / Mouse X1"
    );
}

/// A live source: the turn start pumps it, the members read it, the stop ends it once and nothing starts again.
#[test]
fn a_live_source_is_pumped_each_turn_and_stopped_once() {
    let lua = Lua::new();
    let m = model(json!({"toggleKey": {"value": F9, "control": "keybind"}}));
    let state = InputState::new();
    state.set_side(Side::Client);
    let api = lua.create_table().unwrap();
    let s = settings::install(&lua, &api, "keys-mod", &m.setting_values, &m.settings).unwrap();
    assert!(state.wants(&s));
    let fake = Fake::default().focused();
    state.opened(Ok(Box::new(fake.clone())));
    assert!(state.is_live());
    assert_eq!(
        state.take_lines(),
        vec![(kiln_abi::KILN_LOG_INFO, input::STARTED.to_owned())]
    );
    state.turn_start();
    state.turn_start();
    assert_eq!(fake.0.borrow().pumps, 2);
    package(&lua, "keys-mod", &m, &state);
    fake.hold(F9, true);
    assert_eq!(
        ok(&lua, "keys_mod.input.is_keybind_down('toggleKey')"),
        "true"
    );
    state.stop();
    state.stop();
    assert_eq!(fake.0.borrow().stops, 1);
    assert!(!state.is_live() && !state.wants(&s));
    state.turn_start();
    assert_eq!(fake.0.borrow().pumps, 2, "no pump after the stop");
    assert_eq!(
        ok(&lua, "keys_mod.input.is_keybind_down('toggleKey')"),
        "false",
        "stopped: no key"
    );
}

/// The production list has the input family before the unavailable one, which keeps only is_key_down of
/// shroudforge.input (ShroudForge's removal error).
#[test]
fn the_production_list_gives_the_keybind_members_to_the_input_family() {
    let families = registrar::production(registrar::Parts {
        types: None,
        ecs: None,
        attributes: None,
        assets: None,
        network: network::production_family(),
        ui: ui::family(None),
        trackers: None,
    })
    .unwrap();
    let names: Vec<&str> = families.iter().map(|f| f.name()).collect();
    let at = |name: &str| names.iter().position(|n| *n == name).unwrap();
    assert!(at(input::FAMILY) < at(unavailable::FAMILY), "{names:?}");
    let input_family = families.iter().find(|f| f.name() == input::FAMILY).unwrap();
    assert_eq!(input_family.claims(), input::CLAIMS);
    let rest = families
        .iter()
        .find(|f| f.name() == unavailable::FAMILY)
        .unwrap();
    for claim in input::CLAIMS {
        assert!(!rest.claims().contains(&claim), "{claim}");
    }
    assert!(rest.claims().contains(&"shroudforge.input.is_key_down"));
    assert!(families.hooks().len() >= 3, "the input family has its hook");
}

/// The keys probe of the client pass (mods/tests/sf-keys-probe): its package parses as a 1.7.9 package with one keybind
/// setting, and its Lua on these members writes the lines the check sfi-keybinds reads: the loaded line with the label
/// and the undeclared-key error, one line and one notice per press, held and released.
#[test]
fn the_keys_probe_writes_the_lines_its_client_check_reads() {
    let root =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../mods/tests/sf-keys-probe");
    let read = |name: &str| std::fs::read(root.join(name)).unwrap();
    let parsed = package::Package::parse(
        &read("mod.json"),
        Some(&read("extended.mod.json")),
        ParseOptions {
            version: package::SourceVersion::Sf179,
            ..ParseOptions::default()
        },
    )
    .unwrap();
    let m = parsed.manifest;
    assert_eq!(m.id, "kiln-sf-keys-probe");
    let lua = Lua::new();
    let state = InputState::new();
    let fake = Fake::default().focused();
    fake.0.borrow_mut().names.insert(61, "F4".into());
    live(&state, &fake);
    let p = package(&lua, "kiln-sf-keys-probe", &m, &state);
    let lines = Rc::new(RefCell::new(Vec::<String>::new()));
    let log = lua.create_table().unwrap();
    for level in ["info", "warn"] {
        let lines = lines.clone();
        log.raw_set(
            level,
            lua.create_function(move |_, text: String| {
                lines.borrow_mut().push(format!("{level}: {text}"));
                Ok(())
            })
            .unwrap(),
        )
        .unwrap();
    }
    p.api.raw_set("log", log).unwrap();
    let notices = Rc::new(RefCell::new(Vec::<String>::new()));
    let publish = {
        let notices = notices.clone();
        lua.create_function(move |_, notice: Table| {
            notices.borrow_mut().push(format!(
                "{}|{}|{}",
                notice.get::<String>("id")?,
                notice.get::<String>("title")?,
                notice.get::<String>("message")?
            ));
            Ok(())
        })
        .unwrap()
    };
    let notifications = lua.create_table().unwrap();
    notifications.raw_set("publish", publish).unwrap();
    p.api.raw_set("notifications", notifications).unwrap();
    lua.globals().raw_set("shroudforge", &p.api).unwrap();
    let source = String::from_utf8(read("src/mod.lua")).unwrap();
    let probe: Table = lua.load(source).eval().unwrap();
    assert_eq!(probe.get::<u64>("update_interval_ms").unwrap(), 50);
    let call = |name: &str| {
        probe
            .get::<mlua::Function>(name)
            .unwrap()
            .call::<()>(())
            .unwrap()
    };
    call("on_load");
    let loaded = lines.borrow()[0].clone();
    assert!(
        loaded.starts_with("info: probe loaded: shortcut F4; undeclared: ")
            && loaded.contains(input::NOT_DECLARED),
        "{loaded}"
    );
    call("on_update"); // the first was_keybind_pressed only remembers
    fake.press("SDL:61");
    fake.hold("SDL:61", true);
    call("on_update");
    call("on_update");
    fake.hold("SDL:61", false);
    call("on_update");
    fake.press("SDL:61");
    call("on_update");
    assert_eq!(
        lines.borrow()[1..].to_vec(),
        [
            "info: probe pressed: F4 pressed (1)",
            "info: probe held: F4",
            "info: probe released: F4",
            "info: probe pressed: F4 pressed (2)",
        ]
    );
    assert_eq!(
        *notices.borrow(),
        [
            "press|Keys probe|F4 pressed (1)",
            "press|Keys probe|F4 pressed (2)"
        ]
    );
}
