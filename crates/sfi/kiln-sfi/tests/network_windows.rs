// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family, Windows tests.
//! Native Windows (and Wine): the same-PC mapping `Local\ShroudForge.Network.ServerIdentity.v1` between two
//! processes with GetTickCount64 freshness, and the dedicated server log follower against a writer process that keeps
//! the log open, rotates it by rename and deletes the old one, which it must never block. Behaviour mirrored from
//! ShroudForge (MIT, Copyright (c) 2026 bonsaibauer): modules/transport/native/steam_network.cpp:33-68,212-246,450-482.
//! The child processes are this test binary again (`--ignored --exact`), told their part through the environment.
#![cfg(windows)]

use kiln_sfi::runtime::network::authorization::full_scan;
use kiln_sfi::runtime::network::discovery::{Discovery, MAPPING_NAME, NamedMapping, tick_ms};
use kiln_sfi::runtime::network::log_follow::Follower;
use std::io::Write;
use std::os::windows::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::time::{Duration, Instant};

const ROLE: &str = "KILN_SFI_V4R3_ROLE";
const DIR: &str = "KILN_SFI_V4R3_DIR";
const AGE: &str = "KILN_SFI_V4R3_AGE";
const ID: u64 = 76_561_198_123_456_789;
/// A bound for what must happen; a passing wait returns as soon as it holds.
const WAIT: Duration = Duration::from_secs(300);

fn fresh(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("kiln-sfi-v4r3-win-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}
fn child(test: &str, role: &str, dir: &Path, age: u64) -> Child {
    Command::new(std::env::current_exe().unwrap())
        .args([
            "--ignored",
            "--exact",
            test,
            "--nocapture",
            "--test-threads=1",
        ])
        .env(ROLE, role)
        .env(DIR, dir)
        .env(AGE, age.to_string())
        .spawn()
        .unwrap()
}
fn wait_for(path: &Path) -> bool {
    let start = Instant::now();
    while start.elapsed() < WAIT {
        if path.exists() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    false
}

/// The publisher process: publishes with a time `age` ms in the past, says so, waits for "go".
#[test]
#[ignore = "a child process of the_mapping_reaches_another_process_while_it_is_fresh"]
fn child_publisher() {
    if std::env::var(ROLE).as_deref() != Ok("publisher") {
        return;
    }
    let dir = PathBuf::from(std::env::var_os(DIR).unwrap());
    let age: u64 = std::env::var(AGE).unwrap().parse().unwrap();
    let mapping = NamedMapping::default();
    mapping.publish(ID, tick_ms() - age);
    std::fs::write(dir.join("ready"), b"").unwrap();
    assert!(wait_for(&dir.join("go")));
}

#[test]
fn the_mapping_reaches_another_process_while_it_is_fresh() {
    assert_eq!(MAPPING_NAME, "Local\\ShroudForge.Network.ServerIdentity.v1");
    let reader = NamedMapping::default();
    assert_eq!(reader.read(tick_ms()), None, "nobody publishes yet");
    for (age, expected) in [(0, Some(ID)), (9_000, Some(ID)), (10_500, None)] {
        let dir = fresh(&format!("map-{age}"));
        let mut publisher = child("child_publisher", "publisher", &dir, age);
        assert!(wait_for(&dir.join("ready")), "the publisher started");
        // Both processes share GetTickCount64: the age the publisher wrote is the age read here.
        assert_eq!(reader.read(tick_ms()), expected, "age {age}");
        assert_eq!(reader.read(tick_ms() + 20_000), None, "too old later on");
        std::fs::write(dir.join("go"), b"").unwrap();
        assert!(publisher.wait().unwrap().success());
        // The publisher's handle went with its process: the name is gone.
        assert_eq!(reader.read(tick_ms()), None, "after the publisher ended");
        let _ = std::fs::remove_dir_all(&dir);
    }
    assert!(tick_ms() > 0);
}

const ROUNDS: u32 = 12;

fn line(round: u32, i: u32) -> String {
    let id = ID + u64::from(round);
    match i % 4 {
        0 => format!("[online] Added peer r{round}-{i} (steamid:{id}) from x\r\n"),
        1 => format!("[server] Client '{id}' authenticated by steam\r\n"),
        2 => format!("[server] noise {round} {i}\r\n"),
        _ => format!("[online] Removed peer r{round}-{} left\r\n", i - 3),
    }
}

/// The game-like writer: each round it creates the log (sharing read only, as a plain logger does), appends lines while
/// it holds the file open, closes it, renames it to `.1` over the previous one and deletes that every other round.
/// Every rename and delete must succeed at once while the follower reads; the last round's log stays.
#[test]
#[ignore = "a child process of the_follower_never_blocks_a_writer_that_rotates_the_log"]
fn child_writer() {
    if std::env::var(ROLE).as_deref() != Ok("writer") {
        return;
    }
    let dir = PathBuf::from(std::env::var_os(DIR).unwrap());
    let log = dir.join("enshrouded_server.log");
    let old = dir.join("enshrouded_server.log.1");
    for round in 0..ROUNDS {
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .share_mode(0x1)
            .open(&log)
            .unwrap_or_else(|e| panic!("round {round}: create: {e}"));
        // The last two lines add a peer that stays: each round's log ends with exactly one.
        for i in 0..42 {
            file.write_all(line(round, i).as_bytes()).unwrap();
            file.flush().unwrap();
            std::thread::sleep(Duration::from_millis(1));
        }
        drop(file);
        if round + 1 == ROUNDS {
            break;
        }
        std::fs::rename(&log, &old).unwrap_or_else(|e| panic!("round {round}: rename: {e}"));
        if round % 2 == 1 {
            std::fs::remove_file(&old).unwrap_or_else(|e| panic!("round {round}: delete: {e}"));
        }
    }
    std::fs::write(dir.join("done"), b"").unwrap();
}

#[test]
fn the_follower_never_blocks_a_writer_that_rotates_the_log() {
    let dir = fresh("rotate");
    let log = dir.join("enshrouded_server.log");
    let mut follower = Follower::new(log.clone());
    let mut writer = child("child_writer", "writer", &dir, 0);
    let mut passes = 0u64;
    let start = Instant::now();
    while !dir.join("done").exists() && start.elapsed() < WAIT {
        follower.pass();
        passes += 1;
        if let Ok(Some(_)) = writer.try_wait() {
            break;
        }
    }
    let status = writer.wait().unwrap();
    assert!(
        status.success(),
        "the writer was never blocked ({passes} passes)"
    );
    assert!(
        passes > u64::from(ROUNDS),
        "the follower read while the writer wrote ({passes} passes)"
    );
    follower.pass();
    let last = std::fs::read(&log).unwrap();
    assert_eq!(follower.peers(), full_scan(&last));
    assert_eq!(follower.peers(), [ID + u64::from(ROUNDS - 1)]);
    assert!(
        follower.rescans() >= 2,
        "it started over on rotation ({} rescans, {passes} passes)",
        follower.rescans()
    );
    let _ = std::fs::remove_dir_all(&dir);
}
