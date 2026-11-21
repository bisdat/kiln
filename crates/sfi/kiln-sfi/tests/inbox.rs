// SPDX-License-Identifier: MIT
//! The worker inbox. Producers never block; event classes refuse and count
//! on overflow; state classes coalesce to the latest value; world lifecycle is
//! never dropped. The stress tests here also run under ThreadSanitizer
//! (`-Zsanitizer=thread`).
use kiln_sfi::worker::*;
use std::time::{Duration, Instant};

fn ui(n: usize) -> WorkerCommand {
    WorkerCommand::UiAction(UiAction {
        package: format!("p{}", n % 3),
        action: n.to_string(),
        payload: n.to_le_bytes().to_vec(),
    })
}
fn config(package: &str, value: u8) -> WorkerCommand {
    WorkerCommand::ConfigurationObservation(ConfigurationObservation {
        package: package.into(),
        payload: vec![value],
    })
}
fn input(value: u8) -> WorkerCommand {
    WorkerCommand::InputSnapshot(InputSnapshot {
        payload: vec![value],
    })
}
fn lifecycle(world: u32, session: u32, event: LifecycleEvent) -> WorkerCommand {
    WorkerCommand::WorldLifecycle(WorldLifecycle {
        world,
        session,
        event,
    })
}
fn small() -> Inbox {
    Inbox::new(InboxConfig {
        ui_actions: 8,
        configuration_keys: 4,
    })
}
fn drain(consumer: &mut Consumer<'_>) -> Vec<WorkerCommand> {
    let mut out = Vec::new();
    consumer.drain(|c| out.push(c));
    out
}

#[test]
fn event_classes_refuse_with_a_counted_drop_when_full() {
    let inbox = small();
    for n in 0..8 {
        assert_eq!(inbox.push(ui(n)), Ok(Accepted::Queued));
    }
    for n in 8..20 {
        assert_eq!(
            inbox.push(ui(n)),
            Err(Refused::Full(CommandClass::UiAction))
        );
    }
    let counters = inbox.counters();
    assert_eq!(counters.accepted[CommandClass::UiAction as usize], 8);
    assert_eq!(counters.refused[CommandClass::UiAction as usize], 12);
    let mut consumer = inbox.consumer().unwrap();
    let drained = drain(&mut consumer);
    assert_eq!(
        drained,
        (0..8).map(ui).collect::<Vec<_>>(),
        "FIFO, nothing lost"
    );
    assert_eq!(
        inbox.push(ui(99)),
        Ok(Accepted::Queued),
        "space again after a drain"
    );
    // The network class is a state class (the log follower's latest authorization): it
    // never refuses, the newest replaces what is pending, and it is drained before the UI ring.
    let auth = |generation| {
        WorkerCommand::NetworkAuth(NetworkAuth {
            generation,
            readable: true,
            peers: vec![generation],
        })
    };
    assert_eq!(inbox.push(auth(1)), Ok(Accepted::Queued));
    for generation in 2..100 {
        assert_eq!(inbox.push(auth(generation)), Ok(Accepted::Coalesced));
    }
    let counters = inbox.counters();
    assert_eq!(counters.accepted[CommandClass::NetworkAuth as usize], 1);
    assert_eq!(counters.coalesced[CommandClass::NetworkAuth as usize], 98);
    assert_eq!(counters.refused[CommandClass::NetworkAuth as usize], 0);
    assert_eq!(drain(&mut consumer), vec![auth(99), ui(99)]);
    assert_eq!(
        COMMAND_CLASSES, 7,
        "NetworkAuth replaced NetworkInbound; the live reload added ReloadRequest"
    );
}

#[test]
fn state_classes_coalesce_to_the_latest_value_per_key() {
    let inbox = small();
    assert_eq!(inbox.push(input(0)), Ok(Accepted::Queued));
    for value in 1..100 {
        assert_eq!(inbox.push(input(value)), Ok(Accepted::Coalesced));
    }
    assert_eq!(inbox.push(config("a", 1)), Ok(Accepted::Queued));
    assert_eq!(inbox.push(config("b", 1)), Ok(Accepted::Queued));
    assert_eq!(inbox.push(config("a", 2)), Ok(Accepted::Coalesced));
    assert_eq!(inbox.push(config("a", 3)), Ok(Accepted::Coalesced));
    let mut consumer = inbox.consumer().unwrap();
    let drained = drain(&mut consumer);
    assert_eq!(
        drained,
        vec![config("b", 1), config("a", 3), input(99)],
        "newest per key, oldest first; one input snapshot"
    );
    let counters = inbox.counters();
    assert_eq!(counters.coalesced[CommandClass::InputSnapshot as usize], 99);
    assert_eq!(
        counters.coalesced[CommandClass::ConfigurationObservation as usize],
        2
    );
    assert!(drain(&mut consumer).is_empty());
    // A key table full of other packages refuses (counted) instead of evicting.
    for key in ["c", "d"] {
        assert_eq!(inbox.push(config(key, 0)), Ok(Accepted::Queued));
    }
    assert_eq!(
        inbox.push(config("e", 0)),
        Err(Refused::Full(CommandClass::ConfigurationObservation))
    );
    assert_eq!(
        inbox.push(config("a", 4)),
        Ok(Accepted::Queued),
        "known keys still coalesce"
    );
}

