// SPDX-License-Identifier: MIT
//! The fake DAP adapter suite of `kiln debug --dap` (chapter 12a §6, second bullet; plan 12a S6). The proxy
//! ([`super::proxy`], unchanged) runs as a process of its own, with its real stdin, stdout and signal handling, against
//! a target proven through the real helper protocol over a fake process table; netcoredbg is replaced by a fake adapter
//! process ([`fake_adapter`]) that logs everything it receives. A stand-in process plays the debuggee: its PID is the
//! target's Windows PID, and every case checks that it is still alive at the end and that the adapter ended by itself.
//!
//! `PASS debug.detach-policy` and `PASS debug.pause-watchdog` mean protocol behaviour only (what the proxy sends and
//! refuses, when it detaches and exits). Nothing here shows that a game survives a detach or a long pause: that is the
//! lab run on the Wine dedicated server and the guided client checks.
//!
//! How the processes come about: this test binary runs again in three roles, picked by a constructor before libtest's
//! `main` (the proxy has to run on the process's main thread, as in `kiln`, because signal masks are per thread):
//! - `KILN_DAP_SUITE=proxy`: [`proxy_child`], the proxy over a Planned built from the fake table;
//! - started by that proxy as its netcoredbg (`<this binary> --interpreter=vscode`): the fake adapter;
//! - `KILN_DAP_SUITE=debuggee`: [`debuggee`], which lives until its stdin closes.
//! The roles exist only in the test binary (`#[cfg(test)]`); the `kiln` binary has no switch or variable for them.

use super::*;
use kiln_manage::debug::resolver::fake::{FakeHost, FakeProc, FakeSource};
use std::io::Read;
use std::process::{Child, ChildStdin, ExitStatus};
use std::sync::atomic::{AtomicBool, Ordering};

#[path = "fake_adapter.rs"]
mod fake_adapter;

use fake_adapter::{MODULE, THREAD};

const ROLE: &str = "KILN_DAP_SUITE";
const SHA: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const NONCE: &str = "33333333333333333333333333333333";
const CREATED: u64 = 133_000;
/// The longest a case waits for one thing; a passing case waits far less.
const WAIT: Duration = Duration::from_secs(20);

// ---- the roles (child processes) ----

#[used]
#[unsafe(link_section = ".init_array")]
static ROLE_ENTRY: extern "C" fn(libc::c_int, *const *const libc::c_char, *const *const libc::c_char) = role_entry;

extern "C" fn role_entry(_: libc::c_int, _: *const *const libc::c_char, _: *const *const libc::c_char) {
    let Some(role) = std::env::var_os(ROLE) else { return };
    let args = cmdline();
    let code = std::panic::catch_unwind(move || {
        // as Rust's runtime does before main (and so in kiln): a closed pipe is a write error, not a death
        // SAFETY: setting a disposition; no handler code runs.
        unsafe { libc::signal(libc::SIGPIPE, libc::SIG_IGN) };
        if args.get(1).map(String::as_str) == Some("--interpreter=vscode") {
            fake_adapter::main()
        } else if role == "proxy" {
            proxy_child()
        } else if role == "debuggee" {
            debuggee()
        } else {
            eprintln!("{ROLE}: unknown role {role:?}");
            2
        }
    })
    .unwrap_or(101);
    std::process::exit(code);
}

fn cmdline() -> Vec<String> {
    std::fs::read("/proc/self/cmdline")
        .unwrap_or_default()
        .split(|b| *b == 0)
        .filter(|a| !a.is_empty())
        .map(|a| String::from_utf8_lossy(a).into_owned())
        .collect()
}

/// The suite's safety net: a child that is still running after this long ends itself. Its thread never takes
/// SIGINT/SIGTERM/SIGHUP, so what happens with those is the proxy's own doing.
fn exit_after(after: Duration, code: i32, what: &'static str) {
    std::thread::spawn(move || {
        block_client_signals();
        std::thread::sleep(after);
        eprintln!("{what}: still running after {} s; it ends itself (the suite's safety net)", after.as_secs());
        std::process::exit(code);
    });
}

/// SIGINT, SIGTERM and SIGHUP blocked on the calling thread only.
fn block_client_signals() {
    // SAFETY: a local signal set, applied to this thread only.
    unsafe {
        let mut set: libc::sigset_t = std::mem::zeroed();
        libc::sigemptyset(&mut set);
        for s in [libc::SIGINT, libc::SIGTERM, libc::SIGHUP] {
            libc::sigaddset(&mut set, s);
        }
        libc::pthread_sigmask(libc::SIG_BLOCK, &set, std::ptr::null_mut());
    }
}

