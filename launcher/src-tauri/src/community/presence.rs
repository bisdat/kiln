//! Rich Presence's thread: it follows the game's state the launcher already has (the watcher's game process and its
//! boot.json, crate::watch) and the mod count the window sends, and keeps the Discord app's activity in step: shown
//! while the switch is on, the Community page is shown and a client game runs with Kiln; cleared the moment any of
//! that ends. It runs apart from everything else, so a slow or missing Discord never holds up a game start, and it
//! tries the socket again every 30 seconds while the game runs (kiln_manage::community::presence).

use kiln_manage::community::presence::{self, Facts, Ipc, PresenceStatus, PresenceTexts};
use kiln_manage::community::settings::CommunitySettings;
use std::collections::HashMap;
use std::sync::{Condvar, LazyLock, Mutex, Once};
use std::time::{Duration, SystemTime};

/// A client game as the watcher saw it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Running {
    pid: u32,
    /// When the watcher first saw this process (the activity's start).
    since: SystemTime,
    /// boot.json was written by this run: Kiln is in the game.
    kiln: bool,
}

#[derive(Default)]
struct State {
    on: bool,
    shown: bool,
    games: HashMap<String, Running>,
    mods: Option<u32>,
    texts: Option<PresenceTexts>,
    /// The Discord app answered the last time the thread looked.
    connected: bool,
    /// The activity is shown now.
    showing: bool,
    changed: bool,
}

static STATE: LazyLock<(Mutex<State>, Condvar)> = LazyLock::new(|| (Mutex::new(State::default()), Condvar::new()));
static START: Once = Once::new();

fn poke(f: impl FnOnce(&mut State)) {
    START.call_once(|| {
        if let Ok(d) = crate::state::dirs() {
            let s = CommunitySettings::load(&d);
            let mut st = STATE.0.lock().unwrap();
            st.on = s.presence;
            st.shown = s.shown;
        }
        let _ = std::thread::Builder::new().name("kiln-presence".into()).spawn(run);
    });
    let mut st = STATE.0.lock().unwrap();
    f(&mut st);
    st.changed = true;
    STATE.1.notify_all();
}

/// The watcher's round, for each client game: its process (None: not running) and boot.json's time.
pub fn seen(game_id: &str, pid: Option<u32>, boot: Option<SystemTime>) {
    let before = STATE.0.lock().unwrap().games.get(game_id).copied();
    let now = pid.map(|pid| {
        let since = before.filter(|r| r.pid == pid).map(|r| r.since).unwrap_or_else(SystemTime::now);
        // boot.json written by this run (Wine reuses pids: the time decides), with a little slack for the clocks
        let kiln = boot.is_some_and(|b| b + Duration::from_secs(5) >= since);
        Running { pid, since, kiln }
    });
    if now == before && START.is_completed() {
        return;
    }
    poke(|s| match now {
        Some(r) => {
            s.games.insert(game_id.to_string(), r);
        }
        None => {
            s.games.remove(game_id);
        }
    });
}

pub fn settings_changed(s: &CommunitySettings) {
    let (on, shown) = (s.presence, s.shown);
    poke(|st| {
        st.on = on;
        st.shown = shown;
    });
}

pub fn facts(mods: Option<u32>, texts: Option<PresenceTexts>) {
    poke(|st| {
        st.mods = mods;
        if texts.is_some() {
            st.texts = texts;
        }
    });
}

pub fn status(s: &CommunitySettings) -> PresenceStatus {
    let on = s.presence && s.shown;
    let st = STATE.0.lock().unwrap();
    // the socket is looked for only while the switch is on (a file check, nothing is sent)
    PresenceStatus { on: s.presence, discord_running: on && (st.connected || presence::discord_running()), showing: on && st.showing }
}

/// What should show now: the game's start and the count, or nothing.
fn wanted(st: &State) -> Option<Facts> {
    if !(st.on && st.shown) {
        return None;
    }
    let game = st.games.values().filter(|g| g.kiln).min_by_key(|g| g.since)?;
    let started = game.since.duration_since(SystemTime::UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0);
    Some(Facts { mods: st.mods, started })
}

fn run() {
    let mut ipc: Option<Ipc> = None;
    let mut sent: Option<Facts> = None;
    loop {
        let (want, texts) = {
            let mut st = STATE.0.lock().unwrap();
            if !st.changed {
                st = STATE.1.wait_timeout(st, Duration::from_secs(30)).unwrap().0;
            }
            st.changed = false;
            (wanted(&st), st.texts.clone().unwrap_or_default())
        };
        match want {
            Some(f) => {
                if ipc.is_none() {
                    ipc = presence::connect().and_then(|s| Ipc::handshake(s, std::process::id()).ok());
                    sent = None;
                }
                let ok = match ipc.as_mut() {
                    Some(c) if sent != Some(f) => c.set(&texts, Some(&f)).is_ok(),
                    Some(_) => true,
                    None => false,
                };
                if ok {
                    sent = Some(f);
                } else {
                    ipc = None;
                    sent = None;
                }
                let mut st = STATE.0.lock().unwrap();
                st.connected = ipc.is_some();
                st.showing = sent.is_some();
            }
            None => {
                // cleared at once, then the connection closes (Discord also clears it when the socket closes)
                if let Some(mut c) = ipc.take() {
                    let _ = c.set(&texts, None);
                }
                sent = None;
                let mut st = STATE.0.lock().unwrap();
                st.showing = false;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn st(on: bool, games: &[(&str, Running)]) -> State {
        State { on, shown: true, games: games.iter().map(|(k, v)| (k.to_string(), *v)).collect(), mods: Some(12), ..Default::default() }
    }

    #[test]
    fn presence_shows_only_with_the_switch_on_the_page_shown_and_the_game_running_with_kiln() {
        let t0 = SystemTime::UNIX_EPOCH + Duration::from_secs(1_763_480_000);
        let with = Running { pid: 7, since: t0, kiln: true };
        let without = Running { pid: 8, since: t0, kiln: false };
        assert_eq!(wanted(&st(true, &[("g", with)])), Some(Facts { mods: Some(12), started: 1_763_480_000 }));
        assert_eq!(wanted(&st(false, &[("g", with)])), None, "the switch is off");
        assert_eq!(wanted(&st(true, &[("g", without)])), None, "the game runs without Kiln (Just once, or Kiln did not load)");
        assert_eq!(wanted(&st(true, &[])), None, "the game is not running");
        let mut hidden = st(true, &[("g", with)]);
        hidden.shown = false;
        assert_eq!(wanted(&hidden), None, "the Community page is hidden");
    }
}
