// SPDX-License-Identifier: GPL-3.0-or-later
// test infrastructure of the interpreter's startup seam.
//! One kiln-mode start on the synthetic m6 archive, driven the way Kiln's boot drives it, game-free: a game
//! folder (the synthetic archive, a stand-in executable file that is never run, ShroudForge patch packages below `mods/`), the
//! frozen launch plan written as the launcher's producer writes one (sfi-policy's strict v1, the inspector's frames,
//! the packages' content digests with sfi-policy's own function, SF's load order), the profile naming it, kiln-eml's
//! early phase (`startup_seam::early`), and a kiln-sfi stand-in that calls kiln.eml.startup v1 through the real C table
//! exactly as kiln-sfi's startup_seam.rs does (attach, witness_wait, witness_recheck, native_terminal, outcome). The
//! full two-DLL integration is the two-DLL integration's.
#![allow(dead_code)]
use crate::fixture::archive::{ArchiveFixture, STEM};
use camino::{Utf8Path, Utf8PathBuf};
use kiln_abi::{
    KILN_ERR_BUFFER_TOO_SMALL, KILN_OK, KilnResult, KilnStr,
    services::{
        kiln_eml_archive::KilnEmlArchiveIdentityV1,
        kiln_eml_startup::{KilnEmlStartupApiV1, KilnEmlStartupKeyV1, KilnEmlStartupWitnessV1},
    },
};
use kiln_eml::{
    inspection,
    kiln_mode::{self, KilnMode, Launch},
    startup_seam::{self, EarlyInputs},
};
use serde_json::{Value, json};
use sha2::{Digest as _, Sha256};
use std::{fs, path::PathBuf, sync::Arc};

pub const EXE: &[u8] = b"m6 g14b stand-in executable bytes (never run)";
pub const PLAN_ID: &str = "6714b000-0000-4000-8000-00000000b001";
pub const LAUNCH_ID: u64 = 0x14b0_0000_0000_0042;

pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
pub fn plan_id_bytes() -> [u8; 16] {
    let text = PLAN_ID.replace('-', "");
    let mut out = [0u8; 16];
    for (i, b) in out.iter_mut().enumerate() {
        *b = u8::from_str_radix(&text[i * 2..i * 2 + 2], 16).unwrap();
    }
    out
}

/// One package of the start: a ShroudForge patch package, its required dependencies, and whether it plans a DLL.
#[derive(Clone)]
pub struct Pkg {
    pub id: &'static str,
    pub requires: Vec<&'static str>,
    pub dll: bool,
    /// An EML package (`kind: "eml"`, no extended.mod.json).
    pub eml: bool,
    pub lua: String,
}
pub fn pkg(id: &'static str, lua: &str) -> Pkg {
    Pkg {
        id,
        requires: Vec::new(),
        dll: false,
        eml: false,
        lua: lua.into(),
    }
}
impl Pkg {
    pub fn needs(mut self, id: &'static str) -> Self {
        self.requires.push(id);
        self
    }
    pub fn with_dll(mut self) -> Self {
        self.dll = true;
        self
    }
    pub fn eml(mut self) -> Self {
        self.eml = true;
        self
    }
}

/// A Lua body that creates one content blob `blob`.
pub fn content(blob: &str) -> String {
    format!("game.assets.create_content(buffer.wrap(\"{blob}\"))\nshroudforge.log.info(\"{blob} ran\")")
}

/// An EML package's Lua (EML's own API): resource part 0's `child.extra` set to `value`.
pub fn eml_edit(value: u32) -> String {
    format!(
        r#"local r = game.assets.get_resource("01234567-89ab-cdef-0123-456789abcdef", game.types.get("m6.Root"))
r.data.child.extra = {value}"#
    )
}

