// SPDX-License-Identifier: MIT
//! The release hold of Kiln's interpreter as this build ships it, read the way the core holds it at boot
//! (native/core/src/sfi_release_policy.h, gates.cpp).
//!
//! - The client gate's rows (native/core/gates/sfi.json, compiled in here as it is compiled into the core):
//!   `kiln-sfi.sf18`, and `kiln-sfi.interpreter` while the interpreter is a preview. Held until their guided checks
//!   pass; kiln.json `"preview"` lifts them like any gate row, read as config.cpp reads it ([`preview_lifts`]). A list
//!   with no `kiln-sfi.interpreter` row makes the interpreter a normal choice (the launcher's ShroudForge card offers
//!   it; kiln.json `shroudforge.backend: "kiln"`); hosted ShroudForge stays the default.
//! - On top of the list, the release policy holds, and `"preview"` never lifts: `kiln-sfi.sf18` while this build
//!   records no final ShroudForge 1.8.x ([`UPSTREAM_180_FINAL`], false in every release and preview build; nothing at
//!   run time changes it, no environment variable, kiln.json key, profile or launch plan is read for it), whatever the
//!   list holds; `kiln-sfi.interpreter` when the list holds it on one side only; `kiln-sfi.sf18` in a build whose veto
//!   is open, when the list does not hold it on both sides.
//! - Fail closed: a list that is missing, does not parse or does not pass gen.py's checks holds both rows the same way
//!   (kiln-manage only: the core's build stops on such a list).
//! - Advisory: the launcher's backend card and the plan producer warn with it. The core decides at boot and records the
//!   answer in `kiln/state/launch.json`.

use crate::manifest::RunSide;
use serde::Serialize;
use serde_json::Value;

/// Kiln's interpreter as a ShroudForge backend (kiln.json `shroudforge.backend: "kiln"`).
pub const INTERPRETER: &str = "kiln-sfi.interpreter";
/// Anything that needs ShroudForge 1.8.
pub const SF18: &str = "kiln-sfi.sf18";
/// The record of a final ShroudForge 1.8.x in this build: native/core/src/sfi_release_policy.cpp `kUpstream180Final`
/// (a test pins the two to the same value). Change both only in a reviewed commit naming the upstream release.
pub const UPSTREAM_180_FINAL: bool = false;
/// How the text of a row held by the client gate begins (kiln_caps.h `KILN_CAP_GATE_TEXT`).
pub const GATE_TEXT: &str = "game test pending: ";
/// The veto's text (sfi_release_policy.h `kVetoText`).
pub const VETO_TEXT: &str = "game test pending: the release policy (this Kiln build records no final ShroudForge 1.8.x; kiln.json \"preview\" does not lift it)";
/// `kiln-sfi.interpreter` held on one side only by the list, or `kiln-sfi.sf18` not on both sides with the veto open
/// (sfi_release_policy.h `kUnlistedText`).
pub const UNLISTED_TEXT: &str = "game test pending: the release policy (this build's gate list does not hold it on both sides; kiln.json \"preview\" does not lift it)";
/// A list that cannot be read (kiln-manage only: the core's build stops on such a list).
pub const UNREADABLE_TEXT: &str = "game test pending: the release policy (this build's gate list cannot be read; kiln.json \"preview\" does not lift it)";

/// native/core/gates/sfi.json as this build was made with it.
const SHIPPED_LIST: &str = include_str!("../../../native/core/gates/sfi.json");

/// One row of the gate list (gen.py's reading).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Row {
    pub name: String,
    /// None: held on both sides.
    pub side: Option<RunSide>,
    pub checks: Vec<String>,
}

/// Why a capability is held.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Hold {
    /// In the core's words: it begins with [`GATE_TEXT`].
    pub text: String,
    /// kiln.json `"preview"` would lift it (a row of the list); false for the release policy's rows.
    pub liftable: bool,
}

/// The gate list and the release policy of one build.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReleasePolicy {
    rows: Result<Vec<Row>, String>,
    upstream_180_final: bool,
}

impl ReleasePolicy {
    /// The list and policy this build ships.
    pub fn shipped() -> ReleasePolicy {
        ReleasePolicy::from_list(Some(shipped_list()))
    }

