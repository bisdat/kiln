// SPDX-License-Identifier: GPL-3.0-or-later
//! The admission set of a kiln-mode start (sf_startup::admission) and the retry's order
//! (sf_startup::plan::StartupPlan::fork_order / retry_without), on synthetic plans and discovered packages.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;
#[path = "support/m6_startup/mod.rs"]
mod startup_support;

use kiln_eml::{
    sf_pass::StartupDialect,
    sf_startup::{
        admission::admit,
        plan::{Features, StartupPlan},
    },
    startup::{NativeFacts, PackageKind, PlanDependency, PlanFacts, PlanPackage, Refusal},
};
use sfi_core::host::{Side, SourceVersion};
use startup_support::{Catalog, Start, content, native};
use std::{collections::BTreeMap, path::Path, rc::Rc};

fn package(id: &str, requires: &[&str], optional: &[&str], native: bool) -> PlanPackage {
    let mut dependencies: Vec<PlanDependency> = requires
        .iter()
        .map(|r| PlanDependency {
            id: (*r).into(),
            optional: false,
            included: true,
        })
        .collect();
    dependencies.extend(optional.iter().map(|o| PlanDependency {
        id: (*o).into(),
        optional: true,
        included: true,
    }));
    PlanPackage {
        id: id.into(),
        version: "1.0.0".into(),
        kind: PackageKind::Shroudforge,
        root: format!("mods/{id}"),
        content_sha256: [id.len() as u8; 32],
        config_sha256: [0; 32],
        candidate: true,
        data_required: true,
        native,
        dependencies,
    }
}
fn plan(packages: Vec<PlanPackage>, dialect: StartupDialect) -> PlanFacts {
    PlanFacts {
        plan_id: [1; 16],
        dialect,
        exe_sha256: "00".repeat(32),
        archive_base_sha256: "00".repeat(32),
        archive_input_sha256: "00".repeat(32),
        archive_base_origin: None,
        load_order: packages.iter().map(|p| p.id.clone()).collect(),
        packages,
    }
}
/// Every folder's digest is the plan's, unless it is in `changed` (another digest) or `unreadable`.
fn digest<'a>(changed: &'a [&str], unreadable: &'a [&str]) -> impl Fn(&Path) -> Result<[u8; 32], String> + 'a {
    move |root: &Path| {
        let id = root.file_name().unwrap().to_string_lossy().into_owned();
        if unreadable.contains(&id.as_str()) {
            return Err("cannot be read".into());
        }
        Ok([id.len() as u8 ^ u8::from(changed.contains(&id.as_str())); 32])
    }
}
fn excluded(a: &kiln_eml::sf_startup::admission::Admission) -> BTreeMap<&str, (&str, Vec<&str>)> {
    a.excluded
        .iter()
        .map(|(id, r)| {
            (
                id.as_str(),
                (r.code.as_str(), r.chain.iter().map(String::as_str).collect()),
            )
        })
        .collect()
}

// The terminal's withdrawal leaves the package out (native-refused, its text and chain), and what needs it, through
// another package too, with the chain nearest first; an optional dependency on it is skipped (the package stays).
#[test]
fn a_withdrawn_package_and_what_needs_it_are_left_out_nearest_first() {
    let p = plan(
        vec![
            package("dll", &[], &[], true),
            package("needs", &["dll"], &[], false),
            package("needs-needs", &["needs"], &[], false),
            package("optional", &[], &["dll"], false),
            package("free", &[], &[], false),
        ],
        StartupDialect::Shroudforge,
    );
    let mut facts = native();
    facts.withdrawn.insert(
        "dll".into(),
        Refusal {
            code: "dll-faulted".into(),
            text: "dll faulted".into(),
            chain: vec![],
        },
    );
    let a = admit(&p, &facts, &digest(&[], &[]), Path::new("/game"), None, "enshrouded");
    assert_eq!(
        excluded(&a),
        BTreeMap::from([
            ("dll", ("native-refused", vec![])),
            ("needs", ("native-refused", vec!["dll"])),
            ("needs-needs", ("native-refused", vec!["needs", "dll"])),
        ])
    );
    assert_eq!(a.excluded["dll"].text, "dll faulted");
    assert!(
        a.excluded["needs"].text.starts_with("needs dll, "),
        "{}",
        a.excluded["needs"].text
    );
    assert_eq!(a.order, ["optional", "free"]);
    // the closure does not rely on the plan's dependencies-first order
    let mut reversed = p.clone();
    reversed.load_order.reverse();
    let b = admit(
        &reversed,
        &facts,
        &digest(&[], &[]),
        Path::new("/game"),
        None,
        "enshrouded",
    );
    assert_eq!(excluded(&b), excluded(&a));
}

// A Lua-only package the terminal withdrew (not a pass member) still takes the pass members that need it out.
#[test]
fn a_withdrawn_package_outside_the_pass_still_takes_its_dependents_out() {
    let mut lib = package("lib", &[], &[], false);
    lib.kind = PackageKind::NativeSelection;
    let p = plan(
        vec![lib, package("user", &["lib"], &[], false)],
        StartupDialect::Shroudforge,
    );
    let mut facts = native();
    facts.withdrawn.insert(
        "lib".into(),
        Refusal {
            code: "dependency-withdrawn".into(),
            text: "lib out".into(),
            chain: vec!["dll".into()],
        },
    );
    let a = admit(&p, &facts, &digest(&[], &[]), Path::new("/game"), None, "enshrouded");
    assert_eq!(
        excluded(&a),
        BTreeMap::from([("user", ("native-refused", vec!["lib", "dll"]))])
    );
}

