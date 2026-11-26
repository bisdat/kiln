//! A window that never draws because the page's web process is stuck in the font library (Linux, 0.2.0-dev.31.14).
//!
//! A player (ZorinOS 18.1, kernel 7.0) saw only a black window, in the AppImage and the .deb alike: an eu-stack of the
//! stuck WebKitWebProcess showed one thread in fontconfig's FcPatternGetString (called from libwebkit2gtk-4.1) and the
//! rest in pthread_cond_timedwait; clearing the fontconfig cache fixed it, and GNOME Web had hung the same way. So, when
//! the page has not started at the render watchdog's late point (crate::render: [`crate::render::TIMEOUT`] after the
//! window, whatever the load: a renderer problem gets its restart as soon as before):
//! - **look** ([`probe`]): cheap /proc reads of each web process below the launcher (also behind bwrap), twice,
//!   [`PROBE_GAP`] apart: each thread's state, CPU time, page faults, voluntary context switches and read/write system calls; which
//!   pages of libfontconfig's code it touched in between (`clear_refs`, then smaps' Referenced); the font cache files it
//!   maps or holds open; wchan and the kernel stack for the log. No ptrace, no eu-stack, no signal;
//! - **decide** ([`decide`]): the specific hint only on evidence in fontconfig ([`Kind::Fonts`]); anything else is
//!   [`Kind::Unclear`] and the watchdog goes on as before (its restarts, then its last message, which lists the font
//!   cache step as one try: crate::sandbox::late_message);
//! - **the hint** ([`message`], crate::dialog::hint): native, never through the page. "Fix it and restart" moves the
//!   font cache aside ([`fix`]: renamed to a dated backup, never deleted), runs `fc-cache -r` and starts the launcher
//!   again; "Keep waiting" looks again after [`KEEP_WAITING`]; "Quit". A page that starts while it is up closes it.
//!   Once per chain of starts ([`FIXED_VAR`]): after the fix, the watchdog goes on as before.
//!
//! docs/kiln/launcher/app.md, "A window that can't draw: a broken font cache (Linux)".

use crate::sandbox::{Action, Message};
use std::ffi::OsStr;
use std::path::{Path, PathBuf};
use std::time::Duration;

/// The time between the probe's two looks at the web process.
pub const PROBE_GAP: Duration = Duration::from_secs(2);
/// A stuck thread ran at least this share of [`PROBE_GAP`] (on a box under load a runnable thread gets less, so the
/// specific hint stays away there).
pub const BUSY_SHARE: f64 = 0.5;
/// ... and has used at least this much CPU since it started: a long first layout is not a hang.
pub const STUCK_CPU: Duration = Duration::from_secs(8);
/// ... and faulted at most this many pages in during the gap: a font scan maps font files in (FreeType's Unix streams
/// are mmap'ed, so how many read calls a scan makes depends on the build) and faults by the thousand; a loop over a
/// mapped cache faults none.
pub const SCAN_FAULTS: u64 = 100;
/// A process "does nothing" when all its threads together ran at most this share of [`PROBE_GAP`].
pub const IDLE_SHARE: f64 = 0.1;
/// "Keep waiting": the next look this long after the player's click.
pub const KEEP_WAITING: Duration = Duration::from_secs(30);
/// `fc-cache -r` gets this long before the launcher starts again without it.
pub const FC_CACHE_WITHIN: Duration = Duration::from_secs(120);

pub const TITLE: &str = "Kiln Launcher: the window can't draw";
pub const FIX_LABEL: &str = "Fix it and restart";
pub const KEEP_LABEL: &str = "Keep waiting";

fn log(msg: &str) {
    crate::commands::app::launcher_log("fonts", msg);
}

// ---------------------------------------------------------------- the box's load

/// /proc/loadavg's one-minute average and the CPUs this process may use (for the `fonts:` line: on a busy box a stuck
/// thread gets less than [`BUSY_SHARE`] of the CPU, so rule A stays away).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Load {
    pub load1: Option<f64>,
    pub cpus: Option<usize>,
}

impl Load {
    pub fn says(&self) -> String {
        match (self.load1, self.cpus) {
            (Some(l), Some(n)) => format!("load {l:.2} on {n} CPUs"),
            (Some(l), None) => format!("load {l:.2}"),
            _ => "load not known".into(),
        }
    }
}

/// Now, from /proc.
pub fn load_now() -> Load {
    Load {
        load1: std::fs::read_to_string("/proc/loadavg").ok().as_deref().and_then(crate::webproc::parse_loadavg).map(|l| l[0]),
        cpus: std::thread::available_parallelism().ok().map(|n| n.get()),
    }
}

// ---------------------------------------------------------------- /proc as the kernel writes it

/// `/proc/<pid>/task/<tid>/stat`: the thread's name, its state letter, its CPU time (utime + stime, clock ticks) and its
/// page faults (minflt + majflt).
pub fn parse_task_stat(text: &str) -> Option<(String, char, u64, u64)> {
    let open = text.find('(')?;
    let close = text.rfind(')')?;
    let name = text.get(open + 1..close)?.to_string();
    let rest: Vec<&str> = text.get(close + 1..)?.split_whitespace().collect();
    let state = rest.first()?.chars().next()?;
    // fields 10, 12, 14 and 15 of proc(5); `rest` starts at field 3
    let minflt: u64 = rest.get(7)?.parse().ok()?;
    let majflt: u64 = rest.get(9)?.parse().ok()?;
    let utime: u64 = rest.get(11)?.parse().ok()?;
    let stime: u64 = rest.get(12)?.parse().ok()?;
    Some((name, state, utime + stime, minflt + majflt))
}

/// A `Key: 123` line of a /proc file (status, io).
fn number_of(text: &str, key: &str) -> Option<u64> {
    crate::webproc::kb_of(text, key)
}

/// One mapping of /proc/<pid>/maps or smaps (smaps: with its Referenced kB).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Mapping {
    pub perms: String,
    pub path: String,
    pub referenced_kb: Option<u64>,
}

/// /proc/<pid>/maps or /proc/<pid>/smaps.
pub fn parse_maps(text: &str) -> Vec<Mapping> {
    let mut out: Vec<Mapping> = Vec::new();
    for line in text.lines() {
        if let Some(m) = map_header(line) {
            out.push(m);
        } else if let (Some(last), Some(v)) = (out.last_mut(), line.strip_prefix("Referenced:")) {
            last.referenced_kb = v.split_whitespace().next().and_then(|n| n.parse().ok());
        }
    }
    out
}

/// `7f1c2a000000-7f1c2a02a000 r-xp 0000e000 103:02 1234      /usr/lib/libfontconfig.so.1.17.0`
fn map_header(line: &str) -> Option<Mapping> {
    let mut it = line.splitn(6, ' ');
    let (a, b) = it.next()?.split_once('-')?;
    let hex = |s: &str| !s.is_empty() && s.chars().all(|c| c.is_ascii_hexdigit());
    if !hex(a) || !hex(b) {
        return None;
    }
    let perms = it.next()?.to_string();
    let (_offset, _dev, _inode) = (it.next()?, it.next()?, it.next()?);
    Some(Mapping { perms, path: it.next().unwrap_or("").trim().to_string(), referenced_kb: None })
}

fn file_name(path: &str) -> &str {
    path.rsplit('/').next().unwrap_or(path)
}

/// fontconfig's library (any version, also the AppImage's or a flatpak runtime's).
pub fn is_fontconfig_lib(path: &str) -> bool {
    file_name(path).starts_with("libfontconfig.so")
}

/// A fontconfig cache file (`<md5>-le64.cache-9`) or one of its write's files (`.LCK`, `.NEW`, `.TMP-…`).
pub fn is_font_cache(path: &str) -> bool {
    let name = file_name(path);
    name.rfind(".cache-").is_some_and(|i| name[i + 7..].starts_with(|c: char| c.is_ascii_digit()))
}

// ---------------------------------------------------------------- one look at a web process

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Thread {
    pub tid: u32,
    pub name: String,
    pub state: char,
    /// utime + stime, clock ticks.
    pub ticks: u64,
    /// minflt + majflt.
    pub faults: u64,
    pub voluntary: Option<u64>,
    /// syscr + syscw (/proc/<pid>/task/<tid>/io; None: not readable).
    pub syscalls: Option<u64>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Snap {
    pub threads: Vec<Thread>,
    pub maps: Vec<Mapping>,
    /// Where its open files point.
    pub fds: Vec<String>,
    pub wchan: Option<String>,
    /// The first line of /proc/<pid>/stack (None: not readable, as for every user but root).
    pub stack: Option<String>,
}

/// Which map file a look reads.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Maps {
    No,
    Maps,
    Smaps,
}