    /// A list's text (None: no list) with this build's record of a final 1.8.x.
    pub fn from_list(text: Option<&str>) -> ReleasePolicy {
        let rows = match text {
            Some(t) => parse_list(t),
            None => Err("no gate list".into()),
        };
        ReleasePolicy { rows, upstream_180_final: UPSTREAM_180_FINAL }
    }

    /// The list's rows; Err with why it cannot be read.
    pub fn rows(&self) -> Result<&[Row], &str> {
        self.rows.as_deref().map_err(String::as_str)
    }

    /// Why `name` is held on `side`, with kiln.json as config.cpp reads it (`config`; None when it cannot,
    /// [`crate::sfi_backend::config_view`]); None when it is not held.
    pub fn hold(&self, name: &str, config: Option<&Value>, side: RunSide) -> Option<Hold> {
        if let Some(text) = self.policy_text(name) {
            return Some(Hold { text: text.into(), liftable: false });
        }
        let rows = self.rows.as_deref().ok()?;
        let row = rows.iter().find(|r| r.name == name && r.side.is_none_or(|s| s == side))?;
        if preview_lifts(config, side, name) {
            return None;
        }
        Some(Hold { text: format!("{GATE_TEXT}{}", row.checks.join(", ")), liftable: true })
    }

    /// The release policy's own row for `name` (sfi_release_policy.cpp `forced`), whatever "preview" says.
    fn policy_text(&self, name: &str) -> Option<&'static str> {
        if name != INTERPRETER && name != SF18 {
            return None;
        }
        if name == SF18 && !self.upstream_180_final {
            return Some(VETO_TEXT);
        }
        let rows = match &self.rows {
            Err(_) => return Some(UNREADABLE_TEXT),
            Ok(rows) => rows,
        };
        let named = rows.iter().any(|r| r.name == name);
        let both = rows.iter().any(|r| r.name == name && r.side.is_none());
        // the interpreter: no row is a normal choice, a row on both sides a gate row, a row on one side only held on
        // both; sf18 with the veto open: held unless its row is on both sides
        let held = if name == INTERPRETER { named && !both } else { !both };
        held.then_some(UNLISTED_TEXT)
    }
}

/// The list [`ReleasePolicy::shipped`] reads: this build's.
#[cfg(not(test))]
fn shipped_list() -> &'static str {
    SHIPPED_LIST
}

/// The list [`ReleasePolicy::shipped`] reads in kiln-manage's own unit tests: this build's, or the one a test set for its
/// thread ([`test_list::with`]).
#[cfg(test)]
fn shipped_list() -> &'static str {
    test_list::get().unwrap_or(SHIPPED_LIST)
}

/// kiln-manage's own unit tests: both forms of the list, and a list in place of the shipped one on this thread.
#[cfg(test)]
pub(crate) mod test_list {
    use std::cell::Cell;

    /// The interpreter as a preview: its row on both sides, with the sf18 row.
    pub(crate) const HELD_FORM: &str = r#"{"gates": [{"name": "kiln-sfi.interpreter", "checks": ["sfi-backend-roundtrip", "sfi-ui-page"]}, {"name": "kiln-sfi.sf18", "checks": ["sfi-sf18-final"]}]}"#;
    /// The interpreter as a normal choice: the sf18 row only.
    pub(crate) const RELEASED_FORM: &str = r#"{"gates": [{"name": "kiln-sfi.sf18", "checks": ["sfi-sf18-final"]}]}"#;
    /// The interpreter's row on the client only (the policy holds it on both sides).
    pub(crate) const ONE_SIDED: &str = r#"{"gates": [{"name": "kiln-sfi.interpreter", "checks": ["a"], "side": "client"}, {"name": "kiln-sfi.sf18", "checks": ["sfi-sf18-final"]}]}"#;

    thread_local! {
        static LIST: Cell<Option<&'static str>> = const { Cell::new(None) };
    }

    /// The list this thread had before [`set`]: back when dropped.
    pub(crate) struct Restore(Option<&'static str>);
    impl Drop for Restore {
        fn drop(&mut self) {
            LIST.with(|l| l.set(self.0));
        }
    }

    /// `list` as the shipped list on this thread until the guard is dropped.
    #[must_use]
    pub(crate) fn set(list: &'static str) -> Restore {
        Restore(LIST.with(|l| l.replace(Some(list))))
    }

    /// Runs `f` with `list` as the shipped list on this thread.
    pub(crate) fn with<T>(list: &'static str, f: impl FnOnce() -> T) -> T {
        let _restore = set(list);
        f()
    }

    pub(super) fn get() -> Option<&'static str> {
        LIST.with(Cell::get)
    }
}

