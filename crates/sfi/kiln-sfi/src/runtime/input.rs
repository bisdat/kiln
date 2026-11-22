// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (1.7.9, the pin the interpreter targets): modules/mod-runtime/lua/shroudforge/v1/create_mod_api.rs:75-91
// (the key state starts once per process, in a game client, for the first mod with a keybind setting; "Keyboard
// shortcuts unavailable" when it cannot), :117-189 (is_keybind_down, was_keybind_pressed, keybind_label),
// :273-311 (read_keybind, read_setting_value), :313-332 (is_game_focused); modules/modloader/
// validate_mod_settings.rs:44-72 (setting_is_enabled); modules/hotkeys/parse_hotkey.rs:109-180 (parse_hotkey,
// parse_value).
//! `shroudforge.input.is_keybind_down`, `was_keybind_pressed` and `keybind_label` on Kiln's input feed.
//!
//! **The key state.** A package reads its keyboard shortcuts through its own `control: "keybind"` settings, never a
//! raw key. The state behind them is the input feed ([`crate::input`]): kiln.ui's window input records, drained
//! once per worker turn ([`InputState::turn_start`], the family's turn hook, before the mods' update). As in
//! ShroudForge it starts once per process, in a game client, when the first package that declares a keybind setting
//! is registered; when it cannot start, kiln.log says "Keyboard shortcuts unavailable: <why>" and every query answers
//! as with no key (ShroudForge's answer without its input process). A server never starts it. The runtime's orderly
//! stop ends it (the family's stop hook).
//!
//! **A query** (each member, per call, on the worker): ShroudForge's `read_keybind` first: a key that is not one of
//! the package's declared keybind settings raises "setting is not a declared keybind"; a setting switched off by its
//! `enabledWhen` chain counts as no shortcut; the current value (the settings registry, so a live settings change
//! is seen at the next call) must be a binding string, else "Hotkey must be an SDL binding string" or the parser's
//! own text is raised; "" is no shortcut. Then:
//! - `is_keybind_down(key)`: the game window is focused, Kiln is not capturing the input, and one of the binding's
//!   chords is held with exactly its modifiers;
//! - `was_keybind_pressed(key)`: a press of the binding newer than the one this package's previous call for that
//!   key saw, while focused and not capturing. The first call for a key, and the first after its binding changed
//!   (a rebinding), only remembers: a press from before is never reported. Each package has its own memory;
//! - `keybind_label(key)`: the binding's label in the game window's keyboard layout ("Ctrl+F9 / Mouse X1"), nil
//!   with no shortcut.
//!
//! **Keys are not kept from the game.** ShroudForge 1.7.9 only observes the keys: the game sees a mod's shortcut too.
//! Keeping keys from the game is ShroudForge 1.8's input reservations, which kiln mode does not offer.
//!
//! [`family`] claims the three names; `shroudforge.input.is_key_down` keeps the unavailable family's answer
//! (ShroudForge's removal error).

use crate::input::label::NoKeyNames;
use crate::input::{Binding, Desktop, FeedConfig, InputFeed, PressTracker, StopOutcome};
use crate::runtime::context::RuntimeContext;
use crate::runtime::install::{Family, RuntimeHook};
use crate::runtime::services::ServiceAdapter;
use crate::worker::WorkerContext;
use kiln_abi::{KILN_LOG_INFO, KILN_LOG_WARN, KilnOwner};
use mlua::{Lua, Table, Value};
use serde_json::Value as Json;
use sfi_core::host::Side;
use sfi_core::package::manifest::SettingControl;
use sfi_core::package::settings::setting_is_enabled;
use sfi_core::settings::{Settings, read_value};
use std::cell::{Cell, RefCell};
use std::rc::Rc;

/// The family's name.
pub const FAMILY: &str = "input";

/// The members this family installs in each package's `shroudforge.input`.
pub const CLAIMS: [&str; 3] = [
    "shroudforge.input.is_keybind_down",
    "shroudforge.input.was_keybind_pressed",
    "shroudforge.input.keybind_label",
];

