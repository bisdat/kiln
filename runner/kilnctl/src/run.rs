// SPDX-License-Identifier: MIT
//! `kilnctl run`: the server in the foreground, the way systemd runs it. Shims over kiln_manage::server::run
//!: SIGINT, SIGTERM and SIGHUP become one SIGINT to the server process, then `wineserver -w`.
use crate::config::Instance;
#[cfg(unix)]
use crate::util::Km;
use crate::wine::Runner;
use anyhow::Result;
use std::time::Duration;

#[cfg(unix)]
#[allow(unused_imports)]
pub use kiln_manage::server::run::{exit_code, signame};

/// `wineserver -w` for the instance's prefix, bounded: on timeout `wineserver -k`.
#[cfg(unix)]
pub fn wineserver_wait(runner: &Runner, inst: &Instance, timeout: Duration) -> Result<()> {
    kiln_manage::server::run::wineserver_wait(runner, inst, timeout).km()
}

#[cfg(unix)]
pub fn run(inst: &Instance) -> Result<i32> {
    use kiln_manage::{start_progress as sp, stopped_start as ss};
    let path = inst.run_log().with_extension("stopped-start.json");
    // kilnctl's receipt, never a Kiln state file. Clear it before asking Wine for another start.
    if let Err(e) = std::fs::remove_file(&path) {
        if e.kind() != std::io::ErrorKind::NotFound { return Err(e.into()); }
    }
    let began_ms = sp::unix_ms(std::time::SystemTime::now());
    let result = kiln_manage::server::run::run_foreground(inst).km();
    if let Some(stop) = ss::read(&inst.dir, &ss::StartIdentity { pid: None, began_ms,
        ended_ms: Some(sp::unix_ms(std::time::SystemTime::now())) }, &Default::default()) {
        eprintln!("Kiln stopped the start: {}\n{}", stop.reason, stop.advice);
        if let Ok(bytes) = serde_json::to_vec(&stop) { let _ = std::fs::write(path, bytes); }
    }
    result
}

// Windows: kilnctl's instances are Wine servers on Linux; bundle, client, admin and deploy --bundle work on Windows,
// a native Windows server is run by the Kiln Launcher (server::run's windows runner).
#[cfg(not(unix))]
pub fn wineserver_wait(_runner: &Runner, _inst: &Instance, _timeout: Duration) -> Result<()> {
    anyhow::bail!("{}", crate::util::UNIX_ONLY)
}

#[cfg(not(unix))]
pub fn run(_inst: &Instance) -> Result<i32> {
    anyhow::bail!("{}", crate::util::UNIX_ONLY)
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use std::os::unix::process::ExitStatusExt;
    use std::process::ExitStatus;

    #[test]
    fn exit_codes() {
        assert_eq!(exit_code(ExitStatus::from_raw(0)), 0);
        assert_eq!(exit_code(ExitStatus::from_raw(3 << 8)), 3);
        assert_eq!(exit_code(ExitStatus::from_raw(libc::SIGKILL)), 128 + 9);
        assert_eq!(signame(libc::SIGTERM), "SIGTERM");
    }
}

/// kilnctl's own last-run receipt. No reads of Kiln's historical .last files.
pub fn last_stop(inst: &Instance) -> Option<kiln_manage::stopped_start::StoppedStart> {
    use std::io::Read;
    let f = std::fs::File::open(inst.run_log().with_extension("stopped-start.json")).ok()?;
    let mut bytes = Vec::new();
    f.take(kiln_manage::stopped_start::LIMIT + 1).read_to_end(&mut bytes).ok()?;
    if bytes.len() as u64 > kiln_manage::stopped_start::LIMIT { return None; }
    serde_json::from_slice(&bytes).ok()
}
