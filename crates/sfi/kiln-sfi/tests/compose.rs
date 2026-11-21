// SPDX-License-Identifier: MIT
//! sfi-core's module composition on the interpreter worker, with the
//! fake pool, the lifecycle host and real Lua:
//! - two packages reach every pure family, and one oracle-recorded fixture per family
//!   runs inside a composed package environment ON THE WORKER and matches its v2 golden;
//! - an unsupported package (sfi-policy's native selection) is withdrawn whole before
//!   the VM exists: its reason, the loud warning, no Lua, no source read;
//! - Lua is entered on the worker thread only;
//! - namespace identity, `runtime.types` IS `game.types` (fake registry port), and no
//!   fabricated namespace without ports;
//! - the fast leave makes no port or cleanup call with the composed VM.
mod support;

use kiln_abi::*;
use kiln_sfi::lifecycle::*;
use kiln_sfi::worker::compose::{ComposePlan, RuntimeHosts};
use kiln_sfi::worker::*;
use mlua::{Lua, Table};
use serde_json::json;
use sfi_core::bindings::runtime::{
    Availability, EffectState, RuntimeHost, RuntimeOperation, RuntimeOwnerContext,
    RuntimeProviderFacts,
};
use sfi_core::compose::GameTypes;
use sfi_core::conformance::{GoldenMatch, compare_golden, run_fixture_in_vm};
use sfi_core::host::{
    DiskSource, HostContext, Package as RuntimePackage, PackageSource, Side, SourceVersion,
};
use sfi_core::lifecycle::Cleanup;
use sfi_core::locales::LocaleSource;
use sfi_core::package::{
    self,
    discover::{PackageRecord, Registry},
    manifest::Capability,
};
use sfi_policy::native_selection::{self as native, Exclusions, ExecutionPlan, PluginIni};
use std::collections::{BTreeMap, HashMap};
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, LazyLock, Mutex};
use std::thread::ThreadId;
use std::time::Duration;
use support::*;

/// A liveness bound, not a latency claim: how long a wait for something that must happen holds before the test
/// reports a hang. A passing wait returns as soon as its condition holds, so this sits far above any delay that
/// scheduling alone can cause on a loaded machine (300 s, as worker.rs and the lifecycle bodies).
const WAIT: Duration = Duration::from_secs(300);

// ---- per-owner observations (the plan and the ports run on the worker thread) ----

#[derive(Default)]
struct Seen {
    records: Vec<String>,
    /// Thread of every port call and every Lua-to-Rust call.
    threads: Vec<ThreadId>,
    port_calls: usize,
    reads: BTreeMap<String, usize>,
    restores: usize,
    transcripts: BTreeMap<String, String>,
}
static SEEN: LazyLock<Mutex<HashMap<KilnOwner, Seen>>> = LazyLock::new(Default::default);
fn seen<R>(owner: KilnOwner, f: impl FnOnce(&mut Seen) -> R) -> R {
    f(SEEN.lock().unwrap().entry(owner).or_default())
}
fn note(owner: KilnOwner, text: String) {
    seen(owner, |s| s.records.push(text));
}
fn here(owner: KilnOwner) {
    seen(owner, |s| s.threads.push(std::thread::current().id()));
}
fn has(owner: KilnOwner, text: &str) -> bool {
    seen(owner, |s| s.records.iter().any(|r| r == text))
}
fn records(owner: KilnOwner) -> Vec<String> {
    seen(owner, |s| s.records.clone())
}

// ---- scenario ----

#[derive(Clone)]
struct Pkg {
    id: &'static str,
    /// In-memory `src/mod.lua`, or a disk package (the oracle seed) when `disk` is set.
    entry: String,
    disk: Option<PathBuf>,
    caps: Vec<Capability>,
    oracle_model: bool,
}
#[derive(Clone)]
struct Scenario {
    packages: Vec<Pkg>,
    ports: bool,
    export: bool,
    withdrawn: Vec<native::Withdrawn>,
    fixtures: bool,
}
static SCENARIOS: LazyLock<Mutex<HashMap<KilnOwner, Scenario>>> = LazyLock::new(Default::default);

