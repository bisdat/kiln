//! Tauri commands: the optional Discord features (docs/kiln/launcher/discord.md). The logic is in
//! kiln_manage::community; this file holds what lives as long as the launcher runs: the account and posts as last
//! read, how the relay last answered, the sign-in attempts and the prepared reports, and the background pieces
//! (the posts' refresh, Rich Presence).
//!
//! Every command that reaches the relay checks `community.json` first: with the Community page hidden it is refused
//! before anything is sent (`detail.kind` "hidden"). Signed out, only `community_feed` reaches the network, and the
//! UI calls it only when the Community page opens.

use crate::state::blocking;
use kiln_manage::community::relay::{self, Device, Relay, SigninMode};
use kiln_manage::community::session::{SessionStore, Stored};
use kiln_manage::community::settings::{CommunitySettings, SettingsPatch};
use kiln_manage::community::signin::{Attempt, PageTexts, Progress};
use kiln_manage::community::{Account, Feed, SupportPost};
use kiln_manage::net::Transport;
use kiln_manage::paths::DataDirs;
use kiln_manage::{Error, ErrorCode};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::sync::{LazyLock, Mutex, OnceLock};
use tauri::{AppHandle, Emitter};

/// `kiln://community`: something the Community page shows changed (`changed`), or Kiln support replied (`reply`).
pub const EVENT: &str = "kiln://community";

/// The launcher's release version as About and Settings show it (the app's package version: a release build gets
/// 0.2.0-dev.29.18 there from tools/release; Cargo's own version is only VERSION, 0.2.0-dev). It goes to the relay in
/// the User-Agent and at sign-in. Set when the app starts ([`set_version`]); Cargo's until then.
static APP_VERSION: OnceLock<String> = OnceLock::new();

