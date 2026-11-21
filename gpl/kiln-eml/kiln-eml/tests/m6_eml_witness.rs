// SPDX-License-Identifier: GPL-3.0-or-later
//! Current-dialect EML witnesses through unchanged discover -> runner -> mod-loader-lua::run_with.
//! No SF oracle or production interpreter parity is asserted by these Brabb3l tests.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;

use fixture::{
    archive::{self, ArchiveFixture, validate},
    types,
};
use kfc::{
    guid::{Guid, ResourceId},
    hash::fnv,
};
use kiln_eml::runner::Phase;
use std::{collections::BTreeMap, fs};

const CONTENT: &str = r#"game.assets.create_content(buffer.wrap("m6 created content"))"#;

fn witness(tag: &str) -> ArchiveFixture {
    let f = ArchiveFixture::new(tag);
    let package = f.package(
        "witness",
        include_str!("fixtures/m6/witness.lua"),
        &["patch", "export"],
    );
    fs::write(
        package.join("src/alias.lua"),
        include_str!("fixtures/m6/alias.lua"),
    )
    .unwrap();
    f
}

#[test]
fn current_eml_bindings_keep_lookup_visibility_alias_clone_edits_arities_and_refusals() {
    // These are old-dialect evidence, not ShroudForge API rows.
    let f = witness("old-eml-bindings");
    let before = f.semantics();
    let out = f.run_packages(&["witness"]);
    assert_eq!(
        (out.phase, out.error.as_deref(), out.patched),
        (Phase::Done, None, 2),
        "{out:?}"
    );
    assert_eq!(
        out.mods[0].state, "loaded",
        "Lua assertions must actually execute: {out:?}"
    );
    assert_eq!(
        fs::read(f.root.0.join("export/completed.txt")).unwrap(),
        b"m6 old-eml witness complete"
    );
    let after = f.semantics();
    let expected: serde_json::Value =
        serde_json::from_str(include_str!("fixtures/m6/expected-root.json")).unwrap();
    assert_eq!(
        after.resources[&archive::resource_key(&archive::resource_id(0))],
        expected
    );
    for part in [3, 9] {
        let id = archive::resource_key(&archive::resource_id(part));
        assert_eq!(
            after.resources[&id], before.resources[&id],
            "untouched part {part}"
        );
    }
    let created = archive::resource_key(&ResourceId::new(
        Guid::parse(archive::CREATED_GUID).unwrap(),
        fnv(types::CHILD),
        5,
    ));
    assert_eq!(
        after.resources[&created],
        serde_json::json!({"base":7,"extra":151})
    );
    assert_eq!((after.resources.len(), after.contents.len()), (4, 4));
    archive::assert_original_prefix(&f);
    let mut raw_errors = BTreeMap::new();
    for label in [
        "missing-type",
        "missing-part",
        "invalid-guid",
        "static-array",
        "invalid-field",
        "duplicate-resource",
        "readonly-content",
    ] {
        raw_errors.insert(
            label,
            fs::read_to_string(f.root.0.join(format!("export/errors/{label}.txt"))).unwrap(),
        );
    }
    // Raw errors remain raw; no private normalization or auto-generated expected transcript.
    fixture::save_observation(
        "old-eml-semantic",
        &serde_json::json!({"before":before,"after":after,"rawErrors":raw_errors}),
    );
    fixture::save_observation(
        "old-eml-physical",
        &serde_json::to_value(validate(&f.game, archive::STEM).unwrap()).unwrap(),
    );
}

#[test]
fn owned_output_has_real_sealed_backups_and_no_mods_restore_exactly_once() {
    let f = witness("old-eml-restore");
    let out = f.run_packages(&["witness"]);
    assert_eq!(out.mods[0].state, "loaded", "{out:?}");
    for extension in ["kfc", "kfc_resources"] {
        let name = format!("enshrouded.{extension}");
        assert_eq!(
            fs::read(f.game.join(format!("{name}.bak"))).unwrap(),
            f.original.0[&name]
        );
    }
    let seal: serde_json::Value =
        serde_json::from_slice(&fs::read(f.backup_seal()).unwrap()).unwrap();
    assert_eq!(seal["header"][0], f.original.0["enshrouded.kfc"].len());
    assert_eq!(
        seal["resources"][0],
        f.original.0["enshrouded.kfc_resources"].len()
    );
    let receipt: serde_json::Value =
        serde_json::from_slice(&fs::read(f.receipt()).unwrap()).unwrap();
    assert!(
        receipt["files"]
            .as_object()
            .unwrap()
            .contains_key("enshrouded_004.dat")
    );
    let out = f.run_packages(&[]);
    assert_eq!((out.phase, out.restored), (Phase::Idle, true), "{out:?}");
    archive::assert_restored(&f);
    for i in 4..8 {
        assert!(!f.game.join(format!("enshrouded_{i:03}.dat")).exists());
    }
    assert!(!f.run_packages(&[]).restored);
}

