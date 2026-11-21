// SPDX-License-Identifier: MIT
use sfi_policy::pe_gate::{self, GateVerdict, Image, ImportKind, Policy, ReviewedImage};

// Synthetic PE32+ DLL, never a copied DLL or game byte. One file-backed section.
fn fixture(module: Option<&str>, delay: bool, ordinal: bool) -> Vec<u8> {
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
        put64(
            &mut b,
            0x380,
            if ordinal { (1u64 << 63) | 17 } else { 0x1200 },
        );
        b[0x402..0x40d].copy_from_slice(b"FixtureCall");
    }
    b
}
fn put16(b: &mut [u8], at: usize, value: u16) {
    b[at..at + 2].copy_from_slice(&value.to_le_bytes());
}
fn put32(b: &mut [u8], at: usize, value: u32) {
    b[at..at + 4].copy_from_slice(&value.to_le_bytes());
}
fn put64(b: &mut [u8], at: usize, value: u64) {
    b[at..at + 8].copy_from_slice(&value.to_le_bytes());
}
fn one(bytes: &[u8]) -> pe_gate::GateReport {
    pe_gate::gate(
        "entry.dll",
        &[Image {
            path: "entry.dll",
            bytes,
        }],
        &Policy::default(),
    )
}

#[test]
fn ordinary_delay_path_and_ordinal_imports_identify_banned_module() {
    for module in [
        "SHROUDFORGE-RUNTIME.DLL",
        concat!("C:\\runtime\\KF", "C-RUNTIME.DLL"),
        "path/to/Shroudforge-Runtime.dll",
    ] {
        for delay in [false, true] {
            let b = fixture(Some(module), delay, false);
            let parsed = pe_gate::parse(&b).unwrap();
            assert_eq!(
                parsed.imports[0].kind,
                if delay {
                    ImportKind::Delay
                } else {
                    ImportKind::Ordinary
                }
            );
            assert_eq!(parsed.imports[0].symbols, ["FixtureCall"]);
            assert!(matches!(one(&b).verdict, GateVerdict::Banned { .. }));
        }
    }
    let b = fixture(Some("shroudforge-runtime.dll"), false, true);
    assert_eq!(pe_gate::parse(&b).unwrap().imports[0].symbols, ["#17"]);
    assert!(matches!(one(&b).verdict, GateVerdict::Banned { .. }));
}

#[test]
fn transitive_banned_import_is_not_hidden_by_unreviewed_root() {
    let root = fixture(Some("helper.dll"), false, false);
    let helper = fixture(Some(concat!("kf", "c-runtime.dll")), true, false);
    let result = pe_gate::gate(
        "entry.dll",
        &[
            Image {
                path: "entry.dll",
                bytes: &root,
            },
            Image {
                path: "helper.dll",
                bytes: &helper,
            },
        ],
        &Policy::default(),
    );
    match result.verdict {
        GateVerdict::Banned {
            image,
            module,
            symbols,
            ..
        } => {
            assert_eq!(image, "helper.dll");
            assert_eq!(module, concat!("kf", "c-runtime.dll"));
            assert_eq!(symbols, ["FixtureCall"]);
        }
        other => panic!("expected transitive ban, got {other:?}"),
    }
}

#[test]
fn no_strings_is_not_a_proof_and_reviews_bind_exact_bytes() {
    let b = fixture(None, false, false);
    assert!(matches!(
        one(&b).verdict,
        GateVerdict::UnknownDynamic { .. }
    ));
    let review = [ReviewedImage {
        path: "entry.dll",
        bytes: &b,
        contract: "synthetic reviewed fixture",
        dynamic_lookups_reviewed: false,
    }];
    let policy = Policy {
        reviews: &review,
        system_modules: &[],
    };
    assert_eq!(
        pe_gate::gate(
            "entry.dll",
            &[Image {
                path: "entry.dll",
                bytes: &b
            }],
            &policy
        )
        .verdict,
        GateVerdict::Pass
    );
    let mut changed = b.clone();
    changed[0xf00] = 1;
    assert!(matches!(
        pe_gate::gate(
            "entry.dll",
            &[Image {
                path: "entry.dll",
                bytes: &changed
            }],
            &policy
        )
        .verdict,
        GateVerdict::UnknownDynamic { .. }
    ));
}

