// SPDX-License-Identifier: MIT
//! kiln.json `"shroudforge": {"backend": ...}`: which ShroudForge backend a game asks for, read and written the way the
//! core decides it at boot (native/core/src/launch.cpp `read_setting`/`decide`, docs/kiln/services/launch.md).
//!
//! - Values: `hosted` (Kiln's bundled ShroudForge runtime, the default), `installed` (the player's own ShroudForge in
//!   `<game>/shroudforge`), `kiln` (Kiln's interpreter, kiln-sfi). Game level: the top-level `shroudforge` section,
//!   never a `client`/`server` section.
//! - Precedence: an explicit `backend` > the older `useInstalled: true` (installed) > hosted. Any other `backend`
//!   value is refused: every ShroudForge backend stands aside that start, as the core rules.
//! - `kiln` is refused while the capability `kiln-sfi.interpreter` is held ([`interpreter_held`]): [`resolve`] says
//!   "refused", and [`set_backend`] does not write it.
//! - One write never leaves both keys: [`set_backend`] writes `backend` and removes `useInstalled`.
//! - The environment as the game process reads it ([`Env`]): `KILN_SHROUDFORGE_RUNTIME=installed|bundled` chooses
//!   between hosted and installed, never over `kiln`; `KILN_SHROUDFORGE=0` stands every ShroudForge backend aside,
//!   Kiln's interpreter included, and the note stays main's ([`crate::own_loaders::RUNTIME_ENV_OFF_NOTE`]).
//!
//! Not decided here: the core's idle class (safe idle, play-once no-mods, an invalid profile, `"plugins": false`) and
//! the launch-plan checks of a `kiln` start. The core freezes the real answer at boot (kiln.launch in the process,
//! `kiln/state/launch.json` for the launcher).

use crate::manifest::RunSide;
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::path::Path;

/// The held capability a `kiln` request needs open.
pub const INTERPRETER_CAPABILITY: &str = "kiln-sfi.interpreter";

/// A ShroudForge backend (kiln.launch's `KILN_LAUNCH_BACKEND_*` other than none).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Backend {
    Hosted,
    Installed,
    Kiln,
}

impl Backend {
    pub const ALL: [Backend; 3] = [Backend::Hosted, Backend::Installed, Backend::Kiln];

    pub fn as_str(self) -> &'static str {
        match self {
            Backend::Hosted => "hosted",
            Backend::Installed => "installed",
            Backend::Kiln => "kiln",
        }
    }

    /// Exactly one of the three words (case matters, as in the core).
    pub fn parse(s: &str) -> Option<Backend> {
        Backend::ALL.into_iter().find(|b| b.as_str() == s)
    }
}

/// The kiln.json setting alone (the core's `read_setting`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Setting {
    /// No usable setting (no file, no `shroudforge` object, neither key, `useInstalled` not `true`): hosted.
    Default,
    /// `backend` with one of the three values.
    Backend(Backend),
    /// `useInstalled: true` without `backend`: installed.
    UseInstalled,
    /// `backend` with any other value, as written: a string in quotes, anything else as its JSON.
    Invalid(String),
}

/// Where the backend a start gets comes from (kiln.launch reason codes 0x001-0x004).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum Source {
    Default,
    Setting,
    UseInstalled,
    EnvRuntime,
}

/// Why every ShroudForge backend stands aside (kiln.launch's refused class).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum Refusal {
    /// `backend` is not hosted, installed or kiln (`KILN_LAUNCH_REASON_SETTING_INVALID`).
    SettingInvalid,
    /// `kiln` while `kiln-sfi.interpreter` is held (`KILN_LAUNCH_REASON_INTERPRETER_HELD`).
    InterpreterHeld,
}

impl Refusal {
    /// kiln.launch's reason code.
    pub fn code(self) -> u32 {
        match self {
            Refusal::SettingInvalid => 0x100,
            Refusal::InterpreterHeld => 0x101,
        }
    }
}

/// What a game's next start gets from its kiln.json and the environment, before the core's idle class and plan checks.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Resolution {
    /// What kiln.json asks for; None for an invalid `backend`.
    pub requested: Option<Backend>,
    /// The backend that start gets; None: refused, every ShroudForge backend stands aside.
    pub selected: Option<Backend>,
    pub source: Source,
    pub refusal: Option<Refusal>,
    /// One line for a log or a card, in the core's words.
    pub reason: String,
    /// `KILN_SHROUDFORGE=0` with a backend selected: every ShroudForge backend stands aside that start (Kiln's
    /// interpreter included; the core's record still names the backend), and the note an EML package that needs
    /// ShroudForge's runtime shows is main's.
    pub env_off_note: Option<&'static str>,
}

/// The two variables, as the game process reads them (pass them explicitly in tests).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Env {
    /// `KILN_SHROUDFORGE`: only the exact value `0` switches kiln-shroudforge off.
    pub shroudforge: Option<String>,
    /// `KILN_SHROUDFORGE_RUNTIME`: `installed` is installed, any other value of 1 to 15 characters hosted.
    pub runtime: Option<String>,
}

impl Env {
    pub fn from_process() -> Env {
        Env { shroudforge: std::env::var("KILN_SHROUDFORGE").ok(), runtime: std::env::var("KILN_SHROUDFORGE_RUNTIME").ok() }
    }
}

/// The core's `read_setting` on a parsed kiln.json (None: no file, or one that does not parse).
pub fn read_setting(doc: Option<&Value>) -> Setting {
    let Some(sf) = doc.and_then(|d| d.get("shroudforge")).and_then(Value::as_object) else { return Setting::Default };
    if let Some(b) = sf.get("backend") {
        // the new key wins over useInstalled, whatever useInstalled says
        return match b.as_str().and_then(Backend::parse) {
            Some(backend) => Setting::Backend(backend),
            None => Setting::Invalid(match b {
                Value::String(s) => format!("\"{s}\""),
                other => other.to_string(),
            }),
        };
    }
    // the older key, as kiln-shroudforge reads it: true means installed; any other value leaves the default
    if sf.get("useInstalled") == Some(&Value::Bool(true)) { Setting::UseInstalled } else { Setting::Default }
}

/// Why `kiln-sfi.interpreter` is held for `side` (the core's words); None when it is not: this build's list has no row
/// of it (a normal choice), or kiln.json's `"preview"` lifts its row.
///
/// The client gate's row as this build ships it, with the release policy on top ([`crate::sfi_release`]): held until
/// its guided checks pass, lifted by `"preview"` read as config.cpp reads it ([`crate::sfi_release::preview_lifts`]),
/// and held whatever `"preview"` says when this build's list holds the row on one side only. `doc` is kiln.json
/// as config.cpp parses it (None when it cannot: [`config_view`]).
pub fn interpreter_held(doc: Option<&Value>, side: RunSide) -> Option<String> {
    crate::sfi_release::ReleasePolicy::shipped().hold(INTERPRETER_CAPABILITY, doc, side).map(|h| h.text)
}

/// kiln.json as config.cpp parses it, from the tree of its text: nlohmann with comments allowed and no trailing comma.
/// A trailing comma anywhere makes config.cpp read none of the file (every key at its default, no `"preview"`), while
/// the backend setting is still read (launch.cpp reads kiln.json through jsonc_to_json, which drops trailing commas).
pub fn config_view<'a>(tree: &crate::jsonc::Node, doc: &'a Value) -> Option<&'a Value> {
    fn trailing_comma(n: &crate::jsonc::Node) -> bool {
        use crate::jsonc::JValue;
        match &n.value {
            JValue::Object(members) => {
                members.last().is_some_and(|m| m.comma.is_some()) || members.iter().any(|m| trailing_comma(&m.value))
            }
            JValue::Array(items) => {
                items.last().is_some_and(|e| e.comma.is_some()) || items.iter().any(|e| trailing_comma(&e.node))
            }
            _ => false,
        }
    }
    (!trailing_comma(tree)).then_some(doc)
}

/// What the next start of a game with this kiln.json gets on `side`. `doc` serves both of the core's readers; with a
/// file's text, [`resolve_game`] tells them apart ([`config_view`]).
pub fn resolve(doc: Option<&Value>, side: RunSide, env: &Env) -> Resolution {
    resolve_parts(doc, doc, side, env)
}

