//! Rich Presence (PLAN 3.6, 7.4): the Discord app on this PC shows "Playing Kiln / Enshrouded with Kiln / 12 mods on /
//! elapsed" while the game runs with Kiln and the switch is on. Over the app's local IPC socket only: nothing goes to
//! Frostdev, and nothing at all while the switch is off. Never a server, world, character or mod name: the activity is
//! built from the line texts, the mod count and the start time, and nothing else ([`activity`]).
//!
//! One opt-in more (Crucible, DESIGN.md 8; community.json `presencePack`, off by default): while the game runs a
//! profile that is an instance of a Crucible pack, the state line names the pack ("Ember run (12 mods)") and the second
//! button becomes "Get this modpack", opening the pack's page on frostdev.io ([`pack_for_game`], [`pack_page`]). Only
//! packs from Crucible (their instance carries Crucible's code or slug): a pack made or opened locally is never named.
//!
//! The IPC (Discord's `discord-rpc` protocol, docs `topics/rpc`): frames of `u32 opcode, u32 length` (little endian)
//! and a JSON payload; opcode 0 HANDSHAKE `{"v": 1, "client_id": ...}` answered by a READY dispatch, 1 FRAME (a
//! command and its answer), 2 CLOSE, 3 PING (answered with 4 PONG). `SET_ACTIVITY {pid, activity}` sets it; an
//! activity of null clears it, as does closing the socket.
//!
//! Where the socket is: Windows `\\.\pipe\discord-ipc-{0..9}`; elsewhere `discord-ipc-{0..9}` in `$XDG_RUNTIME_DIR`,
//! its Flatpak (`app/com.discordapp.Discord`, Vesktop's `.flatpak/dev.vencord.Vesktop/xdg-run`) and Snap
//! (`snap.discord`) folders, then `$TMPDIR`, `$TMP`, `$TEMP`, `/tmp` ([`socket_paths`]).

use super::{CLIENT_ID, INVITE, KILN_PAGE};
use crate::modpack::pack::CrucibleRef;
use crate::paths::DataDirs;
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::io::{Read, Write};
use std::path::PathBuf;

/// The art keys in the application's Rich Presence assets. Not uploaded yet: Discord shows no picture for a key it
/// doesn't know, and an activity it refuses is sent again without art ([`Ipc::set`]).
pub const ART_LARGE: &str = "kiln-hearth";
pub const ART_SMALL: &str = "kiln-mark";
/// A frame may be this long (Discord's are a few KiB).
pub const FRAME_MAX: u32 = 64 * 1024;

pub const OP_HANDSHAKE: u32 = 0;
pub const OP_FRAME: u32 = 1;
pub const OP_CLOSE: u32 = 2;
pub const OP_PING: u32 = 3;
pub const OP_PONG: u32 = 4;

/// What the Community page shows about presence.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PresenceStatus {
    /// The switch.
    pub on: bool,
    /// The Discord app's socket is there (only looked for while the switch is on).
    pub discord_running: bool,
    /// Shown on Discord now (the game runs with Kiln).
    pub showing: bool,
}

/// The lines in the launcher's language (en.json `community.presence.*`); the window sends them with the facts. A field
/// an older window does not send keeps its English.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct PresenceTexts {
    /// "Enshrouded with Kiln"
    pub details: String,
    /// "{count} mod on"
    pub state_one: String,
    /// "{count} mods on"
    pub state_other: String,
    /// "Get Kiln"
    pub get_kiln: String,
    /// "Join the Discord"
    pub join_discord: String,
    /// "{pack} ({count} mod)" (`community.presence.pack.state.one`)
    pub pack_state_one: String,
    /// "{pack} ({count} mods)"
    pub pack_state_other: String,
    /// "Get this modpack"
    pub get_pack: String,
}

impl Default for PresenceTexts {
    /// en.json's English (checked against en.json by a test), used until the window has sent its own.
    fn default() -> Self {
        PresenceTexts {
            details: "Enshrouded with Kiln".into(),
            state_one: "{count} mod on".into(),
            state_other: "{count} mods on".into(),
            get_kiln: "Get Kiln".into(),
            join_discord: "Join the Discord".into(),
            pack_state_one: "{pack} ({count} mod)".into(),
            pack_state_other: "{pack} ({count} mods)".into(),
            get_pack: "Get this modpack".into(),
        }
    }
}