#[test]
fn world_lifecycle_is_never_dropped_and_yields_at_most_two_per_world() {
    let inbox = small();
    // Fill every event ring first: lifecycle has its own slots.
    while inbox.push(ui(0)).is_ok() {}
    let mut consumer = inbox.consumer().unwrap();
    let mut last_live = None;
    for session in 1..=500u32 {
        for event in [LifecycleEvent::Started, LifecycleEvent::Stopping] {
            assert!(
                inbox.push(lifecycle(0, session, event)).is_ok(),
                "never refused"
            );
        }
        assert!(
            inbox
                .push(lifecycle(2, session, LifecycleEvent::Started))
                .is_ok()
        );
        if session % 97 == 0 {
            let events: Vec<_> = drain(&mut consumer)
                .into_iter()
                .filter_map(|c| match c {
                    WorkerCommand::WorldLifecycle(e) => Some(e),
                    _ => None,
                })
                .collect();
            let server: Vec<_> = events.iter().filter(|e| e.world == 0).collect();
            let client: Vec<_> = events.iter().filter(|e| e.world == 2).collect();
            assert!(server.len() <= 2 && client.len() <= 2, "{events:?}");
            assert_eq!(
                server.last().map(|e| (e.session, e.event)),
                Some((session, LifecycleEvent::Stopping))
            );
            // The client session the consumer last reported live is ended
            // explicitly before the newer one starts.
            if let Some(previous) = last_live {
                assert_eq!(
                    client.first().map(|e| (e.session, e.event)),
                    Some((previous, LifecycleEvent::Stopping))
                );
            }
            assert_eq!(
                client.last().map(|e| (e.session, e.event)),
                Some((session, LifecycleEvent::Started))
            );
            last_live = Some(session);
        }
    }
    assert_eq!(
        inbox.counters().refused[CommandClass::WorldLifecycle as usize],
        0
    );
    assert_eq!(
        inbox.push(lifecycle(3, 1, LifecycleEvent::Started)),
        Err(Refused::Argument),
        "world kind out of range"
    );
    assert_eq!(
        inbox.push(lifecycle(0, 0, LifecycleEvent::Started)),
        Err(Refused::Argument)
    );
}

/// The lifecycle slot keeps the newest event, not the last one to arrive: an
/// event whose producer was preempted and arrives late never hides a newer one.
#[test]
fn a_late_lifecycle_event_never_hides_a_newer_one() {
    use LifecycleEvent::{Started, Stopping};
    let inbox = small();
    let mut consumer = inbox.consumer().unwrap();
    let mut events = || {
        drain(&mut consumer)
            .into_iter()
            .map(|c| match c {
                WorkerCommand::WorldLifecycle(e) => (e.session, e.event),
                other => panic!("unexpected {other:?}"),
            })
            .collect::<Vec<_>>()
    };
    inbox.push(lifecycle(0, 1, Started)).unwrap();
    assert_eq!(events(), [(1, Started)]);
    // Session 1 ends and session 2 starts, but 1's STOPPING arrives after 2's STARTED.
    assert_eq!(inbox.push(lifecycle(0, 2, Started)), Ok(Accepted::Queued));
    assert_eq!(
        inbox.push(lifecycle(0, 1, Stopping)),
        Ok(Accepted::Superseded)
    );
    assert_eq!(events(), [(1, Stopping), (2, Started)]);
    // Session 2 ends; a late duplicate STARTED of 2 does not reopen it.
    inbox.push(lifecycle(0, 2, Stopping)).unwrap();
    inbox.push(lifecycle(0, 2, Started)).unwrap();
    assert_eq!(events(), [(2, Stopping)]);
    // Arriving after the drain, an already superseded event is not delivered again.
    inbox.push(lifecycle(0, 2, Started)).unwrap();
    assert!(events().is_empty());
    assert_eq!(
        inbox.counters().refused[CommandClass::WorldLifecycle as usize],
        0
    );
}

#[test]
fn only_one_consumer_per_inbox() {
    let inbox = small();
    {
        let first = inbox.consumer();
        assert!(first.is_some());
        assert!(inbox.consumer().is_none());
    }
    assert!(
        inbox.consumer().is_none(),
        "the claim is for the inbox's life"
    );
}

