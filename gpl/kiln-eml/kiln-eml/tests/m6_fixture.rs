// SPDX-License-Identifier: GPL-3.0-or-later
//! Fixture infrastructure only: valid synthetic sets and rejection of broken physical references.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;

use fixture::{
    archive::{self, ArchiveFixture, validate},
    types,
};
use kfc::{
    container::KFCReader,
    guid::ContentHash,
    reflection::{LookupKey, PrimitiveType, TypeFlags},
    resource::value::Value,
};
use std::{fs, io::Write};

#[test]
fn valid_set_roundtrips_every_shape_and_explicit_part() {
    let f = ArchiveFixture::new("valid");
    let index = f.index();
    assert_eq!(index.game_version(), types::VERSION);
    assert_eq!(index.resources().len(), 3);
    assert_eq!(index.containers().len(), 4);
    assert_eq!(index.containers()[3].size, 0, "writer-created padding DAT");
    for part in [0, 3, 9] {
        assert_eq!(f.resource_value(part), types::root_value());
    }
    for t in f.types.iter() {
        assert_ne!(t.qualified_hash, t.internal_hash);
    }
    let base = f
        .types
        .get_by_name(LookupKey::Qualified("m6.Base"))
        .unwrap();
    let child = f
        .types
        .get_by_name(LookupKey::Qualified(types::CHILD))
        .unwrap();
    assert!(f.types.is_sub_type(base, child));
    assert_eq!(
        f.types
            .iter()
            .filter(|t| t.flags.contains(TypeFlags::HAS_DS))
            .count(),
        4
    );
    for primitive in [
        PrimitiveType::StaticArray,
        PrimitiveType::BlobArray,
        PrimitiveType::BlobOptional,
        PrimitiveType::BlobVariant,
    ] {
        assert!(f.types.iter().any(|t| t.primitive_type == primitive));
    }
    let semantics = f.semantics();
    assert_eq!(
        (semantics.resources.len(), semantics.contents.len()),
        (3, 3)
    );
    for (name, bytes) in &f.original.0 {
        assert_eq!(
            fs::read(f.root.0.join("original").join(name)).unwrap(),
            *bytes
        );
    }
}

#[test]
fn roots_are_unique_and_only_their_own_files_are_cleaned() {
    let a = ArchiveFixture::new("same-tag");
    let b = ArchiveFixture::new("same-tag");
    assert_ne!(a.root.0, b.root.0);
    let path = a.root.0.clone();
    drop(a);
    assert!(!path.exists());
    validate(&b.game, archive::STEM).unwrap();
    let path = b.root.0.clone();
    drop(b);
    assert!(!path.exists());
}

#[test]
fn every_baseline_file_role_is_required_and_truncation_is_detected() {
    for name in ArchiveFixture::new("role-names").original.0.keys() {
        for omit in [false, true] {
            let f = ArchiveFixture::new("missing-or-short");
            let path = f.game.join(name);
            if omit {
                fs::remove_file(&path).unwrap();
            } else if path.metadata().unwrap().len() == 0 {
                fs::write(&path, b"not empty").unwrap();
            } else {
                fs::OpenOptions::new()
                    .write(true)
                    .open(&path)
                    .unwrap()
                    .set_len(1)
                    .unwrap();
            }
            assert!(
                validate(&f.game, archive::STEM).is_err(),
                "accepted {name}, omit={omit}"
            );
        }
    }
}

#[test]
fn corruption_of_header_companion_content_and_padding_is_detected() {
    for name in ArchiveFixture::new("corruption-names").original.0.keys() {
        let f = ArchiveFixture::new("corrupt-role");
        let path = f.game.join(name);
        if path.metadata().unwrap().len() == 0 {
            fs::write(&path, b"unexpected padding").unwrap();
        } else {
            archive::corrupt_first_byte(&path);
        }
        assert!(
            validate(&f.game, archive::STEM).is_err(),
            "accepted corrupt {name}"
        );
    }
}

