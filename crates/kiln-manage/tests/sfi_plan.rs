// SPDX-License-Identifier: MIT
//! The launch-plan producer: what it writes for a `kiln` start, that every reader takes it (sfi-policy's
//! strict reader, the core's checks, kiln-sfi's read of the frozen plan), byte-stable identity, fingerprints, the archive
//! inspector's facts, fail-closed refusals, the release hold, and that hosted/installed starts and
//! `<game>/shroudforge` are never touched. Every package, DLL and inspection here is synthetic.
mod sfi_scan_common;
use kiln_manage::manifest::RunSide;
use kiln_manage::sfi_backend::{self, Env};
use kiln_manage::sfi_inspection::{self, Inspect, ORIGINS};
use kiln_manage::sfi_plan::{self, Mode, Outcome, ProfileIdentity, Reason, Request, Written};
use kiln_manage::sfi_scan::{native::NativeConfig, parity::CatalogInput};
use serde_json::{Value, json};
use sfi_policy::digest;
use sfi_policy::launch_plan::{Decision, LaunchPlan, Permission, Severity, ValidationContext};
use sfi_scan_common::*;
use std::cell::{Cell, RefCell};
use std::collections::BTreeMap;
use std::fs;
use std::io::Read as _;
use std::path::{Path, PathBuf};

const EXE: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const BASE: &str = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
const INPUT: &str = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
const VERSION: &str = "0.2.0-test";
/// kiln.json for a `kiln` start: the hold lifted the way the core lifts it.
const KILN: &str = r#"{"preview": true, "shroudforge": {"backend": "kiln"}}"#;

fn label(token: &str) -> &'static str {
    ORIGINS
        .iter()
        .find(|(t, _)| *t == token)
        .map(|(_, l)| *l)
        .unwrap_or("no such label")
}
/// An inspection as kiln-eml-inspect prints it (status ok).
fn inspection(side: RunSide, origin: &str, evidence: &str) -> Value {
    let (s, stem) = match side {
        RunSide::Client => ("client", "enshrouded"),
        RunSide::Server => ("server", "enshrouded_server"),
    };
    json!({"schema_version":1,"inspector":{"component":"kiln-eml-inspect","version":VERSION},"status":"ok",
        "target":{"side":s,"stem":stem,"executable":{"file":format!("{stem}.exe"),"size":10,"sha256":EXE}},
        "proof":{"framing":"kiln-eml-inspect archive-set v1","containers":"size"},
        "input":{"class":"unclassified","sha256":INPUT,"files":[]},
        "base":{"evidence":evidence,"sha256":BASE,"origin":{"token":origin,"label":label(origin)},"baseline":null,"files":[]},
        "receipt":{"state":"absent","file":format!("kiln/eml/ownership/{stem}.json")},
        "inventory":[{"role":"executable","file":format!("{stem}.exe"),"size":10},{"role":"header","file":format!("{stem}.kfc"),"size":5}],
        "reasons":[],"notes":[],"cost":{"bytesRead":15,"filesRead":2}})
}
fn first_managed() -> Value {
    inspection(RunSide::Client, "first-managed-unclassified", "current-set")
}

/// A stand-in for the shipped helper: its stdout, and how often it ran.
struct Fake {
    out: RefCell<Result<Vec<u8>, String>>,
    calls: Cell<u32>,
}
impl Fake {
    fn new(v: &Value) -> Self {
        Self::raw(Ok(format!("{v}\n").into_bytes()))
    }
    fn raw(out: Result<Vec<u8>, String>) -> Self {
        Self {
            out: RefCell::new(out),
            calls: Cell::new(0),
        }
    }
}
impl Inspect for Fake {
    fn run(&self, _: &Path, _: RunSide) -> Result<Vec<u8>, String> {
        self.calls.set(self.calls.get() + 1);
        self.out.borrow().clone()
    }
    fn version(&self) -> &str {
        VERSION
    }
}

fn kiln_game(tag: &str, kiln_json: &str) -> Game {
    let g = Game::new(&format!("plan-{tag}"));
    fs::create_dir_all(g.0.join("kiln/plugins")).unwrap();
    fs::write(g.0.join("kiln/kiln.json"), kiln_json).unwrap();
    // Kiln's interpreter, which a plan is written for (none without it)
    fs::write(g.0.join("kiln/plugins/kiln-sfi.dll"), b"interpreter").unwrap();
    g
}
fn profile() -> ProfileIdentity {
    ProfileIdentity::of(
        &json!({"format":1,"id":"p1","generation":7,"mods":{}}),
        Mode::Active,
    )
    .unwrap()
}
fn produce_with(
    g: &Game,
    side: RunSide,
    inspector: &dyn Inspect,
    native: NativeConfig,
    carried: &[Reason],
) -> Outcome {
    let p = profile();
    let policy = policy();
    sfi_plan::produce(Request {
        game: &g.0,
        side,
        env: &Env::default(),
        profile: &p,
        inspector,
        native,
        policy: &policy,
        catalog: CatalogInput::Missing,
        carried,
    })
}
fn produce(g: &Game, inspector: &dyn Inspect) -> Outcome {
    produce_with(g, RunSide::Client, inspector, native(), &[])
}
fn written(o: Outcome) -> Written {
    match o {
        Outcome::Written(w) => *w,
        other => panic!("no plan written: {other:?}"),
    }
}
fn refused(o: Outcome) -> (&'static str, String) {
    match o {
        Outcome::Refused { code, reason, .. } => (code, reason),
        other => panic!("not refused: {other:?}"),
    }
}
fn plan_file(g: &Game, w: &Written) -> PathBuf {
    g.0.join("kiln").join(&w.reference.path)
}
fn plan_json(g: &Game, w: &Written) -> Value {
    serde_json::from_slice(&fs::read(plan_file(g, w)).unwrap()).unwrap()
}
fn context() -> ValidationContext {
    ValidationContext {
        exe_sha256: EXE.into(),
        side: "client".into(),
        upstream_180_final: false,
    }
}
fn package<'a>(plan: &'a Value, id: &str) -> &'a Value {
    plan["packages"]
        .as_array()
        .unwrap()
        .iter()
        .find(|p| p["id"] == id)
        .unwrap_or_else(|| panic!("{id} not in the plan"))
}
fn plan_files(g: &Game) -> Vec<String> {
    let mut names: Vec<String> = fs::read_dir(g.0.join("kiln/launch-plans"))
        .map(|d| {
            d.flatten()
                .map(|e| e.file_name().to_string_lossy().into_owned())
                .collect()
        })
        .unwrap_or_default();
    names.sort();
    names
}
/// Every path under `root` with its size and modification time (unreadable folders are listed, not entered).
fn snapshot(root: &Path) -> BTreeMap<PathBuf, (u64, Option<std::time::SystemTime>)> {
    let mut out = BTreeMap::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(d) = stack.pop() {
        let Ok(entries) = fs::read_dir(&d) else {
            continue;
        };
        for e in entries.flatten() {
            let m = fs::symlink_metadata(e.path()).unwrap();
            out.insert(e.path(), (m.len(), m.modified().ok()));
            if m.is_dir() {
                stack.push(e.path());
            }
        }
    }
    out
}

/// The mixed game most tests use: a library and its dependent, startup data, a switched-off and an other-side
/// package, one whose dependency is missing and its dependent.
fn mixed(g: &Game) {
    g.simple("lib", "lib", &["runtime"], json!([]));
    g.lua("lib", "src/mod.lua", "return {}\n");
    g.simple(
        "mod",
        "mod",
        &["runtime"],
        json!([{"id":"lib","version":"^1.0.0"}]),
    );
    g.lua("mod", "src/mod.lua", "return {}\n");
    g.simple("patch", "patch", &["patch"], json!([]));
    g.sf(
        "off",
        json!({"id":"off","name":"off","version":"1.0.0","capabilities":["runtime"]}),
        json!({"schemaVersion":1,"enabled":false}),
    );
    g.sf(
        "far",
        json!({"id":"far","name":"far","version":"1.0.0","capabilities":["runtime"]}),
        json!({"schemaVersion":1,"enabled":true,"targets":["server"]}),
    );
    g.simple(
        "orphan",
        "orphan",
        &["runtime"],
        json!([{"id":"missing","version":"^1"}]),
    );
    g.simple(
        "leaf",
        "leaf",
        &["runtime"],
        json!([{"id":"orphan","version":"^1"}]),
    );
}

