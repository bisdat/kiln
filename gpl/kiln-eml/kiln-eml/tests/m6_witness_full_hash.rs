// SPDX-License-Identifier: GPL-3.0-or-later
//! `KILN_EML_FULL_HASH=1`, the change cache's escape hatch ("every use of the stamp off", docs/kiln/eml.md),
//! turns the startup witness's stamps off too: every input is hashed at the capture and again at every check. A binary
//! of its own: the variable is process-wide, and m6_witness_stamps proves the stamped checks in parallel tests.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;

use fixture::{TestRoot, archive::STEM};
use mod_loader::lua::startup_host::InputWitness;
use std::{fs, path::Path, time::Duration};

const MIB: usize = 1 << 20;

#[test]
fn full_hash_requested_hashes_every_large_input_at_every_check() {
    // SAFETY: this binary's only test sets it before any witness exists; no other thread reads the environment.
    unsafe { std::env::set_var("KILN_EML_FULL_HASH", "1") };
    let root = TestRoot::new("g13p-full-hash");
    let dir = root.0.join("game").into_std_path_buf();
    fs::create_dir(&dir).unwrap();
    let file = |name: &str| dir.join(format!("{STEM}{name}"));
    for (name, len) in [(".kfc", 4096), (".kfc.bak", 4096), (".kfc.bak.integrity", 64)] {
        fs::write(file(name), vec![7u8; len]).unwrap();
    }
    let large = [".kfc_resources", ".kfc_resources.bak", "_000.dat", "_001.dat"].map(file);
    for path in &large {
        fs::write(path, vec![9u8; 8 * MIB]).unwrap();
    }
    // longer than the racy window: without the variable each large file would be stamped (m6_witness_stamps)
    std::thread::sleep(Duration::from_millis(3300));
    let w = InputWitness::capture(&dir, STEM, 2).unwrap();
    for _ in 0..2 {
        hashed(&w, &dir, &large);
    }
}

/// A check reads every large file: Linux counts the bytes; on Windows a large file another handle holds with no
/// sharing cannot be checked any more (a stamp would need no data access).
#[cfg(target_os = "linux")]
fn hashed(w: &InputWitness, dir: &Path, large: &[std::path::PathBuf]) {
    let rchar = || -> u64 {
        let io = fs::read_to_string("/proc/thread-self/io").unwrap();
        io.lines().find_map(|l| l.strip_prefix("rchar: ")).unwrap().trim().parse().unwrap()
    };
    let before = rchar();
    w.check(dir).unwrap();
    let read = rchar() - before;
    assert!(read >= (large.len() * 8 * MIB) as u64, "a check read {read} bytes");
}
#[cfg(windows)]
fn hashed(w: &InputWitness, dir: &Path, large: &[std::path::PathBuf]) {
    use std::os::windows::fs::OpenOptionsExt;
    w.check(dir).unwrap();
    for path in large {
        let held = fs::OpenOptions::new().read(true).write(true).share_mode(0).open(path).unwrap();
        assert!(w.check(dir).is_err(), "{} is read by the check", path.display());
        drop(held);
    }
    w.check(dir).unwrap();
}
#[cfg(not(any(target_os = "linux", windows)))]
fn hashed(w: &InputWitness, dir: &Path, _large: &[std::path::PathBuf]) {
    w.check(dir).unwrap();
}
