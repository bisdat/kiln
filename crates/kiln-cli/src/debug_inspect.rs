// SPDX-License-Identifier: MIT
//! `kiln debug-inspect` (hidden): the Windows side of a debug target for a kiln that cannot see the target's Windows
//! processes itself (chapter 12a §4 step 4, §5). The Windows build of kiln runs it under the target's own runner and
//! prefix (Wine, Proton), so the PID it is given means the same process the debugger will attach to. It serves
//! kiln_manage::debug's `ProcessSource` (in-process `NativeSource`) as helper protocol v1 on stdin/stdout:
//!
//! ```text
//! -> {"v":1,"op":"inspect"|"prepare_attach"|"watch_exit"|"close","challenge":"<fresh random>",
//!     "target":{"windowsPid","image","creationTime100ns","imageSha256","sessionNonce","pipe"}}
//! <- {"v":1,"ok":true,"op":..,"challenge":<echoed>,"elevated":bool|null,"exited":bool,"seen":{Seen}|null}
//! <- {"v":1,"ok":false,"op":..,"challenge":<echoed>|null,"error":{"code","message","reason"?}}
//! ```
//! One JSON object per line, at most 16 KiB a line; one answer per request; an unknown op runs nothing. The helper
//! checks what it saw against the target before it answers (`check_seen`) and holds the process handle from `inspect`
//! to `close` (or end of input). `close` releases the handle and exits; nothing here ends any process.
//!
//! [`HelperSource`] is the other end: the host's `ProcessSource` that starts the helper and speaks to it, with the
//! first answer due within 15 s (the runner starts the helper) and every later one within 5 s.

#[cfg(not(windows))]
use crate::out::CliError;
use anyhow::Result;
use kiln_manage::debug::resolver::{Probe, ProcessSource, Seen, Watch, check_seen};
use kiln_manage::debug::{DebugCode, DebugError};
use serde_json::{Value, json};
use std::io::{BufRead, Write};
use std::sync::mpsc;
use std::time::Duration;

const MAX_LINE: usize = kiln_manage::debug::identity::MAX_FRAME;
/// The answer to every request but the first is due within this.
const ANSWER: Duration = Duration::from_secs(5);
/// The first answer includes starting the helper under its runner.
const FIRST_ANSWER: Duration = Duration::from_secs(15);

#[derive(clap::Args)]
pub struct InspectArgs {
    /// Check the helper this kiln would start (tools/debug-inspect next to it, else in the Kiln data folder) against its
    /// manifest and print where it is: the release check of an SDK folder.
    #[arg(long)]
    verify_bundle: bool,
}

pub fn run(a: InspectArgs, json: bool) -> Result<i32> {
    if a.verify_bundle {
        let helper = find_helper().map_err(crate::debug::cli_error)?;
        if json {
            println!("{}", json!({ "ok": true, "helper": helper }));
        } else {
            println!("{}", helper.display());
        }
        return Ok(0);
    }
    #[cfg(windows)]
    {
        let mut src = kiln_manage::debug::windows::NativeSource::default();
        let stdin = std::io::stdin();
        serve(stdin.lock(), std::io::stdout(), &mut src)?;
        Ok(0)
    }
    #[cfg(not(windows))]
    Err(CliError::new("unsupported", "kiln debug-inspect is the Windows side of kiln debug: it runs on Windows, or under Wine with the Windows build of kiln").into())
}

/// Reads one line of at most MAX_LINE bytes. None at end of input; Err(()) for a longer line.
fn read_line<R: BufRead>(r: &mut R) -> std::io::Result<Option<std::result::Result<String, ()>>> {
    let mut buf = Vec::new();
    loop {
        let chunk = r.fill_buf()?;
        if chunk.is_empty() {
            return Ok(if buf.is_empty() { None } else { Some(Ok(String::from_utf8_lossy(&buf).into_owned())) });
        }
        if let Some(i) = chunk.iter().position(|&b| b == b'\n') {
            buf.extend_from_slice(&chunk[..i]);
            r.consume(i + 1);
            if buf.len() > MAX_LINE {
                return Ok(Some(Err(())));
            }
            if buf.last() == Some(&b'\r') {
                buf.pop();
            }
            return Ok(Some(Ok(String::from_utf8_lossy(&buf).into_owned())));
        }
        let n = chunk.len();
        buf.extend_from_slice(chunk);
        r.consume(n);
        if buf.len() > MAX_LINE {
            return Ok(Some(Err(())));
        }
    }
}