/// One look at process `pid` under `root` (/proc, or a test's fake tree). None: it is gone.
pub fn read_snap(root: &Path, pid: u32, maps: Maps) -> Option<Snap> {
    let dir = root.join(pid.to_string());
    let read = |p: PathBuf| std::fs::read_to_string(p).ok();
    let mut threads = Vec::new();
    for e in std::fs::read_dir(dir.join("task")).ok()?.flatten() {
        let Some(tid) = e.file_name().to_str().and_then(|s| s.parse::<u32>().ok()) else { continue };
        let t = e.path();
        let Some((name, state, ticks, faults)) = read(t.join("stat")).as_deref().and_then(parse_task_stat) else { continue };
        let voluntary = read(t.join("status")).and_then(|s| number_of(&s, "voluntary_ctxt_switches"));
        let syscalls = read(t.join("io")).and_then(|s| Some(number_of(&s, "syscr")? + number_of(&s, "syscw")?));
        threads.push(Thread { tid, name, state, ticks, faults, voluntary, syscalls });
    }
    threads.sort_by_key(|t| t.tid);
    let maps = match maps {
        Maps::No => Vec::new(),
        Maps::Maps => read(dir.join("maps")).map(|m| parse_maps(&m)).unwrap_or_default(),
        Maps::Smaps => read(dir.join("smaps")).map(|m| parse_maps(&m)).unwrap_or_default(),
    };
    let mut fds: Vec<String> = std::fs::read_dir(dir.join("fd")).into_iter().flatten().flatten()
        .filter_map(|e| std::fs::read_link(e.path()).ok())
        .map(|p| p.to_string_lossy().into_owned())
        .collect();
    fds.sort();
    let wchan = read(dir.join("wchan")).map(|w| w.trim().to_string()).filter(|w| !w.is_empty());
    let stack = read(dir.join("stack")).and_then(|s| s.lines().next().map(|l| l.trim().to_string())).filter(|l| !l.is_empty());
    Some(Snap { threads, maps, fds, wchan, stack })
}

/// Clears the page references of `pid` (`/proc/<pid>/clear_refs`, "1"), so smaps' Referenced says what it touched since.
/// The kernel walks its page tables (the mm's read lock), clears the accessed bits and the pages' referenced flags (for
/// shared library pages the system's), and flushes its TLB: 12 to 16 ms for 1 GB resident. Its memory, data and
/// soft-dirty bits stay as they are; its pages only look unused to reclaim for a while. The file is mode 0200 and the
/// process's own: the same user, and not a process that made itself non-dumpable. False: not allowed or gone.
pub fn clear_refs(root: &Path, pid: u32) -> bool {
    use std::io::Write;
    std::fs::OpenOptions::new().write(true).open(root.join(pid.to_string()).join("clear_refs"))
        .and_then(|mut f| f.write_all(b"1")).is_ok()
}

// ---------------------------------------------------------------- the rules

/// What the specific hint is based on.
#[derive(Debug, Clone, PartialEq)]
pub enum Seen {
    /// Rule A: the process's main thread (where WebKit lays out text and asks fontconfig) busy without pause (running
    /// at both looks, at least [`BUSY_SHARE`] of the gap on the CPU, not one read or write system call, not one
    /// voluntary switch and at most [`SCAN_FAULTS`] page faults in between, [`STUCK_CPU`] in all), its other threads
    /// idle (together at most [`IDLE_SHARE`] of the gap: the player's were in pthread_cond_timedwait), and
    /// libfontconfig's code touched in that gap. The last alone proves little: smaps' Referenced also counts a page's
    /// shared flags, which other processes using the same library can set (an idle test process showed 32 kB), so the
    /// main thread's loop and the idle rest carry the rule.
    Busy { tid: u32, name: String, ran: Duration, gap: Duration, total: Duration },
    /// Rule B: the process holds a font cache file open at both looks and does nothing else (all threads together at
    /// most [`IDLE_SHARE`] of the gap).
    Waiting { file: String },
}

#[derive(Debug, Clone, PartialEq)]
pub enum Kind {
    Fonts(Seen),
    Unclear,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Verdict {
    pub pid: u32,
    pub kind: Kind,
    /// Everything the two looks saw, in one line (launcher.log).
    pub said: String,
}

impl Verdict {
    pub fn fonts(&self) -> bool {
        matches!(self.kind, Kind::Fonts(_))
    }
    /// A few words for the choice's line.
    pub fn short(&self) -> String {
        match &self.kind {
            Kind::Fonts(Seen::Busy { tid, name, .. }) => format!("web process {} stuck in fontconfig (thread {tid} {name})", self.pid),
            Kind::Fonts(Seen::Waiting { file }) => format!("web process {} waiting with a font cache file open ({file})", self.pid),
            Kind::Unclear => "the cause is not known".into(),
        }
    }
}

fn secs(ticks: u64, hz: u64) -> Duration {
    Duration::from_secs_f64(ticks as f64 / hz.max(1) as f64)
}

/// The two looks at web process `pid` ([`PROBE_GAP`] apart; `b` read smaps after `clear_refs` when `refs_cleared`):
/// the font cache hint, or not conclusive. `user_cache`: $XDG_CACHE_HOME/fontconfig, for the line.
pub fn decide(pid: u32, a: &Snap, b: &Snap, gap: Duration, hz: u64, refs_cleared: bool, user_cache: Option<&Path>) -> Verdict {
    let gap_s = gap.as_secs_f64().max(0.001);
    struct Step<'t> {
        t: &'t Thread,
        before: &'t Thread,
        ran: Duration,
    }
    let steps: Vec<Step> = b.threads.iter()
        .filter_map(|t| a.threads.iter().find(|x| x.tid == t.tid).map(|before| Step { t, before, ran: secs(t.ticks.saturating_sub(before.ticks), hz) }))
        .collect();
    let calls = |s: &Step| Some(s.t.syscalls?.saturating_sub(s.before.syscalls?));
    let switches = |s: &Step| Some(s.t.voluntary?.saturating_sub(s.before.voluntary?));
    let faults = |s: &Step| s.t.faults.saturating_sub(s.before.faults);
    let busy = |s: &Step| s.before.state == 'R' && s.t.state == 'R' && s.ran.as_secs_f64() >= BUSY_SHARE * gap_s
        && calls(s) == Some(0) && switches(s) == Some(0) && faults(s) <= SCAN_FAULTS && secs(s.t.ticks, hz) >= STUCK_CPU;
    // (fold from 0.0: an empty f64 sum is -0.0, "-0.00 s" in the line)
    let all_ran = steps.iter().map(|s| s.ran.as_secs_f64()).fold(0.0, |x, y| x + y);
    let others_ran = steps.iter().filter(|s| s.t.tid != pid).map(|s| s.ran.as_secs_f64()).fold(0.0, |x, y| x + y);
    let fc_mapped = b.maps.iter().any(|m| is_fontconfig_lib(&m.path));
    let fc_code = |m: &&Mapping| is_fontconfig_lib(&m.path) && m.perms.contains('x');
    let fc_touched: Option<u64> = (refs_cleared && fc_mapped)
        .then(|| b.maps.iter().filter(fc_code).filter_map(|m| m.referenced_kb).sum());
    let mut caches: Vec<&Mapping> = b.maps.iter().filter(|m| is_font_cache(&m.path)).collect();
    caches.dedup_by(|x, y| x.path == y.path);
    let caches_touched: u64 = b.maps.iter().filter(|m| is_font_cache(&m.path)).filter_map(|m| m.referenced_kb).sum();
    let in_user = |p: &str| user_cache.is_some_and(|u| Path::new(p).starts_with(u));
    let open: Vec<&String> = b.fds.iter().filter(|f| is_font_cache(f) && a.fds.contains(f)).collect();

    let spinner = steps.iter().find(|s| s.t.tid == pid && busy(s)).filter(|_| others_ran <= IDLE_SHARE * gap_s);
    let kind = match (spinner, fc_touched) {
        (Some(s), Some(kb)) if kb > 0 => Kind::Fonts(Seen::Busy {
            tid: s.t.tid, name: s.t.name.clone(), ran: s.ran, gap, total: secs(s.t.ticks, hz),
        }),
        _ if !open.is_empty() && all_ran <= IDLE_SHARE * gap_s => Kind::Fonts(Seen::Waiting { file: open[0].clone() }),
        _ => Kind::Unclear,
    };

