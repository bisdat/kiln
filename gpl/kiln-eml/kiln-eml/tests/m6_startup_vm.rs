// SPDX-License-Identifier: GPL-3.0-or-later
//! The SF-dialect startup pass end to end. Real ShroudForge-shaped packages (mod.json + extended.mod.json + Lua,
//! all synthetic) are discovered by the core's C3 discovery, planned by its load order, registered in ONE production
//! startup VM (sfi-core's LuaVm and composition with the runtime registrar, the GPL game builders on the same table, the wrappers' table port
//! and the asset session's Buffer port attached before any Lua access) and run over the synthetic fixture archive through a real
//! StartupHost generation: their startup data reaches the stage, AssetSession::prepare serializes it while the VM
//! lives, and the sole publisher publishes it. Expectations are ShroudForge 1.7.9's (run_startup_mods.rs:18-97,
//! mod_runner.rs:49-74,99-135, require_mod.rs:70-175, bindings/mod.rs:66-114, eml/v1/create_loader_api.rs:7-47,
//! ingame/check_ingame_access.rs), source-derived.
//! It adds the runtime families of the pregame VM: checked against ShroudForge's own mod runtime on both pins (the fixture
//! fixtures/m6/startup/oracle/runtime-pregame.lua and its two transcripts), on a registry the oracle cannot construct,
//! and in a package that touches every family at top level and runs in the pass.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;

use fixture::{
    archive::{self, ArchiveFixture, STEM},
    types,
};
use kfc::{
    container::KFCFile,
    guid::{ContentHash, Guid, ResourceId},
    hash::fnv,
    reflection::TypeIndex,
    resource::value::{Value, Variant},
};
use kiln_eml::{
    sf_pass::{PassError, StartupDialect, run_pass},
    sf_startup::{
        plan::{ExportRoot, Features, RuntimeCatalog, StartupPlan},
        runner::{Ports, PreparedStartup, SideEffects, Step, run_attempt},
    },
};
use mod_loader::lua::startup_host::StartupHost;
use sfi_core::{
    bindings::runtime::{Availability, RuntimeOperation},
    host::{Clock, LogLevel, LogSink, Side, SourceVersion},
    package::{
        ParseOptions,
        discover::{PackageRoots, Registry, discover},
        plan::startup_plan,
    },
};
use std::{
    cell::RefCell,
    collections::BTreeMap,
    fs,
    path::{Path, PathBuf},
    rc::Rc,
};

const BUILD: &str = "m6-build";

