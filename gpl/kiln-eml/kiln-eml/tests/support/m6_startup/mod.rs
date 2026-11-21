// SPDX-License-Identifier: GPL-3.0-or-later
//! One kiln-mode start on the synthetic m6 archive for the startup controller's tests: a game folder (the the fixture
//! archive, a stand-in executable file that is never run, packages below `mods/`), the held install lock, a launch plan
//! made the way the launcher makes one (the inspector's frames, the packages' content digests, SF's load order), and
//! the startup controller's ports with recorders. Synthetic and GPL only.
#![allow(dead_code)]
use crate::fixture::archive::{ArchiveFixture, STEM};
use kfc::reflection::TypeRegistry;
use kiln_eml::{
    inspection,
    sf_pass::StartupDialect,
    sf_startup::{plan::RuntimeCatalog, runner},
    startup::{
        self, EmlSelection, LaunchKey, NativeFacts, PackageKind, PlanDependency, PlanFacts, PlanPackage, Point, Ports,
        Refused, StartupOutcome, Witnessed,
    },
};
use mod_loader::ModEnvironment;
use mod_loader_lua::test_gate;
use sfi_core::{
    bindings::runtime::{Availability, RuntimeOperation},
    host::{Clock, LogLevel, LogSink, SourceVersion},
    package::{
        ParseOptions,
        discover::{PackageRoots, Registry, discover},
        plan::startup_plan,
    },
};
use sha2::{Digest as _, Sha256};
use std::{
    cell::RefCell,
    collections::BTreeMap,
    fs,
    path::{Path, PathBuf},
    rc::Rc,
};

pub const EXE: &[u8] = b"m6 g13 stand-in executable bytes (never run)";

/// A package folder's content digest as the plan's writer computes it in these tests (sorted relative paths, lengths
/// and SHA-256s); the startup controller's port gets the same function.
pub fn tree_digest(root: &Path) -> Result<[u8; 32], String> {
    fn walk(base: &Path, dir: &Path, out: &mut Vec<(String, Vec<u8>)>) -> std::io::Result<()> {
        for e in fs::read_dir(dir)? {
            let e = e?;
            let path = e.path();
            if e.file_type()?.is_dir() {
                walk(base, &path, out)?;
            } else {
                let rel = path.strip_prefix(base).unwrap().to_string_lossy().replace('\\', "/");
                out.push((rel, fs::read(&path)?));
            }
        }
        Ok(())
    }
    let mut files = Vec::new();
    walk(root, root, &mut files).map_err(|e| format!("{}: {e}", root.display()))?;
    files.sort();
    let mut h = Sha256::new();
    for (rel, bytes) in files {
        h.update((rel.len() as u64).to_le_bytes());
        h.update(rel.as_bytes());
        h.update((bytes.len() as u64).to_le_bytes());
        h.update(Sha256::digest(&bytes));
    }
    Ok(h.finalize().into())
}

/// Lines the packages log, as "<owner>: <text>".
#[derive(Default)]
pub struct VmLog(pub RefCell<Vec<String>>);
impl LogSink for VmLog {
    fn enabled(&self, _: LogLevel) -> bool {
        true
    }
    fn write(&self, owner: &str, _: LogLevel, payload: &[u8]) {
        self.0
            .borrow_mut()
            .push(format!("{owner}: {}", String::from_utf8_lossy(payload)));
    }
}
struct FixedClock;
impl Clock for FixedClock {
    fn seconds(&self) -> f64 {
        2.5
    }
}
pub struct Catalog;
impl RuntimeCatalog for Catalog {
    fn game_build(&self) -> &str {
        "m6-g13-build"
    }
    fn static_support(&self, operation: RuntimeOperation) -> Availability {
        match operation {
            RuntimeOperation::Lifecycle => Availability::Available,
            _ => Availability::Unavailable {
                reason: "not in the m6 contract".into(),
            },
        }
    }
}

/// A Lua body that edits resource part `part` (`child.extra = value`) and creates one content blob `blob`.
pub fn edits(part: u32, value: u32, blob: &str) -> String {
    format!(
        r#"local r = game.assets.get_resource("01234567-89ab-cdef-0123-456789abcdef", "m6.Root", {part})
r.data.child.extra = {value}
game.assets.create_content(buffer.wrap("{blob}"))
shroudforge.log.info("{blob} ran")"#
    )
}
/// A Lua body that only creates the content blob `blob`.
pub fn content(blob: &str) -> String {
    format!("game.assets.create_content(buffer.wrap(\"{blob}\"))\nshroudforge.log.info(\"{blob} ran\")")
}
/// A Lua body whose entrypoint fails with `message` (the fork's own check failing).
pub fn broken(message: &str) -> String {
    format!("local function check()\n  error('{message}')\nend\ncheck()\nreturn {{}}")
}