/// Called once in the app's setup, before any command can run.
pub fn set_version<R: tauri::Runtime>(app: &AppHandle<R>) {
    let _ = APP_VERSION.set(app.package_info().version.to_string());
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CommunityEvent {
    pub kind: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub post: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub title: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub url: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub by: Option<String>,
}

pub(crate) fn emit_changed(app: &AppHandle) {
    let _ = app.emit(EVENT, CommunityEvent { kind: "changed", post: None, title: None, url: None, by: None });
}

/// How the relay answered last.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "lowercase")]
pub enum Reach {
    #[default]
    Ok,
    Offline,
    Down,
}

#[derive(Default)]
struct Hub {
    /// The account as last read (also kept with the session, so it shows while offline).
    account: Option<Account>,
    posts: Vec<SupportPost>,
    /// When `posts` was read (RFC 3339).
    posts_at: Option<String>,
    reach: Reach,
    /// One lock each: a poll that is still asking the relay keeps the next one waiting instead of ending the attempt.
    attempts: HashMap<String, std::sync::Arc<Mutex<Attempt>>>,
}

static HUB: LazyLock<Mutex<Hub>> = LazyLock::new(|| Mutex::new(Hub::default()));

/// The transport to the relay: the real one (https, public addresses only). Debug builds can point at a relay on this
/// machine instead (`KILN_COMMUNITY_RELAY_LOOPBACK=127.0.0.1:8790`): the requests keep frostdev.io's host name and the
/// same checks, but go to that port in plain HTTP. Release builds ignore the variable.
fn transport() -> Box<dyn Transport> {
    #[cfg(debug_assertions)]
    if let Some(addr) = std::env::var("KILN_COMMUNITY_RELAY_LOOPBACK").ok().and_then(|v| v.parse::<std::net::SocketAddr>().ok())
        && let Ok(t) = relay::LoopbackTransport::new(addr)
    {
        return Box::new(t);
    }
    Box::new(kiln_manage::net::UreqTransport::with_timeouts(std::time::Duration::from_secs(10), std::time::Duration::from_secs(60)))
}

fn settings() -> Result<(DataDirs, CommunitySettings), Error> {
    let d = crate::state::dirs()?;
    let s = CommunitySettings::load(&d);
    Ok((d, s))
}

/// The kept session as last read: (data folder, relay host, the session, when read). The keyring is read once and
/// then again only after a save or a clear here, so the status the page asks for every minute never starts
/// `secret-tool` (a locked keyring would ask to be unlocked each time). "No session" is read again after 5 minutes.
static KEPT: LazyLock<Mutex<Option<(std::path::PathBuf, String, Option<Stored>, std::time::Instant)>>> = LazyLock::new(|| Mutex::new(None));

/// The kept session for the relay the settings name.
fn stored(d: &DataDirs, s: &CommunitySettings) -> Option<Stored> {
    let host = s.relay().host;
    if let Some((root, h, v, at)) = &*KEPT.lock().unwrap()
        && *root == d.root
        && *h == host
        && (v.is_some() || at.elapsed() < std::time::Duration::from_secs(5 * 60))
    {
        return v.clone();
    }
    let v = SessionStore::new(d).load(&host);
    *KEPT.lock().unwrap() = Some((d.root.clone(), host, v.clone(), std::time::Instant::now()));
    v
}

/// Keeps the session (keyring, else the owner-only file) and what [`stored`] answers.
fn keep_session(d: &DataDirs, k: &Stored) -> Result<(), Error> {
    *KEPT.lock().unwrap() = None;
    SessionStore::new(d).save(k)?;
    Ok(())
}

/// Forgets what [`stored`] answers (the key itself is removed by the caller, or was just saved).
fn session_changed() {
    *KEPT.lock().unwrap() = None;
}

/// Notes how a call went: the page's offline and relay-down words follow the last answer.
fn note<T>(r: &Result<T, Error>) {
    let reach = match r {
        Ok(_) => Reach::Ok,
        Err(e) => match relay::kind_of(e) {
            Some("offline") => Reach::Offline,
            Some("relay") => Reach::Down,
            Some("hidden") => return,
            _ => Reach::Ok,
        },
    };
    HUB.lock().unwrap().reach = reach;
}

/// One call to the relay with the player's session (when `auth`). A session the relay no longer knows is forgotten
/// here too, so the page says "signed out" instead of failing on every call.
fn call<T>(auth: bool, f: impl FnOnce(&Relay) -> Result<T, Error>) -> Result<T, Error> {
    let (d, s) = settings()?;
    s.network_allowed()?;
    let t = transport();
    let mut r = Relay::new(s.relay(), t.as_ref(), version());
    let kept = stored(&d, &s);
    if auth {
        let k = kept.as_ref().ok_or_else(|| Error::new(ErrorCode::AuthFailed, "Sign in with Discord first.").with_detail(serde_json::json!({ "kind": "signin" })))?;
        r = r.with_session(&k.session);
    }
    let out = f(&r);
    note(&out);
    if let Err(e) = &out
        && auth
        && relay::kind_of(e) == Some("signin")
    {
        let _ = SessionStore::new(&d).clear();
        session_changed();
        let mut h = HUB.lock().unwrap();
        h.account = None;
        h.posts.clear();
        h.posts_at = None;
    }
    out
}

// ---------------------------------------------------------------- settings and status

#[tauri::command]
pub async fn community_settings_get() -> Result<CommunitySettings, Error> {
    blocking(|| Ok(settings()?.1)).await
}

#[tauri::command]
pub async fn community_settings_set(app: AppHandle, patch: SettingsPatch) -> Result<CommunitySettings, Error> {
    let s = blocking(move || CommunitySettings::update(&crate::state::dirs()?, &patch)).await?;
    // hidden: a sign-in on its way ends here, and its listener on 127.0.0.1 closes with it
    if !s.shown {
        HUB.lock().unwrap().attempts.clear();
    }
    crate::community::presence::settings_changed(&s);
    emit_changed(&app);
    Ok(s)
}

/// `community_status`: what the page and the header show, from what the launcher already has. No request.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CommunityStatus {
    pub shown: bool,
    pub account: Option<Account>,
    pub online: bool,
    /// "ok" or "down".
    pub relay: &'static str,
    pub presence: kiln_manage::community::presence::PresenceStatus,
    pub notify: bool,
    pub unread: u32,
    pub posts_as_of: Option<String>,
}