/// The game folder of one start.
pub struct Game {
    pub f: ArchiveFixture,
    pub packages: Vec<Pkg>,
}
impl Game {
    pub fn new(tag: &str, packages: &[Pkg]) -> Self {
        let f = ArchiveFixture::new(tag);
        fs::write(f.game.join(format!("{STEM}.exe")), EXE).unwrap();
        for p in packages {
            let dir = f.game.join("mods").join(p.id);
            fs::create_dir_all(dir.join("src")).unwrap();
            let deps: Vec<Value> = p
                .requires
                .iter()
                .map(|r| json!({"id": r, "version": "^1.0.0"}))
                .collect();
            fs::write(
                dir.join("mod.json"),
                serde_json::to_vec(
                    &json!({"id": p.id, "name": p.id, "version": "1.0.0", "capabilities": ["patch"],
                    "dependencies": deps}),
                )
                .unwrap(),
            )
            .unwrap();
            if !p.eml {
                fs::write(dir.join("extended.mod.json"), br#"{"schemaVersion":1,"enabled":true}"#).unwrap();
            }
            fs::write(dir.join("src/mod.lua"), &p.lua).unwrap();
        }
        Self {
            f,
            packages: packages.to_vec(),
        }
    }
    pub fn game(&self) -> &Utf8Path {
        &self.f.game
    }
    pub fn kiln(&self) -> Utf8PathBuf {
        self.f.game.join("kiln")
    }
    pub fn exe_sha256(&self) -> String {
        hex(&Sha256::digest(EXE))
    }

    /// The frozen launch plan, as the producer writes it (sfi-policy's strict reader accepts it), with `edit` applied.
    pub fn plan(&self, edit: impl FnOnce(&mut Value)) -> Vec<u8> {
        let inspected = inspection::inspect(
            self.f.game.as_std_path(),
            &inspection::Options {
                side: inspection::Side::Client,
                hash_containers: false,
            },
        );
        assert_eq!(inspected.status, inspection::Status::Ok, "{:?}", inspected.reasons);
        // dependencies first, then by id: the order SF's planner gives these fixtures
        let mut order: Vec<&Pkg> = Vec::new();
        let mut left: Vec<&Pkg> = self.packages.iter().collect();
        left.sort_by_key(|p| p.id);
        while !left.is_empty() {
            let at = left
                .iter()
                .position(|p| p.requires.iter().all(|r| order.iter().any(|o| o.id == *r)))
                .expect("no cycle");
            order.push(left.remove(at));
        }
        let ff = "f".repeat(64);
        // the dialect sfi-policy's reader requires of these packages: a ShroudForge patch package makes it ShroudForge's
        let dialect = if self.packages.iter().any(|p| !p.eml) {
            "shroudforge"
        } else if self.packages.is_empty() {
            "none"
        } else {
            "eml"
        };
        let packages: Vec<Value> = order
            .iter()
            .map(|p| {
                let digest =
                    sfi_policy::digest::package_content_sha256(self.f.game.join("mods").join(p.id).as_std_path()).unwrap();
                let dlls: Vec<Value> = if p.dll {
                    vec![json!({"path": format!("{}.dll", p.id), "sha256": "a".repeat(64), "mode": "library",
                        "source": "ini", "required": true, "gate": "pass", "gateProof_sha256": "b".repeat(64), "bridgeAbi": 0})]
                } else {
                    Vec::new()
                };
                json!({
                    "id": p.id, "version": "1.0.0", "kind": if p.eml { "eml" } else { "shroudforge" }, "root": format!("mods/{}", p.id),
                    "content_sha256": hex(&digest), "config_sha256": hex(&Sha256::digest(b"{}")),
                    "enabled": true, "targets": ["client"], "capabilities": ["patch"],
                    "dependencies": p.requires.iter().map(|r| json!({"id": r, "range": "^1.0.0", "optional": false, "included": true})).collect::<Vec<_>>(),
                    "requiredApis": [], "services": [], "requires180": false, "saveSensitive": false,
                    "decision": "candidate", "reasons": [],
                    "data": {"required": true, "admission": "admit"},
                    "lua": {"required": false, "admission": "not-needed"},
                    "dlls": dlls,
                })
            })
            .collect();
        let mut plan = json!({
            "format": 1, "minReader": 1, "plan_id": PLAN_ID, "launch_id": null, "startupDialect": dialect,
            "writer": {"component": "kiln-manage", "version": "0.2.0-test"},
            "profile": {"id": "g14b", "generation": 1, "mode": "active", "semantic_sha256": ff},
            "game": {"side": "client", "exe_sha256": inspected.target.executable.as_ref().unwrap().sha256,
                "archiveBase_sha256": inspected.base.as_ref().unwrap().sha256,
                "archiveInput_sha256": inspected.input.as_ref().unwrap().sha256},
            "backend": {"requested": "kiln", "selected": "kiln", "fallbackOrder": [], "dataWriter": "kiln-eml",
                "selectionReason": "kiln.json shroudforge.backend: \"kiln\""},
            "contract": {"apiVersion": "1.7.9", "sourceCommit": "d8a394bcb180ceb5bdfce48048dcc4f2e0108bd8",
                "upstream180Final": false, "inventory_sha256": ff, "parity_sha256": ff, "serviceContracts_sha256": ff},
            "fingerprints": {"packages_sha256": ff, "settings_sha256": ff, "chain_sha256": ff, "loaderInventory_sha256": ff,
                "compat_sha256": ff, "releasePolicy_sha256": ff, "placementReceipts_sha256": ff},
            "packages": packages,
            "loadOrder": order.iter().map(|p| p.id).collect::<Vec<_>>(),
            "warnings": [],
        });
        edit(&mut plan);
        serde_json::to_vec(&plan).unwrap()
    }

    /// Writes `bytes` as the frozen plan; returns its SHA-256 and the profile naming it.
    pub fn freeze(&self, bytes: &[u8]) -> ([u8; 32], String) {
        let folder = self.kiln().join("launch-plans");
        fs::create_dir_all(&folder).unwrap();
        fs::write(folder.join(format!("{PLAN_ID}.json")), bytes).unwrap();
        let hash: [u8; 32] = Sha256::digest(bytes).into();
        let profile =
            json!({"mods": {}, "sfLaunchPlan": {"path": format!("launch-plans/{PLAN_ID}.json"), "sha256": hex(&hash)}});
        (hash, profile.to_string())
    }

    /// kiln-eml's early phase of this start, with the production install lock and witness.
    pub fn early(&self, hash: [u8; 32], profile: &str) -> Result<kiln_mode::Plan, String> {
        self.early_with(hash, profile, &startup_seam::install_lock, None)
    }
    pub fn early_with(
        &self,
        hash: [u8; 32],
        profile: &str,
        lock: &dyn Fn(&Utf8Path) -> std::io::Result<Arc<fs::File>>,
        foreign: Option<String>,
    ) -> Result<kiln_mode::Plan, String> {
        let kiln = self.kiln();
        startup_seam::early(&EarlyInputs {
            game: self.game(),
            kiln: &kiln,
            stem: STEM,
            launch_id: LAUNCH_ID,
            plan_sha256: hash,
            build: Some((self.exe_sha256(), "client")),
            profile_json: Some(profile),
            foreign_loader: foreign,
            lock,
            witness: &kiln_eml::startup::witness,
        })
    }

    /// Kiln mode as plugin.rs builds it at on_ready (the early phase's plan).
    pub fn mode(&self, hash: [u8; 32]) -> Arc<KilnMode> {
        let launch = Launch::Kiln {
            plan_hash: hash,
            launch_id: LAUNCH_ID,
        };
        Arc::new(
            kiln_mode::select(&launch, None, self.game(), |_| {
                startup_seam::early_plan().expect("the early phase ran")
            })
            .expect("kiln mode"),
        )
    }

    /// kiln-eml's on_ready for this start (plugin.rs `run_now`: discovery of `<game>/mods` and the drop-ins, then the
    /// runner, which takes kiln mode to the startup barrier): kiln/state/eml.json's outcome.
    pub fn on_ready(&self, hash: [u8; 32]) -> kiln_eml::runner::Outcome {
        let kiln = self.kiln();
        let mut candidates = kiln_eml::discover::folder_candidates(&kiln.join("eml/mods"), "kiln-dropin");
        candidates.extend(kiln_eml::discover::folder_candidates(
            &self.game().join("mods"),
            "game-mods",
        ));
        let (mods, entries) = kiln_eml::discover::resolve(&candidates);
        let cfg = kiln_eml::runner::RunConfig {
            game_dir: self.game().to_owned(),
            cache_dir: kiln.join("eml/cache"),
            file_name: Some(STEM.into()),
            patch: true,
            kiln: Some(self.mode(hash)),
            ..Default::default()
        };
        kiln_eml::runner::run(&cfg, mods, entries)
    }

    /// The live `child.extra` of resource part `part`.
    pub fn extra(&self, part: u32) -> Value {
        let s = self.f.semantics();
        let key = crate::fixture::archive::resource_key(&crate::fixture::archive::resource_id(part));
        s.resources[&key]["child"]["extra"].clone()
    }

    pub fn contents(&self) -> Vec<String> {
        self.f
            .semantics()
            .contents
            .values()
            .map(|b| String::from_utf8_lossy(b).into_owned())
            .collect()
    }
}

// ---- the kiln-sfi stand-in (crates/sfi/kiln-sfi/src/startup_seam.rs's calls) ----

pub fn table() -> &'static KilnEmlStartupApiV1 {
    let table = startup_seam::kiln_eml_startup_service(1);
    assert!(!table.is_null());
    // SAFETY: the process-lifetime table.
    unsafe { &*table }
}