/// `doc`: kiln.json as launch.cpp reads it (the setting); `config`: as config.cpp reads it (the preview lift).
fn resolve_parts(doc: Option<&Value>, config: Option<&Value>, side: RunSide, env: &Env) -> Resolution {
    let refused = |requested, refusal, reason| Resolution { requested, selected: None, source: Source::Setting, refusal: Some(refusal), reason, env_off_note: None };
    let (requested, source, said) = match read_setting(doc) {
        Setting::Invalid(value) => {
            return refused(None, Refusal::SettingInvalid,
                format!("kiln.json shroudforge.backend: {value} is not \"hosted\", \"installed\" or \"kiln\""));
        }
        Setting::Backend(Backend::Kiln) => {
            if let Some(why) = interpreter_held(config, side) {
                return refused(Some(Backend::Kiln), Refusal::InterpreterHeld, format!(
                    "kiln.json asks for Kiln's interpreter (shroudforge.backend: \"kiln\"), but the capability \
                     {INTERPRETER_CAPABILITY} is held ({why})"));
            }
            // the launch plan the profile names decides the rest at boot; KILN_SHROUDFORGE_RUNTIME never overrides kiln,
            // and KILN_SHROUDFORGE=0 stands the interpreter aside too
            return Resolution { requested: Some(Backend::Kiln), selected: Some(Backend::Kiln), source: Source::Setting,
                                refusal: None, reason: "kiln.json shroudforge.backend: \"kiln\"".into(),
                                env_off_note: env_off_note(env) };
        }
        Setting::Backend(b) => (b, Source::Setting, format!("kiln.json shroudforge.backend: \"{}\"", b.as_str())),
        Setting::UseInstalled => (Backend::Installed, Source::UseInstalled,
                                  "kiln.json shroudforge.useInstalled: true (the older setting): installed".to_string()),
        Setting::Default => (Backend::Hosted, Source::Default, if doc.is_some() {
            "no kiln.json setting: hosted, the default".to_string()
        } else {
            "no kiln.json, or kiln.json does not parse: hosted, the default".to_string()
        }),
    };
    // KILN_SHROUDFORGE_RUNTIME, read into 16 wchar_t as kiln-shroudforge reads it: 1 to 15 characters count
    let runtime = env.runtime.as_deref().filter(|v| !v.is_empty() && v.encode_utf16().count() < 16);
    let (selected, source, reason) = match runtime {
        Some(v) => {
            let b = if v == "installed" { Backend::Installed } else { Backend::Hosted };
            (b, Source::EnvRuntime, format!("KILN_SHROUDFORGE_RUNTIME={v}: {}, over {said}", b.as_str()))
        }
        None => (requested, source, said),
    };
    Resolution { requested: Some(requested), selected: Some(selected), source, refusal: None, reason, env_off_note: env_off_note(env) }
}

/// Main's note while `KILN_SHROUDFORGE=0` (only the exact value 0, as kiln-shroudforge reads it).
fn env_off_note(env: &Env) -> Option<&'static str> {
    (env.shroudforge.as_deref() == Some("0")).then_some(crate::own_loaders::RUNTIME_ENV_OFF_NOTE)
}

/// [`resolve`] for `<game>/kiln/kiln.json`, read as the core reads it: a UTF-8 BOM skipped (nlohmann skips it), the
/// setting with comments and trailing commas, the preview lift as config.cpp reads it ([`config_view`]); a missing or
/// unparsable file is the default.
pub fn resolve_game(game: &Path, side: RunSide, env: &Env) -> Resolution {
    resolve_bytes(std::fs::read(kiln_json(game)).ok().as_deref(), side, env)
}

/// [`resolve_game`] on kiln.json's bytes (None: no file).
pub fn resolve_bytes(bytes: Option<&[u8]>, side: RunSide, env: &Env) -> Resolution {
    let parsed = bytes.and_then(|b| crate::jsonc::parse_tree_bytes(b).ok());
    let doc = parsed.as_ref().map(|(_, tree)| tree.to_value());
    let config = parsed.as_ref().zip(doc.as_ref()).and_then(|((_, tree), doc)| config_view(tree, doc));
    resolve_parts(doc.as_ref(), config, side, env)
}

fn kiln_json(game: &Path) -> std::path::PathBuf {
    game.join(crate::paths::game::KILN).join("kiln.json")
}

/// kiln.json text with `shroudforge.backend` set to `backend` (None: back to the default, `backend` removed) and the
/// older `useInstalled` removed in the same edit. Comments and layout stay as they are, a UTF-8 BOM too.
pub fn set_backend(text: &str, backend: Option<Backend>, side: RunSide) -> Result<String> {
    use crate::jsonc::{remove_value, set_value, split_path};
    let (bom, text) = text.strip_prefix('\u{feff}').map_or(("", text), |rest| ("\u{feff}", rest));
    let text = if text.trim().is_empty() { "{}\n" } else { text };
    let tree = crate::jsonc::parse_tree(text)?;
    let doc = tree.to_value();
    if !doc.is_object() {
        return Err(Error::new(ErrorCode::Schema, "kiln.json is not an object"));
    }
    if doc.get("shroudforge").is_some_and(|s| !s.is_object()) {
        return Err(Error::new(ErrorCode::Schema, "kiln.json shroudforge is not an object"));
    }
    if backend == Some(Backend::Kiln)
        && let Some(why) = interpreter_held(config_view(&tree, &doc), side)
    {
        return Err(Error::new(ErrorCode::Refused, format!(
            "Kiln's interpreter cannot be chosen: the capability {INTERPRETER_CAPABILITY} is held ({why})")));
    }
    // every useInstalled member goes (a duplicate key would otherwise be the one readers take next)
    let (old, new) = (split_path("shroudforge.useInstalled"), split_path("shroudforge.backend"));
    let mut out = text.to_string();
    for _ in 0..crate::jsonc::MAX_DEPTH {
        let next = remove_value(&out, &old)?;
        if next == out {
            break;
        }
        out = next;
    }
    out = match backend {
        Some(b) => set_value(&out, &new, &Value::String(b.as_str().into()))?,
        None => remove_value(&out, &new)?,
    };
    // what readers take from the result is what was asked, and only one key is left
    let after = crate::jsonc::parse(&out)?;
    let expected = backend.map_or(Setting::Default, Setting::Backend);
    if read_setting(Some(&after)) != expected || after["shroudforge"].get("useInstalled").is_some() {
        return Err(Error::new(ErrorCode::Internal, "kiln.json shroudforge.backend could not be written as asked"));
    }
    Ok(format!("{bom}{out}"))
}

/// [`set_backend`] on `<game>/kiln/kiln.json`, written atomically (a missing file is created).
pub fn write_game(game: &Path, backend: Option<Backend>, side: RunSide) -> Result<()> {
    let file = kiln_json(game);
    let text = match std::fs::read_to_string(&file) {
        Ok(t) => t,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => String::new(),
        Err(e) => return Err(crate::util::io_at(&file, "reading")(e)),
    };
    let out = set_backend(&text, backend, side)?;
    crate::util::write_atomic(&file, out.as_bytes())
}

// ---- the launcher's ShroudForge card (Settings) ----

/// kiln.json's setting as the card shows it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(tag = "kind", rename_all = "camelCase")]
pub enum SettingView {
    /// No setting: hosted, the default.
    Default,
    /// `backend`, as the card writes it (or a hand edit).
    Explicit { backend: Backend },
    /// The older `useInstalled: true` and no `backend`: installed, until a choice replaces it.
    Inherited { backend: Backend },
    /// `backend` with a value no reader takes, as written ([`Setting::Invalid`]).
    Invalid { value: String },
}

impl SettingView {
    fn of(s: Setting) -> SettingView {
        match s {
            Setting::Default => SettingView::Default,
            Setting::Backend(backend) => SettingView::Explicit { backend },
            Setting::UseInstalled => SettingView::Inherited { backend: Backend::Installed },
            Setting::Invalid(value) => SettingView::Invalid { value },
        }
    }
}

/// What a selected `kiln` meets in the game folder (sfi_routing's inputs past the setting).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum InterpreterRoute {
    /// It has the ShroudForge-format mods of the next start.
    Ready,
    /// `KILN_SHROUDFORGE=0` in the launcher's environment stands it aside.
    EnvOff,
    /// `kiln/plugins/kiln-sfi.dll` is not in the game.
    Missing,
    /// ShroudForge's own loader is in the game: it runs them, and both Kiln backends stand aside.
    OwnLoader,
    /// EML-R's own loader is in the game and may run the same mods.
    EmlR,
}