/// kiln.json `"preview"` lifts `name` on `side`, as config.cpp reads it: `true | "all" | ["<capability>", ...]` at the
/// top level, and a `client`/`server` section's `"preview"` replaces the top-level one on that side; any other value
/// lifts nothing. config.cpp reads `policy`, `clr`, `plugins` and `log_level` before the top-level `"preview"`, and a
/// wrong type in one of them stops it there: that `"preview"` is then never read (a side section's still is).
pub fn preview_lifts(config: Option<&Value>, side: RunSide, name: &str) -> bool {
    let lifts = |p: &Value| match p {
        Value::Bool(b) => *b,
        Value::String(s) => s == "all",
        Value::Array(names) => names.iter().any(|n| n.as_str() == Some(name)),
        _ => false,
    };
    let side_name = match side {
        RunSide::Client => "client",
        RunSide::Server => "server",
    };
    let doc = config.filter(|d| d.is_object());
    let section = doc.and_then(|d| d.get(side_name)).filter(|s| s.is_object()).and_then(|s| s.get("preview"));
    // config.cpp load_config: doc.value("policy", ...), "clr", "plugins", "log_level" throw on a wrong type
    let top_read = doc.filter(|d| {
        let typed = |key: &str, ok: fn(&Value) -> bool| d.get(key).is_none_or(ok);
        typed("policy", Value::is_string) && typed("clr", Value::is_boolean) && typed("plugins", Value::is_boolean)
            && typed("log_level", Value::is_string)
    });
    section.or_else(|| top_read.and_then(|d| d.get("preview"))).is_some_and(lifts)
}