pub fn key(launch_id: u64, plan_sha256: [u8; 32], session: u64) -> KilnEmlStartupKeyV1 {
    KilnEmlStartupKeyV1 {
        size: 56,
        flags: 0,
        launch_id,
        plan_sha256,
        session,
    }
}

pub fn blank_witness() -> KilnEmlStartupWitnessV1 {
    KilnEmlStartupWitnessV1 {
        size: 40,
        state: 0xdead,
        plan_id: [0xee; 16],
        revision: 0xdead_beef,
        flags: 0,
        reserved: 0,
    }
}

pub fn blank_identity() -> KilnEmlArchiveIdentityV1 {
    // SAFETY: integers and byte arrays.
    let mut identity: KilnEmlArchiveIdentityV1 = unsafe { std::mem::zeroed() };
    identity.size = 160;
    identity.generation = 0xdead;
    identity
}

/// kiln.boot's call (the boot identity, native/core/src/services/boot_archive_eml.h published_identity_of): published_identity with
/// a session-free key: (result, identity). The identity is `blank_identity()` with `size` bytes declared and the bytes
/// past v1 set to 0xab (an output never touches them).
pub fn published(launch_id: u64, plan_sha256: [u8; 32], session: u64, size: u32) -> (KilnResult, KilnEmlArchiveIdentityV1, [u8; 8]) {
    #[repr(C)]
    struct Wide {
        identity: KilnEmlArchiveIdentityV1,
        tail: [u8; 8],
    }
    let k = key(launch_id, plan_sha256, session);
    let mut out = Wide {
        identity: blank_identity(),
        tail: [0xab; 8],
    };
    out.identity.size = size;
    // SAFETY: a sized key; the record is 168 bytes, at least the size it declares.
    let rc = unsafe { (table().published_identity.unwrap())(&raw const k, &raw mut out.identity) };
    (rc, out.identity, out.tail)
}

