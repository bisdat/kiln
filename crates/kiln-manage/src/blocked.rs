//! A file Kiln's installer could not write because Windows refused it (0.2.0-dev.29.18, inbox 92).
//!
//! On a player's PC, "Access is denied" (os error 5) or a sharing violation (os error 32) on a DLL or an exe that Kiln
//! writes into the game folder is almost always an antivirus that holds the file to scan it or is moving it to its
//! quarantine (inbox 85 and 92: kiln-greeneye.dll), or another program that has it open. The game is not one of them:
//! an install does not start while the game runs (`install::refuse_busy`), and a failure while it runs gets no hint
//! (`install::install_report_adding`). Every other error, and every error off Windows (the numbers mean other things
//! there, and no antivirus takes a game's files), is left as it is.
//!
//! - [`io_at`] / [`mark_with`]: the installer's error keeps its words and gains `detail.blocked` ([`Blocked`]). The
//!   launcher says in plain words what may be happening, names the file and links the docs' Antivirus section; the
//!   raw error stays in the message and in launcher.log.
//! - [`last_in_log`]: the support report finds the last such failure in launcher.log again (finding `install-blocked`),
//!   also in the log of a launcher older than this one, whose line has only the words: the path and "(os error 5)".

use crate::error::Error;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::Path;

/// Windows' ERROR_ACCESS_DENIED.
pub const ACCESS_DENIED: i32 = 5;
/// Windows' ERROR_SHARING_VIOLATION.
pub const SHARING_VIOLATION: i32 = 32;

/// The file Windows refused, as an error's `detail.blocked`: `{"path": "kiln/plugins/kiln-greeneye.dll", "osError": 5}`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Blocked {
    /// Game-relative, `/` separators, as the receipt names it (a file staged for the install has its place in the game).
    pub path: String,
    /// [`ACCESS_DENIED`] or [`SHARING_VIOLATION`].
    pub os_error: i32,
}

/// A DLL or an exe: what an antivirus holds or quarantines.
pub fn program_file(path: &str) -> bool {
    let low = path.to_ascii_lowercase();
    low.ends_with(".dll") || low.ends_with(".exe")
}

/// The game-relative path of a file the install writes: `/` separators, and a staged file's place in the game
/// (`kiln/.staging-<id>/new/kiln/x.dll` is `kiln/x.dll`).
pub fn game_rel(rel: &str) -> String {
    let p = rel.replace('\\', "/");
    if let Some(rest) = p.strip_prefix("kiln/.staging-")
        && let Some((_, inner)) = rest.split_once("/new/")
    {
        return inner.to_string();
    }
    p
}

/// `raw`: the OS error number of a failure on the file `rel`; `windows`: the number is Windows' (`cfg!(windows)` for an
/// error of this process). Access denied or a sharing violation on a DLL or an exe; anything else is None.
pub fn classify(raw: Option<i32>, rel: &str, windows: bool) -> Option<Blocked> {
    let code = raw?;
    (windows && matches!(code, ACCESS_DENIED | SHARING_VIOLATION) && program_file(rel)).then(|| Blocked { path: game_rel(rel), os_error: code })
}

/// `err` with `detail.blocked` when [`classify`] says so. An error that has a detail already keeps it as it is.
pub fn mark_with(err: Error, raw: Option<i32>, rel: &str, windows: bool) -> Error {
    match classify(raw, rel, windows) {
        Some(b) if err.detail.is_none() => err.with_detail(serde_json::json!({ "blocked": b })),
        _ => err,
    }
}

/// [`crate::util::io_at`] for a file the install writes (`rel`: its path in the game or in the staging folder), with
/// `detail.blocked` when Windows refused it.
pub fn io_at(path: &Path, what: &str, rel: &str) -> impl FnOnce(std::io::Error) -> Error {
    let at = crate::util::io_at(path, what);
    let rel = rel.to_string();
    move |e| {
        let raw = e.raw_os_error();
        mark_with(at(e), raw, &rel, cfg!(windows))
    }
}

/// The file an error says Windows refused (`detail.blocked`).
pub fn of(e: &Error) -> Option<Blocked> {
    serde_json::from_value(e.detail.as_ref()?.get("blocked")?.clone()).ok()
}

/// `e` without `detail.blocked` (the game started during the install, so it may be what holds the file).
pub fn unmark(mut e: Error) -> Error {
    if of(&e).is_some() {
        e.detail = None;
    }
    e
}

/// A failed runtime change in launcher.log that Windows blocked.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Logged {
    /// When it failed: the line's time (`2026-11-18T23:58:08Z`).
    pub at: String,
    /// The task's title from its "started" line (`Install Kiln 0.2.0-dev.29.17`), when the log still has it.
    pub title: Option<String>,
    pub blocked: Blocked,
    /// The error's words as the log has them (`installing D:\...\kiln-greeneye.dll: Access is denied. (os error 5)`).
    pub error: String,
}

