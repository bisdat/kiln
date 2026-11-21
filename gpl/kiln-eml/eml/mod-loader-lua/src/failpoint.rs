// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln-eml: named publication boundaries for mod-loader-lua's own unit tests. Everything here is cfg(test):
//! production builds (and every other crate's tests) have no failpoint, recorder or kill switch, and the boundaries
//! cost nothing there.
//!
//! A boundary is passed AFTER the step it names is complete (a file written and synced, a rename done, a record
//! removed). The test that drives a run either records every boundary it passes (`record`), or arms the n-th one
//! (`arm`): that boundary then fails with an injected I/O error, or (in a child process of the kill matrix) writes a
//! marker file and waits there until the parent kills the process.
//!
//! The kill matrix creates its child processes through `test_gate::spawn`, which never creates one while a thread of
//! this test process holds a file lock (test_gate.rs).
use std::{cell::RefCell, io, path::Path, time::Duration};

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Boundary {
    pub(crate) point: &'static str,
    /// Which file, with UUIDs written `*` and the private stage written `stage`, so runs compare.
    pub(crate) detail: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Action {
    /// The step after the boundary does not happen: the boundary returns an I/O error.
    Fail,
    /// The process stops at the boundary: write `marker`, then wait to be killed (never returns).
    Kill,
}

struct State {
    seen: Vec<Boundary>,
    armed: Option<(usize, Action)>,
    marker: Option<std::path::PathBuf>,
}

thread_local! {
    static STATE: RefCell<Option<State>> = const { RefCell::new(None) };
}

/// Records every boundary this thread passes from now on (and disarms).
pub(crate) fn record() {
    STATE.with(|s| {
        *s.borrow_mut() = Some(State {
            seen: Vec::new(),
            armed: None,
            marker: None,
        })
    });
}

/// Records, and arms boundary number `at` (0 = the first passed from now on).
pub(crate) fn arm(at: usize, action: Action, marker: Option<&Path>) {
    STATE.with(|s| {
        *s.borrow_mut() = Some(State {
            seen: Vec::new(),
            armed: Some((at, action)),
            marker: marker.map(Path::to_path_buf),
        })
    });
}

/// The boundaries passed since `record`/`arm`; recording stops.
pub(crate) fn take() -> Vec<Boundary> {
    STATE.with(|s| s.borrow_mut().take().map(|s| s.seen).unwrap_or_default())
}

/// `point` with the file it concerns: `<parent>/<name>` of `path`, normalized.
pub(crate) fn at(point: &'static str, path: &Path) -> io::Result<()> {
    let name = path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let parent = path
        .parent()
        .and_then(Path::file_name)
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let in_stage = parent.starts_with(".kiln-eml-stage-") || path.starts_with("/proc/self/fd");
    hit(
        point,
        &format!(
            "{}/{name}",
            if in_stage { "stage" } else { parent.as_str() }
        ),
    )
}

pub(crate) fn hit(point: &'static str, detail: &str) -> io::Result<()> {
    // The fingerprint journal is replaced on a timer (cache/prints.rs): a performance help, never a publication step.
    if detail.ends_with("files.partial.json") {
        return Ok(());
    }
    let fire = STATE.with(|s| {
        let mut s = s.borrow_mut();
        let state = s.as_mut()?;
        let index = state.seen.len();
        state.seen.push(Boundary {
            point,
            detail: normalize(detail),
        });
        match state.armed {
            Some((at, action)) if at == index => Some((action, state.marker.clone())),
            _ => None,
        }
    });
    match fire {
        None => Ok(()),
        Some((Action::Fail, _)) => Err(io::Error::other(format!(
            "injected failure at {point} {detail}"
        ))),
        Some((Action::Kill, marker)) => {
            if let Some(marker) = marker {
                // Written whole, then renamed: the parent never reads a half-written marker.
                let tmp = marker.with_extension("tmp");
                let _ = std::fs::write(&tmp, format!("{point} {}", normalize(detail)));
                let _ = std::fs::rename(&tmp, &marker);
            }
            loop {
                std::thread::sleep(Duration::from_secs(1));
            }
        }
    }
}

fn normalize(detail: &str) -> String {
    let mut out = String::with_capacity(detail.len());
    let bytes = detail.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if i + 36 <= bytes.len()
            && uuid::Uuid::try_parse(&detail[i..i + 36]).is_ok()
            && detail.is_char_boundary(i + 36)
        {
            out.push('*');
            i += 36;
        } else {
            let c = detail[i..].chars().next().unwrap();
            out.push(c);
            i += c.len_utf8();
        }
    }
    out
}
