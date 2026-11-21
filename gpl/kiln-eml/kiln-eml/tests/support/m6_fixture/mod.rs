// SPDX-License-Identifier: GPL-3.0-or-later
//! Synthetic, GPL-only test inputs. No executable, archive or defaults came from a game.
//! These helpers exercise the existing writer and Brabb3l VM, not the SF conformance sandbox.
#![allow(dead_code)] // Shared by independent integration-test binaries and subsequent slices.

pub mod archive;
pub mod package;
pub mod types;

use camino::Utf8PathBuf;
use std::{
    fs,
    sync::atomic::{AtomicU64, Ordering},
};

static NEXT_ROOT: AtomicU64 = AtomicU64::new(0);

/// Atomically reserves a unique root below TMPDIR. Never removes a pre-existing test's directory.
pub struct TestRoot(pub Utf8PathBuf);
impl TestRoot {
    pub fn new(tag: &str) -> Self {
        assert!(tag.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-'));
        let parent = Utf8PathBuf::from_path_buf(std::env::temp_dir()).unwrap();
        loop {
            let id = NEXT_ROOT.fetch_add(1, Ordering::Relaxed);
            let path = parent.join(format!("m6-{tag}-{}-{id}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(e) => panic!("reserve test root: {e}"),
            }
        }
    }
}
impl Drop for TestRoot {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

/// Optional raw observations for the binding differential. Never read as expected results or executable proof.
pub fn save_observation(name: &str, value: &serde_json::Value) {
    if let Some(root) = std::env::var_os("M6_WITNESS_OUT") {
        let root = std::path::PathBuf::from(root);
        assert!(root.is_absolute());
        fs::create_dir_all(&root).unwrap();
        fs::write(
            root.join(format!("{name}.json")),
            serde_json::to_vec_pretty(value).unwrap(),
        )
        .unwrap();
    }
}