/// The launcher's task kinds that write Kiln's files into a game folder (launcher commands/runtime.rs): an install,
/// update or Repair is `runtime_install`, a rollback `runtime_rollback`.
const KINDS: &[&str] = &["runtime_install", "runtime_rollback"];

/// The last failed install, update, Repair or rollback of Kiln in the game folder `game` that launcher.log's text
/// `log` says Windows blocked: a `task: runtime_install-<n>-<x> failed: [<code>] <what> <path>: <words> (os error N)`
/// line whose path is in `game`. A launcher since 0.2.0-dev.29.18 writes the error's `{"blocked": ...}` after the words
/// and that decides; an older one's line is judged by its words ([`classify`], Windows numbers when `game` is a Windows
/// path). Failures of other kinds, in other folders or without a path are passed over.
pub fn last_in_log(log: &str, game: &Path) -> Option<Logged> {
    let game_text = game.to_string_lossy();
    let windows = is_windows_path(&game_text);
    let folder = norm(&game_text).trim_end_matches('/').to_string();
    if folder.is_empty() {
        return None;
    }
    let mut titles: HashMap<&str, &str> = HashMap::new();
    let mut last = None;
    for line in log.lines() {
        let line = line.trim_end_matches('\r');
        let Some((at, rest)) = line.split_once(' ') else { continue };
        let Some(rest) = rest.strip_prefix("task: ") else { continue };
        let Some((id, rest)) = rest.split_once(' ') else { continue };
        if !KINDS.iter().any(|k| id.strip_prefix(k).is_some_and(|r| r.starts_with('-'))) {
            continue;
        }
        if let Some(title) = rest.strip_prefix("started: ") {
            titles.insert(id, title);
        } else if let Some(failure) = rest.strip_prefix("failed: ")
            && let Some((blocked, error)) = parse_failure(failure, &folder, windows)
        {
            last = Some(Logged { at: at.to_string(), title: titles.get(id).map(|t| t.to_string()), blocked, error });
        }
    }
    last
}

/// `[<code>] <message>[ <detail>] (last step: <step>)`, the failure part of a task line (launcher commands/app.rs).
fn parse_failure(text: &str, folder: &str, windows: bool) -> Option<(Blocked, String)> {
    let (_, msg) = text.strip_prefix('[')?.split_once("] ")?;
    let msg = msg.rfind(" (last step: ").map_or(msg, |i| &msg[..i]);
    // the error's detail, when the launcher wrote one: `{"blocked":{...}}`
    let (msg, detail) = match msg.find(" {\"") {
        Some(i) => {
            let v = serde_json::Deserializer::from_str(&msg[i + 1..]).into_iter::<serde_json::Value>().next().and_then(|v| v.ok());
            (&msg[..i], v)
        }
        None => (msg, None),
    };
    // `<what> <path>: <words>`: the path is the one in the game folder, up to the ": " after it
    let low = norm(msg);
    let start = low.find(&format!("{folder}/"))? + folder.len() + 1;
    let end = start + low[start..].find(": ")?;
    let rel = msg[start..end].replace('\\', "/");
    // a launcher that wrote a detail decided (a failure it did not mark has none, or another one); an older one's words
    let blocked = match &detail {
        Some(d) => d.get("blocked").and_then(|b| serde_json::from_value::<Blocked>(b.clone()).ok()),
        None => classify(os_error(&msg[end..]), &rel, windows),
    }?;
    Some((blocked, msg.to_string()))
}

/// The number of the last `(os error N)` in `text`.
fn os_error(text: &str) -> Option<i32> {
    let i = text.rfind("(os error ")? + "(os error ".len();
    let n = &text[i..];
    n[..n.find(')')?].trim().parse().ok()
}

/// A path for comparing: `/` separators, ASCII lower case (byte for byte as long as the text, so a position in it is one
/// in the original).
fn norm(s: &str) -> String {
    s.replace('\\', "/").to_ascii_lowercase()
}

/// `D:\...`, `D:/...` or `\\server\...`: a path of a Windows launcher (a Linux launcher's are `/...`).
fn is_windows_path(s: &str) -> bool {
    let b = s.as_bytes();
    (b.len() >= 2 && b[0].is_ascii_alphabetic() && b[1] == b':') || s.starts_with("\\\\")
}