/// `read_keybind`'s error for a key that is not a declared keybind setting (create_mod_api.rs:279-283).
pub const NOT_DECLARED: &str = "setting is not a declared keybind";

/// `parse_value`'s error for a value that is not a string (parse_hotkey.rs:174-180).
pub const NOT_A_BINDING: &str = "Hotkey must be an SDL binding string";

/// The kiln.log line when the key state cannot start (create_mod_api.rs:89), before the reason.
pub const UNAVAILABLE: &str = "kiln-sfi: Keyboard shortcuts unavailable: ";

/// The kiln.log line once the key state has started.
pub const STARTED: &str =
    "kiln-sfi: keyboard shortcuts of ShroudForge-format mods read the game window's keys";

/// Where the members' key state comes from: the input feed in a game client, a stand-in in tests.
pub trait KeySource {
    /// Takes in what arrived since the last turn (the worker, at each turn start).
    fn pump(&mut self);
    /// ShroudForge's `is_game_focused() && !is_capture_active()`: the game window is focused and Kiln does not
    /// capture the input.
    fn focused(&self) -> bool;
    /// `is_keybind_down` for a binding `read_keybind` gave (focus and capture included).
    fn is_down(&self, binding: &Binding) -> bool;
    /// The binding's newest press revision (`hotkey_press_revision`), 0 for none.
    fn press_revision(&self, binding: &Binding) -> u64;
    /// The binding's label in the current keyboard layout.
    fn label(&mut self, binding: &Binding) -> String;
    /// Ends the source; the text says what did not go as planned.
    fn stop(&mut self) -> Option<String>;
}

/// Starts the process's key source with the runtime's service adapter and kiln-sfi's owner.
pub type Open = Rc<dyn Fn(&Rc<ServiceAdapter>, KilnOwner) -> Result<Box<dyn KeySource>, String>>;

/// The input feed as the members' key source.
pub struct FeedSource {
    feed: InputFeed,
    adapter: Rc<ServiceAdapter>,
}
impl KeySource for FeedSource {
    fn pump(&mut self) {
        self.feed.pump(&self.adapter);
    }
    fn focused(&self) -> bool {
        self.feed.focused()
    }
    fn is_down(&self, binding: &Binding) -> bool {
        self.feed.is_keybind_down(binding)
    }
    fn press_revision(&self, binding: &Binding) -> u64 {
        self.feed.press_revision(binding)
    }
    fn label(&mut self, binding: &Binding) -> String {
        self.feed.label(binding)
    }
    fn stop(&mut self) -> Option<String> {
        match self.feed.stop(&self.adapter) {
            StopOutcome::Kept(code) => Some(format!(
                "kiln-sfi: kiln.ui did not remove the keyboard shortcuts' window input subscription ({code}); it stays, unused, until the game exits"
            )),
            StopOutcome::Unsubscribed | StopOutcome::AlreadyStopped => None,
        }
    }
}

/// Opens the input feed on kiln.ui (v5) with the desktop `desktop` makes and `config`.
pub fn open_feed(desktop: Rc<dyn Fn() -> Box<dyn Desktop>>, config: FeedConfig) -> Open {
    Rc::new(move |adapter: &Rc<ServiceAdapter>, owner: KilnOwner| {
        InputFeed::subscribe(adapter, owner, desktop(), config)
            .map(|feed| {
                Box::new(FeedSource {
                    feed,
                    adapter: adapter.clone(),
                }) as Box<dyn KeySource>
            })
            .map_err(|error| error.to_string())
    })
}

enum Source {
    /// Not started (no client package with a keybind setting yet).
    Unopened,
    Live(Box<dyn KeySource>),
    /// Could not start, or stopped: every query answers as with no key, and nothing starts again.
    Off,
}

/// The family's state for one runtime-create: the side, the key source and the lines for kiln.log. It belongs to
/// the process's runtime, so a live reload's new VM reads the same source.
pub struct InputState {
    side: Cell<Option<Side>>,
    source: RefCell<Source>,
    pending: RefCell<Vec<(i32, String)>>,
}

