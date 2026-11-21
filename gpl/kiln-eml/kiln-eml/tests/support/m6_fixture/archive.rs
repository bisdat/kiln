// SPDX-License-Identifier: GPL-3.0-or-later
//! GPL test-only physical validator, NOT a new publisher or a promise of game acceptance.
//! Structure sources: eml/kfc-base/src/container/{header,file,reader,writer}.rs.
use super::{TestRoot, types};
use anyhow::{Context, ensure};
use camino::{Utf8Path, Utf8PathBuf};
use kfc::{
    container::{KFCFile, KFCReader, KFCWriteOptions, KFCWriter},
    guid::{ContentHash, Guid, ResourceId},
    hash::fnv,
    reflection::{LookupKey, TypeRegistry},
    resource::value::Value,
};
use std::{
    collections::BTreeMap,
    fs,
    io::{Read, Seek, SeekFrom, Write},
    rc::Rc,
};

pub const STEM: &str = "enshrouded";
pub const RESOURCE_GUID: &str = "01234567-89ab-cdef-0123-456789abcdef";
pub const CREATED_GUID: &str = "fedcba98-7654-3210-fedc-ba9876543210";
pub const BLOBS: [&[u8]; 3] = [b"m6 vanilla one", b"m6 vanilla two", b"m6 vanilla three"];
pub const NEW_CONTENT: &[u8] = b"m6 created content";

/// Exact physical bytes are recorded independently from decoded values. HashMap serialization order is not parity.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
pub struct PhysicalSet(pub BTreeMap<String, Vec<u8>>);
#[derive(Debug, Clone, PartialEq, serde::Serialize)]
pub struct SemanticSet {
    pub resources: BTreeMap<String, serde_json::Value>,
    pub contents: BTreeMap<String, Vec<u8>>,
}

pub struct ArchiveFixture {
    pub root: TestRoot,
    pub game: Utf8PathBuf,
    pub types: Rc<TypeRegistry>,
    pub original: PhysicalSet,
}
impl ArchiveFixture {
    pub fn new(tag: &str) -> Self {
        let types = Rc::new(types::registry());
        let ty = types
            .get_by_name(LookupKey::Qualified(types::ROOT))
            .unwrap();
        let bytes = types::root_value().to_bytes(&types, ty).unwrap();
        // Deliberately not ascending parts. Tests compare catalog membership, not randomized storage order.
        let resources: Vec<_> = [9, 0, 3]
            .into_iter()
            .map(|part| (resource_id(part), bytes.clone()))
            .collect();
        Self::with_contents(tag, types, &resources, &BLOBS)
    }
    /// A game folder whose archive holds `resources` and `blobs` over `types` (other synthetic registries).
    pub fn with_contents(
        tag: &str,
        types: Rc<TypeRegistry>,
        resources: &[(ResourceId, Vec<u8>)],
        blobs: &[&[u8]],
    ) -> Self {
        let root = TestRoot::new(tag);
        let game = root.0.join("game");
        fs::create_dir(&game).unwrap();
        fs::File::create(game.join(format!("{STEM}.kfc"))).unwrap();
        fs::File::create(game.join(format!("{STEM}.kfc_resources"))).unwrap();
        let mut writer = KFCWriter::<Rc<KFCFile>, _>::new_with_options(
            &game,
            STEM,
            types.clone(),
            &types.version,
            KFCWriteOptions {
                max_container_size: 1,
                truncate_containers: true,
                ..Default::default()
            },
        )
        .unwrap();
        for (id, bytes) in resources {
            writer.write_resource(id, bytes).unwrap();
        }
        for blob in blobs {
            writer
                .write_content(&ContentHash::from_data(blob), blob)
                .unwrap();
        }
        writer.finalize().unwrap();
        let cache = game.join("kiln/eml/cache");
        fs::create_dir_all(&cache).unwrap();
        fs::write(
            cache.join(format!("{STEM}-types.json")),
            serde_json::to_vec(&*types).unwrap(),
        )
        .unwrap();
        let original = validate(&game, STEM).unwrap();
        let originals = root.0.join("original");
        fs::create_dir(&originals).unwrap();
        for (name, bytes) in &original.0 {
            fs::write(originals.join(name), bytes).unwrap();
        }
        Self {
            root,
            game,
            types,
            original,
        }
    }
    pub fn index(&self) -> KFCFile {
        KFCFile::from_path(self.game.join(format!("{STEM}.kfc")), false).unwrap()
    }
    pub fn resource_value(&self, part: u32) -> Value {
        let bytes = KFCReader::new(&self.game, STEM)
            .unwrap()
            .into_cursor()
            .unwrap()
            .read_resource(&resource_id(part))
            .unwrap()
            .unwrap();
        Value::from_bytes(
            &self.types,
            self.types
                .get_by_name(LookupKey::Qualified(types::ROOT))
                .unwrap(),
            bytes,
        )
        .unwrap()
    }
    pub fn semantics(&self) -> SemanticSet {
        semantics(&self.game, STEM, &self.types).unwrap()
    }
}

