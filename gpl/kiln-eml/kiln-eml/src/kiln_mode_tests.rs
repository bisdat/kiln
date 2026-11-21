// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln-eml in kiln mode (kiln_mode.rs) on fixture installs: the frozen plan read by the interpreter's
//! own reader (sfi-policy), the decisions for every discovered package, and the runner's kiln route, which goes to the
//! startup barrier (startup_seam.rs). A unit-test process never runs kiln-eml's early phase, so its barrier has no
//! start: these tests see what kiln mode decides and that nothing runs on its own (tests/m6_plugin_barrier.rs runs whole
//! kiln starts).

use super::*;
use camino::{Utf8Path, Utf8PathBuf};
use crate::discover::{self, Candidate};
use crate::runner::{self, Outcome, Phase, RunConfig};
use std::sync::Arc;

/// What the legacy rule reads from kiln-shroudforge's state file: it must never show up in kiln mode.
const SF_STATE_REASON: &str = "SF-STATE-REASON-sentinel";
/// The text of a package the EML run tried (and failed to prepare) in a fixture install.
const RAN: &str = "could not prepare the game data";
/// The plan id of every plan here, and the executable digest of the context they are read with.
pub(crate) const PLAN: &str = "0f1e2d3c-4b5a-4968-8776-a5b4c3d2e1f0";
pub(crate) const EXE: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const PLAN_NAME: &str = "launch-plans/0f1e2d3c-4b5a-4968-8776-a5b4c3d2e1f0.json";

/// The context the core gives kiln-eml (build_info): the plans here are made for it.
pub(crate) fn context() -> ValidationContext {
    ValidationContext { exe_sha256: EXE.into(), side: "client".into(), upstream_180_final: false }
}

/// One package of a strict launch plan (sfi-policy's reader takes it).
#[derive(Clone, Copy)]
pub(crate) struct P<'a> {
    pub id: &'a str,
    pub root: &'a str,
    /// `shroudforge` or `eml`
    pub kind: &'a str,
    pub caps: &'a [&'a str],
    /// `candidate`, `skip`, `disabled` or `other-side`
    pub decision: &'a str,
    pub reasons: &'a [&'a str],
}
pub(crate) fn p<'a>(id: &'a str, root: &'a str, kind: &'a str, caps: &'a [&'a str], decision: &'a str) -> P<'a> {
    P { id, root, kind, caps, decision, reasons: &[] }
}