impl InputState {
    pub fn new() -> Rc<Self> {
        Rc::new(Self {
            side: Cell::new(None),
            source: RefCell::new(Source::Unopened),
            pending: RefCell::new(Vec::new()),
        })
    }
    /// The plan's side (the hook's runtime-create).
    pub fn set_side(&self, side: Side) {
        self.side.set(Some(side));
    }
    /// Whether registering a package with `settings` starts the key source now: a game client, a package that
    /// declares a keybind setting, and no start tried before (create_mod_api.rs:75-91).
    pub fn wants(&self, settings: &Settings) -> bool {
        self.side.get() == Some(Side::Client)
            && matches!(*self.source.borrow(), Source::Unopened)
            && declares_keybind(settings)
    }
    /// The outcome of the one start: the source, or why there is none (logged at the next hook).
    pub fn opened(&self, outcome: Result<Box<dyn KeySource>, String>) {
        let mut source = self.source.borrow_mut();
        if !matches!(*source, Source::Unopened) {
            if let Ok(mut extra) = outcome {
                extra.stop();
            }
            return;
        }
        *source = match outcome {
            Ok(live) => {
                self.pending
                    .borrow_mut()
                    .push((KILN_LOG_INFO, STARTED.to_owned()));
                Source::Live(live)
            }
            Err(why) => {
                self.pending
                    .borrow_mut()
                    .push((KILN_LOG_WARN, format!("{UNAVAILABLE}{why}")));
                Source::Off
            }
        };
    }
    /// Whether the key source runs.
    pub fn is_live(&self) -> bool {
        matches!(*self.source.borrow(), Source::Live(_))
    }
    /// The turn start: the key source takes in what arrived.
    pub fn turn_start(&self) {
        if let Source::Live(source) = &mut *self.source.borrow_mut() {
            source.pump();
        }
    }
    /// The runtime's orderly stop: the key source ends, and nothing starts again.
    pub fn stop(&self) {
        let previous = std::mem::replace(&mut *self.source.borrow_mut(), Source::Off);
        if let Source::Live(mut source) = previous
            && let Some(line) = source.stop()
        {
            self.pending.borrow_mut().push((KILN_LOG_WARN, line));
        }
    }
    /// The kiln.log lines waiting for a worker context (level, text).
    pub fn take_lines(&self) -> Vec<(i32, String)> {
        std::mem::take(&mut *self.pending.borrow_mut())
    }

    fn is_down(&self, binding: &Binding) -> bool {
        match &*self.source.borrow() {
            Source::Live(source) => source.is_down(binding),
            _ => false,
        }
    }
    /// (press revision, focused); without a source ShroudForge's answer with no input process: (0, false).
    fn press(&self, binding: &Binding) -> (u64, bool) {
        match &*self.source.borrow() {
            Source::Live(source) => (source.press_revision(binding), source.focused()),
            _ => (0, false),
        }
    }
    /// Without a source no key has a name: `SDL:<n>` (ShroudForge's label with no labels table).
    fn label(&self, binding: &Binding) -> String {
        match &mut *self.source.borrow_mut() {
            Source::Live(source) => source.label(binding),
            _ => binding.label(&NoKeyNames),
        }
    }

    fn flush(&self, cx: &WorkerContext<'_>) {
        for (level, line) in self.take_lines() {
            cx.log(level, &line);
        }
    }
}

/// The package declares at least one keybind setting.
pub fn declares_keybind(settings: &Settings) -> bool {
    settings
        .definitions()
        .iter()
        .any(|setting| matches!(setting.control, Some(SettingControl::Keybind)))
}

/// ShroudForge's `parse_value` (parse_hotkey.rs:174-180): a binding string, parsed; "" is no shortcut.
pub fn parse_value(value: &Json) -> Result<Option<Binding>, String> {
    Binding::parse(value.as_str().ok_or(NOT_A_BINDING)?)
}

