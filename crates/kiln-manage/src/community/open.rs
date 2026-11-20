//! Opening a Discord link in the Discord app when this PC has it, else in the browser (the launcher's "Open in Discord"
//! buttons: a support post, the build's post in #testing, the server). The Discord app registers the `discord:` URL
//! scheme on Windows, macOS and Linux; `discord://-/channels/<guild>/<channel>[/<message>]` opens that place in it.
//!
//! Whether the scheme is registered is asked before it is used (the launcher's glue, commands/app.rs), because opening
//! an unregistered scheme does not fail where it can be seen: Windows' `explorer` shows "Get an app to open this link"
//! and exits like it does for every link, and a Linux desktop without a handler may hand it to the browser. The probes'
//! answers are read here ([`xdg_handler`], [`reg_command_exe`]) so they can be tested; [`open_with`] tries the app link
//! and falls back to the web link when the app link can't be used or opening it fails.

/// The app link for a Discord web link: `https://discord.com/channels/<id>[/<id>[/<id>]]` (ids are digits) becomes
/// `discord://-/channels/...`. Anything else has no app link.
pub fn app_link(web: &str) -> Option<String> {
    let rest = web.strip_prefix("https://discord.com/channels/")?;
    let parts: Vec<&str> = rest.split('/').collect();
    let ok = (1..=3).contains(&parts.len()) && parts.iter().all(|p| !p.is_empty() && p.len() <= 24 && p.bytes().all(|b| b.is_ascii_digit()));
    ok.then(|| format!("discord://-/channels/{rest}"))
}

/// Where a link was opened.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Opened {
    App,
    Web,
}

impl Opened {
    pub fn as_str(self) -> &'static str {
        match self {
            Opened::App => "app",
            Opened::Web => "web",
        }
    }
}

/// Opens `link` (a Discord web link) in the app when `app_usable` (the scheme is registered and this is no Game Mode
/// session) and it has an app link; else, or when opening the app link fails, opens `fallback` (the web link to use
/// instead; `link` itself when none). `open` opens one target and says whether that worked.
pub fn open_with<E>(link: &str, fallback: Option<&str>, app_usable: bool, mut open: impl FnMut(&str) -> Result<(), E>) -> Result<Opened, E> {
    if app_usable && let Some(app) = app_link(link) && open(&app).is_ok() {
        return Ok(Opened::App);
    }
    open(fallback.unwrap_or(link)).map(|()| Opened::Web)
}

/// `xdg-mime query default x-scheme-handler/discord`: the desktop file that handles the scheme, or None when nothing
/// does (xdg-mime prints nothing then).
pub fn xdg_handler(stdout: &str) -> Option<&str> {
    let line = stdout.lines().map(str::trim).find(|l| !l.is_empty())?;
    (line.ends_with(".desktop") && !line.contains('/') && !line.contains(char::is_whitespace)).then_some(line)
}

