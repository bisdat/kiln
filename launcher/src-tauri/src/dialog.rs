//! The launcher's own messages when its page cannot start (Linux, 0.2.0-dev.29.18). Native: never through the web
//! page or WebKit's processes. GTK's MessageDialog in this process (GTK 3 alone, the toolkit the window already uses);
//! when GTK cannot show one (before the window: no display; after it: the main loop does not answer within
//! [`SHOWN_WITHIN`]), zenity, then kdialog, then notify-send (no buttons: the text names the flags). The same text always
//! goes to stderr and launcher.log.
//!
//! Two messages (their words: crate::sandbox): at the start, before the window, when WebKit would run its sandbox and
//! bwrap cannot start it ([`at_start`]); and when the page did not start and the launcher has nothing left to try by
//! itself ([`late`], from crate::render's watchdog). Each button is the player's choice; nothing is chosen for them.

use crate::sandbox::{Action, Message, State};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::OnceLock;
use std::time::Duration;

/// How long the window's main loop has to put the message up before the desktop's tools are asked instead.
pub const SHOWN_WITHIN: Duration = Duration::from_secs(5);
/// Debug and QA builds (`dev-eval`) only: `tools` skips GTK (the fallbacks); `<button index>@<seconds>` answers a GTK
/// message by itself after that long (a test on a private Xvfb display has no mouse).
#[cfg(any(debug_assertions, feature = "dev-eval"))]
const TEST_DIALOG: &str = "KILN_LAUNCHER_TEST_DIALOG";

static APP: OnceLock<tauri::AppHandle> = OnceLock::new();
/// The late message went to the desktop's tools: a main loop that answers after all puts up no second one.
static TO_TOOLS: AtomicBool = AtomicBool::new(false);

/// The window exists (crate::render::window_created): a late message goes through its main loop.
pub fn set_app(app: &tauri::AppHandle) {
    let _ = APP.set(app.clone());
}

fn log(msg: &str) {
    crate::commands::app::launcher_log("message", msg);
}

/// launcher.log's path, for the message.
fn log_path() -> String {
    crate::state::dirs().ok().or_else(crate::folder::found_dirs)
        .map(|d| d.logs().join("launcher.log").display().to_string()).unwrap_or_else(|| "(no data folder)".into())
}

/// stderr and launcher.log get the message as it is shown.
fn say(m: &Message) {
    let labels: Vec<&str> = m.buttons.iter().map(|b| b.1).collect();
    eprintln!("{}\n{}\n[{}]", m.title, m.body, labels.join("] ["));
    log(&format!("{}: {} (buttons: {})", m.title, m.body.replace("\n\n", " / ").replace('\n', " / "), labels.join(", ")));
}

#[cfg(any(debug_assertions, feature = "dev-eval"))]
fn test_setting() -> Option<String> {
    std::env::var(TEST_DIALOG).ok()
}
#[cfg(not(any(debug_assertions, feature = "dev-eval")))]
fn test_setting() -> Option<String> {
    None
}

fn label_of(m: &Message, a: Action) -> &'static str {
    m.buttons.iter().find(|b| b.0 == a).map_or("?", |b| b.1)
}

// ---------------------------------------------------------------- at the start

/// Before the window (main thread, before Tauri): the message, then what the player chose. Closing it starts as before.
pub fn at_start(s: &State) {
    let m = crate::sandbox::start_message(s);
    say(&m);
    let answer = match test_setting().as_deref() {
        Some("tools") => None,
        _ => ask_gtk_here(&m),
    }
    .or_else(|| ask_tools(&m, Closed::StartAnyway));
    match answer.flatten() {
        Some(Action::SandboxOff) => {
            log(&format!("the player chose \"{}\": starting again without WebKit's sandbox", label_of(&m, Action::SandboxOff)));
            // the marker waits for the beta copy's gate (crate::sandbox::after_gate in the new start)
            let err = crate::render::restart(&crate::sandbox::relaunch_off("probe"));
            log(&format!("PROBLEM: could not start again: {err}; start the launcher with {}", crate::sandbox::ARG_OFF));
            eprintln!("Kiln Launcher: could not start again: {err}. Start it with {}.", crate::sandbox::ARG_OFF);
        }
        Some(Action::Quit) => {
            log("the player chose \"Quit\"");
            std::process::exit(0);
        }
        Some(a) => log(&format!("the player chose \"{}\": starting as before", label_of(&m, a))),
        None => log("no choice (the message was closed or could not be shown): starting as before"),
    }
}