/// The stand-in debuggee: alive until its stdin closes.
fn debuggee() -> i32 {
    exit_after(Duration::from_secs(300), 0, "debuggee");
    let mut b = [0u8; 64];
    let mut input = std::io::stdin();
    while matches!(input.read(&mut b), Ok(n) if n > 0) {}
    0
}

/// The proxy, as `kiln debug --server <dir> --dap` runs it on Linux, but with a fake target: a server folder with one
/// debug descriptor, its process in a fake table answering through the helper protocol, and this binary as netcoredbg.
/// As in kiln, the HelperSource's reader thread exists before the proxy starts and does not block any signal.
fn proxy_child() -> i32 {
    exit_after(Duration::from_secs(150), 98, "proxy");
    let root = PathBuf::from(std::env::var_os("KILN_DAP_ROOT").expect("KILN_DAP_ROOT"));
    let pid: u32 = std::env::var("KILN_DAP_DEBUGGEE").expect("KILN_DAP_DEBUGGEE").parse().expect("a PID");
    let max_pause_ms = std::env::var("KILN_DAP_MAX_PAUSE_MS").ok().map(|v| v.parse::<u64>().expect("milliseconds"));
    let install = root.join("srv");
    let exe = install.join(SERVER_EXE);
    let image = kiln_manage::winpath::unix_to_z(&exe).expect("an absolute path");
    let (process, descriptor) = FakeProc::kiln(pid, &image, CREATED, SHA, NONCE, TargetKind::Server);
    std::fs::write(install.join("kiln/state/debug").join(format!("{pid}.json")), serde_json::to_vec(&descriptor).unwrap()).unwrap();
    let mut host = FakeHost {
        spec: HostSpec { kind: TargetKind::Server, install_id: "install-fake".into(), state_dir: install.join("kiln/state"), install_exe: exe, prefix: None },
        facts: HostFacts { prefix_identity: None, runner_sha256: Some("r".repeat(64)), runner_context: None, host_pid: Some(4001) },
        calls: 0,
        later: None,
    };
    let (up_r, up_w) = std::io::pipe().expect("a pipe");
    let (down_r, down_w) = std::io::pipe().expect("a pipe");
    std::thread::spawn(move || {
        // in kiln the helper is another process: its stand-in thread here never takes the proxy's signals
        block_client_signals();
        let mut table = FakeSource::new(vec![process]);
        let _ = crate::debug_inspect::serve(BufReader::new(up_r), down_w, &mut table);
    });
    let mut source: Box<dyn ProcessSource + Send> = Box::new(crate::debug_inspect::HelperSource::over(Box::new(up_w), BufReader::new(down_r)));
    let resolved = match prepare(TargetKind::Server, false, &mut host, source.as_mut(), &mut resolver::new_challenge) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("proxy child: the fake target does not resolve: {} {}", e.code.as_str(), e.message);
            return 96;
        }
    };
    let target = Target {
        kind: TargetKind::Server,
        install,
        via: "fake",
        lab: None,
        wine: None,
        prefix: None,
        netcoredbg: std::env::current_exe().expect("this test binary"),
        resolved,
        max_pause_ms,
        allow_client_pause: false,
        runner_env: None,
    };
    match proxy(Planned { target, host: Box::new(host), source }) {
        Ok(code) => code,
        Err(e) => {
            eprintln!("proxy child: {e:#}");
            97
        }
    }
}

// ---- the editor's side (this test process) ----

enum Frame {
    Json(Value),
    Raw(String),
}

/// One proxy with its fake adapter and stand-in debuggee, driven as an editor would.
struct Session {
    name: &'static str,
    root: PathBuf,
    install: PathBuf,
    proxy: Child,
    input: Option<ChildStdin>,
    frames: mpsc::Receiver<(Instant, Frame)>,
    seen: Vec<(Instant, Frame)>,
    debuggee: Child,
    debuggee_in: Option<ChildStdin>,
    seq: i64,
    gc: Arc<AtomicBool>,
    status: Option<ExitStatus>,
}

