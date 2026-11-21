// SPDX-License-Identifier: MIT
use serde_json::json;
use sfi_policy::native_contracts::{AdmissionVerdict, NativeContracts};
use sfi_policy::pe_gate::Image;
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
fn registry(bytes: &[u8], verdict: &str, absent: &str) -> NativeContracts {
    let value = json!({"format":1,"minReader":1,"contracts":{NativeContracts::sha256(bytes):{
        "verdict":verdict,"evidence":["synthetic disassembly evidence"],"reviewer":"fixture reviewer","date":"2026-11-15","absentPath":absent
    }}});
    NativeContracts::parse(&serde_json::to_vec(&value).unwrap()).unwrap()
}
#[test]
fn empty_registry_admits_generic_unknown_only_with_enforcement() {
    let registry = NativeContracts::parse(include_bytes!("../sfi-native-contracts.json")).unwrap();
    let b = pe();
    let result = registry.evaluate(
        "a.dll",
        &[Image {
            path: "a.dll",
            bytes: &b,
        }],
    );
    assert_eq!(result.verdict, AdmissionVerdict::AdmittedWithEnforcement);
    assert!(result.requires_enforcement);
    assert_eq!(
        NativeContracts::sha256(b"abc"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
}
#[test]
fn banned_ascii_and_utf16_evidence_needs_exact_hash_contract() {
    for wide in [false, true] {
        let mut b = pe();
        let needle = concat!("Kf", "cRuntimeGetSomething");
        let data = if wide {
            needle
                .encode_utf16()
                .flat_map(u16::to_le_bytes)
                .collect::<Vec<_>>()
        } else {
            needle.as_bytes().to_vec()
        };
        b[0x300..0x300 + data.len()].copy_from_slice(&data);
        let empty =
            NativeContracts::parse(b"{\"format\":1,\"minReader\":1,\"contracts\":{}}").unwrap();
        assert_eq!(
            empty
                .evaluate(
                    "a.dll",
                    &[Image {
                        path: "a.dll",
                        bytes: &b
                    }]
                )
                .verdict,
            AdmissionVerdict::NeedsContract
        );
        let reviewed = registry(&b, "presence-probe", "supported");
        let report = reviewed.evaluate(
            "a.dll",
            &[Image {
                path: "a.dll",
                bytes: &b,
            }],
        );
        assert_eq!(report.verdict, AdmissionVerdict::AdmittedWithEnforcement);
        assert_eq!(report.classification.as_deref(), Some("presence-probe"));
        assert!(!report.needs_reviewed_absent_path);
        b[0x3ff] ^= 1;
        assert_eq!(
            reviewed
                .evaluate(
                    "a.dll",
                    &[Image {
                        path: "a.dll",
                        bytes: &b
                    }]
                )
                .verdict,
            AdmissionVerdict::NeedsContract
        );
    }
}
#[test]
fn guarded_export_exception_and_feature_disabled_absent_path_are_explicit() {
    let b = pe();
    assert_eq!(
        registry(&b, "banned-export-resolution", "not-reviewed")
            .evaluate(
                "a.dll",
                &[Image {
                    path: "a.dll",
                    bytes: &b
                }]
            )
            .verdict,
        AdmissionVerdict::Refused
    );
    assert_eq!(
        registry(&b, "banned-export-resolution", "supported")
            .evaluate(
                "a.dll",
                &[Image {
                    path: "a.dll",
                    bytes: &b
                }]
            )
            .verdict,
        AdmissionVerdict::AdmittedWithEnforcement
    );
    let partial = registry(&b, "presence-probe", "feature-disabled").evaluate(
        "a.dll",
        &[Image {
            path: "a.dll",
            bytes: &b,
        }],
    );
    assert!(partial.partial && partial.requires_enforcement);
    assert_eq!(
        registry(&b, "presence-probe", "not-reviewed")
            .evaluate(
                "a.dll",
                &[Image {
                    path: "a.dll",
                    bytes: &b
                }]
            )
            .verdict,
        AdmissionVerdict::NeedsContract
    );
}
#[test]
fn advisory_inventory_never_becomes_an_unresolved_executable_permit() {
    let mut b = pe();
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
    b[0x260..0x26b].copy_from_slice(b"unknown.dll");
    b[0x300..0x308].copy_from_slice(&((1u64 << 63) | 1).to_le_bytes());
    let registry = NativeContracts::parse(include_bytes!("../sfi-native-contracts.json")).unwrap();
    let images = [Image {
        path: "entry.dll",
        bytes: &b,
    }];
    assert_eq!(
        registry.advisory("entry.dll", &images).verdict,
        AdmissionVerdict::AdmittedWithEnforcement
    );
    assert_eq!(
        registry.evaluate("entry.dll", &images).verdict,
        AdmissionVerdict::Refused
    );
}

#[test]
fn reviewed_pass_never_overrides_a_banned_import() {
    let mut b = pe();
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
    let module = b"shroudforge-runtime.dll";
    b[0x260..0x260 + module.len()].copy_from_slice(module);
    b[0x300..0x308].copy_from_slice(&((1u64 << 63) | 1).to_le_bytes());
    let registry = registry(&b, "pass", "not-applicable");
    assert_eq!(
        registry
            .evaluate(
                "a.dll",
                &[Image {
                    path: "a.dll",
                    bytes: &b
                }]
            )
            .verdict,
        AdmissionVerdict::Refused
    );
}
#[test]
fn filenames_cannot_downgrade_structural_failure_or_skip_entry_inspection() {
    let empty = NativeContracts::parse(include_bytes!("../sfi-native-contracts.json")).unwrap();
    for name in [
        "dynamic behavior unproven.dll",
        "dynamic lookup evidence.dll",
        "unresolved dependency/search identity.dll",
    ] {
        assert_eq!(
            empty
                .evaluate(
                    name,
                    &[Image {
                        path: name,
                        bytes: b"not a PE"
                    }]
                )
                .verdict,
            AdmissionVerdict::Refused
        );
    }
    let mut banned = pe();
    for (at, value) in [
        (0x98 + 108, 16u32),
        (0x98 + 120, 0x1000),
        (0x98 + 124, 40),
        (0x200, 0x1100),
        (0x20c, 0x1060),
        (0x210, 0x1100),
    ] {
        banned[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }
    let module = b"shroudforge-runtime.dll";
    banned[0x260..0x260 + module.len()].copy_from_slice(module);
    banned[0x300..0x308].copy_from_slice(&((1u64 << 63) | 1).to_le_bytes());
    for other in [
        "dynamic behavior unproven/é.dll",
        "dynamic lookup evidence/é.dll",
    ] {
        assert_eq!(
            empty
                .evaluate(
                    "entry.dll",
                    &[
                        Image {
                            path: "entry.dll",
                            bytes: &banned
                        },
                        Image {
                            path: other,
                            bytes: &[]
                        }
                    ]
                )
                .verdict,
            AdmissionVerdict::Refused
        );
    }
    assert_eq!(
        empty
            .evaluate(
                "entry.dll",
                &[
                    Image {
                        path: "entry.dll",
                        bytes: &banned
                    },
                    Image {
                        path: "DYNAMIC BEHAVIOR UNPROVEN.DLL",
                        bytes: &[]
                    },
                    Image {
                        path: "dynamic behavior unproven.dll",
                        bytes: &[]
                    }
                ]
            )
            .verdict,
        AdmissionVerdict::Refused
    );
}

#[test]
fn registry_rejects_unknown_versions_fields_duplicates_and_bad_provenance() {
    for text in [
        r#"{"format":2,"minReader":1,"contracts":{}}"#,
        r#"{"format":1,"minReader":1,"contracts":{},"extra":true}"#,
        r#"{"format":1,"format":1,"minReader":1,"contracts":{}}"#,
        r#"{"format":1,"minReader":2,"contracts":{}}"#,
    ] {
        assert!(NativeContracts::parse(text.as_bytes()).is_err());
    }
    let mut value = json!({"format":1,"minReader":1,"contracts":{NativeContracts::sha256(b"a"): {"verdict":"pass","evidence":[],"reviewer":"fixture","date":"2026-99-99","absentPath":"not-applicable"}}});
    assert!(NativeContracts::parse(&serde_json::to_vec(&value).unwrap()).is_err());
    value["contracts"][NativeContracts::sha256(b"a")]["evidence"] = json!(["evidence"]);
    assert!(NativeContracts::parse(&serde_json::to_vec(&value).unwrap()).is_err());
}

fn importing(module: &[u8]) -> Vec<u8> {
    let mut b = pe();
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
    b
}
#[test]
fn shipped_registry_is_the_checked_in_empty_file() {
    assert_eq!(
        sfi_policy::native_contracts::SHIPPED,
        include_bytes!("../sfi-native-contracts.json")
    );
    let shipped = NativeContracts::shipped().unwrap();
    let b = pe();
    let images = [Image {
        path: "a.dll",
        bytes: &b,
    }];
    // Empty: no exact-hash classification exists for any image.
    assert_eq!(shipped.evaluate("a.dll", &images).classification, None);
}
#[test]
fn inventory_reports_eligibility_facts_and_never_admits_a_refusal() {
    let registry = NativeContracts::shipped().unwrap();
    let generic = pe();
    let unresolved = importing(b"unknown.dll");
    let banned = importing(b"shroudforge-runtime.dll");
    let mut evidence = pe();
    let needle = concat!("Kf", "cRuntimeGetSomething");
    evidence[0x300..0x300 + needle.len()].copy_from_slice(needle.as_bytes());
    for (bytes, verdict, eligible) in [
        (&generic, AdmissionVerdict::AdmittedWithEnforcement, true),
        // A host cannot bind system images: unresolved imports stay advisory there ...
        (&unresolved, AdmissionVerdict::AdmittedWithEnforcement, true),
        (&banned, AdmissionVerdict::Refused, false),
        (&evidence, AdmissionVerdict::NeedsContract, false),
    ] {
        let images = [Image {
            path: "entry.dll",
            bytes,
        }];
        let inventory = registry.inventory("entry.dll", &images);
        let advisory = registry.advisory("entry.dll", &images);
        assert_eq!(inventory.verdict(), verdict);
        assert_eq!(inventory.eligible(), eligible);
        assert_eq!(inventory.reasons(), advisory.reasons.as_slice());
        assert_eq!(inventory.evidence(), advisory.evidence.as_slice());
        assert_eq!(inventory.partial(), advisory.partial);
        assert_eq!(
            inventory.needs_reviewed_absent_path(),
            advisory.needs_reviewed_absent_path
        );
        assert_eq!(inventory.classification(), None);
    }
    // ... while the executable evaluation refuses them without an OS binding.
    let images = [Image {
        path: "entry.dll",
        bytes: &unresolved,
    }];
    assert_eq!(
        registry.evaluate("entry.dll", &images).verdict,
        AdmissionVerdict::Refused
    );
    // A feature-disabled reviewed probe is eligible but partial.
    let reviewed = registry_for(&evidence, "presence-probe", "feature-disabled");
    let images = [Image {
        path: "entry.dll",
        bytes: &evidence,
    }];
    let inventory = reviewed.inventory("entry.dll", &images);
    assert!(inventory.eligible() && inventory.partial());
    assert_eq!(inventory.classification(), Some("presence-probe"));
}
fn registry_for(bytes: &[u8], verdict: &str, absent: &str) -> NativeContracts {
    registry(bytes, verdict, absent)
}