/// GTK in this thread, before Tauri: a modal message. None: GTK cannot show one here.
fn ask_gtk_here(m: &Message) -> Option<Option<Action>> {
    use gtk::prelude::*;
    if gtk::init().is_err() {
        log("GTK could not start for the message (no display?)");
        return None;
    }
    let d = build(m, None::<&gtk::Window>);
    auto_answer(&d);
    d.show_all();
    focus_default(&d, &m);
    let r = d.run();
    d.hide();
    while gtk::events_pending() {
        gtk::main_iteration();
    }
    Some(index(r).and_then(|i| m.buttons.get(i)).map(|b| b.0))
}

// ---------------------------------------------------------------- the page did not start

/// The page did not start and the launcher has nothing left to try by itself (crate::render's watchdog thread). The
/// message goes up through the window's main loop; when that does not answer, through the desktop's tools from here.
pub fn late(render: &crate::render::Choice, what: &str, restart_failed: Option<&str>) {
    let mut m = crate::sandbox::late_message(crate::sandbox::state(), render, what, &log_path());
    if let Some(e) = restart_failed {
        m.body = m.body.replacen("starting again without it did not work.", &format!("starting again without it did not work ({e})."), 1);
    }
    say(&m);
    let (tx, rx) = std::sync::mpsc::channel::<()>();
    let shown = match (APP.get(), test_setting().as_deref()) {
        (Some(app), t) if t != Some("tools") => {
            let (app2, m2) = (app.clone(), m.clone());
            app.run_on_main_thread(move || show_late(&app2, m2, tx)).is_ok() && rx.recv_timeout(SHOWN_WITHIN).is_ok()
        }
        _ => false,
    };
    if shown {
        return;
    }
    TO_TOOLS.store(true, Ordering::SeqCst);
    log("the window's main loop did not put the message up: the desktop's tools");
    loop {
        match ask_tools(&m, Closed::Nothing).flatten() {
            Some(a) => {
                if !act_late(&m, a, false) {
                    return;
                }
            }
            None => return,
        }
    }
}

/// On the main thread: the message over the window, not modal; the buttons act, "Open the log folder" keeps it up.
fn show_late(app: &tauri::AppHandle, m: Message, shown: std::sync::mpsc::Sender<()>) {
    use gtk::prelude::*;
    use tauri::Manager;
    if TO_TOOLS.load(Ordering::SeqCst) {
        return;
    }
    let parent = app.get_webview_window("main").and_then(|w| w.gtk_window().ok());
    let d = build(&m, parent.as_ref());
    auto_answer(&d);
    let focus = default_button(&m);
    d.connect_response(move |d, r| {
        let Some(a) = index(r).and_then(|i| m.buttons.get(i)).map(|b| b.0) else {
            log("the message was closed: the window stays as it is");
            d.hide();
            return;
        };
        if !act_late(&m, a, true) {
            d.hide();
        }
    });
    d.show_all();
    if let Some(b) = d.widget_for_response(gtk::ResponseType::Other(focus as u16)) {
        b.grab_focus();
    }
    let _ = shown.send(());
}