    // the line: what the looks saw, whatever the verdict
    let sleeping = b.threads.iter().filter(|t| matches!(t.state, 'S' | 'D')).count();
    let thread = match spinner.or_else(|| steps.iter().find(|s| s.t.tid == pid)).or_else(|| steps.iter().max_by_key(|s| s.ran)) {
        Some(s) => format!("{} {} ({}) {}->{}, ran {:.2} s of {:.2} s, {} read/write system calls, {} voluntary switches, \
            {} page faults, {:.1} s of CPU in all", if s.t.tid == pid { "main thread" } else { "busiest thread" }, s.t.tid, s.t.name, s.before.state, s.t.state, s.ran.as_secs_f64(), gap_s,
            calls(s).map_or("? (io not readable)".into(), |n| n.to_string()), switches(s).map_or("?".into(), |n| n.to_string()),
            faults(s), secs(s.t.ticks, hz).as_secs_f64()),
        None => "no thread seen twice".into(),
    };
    let fc = match (fc_mapped, fc_touched) {
        (false, _) => "libfontconfig not mapped".to_string(),
        (true, Some(kb)) => format!("libfontconfig mapped, {kb} kB of its code touched in the gap"),
        (true, None) => "libfontconfig mapped (what it touched is not known: its page references could not be cleared)".into(),
    };
    let user = caches.iter().filter(|m| in_user(&m.path)).count();
    let cache_line = format!("font caches mapped: {} ({user} in {}){}", caches.len(),
        user_cache.map_or("the user's cache".into(), |u| u.display().to_string()),
        if refs_cleared { format!(", {caches_touched} kB touched") } else { String::new() });
    let open_line = if open.is_empty() { "none".to_string() } else { open.iter().map(|s| s.as_str()).collect::<Vec<_>>().join(", ") };
    let said = format!("web process {pid}: {} threads ({sleeping} sleeping, {all_ran:.2} s of CPU together in the gap, {others_ran:.2} s \
        outside the main thread); {thread}; {fc}; \
        {cache_line}; font cache files open: {open_line}; wchan {}; kernel stack {}",
        b.threads.len(), b.wchan.as_deref().unwrap_or("?"), b.stack.as_deref().unwrap_or("not readable"));
    Verdict { pid, kind, said }
}

/// Of the web processes' verdicts: one in fontconfig first, else the first. None: no web process was found.
pub fn pick(list: Vec<Verdict>) -> Option<Verdict> {
    let at = list.iter().position(Verdict::fonts).unwrap_or(0);
    list.into_iter().nth(at)
}

// ---------------------------------------------------------------- the user's font cache

/// fontconfig's user cache as fontconfig itself finds it (FcConfigXdgCacheHome): `$XDG_CACHE_HOME/fontconfig` when it
/// is set and not empty, a relative value from the current folder `cwd` (fontconfig takes it as it is, unlike the XDG
/// spec; the web process has the launcher's folder), else `$HOME/.cache/fontconfig`.
pub fn cache_dir(xdg_cache_home: Option<&OsStr>, home: Option<&OsStr>, cwd: Option<&Path>) -> Option<PathBuf> {
    let set = |v: Option<&OsStr>| v.filter(|v| !v.is_empty()).map(PathBuf::from);
    let from_cwd = |p: PathBuf| if p.is_absolute() { Some(p) } else { cwd.filter(|c| c.is_absolute()).map(|c| c.join(&p)) };
    match set(xdg_cache_home) {
        Some(x) => from_cwd(x),
        None => set(home).and_then(from_cwd).map(|h| h.join(".cache")),
    }
    .map(|c| c.join("fontconfig"))
}

/// This process's.
pub fn user_cache_dir() -> Option<PathBuf> {
    cache_dir(std::env::var_os("XDG_CACHE_HOME").as_deref(), std::env::var_os("HOME").as_deref(), std::env::current_dir().ok().as_deref())
}

/// `dir` as the player reads it: `~/.cache/fontconfig` under their home.
pub fn shown(dir: Option<&Path>, home: Option<&Path>) -> String {
    match (dir, home) {
        (Some(d), Some(h)) if h.is_absolute() && d.starts_with(h) && d != h => {
            format!("~/{}", d.strip_prefix(h).map(|r| r.display().to_string()).unwrap_or_default())
        }
        (Some(d), _) => d.display().to_string(),
        (None, _) => "~/.cache/fontconfig".into(),
    }
}

/// This process's cache folder, as shown.
pub fn shown_dir() -> String {
    shown(user_cache_dir().as_deref(), std::env::var_os("HOME").map(PathBuf::from).as_deref())
}

/// The backup's name next to `dir`: `fontconfig.broken-20261125-142139Z`, then `-2`, `-3` … when taken.
pub fn backup_path(dir: &Path, secs: i64) -> PathBuf {
    let name = dir.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| "fontconfig".into());
    let base = format!("{name}.broken-{}", kiln_manage::util::stamp(std::time::UNIX_EPOCH + Duration::from_secs(secs.max(0) as u64)));
    let at = |n: u32| dir.with_file_name(if n == 1 { base.clone() } else { format!("{base}-{n}") });
    (1..).map(at).find(|p| std::fs::symlink_metadata(p).is_err()).unwrap_or_else(|| at(1))
}

/// Moves `dir` aside (a rename in the same folder: nothing is deleted or copied). None: there was no such folder.
pub fn move_aside(dir: &Path, secs: i64) -> std::io::Result<Option<PathBuf>> {
    match std::fs::symlink_metadata(dir) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(e),
        Ok(_) => {}
    }
    let to = backup_path(dir, secs);
    std::fs::rename(dir, &to)?;
    Ok(Some(to))
}

/// How `fc-cache -r` went.
#[derive(Debug, Clone, PartialEq)]
pub enum FcCache {
    /// Not installed (fontconfig builds the cache again by itself at the next start, more slowly).
    NotFound,
    Done(Duration),
    Failed(String),
    /// Ended after [`FC_CACHE_WITHIN`].
    TimedOut,
}

/// Runs `cmd` (`fc-cache -r`, a program of the host) and waits up to `within`; a run past that is ended (our own child).
pub fn run_fc_cache(mut cmd: std::process::Command, within: Duration) -> FcCache {
    use std::process::Stdio;
    let begun = std::time::Instant::now();
    let mut child = match cmd.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).spawn() {
        Ok(c) => c,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return FcCache::NotFound,
        Err(e) => return FcCache::Failed(e.to_string()),
    };
    loop {
        match child.try_wait() {
            Ok(Some(s)) if s.success() => return FcCache::Done(begun.elapsed()),
            Ok(Some(s)) => return FcCache::Failed(format!("it ended with {s}")),
            Ok(None) if begun.elapsed() >= within => {
                let _ = child.kill();
                let _ = child.wait();
                return FcCache::TimedOut;
            }
            Ok(None) => std::thread::sleep(Duration::from_millis(100)),
            Err(e) => return FcCache::Failed(e.to_string()),
        }
    }
}

/// What the fix did.
#[derive(Debug, Clone, PartialEq)]
pub struct Fixed {
    pub dir: Option<PathBuf>,
    /// The backup (None: there was no font cache folder).
    pub moved: Result<Option<PathBuf>, String>,
    pub fc_cache: FcCache,
}

impl Fixed {
    pub fn says(&self) -> String {
        let dir = self.dir.as_ref().map_or("the font cache folder".into(), |d| d.display().to_string());
        let moved = match &self.moved {
            Ok(Some(to)) => format!("moved {dir} to {}", to.display()),
            Ok(None) => format!("there was no {dir}"),
            Err(e) => format!("could not move {dir} aside ({e})"),
        };
        let fc = match &self.fc_cache {
            FcCache::NotFound => "fc-cache is not installed (fontconfig builds the cache again at the next start)".to_string(),
            FcCache::Done(d) => format!("fc-cache -r done in {:.1} s", d.as_secs_f64()),
            FcCache::Failed(e) => format!("fc-cache -r failed ({e})"),
            FcCache::TimedOut => format!("fc-cache -r did not end within {} s and was stopped", FC_CACHE_WITHIN.as_secs()),
        };
        format!("{moved}; {fc}")
    }
}

