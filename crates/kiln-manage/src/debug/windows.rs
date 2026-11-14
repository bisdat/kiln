//! Native Windows (chapter 12a §4 steps 2, 4, 8): the Windows side of a debug target seen from inside its namespace
//! ([`NativeSource`]: the native Windows CLI uses it in-process, the `kiln debug-inspect` helper serves it under a
//! runner), and the lifecycle of an attached adapter (console events, job objects).
//!
//! What is checked here, and how a missing operation shows:
//! - the process: `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE)`, held until `close` (or the next
//!   `inspect`), so its exit is observable and its PID cannot be reused under it; `GetProcessTimes`' creation time;
//!   `QueryFullProcessImageNameW`; the image file's SHA-256;
//! - the control pipe: opened by its nonce name, its server PID (`GetNamedPipeServerProcessId`, where it exists) and
//!   its answer to the challenge.
//!
//! A query or pipe operation the platform lacks (Wine) answers `unsupported_identity` naming it; there is no fallback.
//! Nothing here terminates a process, and no job created here ends what it holds.

use super::errors::{DebugCode, DebugError};
#[cfg(windows)]
use super::resolver::{PipeAnswer, Probe, ProcessSource, Seen, Watch};

/// Process creation flags (Win32 values; the same numbers on every host so the decision can be tested anywhere).
pub const DETACHED_PROCESS: u32 = 0x0000_0008;
pub const CREATE_NEW_PROCESS_GROUP: u32 = 0x0000_0200;
pub const CREATE_BREAKAWAY_FROM_JOB: u32 = 0x0100_0000;

/// The job this process runs in, as far as an adapter started from it is concerned.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct JobFacts {
    pub in_job: bool,
    /// The job ends every process in it when its last handle closes (an editor's job does this when the editor exits).
    pub kill_on_close: bool,
    /// A child may leave the job (`CREATE_BREAKAWAY_FROM_JOB` is allowed).
    pub breakaway_ok: bool,
    /// Children leave the job without asking.
    pub silent_breakaway: bool,
}

/// How netcoredbg is started on Windows (chapter 12a §4 step 8): without a console (console close and Ctrl-C never
/// reach it), in its own process group, and out of a job that would end it with the editor when the job allows that.
/// A debugger that dies while attached can take the debuggee with it, so the adapter must outlive whatever ends the
/// editor; the proxy closing the adapter's stdin is what makes it detach. Returns the flags and, when the adapter
/// stays in a kill-on-close job, the warning to show.
pub fn adapter_flags(job: JobFacts) -> (u32, Option<String>) {
    let mut flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    if !job.in_job || !job.kill_on_close || job.silent_breakaway {
        return (flags, None);
    }
    if job.breakaway_ok {
        flags |= CREATE_BREAKAWAY_FROM_JOB;
        return (flags, None);
    }
    (
        flags,
        Some("kiln debug runs in a job that ends every process in it when it closes and does not let the debugger leave it: if the editor (or its terminal) is closed while attached, the debugger ends too and can take the debuggee with it. Disconnect before closing the editor.".into()),
    )
}

/// The Win32 error of a failed query as a refusal: the rights (privilege_mismatch), the process is gone (not_running),
/// or the operation is missing (unsupported_identity naming it).
pub fn refusal(op: &str, error: u32, pid: u32) -> DebugError {
    match error {
        5 => DebugError::new(DebugCode::PrivilegeMismatch, format!("{op}({pid}): access denied (another user's or an elevated process)")).because("open_denied"),
        87 => DebugError::new(DebugCode::NotRunning, format!("no process {pid}")).because("no_process"),
        1 | 50 | 120 => DebugError::new(DebugCode::UnsupportedIdentity, format!("{op} is not supported here (error {error}): the identity of PID {pid} cannot be proven")).because(op),
        e => DebugError::new(DebugCode::UnsupportedIdentity, format!("{op} failed for PID {pid} (error {e}): its identity cannot be proven")).because(op),
    }
}

#[cfg(windows)]
pub use imp::*;