/// A late message's button. True: the message stays up (the log folder was opened).
fn act_late(m: &Message, a: Action, main_thread: bool) -> bool {
    log(&format!("the player chose \"{}\"", label_of(m, a)));
    match a {
        Action::OpenLogs => {
            let logs = crate::state::dirs().ok().or_else(crate::folder::found_dirs).map(|d| d.logs());
            match logs.map(|l| crate::reveal::open(None, &l.display().to_string())) {
                Some(Ok(())) => {}
                Some(Err(e)) => log(&format!("could not open the log folder: {}", e.message)),
                None => log("could not open the log folder: no data folder"),
            }
            true
        }
        Action::SandboxOff => {
            match crate::state::dirs().and_then(|d| crate::sandbox::write_marker(&d, crate::sandbox::restart_why("late")).map(|_| crate::sandbox::marker(&d))) {
                Ok(p) => log(&format!("wrote {}", p.display())),
                Err(e) => log(&format!("could not write the marker yet ({}); the next start writes it", e.message)),
            }
            let err = crate::render::restart(&crate::sandbox::relaunch_off("late"));
            log(&format!("PROBLEM: could not start again: {err}; start the launcher with {}", crate::sandbox::ARG_OFF));
            false
        }
        Action::NoGpu => {
            let why = "Turned on by the player in the launcher's message after the page did not start";
            match crate::state::dirs().and_then(|d| crate::render::write_marker(&d, why).map(|_| crate::render::marker(&d))) {
                Ok(p) => log(&format!("wrote {}", p.display())),
                Err(e) => log(&format!("could not write the render marker ({}); this restart still draws without the GPU", e.message)),
            }
            let err = crate::render::restart(&crate::render::Relaunch::software_by_click().keeping());
            log(&format!("PROBLEM: could not start again: {err}; start the launcher with {}", crate::render::ARG_SOFTWARE));
            false
        }
        Action::Quit => {
            match APP.get() {
                Some(app) => app.exit(0),
                None => std::process::exit(0),
            }
            if !main_thread {
                // the main loop may be what does not answer
                std::thread::sleep(Duration::from_secs(3));
                std::process::exit(0);
            }
            false
        }
        Action::StartAnyway => false,
    }
}

// ---------------------------------------------------------------- GTK

fn build(m: &Message, parent: Option<&impl gtk::prelude::IsA<gtk::Window>>) -> gtk::MessageDialog {
    use gtk::prelude::*;
    let (head, rest) = m.body.split_once('\n').unwrap_or((&m.body, ""));
    let d = gtk::MessageDialog::new(parent, gtk::DialogFlags::empty(), gtk::MessageType::Warning, gtk::ButtonsType::None, head);
    d.set_title(&m.title);
    d.set_secondary_text(Some(rest.trim_start()));
    for (i, (_, label)) in m.buttons.iter().enumerate() {
        d.add_button(label, gtk::ResponseType::Other(i as u16));
    }
    d.set_default_response(gtk::ResponseType::Other(default_button(m) as u16));
    d.set_keep_above(true);
    // the log's path and the link can be selected and copied
    if let Ok(area) = d.message_area().downcast::<gtk::Box>() {
        for w in area.children() {
            if let Ok(l) = w.downcast::<gtk::Label>() {
                l.set_selectable(true);
                l.set_max_width_chars(90);
            }
        }
    }
    d
}

/// The button Enter presses: the first one that does not lower protection. Turning WebKit's sandbox off is always a
/// button of its own the player picks, never what a stray Enter does.
fn default_button(m: &Message) -> usize {
    m.buttons.iter().position(|b| b.0 != Action::SandboxOff).unwrap_or(0)
}

/// The default button has the focus (a selectable text would otherwise get it, all selected).
fn focus_default(d: &gtk::MessageDialog, m: &Message) {
    use gtk::prelude::*;
    if let Some(b) = d.widget_for_response(gtk::ResponseType::Other(default_button(m) as u16)) {
        b.grab_focus();
    }
}

fn index(r: gtk::ResponseType) -> Option<usize> {
    match r {
        gtk::ResponseType::Other(i) => Some(i as usize),
        _ => None,
    }
}

/// Tests (debug and `dev-eval` builds): `KILN_LAUNCHER_TEST_DIALOG=<index>@<seconds>` presses that button by itself.
fn auto_answer(d: &gtk::MessageDialog) {
    use gtk::prelude::*;
    let Some((i, secs)) = test_setting().and_then(|t| {
        let (i, s) = t.split_once('@')?;
        Some((i.parse::<u16>().ok()?, s.parse::<u64>().ok()?))
    }) else { return };
    log(&format!("test: button {i} is pressed in {secs} s"));
    let d = d.clone();
    gtk::glib::timeout_add_local_once(Duration::from_secs(secs), move || d.response(gtk::ResponseType::Other(i)));
}

