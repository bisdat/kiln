// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (modules/transport is identical in both pins): native/steam_network.cpp:19-35,
// 284-317 (the service channel, probe state), :375-427 (the health pass), :431-442 (stats), :484-494 (the remote
// server); lib.rs:154-197 (the stats as Rust reads them); report_network_health.rs:1-85 (the log lines).
//! `wire.health.v1` (row 43): ShroudForge's `SFN1` health check on Steam channel 65535, and its log lines.
//!
//! - Each pass drains at most [`DRAIN`] messages; only 1 to [`MAX_PAYLOAD`] bytes count. `SFN1:PING:<x>` is
//!   answered `SFN1:PONG:<x>` (verbatim, both roles, reliable); a client also remembers the sender as the remote
//!   dedicated server ([`Health::remote_server`], at most 15 s old). A server takes `SFN1:PONG:<n>` only when `n`
//!   is all decimal digits that fit a u64, the nonce is pending and the sender is the peer it was sent to: one
//!   acknowledgement, its round trip, the nonce is gone.
//! - Pending probes older than 15,000 ms (strictly) expire; every 5,000 ms or more a server probes each authorized
//!   peer with the next nonce; a probe is pending only when Steam answered EResult 1.
//! - [`Health::report`]: the state-change and probe lines `report_network_health` logs after each periodic pass.
//!
//! Pure state: the worker's maintenance pass ([`super::maintenance`]) does the lease calls and passes the clock in
//! (GetTickCount64 milliseconds).

use std::collections::BTreeMap;

/// The Network health channel (reserved: mods cannot send or receive on it).
pub const CHANNEL: u32 = 65_535;
/// Messages taken off the channel per pass.
pub const DRAIN: usize = 16;
/// The largest health message read (bytes).
pub const MAX_PAYLOAD: usize = 128;
pub const PROBE_INTERVAL_MS: u64 = 5_000;
pub const PROBE_MAX_AGE_MS: u64 = 15_000;
/// Pending probes kept (never reached with a server's players; a bound, not a ShroudForge behaviour).
pub const PENDING_CAP: usize = 1_024;
pub const PING: &[u8] = b"SFN1:PING:";
pub const PONG: &[u8] = b"SFN1:PONG:";
/// Steam's EResult for success.
pub const STEAM_OK: i32 = 1;

/// The native service statistics (steam_network.cpp:431-442).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Stats {
    pub probes_sent: u64,
    pub probe_send_failures: u64,
    pub probes_acknowledged: u64,
    pub last_acknowledged_peer: u64,
    pub last_round_trip_ms: u64,
    /// The EResult of the last health message sent (a probe or an answer); -1: the lease refused it.
    pub last_send_result: i32,
    pub probe_timeouts: u64,
}

/// What `report_network_health` remembers between passes.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
struct Reported {
    acks: u64,
    send_failures: u64,
    timeouts: u64,
    /// 0 never reported, 1 ready, 2 unavailable, 3 waiting for the session callback.
    state: u8,
    outage: bool,
    acknowledged_peer: u64,
    last_failure_ms: Option<u64>,
}

/// One log line: (Kiln log level, text).
pub type Line = (i32, String);

/// The health check's state for one process.
#[derive(Clone, Debug, Default)]
pub struct Health {
    /// nonce -> (peer, sent at).
    pending: BTreeMap<u64, (u64, u64)>,
    nonce: u64,
    last_probe_ms: u64,
    remote: Option<(u64, u64)>,
    stats: Stats,
    reported: Reported,
}

/// ShroudForge's `std::from_chars` into a u64 that must use the whole text: digits only (no sign, no space), no
/// overflow, not empty.
pub fn parse_nonce(text: &[u8]) -> Option<u64> {
    if text.is_empty() || !text.iter().all(u8::is_ascii_digit) {
        return None;
    }
    text.iter().try_fold(0u64, |value, digit| {
        value.checked_mul(10)?.checked_add(u64::from(digit - b'0'))
    })
}

impl Health {
    pub fn stats(&self) -> Stats {
        self.stats
    }
    /// Pending probes: nonce -> (peer, sent at).
    pub fn pending(&self) -> &BTreeMap<u64, (u64, u64)> {
        &self.pending
    }

    /// One message taken off the channel from `peer` at `now`; the answer to send back to `peer`, if any.
    pub fn on_message(
        &mut self,
        server: bool,
        peer: u64,
        payload: &[u8],
        now: u64,
    ) -> Option<Vec<u8>> {
        if payload.is_empty() || payload.len() > MAX_PAYLOAD || peer == 0 {
            return None;
        }
        if let Some(nonce) = payload.strip_prefix(PING) {
            if !server {
                self.remote = Some((peer, now));
            }
            return Some([PONG, nonce].concat());
        }
        if server
            && let Some(text) = payload.strip_prefix(PONG)
            && let Some(nonce) = parse_nonce(text)
            && let Some(&(to, sent_at)) = self.pending.get(&nonce)
            && to == peer
        {
            self.stats.probes_acknowledged += 1;
            self.stats.last_acknowledged_peer = peer;
            self.stats.last_round_trip_ms = now.wrapping_sub(sent_at);
            self.pending.remove(&nonce);
        }
        None
    }

    /// A health message was sent: Steam's EResult (-1: the lease refused it).
    pub fn sent(&mut self, result: i32) {
        self.stats.last_send_result = result;
    }