struct Counted {
    owner: KilnOwner,
    id: String,
    inner: Rc<dyn PackageSource>,
}
impl PackageSource for Counted {
    fn read_file(&self, path: &str) -> std::io::Result<Vec<u8>> {
        seen(self.owner, |s| {
            *s.reads.entry(self.id.clone()).or_default() += 1
        });
        self.inner.read_file(path)
    }
    fn metadata(&self, path: &str) -> std::io::Result<sfi_core::host::SourceMetadata> {
        seen(self.owner, |s| {
            *s.reads.entry(self.id.clone()).or_default() += 1
        });
        self.inner.metadata(path)
    }
    fn read_dir(
        &self,
        path: &str,
    ) -> std::io::Result<Vec<std::io::Result<sfi_core::host::SourceEntry>>> {
        seen(self.owner, |s| {
            *s.reads.entry(self.id.clone()).or_default() += 1
        });
        self.inner.read_dir(path)
    }
}
struct Memory(String);
impl PackageSource for Memory {
    fn read_file(&self, path: &str) -> std::io::Result<Vec<u8>> {
        if path == "src/mod.lua" {
            Ok(self.0.as_bytes().to_vec())
        } else {
            Err(std::io::ErrorKind::NotFound.into())
        }
    }
}
struct Locale;
impl LocaleSource for Locale {
    fn locale(&self) -> Option<String> {
        Some("de".into())
    }
}
struct CountingCleanup(KilnOwner);
impl Cleanup for CountingCleanup {
    fn restore(&self, _: &str) -> Result<(), String> {
        seen(self.0, |s| s.restores += 1);
        Ok(())
    }
    fn release_input_reservations(&self, _: &str) {
        seen(self.0, |s| s.restores += 1);
    }
}

/// The fake registry port.
struct FakeTypes(KilnOwner);
impl GameTypes for FakeTypes {
    fn types(&self, lua: &Lua, owner: &RuntimePackage) -> mlua::Result<Table> {
        here(self.0);
        seen(self.0, |s| s.port_calls += 1);
        note(self.0, format!("types:{}", owner.id()));
        let table = lua.create_table()?;
        table.raw_set("__fake_registry_owner", owner.id())?;
        Ok(table)
    }
}
struct FakeRuntime {
    owner: KilnOwner,
    context: RuntimeOwnerContext,
}
impl FakeRuntime {
    fn call(&self) {
        here(self.owner);
        seen(self.owner, |s| s.port_calls += 1);
    }
}
impl RuntimeHost for FakeRuntime {
    fn owner_context(&self) -> &RuntimeOwnerContext {
        self.call();
        &self.context
    }
    fn static_support(&self, _: RuntimeOperation) -> Availability {
        self.call();
        Availability::Available
    }
    fn provider_facts(&self) -> RuntimeProviderFacts {
        self.call();
        RuntimeProviderFacts::default()
    }
    fn lifecycle_active(&self) -> bool {
        self.call();
        true
    }
    fn write_fault(&self) -> Option<String> {
        self.call();
        None
    }
    fn report_effect(&self, _: EffectState, _: &str) {
        self.call();
    }
}

fn oracle_settings(manifest: &mut package::Manifest) {
    // The differential test's fixed seed, as sfi-core's package-host tests build it.
    manifest.setting_values =
        json!({"enabled_flag":true,"count":7,"title":"oracle","fraction":1.25,"hotkey":"SDL:65"});
    for (code, hello, only) in [("en", "Hello", "English"), ("de", "Hallo", "Deutsch")] {
        manifest
            .locales
            .entry(code.into())
            .or_default()
            .strings
            .extend([
                ("hello".into(), hello.into()),
                ("english_only".into(), only.into()),
            ]);
    }
}