// ---------------------------------------------------------------- the desktop's tools

/// What closing a tool's window without a button means.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Closed {
    /// The message at the start: as before.
    StartAnyway,
    /// A late message: nothing (the window stays).
    Nothing,
}

/// zenity, then kdialog, then notify-send. None: none of them could show it; Some(None): shown, no choice.
fn ask_tools(m: &Message, closed: Closed) -> Option<Option<Action>> {
    if let Some(a) = zenity(m, closed) {
        return Some(a);
    }
    if let Some(a) = kdialog(m, closed) {
        return Some(a);
    }
    let mut body = m.body.clone();
    let flags: Vec<&str> = m.buttons.iter().filter_map(|b| match b.0 {
        Action::SandboxOff => Some(crate::sandbox::ARG_OFF),
        Action::NoGpu => Some(crate::render::ARG_SOFTWARE),
        _ => None,
    }).collect();
    if !flags.is_empty() {
        body += &format!("\n\nThis notice has no buttons: to choose, start the launcher with {}.", flags.join(" or "));
    }
    let ok = kiln_manage::hostenv::command("notify-send").args(["-u", "critical", "-a", "Kiln Launcher", &m.title, &body])
        .stdin(std::process::Stdio::null()).stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null())
        .status().is_ok_and(|s| s.success());
    log(if ok { "the message went out as a desktop notice (notify-send)" } else { "no tool could show the message (zenity, kdialog, notify-send)" });
    ok.then_some(None)
}

/// The buttons as a tool shows them.
#[derive(Debug, Clone, PartialEq, Eq)]
struct ToolButtons {
    /// Its OK button, the one Enter presses: never "Turn the sandbox off" while another is there.
    ok: (Action, &'static str),
    /// The label of its Cancel button, which means what closing means.
    cancel: &'static str,
    /// The rest, in the message's order.
    extra: Vec<(Action, &'static str)>,
}

fn tool_buttons(m: &Message, closed: Closed) -> Option<ToolButtons> {
    let (cancel, mut left): (&'static str, Vec<(Action, &'static str)>) = match closed {
        Closed::StartAnyway => (label_of(m, Action::StartAnyway), m.buttons.iter().copied().filter(|b| b.0 != Action::StartAnyway).collect()),
        Closed::Nothing => ("Close", m.buttons.clone()),
    };
    if left.is_empty() {
        return None;
    }
    let ok = left.remove(left.iter().position(|b| b.0 != Action::SandboxOff).unwrap_or(0));
    Some(ToolButtons { ok, cancel, extra: left })
}

fn closed_means(closed: Closed) -> Option<Action> {
    match closed {
        Closed::StartAnyway => Some(Action::StartAnyway),
        Closed::Nothing => None,
    }
}

fn zenity(m: &Message, closed: Closed) -> Option<Option<Action>> {
    let b = tool_buttons(m, closed)?;
    let mut cmd = kiln_manage::hostenv::command("zenity");
    cmd.args(["--question", "--no-markup", "--width=640", "--title", &m.title, "--text", &m.body])
        .arg(format!("--ok-label={}", b.ok.1)).arg(format!("--cancel-label={}", b.cancel));
    for (_, l) in &b.extra {
        cmd.arg(format!("--extra-button={l}"));
    }
    // stdout: an extra button's label; stderr: why it showed nothing
    let out = cmd.stdin(std::process::Stdio::null()).output().ok()?;
    zenity_answer(&b, closed, out.status.code(), &String::from_utf8_lossy(&out.stdout), &String::from_utf8_lossy(&out.stderr))
}

/// What zenity's end means. None: it showed nothing (the next tool is asked); Some(None): shown, no choice.
fn zenity_answer(b: &ToolButtons, closed: Closed, code: Option<i32>, stdout: &str, stderr: &str) -> Option<Option<Action>> {
    let said = stdout.trim();
    match code {
        Some(0) => Some(Some(b.ok.0)),
        Some(1) if !said.is_empty() => Some(b.extra.iter().find(|x| x.1 == said).map(|x| x.0).or(closed_means(closed))),
        // 1 is also GTK's exit when zenity cannot open a display ("Failed to open display", GTK 3's "cannot open
        // display"): nothing was up, nobody chose
        Some(1) if stderr.contains("open display") => None,
        Some(1) => Some(closed_means(closed)),
        // ended by a signal: it was up and someone closed it
        None => Some(closed_means(closed)),
        // 5: its own timeout; anything else: it could not show the message
        _ => None,
    }
}

fn kdialog(m: &Message, closed: Closed) -> Option<Option<Action>> {
    let b = tool_buttons(m, closed)?;
    let mut cmd = kiln_manage::hostenv::command("kdialog");
    cmd.args(["--title", &m.title]);
    // at most three buttons: OK, the next one ("Turn the sandbox off" when it is there, else Quit), what closing means
    match kdialog_second(&b) {
        Some(second) => cmd.args(["--yesnocancel", &m.body, "--yes-label", b.ok.1, "--no-label", second.1, "--cancel-label", b.cancel]),
        None => cmd.args(["--yesno", &m.body, "--yes-label", b.ok.1, "--no-label", b.cancel]),
    };
    let out = cmd.stdin(std::process::Stdio::null()).stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null()).status().ok()?;
    kdialog_answer(&b, closed, out.code())
}