/// A plan as the launcher's producer writes it, for these packages; its startup dialect is the one sfi-policy's
/// reader requires of them (a ShroudForge patch package, or an EML package that needs the runtime: ShroudForge's; another
/// EML package: EML's; else none).
pub(crate) fn strict_plan(packages: &[P<'_>]) -> String {
    let ff = "f".repeat(64);
    let runtime = |caps: &[&str]| caps.iter().any(|c| *c == "runtime" || *c == "runtime-register-dll");
    let (mut needs_sf, mut has_eml) = (false, false);
    for p in packages {
        if p.decision != "disabled" && p.decision != "other-side" {
            needs_sf |= (p.kind == "shroudforge" && p.caps.contains(&"patch")) || (p.kind == "eml" && runtime(p.caps));
            has_eml |= p.kind == "eml";
        }
    }
    let dialect = if needs_sf { "shroudforge" } else if has_eml { "eml" } else { "none" };
    let list: Vec<serde_json::Value> = packages.iter().map(|p| {
        let candidate = p.decision == "candidate";
        let data = p.caps.contains(&"patch") || (p.caps.contains(&"export") && !runtime(p.caps));
        let lua = runtime(p.caps);
        let part = |required: bool| serde_json::json!({"required": required,
            "admission": if !required { "not-needed" } else if candidate { "admit" } else { "refuse" }});
        serde_json::json!({
            "id": p.id, "version": "1.0.0", "kind": p.kind, "root": p.root,
            "content_sha256": ff, "config_sha256": ff,
            "enabled": p.decision != "disabled", "targets": [if p.decision == "other-side" { "server" } else { "client" }],
            "capabilities": p.caps, "dependencies": [], "requiredApis": [], "services": [], "requires180": false,
            "saveSensitive": false, "decision": p.decision,
            "reasons": p.reasons.iter().map(|m| serde_json::json!({"code": "x", "message": m, "causedBy": [], "severity": "warning"})).collect::<Vec<_>>(),
            "data": part(data), "lua": part(lua), "dlls": [],
        })
    }).collect();
    serde_json::json!({
        "format": 1, "minReader": 1, "plan_id": PLAN, "launch_id": null, "startupDialect": dialect,
        "writer": {"component": "kiln-manage", "version": "test"},
        "profile": {"id": "p", "generation": 1, "mode": "active", "semantic_sha256": ff},
        "game": {"side": "client", "exe_sha256": EXE, "archiveBase_sha256": ff, "archiveInput_sha256": ff},
        "backend": {"requested": "kiln", "selected": "kiln", "fallbackOrder": [], "dataWriter": "kiln-eml", "selectionReason": ""},
        "contract": {"apiVersion": "1.7.9", "sourceCommit": "d8a394bcb180ceb5bdfce48048dcc4f2e0108bd8", "upstream180Final": false,
            "inventory_sha256": ff, "parity_sha256": ff, "serviceContracts_sha256": ff},
        "fingerprints": {"packages_sha256": ff, "settings_sha256": ff, "chain_sha256": ff, "loaderInventory_sha256": ff,
            "compat_sha256": ff, "releasePolicy_sha256": ff, "placementReceipts_sha256": ff},
        "packages": list,
        "loadOrder": packages.iter().filter(|p| p.decision == "candidate").map(|p| p.id).collect::<Vec<_>>(),
        "warnings": [],
    }).to_string()
}

struct Install {
    game: Utf8PathBuf,
}

impl Install {
    fn new(tag: &str) -> Self {
        let game = Utf8PathBuf::from_path_buf(std::env::temp_dir()).unwrap().join(format!("kiln-eml-v19b-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&game);
        std::fs::create_dir_all(game.join("kiln/state")).unwrap();
        std::fs::create_dir_all(game.join("mods")).unwrap();
        std::fs::write(game.join("enshrouded.kfc"), b"KFC fixture").unwrap();
        // kiln-shroudforge's state file of this process, as it writes it when it stands aside (mode off)
        std::fs::write(game.join("kiln/state/shroudforge.json"), serde_json::json!({
            "pid": std::process::id(), "mode": "off", "reason": SF_STATE_REASON }).to_string()).unwrap();
        Self { game }
    }

    fn kiln(&self) -> Utf8PathBuf {
        self.game.join("kiln")
    }

    /// An EML package `<game>/<dir>` with these capabilities.
    fn eml(&self, dir: &str, id: &str, caps: &[&str]) -> Utf8PathBuf {
        let p = self.game.join(dir);
        std::fs::create_dir_all(p.join("src")).unwrap();
        std::fs::write(p.join("mod.json"), serde_json::json!({"id": id, "name": id, "version": "1.0.0", "capabilities": caps}).to_string()).unwrap();
        std::fs::write(p.join("src/mod.lua"), "-- fixture").unwrap();
        p
    }

    /// A ShroudForge package (its extended.mod.json names ShroudForge): discovery sets it aside.
    fn sf(&self, dir: &str, id: &str, caps: &[&str]) -> Utf8PathBuf {
        let p = self.eml(dir, id, caps);
        std::fs::write(p.join("extended.mod.json"), r#"{"schemaVersion":1,"launcher":"SF","enabled":true}"#).unwrap();
        p
    }

    /// Writes the plan to `<kiln>/launch-plans/<id>.json`; returns its SHA-256 and the profile that names it.
    fn plan(&self, text: &str) -> ([u8; 32], String) {
        std::fs::create_dir_all(self.kiln().join("launch-plans")).unwrap();
        std::fs::write(self.kiln().join(format!("launch-plans/{PLAN}.json")), text).unwrap();
        let hash: [u8; 32] = Sha256::digest(text.as_bytes()).as_slice().try_into().unwrap();
        let hex: String = hash.iter().map(|b| format!("{b:02x}")).collect();
        (hash, serde_json::json!({"mods": {}, "sfLaunchPlan": {"path": format!("launch-plans/{PLAN}.json"), "sha256": hex}}).to_string())
    }

    /// Kiln mode with this plan, read as plugin.rs reads it (the record's hash, the profile's reference, build_info).
    fn mode(&self, text: &str) -> KilnMode {
        let (hash, profile) = self.plan(text);
        select(&Launch::Kiln { plan_hash: hash, launch_id: 1 }, None, &self.game, |h| read_plan(Some(&profile), &self.kiln(), &self.game, h, &context())).unwrap()
    }

    fn run(&self, kiln: Option<KilnMode>, paths: &[&Utf8PathBuf]) -> Outcome {
        self.run_from(kiln, &paths.iter().map(|p| ((*p).clone(), "game-mods")).collect::<Vec<_>>())
    }

    fn run_from(&self, kiln: Option<KilnMode>, paths: &[(Utf8PathBuf, &str)]) -> Outcome {
        let candidates: Vec<Candidate> = paths.iter().map(|(p, source)| Candidate { path: p.clone(), source: (*source).into(), enabled: true }).collect();
        let (mods, entries) = discover::resolve(&candidates);
        let cfg = RunConfig { game_dir: self.game.clone(), cache_dir: self.kiln().join("eml/cache"), file_name: Some("enshrouded".into()),
            patch: true, kiln: kiln.map(Arc::new), ..Default::default() };
        runner::run(&cfg, mods, entries)
    }

    /// Nothing written: no backup, no stage, no cache, no receipt.
    fn untouched(&self) {
        assert_eq!(std::fs::read(self.game.join("enshrouded.kfc")).unwrap(), b"KFC fixture");
        assert!(!self.game.join("enshrouded.kfc.bak").exists());
        assert!(!self.kiln().join("eml/cache").exists() && !self.kiln().join("eml/ownership").exists());
    }
}

impl Drop for Install {
    fn drop(&mut self) {
        // A run keeps the install lock; the test gate creates no process while it is kept (runner::release_game_lock).
        runner::release_game_lock(&self.game);
        let _ = std::fs::remove_dir_all(&self.game);
    }
}

fn entry<'a>(out: &'a Outcome, id: &str) -> (&'a str, &'a str) {
    let e = out.mods.iter().find(|e| e.id.as_deref() == Some(id) || e.path.ends_with(&format!("/{id}"))).unwrap_or_else(|| panic!("{id}: {:#?}", out.mods));
    (e.state.as_str(), e.reason.as_deref().unwrap_or(""))
}

fn no_shroudforge_json(out: &Outcome) {
    for e in &out.mods {
        assert!(!e.reason.as_deref().unwrap_or("").contains(SF_STATE_REASON), "kiln mode read shroudforge.json: {e:?}");
    }
}

const INTERPRETER: &str = "this start's launch plan (launch-plans/0f1e2d3c-4b5a-4968-8776-a5b4c3d2e1f0.json) gives this \
    package to Kiln's interpreter, with no startup data to apply this start, so kiln-eml has nothing to run for it";
const NEEDS: &str = "needs ShroudForge's runtime, and Kiln's interpreter runs only what this start's launch \
    plan (launch-plans/0f1e2d3c-4b5a-4968-8776-a5b4c3d2e1f0.json) admits: ";
const LEFT_OUT: &str = "this start's launch plan (launch-plans/0f1e2d3c-4b5a-4968-8776-a5b4c3d2e1f0.json) ";
const LEFT_OUT_TAIL: &str = ", and Kiln's interpreter has this start: kiln-eml runs no package the plan leaves out; \
    its data was not applied, so the game is as without the mod";
fn unlisted(dialect: &str) -> String {
    format!("this start's launch plan ({PLAN_NAME}) does not list this EML mod, and Kiln's interpreter has this \
        start: it runs one startup pass ({dialect}), the plan's, so kiln-eml runs no EML mod the plan leaves out; its data \
        was not applied, so the game is as without the mod. Activate the profile in the Kiln launcher again to plan it")
}

/// The backend x record table: only kiln, without KILN_SHROUDFORGE=0, is kiln mode; every other
/// record (no table, an unreadable one, the idle class, none, hosted, installed) and KILN_SHROUDFORGE=0 keep the run of
/// before kiln.launch byte for byte.
#[test]
fn every_record_but_kiln_keeps_the_rules_from_before() {
    for launch in [Launch::Absent, Launch::Idle, Launch::Refused, Launch::Shroudforge] {
        assert!(select(&launch, None, Utf8Path::new("/g"), |_| panic!("only kiln reads a plan")).is_none(), "{launch:?}");
    }
    assert!(select(&Launch::Kiln { plan_hash: [0; 32], launch_id: 1 }, Some("0"), Utf8Path::new("/g"), |_| panic!("not read")).is_none());
    for env in [None, Some("1"), Some(""), Some("00")] {
        assert!(select(&Launch::Kiln { plan_hash: [0; 32], launch_id: 1 }, env, Utf8Path::new("/g"), |_| Err("x".into())).is_some(), "{env:?}");
    }
    let i = Install::new("legacy-bytes");
    let paths = [i.eml("mods/pure", "pure", &["patch"]), i.eml("mods/rt", "rt", &["patch", "runtime"]), i.sf("mods/sf", "sf", &["runtime"])];
    let refs: Vec<&Utf8PathBuf> = paths.iter().collect();
    let strip = |o: &Outcome| {
        let mut v = serde_json::to_value(o).unwrap();
        v.as_object_mut().unwrap().remove("ms");
        v
    };
    let before = i.run(None, &refs);
    let today = format!("needs ShroudForge's runtime, which is not running: {SF_STATE_REASON}. Enable/install the ShroudForge \
        component and activate the profile again; its data was not applied, so the game is as without the mod");
    assert_eq!(entry(&before, "rt"), ("skipped", today.as_str()));
    assert!(entry(&before, "pure").0 == "failed" && entry(&before, "pure").1.contains(RAN), "{:?}", entry(&before, "pure"));
    let before = strip(&before);
    for launch in [Launch::Absent, Launch::Idle, Launch::Refused, Launch::Shroudforge] {
        let mode = select(&launch, None, &i.game, |_| unreachable!());
        assert_eq!(strip(&i.run(mode, &refs)), before, "{launch:?}");
    }
    let (hash, profile) = i.plan(&strict_plan(&[p("pure", "mods/pure", "eml", &["patch"], "candidate")]));
    let zero = select(&Launch::Kiln { plan_hash: hash, launch_id: 1 }, Some("0"), &i.game, |h| read_plan(Some(&profile), &i.kiln(), &i.game, h, &context()));
    assert_eq!(strip(&i.run(zero, &refs)), before, "KILN_SHROUDFORGE=0");
}

/// Kiln mode through the runner, without kiln-eml's early phase (no start): no package runs on its own and nothing is
/// written. Each discovered package gets the plan's decision: a participant of the startup dialect's pass waits for the
/// startup outcome (none here: NO_START); a candidate outside the pass is the interpreter's; a listed package the plan
/// does not admit says so; one that needs the runtime and is not admitted says so; an unlisted EML mod follows owner
/// question Q1 (skipped in the ShroudForge and EML dialects, the EML pass's own in a start without a dialect). Never
/// kiln/state/shroudforge.json.
#[test]
fn kiln_mode_decides_every_package_and_runs_none_on_its_own() {
    let i = Install::new("table");
    let sf_patch = i.sf("mods/sf-patch", "sf-patch", &["patch"]);
    let sf_rt = i.sf("mods/sf-rt", "sf-rt", &["runtime"]);
    let eml_patch = i.eml("mods/eml-patch", "eml-patch", &["patch"]);
    let skipped = i.eml("mods/skipped", "skipped", &["patch"]);
    let off = i.eml("mods/off", "off", &["patch"]);
    let other = i.eml("mods/other", "other", &["export"]);
    let stray = i.eml("kiln/eml/mods/stray", "stray", &["patch"]);
    let rt_stray = i.eml("mods/rt-stray", "rt-stray", &["runtime-register-dll"]);
    let all = [&sf_patch, &sf_rt, &eml_patch, &skipped, &off, &other, &stray, &rt_stray];
    let listed = |sf: bool| {
        let mut list = vec![
            p("eml-patch", "mods/eml-patch", "eml", &["patch"], "candidate"),
            P { reasons: &["needs the missing package quiver-core"], ..p("skipped", "mods/skipped", "eml", &["patch"], "skip") },
            p("off", "mods/off", "eml", &["patch"], "disabled"),
            P { reasons: &["targets the server"], ..p("other", "mods/other", "eml", &["export"], "other-side") },
            p("sf-rt", "mods/sf-rt", "shroudforge", &["runtime"], "candidate"),
        ];
        if sf {
            list.push(p("sf-patch", "mods/sf-patch", "shroudforge", &["patch"], "candidate"));
        }
        list
    };
    let common = |out: &Outcome, dialect: &str| {
        no_shroudforge_json(out);
        assert_eq!(entry(out, "skipped"), ("skipped", format!("{LEFT_OUT}does not admit this package (skip: needs the \
            missing package quiver-core){LEFT_OUT_TAIL}").as_str()), "{dialect}");
        assert_eq!(entry(out, "off"), ("skipped", format!("{LEFT_OUT}has this package switched off (disabled){LEFT_OUT_TAIL}").as_str()), "{dialect}");
        assert_eq!(entry(out, "other"), ("skipped", format!("{LEFT_OUT}has this package for the other side only (other-side: \
            targets the server){LEFT_OUT_TAIL}").as_str()), "{dialect}");
        assert_eq!(entry(out, "rt-stray"), ("skipped", format!("{NEEDS}it does not list it: the package was added after the \
            launcher wrote the plan; activate the profile in the Kiln launcher again; its data was not applied, so the game is \
            as without the mod").as_str()), "{dialect}");
        assert_eq!(entry(out, "sf-rt").1, INTERPRETER, "{dialect}");
        assert!(!out.mods.iter().any(|e| e.reason.as_deref().unwrap_or("").contains(RAN)), "{dialect}: something ran");
    };
    // ShroudForge's dialect: its pass has the plan's EML and ShroudForge patch packages
    let out = i.run_from(Some(i.mode(&strict_plan(&listed(true)))), &all.iter().map(|p| ((*p).clone(), "game-mods")).collect::<Vec<_>>());
    common(&out, "shroudforge");
    for id in ["eml-patch", "sf-patch"] {
        assert_eq!(entry(&out, id), ("skipped", crate::startup_seam::NO_START), "{id}");
    }
    assert_eq!(entry(&out, "stray"), ("skipped", unlisted("ShroudForge").as_str()));
    // EML's dialect: its pass has the plan's EML packages
    let out = i.run(Some(i.mode(&strict_plan(&listed(false)))), &all);
    common(&out, "eml");
    assert_eq!(entry(&out, "eml-patch"), ("skipped", crate::startup_seam::NO_START));
    assert_eq!(entry(&out, "stray"), ("skipped", unlisted("EML").as_str()));
    // no startup dialect: the unlisted EML mods are the EML pass's (no start here: NO_START)
    let none = [p("sf-rt", "mods/sf-rt", "shroudforge", &["runtime"], "candidate"),
        P { reasons: &["targets the server"], ..p("other", "mods/other", "eml", &["export"], "other-side") }];
    let out = i.run(Some(i.mode(&strict_plan(&none))), &[&sf_rt, &stray, &other]);
    no_shroudforge_json(&out);
    assert_eq!(entry(&out, "sf-rt"), ("skipped", INTERPRETER));
    assert_eq!(entry(&out, "stray"), ("skipped", crate::startup_seam::NO_START));
    assert_eq!(out.phase, Phase::Idle, "nothing ran");
    // the frozen plan cannot be read again: nothing runs
    let gone = KilnMode::new(&i.game, Err("launch-plans/x.json changed since this start froze it".into()));
    let out = i.run(Some(gone), &all);
    for id in ["eml-patch", "skipped", "stray", "rt-stray"] {
        let (state, reason) = entry(&out, id);
        assert!(state == "skipped" && reason.starts_with("Kiln could not read this start's launch plan again \
            (launch-plans/x.json changed since this start froze it)"), "{id}: {state} {reason}");
    }
    i.untouched();
}

/// An excluded package never comes back: a copy of a package the plan leaves out, under
/// the same manifest id, keeps the plan's decision whether it is a drop-in, a profile entry (a Library path) or an
/// engine's add_mod handover; a folder at a plan root whose manifest cannot be read is the plan's package by its root;
/// and a copy of a participant elsewhere is the pass's, never an EML mod on its own.
#[test]
fn an_excluded_package_never_comes_back() {
    let i = Install::new("comeback");
    let plan = strict_plan(&[
        P { reasons: &["a dependent of quiver"], ..p("left", "mods/left", "eml", &["patch"], "skip") },
        p("pass", "mods/pass", "eml", &["patch"], "candidate"),
    ]);
    let mode = i.mode(&plan);
    let left = i.eml("mods/left", "left", &["patch"]);
    let dropin = i.eml("kiln/eml/mods/left-copy", "left", &["patch"]);
    let library = i.eml("kiln/library/left", "left", &["patch"]);
    let handover = i.eml("kiln/mods/left-engine", "left", &["patch"]);
    let pass_copy = i.eml("kiln/eml/mods/pass-copy", "pass", &["patch"]);
    let reason = format!("{LEFT_OUT}does not admit this package (skip: a dependent of quiver){LEFT_OUT_TAIL}");
    let mut entries = Vec::new();
    for (path, source) in [(&left, "game-mods"), (&dropin, "kiln-dropin"), (&library, "profile"), (&handover, "plugin"), (&pass_copy, "kiln-dropin")] {
        let (mods, mut found) = discover::resolve(&[Candidate { path: path.clone(), source: source.into(), enabled: true }]);
        let unlisted = mode.apply(mods, &mut found);
        assert!(unlisted.is_empty(), "{source}: {unlisted:?}");
        entries.push((source, found.remove(0)));
    }
    for (source, e) in &entries[..4] {
        assert_eq!((e.state.as_str(), e.reason.as_deref()), ("skipped", Some(reason.as_str())), "{source}");
    }
    assert_eq!(entries[4].1.state, "pending", "the participant's copy waits for the startup outcome: {:?}", entries[4].1);
    // classification: no readable manifest at the plan's root
    std::fs::write(left.join("mod.json"), "{").unwrap();
    assert!(matches!(mode.decide(&left, None, false), Decision::Skip(r) if r == reason));
    assert!(mode.listed(&left, None) && mode.listed(&dropin, Some("left")) && !mode.listed(&dropin, Some("x")));
}

/// Kiln mode never reads kiln/state/shroudforge.json, for the launcher's delegated entries either.
#[test]
fn kiln_mode_never_reads_shroudforge_json() {
    let i = Install::new("no-sf-json");
    let rt = i.eml("mods/quiver", "quiver", &["runtime"]);
    let other = i.eml("mods/stacks", "stacks", &["patch", "runtime"]);
    let blocked = i.eml("mods/blocked", "blocked", &["runtime"]);
    let mode = i.mode(&strict_plan(&[p("quiver", "mods/quiver", "shroudforge", &["runtime"], "candidate")]));
    let json = serde_json::json!({"mods": {"eml": [
        {"id": "quiver", "path": rt.as_str(), "enabled": false},
        {"id": "stacks", "path": other.as_str(), "enabled": false},
        {"id": "blocked", "path": blocked.as_str(), "enabled": false},
    ]}, "emlRuntime": ["quiver", "stacks"], "emlRuntimeBlocked": {"blocked": "the launcher's own reason"}}).to_string();
    let (_, mut entries) = discover::resolve(&discover::profile_candidates(&json, &i.kiln()));
    discover::apply_runtime_blocks_with(&json, &i.kiln(), &mut entries, |e| mode.delegated(e));
    assert_eq!((entries[0].state.as_str(), entries[0].reason.as_deref()), ("skipped", Some(INTERPRETER)));
    assert!(entries[1].reason.as_deref().unwrap().starts_with(NEEDS), "{:?}", entries[1]);
    assert_eq!(entries[2].reason.as_deref(), Some("the launcher's own reason"), "the launcher's block stays");
    for e in &entries {
        assert!(!e.reason.as_deref().unwrap().contains(SF_STATE_REASON), "{e:?}");
    }
    let out = i.run(Some(mode), &[&rt]);
    no_shroudforge_json(&out);
    assert_eq!(entry(&out, "quiver"), ("skipped", INTERPRETER));
    assert!(entry(&i.run(None, &[&rt]), "quiver").1.contains(SF_STATE_REASON), "without kiln mode the state file is read");
}

/// The plan is used only when it is the file the record froze, named the way the core names it, and when the
/// interpreter's reader takes it for this executable and side, with the plan id its file name carries and backend kiln
/// selected (as kiln-sfi's plan_source.rs reads it).
#[test]
fn the_plan_is_bound_to_the_frozen_hash_and_read_as_kiln_sfi_reads_it() {
    let i = Install::new("plan-read");
    let text = strict_plan(&[p("a", "mods/a", "shroudforge", &["patch"], "candidate")]);
    let (hash, profile) = i.plan(&text);
    let kiln = i.kiln();
    let plan = read_plan(Some(&profile), &kiln, &i.game, &hash, &context()).unwrap();
    assert_eq!((plan.name(), plan.dialect(), plan.packages.len(), plan.sha256()), (PLAN_NAME, Dialect::Shroudforge, 1, hash));
    assert!(read_plan(Some(&format!("\u{feff}{profile}")), &kiln, &i.game, &hash, &context()).is_ok(), "a profile with a byte order mark");
    // another executable or side than the core's
    let other_exe = ValidationContext { exe_sha256: "b".repeat(64), ..context() };
    assert!(read_plan(Some(&profile), &kiln, &i.game, &hash, &other_exe).unwrap_err().contains("plan executable hash or side mismatch"));
    let server = ValidationContext { side: "server".into(), ..context() };
    assert!(read_plan(Some(&profile), &kiln, &i.game, &hash, &server).unwrap_err().contains("plan executable hash or side mismatch"));
    // one byte changed since the freeze
    let file = kiln.join(format!("launch-plans/{PLAN}.json"));
    std::fs::write(&file, text.replace("\"a\"", "\"b\"")).unwrap();
    assert_eq!(read_plan(Some(&profile), &kiln, &i.game, &hash, &context()).unwrap_err(), format!("{PLAN_NAME} changed since this start froze it"));
    std::fs::write(&file, &text).unwrap();
    // the plan id is not the file name's; another backend selected; a dialect the packages contradict
    for (broken, why) in [(text.replace(PLAN, "0f1e2d3c-4b5a-4968-8776-a5b4c3d2e1f1"), "has a plan id that is not the one its file name carries"),
                          (text.replace("\"selected\":\"kiln\"", "\"selected\":\"none\"").replace("\"dataWriter\":\"kiln-eml\"", "\"dataWriter\":\"none\"")
                               .replace("\"startupDialect\":\"shroudforge\"", "\"startupDialect\":\"none\"").replace("\"loadOrder\":[\"a\"]", "\"loadOrder\":[]")
                               .replace("\"decision\":\"candidate\"", "\"decision\":\"skip\"")
                               .replace("\"reasons\":[]", "\"reasons\":[{\"causedBy\":[],\"code\":\"x\",\"message\":\"m\",\"severity\":\"info\"}]")
                               .replace("\"admission\":\"admit\"", "\"admission\":\"refuse\""), "does not select Kiln's interpreter"),
                          (text.replace("\"startupDialect\":\"shroudforge\"", "\"startupDialect\":\"eml\""), "startupDialect contradicts")] {
        let (h, p) = i.plan(&broken);
        let err = read_plan(Some(&p), &kiln, &i.game, &h, &context()).unwrap_err();
        assert!(err.contains(why), "{why}: {err}");
    }
    let (hash, profile) = i.plan(&text);
    // no profile, no reference, a reference the core would not follow
    assert_eq!(read_plan(None, &kiln, &i.game, &hash, &context()).unwrap_err(), "this start has no profile");
    assert_eq!(read_plan(Some("{"), &kiln, &i.game, &hash, &context()).unwrap_err(), "the profile does not parse");
    assert_eq!(read_plan(Some(r#"{"mods":{}}"#), &kiln, &i.game, &hash, &context()).unwrap_err(), "the profile names no launch plan");
    for path in ["launch-plans/0F1E2D3C-4B5A-4968-8776-A5B4C3D2E1F0.json", "launch-plans/../launch-plans/0f1e2d3c-4b5a-4968-8776-a5b4c3d2e1f0.json",
                 "0f1e2d3c-4b5a-4968-8776-a5b4c3d2e1f0.json", "launch-plans/x.json", "launch-plans\\0f1e2d3c-4b5a-4968-8776-a5b4c3d2e1f0.json"] {
        let profile = serde_json::json!({"sfLaunchPlan": {"path": path, "sha256": "0"}}).to_string();
        assert_eq!(read_plan(Some(&profile), &kiln, &i.game, &hash, &context()).unwrap_err(), "the profile's launch plan is not launch-plans/<plan id>.json", "{path}");
    }
    // not a plain file, too large
    std::fs::remove_file(&file).unwrap();
    std::fs::create_dir(&file).unwrap();
    assert!(read_plan(Some(&profile), &kiln, &i.game, &hash, &context()).unwrap_err().ends_with(" is not a plain file"));
    std::fs::remove_dir(&file).unwrap();
    let big = format!("{}{}", text, " ".repeat(8 * 1024 * 1024));
    let (big_hash, big_profile) = i.plan(&big);
    assert!(read_plan(Some(&big_profile), &kiln, &i.game, &big_hash, &context()).unwrap_err().ends_with(" is larger than 8 MiB"));
    // the frozen bytes, but not a plan the interpreter reads
    for broken in [r#"{"startupDialect":"none","packages":[]}"#, "[1,2"] {
        let (h, p) = i.plan(broken);
        assert!(read_plan(Some(&p), &kiln, &i.game, &h, &context()).unwrap_err().starts_with(&format!("{PLAN_NAME} is not a launch plan Kiln's interpreter reads")), "{broken}");
    }
}

/// Two packages whose roots are one folder of the game under two names (a link; on Windows a junction) are overlapping
/// roots no string comparison sees: the plan is refused by identity (sfi-policy's `check_folders`, as kiln-sfi's
/// plan_source.rs refuses it), so kiln-eml decides nothing from it. Two folders, or a root with no folder yet, are read.
#[test]
fn two_packages_in_one_folder_under_two_names_refuse_the_plan() {
    let i = Install::new("one-folder");
    let kiln = i.kiln();
    std::fs::create_dir_all(i.game.join("mods/a")).unwrap();
    let text = strict_plan(&[p("alpha", "mods/a", "shroudforge", &["patch"], "candidate"),
        p("beta", "mods/b", "shroudforge", &["patch"], "candidate")]);
    let (hash, profile) = i.plan(&text);
    assert!(read_plan(Some(&profile), &kiln, &i.game, &hash, &context()).is_ok(), "mods/b does not exist: by name only");
    std::fs::create_dir_all(i.game.join("mods/b")).unwrap();
    assert!(read_plan(Some(&profile), &kiln, &i.game, &hash, &context()).is_ok(), "two folders");
    std::fs::remove_dir(i.game.join("mods/b")).unwrap();
    #[cfg(unix)]
    std::os::unix::fs::symlink(i.game.join("mods/a"), i.game.join("mods/b")).unwrap();
    #[cfg(windows)]
    {
        // each component joined, so cmd sees backslashes only (a "/b" would be a switch to it)
        let mods = i.game.as_std_path().join("mods");
        let out = mod_loader_lua::test_gate::output(std::process::Command::new("cmd").args(["/C", "mklink", "/J"])
            .arg(mods.join("b")).arg(mods.join("a"))).unwrap();
        assert!(out.status.success(), "{out:?}");
    }
    assert_eq!(read_plan(Some(&profile), &kiln, &i.game, &hash, &context()).unwrap_err(),
        format!("{PLAN_NAME} is not a launch plan Kiln's interpreter reads (overlapping package roots: alpha and beta are one folder)"));
    #[cfg(unix)]
    std::fs::remove_file(i.game.join("mods/b")).unwrap();
    #[cfg(windows)]
    std::fs::remove_dir(i.game.join("mods/b")).unwrap(); // the junction only
}

/// The shared launch-plan vectors (schema/fixtures/sfi/launch-plan, sfi-policy's own test runs them too): kiln-eml's
/// reader takes every plan in accept/ and refuses every plan in refuse/, as the interpreter's (the plugin wiring moved kiln-eml onto
/// it). `SFI_LAUNCH_PLAN_VECTORS`: where the vectors are when the source tree is not.
#[test]
fn the_shared_accept_and_refuse_vectors() {
    let root = std::env::var_os("SFI_LAUNCH_PLAN_VECTORS").map_or_else(
        || std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../schema/fixtures/sfi/launch-plan"),
        std::path::PathBuf::from);
    let mut seen = 0;
    for (folder, accept) in [("accept", true), ("refuse", false)] {
        let mut files: Vec<_> = std::fs::read_dir(root.join(folder)).unwrap().map(|e| e.unwrap().path()).collect();
        files.sort();
        assert!(!files.is_empty(), "{folder}");
        for file in files {
            let bytes = std::fs::read(&file).unwrap();
            let ours = parse_plan("p", &bytes, &context());
            let theirs = sfi_policy::launch_plan::LaunchPlan::parse(&bytes, &context());
            assert_eq!((ours.is_ok(), theirs.is_ok()), (accept, accept), "{}: {:?}", file.display(), ours.as_ref().err());
            seen += 1;
        }
    }
    assert!(seen >= 13, "{seen}");
}

/// The startup controller's view of a plan (`plan_facts`, its `PlanFacts`) is the plan's: dialect, digests, the BASE origin
/// when present, each package's kind, root, digests, decision, startup data, native code (a planned DLL or
/// runtime-register-dll) and dependencies, and the load order.
#[test]
fn the_plan_facts_are_the_plans() {
    let mut doc: serde_json::Value = serde_json::from_str(&strict_plan(&[
        p("lib", "mods/lib", "shroudforge", &["patch"], "candidate"),
        p("dll", "mods/dll", "shroudforge", &["patch"], "candidate"),
        p("reg", "mods/reg", "eml", &["runtime-register-dll"], "candidate"),
        P { reasons: &["why"], ..p("left", "mods/left", "eml", &["patch"], "skip") },
    ])).unwrap();
    doc["game"]["archiveBaseOrigin"] = "unrecorded".into();
    doc["game"]["archiveInput_sha256"] = "c".repeat(64).into();
    doc["packages"][0]["content_sha256"] = "0".repeat(64).into();
    doc["packages"][1]["config_sha256"] = "1".repeat(64).into();
    doc["packages"][1]["dlls"] = serde_json::json!([{"path": "x.dll", "sha256": "a".repeat(64), "mode": "library", "source": "ini",
        "required": true, "gate": "pass", "gateProof_sha256": "b".repeat(64), "bridgeAbi": 0}]);
    doc["packages"][1]["dependencies"] = serde_json::json!([{"id": "lib", "range": "^1", "optional": false, "included": true}]);
    let plan = parse_plan("p", doc.to_string().as_bytes(), &context()).unwrap();
    let facts = plan.facts();
    assert_eq!(facts.dialect, crate::sf_pass::StartupDialect::Shroudforge);
    assert_eq!((facts.exe_sha256.as_str(), facts.archive_input_sha256.as_str()), (EXE, "c".repeat(64).as_str()));
    assert_eq!(facts.archive_base_origin.as_deref(), Some("unrecorded"));
    assert_eq!(facts.plan_id, sfi_policy::launch_plan::LaunchPlan::parse(doc.to_string().as_bytes(), &context()).unwrap().plan_id());
    assert_eq!(facts.load_order, ["lib", "dll", "reg"]);
    let f = |id: &str| facts.package(id).unwrap().clone();
    assert_eq!((f("lib").content_sha256, f("dll").config_sha256), ([0; 32], [0x11; 32]));
    assert_eq!((f("lib").kind, f("reg").kind), (crate::startup::PackageKind::Shroudforge, crate::startup::PackageKind::Eml));
    assert_eq!((f("lib").native, f("dll").native, f("reg").native, f("left").native), (false, true, true, false));
    assert_eq!((f("lib").data_required, f("reg").data_required, f("left").data_required), (true, false, true));
    assert_eq!((f("lib").candidate, f("left").candidate), (true, false));
    assert_eq!(f("dll").dependencies, [crate::startup::PlanDependency { id: "lib".into(), optional: false, included: true }]);
    assert_eq!(f("dll").root, "mods/dll");
    let participants: Vec<&str> = facts.participants().iter().map(|p| p.id.as_str()).collect();
    assert_eq!(participants, ["lib", "dll"], "the ShroudForge pass's packages with startup data");
}

/// Kiln mode runs no package on its own that the rules from before would skip: whatever the plan says, a package it
/// leaves to the runner (Run) is unlisted and needs no runtime, a package of the pass (Pass) is a listed participant,
/// and an unreadable plan decides Skip for everything.
#[test]
fn kiln_mode_never_runs_a_package_the_rules_from_before_skip() {
    let game = Utf8Path::new("/g");
    let path = game.join("mods/p");
    for (kind, caps) in [("shroudforge", &["patch"][..]), ("eml", &["patch"]), ("eml", &["export"]), ("shroudforge", &["runtime"])] {
        for decision in ["candidate", "skip", "disabled", "other-side"] {
            for root in ["mods/p", "mods/q"] {
                let text = strict_plan(&[P { reasons: &["why"], ..p("p", root, kind, caps, decision) }]);
                let plan = parse_plan("plan", text.as_bytes(), &context()).unwrap_or_else(|e| panic!("{kind} {caps:?} {decision}: {e}"));
                let mode = KilnMode::new(game, Ok(plan));
                for id in [Some("p"), Some("x"), None] {
                    for runtime in [false, true] {
                        let d = mode.decide(&path, id, runtime);
                        let listed = root == "mods/p" || id == Some("p");
                        assert!(d != Decision::Run || (!runtime && !listed), "{kind} {decision} root {root} id {id:?} runtime {runtime}: {d:?}");
                        assert!(d != Decision::Pass || (listed && decision == "candidate"), "{kind} {decision} root {root} id {id:?}: {d:?}");
                    }
                }
            }
        }
    }
    let gone = KilnMode::new(game, Err("gone".into()));
    for runtime in [false, true] {
        assert!(matches!(gone.decide(&path, Some("p"), runtime), Decision::Skip(_)));
    }
}

/// The plan's folder must be a plain one, as the core reads it (launch_boot.cpp read_plan): a link to a folder holding
/// the same file is refused.
#[test]
fn the_plans_folder_must_be_a_plain_one() {
    let i = Install::new("plan-folder");
    let (hash, profile) = i.plan(&strict_plan(&[]));
    let kiln = i.kiln();
    assert!(read_plan(Some(&profile), &kiln, &i.game, &hash, &context()).is_ok());
    #[cfg(unix)]
    std::os::unix::fs::symlink(kiln.join("real"), kiln.join("launch-plans.link")).unwrap();
    #[cfg(windows)]
    let linked = std::os::windows::fs::symlink_dir(kiln.join("real"), kiln.join("launch-plans.link")).is_ok();
    #[cfg(not(windows))]
    let linked = true;
    if !linked {
        eprintln!("no symbolic link privilege: the folder link case was not run");
        return;
    }
    std::fs::rename(kiln.join("launch-plans"), kiln.join("real")).unwrap();
    std::fs::rename(kiln.join("launch-plans.link"), kiln.join("launch-plans")).unwrap();
    assert_eq!(read_plan(Some(&profile), &kiln, &i.game, &hash, &context()).unwrap_err(), "kiln/launch-plans is not a plain folder");
}

/// A file is checked through the handle it is read from: a link swapped in after the check is not the checked file.
#[test]
fn the_plan_is_read_through_a_handle_without_following_a_link() {
    let i = Install::new("plan-handle");
    let text = strict_plan(&[]);
    let (hash, profile) = i.plan(&text);
    let kiln = i.kiln();
    assert!(read_plan(Some(&profile), &kiln, &i.game, &hash, &context()).is_ok());
    let file = kiln.join(format!("launch-plans/{PLAN}.json"));
    #[cfg(unix)]
    {
        let target = kiln.join("target.json");
        std::fs::write(&target, &text).unwrap();
        let link = kiln.join("link.json");
        std::os::unix::fs::symlink(&target, &link).unwrap();
        let checked = std::fs::symlink_metadata(&link).unwrap();
        let held = open_no_follow(&link).unwrap().metadata().unwrap();
        assert!(!same_file(&checked, &held), "a handle on another file than the one checked is refused");
        assert!(same_file(&std::fs::symlink_metadata(&file).unwrap(), &open_no_follow(&file).unwrap().metadata().unwrap()));
    }
    #[cfg(windows)]
    {
        let target = kiln.join("target.json");
        std::fs::write(&target, &text).unwrap();
        let link = kiln.join("link.json");
        if std::os::windows::fs::symlink_file(&target, &link).is_ok() {
            let held = open_no_follow(&link).unwrap().metadata().unwrap();
            assert!(reparse(&held) && !held.is_file(), "the handle is the link's, not its target's");
            std::fs::remove_file(&link).unwrap();
        } else {
            eprintln!("no symbolic link privilege: the link case was not run");
        }
        assert!(!reparse(&open_no_follow(&file).unwrap().metadata().unwrap()));
    }
}