/// The last start as the core froze it (`kiln/state/launch.json`; docs/kiln/services/launch.md).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LastStart {
    /// `hosted`, `installed`, `kiln`, or None (`none`: refused or idle).
    pub backend: Option<String>,
    /// `selected`, `refused` or `idle`.
    pub reason_class: Option<String>,
    pub reason: Option<String>,
    pub pid: Option<u64>,
}

/// Everything the launcher's ShroudForge card shows for one game, from one read of kiln.json.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Card {
    pub setting: SettingView,
    /// What the next start gets (the core's idle class and the launch plan aside).
    pub next: Resolution,
    /// Why `kiln` cannot be chosen now; None: it can. ShroudForge 1.8 is never a choice (its veto is not read here).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub interpreter_held: Option<String>,
    /// With `kiln` selected for the next start: what it meets in the game folder.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub interpreter_route: Option<InterpreterRoute>,
    /// The game-relative path of the loader blocking the interpreter, read only with `kiln` selected.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub own_loader: Option<String>,
    /// kiln.json cannot be read or does not parse: why. Nothing is written until it is fixed.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub unreadable: Option<String>,
    /// sha256 of kiln.json's bytes ("absent": no file). A choice is written only over the bytes it was made on.
    pub revision: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub last: Option<LastStart>,
}

/// What choosing `backend` (None: back to the default) would do; nothing is written.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Preview {
    pub backend: Option<Backend>,
    /// The kiln.json bytes the preview was made on ([`Card::revision`]).
    pub revision: String,
    /// The next start's answer with the choice written; None when it cannot be written (`refused`).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub next: Option<Resolution>,
    /// Why the choice cannot be written (the hold, an unreadable kiln.json).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub refused: Option<String>,
    /// kiln.json would stay byte for byte as it is.
    pub unchanged: bool,
}

/// kiln.json's bytes: None when there is no file; Err for any other read error.
fn read_bytes(game: &Path) -> Result<Option<Vec<u8>>> {
    let file = kiln_json(game);
    match std::fs::read(&file) {
        Ok(b) => Ok(Some(b)),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(crate::util::io_at(&file, "reading")(e)),
    }
}

/// [`Card::revision`] of kiln.json's bytes.
pub fn revision_of(bytes: Option<&[u8]>) -> String {
    bytes.map_or_else(|| "absent".to_string(), crate::hash::sha256_hex)
}

/// The text [`set_backend`] edits, or why there is none.
fn text_of(bytes: Option<&[u8]>) -> std::result::Result<&str, String> {
    match bytes {
        None => Ok(""),
        Some(b) => std::str::from_utf8(b).map_err(|_| "kiln.json is not UTF-8 text".to_string()),
    }
}

/// The card of `game` on `side`, kiln.json read once.
pub fn card(game: &Path, side: RunSide, env: &Env) -> Result<Card> {
    let bytes = read_bytes(game)?;
    Ok(card_bytes(game, bytes.as_deref(), side, env))
}

/// [`card`] on kiln.json's bytes (None: no file).
pub fn card_bytes(game: &Path, bytes: Option<&[u8]>, side: RunSide, env: &Env) -> Card {
    let parsed = bytes.map(crate::jsonc::parse_tree_bytes);
    let unreadable = match (&parsed, text_of(bytes)) {
        (_, Err(e)) => Some(e),
        (Some(Err(e)), _) => Some(format!("kiln.json does not parse: {e}")),
        _ => None,
    };
    let tree = parsed.and_then(|p| p.ok());
    let doc = tree.as_ref().map(|(_, t)| t.to_value());
    let config = tree.as_ref().zip(doc.as_ref()).and_then(|((_, t), d)| config_view(t, d));
    let next = resolve_parts(doc.as_ref(), config, side, env);
    #[cfg_attr(not(feature = "sfi-scan"), allow(unused_mut))]
    let (mut interpreter_route, mut own_loader) = (None, None);
    // Without sfi-scan the card shows the setting without probing for interpreter blockers.
    #[cfg(feature = "sfi-scan")]
    if next.selected == Some(Backend::Kiln) {
        use crate::sfi_routing::{Legacy, Route, Runner};
        let route = crate::sfi_routing::route_from(game, &next);
        (interpreter_route, own_loader) = match route {
            Route::Interpreter(Runner::Ready) => (Some(InterpreterRoute::Ready), None),
            Route::Interpreter(Runner::EnvOff) => (Some(InterpreterRoute::EnvOff), None),
            Route::Interpreter(Runner::Missing) => (Some(InterpreterRoute::Missing), None),
            Route::Legacy(Legacy::EmlR(file)) => {
                (Some(InterpreterRoute::EmlR), Some(file))
            }
            Route::Legacy(Legacy::OwnLoader(file)) => {
                (Some(InterpreterRoute::OwnLoader), Some(file))
            }
            Route::Legacy(_) => (None, None),
        };
    }
    Card {
        setting: SettingView::of(read_setting(doc.as_ref())),
        next,
        interpreter_held: interpreter_held(config, side),
        interpreter_route,
        own_loader,
        unreadable,
        revision: revision_of(bytes),
        last: last_start(game),
    }
}

/// What choosing `backend` would do in `game`, from one read of kiln.json; writes nothing.
pub fn preview(game: &Path, backend: Option<Backend>, side: RunSide, env: &Env) -> Result<Preview> {
    let bytes = read_bytes(game)?;
    Ok(preview_bytes(bytes.as_deref(), backend, side, env))
}

/// [`preview`] on kiln.json's bytes (None: no file).
pub fn preview_bytes(bytes: Option<&[u8]>, backend: Option<Backend>, side: RunSide, env: &Env) -> Preview {
    let revision = revision_of(bytes);
    let written = text_of(bytes).and_then(|t| set_backend(t, backend, side).map_err(|e| e.message));
    match written {
        Ok(out) => Preview {
            backend,
            unchanged: bytes.map_or(out.is_empty(), |b| b == out.as_bytes()),
            next: Some(resolve_bytes(Some(out.as_bytes()), side, env)),
            refused: None,
            revision,
        },
        Err(why) => Preview { backend, revision, next: None, refused: Some(why), unchanged: false },
    }
}

/// [`write_game`], only over the kiln.json bytes `revision` names ([`Card::revision`]): a file changed since the card
/// read it is a Conflict, and nothing is written. Returns the card of the file as written.
///
/// The write is a compare-and-replace ([`replace_if_unchanged`]): the new file is written to a temporary file first,
/// then kiln.json is read again and compared with the bytes the change was made on, and only then renamed over it. No
/// file lock guards kiln.json: the launcher's other writer (the kiln.json settings card, `kiln_set`) and kilnctl's
/// policy write take none, the core only reads it at boot, and a hand edit in an editor cannot take one, so a lock
/// here would order nothing that matters. What is left is the moment between that last read and the rename (one read
/// of a small file and one rename, no parse or edit in between): a hand edit saved exactly then is replaced by the
/// card's file. The file is never torn (the rename is atomic), and the card shown afterwards is read from the file.
pub fn write_game_at(game: &Path, backend: Option<Backend>, side: RunSide, revision: &str, env: &Env) -> Result<Card> {
    write_game_at_with(game, backend, side, revision, env, &|| {})
}

const CONFLICT: &str = "kiln.json changed since the ShroudForge card read it: nothing was written; look at the card again";

/// [`write_game_at`] with `meanwhile` run after the new file is staged and before kiln.json is compared again (tests).
fn write_game_at_with(game: &Path, backend: Option<Backend>, side: RunSide, revision: &str, env: &Env,
    meanwhile: &dyn Fn()) -> Result<Card> {
    let bytes = read_bytes(game)?;
    if revision_of(bytes.as_deref()) != revision {
        return Err(Error::new(ErrorCode::Conflict, CONFLICT));
    }
    let text = text_of(bytes.as_deref()).map_err(|e| Error::new(ErrorCode::Parse, e))?;
    let out = set_backend(text, backend, side)?;
    if bytes.as_deref() != Some(out.as_bytes()) {
        replace_if_unchanged(game, bytes.as_deref(), out.as_bytes(), meanwhile)?;
    }
    card(game, side, env)
}