#[cfg_attr(not(windows), allow(dead_code))] // the helper serves on Windows; tests drive it everywhere
fn good_challenge(c: &str) -> bool {
    (8..=128).contains(&c.len()) && c.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}

#[cfg_attr(not(windows), allow(dead_code))] // the helper serves on Windows; tests drive it everywhere
fn answer_err(op: &Value, challenge: &Value, e: &DebugError) -> Value {
    json!({ "v": 1, "ok": false, "op": op, "challenge": challenge, "error": e })
}

/// The helper's loop: one answer per request line, until `close` or the end of input (both release the handle).
#[cfg_attr(not(windows), allow(dead_code))] // the helper serves on Windows; tests drive it everywhere
fn send<W: Write>(w: &mut W, v: &Value) -> std::io::Result<()> {
    writeln!(w, "{v}")?;
    w.flush()
}

#[cfg_attr(not(windows), allow(dead_code))] // the helper serves on Windows; tests drive it everywhere
pub fn serve<R: BufRead, W: Write>(mut r: R, mut w: W, src: &mut dyn ProcessSource) -> std::io::Result<()> {
    loop {
        let line = match read_line(&mut r)? {
            None => break,
            Some(Err(())) => {
                let e = DebugError::new(DebugCode::Refused, format!("a request line is at most {MAX_LINE} bytes")).because("frame_too_large");
                send(&mut w, &answer_err(&Value::Null, &Value::Null, &e))?;
                break;
            }
            Some(Ok(l)) => l,
        };
        if line.trim().is_empty() {
            continue;
        }
        let bad = |why: &str| DebugError::new(DebugCode::Refused, why.to_string()).because("bad_request");
        let Ok(req) = serde_json::from_str::<Value>(&line) else {
            send(&mut w, &answer_err(&Value::Null, &Value::Null, &bad("a request is one JSON object on one line")))?;
            continue;
        };
        let op = req["op"].clone();
        if req["v"] != 1 {
            send(&mut w, &answer_err(&op, &Value::Null, &bad("protocol version 1 only (\"v\": 1)")))?;
            continue;
        }
        let challenge = match req["challenge"].as_str() {
            Some(c) if good_challenge(c) => c.to_string(),
            _ => {
                send(&mut w, &answer_err(&op, &Value::Null, &bad("\"challenge\" is 8 to 128 characters of [0-9A-Za-z_-]")))?;
                continue;
            }
        };
        let echo = json!(challenge);
        let opname = op.as_str().unwrap_or_default();
        if opname == "close" {
            src.close();
            send(&mut w, &json!({ "v": 1, "ok": true, "op": "close", "challenge": echo, "elevated": Value::Null, "exited": false, "seen": Value::Null }))?;
            return Ok(());
        }
        if !matches!(opname, "inspect" | "prepare_attach" | "watch_exit") {
            send(&mut w, &answer_err(&op, &echo, &bad("the operations are inspect, prepare_attach, watch_exit and close")))?;
            continue;
        }
        let mut target = req["target"].clone();
        if !target.is_object() {
            send(&mut w, &answer_err(&op, &echo, &bad("no \"target\"")))?;
            continue;
        }
        target["challenge"] = echo.clone();
        let probe: Probe = match serde_json::from_value(target) {
            Ok(p) => p,
            Err(e) => {
                send(&mut w, &answer_err(&op, &echo, &bad(&format!("\"target\": {e}"))))?;
                continue;
            }
        };
        let got: std::result::Result<(bool, Option<Seen>), DebugError> = match opname {
            "inspect" => src.inspect(&probe).map(|s| (false, Some(s))),
            "prepare_attach" => src.prepare_attach(&probe).map(|s| (false, Some(s))),
            _ => src.watch(&probe).map(|x| match x {
                Watch::Exited => (true, None),
                Watch::Alive(s) => (false, Some(s)),
            }),
        };
        // refuse before answering: what was seen must be the target, every field of it
        let got = got.and_then(|(exited, seen)| match &seen {
            Some(s) => check_seen(&probe, s).map(|()| (exited, seen)),
            None => Ok((exited, seen)),
        });
        let v = match got {
            Ok((exited, seen)) => json!({ "v": 1, "ok": true, "op": op, "challenge": echo, "elevated": src.elevated(), "exited": exited, "seen": seen }),
            Err(e) => answer_err(&op, &echo, &e),
        };
        send(&mut w, &v)?;
    }
    src.close();
    Ok(())
}