/// kiln-sfi after its attach.
pub struct Sfi {
    pub plan_sha256: [u8; 32],
    pub session: u64,
}
impl Sfi {
    pub fn attach(plan_sha256: [u8; 32], planned: u32) -> Result<Self, KilnResult> {
        let k = key(LAUNCH_ID, plan_sha256, 0);
        let mut session = 0u64;
        // SAFETY: a sized key, a live out-pointer.
        let rc = unsafe { (table().attach.unwrap())(&raw const k, planned, &raw mut session) };
        if rc != KILN_OK {
            return Err(rc);
        }
        assert_ne!(session, 0);
        Ok(Self { plan_sha256, session })
    }
    pub fn key(&self) -> KilnEmlStartupKeyV1 {
        key(LAUNCH_ID, self.plan_sha256, self.session)
    }
    /// witness_wait with `timeout`: (result, state, plan id, revision, reason).
    pub fn witness(&self, timeout: u32) -> (KilnResult, u32, [u8; 16], u64, Option<String>) {
        let k = self.key();
        let mut out = blank_witness();
        let mut buffer = vec![0u8; 4096];
        let mut required = 0usize;
        // SAFETY: sized records, a buffer of its length.
        let rc = unsafe {
            (table().witness_wait.unwrap())(
                &raw const k,
                timeout,
                &raw mut out,
                buffer.as_mut_ptr().cast(),
                buffer.len(),
                &raw mut required,
            )
        };
        let text = (rc == KILN_OK && required > 0).then(|| String::from_utf8_lossy(&buffer[..required]).into_owned());
        (rc, out.state, out.plan_id, out.revision, text)
    }
    /// witness_recheck of the READY witness: (result, reason).
    pub fn recheck(&self, plan_id: [u8; 16], revision: u64) -> (KilnResult, Option<String>) {
        let k = self.key();
        let w = KilnEmlStartupWitnessV1 {
            size: 40,
            state: 1,
            plan_id,
            revision,
            flags: 0,
            reserved: 0,
        };
        let mut buffer = vec![0u8; 8192];
        let mut required = 0usize;
        // SAFETY: sized records, a buffer of its length.
        let rc = unsafe {
            (table().witness_recheck.unwrap())(
                &raw const k,
                &raw const w,
                buffer.as_mut_ptr().cast(),
                buffer.len(),
                &raw mut required,
            )
        };
        let text = (rc == KILN_OK && required > 0).then(|| String::from_utf8_lossy(&buffer[..required]).into_owned());
        (rc, text)
    }
    pub fn terminal(&self, json: &str) -> KilnResult {
        let k = self.key();
        // SAFETY: a sized key, counted UTF-8 for the call.
        unsafe {
            (table().native_terminal.unwrap())(
                &raw const k,
                KilnStr {
                    ptr: json.as_ptr().cast(),
                    len: json.len(),
                },
            )
        }
    }
    /// outcome, read as kiln-sfi reads it (size probe, then the complete size): (result, identity, record).
    pub fn outcome(&self) -> (KilnResult, KilnEmlArchiveIdentityV1, Option<Value>) {
        let k = self.key();
        let mut identity = blank_identity();
        let mut required = 0usize;
        // SAFETY: sized records, no buffer.
        let rc = unsafe {
            (table().outcome.unwrap())(
                &raw const k,
                &raw mut identity,
                std::ptr::null_mut(),
                0,
                &raw mut required,
            )
        };
        if rc != KILN_ERR_BUFFER_TOO_SMALL {
            return (rc, identity, None);
        }
        assert_eq!(
            identity.generation, 0xdead,
            "BUFFER_TOO_SMALL writes nothing but required"
        );
        let mut buffer = vec![0u8; required];
        let mut again = 0usize;
        // SAFETY: as above, a buffer of the complete size.
        let rc = unsafe {
            (table().outcome.unwrap())(
                &raw const k,
                &raw mut identity,
                buffer.as_mut_ptr().cast(),
                buffer.len(),
                &raw mut again,
            )
        };
        assert_eq!(again, required, "the complete size, both times");
        let record = (rc == KILN_OK).then(|| {
            save_record("outcome", &buffer);
            serde_json::from_slice(&buffer).expect("the record is JSON")
        });
        (rc, identity, record)
    }
}