fn registry(owner: KilnOwner, packages: &[Pkg]) -> Registry {
    let mut registry = Registry::default();
    for p in packages {
        let inner: Rc<dyn PackageSource> = match &p.disk {
            Some(root) => Rc::new(DiskSource::new(root).unwrap()),
            None => Rc::new(Memory(p.entry.clone())),
        };
        let source: Rc<dyn PackageSource> = Rc::new(Counted {
            owner,
            id: p.id.into(),
            inner,
        });
        let caps: Vec<&str> = p
            .caps
            .iter()
            .map(|c| match c {
                Capability::Export => "export",
                Capability::Runtime => "runtime",
                Capability::Patch => "patch",
                Capability::RuntimeRegisterDll => "runtime-register-dll",
            })
            .collect();
        let mut parsed = package::Package::parse(
            json!({"id": p.id, "name": p.id, "version": "1.0.0", "capabilities": caps})
                .to_string()
                .as_bytes(),
            None,
            package::ParseOptions::default(),
        )
        .unwrap();
        parsed.manifest.enabled = true;
        if p.oracle_model {
            oracle_settings(&mut parsed.manifest);
        }
        let mut runtime = RuntimePackage::new(p.id, source.clone());
        runtime.export_root = p.disk.as_ref().map(|root| root.with_extension("exports"));
        registry
            .insert(PackageRecord::new(p.id.into(), parsed, runtime, source).unwrap())
            .unwrap();
    }
    registry
}

/// One oracle-recorded fixture per pure family, with its v2 golden for 1.7.9 (the
/// plan's version). Paths are sfi-core's committed fixtures; nothing is copied.
const FIXTURES: &[(&str, &str, &str)] = &[
    // settings/locales, logging, clock and the package model (loader metadata)
    (
        "common",
        "tests/fixtures/c5/common.lua",
        "tests/fixtures/c5/sf179.golden.json",
    ),
    (
        "buffer",
        "tests/fixtures/c7b/buffer.lua",
        "tests/fixtures/c7b/sf-1.7.9.golden.json",
    ),
    (
        "hash-guid",
        "tests/fixtures/c7c/hash-guid.lua",
        "tests/fixtures/c7c/sf-1.7.9.golden.json",
    ),
    (
        "integer",
        "tests/conformance/integer/conversions.lua",
        "tests/conformance/integer/conversions.golden.json",
    ),
    (
        "image",
        "tests/conformance/image/image.lua",
        "tests/conformance/image/image.golden.json",
    ),
    (
        "texture",
        "tests/conformance/texture/arguments.lua",
        "tests/conformance/texture/arguments.golden.json",
    ),
    (
        "io",
        "tests/fixtures/c7d/io.lua",
        "tests/fixtures/c7d/sf-1.7.9.golden.json",
    ),
];
/// sfi-core's package root; `SFI_CORE_ROOT` overrides it where the test binary runs
/// away from the source tree (the native-Windows runner).
fn core_file(path: &str) -> String {
    let root = std::env::var_os("SFI_CORE_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../sfi-core"));
    std::fs::read_to_string(root.join(path)).unwrap_or_else(|e| panic!("{path}: {e}"))
}

struct ComposeTest;
impl PlanSource for ComposeTest {
    fn plan(cx: &WorkerContext<'_>) -> Result<RuntimePlan, String> {
        let owner = cx.owner();
        here(owner);
        let scenario = SCENARIOS.lock().unwrap()[&owner].clone();
        let ids: Vec<String> = scenario.packages.iter().map(|p| p.id.to_string()).collect();
        let runtime: RuntimeHosts =
            Rc::new(move |manifest: &package::Manifest, context: &HostContext| {
                here(owner);
                Rc::new(FakeRuntime {
                    owner,
                    context: RuntimeOwnerContext::new(manifest, context, false, true, "test-build"),
                }) as Rc<dyn RuntimeHost>
            });
        let fixtures = scenario.fixtures;
        let register: Register = Box::new(move |lua, env, package, _ports| {
            let id = package.id().to_owned();
            let who = id.clone();
            env.set(
                "record",
                lua.create_function(move |_, text: String| {
                    here(owner);
                    note(owner, format!("{who}:{text}"));
                    Ok(())
                })?,
            )?;
            // The oracle package's composed environment, for the fixture turn below.
            if fixtures && id == "oracle" {
                STASH.with(|stash| *stash.borrow_mut() = Some((lua.clone(), env.clone())));
            }
            Ok(())
        });
        Ok(RuntimePlan {
            side: Side::Server,
            version: SourceVersion::Sf179,
            registry: registry(owner, &scenario.packages),
            plan: ids.clone(),
            admitted: ids.into_iter().collect(),
            cleanup: Rc::new(CountingCleanup(owner)),
            locale: Rc::new(Locale),
            clock: process_clock(),
            loader_patch: false,
            loader_export: scenario.export,
            attachments: None,
            register: Some(register),
            compose: ComposePlan {
                game_types: scenario
                    .ports
                    .then(|| Rc::new(FakeTypes(owner)) as Rc<dyn GameTypes>),
                runtime: scenario.ports.then_some(runtime),
                withdrawn: scenario.withdrawn.clone(),
            },
        })
    }
}

