// SPDX-License-Identifier: MIT
//! A root-style consumer (the shape kiln-manage will have) of sfi-policy alone: the strict
//! launch-plan reader, the read-only PE gate and the advisory native inventory, built and
//! run from a foreign workspace with default features.
use sfi_policy::{
    launch_plan::{LaunchPlan, Permission, ValidationContext},
    native_contracts::{AdmissionVerdict, NativeContracts},
    pe_gate::{self, GateVerdict, Image, Policy},
};

fn pe() -> Vec<u8> {
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
    b
}

fn main() {
    let h = "a".repeat(64);
    let plan = format!(
        r#"{{"format":1,"minReader":1,"plan_id":"12345678-1234-1234-1234-123456789abc","launch_id":null,
        "startupDialect":"none","writer":{{"component":"kiln-manage","version":"0.1.0"}},
        "profile":{{"id":"test","generation":1,"mode":"active","semantic_sha256":"{h}"}},
        "game":{{"side":"client","exe_sha256":"{h}","archiveBase_sha256":"{h}","archiveInput_sha256":"{h}"}},
        "backend":{{"requested":"kiln","selected":"kiln","fallbackOrder":[],"dataWriter":"kiln-eml","selectionReason":"test"}},
        "contract":{{"apiVersion":"1.7.9","sourceCommit":"{}","upstream180Final":false,"inventory_sha256":"{h}","parity_sha256":"{h}","serviceContracts_sha256":"{h}"}},
        "fingerprints":{{"packages_sha256":"{h}","settings_sha256":"{h}","chain_sha256":"{h}","loaderInventory_sha256":"{h}","compat_sha256":"{h}","releasePolicy_sha256":"{h}","placementReceipts_sha256":"{h}"}},
        "packages":[],"loadOrder":[],"warnings":[]}}"#,
        "b".repeat(40)
    );
    let context = ValidationContext {
        exe_sha256: h.clone(),
        side: "client".into(),
        upstream_180_final: false,
    };
    let parsed = LaunchPlan::parse(plan.as_bytes(), &context).unwrap();
    assert_eq!(parsed.permission(true), Permission::EligiblePendingAdmission);
    assert!(!parsed.runtime_started());
    // The strict reader still refuses a duplicate key the serde parser would overwrite.
    let duplicate = plan.replacen(r#""warnings":[]"#, r#""warnings":[],"warnings":[]"#, 1);
    assert!(LaunchPlan::parse(duplicate.as_bytes(), &context).is_err());

    let bytes = pe();
    let images = [Image {
        path: "a.dll",
        bytes: &bytes,
    }];
    let report = pe_gate::gate("a.dll", &images, &Policy::default());
    assert!(matches!(report.verdict, GateVerdict::UnknownDynamic { .. }));

    let registry = NativeContracts::shipped().unwrap();
    let inventory = registry.inventory("a.dll", &images);
    assert_eq!(inventory.verdict(), AdmissionVerdict::AdmittedWithEnforcement);
    assert!(inventory.eligible());
    assert!(!inventory.reasons().is_empty());
    println!("consumer ok");
}