// ---- the host's end ----

/// `<bundle>/tools/debug-inspect/manifest.json`, written by the release beside the helper: which file, and its SHA-256.
#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct Manifest {
    format: u32,
    helper: String,
    sha256: String,
    #[allow(dead_code)]
    kiln: Option<String>,
}

/// The helper of this kiln's bundle (the folder this kiln runs from): `tools/debug-inspect/win-x64/kiln.exe`, started
/// only when its SHA-256 is the one `tools/debug-inspect/manifest.json` names. Missing or mismatched: tool_missing,
/// never another program (chapter 12a §4 step 4).
/// The helper next to this kiln (`<kiln dir>/tools/debug-inspect`, how the SDK folder ships it), else in the Kiln data
/// folder (`<data>/tools/debug-inspect`, where the release's kiln-debug-inspect zip unpacks for a kiln downloaded on its
/// own). A helper next to kiln whose hash does not match is refused, never passed over for another.
pub fn find_helper() -> std::result::Result<std::path::PathBuf, DebugError> {
    let exe = std::env::current_exe().map_err(|e| DebugError::new(DebugCode::ToolMissing, format!("cannot tell where this kiln runs from: {e}")).because("helper_missing"))?;
    let root = exe.canonicalize().unwrap_or(exe);
    let data = kiln_manage::paths::DataDirs::locate().ok().map(|d| d.root);
    find_helper_from(root.parent().unwrap_or(std::path::Path::new(".")), data.as_deref())
}

pub fn find_helper_from(beside: &std::path::Path, data: Option<&std::path::Path>) -> std::result::Result<std::path::PathBuf, DebugError> {
    let first = match find_helper_in(beside) {
        Ok(h) => return Ok(h),
        Err(e) if !beside.join("tools/debug-inspect/manifest.json").exists() => e,
        Err(e) => return Err(e),
    };
    match data {
        Some(d) if d.join("tools/debug-inspect/manifest.json").exists() => find_helper_in(d),
        Some(d) => Err(DebugError::new(
            DebugCode::ToolMissing,
            format!("{} (nor in the Kiln data folder, {}): use the kiln of Kiln's SDK folder, or unpack the release's kiln-debug-inspect zip into the data folder", first.message, d.join("tools/debug-inspect").display()),
        )
        .because("helper_missing")),
        None => Err(first),
    }
}

pub fn find_helper_in(root: &std::path::Path) -> std::result::Result<std::path::PathBuf, DebugError> {
    let dir = root.join("tools/debug-inspect");
    let missing = |why: String| DebugError::new(DebugCode::ToolMissing, why).because("helper_missing");
    let mpath = dir.join("manifest.json");
    let text = match std::fs::metadata(&mpath) {
        Ok(m) if m.len() <= MAX_LINE as u64 => std::fs::read(&mpath).map_err(|e| missing(format!("cannot read {}: {e}", mpath.display())))?,
        Ok(_) => return Err(missing(format!("{} is too large to be the helper's manifest", mpath.display()))),
        Err(_) => return Err(missing(format!("this kiln has no debug-inspect helper ({} is missing); it ships next to kiln in the SDK folder of a Kiln release", mpath.display()))),
    };
    let m: Manifest = serde_json::from_slice(&text).map_err(|e| missing(format!("{} is not the helper's manifest: {e}", mpath.display())))?;
    if m.format != 1 || m.helper != "win-x64/kiln.exe" || m.sha256.len() != 64 || !m.sha256.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(missing(format!("{} does not name win-x64/kiln.exe with a SHA-256", mpath.display())));
    }
    let helper = dir.join("win-x64").join("kiln.exe");
    if !helper.is_file() {
        return Err(missing(format!("the debug-inspect helper {} is missing", helper.display())));
    }
    let (got, _) = kiln_manage::hash::sha256_file(&helper).map_err(|e| missing(format!("cannot read {}: {}", helper.display(), e.message)))?;
    if !got.eq_ignore_ascii_case(&m.sha256) {
        return Err(DebugError::new(DebugCode::ToolMissing, format!("the debug-inspect helper {} is not the one this kiln shipped with (its SHA-256 differs from the manifest's); reinstall Kiln", helper.display())).because("helper_hash"));
    }
    Ok(helper)
}

