// Kiln Launcher.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

/// Release builds are GUI programs on Windows (no console): `--selftest` attaches to the console it was started from
/// (cmd, PowerShell), so its report is visible there. Output redirected to a file or pipe is left alone.
#[cfg(windows)]
fn attach_parent_console() {
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn AttachConsole(process_id: u32) -> i32;
        fn GetStdHandle(std_handle: u32) -> *mut core::ffi::c_void;
    }
    const ATTACH_PARENT_PROCESS: u32 = u32::MAX;
    const STD_OUTPUT_HANDLE: u32 = -11i32 as u32;
    // SAFETY: plain Win32 calls without pointers; failure (no parent console) is harmless.
    unsafe {
        let out = GetStdHandle(STD_OUTPUT_HANDLE);
        if out.is_null() || out as isize == -1 {
            AttachConsole(ATTACH_PARENT_PROCESS);
        }
    }
}

/// URL invocations have their own argument shape. A quote in an OS-supplied link must never turn into a launcher
/// option (especially the console helper or selftest). The registered handlers pass exactly one link.
fn nxm_argument(args: &[String]) -> Result<Option<&str>, ()> {
    let is_url = |s: &str| s.get(..6).is_some_and(|p| p.eq_ignore_ascii_case("nxm://"));
    if !args.iter().skip(1).any(|a| a == "--nxm" || is_url(a)) { return Ok(None); }
    match args {
        [_, flag, url] if flag == "--nxm" && is_url(url) => Ok(Some(url)),
        [_, url] if is_url(url) => Ok(Some(url)),
        _ => Err(()),
    }
}

/// A kiln:// link (Crucible): `--link <url>` (what Kiln registers) or a bare `kiln://` argument, nothing else with it.
fn link_argument(args: &[String]) -> Result<Option<&str>, ()> {
    let is_url = |s: &str| s.get(..7).is_some_and(|p| p.eq_ignore_ascii_case("kiln://"));
    if !args.iter().skip(1).any(|a| a == "--link" || is_url(a)) { return Ok(None); }
    match args {
        [_, flag, url] if flag == "--link" && is_url(url) => Ok(Some(url)),
        [_, url] if is_url(url) => Ok(Some(url)),
        _ => Err(()),
    }
}