#[tauri::command]
pub async fn community_status(app: AppHandle) -> Result<CommunityStatus, Error> {
    blocking(move || {
        let (d, s) = settings()?;
        let kept = if s.shown { stored(&d, &s) } else { None };
        // signed in with the page shown: the posts are read every 5 minutes from now on (community/posts.rs)
        if kept.is_some() {
            crate::community::posts::start(&app);
        }
        let mut h = HUB.lock().unwrap();
        if kept.is_none() {
            h.account = None;
            h.posts.clear();
            h.posts_at = None;
        } else if h.account.is_none() {
            h.account = kept.as_ref().map(|k| k.account.clone());
        }
        Ok(CommunityStatus {
            shown: s.shown,
            account: h.account.clone(),
            online: h.reach != Reach::Offline,
            relay: if h.reach == Reach::Down { "down" } else { "ok" },
            presence: crate::community::presence::status(&s),
            notify: s.notify,
            unread: if kept.is_some() { h.posts.iter().map(|p| p.unread).sum() } else { 0 },
            posts_as_of: h.posts_at.clone(),
        })
    })
    .await
}

// ---------------------------------------------------------------- public: news, events, known problems

/// News and events, and a fresh copy of the known-problems list kept for offline use. Public: no session is sent.
#[tauri::command]
pub async fn community_feed() -> Result<Feed, Error> {
    blocking(|| {
        let feed = call(false, |r| r.feed())?;
        // the list is fetched with the news (also public); a failure keeps the last copy
        if let Ok(list) = call(false, |r| r.known_issues()) {
            let _ = crate::community::report::keep_known(&list);
        }
        Ok(feed)
    })
    .await
}

// ---------------------------------------------------------------- the account

#[tauri::command]
pub async fn community_check_member(app: AppHandle) -> Result<Option<Account>, Error> {
    let a = blocking(|| {
        let (d, s) = settings()?;
        if stored(&d, &s).is_none() {
            return Ok(None);
        }
        let a = call(true, |r| r.me())?;
        if let Some(mut k) = stored(&d, &s) {
            k.account = a.clone();
            let _ = keep_session(&d, &k);
        }
        HUB.lock().unwrap().account = Some(a.clone());
        Ok(Some(a))
    })
    .await?;
    emit_changed(&app);
    Ok(a)
}

/// Signs out: the relay deletes the session (and with `forget`, what frostdev.io keeps; with `posts`, the player's
/// support posts on Discord), then the launcher deletes its key. The key goes even when the relay can't be reached:
/// signing out never leaves a key behind.
#[tauri::command]
pub async fn community_signout(app: AppHandle, forget: bool, posts: Option<bool>) -> Result<(), Error> {
    let r = blocking(move || {
        let (d, s) = settings()?;
        let has = stored(&d, &s).is_some() && s.shown;
        let r = kiln_manage::community::session::sign_out(&SessionStore::new(&d), forget, || {
            if has { call(true, |r| if forget { r.forget(posts.unwrap_or(false)) } else { r.signout() }) } else { Ok(()) }
        });
        session_changed();
        let mut h = HUB.lock().unwrap();
        h.account = None;
        h.posts.clear();
        h.posts_at = None;
        drop(h);
        crate::community::testers::forget_local();
        r
    })
    .await;
    emit_changed(&app);
    r
}

// ---------------------------------------------------------------- sign-in

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SignInStart {
    pub id: String,
    pub code: String,
    pub url: String,
    pub expires_at: String,
}