/// Saves a record below `G14B_RECORDS_OUT` (for the schema check), when set.
pub fn save_record(kind: &str, bytes: &[u8]) {
    if let Some(dir) = std::env::var_os("G14B_RECORDS_OUT") {
        let dir = PathBuf::from(dir);
        fs::create_dir_all(&dir).unwrap();
        let case = std::env::var("KILN_EML_BARRIER_CASE").unwrap_or_else(|_| "case".into());
        fs::write(dir.join(format!("{case}-{kind}.json")), bytes).unwrap();
    }
}

/// A native terminal (native-terminal.schema.json v1) for this start's plan.
pub fn terminal(
    not_armed: bool,
    deadline: bool,
    coherent: Result<(), &str>,
    withdrawn: &[(&str, &str, &str, &[&str])],
    dlls: &[(&str, &str)],
) -> String {
    let coherent = match coherent {
        Ok(()) => json!({"ok": true}),
        Err(text) => json!({"ok": false, "text": text}),
    };
    let doc = json!({
        "format": 1, "planId": PLAN_ID, "notArmed": not_armed, "deadlinePassed": deadline, "coherent": coherent,
        "withdrawn": withdrawn.iter().map(|(id, code, text, chain)| json!({"id": id, "code": code, "text": text, "chain": chain})).collect::<Vec<_>>(),
        "dlls": dlls.iter().map(|(id, outcome)| json!({"statusId": format!("dll:{id}:{id}.dll"), "modId": id, "outcome": outcome})).collect::<Vec<_>>(),
    });
    let text = doc.to_string();
    save_record("terminal", text.as_bytes());
    text
}

/// The outcome row of `id`.
pub fn row<'a>(record: &'a Value, id: &str) -> &'a Value {
    record["packages"]
        .as_array()
        .unwrap()
        .iter()
        .find(|r| r["id"] == id)
        .unwrap_or_else(|| panic!("no row {id}: {record}"))
}
