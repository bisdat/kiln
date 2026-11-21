// SPDX-License-Identifier: MIT
//! The pre-select inventory's native half: the DLL Manager selection over Kiln-owned inputs and the read-only import gate on every
//! selected DLL. Authoritative: banned static/delay imports (direct or transitive) and needs-contract; eligible:
//! pass and generic unknown-dynamic admitted with enforcement. Nothing is loaded. The PE builder follows
//! crates/sfi/sfi-policy/tests/pe_gate.rs (synthetic PE32+ images, never a copied DLL).
mod sfi_scan_common;
use kiln_manage::manifest::RunSide;
use kiln_manage::sfi_scan::{Kind, parity::CatalogInput};
use serde_json::json;
use sfi_package::package::profiles::DllSelection;
use sfi_policy::native_contracts::NativeContracts;
use sfi_policy::native_selection::{NativePermission, NativePolicy};
use sfi_scan_common::*;

fn put16(b: &mut [u8], at: usize, value: u16) {
    b[at..at + 2].copy_from_slice(&value.to_le_bytes());
}
fn put32(b: &mut [u8], at: usize, value: u32) {
    b[at..at + 4].copy_from_slice(&value.to_le_bytes());
}
fn put64(b: &mut [u8], at: usize, value: u64) {
    b[at..at + 8].copy_from_slice(&value.to_le_bytes());
}
/// A synthetic PE32+ DLL importing `FixtureCall` from `module` (ordinary or delay), or importing nothing.
fn pe(module: Option<&str>, delay: bool) -> Vec<u8> {
    let mut b = vec![0u8; 0x1000];
    b[..2].copy_from_slice(b"MZ");
    put32(&mut b, 0x3c, 0x80);
    b[0x80..0x84].copy_from_slice(b"PE\0\0");
    put16(&mut b, 0x84, 0x8664);
    put16(&mut b, 0x86, 1);
    put16(&mut b, 0x94, 240);
    put16(&mut b, 0x96, 0x2022);
    put16(&mut b, 0x98, 0x20b);
    put64(&mut b, 0x98 + 24, 0x180000000);
    put32(&mut b, 0x98 + 56, 0x2000);
    put32(&mut b, 0x98 + 60, 0x200);
    put16(&mut b, 0x98 + 68, 3);
    put32(&mut b, 0x98 + 108, 16);
    b[0x188..0x18d].copy_from_slice(b".test");
    put32(&mut b, 0x188 + 8, 0xe00);
    put32(&mut b, 0x188 + 12, 0x1000);
    put32(&mut b, 0x188 + 16, 0xe00);
    put32(&mut b, 0x188 + 20, 0x200);
    if let Some(module) = module {
        let directory = 0x98 + 112 + if delay { 13 * 8 } else { 8 };
        put32(&mut b, directory, 0x1000);
        put32(&mut b, directory + 4, if delay { 64 } else { 40 });
        if delay {
            put32(&mut b, 0x200, 1);
            put32(&mut b, 0x204, 0x1100);
            put32(&mut b, 0x20c, 0x1180);
            put32(&mut b, 0x210, 0x1180);
        } else {
            put32(&mut b, 0x200, 0x1180);
            put32(&mut b, 0x20c, 0x1100);
            put32(&mut b, 0x210, 0x1180);
        }
        b[0x300..0x300 + module.len()].copy_from_slice(module.as_bytes());
        put64(&mut b, 0x380, 0x1200);
        b[0x402..0x40d].copy_from_slice(b"FixtureCall");
    }
    b
}
const BANNED: &str = "shroudforge-runtime.dll";
const BANNED2: &str = concat!("kf", "c-runtime.dll");

/// A runtime package whose native-plugin.ini names `entry.dll`.
fn native_pkg(g: &Game, dir: &str, files: &[(&str, Vec<u8>)]) {
    g.simple(dir, dir, &["runtime"], json!([]));
    g.file(
        dir,
        "native-plugin.ini",
        "[Plugin]\nEnabled=true\nDll=entry.dll\n",
    );
    for (name, bytes) in files {
        g.file(dir, name, bytes);
    }
}
fn registry(bytes: &[u8], verdict: &str, absent: &str) -> NativeContracts {
    let value = json!({"format":1,"minReader":1,"contracts":{NativeContracts::sha256(bytes):{
        "verdict":verdict,"evidence":["synthetic disassembly evidence"],"reviewer":"fixture reviewer","date":"2026-11-20","absentPath":absent
    }}});
    NativeContracts::parse(&serde_json::to_vec(&value).unwrap()).unwrap()
}