/// Starts a sign-in. `mode` browser: the desktop (the UI opens `url` in the browser); qr: the Deck shows `url` as a
/// QR code and `code` for a phone. `texts`: what the browser tab says at the end.
#[tauri::command]
pub async fn community_signin_start(mode: SigninMode, join: bool, texts: Option<PageTexts>) -> Result<SignInStart, Error> {
    blocking(move || {
        let device = if crate::deck::detect::current().detected { Device::Deck } else { Device::Pc };
        let a = call(false, |r| Attempt::start(r, mode, join, device, version(), texts.unwrap_or_default()))?;
        let out = SignInStart { id: a.started.id.clone(), code: a.started.code.clone(), url: a.started.url.clone(), expires_at: a.started.expires_at.clone() };
        let mut h = HUB.lock().unwrap();
        // one attempt at a time: a new one ends the old (its listener closes)
        h.attempts.clear();
        h.attempts.insert(out.id.clone(), std::sync::Arc::new(Mutex::new(a)));
        Ok(out)
    })
    .await
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "lowercase", tag = "state")]
pub enum SignInState {
    Waiting,
    Approved { account: Account },
    Denied,
    Expired,
    Canceled,
}

#[tauri::command]
pub async fn community_signin_poll(app: AppHandle, id: String) -> Result<SignInState, Error> {
    let r = blocking(move || {
        let (d, s) = settings()?;
        s.network_allowed()?;
        let Some(slot) = HUB.lock().unwrap().attempts.get(&id).cloned() else { return Ok(SignInState::Expired) };
        // another poll of this attempt is still asking: this one waits its turn
        let Ok(mut a) = slot.try_lock() else { return Ok(SignInState::Waiting) };
        let t = transport();
        let relay = Relay::new(s.relay(), t.as_ref(), version());
        let p = a.poll(&relay);
        drop(a);
        note(&p);
        let p = p?;
        if p != Progress::Waiting {
            HUB.lock().unwrap().attempts.remove(&id);
        }
        let state = match p {
            Progress::Waiting => return Ok(SignInState::Waiting),
            Progress::Approved(g) => {
                keep_session(&d, &Stored { format: 1, relay: s.relay().host, session: g.session, account: g.account.clone() })?;
                let mut h = HUB.lock().unwrap();
                h.account = Some(g.account.clone());
                h.posts.clear();
                h.posts_at = None;
                crate::commands::app::launcher_log("community", "signed in with Discord");
                SignInState::Approved { account: g.account }
            }
            Progress::Denied => SignInState::Denied,
            Progress::Expired => SignInState::Expired,
            Progress::Canceled => SignInState::Canceled,
        };
        Ok(state)
    })
    .await;
    if matches!(r, Ok(SignInState::Approved { .. })) {
        emit_changed(&app);
        crate::community::posts::wake();
    }
    r
}

#[tauri::command]
pub async fn community_signin_cancel(id: String) -> Result<(), Error> {
    HUB.lock().unwrap().attempts.remove(&id);
    Ok(())
}

// ---------------------------------------------------------------- posts

/// The player's support posts, fresh from the relay (the page asks when it opens; the launcher every 5 minutes).
/// Offline or with the relay down, the error says so and the last list stays in `community_status`'s time.
#[tauri::command]
pub async fn community_posts(app: AppHandle) -> Result<Vec<SupportPost>, Error> {
    blocking(move || refresh_posts(&app)).await
}