/// The helper as the host's Windows side. Started under the target's runner; every answer is checked for the
/// challenge it must echo. Dropping or closing it ends the helper by closing its input, never by a kill.
pub struct HelperSource {
    to: Option<Box<dyn Write + Send>>,
    from: mpsc::Receiver<String>,
    child: Option<std::process::Child>,
    first: bool,
    broken: Option<DebugError>,
    elevated: Option<bool>,
}

impl HelperSource {
    /// Starts `cmd` (the helper under its runner: `wine "<helper>" debug-inspect`) with piped stdin/stdout.
    pub fn spawn(mut cmd: std::process::Command) -> std::result::Result<HelperSource, DebugError> {
        cmd.stdin(std::process::Stdio::piped()).stdout(std::process::Stdio::piped()).stderr(std::process::Stdio::null());
        let mut child = cmd.spawn().map_err(|e| DebugError::new(DebugCode::ToolMissing, format!("cannot start the debug-inspect helper: {e}")).because("helper_start"))?;
        let stdin = child.stdin.take().expect("piped");
        let stdout = child.stdout.take().expect("piped");
        let mut s = HelperSource::over(Box::new(stdin), std::io::BufReader::new(stdout));
        s.child = Some(child);
        Ok(s)
    }

    /// Over any transport (the tests run [`serve`] on the other end).
    pub fn over<R: BufRead + Send + 'static>(to: Box<dyn Write + Send>, mut from: R) -> HelperSource {
        let (tx, rx) = mpsc::channel();
        std::thread::spawn(move || {
            while let Ok(Some(Ok(line))) = read_line(&mut from) {
                if tx.send(line).is_err() {
                    break;
                }
            }
        });
        HelperSource { to: Some(to), from: rx, child: None, first: true, broken: None, elevated: None }
    }

    fn request(&mut self, op: &str, probe: &Probe) -> std::result::Result<Value, DebugError> {
        if let Some(e) = &self.broken {
            return Err(e.clone());
        }
        let mut target = serde_json::to_value(probe).expect("plain data");
        target.as_object_mut().expect("an object").remove("challenge");
        let line = json!({ "v": 1, "op": op, "challenge": probe.challenge, "target": target }).to_string();
        let gone = |what: String| DebugError::new(DebugCode::Timeout, what).because("helper_no_answer");
        let sent = self.to.as_mut().map(|w| writeln!(w, "{line}").and_then(|()| w.flush()));
        if !matches!(sent, Some(Ok(()))) {
            let e = gone("the debug-inspect helper is not running".into());
            self.broken = Some(e.clone());
            return Err(e);
        }
        let wait = if std::mem::take(&mut self.first) { FIRST_ANSWER } else { ANSWER };
        let answer = match self.from.recv_timeout(wait) {
            Ok(a) => a,
            Err(_) => {
                let e = gone(format!("the debug-inspect helper did not answer {op} within {} s", wait.as_secs()));
                self.broken = Some(e.clone());
                return Err(e);
            }
        };
        let v: Value = serde_json::from_str(&answer).map_err(|_| gone("the debug-inspect helper answered something that is not its protocol".into()))?;
        if v["challenge"] != json!(probe.challenge) {
            let e = DebugError::identity_changed("the debug-inspect helper's answer does not echo this request's challenge", "helper_challenge");
            self.broken = Some(e.clone());
            return Err(e);
        }
        if v["ok"] != true {
            return Err(serde_json::from_value::<DebugError>(v["error"].clone())
                .unwrap_or_else(|_| DebugError::new(DebugCode::UnsupportedIdentity, format!("the debug-inspect helper refused {op}")).because("helper_refused")));
        }
        if let Some(e) = v["elevated"].as_bool() {
            self.elevated = Some(e);
        }
        Ok(v)
    }

    fn seen(v: &Value) -> std::result::Result<Seen, DebugError> {
        serde_json::from_value(v["seen"].clone()).map_err(|e| DebugError::new(DebugCode::UnsupportedIdentity, format!("the debug-inspect helper's answer lacks what it saw ({e})")).because("helper_answer"))
    }
}

