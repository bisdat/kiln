// SPDX-License-Identifier: MIT
//! Native Windows: sfi.json's replacing rename while another process-style reader holds the old file open.
//! A reader that does not share delete (.NET's FileShare.Read/ReadWrite, most launchers) makes Windows refuse the
//! rename; the writer's retry (launch.json's 20 × 25 ms) gets through once the reader lets go, and when it does not,
//! the old file stays whole and no temp file is left. Run it natively on Windows.
#![cfg(windows)]

use kiln_sfi::state::writer::{self, WriteOps};
use std::os::windows::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Barrier};
use std::time::{Duration, Instant};

const FILE_SHARE_READ: u32 = 0x1;
const FILE_SHARE_WRITE: u32 = 0x2;
const FILE_SHARE_DELETE: u32 = 0x4;

fn fresh(name: &str) -> PathBuf {
    let dir =
        std::env::temp_dir().join(format!("kiln-sfi-state-win-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}
fn temps(dir: &Path) -> Vec<String> {
    std::fs::read_dir(dir)
        .unwrap()
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.ends_with(".tmp"))
        .collect()
}
fn reader(path: &Path, share: u32) -> std::fs::File {
    std::fs::OpenOptions::new()
        .read(true)
        .share_mode(share)
        .open(path)
        .unwrap()
}

#[test]
fn a_reader_that_does_not_share_delete_holds_the_rename_and_the_retry_gets_through() {
    let dir = fresh("held");
    let file = dir.join(writer::FILE);
    std::fs::write(&file, b"{\"old\":true}\n").unwrap();
    // the rename is refused while it is open (one plain try shows it)
    let held = reader(&file, FILE_SHARE_READ | FILE_SHARE_WRITE);
    assert!(
        writer::write_once(&dir, b"{\"try\":1}\n", &WriteOps::REAL).is_err(),
        "Windows refuses the rename"
    );
    assert!(
        temps(&dir).is_empty(),
        "the refused try removed its temp file"
    );
    // held for 150 ms, then let go: the retry gets through
    let release = std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(150));
        drop(held);
    });
    let started = Instant::now();
    let tries = writer::write_retrying(&dir, b"{\"new\":true}\n", &WriteOps::REAL)
        .expect("through after the reader let go");
    release.join().unwrap();
    println!(
        "held 150 ms: written on try {tries} after {:?}",
        started.elapsed()
    );
    assert!(
        tries >= 2,
        "the reader blocked at least the first try: {tries}"
    );
    assert_eq!(std::fs::read(&file).unwrap(), b"{\"new\":true}\n");
    assert!(temps(&dir).is_empty());
}

#[test]
fn a_reader_that_holds_past_the_retry_leaves_the_old_file_whole() {
    let dir = fresh("past");
    let file = dir.join(writer::FILE);
    std::fs::write(&file, b"{\"old\":true}\n").unwrap();
    let held = reader(&file, FILE_SHARE_READ | FILE_SHARE_WRITE);
    let started = Instant::now();
    let error = writer::write_retrying(&dir, b"{\"new\":true}\n", &WriteOps::REAL).unwrap_err();
    let took = started.elapsed();
    println!(
        "held throughout: gave up after {took:?}: {error} ({:?})",
        error.raw_os_error()
    );
    assert!(
        took >= Duration::from_millis(19 * 25),
        "20 tries, 25 ms apart: {took:?}"
    );
    assert!(took < Duration::from_secs(5), "{took:?}");
    assert_eq!(
        std::fs::read(&file).unwrap(),
        b"{\"old\":true}\n",
        "the old file stays whole"
    );
    assert!(
        temps(&dir).is_empty(),
        "no temp file is left: {:?}",
        temps(&dir)
    );
    drop(held);
    assert_eq!(
        writer::write_retrying(&dir, b"{\"new\":true}\n", &WriteOps::REAL).unwrap(),
        1
    );
}

#[test]
fn a_reader_that_shares_delete_never_blocks_it() {
    let dir = fresh("shared");
    let file = dir.join(writer::FILE);
    std::fs::write(&file, b"{\"old\":true}\n").unwrap();
    let held = reader(
        &file,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    );
    let tries = writer::write_retrying(&dir, b"{\"new\":true}\n", &WriteOps::REAL);
    println!("reader sharing delete: {tries:?}");
    drop(held);
    assert!(tries.is_ok(), "{tries:?}");
    assert_eq!(std::fs::read(&file).unwrap(), b"{\"new\":true}\n");
}

#[test]
fn a_polling_launcher_never_sees_a_partial_file_and_never_loses_a_write() {
    // a launcher-style reader opens, reads whole and closes the file in a loop; 300 writes go through it
    let dir = fresh("poll");
    let file = dir.join(writer::FILE);
    std::fs::write(&file, b"{\"n\":-1}\n").unwrap();
    let stop = Arc::new(AtomicBool::new(false));
    let reads = Arc::new(AtomicU32::new(0));
    let barrier = Arc::new(Barrier::new(2));
    let poller = {
        let (file, stop, reads, barrier) =
            (file.clone(), stop.clone(), reads.clone(), barrier.clone());
        std::thread::spawn(move || {
            barrier.wait();
            while !stop.load(Ordering::Acquire) {
                let Ok(mut f) = std::fs::OpenOptions::new()
                    .read(true)
                    .share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE)
                    .open(&file)
                else {
                    continue;
                };
                let mut bytes = Vec::new();
                std::io::Read::read_to_end(&mut f, &mut bytes).unwrap();
                drop(f);
                let doc: serde_json::Value =
                    serde_json::from_slice(&bytes).expect("always one whole document");
                assert!(doc["n"].is_i64());
                reads.fetch_add(1, Ordering::Relaxed);
            }
        })
    };
    barrier.wait();
    let (mut retried, mut most) = (0, 0);
    for n in 0..300 {
        let tries = writer::write_retrying(
            &dir,
            format!("{{\"n\":{n},\"pad\":\"{}\"}}\n", "x".repeat(2048)).as_bytes(),
            &WriteOps::REAL,
        )
        .unwrap_or_else(|e| panic!("write {n} lost: {e}"));
        if tries > 1 {
            retried += 1;
        }
        most = most.max(tries);
    }
    stop.store(true, Ordering::Release);
    poller.join().unwrap();
    println!(
        "300 writes under a polling reader ({} reads): {retried} needed a retry, at most {most} tries",
        reads.load(Ordering::Relaxed)
    );
    let last: serde_json::Value = serde_json::from_slice(&std::fs::read(&file).unwrap()).unwrap();
    assert_eq!(last["n"], 299);
    assert!(temps(&dir).is_empty());
}
