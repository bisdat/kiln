// SPDX-License-Identifier: GPL-3.0-or-later
//! Deterministic package trees, existing production discover/runner, and synthetic handoff state.
use super::archive::{ArchiveFixture, STEM};
use camino::Utf8PathBuf;
use kiln_eml::{
    discover::{self, Candidate},
    runner::{self, Outcome, RunConfig},
};
use std::fs;

impl ArchiveFixture {
    pub fn package(&self, id: &str, lua: &str, capabilities: &[&str]) -> Utf8PathBuf {
        assert!(id.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-'));
        let path = self.root.0.join("packages").join(id);
        fs::create_dir_all(path.join("src")).unwrap();
        fs::write(
            path.join("mod.json"),
            serde_json::to_vec(&serde_json::json!({
                "id": id, "name": id, "version": "1.0.0", "capabilities": capabilities,
            }))
            .unwrap(),
        )
        .unwrap();
        fs::write(path.join("src/mod.lua"), lua).unwrap();
        path
    }
    pub fn config(&self) -> RunConfig {
        RunConfig {
            game_dir: self.game.clone(),
            file_name: Some(STEM.into()),
            cache_dir: self.game.join("kiln/eml/cache"),
            patch: true,
            export: true,
            export_dir: Some(self.root.0.join("export").into_std_path_buf()),
            ..Default::default()
        }
    }
    pub fn run_packages(&self, ids: &[&str]) -> Outcome {
        let cfg = self.config();
        fs::create_dir_all(cfg.export_dir.as_ref().unwrap()).unwrap();
        let candidates: Vec<_> = ids
            .iter()
            .map(|id| Candidate {
                path: self.root.0.join("packages").join(id),
                source: "m6-synthetic".into(),
                enabled: true,
            })
            .collect();
        let (mods, entries) = discover::resolve(&candidates);
        runner::run(&cfg, mods, entries)
    }
    pub fn receipt(&self) -> Utf8PathBuf {
        self.game.join(format!("kiln/eml/ownership/{STEM}.json"))
    }
    pub fn change_cache(&self) -> Utf8PathBuf {
        self.game.join("kiln/eml/cache/files.json")
    }
    pub fn backup_seal(&self) -> Utf8PathBuf {
        self.game.join(format!("{STEM}.kfc.bak.integrity"))
    }
    /// Synthetic schema matching both SF pins' asset-preparation.schema.json, not a real SF state file.
    pub fn sf_applied(&self) -> Utf8PathBuf {
        let path = self.game.join("shroudforge/state.json");
        fs::create_dir_all(self.game.join("shroudforge/config")).unwrap();
        fs::write(
            &path,
            serde_json::to_vec(&serde_json::json!({
                "schemaVersion": 1, "unrelated": {"preserve": "synthetic setting"},
                "assets": {"schemaVersion":1, "status":"applied", "phase":"startup", "target":STEM,
                    "fingerprint":"a".repeat(64), "completedAt":42,
                    "mods":[{"id":"synthetic-sf", "version":"1.0.0"}]}
            }))
            .unwrap(),
        )
        .unwrap();
        path
    }
    pub fn forget_ownership(&self) {
        fs::remove_file(self.receipt()).unwrap();
        fs::remove_file(self.change_cache()).unwrap();
    }
}