#[test]
fn out_of_range_content_resource_chunk_and_index_declarations_are_rejected() {
    // Slots/widths are KFCHeader's documented GPL layout, not game-derived offsets.
    for (table, within, value, expected) in [
        (8, 0, u32::MAX, "content"),
        (11, 0, u32::MAX, "resource"),
        (15, 0, u32::MAX, "companion"),
        (15, 8, u32::MAX, "compressed chunk"),
    ] {
        let f = ArchiveFixture::new("bad-range");
        archive::change_table_u32(&f.game, table, within, value);
        let error = validate(&f.game, archive::STEM).unwrap_err();
        assert!(error.to_string().contains(expected), "{error:#}");
    }
    for offset in [4, 24] {
        // index declared length; container-table relative offset
        let f = ArchiveFixture::new("bad-header-size");
        let path = f.game.join("enshrouded.kfc");
        let mut bytes = fs::read(&path).unwrap();
        bytes[offset..offset + 4].copy_from_slice(&u32::MAX.to_le_bytes());
        fs::write(path, bytes).unwrap();
        assert!(validate(&f.game, archive::STEM).is_err());
    }
}

#[test]
fn content_value_table_must_fit_declared_index_including_padding() {
    let f = ArchiveFixture::new("content-table-range");
    let path = f.game.join("enshrouded.kfc");
    let mut bytes = fs::read(&path).unwrap();
    let declared = u32::from_le_bytes(bytes[4..8].try_into().unwrap()) as usize;
    let (source, count) = archive::table_location(&bytes, 8);
    assert_eq!(count, archive::BLOBS.len());
    // ContentEntry's eight field bytes plus eight padding bytes occupy 16 bytes on disk
    // (eml/kfc-base/src/container/header.rs:294-343, identical in both SF parser pins).
    let size = count * 16;
    let relocated = declared.next_multiple_of(16);
    let end = relocated + size;
    assert!(source + size <= declared && end <= bytes.len());
    assert!(bytes[relocated..end].iter().all(|byte| *byte == 0));
    bytes.copy_within(source..source + size, relocated);
    let field = 16 + 8 * 8;
    bytes[field..field + 4].copy_from_slice(&((relocated - field) as u32).to_le_bytes());
    bytes[4..8].copy_from_slice(&(end as u32).to_le_bytes());
    fs::write(&path, &bytes).unwrap();
    validate(&f.game, archive::STEM).unwrap(); // The exact full extent is valid; file padding remains.

    // Keep all complete records physically readable, but declare only eight bytes per record.
    bytes[4..8].copy_from_slice(&((relocated + count * 8) as u32).to_le_bytes());
    fs::write(&path, &bytes).unwrap();
    let error = match validate(&f.game, archive::STEM) {
        Ok(_) => panic!("accepted content-value table beyond declared index"),
        Err(error) => error,
    };
    assert_eq!(
        error.to_string(),
        "index table: referenced range exceeds declared size"
    );
}

#[test]
fn vanilla_header_can_read_its_immutable_prefix_after_companion_publication() {
    // transaction.rs publishes the vanilla header, then the longer companion, then the new header.
    // Unreferenced companion tail and DATs are not broken references in that safe-base window.
    let f = ArchiveFixture::new("vanilla-barrier");
    f.package(
        "patch",
        r#"game.assets.create_content(buffer.wrap("m6 created content"))"#,
        &["patch"],
    );
    assert_eq!(f.run_packages(&["patch"]).mods[0].state, "loaded");
    archive::assert_original_prefix(&f);
    fs::write(
        f.game.join("enshrouded.kfc"),
        &f.original.0["enshrouded.kfc"],
    )
    .unwrap();
    validate(&f.game, archive::STEM).unwrap();
    assert_eq!(f.resource_value(0), types::root_value());
    assert_eq!(f.semantics().contents.len(), 3);
}

#[test]
fn declared_uncompressed_chunk_size_must_match_actual_decode() {
    let f = ArchiveFixture::new("bad-logical-size");
    let size = f.index().resource_chunks()[0].uncompressed_size as u32;
    archive::change_table_u32(&f.game, 15, 16, size + 1);
    let error = validate(&f.game, archive::STEM).unwrap_err();
    assert!(
        error.to_string().contains("decoded chunk size"),
        "{error:#}"
    );
}