impl Session {
    fn start(name: &'static str, mode: &str, max_pause_ms: Option<u64>) -> Session {
        let root = std::env::temp_dir().join(format!("kiln-dap-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let install = root.join("srv");
        std::fs::create_dir_all(install.join("kiln/state/debug")).unwrap();
        std::fs::write(install.join(SERVER_EXE), b"MZ").unwrap();
        let me = std::env::current_exe().unwrap();
        let mut debuggee = Command::new(&me).env(ROLE, "debuggee").stdin(Stdio::piped()).stdout(Stdio::null()).stderr(Stdio::null()).spawn().unwrap();
        let debuggee_in = debuggee.stdin.take();
        let gc = answer_gc(&install);
        let mut cmd = Command::new(&me);
        cmd.env(ROLE, "proxy")
            .env("KILN_DAP_ROOT", &root)
            .env("KILN_DAP_DEBUGGEE", debuggee.id().to_string())
            .env("KILN_DAP_FAKE_LOG", root.join("adapter.jsonl"))
            .env("KILN_DAP_FAKE_MODE", mode)
            .env_remove("KILN_DAP_MAX_PAUSE_MS");
        if let Some(ms) = max_pause_ms {
            cmd.env("KILN_DAP_MAX_PAUSE_MS", ms.to_string());
        }
        cmd.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(std::fs::File::create(root.join("proxy.stderr")).unwrap());
        let mut proxy = cmd.spawn().unwrap();
        let input = proxy.stdin.take();
        let out = proxy.stdout.take().unwrap();
        let (tx, frames) = mpsc::channel();
        std::thread::spawn(move || {
            let mut r = BufReader::new(out);
            while let Ok(Some(b)) = read_frame(&mut r) {
                let f = match serde_json::from_slice::<Value>(&b) {
                    Ok(v) => Frame::Json(v),
                    Err(_) => Frame::Raw(String::from_utf8_lossy(&b).into_owned()),
                };
                if tx.send((Instant::now(), f)).is_err() {
                    break;
                }
            }
        });
        Session { name, root, install, proxy, input, frames, seen: Vec::new(), debuggee, debuggee_in, seq: 0, gc, status: None }
    }

    fn pid(&self) -> u32 {
        self.debuggee.id()
    }

    fn send(&mut self, mut v: Value) -> i64 {
        self.seq += 1;
        v["seq"] = json!(self.seq);
        write_json(self.input.as_mut().expect("the proxy's input is open"), &v).expect("writing to the proxy");
        self.seq
    }

    fn request(&mut self, command: &str, arguments: Value) -> i64 {
        self.send(json!({ "type": "request", "command": command, "arguments": arguments }))
    }

    fn raw(&mut self, bytes: &[u8]) {
        let w = self.input.as_mut().expect("the proxy's input is open");
        w.write_all(bytes).unwrap();
        w.flush().unwrap();
    }

    fn close_input(&mut self) {
        self.input.take();
    }

    /// One more frame from the proxy into `seen`, waiting until `until` at most.
    fn next(&mut self, until: Instant) -> bool {
        match self.frames.recv_timeout(until.saturating_duration_since(Instant::now())) {
            Ok(f) => {
                self.seen.push(f);
                true
            }
            Err(_) => false,
        }
    }

    /// The first JSON frame from index `from` on that matches, waiting up to WAIT for it.
    fn find_from(&mut self, from: usize, what: &str, pred: impl Fn(&Value) -> bool) -> (usize, Value) {
        let until = Instant::now() + WAIT;
        let mut i = from;
        loop {
            while i < self.seen.len() {
                if let Frame::Json(v) = &self.seen[i].1
                    && pred(v)
                {
                    return (i, v.clone());
                }
                i += 1;
            }
            if !self.next(until) {
                panic!("{}: no {what} within {WAIT:?}\n{}", self.name, self.diag());
            }
        }
    }

    fn wait_for(&mut self, what: &str, pred: impl Fn(&Value) -> bool) -> Value {
        self.find_from(0, what, pred).1
    }

    fn response(&mut self, seq: i64) -> Value {
        self.wait_for(&format!("response to request {seq}"), |v| v["type"] == "response" && v["request_seq"] == seq)
    }

    /// Everything the proxy has sent so far, a little time given for frames on their way.
    fn drain(&mut self) {
        while self.next(Instant::now() + Duration::from_millis(200)) {}
    }

    fn json_seen(&self, from: usize) -> Vec<Value> {
        self.seen[from..].iter().filter_map(|(_, f)| if let Frame::Json(v) = f { Some(v.clone()) } else { None }).collect()
    }

    fn initialize_and_attach(&mut self) {
        let i = self.request("initialize", json!({ "clientID": "kiln-dap-suite", "adapterID": "coreclr" }));
        let r = self.response(i);
        assert_eq!(r["success"], true, "{}: initialize: {r}", self.name);
        let a = self.request("attach", json!({}));
        let r = self.response(a);
        assert_eq!(r["success"], true, "{}: attach: {r}\n{}", self.name, self.diag());
        self.wait_for("state running", |v| is_state(v, "running"));
    }

    fn wait_exit(&mut self, within: Duration) -> ExitStatus {
        let t0 = Instant::now();
        loop {
            if let Some(s) = self.proxy.try_wait().unwrap() {
                self.status = Some(s);
                return s;
            }
            if t0.elapsed() > within {
                panic!("{}: the proxy has not exited within {within:?}\n{}", self.name, self.diag());
            }
            std::thread::sleep(Duration::from_millis(20));
        }
    }

    fn adapter_log(&self) -> Vec<Value> {
        std::fs::read_to_string(self.root.join("adapter.jsonl"))
            .unwrap_or_default()
            .lines()
            .filter_map(|l| serde_json::from_str(l).ok())
            .collect()
    }

    /// The requests of this command the adapter received, as it received them.
    fn adapter_got(&self, command: &str) -> Vec<Value> {
        self.adapter_log().into_iter().filter(|e| e["ev"] == "recv" && e["msg"]["type"] == "request" && e["msg"]["command"] == command).map(|e| e["msg"].clone()).collect()
    }

    fn wait_adapter(&self, what: &str, within: Duration, pred: impl Fn(&[Value]) -> bool) -> Vec<Value> {
        let t0 = Instant::now();
        loop {
            let log = self.adapter_log();
            if pred(&log) {
                return log;
            }
            if t0.elapsed() > within {
                panic!("{}: the adapter's log shows no {what} within {within:?}\n{}", self.name, self.diag());
            }
            std::thread::sleep(Duration::from_millis(20));
        }
    }

    /// Exactly `n` disconnects reached the adapter, every one with terminateDebuggee and suspendDebuggee false and no
    /// restart.
    fn assert_safe_disconnects(&self, n: usize) -> Vec<Value> {
        let d = self.adapter_got("disconnect");
        assert_eq!(d.len(), n, "{}: disconnects that reached the adapter: {d:?}\n{}", self.name, self.diag());
        for m in &d {
            let a = &m["arguments"];
            assert_eq!(a["terminateDebuggee"], json!(false), "{}: {m}", self.name);
            assert_eq!(a["suspendDebuggee"], json!(false), "{}: {m}", self.name);
            assert!(a.get("restart").is_none(), "{}: {m}", self.name);
        }
        d
    }

    /// Index in the adapter's log of the first entry that matches.
    fn adapter_index(log: &[Value], pred: impl Fn(&Value) -> bool) -> Option<usize> {
        log.iter().position(pred)
    }

    fn session_file(&self) -> Value {
        let text = std::fs::read_to_string(kiln_manage::debug::session_path(&self.install)).unwrap_or_default();
        serde_json::from_str(&text).unwrap_or(Value::Null)
    }

    fn stderr(&self) -> String {
        std::fs::read_to_string(self.root.join("proxy.stderr")).unwrap_or_default()
    }

    fn diag(&self) -> String {
        let frames: Vec<String> = self
            .seen
            .iter()
            .map(|(_, f)| match f {
                Frame::Json(v) => v.to_string(),
                Frame::Raw(s) => format!("raw {s:?}"),
            })
            .collect();
        format!(
            "--- proxy stderr ---\n{}--- adapter log ---\n{}\n--- frames to the editor ---\n{}",
            self.stderr(),
            std::fs::read_to_string(self.root.join("adapter.jsonl")).unwrap_or_default(),
            frames.join("\n")
        )
    }

    /// Every case ends here: the proxy has exited, the adapter ended by itself (its log says how; a killed adapter
    /// writes nothing more), the collection ran before the attach, and the stand-in debuggee is still alive.
    fn end(mut self) {
        if self.status.is_none() {
            self.wait_exit(WAIT);
        }
        self.wait_adapter("own exit", WAIT, |l| l.iter().any(|e| e["ev"] == "exit"));
        assert!(self.gc.load(Ordering::SeqCst), "{}: no gc before the attach\n{}", self.name, self.diag());
        assert!(self.debuggee.try_wait().unwrap().is_none(), "{}: the debuggee is gone\n{}", self.name, self.diag());
        self.debuggee_in.take();
        let _ = self.debuggee.wait();
        let _ = std::fs::remove_dir_all(&self.root);
    }
}

impl Drop for Session {
    /// A failed case leaves nothing running: closed inputs end the proxy, the adapter and the debuggee by themselves.
    fn drop(&mut self) {
        self.input.take();
        self.debuggee_in.take();
    }
}

/// Answers the proxy's pre-attach `gc` through the control folder, as Kiln in the server does.
fn answer_gc(install: &Path) -> Arc<AtomicBool> {
    let done = Arc::new(AtomicBool::new(false));
    let dir = install.join(kiln_manage::control::CONTROL);
    let flag = done.clone();
    std::thread::spawn(move || {
        let t0 = Instant::now();
        while t0.elapsed() < Duration::from_secs(60) {
            if let Some(cmd) = kiln_manage::control::pending(&dir).into_iter().next() {
                let text = std::fs::read_to_string(&cmd).unwrap_or_default();
                let _ = std::fs::remove_file(&cmd);
                let tmp = cmd.with_extension("result.json.tmp");
                let body = json!({ "ok": true, "command": text.trim(), "message": "0 unloaded mod context(s) still alive" });
                std::fs::write(&tmp, body.to_string()).unwrap();
                std::fs::rename(&tmp, cmd.with_extension("result.json")).unwrap();
                flag.store(text.trim() == "gc", Ordering::SeqCst);
                return;
            }
            std::thread::sleep(Duration::from_millis(20));
        }
    });
    done
}

fn is_event(v: &Value, name: &str) -> bool {
    v["type"] == "event" && v["event"] == name
}

fn is_state(v: &Value, state: &str) -> bool {
    is_event(v, "kiln/debugState") && v["body"]["state"] == state
}

fn is_module_new(v: &Value) -> bool {
    is_event(v, "module") && v["body"]["reason"] == "new" && v["body"]["module"]["name"] == MODULE
}

fn output_has(v: &Value, text: &str) -> bool {
    is_event(v, "output") && v["body"]["output"].as_str().is_some_and(|o| o.contains(text))
}

/// A continue the proxy sent on its own (its sequence numbers start at OWN_SEQ).
fn own_continue(e: &Value) -> bool {
    e["ev"] == "recv" && e["msg"]["command"] == "continue" && e["msg"]["seq"].as_i64().is_some_and(|s| s >= OWN_SEQ)
}

fn alive(pid: u64) -> bool {
    let Ok(stat) = std::fs::read_to_string(format!("/proc/{pid}/stat")) else { return false };
    let state = stat.rsplit_once(')').map(|(_, rest)| rest.trim_start().chars().next()).unwrap_or_default();
    !matches!(state, Some('Z' | 'X') | None)
}

// ---- the cases ----

/// 1. A wrong attach PID never reaches the adapter; no PID, 0 or the right one become the target's Windows PID.
fn case_attach_pid() {
    let mut s = Session::start("attach-pid", "", None);
    let pid = s.pid();
    let i = s.request("initialize", json!({}));
    s.response(i);
    let wrong = u64::from(pid) + 100_000;
    let bad = s.request("attach", json!({ "processId": wrong }));
    let r = s.response(bad);
    assert_eq!(r["success"], false, "attach to {wrong}: {r}");
    assert!(r["message"].as_str().unwrap_or_default().contains("attaches only"), "{r}");
    let bad_text = s.request("attach", json!({ "processId": "${command:pickProcess}" }));
    assert_eq!(s.response(bad_text)["success"], false);
    for args in [json!({ "processId": 0 }), json!({}), json!({ "processId": pid })] {
        let a = s.request("attach", args.clone());
        let r = s.response(a);
        assert_eq!(r["success"], true, "attach {args}: {r}");
    }
    let attaches = s.adapter_got("attach");
    assert_eq!(attaches.len(), 3, "{attaches:?}");
    for m in &attaches {
        assert_eq!(m["arguments"]["processId"], json!(pid), "{m}");
        assert!(m["seq"] != bad && m["seq"] != bad_text, "a refused attach reached the adapter: {m}");
    }
    let d = s.request("disconnect", json!({}));
    s.response(d);
    assert_eq!(s.wait_exit(WAIT).code(), Some(0), "{}", s.diag());
    s.assert_safe_disconnects(1);
    s.end();
}

/// 2. launch, terminate, restart and terminateThreads are refused and never reach the adapter; the initialize
/// response stops offering them (and what disconnect would honour).
fn case_dangerous_requests() {
    let mut s = Session::start("dangerous", "", None);
    let i = s.request("initialize", json!({}));
    let r = s.response(i);
    for k in ["supportsTerminateRequest", "supportTerminateDebuggee", "supportSuspendDebuggee", "supportsRestartRequest", "supportsTerminateThreadsRequest"] {
        assert_eq!(r["body"][k], json!(false), "{k} still offered: {r}");
    }
    assert_eq!(r["body"]["supportsConfigurationDoneRequest"], json!(true), "an unrelated capability changed: {r}");
    let a = s.request("attach", json!({}));
    assert_eq!(s.response(a)["success"], true);
    let dangerous = ["launch", "terminate", "restart", "terminateThreads"];
    for c in dangerous {
        let q = s.request(c, json!({ "restart": true, "threadIds": [THREAD] }));
        let r = s.response(q);
        assert_eq!(r["success"], false, "{c} was not refused: {r}");
    }
    // the session goes on, and the adapter, which handles requests in order, saw none of them
    let t = s.request("threads", json!({}));
    assert_eq!(s.response(t)["success"], true);
    for c in dangerous {
        assert!(s.adapter_got(c).is_empty(), "{c} reached the adapter\n{}", s.diag());
    }
    let d = s.request("disconnect", json!({}));
    s.response(d);
    assert_eq!(s.wait_exit(WAIT).code(), Some(0), "{}", s.diag());
    s.assert_safe_disconnects(1);
    s.end();
}

/// 3. The client's disconnect reaches the adapter made safe, whatever it asked for.
fn case_disconnect_sanitized() {
    let mut s = Session::start("disconnect", "", None);
    s.initialize_and_attach();
    let d = s.request("disconnect", json!({ "terminateDebuggee": true, "suspendDebuggee": true, "restart": true }));
    let r = s.response(d);
    assert_eq!(r["success"], true, "{r}");
    assert_eq!(s.wait_exit(WAIT).code(), Some(0), "{}", s.diag());
    let got = s.assert_safe_disconnects(1);
    assert_eq!(got[0]["seq"], json!(d), "the client's own disconnect, made safe");
    let f = s.session_file();
    assert_eq!((f["state"]["state"].as_str(), f["state"]["reason"].as_str()), (Some("detached"), Some("client_disconnect")), "{f}");
    s.end();
}

/// 4a. The editor goes away (stdin closes) while attached: one safe disconnect of the proxy's own, then the adapter's
/// stdin closes and it ends by itself.
fn case_editor_eof() {
    let mut s = Session::start("eof", "", None);
    s.initialize_and_attach();
    s.close_input();
    assert_eq!(s.wait_exit(WAIT).code(), Some(0), "{}", s.diag());
    let got = s.assert_safe_disconnects(1);
    assert!(got[0]["seq"].as_i64().is_some_and(|q| q >= OWN_SEQ), "the proxy's own disconnect: {}", got[0]);
    let log = s.wait_adapter("end of input", WAIT, |l| l.iter().any(|e| e["ev"] == "eof"));
    let (d, eof) = (Session::adapter_index(&log, |e| e["msg"]["command"] == "disconnect"), Session::adapter_index(&log, |e| e["ev"] == "eof"));
    assert!(d < eof, "the disconnect came before the adapter's input closed: {log:?}");
    let f = s.session_file();
    assert_eq!((f["state"]["state"].as_str(), f["state"]["reason"].as_str()), (Some("detached"), Some("client_gone")), "{f}");
    s.end();
}

/// 4b. SIGTERM, SIGINT or SIGHUP to the proxy while attached: the same safe detach, and the proxy exits 0 (not by the
/// signal).
fn case_signal(sig: libc::c_int, name: &'static str) {
    let mut s = Session::start(name, "", None);
    s.initialize_and_attach();
    // SAFETY: a signal to the proxy this test started, by its exact PID.
    assert_eq!(unsafe { libc::kill(s.proxy.id() as libc::pid_t, sig) }, 0);
    let st = s.wait_exit(WAIT);
    assert_eq!(st.code(), Some(0), "{name}: the proxy did not detach and exit by itself ({st})\n{}", s.diag());
    let got = s.assert_safe_disconnects(1);
    assert!(got[0]["seq"].as_i64().is_some_and(|q| q >= OWN_SEQ), "{}", got[0]);
    s.wait_adapter("end of input", WAIT, |l| l.iter().any(|e| e["ev"] == "eof"));
    assert!(s.stderr().contains(&format!("{name}; detaching")), "{}", s.diag());
    s.close_input();
    s.end();
}

/// 5. An adapter that never answers the disconnect: the proxy waits its DISCONNECT_WAIT, closes the adapter's input,
/// exits, and leaves the adapter running (it ends later, by itself).
fn case_unresponsive_adapter() {
    let mut s = Session::start("unresponsive", "deaf,linger", None);
    s.initialize_and_attach();
    let t0 = Instant::now();
    s.close_input();
    let st = s.wait_exit(DISCONNECT_WAIT + Duration::from_secs(25));
    let took = t0.elapsed();
    assert_eq!(st.code(), Some(0), "{}", s.diag());
    assert!(took >= DISCONNECT_WAIT && took < DISCONNECT_WAIT + Duration::from_secs(15), "the proxy gave up after {took:?}");
    s.assert_safe_disconnects(1);
    assert!(s.stderr().contains("has not finished after a disconnect"), "{}", s.diag());
    let log = s.adapter_log();
    assert!(log.iter().any(|e| e["ev"] == "ignored"), "{log:?}");
    let adapter = log.iter().find(|e| e["ev"] == "start").and_then(|e| e["pid"].as_u64()).expect("the adapter's PID");
    assert!(alive(adapter), "the adapter did not outlive the proxy\n{}", s.diag());
    s.wait_adapter("exit after lingering", WAIT, |l| l.iter().any(|e| e["ev"] == "exit" && e["why"] == "linger"));
    s.end();
}

/// 7. A module that comes a second time (a reload under the session) is forwarded, with the reload notice.
fn case_reload() {
    let mut s = Session::start("reload", "", None);
    s.initialize_and_attach();
    let (first, _) = s.find_from(0, "the module event", is_module_new);
    let e = s.request("evaluate", json!({ "expression": "fake:reload" }));
    s.response(e);
    let (second, _) = s.find_from(first + 1, "the module event again", is_module_new);
    let notice = |v: &Value| output_has(v, &format!("{MODULE} was reloaded"));
    let (at, n) = s.find_from(0, "the reload notice", notice);
    assert!(at > first && at < second, "the notice belongs to the second load: {at} ({first}, {second})");
    assert_eq!(n["body"]["category"], "important", "{n}");
    s.drain();
    assert_eq!(s.json_seen(0).iter().filter(|v| notice(v)).count(), 1, "{}", s.diag());
    let d = s.request("disconnect", json!({}));
    s.response(d);
    assert_eq!(s.wait_exit(WAIT).code(), Some(0), "{}", s.diag());
    s.end();
}

/// 8a. Noise lines, a non-JSON body and an unreadable Content-Length from either side: the session goes on.
fn case_malformed_both_sides() {
    let mut s = Session::start("malformed", "", None);
    s.initialize_and_attach();
    s.raw(b"just a noise line\r\n");
    s.raw(b"X-Noise: 1\r\n\r\n");
    s.raw(b"Content-Length: 9\r\n\r\nnot json!");
    s.raw(b"Content-Length: abc\r\n\r\nnot a body either\r\n");
    let t = s.request("threads", json!({}));
    assert_eq!(s.response(t)["success"], true, "{}", s.diag());
    let g = s.request("evaluate", json!({ "expression": "fake:garbage" }));
    s.response(g);
    s.wait_for("the adapter's frame after its garbage", |v| output_has(v, "after-garbage"));
    assert!(s.seen.iter().any(|(_, f)| matches!(f, Frame::Raw(r) if r == "not json from adapter")), "the adapter's non-JSON body is passed on as it came\n{}", s.diag());
    let log = s.adapter_log();
    assert!(!log.iter().any(|e| e["ev"] == "recv_bad"), "the client's garbage reached the adapter: {log:?}");
    assert_eq!(s.adapter_got("threads").len(), 1);
    let d = s.request("disconnect", json!({}));
    s.response(d);
    assert_eq!(s.wait_exit(WAIT).code(), Some(0), "{}", s.diag());
    s.assert_safe_disconnects(1);
    s.end();
}

/// 8b. A client frame over the size limit ends the client's side: one safe disconnect, nothing killed.
fn case_client_frame_too_large() {
    let mut s = Session::start("client-huge", "", None);
    s.initialize_and_attach();
    s.raw(b"Content-Length: 999999999999\r\n\r\n");
    assert_eq!(s.wait_exit(WAIT).code(), Some(0), "{}", s.diag());
    s.assert_safe_disconnects(1);
    s.end();
}

/// 8c. An adapter frame over the size limit: the proxy treats the adapter as gone, tells the editor, closes the
/// adapter's input and exits; the adapter ends by itself.
fn case_adapter_frame_too_large() {
    let mut s = Session::start("adapter-huge", "", None);
    s.initialize_and_attach();
    s.request("evaluate", json!({ "expression": "fake:huge" }));
    assert_eq!(s.wait_exit(WAIT).code(), Some(1), "{}", s.diag());
    s.wait_for("terminated", |v| is_event(v, "terminated"));
    s.wait_adapter("end of input", WAIT, |l| l.iter().any(|e| e["ev"] == "exit" && e["why"] == "eof"));
    s.end();
}

/// 9. The adapter dies while attached: the editor is told (output, terminated), the session file says so, and the
/// debuggee is left alone.
fn case_adapter_dies() {
    let mut s = Session::start("adapter-dies", "", None);
    s.initialize_and_attach();
    s.request("evaluate", json!({ "expression": "fake:die" }));
    assert_eq!(s.wait_exit(WAIT).code(), Some(1), "{}", s.diag());
    s.wait_for("the notice", |v| output_has(v, "netcoredbg exited while attached; the server is still running"));
    s.wait_for("terminated", |v| is_event(v, "terminated"));
    let f = s.session_file();
    assert_eq!((f["state"]["state"].as_str(), f["state"]["reason"].as_str()), (Some("ended"), Some("adapter_exited")), "{f}");
    assert!(s.adapter_got("disconnect").is_empty());
    s.end();
}

/// Cases 1-5 and 7-9 at once (case 5 alone takes DISCONNECT_WAIT).
#[test]
fn detach_policy() {
    let cases: Vec<(&str, Box<dyn Fn() + Send + Sync>)> = vec![
        ("1 attach pid", Box::new(case_attach_pid)),
        ("2 dangerous requests", Box::new(case_dangerous_requests)),
        ("3 disconnect sanitized", Box::new(case_disconnect_sanitized)),
        ("4 editor eof", Box::new(case_editor_eof)),
        ("4 SIGTERM", Box::new(|| case_signal(libc::SIGTERM, "SIGTERM"))),
        ("4 SIGINT", Box::new(|| case_signal(libc::SIGINT, "SIGINT"))),
        ("4 SIGHUP", Box::new(|| case_signal(libc::SIGHUP, "SIGHUP"))),
        ("5 unresponsive adapter", Box::new(case_unresponsive_adapter)),
        ("7 reload", Box::new(case_reload)),
        ("8 malformed frames", Box::new(case_malformed_both_sides)),
        ("8 client frame too large", Box::new(case_client_frame_too_large)),
        ("8 adapter frame too large", Box::new(case_adapter_frame_too_large)),
        ("9 adapter dies", Box::new(case_adapter_dies)),
    ];
    let failed: Vec<&str> = std::thread::scope(|sc| {
        let running: Vec<_> = cases.iter().map(|(name, f)| (*name, sc.spawn(|| f()))).collect();
        running.into_iter().filter_map(|(name, h)| h.join().is_err().then_some(name)).collect()
    });
    assert!(failed.is_empty(), "failed cases: {failed:?} (their messages are above)");
    // Protocol behaviour only (a fake adapter and a fake process table), not game survival.
    println!("PASS debug.detach-policy (protocol behaviour only, fake adapter)");
}

/// 6. The pause watchdog: a stop held past maxPauseMs (an evaluation that never answers) is continued by the proxy
/// with its own seq; the editor gets `continued` and `kiln/debugState` running/timeout_continue. A resume before the
/// deadline cancels it.
#[test]
fn pause_watchdog() {
    const MS: u64 = 1000;
    let mut s = Session::start("watchdog", "", Some(MS));
    s.initialize_and_attach();
    let q = s.request("evaluate", json!({ "expression": "fake:stop" }));
    s.response(q);
    let (stop, _) = s.find_from(0, "stopped", |v| is_event(v, "stopped"));
    let stopped_at = s.seen[stop].0;
    s.find_from(stop, "state paused", |v| is_state(v, "paused"));
    let hold = s.request("evaluate", json!({ "expression": "fake:hold" }));
    let (cont, c) = s.find_from(stop, "the proxy's continued event", |v| is_event(v, "continued"));
    let waited = s.seen[cont].0 - stopped_at;
    assert!(waited >= Duration::from_millis(MS - 50) && waited < Duration::from_millis(MS + 3000), "continued after {waited:?}");
    assert_eq!(c["body"]["threadId"], json!(THREAD), "{c}");
    s.find_from(stop, "state running (timeout_continue)", |v| is_state(v, "running") && v["body"]["reason"] == "timeout_continue");
    let log = s.wait_adapter("the proxy's continue", WAIT, |l| l.iter().any(own_continue));
    let own: Vec<&Value> = log.iter().filter(|e| own_continue(e)).collect();
    assert_eq!(own.len(), 1, "{own:?}");
    assert_eq!(own[0]["msg"]["arguments"]["threadId"], json!(THREAD), "{}", own[0]);
    assert!(log.iter().any(|e| e["ev"] == "held" && e["seq"] == hold), "the evaluation was still pending: {log:?}");
    // a resume before the deadline: no continue from the proxy
    let q2 = s.request("evaluate", json!({ "expression": "fake:stop" }));
    s.response(q2);
    let (stop2, _) = s.find_from(cont + 1, "the second stop", |v| is_event(v, "stopped"));
    std::thread::sleep(Duration::from_millis(200));
    let resume = s.request("continue", json!({ "threadId": THREAD }));
    assert_eq!(s.response(resume)["success"], true);
    std::thread::sleep(Duration::from_millis(2 * MS + 500));
    s.drain();
    let late: Vec<Value> = s.json_seen(stop2).into_iter().filter(|v| is_event(v, "continued") || v["body"]["reason"] == "timeout_continue").collect();
    assert!(late.is_empty(), "the proxy continued a pause the client had ended: {late:?}");
    assert_eq!(s.adapter_log().iter().filter(|e| own_continue(e)).count(), 1, "{}", s.diag());
    assert!(s.json_seen(stop2).iter().any(|v| is_state(v, "running") && v["body"]["reason"].is_null()), "{}", s.diag());
    let d = s.request("disconnect", json!({}));
    s.response(d);
    assert_eq!(s.wait_exit(WAIT).code(), Some(0), "{}", s.diag());
    s.assert_safe_disconnects(1);
    s.end();
    // Protocol behaviour only (a fake adapter and a fake process table), not game survival.
    println!("PASS debug.pause-watchdog (protocol behaviour only, fake adapter)");
}