pub fn resource_id(part: u32) -> ResourceId {
    ResourceId::new(Guid::parse(RESOURCE_GUID).unwrap(), fnv(types::ROOT), part)
}

/// Display for ResourceId is GUID-only. Never collapse different types/parts in semantic comparisons.
pub fn resource_key(id: &ResourceId) -> String {
    format!("{}/{:08x}/{}", id.guid(), id.type_hash(), id.part_index())
}

fn read_regular(path: &Utf8Path) -> anyhow::Result<Vec<u8>> {
    let meta = fs::symlink_metadata(path).with_context(|| {
        format!(
            "missing referenced file {}",
            path.file_name().unwrap_or("?")
        )
    })?;
    ensure!(
        meta.is_file() && !meta.file_type().is_symlink(),
        "non-regular referenced file {path}"
    );
    fs::read(path).with_context(|| format!("read referenced file {path}"))
}
fn bounded(offset: u64, size: u64, limit: u64, role: &str) -> anyhow::Result<()> {
    ensure!(
        offset.checked_add(size).is_some_and(|end| end <= limit),
        "{role}: referenced range exceeds declared size"
    );
    Ok(())
}
fn u32_at(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}
/// KFCHeader's public layout documentation, resolved relative offsets. GPL test infrastructure only.
pub fn table_location(bytes: &[u8], table: usize) -> (usize, usize) {
    let field = 16 + table * 8;
    (
        field + u32_at(bytes, field) as usize,
        u32_at(bytes, field + 4) as usize,
    )
}