#[cfg(windows)]
mod imp {
    use super::*;
    use std::io::{Read, Write};
    use std::os::windows::io::AsRawHandle;
    use std::sync::{Condvar, Mutex, OnceLock};
    use std::time::Duration;
    use windows_sys::Win32::Foundation::{CloseHandle, FILETIME, GetLastError, HANDLE, WAIT_OBJECT_0};
    use windows_sys::Win32::Security::{GetTokenInformation, TOKEN_ELEVATION, TOKEN_QUERY, TokenElevation};
    use windows_sys::Win32::System::Console::{
        CTRL_BREAK_EVENT, CTRL_C_EVENT, CTRL_CLOSE_EVENT, CTRL_LOGOFF_EVENT, CTRL_SHUTDOWN_EVENT, SetConsoleCtrlHandler,
    };
    use windows_sys::Win32::System::JobObjects::{
        IsProcessInJob, JOB_OBJECT_LIMIT_BREAKAWAY_OK, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK,
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JobObjectExtendedLimitInformation, QueryInformationJobObject,
    };
    use windows_sys::Win32::System::Pipes::{GetNamedPipeServerProcessId, WaitNamedPipeW};
    use windows_sys::Win32::System::Threading::{
        GetCurrentProcess, GetProcessTimes, OpenProcess, OpenProcessToken, PROCESS_NAME_WIN32, PROCESS_QUERY_LIMITED_INFORMATION,
        PROCESS_SYNCHRONIZE, QueryFullProcessImageNameW, WaitForSingleObject,
    };

    /// How long the control pipe has to answer (the helper's whole answer is due within 5 s).
    const PIPE_ANSWER: Duration = Duration::from_secs(3);

    /// An open process handle (query + synchronize). Closed on drop; never used to end the process.
    struct Held {
        pid: u32,
        h: HANDLE,
    }

    // SAFETY: a process handle may be used from any thread.
    unsafe impl Send for Held {}

    impl Drop for Held {
        fn drop(&mut self) {
            // SAFETY: our own handle, closed once.
            unsafe { CloseHandle(self.h) };
        }
    }

