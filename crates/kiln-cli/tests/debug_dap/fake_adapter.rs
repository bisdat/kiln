// SPDX-License-Identifier: MIT
//! The fake netcoredbg of the DAP suite (`suite.rs`): a process started by the proxy exactly as it starts netcoredbg
//! (`<adapter> --interpreter=vscode`, stdin/stdout piped). It speaks DAP over stdio and logs, one JSON line each, what
//! it received (`{"ev":"recv","msg":...}`), what it could not read, end of input and how it ended. It only ever ends by
//! itself: on end of input (after a pause with `linger`), on `fake:die`, or after its safety time. A line
//! `{"ev":"exit",...}` in the log is what tells the suite it was not killed.
//!
//! Modes (`KILN_DAP_FAKE_MODE`, comma separated): `deaf` never answers a disconnect; `linger` stays 8 s after end of
//! input. Requests the suite uses to steer it are `evaluate` expressions: `fake:stop` (answers, then a `stopped`
//! event on thread 7), `fake:hold` (never answers: an evaluation that hangs), `fake:reload` (the module comes again),
//! `fake:garbage` (noise, a non-JSON body, an unreadable length, then an `output` event), `fake:huge` (a length
//! over the proxy's limit), `fake:die` (exits at once, as a crashed adapter).
//! Built only into the kiln-cli test binary (`#[cfg(test)]`); nothing of it ships.

use crate::debug::{read_frame, write_frame, write_json};
use serde_json::{Value, json};
use std::io::Write;
use std::sync::Mutex;
use std::time::{Duration, Instant};

pub(super) const THREAD: i64 = 7;
pub(super) const MODULE: &str = "DebugCounter";

struct Log {
    file: Mutex<std::fs::File>,
    t0: Instant,
}

impl Log {
    fn put(&self, mut v: Value) {
        v["ms"] = json!(self.t0.elapsed().as_millis() as u64);
        let mut f = self.file.lock().unwrap_or_else(|e| e.into_inner());
        let _ = writeln!(f, "{v}");
        let _ = f.flush();
    }
}

struct Out {
    w: std::io::Stdout,
    seq: i64,
}

impl Out {
    fn send(&mut self, mut v: Value) {
        self.seq += 1;
        v["seq"] = json!(self.seq);
        let _ = write_json(&mut self.w.lock(), &v);
    }
    fn respond(&mut self, req: &Value, body: Value) {
        self.send(json!({ "type": "response", "request_seq": req["seq"], "command": req["command"], "success": true, "body": body }));
    }
    fn event(&mut self, event: &str, body: Value) {
        self.send(json!({ "type": "event", "event": event, "body": body }));
    }
    fn raw(&mut self, bytes: &[u8]) {
        let mut w = self.w.lock();
        let _ = w.write_all(bytes);
        let _ = w.flush();
    }
}

fn module_new() -> Value {
    json!({ "reason": "new", "module": { "id": "1", "name": MODULE, "path": "C:\\mods\\DebugCounter\\DebugCounter.dll" } })
}

pub(super) fn main() -> i32 {
    let Some(path) = std::env::var_os("KILN_DAP_FAKE_LOG") else {
        eprintln!("fake adapter: no KILN_DAP_FAKE_LOG");
        return 2;
    };
    let mode = std::env::var("KILN_DAP_FAKE_MODE").unwrap_or_default();
    let has = |m: &str| mode.split(',').any(|x| x == m);
    let Ok(file) = std::fs::OpenOptions::new().create(true).append(true).open(&path) else { return 2 };
    let log = Log { file: Mutex::new(file), t0: Instant::now() };
    log.put(json!({ "ev": "start", "pid": std::process::id(), "args": super::cmdline(), "mode": mode }));
    super::exit_after(Duration::from_secs(150), 0, "fake adapter");
    let mut out = Out { w: std::io::stdout(), seq: 0 };
    let mut r = std::io::BufReader::new(std::io::stdin());
    loop {
        let body = match read_frame(&mut r) {
            Ok(Some(b)) => b,
            Ok(None) => break,
            Err(e) => {
                log.put(json!({ "ev": "recv_bad", "error": e.to_string() }));
                break;
            }
        };
        let Ok(m) = serde_json::from_slice::<Value>(&body) else {
            log.put(json!({ "ev": "recv_bad", "raw": String::from_utf8_lossy(&body) }));
            continue;
        };
        log.put(json!({ "ev": "recv", "msg": m }));
        if m["type"] != "request" {
            continue;
        }
        match m["command"].as_str().unwrap_or_default() {
            "initialize" => {
                // everything a real adapter could offer, the dangerous ones included: the proxy must take them back
                let caps = json!({
                    "supportsConfigurationDoneRequest": true, "supportsTerminateRequest": true, "supportTerminateDebuggee": true,
                    "supportSuspendDebuggee": true, "supportsRestartRequest": true, "supportsTerminateThreadsRequest": true,
                    "supportsEvaluateForHovers": true,
                });
                out.respond(&m, caps);
                out.event("initialized", json!({}));
            }
            "attach" => {
                out.respond(&m, json!({}));
                out.event("module", module_new());
            }
            "disconnect" if has("deaf") => log.put(json!({ "ev": "ignored", "command": "disconnect" })),
            "continue" => out.respond(&m, json!({ "allThreadsContinued": true })),
            "evaluate" => match m["arguments"]["expression"].as_str().unwrap_or_default() {
                "fake:stop" => {
                    out.respond(&m, json!({ "result": "ok", "variablesReference": 0 }));
                    out.event("stopped", json!({ "reason": "breakpoint", "threadId": THREAD, "allThreadsStopped": true }));
                }
                "fake:hold" => log.put(json!({ "ev": "held", "seq": m["seq"] })),
                "fake:reload" => {
                    out.respond(&m, json!({ "result": "ok", "variablesReference": 0 }));
                    out.event("module", module_new());
                }
                "fake:garbage" => {
                    out.respond(&m, json!({ "result": "ok", "variablesReference": 0 }));
                    out.raw(b"this line is noise from the adapter\r\n");
                    out.raw(b"X-Noise: 1\r\n\r\n");
                    let _ = write_frame(&mut out.w.lock(), b"not json from adapter");
                    out.raw(b"Content-Length: abc\r\n\r\njunk after a bad length\r\n");
                    out.event("output", json!({ "category": "console", "output": "after-garbage\n" }));
                }
                "fake:huge" => out.raw(b"Content-Length: 999999999999\r\n\r\n"),
                "fake:die" => {
                    log.put(json!({ "ev": "exit", "why": "die" }));
                    std::process::exit(3);
                }
                _ => out.respond(&m, json!({ "result": "1", "variablesReference": 0 })),
            },
            _ => out.respond(&m, json!({})),
        }
    }
    log.put(json!({ "ev": "eof" }));
    if has("linger") {
        std::thread::sleep(Duration::from_secs(8));
        log.put(json!({ "ev": "exit", "why": "linger" }));
        return 0;
    }
    log.put(json!({ "ev": "exit", "why": "eof" }));
    0
}