#[test]
fn known_dynamic_strings_are_advisory_and_never_a_static_ban() {
    let mut b = fixture(None, false, false);
    b[0x800..0x80e].copy_from_slice(b"GetProcAddress");
    let report = one(&b);
    assert!(!report.evidence.is_empty());
    assert!(matches!(report.verdict, GateVerdict::UnknownDynamic { .. }));
    let review = [ReviewedImage {
        path: "entry.dll",
        bytes: &b,
        contract: "reviewed dynamic fixture",
        dynamic_lookups_reviewed: false,
    }];
    assert!(matches!(
        pe_gate::gate(
            "entry.dll",
            &[Image {
                path: "entry.dll",
                bytes: &b
            }],
            &Policy {
                reviews: &review,
                system_modules: &[]
            }
        )
        .verdict,
        GateVerdict::UnknownDynamic { .. }
    ));
}

#[test]
fn hostile_truncations_bad_rvas_and_unterminated_tables_never_panic_or_pass() {
    let b = fixture(Some("helper.dll"), false, false);
    for end in 0..0x500 {
        let result = std::panic::catch_unwind(|| pe_gate::parse(&b[..end]));
        assert!(result.is_ok(), "panic at length {end}");
        assert!(result.unwrap().is_err(), "accepted truncation {end}");
    }
    for at in [
        0x3c,
        0x188 + 12,
        0x188 + 16,
        0x188 + 20,
        0x98 + 120,
        0x20c,
        0x200,
    ] {
        let mut bad = b.clone();
        put32(&mut bad, at, u32::MAX);
        assert!(
            pe_gate::parse(&bad).is_err(),
            "accepted hostile field at {at:#x}"
        );
    }
    let mut bad = b.clone();
    put32(&mut bad, 0x98 + 124, 20); // no null descriptor inside declared import directory
    assert!(pe_gate::parse(&bad).is_err());
}

#[test]
fn legacy_delay_va_and_ordinary_iat_fallback_are_supported() {
    let mut b = fixture(Some("helper.dll"), true, false);
    put64(&mut b, 0x98 + 24, 0x10000000);
    put32(&mut b, 0x200, 0);
    put32(&mut b, 0x204, 0x10001100);
    put32(&mut b, 0x20c, 0x10001180);
    put32(&mut b, 0x210, 0x10001180);
    put64(&mut b, 0x380, 0x10001200);
    assert_eq!(
        pe_gate::parse(&b).unwrap().imports[0].symbols,
        ["FixtureCall"]
    );
    put32(&mut b, 0x200, 2);
    assert!(pe_gate::parse(&b).is_err());
    let mut ordinary = fixture(Some("helper.dll"), false, false);
    put32(&mut ordinary, 0x200, 0);
    assert_eq!(
        pe_gate::parse(&ordinary).unwrap().imports[0].symbols,
        ["FixtureCall"]
    );
}

#[test]
fn export_forwarders_are_included_and_bad_export_ranges_refuse() {
    let mut b = fixture(None, false, false);
    put32(&mut b, 0x98 + 112, 0x1000);
    put32(&mut b, 0x98 + 116, 0x100);
    put32(&mut b, 0x200 + 20, 1);
    put32(&mut b, 0x200 + 28, 0x1050);
    put32(&mut b, 0x250, 0x1080);
    let forwarder = b"shroudforge-runtime.FixtureCall";
    b[0x280..0x280 + forwarder.len()].copy_from_slice(forwarder);
    let parsed = pe_gate::parse(&b).unwrap();
    assert_eq!(parsed.imports[0].kind, ImportKind::ForwardedExport);
    assert!(matches!(
        one(&b).verdict,
        GateVerdict::Banned {
            kind: ImportKind::ForwardedExport,
            ..
        }
    ));
    put32(&mut b, 0x200 + 28, 0);
    assert!(pe_gate::parse(&b).is_err());
}