/// The discovered packages below `<game>/mods`.
pub fn registry_of(game: &Path) -> Registry {
    let report = discover(
        &PackageRoots {
            mods: game.join("mods"),
            config: game.join("kiln/test-config"),
            profiles: game.join("kiln/test-profiles"),
        },
        ParseOptions {
            version: SourceVersion::Sf179,
            ..Default::default()
        },
        &[],
    )
    .unwrap();
    assert!(report.errors.is_empty(), "{:?}", report.errors);
    report.registry
}

/// The launch plan the launcher would make now: every discovered package a candidate in SF's startup load order, the
/// inspector's frames and executable digest, each package's content digest. `native`: the ids with native code.
pub fn plan_of(game: &Path, dialect: StartupDialect, native: &[&str]) -> PlanFacts {
    let registry = registry_of(game);
    let order = startup_plan(&registry, false, None);
    assert!(order.issues.is_empty(), "{:?}", order.issues);
    let inspected = inspection::inspect(
        game,
        &inspection::Options {
            side: inspection::Side::Client,
            hash_containers: false,
        },
    );
    assert_eq!(inspected.status, inspection::Status::Ok, "{:?}", inspected.reasons);
    let planned: Vec<&str> = order.ids.iter().map(String::as_str).collect();
    let packages = order
        .ids
        .iter()
        .map(|id| {
            let record = registry.get(id).unwrap();
            let m = &record.parsed().manifest;
            PlanPackage {
                id: id.clone(),
                version: m.version.to_string(),
                kind: PackageKind::Shroudforge,
                root: format!("mods/{id}"),
                content_sha256: tree_digest(&game.join("mods").join(id)).unwrap(),
                config_sha256: Sha256::digest(b"{}").into(),
                candidate: true,
                data_required: m.requires_pregame(),
                native: native.contains(&id.as_str()),
                dependencies: m
                    .dependencies
                    .iter()
                    .filter(|d| d.id != "shroudforge-api")
                    .map(|d| PlanDependency {
                        id: d.id.clone(),
                        optional: d.optional.unwrap_or(false),
                        included: planned.contains(&d.id.as_str()),
                    })
                    .collect(),
            }
        })
        .collect();
    PlanFacts {
        plan_id: *b"m6-g13-plan-id-0",
        dialect,
        exe_sha256: inspected.target.executable.as_ref().unwrap().sha256.clone(),
        archive_base_sha256: inspected.base.as_ref().unwrap().sha256.clone(),
        archive_input_sha256: inspected.input.as_ref().unwrap().sha256.clone(),
        archive_base_origin: None,
        packages,
        load_order: order.ids,
    }
}

pub fn key() -> LaunchKey {
    LaunchKey {
        launch_id: 0x4d2,
        plan_sha256: [0xaa; 32],
        plan_id: *b"m6-g13-plan-id-0",
    }
}

/// The native facts of a start whose terminal arrived, coherent, nothing withdrawn.
pub fn native() -> NativeFacts {
    NativeFacts {
        attached: true,
        received: true,
        withdrawn: BTreeMap::new(),
        coherent: Ok(()),
        dlls: BTreeMap::new(),
    }
}

