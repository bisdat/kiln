// SPDX-License-Identifier: MIT
// the interpreter's UI bridge, ShroudForge's ui.toggle_modloader.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (1.7.9; the same in 1.8.0): modules/mod-runtime/lua/shroudforge/v1/create_mod_api.rs:202-205 (the member)
// and :343-377 (request_modloader_toggle), verbatim.
//! `shroudforge.ui.toggle_modloader()`. It signals the named event
//! `Local\ShroudForge.ModMenu.Open.<pid>` and returns nothing; the mod page opens or closes when its owner waits on
//! the event (in kiln mode Kiln.Ui's ShroudForge menu creates it). The signal is the whole
//! answer: no wait for the page and no render acknowledgment here. Without the event the call raises ShroudForge's
//! `Modloader UI toggle bridge is unavailable: <system error>`; off Windows it raises ShroudForge's own text.

use mlua::{Lua, Table};

/// The event's name for this process (create_mod_api.rs:350-353).
pub fn event_name(pid: u32) -> String {
    format!("Local\\ShroudForge.ModMenu.Open.{pid}")
}

/// ShroudForge's request_modloader_toggle (create_mod_api.rs:343-372).
#[cfg(windows)]
pub fn request_modloader_toggle() -> Result<(), String> {
    type Handle = *mut core::ffi::c_void;
    const EVENT_MODIFY_STATE: u32 = 0x0002;
    unsafe extern "system" {
        fn OpenEventW(access: u32, inherit: i32, name: *const u16) -> Handle;
        fn SetEvent(event: Handle) -> i32;
        fn CloseHandle(handle: Handle) -> i32;
    }
    let name = event_name(std::process::id());
    let name: Vec<u16> = name.encode_utf16().chain(Some(0)).collect();
    let event = unsafe { OpenEventW(EVENT_MODIFY_STATE, 0, name.as_ptr()) };
    if event.is_null() {
        return Err(format!(
            "Modloader UI toggle bridge is unavailable: {}",
            std::io::Error::last_os_error()
        ));
    }
    let signaled = unsafe { SetEvent(event) } != 0;
    unsafe { CloseHandle(event) };
    if signaled {
        Ok(())
    } else {
        Err(format!(
            "Could not signal Modloader UI toggle: {}",
            std::io::Error::last_os_error()
        ))
    }
}

/// ShroudForge's request_modloader_toggle off Windows (create_mod_api.rs:374-377).
#[cfg(not(windows))]
pub fn request_modloader_toggle() -> Result<(), String> {
    Err("Modloader UI toggle is only available on Windows clients".into())
}

/// `ui.toggle_modloader`, verbatim (create_mod_api.rs:202-205): no arguments are read, nothing is returned, a failure is
/// a runtime error with the text above.
pub fn install(lua: &Lua, ui: &Table) -> mlua::Result<()> {
    ui.raw_set(
        "toggle_modloader",
        lua.create_function(|_, ()| request_modloader_toggle().map_err(mlua::Error::runtime))?,
    )
}