pub fn validate(game: &Utf8Path, stem: &str) -> anyhow::Result<PhysicalSet> {
    let header_name = format!("{stem}.kfc");
    let header = read_regular(&game.join(&header_name))?;
    ensure!(
        header.len() >= 144 && &header[..4] == b"KFC4",
        "invalid KFC4 header"
    );
    let declared = u32_at(&header, 4) as u64;
    ensure!(
        declared >= 144 && declared <= header.len() as u64,
        "index declared size exceeds file"
    );
    // Check ranges BEFORE parser allocation/indexing. Unused header slots intentionally have no table.
    let widths = [1, 16, 0, 0, 12, 4, 8, 16, 16, 8, 32, 8, 8, 4, 12, 20];
    for (table, width) in widths.into_iter().enumerate().filter(|(_, w)| *w != 0) {
        let (offset, count) = table_location(&header, table);
        bounded(offset as u64, count as u64 * width, declared, "index table")?;
    }
    let file = KFCFile::from_path(game.join(&header_name), false).context("reopen index")?;
    let (indices, index_count) = table_location(&header, 5);
    for i in 0..index_count {
        ensure!(
            (u32_at(&header, indices + i * 4) as usize) < file.resources().len(),
            "resource index out of range"
        );
    }
    let mut covered = vec![false; file.resources().len()];
    for (type_hash, bundle) in file.resource_bundles().iter() {
        bounded(
            bundle.index as u64,
            bundle.count as u64,
            index_count as u64,
            "resource bundle",
        )?;
        for i in bundle.index..bundle.index + bundle.count {
            let index = u32_at(&header, indices + i * 4) as usize;
            ensure!(
                !covered[index] && file.resources().keys()[index].type_hash() == *type_hash,
                "resource bundle identity mismatch"
            );
            covered[index] = true;
        }
    }
    ensure!(
        covered.iter().all(|present| *present),
        "resource missing from bundles"
    );
    let (location, location_count) = table_location(&header, 4);
    ensure!(
        location_count == 1,
        "synthetic validator requires one resource location"
    );
    let declared_location = [
        u32_at(&header, location),
        u32_at(&header, location + 4),
        u32_at(&header, location + 8),
    ];
    let mut physical = BTreeMap::from([(header_name, header)]);
    let resource_name = format!("{stem}.kfc_resources");
    let resources = read_regular(&game.join(&resource_name))?;
    let mut logical_end = 0u64;
    for chunk in file.resource_chunks() {
        bounded(
            chunk.offset,
            chunk.size,
            resources.len() as u64,
            "resource companion chunk",
        )?;
        ensure!(
            chunk.compressed_size <= chunk.size,
            "compressed chunk exceeds declared size"
        );
        ensure!(
            chunk.uncompressed_offset == logical_end,
            "non-contiguous logical resource chunks"
        );
        logical_end = logical_end
            .checked_add(chunk.uncompressed_size)
            .context("logical chunk overflow")?;
    }
    // Header-last publication may leave an unreferenced appended companion tail under the vanilla
    // header. Its referenced prefix must decode exactly; unlike DATs, total companion length is not
    // declared by the header. The physical snapshot still retains every byte for ownership checks.
    validate_decoded_chunks(&file, &resources)?;
    ensure!(
        declared_location[0] as u64
            == file
                .resource_chunks()
                .iter()
                .map(|c| c.uncompressed_size)
                .sum::<u64>(),
        "resource location uncompressed declared size mismatch"
    );
    ensure!(
        declared_location[1] as u64
            == file
                .resource_chunks()
                .iter()
                .map(|c| c.compressed_size)
                .sum::<u64>(),
        "resource location compressed declared size mismatch"
    );
    ensure!(
        declared_location[2] as usize == file.resources().len(),
        "resource location count mismatch"
    );
    physical.insert(resource_name, resources);
    for (i, container) in file.containers().iter().enumerate() {
        let name = format!("{stem}_{i:03}.dat");
        let bytes = read_regular(&game.join(&name))?;
        ensure!(
            bytes.len() as u64 == container.size,
            "{name}: declared size {} != actual {}",
            container.size,
            bytes.len()
        );
        let count = file
            .contents()
            .values()
            .iter()
            .filter(|e| e.container_index == i)
            .count();
        ensure!(
            count == container.count,
            "{name}: declared content count mismatch"
        );
        physical.insert(name, bytes);
    }
    let mut cursor = KFCReader::new(game, stem)?.into_cursor()?;
    for (hash, entry) in file.contents().iter() {
        let container = file
            .containers()
            .get(entry.container_index)
            .context("content references missing container")?;
        bounded(entry.offset, hash.size() as u64, container.size, "content")?;
        let bytes = cursor.read_content(hash)?.context("missing content")?;
        ensure!(
            ContentHash::from_data(&bytes) == *hash,
            "content hash mismatch"
        );
    }
    for (id, entry) in file.resources().iter() {
        bounded(entry.offset, entry.size, logical_end, "resource")?;
        let bytes = cursor
            .read_resource(id)?
            .context("resource has no covering chunk")?;
        ensure!(
            bytes.len() as u64 == entry.size,
            "resource decoded size mismatch"
        );
    }
    Ok(PhysicalSet(physical))
}