thread_local! {
    /// The composed VM and the oracle package's environment, on the worker thread only.
    static STASH: std::cell::RefCell<Option<(Lua, Table)>> = const { std::cell::RefCell::new(None) };
}

/// The lifecycle host plus one step after its first turn: each family fixture runs in
/// the oracle package's composed environment, on the worker, at top level (no Lua frame
/// below it, as in the oracle), through sfi-core's shared conformance runner.
struct FixtureTurn {
    inner: Box<dyn WorkerRuntime>,
    owner: KilnOwner,
}
impl WorkerRuntime for FixtureTurn {
    fn command(&mut self, cx: &WorkerContext<'_>, command: WorkerCommand) {
        self.inner.command(cx, command)
    }
    fn turn(&mut self, cx: &WorkerContext<'_>, dt: Duration) {
        self.inner.turn(cx, dt);
        if let Some((lua, env)) = STASH.with(|stash| stash.borrow_mut().take()) {
            here(self.owner);
            for (name, fixture, _) in FIXTURES {
                let transcript = run_fixture_in_vm(&lua, &core_file(fixture), |_| Ok(env.clone()))
                    .unwrap_or_else(|e| format!("host error: {e}"));
                seen(self.owner, |s| {
                    s.transcripts.insert(name.to_string(), transcript)
                });
            }
            note(self.owner, "oracle:fixtures".into());
        }
    }
    fn stop(&mut self, cx: &WorkerContext<'_>) {
        self.inner.stop(cx)
    }
}
fn with_fixtures(cx: &WorkerContext<'_>) -> Result<Box<dyn WorkerRuntime>, String> {
    Ok(Box::new(FixtureTurn {
        inner: factory::<ComposeTest>(cx)?,
        owner: cx.owner(),
    }))
}

struct Run {
    owner: KilnOwner,
    pool: Arc<FakePool>,
    shared: Arc<WorkerShared>,
}
impl Run {
    fn finished(&self) -> bool {
        self.pool.task(0).wait_finished(WAIT)
    }
    fn logs(&self, level: i32) -> Vec<String> {
        logs(self.owner)
            .into_iter()
            .filter(|(l, _)| *l == level)
            .map(|(_, t)| t)
            .collect()
    }
    fn wait_for(&self, text: &str) {
        assert!(
            eventually(WAIT, || has(self.owner, text)),
            "never saw {text}: {:?} / {:?} / errors {:?}",
            records(self.owner),
            self.shared.status(),
            self.logs(KILN_LOG_ERROR)
        );
    }
}
fn start(scenario: Scenario) -> Run {
    let owner = next_owner();
    let pool = FakePool::new(owner, PoolConfig::default());
    let fixtures = scenario.fixtures;
    SCENARIOS.lock().unwrap().insert(owner, scenario);
    let runtime: RuntimeFactory = if fixtures {
        with_fixtures
    } else {
        factory::<ComposeTest>
    };
    let worker: &'static Worker = Box::leak(Box::new(Worker::new(runtime)));
    let api = api(true);
    assert!(worker.bind(unsafe { Host::read(&*api) }, owner));
    worker.start().expect("accepted");
    let shared = worker.shared().unwrap().clone();
    Run {
        owner,
        pool,
        shared,
    }
}
fn memory(id: &'static str, entry: &str) -> Pkg {
    Pkg {
        id,
        entry: entry.into(),
        disk: None,
        caps: vec![Capability::Runtime],
        oracle_model: false,
    }
}
fn scenario(packages: Vec<Pkg>, ports: bool) -> Scenario {
    Scenario {
        packages,
        ports,
        export: false,
        withdrawn: Vec::new(),
        fixtures: false,
    }
}