#[test]
fn foreign_output_is_left_byte_identical_without_ownership_proof() {
    let f = ArchiveFixture::new("old-eml-foreign");
    f.package("patch", CONTENT, &["patch"]);
    assert_eq!(f.run_packages(&["patch"]).mods[0].state, "loaded");
    f.forget_ownership();
    let foreign = validate(&f.game, archive::STEM).unwrap();
    for _ in 0..2 {
        let out = f.run_packages(&[]);
        assert_eq!((out.phase, out.restored), (Phase::Idle, false), "{out:?}");
        assert!(
            out.reason
                .as_deref()
                .unwrap()
                .contains("no ownership receipt")
        );
        assert_eq!(validate(&f.game, archive::STEM).unwrap(), foreign);
    }
}

#[test]
fn existing_sf_handoff_invalidates_only_the_applied_claim_and_keeps_original_bytes() {
    let f = ArchiveFixture::new("old-eml-handoff");
    let state = f.sf_applied();
    let old = fs::read_to_string(&state).unwrap();
    let config = f.game.join("shroudforge/config/modloader-config.json");
    let config_bytes = b"{ \"unrelated\": \"synthetic\" }\n";
    fs::write(&config, config_bytes).unwrap();
    f.package("patch", CONTENT, &["patch"]);
    let out = f.run_packages(&["patch"]);
    assert_eq!(out.mods[0].state, "loaded", "{out:?}");
    assert_eq!(out.sf_displaced, ["synthetic-sf 1.0.0"]);
    let value: serde_json::Value = serde_json::from_slice(&fs::read(&state).unwrap()).unwrap();
    assert_eq!(value["assets"]["status"], "failed");
    assert_eq!(value["unrelated"]["preserve"], "synthetic setting");
    assert_eq!(fs::read(config).unwrap(), config_bytes);
    let journals: Vec<_> = fs::read_dir(f.game.join("kiln/eml/ownership"))
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| {
            p.file_name()
                .unwrap()
                .to_string_lossy()
                .starts_with("sf-handoff-")
        })
        .collect();
    assert_eq!(journals.len(), 1);
    let journal: serde_json::Value =
        serde_json::from_slice(&fs::read(&journals[0]).unwrap()).unwrap();
    assert_eq!(journal["original"], old);
    assert_eq!(journal["replacement"], fs::read_to_string(&state).unwrap());
    archive::assert_original_prefix(&f);
    assert!(f.run_packages(&[]).restored);
    archive::assert_restored(&f);
    assert_eq!(
        fs::read_to_string(&state).unwrap(),
        journal["replacement"],
        "restore never resurrects an applied claim"
    );
}

#[test]
fn busy_or_unknown_sf_state_prevents_any_pair_or_backup_changes() {
    for busy in [false, true] {
        let f = ArchiveFixture::new("old-eml-refused-handoff");
        let state = f.sf_applied();
        f.package("patch", CONTENT, &["patch"]);
        let lock = if busy {
            let file = fs::File::create(f.game.join("shroudforge/config/.shroudforge-write.lock"))
                .unwrap();
            file.lock().unwrap();
            Some(file)
        } else {
            let mut value: serde_json::Value =
                serde_json::from_slice(&fs::read(&state).unwrap()).unwrap();
            value["schemaVersion"] = 99.into();
            fs::write(&state, serde_json::to_vec(&value).unwrap()).unwrap();
            None
        };
        let state_bytes = fs::read(&state).unwrap();
        let out = f.run_packages(&["patch"]);
        assert_eq!(out.phase, Phase::Failed, "{out:?}");
        assert!(out.error.as_deref().unwrap().contains(if busy {
            "lock"
        } else {
            "unknown ShroudForge state schema"
        }));
        assert_eq!(validate(&f.game, archive::STEM).unwrap(), f.original);
        assert_eq!(fs::read(state).unwrap(), state_bytes);
        assert!(!f.game.join("enshrouded.kfc.bak").exists());
        assert!(!f.receipt().exists());
        drop(lock);
    }
}

#[test]
fn independent_runs_compare_semantics_not_randomized_physical_serialization_order() {
    let a = witness("semantic-a");
    let b = witness("semantic-b");
    let path_a = a.root.0.join("packages/witness");
    let path_b = b.root.0.join("packages/witness");
    for file in ["mod.json", "src/mod.lua", "src/alias.lua"] {
        assert_eq!(
            fs::read(path_a.join(file)).unwrap(),
            fs::read(path_b.join(file)).unwrap()
        );
    }
    assert_eq!(a.run_packages(&["witness"]).mods[0].state, "loaded");
    assert_eq!(b.run_packages(&["witness"]).mods[0].state, "loaded");
    assert_eq!(a.semantics(), b.semantics());
    validate(&a.game, archive::STEM).unwrap();
    validate(&b.game, archive::STEM).unwrap();
}