/// The support report's sentence for `l` (finding `install-blocked`).
pub fn finding_text(l: &Logged) -> String {
    let what = match &l.title {
        Some(t) => format!("The last Kiln install in this game folder ({t}, {})", l.at),
        None => format!("The last Kiln install in this game folder ({})", l.at),
    };
    let why = if l.blocked.os_error == SHARING_VIOLATION {
        format!("{} was in use by another program (os error {})", l.blocked.path, l.blocked.os_error)
    } else {
        format!("Windows denied access to {} (os error {})", l.blocked.path, l.blocked.os_error)
    };
    format!(
        "{what} failed because {why}. An antivirus or another program may be blocking the file: add an exclusion for the \
         game folder in the antivirus, restore Kiln's files from its quarantine, then install Kiln again on the launcher's \
         Runtime page (docs: launcher/app.md, Antivirus)."
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::error::ErrorCode;

    /// The line of inbox 92's report (a 0.2.0-dev.29.17 launcher on Windows 11), word for word.
    const INBOX92: &str = "2026-11-18T23:58:04Z task: runtime_install-1-6afe3b8c started: Install Kiln 0.2.0-dev.29.17\n\
        2026-11-18T23:58:08Z task: runtime_install-1-6afe3b8c failed: [permission] installing D:\\Steam\\steamapps\\common\\Enshrouded\\kiln/plugins/kiln-greeneye.dll: Access is denied. (os error 5) (last step: installed kiln/plugins/kiln-flame.dll)\n";
    const GAME: &str = r"D:\Steam\steamapps\common\Enshrouded";

    #[test]
    fn access_denied_and_a_sharing_violation_on_a_dll_or_exe_on_windows_are_blocked() {
        let b = |raw, rel: &str, windows| classify(Some(raw), rel, windows);
        assert_eq!(b(5, "kiln/plugins/kiln-greeneye.dll", true), Some(Blocked { path: "kiln/plugins/kiln-greeneye.dll".into(), os_error: 5 }));
        assert_eq!(b(32, "DINPUT8.DLL", true).map(|x| x.os_error), Some(32));
        assert!(b(5, r"kiln\host\Kiln.Host.exe", true).is_some_and(|x| x.path == "kiln/host/Kiln.Host.exe"));
        // a staged file is named by its place in the game
        assert_eq!(b(5, "kiln/.staging-3c989a8dd3a9/new/kiln/plugins/kiln-flame.dll", true).unwrap().path, "kiln/plugins/kiln-flame.dll");
        // other files, other errors, no number, and every number off Windows (5 is EIO on Linux) are not
        assert_eq!(b(5, "kiln/kiln.json", true), None);
        assert_eq!(b(5, "kiln/plugins/kiln-greeneye.dll", false), None);
        assert_eq!(b(13, "kiln/plugins/kiln-greeneye.dll", false), None);
        assert_eq!(b(2, "kiln/plugins/kiln-greeneye.dll", true), None);
        assert_eq!(b(112, "kiln/kiln-core.dll", true), None, "a full disk is no antivirus");
        assert_eq!(classify(None, "kiln/kiln-core.dll", true), None);
    }

    #[test]
    fn a_blocked_error_keeps_its_words_and_gains_detail_blocked() {
        let raw = Error::new(ErrorCode::Permission, r"installing D:\G\kiln/plugins/kiln-greeneye.dll: Access is denied. (os error 5)");
        let e = mark_with(raw.clone(), Some(5), "kiln/plugins/kiln-greeneye.dll", true);
        assert_eq!(e.message, raw.message);
        assert_eq!(e.detail, Some(serde_json::json!({ "blocked": { "path": "kiln/plugins/kiln-greeneye.dll", "osError": 5 } })));
        assert_eq!(of(&e), Some(Blocked { path: "kiln/plugins/kiln-greeneye.dll".into(), os_error: 5 }));
        assert_eq!(of(&unmark(e)), None);
        // not blocked: unchanged; a detail that is there stays
        assert_eq!(mark_with(raw.clone(), Some(5), "kiln/plugins/kiln-greeneye.dll", false), raw);
        let with = raw.clone().with_detail(serde_json::json!({ "pid": 4 }));
        assert_eq!(mark_with(with.clone(), Some(5), "kiln/plugins/kiln-greeneye.dll", true), with);
        assert_eq!(of(&with), None);
    }

    #[test]
    fn io_at_marks_what_this_system_refuses() {
        let e = io_at(Path::new("/g/kiln/x.dll"), "installing", "kiln/x.dll")(std::io::Error::from_raw_os_error(5));
        assert!(e.message.starts_with("installing /g/kiln/x.dll: "), "{}", e.message);
        // Windows' 5 is access denied; elsewhere it is not a refusal at all
        assert_eq!(of(&e).is_some(), cfg!(windows));
    }

    #[test]
    fn inbox92s_line_of_a_launcher_before_this_one_is_found_by_its_words() {
        let l = last_in_log(INBOX92, Path::new(GAME)).expect("blocked");
        assert_eq!(l.at, "2026-11-18T23:58:08Z");
        assert_eq!(l.title.as_deref(), Some("Install Kiln 0.2.0-dev.29.17"));
        assert_eq!(l.blocked, Blocked { path: "kiln/plugins/kiln-greeneye.dll".into(), os_error: 5 });
        assert_eq!(l.error, r"installing D:\Steam\steamapps\common\Enshrouded\kiln/plugins/kiln-greeneye.dll: Access is denied. (os error 5)");
        let text = finding_text(&l);
        assert!(text.starts_with("The last Kiln install in this game folder (Install Kiln 0.2.0-dev.29.17, 2026-11-18T23:58:08Z) failed because Windows denied access to kiln/plugins/kiln-greeneye.dll (os error 5). An antivirus or another program may be blocking the file"), "{text}");
        // the folder in another spelling (Windows does not care), with a trailing separator
        assert!(last_in_log(INBOX92, Path::new(r"d:/steam/STEAMAPPS/common/Enshrouded\")).is_some());
        // the system's own words in another language
        let de = INBOX92.replace("Access is denied.", "Zugriff verweigert");
        assert!(last_in_log(&de, Path::new(GAME)).is_some());
    }

    #[test]
    fn other_folders_errors_and_files_are_passed_over() {
        // another game folder (a dedicated server next to the client)
        assert_eq!(last_in_log(INBOX92, Path::new(r"D:\Steam\steamapps\common\EnshroudedServer")), None);
        // a full disk, a missing file, a JSON file
        for (from, to) in [("Access is denied. (os error 5)", "There is not enough space on the disk. (os error 112)"),
            ("Access is denied. (os error 5)", "The system cannot find the file specified. (os error 2)"),
            ("kiln-greeneye.dll", "kiln.json")] {
            assert_eq!(last_in_log(&INBOX92.replace(from, to), Path::new(GAME)), None, "{to}");
        }
        // the same words from a Linux launcher: 5 is EIO there
        let linux = INBOX92.replace(r"D:\Steam\steamapps\common\Enshrouded\", "/home/p/.steam/steam/steamapps/common/Enshrouded/");
        assert_eq!(last_in_log(&linux, Path::new("/home/p/.steam/steam/steamapps/common/Enshrouded")), None);
        // an uninstall is not an install; a line that is not a task's; a task that was done
        assert_eq!(last_in_log(&INBOX92.replace("runtime_install-", "runtime_uninstall-"), Path::new(GAME)), None);
        assert_eq!(last_in_log("2026-11-18T23:58:08Z update: automatic check (dev): up to date\n", Path::new(GAME)), None);
        assert_eq!(last_in_log("2026-11-18T23:58:08Z task: runtime_install-1-6afe3b8c done: Kiln 0.2.0-dev.29.17 is in the game folder\n", Path::new(GAME)), None);
    }

    #[test]
    fn a_line_with_the_errors_detail_is_decided_by_it_and_the_last_one_counts() {
        // 0.2.0-dev.29.18 and later: the detail follows the words; a sharing violation on Linux paths (a test folder)
        let line = |at: &str, id: &str, file: &str, code: i32| format!(
            "{at} task: {id} failed: [io] moving aside /g/Enshrouded/{file}: busy (os error {code}) {{\"blocked\":{{\"osError\":{code},\"path\":\"{file}\"}}}} (last step: staged {file})\n");
        let log = format!("{}{}2026-11-18T23:00:00Z task: runtime_rollback-3-a started: Roll back to 0.2.0-dev.29.16\n{}",
            line("2026-11-18T22:00:00Z", "runtime_install-1-a", "dinput8.dll", 32),
            line("2026-11-18T22:30:00Z", "runtime_install-2-a", "kiln/kiln-core.dll", 5),
            line("2026-11-18T23:00:01Z", "runtime_rollback-3-a", "kiln/plugins/kiln-eml.dll", 32));
        let l = last_in_log(&log, Path::new("/g/Enshrouded")).expect("blocked");
        assert_eq!((l.at.as_str(), l.title.as_deref()), ("2026-11-18T23:00:01Z", Some("Roll back to 0.2.0-dev.29.16")));
        assert_eq!(l.blocked, Blocked { path: "kiln/plugins/kiln-eml.dll".into(), os_error: 32 });
        assert_eq!(l.error, "moving aside /g/Enshrouded/kiln/plugins/kiln-eml.dll: busy (os error 32)", "the words without the detail");
        assert!(finding_text(&l).contains("because kiln/plugins/kiln-eml.dll was in use by another program (os error 32)."), "{}", finding_text(&l));
        // a detail without `blocked` is no hint, whatever the words (the launcher decided: the game started meanwhile)
        let other = INBOX92.replace(" (last step:", " {\"pid\":4120} (last step:");
        assert_eq!(last_in_log(&other, Path::new(GAME)), None);
    }
}