/// A producer never blocks: four producer threads against an inbox nobody drains
/// (a parked worker). Every push returns at once; overflow is a counted refusal.
/// Plain threads, not a scope: a blocked producer fails the test instead of
/// hanging its join.
#[test]
fn producers_never_block_against_a_parked_worker() {
    let inbox = std::sync::Arc::new(small());
    const PER_PRODUCER: usize = 20_000;
    let (tx, rx) = std::sync::mpsc::channel();
    let mut producers = Vec::new();
    for producer in 0..4u8 {
        let inbox = inbox.clone();
        let tx = tx.clone();
        producers.push(std::thread::spawn(move || {
            let mut slowest = Duration::ZERO;
            for n in 0..PER_PRODUCER {
                let command = match n % 4 {
                    0 => ui(n),
                    1 => config(&format!("k{}", n % 7), producer),
                    2 => input(producer),
                    _ => lifecycle(
                        u32::from(producer % 3),
                        n as u32 + 1,
                        LifecycleEvent::Started,
                    ),
                };
                let at = Instant::now();
                let _ = inbox.push(command);
                slowest = slowest.max(at.elapsed());
            }
            tx.send(slowest).unwrap();
        }));
    }
    drop(tx);
    for _ in 0..4 {
        let slowest = rx
            // a liveness bound, not a latency claim: a passing receive returns as soon as the producer reports
            .recv_timeout(Duration::from_secs(300))
            .expect("a producer blocked against the parked worker");
        // Blocking is caught by the receive timeout above; this bound only flags gross
        // stalls (ThreadSanitizer alone has shown ~0.5 s under contention and load).
        assert!(slowest < Duration::from_secs(2), "a push took {slowest:?}");
    }
    // Every producer reported, so none is blocked: join them, so the last Arc drop
    // is this thread's (a cross-thread last drop frees through a fence that
    // uninstrumented std hides from ThreadSanitizer).
    for producer in producers {
        producer.join().unwrap();
    }
    let counters = inbox.counters();
    let ui = CommandClass::UiAction as usize;
    assert_eq!(counters.accepted[ui], 8, "the ring took its capacity");
    assert_eq!(counters.refused[ui], PER_PRODUCER as u64 - 8);
    assert_eq!(counters.refused[CommandClass::WorldLifecycle as usize], 0);
    assert_eq!(counters.refused[CommandClass::InputSnapshot as usize], 0);
}

/// Four producers and the consumer concurrently: every accepted event is delivered
/// exactly once and in each producer's order; refusals are exactly the counted ones.
#[test]
fn concurrent_producers_and_consumer_lose_and_duplicate_nothing() {
    let inbox = small();
    const PER_PRODUCER: u32 = 20_000;
    let mut accepted = [0u64; 4];
    let mut delivered: Vec<(u8, u32)> = Vec::new();
    let mut inputs = 0usize;
    std::thread::scope(|scope| {
        let handles: Vec<_> = (0..4u8)
            .map(|producer| {
                let inbox = &inbox;
                scope.spawn(move || {
                    let mut ok = 0u64;
                    for n in 0..PER_PRODUCER {
                        let mut payload = vec![producer];
                        payload.extend_from_slice(&n.to_le_bytes());
                        let command = WorkerCommand::UiAction(UiAction {
                            package: String::new(),
                            action: String::new(),
                            payload,
                        });
                        ok += inbox.push(command).is_ok() as u64;
                        let _ = inbox.push(input(producer));
                        if n % 64 == 0 {
                            std::thread::yield_now();
                        }
                    }
                    ok
                })
            })
            .collect();
        let consumer = scope.spawn(|| {
            let mut consumer = inbox.consumer().unwrap();
            let mut seen = Vec::new();
            let mut inputs = 0;
            let mut idle_after_done = 0;
            loop {
                let mut got = 0;
                consumer.drain(|c| {
                    got += 1;
                    match c {
                        WorkerCommand::UiAction(a) => seen.push((
                            a.payload[0],
                            u32::from_le_bytes(a.payload[1..5].try_into().unwrap()),
                        )),
                        WorkerCommand::InputSnapshot(_) => inputs += 1,
                        other => panic!("unexpected {other:?}"),
                    }
                });
                let produced: u64 = inbox.counters().accepted[CommandClass::UiAction as usize]
                    + inbox.counters().refused[CommandClass::UiAction as usize];
                if produced == 4 * u64::from(PER_PRODUCER) && got == 0 {
                    idle_after_done += 1;
                    if idle_after_done > 3 {
                        break;
                    }
                }
                std::thread::yield_now();
            }
            (seen, inputs)
        });
        for (i, h) in handles.into_iter().enumerate() {
            accepted[i] = h.join().unwrap();
        }
        (delivered, inputs) = consumer.join().unwrap();
    });
    let counters = inbox.counters();
    let ui = CommandClass::UiAction as usize;
    assert_eq!(accepted.iter().sum::<u64>(), counters.accepted[ui]);
    assert_eq!(
        delivered.len() as u64,
        counters.accepted[ui],
        "every accepted event delivered exactly once"
    );
    assert_eq!(counters.delivered[ui], counters.accepted[ui]);
    for producer in 0..4u8 {
        let order: Vec<u32> = delivered
            .iter()
            .filter(|(p, _)| *p == producer)
            .map(|(_, n)| *n)
            .collect();
        assert_eq!(order.len() as u64, accepted[producer as usize]);
        assert!(
            order.windows(2).all(|w| w[0] < w[1]),
            "producer {producer} order kept"
        );
    }
    assert!(inputs >= 1);
    eprintln!(
        "stress: accepted {} refused {} input coalesced {}",
        counters.accepted[ui],
        counters.refused[ui],
        counters.coalesced[CommandClass::InputSnapshot as usize]
    );
}
