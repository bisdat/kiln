// SPDX-License-Identifier: MIT
// the interpreter's UI bridge, ShroudForge's notifications.publish.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (1.7.9): modules/mod-runtime/lua/shroudforge/v1/create_mod_api.rs:232-268 (publish),
// modules/notifications/write_notification_event.rs:4-15 (the file name check, then the schema),
// validate_notification_event.rs:4-20 and event-schema.json (the embedded schema: its level enum), with
// jsonschema 0.33.0's enum message (src/error.rs:814-852), the version 1.7.9's Cargo.lock resolves.
//! `shroudforge.notifications.publish(notice)`.
//!
//! ShroudForge's checks, verbatim and in its order: `id`, `title`, `message` read as strings (mlua's conversion error),
//! `level` (default `info`) and `action_url`; then `invalid notification payload` (an id that is not an
//! identifier, a title over 120 bytes, a message over 2,000 bytes), `invalid notification level` (not info, success,
//! warning, error or update) and `notification action_url must use HTTPS`. Those are runtime errors. Then the write:
//! the event file name check and the event schema, whose level enum is trace, debug, info, warn, error, so success,
//! warning and update fail there with the schema's message (1.7.9 stores info and error only); then Kiln's durable
//! write ([`Transport::publish`]: `<configDir>/sfi/notifications`, never ShroudForge's folders). Those are external errors
//! with the text, as ShroudForge's `map_err(mlua::Error::external)` gives them, raised by the same call: nothing is
//! queued for later. Success returns nothing.

use super::transport::{self, Notice, Transport, valid_identifier};
use mlua::{Lua, Table};
use std::rc::Rc;
use std::time::{SystemTime, UNIX_EPOCH};

/// The levels the first check accepts (create_mod_api.rs:248).
pub const PUBLISH_LEVELS: [&str; 5] = ["info", "success", "warning", "error", "update"];
/// event-schema.json's level enum, in its order.
pub const SCHEMA_LEVELS: [&str; 5] = ["trace", "debug", "info", "warn", "error"];

/// write_notification_event.rs:5-14: the event file name ShroudForge accepts.
pub fn valid_event_filename(filename: &str) -> bool {
    !(filename.is_empty()
        || filename.len() > 240
        || filename == "."
        || filename == ".."
        || !filename
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'-' | b'_' | b'+')))
}

/// The event schema's error for a level outside its enum: jsonschema 0.33.0's enum message for five options
/// (`<value> is not one of <first>, <second> or 3 other candidates`); `None` for a level it accepts.
pub fn schema_level_error(level: &str) -> Option<String> {
    if SCHEMA_LEVELS.contains(&level) {
        return None;
    }
    let quote = |text: &str| serde_json::to_string(text).expect("a string serializes");
    Some(format!(
        "{} is not one of {}, {} or {} other candidates",
        quote(level),
        quote(SCHEMA_LEVELS[0]),
        quote(SCHEMA_LEVELS[1]),
        SCHEMA_LEVELS.len() - 2
    ))
}

/// `notifications.publish` of package `mod_id`. `transport` is this start's (`None`: no notice can be written, every
/// publish that passes ShroudForge's checks raises an external error saying so).
pub fn install(
    lua: &Lua,
    api: &Table,
    mod_id: &str,
    transport: Option<Rc<Transport>>,
) -> mlua::Result<()> {
    let notifications = lua.create_table()?;
    let mod_id = mod_id.to_owned();
    notifications.raw_set(
        "publish",
        lua.create_function(move |lua, notice: Table| {
            let id = notice.get::<String>("id")?;
            let title = notice.get::<String>("title")?;
            let message = notice.get::<String>("message")?;
            let level = notice
                .get::<Option<String>>("level")?
                .unwrap_or_else(|| "info".into());
            let action_url = notice.get::<Option<String>>("action_url")?;
            if !valid_identifier(&mod_id)
                || !valid_identifier(&id)
                || title.len() > 120
                || message.len() > 2_000
            {
                return Err(mlua::Error::runtime("invalid notification payload"));
            }
            if !PUBLISH_LEVELS.contains(&level.as_str()) {
                return Err(mlua::Error::runtime("invalid notification level"));
            }
            if action_url
                .as_deref()
                .is_some_and(|url| !url.starts_with("https://"))
            {
                return Err(mlua::Error::runtime(
                    "notification action_url must use HTTPS",
                ));
            }
            let key = format!("{mod_id}-{id}");
            if !valid_event_filename(&key) {
                return Err(mlua::Error::external("invalid notification filename"));
            }
            if let Some(error) = schema_level_error(&level) {
                return Err(mlua::Error::external(error));
            }
            let notice = Notice {
                id: format!("{mod_id}.{id}"),
                mod_id: mod_id.clone(),
                title,
                message,
                level,
                action_url,
                updated_at: SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .unwrap_or_default()
                    .as_secs(),
            };
            let transport = transport.as_ref().ok_or_else(|| {
                mlua::Error::external("notification not written: this start has no notices folder")
            })?;
            transport
                .publish(&key, transport::owner_epoch(lua), notice)
                .map_err(mlua::Error::external)
        })?,
    )?;
    api.raw_set("notifications", notifications)
}