/// The differential test's seed package on disk (as the IO fixtures lay it out).
struct SeedDir(PathBuf);
impl SeedDir {
    fn new(entry: &str) -> Self {
        static N: AtomicUsize = AtomicUsize::new(0);
        let root = std::env::temp_dir().join(format!(
            "kiln-sfi-c8-{}-{}",
            std::process::id(),
            N.fetch_add(1, Ordering::SeqCst)
        ));
        let package = root.join("oracle");
        std::fs::create_dir_all(package.join("src")).unwrap();
        std::fs::create_dir_all(package.join("locales")).unwrap();
        std::fs::write(package.join("data.txt"), "oracle bytes\n").unwrap();
        std::fs::write(package.join("locales/en.json"), "{}").unwrap();
        std::fs::write(package.join("locales/de.json"), "{}").unwrap();
        std::fs::write(package.join("src/mod.lua"), entry).unwrap();
        Self(root)
    }
    fn package(&self) -> PathBuf {
        self.0.join("oracle")
    }
}
impl Drop for SeedDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

// ---- 1. positive scenario ----

const REACH: &str = r#"
local keys = {} for k in pairs(builtin) do keys[#keys + 1] = k end table.sort(keys)
for _, k in ipairs(keys) do
  assert(rawequal(builtin[k], _G[k]), k) assert(next(_G[k]) ~= nil, k)
end
assert(rawequal(runtime.types, game.types) and game.types.__fake_registry_owner == shroudforge.mod_id)
assert(buffer.create(4):capacity() >= 4 and hasher.crc32('a') ~= nil and game.guid.hash(game.guid.NONE) ~= nil)
assert(integer.u8.add(1, 2) == 3 and image.create(1, 1).width == 1 and image.encode_texture)
assert(shroudforge.settings.get and shroudforge.i18n.get and shroudforge.log.info and shroudforge.clock() >= 0)
assert(io.exists and loader.has_mod('oracle') and loader.has_mod('beta') and runtime.has)
record('builtin:' .. table.concat(keys, ','))
record('identity:' .. tostring(io) .. tostring(game) .. tostring(runtime) .. tostring(shroudforge))
"#;

#[test]
fn two_packages_reach_every_family_and_match_the_oracle_through_the_worker() {
    let seed = SeedDir::new(&format!(
        "{REACH}\nreturn {{ on_update = function() record('u') end }}"
    ));
    let mut oracle = memory("oracle", "");
    oracle.disk = Some(seed.package());
    oracle.caps = vec![Capability::Export, Capability::Runtime];
    oracle.oracle_model = true;
    let beta = memory(
        "beta",
        &format!("{REACH}\nreturn {{ on_update = function() record('u') end }}"),
    );
    let mut s = scenario(vec![oracle, beta], true);
    s.export = true;
    s.fixtures = true;
    let run = start(s);
    run.wait_for("oracle:u");
    run.wait_for("beta:u");
    run.wait_for("oracle:fixtures");
    let keys = "builtin:buffer,game,hasher,image,integer,io,loader,runtime,shroudforge";
    assert!(
        has(run.owner, &format!("oracle:{keys}")),
        "{:?}",
        records(run.owner)
    );
    assert!(has(run.owner, &format!("beta:{keys}")));
    let identity = |id: &str| {
        records(run.owner)
            .into_iter()
            .find(|r| r.starts_with(&format!("{id}:identity:")))
            .unwrap()
            .split_once(":identity:")
            .unwrap()
            .1
            .to_owned()
    };
    assert_ne!(
        identity("oracle"),
        identity("beta"),
        "one table per owner and namespace"
    );
    let transcripts = seen(run.owner, |s| s.transcripts.clone());
    assert_eq!(transcripts.len(), FIXTURES.len());
    for (name, _, golden) in FIXTURES {
        let actual = &transcripts[*name];
        let matched = compare_golden(&core_file(golden), actual)
            .unwrap_or_else(|diff| panic!("{name} through the worker: {diff}"));
        assert_eq!(matched, GoldenMatch::MessageVerifiedV2, "{name}");
    }
    // The common fixture's own `shroudforge.log.error('', 'x')` is the only error line.
    assert_eq!(run.logs(KILN_LOG_ERROR), ["[oracle] \tx"]);
    run.shared.request_stop();
    assert!(run.finished());
    assert_eq!(run.shared.status().mods_failed, 0);
}

// ---- 2. withdrawal ----

fn native_package(id: &str, ini: PluginIni) -> native::Package {
    native::Package {
        directory: id.into(),
        manifest: native::Manifest::Valid {
            id: id.into(),
            enabled: true,
            target: native::ManifestTarget::Both,
        },
        ini,
        profile: None,
        files: Vec::new(),
    }
}

#[test]
fn an_unsupported_package_is_withdrawn_whole_before_any_lua() {
    // `bad` names a native DLL that is not in its folder; `dep` depends on it.
    let plan = ExecutionPlan {
        order: vec!["bad".into(), "dep".into(), "good".into()],
        issues: BTreeMap::new(),
        dependencies: BTreeMap::from([("dep".into(), vec!["bad".into()])]),
        error: None,
    };
    let packages = [
        native_package(
            "bad",
            PluginIni::Enabled {
                dll: "missing.dll".into(),
            },
        ),
        native_package("dep", PluginIni::Missing),
        native_package("good", PluginIni::Missing),
    ];
    let exclusions = Exclusions::default();
    let selection = native::select(&native::Inputs {
        manager_enabled: true,
        server: true,
        exclusions: &exclusions,
        selections: &[],
        packages: &packages,
        plan: &plan,
        registrations: &[],
        permission: native::NativePermission::ALWAYS,
    });
    let withdrawn: Vec<_> = selection
        .withdrawn
        .iter()
        .map(|w| w.mod_id.as_str())
        .collect();
    assert_eq!(withdrawn, ["bad", "dep"], "{selection:?}");
    assert!(
        selection
            .entries
            .iter()
            .all(|e| e.mod_id.as_deref() == Some("good"))
    );
    let lua = "record('lua') return { on_load = function() record('load') end }";
    let mut s = scenario(
        vec![
            memory("bad", lua),
            memory("dep", lua),
            memory(
                "good",
                "record('lua') record('sees-bad:' .. tostring(loader.has_mod('bad'))) \
                 return { on_update = function() record('u') end }",
            ),
        ],
        true,
    );
    s.withdrawn = selection.withdrawn.clone();
    let run = start(s);
    run.wait_for("good:u");
    let warnings = run.logs(KILN_LOG_WARN);
    for w in &selection.withdrawn {
        let line = warnings
            .iter()
            .find(|l| l.contains(&format!("'{}' will not run: {}", w.mod_id, w.reason)))
            .unwrap_or_else(|| panic!("no loud warning for {}: {warnings:?}", w.mod_id));
        assert!(line.starts_with("kiln-sfi: WARNING:"), "{line}");
    }
    assert!(
        warnings
            .iter()
            .any(|l| l.contains("'dep'") && l.contains("(via bad)"))
    );
    let all = logs(run.owner);
    let first_warning = all.iter().position(|(l, _)| *l == KILN_LOG_WARN).unwrap();
    let initialized = all
        .iter()
        .position(|(_, t)| t == "kiln-sfi: Runtime initialized")
        .unwrap();
    assert!(
        first_warning < initialized,
        "withdrawn before runtime-create ends"
    );
    let reads = seen(run.owner, |s| s.reads.clone());
    assert_eq!(
        reads.get("bad"),
        None,
        "no Lua or file read for bad: {reads:?}"
    );
    assert_eq!(
        reads.get("dep"),
        None,
        "no Lua or file read for dep: {reads:?}"
    );
    assert!(reads["good"] >= 1);
    assert!(!has(run.owner, "bad:lua") && !has(run.owner, "dep:lua"));
    assert!(
        has(run.owner, "types:good")
            && !has(run.owner, "types:bad")
            && !has(run.owner, "types:dep"),
        "a withdrawn package is never registered: {:?}",
        records(run.owner)
    );
    assert!(
        has(run.owner, "good:sees-bad:true"),
        "discovered membership stays visible"
    );
    assert_eq!(run.shared.status().mods_active, 1);
    run.shared.request_stop();
    assert!(run.finished());
}

// ---- 3. threads ----

#[test]
fn lua_is_entered_only_on_the_worker_thread() {
    let run = start(scenario(
        vec![memory(
            "a",
            "record('top') local _ = runtime.has('x') \
             return { on_update = function() runtime.status('x') record('u') end }",
        )],
        true,
    ));
    run.wait_for("a:u");
    // Producers on other threads only reach the inbox; the command waits for a turn.
    let shared = run.shared.clone();
    let producer = std::thread::spawn(move || {
        for session in 1..=3 {
            let _ = shared.submit(WorkerCommand::WorldLifecycle(WorldLifecycle {
                world: 0,
                session,
                event: LifecycleEvent::Started,
            }));
        }
        std::thread::current().id()
    })
    .join()
    .unwrap();
    let count = records(run.owner).iter().filter(|r| *r == "a:u").count();
    assert!(eventually(WAIT, || records(run.owner)
        .iter()
        .filter(|r| *r == "a:u")
        .count()
        > count + 1));
    run.shared.request_stop();
    assert!(run.finished());
    let worker = run
        .pool
        .task(0)
        .body_thread
        .lock()
        .unwrap()
        .expect("body ran");
    let threads = seen(run.owner, |s| s.threads.clone());
    assert!(threads.len() > 4, "{threads:?}");
    assert!(
        threads.iter().all(|t| *t == worker),
        "every Lua entry and port call on the worker"
    );
    assert_ne!(worker, std::thread::current().id());
    assert_ne!(worker, producer);
}

// ---- 4. namespace identity without ports ----

#[test]
fn without_ports_there_is_no_fabricated_namespace() {
    let run = start(scenario(
        vec![memory(
            "a",
            "record('runtime:' .. tostring(runtime) .. ':' .. tostring(builtin.runtime)) \
             record('types:' .. tostring(game.types) .. ':' .. tostring(game.guid ~= nil)) \
             for k, v in pairs(builtin) do assert(next(v) ~= nil, k) end \
             return { on_update = function() record('u') end }",
        )],
        false,
    ));
    run.wait_for("a:u");
    assert!(
        has(run.owner, "a:runtime:nil:nil"),
        "{:?}",
        records(run.owner)
    );
    assert!(has(run.owner, "a:types:nil:true"));
    assert_eq!(seen(run.owner, |s| s.port_calls), 0);
    run.shared.request_stop();
    assert!(run.finished());
}

// ---- 5. fast leave ----

const LEAVE_PROBE: &str = "keep = setmetatable({}, { __gc = function() record('gc') runtime.has('gc') end }) \
    return { on_update = function() record('u') end, \
             on_unload = function() runtime.has('unload') io.exists('x') record('unload') end }";

#[test]
fn the_fast_leave_makes_no_port_or_cleanup_call_with_the_composed_vm() {
    let run = start(scenario(vec![memory("a", LEAVE_PROBE)], true));
    run.wait_for("a:u");
    let before = seen(run.owner, |s| (s.port_calls, s.restores, s.reads.clone()));
    run.pool.cancel(run.shared.task());
    assert!(run.finished());
    std::thread::sleep(Duration::from_millis(50));
    let after = seen(run.owner, |s| (s.port_calls, s.restores, s.reads.clone()));
    assert_eq!(
        before, after,
        "the fast leave called a port, cleanup or the source"
    );
    assert!(!has(run.owner, "a:unload") && !has(run.owner, "a:gc"));
    assert_eq!(run.shared.status().reason, StopReason::Cancelled);
    // Control: the orderly leave of the same package does call the ports.
    let run = start(scenario(vec![memory("a", LEAVE_PROBE)], true));
    run.wait_for("a:u");
    let before = seen(run.owner, |s| s.port_calls);
    run.shared.request_stop();
    assert!(run.finished());
    assert!(has(run.owner, "a:unload"));
    assert!(seen(run.owner, |s| s.port_calls) > before);
}