/// Reads the posts, keeps them, and says what changed: `reply` for each post with new unread replies.
pub(crate) fn refresh_posts(app: &AppHandle) -> Result<Vec<SupportPost>, Error> {
    let (d, s) = settings()?;
    if !s.shown || stored(&d, &s).is_none() {
        return Ok(Vec::new());
    }
    let posts = match call(true, |r| r.posts()) {
        Ok(p) => p,
        Err(e) => {
            if relay::kind_of(&e) == Some("signin") {
                emit_changed(app);
            }
            return Err(e);
        }
    };
    let before = {
        let mut h = HUB.lock().unwrap();
        let before = std::mem::replace(&mut h.posts, posts.clone());
        h.posts_at = Some(kiln_manage::util::now_rfc3339());
        before
    };
    let fresh = crate::community::posts::new_replies(&before, &posts);
    for p in &fresh {
        let by = p.last_reply.as_ref().map(|r| r.name.clone());
        let _ = app.emit(EVENT, CommunityEvent { kind: "reply", post: Some(p.id.clone()), title: Some(p.title.clone()), url: Some(p.url.clone()), by: by.clone() });
        if s.notify {
            crate::community::posts::notify(app, p);
        }
    }
    if before != posts {
        emit_changed(app);
    }
    Ok(posts)
}

fn post_action(app: &AppHandle, f: impl FnOnce(&Relay) -> Result<(), Error>) -> Result<(), Error> {
    call(true, f)?;
    let _ = refresh_posts(app);
    Ok(())
}

#[tauri::command]
pub async fn community_post_seen(app: AppHandle, id: String) -> Result<(), Error> {
    blocking(move || {
        call(true, |r| r.post_seen(&id))?;
        let mut h = HUB.lock().unwrap();
        for p in h.posts.iter_mut().filter(|p| p.id == id) {
            p.unread = 0;
        }
        drop(h);
        emit_changed(&app);
        Ok(())
    })
    .await
}

#[tauri::command]
pub async fn community_post_solved(app: AppHandle, id: String) -> Result<(), Error> {
    blocking(move || post_action(&app, |r| r.post_solved(&id))).await
}

#[tauri::command]
pub async fn community_post_delete(app: AppHandle, id: String) -> Result<(), Error> {
    blocking(move || post_action(&app, |r| r.post_delete(&id))).await
}

/// For the other community modules: a call with the session, and the status's account.
pub(crate) fn relay_call<T>(f: impl FnOnce(&Relay) -> Result<T, Error>) -> Result<T, Error> {
    call(true, f)
}

pub(crate) fn account() -> Option<Account> {
    HUB.lock().unwrap().account.clone()
}

pub(crate) fn signed_in() -> bool {
    settings().is_ok_and(|(d, s)| s.shown && stored(&d, &s).is_some())
}

pub(crate) fn version() -> &'static str {
    APP_VERSION.get().map(String::as_str).unwrap_or(env!("CARGO_PKG_VERSION"))
}

// ---------------------------------------------------------------- Rich Presence (step 3), reports (step 5), testers (step 8)

/// What only the window knows: how many mods are on (the Play page's count, every kind) for Rich Presence, and the
/// words in the launcher's language (Rich Presence's lines, the reply notification's).
#[tauri::command]
pub async fn community_presence_facts(mods: Option<u32>, texts: Option<kiln_manage::community::presence::PresenceTexts>,
    notify: Option<crate::community::posts::NotifyTexts>) -> Result<(), Error> {
    crate::community::presence::facts(mods, texts);
    if let Some(n) = notify {
        crate::community::posts::set_texts(n);
    }
    Ok(())
}

#[tauri::command]
pub async fn community_report_prepare(app: AppHandle, target: Option<super::logs::LogTarget>, from: kiln_manage::community::SendFrom) -> Result<crate::community::report::ReportPreview, Error> {
    crate::community::report::prepare(app, target, from).await
}

#[tauri::command]
pub async fn community_report_send(app: AppHandle, prepared: String, draft: kiln_manage::community::ReportDraft) -> Result<SupportPost, Error> {
    let post = blocking(move || crate::community::report::send(&prepared, &draft)).await?;
    let _ = blocking({ let app = app.clone(); move || refresh_posts(&app) }).await;
    emit_changed(&app);
    Ok(post)
}

#[tauri::command]
pub async fn tester_status() -> Result<crate::community::testers::TesterStatus, Error> {
    blocking(crate::community::testers::status).await
}

#[tauri::command]
pub async fn tester_join(app: AppHandle, beta: bool) -> Result<(), Error> {
    blocking(move || crate::community::testers::join(beta)).await?;
    emit_changed(&app);
    Ok(())
}