#[test]
fn banned_normal_delay_and_transitive_imports_skip_the_package_and_its_dependents() {
    let g = Game::new("banned");
    native_pkg(&g, "normal", &[("entry.dll", pe(Some(BANNED), false))]);
    native_pkg(&g, "delay", &[("entry.dll", pe(Some(BANNED2), true))]);
    native_pkg(
        &g,
        "transitive",
        &[
            ("entry.dll", pe(Some("helper.dll"), false)),
            ("helper.dll", pe(Some(BANNED), true)),
        ],
    );
    g.simple(
        "needs-normal",
        "needs-normal",
        &["runtime"],
        json!([{"id":"normal","version":"^1"}]),
    );
    g.simple(
        "needs-needs",
        "needs-needs",
        &["runtime"],
        json!([{"id":"needs-normal","version":"^1"}]),
    );
    let inv = client(&g);
    for dir in ["normal", "delay", "transitive"] {
        let p = pkg(&inv, dir);
        assert_eq!(codes(p), ["native-gate"], "{dir}: {:?}", p.blockers);
        assert_eq!(p.dlls.len(), 1);
        assert_eq!(
            (p.dlls[0].verdict, p.dlls[0].eligible, p.dlls[0].source),
            ("refused", false, "ini")
        );
        assert!(
            p.blockers[0].detail.contains("banned"),
            "{:?}",
            p.blockers[0]
        );
        assert!(
            p.parts.native && p.planned,
            "SF plans it; Kiln's gate skips it"
        );
    }
    assert!(
        pkg(&inv, "transitive").blockers[0]
            .detail
            .contains("helper.dll")
    );
    let p = pkg(&inv, "needs-needs");
    assert_eq!(codes(p), ["dependency-skipped"]);
    assert_eq!(p.blockers[0].caused_by, ["needs-normal", "normal"]);
    assert_eq!(pkg(&inv, "needs-normal").blockers[0].caused_by, ["normal"]);
}

#[test]
fn generic_unknown_dynamic_is_admitted_with_enforcement_and_stays_eligible() {
    let g = Game::new("unknown");
    native_pkg(&g, "dyn", &[("entry.dll", pe(Some("kernel32.dll"), false))]);
    let inv = client(&g);
    let p = pkg(&inv, "dyn");
    assert!(p.eligible(), "{:?}", p.blockers);
    assert_eq!(p.dlls[0].verdict, "admitted-with-enforcement");
    assert!(
        p.advisories.iter().any(|a| a.class == "native"),
        "{:?}",
        p.advisories
    );
    assert_eq!(
        p.dlls[0].sha256.as_deref(),
        Some(NativeContracts::sha256(&pe(Some("kernel32.dll"), false)).as_str())
    );
}

fn evidence_dll() -> Vec<u8> {
    let mut b = pe(None, false);
    let needle = concat!("Kf", "cRuntimeGetSomething");
    b[0x600..0x600 + needle.len()].copy_from_slice(needle.as_bytes());
    b
}

#[test]
fn needs_contract_binds_the_exact_hash() {
    let g = Game::new("contract");
    let dll = evidence_dll();
    native_pkg(&g, "ev", &[("entry.dll", dll.clone())]);
    let empty = native();
    let inv = run(&g, RunSide::Client, &empty, CatalogInput::Missing);
    let p = pkg(&inv, "ev");
    assert_eq!(
        (p.dlls[0].verdict, codes(p)),
        ("needs-contract", vec!["native-gate".to_string()])
    );
    let mut reviewed = native();
    reviewed.contracts = registry(&dll, "pass", "not-applicable");
    let inv = run(&g, RunSide::Client, &reviewed, CatalogInput::Missing);
    let p = pkg(&inv, "ev");
    assert!(p.eligible(), "{:?}", p.blockers);
    assert_eq!(p.dlls[0].verdict, "pass");
    // one byte changed: the review no longer applies
    let mut changed = dll.clone();
    changed[0xf00] ^= 1;
    g.file("ev", "entry.dll", &changed);
    let inv = run(&g, RunSide::Client, &reviewed, CatalogInput::Missing);
    assert_eq!(pkg(&inv, "ev").dlls[0].verdict, "needs-contract");
}