fn main() {
    // `<launcher> --kiln-ctrl-c <pid>`: the short-lived helper that sends a Windows server its Ctrl-C (Stop), so the
    // launcher itself never attaches to the server's console (kiln_manage::server::run::set_ctrl_c_helper)
    let args: Vec<String> = std::env::args().collect();
    let nxm = match nxm_argument(&args) {
        Ok(link) => link,
        Err(()) => std::process::exit(1),
    };
    let link = match link_argument(&args) {
        Ok(link) => link,
        Err(()) => std::process::exit(1),
    };
    if args.len() == 3 && args[1] == kiln_manage::server::run::CTRL_C_HELPER_ARG {
        let pid = args[2].parse::<u32>().unwrap_or(0);
        std::process::exit(if pid == 0 { 1 } else { kiln_manage::server::run::ctrl_c_helper_main(pid) });
    }
    if let Ok(exe) = std::env::current_exe() {
        kiln_manage::server::run::set_ctrl_c_helper(exe);
    }
    // `--nxm <link>` (what Kiln registers as the nxm:// handler) or a bare `nxm://` argument: the link goes to the
    // running launcher (or to another game's mod manager) and this process ends; else it starts with the link held for
    // its window (given to it once the data folder's gates open, lib.rs `after_snapshot`)
    if let Some(url) = nxm
        && !kiln_launcher_lib::nxm_entry(url)
    {
        std::process::exit(0);
    }
    // `--link <kiln://...>`: a Crucible link goes to the running launcher (this process ends) or waits for this one's
    // window, the same way
    if let Some(url) = link
        && !kiln_launcher_lib::link_entry(url)
    {
        std::process::exit(0);
    }
    if nxm.is_none() && link.is_none() && args.iter().any(|a| a == "--selftest") {
        #[cfg(windows)]
        attach_parent_console();
        std::process::exit(kiln_launcher_lib::selftest());
    }
    // Windows: the AppUserModelID of the installer's shortcuts, so the pinned shortcut and the window are one button
    #[cfg(windows)]
    kiln_launcher_lib::desktop::set_app_user_model_id();
    // the window's Wayland app id and X11 WM_CLASS: `kiln-launcher`, whatever the executable is called, so taskbars and
    // docks find the entry and its icon (desktop.rs)
    #[cfg(target_os = "linux")]
    kiln_launcher_lib::desktop::set_app_id();
    // How the window is drawn: `--software-render` / `--gpu-render` (here, after the nxm check above: a link start has
    // no other argument), `KILN_LAUNCHER_RENDER`, the marker; software mode's variables, the watchdog (render.rs). And
    // WebKit's DMA-BUF renderer (0.2.0-dev.29.19): on by default (compositing; up to 29.18 always off here, for "Gdk
    // Error 71 (Protocol error)" on GNOME + NVIDIA, which made large windows lag), with its start guard and the
    // watchdog's step without it before software mode; `--dmabuf` / `--no-dmabuf`, `KILN_LAUNCHER_DMABUF`, and the
    // user's own WEBKIT_DISABLE_DMABUF_RENDERER, which always wins (render.rs).
    #[cfg(target_os = "linux")]
    kiln_launcher_lib::render::prepare(&args);
    // WebKit's sandbox (`--no-webkit-sandbox` / `--webkit-sandbox`, KILN_LAUNCHER_WEBKIT_SANDBOX, the marker; its
    // variable before GTK and WebKit start), the probe of bwrap and, when WebKit would run a sandbox that cannot start,
    // the player's message (sandbox.rs, dialog.rs). Then the render watchdog.
    #[cfg(target_os = "linux")]
    kiln_launcher_lib::sandbox::prepare(&args);
    #[cfg(target_os = "linux")]
    kiln_launcher_lib::render::watch();
    kiln_launcher_lib::run()
}

#[cfg(test)]
mod tests {
    use super::*;
    fn args(tail: &[&str]) -> Vec<String> { std::iter::once("kiln").chain(tail.iter().copied()).map(str::to_string).collect() }
    #[test]
    fn nxm_invocations_cannot_inject_launcher_options() {
        let url = "nxm://enshrouded/mods/1/files/2";
        assert_eq!(nxm_argument(&args(&["--nxm", url])), Ok(Some(url)));
        assert_eq!(nxm_argument(&args(&[url])), Ok(Some(url)));
        assert_eq!(nxm_argument(&args(&["--selftest"])), Ok(None));
        for tail in [vec!["--nxm"], vec!["--nxm", "--selftest"], vec!["--nxm", url, "--kiln-ctrl-c", "1"], vec!["--kiln-ctrl-c", "1", url], vec![url, "--selftest"],
            vec!["--nxm", url, "--software-render"], vec!["--gpu-render", url], vec!["--nxm", url, "--no-webkit-sandbox"],
            vec!["--nxm", url, "--no-dmabuf"], vec!["--dmabuf", url], vec![url, "--no-dmabuf"]] {
            assert!(nxm_argument(&args(&tail)).is_err());
        }
    }

    #[test]
    fn link_invocations_cannot_inject_launcher_options() {
        let url = "kiln://crucible/H7QK-3MZP";
        assert_eq!(link_argument(&args(&["--link", url])), Ok(Some(url)));
        assert_eq!(link_argument(&args(&[url])), Ok(Some(url)));
        assert_eq!(link_argument(&args(&["--deck"])), Ok(None));
        for tail in [vec!["--link"], vec!["--link", "--selftest"], vec!["--link", url, "--selftest"], vec![url, "--software-render"],
            vec!["--kiln-ctrl-c", "1", url], vec!["--link", "nxm://enshrouded/mods/1/files/2"]] {
            assert!(link_argument(&args(&tail)).is_err(), "{tail:?}");
        }
    }
}