/// The player's "Fix it and restart": `dir` (the user's cache, [`cache_dir`]) moved aside, then `fc_cache` run
/// (None: none is installed).
pub fn fix(dir: Option<&Path>, secs: i64, fc_cache: Option<std::process::Command>, within: Duration) -> Fixed {
    let moved = match dir {
        Some(d) => move_aside(d, secs).map_err(|e| e.to_string()),
        None => Err("the font cache folder is not known (no HOME, or a relative XDG_CACHE_HOME without a current folder)".into()),
    };
    let fc_cache = fc_cache.map_or(FcCache::NotFound, |c| run_fc_cache(c, within));
    Fixed { dir: dir.map(Path::to_path_buf), moved, fc_cache }
}

// ---------------------------------------------------------------- what the player reads

fn seen_text(v: &Verdict) -> String {
    match &v.kind {
        Kind::Fonts(Seen::Busy { name, ran, gap, total, .. }) => format!(
            "The launcher looked at its page's web process (pid {}): one thread ({name}) has been running in the font library \
            (fontconfig) without a pause, {:.1} s of the last {:.0} s without a single system call and {:.0} s of CPU since it \
            started, while the other threads wait and the page draws nothing.", v.pid, ran.as_secs_f64(), gap.as_secs_f64(), total.as_secs_f64()),
        Kind::Fonts(Seen::Waiting { file }) => format!(
            "The launcher looked at its page's web process (pid {}): it has a font cache file open ({file}) and does nothing \
            else, while the page draws nothing.", v.pid),
        Kind::Unclear => "The launcher could not see the cause.".into(),
    }
}

/// The font cache hint (only for [`Kind::Fonts`]): `dir` as [`shown`], `log` launcher.log's path.
pub fn message(v: &Verdict, dir: &str, log: &str) -> Message {
    Message {
        title: TITLE.into(),
        body: format!(
            "The window can't draw: your font cache looks broken. Run fc-cache -r, or remove {dir}, then restart the launcher.\n\n\
            {}\n\n\"{FIX_LABEL}\" moves {dir} aside (renamed with today's date and time; nothing is deleted), runs fc-cache -r \
            and starts the launcher again. If the window still can't draw after that, sudo fc-cache -r rebuilds the system's \
            font cache too. \"{KEEP_LABEL}\" looks again in {} s.\n\nlauncher.log has the details:\n{log}",
            seen_text(v), KEEP_WAITING.as_secs()),
        buttons: vec![(Action::FixFonts, FIX_LABEL), (Action::KeepWaiting, KEEP_LABEL), (Action::Quit, "Quit")],
    }
}

// ---------------------------------------------------------------- once per chain of starts

/// Set by the fix's restart and carried along by the watchdog's later restarts ([`keep`]): the font cache was rebuilt
/// in this chain of starts, so the hint is not shown again (a wrong verdict would otherwise offer the same fix forever
/// and never let the watchdog reach the renderer step that may help).
pub const FIXED_VAR: &str = "KILN_LAUNCHER_FONTS_FIXED";
static FIXED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();

/// crate::render::prepare, before any thread: whether this start came from the fix ([`FIXED_VAR`], which it removes
/// from the environment then, so a program the launcher starts never gets it).
pub fn seen_fixed(set: bool) {
    let _ = FIXED.set(set);
}

/// This start came after the fix, in this chain of starts.
pub fn fixed_before() -> bool {
    FIXED.get().copied().unwrap_or(false)
}

/// crate::render::Relaunch::keeping: a restart after the fix says so too.
pub fn keep(r: &mut crate::render::Relaunch, fixed: bool) {
    if fixed {
        r.add(FIXED_VAR, "1");
    }
}

/// The fix's restart: the same arguments and the watchdog's restart variables, and [`FIXED_VAR`].
pub fn fix_relaunch() -> crate::render::Relaunch {
    crate::render::Relaunch { set: vec![(FIXED_VAR, "1".into())], ..Default::default() }.keeping()
}

/// What a look leads to.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Step {
    /// No web process found: on as before.
    NoProcess,
    /// Not conclusive: on as before.
    Unclear,
    /// Evidence in fontconfig, but the fix already ran in this chain of starts: on as before.
    FixedBefore,
    /// The font cache hint.
    Hint,
}

pub fn step(found: Option<&Verdict>, fixed_before: bool) -> Step {
    match found {
        None => Step::NoProcess,
        Some(v) if !v.fonts() => Step::Unclear,
        Some(_) if fixed_before => Step::FixedBefore,
        Some(_) => Step::Hint,
    }
}

/// What the hint's outcome leads to.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum After {
    Started,
    Fix,
    Quit,
    /// "Keep waiting", or closed: another look after [`KEEP_WAITING`].
    LookAgain,
    /// Nobody can answer it (a notice without buttons): on as before, instead of a notice every [`KEEP_WAITING`].
    GoOn,
}

pub fn after(h: crate::dialog::Hint) -> After {
    use crate::dialog::Hint;
    match h {
        Hint::Started => After::Started,
        Hint::Chose(Action::FixFonts) => After::Fix,
        Hint::Chose(Action::Quit) => After::Quit,
        Hint::Notice => After::GoOn,
        Hint::Chose(_) | Hint::Closed => After::LookAgain,
    }
}

// ---------------------------------------------------------------- the live check (the watchdog's thread)

/// What [`check`] ended with.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Checked {
    /// The page started meanwhile.
    Started,
    /// The watchdog goes on as before.
    GoOn,
}

fn hz() -> u64 {
    // SAFETY: sysconf only reads a constant of the system.
    let n = unsafe { libc::sysconf(libc::_SC_CLK_TCK) };
    if n > 0 { n as u64 } else { 100 }
}

/// The two looks at every web process below the launcher, [`PROBE_GAP`] apart.
pub fn probe() -> Vec<Verdict> {
    let root = Path::new("/proc");
    let firsts: Vec<(u32, Snap, bool)> = crate::webproc::web_processes().into_iter()
        .filter_map(|(pid, _)| {
            let a = read_snap(root, pid, Maps::No)?;
            Some((pid, a, clear_refs(root, pid)))
        })
        .collect();
    if firsts.is_empty() {
        return Vec::new();
    }
    let begun = std::time::Instant::now();
    std::thread::sleep(PROBE_GAP);
    let gap = begun.elapsed();
    let (hz, user) = (hz(), user_cache_dir());
    firsts.into_iter()
        .filter_map(|(pid, a, cleared)| {
            let b = read_snap(root, pid, if cleared { Maps::Smaps } else { Maps::Maps })?;
            Some(decide(pid, &a, &b, gap, hz, cleared, user.as_deref()))
        })
        .collect()
}

/// Debug and QA builds (`dev-eval`) only: `KILN_LAUNCHER_TEST_FONT_HINT=fonts|unclear` takes the probe's place, so the
/// hint can be seen on a private Xvfb display (with `KILN_LAUNCHER_TEST_WITHHOLD_READY`).
#[cfg(any(debug_assertions, feature = "dev-eval"))]
fn test_verdict() -> Option<Option<Verdict>> {
    let v = std::env::var("KILN_LAUNCHER_TEST_FONT_HINT").ok()?;
    let kind = match v.as_str() {
        "fonts" => Kind::Fonts(Seen::Busy { tid: 1, name: "WebKitWebProces".into(), ran: PROBE_GAP, gap: PROBE_GAP, total: STUCK_CPU }),
        _ => Kind::Unclear,
    };
    Some(Some(Verdict { pid: 1, kind, said: format!("test: KILN_LAUNCHER_TEST_FONT_HINT={v}") }))
}
#[cfg(not(any(debug_assertions, feature = "dev-eval")))]
fn test_verdict() -> Option<Option<Verdict>> {
    None
}