/// `reg query HKCR\discord\shell\open\command /ve`: the program the scheme starts (its first word, or the quoted
/// path), with `%VAR%` expanded through `env`. The value's name is the system language's ("(Default)", "(Standard)"),
/// so the line is found by its type, `REG_SZ` or `REG_EXPAND_SZ`.
pub fn reg_command_exe(stdout: &str, env: &dyn Fn(&str) -> Option<String>) -> Option<String> {
    let value = stdout.lines().find_map(|l| {
        let i = l.find("REG_EXPAND_SZ").map(|i| i + "REG_EXPAND_SZ".len()).or_else(|| l.find("REG_SZ").map(|i| i + "REG_SZ".len()))?;
        Some(l[i..].trim())
    })?;
    let exe = match value.strip_prefix('"') {
        Some(q) => q.split('"').next()?,
        None => value.split_whitespace().next()?,
    };
    if exe.is_empty() {
        return None;
    }
    // %VAR% from the environment; an unknown one leaves the path unusable
    let mut out = String::new();
    let mut rest = exe;
    while let Some(a) = rest.find('%') {
        out.push_str(&rest[..a]);
        let after = &rest[a + 1..];
        let b = after.find('%')?;
        out.push_str(&env(&after[..b])?);
        rest = &after[b + 1..];
    }
    out.push_str(rest);
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    const POST: &str = "https://discord.com/channels/1568454645683195976/1573130884210000003";

    #[test]
    fn a_discord_web_link_has_an_app_link_and_nothing_else_does() {
        assert_eq!(app_link(POST).as_deref(), Some("discord://-/channels/1568454645683195976/1573130884210000003"));
        assert_eq!(app_link(&format!("{POST}/1573130884210000099")).as_deref(), Some("discord://-/channels/1568454645683195976/1573130884210000003/1573130884210000099"));
        assert_eq!(app_link("https://discord.com/channels/1568454645683195976").as_deref(), Some("discord://-/channels/1568454645683195976"));
        for bad in ["https://discord.gg/7pjr8SMTfU", "http://discord.com/channels/1/2", "https://discord.com.evil.io/channels/1/2", "https://discord.com/channels/",
                    "https://discord.com/channels/1/2/3/4", "https://discord.com/channels/a/2", "https://discord.com/channels//2", "https://discord.com/channels/1/2?x=1",
                    "https://frostdev.io/kiln", "javascript:alert(1)"] {
            assert_eq!(app_link(bad), None, "{bad}");
        }
    }

    #[test]
    fn the_app_first_then_the_web_link_when_the_app_link_fails() {
        let run = |usable: bool, app_works: bool, fallback: Option<&str>, link: &str| {
            let mut tried = vec![];
            let r = open_with(link, fallback, usable, |t: &str| -> Result<(), ()> {
                tried.push(t.to_string());
                if t.starts_with("discord:") && !app_works { Err(()) } else { Ok(()) }
            });
            (r, tried)
        };
        let app = "discord://-/channels/1568454645683195976/1573130884210000003";
        // the scheme is there and opens: only the app
        assert_eq!(run(true, true, None, POST), (Ok(Opened::App), vec![app.to_string()]));
        // opening it failed: the web link after it
        assert_eq!(run(true, false, None, POST), (Ok(Opened::Web), vec![app.to_string(), POST.to_string()]));
        // not registered, or Game Mode: the web link only, never the scheme
        assert_eq!(run(false, true, None, POST), (Ok(Opened::Web), vec![POST.to_string()]));
        // a separate web link for the fallback (the server's invite for "Open Discord")
        let guild = "https://discord.com/channels/1568454645683195976";
        assert_eq!(run(false, true, Some("https://discord.gg/7pjr8SMTfU"), guild), (Ok(Opened::Web), vec!["https://discord.gg/7pjr8SMTfU".to_string()]));
        assert_eq!(run(true, false, Some("https://discord.gg/7pjr8SMTfU"), guild).1, vec!["discord://-/channels/1568454645683195976".to_string(), "https://discord.gg/7pjr8SMTfU".to_string()]);
        // a link with no app link goes to the browser as it is
        assert_eq!(run(true, true, None, "https://discord.gg/7pjr8SMTfU"), (Ok(Opened::Web), vec!["https://discord.gg/7pjr8SMTfU".to_string()]));
        // the web link failing too is the error
        assert_eq!(open_with(POST, None, true, |_: &str| Err::<(), _>("no opener")), Err("no opener"));
    }

    #[test]
    fn xdg_mime_names_the_handler_or_nothing() {
        assert_eq!(xdg_handler("discord.desktop\n"), Some("discord.desktop"));
        assert_eq!(xdg_handler("com.discordapp.Discord.desktop\n"), Some("com.discordapp.Discord.desktop"));
        assert_eq!(xdg_handler(""), None);
        assert_eq!(xdg_handler("\n"), None);
        assert_eq!(xdg_handler("xdg-mime: no method available for querying MIME type\n"), None);
    }

    #[test]
    fn the_registry_command_gives_the_program_in_any_language() {
        let env = |k: &str| (k == "LOCALAPPDATA").then(|| r"C:\Users\Ember\AppData\Local".to_string());
        let en = "\r\nHKEY_CLASSES_ROOT\\discord\\shell\\open\\command\r\n    (Default)    REG_SZ    \"C:\\Users\\Ember\\AppData\\Local\\Discord\\Update.exe\" --processStart Discord.exe --process-start-args \"%1\"\r\n\r\n";
        assert_eq!(reg_command_exe(en, &env).as_deref(), Some(r"C:\Users\Ember\AppData\Local\Discord\Update.exe"));
        let de = "\r\nHKEY_CLASSES_ROOT\\discord\\shell\\open\\command\r\n    (Standard)    REG_EXPAND_SZ    %LOCALAPPDATA%\\Discord\\Update.exe --processStart Discord.exe\r\n";
        assert_eq!(reg_command_exe(de, &env).as_deref(), Some(r"C:\Users\Ember\AppData\Local\Discord\Update.exe"));
        // no value, an empty one, or a variable the environment doesn't have: no program
        assert_eq!(reg_command_exe("ERROR: The system was unable to find the specified registry key or value.\r\n", &env), None);
        assert_eq!(reg_command_exe("    (Default)    REG_SZ    \r\n", &env), None);
        assert_eq!(reg_command_exe("    (Default)    REG_EXPAND_SZ    %NOPE%\\Discord.exe\r\n", &env), None);
    }
}