/// Replaces kiln.json with `data` only while it still holds `expected` (None: no file): `data` goes to a temporary
/// file of its own name (created exclusively, as [`crate::util::write_atomic_exclusive`] does, so another writer's
/// `kiln.json.tmp` is never shared), kiln.json is read again, and the temporary file is renamed over it when that
/// read is `expected`; otherwise it is removed and the answer is a Conflict.
fn replace_if_unchanged(game: &Path, expected: Option<&[u8]>, data: &[u8], meanwhile: &dyn Fn()) -> Result<()> {
    use std::io::Write;
    let path = kiln_json(game);
    let dir = path.parent().expect("kiln folder");
    std::fs::create_dir_all(dir).map_err(crate::util::io_at(dir, "creating"))?;
    let mut staged = None;
    for _ in 0..16 {
        let hex: String = crate::util::random_bytes::<8>()?.iter().map(|b| format!("{b:02x}")).collect();
        let tmp = dir.join(format!(".kiln.json.{hex}.kiln-tmp"));
        match std::fs::OpenOptions::new().write(true).create_new(true).open(&tmp) {
            Ok(f) => {
                staged = Some((tmp, f));
                break;
            }
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(crate::util::io_at(&tmp, "writing")(e)),
        }
    }
    let (tmp, mut f) = staged.ok_or_else(|| Error::new(ErrorCode::Io, format!("{}: no free temporary file name", dir.display())))?;
    let result = f
        .write_all(data)
        .map_err(crate::util::io_at(&tmp, "writing"))
        .and_then(|_| f.sync_all().map_err(crate::util::io_at(&tmp, "flushing")))
        .and_then(|_| {
            drop(f);
            meanwhile();
            if read_bytes(game)?.as_deref() != expected {
                return Err(Error::new(ErrorCode::Conflict, CONFLICT));
            }
            crate::util::rename_replace(&tmp, &path)
        });
    if result.is_err() {
        let _ = std::fs::remove_file(&tmp);
    }
    result
}

/// The largest launch.json the card reads (the core writes a few hundred bytes).
const LAST_START_MAX: u64 = 64 * 1024;