/// The watchdog's late point (crate::render, the page not started, the window made; `what` says so): looks at the
/// web process, and on evidence in fontconfig shows the hint until the player chooses or the page starts. Returns only
/// when the page started or the watchdog should go on as before (not conclusive, or a restart that failed).
pub fn check(what: &str, started: &dyn Fn() -> bool) -> Checked {
    let mut look = 1;
    loop {
        let found = test_verdict().unwrap_or_else(|| pick(probe()));
        if started() {
            log(&format!("{what}; the page started during the check: no hint"));
            return Checked::Started;
        }
        let load = load_now().says();
        let v = match (step(found.as_ref(), fixed_before()), found) {
            (Step::NoProcess, _) | (_, None) => {
                log(&format!("{what}; no web process found below the launcher: not conclusive, going on as before"));
                return Checked::GoOn;
            }
            (Step::Unclear, Some(v)) => {
                log(&format!("{what}; {}; {load}; not conclusive: going on as before (the last message lists the font cache step)", v.said));
                return Checked::GoOn;
            }
            (Step::FixedBefore, Some(v)) => {
                log(&format!("{what}; {}; {load}; stuck in the font library again, but the font cache was rebuilt before this start \
                    ({FIXED_VAR}): no hint again, going on as before", v.said));
                return Checked::GoOn;
            }
            (Step::Hint, Some(v)) => v,
        };
        log(&format!("{what}; {}; {load}; stuck in the font library: the font cache hint (look {look})", v.said));
        let m = message(&v, &shown_dir(), &crate::dialog::log_path());
        let shown = crate::dialog::hint(&m, started);
        match after(shown) {
            After::Started => {
                log(&format!("{}; the page started while the hint was up: the hint closed by itself", v.short()));
                return Checked::Started;
            }
            After::Fix => {
                fix_and_restart(&v.short(), FIX_LABEL);
                return Checked::GoOn;
            }
            After::Quit => {
                log(&format!("{}; the player chose \"Quit\"", v.short()));
                crate::dialog::quit(false);
                return Checked::GoOn;
            }
            After::GoOn => {
                log(&format!("{}; the hint had no buttons (a notice, or nothing could show it): going on as before", v.short()));
                return Checked::GoOn;
            }
            After::LookAgain => {
                let how = if shown == crate::dialog::Hint::Chose(Action::KeepWaiting) { "chose \"Keep waiting\"" } else { "closed the hint" };
                log(&format!("{}; the player {how}: looking again in {} s", v.short(), KEEP_WAITING.as_secs()));
                let until = std::time::Instant::now() + KEEP_WAITING;
                while std::time::Instant::now() < until {
                    if started() {
                        log("the page started after the player chose to keep waiting");
                        return Checked::Started;
                    }
                    std::thread::sleep(Duration::from_millis(250));
                }
                look += 1;
            }
        }
    }
}

/// The fix, one line in launcher.log (what was seen, the player's choice, what the fix did), and a new start of the
/// launcher (exec: returns only when that failed).
pub fn fix_and_restart(seen: &str, label: &str) {
    let dir = user_cache_dir();
    let fixed = fix(dir.as_deref(), kiln_manage::util::now_unix(), Some(fc_cache_command()), FC_CACHE_WITHIN);
    log(&format!("{seen}; the player chose \"{label}\": {}; starting the launcher again", fixed.says()));
    let err = crate::render::restart(&fix_relaunch());
    log(&format!("PROBLEM: could not start again: {err}; start the launcher again yourself"));
    eprintln!("Kiln Launcher: the font cache was rebuilt ({}); could not start again: {err}. Start the launcher again.", fixed.says());
}