/// What the activity says: how many mods are on (every kind; unknown: no state line), when the game started, and with
/// "Show my Crucible pack" on, the Crucible pack being played.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Facts {
    pub mods: Option<u32>,
    /// Unix seconds.
    pub started: i64,
    pub pack: Option<PackShown>,
}

/// A Crucible pack as the activity shows it: its name and its page.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PackShown {
    pub name: String,
    /// <https://frostdev.io/kiln/crucible/p/CODE> or <https://frostdev.io/kiln/crucible/slug> ([`pack_page`]).
    pub url: String,
}

/// Crucible's pages (DESIGN.md 12: frostdev.io/kiln/crucible). A presence button opens only an address under it.
pub const CRUCIBLE_PAGE: &str = "https://frostdev.io/kiln/crucible";
/// A share code's alphabet (Crucible's src/ids.mjs: no 0/O, 1/I/L).
const CODE_ALPHABET: &str = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";

/// The pack's page from the Crucible block of its instance: by the share code (which every pack has, and which
/// Crucible sends on to the slug page of a public pack), else the slug; None when neither is a proper one.
pub fn pack_page(c: &CrucibleRef) -> Option<String> {
    if let Some(code) = c.code.as_deref() {
        let k: String = code.chars().filter(|ch| *ch != '-').map(|ch| ch.to_ascii_uppercase()).collect();
        if k.len() == 8 && k.chars().all(|ch| CODE_ALPHABET.contains(ch)) {
            return Some(format!("{CRUCIBLE_PAGE}/p/{}-{}", &k[..4], &k[4..]));
        }
    }
    let slug = c.slug.as_deref()?;
    let ok = (3..=48).contains(&slug.len())
        && slug.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
        && !slug.starts_with('-')
        && !slug.ends_with('-');
    ok.then(|| format!("{CRUCIBLE_PAGE}/{slug}"))
}

/// The Crucible pack a profile is an instance of (None: no pack, or a pack not from Crucible).
pub fn pack_for_profile(dirs: &DataDirs, pid: &str) -> Option<PackShown> {
    let inst = crate::modpack::pack::instance(dirs, pid).ok()??;
    let url = pack_page(inst.crucible.as_ref()?)?;
    let name = inst.name.split_whitespace().collect::<Vec<_>>().join(" ");
    (!name.is_empty()).then_some(PackShown { name, url })
}

/// The Crucible pack of the profile active on a game install (the watcher's game id).
pub fn pack_for_game(dirs: &DataDirs, game_id: &str) -> Option<PackShown> {
    let profiles = crate::profile::list(dirs).ok()?;
    let p = profiles.iter().find(|p| p.active.iter().any(|g| g == game_id))?;
    pack_for_profile(dirs, &p.id)
}

/// A text as Discord takes it: one line, 2 to `max` characters (cut with an ellipsis); None when too short.
fn line(s: &str, max: usize) -> Option<String> {
    let s: String = s.split_whitespace().collect::<Vec<_>>().join(" ");
    let n = s.chars().count();
    if n < 2 {
        return None;
    }
    Some(if n > max { s.chars().take(max - 1).collect::<String>() + "…" } else { s })
}