#[test]
fn a_kiln_start_gets_a_plan_every_reader_takes() {
    let g = kiln_game("mixed", KILN);
    mixed(&g);
    let fake = Fake::new(&first_managed());
    let w = written(produce(&g, &fake));
    assert_eq!(fake.calls.get(), 1, "one inspection per plan");
    // where and what: launch-plans/<plan id>.json, pinned by its SHA-256, nothing else left in the folder
    assert_eq!(w.reference.path, format!("launch-plans/{}.json", w.plan_id));
    assert_eq!(
        plan_files(&g),
        [format!("{}.json", w.plan_id)],
        "no temporary file is left"
    );
    let bytes = fs::read(plan_file(&g, &w)).unwrap();
    assert_eq!(w.reference.sha256, kiln_manage::hash::sha256_hex(&bytes));
    assert_eq!(
        w.reference.profile_value(),
        json!({"path": w.reference.path, "sha256": w.reference.sha256})
    );
    // the strict reader (kiln-sfi's) and the producer's own validation take it
    let plan = LaunchPlan::parse(&bytes, &context()).unwrap();
    sfi_plan::validate(&bytes, EXE, RunSide::Client).unwrap();
    assert_eq!(plan.permission(true), Permission::EligiblePendingAdmission);
    assert_eq!(plan.permission(false), Permission::Held);
    let v: Value = serde_json::from_slice(&bytes).unwrap();
    assert_eq!(
        v["launch_id"],
        Value::Null,
        "the launcher's plan carries no launch id"
    );
    assert_eq!(
        (v["format"].clone(), v["minReader"].clone()),
        (json!(1), json!(1))
    );
    assert_eq!(v["writer"]["component"], "kiln-manage");
    assert_eq!(
        v["backend"],
        json!({"requested":"kiln","selected":"kiln","fallbackOrder":[],"dataWriter":"kiln-eml",
        "selectionReason":"kiln.json shroudforge.backend: \"kiln\""})
    );
    assert_eq!(
        v["game"],
        json!({"side":"client","exe_sha256":EXE,"archiveBase_sha256":BASE,"archiveInput_sha256":INPUT,
        "archiveBaseOrigin":"first-managed-unclassified"})
    );
    assert_eq!(
        v["profile"],
        json!({"id":"p1","generation":7,"mode":"active","semantic_sha256":profile().semantic_sha256})
    );
    assert_eq!(v["contract"]["upstream180Final"], false);
    assert_eq!(v["contract"]["sourceCommit"], sfi_plan::SF179_SOURCE_COMMIT);
    // the dialect is top level, ShroudForge's because a patch package is on (even skipped, see below)
    assert_eq!(v["startupDialect"], "shroudforge");
    assert!(v["packages"][0].get("startupDialect").is_none());
    // decisions: dependencies first in the load order; the rest listed with their status
    assert_eq!(w.load_order, ["lib", "mod", "patch"]);
    assert_eq!(plan.load_order(), ["lib", "mod", "patch"]);
    for (id, decision) in [
        ("lib", "candidate"),
        ("mod", "candidate"),
        ("patch", "candidate"),
        ("off", "disabled"),
        ("far", "other-side"),
        ("orphan", "skip"),
        ("leaf", "skip"),
    ] {
        assert_eq!(package(&v, id)["decision"], decision, "{id}");
        assert_eq!(w.decisions[id], decision, "{id}");
    }
    let m = package(&v, "mod");
    assert_eq!(
        m["dependencies"],
        json!([{"id":"lib","range":"^1.0.0","optional":false,"included":true}])
    );
    assert_eq!(m["lua"], json!({"required":true,"admission":"admit"}));
    assert_eq!(
        m["data"],
        json!({"required":false,"admission":"not-needed"})
    );
    assert_eq!(
        package(&v, "patch")["data"],
        json!({"required":true,"admission":"admit"})
    );
    assert_eq!(
        package(&v, "patch")["lua"],
        json!({"required":false,"admission":"not-needed"})
    );
    assert_eq!(m["root"], "mods/mod");
    // skips carry actionable reasons, the dependency chain nearest first; disabled and other-side carry none
    let orphan = package(&v, "orphan");
    assert!(
        !orphan["reasons"].as_array().unwrap().is_empty(),
        "{orphan}"
    );
    assert_eq!(orphan["lua"]["admission"], "refuse");
    // ShroudForge's planner names the dependency that failed (its own text, kept as it is)
    let leaf = &package(&v, "leaf")["reasons"][0];
    assert!(
        leaf["code"].as_str().unwrap().starts_with("plan:")
            && leaf["message"].as_str().unwrap().contains("orphan"),
        "{leaf}"
    );
    assert_eq!(package(&v, "off")["reasons"], json!([]));
    assert_eq!(package(&v, "far")["reasons"], json!([]));
    // a candidate is not a success: the plan has no loaded/active/status member anywhere
    let text = String::from_utf8(bytes.clone()).unwrap();
    for word in [
        "\"loaded\":",
        "\"active\":",
        "\"status\":",
        "\"success\":",
        "vanilla",
    ] {
        assert!(!text.contains(word), "{word} in the plan");
    }
    // mutations, a duplicate key and an unknown member are refused by both validators
    for edit in [r#""format": 1,"#, r#""minReader": 1,"#] {
        let doubled = text.replacen(edit, &format!("{edit}{edit}"), 1);
        assert!(
            sfi_plan::validate(doubled.as_bytes(), EXE, RunSide::Client).is_err(),
            "duplicate {edit}"
        );
    }
    let mut extra = v.clone();
    extra["packages"][0]["loaded"] = json!(true);
    assert!(
        sfi_plan::validate(&serde_json::to_vec(&extra).unwrap(), EXE, RunSide::Client).is_err()
    );
    let mut extra = v.clone();
    extra["game"]["archiveBaseOrigin"] = json!("vanilla");
    assert!(LaunchPlan::parse(&serde_json::to_vec(&extra).unwrap(), &context()).is_err());
    let mut nested = v.clone();
    nested["packages"][0]["startupDialect"] = json!("shroudforge");
    assert!(
        sfi_plan::validate(&serde_json::to_vec(&nested).unwrap(), EXE, RunSide::Client).is_err()
    );
    // another executable or side is another plan
    assert!(sfi_plan::validate(&bytes, &"f".repeat(64), RunSide::Client).is_err());
    assert!(sfi_plan::validate(&bytes, EXE, RunSide::Server).is_err());
}

/// kiln-sfi's read of the frozen plan (plan_source.rs read_checked), step by step: the profile's reference, the
/// checked open below `kiln/launch-plans`, the hash, the strict parse of the same bytes, the plan id; then what the
/// runtime-create re-hash checks. It admits exactly the candidates the producer decided, nothing more.
#[test]
fn the_frozen_plan_round_trip_admits_exactly_what_was_decided() {
    let g = kiln_game("roundtrip", KILN);
    mixed(&g);
    let w = written(produce(&g, &Fake::new(&first_managed())));
    let profile = json!({"format":1,"id":"p1","generation":7,"mods":{},"sfLaunchPlan": w.reference.profile_value()});
    // reference(): exactly {path, sha256}, the path launch-plans/<canonical plan id>.json
    let reference = profile["sfLaunchPlan"].as_object().unwrap();
    assert_eq!(reference.len(), 2);
    let path = reference["path"].as_str().unwrap();
    let id = path
        .strip_prefix("launch-plans/")
        .and_then(|r| r.strip_suffix(".json"))
        .unwrap();
    assert!(
        id.len() == 36
            && id
                .bytes()
                .enumerate()
                .all(|(i, b)| if [8, 13, 18, 23].contains(&i) {
                    b == b'-'
                } else {
                    b.is_ascii_digit() || (b'a'..=b'f').contains(&b)
                }),
        "{id}"
    );
    // read_plan_file(): a plain folder, the checked open, at most 8 MiB
    let folder = g.0.join("kiln/launch-plans");
    assert!(digest::plain_folder(&folder));
    let mut bytes = Vec::new();
    digest::open_plain_file(&folder.join(format!("{id}.json")))
        .unwrap()
        .read_to_end(&mut bytes)
        .unwrap();
    assert_eq!(
        kiln_manage::hash::hex(&digest::sha256(&bytes)),
        reference["sha256"].as_str().unwrap(),
        "the frozen hash"
    );
    let plan = LaunchPlan::parse(&bytes, &context()).unwrap();
    assert_eq!(kiln_manage::hash::hex(&plan.plan_id()), id.replace('-', ""));
    assert_eq!(plan.permission(true), Permission::EligiblePendingAdmission);
    assert_eq!(
        plan.archive_base_origin(),
        Some("first-managed-unclassified")
    );
    let candidates: Vec<&str> = plan
        .packages()
        .filter(|p| p.decision == Decision::Candidate)
        .map(|p| p.id)
        .collect();
    let mut decided: Vec<&str> = w
        .decisions
        .iter()
        .filter(|(_, d)| **d == "candidate")
        .map(|(id, _)| id.as_str())
        .collect();
    let mut admitted = candidates.clone();
    admitted.sort();
    decided.sort();
    assert_eq!(admitted, decided);
    assert_eq!(plan.load_order(), w.load_order);
    for p in plan.packages() {
        // runtime-create: each package folder hashes to the plan's content digest
        let folder = g.0.join("mods").join(p.folder());
        assert_eq!(
            p.content_sha256,
            digest::package_content_sha256(&folder).unwrap(),
            "{}",
            p.id
        );
        match p.decision {
            Decision::Skip => assert!(!p.reasons.is_empty(), "{}", p.id),
            Decision::Disabled | Decision::OtherSide => assert!(p.reasons.is_empty(), "{}", p.id),
            Decision::Candidate => {}
        }
    }
}

#[test]
fn equal_inputs_give_equal_bytes_but_for_the_random_id() {
    let g = kiln_game("stable", KILN);
    mixed(&g);
    let fake = Fake::new(&first_managed());
    let a = written(produce(&g, &fake));
    let b = written(produce(&g, &fake));
    assert_ne!(a.plan_id, b.plan_id, "a new random id per activation");
    let read = |w: &Written| {
        String::from_utf8(fs::read(plan_file(&g, w)).unwrap())
            .unwrap()
            .replace(&w.plan_id, "ID")
    };
    assert_eq!(read(&a), read(&b));
    assert_eq!(plan_files(&g).len(), 2, "an earlier plan is never replaced");
    // render() is a pure function of the draft and the id
    let p = profile();
    let policy = policy();
    let req = Request {
        game: &g.0,
        side: RunSide::Client,
        env: &Env::default(),
        profile: &p,
        inspector: &fake,
        native: native(),
        policy: &policy,
        catalog: CatalogInput::Missing,
        carried: &[],
    };
    let resolution = sfi_backend::resolve_game(&g.0, RunSide::Client, &Env::default());
    let (d1, _) = sfi_plan::draft(&req, &resolution).unwrap();
    let (d2, _) = sfi_plan::draft(&req, &resolution).unwrap();
    let id = "01234567-89ab-4def-8123-456789abcdef";
    assert_eq!(sfi_plan::render(&d1, id), sfi_plan::render(&d2, id));
    let text = String::from_utf8(sfi_plan::render(&d1, id)).unwrap();
    assert!(
        text.ends_with("}\n") && text.starts_with("{\n  \"backend\": {"),
        "keys in byte order, two-space indent"
    );
}

