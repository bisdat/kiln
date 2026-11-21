// SPDX-License-Identifier: MIT
//! A healthy thread handles the rare parked-stack signal; the faulted stack
//! must never return to Rust just to report its own failure.
use super::*;
use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    thread::{self, JoinHandle},
    time::Duration,
};
#[derive(Default)]
pub(super) struct ParkedState {
    pub all: bool,
    pub owners: BTreeSet<u64>,
}
pub(super) struct Supervisor {
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}
impl Supervisor {
    pub fn spawn<S: Sink + Send + 'static>(
        mut source: Box<dyn FaultSource>,
        sink: Arc<Mutex<S>>,
        state: Arc<Mutex<ParkedState>>,
    ) -> Result<Self, String> {
        let stop = Arc::new(AtomicBool::new(false));
        let stopped = stop.clone();
        let thread = thread::Builder::new()
            .name("kiln-sfi-fault-report".into())
            .spawn(move || {
                while !stopped.load(Ordering::Acquire) {
                    let event = match source.wait(Duration::from_millis(100)) {
                        Ok(Some(event)) => event,
                        Ok(None) => continue,
                        Err(reason) => ParkedFault {
                            owner: None,
                            all: true,
                            reason,
                        },
                    };
                    {
                        let mut status = state.lock().unwrap_or_else(|e| e.into_inner());
                        if event.all || event.owner.is_none() {
                            status.all = true;
                        } else if let Some(owner) = event.owner {
                            status.owners.insert(owner);
                        }
                    }
                    let reason = if event.all {
                        format!("interpreter runtime stopped, do not save: {}", event.reason)
                    } else {
                        format!("package runtime stopped, do not save: {}", event.reason)
                    };
                    let _ = crate::guard_callback(|| {
                        let mut sink = sink.lock().unwrap_or_else(|e| e.into_inner());
                        sink.quarantine(if event.all { None } else { event.owner }, &reason);
                        sink.record(&reason);
                        sink.save_warning(&reason);
                    });
                    if event.all {
                        break;
                    }
                }
            })
            .map_err(|e| e.to_string())?;
        Ok(Self {
            stop,
            thread: Some(thread),
        })
    }
}
impl Drop for Supervisor {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(thread) = self.thread.take()
            && thread.is_finished()
        {
            let _ = thread.join();
        } // No unbounded wait; source has a100ms bounded wake.
    }
}