impl ProcessSource for HelperSource {
    fn elevated(&mut self) -> Option<bool> {
        self.elevated
    }
    fn inspect(&mut self, probe: &Probe) -> std::result::Result<Seen, DebugError> {
        let v = self.request("inspect", probe)?;
        HelperSource::seen(&v)
    }
    fn prepare_attach(&mut self, probe: &Probe) -> std::result::Result<Seen, DebugError> {
        let v = self.request("prepare_attach", probe)?;
        HelperSource::seen(&v)
    }
    fn watch(&mut self, probe: &Probe) -> std::result::Result<Watch, DebugError> {
        let v = self.request("watch_exit", probe)?;
        if v["exited"] == true {
            return Ok(Watch::Exited);
        }
        HelperSource::seen(&v).map(Watch::Alive)
    }
    fn close(&mut self) {
        if self.broken.is_none()
            && let Some(w) = self.to.as_mut()
        {
            let c = json!({ "v": 1, "op": "close", "challenge": "close-request" }).to_string();
            if writeln!(w, "{c}").and_then(|()| w.flush()).is_ok() {
                let _ = self.from.recv_timeout(Duration::from_secs(2));
            }
        }
        // its input closes: the helper exits on its own (it is never killed)
        self.to = None;
        self.broken.get_or_insert_with(|| DebugError::new(DebugCode::Timeout, "the debug-inspect helper was closed").because("helper_closed"));
        if let Some(mut c) = self.child.take() {
            let t0 = std::time::Instant::now();
            while t0.elapsed() < Duration::from_secs(3) {
                if !matches!(c.try_wait(), Ok(None)) {
                    break;
                }
                std::thread::sleep(Duration::from_millis(50));
            }
        }
    }
}

