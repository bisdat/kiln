// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (both pins): modules/mod-runtime/lua/shroudforge/v1/read_ui_locale.rs:5-10;
// create_mod_api.rs:208-230 (1.7.9), :236-258 (1.8.0).
use crate::package::{locales::localized_string, manifest::ModLocaleCatalog};
use mlua::{Lua, Table};
use std::{collections::BTreeMap, rc::Rc};

/// Kiln-owned validated configuration, including its merged defaults. Evaluate on
/// every call. None means unreadable/invalid/missing locale; no SF path is consulted.
pub trait LocaleSource {
    fn locale(&self) -> Option<String>;
}

pub(crate) fn install(
    lua: &Lua,
    api: &Table,
    source: Rc<dyn LocaleSource>,
    catalogs: BTreeMap<String, ModLocaleCatalog>,
) -> mlua::Result<()> {
    api.raw_set(
        "locale",
        lua.create_function(move |_, ()| {
            Ok(source
                .locale()
                .filter(|locale| !locale.is_empty())
                .unwrap_or_else(|| "en".to_owned()))
        })?,
    )?;
    let i18n = crate::bindings::common::child(lua, api, "i18n")?;
    i18n.raw_set(
        "get",
        lua.create_function(
            move |_, (key, locale, fallback): (String, String, Option<String>)| {
                Ok(localized_string(
                    &catalogs,
                    &key,
                    &locale,
                    fallback.as_deref(),
                ))
            },
        )?,
    )
}