    fn open(pid: u32) -> Result<Held, DebugError> {
        // SAFETY: plain Win32 call; the handle is owned by Held.
        let h = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SYNCHRONIZE, 0, pid) };
        if h.is_null() {
            // SAFETY: reads this thread's last error.
            return Err(refusal("OpenProcess", unsafe { GetLastError() }, pid));
        }
        Ok(Held { pid, h })
    }

    fn exited(held: &Held) -> bool {
        // SAFETY: a handle with SYNCHRONIZE access; a zero timeout never blocks.
        unsafe { WaitForSingleObject(held.h, 0) == WAIT_OBJECT_0 }
    }

    fn creation(held: &Held) -> Result<u64, DebugError> {
        // SAFETY: zeroed out-structs, our handle.
        unsafe {
            let (mut c, mut x, mut k, mut u): (FILETIME, FILETIME, FILETIME, FILETIME) = std::mem::zeroed();
            if GetProcessTimes(held.h, &mut c, &mut x, &mut k, &mut u) == 0 {
                return Err(refusal("GetProcessTimes", GetLastError(), held.pid));
            }
            Ok((u64::from(c.dwHighDateTime) << 32) | u64::from(c.dwLowDateTime))
        }
    }

    fn image(held: &Held) -> Result<String, DebugError> {
        let mut buf = vec![0u16; 32768];
        let mut len = buf.len() as u32;
        // SAFETY: the buffer and its length are ours.
        if unsafe { QueryFullProcessImageNameW(held.h, PROCESS_NAME_WIN32, buf.as_mut_ptr(), &mut len) } == 0 {
            // SAFETY: reads this thread's last error.
            return Err(refusal("QueryFullProcessImageNameW", unsafe { GetLastError() }, held.pid));
        }
        Ok(String::from_utf16_lossy(&buf[..len as usize]))
    }

    fn digest(image: &str, pid: u32) -> Result<String, DebugError> {
        crate::hash::sha256_file(std::path::Path::new(image)).map(|(h, _)| h).map_err(|e| {
            DebugError::new(DebugCode::UnsupportedIdentity, format!("the image of PID {pid} ({image}) cannot be read: {}", e.message)).because("image_read")
        })
    }

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(std::iter::once(0)).collect()
    }

    /// One identify exchange on the control pipe. A pipe that is not there is not this process's (identity_changed);
    /// one that does not answer in time proves nothing (timeout).
    fn ask_pipe(probe: &Probe) -> Result<PipeAnswer, DebugError> {
        let pid = probe.windows_pid;
        let mut tries = 0;
        let mut f = loop {
            match std::fs::OpenOptions::new().read(true).write(true).open(&probe.pipe) {
                Ok(f) => break f,
                Err(e) if e.raw_os_error() == Some(231) && tries < 3 => {
                    // ERROR_PIPE_BUSY: another client is being served; wait for the instance
                    tries += 1;
                    // SAFETY: a NUL-terminated name.
                    unsafe { WaitNamedPipeW(wide(&probe.pipe).as_ptr(), 1000) };
                }
                Err(e) if matches!(e.raw_os_error(), Some(2 | 3)) => {
                    return Err(DebugError::identity_changed(format!("PID {pid} serves no control pipe {}", probe.pipe), "pipe_missing"));
                }
                Err(e) if e.raw_os_error() == Some(5) => {
                    return Err(DebugError::new(DebugCode::PrivilegeMismatch, format!("the control pipe of PID {pid} refuses this user")).because("pipe_denied"));
                }
                Err(e) if e.raw_os_error() == Some(231) => {
                    return Err(DebugError::new(DebugCode::Timeout, format!("the control pipe of PID {pid} stayed busy")).because("pipe_busy"));
                }
                Err(e) => return Err(refusal("CreateFileW(pipe)", e.raw_os_error().unwrap_or(0) as u32, pid)),
            }
        };
        let mut server = 0u32;
        // SAFETY: the file's own handle; an out u32.
        if unsafe { GetNamedPipeServerProcessId(f.as_raw_handle() as HANDLE, &mut server) } != 0 && server != pid {
            return Err(DebugError::identity_changed(format!("the control pipe named for PID {pid} is served by PID {server}"), "pipe_server"));
        }
        let mut line = super::super::resolver::pipe_request(&probe.session_nonce, &probe.challenge);
        line.push('\n');
        f.write_all(line.as_bytes()).map_err(|e| DebugError::new(DebugCode::Timeout, format!("the control pipe of PID {pid} took no request: {e}")).because("pipe_write"))?;
        // the read runs on its own thread so a pipe that never answers cannot hold this one past PIPE_ANSWER
        let (tx, rx) = std::sync::mpsc::channel();
        std::thread::spawn(move || {
            let mut got = Vec::new();
            let mut b = [0u8; 4096];
            let r = loop {
                match f.read(&mut b) {
                    Ok(0) => break Err("closed".to_string()),
                    Ok(n) => {
                        got.extend_from_slice(&b[..n]);
                        if let Some(end) = got.iter().position(|&c| c == b'\n') {
                            got.truncate(end);
                            break Ok(String::from_utf8_lossy(&got).trim_end_matches('\r').to_string());
                        }
                        if got.len() > super::super::identity::MAX_FRAME {
                            break Err("an answer over 16 KiB".to_string());
                        }
                    }
                    Err(e) => break Err(e.to_string()),
                }
            };
            let _ = tx.send(r);
        });
        match rx.recv_timeout(PIPE_ANSWER) {
            Ok(Ok(answer)) => super::super::resolver::parse_pipe_answer(&answer),
            Ok(Err(why)) => Err(DebugError::new(DebugCode::Timeout, format!("the control pipe of PID {pid} gave no answer ({why})")).because("pipe_no_answer")),
            Err(_) => Err(DebugError::new(DebugCode::Timeout, format!("the control pipe of PID {pid} did not answer within {} s", PIPE_ANSWER.as_secs())).because("pipe_no_answer")),
        }
    }

    fn look(held: &Held, probe: &Probe) -> Result<Seen, DebugError> {
        if exited(held) {
            return Err(DebugError::new(DebugCode::NotRunning, format!("PID {} has exited", held.pid)).because("exited"));
        }
        let created = creation(held)?;
        let image = image(held)?;
        let image_sha256 = digest(&image, held.pid)?;
        let pipe = ask_pipe(probe)?;
        Ok(Seen { windows_pid: held.pid, image, creation_time_100ns: created.to_string(), image_sha256, pipe })
    }

    /// Whether this process's token is elevated.
    pub fn token_elevated() -> Option<bool> {
        // SAFETY: our own token, closed; a zeroed out-struct of the size passed.
        unsafe {
            let mut token: HANDLE = std::ptr::null_mut();
            if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) == 0 {
                return None;
            }
            let mut e: TOKEN_ELEVATION = std::mem::zeroed();
            let mut len = 0u32;
            let ok = GetTokenInformation(token, TokenElevation, (&mut e as *mut TOKEN_ELEVATION).cast(), std::mem::size_of::<TOKEN_ELEVATION>() as u32, &mut len);
            CloseHandle(token);
            (ok != 0).then_some(e.TokenIsElevated != 0)
        }
    }

    /// The Windows side, in this process.
    #[derive(Default)]
    pub struct NativeSource {
        held: Option<Held>,
    }

    impl NativeSource {
        fn held(&self, pid: u32) -> Result<&Held, DebugError> {
            self.held.as_ref().filter(|h| h.pid == pid).ok_or_else(|| DebugError::identity_changed(format!("PID {pid} is not the held process"), "not_held"))
        }
    }

    impl ProcessSource for NativeSource {
        fn elevated(&mut self) -> Option<bool> {
            token_elevated()
        }
        fn inspect(&mut self, probe: &Probe) -> Result<Seen, DebugError> {
            self.held = None;
            let held = open(probe.windows_pid)?;
            let seen = look(&held, probe)?;
            self.held = Some(held);
            Ok(seen)
        }
        fn prepare_attach(&mut self, probe: &Probe) -> Result<Seen, DebugError> {
            look(self.held(probe.windows_pid)?, probe)
        }
        fn watch(&mut self, probe: &Probe) -> Result<Watch, DebugError> {
            let held = self.held(probe.windows_pid)?;
            if exited(held) {
                return Ok(Watch::Exited);
            }
            look(held, probe).map(Watch::Alive)
        }
        fn close(&mut self) {
            self.held = None;
        }
    }

    /// This process's job, for [`adapter_flags`].
    pub fn job_facts() -> JobFacts {
        // SAFETY: queries about our own process; a zeroed out-struct of the size passed; a null job handle means "the
        // job of this process".
        unsafe {
            let mut in_job = 0;
            if IsProcessInJob(GetCurrentProcess(), std::ptr::null_mut(), &mut in_job) == 0 || in_job == 0 {
                return JobFacts::default();
            }
            let mut info: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = std::mem::zeroed();
            let mut len = 0u32;
            if QueryInformationJobObject(
                std::ptr::null_mut(),
                JobObjectExtendedLimitInformation,
                (&mut info as *mut JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
                std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
                &mut len,
            ) == 0
            {
                // in a job we cannot read: assume the worst (it may end us) and that we may not leave it
                return JobFacts { in_job: true, kill_on_close: true, breakaway_ok: false, silent_breakaway: false };
            }
            let f = info.BasicLimitInformation.LimitFlags;
            JobFacts {
                in_job: true,
                kill_on_close: f & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE != 0,
                breakaway_ok: f & JOB_OBJECT_LIMIT_BREAKAWAY_OK != 0,
                silent_breakaway: f & JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK != 0,
            }
        }
    }

    type OnEvent = Box<dyn Fn(&'static str) + Send + Sync>;
    static ON_EVENT: OnceLock<OnEvent> = OnceLock::new();
    static DETACHED: (Mutex<bool>, Condvar) = (Mutex::new(false), Condvar::new());

    unsafe extern "system" fn on_console(event: u32) -> windows_sys::core::BOOL {
        let what = match event {
            CTRL_C_EVENT => "Ctrl-C",
            CTRL_BREAK_EVENT => "Ctrl-Break",
            CTRL_CLOSE_EVENT => "the console was closed",
            CTRL_LOGOFF_EVENT => "logoff",
            CTRL_SHUTDOWN_EVENT => "shutdown",
            _ => return 0,
        };
        if let Some(f) = ON_EVENT.get() {
            f(what);
        }
        if matches!(event, CTRL_CLOSE_EVENT | CTRL_LOGOFF_EVENT | CTRL_SHUTDOWN_EVENT) {
            // Windows ends this process when the handler returns (or about 5 s after the event): give the safe detach
            // that time. The adapter has no console and may have left the job, so it is not ended with us; its stdin
            // closing is what makes it detach if we are cut short.
            let (lock, cv) = &DETACHED;
            let done = lock.lock().unwrap_or_else(|e| e.into_inner());
            let _ = cv.wait_timeout_while(done, Duration::from_millis(4500), |d| !*d);
        }
        1
    }

    /// Console events (Ctrl-C, Ctrl-Break, console close, logoff, shutdown) call `f` instead of ending this process.
    pub fn watch_console(f: OnEvent) -> bool {
        if ON_EVENT.set(f).is_err() {
            return true;
        }
        // SAFETY: registers a static handler for this process.
        unsafe { SetConsoleCtrlHandler(Some(on_console), 1) != 0 }
    }

    /// The safe detach is over: a handler holding the process for it may return now.
    pub fn detach_finished() {
        let (lock, cv) = &DETACHED;
        *lock.lock().unwrap_or_else(|e| e.into_inner()) = true;
        cv.notify_all();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn adapter_flags_never_leave_it_in_a_job_that_ends_it_when_avoidable() {
        let base = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
        assert_eq!(adapter_flags(JobFacts::default()), (base, None));
        // a job that does not end its processes: nothing to leave
        assert_eq!(adapter_flags(JobFacts { in_job: true, ..Default::default() }), (base, None));
        // an editor's kill-on-close job that allows leaving: leave it
        let k = JobFacts { in_job: true, kill_on_close: true, breakaway_ok: true, silent_breakaway: false };
        assert_eq!(adapter_flags(k), (base | CREATE_BREAKAWAY_FROM_JOB, None));
        // children leave silently anyway
        let s = JobFacts { in_job: true, kill_on_close: true, breakaway_ok: false, silent_breakaway: true };
        assert_eq!(adapter_flags(s), (base, None));
        // cannot leave: started anyway (no kill), with a warning
        let stuck = JobFacts { in_job: true, kill_on_close: true, breakaway_ok: false, silent_breakaway: false };
        let (f, w) = adapter_flags(stuck);
        assert_eq!(f, base);
        assert!(w.unwrap().contains("Disconnect before closing the editor"));
    }

    #[test]
    fn win32_errors_as_refusals() {
        assert_eq!(refusal("OpenProcess", 5, 32).code, DebugCode::PrivilegeMismatch);
        assert_eq!(refusal("OpenProcess", 87, 32).code, DebugCode::NotRunning);
        let e = refusal("GetProcessTimes", 120, 32);
        assert_eq!((e.code, e.reason.as_deref()), (DebugCode::UnsupportedIdentity, Some("GetProcessTimes")));
        assert_eq!(refusal("QueryFullProcessImageNameW", 31, 32).reason.as_deref(), Some("QueryFullProcessImageNameW"));
    }

    /// Nothing in the Windows side or the helper may end a process: no terminate call, no kill-on-close job of ours.
    #[test]
    fn no_terminate_call_in_the_windows_side() {
        let here = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
        for f in [here.join("src/debug/windows.rs"), here.join("src/debug/resolver.rs"), here.join("../kiln-cli/src/debug_inspect.rs")] {
            let text = std::fs::read_to_string(&f).unwrap_or_default();
            let code: String = text.lines().filter(|l| !l.trim_start().starts_with("//") && !l.contains("assert") && !l.contains("for word in")).collect::<Vec<_>>().join("\n");
            for word in ["TerminateProcess", "TerminateJobObject", "SetInformationJobObject", "AssignProcessToJobObject", ".kill("] {
                assert!(!code.contains(word), "{} uses {word}", f.display());
            }
        }
    }
}