/// Exercise the existing GPL decoder for *every* chunk, including empty/unreferenced ones.
/// A writer-built one-resource probe reads one byte beyond the declared logical size. Correct decoding
/// therefore returns exactly the declaration; too-short AND too-long frames are rejected. Only the
/// disposable probe's header changes, never the archive being validated. No new zstd dependency/lock.
fn validate_decoded_chunks(index: &KFCFile, companion: &[u8]) -> anyhow::Result<()> {
    let scratch = TestRoot::new("decode-probe");
    let stem = "probe";
    fs::File::create(scratch.0.join("probe.kfc"))?;
    fs::File::create(scratch.0.join("probe.kfc_resources"))?;
    let mut writer = KFCWriter::<Rc<KFCFile>, _>::new(
        &scratch.0,
        stem,
        Rc::new(types::registry()),
        types::VERSION,
    )?;
    writer.write_resource(&resource_id(0), &[0])?;
    writer.finalize()?;
    let template = fs::read(scratch.0.join("probe.kfc"))?;
    fs::write(scratch.0.join("probe.kfc_resources"), companion)?;
    let (resource_table, _) = table_location(&template, 11);
    let (chunk_table, _) = table_location(&template, 15);
    for chunk in index.resource_chunks() {
        let probe_size = chunk
            .uncompressed_size
            .checked_add(1)
            .context("chunk probe size overflow")?;
        ensure!(
            probe_size <= u32::MAX as u64,
            "synthetic chunk too large for probe"
        );
        let mut header = template.clone();
        header[resource_table..resource_table + 4].copy_from_slice(&0u32.to_le_bytes());
        header[resource_table + 4..resource_table + 8]
            .copy_from_slice(&(probe_size as u32).to_le_bytes());
        for (i, value) in [
            chunk.offset,
            chunk.size,
            chunk.compressed_size,
            0,
            probe_size,
        ]
        .into_iter()
        .enumerate()
        {
            header[chunk_table + i * 4..chunk_table + i * 4 + 4]
                .copy_from_slice(&(value as u32).to_le_bytes());
        }
        fs::write(scratch.0.join("probe.kfc"), header)?;
        let bytes = KFCReader::new(&scratch.0, stem)?
            .into_cursor()?
            .read_resource(&resource_id(0))?
            .context("chunk probe unavailable")?;
        ensure!(
            bytes.len() as u64 == chunk.uncompressed_size,
            "decoded chunk size mismatch"
        );
    }
    Ok(())
}

pub fn semantics(game: &Utf8Path, stem: &str, types: &TypeRegistry) -> anyhow::Result<SemanticSet> {
    validate(game, stem)?;
    let file = KFCFile::from_path(game.join(format!("{stem}.kfc")), false)?;
    let mut cursor = KFCReader::new(game, stem)?.into_cursor()?;
    let mut resources = BTreeMap::new();
    let mut contents = BTreeMap::new();
    for id in file.resources().keys() {
        let ty = types
            .get_by_hash(LookupKey::Qualified(id.type_hash()))
            .context("resource type missing")?;
        let bytes = cursor.read_resource(id)?.context("resource missing")?;
        let value = Value::from_bytes(types, ty, bytes)?;
        ensure!(
            resources
                .insert(resource_key(id), serde_json::to_value(value)?)
                .is_none(),
            "duplicate semantic resource identity"
        );
    }
    for id in file.contents().keys() {
        contents.insert(
            id.to_string(),
            cursor.read_content(id)?.context("content missing")?,
        );
    }
    Ok(SemanticSet {
        resources,
        contents,
    })
}

/// Controlled header corruption in disposable synthetic sets, using a public documented relative-table slot.
pub fn change_table_u32(game: &Utf8Path, table: usize, within: usize, value: u32) {
    let path = game.join(format!("{STEM}.kfc"));
    let bytes = fs::read(&path).unwrap();
    let (base, _) = table_location(&bytes, table);
    let mut file = fs::OpenOptions::new().write(true).open(path).unwrap();
    file.seek(SeekFrom::Start((base + within) as u64)).unwrap();
    file.write_all(&value.to_le_bytes()).unwrap();
}

pub fn corrupt_first_byte(path: &Utf8Path) {
    let mut file = fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(path)
        .unwrap();
    let mut byte = [0];
    file.read_exact(&mut byte).unwrap();
    byte[0] ^= 0xff;
    file.rewind().unwrap();
    file.write_all(&byte).unwrap();
}

pub fn assert_original_prefix(f: &ArchiveFixture) {
    let companion = fs::read(f.game.join(format!("{STEM}.kfc_resources"))).unwrap();
    assert!(companion.starts_with(&f.original.0[&format!("{STEM}.kfc_resources")]));
    for (name, bytes) in &f.original.0 {
        if name.ends_with(".dat") {
            assert_eq!(
                fs::read(f.game.join(name)).unwrap(),
                *bytes,
                "immutable vanilla {name}"
            );
        }
    }
}

pub fn assert_restored(f: &ArchiveFixture) {
    let physical = validate(&f.game, STEM).unwrap();
    assert_eq!(physical, f.original);
    // Unreferenced foreign files are intentionally outside validate's physical set.
}
