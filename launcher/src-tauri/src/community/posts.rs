//! The player's support posts while the launcher runs: read every 5 minutes while signed in and the Community page is
//! shown (the page reads them itself when it opens), and, only with "Tell me when Kiln support replies" on, a system
//! notification for a new reply, with "Open in Discord".
//!
//! The notification uses what the system already has, without a new library: Linux `notify-send` (its action button
//! opens the post; `gdbus` and the notification service without a button when notify-send is missing), Windows a toast
//! through PowerShell whose button opens the post's link. It says who replied and to which post, nothing else.

use kiln_manage::community::SupportPost;
use std::sync::{Condvar, LazyLock, Mutex, Once};
use std::time::Duration;

/// How often the posts are read in the background.
pub const EVERY: Duration = Duration::from_secs(5 * 60);
/// The first background read after the launcher starts (so the header's count is there).
const FIRST: Duration = Duration::from_secs(20);

/// Posts with replies the player has not seen and that were not unread before.
pub fn new_replies(before: &[SupportPost], after: &[SupportPost]) -> Vec<SupportPost> {
    after.iter().filter(|p| p.unread > before.iter().find(|b| b.id == p.id).map(|b| b.unread).unwrap_or(0)).cloned().collect()
}

static WAKE: LazyLock<(Mutex<bool>, Condvar)> = LazyLock::new(|| (Mutex::new(false), Condvar::new()));
static START: Once = Once::new();

/// Starts the background reads (once).
pub fn start(app: &tauri::AppHandle) {
    START.call_once(|| {
        let app = app.clone();
        let _ = std::thread::Builder::new().name("kiln-community-posts".into()).spawn(move || {
            let mut wait = FIRST;
            loop {
                {
                    let (m, cv) = &*WAKE;
                    let mut woke = m.lock().unwrap();
                    if !*woke {
                        woke = cv.wait_timeout(woke, wait).unwrap().0;
                    }
                    *woke = false;
                }
                wait = EVERY;
                // only signed in with the page shown: the relay hears nothing otherwise
                if crate::commands::community::signed_in() {
                    let _ = crate::commands::community::refresh_posts(&app);
                }
            }
        });
    });
}

/// Reads the posts soon (after a sign-in).
pub fn wake() {
    let (m, cv) = &*WAKE;
    *m.lock().unwrap() = true;
    cv.notify_all();
}

/// The notification's words in the launcher's language (en.json community.notify.*, community.posts.open).
#[derive(Debug, Clone, PartialEq, Eq, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct NotifyTexts {
    /// "Kiln support replied"
    pub title: String,
    /// "{by} replied to “{title}”."
    pub detail: String,
    /// "Open in Discord"
    pub open: String,
}

impl Default for NotifyTexts {
    fn default() -> Self {
        NotifyTexts { title: "Kiln support replied".into(), detail: "{by} replied to “{title}”.".into(), open: "Open in Discord".into() }
    }
}

static TEXTS: LazyLock<Mutex<NotifyTexts>> = LazyLock::new(|| Mutex::new(NotifyTexts::default()));

pub fn set_texts(t: NotifyTexts) {
    *TEXTS.lock().unwrap() = t;
}

/// The notification's two lines for a post.
pub fn lines(t: &NotifyTexts, p: &SupportPost) -> (String, String) {
    let by = p.last_reply.as_ref().map(|r| r.name.as_str()).unwrap_or("Kiln support");
    let clean = |s: &str| s.chars().filter(|c| !c.is_control()).take(120).collect::<String>();
    (clean(&t.title), clean(&t.detail.replace("{by}", by).replace("{title}", &p.title)))
}

/// A system notification for a new reply (only called with the switch on). Runs apart: it never holds anything up.
pub fn notify(_app: &tauri::AppHandle, p: &SupportPost) {
    if !kiln_manage::community::is_discord_url(&p.url) {
        return;
    }
    let t = TEXTS.lock().unwrap().clone();
    let (title, body) = lines(&t, p);
    let url = p.url.clone();
    std::thread::spawn(move || show(&title, &body, &t.open, &url));
}

