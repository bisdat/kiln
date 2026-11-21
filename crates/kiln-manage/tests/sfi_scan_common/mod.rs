// SPDX-License-Identifier: MIT
//! Shared helpers for the sfi_scan tests: a synthetic game folder and inventory runs. Every package, DLL and catalog
//! here is synthetic; nothing is copied from a real mod or the game.
#![allow(dead_code)]
use kiln_manage::manifest::RunSide;
use kiln_manage::sfi_scan::{
    self, Inputs, Inventory, PackageEntry, native::NativeConfig, parity::CatalogInput,
};
use serde_json::{Value, json};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};

pub struct Game(pub PathBuf);
static N: AtomicUsize = AtomicUsize::new(0);
impl Game {
    pub fn new(tag: &str) -> Self {
        let p = std::env::temp_dir().join(format!(
            "kiln-sfi-scan-{tag}-{}-{}",
            std::process::id(),
            N.fetch_add(1, Ordering::SeqCst)
        ));
        let _ = fs::remove_dir_all(&p);
        fs::create_dir_all(p.join("mods")).unwrap();
        Self(p)
    }
    pub fn dir(&self, folder: &str) -> PathBuf {
        self.0.join("mods").join(folder)
    }
    pub fn file(&self, folder: &str, rel: &str, bytes: impl AsRef<[u8]>) {
        let p = self.dir(folder).join(rel);
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        fs::write(p, bytes).unwrap();
    }
    /// A ShroudForge package: mod.json + extended.mod.json.
    pub fn sf(&self, folder: &str, manifest: Value, extension: Value) {
        self.file(folder, "mod.json", manifest.to_string());
        self.file(folder, "extended.mod.json", extension.to_string());
    }
    pub fn simple(&self, folder: &str, id: &str, caps: &[&str], deps: Value) {
        self.sf(
            folder,
            json!({"id":id,"name":id,"version":"1.0.0","capabilities":caps,"dependencies":deps}),
            json!({"schemaVersion":1,"enabled":true,"targets":["client","server"]}),
        );
    }
    pub fn lua(&self, folder: &str, rel: &str, src: &str) {
        self.file(folder, rel, src);
    }
}
impl Drop for Game {
    fn drop(&mut self) {
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let sf = self.0.join("shroudforge");
            if sf.exists() {
                let _ = fs::set_permissions(&sf, fs::Permissions::from_mode(0o755));
            }
        }
        let _ = fs::remove_dir_all(&self.0);
    }
}

pub fn native() -> NativeConfig {
    NativeConfig::shipped().unwrap()
}
pub fn policy() -> sfi_scan::safety::Policy {
    sfi_scan::shipped_policy().unwrap()
}

pub fn run(
    game: &Game,
    side: RunSide,
    native: &NativeConfig,
    catalog: CatalogInput<'_>,
) -> Inventory {
    run_with(game, side, native, &policy(), catalog, None)
}
pub fn run_with(
    game: &Game,
    side: RunSide,
    native: &NativeConfig,
    policy: &sfi_scan::safety::Policy,
    catalog: CatalogInput<'_>,
    profiles: Option<&Path>,
) -> Inventory {
    sfi_scan::inventory(&Inputs {
        game: &game.0,
        side,
        profiles,
        rules: None,
        native,
        policy,
        catalog,
    })
}
pub fn client(game: &Game) -> Inventory {
    run(game, RunSide::Client, &native(), CatalogInput::Missing)
}
pub fn pkg<'a>(inv: &'a Inventory, dir: &str) -> &'a PackageEntry {
    inv.packages
        .iter()
        .find(|p| p.directory == dir)
        .unwrap_or_else(|| {
            panic!(
                "{dir} not listed: {:?}",
                inv.packages
                    .iter()
                    .map(|p| &p.directory)
                    .collect::<Vec<_>>()
            )
        })
}
pub fn codes(p: &PackageEntry) -> Vec<String> {
    p.blockers.iter().map(|b| b.code.clone()).collect()
}
pub fn classes(p: &PackageEntry) -> Vec<String> {
    p.advisories.iter().map(|a| a.class.clone()).collect()
}