/// The chain fingerprint frames the chain files the plan's own start reads: the active profile's plan chain.ini and
/// EMF's chain only (a play-once start's once.ini and play-once.ini leave it as it is, so the next Play keeps the active
/// plan), a play-once plan every one of them.
#[test]
fn the_chain_fingerprint_follows_the_plans_mode() {
    let g = kiln_game("chain-mode", KILN);
    mixed(&g);
    let fake = Fake::new(&first_managed());
    let policy = policy();
    let resolution = sfi_backend::resolve_game(&g.0, RunSide::Client, &Env::default());
    let chain = |mode: Mode| -> String {
        let p = ProfileIdentity::of(
            &json!({"format":1,"id":"p1","generation":7,"mods":{}}),
            mode,
        )
        .unwrap();
        let req = Request {
            game: &g.0,
            side: RunSide::Client,
            env: &Env::default(),
            profile: &p,
            inspector: &fake,
            native: native(),
            policy: &policy,
            catalog: CatalogInput::Missing,
            carried: &[],
        };
        let (d, _) = sfi_plan::draft(&req, &resolution).unwrap();
        let v: Value = serde_json::from_slice(&sfi_plan::render(
            &d,
            "01234567-89ab-4def-8123-456789abcdef",
        ))
        .unwrap();
        v["fingerprints"]["chain_sha256"]
            .as_str()
            .unwrap()
            .to_owned()
    };
    let mut prev = (chain(Mode::Active), chain(Mode::Once));
    let steps: [(&str, &str, &str, bool); 5] = [
        (
            "a play-once start's once.ini",
            "kiln/chain/once.ini",
            "[profile]\r\nonce.dll\r\n",
            false,
        ),
        (
            "the play-once flag",
            "kiln/play-once.ini",
            "[play-once]\r\nmode=profile\r\n",
            false,
        ),
        (
            "the play-once flag's mode",
            "kiln/play-once.ini",
            "[play-once]\r\nmode=vanilla\r\n",
            false,
        ),
        (
            "chain.ini",
            "kiln/chain.ini",
            "[profile]\r\na.dll\r\n",
            true,
        ),
        ("EMF's chain", "dinput8-chain.ini", "x", true),
    ];
    for (what, rel, text, active_too) in steps {
        let p = g.0.join(rel);
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        fs::write(&p, text).unwrap();
        let now = (chain(Mode::Active), chain(Mode::Once));
        assert_eq!(
            now.0 != prev.0,
            active_too,
            "{what}: the active plan's chain fingerprint"
        );
        assert_ne!(
            now.1, prev.1,
            "{what}: the play-once plan's chain fingerprint"
        );
        prev = now;
    }
    // and removed again (the next activation removes a play-once start's files)
    fs::remove_file(g.0.join("kiln/chain/once.ini")).unwrap();
    fs::remove_file(g.0.join("kiln/play-once.ini")).unwrap();
    let now = (chain(Mode::Active), chain(Mode::Once));
    assert_eq!(
        now.0, prev.0,
        "removing a play-once start's files keeps the active plan's"
    );
    assert_ne!(now.1, prev.1);
}