#[cfg(target_os = "linux")]
fn show(title: &str, body: &str, open: &str, url: &str) {
    use std::process::Stdio;
    if let Some(exe) = kiln_manage::util::which("notify-send") {
        // with the button: notify-send waits and prints the action's key when it is pressed
        let out = kiln_manage::hostenv::command(&exe)
            .args(["--app-name=Kiln Launcher", "--icon=kiln-launcher", &format!("--action=open={open}"), "--wait", "--", title, body])
            .stdin(Stdio::null()).stderr(Stdio::null()).output();
        match out {
            Ok(o) if o.status.success() => {
                if String::from_utf8_lossy(&o.stdout).trim() == "open" {
                    let _ = crate::commands::app::open_discord_link(url, None);
                }
                return;
            }
            _ => {
                // an older notify-send without actions
                if kiln_manage::hostenv::command(&exe).args(["--app-name=Kiln Launcher", "--", title, body]).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).status().is_ok_and(|s| s.success()) {
                    return;
                }
            }
        }
    }
    if let Some(exe) = kiln_manage::util::which("gdbus") {
        let q = |s: &str| format!("'{}'", s.replace('\\', "\\\\").replace('\'', "\\'"));
        let _ = kiln_manage::hostenv::command(&exe)
            .args(["call", "--session", "--dest", "org.freedesktop.Notifications", "--object-path", "/org/freedesktop/Notifications",
                "--method", "org.freedesktop.Notifications.Notify", "'Kiln Launcher'", "0", "'kiln-launcher'", &q(title), &q(body), "[]", "{}", "10000"])
            .stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).status();
    }
}

#[cfg(windows)]
fn show(title: &str, body: &str, open: &str, url: &str) {
    use std::os::windows::process::CommandExt;
    // the Discord app's link when it is there (the toast opens it through Windows' protocol activation), else the web link
    let app = crate::commands::app::discord_app_usable(url).then(|| kiln_manage::community::open::app_link(url)).flatten();
    let url = app.as_deref().unwrap_or(url);
    let x = |s: &str| s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;").replace('\'', "&apos;");
    let xml = format!(
        "<toast activationType=\"protocol\" launch=\"{u}\"><visual><binding template=\"ToastGeneric\"><text>{t}</text><text>{b}</text></binding></visual><actions><action content=\"{o}\" activationType=\"protocol\" arguments=\"{u}\"/></actions></toast>",
        u = x(url), t = x(title), b = x(body), o = x(open));
    // PowerShell's own app id: a desktop app without a Start menu shortcut of its own may show toasts under it
    let script = format!(
        "[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] > $null; \
         [Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType = WindowsRuntime] > $null; \
         $d = New-Object Windows.Data.Xml.Dom.XmlDocument; $d.LoadXml('{}'); \
         [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('{{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}}\\WindowsPowerShell\\v1.0\\powershell.exe').Show([Windows.UI.Notifications.ToastNotification]::new($d))",
        xml.replace('\'', "''"));
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    let _ = std::process::Command::new("powershell.exe")
        .args(["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", &script])
        .creation_flags(CREATE_NO_WINDOW)
        .stdin(std::process::Stdio::null()).stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null())
        .status();
}

#[cfg(not(any(target_os = "linux", windows)))]
fn show(_title: &str, _body: &str, _open: &str, _url: &str) {}

#[cfg(test)]
mod tests {
    use super::*;
    use kiln_manage::community::{LastReply, PostStatus, ZipAudience};

    fn post(id: &str, unread: u32) -> SupportPost {
        SupportPost { id: id.into(), title: "Mods don't load after the game update".into(), url: "https://discord.com/channels/1/2".into(), created: "2026-11-18T10:00:00Z".into(),
            status: PostStatus::Replied, replies: 3, unread, last_reply: Some(LastReply { name: "Rowan".into(), team: true, at: "2026-11-18T11:00:00Z".into() }), tags: vec![], zip: ZipAudience::Team }
    }

    #[test]
    fn only_replies_that_are_new_since_the_last_read_count() {
        assert_eq!(new_replies(&[post("a", 1)], &[post("a", 1)]), vec![]);
        assert_eq!(new_replies(&[post("a", 0)], &[post("a", 1)]).len(), 1);
        assert_eq!(new_replies(&[], &[post("b", 2), post("c", 0)]).iter().map(|p| p.id.as_str()).collect::<Vec<_>>(), ["b"]);
        assert_eq!(new_replies(&[post("a", 2)], &[post("a", 0)]), vec![], "seen elsewhere: nothing to tell");
    }

    #[test]
    fn the_notification_says_who_replied_to_which_post_and_nothing_else() {
        let (t, b) = lines(&NotifyTexts::default(), &post("a", 1));
        assert_eq!(t, "Kiln support replied");
        assert_eq!(b, "Rowan replied to “Mods don't load after the game update”.");
    }

    #[test]
    fn the_default_words_are_en_jsons() {
        let en: serde_json::Value = serde_json::from_str(include_str!("../../../src/i18n/en.json")).unwrap();
        let d = NotifyTexts::default();
        assert_eq!(en["community"]["notify"]["title"], d.title.as_str());
        assert_eq!(en["community"]["notify"]["detail"], d.detail.as_str());
        assert_eq!(en["community"]["posts"]["open"], d.open.as_str());
    }
}
