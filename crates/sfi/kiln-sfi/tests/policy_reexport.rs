// SPDX-License-Identifier: MIT
//! Compatibility: kiln-sfi's old policy paths are the moved sfi-policy items, not copies,
//! so the native-load enforcement's callers and a host-side consumer get the same strict plan and native decisions.
use serde_json::{Value, json};
use std::any::TypeId;

#[test]
fn old_paths_are_the_same_types() {
    use kiln_sfi::{launch_plan as old_plan, native_contracts as old_nc, pe_gate as old_pe};
    use sfi_policy::{launch_plan as new_plan, native_contracts as new_nc, pe_gate as new_pe};
    let pairs = [
        (
            TypeId::of::<old_plan::LaunchPlan>(),
            TypeId::of::<new_plan::LaunchPlan>(),
        ),
        (
            TypeId::of::<old_plan::ValidationContext>(),
            TypeId::of::<new_plan::ValidationContext>(),
        ),
        (
            TypeId::of::<old_plan::Permission>(),
            TypeId::of::<new_plan::Permission>(),
        ),
        (
            TypeId::of::<old_nc::NativeContracts>(),
            TypeId::of::<new_nc::NativeContracts>(),
        ),
        (
            TypeId::of::<old_nc::AdmissionReport>(),
            TypeId::of::<new_nc::AdmissionReport>(),
        ),
        (
            TypeId::of::<old_nc::AdmissionVerdict>(),
            TypeId::of::<new_nc::AdmissionVerdict>(),
        ),
        (
            TypeId::of::<old_pe::GateVerdict>(),
            TypeId::of::<new_pe::GateVerdict>(),
        ),
        (
            TypeId::of::<old_pe::Image<'static>>(),
            TypeId::of::<new_pe::Image<'static>>(),
        ),
        (
            TypeId::of::<old_pe::PeImports>(),
            TypeId::of::<new_pe::PeImports>(),
        ),
    ];
    for (i, (old, new)) in pairs.iter().enumerate() {
        assert_eq!(old, new, "pair {i} is a copy, not the moved item");
    }
    assert_eq!(old_pe::MAX_IMAGE_BYTES, new_pe::MAX_IMAGE_BYTES);
}

fn plan() -> Value {
    let h = "a".repeat(64);
    json!({"format":1,"minReader":1,"plan_id":"12345678-1234-1234-1234-123456789abc","launch_id":null,
        "startupDialect":"none","writer":{"component":"kiln-manage","version":"0.1.0"},
        "profile":{"id":"test","generation":1,"mode":"active","semantic_sha256":h},
        "game":{"side":"client","exe_sha256":h,"archiveBase_sha256":h,"archiveInput_sha256":h},
        "backend":{"requested":"kiln","selected":"kiln","fallbackOrder":[],"dataWriter":"kiln-eml","selectionReason":"test"},
        "contract":{"apiVersion":"1.7.9","sourceCommit":"b".repeat(40),"upstream180Final":false,"inventory_sha256":h,"parity_sha256":h,"serviceContracts_sha256":h},
        "fingerprints":{"packages_sha256":h,"settings_sha256":h,"chain_sha256":h,"loaderInventory_sha256":h,"compat_sha256":h,"releasePolicy_sha256":h,"placementReceipts_sha256":h},
        "packages":[],"loadOrder":[],"warnings":[]})
}