/// The activity: details, state (the mod count, or the pack and its count), the start time, two buttons (the second the
/// pack's page when a pack is shown), and the art when `art`.
pub fn activity(texts: &PresenceTexts, facts: &Facts, art: bool) -> Value {
    let details = line(&texts.details, 128).unwrap_or_else(|| "Enshrouded with Kiln".into());
    let mut a = json!({
        "type": 0,
        "details": details,
        "timestamps": { "start": facts.started },
        "instance": false,
    });
    let pack = facts.pack.as_ref().filter(|p| p.url.starts_with(&format!("{CRUCIBLE_PAGE}/")));
    let state = match (pack, facts.mods) {
        (Some(p), Some(n)) => {
            let tpl = if n == 1 { &texts.pack_state_one } else { &texts.pack_state_other };
            line(&tpl.replace("{pack}", &p.name).replace("{count}", &n.to_string()), 128)
        }
        (Some(p), None) => line(&p.name, 128),
        (None, Some(n)) => {
            let tpl = if n == 1 { &texts.state_one } else { &texts.state_other };
            line(&tpl.replace("{count}", &n.to_string()), 128)
        }
        (None, None) => None,
    };
    if let Some(state) = state {
        a["state"] = json!(state);
    }
    let second = match pack {
        Some(p) => (&texts.get_pack, p.url.as_str()),
        None => (&texts.join_discord, INVITE),
    };
    let buttons: Vec<Value> = [(&texts.get_kiln, KILN_PAGE), second]
        .iter()
        .filter_map(|(label, url)| line(label, 32).map(|l| json!({ "label": l, "url": url })))
        .collect();
    if !buttons.is_empty() {
        a["buttons"] = json!(buttons);
    }
    if art {
        a["assets"] = json!({ "large_image": ART_LARGE, "large_text": details, "small_image": ART_SMALL, "small_text": "Kiln" });
        // the member list shows the details line ("Enshrouded with Kiln") rather than the app's name
        a["status_display_type"] = json!(2);
    }
    a
}

// ---------------------------------------------------------------- frames

pub fn encode(op: u32, payload: &Value) -> Vec<u8> {
    let body = payload.to_string().into_bytes();
    let mut out = Vec::with_capacity(8 + body.len());
    out.extend_from_slice(&op.to_le_bytes());
    out.extend_from_slice(&(body.len() as u32).to_le_bytes());
    out.extend_from_slice(&body);
    out
}

pub fn read_frame(r: &mut dyn Read) -> Result<(u32, Value)> {
    let mut head = [0u8; 8];
    r.read_exact(&mut head).map_err(|_| gone())?;
    let op = u32::from_le_bytes([head[0], head[1], head[2], head[3]]);
    let len = u32::from_le_bytes([head[4], head[5], head[6], head[7]]);
    if len > FRAME_MAX {
        return Err(Error::new(ErrorCode::Parse, "The Discord app sent a frame that is too long."));
    }
    let mut body = vec![0u8; len as usize];
    r.read_exact(&mut body).map_err(|_| gone())?;
    let v = if body.is_empty() { Value::Null } else { serde_json::from_slice(&body).map_err(|_| Error::new(ErrorCode::Parse, "The Discord app sent a frame the launcher cannot read."))? };
    Ok((op, v))
}

fn gone() -> Error {
    Error::new(ErrorCode::Network, "The Discord app closed the connection.")
}

// ---------------------------------------------------------------- the socket

/// Where the Discord app's socket may be on Linux (and other Unix), in the order they are tried.
pub fn socket_paths(env: &dyn Fn(&str) -> Option<String>) -> Vec<PathBuf> {
    let mut bases: Vec<PathBuf> = Vec::new();
    for k in ["XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"] {
        if let Some(v) = env(k).filter(|v| !v.is_empty()) {
            bases.push(PathBuf::from(v));
        }
    }
    bases.push(PathBuf::from("/tmp"));
    let mut seen = std::collections::HashSet::new();
    bases.retain(|b| seen.insert(b.clone()));
    let mut out = Vec::new();
    for (i, b) in bases.iter().enumerate() {
        // the sandboxed apps' folders live in the runtime dir
        let mut dirs = vec![b.clone()];
        if i == 0 && env("XDG_RUNTIME_DIR").is_some_and(|v| !v.is_empty()) {
            dirs.push(b.join("app/com.discordapp.Discord"));
            dirs.push(b.join(".flatpak/dev.vencord.Vesktop/xdg-run"));
            dirs.push(b.join("snap.discord"));
        }
        for d in dirs {
            for n in 0..10 {
                out.push(d.join(format!("discord-ipc-{n}")));
            }
        }
    }
    out
}