#[test]
fn patch_has_generated_and_padding_dats_and_preserves_vanilla_prefix() {
    let f = ArchiveFixture::new("generated");
    f.package(
        "patch",
        r#"game.assets.create_content(buffer.wrap("m6 created content"))"#,
        &["patch"],
    );
    let out = f.run_packages(&["patch"]);
    assert_eq!(out.phase, kiln_eml::runner::Phase::Done, "{out:?}");
    assert_eq!(out.mods[0].state, "loaded", "{out:?}");
    let patched = validate(&f.game, archive::STEM).unwrap();
    assert_ne!(patched, f.original);
    let index = f.index();
    assert_eq!(index.containers().len(), 8);
    assert!(index.containers()[4].size > 0);
    assert!(index.containers()[5..].iter().all(|c| c.size == 0));
    archive::assert_original_prefix(&f);
    assert!(f.receipt().exists() && f.backup_seal().exists() && f.change_cache().exists());
    let mut reader = KFCReader::new(&f.game, archive::STEM)
        .unwrap()
        .into_cursor()
        .unwrap();
    assert_eq!(
        reader
            .read_content(&ContentHash::from_data(archive::NEW_CONTENT))
            .unwrap()
            .unwrap(),
        archive::NEW_CONTENT
    );
    drop(reader);
    for name in [
        "enshrouded_004.dat",
        "enshrouded_005.dat",
        "enshrouded_006.dat",
        "enshrouded_007.dat",
    ] {
        let bytes = fs::read(f.game.join(name)).unwrap();
        fs::remove_file(f.game.join(name)).unwrap();
        assert!(
            validate(&f.game, archive::STEM).is_err(),
            "accepted missing {name}"
        );
        fs::write(f.game.join(name), &bytes).unwrap();
        fs::OpenOptions::new()
            .append(true)
            .open(f.game.join(name))
            .unwrap()
            .write_all(b"bad length")
            .unwrap();
        assert!(
            validate(&f.game, archive::STEM).is_err(),
            "accepted wrong size {name}"
        );
        fs::write(f.game.join(name), &bytes).unwrap();
        if !bytes.is_empty() {
            fs::OpenOptions::new()
                .write(true)
                .open(f.game.join(name))
                .unwrap()
                .set_len(1)
                .unwrap();
            assert!(
                validate(&f.game, archive::STEM).is_err(),
                "accepted truncated {name}"
            );
            fs::write(f.game.join(name), &bytes).unwrap();
            archive::corrupt_first_byte(&f.game.join(name));
            assert!(
                validate(&f.game, archive::STEM).is_err(),
                "accepted corrupt {name}"
            );
            fs::write(f.game.join(name), bytes).unwrap();
        }
    }
    assert!(f.run_packages(&[]).restored);
    archive::assert_restored(&f);
    for i in 4..8 {
        assert!(!f.game.join(format!("enshrouded_{i:03}.dat")).exists());
    }
    assert!(!f.run_packages(&[]).restored, "restore is idempotent");
}

#[test]
fn empty_optional_and_zero_variant_follow_existing_parser_contract() {
    let registry = types::registry();
    let mut value = types::root_value();
    value
        .as_struct_mut()
        .unwrap()
        .insert("maybe".into(), Value::None);
    value
        .as_struct_mut()
        .unwrap()
        .insert("choice".into(), Value::None);
    value
        .as_struct_mut()
        .unwrap()
        .insert("children".into(), Value::Array(Vec::new()));
    let ty = registry
        .get_by_name(LookupKey::Qualified(types::ROOT))
        .unwrap();
    let bytes = value.to_bytes(&registry, ty).unwrap();
    // The current parser maps a zero variant to its base type over the zero header, not to Value::None
    // (eml/kfc-resource/src/mapped/mod.rs:574-595). Record that old-dialect behavior, not an SF expectation.
    assert_eq!(&bytes[28..40], &[0; 12]);
    value.as_struct_mut().unwrap().insert(
        "choice".into(),
        Value::Variant(Box::new(kfc::resource::value::Variant {
            type_index: kfc::reflection::TypeIndex::new(1),
            value: types::structure([("base", Value::UInt(0))])
                .into_struct()
                .unwrap(),
        })),
    );
    assert_eq!(Value::from_bytes(&registry, ty, bytes).unwrap(), value);
}

#[test]
fn validator_rejects_a_truncated_referenced_dat() {
    let f = ArchiveFixture::new("truncated-dat");
    validate(&f.game, fixture::archive::STEM).unwrap();
    let dat = f.game.join("enshrouded_000.dat");
    std::fs::OpenOptions::new()
        .write(true)
        .open(dat)
        .unwrap()
        .set_len(1)
        .unwrap();
    let error = validate(&f.game, fixture::archive::STEM).unwrap_err();
    assert!(error.to_string().contains("declared size"), "{error:#}");
}