#[test]
fn unresolved_dependencies_and_wide_dynamic_hints_are_unknown() {
    let b = fixture(Some("missing.dll"), false, false);
    let reviews = [ReviewedImage {
        path: "entry.dll",
        bytes: &b,
        contract: "fixture",
        dynamic_lookups_reviewed: false,
    }];
    assert!(matches!(
        pe_gate::gate(
            "entry.dll",
            &[Image {
                path: "entry.dll",
                bytes: &b
            }],
            &Policy {
                reviews: &reviews,
                system_modules: &[]
            }
        )
        .verdict,
        GateVerdict::UnknownDynamic { .. }
    ));
    let mut wide = fixture(None, false, false);
    let text: Vec<u8> = "LoadLibraryW"
        .encode_utf16()
        .flat_map(u16::to_le_bytes)
        .collect();
    wide[0x800..0x800 + text.len()].copy_from_slice(&text);
    assert!(
        one(&wide)
            .evidence
            .iter()
            .any(|e| e.contains("LoadLibraryW"))
    );
}

#[test]
fn managed_amd64_il_only_and_mixed_mode_are_not_native_admission() {
    for flags in [1, 0] {
        let mut b = fixture(None, false, false);
        put32(&mut b, 0x98 + 112 + 14 * 8, 0x1000);
        put32(&mut b, 0x98 + 116 + 14 * 8, 72);
        put32(&mut b, 0x200, 72);
        put32(&mut b, 0x208, 0x1100);
        put32(&mut b, 0x20c, 4);
        put32(&mut b, 0x210, flags);
        b[0x300..0x304].copy_from_slice(b"BSJB");
        assert!(
            pe_gate::parse(&b).is_err(),
            "managed flags {flags} accepted"
        );
    }
}

#[test]
fn zero_initialized_exported_data_uses_virtual_not_file_bounds() {
    let mut b = fixture(None, false, false);
    put32(&mut b, 0x188 + 8, 0x1000);
    put32(&mut b, 0x98 + 112, 0x1000);
    put32(&mut b, 0x98 + 116, 0x100);
    put32(&mut b, 0x200 + 20, 1);
    put32(&mut b, 0x200 + 28, 0x1050);
    put32(&mut b, 0x250, 0x1f00);
    assert!(
        pe_gate::parse(&b).is_ok(),
        "valid zero-fill DATA export rejected"
    );
    put32(&mut b, 0x250, 0x2100);
    assert!(pe_gate::parse(&b).is_err());
}

#[test]
fn cyclic_import_graph_terminates_and_case_collision_is_unknown() {
    let a = fixture(Some("b.dll"), false, false);
    let b = fixture(Some("a.dll"), false, false);
    let images = [
        Image {
            path: "a.dll",
            bytes: &a,
        },
        Image {
            path: "b.dll",
            bytes: &b,
        },
    ];
    let reviews = [
        ReviewedImage {
            path: "a.dll",
            bytes: &a,
            contract: "fixture a",
            dynamic_lookups_reviewed: false,
        },
        ReviewedImage {
            path: "b.dll",
            bytes: &b,
            contract: "fixture b",
            dynamic_lookups_reviewed: false,
        },
    ];
    let report = pe_gate::gate(
        "a.dll",
        &images,
        &Policy {
            reviews: &reviews,
            system_modules: &[],
        },
    );
    assert_eq!(report.verdict, GateVerdict::Pass);
    assert_eq!(report.inspected.len(), 2);
    assert!(matches!(
        pe_gate::gate(
            "a.dll",
            &[
                Image {
                    path: "a.dll",
                    bytes: &a
                },
                Image {
                    path: "A.DLL",
                    bytes: &a
                }
            ],
            &Policy::default()
        )
        .verdict,
        GateVerdict::UnknownDynamic { .. }
    ));
}