#[test]
fn both_paths_give_identical_plan_decisions() {
    let mut cases = vec![serde_json::to_vec(&plan()).unwrap()];
    let mut hosted = plan();
    hosted["backend"]["requested"] = json!("hosted");
    hosted["backend"]["selected"] = json!("hosted");
    hosted["backend"]["dataWriter"] = json!("shroudforge");
    cases.push(serde_json::to_vec(&hosted).unwrap());
    let mut wrong_side = plan();
    wrong_side["game"]["side"] = json!("server");
    cases.push(serde_json::to_vec(&wrong_side).unwrap());
    let text = serde_json::to_string(&plan()).unwrap();
    cases.push(
        text.replacen(r#""warnings":[]"#, r#""warnings":[],"warnings":[]"#, 1)
            .into_bytes(),
    );
    cases.push(
        text.replacen(r#""generation":1"#, r#""generation":01"#, 1)
            .into_bytes(),
    );
    let old_context = kiln_sfi::launch_plan::ValidationContext {
        exe_sha256: "a".repeat(64),
        side: "client".into(),
        upstream_180_final: false,
    };
    let new_context = sfi_policy::launch_plan::ValidationContext {
        exe_sha256: "a".repeat(64),
        side: "client".into(),
        upstream_180_final: false,
    };
    let mut outcomes = Vec::new();
    for bytes in &cases {
        let old = kiln_sfi::launch_plan::LaunchPlan::parse(bytes, &old_context)
            .map(|p| (p.plan_id(), p.permission(true), p.permission(false)));
        let new = sfi_policy::launch_plan::LaunchPlan::parse(bytes, &new_context)
            .map(|p| (p.plan_id(), p.permission(true), p.permission(false)));
        assert_eq!(old, new);
        outcomes.push(old.is_ok());
    }
    assert_eq!(outcomes, [true, true, false, false, false]);
}

fn pe(import: Option<&[u8]>) -> Vec<u8> {
    let mut b = vec![0; 0x400];
    b[..2].copy_from_slice(b"MZ");
    for (at, value) in [
        (0x3c, 0x80u32),
        (0x98 + 56, 0x2000),
        (0x98 + 60, 0x200),
        (0x188 + 8, 0x200),
        (0x188 + 12, 0x1000),
        (0x188 + 16, 0x200),
        (0x188 + 20, 0x200),
    ] {
        b[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }
    b[0x80..0x84].copy_from_slice(b"PE\0\0");
    for (at, value) in [
        (0x84, 0x8664u16),
        (0x86, 1),
        (0x94, 240),
        (0x96, 0x2000),
        (0x98, 0x20b),
        (0x98 + 68, 3),
    ] {
        b[at..at + 2].copy_from_slice(&value.to_le_bytes());
    }
    if let Some(module) = import {
        for (at, value) in [
            (0x98 + 108, 16u32),
            (0x98 + 120, 0x1000),
            (0x98 + 124, 40),
            (0x200, 0x1100),
            (0x20c, 0x1060),
            (0x210, 0x1100),
        ] {
            b[at..at + 4].copy_from_slice(&value.to_le_bytes());
        }
        b[0x260..0x260 + module.len()].copy_from_slice(module);
        b[0x300..0x308].copy_from_slice(&((1u64 << 63) | 1).to_le_bytes());
    }
    b
}

#[test]
fn both_paths_give_identical_native_decisions() {
    let generic = pe(None);
    let unresolved = pe(Some(b"unknown.dll"));
    let banned = pe(Some(b"shroudforge-runtime.dll"));
    let old = kiln_sfi::native_contracts::NativeContracts::shipped().unwrap();
    let new = sfi_policy::native_contracts::NativeContracts::shipped().unwrap();
    for bytes in [&generic, &unresolved, &banned] {
        let old_images = [kiln_sfi::pe_gate::Image {
            path: "entry.dll",
            bytes,
        }];
        let new_images = [sfi_policy::pe_gate::Image {
            path: "entry.dll",
            bytes,
        }];
        for advisory in [false, true] {
            let (a, b) = if advisory {
                (
                    old.advisory("entry.dll", &old_images),
                    new.advisory("entry.dll", &new_images),
                )
            } else {
                (
                    old.evaluate("entry.dll", &old_images),
                    new.evaluate("entry.dll", &new_images),
                )
            };
            assert_eq!(a.verdict, b.verdict);
            assert_eq!(a.reasons, b.reasons);
            assert_eq!(a.evidence, b.evidence);
            assert_eq!(a.requires_enforcement, b.requires_enforcement);
        }
        let inventory = new.inventory("entry.dll", &new_images);
        assert_eq!(
            inventory.verdict(),
            new.advisory("entry.dll", &new_images).verdict
        );
        assert_eq!(
            kiln_sfi::pe_gate::gate("entry.dll", &old_images, &Default::default()),
            sfi_policy::pe_gate::gate("entry.dll", &new_images, &Default::default())
        );
    }
}

#[test]
fn bound_evaluation_refuses_an_import_the_os_did_not_bind() {
    // kiln-sfi's resolver refuses an unbound import first; evaluate_bound refuses it too,
    // so the os-binding member is never an advisory path in disguise.
    let registry = sfi_policy::native_contracts::NativeContracts::shipped().unwrap();
    let bytes = pe(Some(b"unknown.dll"));
    let images = [sfi_policy::pe_gate::Image {
        path: "entry.dll",
        bytes: &bytes,
    }];
    let unbound = registry.evaluate_bound("entry.dll", &images, &[]);
    assert_eq!(
        unbound.verdict,
        sfi_policy::native_contracts::AdmissionVerdict::Refused
    );
    assert!(!unbound.requires_enforcement);
    let bound = registry.evaluate_bound("entry.dll", &images, &["unknown.dll"]);
    assert_eq!(
        bound.verdict,
        sfi_policy::native_contracts::AdmissionVerdict::AdmittedWithEnforcement
    );
    assert!(bound.requires_enforcement);
}