/// `fc-cache -r` as a program of the host (without the AppImage's environment: the host's fontconfig and caches).
fn fc_cache_command() -> std::process::Command {
    let mut c = kiln_manage::hostenv::command("fc-cache");
    c.arg("-r");
    c
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    const HZ: u64 = 100;
    const GAP: Duration = Duration::from_secs(2);

    fn stat(tid: u32, name: &str, state: char, ticks: u64) -> String {
        stat_faults(tid, name, state, ticks, FAULTS)
    }

    /// The page faults a fake thread has made (minflt, field 10; majflt, field 12, stays 0).
    const FAULTS: u64 = 2286;

    fn stat_faults(tid: u32, name: &str, state: char, ticks: u64, minflt: u64) -> String {
        // utime = ticks, stime = 0 (fields 14 and 15)
        format!("{tid} ({name}) {state} 4100 4100 4100 0 -1 4194560 {minflt} 0 0 0 {ticks} 0 0 0 20 0 18 0 180222222 1234 997 18446744073709551615")
    }

    /// (tid, name, state, ticks, voluntary switches, read/write system calls; None: io not readable)
    type T<'a> = (u32, &'a str, char, u64, u64, Option<u64>);

    /// A fake /proc/<pid> under `root`: its threads, smaps text, open files and wchan.
    fn fake(root: &Path, pid: u32, threads: &[T], smaps: &str, fds: &[&str], wchan: &str) {
        let dir = root.join(pid.to_string());
        let _ = fs::remove_dir_all(&dir);
        for &(tid, name, state, ticks, vol, sys) in threads {
            let t = dir.join("task").join(tid.to_string());
            fs::create_dir_all(&t).unwrap();
            fs::write(t.join("stat"), stat(tid, name, state, ticks)).unwrap();
            fs::write(t.join("status"), format!("Name:\t{name}\nState:\t{state}\nvoluntary_ctxt_switches:\t{vol}\nnonvoluntary_ctxt_switches:\t99\n")).unwrap();
            if let Some(n) = sys {
                fs::write(t.join("io"), format!("rchar: 7930\nwchar: 8\nsyscr: {n}\nsyscw: 1\nread_bytes: 0\n")).unwrap();
            }
        }
        fs::write(dir.join("smaps"), smaps).unwrap();
        fs::write(dir.join("maps"), smaps.lines().filter(|l| map_header(l).is_some()).collect::<Vec<_>>().join("\n")).unwrap();
        fs::write(dir.join("wchan"), wchan).unwrap();
        fs::write(dir.join("clear_refs"), "").unwrap();
        fs::create_dir_all(dir.join("fd")).unwrap();
        for (i, f) in fds.iter().enumerate() {
            std::os::unix::fs::symlink(f, dir.join("fd").join(i.to_string())).unwrap();
        }
    }

    fn smaps(fc_code_kb: u64, cache_kb: u64) -> String {
        format!("\
55d0c0a00000-55d0c0a01000 r--p 00000000 103:02 11 /usr/lib/x86_64-linux-gnu/webkit2gtk-4.1/WebKitWebProcess
Size:                  4 kB
Referenced:            0 kB
VmFlags: rd mr mw me
7f1c2a000000-7f1c2a00e000 r--p 00000000 103:02 1234                       /usr/lib/x86_64-linux-gnu/libfontconfig.so.1.12.0
Size:                 56 kB
Rss:                  56 kB
Referenced:            4 kB
7f1c2a00e000-7f1c2a038000 r-xp 0000e000 103:02 1234                       /usr/lib/x86_64-linux-gnu/libfontconfig.so.1.12.0
Size:                168 kB
Rss:                168 kB
Referenced:          {fc_code_kb} kB
Anonymous:             0 kB
VmFlags: rd ex mr mw me sd
7f1c30000000-7f1c30200000 r--p 00000000 103:02 777                        /home/player/.cache/fontconfig/3830d5c3ddfd5cd38a049b759396e72e-le64.cache-9
Size:               2048 kB
Referenced:          {cache_kb} kB
7f1c31000000-7f1c31100000 r--p 00000000 103:02 778                        /var/cache/fontconfig/7ef2298fde41cc6eeb7af42e48b7d293-le64.cache-9
Size:               1024 kB
Referenced:            0 kB
7ffd1c000000-7ffd1c021000 rw-p 00000000 00:00 0                          [stack]
Size:                132 kB
Referenced:          132 kB
")
    }

    const USER: &str = "/home/player/.cache/fontconfig";

    /// Two looks at a fake web process 4242: `a` then `b` (after the gap; refs cleared unless `cleared` is false).
    fn looks(a: &[T], b: &[T], fc_code_kb: u64, fds: (&[&str], &[&str]), cleared: bool) -> Verdict {
        let tmp = kiln_manage::library::TempDir::new(&std::env::temp_dir(), "fonthang-proc").unwrap();
        let root = tmp.path();
        fake(root, 4242, a, &smaps(0, 0), fds.0, "0");
        let first = read_snap(root, 4242, Maps::No).unwrap();
        assert!(clear_refs(root, 4242));
        fake(root, 4242, b, &smaps(fc_code_kb, 40), fds.1, "0");
        let second = read_snap(root, 4242, if cleared { Maps::Smaps } else { Maps::Maps }).unwrap();
        decide(4242, &first, &second, GAP, HZ, cleared, Some(Path::new(USER)))
    }

    // the player's process: the main thread spins in fontconfig, the rest wait (pthread_cond_timedwait)
    const WAITERS_A: [T; 2] = [(4250, "JSC Heap Collec", 'S', 12, 40, Some(3)), (4251, "WebCore: Worker", 'S', 3, 9, Some(1))];
    const WAITERS_B: [T; 2] = [(4250, "JSC Heap Collec", 'S', 12, 41, Some(3)), (4251, "WebCore: Worker", 'S', 3, 10, Some(1))];

    fn with<'a>(main: T<'a>, rest: &[T<'a>]) -> Vec<T<'a>> {
        std::iter::once(main).chain(rest.iter().copied()).collect()
    }

    /// [`looks`] (refs cleared, nothing open) with the main thread (4242) at `minflt` page faults at the second look.
    fn looks_faulting(a: &[T], b: &[T], fc_code_kb: u64, minflt: u64) -> Verdict {
        let tmp = kiln_manage::library::TempDir::new(&std::env::temp_dir(), "fonthang-proc").unwrap();
        let root = tmp.path();
        fake(root, 4242, a, &smaps(0, 0), &[], "0");
        let first = read_snap(root, 4242, Maps::No).unwrap();
        assert!(clear_refs(root, 4242));
        fake(root, 4242, b, &smaps(fc_code_kb, 40), &[], "0");
        let main = b.iter().find(|t| t.0 == 4242).unwrap();
        fs::write(root.join("4242/task/4242/stat"), stat_faults(main.0, main.1, main.2, main.3, minflt)).unwrap();
        let second = read_snap(root, 4242, Maps::Smaps).unwrap();
        decide(4242, &first, &second, GAP, HZ, true, Some(Path::new(USER)))
    }

    /// (review) A healthy font scan (a first start, new fonts, a fontconfig update's new cache version) with no read or
    /// write system call in the gap: FreeType's Unix streams map font files with mmap, so the read count of a scan
    /// depends on the build; its page faults do not. Measured on Arch (fontconfig 2.18.3, 2 s looks): python3 spinning
    /// in FcPatternGetString 0 faults (and a loop outside fontconfig 0); a loop of FcFreeTypeQuery over 710 fonts 39 000
    /// to 55 000 faults (and there about 10 000 read calls a second as well, so the system call rule held too).
    #[test]
    fn a_font_scan_that_faults_pages_in_is_not_a_hang() {
        let a = with((4242, "WebKitWebProces", 'R', 1400, 310, Some(5210)), &WAITERS_A);
        let b = with((4242, "WebKitWebProces", 'R', 1597, 310, Some(5210)), &WAITERS_B);
        let v = looks_faulting(&a, &b, 112, FAULTS + 39_357);
        assert_eq!(v.kind, Kind::Unclear, "{}", v.said);
        assert!(v.said.contains("39357 page faults"), "{}", v.said);
        // a few faults (the allocator, a JIT) do not hide the loop
        let v = looks_faulting(&a, &b, 12, FAULTS + 20);
        assert!(v.fonts(), "{}", v.said);
        assert!(v.said.contains("20 page faults"), "{}", v.said);
    }

    #[test]
    fn the_players_hang_is_stuck_in_fontconfig() {
        let a = with((4242, "WebKitWebProces", 'R', 1400, 310, Some(5210)), &WAITERS_A);
        let b = with((4242, "WebKitWebProces", 'R', 1597, 310, Some(5210)), &WAITERS_B);
        let v = looks(&a, &b, 12, (&[], &[]), true);
        let Kind::Fonts(Seen::Busy { tid, name, ran, total, .. }) = &v.kind else { panic!("{v:?}") };
        assert_eq!((*tid, name.as_str()), (4242, "WebKitWebProces"));
        assert!((ran.as_secs_f64() - 1.97).abs() < 0.001 && (total.as_secs_f64() - 15.97).abs() < 0.001);
        for part in ["web process 4242: 3 threads (2 sleeping", "0.00 s outside the main thread", "main thread 4242 (WebKitWebProces) R->R, ran 1.97 s of 2.00 s, \
            0 read/write system calls, 0 voluntary switches, 0 page faults, 16.0 s of CPU in all", "libfontconfig mapped, 12 kB of its code touched",
            "font caches mapped: 2 (1 in /home/player/.cache/fontconfig), 40 kB touched", "font cache files open: none", "wchan 0",
            "kernel stack not readable"] {
            assert!(v.said.contains(part), "{part:?} in {}", v.said);
        }
        assert!(!v.said.contains('\n') && !v.said.contains("-0.00"));
        assert_eq!(v.short(), "web process 4242 stuck in fontconfig (thread 4242 WebKitWebProces)");
    }

    /// The decision table: each row changes one thing of the player's case; none of them is the specific hint.
    #[test]
    fn anything_less_than_the_evidence_is_not_conclusive() {
        let main = |state_b: char, ticks_b: u64, vol_b: u64, sys_b: Option<u64>| {
            (with((4242, "WebKitWebProces", 'R', 1400, 310, Some(5210)), &WAITERS_A),
             with((4242, "WebKitWebProces", state_b, ticks_b, vol_b, sys_b), &WAITERS_B))
        };
        // (why, the two looks, kB of fontconfig's code touched, refs cleared)
        type Row<'a> = (&'a str, (Vec<T<'a>>, Vec<T<'a>>), u64, bool);
        let rows: [Row; 7] = [
            // busy in other code (a script's loop, layout): fontconfig's code untouched in the gap
            ("fontconfig untouched", main('R', 1597, 310, Some(5210)), 0, true),
            // fontconfig scanning fonts for a missing cache: it reads files (system calls), healthy
            ("system calls", main('R', 1590, 310, Some(5890)), 12, true),
            // it blocked (a voluntary switch): not a loop without end
            ("a voluntary switch", main('R', 1590, 311, Some(5210)), 12, true),
            // a busy box: runnable but on the CPU only 0.6 s of 2 s
            ("under load", main('R', 1460, 310, Some(5210)), 12, true),
            // asleep at the second look
            ("asleep", main('S', 1597, 310, Some(5210)), 12, true),
            // io not readable: a scan cannot be told from a loop
            ("io not readable", main('R', 1597, 310, None), 12, true),
            // what it touched is not known
            ("refs not cleared", main('R', 1597, 310, Some(5210)), 12, false),
        ];
        for (why, (a, b), fc_kb, cleared) in rows {
            let v = looks(&a, &b, fc_kb, (&[], &[]), cleared);
            assert_eq!(v.kind, Kind::Unclear, "{why}: {}", v.said);
        }
        // another thread spins (a GPU driver's busy wait on the compositor's thread), the main thread waits
        let a = with((4242, "WebKitWebProces", 'S', 300, 900, Some(5210)), &[(4260, "WebKitThreaded", 'R', 1400, 50, Some(80))]);
        let b = with((4242, "WebKitWebProces", 'S', 300, 900, Some(5210)), &[(4260, "WebKitThreaded", 'R', 1598, 50, Some(80))]);
        let v = looks(&a, &b, 12, (&[], &[]), true);
        assert_eq!(v.kind, Kind::Unclear, "not the main thread: {}", v.said);
        assert!(v.said.contains("main thread 4242 (WebKitWebProces) S->S") && v.said.contains("1.98 s outside the main thread"), "{}", v.said);
        // the main thread spins and the others work too (a busy, healthy start)
        let a = with((4242, "WebKitWebProces", 'R', 1400, 310, Some(5210)), &[(4260, "JSC Heap Collec", 'R', 50, 70, Some(3))]);
        let b = with((4242, "WebKitWebProces", 'R', 1597, 310, Some(5210)), &[(4260, "JSC Heap Collec", 'R', 120, 75, Some(3))]);
        assert_eq!(looks(&a, &b, 12, (&[], &[]), true).kind, Kind::Unclear, "the other threads work");
        // a young process (a long first layout): 3 s of CPU in all
        let a = with((4242, "WebKitWebProces", 'R', 100, 310, Some(5210)), &WAITERS_A);
        let b = with((4242, "WebKitWebProces", 'R', 297, 310, Some(5210)), &WAITERS_B);
        assert_eq!(looks(&a, &b, 12, (&[], &[]), true).kind, Kind::Unclear, "under STUCK_CPU");
        // a healthy idle page (all asleep, nothing open)
        let idle = with((4242, "WebKitWebProces", 'S', 300, 900, Some(5210)), &WAITERS_A);
        let idle_b = with((4242, "WebKitWebProces", 'S', 301, 930, Some(5260)), &WAITERS_B);
        let v = looks(&idle, &idle_b, 0, (&[], &[]), true);
        assert_eq!(v.kind, Kind::Unclear);
        assert!(v.said.contains("libfontconfig mapped, 0 kB"), "{}", v.said);
    }

    #[test]
    fn a_process_waiting_with_a_font_cache_file_open_is_the_hint_too() {
        let lock = "/home/player/.cache/fontconfig/3830d5c3ddfd5cd38a049b759396e72e-le64.cache-9.LCK";
        let fds: &[&str] = &["/dev/null", "socket:[123]", lock];
        let a = with((4242, "WebKitWebProces", 'D', 300, 900, Some(5210)), &WAITERS_A);
        let b = with((4242, "WebKitWebProces", 'D', 301, 900, Some(5210)), &WAITERS_B);
        let v = looks(&a, &b, 0, (fds, fds), true);
        assert_eq!(v.kind, Kind::Fonts(Seen::Waiting { file: lock.into() }), "{}", v.said);
        // open at one look only: not held
        assert_eq!(looks(&a, &b, 0, (fds, &["/dev/null"]), true).kind, Kind::Unclear);
        // open but the process works: not waiting on it
        let busy_b = with((4242, "WebKitWebProces", 'R', 450, 900, Some(5900)), &WAITERS_B);
        assert_eq!(looks(&a, &busy_b, 0, (fds, fds), true).kind, Kind::Unclear);
    }

    #[test]
    fn proc_files_are_read_as_the_kernel_writes_them() {
        assert_eq!(parse_task_stat(&stat(7, "a (b) c", 'R', 140)), Some(("a (b) c".into(), 'R', 140, FAULTS)));
        assert_eq!(parse_task_stat("454718 (bash) R 454716 454716 454716 0 -1 4194304 226 0 7 0 140 3 0 0 25 5 1 0 180222222"),
            Some(("bash".into(), 'R', 143, 233)));
        assert_eq!(parse_task_stat("1 (x) R 0 0"), None);
        let m = parse_maps(&smaps(12, 40));
        assert_eq!(m.len(), 6);
        assert_eq!(m[2], Mapping { perms: "r-xp".into(), path: "/usr/lib/x86_64-linux-gnu/libfontconfig.so.1.12.0".into(), referenced_kb: Some(12) });
        assert_eq!(m[5].path, "[stack]");
        // maps without smaps' fields, an anonymous mapping, a deleted file
        let m = parse_maps("7f00-7f10 rw-p 00000000 00:00 0 \n7f20-7f30 r-xp 00000000 08:01 9 /tmp/libx.so (deleted)\n");
        assert_eq!((m[0].path.as_str(), m[0].referenced_kb), ("", None));
        assert_eq!(m[1].path, "/tmp/libx.so (deleted)");
        assert!(is_fontconfig_lib("/usr/lib/libfontconfig.so.1.17.0") && is_fontconfig_lib("/tmp/.mount_Kiln/usr/lib/libfontconfig.so.1"));
        assert!(!is_fontconfig_lib("/usr/lib/libfontconfig-extra.so") && !is_fontconfig_lib("/usr/lib/libfreetype.so.6"));
        for p in ["/home/p/.cache/fontconfig/x-le64.cache-9", "/var/cache/fontconfig/y-le64.cache-7", "/home/p/.cache/fontconfig/x-le64.cache-9.LCK",
            "/home/p/.cache/fontconfig/x-le64.cache-9.TMP-a1b2c3"] {
            assert!(is_font_cache(p), "{p}");
        }
        for p in ["/home/p/.cache/fontconfig/CACHEDIR.TAG", "/home/p/.cache/mesa_shader.cache-x", "/usr/lib/libfontconfig.so.1"] {
            assert!(!is_font_cache(p), "{p}");
        }
        assert_eq!(pick(Vec::new()), None);
    }

    #[test]
    fn the_load_goes_into_the_line() {
        assert_eq!(Load { load1: Some(54.1), cpus: Some(16) }.says(), "load 54.10 on 16 CPUs");
        assert_eq!(Load { load1: Some(1.5), cpus: None }.says(), "load 1.50");
        assert_eq!(Load { load1: None, cpus: Some(4) }.says(), "load not known");
    }

    /// (review) The folder fontconfig itself uses: it takes a relative XDG_CACHE_HOME from the current folder (checked
    /// with fontconfig 2.18.3: `XDG_CACHE_HOME=relcache fc-cache -r` wrote `<cwd>/relcache/fontconfig`), so moving
    /// `$HOME/.cache/fontconfig` then would move a folder the web process does not use.
    #[test]
    fn the_cache_folder_is_the_one_fontconfig_uses() {
        let os = |s: &str| std::ffi::OsString::from(s);
        let cwd = Some(Path::new("/run/cwd"));
        assert_eq!(cache_dir(Some(&os("/x/cache")), Some(&os("/home/p")), cwd), Some(PathBuf::from("/x/cache/fontconfig")));
        // empty: unset
        assert_eq!(cache_dir(Some(&os("")), Some(&os("/home/p")), cwd), Some(PathBuf::from("/home/p/.cache/fontconfig")));
        assert_eq!(cache_dir(None, Some(&os("/home/p")), cwd), Some(PathBuf::from("/home/p/.cache/fontconfig")));
        // relative: from the current folder, as fontconfig reads it
        assert_eq!(cache_dir(Some(&os("rel/cache")), Some(&os("/home/p")), cwd), Some(PathBuf::from("/run/cwd/rel/cache/fontconfig")));
        // not known where that is: nothing is moved
        assert_eq!(cache_dir(Some(&os("rel/cache")), Some(&os("/home/p")), None), None);
        assert_eq!(cache_dir(None, None, cwd), None);
        let home = Path::new("/home/p");
        assert_eq!(shown(Some(Path::new("/home/p/.cache/fontconfig")), Some(home)), "~/.cache/fontconfig");
        assert_eq!(shown(Some(Path::new("/x/cache/fontconfig")), Some(home)), "/x/cache/fontconfig");
        assert_eq!(shown(None, Some(home)), "~/.cache/fontconfig");
    }

    /// The fix against a temp HOME only (never this PC's own cache), with a stand-in for fc-cache run through sh.
    #[test]
    fn the_fix_moves_the_cache_aside_and_runs_fc_cache() {
        let tmp = kiln_manage::library::TempDir::new(&std::env::temp_dir(), "fonthang-home").unwrap();
        let home = tmp.path();
        let dir = cache_dir(None, Some(home.as_os_str()), None).unwrap();
        assert!(dir.starts_with(home));
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("3830d5c3ddfd5cd38a049b759396e72e-le64.cache-9"), b"broken").unwrap();
        let ran = home.join("fc-cache-ran");
        let script = home.join("fc-cache.sh");
        fs::write(&script, "printf '%s ' \"$@\" > \"$RAN\"\n").unwrap();
        let fake = || {
            let mut c = std::process::Command::new("sh");
            c.arg(&script).arg("-r").env("RAN", &ran);
            c
        };
        let secs = 1_795_616_499; // 2026-11-25 14:21:39 UTC
        let f = fix(Some(&dir), secs, Some(fake()), Duration::from_secs(20));
        let backup = home.join(".cache/fontconfig.broken-20261125-142139Z");
        assert_eq!(f.moved, Ok(Some(backup.clone())), "{f:?}");
        assert!(matches!(f.fc_cache, FcCache::Done(_)), "{f:?}");
        assert!(!dir.exists(), "moved, not copied");
        assert_eq!(fs::read(backup.join("3830d5c3ddfd5cd38a049b759396e72e-le64.cache-9")).unwrap(), b"broken", "nothing deleted");
        assert_eq!(fs::read_to_string(&ran).unwrap(), "-r ");
        assert!(f.says().starts_with(&format!("moved {} to {}; fc-cache -r done in ", dir.display(), backup.display())), "{}", f.says());
        // the same second again: a second backup, the first stays
        fs::create_dir_all(&dir).unwrap();
        let f = fix(Some(&dir), secs, None, Duration::from_secs(1));
        assert_eq!(f.moved, Ok(Some(home.join(".cache/fontconfig.broken-20261125-142139Z-2"))));
        assert_eq!(f.fc_cache, FcCache::NotFound);
        assert!(backup.exists());
        // no folder: nothing to move, fc-cache still runs
        let f = fix(Some(&dir), secs, Some(fake()), Duration::from_secs(20));
        assert_eq!(f.moved, Ok(None));
        assert!(f.says().starts_with(&format!("there was no {}", dir.display())), "{}", f.says());
        // no HOME and no XDG_CACHE_HOME
        assert!(fix(None, secs, None, Duration::from_secs(1)).moved.is_err());
        // a symlinked cache folder: the link is renamed, its target (elsewhere) is left as it is
        let target = home.join("elsewhere/fc");
        fs::create_dir_all(&target).unwrap();
        fs::write(target.join("x-le64.cache-9"), b"broken").unwrap();
        std::os::unix::fs::symlink(&target, &dir).unwrap();
        let f = fix(Some(&dir), secs + 60, None, Duration::from_secs(1));
        let link = home.join(".cache/fontconfig.broken-20261125-142239Z");
        assert_eq!(f.moved, Ok(Some(link.clone())), "{f:?}");
        assert_eq!(fs::read_link(&link).unwrap(), target);
        assert!(fs::symlink_metadata(&dir).is_err() && target.join("x-le64.cache-9").is_file());
    }

    #[test]
    fn fc_cache_that_fails_or_hangs_is_said() {
        assert_eq!(run_fc_cache(std::process::Command::new("/nonexistent/fc-cache"), Duration::from_secs(1)), FcCache::NotFound);
        let mut fail = std::process::Command::new("sh");
        fail.args(["-c", "exit 3"]);
        assert!(matches!(run_fc_cache(fail, Duration::from_secs(20)), FcCache::Failed(e) if e.contains('3')));
        let mut hang = std::process::Command::new("sh");
        hang.args(["-c", "exec sleep 30"]);
        let begun = std::time::Instant::now();
        assert_eq!(run_fc_cache(hang, Duration::from_millis(300)), FcCache::TimedOut);
        assert!(begun.elapsed() < Duration::from_secs(10), "ended, not waited for");
    }

    /// The rules on real processes of this test's own (`cargo test -p kiln-launcher --lib fonthang -- --ignored`, about
    /// 12 s): python3 spinning in fontconfig's FcPatternGetString (its main thread) is the hint; a shell's loop, and a
    /// python3 asleep with libfontconfig loaded, are not.
    #[test]
    #[ignore]
    fn live_processes_follow_the_rules() {
        let spin = "import ctypes\nfc = ctypes.CDLL('libfontconfig.so.1')\nfc.FcPatternCreate.restype = ctypes.c_void_p\n\
            p = fc.FcPatternCreate()\ns = ctypes.c_char_p()\nget = fc.FcPatternGetString\n\
            get.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_char_p)]\n\
            while True: get(p, b'family', 0, ctypes.byref(s))\n";
        let idle = "import ctypes, time\nctypes.CDLL('libfontconfig.so.1')\ntime.sleep(60)\n";
        let start = |prog: &str, args: &[&str]| std::process::Command::new(prog).args(args).spawn().unwrap();
        let mut kids = [start("python3", &["-I", "-c", spin]), start("bash", &["-c", "while :; do :; done"]), start("python3", &["-I", "-c", idle])];
        // past STUCK_CPU for the spinners
        std::thread::sleep(STUCK_CPU + Duration::from_secs(1));
        let root = Path::new("/proc");
        let firsts: Vec<(u32, Snap, bool)> = kids.iter().map(|k| (k.id(), read_snap(root, k.id(), Maps::No).unwrap(), clear_refs(root, k.id()))).collect();
        let begun = std::time::Instant::now();
        std::thread::sleep(PROBE_GAP);
        let gap = begun.elapsed();
        let v: Vec<Verdict> = firsts.iter().map(|(pid, a, cleared)| {
            let b = read_snap(root, *pid, if *cleared { Maps::Smaps } else { Maps::Maps }).unwrap();
            decide(*pid, a, &b, gap, hz(), *cleared, user_cache_dir().as_deref())
        }).collect();
        for k in &mut kids {
            let _ = k.kill();
            let _ = k.wait();
        }
        for x in &v {
            eprintln!("live: {:?} | {}", x.kind, x.said);
        }
        assert!(v[0].fonts(), "spinning in fontconfig: {}", v[0].said);
        assert_eq!(v[1].kind, Kind::Unclear, "a shell's loop: {}", v[1].said);
        assert_eq!(v[2].kind, Kind::Unclear, "asleep: {}", v[2].said);
        assert_eq!(pick(v).map(|x| x.fonts()), Some(true));
    }

    /// (review) After "Fix it and restart" the new start, and every restart of the watchdog after it, carries
    /// [`FIXED_VAR`]; such a start never shows the hint again, so a wrong verdict (another loop on the main thread, a
    /// broken system cache) cannot offer the same fix forever and keep the watchdog from its renderer steps.
    #[test]
    fn the_hint_comes_once_per_chain_of_starts() {
        let fonts = Verdict { pid: 1, kind: Kind::Fonts(Seen::Waiting { file: "/x/a-le64.cache-9".into() }), said: String::new() };
        let unclear = Verdict { pid: 1, kind: Kind::Unclear, said: String::new() };
        assert_eq!(step(Some(&fonts), false), Step::Hint);
        assert_eq!(step(Some(&fonts), true), Step::FixedBefore);
        assert_eq!(step(Some(&unclear), false), Step::Unclear);
        assert_eq!(step(Some(&unclear), true), Step::Unclear);
        assert_eq!(step(None, false), Step::NoProcess);
        assert!(fix_relaunch().set.contains(&(FIXED_VAR, "1".to_string())));
        // the watchdog's restarts after the fix keep it; before it, they do not set it
        let mut r = crate::render::Relaunch::dmabuf_fallback();
        keep(&mut r, true);
        assert!(r.set.contains(&(FIXED_VAR, "1".to_string())), "{r:?}");
        let mut r = crate::render::Relaunch::dmabuf_fallback();
        keep(&mut r, false);
        assert!(!r.set.iter().any(|(k, _)| *k == FIXED_VAR), "{r:?}");
    }

    /// (review) What each outcome of the hint leads to: a notice without buttons (notify-send, when the window's main
    /// loop does not answer and there is no zenity or kdialog) goes on as before, instead of a critical notice every
    /// 30 s for as long as the window can't draw.
    #[test]
    fn a_hint_nobody_can_answer_is_not_sent_again() {
        use crate::dialog::Hint as H;
        assert_eq!(after(H::Notice), After::GoOn);
        assert_eq!(after(H::Closed), After::LookAgain);
        assert_eq!(after(H::Chose(Action::KeepWaiting)), After::LookAgain);
        assert_eq!(after(H::Chose(Action::FixFonts)), After::Fix);
        assert_eq!(after(H::Chose(Action::Quit)), After::Quit);
        assert_eq!(after(H::Started), After::Started);
    }

    #[test]
    fn the_hint_says_what_it_saw_and_offers_the_fix() {
        let v = Verdict { pid: 4242, kind: Kind::Fonts(Seen::Busy { tid: 4242, name: "WebKitWebProces".into(), ran: Duration::from_millis(1970),
            gap: GAP, total: Duration::from_secs(16) }), said: String::new() };
        let m = message(&v, "~/.cache/fontconfig", "/home/p/.local/share/kiln/logs/launcher.log");
        assert_eq!(m.title, "Kiln Launcher: the window can't draw");
        assert!(m.body.starts_with("The window can't draw: your font cache looks broken. Run fc-cache -r, or remove ~/.cache/fontconfig, \
            then restart the launcher.\n\n"), "{}", m.body);
        for part in ["web process (pid 4242)", "(WebKitWebProces)", "2.0 s of the last 2 s", "16 s of CPU", "nothing is deleted",
            "sudo fc-cache -r", "looks again in 30 s", "/home/p/.local/share/kiln/logs/launcher.log"] {
            assert!(m.body.contains(part), "{part:?} in {}", m.body);
        }
        assert_eq!(m.buttons, vec![(Action::FixFonts, "Fix it and restart"), (Action::KeepWaiting, "Keep waiting"), (Action::Quit, "Quit")]);
        let w = Verdict { pid: 7, kind: Kind::Fonts(Seen::Waiting { file: "/x/a-le64.cache-9".into() }), said: String::new() };
        assert!(message(&w, "/x", "l").body.contains("it has a font cache file open (/x/a-le64.cache-9)"));
    }
}