#[test]
fn presence_probe_needs_a_reviewed_absent_path() {
    let g = Game::new("probe");
    let dll = evidence_dll();
    native_pkg(&g, "probe", &[("entry.dll", dll.clone())]);
    let mut supported = native();
    supported.contracts = registry(&dll, "presence-probe", "supported");
    let inv = run(&g, RunSide::Client, &supported, CatalogInput::Missing);
    let p = pkg(&inv, "probe");
    assert!(p.eligible(), "{:?}", p.blockers);
    assert_eq!(p.dlls[0].verdict, "admitted-with-enforcement");
    let mut unreviewed = native();
    unreviewed.contracts = registry(&dll, "presence-probe", "not-reviewed");
    let inv = run(&g, RunSide::Client, &unreviewed, CatalogInput::Missing);
    let p = pkg(&inv, "probe");
    assert_eq!((p.dlls[0].verdict, p.eligible()), ("needs-contract", false));
}

#[test]
fn one_dll_declared_twice_is_one_record_loaded_once() {
    let g = Game::new("twice");
    g.simple("sel", "sel", &["runtime"], json!([]));
    g.file("sel", "bin/x.dll", pe(Some("kernel32.dll"), false));
    let mut n = native();
    n.selections = vec![
        serde_json::from_value::<DllSelection>(json!({
            "id":"s1","packageDirectory":"sel","name":"Sel","target":"both","enabled":true,"approved":true,
            "files":[{"path":"bin/x.dll"},{"path":"bin//X.DLL"}]
        }))
        .unwrap(),
    ];
    let inv = run(&g, RunSide::Client, &n, CatalogInput::Missing);
    let p = pkg(&inv, "sel");
    assert_eq!(p.dlls.len(), 1, "{:?}", p.dlls);
    assert_eq!(p.dlls[0].source, "selection");
    assert_eq!(p.dlls[0].aliases.len(), 1);
    assert!(p.eligible(), "{:?}", p.blockers);
}

#[test]
fn kiln_native_policy_never_withdraws_in_kiln_words() {
    let g = Game::new("never");
    native_pkg(&g, "dyn", &[("entry.dll", pe(Some("kernel32.dll"), false))]);
    g.simple(
        "lua-only",
        "lua-only",
        &["runtime"],
        json!([{"id":"dyn","version":"^1"}]),
    );
    let mut n = native();
    n.permission = NativePermission {
        policy: NativePolicy::Never,
        verified: false,
    };
    let inv = run(&g, RunSide::Client, &n, CatalogInput::Missing);
    let p = pkg(&inv, "dyn");
    assert!(p.dlls.is_empty());
    assert_eq!(codes(p), ["native-selection"]);
    assert!(
        p.blockers[0].detail.contains("native.policy"),
        "{:?}",
        p.blockers
    );
    assert!(
        !pkg(&inv, "lua-only").eligible(),
        "a dependent is withdrawn with it"
    );
}

#[test]
fn a_manifestless_folder_with_a_dll_is_listed_and_gated() {
    let g = Game::new("bare");
    g.file(
        "bare",
        "native-plugin.ini",
        "[Plugin]\nEnabled=1\nDll=entry.dll\n",
    );
    g.file("bare", "entry.dll", b"MZ but not a PE image");
    let inv = client(&g);
    let p = pkg(&inv, "bare");
    assert_eq!((p.kind, p.id.clone()), (Kind::NativeSelection, None));
    assert_eq!(p.dlls.len(), 1);
    assert_eq!(
        p.dlls[0].verdict, "refused",
        "an invalid image is refused, never loaded to find out"
    );
    assert!(!p.eligible());
}

#[test]
fn a_dll_on_the_other_side_or_switched_off_is_not_selected() {
    let g = Game::new("side");
    native_pkg(&g, "dyn", &[("entry.dll", pe(Some("kernel32.dll"), false))]);
    let mut n = native();
    n.selections = vec![
        serde_json::from_value::<DllSelection>(json!({
            "id":"s1","packageDirectory":"dyn","name":"D","target":"server","enabled":true,"approved":true
        }))
        .unwrap(),
    ];
    let inv = run(&g, RunSide::Client, &n, CatalogInput::Missing);
    assert!(pkg(&inv, "dyn").dlls.is_empty());
    let inv = run(&g, RunSide::Server, &n, CatalogInput::Missing);
    assert_eq!(pkg(&inv, "dyn").dlls.len(), 1);
    n.manager_enabled = false;
    let inv = run(&g, RunSide::Server, &n, CatalogInput::Missing);
    assert!(pkg(&inv, "dyn").dlls.is_empty());
}