    /// Pending probes older than 15,000 ms expire.
    pub fn expire(&mut self, now: u64) {
        let before = self.pending.len();
        self.pending
            .retain(|_, (_, at)| !(now >= *at && now - *at > PROBE_MAX_AGE_MS));
        self.stats.probe_timeouts += (before - self.pending.len()) as u64;
    }

    /// A server's probe round is due (5,000 ms or more since the last one): it starts now.
    pub fn probe_due(&mut self, server: bool, now: u64) -> bool {
        if !server || now.wrapping_sub(self.last_probe_ms) < PROBE_INTERVAL_MS {
            return false;
        }
        self.last_probe_ms = now;
        true
    }

    /// The next probe's nonce (counting from 1 for the process).
    pub fn next_nonce(&mut self) -> u64 {
        self.nonce += 1;
        self.nonce
    }

    /// The probe `nonce` to `peer` was sent at `now`; Steam answered `result`.
    pub fn probed(&mut self, nonce: u64, peer: u64, now: u64, result: i32) {
        self.sent(result);
        if result == STEAM_OK {
            self.stats.probes_sent += 1;
            if self.pending.len() < PENDING_CAP {
                self.pending.insert(nonce, (peer, now));
            }
        } else {
            self.stats.probe_send_failures += 1;
        }
    }

    /// The last PING sender, while it is at most 15 s old (a client's remote dedicated server).
    pub fn remote_server(&self, now: u64) -> Option<u64> {
        let (id, at) = self.remote?;
        (at != 0 && now >= at && now - at <= PROBE_MAX_AGE_MS && id != 0).then_some(id)
    }

    /// `report_network_health` after a periodic pass: the lines to log, in order.
    pub fn report(
        &mut self,
        available: bool,
        service_ready: bool,
        server: bool,
        now: u64,
    ) -> Vec<Line> {
        use kiln_abi::{KILN_LOG_INFO as INFO, KILN_LOG_WARN as WARN};
        let mut lines = Vec::new();
        let line = |text: &str, fields: String| format!("shroudforge::network: {text} ({fields})");
        let state = if !available {
            2
        } else if service_ready {
            1
        } else {
            3
        };
        if std::mem::replace(&mut self.reported.state, state) != state {
            if state == 1 {
                lines.push(if server {
                    (INFO, line("Network service is ready. Automatic Steam session acceptance and authenticated-peer health probes are active", "role=server".into()))
                } else {
                    (INFO, line("Network service is ready. Automatic Steam session acceptance is active and remote server discovery is listening", "role=client".into()))
                });
            } else {
                lines.push((
                    WARN,
                    line(
                        "Network service is waiting for Steam Networking Messages",
                        format!("available={available}, service_ready={service_ready}"),
                    ),
                ));
            }
        }
        let stats = self.stats;
        let previous_acks = std::mem::replace(&mut self.reported.acks, stats.probes_acknowledged);
        let previous_failures =
            std::mem::replace(&mut self.reported.send_failures, stats.probe_send_failures);
        if stats.probe_send_failures > previous_failures && self.failure_started(now) {
            // ShroudForge reports the last send's result only once anything was sent.
            let result = if stats.probes_sent > 0 || stats.probe_send_failures > 0 {
                stats.last_send_result
            } else {
                0
            };
            lines.push((
                WARN,
                line(
                    "Steam P2P health probe send failed. Check the peer identity and Steam session state",
                    format!("result={result}, total_failures={}", stats.probe_send_failures),
                ),
            ));
        }
        let previous_timeouts =
            std::mem::replace(&mut self.reported.timeouts, stats.probe_timeouts);
        if stats.probe_timeouts > previous_timeouts && self.failure_started(now) {
            lines.push((
                WARN,
                line(
                    "Steam P2P health probe received no acknowledgement for 15 seconds",
                    format!(
                        "total_timeouts={}, timeout_seconds=15",
                        stats.probe_timeouts
                    ),
                ),
            ));
        }
        if stats.probes_acknowledged > previous_acks {
            let peer_id = stats.last_acknowledged_peer;
            let peer = if peer_id != 0 {
                peer_id.to_string()
            } else {
                "unknown".to_owned()
            };
            let previous_peer = std::mem::replace(&mut self.reported.acknowledged_peer, peer_id);
            let recovered = self.recovered(now);
            if recovered || previous_acks == 0 || (peer_id != 0 && peer_id != previous_peer) {
                let round_trip = if peer_id != 0 {
                    stats.last_round_trip_ms
                } else {
                    0
                };
                lines.push((
                    INFO,
                    line(
                        "Steam P2P health probe confirmed",
                        format!(
                            "peer={peer}, round_trip_ms={round_trip}, total_acknowledged={}",
                            stats.probes_acknowledged
                        ),
                    ),
                ));
            }
        }
        lines
    }

    /// `network_probe_failure_started`: true for the first failure of an outage.
    fn failure_started(&mut self, now: u64) -> bool {
        self.reported.last_failure_ms = Some(now);
        !std::mem::replace(&mut self.reported.outage, true)
    }
    /// `network_probe_recovered`: 15 s or more since the last failure ends the outage.
    fn recovered(&mut self, now: u64) -> bool {
        let recovered = self
            .reported
            .last_failure_ms
            .is_some_and(|last| now.saturating_sub(last) >= PROBE_MAX_AGE_MS);
        if recovered {
            self.reported.last_failure_ms = None;
            std::mem::replace(&mut self.reported.outage, false)
        } else {
            false
        }
    }
}