impl Drop for HelperSource {
    fn drop(&mut self) {
        if self.to.is_some() {
            self.close();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use kiln_manage::debug::TargetKind;
    use kiln_manage::debug::resolver::fake::{FakeProc, FakeSource};
    use std::io::Read;
    use std::sync::{Arc, Mutex};

    const SHA: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const N1: &str = "11111111111111111111111111111111";
    const N2: &str = "22222222222222222222222222222222";

    /// A byte pipe between two threads.
    #[derive(Clone, Default)]
    struct Chan(Arc<(Mutex<(Vec<u8>, bool)>, std::sync::Condvar)>);
    struct ChanW(Chan);
    struct ChanR(Chan);
    impl Write for ChanW {
        fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
            let (m, cv) = &*self.0.0;
            m.lock().unwrap().0.extend_from_slice(b);
            cv.notify_all();
            Ok(b.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }
    impl Drop for ChanW {
        fn drop(&mut self) {
            let (m, cv) = &*self.0.0;
            m.lock().unwrap().1 = true;
            cv.notify_all();
        }
    }
    impl Read for ChanR {
        fn read(&mut self, out: &mut [u8]) -> std::io::Result<usize> {
            let (m, cv) = &*self.0.0;
            let mut g = cv.wait_while(m.lock().unwrap(), |(b, closed)| b.is_empty() && !*closed).unwrap();
            let n = out.len().min(g.0.len());
            out[..n].copy_from_slice(&g.0[..n]);
            g.0.drain(..n);
            Ok(n)
        }
    }

    /// A helper (serve over a fake process table) on its own thread, and the host's HelperSource talking to it.
    fn pair(src: FakeSource) -> (HelperSource, std::thread::JoinHandle<FakeSource>) {
        let (up, down) = (Chan::default(), Chan::default());
        let (up_r, down_w) = (ChanR(up.clone()), ChanW(down.clone()));
        let h = std::thread::spawn(move || {
            let mut src = src;
            serve(std::io::BufReader::new(up_r), down_w, &mut src).unwrap();
            src
        });
        (HelperSource::over(Box::new(ChanW(up)), std::io::BufReader::new(ChanR(down))), h)
    }

    fn probe_of(p: &FakeProc, nonce: &str, challenge: &str) -> Probe {
        Probe {
            windows_pid: p.windows_pid,
            image: p.image.clone(),
            creation_time_100ns: p.creation_time_100ns.clone(),
            image_sha256: p.image_sha256.clone(),
            session_nonce: nonce.into(),
            pipe: kiln_manage::debug::identity::pipe_name(p.windows_pid, nonce),
            challenge: challenge.into(),
        }
    }

    fn kiln(pid: u32) -> FakeProc {
        FakeProc::kiln(pid, r"Z:\srv\a\enshrouded_server.exe", 100, SHA, N1, TargetKind::Server).0
    }

    #[test]
    fn the_protocol_round_trip_holds_and_closes_without_a_terminate() {
        let p = kiln(32);
        let (mut h, t) = pair(FakeSource::new(vec![p.clone()]));
        let probe = probe_of(&p, N1, "challenge-0001");
        let seen = h.inspect(&probe).unwrap();
        assert_eq!((seen.windows_pid, seen.pipe.challenge.as_str()), (32, "challenge-0001"));
        assert_eq!(h.prepare_attach(&probe_of(&p, N1, "challenge-0002")).unwrap().pipe.challenge, "challenge-0002");
        assert!(matches!(h.watch(&probe_of(&p, N1, "challenge-0003")).unwrap(), Watch::Alive(_)));
        h.close();
        let src = t.join().unwrap();
        assert_eq!(src.calls, vec!["inspect 32", "prepare_attach 32", "watch 32", "close"]);
        assert_eq!(src.held, None, "the handle is released");
        assert!(!src.calls.iter().any(|c| c.contains("kill") || c.contains("terminate")));
    }

    #[test]
    fn every_mismatch_is_refused_by_the_helper() {
        type Change = Box<dyn Fn(&mut FakeProc)>;
        let cases: Vec<(&str, Change)> = vec![
            ("nonce", Box::new(|p| p.pipe.as_mut().unwrap().session_nonce = N2.into())),
            ("challenge", Box::new(|p| p.pipe.as_mut().unwrap().echo = false)),
            ("creation_time", Box::new(|p| p.creation_time_100ns = "101".into())),
            ("image_path", Box::new(|p| p.image = r"Z:\srv\b\enshrouded_server.exe".into())),
            ("image_digest", Box::new(|p| p.image_sha256 = "b".repeat(64))),
        ];
        for (reason, change) in cases {
            let want = kiln(32);
            let mut actual = want.clone();
            change(&mut actual);
            let (mut h, t) = pair(FakeSource::new(vec![actual]));
            let e = h.inspect(&probe_of(&want, N1, "challenge-0001")).unwrap_err();
            assert_eq!((e.code, e.reason.as_deref()), (DebugCode::IdentityChanged, Some(reason)), "{reason}: {}", e.message);
            h.close();
            t.join().unwrap();
        }
        // and the Windows side's own refusals come through with their codes
        let (mut h, t) = pair(FakeSource::default());
        assert_eq!(h.inspect(&probe_of(&kiln(32), N1, "challenge-0001")).unwrap_err().code, DebugCode::NotRunning);
        drop(h);
        t.join().unwrap();
        let mut denied = kiln(32);
        denied.denied = true;
        let (mut h, t) = pair(FakeSource::new(vec![denied.clone()]));
        assert_eq!(h.inspect(&probe_of(&denied, N1, "challenge-0001")).unwrap_err().code, DebugCode::PrivilegeMismatch);
        drop(h);
        t.join().unwrap();
    }

    #[test]
    fn the_raw_protocol_refuses_what_it_does_not_know() {
        let p = kiln(32);
        let mut target = serde_json::to_value(probe_of(&p, N1, "x")).unwrap();
        target.as_object_mut().unwrap().remove("challenge");
        let lines = [
            "not json".to_string(),
            json!({ "v": 2, "op": "inspect", "challenge": "challenge-0001", "target": target }).to_string(),
            json!({ "v": 1, "op": "inspect", "challenge": "short", "target": target }).to_string(),
            json!({ "v": 1, "op": "run", "challenge": "challenge-0002", "target": target }).to_string(),
            json!({ "v": 1, "op": "inspect", "challenge": "challenge-0003" }).to_string(),
            json!({ "v": 1, "op": "inspect", "challenge": "challenge-0004", "target": target }).to_string(),
            json!({ "v": 1, "op": "close", "challenge": "challenge-0005" }).to_string(),
            json!({ "v": 1, "op": "inspect", "challenge": "challenge-0006", "target": target }).to_string(),
        ];
        let input = lines.join("\n") + "\n";
        let mut out = Vec::new();
        let mut src = FakeSource::new(vec![p]);
        serve(std::io::Cursor::new(input.into_bytes()), &mut out, &mut src).unwrap();
        let answers: Vec<Value> = String::from_utf8(out).unwrap().lines().map(|l| serde_json::from_str(l).unwrap()).collect();
        assert_eq!(answers.len(), 7, "close ends the helper: the request after it is not read");
        for a in &answers[..5] {
            assert_eq!(a["ok"], false);
            assert_eq!(a["error"]["reason"], "bad_request");
        }
        assert_eq!(answers[3]["challenge"], "challenge-0002", "a refusal echoes a good challenge");
        assert_eq!(answers[5]["ok"], true);
        assert_eq!(answers[5]["seen"]["creationTime100ns"], "100");
        assert_eq!(answers[6]["op"], "close");
        assert_eq!(src.calls, vec!["inspect 32", "close"], "no call for a refused request");
        // a line over 16 KiB is refused and ends the helper
        let mut out = Vec::new();
        let mut src = FakeSource::default();
        serve(std::io::Cursor::new(vec![b' '; MAX_LINE + 2]), &mut out, &mut src).unwrap();
        let a: Value = serde_json::from_str(String::from_utf8(out).unwrap().trim()).unwrap();
        assert_eq!(a["error"]["reason"], "frame_too_large");
        assert_eq!(src.calls, vec!["close"]);
        // end of input releases the handle
        let mut src = FakeSource::new(vec![kiln(32)]);
        let one = json!({ "v": 1, "op": "inspect", "challenge": "challenge-0007", "target": target }).to_string() + "\n";
        serve(std::io::Cursor::new(one.into_bytes()), std::io::sink(), &mut src).unwrap();
        assert_eq!((src.calls.last().map(String::as_str), src.held), (Some("close"), None));
    }

    #[test]
    fn the_helper_runs_only_with_the_manifests_hash() {
        let t = std::env::temp_dir().join(format!("kiln-cli-helper-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        let e = find_helper_in(&t).unwrap_err();
        assert_eq!((e.code, e.reason.as_deref()), (DebugCode::ToolMissing, Some("helper_missing")));
        let dir = t.join("tools/debug-inspect");
        std::fs::create_dir_all(dir.join("win-x64")).unwrap();
        std::fs::write(dir.join("win-x64/kiln.exe"), b"MZ the helper").unwrap();
        let good = kiln_manage::hash::sha256_hex(b"MZ the helper");
        let manifest = |sha: &str| json!({ "format": 1, "helper": "win-x64/kiln.exe", "sha256": sha, "kiln": "0.2.0-dev" }).to_string();
        std::fs::write(dir.join("manifest.json"), manifest(&good)).unwrap();
        assert_eq!(find_helper_in(&t).unwrap(), dir.join("win-x64/kiln.exe"));
        // another file in its place
        std::fs::write(dir.join("win-x64/kiln.exe"), b"MZ something else").unwrap();
        let e = find_helper_in(&t).unwrap_err();
        assert_eq!((e.code, e.reason.as_deref()), (DebugCode::ToolMissing, Some("helper_hash")));
        // a manifest naming another file, or none
        std::fs::write(dir.join("manifest.json"), json!({ "format": 1, "helper": "../../evil.exe", "sha256": good }).to_string()).unwrap();
        assert_eq!(find_helper_in(&t).unwrap_err().code, DebugCode::ToolMissing);
        std::fs::write(dir.join("manifest.json"), "{}").unwrap();
        assert_eq!(find_helper_in(&t).unwrap_err().code, DebugCode::ToolMissing);
        std::fs::remove_dir_all(&t).unwrap();
    }

    #[test]
    fn the_helper_is_found_next_to_kiln_else_in_the_data_folder_never_past_a_bad_one() {
        let t = std::env::temp_dir().join(format!("kiln-cli-helper-from-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        let (beside, data) = (t.join("sdk"), t.join("data"));
        let stage = |root: &std::path::Path, bytes: &[u8], sha: &str| {
            let dir = root.join("tools/debug-inspect");
            std::fs::create_dir_all(dir.join("win-x64")).unwrap();
            std::fs::write(dir.join("win-x64/kiln.exe"), bytes).unwrap();
            std::fs::write(dir.join("manifest.json"), json!({ "format": 1, "helper": "win-x64/kiln.exe", "sha256": sha, "kiln": "0.2.0-dev" }).to_string()).unwrap();
            dir.join("win-x64/kiln.exe")
        };
        std::fs::create_dir_all(&beside).unwrap();
        // nothing anywhere: missing, and the message names both places
        let e = find_helper_from(&beside, Some(&data)).unwrap_err();
        assert_eq!(e.reason.as_deref(), Some("helper_missing"));
        assert!(e.message.contains("data folder"), "{}", e.message);
        // only in the data folder (a kiln downloaded on its own, the zip unpacked there)
        let in_data = stage(&data, b"MZ data", &kiln_manage::hash::sha256_hex(b"MZ data"));
        assert_eq!(find_helper_from(&beside, Some(&data)).unwrap(), in_data);
        // next to kiln wins
        let next = stage(&beside, b"MZ sdk", &kiln_manage::hash::sha256_hex(b"MZ sdk"));
        assert_eq!(find_helper_from(&beside, Some(&data)).unwrap(), next);
        // a changed helper next to kiln is refused, not passed over for the data folder's
        std::fs::write(&next, b"MZ changed").unwrap();
        assert_eq!(find_helper_from(&beside, Some(&data)).unwrap_err().reason.as_deref(), Some("helper_hash"));
        std::fs::remove_dir_all(&t).unwrap();
    }

    #[test]
    fn a_helper_that_lies_or_stalls_is_refused() {
        // answers with another challenge
        let reply = json!({ "v": 1, "ok": true, "op": "inspect", "challenge": "challenge-other", "seen": null }).to_string() + "\n";
        let mut h = HelperSource::over(Box::new(std::io::sink()), std::io::Cursor::new(reply.into_bytes()));
        let e = h.inspect(&probe_of(&kiln(32), N1, "challenge-0001")).unwrap_err();
        assert_eq!(e.reason.as_deref(), Some("helper_challenge"));
        // says nothing: a timeout (never proof that the target is gone), and the source stays broken
        let silent = Chan::default();
        let mut h = HelperSource::over(Box::new(std::io::sink()), std::io::BufReader::new(ChanR(silent.clone())));
        h.first = false;
        let t0 = std::time::Instant::now();
        let e = h.inspect(&probe_of(&kiln(32), N1, "challenge-0001")).unwrap_err();
        assert_eq!(e.code, DebugCode::Timeout);
        assert!(t0.elapsed() >= ANSWER && t0.elapsed() < ANSWER + Duration::from_secs(2));
        assert_eq!(h.prepare_attach(&probe_of(&kiln(32), N1, "challenge-0002")).unwrap_err().code, DebugCode::Timeout);
        drop(ChanW(silent));
    }
}