// Edge cases.

#[test]
fn a_doubled_slash_in_the_declared_path_gates_the_same_file_the_selection_found() {
    let g = Game::new("slashes");
    g.simple("p", "p", &["runtime"], json!([]));
    g.file(
        "p",
        "native-plugin.ini",
        "[Plugin]\nEnabled=true\nDll=sub//entry.dll\n",
    );
    g.file("p", "sub/entry.dll", pe(Some("kernel32.dll"), false));
    let inv = client(&g);
    let p = pkg(&inv, "p");
    assert_eq!(p.dlls.len(), 1, "{:?}", p.advisories);
    assert_eq!(
        p.dlls[0].verdict, "admitted-with-enforcement",
        "{:?}",
        p.dlls[0]
    );
    assert!(p.eligible(), "{:?}", p.blockers);
}

/// True when the folder's file system tells names apart by letter case only (Linux); false on a case-insensitive one
/// (macOS APFS by default, NTFS on Windows), where a second spelling opens the first file.
fn case_sensitive(dir: &std::path::Path) -> bool {
    std::fs::create_dir_all(dir).unwrap();
    std::fs::write(dir.join("case-probe"), "a").unwrap();
    let sensitive = !dir.join("CASE-PROBE").exists();
    std::fs::remove_file(dir.join("case-probe")).unwrap();
    sensitive
}

#[test]
fn native_plugin_ini_is_found_in_any_letter_case_and_the_exact_name_wins() {
    // Only an upper-case spelling: ShroudForge on Windows (and Wine on Linux) opens it as native-plugin.ini.
    let g = Game::new("inicase");
    g.simple("upper", "upper", &["runtime"], json!([]));
    g.file(
        "upper",
        "NATIVE-PLUGIN.INI",
        "[Plugin]\nEnabled=true\nDll=entry.dll\n",
    );
    g.file("upper", "entry.dll", pe(Some("kernel32.dll"), false));
    // Several spellings side by side, possible only on a case-sensitive file system: Wine opens the exact name first.
    g.simple("many", "many", &["runtime"], json!([]));
    let sensitive = case_sensitive(&g.dir("many"));
    g.file(
        "many",
        "native-plugin.ini",
        "[Plugin]\nEnabled=true\nDll=entry.dll\n",
    );
    for other in [
        "NATIVE-PLUGIN.INI",
        "Native-Plugin.ini",
        "native-plugin.INI",
        "Native-plugin.ini",
    ] {
        if sensitive {
            g.file("many", other, "[Plugin]\nEnabled=false\nDll=entry.dll\n");
        }
    }
    g.file("many", "entry.dll", pe(Some("kernel32.dll"), false));
    let inv = client(&g);
    let p = pkg(&inv, "upper");
    assert_eq!(p.dlls.len(), 1, "{:?}", p.advisories);
    assert_eq!((p.dlls[0].source, p.eligible()), ("ini", true));
    let p = pkg(&inv, "many");
    assert_eq!(p.dlls.len(), 1, "sensitive={sensitive}: {:?}", p.advisories);
    assert!(p.parts.native && p.eligible());
}

#[test]
fn the_gate_inspects_the_exactly_named_dll_when_two_spellings_exist() {
    let g = Game::new("dllcase");
    if !case_sensitive(&g.dir("p")) {
        return; // one file per name in any case: nothing to choose between
    }
    native_pkg(
        &g,
        "p",
        &[
            ("entry.dll", pe(Some("kernel32.dll"), false)),
            ("Entry.dll", pe(Some(BANNED), false)),
        ],
    );
    let inv = client(&g);
    let p = pkg(&inv, "p");
    assert_eq!(p.dlls.len(), 1);
    assert_eq!(
        p.dlls[0].sha256.as_deref(),
        Some(NativeContracts::sha256(&pe(Some("kernel32.dll"), false)).as_str()),
        "the file Windows-style lookup opens for entry.dll"
    );
    // The import gate itself refuses a closure with two images whose names differ only by case.
    assert_eq!(codes(p), ["native-gate"]);
    assert!(
        p.blockers[0].detail.contains("case-colliding"),
        "{:?}",
        p.blockers
    );
}