#[tauri::command]
pub async fn tester_leave(app: AppHandle) -> Result<(), Error> {
    blocking(crate::community::testers::leave).await?;
    emit_changed(&app);
    Ok(())
}

/// `result` or `note` left out: that one stays as it is (a note typed after a pick never undoes the pick).
#[tauri::command]
pub async fn tester_result_set(step: String, result: Option<kiln_manage::community::testers::ResultChange>, note: Option<String>) -> Result<Option<crate::community::testers::PlanView>, Error> {
    blocking(move || crate::community::testers::set_result(&step, result, note)).await
}

#[tauri::command]
pub async fn tester_results_send(app: AppHandle, with_report: bool) -> Result<Option<crate::community::testers::PlanView>, Error> {
    let r = crate::community::testers::send_results(app.clone(), with_report).await?;
    emit_changed(&app);
    Ok(r)
}

/// Deck: asks Steam for its on-screen keyboard (`steam://open/keyboard`) when a field that takes words opens. Only on a
/// Steam Deck, SteamOS or gamescope; elsewhere nothing happens. Nothing leaves the PC.
#[tauri::command]
pub async fn community_steam_keyboard() -> Result<(), Error> {
    if !cfg!(target_os = "linux") || !crate::deck::detect::current().detected {
        return Ok(());
    }
    crate::state::open_external(std::ffi::OsStr::new("steam://open/keyboard"))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The keyring (or the file) is read once, and again after a save or a clear here: the status the page reads every
    /// minute starts no `secret-tool`.
    #[test]
    fn the_kept_session_is_read_once_and_again_after_a_save_or_a_clear() {
        // SAFETY: no other test of this crate reads the variable or the session store (the file, never the keyring)
        unsafe { std::env::set_var("KILN_NO_KEYRING", "1") };
        let tmp = std::env::temp_dir().join(format!("kiln-launcher-kept-{}", std::process::id()));
        let d = DataDirs::at(&tmp);
        let s = CommunitySettings::default();
        assert!(stored(&d, &s).is_none());
        let k = Stored { format: 1, relay: "frostdev.io".into(), session: "kiln_s_0123456789abcdefghijklmnop".into(),
            account: Account { id: "731204485922648124".into(), name: "emberfox".into(), member: true, tester: false } };
        keep_session(&d, &k).unwrap();
        assert_eq!(stored(&d, &s).unwrap().account.name, "emberfox");
        // read once: the file going behind the launcher's back changes nothing until the launcher itself changes it
        std::fs::remove_file(SessionStore::new(&d).file_path()).unwrap();
        assert!(stored(&d, &s).is_some());
        session_changed();
        assert!(stored(&d, &s).is_none());
        let _ = std::fs::remove_dir_all(&tmp);
    }

    /// The relay hears the version About shows (the app's package version, a release's 0.2.0-dev.29.18), not Cargo's
    /// 0.2.0-dev: the User-Agent and the sign-in request take it from [`version`].
    #[test]
    fn the_relay_hears_the_release_version_about_shows() {
        let mut ctx = tauri::test::mock_context(tauri::test::noop_assets());
        ctx.package_info_mut().version = "0.2.0-dev.29.18".parse().unwrap();
        let app = tauri::test::mock_builder().build(ctx).unwrap();
        set_version(app.handle());
        assert_eq!(version(), "0.2.0-dev.29.18");
        assert_eq!(version(), app.handle().package_info().version.to_string());
        // every request to the relay is made with it: no other version in this file
        let src = include_str!("community.rs");
        assert_eq!(src.matches(concat!("env!(\"CARGO_", "PKG_VERSION\")")).count(), 1, "only the fallback before set_version");
        assert!(src.contains(concat!("Relay::new(s.relay(), t.as_ref(), ", "version())")) && src.contains(concat!("device, ", "version(), ")));
    }
}