/// `kiln/state/launch.json` as the core wrote it at its last boot; None when there is none or it cannot be read.
pub fn last_start(game: &Path) -> Option<LastStart> {
    use std::io::Read;
    let file = game.join(crate::paths::game::STATE).join("launch.json");
    let mut text = String::new();
    std::fs::File::open(file).ok()?.take(LAST_START_MAX + 1).read_to_string(&mut text).ok()?;
    if text.len() as u64 > LAST_START_MAX {
        return None;
    }
    let v: Value = serde_json::from_str(&text).ok()?;
    if v.get("format").and_then(Value::as_u64) != Some(1) {
        return None;
    }
    let s = |k: &str| v.get(k).and_then(Value::as_str).map(|s| s.chars().take(1024).collect::<String>());
    Some(LastStart {
        backend: s("backend").filter(|b| b != "none"),
        reason_class: s("reasonClass"),
        reason: s("reason"),
        pid: v.get("pid").and_then(Value::as_u64),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sfi_release::test_list::{self, HELD_FORM, ONE_SIDED, RELEASED_FORM};
    use serde_json::json;

    fn doc(text: &str) -> Option<Value> {
        Some(crate::jsonc::parse(text).unwrap())
    }
    fn env() -> Env {
        Env::default()
    }
    fn open() -> &'static str {
        r#""preview": ["kiln-sfi.interpreter"]"#
    }
    fn res(text: &str) -> Resolution {
        resolve(doc(text).as_ref(), RunSide::Client, &env())
    }

    #[test]
    fn precedence_backend_over_use_installed_over_hosted() {
        // (kiln.json, selected, source): the core's read_setting table (native/core/tests/launch_test.cpp)
        let open = open();
        let rows: Vec<(String, Option<Backend>, Source)> = vec![
            ("{}".into(), Some(Backend::Hosted), Source::Default),
            (r#"{"shroudforge": {}}"#.into(), Some(Backend::Hosted), Source::Default),
            (r#"{"shroudforge": true}"#.into(), Some(Backend::Hosted), Source::Default),
            (r#"{"shroudforge": {"useInstalled": true}}"#.into(), Some(Backend::Installed), Source::UseInstalled),
            (r#"{"shroudforge": {"useInstalled": false}}"#.into(), Some(Backend::Hosted), Source::Default),
            (r#"{"shroudforge": {"useInstalled": "yes"}}"#.into(), Some(Backend::Hosted), Source::Default),
            (r#"{"shroudforge": {"backend": "hosted"}}"#.into(), Some(Backend::Hosted), Source::Setting),
            (r#"{"shroudforge": {"backend": "installed"}}"#.into(), Some(Backend::Installed), Source::Setting),
            (r#"{"shroudforge": {"backend": "hosted", "useInstalled": true}}"#.into(), Some(Backend::Hosted), Source::Setting),
            (r#"{"shroudforge": {"useInstalled": true, "backend": "hosted"}}"#.into(), Some(Backend::Hosted), Source::Setting),
            (r#"{"shroudforge": {"backend": "installed", "useInstalled": false}}"#.into(), Some(Backend::Installed), Source::Setting),
            (format!(r#"{{{open}, "shroudforge": {{"backend": "kiln", "useInstalled": true}}}}"#), Some(Backend::Kiln), Source::Setting),
            // a side section's shroudforge is not read (game level only)
            (r#"{"client": {"shroudforge": {"backend": "installed"}}}"#.into(), Some(Backend::Hosted), Source::Default),
            // comments and trailing commas, as the core's jsonc_to_json reads them
            ("{\n  // mine\n  \"shroudforge\": {\"backend\": \"installed\",},\n}".into(), Some(Backend::Installed), Source::Setting),
            // trailing commas around the older key too (the core's jsonc_to_json drops them)
            (r#"{"shroudforge": {"useInstalled": true,},}"#.into(), Some(Backend::Installed), Source::UseInstalled),
            // duplicate keys: the last one counts
            (r#"{"shroudforge": {"backend": "installed", "backend": "hosted"}}"#.into(), Some(Backend::Hosted), Source::Setting),
        ];
        for (text, selected, source) in rows {
            let r = res(&text);
            assert_eq!((r.selected, r.source, r.refusal), (selected, source, None), "{text}");
            assert_eq!(r.requested, selected, "{text}");
        }
        let none = resolve(None, RunSide::Server, &env());
        assert_eq!((none.selected, none.source), (Some(Backend::Hosted), Source::Default));
        assert_eq!(read_setting(None), Setting::Default);
        assert_eq!(read_setting(doc(r#"{"shroudforge": {"useInstalled": true}}"#).as_ref()), Setting::UseInstalled);
    }

    #[test]
    fn an_invalid_backend_is_refused_and_named() {
        for (value, shown) in [("\"Kiln\"", "\"Kiln\""), ("\"bundled\"", "\"bundled\""), ("\"\"", "\"\""), ("1", "1"),
                               ("null", "null"), ("true", "true"), ("[\"kiln\"]", "[\"kiln\"]")] {
            let text = format!(r#"{{"shroudforge": {{"backend": {value}, "useInstalled": true}}}}"#);
            assert_eq!(read_setting(doc(&text).as_ref()), Setting::Invalid(shown.into()), "{text}");
            let r = res(&text);
            assert_eq!((r.requested, r.selected, r.refusal), (None, None, Some(Refusal::SettingInvalid)), "{text}");
            assert_eq!(r.reason, format!("kiln.json shroudforge.backend: {shown} is not \"hosted\", \"installed\" or \"kiln\""));
            assert_eq!(Refusal::SettingInvalid.code(), 0x100);
            // no environment brings an invalid setting back to a backend
            let e = Env { shroudforge: Some("1".into()), runtime: Some("installed".into()) };
            assert_eq!(resolve(doc(&text).as_ref(), RunSide::Client, &e).selected, None, "{text}");
        }
    }

    #[test]
    fn kiln_is_refused_while_held() {
        let _list = test_list::set(HELD_FORM);  // the interpreter as a preview
        let kiln = r#""shroudforge": {"backend": "kiln"}"#;
        // held: no preview, preview false, another capability, a side section that replaces the lift with nothing
        for (text, side) in [
            (format!("{{{kiln}}}"), RunSide::Client),
            (format!(r#"{{"preview": false, {kiln}}}"#), RunSide::Client),
            (format!(r#"{{"preview": ["kiln-sfi.sf18"], {kiln}}}"#), RunSide::Client),
            (format!(r#"{{"preview": "some", {kiln}}}"#), RunSide::Client),
            (format!(r#"{{"preview": true, "client": {{"preview": false}}, {kiln}}}"#), RunSide::Client),
            (format!(r#"{{"preview": true, "server": {{"preview": []}}, {kiln}}}"#), RunSide::Server),
        ] {
            let d = doc(&text);
            let held = interpreter_held(d.as_ref(), side);
            assert!(held.is_some(), "{text}");
            let r = resolve(d.as_ref(), side, &env());
            assert_eq!((r.requested, r.selected, r.refusal), (Some(Backend::Kiln), None, Some(Refusal::InterpreterHeld)), "{text}");
            assert_eq!(Refusal::InterpreterHeld.code(), 0x101);
            assert!(r.reason.contains("kiln-sfi.interpreter") && r.reason.contains(held.as_deref().unwrap()), "{}", r.reason);
            // KILN_SHROUDFORGE_RUNTIME never selects or overrides kiln
            let e = Env { shroudforge: None, runtime: Some("installed".into()) };
            assert_eq!(resolve(d.as_ref(), side, &e).selected, None, "{text}");
            assert!(set_backend(&text, Some(Backend::Kiln), side).is_err_and(|e| e.code == ErrorCode::Refused), "{text}");
        }
        assert!(interpreter_held(None, RunSide::Client).is_some(), "no kiln.json: held");
        // in the core's words: the client gate's row with its guided checks (native/core/gates/sfi.json)
        let held = interpreter_held(None, RunSide::Client).unwrap();
        assert!(held.starts_with(crate::sfi_release::GATE_TEXT) && held.contains("sfi-ui-page"), "{held}");
        let r = res(r#"{"shroudforge": {"backend": "kiln"}}"#);
        assert!(r.reason.ends_with(&format!("is held ({held})")), "{}", r.reason);
        // open: preview true, "all", a list naming it, a side section that lifts it for that side only
        for (text, side) in [
            (format!(r#"{{"preview": true, {kiln}}}"#), RunSide::Client),
            (format!(r#"{{"preview": "all", {kiln}}}"#), RunSide::Server),
            (format!(r#"{{"preview": ["x", "kiln-sfi.interpreter"], {kiln}}}"#), RunSide::Client),
            (format!(r#"{{"client": {{"preview": ["kiln-sfi.interpreter"]}}, {kiln}}}"#), RunSide::Client),
            (format!(r#"{{"preview": false, "server": {{"preview": true}}, {kiln}}}"#), RunSide::Server),
        ] {
            let d = doc(&text);
            assert_eq!(interpreter_held(d.as_ref(), side), None, "{text}");
            let r = resolve(d.as_ref(), side, &env());
            assert_eq!((r.requested, r.selected, r.source, r.refusal), (Some(Backend::Kiln), Some(Backend::Kiln), Source::Setting, None), "{text}");
        }
        // a client's lift is not the server's
        let d = doc(&format!(r#"{{"client": {{"preview": true}}, {kiln}}}"#));
        assert!(interpreter_held(d.as_ref(), RunSide::Server).is_some());
    }

    #[test]
    fn env_runtime_chooses_between_hosted_and_installed_only() {
        let rt = |v: &str| Env { shroudforge: None, runtime: Some(v.into()) };
        for (text, value, selected) in [
            ("{}", "installed", Backend::Installed),
            ("{}", "bundled", Backend::Hosted),
            (r#"{"shroudforge": {"useInstalled": true}}"#, "bundled", Backend::Hosted),
            (r#"{"shroudforge": {"backend": "installed"}}"#, "anything", Backend::Hosted),
            (r#"{"shroudforge": {"backend": "hosted"}}"#, "installed", Backend::Installed),
        ] {
            let r = resolve(doc(text).as_ref(), RunSide::Client, &rt(value));
            assert_eq!((r.selected, r.source, r.refusal), (Some(selected), Source::EnvRuntime, None), "{text} {value}");
            assert!(r.reason.starts_with(&format!("KILN_SHROUDFORGE_RUNTIME={value}: ")), "{}", r.reason);
        }
        // an empty value or one of 16 characters or more is as if unset (GetEnvironmentVariableW into 16 wchar_t)
        for value in ["", "installed-but-long", "0123456789abcdef"] {
            let r = resolve(doc(r#"{"shroudforge": {"useInstalled": true}}"#).as_ref(), RunSide::Client, &rt(value));
            assert_eq!((r.selected, r.source), (Some(Backend::Installed), Source::UseInstalled), "{value:?}");
        }
        let fifteen = resolve(doc("{}").as_ref(), RunSide::Client, &rt("installed______"));
        assert_eq!((fifteen.selected, fifteen.source), (Some(Backend::Hosted), Source::EnvRuntime));
        let kiln = format!(r#"{{{}, "shroudforge": {{"backend": "kiln"}}}}"#, open());
        let r = resolve(doc(&kiln).as_ref(), RunSide::Client, &rt("installed"));
        assert_eq!((r.selected, r.source), (Some(Backend::Kiln), Source::Setting));
    }

    #[test]
    fn shroudforge_zero_stands_every_backend_aside_with_mains_note() {
        let _list = test_list::set(HELD_FORM);  // the interpreter as a preview
        let off = Env { shroudforge: Some("0".into()), runtime: None };
        let kiln = format!(r#"{{{}, "shroudforge": {{"backend": "kiln"}}}}"#, open());
        for text in ["{}", r#"{"shroudforge": {"useInstalled": true}}"#, r#"{"shroudforge": {"backend": "installed"}}"#, &kiln] {
            let r = resolve(doc(text).as_ref(), RunSide::Client, &off);
            assert_eq!(r.env_off_note, Some(crate::own_loaders::RUNTIME_ENV_OFF_NOTE), "{text}");
            assert!(r.selected.is_some(), "the selection itself stays: {text}");
        }
        for value in ["1", "00", "", "false"] {
            let e = Env { shroudforge: Some(value.into()), runtime: None };
            assert_eq!(resolve(doc("{}").as_ref(), RunSide::Client, &e).env_off_note, None, "{value:?}");
        }
        // a refusal stands every backend aside already: no second note
        assert_eq!(resolve(doc(r#"{"shroudforge": {"backend": "x"}}"#).as_ref(), RunSide::Client, &off).env_off_note, None);
        let held = r#"{"shroudforge": {"backend": "kiln"}}"#;
        assert_eq!(resolve(doc(held).as_ref(), RunSide::Client, &off).env_off_note, None);
    }

    #[test]
    fn one_write_never_leaves_both_keys() {
        let side = RunSide::Client;
        let base = "{\n  // the player's note\n  \"policy\": \"any\",\n  \"shroudforge\": {\n    \"useInstalled\": true\n  }\n}\n";
        for backend in [Backend::Hosted, Backend::Installed] {
            let out = set_backend(base, Some(backend), side).unwrap();
            let v = crate::jsonc::parse(&out).unwrap();
            assert_eq!(v["shroudforge"], json!({"backend": backend.as_str()}), "{out}");
            assert!(out.contains("// the player's note") && out.contains("\"policy\": \"any\""), "{out}");
            assert_eq!(res(&out).selected, Some(backend));
        }
        // back to the default: both keys gone
        let out = set_backend(base, None, side).unwrap();
        let v = crate::jsonc::parse(&out).unwrap();
        assert!(v["shroudforge"].get("backend").is_none() && v["shroudforge"].get("useInstalled").is_none(), "{out}");
        assert_eq!(res(&out).source, Source::Default);
        // duplicate useInstalled keys: none is left
        let dup = r#"{"shroudforge": {"useInstalled": true, "useInstalled": true}}"#;
        let out = set_backend(dup, Some(Backend::Hosted), side).unwrap();
        assert!(!out.contains("useInstalled"), "{out}");
        // kiln with the lift
        let lifted = format!(r#"{{{}, "shroudforge": {{"useInstalled": true}}}}"#, open());
        let out = set_backend(&lifted, Some(Backend::Kiln), side).unwrap();
        assert_eq!(crate::jsonc::parse(&out).unwrap()["shroudforge"], json!({"backend": "kiln"}));
        // no kiln.json yet, or an empty one
        for empty in ["", "  \n"] {
            let out = set_backend(empty, Some(Backend::Installed), side).unwrap();
            assert_eq!(crate::jsonc::parse(&out).unwrap(), json!({"shroudforge": {"backend": "installed"}}));
        }
        // refused: a kiln.json that does not parse (never overwritten), a shroudforge that is not an object
        assert!(set_backend("{ nope", Some(Backend::Hosted), side).is_err_and(|e| e.code == ErrorCode::Parse));
        assert!(set_backend(r#"{"shroudforge": 1}"#, Some(Backend::Hosted), side).is_err_and(|e| e.code == ErrorCode::Schema));
    }

    #[test]
    fn game_files_read_and_write() {
        let _list = test_list::set(HELD_FORM);  // the interpreter as a preview
        let g = std::env::temp_dir().join(format!("kiln-sfi-backend-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&g);
        std::fs::create_dir_all(&g).unwrap();
        let side = RunSide::Server;
        assert_eq!(resolve_game(&g, side, &env()).source, Source::Default, "no kiln/ folder");
        write_game(&g, Some(Backend::Installed), side).unwrap();
        let file = g.join("kiln/kiln.json");
        assert_eq!(crate::jsonc::parse(&std::fs::read_to_string(&file).unwrap()).unwrap(), json!({"shroudforge": {"backend": "installed"}}));
        assert_eq!(resolve_game(&g, side, &env()).selected, Some(Backend::Installed));
        assert!(write_game(&g, Some(Backend::Kiln), side).is_err_and(|e| e.code == ErrorCode::Refused));
        assert_eq!(resolve_game(&g, side, &env()).selected, Some(Backend::Installed), "a refused write changes nothing");
        std::fs::write(&file, "{ broken").unwrap();
        assert!(write_game(&g, Some(Backend::Hosted), side).is_err());
        assert_eq!(std::fs::read_to_string(&file).unwrap(), "{ broken", "an unreadable kiln.json is never overwritten");
        let r = resolve_game(&g, side, &env());
        assert_eq!((r.selected, r.source), (Some(Backend::Hosted), Source::Default));
        assert!(r.reason.contains("does not parse"), "{}", r.reason);
        let _ = std::fs::remove_dir_all(&g);
    }

    /// A kiln.json with a UTF-8 BOM (Notepad's "UTF-8 with BOM", PowerShell 5.1's -Encoding UTF8)
    /// is read by the core (nlohmann skips the BOM); kiln-manage reads the same setting and keeps the BOM when it writes.
    #[test]
    fn a_bom_is_read_as_the_core_reads_it() {
        let g = std::env::temp_dir().join(format!("kiln-sfi-backend-bom-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&g);
        std::fs::create_dir_all(g.join("kiln")).unwrap();
        let file = g.join("kiln/kiln.json");
        let side = RunSide::Client;
        for (text, selected, source) in [
            ("\u{feff}{\"shroudforge\": {\"useInstalled\": true}}", Backend::Installed, Source::UseInstalled),
            ("\u{feff}{\"shroudforge\": {\"backend\": \"installed\"}}", Backend::Installed, Source::Setting),
            ("\u{feff}{}", Backend::Hosted, Source::Default),
        ] {
            std::fs::write(&file, text).unwrap();
            let r = resolve_game(&g, side, &env());
            assert_eq!((r.selected, r.source), (Some(selected), source), "{text:?}");
            assert!(!r.reason.contains("does not parse"), "{text:?}: {}", r.reason);
        }
        let kiln = format!("\u{feff}{{{}, \"shroudforge\": {{\"backend\": \"kiln\"}}}}", open());
        std::fs::write(&file, &kiln).unwrap();
        assert_eq!(resolve_game(&g, side, &env()).selected, Some(Backend::Kiln));
        // the writer edits a BOM file and keeps its BOM, comments and layout
        std::fs::write(&file, "\u{feff}{\n  // mine\n  \"shroudforge\": {\"useInstalled\": true}\n}\n").unwrap();
        write_game(&g, Some(Backend::Hosted), side).unwrap();
        let out = std::fs::read_to_string(&file).unwrap();
        assert!(out.starts_with("\u{feff}{\n  // mine\n") && !out.contains("useInstalled"), "{out:?}");
        assert_eq!(resolve_game(&g, side, &env()).source, Source::Setting);
        let _ = std::fs::remove_dir_all(&g);
    }

    /// The preview lift comes from config.cpp, which reads kiln.json with nlohmann (comments, no
    /// trailing comma) and reads `policy`, `clr`, `plugins` and `log_level` before the top-level "preview" (a wrong
    /// type stops it there). launch.cpp still reads the backend setting through jsonc_to_json (trailing commas allowed).
    #[test]
    fn the_preview_lift_is_read_as_config_cpp_reads_it() {
        let _list = test_list::set(HELD_FORM);  // the interpreter as a preview
        let g = std::env::temp_dir().join(format!("kiln-sfi-backend-preview-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&g);
        std::fs::create_dir_all(g.join("kiln")).unwrap();
        let file = g.join("kiln/kiln.json");
        let side = RunSide::Client;
        let open = open();
        let kiln = r#""shroudforge": {"backend": "kiln"}"#;
        // held: config.cpp never reads the lift (the core refuses kiln: 0x101)
        for text in [
            format!("{{{open}, \"shroudforge\": {{\"backend\": \"kiln\",}},}}"),
            format!("{{{open}, \"other\": [1, 2,], {kiln}}}"),
            format!("{{\"clr\": \"no\", {open}, {kiln}}}"),
            format!("{{{open}, \"policy\": 1, {kiln}}}"),
            format!("{{\"plugins\": null, {open}, {kiln}}}"),
            format!("{{\"log_level\": true, {open}, {kiln}}}"),
        ] {
            std::fs::write(&file, &text).unwrap();
            let r = resolve_game(&g, side, &env());
            assert_eq!((r.requested, r.selected, r.refusal), (Some(Backend::Kiln), None, Some(Refusal::InterpreterHeld)), "{text}");
            assert!(set_backend(&text, Some(Backend::Kiln), side).is_err_and(|e| e.code == ErrorCode::Refused), "{text}");
        }
        // lifted: well-typed keys, or the side section's lift (apply_side reads it even when the top level stopped)
        for text in [
            format!("{{\"clr\": false, \"policy\": \"any\", \"plugins\": true, \"log_level\": \"debug\", {open}, {kiln}}}"),
            format!("{{\"clr\": \"no\", \"client\": {{\"preview\": true}}, {kiln}}}"),
            format!("{{\n  // a note\n  {open}, /* x */ {kiln}\n}}"),
        ] {
            std::fs::write(&file, &text).unwrap();
            assert_eq!(resolve_game(&g, side, &env()).selected, Some(Backend::Kiln), "{text}");
            assert!(set_backend(&text, Some(Backend::Kiln), side).is_ok(), "{text}");
        }
        // a trailing comma does not hide the setting itself (launch.cpp's reading): installed stays installed
        std::fs::write(&file, r#"{"other": [1,], "shroudforge": {"useInstalled": true,},}"#).unwrap();
        assert_eq!(resolve_game(&g, side, &env()).selected, Some(Backend::Installed));
        let _ = std::fs::remove_dir_all(&g);
    }

    #[test]
    fn backend_words() {
        for b in Backend::ALL {
            assert_eq!(Backend::parse(b.as_str()), Some(b));
            assert_eq!(serde_json::to_value(b).unwrap(), json!(b.as_str()));
        }
        for w in ["Hosted", "KILN", " kiln", "bundled", ""] {
            assert_eq!(Backend::parse(w), None, "{w:?}");
        }
    }

    // ---- the launcher's ShroudForge card ----

    /// A game folder of its own under the temp folder, with kiln/ (and kiln.json when `text` is given).
    fn card_game(name: &str, text: Option<&str>) -> std::path::PathBuf {
        let g = std::env::temp_dir().join(format!("kiln-sfi-card-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&g);
        std::fs::create_dir_all(g.join("kiln")).unwrap();
        if let Some(t) = text {
            std::fs::write(g.join("kiln/kiln.json"), t).unwrap();
        }
        g
    }

    #[test]
    fn the_card_tells_default_inherited_explicit_and_invalid_apart() {
        let side = RunSide::Client;
        let rows: [(Option<&str>, SettingView, Option<Backend>); 6] = [
            (None, SettingView::Default, Some(Backend::Hosted)),
            (Some("{}"), SettingView::Default, Some(Backend::Hosted)),
            (Some(r#"{"shroudforge": {"useInstalled": true}}"#), SettingView::Inherited { backend: Backend::Installed }, Some(Backend::Installed)),
            (Some(r#"{"shroudforge": {"backend": "hosted", "useInstalled": true}}"#), SettingView::Explicit { backend: Backend::Hosted }, Some(Backend::Hosted)),
            (Some(r#"{"shroudforge": {"backend": "installed"}}"#), SettingView::Explicit { backend: Backend::Installed }, Some(Backend::Installed)),
            (Some(r#"{"shroudforge": {"backend": "sf18"}}"#), SettingView::Invalid { value: "\"sf18\"".into() }, None),
        ];
        for (i, (text, setting, selected)) in rows.into_iter().enumerate() {
            let g = card_game(&format!("kinds{i}"), text);
            let c = card(&g, side, &env()).unwrap();
            assert_eq!((&c.setting, c.next.selected), (&setting, selected), "{text:?}");
            assert_eq!(c.revision, revision_of(text.map(str::as_bytes)), "{text:?}");
            assert!(c.unreadable.is_none() && c.interpreter_route.is_none(), "{text:?}: {c:?}");
            let _ = std::fs::remove_dir_all(&g);
        }
    }

    #[test]
    fn kiln_is_held_with_its_reason_and_a_preview_of_it_writes_nothing() {
        let _list = test_list::set(HELD_FORM);  // the interpreter as a preview
        let side = RunSide::Client;
        let text = "{\n  // mine\n  \"shroudforge\": {\"useInstalled\": true}\n}\n";
        let g = card_game("held", Some(text));
        let c = card(&g, side, &env()).unwrap();
        let held = c.interpreter_held.clone().expect("kiln is held in this build");
        assert!(held.starts_with(crate::sfi_release::GATE_TEXT), "{held}");
        let p = preview(&g, Some(Backend::Kiln), side, &env()).unwrap();
        assert!(p.next.is_none() && p.refused.as_deref().is_some_and(|r| r.contains(INTERPRETER_CAPABILITY)), "{p:?}");
        // a preview of a choice that can be written writes nothing either
        let p = preview(&g, Some(Backend::Hosted), side, &env()).unwrap();
        let next = p.next.expect("hosted can be written");
        assert_eq!((next.selected, next.source, p.unchanged), (Some(Backend::Hosted), Source::Setting, false));
        assert_eq!(std::fs::read_to_string(g.join("kiln/kiln.json")).unwrap(), text);
        // the same choice again changes nothing
        let p = preview(&g, None, side, &env()).unwrap();
        assert_eq!(p.next.map(|n| n.selected), Some(Some(Backend::Hosted)));
        let _ = std::fs::remove_dir_all(&g);
        let g = card_game("same", Some(r#"{"shroudforge": {"backend": "installed"}}"#));
        assert!(preview(&g, Some(Backend::Installed), side, &env()).unwrap().unchanged);
        let _ = std::fs::remove_dir_all(&g);
    }

    /// A build whose list has no kiln-sfi.interpreter row: kiln is a normal choice. kiln.json needs no "preview", the
    /// card offers kiln with no held reason, a preview of it can be written and the next start gets it; ShroudForge 1.8
    /// is still never a choice.
    #[test]
    fn without_the_interpreter_row_kiln_is_a_normal_choice() {
        let _list = test_list::set(RELEASED_FORM);
        let kiln = r#""shroudforge": {"backend": "kiln"}"#;
        for (text, side) in [
            (format!("{{{kiln}}}"), RunSide::Client),
            (format!("{{{kiln}}}"), RunSide::Server),
            (format!(r#"{{"preview": false, {kiln}}}"#), RunSide::Client),
            (format!(r#"{{"preview": ["kiln-sfi.sf18"], {kiln}}}"#), RunSide::Server),
            (format!(r#"{{"preview": true, "client": {{"preview": false}}, {kiln}}}"#), RunSide::Client),
        ] {
            let d = doc(&text);
            assert_eq!(interpreter_held(d.as_ref(), side), None, "{text}");
            let r = resolve(d.as_ref(), side, &env());
            assert_eq!((r.requested, r.selected, r.source, r.refusal), (Some(Backend::Kiln), Some(Backend::Kiln), Source::Setting, None), "{text}");
            assert!(set_backend(&text, Some(Backend::Kiln), side).is_ok(), "{text}");
        }
        assert_eq!(interpreter_held(None, RunSide::Client), None, "no kiln.json: open");
        // the card: hosted by default, kiln offered with no held reason; choosing it writes the one key, no "preview"
        let side = RunSide::Client;
        let g = card_game("released", None);
        let c = card(&g, side, &env()).unwrap();
        assert_eq!((c.interpreter_held.as_deref(), c.next.selected, &c.setting), (None, Some(Backend::Hosted), &SettingView::Default), "{c:?}");
        let p = preview(&g, Some(Backend::Kiln), side, &env()).unwrap();
        assert_eq!((p.refused.as_deref(), p.next.as_ref().and_then(|n| n.selected), p.unchanged), (None, Some(Backend::Kiln), false), "{p:?}");
        let after = write_game_at(&g, Some(Backend::Kiln), side, &c.revision, &env()).unwrap();
        assert_eq!(after.setting, SettingView::Explicit { backend: Backend::Kiln });
        let text = std::fs::read_to_string(g.join("kiln/kiln.json")).unwrap();
        assert!(!text.contains("preview"), "{text}");
        let c = card(&g, side, &env()).unwrap();
        assert_eq!((c.interpreter_held.as_deref(), c.next.selected, c.next.refusal), (None, Some(Backend::Kiln), None), "{c:?}");
        let all = format!("{}", json!({"card": c, "preview": p}));
        assert!(!all.contains("sf18") && !all.contains("1.8"), "{all}");
        let _ = std::fs::remove_dir_all(&g);
    }

    /// A list holding the interpreter on one side only holds it on both, and "preview" does not lift that.
    #[test]
    fn a_one_sided_row_holds_kiln_on_both_sides_whatever_preview_says() {
        let _list = test_list::set(ONE_SIDED);
        let kiln = r#""shroudforge": {"backend": "kiln"}"#;
        for side in [RunSide::Client, RunSide::Server] {
            for text in [format!("{{{kiln}}}"), format!(r#"{{"preview": true, {kiln}}}"#),
                         format!(r#"{{"preview": ["kiln-sfi.interpreter"], {kiln}}}"#)] {
                let d = doc(&text);
                assert_eq!(interpreter_held(d.as_ref(), side).as_deref(), Some(crate::sfi_release::UNLISTED_TEXT), "{text} {side:?}");
                let r = resolve(d.as_ref(), side, &env());
                assert_eq!((r.selected, r.refusal), (None, Some(Refusal::InterpreterHeld)), "{text} {side:?}");
                assert!(set_backend(&text, Some(Backend::Kiln), side).is_err_and(|e| e.code == ErrorCode::Refused), "{text}");
            }
        }
        let g = card_game("one-sided", Some(r#"{"preview": true}"#));
        let c = card(&g, RunSide::Server, &env()).unwrap();
        assert_eq!(c.interpreter_held.as_deref(), Some(crate::sfi_release::UNLISTED_TEXT), "{c:?}");
        let _ = std::fs::remove_dir_all(&g);
    }

    #[test]
    fn the_preview_lift_opens_kiln_and_nothing_names_shroudforge_1_8() {
        let side = RunSide::Client;
        let g = card_game("lift", Some(&format!("{{{}}}", open())));
        let c = card(&g, side, &env()).unwrap();
        assert_eq!(c.interpreter_held, None);
        let p = preview(&g, Some(Backend::Kiln), side, &env()).unwrap();
        assert_eq!(p.next.as_ref().and_then(|n| n.selected), Some(Backend::Kiln), "{p:?}");
        // the card offers hosted, installed and kiln; ShroudForge 1.8 is never a choice, whatever "preview" says
        let all = format!("{}", json!({"card": c, "preview": p}));
        assert!(!all.contains("sf18") && !all.contains("1.8"), "{all}");
        std::fs::write(g.join("kiln/kiln.json"), r#"{"preview": true}"#).unwrap();
        let all = format!("{}", json!(card(&g, side, &env()).unwrap()));
        assert!(!all.contains("sf18") && !all.contains("1.8"), "{all}");
        let _ = std::fs::remove_dir_all(&g);
    }

    #[test]
    fn a_choice_is_written_only_over_the_bytes_it_was_made_on() {
        let _list = test_list::set(HELD_FORM);  // the interpreter as a preview
        let side = RunSide::Client;
        let g = card_game("rev", Some(r#"{"shroudforge": {"useInstalled": true}}"#));
        let c = card(&g, side, &env()).unwrap();
        // a hand edit (or another launcher window) after the card read the file
        let edited = r#"{"shroudforge": {"useInstalled": true}, "log_level": "debug"}"#;
        std::fs::write(g.join("kiln/kiln.json"), edited).unwrap();
        let e = write_game_at(&g, Some(Backend::Hosted), side, &c.revision, &env()).unwrap_err();
        assert_eq!(e.code, ErrorCode::Conflict, "{e:?}");
        assert_eq!(std::fs::read_to_string(g.join("kiln/kiln.json")).unwrap(), edited);
        // on the bytes it read again: one key written, the older one gone
        let c = card(&g, side, &env()).unwrap();
        let after = write_game_at(&g, Some(Backend::Hosted), side, &c.revision, &env()).unwrap();
        assert_eq!(after.setting, SettingView::Explicit { backend: Backend::Hosted });
        let out = std::fs::read_to_string(g.join("kiln/kiln.json")).unwrap();
        assert!(!out.contains("useInstalled") && out.contains("\"log_level\": \"debug\""), "{out}");
        // back to the default: the key goes
        let after = write_game_at(&g, None, side, &after.revision, &env()).unwrap();
        assert_eq!(after.setting, SettingView::Default);
        // a missing file has its own revision, and kiln is refused there while held (nothing written)
        std::fs::remove_file(g.join("kiln/kiln.json")).unwrap();
        let e = write_game_at(&g, Some(Backend::Kiln), side, "absent", &env()).unwrap_err();
        assert_eq!(e.code, ErrorCode::Refused, "{e:?}");
        assert!(!g.join("kiln/kiln.json").exists());
        let _ = std::fs::remove_dir_all(&g);
    }

    #[test]
    fn a_hand_edit_while_the_choice_is_being_written_is_kept_and_a_conflict() {
        let side = RunSide::Client;
        let g = card_game("cas", Some(r#"{"shroudforge": {"useInstalled": true}}"#));
        let file = g.join("kiln/kiln.json");
        let c = card(&g, side, &env()).unwrap();
        // the edit lands after the revision check passed and the new file was made, before it replaces kiln.json
        let edited = r#"{"shroudforge": {"useInstalled": true}, "policy": "verified-only"}"#;
        let e = write_game_at_with(&g, Some(Backend::Hosted), side, &c.revision, &env(),
            &|| std::fs::write(&file, edited).unwrap()).unwrap_err();
        assert_eq!(e.code, ErrorCode::Conflict, "{e:?}");
        assert_eq!(std::fs::read_to_string(&file).unwrap(), edited);
        // a file made meanwhile where there was none is a change too
        std::fs::remove_file(&file).unwrap();
        let e = write_game_at_with(&g, Some(Backend::Hosted), side, "absent", &env(),
            &|| std::fs::write(&file, "{}").unwrap()).unwrap_err();
        assert_eq!(e.code, ErrorCode::Conflict, "{e:?}");
        assert_eq!(std::fs::read_to_string(&file).unwrap(), "{}");
        // nothing staged is left behind, and with no edit meanwhile the choice is written
        let left: Vec<_> = std::fs::read_dir(g.join("kiln")).unwrap().map(|e| e.unwrap().file_name()).collect();
        assert_eq!(left, ["kiln.json"], "{left:?}");
        let c = card(&g, side, &env()).unwrap();
        let after = write_game_at_with(&g, Some(Backend::Installed), side, &c.revision, &env(), &|| {}).unwrap();
        assert_eq!(after.setting, SettingView::Explicit { backend: Backend::Installed });
        let left: Vec<_> = std::fs::read_dir(g.join("kiln")).unwrap().map(|e| e.unwrap().file_name()).collect();
        assert_eq!(left, ["kiln.json"], "{left:?}");
        let _ = std::fs::remove_dir_all(&g);
    }

    #[test]
    fn an_unreadable_kiln_json_is_shown_and_never_written() {
        let side = RunSide::Client;
        let g = card_game("bad", Some("{\"shroudforge\": "));
        let c = card(&g, side, &env()).unwrap();
        assert!(c.unreadable.as_deref().is_some_and(|u| u.contains("does not parse")), "{c:?}");
        let p = preview(&g, Some(Backend::Installed), side, &env()).unwrap();
        assert!(p.refused.is_some() && p.next.is_none(), "{p:?}");
        assert!(write_game_at(&g, Some(Backend::Installed), side, &c.revision, &env()).is_err());
        assert_eq!(std::fs::read_to_string(g.join("kiln/kiln.json")).unwrap(), "{\"shroudforge\": ");
        std::fs::write(g.join("kiln/kiln.json"), [0xffu8, 0xfe, 0x7b]).unwrap();
        assert!(card(&g, side, &env()).unwrap().unreadable.is_some());
        let _ = std::fs::remove_dir_all(&g);
    }

    #[test]
    fn resolve_bytes_reads_as_resolve_game_reads_the_file() {
        let side = RunSide::Client;
        for text in [
            "\u{feff}{\"shroudforge\": {\"useInstalled\": true}}".to_string(),
            format!("{{{}, \"shroudforge\": {{\"backend\": \"kiln\"}},}}", open()),
            format!("{{{}, \"clr\": \"no\", \"shroudforge\": {{\"backend\": \"kiln\"}}}}", open()),
            format!("{{{}, \"shroudforge\": {{\"backend\": \"kiln\"}}}}", open()),
        ] {
            let g = card_game("bytes", Some(&text));
            assert_eq!(resolve_bytes(Some(text.as_bytes()), side, &env()), resolve_game(&g, side, &env()), "{text:?}");
            let _ = std::fs::remove_dir_all(&g);
        }
        assert_eq!(resolve_bytes(None, side, &env()).source, Source::Default);
    }

    #[test]
    fn the_last_start_is_read_from_launch_json() {
        let g = card_game("last", None);
        assert_eq!(last_start(&g), None);
        std::fs::create_dir_all(g.join("kiln/state")).unwrap();
        let rec = json!({"format": 1, "pid": 4242, "backend": "none", "reasonClass": "refused",
                         "reason": "kiln.json asks for Kiln's interpreter", "launchId": "00"});
        std::fs::write(g.join("kiln/state/launch.json"), rec.to_string()).unwrap();
        let l = last_start(&g).unwrap();
        assert_eq!((l.backend, l.reason_class.as_deref(), l.pid), (None, Some("refused"), Some(4242)));
        std::fs::write(g.join("kiln/state/launch.json"), json!({"format": 1, "backend": "hosted"}).to_string()).unwrap();
        assert_eq!(last_start(&g).unwrap().backend.as_deref(), Some("hosted"));
        // another format, or more than the card reads: none
        std::fs::write(g.join("kiln/state/launch.json"), json!({"format": 2, "backend": "hosted"}).to_string()).unwrap();
        assert_eq!(last_start(&g), None);
        std::fs::write(g.join("kiln/state/launch.json"), format!("{{\"format\": 1, \"reason\": \"{}\"}}", "x".repeat(70_000))).unwrap();
        assert_eq!(last_start(&g), None);
        // a record whose first 64 KiB parse whole still is more than the card reads
        std::fs::write(g.join("kiln/state/launch.json"), format!("{}{}", json!({"format": 1, "backend": "hosted"}), " ".repeat(70_000))).unwrap();
        assert_eq!(last_start(&g), None);
        let _ = std::fs::remove_dir_all(&g);
    }
}