/// The startup controller's pass over `game` with recording ports (`lines`: its kiln.log lines, `points`: what it reached).
#[allow(clippy::too_many_arguments)]
pub fn run_over(
    game: &Path,
    types: &Rc<TypeRegistry>,
    guard: &fs::File,
    w: &Witnessed,
    plan: &PlanFacts,
    native: &NativeFacts,
    eml: &dyn Fn(EmlSelection<'_>) -> anyhow::Result<Option<ModEnvironment>>,
    hook: &dyn Fn(Point<'_>),
    lines: &RefCell<Vec<String>>,
    points: &RefCell<Vec<String>>,
    vm_log: Rc<VmLog>,
) -> StartupOutcome {
    let log = |line: &str| lines.borrow_mut().push(line.to_owned());
    let observe = |point: Point<'_>| {
        points.borrow_mut().push(format!("{point:?}"));
        hook(point);
    };
    startup::run(
        w,
        plan,
        native,
        guard,
        Ports {
            types: types.clone(),
            registry: registry_of(game),
            catalog: Rc::new(Catalog),
            locale: None,
            vm: runner::Ports {
                clock: Rc::new(FixedClock),
                log: vm_log,
            },
            digest: &tree_digest,
            eml,
            log: &log,
            observe: &observe,
        },
    )
}

/// One kiln-mode start (or several, on the same game folder).
pub struct Start {
    pub f: ArchiveFixture,
    // the install lock, then its test-gate hold: dropped in that order (no process is created while it is held)
    pub guard: fs::File,
    hold: test_gate::LockHold,
    pub vm_log: Rc<VmLog>,
    pub lines: RefCell<Vec<String>>,
    pub points: RefCell<Vec<String>>,
}

impl Start {
    pub fn new(tag: &str) -> Self {
        let f = ArchiveFixture::new(tag);
        fs::write(f.game.join(format!("{STEM}.exe")), EXE).unwrap();
        fs::create_dir_all(f.game.join("mods")).unwrap();
        Self::over(f)
    }
    /// A start over an existing fixture: takes its install lock.
    pub fn over(f: ArchiveFixture) -> Self {
        let hold = test_gate::lock_hold();
        let guard = fs::OpenOptions::new()
            .create(true)
            .truncate(false)
            .read(true)
            .write(true)
            .open(f.game.join(".kiln-eml.lock"))
            .unwrap();
        guard.try_lock().unwrap();
        Self {
            f,
            guard,
            hold,
            vm_log: Rc::default(),
            lines: RefCell::default(),
            points: RefCell::default(),
        }
    }
    /// The fixture without its install lock (a kill test's child takes it).
    pub fn into_fixture(self) -> ArchiveFixture {
        let Start { f, guard, hold, .. } = self;
        drop(guard);
        drop(hold);
        f
    }
    pub fn game(&self) -> &Path {
        self.f.game.as_std_path()
    }
    /// A package below `<game>/mods/<id>`: mod.json (id, name, version 1.0.0 and `manifest`), extended.mod.json
    /// (schemaVersion 1, enabled, and `extension`), src/mod.lua (unless None).
    pub fn package(&self, id: &str, manifest: serde_json::Value, lua: Option<&str>) -> PathBuf {
        self.package_with(id, manifest, serde_json::json!({}), lua)
    }
    pub fn package_with(
        &self,
        id: &str,
        mut manifest: serde_json::Value,
        mut extension: serde_json::Value,
        lua: Option<&str>,
    ) -> PathBuf {
        let path = self.game().join("mods").join(id);
        fs::create_dir_all(path.join("src")).unwrap();
        manifest["id"] = id.into();
        manifest["name"] = id.into();
        if manifest.get("version").is_none() {
            manifest["version"] = "1.0.0".into();
        }
        extension["schemaVersion"] = 1.into();
        if extension.get("enabled").is_none() {
            extension["enabled"] = true.into();
        }
        fs::write(path.join("mod.json"), serde_json::to_vec(&manifest).unwrap()).unwrap();
        fs::write(path.join("extended.mod.json"), serde_json::to_vec(&extension).unwrap()).unwrap();
        if let Some(lua) = lua {
            fs::write(path.join("src/mod.lua"), lua).unwrap();
        }
        path
    }
    /// A patch package (capability patch) with `requires` (required) and `optional` dependencies.
    pub fn patch(&self, id: &str, requires: &[&str], optional: &[&str], lua: &str) -> PathBuf {
        let mut deps: Vec<serde_json::Value> = requires
            .iter()
            .map(|r| serde_json::json!({"id": r, "version": "^1.0.0"}))
            .collect();
        deps.extend(
            optional
                .iter()
                .map(|o| serde_json::json!({"id": o, "version": "^1.0.0", "optional": true})),
        );
        self.package(
            id,
            serde_json::json!({"capabilities": ["patch"], "dependencies": deps}),
            Some(lua),
        )
    }
    pub fn registry(&self) -> Registry {
        registry_of(self.game())
    }
    pub fn plan(&self, dialect: StartupDialect, native: &[&str]) -> PlanFacts {
        plan_of(self.game(), dialect, native)
    }
    pub fn witness(&self, plan: &PlanFacts) -> Result<Witnessed, Refused> {
        startup::witness(&self.f.game, STEM, &self.guard, plan, key())
    }
    /// The startup controller's pass with the recording ports (no EML pass).
    pub fn run(&self, w: &Witnessed, plan: &PlanFacts, native: &NativeFacts) -> StartupOutcome {
        self.run_with(w, plan, native, &|_| Ok(None), &|_| {})
    }
    pub fn run_with(
        &self,
        w: &Witnessed,
        plan: &PlanFacts,
        native: &NativeFacts,
        eml: &dyn Fn(EmlSelection<'_>) -> anyhow::Result<Option<ModEnvironment>>,
        hook: &dyn Fn(Point<'_>),
    ) -> StartupOutcome {
        self.points.borrow_mut().clear();
        run_over(
            self.game(),
            &self.f.types,
            &self.guard,
            w,
            plan,
            native,
            eml,
            hook,
            &self.lines,
            &self.points,
            self.vm_log.clone(),
        )
    }
    /// Witness and run in one go (the witness must pass).
    pub fn start(&self, plan: &PlanFacts, native: &NativeFacts) -> (Witnessed, StartupOutcome) {
        let w = self.witness(plan).unwrap_or_else(|r| panic!("witness refused: {r:?}"));
        let out = self.run(&w, plan, native);
        (w, out)
    }
    pub fn receipt(&self) -> Option<serde_json::Value> {
        fs::read(self.f.game.join(format!("kiln/eml/ownership/{STEM}.json")))
            .ok()
            .map(|b| serde_json::from_slice(&b).unwrap())
    }
    pub fn receipt_sha256(&self) -> Option<String> {
        fs::read(self.f.game.join(format!("kiln/eml/ownership/{STEM}.json")))
            .ok()
            .map(|b| format!("{:x}", Sha256::digest(b)))
    }
    pub fn owed(&self) -> bool {
        self.f
            .game
            .join(format!("kiln/eml/ownership/{STEM}.sf-handoff-owed.json"))
            .exists()
    }
    /// The live content blobs, as text.
    pub fn contents(&self) -> Vec<String> {
        let s = self.f.semantics();
        s.contents
            .values()
            .map(|b| String::from_utf8_lossy(b).into_owned())
            .collect()
    }
    /// The live `child.extra` of resource part `part`.
    pub fn extra(&self, part: u32) -> serde_json::Value {
        let s = self.f.semantics();
        let key = crate::fixture::archive::resource_key(&crate::fixture::archive::resource_id(part));
        s.resources[&key]["child"]["extra"].clone()
    }
    /// The outcome's JSON, also saved below `G13_OUTCOME_OUT` (for the schema check) as `<name>.json`.
    pub fn json(&self, out: &StartupOutcome, name: &str) -> serde_json::Value {
        outcome_json(out, name)
    }
}

/// `out.json(None)`, parsed and checked for the schema's field order (also saved below `G13_OUTCOME_OUT`).
pub fn outcome_json(out: &StartupOutcome, name: &str) -> serde_json::Value {
    let text = out.json(None);
    if let Some(dir) = std::env::var_os("G13_OUTCOME_OUT") {
        let dir = PathBuf::from(dir);
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join(format!("{name}.json")), &text).unwrap();
    }
    let order = [
        "format",
        "launchId",
        "planId",
        "planSha256",
        "dialect",
        "state",
        "grant",
        "receiptSha256",
        "archiveSha256",
        "baseOrigin",
        "native",
        "attempt",
        "error",
        "retryError",
        "packages",
        "warnings",
    ];
    let mut at = 0;
    for field in order {
        let found = text[at..]
            .find(&format!("\"{field}\":"))
            .unwrap_or_else(|| panic!("{name}: {field} out of order"));
        at += found;
    }
    serde_json::from_str(&text).unwrap()
}

/// A decoy ShroudForge folder with an applied prepared-output state for the archive, as hosted ShroudForge leaves it;
/// returns its snapshot.
pub fn shroudforge_decoy(game: &Path) -> BTreeMap<String, (Vec<u8>, u64, std::time::SystemTime)> {
    let sf = game.join("shroudforge");
    fs::create_dir_all(sf.join("config")).unwrap();
    fs::write(sf.join("config/modloader-config.json"), b"{}").unwrap();
    let state = serde_json::json!({"schemaVersion": 1, "assets": {"schemaVersion": 1, "status": "applied", "target": STEM,
        "fingerprint": "ab".repeat(32), "mods": [{"id": "sf-mod", "version": "1.0.0"}], "completedAt": 1, "phase": "startup"}});
    fs::write(sf.join("state.json"), serde_json::to_vec_pretty(&state).unwrap()).unwrap();
    tree(&sf)
}
/// Every entry below `dir`: name, bytes, length, modification time.
pub fn tree(dir: &Path) -> BTreeMap<String, (Vec<u8>, u64, std::time::SystemTime)> {
    fn walk(base: &Path, dir: &Path, out: &mut BTreeMap<String, (Vec<u8>, u64, std::time::SystemTime)>) {
        for e in fs::read_dir(dir).unwrap().flatten() {
            let path = e.path();
            let rel = path.strip_prefix(base).unwrap().to_string_lossy().replace('\\', "/");
            let meta = fs::symlink_metadata(&path).unwrap();
            if meta.is_dir() {
                out.insert(format!("{rel}/"), (Vec::new(), 0, meta.modified().unwrap()));
                walk(base, &path, out);
            } else {
                out.insert(rel, (fs::read(&path).unwrap(), meta.len(), meta.modified().unwrap()));
            }
        }
    }
    let mut out = BTreeMap::new();
    walk(dir, dir, &mut out);
    out
}