/// The gate list as native/core/gates/gen.py reads and checks it: `{"gates": [{name, checks, side?, note?}, ...]}`,
/// a capability name, a non-empty list of distinct lower-case check ids, side "server" or "client" when given (null is
/// both), each name once. Anything else is refused whole.
pub fn parse_list(text: &str) -> Result<Vec<Row>, String> {
    let doc: Value = serde_json::from_str(text).map_err(|e| format!("not JSON ({e})"))?;
    let entries = doc.get("gates").and_then(Value::as_array).ok_or("expected {\"gates\": [...]}")?;
    let name_ok = |s: &str| {
        (1..=96).contains(&s.len()) && s.bytes().all(|b| b.is_ascii_alphanumeric() || b"._/-".contains(&b))
    };
    let check_ok = |s: &str| {
        let mut b = s.bytes();
        b.next().is_some_and(|c| c.is_ascii_digit() || c.is_ascii_lowercase())
            && b.all(|c| c.is_ascii_digit() || c.is_ascii_lowercase() || b"._-".contains(&c))
    };
    let mut rows: Vec<Row> = Vec::new();
    for (i, e) in entries.iter().enumerate() {
        let at = format!("entry {}", i + 1);
        let obj = e.as_object().filter(|o| o.keys().all(|k| ["name", "checks", "side", "note"].contains(&k.as_str())))
            .ok_or_else(|| format!("{at}: an object with name, checks and optionally side, note"))?;
        let name = obj.get("name").and_then(Value::as_str).filter(|n| name_ok(n))
            .ok_or_else(|| format!("{at}: not a capability name"))?;
        let checks: Vec<String> = obj.get("checks").and_then(Value::as_array)
            .filter(|c| !c.is_empty())
            .and_then(|c| c.iter().map(|x| x.as_str().filter(|s| check_ok(s)).map(str::to_string)).collect::<Option<Vec<_>>>())
            .ok_or_else(|| format!("{at} ({name}): checks must be a non-empty list of lower-case check ids"))?;
        if checks.iter().enumerate().any(|(j, c)| checks[..j].contains(c)) {
            return Err(format!("{at} ({name}): a check id is listed twice"));
        }
        let side = match obj.get("side") {
            None | Some(Value::Null) => None,
            Some(Value::String(s)) if s == "client" => Some(RunSide::Client),
            Some(Value::String(s)) if s == "server" => Some(RunSide::Server),
            Some(_) => return Err(format!("{at} ({name}): side must be \"server\" or \"client\" when given")),
        };
        if rows.iter().any(|r| r.name == name) {
            return Err(format!("{at}: {name} is listed twice"));
        }
        rows.push(Row { name: name.to_string(), side, checks });
    }
    rows.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(rows)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn repo_file(path: &str) -> String {
        std::fs::read_to_string(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").join(path)).unwrap()
    }
    const INTERP_CHECKS: &str = "sfi-backend-roundtrip, sfi-save-sensitive, sfi-three-surface-warning, \
                                 sfi-ui-actions-notices, sfi-ui-page, sfi-ui-settings, sfi-warning-accessibility";

    use super::test_list::{HELD_FORM, ONE_SIDED, RELEASED_FORM};

    /// The shipped list holds kiln-sfi.sf18, and kiln-sfi.interpreter only as a preview (both on both sides): with the
    /// row the interpreter is held until "preview" lifts it, without it a normal choice; the veto holds either way.
    #[test]
    fn the_shipped_list_holds_sf18_and_the_interpreter_only_as_a_preview() {
        let p = ReleasePolicy::shipped();
        let rows = p.rows().unwrap();
        let names: Vec<&str> = rows.iter().map(|r| r.name.as_str()).collect();
        let preview = names.contains(&INTERPRETER);
        assert_eq!(names, if preview { vec![INTERPRETER, SF18] } else { vec![SF18] }, "no dev.29 row is held again");
        assert!(rows.iter().all(|r| r.side.is_none()), "{rows:?}");
        for side in [RunSide::Client, RunSide::Server] {
            let h = p.hold(INTERPRETER, None, side);
            if preview {
                assert_eq!(h, Some(Hold { text: format!("{GATE_TEXT}{INTERP_CHECKS}"), liftable: true }));
            } else {
                assert_eq!(h, None, "without its row the interpreter is a normal choice");
            }
            assert_eq!(p.hold(SF18, None, side), Some(Hold { text: VETO_TEXT.into(), liftable: false }));
            assert_eq!(p.hold("camera.fov", None, side), None, "a name the list does not hold is open");
        }
    }

    /// A list with no kiln-sfi.interpreter row (what a build with the interpreter as a normal choice ships; a near name
    /// is no such row): the interpreter is open on both sides, with or without "preview"; nothing lifts the sf18 veto.
    #[test]
    fn without_the_interpreter_row_it_is_open_and_the_veto_holds() {
        let veto = Some(Hold { text: VETO_TEXT.into(), liftable: false });
        let configs = [json!({}), json!({"preview": false}), json!({"preview": true}), json!({"preview": "all"}),
                       json!({"preview": [INTERPRETER, SF18]}), json!({"preview": [SF18]}), json!({"server": {"preview": []}}),
                       json!({"upstream180Final": true, "shroudforge": {"upstream180Final": true, "backend": "kiln"}})];
        for list in [
            RELEASED_FORM,
            r#"{"gates": []}"#,
            r#"{"gates": [{"name": "camera.fov", "checks": ["03-c11"]}]}"#,
            r#"{"gates": [{"name": "Kiln-Sfi.Interpreter", "checks": ["a"]}, {"name": "kiln-sfi.interpreterx", "checks": ["a"]}]}"#,
            r#"{"gates": [{"name": "kiln-sfi.sf18", "checks": ["sfi-sf18-final"], "side": "client"}]}"#,
        ] {
            let p = ReleasePolicy::from_list(Some(list));
            for side in [RunSide::Client, RunSide::Server] {
                assert_eq!(p.hold(INTERPRETER, None, side), None, "{list} {side:?}");
                assert_eq!(p.hold(SF18, None, side), veto, "{list} {side:?}");
                for config in &configs {
                    assert_eq!(p.hold(INTERPRETER, Some(config), side), None, "{list} {side:?} {config}");
                    assert_eq!(p.hold(SF18, Some(config), side), veto, "{list} {side:?} {config}");
                }
            }
        }
    }

    #[test]
    fn preview_lifts_the_interpreter_but_never_the_sf18_veto() {
        let p = ReleasePolicy::from_list(Some(HELD_FORM));
        for (text, side) in [
            (r#"{"preview": true}"#, RunSide::Client),
            (r#"{"preview": "all"}"#, RunSide::Server),
            (r#"{"preview": ["kiln-sfi.interpreter", "kiln-sfi.sf18"]}"#, RunSide::Client),
            (r#"{"server": {"preview": true}}"#, RunSide::Server),
        ] {
            let d: Value = serde_json::from_str(text).unwrap();
            assert_eq!(p.hold(INTERPRETER, Some(&d), side), None, "{text}");
            assert_eq!(p.hold(SF18, Some(&d), side).map(|h| (h.text, h.liftable)), Some((VETO_TEXT.into(), false)), "{text}");
        }
        // preview naming sf18 alone, or words that look like a switch, lift nothing
        #[allow(unused_mut)]
        let mut texts = vec![r#"{"preview": ["kiln-sfi.sf18"]}"#, r#"{"preview": ["upstream180Final"]}"#,
                             r#"{"upstream180Final": true, "shroudforge": {"upstream180Final": true}}"#];
        for text in texts {
            let d: Value = serde_json::from_str(text).unwrap();
            assert!(p.hold(INTERPRETER, Some(&d), RunSide::Client).is_some_and(|h| h.liftable), "{text}");
            assert!(p.hold(SF18, Some(&d), RunSide::Client).is_some_and(|h| !h.liftable), "{text}");
        }
    }

    /// A list that cannot be read holds both rows; a list holding the interpreter on one side only holds it on both; and
    /// "preview" lifts neither.
    #[test]
    fn an_unreadable_list_or_a_one_sided_row_holds_the_interpreter_whatever_preview_says() {
        let all: Value = json!({"preview": true});
        let cases: Vec<(Option<&str>, &str)> = vec![
            (None, UNREADABLE_TEXT),
            (Some(""), UNREADABLE_TEXT),
            (Some("{ nope"), UNREADABLE_TEXT),
            (Some("[]"), UNREADABLE_TEXT),
            (Some(r#"{"gates": {}}"#), UNREADABLE_TEXT),
            (Some(r#"{"gates": [{"name": "kiln-sfi.interpreter", "checks": []}]}"#), UNREADABLE_TEXT),
            (Some(r#"{"gates": [{"name": "kiln-sfi.interpreter", "checks": ["Upper"]}]}"#), UNREADABLE_TEXT),
            (Some(r#"{"gates": [{"name": "kiln-sfi.interpreter", "checks": ["a", "a"]}]}"#), UNREADABLE_TEXT),
            (Some(r#"{"gates": [{"name": "kiln-sfi.interpreter", "checks": ["a"], "side": "both"}]}"#), UNREADABLE_TEXT),
            (Some(r#"{"gates": [{"name": "kiln-sfi.interpreter", "checks": ["a"], "extra": 1}]}"#), UNREADABLE_TEXT),
            (Some(r#"{"gates": [{"name": "kiln sfi", "checks": ["a"]}]}"#), UNREADABLE_TEXT),
            (Some(r#"{"gates": [{"name": "x", "checks": ["a"]}, {"name": "x", "checks": ["b"]}]}"#), UNREADABLE_TEXT),
            (Some(r#"{"gates": [{"name": "kiln-sfi.interpreter", "checks": ["a\n"]}]}"#), UNREADABLE_TEXT),
            (Some(r#"{"gates": [{"name": "kiln-sfi.interpreter", "checks": ["a"], "side": "client"}]}"#), UNLISTED_TEXT),
            (Some(r#"{"gates": [{"name": "kiln-sfi.interpreter", "checks": ["a"], "side": "server"}]}"#), UNLISTED_TEXT),
            (Some(ONE_SIDED), UNLISTED_TEXT),
        ];
        for (text, why) in cases {
            let p = ReleasePolicy::from_list(text);
            for side in [RunSide::Client, RunSide::Server] {
                for config in [None, Some(&all), Some(&json!({"preview": [INTERPRETER]}))] {
                    assert_eq!(p.hold(INTERPRETER, config, side), Some(Hold { text: why.into(), liftable: false }), "{text:?} {side:?}");
                    assert_eq!(p.hold(SF18, config, side), Some(Hold { text: VETO_TEXT.into(), liftable: false }), "{text:?}");
                }
            }
        }
        // a list with the row on both sides behaves as any gate row
        let p = ReleasePolicy::from_list(Some(r#"{"gates": [{"name": "kiln-sfi.interpreter", "checks": ["a"], "side": null}]}"#));
        assert_eq!(p.hold(INTERPRETER, None, RunSide::Server), Some(Hold { text: format!("{GATE_TEXT}a"), liftable: true }));
        assert_eq!(p.hold(INTERPRETER, Some(&all), RunSide::Server), None);
    }

    #[test]
    fn other_rows_follow_their_side_and_preview_as_the_core_does() {
        let p = ReleasePolicy::from_list(Some(
            r#"{"gates": [{"name": "kiln-sfi.interpreter", "checks": ["a"]}, {"name": "markers.compass", "checks": ["04-m03"], "side": "client"}]}"#,
        ));
        assert!(p.hold("markers.compass", None, RunSide::Client).is_some_and(|h| h.liftable));
        assert_eq!(p.hold("markers.compass", None, RunSide::Server), None, "a client-only row holds nothing on a server");
        assert_eq!(p.hold("markers.compass", Some(&json!({"preview": ["markers.compass"]})), RunSide::Client), None);
    }

    /// The Rust and C++ halves say the same: the record of a final 1.8.x, and every policy text, verbatim.
    #[test]
    fn the_core_policy_says_the_same() {
        let cpp = repo_file("native/core/src/sfi_release_policy.cpp");
        let line = format!("constexpr bool kUpstream180Final = {UPSTREAM_180_FINAL};");
        assert_eq!(cpp.matches("kUpstream180Final =").count(), 1);
        assert!(cpp.contains(&line), "sfi_release_policy.cpp must say {line}");
        // the header splits its literals: join them as the compiler does
        let h = repo_file("native/core/src/sfi_release_policy.h");
        let joined = h.replace("\"\n    \"", "");
        for (name, text) in [("kVetoText", VETO_TEXT), ("kUnlistedText", UNLISTED_TEXT)] {
            let literal = serde_json::to_string(text).unwrap();
            let at = joined.find(&format!("{name} =")).unwrap_or_else(|| panic!("no {name}"));
            assert!(joined[at..].trim_start_matches(|c| c != '"').starts_with(&literal), "{name} differs from the core's");
        }
        assert_eq!(GATE_TEXT, "game test pending: ");
        assert!(repo_file("native/core/abi/services/kiln_caps.h").contains(r#"#define KILN_CAP_GATE_TEXT "game test pending: ""#));
    }

    /// Nothing at run time opens the veto: this module reads no environment variable and no file (its unit tests' list
    /// on a thread included).
    #[test]
    fn the_policy_reads_nothing_at_run_time() {
        let src = include_str!("sfi_release.rs");
        let code = &src[..src.find("#[cfg(test)]\nmod tests").unwrap()];
        for word in ["std::env", "env::var", "var_os", "std::fs", "File::open", "read_to_string"] {
            assert!(!code.contains(word), "sfi_release.rs reads something at run time ({word})");
        }
        assert_eq!(code.matches("UPSTREAM_180_FINAL: bool = false;").count(), 1);
        assert_eq!(code.matches("upstream_180_final: UPSTREAM_180_FINAL").count(), 1, "the constant is the only source");
    }

    #[test]
    fn the_shipped_list_reads_as_gen_py_reads_it() {
        // the text compiled in is the file on disk (no stale copy)
        assert_eq!(SHIPPED_LIST, repo_file("native/core/gates/sfi.json"));
        let rows = parse_list(SHIPPED_LIST).unwrap();
        let checks = |name: &str| rows.iter().find(|r| r.name == name).map(|r| r.checks.join(", "));
        assert!(checks(INTERPRETER).is_none_or(|c| c == INTERP_CHECKS), "{rows:?}");
        assert_eq!(checks(SF18).as_deref(), Some("sfi-sf18-final"));
        // the unit tests' forms of the list are lists gen.py would take
        for list in [HELD_FORM, RELEASED_FORM, ONE_SIDED] {
            assert!(parse_list(list).is_ok(), "{list}");
        }
        assert!(test_list::with(RELEASED_FORM, || ReleasePolicy::shipped().hold(INTERPRETER, None, RunSide::Client)).is_none());
        assert_eq!(ReleasePolicy::shipped().rows().unwrap(), &rows[..], "the thread's list is gone again");
    }
}