/// The Windows pipe names.
pub fn pipe_names() -> Vec<String> {
    (0..10).map(|n| format!(r"\\.\pipe\discord-ipc-{n}")).collect()
}

/// A connection to the Discord app.
pub trait Stream: Read + Write + Send {}
impl<T: Read + Write + Send> Stream for T {}

/// Whether the Discord app's socket is there (nothing is connected or sent).
pub fn discord_running() -> bool {
    #[cfg(unix)]
    {
        use std::os::unix::fs::FileTypeExt;
        let env = |k: &str| std::env::var(k).ok();
        socket_paths(&env).iter().any(|p| std::fs::metadata(p).is_ok_and(|m| m.file_type().is_socket()))
    }
    #[cfg(windows)]
    {
        pipe_names().iter().any(|p| std::path::Path::new(p).exists())
    }
    #[cfg(not(any(unix, windows)))]
    {
        false
    }
}

/// Connects to the first socket that answers.
pub fn connect() -> Option<Box<dyn Stream>> {
    #[cfg(unix)]
    {
        let env = |k: &str| std::env::var(k).ok();
        for p in socket_paths(&env) {
            if let Ok(s) = std::os::unix::net::UnixStream::connect(&p) {
                let _ = s.set_read_timeout(Some(std::time::Duration::from_secs(5)));
                let _ = s.set_write_timeout(Some(std::time::Duration::from_secs(5)));
                return Some(Box::new(s));
            }
        }
        None
    }
    #[cfg(windows)]
    {
        for p in pipe_names() {
            if let Ok(f) = std::fs::OpenOptions::new().read(true).write(true).open(&p) {
                return Some(Box::new(f));
            }
        }
        None
    }
    #[cfg(not(any(unix, windows)))]
    {
        None
    }
}

/// A handshaken connection.
pub struct Ipc {
    s: Box<dyn Stream>,
    pid: u32,
    nonce: u64,
    /// Discord refused the activity with art once: later ones go without.
    no_art: bool,
}

impl Ipc {
    /// Says hello with the public client id and waits for READY.
    pub fn handshake(mut s: Box<dyn Stream>, pid: u32) -> Result<Ipc> {
        s.write_all(&encode(OP_HANDSHAKE, &json!({ "v": 1, "client_id": CLIENT_ID }))).map_err(|_| gone())?;
        loop {
            let (op, v) = read_frame(&mut s)?;
            match op {
                OP_FRAME if v["evt"] == "READY" => return Ok(Ipc { s, pid, nonce: 0, no_art: false }),
                OP_PING => s.write_all(&encode(OP_PONG, &v)).map_err(|_| gone())?,
                OP_CLOSE => return Err(Error::new(ErrorCode::Refused, "The Discord app refused Kiln's Rich Presence.")),
                _ => {}
            }
        }
    }

    fn command(&mut self, args: Value) -> Result<Value> {
        self.nonce += 1;
        let nonce = format!("kiln-{}", self.nonce);
        self.s.write_all(&encode(OP_FRAME, &json!({ "cmd": "SET_ACTIVITY", "args": args, "nonce": nonce }))).map_err(|_| gone())?;
        loop {
            let (op, v) = read_frame(&mut self.s)?;
            match op {
                OP_FRAME if v["nonce"] == nonce.as_str() => return Ok(v),
                OP_PING => self.s.write_all(&encode(OP_PONG, &v)).map_err(|_| gone())?,
                OP_CLOSE => return Err(gone()),
                _ => {}
            }
        }
    }