// No kiln-sfi (K5): every member of the ShroudForge pass is backend-unavailable; the EML dialect does not need it. No
// terminal (K6): the packages with native code and what needs them are native-terminal-missing.
#[test]
fn without_kiln_sfi_or_its_terminal_the_native_side_decides_nothing_it_did_not_report() {
    let members = || {
        vec![
            package("dll", &[], &[], true),
            package("needs", &["dll"], &[], false),
            package("lua", &[], &[], false),
        ]
    };
    let absent = NativeFacts {
        attached: false,
        received: false,
        withdrawn: BTreeMap::new(),
        coherent: Ok(()),
        dlls: BTreeMap::new(),
    };
    let a = admit(
        &plan(members(), StartupDialect::Shroudforge),
        &absent,
        &digest(&[], &[]),
        Path::new("/g"),
        None,
        "enshrouded",
    );
    assert!(a.order.is_empty());
    assert!(a.excluded.values().all(|r| r.code == "backend-unavailable"));
    let mut eml = members();
    for p in &mut eml {
        p.kind = PackageKind::Eml;
    }
    let a = admit(
        &plan(eml, StartupDialect::Eml),
        &absent,
        &digest(&[], &[]),
        Path::new("/g"),
        None,
        "enshrouded",
    );
    assert_eq!(a.order, ["dll", "needs", "lua"], "the EML pass does not need kiln-sfi");
    let silent = NativeFacts {
        attached: true,
        ..absent
    };
    let a = admit(
        &plan(members(), StartupDialect::Shroudforge),
        &silent,
        &digest(&[], &[]),
        Path::new("/g"),
        None,
        "enshrouded",
    );
    assert_eq!(
        excluded(&a),
        BTreeMap::from([
            ("dll", ("native-terminal-missing", vec![])),
            ("needs", ("native-terminal-missing", vec!["dll"]))
        ])
    );
    assert_eq!(a.order, ["lua"]);
}

// A package whose folder no longer has the plan's digest (or cannot be hashed) is package-changed, with what needs it;
// in the ShroudForge dialect so is one the discovery does not have as planned (removed, switched off).
#[test]
fn a_changed_or_missing_package_is_left_out_with_what_needs_it() {
    let p = plan(
        vec![
            package("changed", &[], &[], false),
            package("needs", &["changed"], &[], false),
            package("gone", &[], &[], false),
            package("fine", &[], &[], false),
        ],
        StartupDialect::Shroudforge,
    );
    let a = admit(
        &p,
        &native(),
        &digest(&["changed"], &["gone"]),
        Path::new("/g"),
        None,
        "enshrouded",
    );
    assert_eq!(
        excluded(&a),
        BTreeMap::from([
            ("changed", ("package-changed", vec![])),
            ("gone", ("package-changed", vec![])),
            ("needs", ("package-changed", vec!["changed"]))
        ])
    );
    assert_eq!(a.order, ["fine"]);
    // the discovery: a switched-off package is not the plan's
    let s = Start::new("g13-admit");
    s.patch("on", &[], &[], &content("on"));
    s.package_with(
        "off",
        serde_json::json!({"capabilities": ["patch"]}),
        serde_json::json!({"enabled": false}),
        Some("return {}"),
    );
    let registry = s.registry();
    let p = plan(
        vec![package("on", &[], &[], false), package("off", &[], &[], false)],
        StartupDialect::Shroudforge,
    );
    let a = admit(
        &p,
        &native(),
        &digest(&[], &[]),
        s.game(),
        Some(&registry),
        "enshrouded",
    );
    assert_eq!(excluded(&a), BTreeMap::from([("off", ("package-changed", vec![]))]));
}

fn startup_plan(s: &Start) -> StartupPlan {
    let registry = s.registry();
    let order = sfi_core::package::plan::startup_plan(&registry, false, None).ids;
    StartupPlan::new(
        registry,
        order,
        Side::Client,
        SourceVersion::Sf179,
        Features {
            patch: true,
            export: false,
        },
        None,
        Rc::new(Catalog),
        None,
    )
    .unwrap()
}
fn ids(p: &kiln_eml::sf_pass::PassPlan) -> Vec<&str> {
    p.packages().iter().map(|p| p.id.as_str()).collect()
}

// The retry's order is the fork's re-plan: roots in id order, each after its dependencies in its
// manifest's order; a pulled-ahead dependency goes back to its own place. It only reorders, and the closure is
// PassPlan::without's (required, and switched-on optional ones).
#[test]
fn the_retry_plan_is_the_forks_re_plan() {
    let s = Start::new("g13-fork-order");
    s.patch("a", &["c"], &[], "return {}");
    s.patch("b", &[], &[], "return {}");
    s.patch("c", &[], &[], "return {}");
    s.patch("d", &["e", "b"], &[], "return {}");
    s.patch("e", &[], &[], "return {}");
    let plan = startup_plan(&s);
    assert_eq!(plan.order(), ["c", "a", "b", "e", "d"]);
    assert_eq!(ids(&plan.retry_without("a")), ["b", "c", "e", "d"]);
    assert_eq!(
        ids(&plan.pass_plan().without("a")),
        ["c", "b", "e", "d"],
        "the plain closure keeps the first order"
    );
    assert_eq!(
        ids(&plan.retry_without("b")),
        ["c", "a", "e"],
        "d needs b and drops out"
    );
    // order only
    let first = plan.pass_plan();
    let reordered = plan.fork_order(&first);
    let mut a: Vec<_> = ids(&first);
    let mut b: Vec<_> = ids(&reordered);
    a.sort();
    b.sort();
    assert_eq!(a, b);
}