/// ShroudForge's `read_keybind` (create_mod_api.rs:273-301): the binding of the package's keybind setting `key` now,
/// `None` when its `enabledWhen` chain switches it off or its value is "".
pub fn read_keybind(lua: &Lua, settings: &Settings, key: &str) -> mlua::Result<Option<Binding>> {
    let declared = settings.definitions();
    if !declared.iter().any(|setting| {
        setting.key == key && matches!(setting.control, Some(SettingControl::Keybind))
    }) {
        return Err(mlua::Error::runtime(NOT_DECLARED));
    }
    let current = settings.current(lua)?;
    let mut controllers = serde_json::Map::new();
    for condition in declared
        .iter()
        .filter_map(|setting| setting.enabled_when.as_ref())
    {
        controllers.insert(
            condition.setting.clone(),
            read_value(&current, &condition.setting)?,
        );
    }
    if !setting_is_enabled(key, declared, &Json::Object(controllers)) {
        return Ok(None);
    }
    parse_value(&read_value(&current, key)?).map_err(mlua::Error::runtime)
}

/// The three members in `shroudforge`'s `input` table (made when it is not there yet) for the package `settings`
/// belong to, over `state`. The package gets its own `was_keybind_pressed` memory.
pub fn install_members(
    lua: &Lua,
    shroudforge: &Table,
    settings: &Settings,
    state: &Rc<InputState>,
) -> mlua::Result<()> {
    let input = match shroudforge.raw_get::<Value>("input")? {
        Value::Table(input) => input,
        Value::Nil => {
            let input = lua.create_table()?;
            shroudforge.raw_set("input", &input)?;
            input
        }
        _ => return Err(mlua::Error::runtime("namespace input must be a table")),
    };
    let (s, st) = (settings.clone(), state.clone());
    input.raw_set(
        "is_keybind_down",
        lua.create_function(move |lua, key: String| {
            let binding = read_keybind(lua, &s, &key)?;
            Ok(binding.is_some_and(|binding| st.is_down(&binding)))
        })?,
    )?;
    let (s, st) = (settings.clone(), state.clone());
    let tracker = RefCell::new(PressTracker::default());
    input.raw_set(
        "was_keybind_pressed",
        lua.create_function(move |lua, key: String| {
            let binding = read_keybind(lua, &s, &key)?;
            let (revision, focused) = binding.as_ref().map_or((0, false), |b| st.press(b));
            Ok(tracker
                .borrow_mut()
                .was_pressed(&key, binding.as_ref(), revision, focused))
        })?,
    )?;
    let (s, st) = (settings.clone(), state.clone());
    input.raw_set(
        "keybind_label",
        lua.create_function(move |lua, key: String| {
            Ok(read_keybind(lua, &s, &key)?.map(|binding| st.label(&binding)))
        })?,
    )
}

impl RuntimeHook for InputState {
    fn created(&self, cx: &WorkerContext<'_>, _: &RuntimeContext, side: Side) {
        self.set_side(side);
        self.flush(cx);
    }
    fn turn(&self, cx: &WorkerContext<'_>) {
        self.flush(cx);
        self.turn_start();
    }
    fn stopped(&self, cx: &WorkerContext<'_>) {
        self.stop();
        self.flush(cx);
    }
}

/// The family over `open`: the members for every package; the first client package with a keybind setting starts
/// the key source.
pub fn family(open: Open) -> Family {
    let state = InputState::new();
    let hook: Rc<dyn RuntimeHook> = state.clone();
    Family::new(FAMILY, &CLAIMS, move |lua, _env, _package, composed, cx| {
        let settings = &composed.bindings.settings;
        if state.wants(settings) {
            state.opened(open(cx.services(), cx.owner()));
        }
        install_members(lua, &composed.bindings.shroudforge, settings, &state)
    })
    .with_hook(hook)
}

/// The production family: the input feed on kiln.ui with the real desktop.
pub fn production_family() -> Family {
    family(open_feed(
        Rc::new(|| Box::new(crate::input::SystemDesktop) as Box<dyn Desktop>),
        FeedConfig::default(),
    ))
}