fn kdialog_second(b: &ToolButtons) -> Option<(Action, &'static str)> {
    let find = |a: Action| b.extra.iter().find(|x| x.0 == a);
    find(Action::SandboxOff).or_else(|| find(Action::Quit)).or(b.extra.first()).copied()
}

/// kdialog's exit: 0 yes, 1 no, 2 cancel (a `--yesno` has no cancel: its no is what closing means); a crash or no
/// display (Qt aborts): it showed nothing.
fn kdialog_answer(b: &ToolButtons, closed: Closed, code: Option<i32>) -> Option<Option<Action>> {
    match (code, kdialog_second(b)) {
        (Some(0), _) => Some(Some(b.ok.0)),
        (Some(1), Some(second)) => Some(Some(second.0)),
        (Some(1), None) | (Some(2), _) => Some(closed_means(closed)),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sandbox::{Action as A, Message};

    fn msg(buttons: &[(A, &'static str)]) -> Message {
        Message { title: "t".into(), body: "b".into(), buttons: buttons.to_vec() }
    }
    const OFF: (A, &str) = (A::SandboxOff, "Turn the sandbox off and start");
    const ANYWAY: (A, &str) = (A::StartAnyway, "Start anyway");
    const QUIT: (A, &str) = (A::Quit, "Quit");
    const NO_GPU: (A, &str) = (A::NoGpu, "Start without the GPU");
    const LOGS: (A, &str) = (A::OpenLogs, "Open the log folder");

    /// Enter (GTK's default button, zenity's and kdialog's first) never turns WebKit's sandbox off: that is always a
    /// button the player picks.
    #[test]
    fn enter_never_turns_the_sandbox_off() {
        let start = msg(&[OFF, ANYWAY, QUIT]);
        assert_eq!(start.buttons[default_button(&start)].0, A::StartAnyway);
        let late = msg(&[(A::SandboxOff, "Start without WebKit's sandbox"), NO_GPU, LOGS, QUIT]);
        assert_eq!(late.buttons[default_button(&late)].0, A::NoGpu);
        assert_eq!(default_button(&msg(&[LOGS, QUIT])), 0);
        // the tools: OK is not the off button; closing means what it meant
        let b = tool_buttons(&start, Closed::StartAnyway).unwrap();
        assert_eq!(b, ToolButtons { ok: QUIT, cancel: "Start anyway", extra: vec![OFF] });
        let b = tool_buttons(&late, Closed::Nothing).unwrap();
        assert_eq!((b.ok, b.cancel), (NO_GPU, "Close"));
        assert_eq!(b.extra.iter().map(|x| x.0).collect::<Vec<_>>(), [A::SandboxOff, A::OpenLogs, A::Quit]);
        // the start's message where the launcher cannot turn it off: Quit, and closing starts as before
        let b = tool_buttons(&msg(&[ANYWAY, QUIT]), Closed::StartAnyway).unwrap();
        assert_eq!(b, ToolButtons { ok: QUIT, cancel: "Start anyway", extra: vec![] });
    }

    /// zenity ends with 1 for Cancel, a closed window, an extra button (its label on stdout) and also when it cannot
    /// open a display at all: then nothing was shown and nobody chose, so the next tool is asked and the log does not
    /// say the player chose.
    #[test]
    fn zenity_without_a_display_is_not_an_answer() {
        let start = msg(&[OFF, ANYWAY, QUIT]);
        let b = tool_buttons(&start, Closed::StartAnyway).unwrap();
        let z = |code, out: &str, err: &str| zenity_answer(&b, Closed::StartAnyway, code, out, err);
        assert_eq!(z(Some(1), "", "(zenity:3025286): Gtk-WARNING **: 13:17:15.740: Failed to open display\n"), None);
        assert_eq!(z(Some(1), "", "Gtk-WARNING **: cannot open display: :99"), None);
        // shown, then: OK, the off button (an extra), Cancel or closed, killed, its own timeout
        assert_eq!(z(Some(0), "", ""), Some(Some(A::Quit)));
        assert_eq!(z(Some(1), "Turn the sandbox off and start\n", ""), Some(Some(A::SandboxOff)));
        assert_eq!(z(Some(1), "", "Gtk-Message: GtkDialog mapped without a transient parent"), Some(Some(A::StartAnyway)));
        assert_eq!(z(None, "", ""), Some(Some(A::StartAnyway)));
        assert_eq!(z(Some(5), "", ""), None);
        // a late message: closing chooses nothing
        let late = tool_buttons(&msg(&[LOGS, QUIT]), Closed::Nothing).unwrap();
        assert_eq!(zenity_answer(&late, Closed::Nothing, Some(1), "", ""), Some(None));
        assert_eq!(zenity_answer(&late, Closed::Nothing, Some(1), "", "Failed to open display"), None);
        assert_eq!(zenity_answer(&late, Closed::Nothing, Some(1), "Quit", ""), Some(Some(A::Quit)));
    }

    #[test]
    fn kdialogs_three_buttons_keep_the_choice_the_message_is_about() {
        let start = tool_buttons(&msg(&[OFF, ANYWAY, QUIT]), Closed::StartAnyway).unwrap();
        assert_eq!(kdialog_second(&start), Some(OFF));
        assert_eq!(kdialog_answer(&start, Closed::StartAnyway, Some(0)), Some(Some(A::Quit)));
        assert_eq!(kdialog_answer(&start, Closed::StartAnyway, Some(1)), Some(Some(A::SandboxOff)));
        assert_eq!(kdialog_answer(&start, Closed::StartAnyway, Some(2)), Some(Some(A::StartAnyway)));
        // Qt without a display aborts: nothing shown
        assert_eq!(kdialog_answer(&start, Closed::StartAnyway, None), None);
        // two buttons (--yesno): its no is what closing means
        let two = tool_buttons(&msg(&[ANYWAY, QUIT]), Closed::StartAnyway).unwrap();
        assert_eq!(kdialog_second(&two), None);
        assert_eq!(kdialog_answer(&two, Closed::StartAnyway, Some(1)), Some(Some(A::StartAnyway)));
        // a late message without the off button: OK is the first try, the second is Quit
        let late = tool_buttons(&msg(&[NO_GPU, LOGS, QUIT]), Closed::Nothing).unwrap();
        assert_eq!((late.ok, kdialog_second(&late)), (NO_GPU, Some(QUIT)));
        assert_eq!(kdialog_answer(&late, Closed::Nothing, Some(2)), Some(None));
    }
}
