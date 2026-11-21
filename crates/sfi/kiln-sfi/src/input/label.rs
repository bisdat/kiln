// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source: modules/hotkeys/parse_hotkey.rs:1-97 (Trigger, Chord, Hotkey::label; the same in 1.8.0 38ef448d);
//         modules/hotkeys/observe_keyboard.rs:132-155 (key_label, mouse_label) and :528-541 (record: Labels);
//         modules/hotkeys/sdl_keyboard.c:151-161 (mouse labels); the Kiln layer of the fork:
//         modules/hotkeys/host_keyboard.c:71-104 (sf_host_keyboard_label).
// Contract pin: 1.7.9 d8a394bc (the host feed exists only in the fork).
//! A keybind setting as the input feed matches it, and its label (`shroudforge.input.keybind_label`).
//!
//! The setting's text is parsed by sfi-package's [`Hotkey`] (ShroudForge's `parse_hotkey`); a
//! [`Binding`] is the same chords in the form the feed and the labels need (trigger, modifiers).
//!
//! Labels follow ShroudForge's `Hotkey::label`: per chord the modifier names it carries (Ctrl, Alt,
//! Shift, Meta, in that order) and the trigger's name, joined by "+"; the keyboard chord before the
//! mouse chord, joined by " / ". A key's name comes from a [`KeyNames`] table, which on Windows is
//! the system's own name of the key in the game window's keyboard layout (`GetKeyNameTextW`, as the
//! host feed of ShroudForge's runtime does), and `SDL:<n>` when the table has none. A mouse button is
//! "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2", else "Mouse <n>".
//!
//! Parity difference (no SDL code is used): where Windows has no name for a key, ShroudForge's
//! runtime falls back to SDL's US scancode name; Kiln's table leaves it empty, so the label is
//! `SDL:<n>` (ShroudForge's own fallback for a key with no name).

use sfi_core::package::settings::Hotkey;

/// What a chord is pressed with (ShroudForge's `Trigger`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Trigger {
    /// An SDL scancode, 1..512.
    Keyboard(u16),
    /// An SDL mouse button, 1..=32.
    Mouse(u8),
}
impl Trigger {
    /// The modifier group the key itself belongs to (a Ctrl key is Ctrl), 0 for other keys and buttons.
    pub fn primary_modifier(self) -> u8 {
        match self {
            Self::Keyboard(code) => primary_modifier(code),
            Self::Mouse(_) => 0,
        }
    }
}

/// Modifier groups: Ctrl 1, Alt 2, Shift 4, Meta 8 (ShroudForge's sdl_keyboard.c groups).
pub const CTRL: u8 = 1;
pub const ALT: u8 = 2;
pub const SHIFT: u8 = 4;
pub const META: u8 = 8;

/// SDL_SCANCODE_LCTRL..=SDL_SCANCODE_RGUI as their modifier group (sdl_keyboard.c:32-41).
pub fn primary_modifier(scancode: u16) -> u8 {
    match scancode {
        224 | 228 => CTRL,
        226 | 230 => ALT,
        225 | 229 => SHIFT,
        227 | 231 => META,
        _ => 0,
    }
}

/// One chord of a binding: the trigger and the modifiers written before it (never the trigger's own group).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Chord {
    pub trigger: Trigger,
    pub modifiers: u8,
}
impl Chord {
    /// The modifier groups that must be held: the written ones and the trigger's own.
    pub fn required_modifiers(&self) -> u8 {
        self.modifiers | self.trigger.primary_modifier()
    }
    fn parts(&self, trigger: String) -> String {
        let mut parts = Vec::with_capacity(5);
        for (bit, name) in [
            (CTRL, "Ctrl"),
            (ALT, "Alt"),
            (SHIFT, "Shift"),
            (META, "Meta"),
        ] {
            if self.modifiers & bit != 0 {
                parts.push(name.to_owned());
            }
        }
        parts.push(trigger);
        parts.join("+")
    }
}

/// A keybind setting's chords: at most one keyboard chord, then at most one mouse chord.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Binding {
    chords: Vec<Chord>,
}

impl Binding {
    /// A setting's text as ShroudForge's `parse_hotkey` reads it: `Ok(None)` for "" (no shortcut),
    /// an error text for anything that is not a canonical binding.
    pub fn parse(text: &str) -> Result<Option<Self>, String> {
        if text.is_empty() {
            return Ok(None);
        }
        Hotkey::parse(text).map(|hotkey| Some(Self::from_hotkey(&hotkey)))
    }

    /// The chords of a parsed [`Hotkey`]. Its canonical text is the only form sfi-package gives, and
    /// one it produced always reads back.
    pub fn from_hotkey(hotkey: &Hotkey) -> Self {
        let mut chords = Vec::with_capacity(2);
        let text = hotkey.normalized();
        for alternative in text.split('|').filter(|part| !part.is_empty()) {
            let mut modifiers = 0;
            let mut trigger = None;
            for part in alternative.split('+') {
                match part {
                    "Ctrl" => modifiers |= CTRL,
                    "Alt" => modifiers |= ALT,
                    "Shift" => modifiers |= SHIFT,
                    "Meta" => modifiers |= META,
                    other => {
                        trigger = if let Some(code) = other.strip_prefix("SDL:") {
                            code.parse().ok().map(Trigger::Keyboard)
                        } else {
                            other
                                .strip_prefix("Mouse:")
                                .and_then(|button| button.parse().ok())
                                .map(Trigger::Mouse)
                        }
                    }
                }
            }
            if let Some(trigger) = trigger {
                chords.push(Chord { trigger, modifiers });
            }
        }
        Self { chords }
    }

    pub fn chords(&self) -> &[Chord] {
        &self.chords
    }