/// Lines the packages log (their own log calls and Lua's warnings), as "<owner>: <text>".
#[derive(Default)]
struct Log(RefCell<Vec<String>>);
impl LogSink for Log {
    fn enabled(&self, _: LogLevel) -> bool {
        true
    }
    fn write(&self, owner: &str, _: LogLevel, payload: &[u8]) {
        self.0
            .borrow_mut()
            .push(format!("{owner}: {}", String::from_utf8_lossy(payload)));
    }
}
impl Log {
    fn count(&self, line: &str) -> usize {
        self.0.borrow().iter().filter(|l| l.as_str() == line).count()
    }
    fn lines(&self) -> Vec<String> {
        self.0.borrow().clone()
    }
}
struct FixedClock;
impl Clock for FixedClock {
    fn seconds(&self) -> f64 {
        1.5
    }
}
/// A build contract for the tests: the lifecycle is supported, every other operation is not.
struct Catalog;
impl RuntimeCatalog for Catalog {
    fn game_build(&self) -> &str {
        BUILD
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

/// Every package's src/mod.lua starts with these helpers.
const PRELUDE: &str = r##"
local function eq(got, want, what)
  if got ~= want then error((what or "value") .. ": got " .. tostring(got) .. ", want " .. tostring(want), 2) end
end
local function raises(text, f, ...)
  local ok, e = pcall(f, ...)
  assert(not ok, "expected an error: " .. text)
  assert(string.find(tostring(e), text, 1, true), "got: " .. tostring(e))
end
"##;

/// One game folder (the synthetic archive with its baselines), the held install lock, a mods folder and a log.
struct Setup {
    f: ArchiveFixture,
    guard: fs::File,
    log: Rc<Log>,
}
impl Setup {
    fn new(tag: &str) -> Self {
        Self::over(ArchiveFixture::new(tag))
    }
    /// A game folder whose archive is the synthetic attribute archive (fixtures/m6/startup/attributes).
    fn attributes(tag: &str) -> Self {
        let (types, resources) = attribute_archive();
        Self::over(ArchiveFixture::with_contents(tag, types, &resources, &[]))
    }
    fn over(f: ArchiveFixture) -> Self {
        for ext in ["kfc", "kfc_resources"] {
            fs::copy(
                f.game.join(format!("{STEM}.{ext}")),
                f.game.join(format!("{STEM}.{ext}.bak")),
            )
            .unwrap();
        }
        fs::create_dir_all(f.root.0.join("mods")).unwrap();
        let guard = fs::File::create(f.game.join(".kiln-eml.lock")).unwrap();
        guard.try_lock().unwrap();
        Self {
            f,
            guard,
            log: Rc::default(),
        }
    }
    /// The packages live beside the game folder, so "the game folder is byte-identical" is about game data only.
    fn mods(&self) -> PathBuf {
        self.f.root.0.join("mods").into_std_path_buf()
    }
    /// A package folder: mod.json from \`manifest\` (id, name, version added), extended.mod.json from \`extension\`
    /// (schemaVersion and enabled added unless given), \`src/mod.lua\` with the prelude (unless \`lua\` is None) and
    /// \`files\` below src/.
    fn package(&self, id: &str, capabilities: &[&str], lua: Option<&str>) -> PathBuf {
        self.package_with(
            id,
            id,
            serde_json::json!({ "capabilities": capabilities }),
            serde_json::json!({}),
            lua,
            &[],
        )
    }
    fn package_with(
        &self,
        dir: &str,
        id: &str,
        mut manifest: serde_json::Value,
        mut extension: serde_json::Value,
        lua: Option<&str>,
        files: &[(&str, &str)],
    ) -> PathBuf {
        let path = self.mods().join(dir);
        fs::create_dir_all(path.join("src")).unwrap();
        manifest["id"] = id.into();
        manifest["name"] = id.into();
        manifest["version"] = "1.0.0".into();
        extension["schemaVersion"] = 1.into();
        if extension.get("enabled").is_none() {
            extension["enabled"] = true.into();
        }
        fs::write(path.join("mod.json"), serde_json::to_vec(&manifest).unwrap()).unwrap();
        fs::write(path.join("extended.mod.json"), serde_json::to_vec(&extension).unwrap()).unwrap();
        if let Some(lua) = lua {
            fs::write(path.join("src/mod.lua"), format!("{PRELUDE}\n{lua}")).unwrap();
        }
        for (name, text) in files {
            fs::write(path.join("src").join(name), text).unwrap();
        }
        path
    }
    fn registry(&self) -> Registry {
        let report = discover(
            &PackageRoots {
                mods: self.mods(),
                config: self.f.root.0.join("config").into_std_path_buf(),
                profiles: self.f.root.0.join("profiles").into_std_path_buf(),
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
    /// The plan over every discovered package, in SF's startup load order (client side).
    fn plan(&self, features: Features, export: Option<ExportRoot>, locale: Option<&str>) -> StartupPlan {
        let registry = self.registry();
        let order = startup_plan(&registry, false, None);
        assert!(order.issues.is_empty(), "{:?}", order.issues);
        StartupPlan::new(
            registry,
            order.ids,
            Side::Client,
            SourceVersion::Sf179,
            features,
            export,
            Rc::new(Catalog),
            locale.map(str::to_owned),
        )
        .unwrap()
    }
    fn exports(&self) -> ExportRoot {
        let root = self.f.root.0.join("exports");
        fs::create_dir_all(&root).unwrap();
        ExportRoot::select(root.as_std_path(), self.f.game.as_std_path()).unwrap()
    }
    /// A new StartupHost generation over the captured baseline, under the held install lock.
    fn host(&self) -> StartupHost<'_> {
        let base = Rc::new(KFCFile::from_path(self.f.game.join(format!("{STEM}.kfc.bak")), false).unwrap());
        StartupHost::open(self.f.game.as_std_path(), STEM, self.f.types.clone(), base, &self.guard).unwrap()
    }
    fn ports(&self) -> Ports {
        Ports {
            clock: Rc::new(FixedClock),
            log: self.log.clone(),
        }
    }
    /// One attempt of the plan's whole pass on a new generation.
    fn attempt(
        &self,
        plan: &StartupPlan,
        on_step: impl FnMut(Step<'_>) -> anyhow::Result<()>,
    ) -> (Result<PreparedStartup<'_>, PassError>, SideEffects) {
        let mut effects = SideEffects::default();
        let mut host = self.host();
        let result = run_attempt(&mut host, plan, &plan.pass_plan(), &self.ports(), &mut effects, on_step);
        (result, effects)
    }
    /// Every file of the game folder (the stage and the lock aside).
    fn live_files(&self) -> BTreeMap<String, Vec<u8>> {
        fn visit(root: &Path, path: &Path, out: &mut BTreeMap<String, Vec<u8>>) {
            for e in fs::read_dir(path).unwrap().flatten() {
                let name = e.file_name().to_string_lossy().into_owned();
                if name.starts_with(".kiln-eml-stage-") || name == ".kiln-eml.lock" {
                    continue;
                }
                if e.file_type().unwrap().is_dir() {
                    visit(root, &e.path(), out);
                } else {
                    let key = e
                        .path()
                        .strip_prefix(root)
                        .unwrap()
                        .to_string_lossy()
                        .replace('\\', "/");
                    out.insert(key, fs::read(e.path()).unwrap());
                }
            }
        }
        let mut out = BTreeMap::new();
        visit(self.f.game.as_std_path(), self.f.game.as_std_path(), &mut out);
        out
    }
    /// The game folder is `before`: names only of what differs, never the bytes.
    fn assert_unchanged(&self, before: &BTreeMap<String, Vec<u8>>, what: &str) {
        let after = self.live_files();
        let differing: Vec<&String> = before
            .keys()
            .chain(after.keys())
            .filter(|k| before.get(*k) != after.get(*k))
            .collect();
        assert!(differing.is_empty(), "{what}: the game folder changed: {differing:?}");
    }
    /// No stage holds anything: each one is gone, or kept empty on non-Windows builds (never removed by path there).
    fn stages_reclaimed(&self) -> bool {
        fs::read_dir(&self.f.game).unwrap().flatten().all(|e| {
            let stage = e.path();
            !e.file_name().to_string_lossy().starts_with(".kiln-eml-stage-")
                || (cfg!(not(windows)) && fs::read_dir(&stage).is_ok_and(|mut d| d.next().is_none()))
        })
    }
    fn semantics(&self) -> archive::SemanticSet {
        archive::semantics(&self.f.game, STEM, &self.f.types).unwrap()
    }
}

fn root_with(extra: u64, children: Vec<Value>) -> Value {
    types::structure([
        ("child", types::child(7, extra)),
        ("fixed", Value::Array(vec![Value::UInt(2), Value::UInt(3)])),
        ("children", Value::Array(children)),
        ("maybe", types::child(29, 31)),
        (
            "choice",
            Value::Variant(Box::new(Variant {
                type_index: TypeIndex::new(2),
                value: types::child(37, 41).into_struct().unwrap(),
            })),
        ),
        ("text", Value::String("synthetic".into())),
    ])
}
fn json(value: &Value) -> serde_json::Value {
    serde_json::to_value(value).unwrap()
}
fn created_key() -> String {
    archive::resource_key(&ResourceId::new(
        Guid::parse(archive::CREATED_GUID).unwrap(),
        fnv(types::ROOT),
        0,
    ))
}

/// The editing package of the end-to-end tests: a nested Resource.data edit, a typed-array insert through the table
/// port, the Buffer resource port on the same wrappers (game.types.of answers them through the wrappers' recognizer), a new
/// content and a new resource.
const EDITS: &str = r##"
local a = game.assets
local r = a.get_resource("01234567-89ab-cdef-0123-456789abcdef", "m6.Root", 0)
r.data.child.extra = 99
table.insert(r.data.children, {base = 1, extra = 2})
eq(#r.data.children, 3, "typed insert")
local b = buffer.create()
eq(select("#", b:write_resource("m6.Root", r.data)), 0, "write_resource results")
local back = b:read_resource(game.types.get("m6.Root"))
eq(back.child.extra, 99, "the Buffer port decodes what it encoded")
assert(rawequal(game.types.of(back), game.types.get("m6.Root")), "game.types.of answers a decoded wrapper")
assert(rawequal(game.types.of(r.data), game.types.get("m6.Root")), "game.types.of answers Resource.data")
a.create_content(buffer.wrap("g09 content"))
a.create_resource(r.data, "m6.Root", "fedcba98-7654-3210-fedc-ba9876543210", 0)
shroudforge.log.info("edits done")
"##;

// FINAL (startup): the pass end to end. A package's startup edits (Resource.data nested edit, typed insert, a new
// content, a new resource) reach the published archive through the one stage, serialized by AssetSession::prepare and
// published by the sole publisher (receipt written); the Buffer port and the table port are attached in the
// production VM; ran/applied report what happened.
#[test]
fn a_packages_startup_edits_reach_the_published_archive() {
    let s = Setup::new("g09-e2e");
    s.package("editor", &["patch"], Some(EDITS));
    let plan = s.plan(
        Features {
            patch: true,
            export: false,
        },
        None,
        None,
    );
    let before = s.semantics();
    let (result, effects) = s.attempt(&plan, |_| Ok(()));
    let prepared = result.unwrap();
    assert_eq!(prepared.ran(), ["editor"]);
    assert_eq!(prepared.applied(), 2, "the edited record and the created one");
    assert_eq!(effects.entered, ["editor"]);
    assert!(effects.exporting.is_empty());
    assert_eq!(s.semantics(), before, "nothing is live before publication");
    let published = prepared.publish().unwrap().expect("a patching pass publishes");
    assert!(published.receipt_sha256().is_some(), "the publication is receipted");
    let live = s.semantics();
    let edited = json(&root_with(
        99,
        vec![types::child(13, 17), types::child(19, 23), types::child(1, 2)],
    ));
    assert_eq!(live.resources[&archive::resource_key(&archive::resource_id(0))], edited);
    assert_eq!(live.resources[&created_key()], edited, "the created resource");
    for part in [9, 3] {
        let key = archive::resource_key(&archive::resource_id(part));
        assert_eq!(live.resources[&key], before.resources[&key], "untouched part {part}");
    }
    assert_eq!(live.resources.len(), 4);
    assert_eq!(
        live.contents[&ContentHash::from_data(b"g09 content").to_string()],
        b"g09 content"
    );
    assert_eq!(live.contents.len(), 4);
    assert_eq!(s.log.count("editor: edits done"), 1);
    assert!(s.stages_reclaimed());
}

// The VM outlives AssetSession::prepare (the nested edit lives in it and still reaches the archive), and the lent
// handles are closed before the stage is serialized and the VM closes: the attempt reports Prepared before VmClosed,
// and a finalizer that runs when the VM closes (Lua's warning shows it ran) gets no content into the stage or the
// published archive. (At the VM's close mlua refuses every Rust callback, so the finalizer's call fails before it
// reaches the writer, which is closed by then anyway.)
#[test]
fn the_vm_outlives_prepare_and_late_writers_meet_the_closed_writer() {
    let s = Setup::new("g09-late");
    s.package(
        "late",
        &["patch"],
        Some(&format!(
            r##"{EDITS}
keep = setmetatable({{}}, {{__gc = function() game.assets.create_content(buffer.wrap("from a finalizer")) end}})
"##
        )),
    );
    let plan = s.plan(
        Features {
            patch: true,
            export: false,
        },
        None,
        None,
    );
    let mut steps = Vec::new();
    let (result, _) = s.attempt(&plan, |step| {
        steps.push(format!("{step:?}"));
        Ok(())
    });
    let prepared = result.unwrap();
    assert_eq!(
        steps[steps.len() - 2..],
        ["Prepared", "VmClosed"],
        "prepared before the VM closed"
    );
    let late = ContentHash::from_data(b"from a finalizer");
    assert!(prepared.archive().unwrap().read_content(&late).unwrap().is_none());
    assert!(
        s.log
            .lines()
            .iter()
            .any(|l| l.starts_with("lua: error in") && l.contains("__gc")),
        "the finalizer ran at the VM's close: {:?}",
        s.log.lines()
    );
    prepared.publish().unwrap().unwrap();
    let live = s.semantics();
    assert!(!live.contents.contains_key(&late.to_string()));
    assert_eq!(live.contents.len(), 4);
    assert_eq!(
        live.resources[&archive::resource_key(&archive::resource_id(0))]["child"]["extra"],
        99
    );
}

/// The packages of the boundary matrix: two editors and a reader.
fn matrix_packages(s: &Setup) {
    s.package("one", &["patch"], Some(EDITS));
    s.package(
        "two",
        &["patch"],
        Some(r##"game.assets.create_content(buffer.wrap("two's content"))"##),
    );
    s.package("three", &["export"], Some("shroudforge.log.info('three ran')"));
}

// Every step the attempt reports is a boundary: an error there (from the caller's on_step) stops the attempt as a
// failure no package raised, drops the lent stage (or the prepared one), and leaves the game folder byte-identical, the
// stage reclaimed. So does a package's own Lua error, a preparation refused because the install guard is no longer
// held, and a publication refused for the same reason. The same install then runs the pass whole and publishes.
#[test]
fn an_error_at_every_step_aborts_the_stage_and_leaves_the_game_folder_byte_identical() {
    let probe = Setup::new("g09-steps-probe");
    matrix_packages(&probe);
    let mut steps = Vec::new();
    let plan = probe.plan(
        Features {
            patch: true,
            export: true,
        },
        Some(probe.exports()),
        None,
    );
    let (result, _) = probe.attempt(&plan, |step| {
        steps.push(format!("{step:?}"));
        Ok(())
    });
    result.unwrap();
    assert_eq!(
        steps,
        [
            "StageBegun",
            "ReaderOpened",
            "VmReady",
            "Registered(\"one\")",
            "Registered(\"three\")",
            "Registered(\"two\")",
            "EntryRan(\"one\")",
            "EntryRan(\"three\")",
            "EntryRan(\"two\")",
            "Prepared",
            "VmClosed",
        ]
    );

    let s = Setup::new("g09-steps");
    matrix_packages(&s);
    let plan = s.plan(
        Features {
            patch: true,
            export: true,
        },
        Some(s.exports()),
        None,
    );
    let before = s.live_files();
    for (i, boundary) in steps.iter().enumerate() {
        let mut seen = 0;
        let (result, _) = s.attempt(&plan, |step| {
            seen += 1;
            if seen == i + 1 {
                anyhow::bail!("injected failure at {step:?}");
            }
            Ok(())
        });
        match result {
            Err(PassError::Other(e)) => assert_eq!(e.to_string(), format!("injected failure at {boundary}")),
            Err(e) => panic!("{boundary}: {e}"),
            Ok(_) => panic!("{boundary}: the attempt did not stop"),
        }
        s.assert_unchanged(&before, boundary);
        assert!(s.stages_reclaimed(), "{boundary}: the stage was not aborted");
    }

    // A package's own error: the package is named, nothing was published.
    let boom = s.package(
        "zz-boom",
        &["patch"],
        Some(r##"game.assets.create_content(buffer.wrap("boom's")); error("boom")"##),
    );
    let plan_boom = s.plan(
        Features {
            patch: true,
            export: true,
        },
        Some(s.exports()),
        None,
    );
    let (result, effects) = s.attempt(&plan_boom, |_| Ok(()));
    match result {
        Err(PassError::Package(failure)) => {
            assert_eq!(failure.mod_id, "zz-boom");
            assert!(failure.message.contains("boom"), "{}", failure.message);
            assert!(!failure.io);
        }
        Err(e) => panic!("{e}"),
        Ok(_) => panic!("the failing package did not fail the attempt"),
    }
    assert_eq!(effects.entered, ["one", "three", "two", "zz-boom"]);
    assert_eq!(
        effects.exporting,
        ["one", "three", "two", "zz-boom"],
        "the pass exports: any entry may"
    );
    s.assert_unchanged(&before, "a package's error");
    assert!(s.stages_reclaimed());
    fs::remove_dir_all(boom).unwrap();

    // The install guard lost before preparation: preparation is refused and aborts.
    let (result, _) = s.attempt(&plan, |step| {
        if step == Step::EntryRan("two") {
            s.guard.unlock()?;
        }
        Ok(())
    });
    assert!(
        matches!(result, Err(PassError::Other(_))),
        "prepared without the install guard"
    );
    s.guard.try_lock().unwrap();
    s.assert_unchanged(&before, "a refused preparation");
    assert!(s.stages_reclaimed());

    // ... and before publication: the sole publisher refuses, the prepared generation is dropped.
    let (result, _) = s.attempt(&plan, |_| Ok(()));
    let prepared = result.unwrap();
    s.guard.unlock().unwrap();
    assert!(prepared.publish().is_err(), "published without the install guard");
    s.guard.try_lock().unwrap();
    s.assert_unchanged(&before, "a refused publication");
    assert!(s.stages_reclaimed());

    // The same install, the whole pass.
    let (result, _) = s.attempt(&plan, |_| Ok(()));
    result.unwrap().publish().unwrap().unwrap();
    let live = s.semantics();
    assert_eq!(
        live.resources[&archive::resource_key(&archive::resource_id(0))]["child"]["extra"],
        99
    );
    assert!(
        live.contents
            .contains_key(&ContentHash::from_data(b"two's content").to_string())
    );
}

// SF LuaModRunner::run: a registered package runs its entrypoint early only when it requires the pregame pass (patch,
// or export without a runtime capability) AND its patch meets the global PATCH or its export the global EXPORT. A
// runtime package with export, a runtime-only package and a native-registering one are registered but never run early;
// their top level never executes. Without PATCH no stage exists and nothing is published.
#[test]
fn entries_follow_requires_pregame_and_the_global_features() {
    let s = Setup::new("g09-select");
    let entry = "shroudforge.log.info('entry ' .. shroudforge.mod_id)";
    let never = "error('this top level must not run early')";
    s.package("p-both", &["patch", "export"], Some(entry));
    s.package("p-export", &["export"], Some(entry));
    s.package("p-patch", &["patch"], Some(entry));
    s.package("rt-dll", &["runtime-register-dll", "export"], Some(never));
    s.package("rt-export", &["runtime", "export"], Some(never));
    s.package("rt-only", &["runtime"], Some(never));
    s.package("rt-patch", &["runtime", "patch"], Some(entry));
    let before = s.live_files();
    for (patch, export, want) in [
        (true, true, &["p-both", "p-export", "p-patch", "rt-patch"][..]),
        (true, false, &["p-both", "p-patch", "rt-patch"][..]),
        (false, true, &["p-both", "p-export"][..]),
        (false, false, &[][..]),
    ] {
        s.log.0.borrow_mut().clear();
        let plan = s.plan(Features { patch, export }, export.then(|| s.exports()), None);
        assert_eq!(plan.order().len(), 7, "every enabled package is registered");
        let (result, effects) = s.attempt(&plan, |_| Ok(()));
        let prepared = result.unwrap();
        assert_eq!(prepared.ran(), want, "patch {patch} export {export}");
        assert_eq!(effects.entered, want);
        let logged: Vec<String> = want.iter().map(|id| format!("{id}: entry {id}")).collect();
        assert_eq!(s.log.lines(), logged, "patch {patch} export {export}");
        assert_eq!(prepared.archive().is_some(), patch);
        if !patch {
            assert!(prepared.publish().unwrap().is_none());
            s.assert_unchanged(&before, "a pass without PATCH");
        }
    }
}

// SF require_mod / parse_require_path / C4: entrypoints by exact manifest id, so a dotted id runs; but every module
// path is qualified as mods.<id>.<path> and split at the first dot, so a dotted package cannot require even its own
// modules ("mod not found: dot", SF 1.7.9's own behaviour) and mods.<dotted id> is not reachable either. A plain package
// requires its own modules by local name and another package's entry module by mods.<id>; results are cached, nil and
// false included (each module runs once). A switched-off package is discovered membership (loader.has_mod) but has no
// environment and never runs.
#[test]
fn dotted_ids_require_aliases_cached_results_and_disabled_membership() {
    let s = Setup::new("g09-require");
    s.package_with(
        "dotted",
        "dot.pkg",
        serde_json::json!({"capabilities": ["patch"]}),
        serde_json::json!({}),
        Some(
            r##"
eq(shroudforge.mod_id, "dot.pkg")
raises("mod not found: dot", require, "helper")
shroudforge.log.info("dot entry")
return false
"##,
        ),
        &[("helper.lua", "error('a dotted package's module ran')")],
    );
    s.package_with(
        "cache",
        "cache",
        serde_json::json!({"capabilities": ["patch"]}),
        serde_json::json!({}),
        Some(
            r##"
local h1, h2 = require("helper"), require("helper")
assert(rawequal(h1, h2), "one helper table")
assert(rawequal(require("mods.cache.helper"), h1), "the qualified alias is the same module")
eq(require("nothing"), nil); eq(require("nothing"), nil)
eq(require("falsy"), false); eq(require("falsy"), false)
eq(require("mods.user.util").from, "user", "a later package's module: every package is registered before any entry runs")
shroudforge.log.info("cache entry")
return {helper = h1}
"##,
        ),
        &[
            ("helper.lua", "shroudforge.log.info('helper ran') return {}"),
            ("nothing.lua", "shroudforge.log.info('nothing ran')"),
            ("falsy.lua", "shroudforge.log.info('falsy ran') return false"),
        ],
    );
    s.package_with(
        "off",
        "switched-off",
        serde_json::json!({"capabilities": ["patch"]}),
        serde_json::json!({"enabled": false}),
        Some("error('a switched-off package ran')"),
        &[],
    );
    s.package_with(
        "user",
        "user",
        serde_json::json!({"capabilities": ["patch"]}),
        serde_json::json!({}),
        Some(
            r##"
eq(loader.has_mod("switched-off"), true, "discovered membership")
eq(loader.has_mod("dot.pkg"), true)
eq(loader.has_mod("not-installed"), false)
raises("mod not found: switched-off", require, "mods.switched-off")
raises("mod not found: dot", require, "mods.dot.pkg")
local cache = require("mods.cache")
assert(rawequal(cache.helper, require("mods.cache.helper")), "another package's module, cached once")
shroudforge.log.info("user entry")
"##,
        ),
        &[("util.lua", "return {from = 'user'}")],
    );
    let plan = s.plan(
        Features {
            patch: true,
            export: false,
        },
        None,
        None,
    );
    assert_eq!(plan.order(), ["cache", "dot.pkg", "user"]);
    let (result, _) = s.attempt(&plan, |_| Ok(()));
    let prepared = result.unwrap();
    assert_eq!(prepared.ran(), ["cache", "dot.pkg", "user"]);
    for line in ["helper ran", "nothing ran", "falsy ran"] {
        assert_eq!(s.log.count(&format!("cache: {line}")), 1, "{line}: {:?}", s.log.lines());
    }
    // the entry ran once as the entrypoint (mods:cache) and once as user's mods.cache module, as in SF
    assert_eq!(s.log.count("cache: cache entry"), 2, "{:?}", s.log.lines());
    assert_eq!(s.log.count("dot.pkg: dot entry"), 1);
    assert_eq!(s.log.count("user: user entry"), 1);
}

// SF bindings/mod.rs:66-114 composed by sfi-core with the runtime registrar on the GPL game table: one game table per package with
// version, guid, assets and types; runtime.types IS game.types; builtin aliases; one Type per index across packages
// while each package has its own types table; the pregame phase's runtime answers and denials (the runtime registrar over the
// plan's build catalog and its owner's manifest facts apart from the global features); loader features against the
// capabilities; families the startup VM does not have are absent, never stand-ins.
#[test]
fn one_game_table_per_package_with_runtime_types_alias_and_pregame_answers() {
    let s = Setup::new("g09-compose");
    s.package(
        "alpha",
        &["patch", "runtime"],
        Some(
            r##"
assert(rawequal(runtime.types, game.types), "runtime.types is game.types")
assert(rawequal(builtin.game, game) and rawequal(builtin.runtime, runtime) and rawequal(builtin.loader, loader))
eq(game.version, "m6-synthetic-v1", "game.version")
assert(type(game.guid.hash) == "function" and type(game.assets.get_resource) == "function")
eq(runtime.phase, "pregame"); eq(runtime.schema_version, 1); eq(runtime.is_client, true); eq(runtime.is_server, false)
local v, why = runtime.is_main_menu(); eq(v, nil); eq(why, "runtime APIs are available only during the ingame phase")
v, why = runtime.player.name(); eq(v, nil); eq(why, "runtime APIs are available only during the ingame phase")
local ok, r = runtime.report_effect("waiting"); eq(ok, false)
eq(r, "runtime effect reporting requires an active ingame runtime mod")
eq(runtime.has("runtime.lifecycle"), false, "has: not in the pregame phase")
eq(runtime.has("game.assets.write"), true, "has: global PATCH and the patch capability")
eq(runtime.has("export"), false, "has: no export capability")
eq(runtime.status("runtime.lifecycle").available, true, "status: the build's contract")
eq(runtime.status("runtime.ecs.read").reason, "not in the m6 contract")
eq(runtime.status("runtime.made.up").reason, "operation runtime.made.up has not been verified for game build m6-build")
eq(runtime.status("export").reason, "mod 'alpha' lacks the required capability")
local ops = runtime.get_operations()
eq(ops["runtime.lifecycle"].available, false)
eq(ops["runtime.lifecycle"].reason, "runtime APIs are available only during the ingame phase")
raises("ShroudForge feature is unavailable: runtime.lifecycle", runtime.require, "runtime.lifecycle")
eq(loader.features.patch, true); eq(loader.features.export, false); eq(loader.is_client, true); eq(loader.is_server, false)
eq(shroudforge.mod_id, "alpha"); eq(shroudforge.clock(), 1.5)
-- the families ShroudForge's pregame VM has (lua/ingame/mod.rs:45-82, bindings/mod.rs:76-87)
for _, family in ipairs({"ecs", "impact", "services", "overrides", "world", "network", "patch", "functions", "values"}) do
  eq(type(runtime[family]), "table", "runtime." .. family)
end
-- SF's pregame loader (create_loader_api.rs:22-46; RUNTIME_DLL is set in the ingame phase only,
-- create_mod_session.rs:75-80): no runtime DLL support, register_dll converts its argument, then refuses
eq(loader.features.runtime.dll, false, "loader.features.runtime.dll in the pregame phase")
raises("EML runtime DLL support is unavailable in this phase", loader.runtime.register_dll, "native/alpha.dll")
raises("error converting Lua nil to String", loader.runtime.register_dll)
-- io.export follows the pass's global EXPORT only (manage_exports.rs:5), whatever the package's capabilities
io.export("alpha/out.txt", "exported by alpha")
return {types = game.types, root = game.types.get("m6.Root"), game = game, assets = game.assets}
"##,
        ),
    );
    s.package(
        "beta",
        &["export"],
        Some(
            r##"
eq(runtime.has("game.assets.write"), false, "has: no patch capability")
eq(runtime.has("export"), true, "has: global EXPORT and the export capability")
eq(runtime.status("game.assets.write").reason, "mod 'beta' lacks the required capability")
eq(runtime.status("runtime.lifecycle").reason, "mod 'beta' lacks the required capability")
eq(loader.features.export, true); eq(loader.features.patch, false)
local a = require("mods.alpha")
assert(not rawequal(a.game, game), "each package has its own game table")
assert(not rawequal(a.types, game.types) and not rawequal(a.assets, game.assets), "and its own types and assets tables")
assert(rawequal(a.root, game.types.get("m6.Root")), "one Type per index in the VM")
io.export("beta/out.txt", "exported by beta")
raises("invalid path", io.export, "../game/enshrouded.kfc", "x")
shroudforge.log.info("beta entry")
"##,
        ),
    );
    let exports = s.exports();
    let root = exports.path().to_owned();
    let plan = s.plan(
        Features {
            patch: true,
            export: true,
        },
        Some(exports),
        None,
    );
    let before = s.semantics();
    let (result, effects) = s.attempt(&plan, |_| Ok(()));
    let prepared = result.unwrap();
    assert_eq!(prepared.ran(), ["alpha", "beta"]);
    assert_eq!(
        effects.exporting,
        ["alpha", "beta"],
        "the side effects the stage cannot take back: every entered package may export when the pass exports"
    );
    assert_eq!(fs::read(root.join("alpha/out.txt")).unwrap(), b"exported by alpha");
    assert_eq!(fs::read(root.join("beta/out.txt")).unwrap(), b"exported by beta");
    assert_eq!(s.log.count("beta: beta entry"), 1);
    prepared.publish().unwrap().unwrap();
    assert_eq!(s.semantics(), before, "nothing was edited");
}

// ---- The runtime families of the pregame VM ----------------------------------------------------------------

/// A package's environment in a startup VM composed as `common::register` composes it (base environment, `game.assets`
/// over an asset session with the loaded-resource functions at their place, the core's composition with the runtime registrar over a
/// startup-phase host, then the pregame runtime families), over `registry` and the Setup's archive (the attribute
/// catalog reads it). For the oracle replay (ShroudForge's oracle has an empty registry and no reader; the fixture never
/// reaches the archive) and the registry-backed family checks.
struct PregameVm {
    vm: sfi_core::lua::LuaVm,
    env: mlua::Table,
    /// The VM's asset session.
    assets: kiln_eml::sf_startup::game::assets::AssetSession,
}
fn pregame_vm(
    s: &Setup,
    id: &str,
    capabilities: &[&str],
    side: Side,
    features: Features,
    registry: kfc::reflection::TypeRegistry,
) -> PregameVm {
    use kiln_eml::sf_startup::{
        SelectedTarget, TypeSession,
        common::StartupRuntime,
        game::{self as startup_game, assets::AssetSession, resource::AssetIdentities, types as game_types},
        pregame_runtime::{self, PregameRuntime},
        resource_values::{ValueObjects, objects::ValueTables},
    };
    use sfi_core::{
        bindings::{OwnerBindings, loader::LoaderSnapshot, runtime::RuntimeOwnerContext},
        compose::{self, Composition, GameTypes},
        eml::io::ExportConfig,
        host::{HostContext, Package, Phase},
        locales::LocaleSource,
        lua::{LuaVm, VmAttachments},
    };
    struct Types(TypeSession);
    impl GameTypes for Types {
        fn types(&self, lua: &mlua::Lua, _: &Package) -> mlua::Result<mlua::Table> {
            game_types::create(lua, &self.0)
        }
    }
    struct NoLocale;
    impl LocaleSource for NoLocale {
        fn locale(&self) -> Option<String> {
            None
        }
    }
    s.package(id, capabilities, Some(""));
    let discovered = s.registry();
    let record = discovered.get(id).unwrap();
    let manifest = record.parsed().manifest.clone();
    let mut package = record.runtime().clone();
    let exports = s.exports();
    package.export_root = Some(exports.path().to_owned());
    let mut context = HostContext::new(Phase::Startup, side, SourceVersion::Sf179);
    context.clock = Some(Rc::new(FixedClock));
    context.log = Some(s.log.clone());
    let vm = LuaVm::new(context).unwrap();
    let objects = ValueObjects::with_assets(Rc::new(AssetIdentities));
    vm.attach(VmAttachments {
        typed_tables: Some(Rc::new(ValueTables)),
        buffer_resources: None,
    })
    .unwrap();
    let types = TypeSession::new(
        vm.lua(),
        Rc::new(registry),
        SelectedTarget {
            stem: "enshrouded".into(),
            game_version: String::new(),
        },
    );
    types.add_type_recognizer(kiln_eml::sf_startup::resource_values::recognizer());
    let assets = AssetSession::with_objects(
        &types,
        Rc::new(KFCFile::from_path(s.f.game.join(format!("{STEM}.kfc")), false).unwrap()),
        kfc::container::KFCReader::new(&s.f.game, STEM)
            .unwrap()
            .into_cursor()
            .unwrap(),
        None,
        objects.clone(),
    );
    let context = vm.context().clone();
    let env = vm
        .add_package(package, |lua, env, package| {
            let game: mlua::Table = env.raw_get("game")?;
            let host: Rc<dyn sfi_core::bindings::runtime::RuntimeHost> = Rc::new(StartupRuntime::new(
                RuntimeOwnerContext::new(&manifest, &context, features.patch, features.export, BUILD),
                Rc::new(Catalog),
            ));
            let loaded = host.clone();
            startup_game::install_assets_with(lua, &game, &assets, move |lua, table| {
                pregame_runtime::attach_loaded_resources(lua, table, loaded)
            })?;
            let composed = compose::install(
                lua,
                env,
                package,
                &context,
                Composition {
                    owner: OwnerBindings {
                        manifest: manifest.clone(),
                        locale: Rc::new(NoLocale),
                        loader: LoaderSnapshot {
                            patch: features.patch,
                            export: features.export,
                            discovered_ids: [id.to_owned()].into_iter().collect(),
                        },
                    },
                    exports: ExportConfig {
                        enabled: features.export,
                        root: package.export_root.clone(),
                    },
                    game_types: Some(Rc::new(Types(types.clone()))),
                    runtime: Some(host.clone()),
                },
            )?;
            pregame_runtime::install(
                lua,
                &composed.game,
                composed.runtime.as_ref().unwrap(),
                &PregameRuntime {
                    host,
                    types: types.clone(),
                    objects: objects.clone(),
                    assets: assets.clone(),
                },
            )
        })
        .unwrap();
    PregameVm { vm, env, assets }
}

/// ShroudForge's own mod runtime (1.7.9, in a local differential test, not published: pregame, server side, global EXPORT on and
/// PATCH off, an empty registry and archive, no game process) ran fixtures/m6/startup/oracle/runtime-pregame.lua on
/// both pins (1.7.9, Kiln's target, and 1.8.0, which answers the same); the same fixture runs here in a startup VM of
/// the same construction. Byte-identical shared-v2 records: every runtime.*
/// table's keys, then each family function's argument conversions, raised texts, refusals and result shapes (140
/// records). The pregame runtime surface table by table and function by function.
#[test]
fn oracle_pregame_runtime_transcript_matches_shroudforge() {
    let fixture = include_str!("fixtures/m6/startup/oracle/runtime-pregame.lua");
    let goldens = [
        include_str!("fixtures/m6/startup/oracle/sf-1.7.9/runtime-pregame.json"),
        include_str!("fixtures/m6/startup/oracle/sf-1.8.0/runtime-pregame.json"),
    ];
    let s = Setup::new("g09b-oracle");
    let p = pregame_vm(
        &s,
        "oracle",
        &["export"],
        Side::Server,
        Features {
            patch: false,
            export: true,
        },
        kfc::reflection::TypeRegistry::default(),
    );
    let env = p.env.clone();
    let ours = sfi_core::conformance::run_fixture_in_vm(p.vm.lua(), fixture, move |_| Ok(env)).unwrap();
    sfi_core::conformance::validate_transcript(&ours).unwrap();
    for golden in goldens {
        if let Err(difference) = sfi_core::conformance::compare(golden, &ours) {
            panic!("{difference}\nours: {ours}");
        }
    }
}

/// A registry with ECS components for the registry-backed checks: an owned component with a reflected default, one with
/// engine-allocated storage, a non-ECS struct with a static array and a blob string.
fn ecs_registry() -> kfc::reflection::TypeRegistry {
    use kfc::reflection::{PrimitiveType as P, StructFieldMetadata, TypeFlags, TypeIndex, TypeMetadata};
    fn meta(index: usize, name: &str, primitive_type: P, size: u32) -> TypeMetadata {
        TypeMetadata {
            index: TypeIndex::new(index),
            name: name.rsplit("::").next().unwrap().into(),
            impact_name: name.into(),
            qualified_hash: fnv(name),
            internal_hash: 0xb600_0000 + index as u32,
            name_hash: fnv(name),
            impact_hash: fnv(name),
            qualified_name: name.into(),
            namespace: vec![],
            inner_type: None,
            size,
            alignment: 4,
            element_alignment: 4,
            field_count: 0,
            primitive_type,
            flags: TypeFlags::NONE,
            struct_fields: Default::default(),
            enum_fields: Default::default(),
            default_value: None,
            attributes: Default::default(),
        }
    }
    fn field(t: &mut TypeMetadata, name: &str, index: usize, data_offset: u64) {
        t.struct_fields.insert(
            name.into(),
            StructFieldMetadata {
                name: name.into(),
                r#type: TypeIndex::new(index),
                data_offset,
                attributes: Default::default(),
            },
        );
        t.field_count = t.struct_fields.len() as u32;
    }
    let mut types = vec![
        meta(0, "keen::float32", P::Float32, 4),
        meta(1, "keen::ecs::Health", P::Struct, 8),
        meta(2, "keen::ecs::Inventory", P::DsArray, 24),
        meta(3, "keen::uint32", P::UInt32, 4),
        meta(4, "keen::Pair", P::StaticArray, 8),
        meta(5, "keen::Text", P::BlobString, 8),
        meta(6, "keen::Holder", P::Struct, 16),
    ];
    field(&mut types[1], "value", 0, 0);
    field(&mut types[1], "max", 0, 4);
    types[1].default_value = Some([1.0f32.to_le_bytes(), 10.0f32.to_le_bytes()].concat());
    types[2].inner_type = Some(TypeIndex::new(3));
    types[4].inner_type = Some(TypeIndex::new(3));
    types[4].field_count = 2;
    field(&mut types[6], "pair", 4, 0);
    field(&mut types[6], "text", 5, 8);
    serde_json::from_value(serde_json::json!({"version": "g09b-ecs", "types": types})).unwrap()
}

/// Source-derived where the oracle's empty registry cannot reach: runtime.values works before the game starts on a
/// real registry (values/register_value_api.rs; decode, new, encode and normalize_field with their errors),
/// runtime.ecs.get_catalog lists the keen::ecs types with every live fact false and the codec's reason
/// (read_component_catalog.rs), the claims and functions.find resolve a real selector before refusing, the network
/// status names the client role, and the attribute-catalog members answer ShroudForge's text for an archive without
/// attribute definitions.
#[test]
fn runtime_families_answer_pregame_on_a_real_registry() {
    let s = Setup::new("g09b-registry");
    let p = pregame_vm(
        &s,
        "reader",
        &["runtime"],
        Side::Client,
        Features {
            patch: false,
            export: false,
        },
        ecs_registry(),
    );
    let chunk = format!(
        "{PRELUDE}\n{}",
        r##"
local v = runtime.values
local h = assert(v.new("keen::ecs::Health"))
eq(h.value, 1.0, "new decodes the reflected default"); eq(h.max, 10.0)
local bytes, why = v.encode("keen::ecs::Health", {value = 2.5, max = 4})
eq(why, nil); eq(#bytes, 8); eq(string.unpack("<f", bytes), 2.5)
local back = assert(v.decode("keen::ecs::Health", bytes)); eq(back.value, 2.5); eq(back.max, 4.0)
assert(rawequal(game.types.of(back), game.types.get("keen::ecs::Health")), "a decoded wrapper is typed")
local none, r = v.decode("keen::ecs::Health", "abc"); eq(none, nil)
eq(r, "truncated keen::ecs::Health: expected at least 8 bytes")
none, r = v.new("keen::ecs::Inventory")
eq(r, "keen::ecs::Inventory requires engine allocation and ownership. The KFC value codec cannot construct it")
none, r = v.new("keen::Holder"); eq(r, "no reflected default for keen::Holder")
none, r = v.encode("keen::ecs::Health", {value = "x", max = 1}); eq(none, nil); assert(type(r) == "string" and #r > 0)
eq(v.normalize_field("keen::ecs::Health", "max", 7), 7.0)
eq(v.normalize_field("keen::Holder", "pair[2]", 9), 9)
none, r = v.normalize_field("keen::Holder", "pair[3]", 9); eq(r, "static array index or reflected layout is invalid")
none, r = v.normalize_field("keen::Holder", "pair[0]", 9); eq(r, "array indices are one-based")
none, r = v.normalize_field("keen::Holder", "text", "x")
eq(r, "keen::Text has external storage or reference ownership. Select an inline sibling field")
none, r = v.normalize_field("keen::Holder", "nope", 1); eq(r, "reflected field not found: keen::Holder.nope")
none, r = v.normalize_field("keen::Holder", "pair..x", 1); eq(r, "expected a field name after '.'")
none, r = v.normalize_field("keen::ecs::Health", "value", 0/0)
eq(r, "runtime field writes require finite floating-point values")
none, r = v.normalize_field("keen::ecs::Inventory", "x", 1)
eq(r, "keen::ecs::Inventory requires engine allocation and ownership. The KFC value codec cannot construct it")

local c = runtime.ecs.get_catalog()
eq(c.version, "g09b-ecs"); eq(#c.entries, 2); eq(c.reflected_only_count, 2); eq(c.registry_available, false)
eq(c.entity_access_ready, false); eq(c.entity_writes_ready, false); eq(c.resolved_count, 0)
local health, inventory = c.entries[1], c.entries[2]
eq(health.qualified_name, "keen::ecs::Health"); eq(health.size, 8)
assert(rawequal(health.type, game.types.get("keen::ecs::Health")), "the session's own Type")
eq(health.partial_value, false); eq(health.value_reason, nil); eq(health.storage, "unregistered")
eq(health.registration_state, "reflected-only"); eq(health.read_available, false); eq(health.write_available, false)
eq(health.read_reason, "runtime APIs are available only during the ingame phase")
eq(health.write_reason, "runtime APIs are available only during the ingame phase")
eq(health.reason, "reflected type has no engine component registration"); eq(#health.registrations, 0)
-- every live fact false without the provider (read_component_catalog.rs:58-176)
eq(health.registered, false); eq(health.runtime_resolved, false); eq(health.resolved, false)
eq(health.read_fields_available, false); eq(health.write_fields_available, false)
eq(health.write_fields_reason, "runtime APIs are available only during the ingame phase")
eq(c.registered_type_count, 0); eq(c.runtime_type_count, 0); eq(c.template_type_count, 0)
eq(inventory.partial_value, true)
eq(inventory.value_reason, "keen::ecs::Inventory requires engine allocation and ownership. The KFC value codec cannot construct it")
eq(inventory.write_reason, "runtime APIs are available only during the ingame phase", "the denial before the codec reason")
eq(inventory.storage, "unregistered"); eq(inventory.resolved, false)
local n, why2 = runtime.ecs.get_component("keen::ecs::Health")
eq(why2, "Native provider lacks the component registry extension. Install the matching provider")

local ov = runtime.overrides
n, why2 = ov.get_component_claim(1, "keen::Holder")
eq(why2, "runtime ECS operation requires a keen::ecs::* type, got keen::Holder")
n, why2 = ov.get_component_claim(1, "keen::ecs::Health"); eq(why2, "claimed component layout is unavailable")
n, why2 = ov.component(1, "keen::ecs::Health", "value"); eq(why2, "claimed component layout is unavailable")
n, why2 = runtime.functions.find({component = "keen::ecs::Health"})
eq(why2, "native provider lacks the function inventory extension")
eq(runtime.network.status().role, "client")
-- no building request exists before the game: any id is unknown and cannot be cancelled (submit_building_input.rs:135-160)
eq(runtime.world.building.status(7), "unknown"); eq(runtime.world.building.cancel(7), false)

-- the catalog reads the archive (the fixture's has no attribute definitions): SF's answer for an archive without them
local none = "no attribute definitions in this KFC container"
n, why2 = runtime.ecs.get_attributes(); eq(n, nil); eq(why2, none)
n, why2 = runtime.ecs.get_attribute("Stamina"); eq(why2, none)
n, why2 = runtime.ecs.evaluate_attributes("Stamina", {}); eq(why2, none)
raises("error converting Lua nil to table", runtime.ecs.evaluate_attributes, "Stamina")
n, why2 = ov.attribute(1, "Stamina"); eq(n, nil); eq(why2, none)
"##
    );
    let lua = p.vm.lua();
    lua.load(chunk)
        .set_name("=reader")
        .set_environment(p.env.clone())
        .exec()
        .unwrap();
}

/// A patch package that touches every runtime.* table at top level (what ShroudForge runs and the pass composition skipped) runs in
/// the pass and its edit is published: it reads each family's pregame answer, keeps a tracker, takes the Resource claim
/// on a real archive Resource ("write_loaded_field requires a ResourceService snapshot") and computes its edit with
/// runtime.values.
#[test]
fn a_patch_package_touching_every_runtime_table_at_top_level_runs_in_the_pass() {
    let s = Setup::new("g09b-touch");
    s.package(
        "toucher",
        &["patch", "runtime"],
        Some(
            r##"
local phase = "runtime APIs are available only during the ingame phase"
local _, r = runtime.ecs.query("keen::ecs::Anything"); eq(r, phase, "ecs")
_, r = runtime.impact.call("x", {}); eq(r, phase, "impact")
_, r = runtime.services.get("ecs::GameSettings"); eq(r, phase, "services")
eq(runtime.world.session_id(), 0, "world"); eq(runtime.world.context_kind(), "unknown")
_, r = runtime.world.voxel.read(0, 0, 0, 1, 1, 1); eq(r, phase, "world.voxel")
eq(runtime.network.status().reason, phase, "network")
eq(runtime.patch.available("speed"), false, "patch")
_, r = runtime.functions.list(); eq(r, "native provider lacks the function inventory extension", "functions")
local ops = runtime.functions.get_operations(); eq(ops["runtime.ecs.read"].reason, phase)
local tracker = runtime.overrides.new(); tracker:begin_update()
eq(tracker:summary("pregame"), "pregame | writes=0", "overrides")
local res = game.assets.get_resource("01234567-89ab-cdef-0123-456789abcdef", "m6.Root", 0)
_, r = runtime.overrides.get_resource_claim(res); eq(r, "write_loaded_field requires a ResourceService snapshot")
local bytes = assert(runtime.values.encode("m6.Child", {base = 5, extra = 77}))
local child = assert(runtime.values.decode("m6.Child", bytes))
eq(runtime.values.normalize_field("m6.Child", "extra", 77), 77, "values")
res.data.child.extra = child.extra
shroudforge.log.info("touched every table")
"##,
        ),
    );
    let plan = s.plan(
        Features {
            patch: true,
            export: false,
        },
        None,
        None,
    );
    let (result, effects) = s.attempt(&plan, |_| Ok(()));
    let prepared = result.unwrap_or_else(|e| panic!("the pass skipped the package: {e}"));
    assert_eq!(prepared.ran(), ["toucher"]);
    assert_eq!(effects.entered, ["toucher"]);
    assert_eq!(s.log.count("toucher: touched every table"), 1);
    prepared.publish().unwrap().expect("a patching pass publishes");
    let live = s.semantics();
    let edited = json(&root_with(77, vec![types::child(13, 17), types::child(19, 23)]));
    assert_eq!(live.resources[&archive::resource_key(&archive::resource_id(0))], edited);
    assert!(s.stages_reclaimed());
}

// Settings and locale are frozen inputs: each owner reads its own settings (arrays per owner), from the manifest the
// plan discovered, not from a file changed during the attempt; shroudforge.locale answers the plan's locale.
#[test]
fn settings_and_locale_are_frozen_per_owner() {
    let s = Setup::new("g09-settings");
    s.package_with(
        "s-one",
        "s-one",
        serde_json::json!({"capabilities": ["patch"]}),
        serde_json::json!({"settings": {"list": {"value": [1, 2], "options": {"1": "one", "2": "two", "3": "three"}}, "name": {"value": "one"}}}),
        Some(
            r##"
local list = shroudforge.settings.get("list"); eq(#list, 2); eq(list[1], 1)
table.insert(list, 3)
eq(#shroudforge.settings.get("list"), 3, "the package's own array")
eq(shroudforge.settings.get("name"), "one")
eq(shroudforge.locale(), "de")
"##,
        ),
        &[],
    );
    let two = s.package_with(
        "s-two",
        "s-two",
        serde_json::json!({"capabilities": ["patch"]}),
        serde_json::json!({"settings": {"list": {"value": ["x"], "options": {"x": "X", "y": "Y"}}, "name": {"value": "two"}}}),
        Some(
            r##"
local list = shroudforge.settings.get("list"); eq(#list, 1, "another owner's array is untouched"); eq(list[1], "x")
eq(shroudforge.settings.get("name"), "two", "the frozen setting")
eq(shroudforge.locale(), "de")
"##,
        ),
        &[],
    );
    let plan = s.plan(
        Features {
            patch: true,
            export: false,
        },
        None,
        Some("de"),
    );
    fs::write(
        two.join("extended.mod.json"),
        br#"{"schemaVersion":1,"enabled":true,"settings":{"list":{"value":["y"],"options":{"x":"X","y":"Y"}},"name":{"value":"changed"}}}"#,
    )
    .unwrap();
    let (result, _) = s.attempt(&plan, |_| Ok(()));
    assert_eq!(result.unwrap().ran(), ["s-one", "s-two"]);
}

// Setup failures are host failures, never a package's, and nothing is fabricated: a plan naming a package that was not
// discovered, is switched off, is for the other side or is listed twice, a 1.8 dialect, or exports without a root is
// refused; a pass plan that is not the startup plan's (a package it does not plan, or one listed twice) is refused before
// any stage exists; a missing entrypoint is that package's failure (PassError::Package), not the host's.
#[test]
fn setup_failures_are_refused_and_never_a_packages() {
    let s = Setup::new("g09-setup");
    s.package("ok", &["patch"], Some("return true"));
    s.package_with(
        "srv",
        "server-only",
        serde_json::json!({"capabilities": ["patch"]}),
        serde_json::json!({"targets": ["server"]}),
        Some("return true"),
        &[],
    );
    s.package_with(
        "off",
        "switched-off",
        serde_json::json!({"capabilities": ["patch"]}),
        serde_json::json!({"enabled": false}),
        Some("return true"),
        &[],
    );
    s.package("no-entry", &["patch"], None);
    let new = |order: &[&str], version, features: Features, export: Option<ExportRoot>| {
        StartupPlan::new(
            s.registry(),
            order.iter().map(|id| id.to_string()).collect(),
            Side::Client,
            version,
            features,
            export,
            Rc::new(Catalog),
            None,
        )
        .err()
        .map(|e| e.to_string())
    };
    let patch = Features {
        patch: true,
        export: false,
    };
    let v = SourceVersion::Sf179;
    assert_eq!(new(&["ok"], v, patch, None), None);
    assert_eq!(
        new(&["missing"], v, patch, None).unwrap(),
        "planned package is not discovered: missing"
    );
    assert_eq!(
        new(&["switched-off"], v, patch, None).unwrap(),
        "planned package is switched off: switched-off"
    );
    assert_eq!(
        new(&["server-only"], v, patch, None).unwrap(),
        "planned package is for the other side: server-only"
    );
    assert_eq!(
        new(&["ok", "ok"], v, patch, None).unwrap(),
        "planned package is listed twice: ok"
    );
    assert_eq!(
        new(&["ok"], SourceVersion::Sf180, patch, None).unwrap(),
        "the startup pass runs ShroudForge 1.7.9's dialect only"
    );
    assert_eq!(
        new(
            &["ok"],
            v,
            Features {
                patch: true,
                export: true
            },
            None
        )
        .unwrap(),
        "the startup pass exports, but no export root was selected"
    );

    let plan = StartupPlan::new(
        s.registry(),
        vec!["no-entry".into(), "ok".into()],
        Side::Client,
        v,
        patch,
        None,
        Rc::new(Catalog),
        None,
    )
    .unwrap();
    let before = s.live_files();
    let foreign = kiln_eml::sf_pass::PassPlan::new(vec![kiln_eml::sf_pass::PlannedPackage {
        id: "server-only".into(),
        name: "server-only".into(),
        version: "1.0.0".into(),
        requires: vec![],
        optional: vec![],
        pregame: true,
    }]);
    let ok_twice = kiln_eml::sf_pass::PassPlan::new(vec![plan.pass_plan().packages()[1].clone(); 2]);
    for pass in [foreign, ok_twice] {
        let mut host = s.host();
        let mut effects = SideEffects::default();
        let result = run_attempt(&mut host, &plan, &pass, &s.ports(), &mut effects, |_| Ok(()));
        assert!(
            matches!(&result, Err(PassError::Other(e)) if e.to_string().starts_with("the pass plan is not this startup plan's"))
        );
        // the host never began its stage: it can still stage once
        assert!(host.begin_stage().is_ok());
    }
    // A retry's planner may order the packages it keeps differently (kiln mode takes the fork's order): the pass's order is
    // the registration and entry order.
    let reordered = kiln_eml::sf_pass::PassPlan::new(plan.pass_plan().packages().iter().rev().cloned().collect());
    assert_eq!(plan.registered(&reordered).unwrap(), ["ok", "no-entry"]);
    assert!(
        plan.registered(&plan.pass_plan().without("no-entry")).is_ok(),
        "a retry only removes"
    );
    let (result, _) = s.attempt(&plan, |_| Ok(()));
    match result {
        Err(PassError::Package(failure)) => {
            assert_eq!(failure.mod_id, "no-entry");
            assert!(
                failure.message.contains("failed to load module: src/mod.lua"),
                "{}",
                failure.message
            );
            assert!(!failure.io, "the core formats the read error into its text");
        }
        Err(e) => panic!("{e}"),
        Ok(_) => panic!("a package without an entrypoint ran"),
    }
    s.assert_unchanged(&before, "a missing entrypoint");
    assert!(s.stages_reclaimed());
}

// The export root is Kiln-owned and proven apart from game data: the game folder, an alias (link) of it, a folder that
// contains it, Kiln's own folder and its receipts, the package sources and ShroudForge's folder are refused, as are a
// relative, missing or linked root; a folder outside the game, or one below <game>/kiln other than kiln/eml, is taken.
#[test]
fn the_export_root_never_overlaps_game_data() {
    let s = Setup::new("g09-export-root");
    let game = s.f.game.as_std_path().to_owned();
    let root = s.f.root.0.as_std_path().to_owned();
    for dir in ["kiln/eml/inner", "mods/inner", "shroudforge/inner", "kiln/sfi-exports"] {
        fs::create_dir_all(game.join(dir)).unwrap();
    }
    fs::create_dir_all(root.join("outside")).unwrap();
    let refused = |path: &Path| ExportRoot::select(path, &game).is_err();
    for path in [
        game.clone(),
        root.clone(),
        game.join("kiln"),
        game.join("kiln/eml"),
        game.join("kiln/eml/inner"),
        game.join("mods"),
        game.join("mods/inner"),
        game.join("shroudforge"),
        game.join("shroudforge/inner"),
        root.join("missing"),
        PathBuf::from("relative/exports"),
    ] {
        assert!(refused(&path), "{} was taken", path.display());
    }
    #[cfg(unix)]
    {
        std::os::unix::fs::symlink(&game, root.join("alias")).unwrap();
        std::os::unix::fs::symlink(root.join("outside"), root.join("linked")).unwrap();
        assert!(refused(&root.join("alias")), "a link to the game folder");
        assert!(
            refused(&root.join("alias/kiln")),
            "Kiln's folder through a link of the game folder"
        );
        assert!(refused(&root.join("linked")), "a linked root");
        // a plain folder reached through a link resolves to where it is: that path is what the pass uses
        let through = ExportRoot::select(&root.join("alias/kiln/sfi-exports"), &game).unwrap();
        assert_eq!(through.path(), fs::canonicalize(game.join("kiln/sfi-exports")).unwrap());
    }
    let taken = ExportRoot::select(&root.join("outside"), &game).unwrap();
    assert_eq!(taken.path(), fs::canonicalize(root.join("outside")).unwrap());
    assert!(ExportRoot::select(&game.join("kiln/sfi-exports"), &game).is_ok());
}

// The protected folders are compared where they resolve, as the root is: a package folder or Kiln folder that is
// itself a link (a mods folder kept on another drive) protects its target, and on a case-insensitive filesystem a
// protected folder spelled in another case is the same folder.
#[test]
fn the_export_root_sees_protected_folders_where_they_resolve() {
    let s = Setup::new("g09-export-links");
    let game = s.f.game.as_std_path().to_owned();
    #[cfg(unix)]
    let root = s.f.root.0.as_std_path().to_owned();
    #[cfg(unix)]
    {
        // <game>/mods and <game>/kiln are links to folders beside the game (the fixture's kiln folder moved there)
        fs::rename(game.join("kiln"), root.join("real-kiln")).unwrap();
        for dir in ["real-mods/inner", "real-kiln/eml/inner", "real-kiln/sfi-exports"] {
            fs::create_dir_all(root.join(dir)).unwrap();
        }
        std::os::unix::fs::symlink(root.join("real-mods"), game.join("mods")).unwrap();
        std::os::unix::fs::symlink(root.join("real-kiln"), game.join("kiln")).unwrap();
        for path in [
            root.join("real-mods"),
            root.join("real-mods/inner"),
            root.join("real-kiln"),
            root.join("real-kiln/eml"),
            root.join("real-kiln/eml/inner"),
        ] {
            assert!(
                ExportRoot::select(&path, &game).is_err(),
                "{} was taken: a linked protected folder",
                path.display()
            );
        }
        assert!(ExportRoot::select(&root.join("real-kiln/sfi-exports"), &game).is_ok());
    }
    #[cfg(windows)]
    {
        // NTFS: "Mods" is "mods", "KILN\Eml" is "kiln\eml". Under Wine too, although its canonical path keeps the
        // spelling it was asked for (\\?\...\game\mods for a folder named Mods), so the comparison itself ignores case.
        for dir in ["Mods/inner", "KILN/Eml/inner", "ShroudForge/inner", "KILN/sfi-exports"] {
            fs::create_dir_all(game.join(dir)).unwrap();
        }
        for path in [
            game.join("Mods"),
            game.join("Mods/inner"),
            game.join("KILN"),
            game.join("KILN/Eml"),
            game.join("KILN/Eml/inner"),
            game.join("ShroudForge/inner"),
        ] {
            assert!(
                ExportRoot::select(&path, &game).is_err(),
                "{} was taken: a protected folder in another case",
                path.display()
            );
        }
        assert!(ExportRoot::select(&game.join("KILN/sfi-exports"), &game).is_ok());
    }
}

// Windows: a protected folder is that folder by any of its names. Native Windows resolves a path to the folder's own
// spelling, but Wine (Proton) keeps the spelling it was asked for: another case, an 8.3 short name or another drive
// letter for the same folder; and on both, a share of a local folder resolves to \\?\UNC\... . Every refused path
// exists, so it is refused for what it is, not for being missing; a folder outside is still taken by another path.
#[cfg(windows)]
#[test]
fn the_export_root_knows_a_protected_folder_by_any_of_its_names() {
    let s = Setup::new("g09-export-names");
    let root = s.f.root.0.as_std_path().to_owned();
    // a long game folder name (Steam's "Enshrouded" has an 8.3 name too) with a Greek letter with a subscript iota
    let game = root.join("Enshrouded \u{1FB3} game");
    for dir in ["mods/inner", "shroudforge/inner", "kiln/eml/inner", "kiln/sfi-exports"] {
        fs::create_dir_all(game.join(dir)).unwrap();
    }
    fs::create_dir_all(root.join("outside-folder")).unwrap();
    let refused = |path: &Path, why: &str| {
        assert!(fs::metadata(path).is_ok(), "{} is there ({why})", path.display());
        assert!(ExportRoot::select(path, &game).is_err(), "{} was taken: {why}", path.display());
    };
    let taken = |path: &Path, why: &str| {
        assert!(ExportRoot::select(path, &game).is_ok(), "{} was refused: {why}", path.display());
    };
    // ᾳ U+1FB3 and ᾼ U+1FBC are one letter to NTFS and Wine; their full upper case is two letters
    refused(&root.join("Enshrouded \u{1FBC} game/mods/inner"), "another spelling of the Greek letter");
    refused(&root.join("ENSHROUDED \u{1FBC} GAME/SHROUDFORGE"), "another case");
    for (long, inner) in [(game.clone(), "mods/inner"), (game.join("shroudforge"), "inner")] {
        match windows_names::short(&long) {
            Some(short) if short != long => refused(&short.join(inner), "an 8.3 name"),
            _ => eprintln!("no 8.3 name for {} on this volume", long.display()),
        }
    }
    // Windows' own other spellings, where they open at all (Wine has no index stream)
    for (path, why) in [
        (game.join("mods./inner"), "a trailing dot"),
        (game.join("mods::$INDEX_ALLOCATION/inner"), "the folder's index stream"),
    ] {
        match fs::metadata(&path) {
            Ok(_) => refused(&path, why),
            Err(e) => eprintln!("{} does not open here ({e}): {why}", path.display()),
        }
    }
    // another path to the same volume: Z: (the Unix root) under Wine, a local share on native Windows
    match windows_names::other_path(&game) {
        Some(alias) => {
            refused(&alias.join("mods/inner"), "another drive letter or a share");
            refused(&alias.join("kiln/eml"), "another drive letter or a share");
            refused(&alias, "the game folder by another path");
            taken(&windows_names::other_path(&root.join("outside-folder")).unwrap(), "a folder outside, by another path");
            taken(&alias.join("kiln/sfi-exports"), "below kiln, by another path");
        }
        None => eprintln!("no other path to {} here", game.display()),
    }
    taken(&root.join("outside-folder"), "a folder outside");
    taken(&game.join("kiln/sfi-exports"), "below kiln");
}

#[cfg(windows)]
mod windows_names {
    use std::{
        ffi::{CStr, OsString, c_char, c_void},
        fs,
        os::windows::ffi::{OsStrExt as _, OsStringExt as _},
        path::{Path, PathBuf},
    };
    #[link(name = "kernel32", kind = "raw-dylib")]
    unsafe extern "system" {
        fn GetShortPathNameW(long: *const u16, short: *mut u16, len: u32) -> u32;
        fn GetModuleHandleW(name: *const u16) -> *mut c_void;
        fn GetProcAddress(module: *mut c_void, name: *const c_char) -> *mut c_void;
    }
    fn wide(s: &std::ffi::OsStr) -> Vec<u16> {
        s.encode_wide().chain([0]).collect()
    }
    /// The 8.3 name of `path`, if the volume made one.
    pub fn short(path: &Path) -> Option<PathBuf> {
        let long = wide(path.as_os_str());
        let mut buf = vec![0u16; 4096];
        let len = unsafe { GetShortPathNameW(long.as_ptr(), buf.as_mut_ptr(), buf.len() as u32) } as usize;
        (len > 0 && len < buf.len()).then(|| PathBuf::from(OsString::from_wide(&buf[..len])))
    }
    /// `path` by another way to its volume: under Wine Z:\<its Unix path>, on Windows \\localhost\<drive>$\... (when
    /// that share answers).
    pub fn other_path(path: &Path) -> Option<PathBuf> {
        let full = fs::canonicalize(path).ok()?;
        let full = full.to_str()?.strip_prefix(r"\\?\")?.to_owned();
        let kernel32 = unsafe { GetModuleHandleW(wide("kernel32".as_ref()).as_ptr()) };
        let unix_name = unsafe { GetProcAddress(kernel32, c"wine_get_unix_file_name".as_ptr()) };
        let other = if unix_name.is_null() {
            let (drive, rest) = full.split_once(r":\")?;
            PathBuf::from(format!(r"\\localhost\{drive}$\{rest}"))
        } else {
            // SAFETY: Wine's kernel32 export: char *CDECL wine_get_unix_file_name(LPCWSTR)
            let unix_name: unsafe extern "C" fn(*const u16) -> *const c_char = unsafe { std::mem::transmute(unix_name) };
            let unix = unsafe { unix_name(wide(full.as_ref()).as_ptr()) };
            if unix.is_null() {
                return None;
            }
            let unix = unsafe { CStr::from_ptr(unix) }.to_str().ok()?.replace('/', r"\");
            PathBuf::from(format!("Z:{unix}"))
        };
        (other.as_os_str() != full.as_str() && fs::metadata(&other).is_ok()).then_some(other)
    }
}

// The seam for kiln mode: the retry driver around run_attempt, a NEW StartupHost generation per attempt under the one held
// install lock. The failing package (and the package that needs it) drop out; the retry starts from the clean baseline,
// so the failed attempt's staged content never reaches the published archive; the others' edits do.
#[test]
fn retry_composes_with_the_attempt_on_a_new_generation() {
    let s = Setup::new("g09-retry");
    s.package(
        "bad",
        &["patch"],
        Some(r##"game.assets.create_content(buffer.wrap("bad's content")); error("bad failed")"##),
    );
    s.package("base", &["patch"], Some(EDITS));
    s.package_with(
        "needs-bad",
        "needs-bad",
        serde_json::json!({"capabilities": ["patch"], "dependencies": [{"id": "bad", "version": "^1"}]}),
        serde_json::json!({}),
        Some(r##"game.assets.create_content(buffer.wrap("needs-bad's content"))"##),
        &[],
    );
    s.package(
        "other",
        &["patch"],
        Some(r##"game.assets.create_content(buffer.wrap("other's content"))"##),
    );
    let plan = s.plan(
        Features {
            patch: true,
            export: false,
        },
        None,
        None,
    );
    assert_eq!(plan.order(), ["bad", "base", "needs-bad", "other"]);
    let mut effects = SideEffects::default();
    let mut lines = Vec::new();
    let (result, retry) = run_pass(
        StartupDialect::Shroudforge,
        &plan.pass_plan(),
        |pass| {
            let mut host = s.host();
            let prepared = run_attempt(&mut host, &plan, pass, &s.ports(), &mut effects, |_| Ok(()))?;
            let ran = prepared.ran().to_vec();
            prepared
                .publish()
                .map_err(|e| PassError::Other(e.into()))?
                .ok_or_else(|| PassError::Other(anyhow::anyhow!("nothing published")))?;
            Ok(ran)
        },
        |line| lines.push(line.to_owned()),
    );
    assert_eq!(result.unwrap(), ["base", "other"]);
    let skipped: Vec<&str> = retry.skipped.iter().map(|p| p["id"].as_str().unwrap()).collect();
    assert_eq!(skipped, ["bad", "needs-bad"]);
    assert_eq!(effects.entered, ["bad", "base", "other"]);
    assert_eq!(lines.len(), 2, "{lines:?}");
    let live = s.semantics();
    for (text, present) in [
        (&b"bad's content"[..], false),
        (b"needs-bad's content", false),
        (b"other's content", true),
        (b"g09 content", true),
    ] {
        assert_eq!(
            live.contents.contains_key(&ContentHash::from_data(text).to_string()),
            present
        );
    }
    assert_eq!(
        live.resources[&archive::resource_key(&archive::resource_id(0))]["child"]["extra"],
        99
    );
}


// ---- The attribute catalog and the loaded-resource functions of the pregame VM -----------------------------

/// The synthetic attribute archive (fixtures/m6/startup/attributes/archive.json): its registry and its resources'
/// bytes, written by EML's writer. Three definition roots in an AttributeContainerResource (one also a
/// BaseAttributeResource), one more only as a BaseAttributeResource; two keen::ecs components carry a definition GUID
/// whose FNV-1a is their root, one ECS type is not an attribute component, one has narrow storage.
fn attribute_archive() -> (Rc<kfc::reflection::TypeRegistry>, Vec<(ResourceId, Vec<u8>)>) {
    use kfc::reflection::LookupKey;
    let spec: serde_json::Value =
        serde_json::from_str(include_str!("fixtures/m6/startup/attributes/archive.json")).unwrap();
    let registry: Rc<kfc::reflection::TypeRegistry> =
        Rc::new(serde_json::from_value(spec["registry"].clone()).unwrap());
    let resources = spec["resources"]
        .as_array()
        .unwrap()
        .iter()
        .map(|resource| {
            let ty = registry
                .get_by_name(LookupKey::Qualified(resource["type"].as_str().unwrap()))
                .unwrap();
            let value: Value = serde_json::from_value(resource["value"].clone()).unwrap();
            let bytes = value.to_bytes(&registry, ty).unwrap();
            let guid = Guid::parse(resource["guid"].as_str().unwrap()).unwrap();
            (ResourceId::new(guid, ty.qualified_hash, 0), bytes)
        })
        .collect();
    (registry, resources)
}

/// \`lua\` is the JSON value ShroudForge's \`json_to_lua\` makes of \`json\`: null is nil, an integer an integer, any other
/// number a float, an array a table of exactly 1..n, an object a table of exactly its non-null keys.
fn lua_is_json(lua: &mlua::Value, json: &serde_json::Value, path: &str) -> Result<(), String> {
    use mlua::Value as L;
    use serde_json::Value as J;
    let fail = || Err(format!("{path}: Lua {lua:?} is not JSON {json}"));
    match (lua, json) {
        (L::Nil, J::Null) => Ok(()),
        (L::Boolean(a), J::Bool(b)) if a == b => Ok(()),
        (L::String(a), J::String(b)) if a.to_str().is_ok_and(|a| *a == **b) => Ok(()),
        (L::Integer(a), J::Number(b)) if b.as_i64() == Some(*a) || b.as_u64().is_some_and(|b| b as i64 == *a) => Ok(()),
        (L::Number(a), J::Number(b)) if b.as_i64().is_none() && b.as_u64().is_none() && b.as_f64() == Some(*a) => {
            Ok(())
        }
        (L::Table(t), J::Array(items)) => {
            if t.clone().pairs::<mlua::Value, mlua::Value>().count() != items.len() {
                return fail();
            }
            for (i, item) in items.iter().enumerate() {
                lua_is_json(&t.raw_get(i + 1).unwrap(), item, &format!("{path}[{}]", i + 1))?;
            }
            Ok(())
        }
        (L::Table(t), J::Object(map)) => {
            for pair in t.clone().pairs::<mlua::Value, mlua::Value>() {
                let (key, _) = pair.unwrap();
                let present = match &key {
                    L::String(k) => map.get(&*k.to_str().unwrap()).is_some_and(|v| !v.is_null()),
                    _ => false,
                };
                if !present {
                    return Err(format!("{path}: Lua key {key:?} is not in JSON {json}"));
                }
            }
            for (key, value) in map {
                lua_is_json(&t.raw_get(key.as_str()).unwrap(), value, &format!("{path}.{key}"))?;
            }
            Ok(())
        }
        _ => fail(),
    }
}

/// ShroudForge's own catalog over the same synthetic archive. ShroudForge's oracle cannot reach the attribute
/// catalog (it has no archive reader and panics there), so fixtures/m6/startup/attributes/golden.json was made by
/// compiling ShroudForge 1.7.9's reflection/build_attribute_catalog.rs, evaluate_attribute_program.rs,
/// traverse_reflected_types.rs and attributes/format_attribute_root.rs unchanged over archive.json written with
/// ShroudForge's own kfc-parser (out/v2-g09c/tools/sf-attr-oracle; a few lines of glue copy the Lua bindings' code
/// between the catalog and Lua). Kiln's startup VM, over the same archive written by EML's writer, answers
/// runtime.ecs.get_attributes, get_attribute (every descriptor field, layout and program decode) and
/// evaluate_attributes (values, trace, model, scalar type, and the unverified-program refusal) exactly as that JSON
/// reaches Lua through ShroudForge's json_to_lua.
#[test]
fn attribute_catalog_matches_shroudforge_on_a_synthetic_archive() {
    let s = Setup::attributes("g09c-golden");
    let p = pregame_vm(
        &s,
        "attrs",
        &["runtime"],
        Side::Client,
        Features {
            patch: false,
            export: false,
        },
        (*attribute_archive().0).clone(),
    );
    let golden: serde_json::Value =
        serde_json::from_str(include_str!("fixtures/m6/startup/attributes/golden.json")).unwrap();
    // the inputs of the golden's evaluations, as Lua values (the golden holds their bits)
    let chunk = r##"
local ecs = runtime.ecs
local function wrap(v, e) if v == nil then return {err = e} end return {ok = v} end
local out = {get_attribute = {}, evaluate = {}}
out.get_attributes = ecs.get_attributes()
for _, name in ipairs({"Stamina", "Stamina_Min", "Revive_Delay", "Mana", "Shared", "Missing"}) do
  out.get_attribute[name] = wrap(ecs.get_attribute(name))
end
local cases = {
  {"0:Stamina", "Stamina", {Stamina = 150, Stamina_Min = 0, Stamina_Max = 100}},
  {"1:Stamina_Min", "Stamina_Min", {Stamina = 40, Stamina_Min = 0, Stamina_Max = 50}},
  {"2:Revive_Delay", "Revive_Delay", {Revive_Delay = 0.0, Revive_Base = 1.5}},
  {"3:Mana", "Mana", {Mana = 7, Mana_Max = 5, Shared = 2}},
  {"4:Mana_Max", "Mana_Max", {Mana = 1, Mana_Max = 1, Shared = 0}},
  {"5:Broken", "Broken", {Broken = 1, Shared = 1}},
}
for _, c in ipairs(cases) do out.evaluate[c[1]] = wrap(ecs.evaluate_attributes(c[2], c[3])) end
return out
"##;
    let lua = p.vm.lua();
    let ours: mlua::Value = lua
        .load(chunk)
        .set_name("=attrs")
        .set_environment(p.env.clone())
        .eval()
        .unwrap();
    let mut expected = golden.clone();
    let evaluate = expected["evaluate"].as_object_mut().unwrap();
    for case in evaluate.values_mut() {
        *case = case["result"].clone();
    }
    expected.as_object_mut().unwrap().remove("source");
    // the fixture's roots reach their components (the definition GUIDs hash to the roots)
    assert_eq!(golden["get_attributes"]["count"], 10);
    for name in ["Stamina", "Revive_Delay"] {
        let components = golden["get_attribute"][name]["ok"]["components"].as_array().unwrap();
        assert_eq!(components.len(), 1, "{name}");
    }
    lua_is_json(&ours, &expected, "ours").unwrap();
}

/// Source-derived where the golden's glue does not run the Lua conversions (attributes/mod.rs Catalog::get,
/// encode_attribute_scalar.rs, validate_attribute_bits.rs, evaluate_attributes.rs, convert_json.rs): selectors by ID and
/// their refusals, the input checks and their texts, integers for a float root, non-finite results, and
/// runtime.overrides.attribute reaching the read refusal (the pregame answer) after a descriptor resolves.
#[test]
fn attribute_members_convert_like_shroudforge() {
    let s = Setup::attributes("g09c-convert");
    let (registry, _) = attribute_archive();
    let p = pregame_vm(
        &s,
        "attrs",
        &["runtime"],
        Side::Client,
        Features {
            patch: false,
            export: false,
        },
        (*registry).clone(),
    );
    let spec: serde_json::Value =
        serde_json::from_str(include_str!("fixtures/m6/startup/attributes/archive.json")).unwrap();
    let id = |root: usize, index: usize| {
        spec["resources"][0]["value"]["attributes"][root]["ids"][index]["value"]
            .as_u64()
            .unwrap()
    };
    let chunk = format!(
        "{PRELUDE}\nlocal STAMINA, SHARED_D = {}, {}\n{}",
        id(0, 0),
        id(2, 1),
        r##"
local ecs, ov = runtime.ecs, runtime.overrides
local phase = "runtime APIs are available only during the ingame phase"
local d, r = ecs.get_attribute(STAMINA); eq(r, nil); eq(d.name, "Stamina"); eq(d.index, 0)
eq(d.root_hash, STAMINA); eq(d.storage_reason, nil); eq(d.components[1].qualified_name, "keen::ecs::Stamina")
d, r = ecs.get_attribute(-1); eq(d, nil); eq(r, "attribute ID outside u32")
d, r = ecs.get_attribute(2^32); eq(r, "expected original attribute name or unsigned attribute ID")
d, r = ecs.get_attribute(4294967296 // 1); eq(r, "attribute ID outside u32")
d, r = ecs.get_attribute(12345); eq(r, "attribute ID not found")
d, r = ecs.get_attribute(1.0); eq(r, "expected original attribute name or unsigned attribute ID")
d, r = ecs.get_attribute(); eq(r, "expected original attribute name or unsigned attribute ID")
d, r = ecs.get_attribute(SHARED_D); eq(r, nil); eq(d.name, "Shared")
local all = ecs.get_attributes(1, 2); eq(all.count, 10); eq(#all.entries, 10); eq(#all.component_layouts, 2)
eq(all.storage_resolution, "per-entity-initialized-root-id"); eq(all.version, "g09c-attributes")
assert(not rawequal(all, ecs.get_attributes()), "detached tables")

local function ev(sel, vals) return ecs.evaluate_attributes(sel, vals) end
local v
v, r = ev("Stamina", {Stamina = 150, Stamina_Min = 0}); eq(v, nil)
eq(r, "Stamina_Max: expected finite scalar of type keen::sint32")
v, r = ev("Stamina", {Stamina = 1, Stamina_Min = 0, Stamina_Max = 2, extra = 1})
eq(r, "unexpected attribute names in root values")
v, r = ev("Stamina", {Stamina = math.tointeger(2^31), Stamina_Min = 0, Stamina_Max = 2}); eq(r, "Stamina: value outside sint32")
v, r = ev("Stamina", {Stamina = 1.5, Stamina_Min = 0, Stamina_Max = 2})
eq(r, "Stamina: expected finite scalar of type keen::sint32")
v, r = ev("Stamina", {Stamina = "1", Stamina_Min = 0, Stamina_Max = 2})
eq(r, "Stamina: expected finite scalar of type keen::sint32")
v, r = ev(STAMINA, {Stamina = -5, Stamina_Min = -10, Stamina_Max = 10}); eq(r, nil)
-- ScaleToNewMax rescales -5 in [-10, 10] to [-10, 20]: -10 + trunc(0.25 * 30) = -3, then the clamp keeps it
eq(v.values.Stamina, -3); eq(v.values.Stamina_Max, 20); eq(math.type(v.values.Stamina), "integer")
eq(v.scalar_type, "keen::sint32"); eq(v.calculation_model, "attribute-command-v1"); eq(#v.trace, 2)
v, r = ev("Revive_Base", {Revive_Delay = 0, Revive_Base = 2}); eq(r, nil)
eq(v.values.Revive_Delay, 4.0); eq(math.type(v.values.Revive_Delay), "float"); eq(math.type(v.values.Revive_Base), "float")
v, r = ev("Revive_Base", {Revive_Delay = 0, Revive_Base = 0/0}); eq(r, "Revive_Base: expected finite scalar of type keen::float32")
v, r = ev("Revive_Base", {Revive_Delay = 0, Revive_Base = 1e39}); eq(r, "Revive_Base: expected finite scalar of type keen::float32")
v, r = ev("Revive_Base", {Revive_Delay = 0, Revive_Base = 3e38}); eq(r, "non-finite attribute calculation result")
v, r = ev("Mana", {Mana = -1, Mana_Max = 0, Shared = 0}); eq(r, "Mana: value outside uint32")
v, r = ev("Mana", {Mana = 1, Mana_Max = 1, Shared = 0}); eq(v.values.Shared, 4294967294)
v, r = ev("Shared", {}); eq(r, "Ambiguous attribute name. Use its ID")
v, r = ev(SHARED_D, {Broken = 1, Shared = 1}); eq(r, "attribute root has an unverified calculation program")
v, r = ev(true, {}); eq(r, "expected original attribute name or unsigned attribute ID")
raises("error converting Lua nil to table", ecs.evaluate_attributes, "Stamina")
raises("error converting Lua string to table", ecs.evaluate_attributes, "Stamina", "x")

-- the override target resolves the descriptor, then reads the entity's attributes: the pregame refusal
v, r = ov.attribute(1, "Stamina"); eq(v, nil); eq(r, phase)
v, r = ov.attribute(1, "Mana"); eq(v, nil); eq(r, phase)
v, r = ov.attribute(1, "Missing"); eq(v, nil); eq(r, "attribute not found: Missing")
v, r = ov.attribute(1, "Shared"); eq(r, "Ambiguous attribute name. Use its ID")
"##
    );
    p.vm.lua()
        .load(chunk)
        .set_name("=attrs")
        .set_environment(p.env.clone())
        .exec()
        .unwrap();
}

/// ShroudForge's game.assets carries the loaded-resource functions in both phases (bindings/game/assets/mod.rs:31);
/// in the pregame phase each answers in its own shape (lua/ingame/resources/*.rs): the two lookups raise the ingame-only
/// text before any argument, resolve/prepare/by_field answer the read denial first, write_loaded_field the assets write
/// denial first (no write is reached), and read_loaded_field converts exactly a Resource and a path, then refuses an
/// archive Resource as not a runtime snapshot.
#[test]
fn loaded_resource_functions_answer_pregame() {
    let s = Setup::new("g09c-loaded");
    let p = pregame_vm(
        &s,
        "loaded",
        &["patch", "runtime"],
        Side::Server,
        Features {
            patch: true,
            export: false,
        },
        (*s.f.types).clone(),
    );
    let chunk = format!(
        "{PRELUDE}\n{}",
        r##"
local a = game.assets
local phase = "runtime APIs are available only during the ingame phase"
local ingame = "loaded ResourceService access is available only in the ingame phase"
local res = assert(a.get_resource("01234567-89ab-cdef-0123-456789abcdef", "m6.Root", 0))
for _, name in ipairs({"get_loaded_resource", "get_loaded_resource_by_field", "prepare_loaded_index",
    "resolve_loaded_resource", "read_loaded_field", "write_loaded_field", "get_loaded_resources_by_type"}) do
  eq(type(rawget(a, name)), "function", name)
end
raises(ingame, a.get_loaded_resource)
raises(ingame, a.get_loaded_resource, "01234567-89ab-cdef-0123-456789abcdef", "m6.Root", 0)
raises(ingame, a.get_loaded_resource, {}, {}, "x")
raises(ingame, a.get_loaded_resources_by_type, "m6.Root")
raises(ingame, a.get_loaded_resources_by_type)
local function n(...) return select("#", ...), ... end
local c, x, r, t = n(a.get_loaded_resource_by_field("m6.Root", "child.extra", 1)); eq(c, 3); eq(x, nil); eq(r, phase); eq(t, nil)
c, x, r, t = n(a.get_loaded_resource_by_field()); eq(c, 3); eq(r, phase)
c, x, r, t = n(a.prepare_loaded_index("m6.Root", "child.extra")); eq(c, 3); eq(x, false); eq(r, phase); eq(t, nil)
c, x, r, t = n(a.prepare_loaded_index(1, 2, 3)); eq(x, false); eq(r, phase)
c, x, r = n(a.resolve_loaded_resource("g38_items::ItemRegistry", 7)); eq(c, 2); eq(x, nil); eq(r, phase)
c, x, r = n(a.resolve_loaded_resource()); eq(r, phase)
c, x, r = n(a.write_loaded_field(res, "child.extra", 5)); eq(c, 2); eq(x, nil); eq(r, phase)
c, x, r = n(a.write_loaded_field(res, "child.extra", 5, 11)); eq(r, phase)
c, x, r = n(a.write_loaded_field()); eq(r, phase)
local expects = "read_loaded_field expects a loaded Resource and reflected field path"
c, x, r, t = n(a.read_loaded_field()); eq(c, 3); eq(x, nil); eq(r, expects); eq(t, nil)
c, x, r, t = n(a.read_loaded_field(res)); eq(r, expects)
c, x, r, t = n(a.read_loaded_field(res, "child.extra", 1)); eq(r, expects)
c, x, r, t = n(a.read_loaded_field(res, "child.extra")); eq(c, 3); eq(x, nil); eq(r, "resource is not a runtime snapshot"); eq(t, nil)
c, x, r, t = n(a.read_loaded_field(res, 5)); eq(r, "resource is not a runtime snapshot")
c, x, r, t = n(a.read_loaded_field({}, "child.extra")); eq(r, "bad argument #1 (Resource expected, got table)")
c, x, r, t = n(a.read_loaded_field(res, {})); assert(type(r) == "string" and r ~= expects and r ~= "resource is not a runtime snapshot", r)
-- no function here makes a loaded runtime Resource before the game, so the override claim's assumption (every
-- Resource of the pass is an archive or created record) still holds
c, x, r = n(runtime.overrides.get_resource_claim(res)); eq(x, nil); eq(r, "write_loaded_field requires a ResourceService snapshot")
"##
    );
    p.vm.lua()
        .load(chunk)
        .set_name("=loaded")
        .set_environment(p.env.clone())
        .exec()
        .unwrap();
}

/// A patch package that reads the attribute catalog, evaluates a root, takes the attribute override target and
/// calls the loaded-resource functions at top level runs in the pass (ShroudForge runs it), over the synthetic attribute
/// archive through a real StartupHost generation, and its content edit is published.
#[test]
fn a_package_using_attributes_and_loaded_resources_at_top_level_runs_in_the_pass() {
    let s = Setup::attributes("g09c-pass");
    s.package(
        "attrpkg",
        &["patch", "runtime"],
        Some(
            r##"
local phase = "runtime APIs are available only during the ingame phase"
local all = assert(runtime.ecs.get_attributes()); eq(all.count, 10, "catalog")
local stamina = assert(runtime.ecs.get_attribute("Stamina")); eq(stamina.calculation_writable, true)
local v = assert(runtime.ecs.evaluate_attributes("Stamina", {Stamina = 150, Stamina_Min = 0, Stamina_Max = 100}))
eq(v.values.Stamina, 110, "evaluate"); eq(v.values.Stamina_Max, 110)
local _, r = runtime.overrides.attribute(1, "Stamina"); eq(r, phase, "override target")
_, r = game.assets.prepare_loaded_index("keen::ecs::Stamina", "dataStorage"); eq(r, phase, "loaded index")
local ok = pcall(game.assets.get_loaded_resource, "f552f8bc-ada0-4a58-8739-4081ce0d3873", "keen::BaseAttributeResource")
eq(ok, false, "loaded lookup raises")
local base = game.assets.get_resource("f552f8bc-ada0-4a58-8739-4081ce0d3873", "keen::BaseAttributeResource", 0)
_, r = game.assets.read_loaded_field(base, "type.value"); eq(r, "resource is not a runtime snapshot")
eq(base.data.debugNames[1], "Stamina", "the archive resource reads as data")
game.assets.create_content(buffer.wrap("g09c " .. v.values.Stamina_Max))
shroudforge.log.info("attributes read")
"##,
        ),
    );
    let plan = s.plan(
        Features {
            patch: true,
            export: false,
        },
        None,
        None,
    );
    let (result, effects) = s.attempt(&plan, |_| Ok(()));
    let prepared = result.unwrap_or_else(|e| panic!("the pass skipped the package: {e}"));
    assert_eq!(prepared.ran(), ["attrpkg"]);
    assert_eq!(effects.entered, ["attrpkg"]);
    assert_eq!(s.log.count("attrpkg: attributes read"), 1);
    prepared.publish().unwrap().expect("a patching pass publishes");
    let live = s.semantics();
    assert_eq!(
        live.contents[&ContentHash::from_data(b"g09c 110").to_string()],
        b"g09c 110"
    );
    assert!(s.stages_reclaimed());
}

// ---- Key order and the catalog's lifetime -----------------------------------------------------------

/// ShroudForge's `json_to_lua` (lua/convert_json.rs, MIT, Copyright (c) 2026 bonsaibauer; notice in
/// kiln-eml/src/sf_startup/mod.rs), unchanged: the reference the key-order check builds its tables with.
fn sf_json_to_lua(lua: &mlua::Lua, value: &serde_json::Value) -> mlua::Result<mlua::Value> {
    use mlua::IntoLua;
    match value {
        serde_json::Value::Null => Ok(mlua::Value::Nil),
        serde_json::Value::Bool(value) => value.into_lua(lua),
        serde_json::Value::String(value) => value.as_str().into_lua(lua),
        serde_json::Value::Number(value) => {
            if let Some(value) = value.as_i64() {
                value.into_lua(lua)
            } else if let Some(value) = value.as_u64() {
                Ok(mlua::Value::Integer(value as i64))
            } else {
                value.as_f64().into_lua(lua)
            }
        }
        serde_json::Value::Array(values) => {
            let table = lua.create_table_with_capacity(values.len(), 0)?;
            for (i, value) in values.iter().enumerate() {
                table.raw_set(i + 1, sf_json_to_lua(lua, value)?)?;
            }
            Ok(mlua::Value::Table(table))
        }
        serde_json::Value::Object(values) => {
            let table = lua.create_table_with_capacity(0, values.len())?;
            for (key, value) in values {
                table.raw_set(key.as_str(), sf_json_to_lua(lua, value)?)?;
            }
            Ok(mlua::Value::Table(table))
        }
    }
}

/// `ours` and `reference` walk their keys in the same order under pairs(), nested tables too.
fn same_walk(ours: &mlua::Table, reference: &mlua::Table, path: &str) {
    let keys = |t: &mlua::Table| -> Vec<(String, mlua::Value)> {
        t.clone()
            .pairs::<mlua::Value, mlua::Value>()
            .map(|pair| {
                let (key, value) = pair.unwrap();
                (format!("{key:?}"), value)
            })
            .collect()
    };
    let (a, b) = (keys(ours), keys(reference));
    let names = |k: &[(String, mlua::Value)]| k.iter().map(|(n, _)| n.clone()).collect::<Vec<_>>();
    assert_eq!(names(&a), names(&b), "pairs() order at {path}");
    for ((key, x), (_, y)) in a.iter().zip(&b) {
        if let (mlua::Value::Table(x), mlua::Value::Table(y)) = (x, y) {
            same_walk(x, y, &format!("{path}.{key}"));
        }
    }
}

/// The order pairs() walks a table of the attribute members in. Lua 5.4 seeds its string hashes per state
/// (lstate.c `luai_makeseed`), so no such order is stable from one VM to the next, in ShroudForge either; within one
/// state it follows the order `json_to_lua` inserts the keys in, which is the JSON map's order. This crate graph
/// builds serde_json with `preserve_order`, as ShroudForge's workspace does. Pinned twice: seed-independently, the
/// catalog's JSON (every descriptor and layout, key order included) is ShroudForge's own (the golden, written by
/// ShroudForge's code), and in one VM the tables runtime.ecs.get_attribute and get_attributes answer walk in the same
/// order as the tables ShroudForge's `json_to_lua` builds from that golden, nested tables included.
#[test]
fn attribute_tables_keep_shroudforges_key_order() {
    let s = Setup::attributes("g09c-order");
    let (registry, _) = attribute_archive();
    let p = pregame_vm(
        &s,
        "order",
        &["runtime"],
        Side::Client,
        Features {
            patch: false,
            export: false,
        },
        (*registry).clone(),
    );
    let golden: serde_json::Value =
        serde_json::from_str(include_str!("fixtures/m6/startup/attributes/golden.json")).unwrap();
    // preserve_order reaches this graph: the parsed golden keeps ShroudForge's order, which is not sorted
    let stamina = &golden["get_attribute"]["Stamina"]["ok"];
    let keys: Vec<&str> = stamina.as_object().unwrap().keys().map(String::as_str).collect();
    assert_eq!(keys[..4], ["name", "hash", "hash_domain", "name_source"]);
    let catalog = p.assets.attribute_catalog().unwrap();
    let text = |value: &serde_json::Value| serde_json::to_string(value).unwrap();
    let entries: Vec<_> = catalog.entries.values().cloned().collect();
    assert_eq!(
        text(&serde_json::Value::Array(entries)),
        text(&golden["get_attributes"]["entries"])
    );
    assert_eq!(
        text(&serde_json::Value::Array(catalog.layouts.clone())),
        text(&golden["get_attributes"]["component_layouts"])
    );
    let lua = p.vm.lua();
    let ours: mlua::Table = lua
        .load("return runtime.ecs.get_attribute('Stamina'), runtime.ecs.get_attributes()")
        .set_environment(p.env.clone())
        .eval::<(mlua::Table, mlua::Table)>()
        .map(|(one, _)| one)
        .unwrap();
    let mlua::Value::Table(reference) = sf_json_to_lua(lua, stamina).unwrap() else {
        panic!("a descriptor is an object")
    };
    same_walk(&ours, &reference, "Stamina");
    let all: mlua::Table = lua
        .load("return runtime.ecs.get_attributes()")
        .set_environment(p.env.clone())
        .eval()
        .unwrap();
    let mlua::Value::Table(reference) = sf_json_to_lua(lua, &golden["get_attributes"]).unwrap() else {
        panic!("the catalog is an object")
    };
    same_walk(&all, &reference, "get_attributes");
}

/// The catalog is built once per asset session and kept, as ShroudForge's `AppState::attribute_catalog`
/// OnceCell keeps it: a catalog built before the reader closes (prepare closes it before publication; only a finalizer
/// or a coroutine could still call) answers afterwards from the same build, and a failure is kept too. A catalog
/// first asked for after the reader closed answers ShroudForge's closed-reader text (its `reader()` panics with that
/// `expect` text inside the OnceCell's initializer, which leaves the cell empty).
#[test]
fn the_attribute_catalog_is_built_once_and_kept_after_the_reader_closes() {
    let features = Features {
        patch: false,
        export: false,
    };
    let run = |p: &PregameVm, chunk: &str| {
        p.vm.lua()
            .load(format!("{PRELUDE}\n{chunk}"))
            .set_name("=kept")
            .set_environment(p.env.clone())
            .exec()
            .unwrap()
    };
    let s = Setup::attributes("g09c-kept");
    let (registry, _) = attribute_archive();
    let p = pregame_vm(&s, "kept", &["runtime"], Side::Client, features, (*registry).clone());
    run(
        &p,
        r#"local all = assert(runtime.ecs.get_attributes()); eq(all.count, 10)"#,
    );
    let first = p.assets.attribute_catalog().unwrap();
    p.assets.close_reader();
    assert!(Rc::ptr_eq(&first, &p.assets.attribute_catalog().unwrap()), "built once");
    run(
        &p,
        r#"local all = assert(runtime.ecs.get_attributes()); eq(all.count, 10)
local v = assert(runtime.ecs.evaluate_attributes("Stamina", {Stamina = 150, Stamina_Min = 0, Stamina_Max = 100}))
eq(v.values.Stamina, 110); eq(assert(runtime.ecs.get_attribute("Mana")).name, "Mana")"#,
    );

    let s = Setup::attributes("g09c-closed");
    let p = pregame_vm(&s, "closed", &["runtime"], Side::Client, features, (*registry).clone());
    p.assets.close_reader();
    let closed = r#"local closed = "asset reader is closed after publication"
local n, r = runtime.ecs.get_attributes(); eq(n, nil); eq(r, closed)
n, r = runtime.ecs.get_attribute("Stamina"); eq(n, nil); eq(r, closed)
n, r = runtime.ecs.evaluate_attributes("Stamina", {}); eq(n, nil); eq(r, closed)"#;
    run(&p, closed);
    run(&p, closed);

    let s = Setup::new("g09c-failure-kept");
    let p = pregame_vm(&s, "failed", &["runtime"], Side::Client, features, (*s.f.types).clone());
    let none = r#"local n, r = runtime.ecs.get_attributes(); eq(n, nil); eq(r, "no attribute definitions in this KFC container")"#;
    run(&p, none);
    p.assets.close_reader();
    run(&p, none);
}