    /// Sets the activity (None: clears it). An activity with art that Discord refuses is sent once more without the
    /// art and the display hint, and later ones go without them.
    pub fn set(&mut self, texts: &PresenceTexts, facts: Option<&Facts>) -> Result<()> {
        let Some(f) = facts else {
            self.command(json!({ "pid": self.pid, "activity": Value::Null }))?;
            return Ok(());
        };
        let answer = self.command(json!({ "pid": self.pid, "activity": activity(texts, f, !self.no_art) }))?;
        if answer["evt"] != "ERROR" {
            return Ok(());
        }
        if !self.no_art {
            self.no_art = true;
            let again = self.command(json!({ "pid": self.pid, "activity": activity(texts, f, false) }))?;
            if again["evt"] != "ERROR" {
                return Ok(());
            }
        }
        Err(Error::new(ErrorCode::Refused, "The Discord app did not take Kiln's Rich Presence."))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn facts(mods: Option<u32>) -> Facts {
        Facts { mods, started: 1_763_480_000, pack: None }
    }

    fn ember() -> PackShown {
        PackShown { name: "Ember run".into(), url: "https://frostdev.io/kiln/crucible/p/H7QK-3MZP".into() }
    }

    #[test]
    fn with_a_crucible_pack_the_state_names_it_and_the_second_button_opens_its_page() {
        let f = Facts { pack: Some(ember()), ..facts(Some(12)) };
        let a = activity(&PresenceTexts::default(), &f, true);
        assert_eq!(a["state"], "Ember run (12 mods)");
        assert_eq!(a["details"], "Enshrouded with Kiln");
        assert_eq!(a["buttons"], json!([{ "label": "Get Kiln", "url": "https://frostdev.io/kiln" }, { "label": "Get this modpack", "url": "https://frostdev.io/kiln/crucible/p/H7QK-3MZP" }]));
        // still nothing else: no party, no secrets, no join
        let mut keys: Vec<&str> = a.as_object().unwrap().keys().map(String::as_str).collect();
        keys.sort();
        assert_eq!(keys, ["assets", "buttons", "details", "instance", "state", "status_display_type", "timestamps", "type"]);
        assert_eq!(activity(&PresenceTexts::default(), &Facts { pack: Some(ember()), ..facts(Some(1)) }, false)["state"], "Ember run (1 mod)");
        assert_eq!(activity(&PresenceTexts::default(), &Facts { pack: Some(ember()), ..facts(None) }, false)["state"], "Ember run");
        // a long pack name is cut to Discord's limit
        let long = PackShown { name: "n".repeat(300), ..ember() };
        assert_eq!(activity(&PresenceTexts::default(), &Facts { pack: Some(long), ..facts(Some(3)) }, false)["state"].as_str().unwrap().chars().count(), 128);
        // an address off Crucible's pages is never a button: the plain activity instead
        let off = PackShown { url: "https://evil.example/p".into(), ..ember() };
        let a = activity(&PresenceTexts::default(), &Facts { pack: Some(off), ..facts(Some(12)) }, false);
        assert_eq!(a["state"], "12 mods on");
        assert_eq!(a["buttons"][1]["url"], "https://discord.gg/7pjr8SMTfU");
    }

    #[test]
    fn the_pack_page_comes_from_the_share_code_else_the_slug() {
        let r = |code: Option<&str>, slug: Option<&str>| pack_page(&CrucibleRef { pack: None, version: None, slug: slug.map(Into::into), code: code.map(Into::into) });
        assert_eq!(r(Some("H7QK-3MZP"), Some("ember-run")).as_deref(), Some("https://frostdev.io/kiln/crucible/p/H7QK-3MZP"));
        assert_eq!(r(Some("h7qk3mzp"), None).as_deref(), Some("https://frostdev.io/kiln/crucible/p/H7QK-3MZP"));
        assert_eq!(r(Some("H7QK-3MZO"), Some("ember-run")).as_deref(), Some("https://frostdev.io/kiln/crucible/ember-run"), "O is not in the alphabet");
        assert_eq!(r(None, Some("ember-run")).as_deref(), Some("https://frostdev.io/kiln/crucible/ember-run"));
        for bad in ["../admin", "Ember", "-x-", "ab", "a/b", "ember run"] {
            assert_eq!(r(None, Some(bad)), None, "{bad}");
        }
        assert_eq!(r(Some("H7QK"), None), None);
        assert_eq!(r(None, None), None);
    }

    #[test]
    fn only_an_instance_of_a_crucible_pack_is_named() {
        let tmp = crate::library::TempDir::new(&std::env::temp_dir(), "kiln-presence-pack").unwrap();
        let dirs = DataDirs::at(tmp.path());
        let plain = crate::profile::create(&dirs, "Plain", None).unwrap();
        let local = crate::profile::create(&dirs, "Local pack", None).unwrap();
        let from_crucible = crate::profile::create(&dirs, "Ember run", None).unwrap();
        let write = |pid: &str, crucible: Value| {
            let d = dirs.profile(pid).join(crate::modpack::pack::INSTANCE_DIR);
            std::fs::create_dir_all(&d).unwrap();
            let mut inst = json!({ "format": 1, "pack": "ember-run", "name": "  Ember   run ", "version": "1.0.0", "sha256": "0".repeat(64), "installed": "2026-11-20T10:00:00Z" });
            if !crucible.is_null() {
                inst["crucible"] = crucible;
            }
            std::fs::write(d.join(crate::modpack::pack::INSTANCE_FILE), inst.to_string()).unwrap();
        };
        write(&local.id, Value::Null);
        write(&from_crucible.id, json!({ "pack": "pck_x", "version": "pv_y", "slug": "ember-run", "code": "H7QK-3MZP" }));
        assert_eq!(pack_for_profile(&dirs, &plain.id), None, "not a pack's instance");
        assert_eq!(pack_for_profile(&dirs, &local.id), None, "a pack not from Crucible is never named");
        assert_eq!(pack_for_profile(&dirs, &from_crucible.id), Some(PackShown { name: "Ember run".into(), url: "https://frostdev.io/kiln/crucible/p/H7QK-3MZP".into() }));
        assert_eq!(pack_for_game(&dirs, "no-such-game"), None);
    }

    #[test]
    fn the_lines_are_kiln_enshrouded_and_the_mod_count_only() {
        let a = activity(&PresenceTexts::default(), &facts(Some(12)), true);
        assert_eq!(a["details"], "Enshrouded with Kiln");
        assert_eq!(a["state"], "12 mods on");
        assert_eq!(a["timestamps"]["start"], 1_763_480_000);
        assert_eq!(a["type"], 0);
        assert_eq!(a["status_display_type"], 2);
        assert_eq!(a["buttons"], json!([{ "label": "Get Kiln", "url": "https://frostdev.io/kiln" }, { "label": "Join the Discord", "url": "https://discord.gg/7pjr8SMTfU" }]));
        assert_eq!(a["assets"]["large_image"], "kiln-hearth");
        // nothing else is in it: no party, no secrets, no server or world
        let keys: Vec<&str> = a.as_object().unwrap().keys().map(String::as_str).collect();
        let mut keys = keys;
        keys.sort();
        assert_eq!(keys, ["assets", "buttons", "details", "instance", "state", "status_display_type", "timestamps", "type"]);
        assert_eq!(activity(&PresenceTexts::default(), &facts(Some(1)), false)["state"], "1 mod on");
        assert_eq!(activity(&PresenceTexts::default(), &facts(Some(0)), false)["state"], "0 mods on");
        // an unknown count: no state line rather than a wrong one
        assert!(activity(&PresenceTexts::default(), &facts(None), false).get("state").is_none());
        // without art: no assets and no display hint
        let plain = activity(&PresenceTexts::default(), &facts(Some(3)), false);
        assert!(plain.get("assets").is_none() && plain.get("status_display_type").is_none());
    }

    #[test]
    fn texts_from_the_window_are_kept_to_discords_limits() {
        let t = PresenceTexts { details: format!("  {}  ", "d".repeat(200)), state_one: "x".into(), state_other: "{count}".into(), get_kiln: "g".repeat(40), join_discord: "".into(), ..Default::default() };
        let a = activity(&t, &facts(Some(1)), false);
        assert_eq!(a["details"].as_str().unwrap().chars().count(), 128);
        assert!(a.get("state").is_none(), "a one-character state is not sent");
        let buttons = a["buttons"].as_array().unwrap();
        assert_eq!(buttons.len(), 1);
        assert_eq!(buttons[0]["label"].as_str().unwrap().chars().count(), 32);
        assert_eq!(activity(&t, &facts(Some(42)), false)["state"], "42");
    }

    #[test]
    fn the_default_texts_are_en_jsons() {
        let en: Value = serde_json::from_str(include_str!("../../../../launcher/src/i18n/en.json")).unwrap();
        let p = &en["community"]["presence"];
        let d = PresenceTexts::default();
        assert_eq!(p["details"], d.details.as_str());
        assert_eq!(p["state"]["one"], d.state_one.as_str());
        assert_eq!(p["state"]["other"], d.state_other.as_str());
        assert_eq!(p["getKiln"], d.get_kiln.as_str());
        assert_eq!(p["joinDiscord"], d.join_discord.as_str());
        assert_eq!(p["pack"]["state"]["one"], d.pack_state_one.as_str());
        assert_eq!(p["pack"]["state"]["other"], d.pack_state_other.as_str());
        assert_eq!(p["pack"]["getPack"], d.get_pack.as_str());
        // an older window that sends only the first five keeps the English of the rest
        let older: PresenceTexts = serde_json::from_value(json!({ "details": "D", "stateOne": "a", "stateOther": "b", "getKiln": "g", "joinDiscord": "j" })).unwrap();
        assert_eq!(older.get_pack, "Get this modpack");
    }

    #[cfg(unix)]
    #[test]
    fn the_socket_is_looked_for_in_the_runtime_dir_flatpak_snap_then_the_temp_dirs() {
        let env = |k: &str| match k {
            "XDG_RUNTIME_DIR" => Some("/run/user/1000".to_string()),
            "TMPDIR" => Some("/var/tmp".to_string()),
            _ => None,
        };
        let p = socket_paths(&env);
        let s: Vec<String> = p.iter().map(|x| x.display().to_string()).collect();
        assert_eq!(s[0], "/run/user/1000/discord-ipc-0");
        assert!(s.contains(&"/run/user/1000/app/com.discordapp.Discord/discord-ipc-0".to_string()));
        assert!(s.contains(&"/run/user/1000/snap.discord/discord-ipc-9".to_string()));
        assert!(s.contains(&"/run/user/1000/.flatpak/dev.vencord.Vesktop/xdg-run/discord-ipc-3".to_string()));
        let tmpdir = s.iter().position(|x| x == "/var/tmp/discord-ipc-0").unwrap();
        let flatpak = s.iter().position(|x| x.contains("com.discordapp")).unwrap();
        assert!(flatpak < tmpdir && s.last().unwrap() == "/tmp/discord-ipc-9");
        // no runtime dir: the temp dirs only, without sandbox folders
        let none = |_: &str| None;
        let q = socket_paths(&none);
        assert_eq!(q.len(), 10);
        assert_eq!(q[0], PathBuf::from("/tmp/discord-ipc-0"));
    }

    #[cfg(windows)]
    #[test]
    fn discord_pipes_are_tried_in_index_order() {
        assert_eq!(pipe_names(), [
            r"\\.\pipe\discord-ipc-0", r"\\.\pipe\discord-ipc-1", r"\\.\pipe\discord-ipc-2",
            r"\\.\pipe\discord-ipc-3", r"\\.\pipe\discord-ipc-4", r"\\.\pipe\discord-ipc-5",
            r"\\.\pipe\discord-ipc-6", r"\\.\pipe\discord-ipc-7", r"\\.\pipe\discord-ipc-8",
            r"\\.\pipe\discord-ipc-9",
        ]);
    }

    #[test]
    fn frames_round_trip_and_a_huge_one_is_refused() {
        let bytes = encode(OP_FRAME, &json!({ "cmd": "SET_ACTIVITY" }));
        assert_eq!(&bytes[..4], &1u32.to_le_bytes());
        let (op, v) = read_frame(&mut &bytes[..]).unwrap();
        assert_eq!((op, v["cmd"].as_str()), (1, Some("SET_ACTIVITY")));
        let mut huge = 1u32.to_le_bytes().to_vec();
        huge.extend_from_slice(&(FRAME_MAX + 1).to_le_bytes());
        assert!(read_frame(&mut &huge[..]).is_err());
    }

    /// A fake Discord app on the other end of a connected socket pair (not a socket file: a long TMPDIR would pass the
    /// path's length limit, SUN_LEN): answers the handshake, then each command, refusing an activity with art when
    /// `refuse_art`. Returns the launcher's end and what the fake received.
    #[cfg(unix)]
    fn fake_discord(refuse_art: bool) -> (std::os::unix::net::UnixStream, std::thread::JoinHandle<Vec<Value>>) {
        let (mine, mut s) = std::os::unix::net::UnixStream::pair().unwrap();
        mine.set_read_timeout(Some(std::time::Duration::from_secs(5))).unwrap();
        let t = std::thread::spawn(move || {
            let mut got = Vec::new();
            while let Ok((op, v)) = read_frame(&mut s) {
                got.push(v.clone());
                let reply = if op == OP_HANDSHAKE {
                    json!({ "cmd": "DISPATCH", "evt": "READY", "data": { "v": 1 } })
                } else if refuse_art && v["args"]["activity"].get("assets").is_some() {
                    json!({ "cmd": "SET_ACTIVITY", "evt": "ERROR", "nonce": v["nonce"], "data": { "code": 4000, "message": "invalid asset" } })
                } else {
                    json!({ "cmd": "SET_ACTIVITY", "evt": null, "nonce": v["nonce"], "data": v["args"]["activity"] })
                };
                if s.write_all(&encode(OP_FRAME, &reply)).is_err() {
                    break;
                }
            }
            got
        });
        (mine, t)
    }

    #[cfg(unix)]
    #[test]
    fn the_handshake_names_the_public_client_id_and_set_activity_carries_the_launchers_pid() {
        let (s, fake) = fake_discord(false);
        let mut ipc = Ipc::handshake(Box::new(s), 4242).unwrap();
        ipc.set(&PresenceTexts::default(), Some(&facts(Some(12)))).unwrap();
        ipc.set(&PresenceTexts::default(), None).unwrap();
        drop(ipc);
        let got = fake.join().unwrap();
        assert_eq!(got[0], json!({ "v": 1, "client_id": "1570849158431244459" }));
        assert_eq!(got[1]["cmd"], "SET_ACTIVITY");
        assert_eq!(got[1]["args"]["pid"], 4242);
        assert_eq!(got[1]["args"]["activity"]["state"], "12 mods on");
        // clearing is an activity of null
        assert!(got[2]["args"]["activity"].is_null());
    }

    #[cfg(unix)]
    #[test]
    fn missing_art_is_harmless_the_activity_goes_again_without_it() {
        let (s, fake) = fake_discord(true);
        let mut ipc = Ipc::handshake(Box::new(s), 1).unwrap();
        ipc.set(&PresenceTexts::default(), Some(&facts(Some(2)))).unwrap();
        // later updates go without art at once
        ipc.set(&PresenceTexts::default(), Some(&facts(Some(3)))).unwrap();
        drop(ipc);
        let got = fake.join().unwrap();
        let acts: Vec<&Value> = got.iter().skip(1).map(|v| &v["args"]["activity"]).collect();
        assert_eq!(acts.len(), 3);
        assert!(acts[0].get("assets").is_some() && acts[1].get("assets").is_none() && acts[2].get("assets").is_none());
        assert_eq!(acts[2]["state"], "3 mods on");
    }

    #[cfg(unix)]
    #[test]
    fn discord_closing_the_handshake_is_an_error_not_a_hang() {
        // a connected pair, not a socket file: a long TMPDIR would pass the socket path's length limit (SUN_LEN)
        let (s, mut discord) = std::os::unix::net::UnixStream::pair().unwrap();
        let fake = std::thread::spawn(move || {
            let _ = read_frame(&mut discord);
            let _ = discord.write_all(&encode(OP_CLOSE, &json!({ "code": 4000, "message": "Invalid Client ID" })));
        });
        s.set_read_timeout(Some(std::time::Duration::from_secs(5))).unwrap();
        assert!(Ipc::handshake(Box::new(s), 1).is_err());
        fake.join().unwrap();
    }
}