/// Each fingerprint follows its own inputs and no other.
#[test]
fn fingerprints_follow_their_inputs() {
    let g = kiln_game("fingerprints", KILN);
    mixed(&g);
    let fp = |inspect: &Value| -> BTreeMap<String, String> {
        let w = written(produce(&g, &Fake::new(inspect)));
        let v = plan_json(&g, &w);
        let mut out: BTreeMap<String, String> = v["fingerprints"]
            .as_object()
            .unwrap()
            .iter()
            .map(|(k, v)| (k.clone(), v.as_str().unwrap().to_owned()))
            .collect();
        for k in [
            "inventory_sha256",
            "parity_sha256",
            "serviceContracts_sha256",
        ] {
            out.insert(
                format!("contract.{k}"),
                v["contract"][k].as_str().unwrap().to_owned(),
            );
        }
        out.insert(
            "profile".into(),
            v["profile"]["semantic_sha256"].as_str().unwrap().to_owned(),
        );
        out
    };
    let changed = |a: &BTreeMap<String, String>, b: &BTreeMap<String, String>| -> Vec<String> {
        a.iter()
            .filter(|(k, v)| b[*k] != **v)
            .map(|(k, _)| k.clone())
            .collect()
    };
    let base = fp(&first_managed());
    let mut prev = base.clone();
    let mut step = |what: &str, change: &dyn Fn(), inspect: &Value, expect: &[&str]| {
        change();
        let now = fp(inspect);
        assert_eq!(changed(&prev, &now), expect, "{what}");
        prev = now;
    };
    step(
        "chain.ini",
        &|| fs::write(g.0.join("kiln/chain.ini"), "[profile]\n").unwrap(),
        &first_managed(),
        &["chain_sha256"],
    );
    step(
        "the EMF chain",
        &|| fs::write(g.0.join("dinput8-chain.ini"), "x").unwrap(),
        &first_managed(),
        &["chain_sha256"],
    );
    step(
        "native.policy",
        &|| {
            fs::write(g.0.join("kiln/kiln.json"), r#"{"preview": true, "shroudforge": {"backend": "kiln"}, "native": {"policy": "verified-only"}}"#).unwrap()
        },
        &first_managed(),
        &["settings_sha256"],
    );
    step(
        "a proxy in the game folder",
        &|| fs::write(g.0.join("winmm.dll"), "proxy").unwrap(),
        &first_managed(),
        &["loaderInventory_sha256"],
    );
    step(
        "a folder named like a DLL is listed as one",
        &|| fs::create_dir_all(g.0.join("odd.dll")).unwrap(),
        &first_managed(),
        &["loaderInventory_sha256"],
    );
    step(
        "the placement receipts",
        &|| {
            fs::create_dir_all(g.0.join("kiln/state")).unwrap();
            fs::write(g.0.join("kiln/state/library-placements.json"), "{}").unwrap()
        },
        &first_managed(),
        &["placementReceipts_sha256"],
    );
    step(
        "a package file",
        &|| g.lua("lib", "src/extra.lua", "return 2\n"),
        &first_managed(),
        &["packages_sha256"],
    );
    let rules = g.0.join(sfi_plan::RULES_FILE);
    step(
        "the compatibility rules",
        &|| {
            fs::create_dir_all(rules.parent().unwrap()).unwrap();
            fs::write(&rules, "{\"schemaVersion\": 1, \"conflicts\": []}").unwrap()
        },
        &first_managed(),
        &["compat_sha256"],
    );
    let mut receipt = first_managed();
    receipt["base"]["evidence"] = json!("receipt-baseline");
    receipt["receipt"] = json!({"state":"understood","file":"kiln/eml/ownership/enshrouded.json","size":3,"sha256":"d".repeat(64)});
    step(
        "the archive receipt",
        &|| {},
        &receipt,
        &["placementReceipts_sha256"],
    );
    let mut inventory = receipt.clone();
    inventory["inventory"][1]["size"] = json!(6);
    step(
        "the archive inventory",
        &|| {},
        &inventory,
        &["loaderInventory_sha256"],
    );
    assert_ne!(base, prev);
}

#[test]
fn archive_facts_must_be_proven_or_nothing_is_written() {
    let g = kiln_game("archive", KILN);
    mixed(&g);
    let mut cases: Vec<(&str, Fake)> = Vec::new();
    let mut refused_insp = first_managed();
    refused_insp["status"] = json!("refused");
    refused_insp["input"] = Value::Null;
    refused_insp["base"] = Value::Null;
    refused_insp["reasons"] = json!([{"code":"changed","file":"enshrouded.kfc_resources","text":"changed while it was read"}]);
    cases.push(("a racy read", Fake::new(&refused_insp)));
    let mut unreadable = refused_insp.clone();
    unreadable["reasons"] =
        json!([{"code":"unreadable","file":"enshrouded.kfc","text":"permission denied"}]);
    cases.push(("an unreadable file", Fake::new(&unreadable)));
    cases.push((
        "no helper",
        Fake::raw(Err(
            "the archive inspector could not start: not found".into()
        )),
    ));
    cases.push((
        "not JSON",
        Fake::raw(Ok(b"Microsoft Windows [Version 10]\n".to_vec())),
    ));
    cases.push((
        "two documents",
        Fake::raw(Ok(
            format!("{}\n{}\n", first_managed(), first_managed()).into_bytes()
        )),
    ));
    cases.push((
        "too much output",
        Fake::raw(Ok(vec![b' '; sfi_inspection::MAX_STDOUT + 1])),
    ));
    type Edit = Box<dyn Fn(&mut Value)>;
    let mut edits: Vec<(&str, Edit)> = vec![
        (
            "another inspector version",
            Box::new(|v: &mut Value| v["inspector"]["version"] = json!("9.9.9")),
        ),
        (
            "another component",
            Box::new(|v: &mut Value| v["inspector"]["component"] = json!("kiln-eml-run")),
        ),
        (
            "schema 2",
            Box::new(|v: &mut Value| v["schema_version"] = json!(2)),
        ),
        (
            "an unknown key",
            Box::new(|v: &mut Value| v["extra"] = json!(1)),
        ),
        (
            "another framing",
            Box::new(|v: &mut Value| {
                v["proof"]["framing"] = json!("kiln-eml-inspect archive-set v2")
            }),
        ),
        (
            "the server's inspection",
            Box::new(|v: &mut Value| {
                v["target"]["side"] = json!("server");
                v["target"]["stem"] = json!("enshrouded_server");
            }),
        ),
        (
            "no executable",
            Box::new(|v: &mut Value| v["target"]["executable"] = Value::Null),
        ),
        (
            "a base digest that is not one",
            Box::new(|v: &mut Value| v["base"]["sha256"] = json!("0")),
        ),
        (
            "a vanilla base",
            Box::new(|v: &mut Value| {
                v["base"]["origin"] = json!({"token":"vanilla","label":"vanilla"})
            }),
        ),
        (
            "a label that is not the token's",
            Box::new(|v: &mut Value| v["base"]["origin"]["label"] = json!(label("unrecorded"))),
        ),
        (
            "the current set called unrecorded",
            Box::new(|v: &mut Value| {
                v["base"]["origin"] = json!({"token":"unrecorded","label":label("unrecorded")})
            }),
        ),
        (
            "a seal called first-managed",
            Box::new(|v: &mut Value| v["base"]["evidence"] = json!("sealed-baseline")),
        ),
        (
            "ok with reasons",
            Box::new(|v: &mut Value| {
                v["reasons"] = json!([{"code":"changed","file":null,"text":"x"}])
            }),
        ),
        (
            "an unknown receipt",
            Box::new(|v: &mut Value| v["receipt"]["state"] = json!("unknown")),
        ),
        // the closed schema's values, also where the plan only fingerprints them
        (
            "an unknown inventory role",
            Box::new(|v: &mut Value| v["inventory"][0]["role"] = json!("mystery")),
        ),
        (
            "an unknown note",
            Box::new(|v: &mut Value| {
                v["notes"] = json!([{"code":"mystery","file":null,"text":"x"}])
            }),
        ),
        (
            "a note with another key",
            Box::new(|v: &mut Value| {
                v["notes"] = json!([{"code":"seal-outdated","file":null,"text":"x","extra":1}])
            }),
        ),
    ];
    for (what, edit) in edits.drain(..) {
        let mut v = first_managed();
        edit(&mut v);
        cases.push((what, Fake::new(&v)));
    }
    for (what, fake) in &cases {
        let (code, reason) = refused(produce(&g, fake));
        assert_eq!(code, "no-archive-base", "{what}: {reason}");
        assert!(
            reason.contains("no launch plan was written"),
            "{what}: {reason}"
        );
        assert!(plan_files(&g).is_empty(), "{what}: a plan was written");
    }
    // a refusal names the inspector's reasons
    let (_, reason) = refused(produce(&g, &Fake::new(&refused_insp)));
    assert!(
        reason.contains("changed (enshrouded.kfc_resources): changed while it was read"),
        "{reason}"
    );
}

#[test]
fn proven_origins_are_carried_as_the_inspector_gives_them() {
    let g = kiln_game("origins", KILN);
    mixed(&g);
    for (origin, evidence) in [
        ("first-managed-unclassified", "current-set"),
        ("rebaselined-unclassified", "current-set"),
        // a receipt or seal created since keeps the origin the receipt carried: never upgraded, never lost
        ("first-managed-unclassified", "receipt-baseline"),
        ("unrecorded", "receipt-baseline"),
        ("unrecorded", "sealed-baseline"),
        ("unrecorded", "unsealed-baseline"),
    ] {
        let w = written(produce(
            &g,
            &Fake::new(&inspection(RunSide::Client, origin, evidence)),
        ));
        let v = plan_json(&g, &w);
        assert_eq!(v["game"]["archiveBaseOrigin"], origin, "{evidence}");
        assert_eq!(
            (
                v["game"]["archiveBase_sha256"].as_str(),
                v["game"]["archiveInput_sha256"].as_str()
            ),
            (Some(BASE), Some(INPUT))
        );
    }
    // the inspector's notes and every inventory role of its schema are read (closed values)
    let mut noted = inspection(RunSide::Client, "unrecorded", "receipt-baseline");
    noted["notes"] = json!([{"code":"receipt-origin-ignored","file":"kiln/eml/ownership/enshrouded.json","text":"x"},
        {"code":"unreferenced-containers","file":null,"text":"y"}]);
    noted["inventory"] = json!(
        [
            "executable",
            "header",
            "resources",
            "container",
            "retained-container",
            "container-unverified",
            "base-header",
            "base-resources",
            "base-seal",
            "receipt"
        ]
        .iter()
        .map(|r| json!({"role": r, "file": format!("{r}.bin"), "size": 1}))
        .collect::<Vec<_>>()
    );
    written(produce(&g, &Fake::new(&noted)));
    // after lost backups: the live set rebaselined over the receipt it read, note receipt-stale
    let mut stale = inspection(RunSide::Client, "rebaselined-unclassified", "current-set");
    stale["receipt"] = json!({"state":"understood","file":"kiln/eml/ownership/enshrouded.json","size":10,"sha256":EXE});
    stale["notes"] = json!([{"code":"receipt-stale","file":null,"text":"z"}]);
    let v = plan_json(&g, &written(produce(&g, &Fake::new(&stale))));
    assert_eq!(v["game"]["archiveBaseOrigin"], "rebaselined-unclassified");
    // equal digests computed for each role stay two roles (a restored install)
    let mut equal = first_managed();
    equal["input"]["sha256"] = json!(BASE);
    let v = plan_json(&g, &written(produce(&g, &Fake::new(&equal))));
    assert_eq!(
        (
            v["game"]["archiveBase_sha256"].as_str(),
            v["game"]["archiveInput_sha256"].as_str()
        ),
        (Some(BASE), Some(BASE))
    );
    // server side: the server's executable and stem
    let w = written(produce_with(
        &g,
        RunSide::Server,
        &Fake::new(&inspection(
            RunSide::Server,
            "unrecorded",
            "sealed-baseline",
        )),
        native(),
        &[],
    ));
    let v = plan_json(&g, &w);
    assert_eq!(v["game"]["side"], "server");
    assert_eq!(package(&v, "far")["decision"], "candidate");
}

/// The held cases only while this build's gate list holds the interpreter (a `kiln-sfi.interpreter` row); without its
/// row kiln is a normal choice and those starts get a plan.
#[test]
fn hosted_installed_and_held_starts_write_nothing() {
    let held = kiln_manage::sfi_release::ReleasePolicy::shipped()
        .rows()
        .unwrap()
        .iter()
        .any(|r| r.name == kiln_manage::sfi_release::INTERPRETER);
    for (tag, kiln_json, expect) in [
        (
            "hosted",
            r#"{"shroudforge": {"backend": "hosted"}}"#,
            "not-kiln",
        ),
        (
            "installed",
            r#"{"shroudforge": {"useInstalled": true}}"#,
            "not-kiln",
        ),
        ("default", "{}", "not-kiln"),
        (
            "invalid",
            r#"{"shroudforge": {"backend": "Kiln"}}"#,
            "not-kiln",
        ),
        ("held", r#"{"shroudforge": {"backend": "kiln"}}"#, "held"),
        (
            "held-other-side",
            r#"{"server": {"preview": true}, "shroudforge": {"backend": "kiln"}}"#,
            "held",
        ),
        // config.cpp never reads a "preview" after a wrong-typed key: the hold stands
        (
            "held-typed",
            r#"{"clr": "no", "preview": true, "shroudforge": {"backend": "kiln"}}"#,
            "held",
        ),
    ]
    .into_iter()
    .filter(|(_, _, expect)| held || *expect != "held")
    {
        let g = kiln_game(tag, kiln_json);
        mixed(&g);
        let before = snapshot(&g.0);
        let fake = Fake::new(&first_managed());
        match (produce(&g, &fake), expect) {
            (Outcome::NotKiln { resolution }, "not-kiln") => assert_ne!(
                resolution.selected,
                Some(sfi_backend::Backend::Kiln),
                "{tag}"
            ),
            (
                Outcome::Refused {
                    code: "interpreter-held",
                    reason,
                    ..
                },
                "held",
            ) => {
                assert!(
                    reason.contains("kiln-sfi.interpreter")
                        && reason.contains("no launch plan was written"),
                    "{tag}: {reason}"
                )
            }
            (other, _) => panic!("{tag}: {other:?}"),
        }
        assert_eq!(fake.calls.get(), 0, "{tag}: the inspector ran");
        assert_eq!(snapshot(&g.0), before, "{tag}: something was written");
    }
}

#[test]
fn nothing_below_the_shroudforge_folder_is_read() {
    for unreadable in [false, true] {
        let g = kiln_game(if unreadable { "sf-closed" } else { "sf-open" }, KILN);
        mixed(&g);
        // decoys: ShroudForge's own mods root, rules (a conflict that would skip lib) and profiles
        let sf = g.0.join("shroudforge");
        let decoy = sf.join("mods/wrong");
        fs::create_dir_all(&decoy).unwrap();
        fs::write(
            decoy.join("mod.json"),
            json!({"id":"WRONG","name":"WRONG","version":"1.0.0"}).to_string(),
        )
        .unwrap();
        fs::create_dir_all(sf.join("config/compatibility")).unwrap();
        fs::write(sf.join("config/compatibility/rules.json"), "{ not json").unwrap();
        fs::create_dir_all(sf.join("profiles")).unwrap();
        fs::write(sf.join("WRONG.dll"), "x").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt as _;
            if unreadable {
                fs::set_permissions(&sf, fs::Permissions::from_mode(0o000)).unwrap();
            }
        }
        let before = snapshot(&g.0);
        let w = written(produce(&g, &Fake::new(&first_managed())));
        let text = fs::read_to_string(plan_file(&g, &w)).unwrap();
        assert!(
            !text.contains("WRONG") && !text.contains("mods/wrong"),
            "{text}"
        );
        assert!(
            !text.contains("compatibility-rules"),
            "ShroudForge's own rules file was read"
        );
        assert_eq!(w.load_order, ["lib", "mod", "patch"]);
        let after = snapshot(&g.0);
        let new: Vec<_> = after.keys().filter(|p| !before.contains_key(*p)).collect();
        assert!(
            new.iter()
                .all(|p| p.starts_with(g.0.join("kiln/launch-plans"))),
            "{new:?}"
        );
    }
}

#[test]
fn compatibility_rules_come_from_kilns_own_file() {
    let g = kiln_game("rules", KILN);
    mixed(&g);
    let rules = g.0.join(sfi_plan::RULES_FILE);
    assert_eq!(
        sfi_plan::RULES_FILE,
        "kiln/config/sfi/compatibility/rules.json"
    );
    // missing: ShroudForge's embedded default
    let v = plan_json(&g, &written(produce(&g, &Fake::new(&first_managed()))));
    assert_eq!(
        v["fingerprints"]["compat_sha256"],
        kiln_manage::hash::sha256_hex(sfi_package::package::rules_file::EMBEDDED_RULES.as_bytes())
    );
    // a conflict rule: ShroudForge's planner skips one of the two, with its reason
    fs::create_dir_all(rules.parent().unwrap()).unwrap();
    let doc = json!({"schemaVersion":1,"conflicts":[{"first":{"id":"lib","version":"1.0.0"},"second":{"id":"patch","version":"1.0.0"},"reason":"they fight"}]});
    fs::write(&rules, doc.to_string()).unwrap();
    let v = plan_json(&g, &written(produce(&g, &Fake::new(&first_managed()))));
    let skipped: Vec<&str> = ["lib", "patch"]
        .into_iter()
        .filter(|id| package(&v, id)["decision"] == "skip")
        .collect();
    assert!(!skipped.is_empty(), "{v}");
    assert!(
        package(&v, skipped[0])["reasons"]
            .to_string()
            .contains("they fight"),
        "{v}"
    );
    assert_eq!(
        v["fingerprints"]["compat_sha256"],
        kiln_manage::hash::sha256_hex(doc.to_string().as_bytes())
    );
    // malformed: ShroudForge's result, no package of the family runs, each says why, and the plan warns
    fs::write(&rules, "{\"schemaVersion\": 2, \"conflicts\": []}").unwrap();
    let w = written(produce(&g, &Fake::new(&first_managed())));
    let v = plan_json(&g, &w);
    assert!(w.load_order.is_empty());
    for id in ["lib", "mod", "patch", "orphan", "leaf"] {
        assert_eq!(package(&v, id)["decision"], "skip", "{id}");
        assert!(
            package(&v, id)["reasons"][0]["message"]
                .as_str()
                .unwrap()
                .starts_with("compatibility rules: "),
            "{id}"
        );
    }
    assert_eq!(package(&v, "off")["decision"], "disabled");
    assert!(
        v["warnings"]
            .as_array()
            .unwrap()
            .iter()
            .any(|w| w["code"] == "compatibility-rules"),
        "{v}"
    );
    // the same reader refuses a rules path inside <game>/shroudforge (never read there)
    let read = sfi_package::package::rules_file::read_rules(
        &g.0.join("shroudforge/config/compatibility/rules.json"),
        &g.0,
    );
    assert!(matches!(
        read,
        sfi_package::package::rules_file::RulesRead::Error(_)
    ));
}

#[test]
fn folders_the_plan_cannot_describe_are_named_and_not_planned() {
    let g = kiln_game("unplanned", KILN);
    g.simple("ok", "ok", &["runtime"], json!([]));
    g.simple("dup-a", "twin", &["runtime"], json!([]));
    g.simple("dup-b", "twin", &["runtime"], json!([]));
    g.simple("api", "shroudforge-api", &["runtime"], json!([]));
    g.file("broken", "mod.json", "{");
    // a trailing dot: Windows itself would drop it, so only a case-sensitive, dot-keeping file system has such a folder
    let dotted = !cfg!(windows);
    if dotted {
        g.simple("dev.", "dev", &["runtime"], json!([]));
    }
    g.simple(
        "needs-twin",
        "needs-twin",
        &["runtime"],
        json!([{"id":"twin","version":"^1"}]),
    );
    let case_sensitive = {
        fs::create_dir_all(g.0.join("mods/Case")).unwrap();
        !g.0.join("mods/case").exists()
    };
    fs::remove_dir_all(g.0.join("mods/Case")).unwrap();
    if case_sensitive {
        g.simple("Same", "same-1", &["runtime"], json!([]));
        g.simple("same", "same-2", &["runtime"], json!([]));
        // one name to Windows, though `str::to_lowercase` writes a word-final capital sigma as ς: the strict reader's
        // name key, so the reader never refuses a plan the producer wrote
        g.simple("ΑΣ", "sigma-1", &["runtime"], json!([]));
        g.simple("Ασ", "sigma-2", &["runtime"], json!([]));
    }
    #[cfg(unix)]
    {
        g.simple("linked", "linked", &["runtime"], json!([]));
        std::os::unix::fs::symlink(
            g.dir("ok").join("mod.json"),
            g.dir("linked").join("alias.json"),
        )
        .unwrap();
        // an installed, switched-on optional dependency the plan cannot describe: its dependent does not run either
        g.simple(
            "wants-linked",
            "wants-linked",
            &["runtime"],
            json!([{"id":"linked","version":"^1","optional":true}]),
        );
    }
    let w = written(produce(&g, &Fake::new(&first_managed())));
    let v = plan_json(&g, &w);
    let ids: Vec<&str> = v["packages"]
        .as_array()
        .unwrap()
        .iter()
        .map(|p| p["id"].as_str().unwrap())
        .collect();
    assert!(
        ids.contains(&"ok") && ids.contains(&"needs-twin"),
        "{ids:?}"
    );
    for gone in [
        "twin",
        "shroudforge-api",
        "dev",
        "same-1",
        "same-2",
        "sigma-1",
        "sigma-2",
        "linked",
    ] {
        assert!(!ids.contains(&gone), "{gone} planned: {ids:?}");
    }
    let warned: Vec<String> = v["warnings"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|w| w["code"] == "package-not-planned")
        .map(|w| w["causedBy"][0].as_str().unwrap().to_owned())
        .collect();
    let mut expect = vec!["mods/api", "mods/broken", "mods/dup-a", "mods/dup-b"];
    if dotted {
        expect.push("mods/dev.");
    }
    if case_sensitive {
        expect.extend(["mods/Same", "mods/same", "mods/ΑΣ", "mods/Ασ"]);
    }
    #[cfg(unix)]
    {
        expect.push("mods/linked");
    }
    for folder in expect {
        assert!(
            warned.iter().any(|w| w == folder),
            "{folder} not warned: {warned:?}"
        );
    }
    // its dependents follow: a package needs the twin, which is not planned
    assert_eq!(package(&v, "needs-twin")["decision"], "skip");
    #[cfg(unix)]
    {
        let p = package(&v, "wants-linked");
        assert_eq!(p["decision"], "skip", "{p}");
        assert_eq!(p["reasons"][0]["causedBy"], json!(["linked"]), "{p}");
    }
    assert_eq!(w.load_order, ["ok"]);
}

#[test]
fn sf18_packages_and_their_dependents_never_run() {
    let g = kiln_game("sf18", KILN);
    g.sf(
        "needs-18",
        json!({"id":"needs-18","name":"N","version":"1.0.0"}),
        json!({"schemaVersion":1,"enabled":true,"groups":[{"label":"g","tab":true}]}),
    );
    g.simple(
        "uses-18",
        "uses-18",
        &["runtime"],
        json!([{"id":"needs-18","version":"^1"}]),
    );
    g.simple("plain", "plain", &["runtime"], json!([]));
    let w = written(produce(&g, &Fake::new(&first_managed())));
    let v = plan_json(&g, &w);
    let p = package(&v, "needs-18");
    assert_eq!(
        (p["decision"].as_str(), p["requires180"].as_bool()),
        (Some("skip"), Some(true))
    );
    assert_eq!(p["reasons"][0]["code"], "requires-1.8");
    assert_eq!(package(&v, "uses-18")["decision"], "skip");
    assert_eq!(w.load_order, ["plain"]);
    // the strict reader holds any 1.8 candidate on its own (independent release policy)
    let mut forged = v.clone();
    if let Some(p) = forged["packages"]
        .as_array_mut()
        .unwrap()
        .iter_mut()
        .find(|p| p["id"] == "needs-18")
    {
        p["decision"] = json!("candidate");
        p["reasons"] = json!([]);
        p["enabled"] = json!(true);
        p["lua"] = json!({"required":false,"admission":"not-needed"});
    }
    forged["loadOrder"] = json!(["needs-18", "plain"]);
    let err = sfi_plan::validate(&serde_json::to_vec(&forged).unwrap(), EXE, RunSide::Client)
        .unwrap_err();
    assert!(err.contains("1.8"), "{err}");
}

fn put(b: &mut [u8], at: usize, value: u64, n: usize) {
    b[at..at + n].copy_from_slice(&value.to_le_bytes()[..n]);
}
/// A synthetic PE32+ DLL with no imports (as tests/sfi_scan_native.rs builds them).
fn pe() -> Vec<u8> {
    let mut b = vec![0u8; 0x1000];
    b[..2].copy_from_slice(b"MZ");
    put(&mut b, 0x3c, 0x80, 4);
    b[0x80..0x84].copy_from_slice(b"PE\0\0");
    put(&mut b, 0x84, 0x8664, 2);
    put(&mut b, 0x86, 1, 2);
    put(&mut b, 0x94, 240, 2);
    put(&mut b, 0x96, 0x2022, 2);
    put(&mut b, 0x98, 0x20b, 2);
    put(&mut b, 0x98 + 24, 0x180000000, 8);
    put(&mut b, 0x98 + 56, 0x2000, 4);
    put(&mut b, 0x98 + 60, 0x200, 4);
    put(&mut b, 0x98 + 68, 3, 2);
    put(&mut b, 0x98 + 108, 16, 4);
    b[0x188..0x18d].copy_from_slice(b".test");
    put(&mut b, 0x188 + 8, 0xe00, 4);
    put(&mut b, 0x188 + 12, 0x1000, 4);
    put(&mut b, 0x188 + 16, 0xe00, 4);
    put(&mut b, 0x188 + 20, 0x200, 4);
    b
}

#[test]
fn a_candidates_dlls_are_pinned_and_native_policy_never_skips_them() {
    let g = kiln_game("native", KILN);
    g.simple("nat", "nat", &["runtime"], json!([]));
    g.file(
        "nat",
        "native-plugin.ini",
        "[Plugin]\nEnabled=true\nDll=entry.dll\n",
    );
    let dll = pe();
    g.file("nat", "entry.dll", &dll);
    g.simple(
        "over",
        "over",
        &["runtime"],
        json!([{"id":"nat","version":"^1"}]),
    );
    let w = written(produce(&g, &Fake::new(&first_managed())));
    let v = plan_json(&g, &w);
    let p = package(&v, "nat");
    assert_eq!(p["decision"], "candidate", "{p}");
    let rec = &p["dlls"][0];
    assert_eq!(
        (
            rec["path"].as_str(),
            rec["sha256"].as_str(),
            rec["gate"].as_str(),
            rec["required"].as_bool(),
            rec["bridgeAbi"].as_u64(),
            rec["source"].as_str()
        ),
        (
            Some("entry.dll"),
            Some(kiln_manage::hash::sha256_hex(&dll).as_str()),
            Some("pass"),
            Some(true),
            Some(0),
            Some("ini")
        )
    );
    assert_eq!(w.load_order, ["nat", "over"]);
    // native.policy never (top level or this side's section): the package and its dependent are skipped
    for kiln_json in [
        r#"{"preview": true, "native": {"policy": "never"}, "shroudforge": {"backend": "kiln"}}"#,
        r#"{"preview": true, "client": {"native": {"policy": "never"}}, "shroudforge": {"backend": "kiln"}}"#,
    ] {
        fs::write(g.0.join("kiln/kiln.json"), kiln_json).unwrap();
        let w = written(produce(&g, &Fake::new(&first_managed())));
        let v = plan_json(&g, &w);
        assert_eq!(package(&v, "nat")["decision"], "skip", "{kiln_json}");
        assert_eq!(package(&v, "nat")["dlls"], json!([]));
        assert_eq!(package(&v, "over")["decision"], "skip");
        assert!(w.load_order.is_empty());
    }
}

#[test]
fn native_policy_is_read_as_config_cpp_reads_it() {
    use sfi_policy::native_selection::NativePolicy;
    let policy = |text: &str, side| {
        sfi_plan::native_permission(Some(&serde_json::from_str::<Value>(text).unwrap()), side)
            .policy
    };
    assert_eq!(
        sfi_plan::native_permission(None, RunSide::Client).policy,
        NativePolicy::Always
    );
    assert_eq!(policy("{}", RunSide::Client), NativePolicy::Always);
    assert_eq!(
        policy(r#"{"native": {"policy": "never"}}"#, RunSide::Client),
        NativePolicy::Never
    );
    assert_eq!(
        policy(r#"{"native": {"policy": "bogus"}}"#, RunSide::Client),
        NativePolicy::Always,
        "an unknown value keeps the default"
    );
    assert_eq!(
        policy(
            r#"{"native": {"policy": "never"}, "server": {"native": {"policy": "always"}}}"#,
            RunSide::Server
        ),
        NativePolicy::Always
    );
    assert_eq!(
        policy(
            r#"{"native": {"policy": "never"}, "server": {"native": {"policy": "always"}}}"#,
            RunSide::Client
        ),
        NativePolicy::Never
    );
    assert_eq!(
        policy(
            r#"{"clr": 1, "native": {"policy": "never"}}"#,
            RunSide::Client
        ),
        NativePolicy::Always,
        "a wrong type stops the top level"
    );
    assert_eq!(
        policy(
            r#"{"client": {"plugins": "x", "native": {"policy": "never"}}}"#,
            RunSide::Client
        ),
        NativePolicy::Always
    );
    let verified = sfi_plan::native_permission(
        Some(&json!({"native": {"policy": "verified-only"}})),
        RunSide::Client,
    );
    assert_eq!(
        (verified.policy, verified.verified),
        (NativePolicy::VerifiedOnly, true),
        "the core decides verified at boot"
    );
}

#[test]
fn carried_launcher_findings_become_plan_warnings_once() {
    let g = kiln_game("carried", KILN);
    g.simple("plain", "plain", &["runtime"], json!([]));
    let off = Reason {
        code: "save-sensitive-off".into(),
        message: "ammo-slots was on at the last activation and is off now; do not save".into(),
        caused_by: vec!["ammo-slots".into()],
        severity: "save-warning",
    };
    let carried = [off.clone(), off.clone()];
    let w = written(produce_with(
        &g,
        RunSide::Client,
        &Fake::new(&first_managed()),
        native(),
        &carried,
    ));
    let bytes = fs::read(plan_file(&g, &w)).unwrap();
    let plan = LaunchPlan::parse(&bytes, &context()).unwrap();
    let warnings = plan.warnings();
    let mine: Vec<_> = warnings
        .iter()
        .filter(|w| w.code == "save-sensitive-off")
        .collect();
    assert_eq!(mine.len(), 1, "{warnings:?}");
    assert_eq!(
        (mine[0].severity, mine[0].caused_by.as_slice()),
        (Severity::SaveWarning, ["ammo-slots"].as_slice())
    );
}

#[test]
fn the_profile_projection_leaves_out_the_reference_and_write_stamps() {
    let base = json!({"format":1,"id":"p1","generation":7,"dropins":true,"mods":{"shroudforge":[{"id":"a","path":"x"}]}});
    let h = sfi_plan::semantic_sha256(&base).unwrap();
    let mut same = base.clone();
    same["sfLaunchPlan"] = json!({"path":"launch-plans/x.json","sha256":"0".repeat(64)});
    same["written"] = json!("2026-11-20T00:00:00Z");
    same["writer"] = json!("kiln-launcher 9");
    same["$schema"] = json!("profile.schema.json");
    assert_eq!(sfi_plan::semantic_sha256(&same).unwrap(), h);
    // key order does not matter; any execution member does
    let reordered: Value = serde_json::from_str(r#"{"mods":{"shroudforge":[{"path":"x","id":"a"}]},"dropins":true,"generation":7,"id":"p1","format":1}"#).unwrap();
    assert_eq!(sfi_plan::semantic_sha256(&reordered).unwrap(), h);
    for (key, value) in [
        ("generation", json!(8)),
        ("dropins", json!(false)),
        ("id", json!("p2")),
        ("mods", json!({})),
    ] {
        let mut v = base.clone();
        v[key] = value;
        assert_ne!(sfi_plan::semantic_sha256(&v).unwrap(), h, "{key}");
    }
    assert!(ProfileIdentity::of(&json!({"generation":1}), Mode::Once).is_err());
}

/// schema/fixtures/sfi/launch-plan, or SFI_LAUNCH_PLAN_VECTORS where a test binary runs away from the source tree (the
/// native-Windows runner).
fn vectors() -> PathBuf {
    std::env::var_os("SFI_LAUNCH_PLAN_VECTORS").map_or_else(
        || Path::new(env!("CARGO_MANIFEST_DIR")).join("../../schema/fixtures/sfi/launch-plan"),
        PathBuf::from,
    )
}

/// The shared vectors (schema/fixtures/sfi/launch-plan) go through the producer's validation as through sfi-policy's
/// strict reader: every accept vector both take, every refuse vector both refuse.
#[test]
fn the_shared_vectors_pass_both_validators() {
    let root = vectors();
    let mut seen = 0;
    for (folder, accept) in [("accept", true), ("refuse", false)] {
        let mut files: Vec<PathBuf> = fs::read_dir(root.join(folder))
            .unwrap()
            .map(|e| e.unwrap().path())
            .collect();
        files.sort();
        for file in files {
            let bytes = fs::read(&file).unwrap();
            let ours = sfi_plan::validate(&bytes, EXE, RunSide::Client);
            let strict = LaunchPlan::parse(&bytes, &context());
            assert_eq!(
                (ours.is_ok(), strict.is_ok()),
                (accept, accept),
                "{}: {:?} / {:?}",
                file.display(),
                ours.err(),
                strict.err()
            );
            seen += 1;
        }
    }
    assert!(seen >= 10, "{seen}");
}

/// The accept vector `produced.json` is the producer's own output for a synthetic game (the volatile hashes, the writer
/// version and the id fixed), so the strict reader (sfi-policy's vector test) and the core read what kiln-manage
/// writes. SFI_UPDATE_VECTORS=1 rewrites it.
#[test]
fn the_produced_vector_is_what_the_producer_writes() {
    let g = kiln_game("vector", KILN);
    mixed(&g);
    let fake = Fake::new(&first_managed());
    let p = ProfileIdentity {
        id: "vector".into(),
        generation: 1,
        mode: Mode::Active,
        semantic_sha256: "e".repeat(64),
    };
    let policy = policy();
    let off = [Reason {
        code: "save-sensitive-off".into(),
        message: "off since the last activation; do not save".into(),
        caused_by: vec!["gone".into()],
        severity: "save-warning",
    }];
    let req = Request {
        game: &g.0,
        side: RunSide::Client,
        env: &Env::default(),
        profile: &p,
        inspector: &fake,
        native: native(),
        policy: &policy,
        catalog: CatalogInput::Missing,
        carried: &off,
    };
    let resolution = sfi_backend::resolve_game(&g.0, RunSide::Client, &Env::default());
    let (mut draft, _) = sfi_plan::draft(&req, &resolution).unwrap();
    draft.writer_version = "0.0.0-vector".into();
    for f in draft.fingerprints.iter_mut() {
        f.1 = "f".repeat(64);
    }
    draft.contract = ["f".repeat(64), "f".repeat(64), "f".repeat(64)];
    let bytes = sfi_plan::render(&draft, "00000000-0000-4000-8000-000000000001");
    sfi_plan::validate(&bytes, EXE, RunSide::Client).unwrap();
    let file = vectors().join("accept/produced.json");
    if std::env::var_os("SFI_UPDATE_VECTORS").is_some() {
        fs::write(&file, &bytes).unwrap();
    }
    assert_eq!(
        String::from_utf8(fs::read(&file).unwrap()).unwrap(),
        String::from_utf8(bytes).unwrap(),
        "accept/produced.json is not what the producer writes now (SFI_UPDATE_VECTORS=1 rewrites it)"
    );
}

#[test]
fn a_plan_is_published_whole_and_never_over_another() {
    let g = kiln_game("write", KILN);
    let id = "11111111-2222-4333-8444-555555555555";
    let first = sfi_plan::write_plan(&g.0, id, b"{\"first\": 1}\n")
        .unwrap()
        .unwrap();
    assert_eq!(first.path, format!("launch-plans/{id}.json"));
    assert_eq!(
        first.sha256,
        kiln_manage::hash::sha256_hex(b"{\"first\": 1}\n")
    );
    // the name is taken: nothing replaced, no temporary file left
    assert_eq!(
        sfi_plan::write_plan(&g.0, id, b"{\"second\": 2}\n").unwrap(),
        None
    );
    assert_eq!(
        fs::read(g.0.join("kiln").join(&first.path)).unwrap(),
        b"{\"first\": 1}\n"
    );
    assert_eq!(plan_files(&g), [format!("{id}.json")]);
    // while a reader holds a plan open (the core's and kiln-sfi's open: shared for reading only on Windows), a new plan
    // is written beside it and the held one is never replaced
    let held = digest::open_plain_file(&g.0.join("kiln").join(&first.path)).unwrap();
    let other = "11111111-2222-4333-8444-666666666666";
    assert!(
        sfi_plan::write_plan(&g.0, other, b"{\"other\": 1}\n")
            .unwrap()
            .is_some()
    );
    assert_eq!(
        sfi_plan::write_plan(&g.0, id, b"{\"third\": 3}\n").unwrap(),
        None
    );
    drop(held);
    assert_eq!(
        fs::read(g.0.join("kiln").join(&first.path)).unwrap(),
        b"{\"first\": 1}\n"
    );
    assert_eq!(plan_files(&g).len(), 2);
    // a stale temporary file of an interrupted write is never a plan: readers open <id>.json by name only
    fs::write(
        g.0.join(format!(
            "kiln/launch-plans/.{other}.json.0011223344556677.kiln-tmp"
        )),
        "partial",
    )
    .unwrap();
    assert!(digest::open_plain_file(&g.0.join(format!("kiln/launch-plans/{other}.json"))).is_ok());
    // no kiln folder, or a plan folder that is a link: refused
    let bare = Game::new("plan-bare");
    assert!(sfi_plan::write_plan(&bare.0, id, b"{}").is_err());
    #[cfg(unix)]
    {
        let linked = kiln_game("write-link", KILN);
        let elsewhere = linked.0.join("elsewhere");
        fs::create_dir_all(&elsewhere).unwrap();
        std::os::unix::fs::symlink(&elsewhere, linked.0.join("kiln/launch-plans")).unwrap();
        assert!(sfi_plan::write_plan(&linked.0, id, b"{}").is_err());
        assert!(
            fs::read_dir(&elsewhere).unwrap().next().is_none(),
            "nothing written through the link"
        );
    }
}

#[test]
fn unreadable_inputs_write_no_plan() {
    let g = kiln_game("unreadable", KILN);
    mixed(&g);
    // a chain file that is a folder cannot be read
    fs::create_dir_all(g.0.join("kiln/chain.ini")).unwrap();
    let (code, reason) = refused(produce(&g, &Fake::new(&first_managed())));
    assert_eq!(code, "input-unreadable", "{reason}");
    assert!(reason.contains("kiln/chain.ini"), "{reason}");
    fs::remove_dir_all(g.0.join("kiln/chain.ini")).unwrap();
    // a mods entry that is not a plain folder
    let g2 = kiln_game("mods-file", KILN);
    fs::remove_dir_all(g2.0.join("mods")).unwrap();
    fs::write(g2.0.join("mods"), "x").unwrap();
    assert_eq!(
        refused(produce(&g2, &Fake::new(&first_managed()))).0,
        "input-unreadable"
    );
    // no kiln folder at all
    let g3 = Game::new("plan-nokiln");
    assert_eq!(
        kind(&produce(&g3, &Fake::new(&first_managed()))),
        "not-kiln"
    );
    for g in [&g, &g2] {
        assert!(plan_files(g).is_empty());
    }
}

/// A plan is written only for a start Kiln's interpreter can run: without kiln/plugins/kiln-sfi.dll, or with
/// KILN_SHROUDFORGE=0, it is refused before anything is read (no inspection, nothing written), with its own reason.
#[test]
fn no_plan_for_an_interpreter_that_cannot_run() {
    let g = kiln_game("cannot-run", KILN);
    mixed(&g);
    fs::remove_file(g.0.join("kiln/plugins/kiln-sfi.dll")).unwrap();
    let before = snapshot(&g.0);
    let fake = Fake::new(&first_managed());
    let (code, reason) = refused(produce(&g, &fake));
    assert_eq!(code, "interpreter-missing", "{reason}");
    assert!(
        reason.contains("kiln/plugins/kiln-sfi.dll")
            && reason.contains("no launch plan was written"),
        "{reason}"
    );
    assert_eq!(snapshot(&g.0), before, "nothing was written");
    fs::write(g.0.join("kiln/plugins/kiln-sfi.dll"), b"interpreter").unwrap();
    let before = snapshot(&g.0);
    let p = profile();
    let policy = policy();
    let off = Env {
        shroudforge: Some("0".into()),
        runtime: None,
    };
    let (code, reason) = refused(sfi_plan::produce(Request {
        game: &g.0,
        side: RunSide::Client,
        env: &off,
        profile: &p,
        inspector: &fake,
        native: native(),
        policy: &policy,
        catalog: CatalogInput::Missing,
        carried: &[],
    }));
    assert_eq!(code, "interpreter-off", "{reason}");
    assert!(reason.contains("KILN_SHROUDFORGE=0"), "{reason}");
    assert_eq!(fake.calls.get(), 0, "nothing was inspected");
    assert_eq!(snapshot(&g.0), before, "nothing was written");
    // the same resolution handed in (sfi_plan::produce_resolved, as an activation does) decides the same
    let selection = sfi_backend::resolve_game(&g.0, RunSide::Client, &off);
    let o = sfi_plan::produce_resolved(
        Request {
            game: &g.0,
            side: RunSide::Client,
            env: &off,
            profile: &p,
            inspector: &fake,
            native: native(),
            policy: &policy,
            catalog: CatalogInput::Missing,
            carried: &[],
        },
        selection,
        &mut |_, _| panic!("nothing is written"),
    );
    assert_eq!(refused(o).0, "interpreter-off");
}

fn kind(o: &Outcome) -> &'static str {
    match o {
        Outcome::NotKiln { .. } => "not-kiln",
        Outcome::Refused { .. } => "refused",
        Outcome::Written(_) => "written",
    }
}

/// The shipped helper's checks before anything runs, and the process boundary (Unix: a shell script stands in for the
/// helper; it records its argv).
#[cfg(unix)]
#[test]
fn the_helper_is_checked_and_run_with_an_exact_argv() {
    use std::os::unix::fs::PermissionsExt as _;
    let g = kiln_game("tool", KILN);
    let dir = g.0.join("tools");
    fs::create_dir_all(&dir).unwrap();
    let script = |name: &str, body: &str| -> (PathBuf, String) {
        let p = dir.join(name);
        fs::write(&p, format!("#!/bin/sh\n{body}\n")).unwrap();
        fs::set_permissions(&p, fs::Permissions::from_mode(0o755)).unwrap();
        let sha = kiln_manage::hash::sha256_file(&p).unwrap().0;
        (p, sha)
    };
    let args = dir.join("args.txt");
    let out = first_managed().to_string().replace('\'', "");
    let (exe, sha) = script(
        "kiln-eml-inspect",
        &format!(
            "for a in \"$@\"; do echo \"$a\" >> '{}'; done\nprintf '%s\\n' '{out}'",
            args.display()
        ),
    );
    let tool = sfi_inspection::Tool {
        exe: exe.clone(),
        sha256: sha.clone(),
        version: VERSION.into(),
    };
    let facts = sfi_inspection::inspect(&tool, &g.0, RunSide::Client).unwrap();
    assert_eq!(
        (facts.origin, facts.base_sha256.as_str()),
        ("first-managed-unclassified", BASE)
    );
    let argv: Vec<String> = fs::read_to_string(&args)
        .unwrap()
        .lines()
        .map(str::to_owned)
        .collect();
    assert_eq!(
        argv,
        ["--game", &g.0.display().to_string(), "--side", "client"]
    );
    // the shipped helper only: its exact name, its SHA-256, an absolute path, no link
    let (run, run_sha) = script("kiln-eml-run", "echo '{}'");
    for (what, t) in [
        (
            "another program",
            sfi_inspection::Tool {
                exe: run,
                sha256: run_sha,
                version: VERSION.into(),
            },
        ),
        (
            "another hash",
            sfi_inspection::Tool {
                exe: exe.clone(),
                sha256: "0".repeat(64),
                version: VERSION.into(),
            },
        ),
        (
            "a relative path",
            sfi_inspection::Tool {
                exe: PathBuf::from("tools/kiln-eml-inspect"),
                sha256: sha.clone(),
                version: VERSION.into(),
            },
        ),
    ] {
        let err = t.run(&g.0, RunSide::Client).unwrap_err();
        assert!(err.contains("archive inspector"), "{what}: {err}");
    }
    let link_dir = dir.join("link");
    fs::create_dir_all(&link_dir).unwrap();
    std::os::unix::fs::symlink(&exe, link_dir.join("kiln-eml-inspect")).unwrap();
    assert!(
        sfi_inspection::Tool {
            exe: link_dir.join("kiln-eml-inspect"),
            sha256: sha.clone(),
            version: VERSION.into()
        }
        .run(&g.0, RunSide::Client)
        .is_err(),
        "a link"
    );
    // a failing helper, and one that prints too much
    let fail_dir = dir.join("fail");
    fs::create_dir_all(&fail_dir).unwrap();
    let p = fail_dir.join("kiln-eml-inspect");
    fs::write(&p, "#!/bin/sh\necho 'no game here' >&2\nexit 3\n").unwrap();
    fs::set_permissions(&p, fs::Permissions::from_mode(0o755)).unwrap();
    let t = sfi_inspection::Tool {
        exe: p.clone(),
        sha256: kiln_manage::hash::sha256_file(&p).unwrap().0,
        version: VERSION.into(),
    };
    let err = t.run(&g.0, RunSide::Client).unwrap_err();
    assert!(err.contains("no game here"), "{err}");
    fs::write(
        &p,
        format!(
            "#!/bin/sh\nhead -c {} /dev/zero\n",
            sfi_inspection::MAX_STDOUT + 1
        ),
    )
    .unwrap();
    let t = sfi_inspection::Tool {
        exe: p.clone(),
        sha256: kiln_manage::hash::sha256_file(&p).unwrap().0,
        version: VERSION.into(),
    };
    let err = t.run(&g.0, RunSide::Client).unwrap_err();
    assert!(err.contains("more than"), "{err}");
}

/// Windows: the helper is started while its checked handle is held (shared for reading only), and an answer that is
/// that fails is refused (a copy of whoami.exe stands in for the helper: it rejects the arguments and exits 1).
#[cfg(windows)]
#[test]
fn the_helper_starts_while_its_checked_handle_is_held() {
    let g = kiln_game("tool-win", KILN);
    let dir = g.0.join("tools");
    fs::create_dir_all(&dir).unwrap();
    let exe = dir.join("kiln-eml-inspect.exe");
    let system = std::env::var_os("SystemRoot")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(r"C:\Windows"));
    fs::copy(system.join("System32").join("whoami.exe"), &exe).unwrap();
    let sha = kiln_manage::hash::sha256_file(&exe).unwrap().0;
    let tool = sfi_inspection::Tool {
        exe: exe.clone(),
        sha256: sha.clone(),
        version: VERSION.into(),
    };
    // whoami rejects the arguments: a failing helper, started (the spawn worked while the handle was held); between the
    // check and the start nobody can replace, write or delete the file
    let decoy = dir.join("decoy.exe");
    fs::copy(system.join("System32").join("hostname.exe"), &decoy).unwrap();
    let mut tried = false;
    let err = tool
        .run_with(
            &g.0,
            RunSide::Client,
            std::time::Duration::from_secs(120),
            &mut || {
                tried = true;
                assert!(fs::rename(&decoy, &exe).is_err(), "replaced while held");
                assert!(
                    fs::OpenOptions::new().write(true).open(&exe).is_err(),
                    "opened for writing while held"
                );
                assert!(fs::remove_file(&exe).is_err(), "deleted while held");
            },
        )
        .unwrap_err();
    assert!(tried);
    assert!(err.contains("failed"), "{err}");
    assert_eq!(kiln_manage::hash::sha256_file(&exe).unwrap().0, sha);
    let wrong = sfi_inspection::Tool {
        exe: dir.join("kiln-eml-run.exe"),
        sha256: sha,
        version: VERSION.into(),
    };
    assert!(
        wrong
            .run(&g.0, RunSide::Client)
            .unwrap_err()
            .contains("is not Kiln's")
    );
    assert_eq!(refused(produce(&g, &tool)).0, "no-archive-base");
}

/// A package folder the pre-select inventory could not open, or whose name is not Unicode, is not dropped from the
/// plan without a word: it is named in a warning like any folder the plan cannot describe, and nothing of it runs.
#[cfg(unix)]
#[test]
fn a_folder_the_inventory_cannot_open_is_named_not_dropped() {
    use std::os::unix::fs::PermissionsExt as _;
    let g = kiln_game("closed-folder", KILN);
    mixed(&g);
    g.simple("locked", "locked", &["runtime"], json!([]));
    fs::set_permissions(g.dir("locked"), fs::Permissions::from_mode(0o000)).unwrap();
    #[cfg(target_os = "linux")]
    {
        use std::os::unix::ffi::OsStrExt as _;
        let odd =
            g.0.join("mods")
                .join(std::ffi::OsStr::from_bytes(b"odd\xff"));
        fs::create_dir_all(&odd).unwrap();
        fs::write(
            odd.join("mod.json"),
            r#"{"id":"odd","name":"odd","version":"1.0.0"}"#,
        )
        .unwrap();
    }
    let w = written(produce(&g, &Fake::new(&first_managed())));
    fs::set_permissions(g.dir("locked"), fs::Permissions::from_mode(0o755)).unwrap();
    let v = plan_json(&g, &w);
    let named = |folder: &str| {
        v["warnings"].as_array().unwrap().iter().any(|r| {
            r["code"] == "package-not-planned"
                && r["causedBy"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .any(|c| c.as_str() == Some(folder))
        })
    };
    assert!(named("mods/locked"), "{}", v["warnings"]);
    #[cfg(target_os = "linux")]
    assert!(named("mods/odd\u{fffd}"), "{}", v["warnings"]);
    assert_eq!(w.load_order, ["lib", "mod", "patch"], "the rest still runs");
    assert!(!w.decisions.contains_key("locked"));
}

/// A mods folder that is a link or junction is refused with a reason that says so.
#[cfg(unix)]
#[test]
fn a_linked_mods_folder_is_refused_and_says_why() {
    let g = kiln_game("mods-link", KILN);
    let real = g.0.join("real-mods");
    fs::create_dir_all(&real).unwrap();
    fs::remove_dir_all(g.0.join("mods")).unwrap();
    std::os::unix::fs::symlink(&real, g.0.join("mods")).unwrap();
    let (code, reason) = refused(produce(&g, &Fake::new(&first_managed())));
    assert_eq!(code, "input-unreadable", "{reason}");
    assert!(reason.contains("link or junction"), "{reason}");
    assert!(plan_files(&g).is_empty());
}

/// On Linux, the helper started is the very file that was hashed. A file renamed over its path after the
/// check (here: a script that prints "swapped") starts nothing; the hashed ELF (a copy of echo) runs, through its handle.
#[cfg(target_os = "linux")]
#[test]
fn the_helper_started_is_the_file_hashed_not_one_swapped_in() {
    use std::os::unix::fs::PermissionsExt as _;
    let g = kiln_game("tool-swap", KILN);
    let dir = g.0.join("tools");
    fs::create_dir_all(&dir).unwrap();
    let exe = dir.join("kiln-eml-inspect");
    fs::copy("/usr/bin/echo", &exe).unwrap();
    assert_eq!(
        &fs::read(&exe).unwrap()[..4],
        b"\x7fELF",
        "echo is an ELF file here"
    );
    let sha = kiln_manage::hash::sha256_file(&exe).unwrap().0;
    let swap = dir.join("swap");
    fs::write(&swap, "#!/bin/sh\necho swapped\n").unwrap();
    fs::set_permissions(&swap, fs::Permissions::from_mode(0o755)).unwrap();
    let tool = sfi_inspection::Tool {
        exe: exe.clone(),
        sha256: sha,
        version: VERSION.into(),
    };
    let out = tool
        .run_with(
            &g.0,
            RunSide::Client,
            std::time::Duration::from_secs(60),
            &mut || fs::rename(&swap, &exe).unwrap(),
        )
        .unwrap();
    assert_eq!(
        fs::read_to_string(&exe).unwrap(),
        "#!/bin/sh\necho swapped\n",
        "the swap happened"
    );
    assert_eq!(
        String::from_utf8(out).unwrap(),
        format!("--game {} --side client\n", g.0.display())
    );
}

/// A helper over its time is stopped and reaped, and its output is not waited for when a process it
/// started still holds it; a helper that ends while such a process holds its output gives no answer either.
#[cfg(unix)]
#[test]
fn a_helper_over_its_time_is_stopped_reaped_and_never_waited_for() {
    use std::os::unix::fs::PermissionsExt as _;
    use std::time::{Duration, Instant};
    let g = kiln_game("tool-time", KILN);
    let pid_file = g.0.join("pid");
    for (tag, body, limit) in [
        // the shell waits on a sleep that holds its stdout
        (
            "slow",
            format!("echo $$ > '{}'\nsleep 5\n", pid_file.display()),
            Duration::from_millis(300),
        ),
        // the shell ends at once, a sleep it left behind holds its stdout
        (
            "left",
            "(sleep 5) &\necho '{}'\nexit 0\n".to_owned(),
            Duration::from_millis(800),
        ),
    ] {
        let dir = g.0.join(tag);
        fs::create_dir_all(&dir).unwrap();
        let exe = dir.join("kiln-eml-inspect");
        fs::write(&exe, format!("#!/bin/sh\n{body}")).unwrap();
        fs::set_permissions(&exe, fs::Permissions::from_mode(0o755)).unwrap();
        let tool = sfi_inspection::Tool {
            exe: exe.clone(),
            sha256: kiln_manage::hash::sha256_file(&exe).unwrap().0,
            version: VERSION.into(),
        };
        let started = Instant::now();
        let err = tool
            .run_with(&g.0, RunSide::Client, limit, &mut || {})
            .unwrap_err();
        let took = started.elapsed();
        assert!(
            took < Duration::from_secs(3),
            "{tag}: waited {took:?}: {err}"
        );
        match tag {
            "slow" => {
                assert!(err.contains("took longer than"), "{tag}: {err}");
                let pid: i32 = fs::read_to_string(&pid_file)
                    .unwrap()
                    .trim()
                    .parse()
                    .unwrap();
                // SAFETY: signal 0 only asks whether the process exists
                let alive = unsafe { libc::kill(pid, 0) } == 0;
                assert!(
                    !alive && std::io::Error::last_os_error().raw_os_error() == Some(libc::ESRCH),
                    "{tag}: the stopped helper {pid} is still there (not killed or not reaped)"
                );
            }
            _ => assert!(err.contains("still open"), "{tag}: {err}"),
        }
    }
}