    /// ShroudForge's `Hotkey::normalized`: the canonical text, the per-setting signature
    /// `was_keybind_pressed` keeps.
    pub fn normalized(&self) -> String {
        self.chords
            .iter()
            .map(|chord| {
                chord.parts(match chord.trigger {
                    Trigger::Keyboard(code) => format!("SDL:{code}"),
                    Trigger::Mouse(button) => format!("Mouse:{button}"),
                })
            })
            .collect::<Vec<_>>()
            .join("|")
    }

    /// ShroudForge's `Hotkey::label` with the key names of `names`.
    pub fn label(&self, names: &dyn KeyNames) -> String {
        self.chords
            .iter()
            .map(|chord| {
                chord.parts(match chord.trigger {
                    Trigger::Keyboard(code) => key_label(names, code),
                    Trigger::Mouse(button) => mouse_label(button),
                })
            })
            .collect::<Vec<_>>()
            .join(" / ")
    }
}

/// Key names by SDL scancode.
pub trait KeyNames {
    /// The key's name, or `None` / "" when there is none.
    fn key_name(&self, scancode: u16) -> Option<&str>;
}

/// No names at all: every key is `SDL:<n>` (ShroudForge before its labels arrived).
pub struct NoKeyNames;
impl KeyNames for NoKeyNames {
    fn key_name(&self, _: u16) -> Option<&str> {
        None
    }
}

/// observe_keyboard.rs `key_label`: the table's name, else `SDL:<n>`.
pub fn key_label(names: &dyn KeyNames, scancode: u16) -> String {
    match names.key_name(scancode) {
        Some(name) if !name.is_empty() => name.to_owned(),
        _ => format!("SDL:{scancode}"),
    }
}

/// observe_keyboard.rs `mouse_label` with sdl_keyboard.c's names.
pub fn mouse_label(button: u8) -> String {
    match button {
        1 => "Mouse Left".to_owned(),
        2 => "Mouse Middle".to_owned(),
        3 => "Mouse Right".to_owned(),
        4 => "Mouse X1".to_owned(),
        5 => "Mouse X2".to_owned(),
        _ => format!("Mouse {button}"),
    }
}

/// SDL_SCANCODE_COUNT: the labels table has one entry per scancode below it.
pub const SCANCODE_COUNT: usize = 512;

/// The label table and its layout revision (ShroudForge's `LABELS` and `LAYOUT_REVISION`): a new
/// table counts as a new revision only when it differs from the one it replaces.
#[derive(Clone, Debug, Default)]
pub struct LabelTable {
    names: Vec<String>,
    revision: u64,
}
impl LabelTable {
    /// Replaces the names; true (and a new revision) when they changed.
    pub fn replace(&mut self, names: Vec<String>) -> bool {
        if self.names == names {
            return false;
        }
        self.names = names;
        self.revision += 1;
        true
    }
    pub fn revision(&self) -> u64 {
        self.revision
    }
    pub fn is_empty(&self) -> bool {
        self.names.is_empty()
    }
}
impl KeyNames for LabelTable {
    fn key_name(&self, scancode: u16) -> Option<&str> {
        self.names.get(usize::from(scancode)).map(String::as_str)
    }
}

/// The Windows key-name table the label source gives (`GetKeyNameTextW` for each scancode's
/// set-1 make code, in the keyboard layout `layout`). Worker thread only: when the calling thread's
/// layout differs, it switches that thread's layout for the build and back, also when the build
/// panics (`ActivateKeyboardLayout` without KLF_SETFORPROCESS changes that thread alone), and runs
/// about 500 system calls.
#[cfg(windows)]
pub fn system_key_names(layout: usize) -> Vec<String> {
    use crate::input::feed::set1_of_scancode;
    #[link(name = "user32")]
    unsafe extern "system" {
        fn GetKeyNameTextW(lparam: i32, buffer: *mut u16, size: i32) -> i32;
        fn ActivateKeyboardLayout(layout: usize, flags: u32) -> usize;
        fn GetKeyboardLayout(thread: u32) -> usize;
    }
    /// Gives the thread its own layout back on every way out, a panic included.
    struct Restore(usize);
    impl Drop for Restore {
        fn drop(&mut self) {
            if self.0 != 0 {
                // SAFETY: a plain Win32 call; the thread gets its own layout back.
                unsafe { ActivateKeyboardLayout(self.0, 0) };
            }
        }
    }
    // SAFETY: plain Win32 calls (thread 0: this thread). The thread's layout is switched only when it
    // differs from `layout`; the guard switches it back.
    let _restore = Restore(unsafe {
        if layout != 0 && layout != GetKeyboardLayout(0) {
            ActivateKeyboardLayout(layout, 0)
        } else {
            0
        }
    });
    let mut names = Vec::with_capacity(SCANCODE_COUNT);
    let mut buffer = [0u16; 64];
    for scancode in 0..SCANCODE_COUNT as u16 {
        let key: i32 = match scancode {
            0 => 0,
            // GetKeyNameText tells these two apart the other way round (host_keyboard.c:83-87).
            72 => 0x45 << 16,
            83 => (0x45 << 16) | (1 << 24),
            _ => set1_of_scancode(scancode)
                .map(|index| {
                    (i32::from(index & 0x7f) << 16) | if index & 0x80 != 0 { 1 << 24 } else { 0 }
                })
                .unwrap_or(0),
        };
        let mut name = String::new();
        if key != 0 {
            // SAFETY: the buffer is 64 UTF-16 units long, as passed.
            let length = unsafe { GetKeyNameTextW(key, buffer.as_mut_ptr(), buffer.len() as i32) };
            if length > 0 {
                name = String::from_utf16_lossy(&buffer[..length as usize]);
            }
        }
        names.push(name);
    }
    names
}
