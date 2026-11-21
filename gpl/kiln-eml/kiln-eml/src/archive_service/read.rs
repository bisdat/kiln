// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln.eml.archive v1's read group: resource_json, find_resource, list_resources, read_resource,
//! content_read_range and list_contents, over the archive the startup controller authorized a generation with
//! (`ArchiveBacking`, `view::authorize_with_archive`).
//!
//! What a generation's archive is:
//! - the REFERENCE catalog: the captured baseline index (`<stem>.kfc.bak`, the one a startup pass's
//!   `StartupHost::reference_reader` reads) over the resources file and DAT containers next to it;
//! - its TRANSACTION entries, from one of two backings the startup controller chooses (`ArchiveBacking::open` or
//!   `ArchiveBacking::open_live`):
//!   - STAGED: the finalized staged resources of this generation (the bytes its pass wrote: created and replaced
//!     records), owned here. A later publication or file change never reaches them;
//!   - LIVE: the published archive as it is on disk (`<stem>.kfc` over the same resources file), for a start that
//!     staged nothing (a cached or unchanged start whose output the ownership receipt proves live). Its transaction
//!     entries are the live index's resources that are not the reference's: a key the reference lacks, or one whose
//!     entry or spanning chunks differ. They are read from the files on each call, with the same checks and refusals
//!     as reference reads. Binding the files to the generation (receipt, `BaselineIdentity`, `archive_sha256`) is the
//!     caller's: like every read here, a later change of the files by another program is not prevented.
//!
//! Visibility follows ShroudForge 1.7.9 (session/asset_resources.rs `get_resource_info`, `get_content`;
//! lua/bindings/game/assets/access_file_resources.rs, access_file_content.rs), as the header says:
//! - find_resource and read_resource see the transaction first, then the reference catalog (a replaced record reads
//!   its new bytes; source TRANSACTION);
//! - list_resources enumerates the reference catalog only (SF's lists walk reference keys), in catalog order or, with
//!   PART order, ascending parts (FINAL's selected compatibility rule);
//! - content_read_range and list_contents see reference contents only, never new staged content or arbitrary files.
//!
//! IO and sizing:
//! - a known size (a resource's index length, a content range's count) answers BUFFER_TOO_SMALL before any IO;
//! - every other read is a real Source operation, a count-zero one included: it opens the file, checks the index's
//!   ranges against it and seeks, and may fail;
//! - resource_json's reply is materialized once per resource for the generation (`Generation::reply`) and kept with
//!   it, so the size probe and the copy answer the same bytes and no second file read rebuilds it;
//! - a failure captures its cause once, as owned bytes behind a fresh error token (`errors`); a later call that fails
//!   on the same materialized reply gets a fresh token over the same bytes, never a second IO. A failed
//!   materialization is kept like a reply: a transient IO failure (on Windows, another program holding the file
//!   without read sharing) keeps that resource's resource_json failing for this generation, while read_resource reads
//!   again on its next call.
//!
//! Results: IO and decode failures answer INTERNAL with their cause. A resource type the registry lacks answers
//! UNAVAILABLE (find_resource, resource_json; read_resource needs no type) and, being ShroudForge's own refusal of the
//! read, keeps its text ("resource type not found in current game registry") behind a token. NOT_FOUND and every other
//! UNAVAILABLE (no archive, no named index) carry no token.
//!
//! Hostile archive bytes are refused with an error record, not a panic or an abort: a resource chunk or content
//! range past the end of its file, a chunk that claims more decompressed bytes than its zstd frames can produce or
//! whose frames could write more than this process can reserve (the reader's own reservations are infallible), a
//! chunk shorter than its index claims, data that does not decode as its type. Resource bytes are read through EML's
//! own `KFCReader` (which, as ShroudForge's session reader does, keeps each decompressed chunk for the generation's
//! life) only after the chunks a read spans were checked; value trees are walked with the codec's bounds (depth
//! `MAX_DEPTH`, at most `mapped_value_limit` values of the resource's length). What the reader decodes is not
//! capped at a chunk's claim: crafted frames can still make it write up to their header bound (128 KiB per 4
//! compressed bytes) before its decoder stops them. Capping that needs a decoder of our own (zstd is not a direct
//! dependency of this crate).
//!
//! Memory a generation holds, for its whole life and without a bound:
//! - every resource chunk a read decompressed, through each index it read (the reference's and, LIVE, the published
//!   one's): at worst the whole decompressed resources file. A chunk is up to its claim (an index field of 32 bits:
//!   under 4 GiB; 8 MiB as EML writes them). ShroudForge keeps the same cache, but closes its reader after the startup
//!   publication; a runtime (PUBLISHED) generation here keeps it as long as the generation lives;
//! - every resource_json reply materialized for it (failures included), each up to tens of times its resource's bytes
//!   (an array of bytes becomes one tagged node per element).
//!
//! The startup controller that authorizes generations with an archive bounds this by their lifetime: retire a
//! generation, or authorize the next one with a fresh `ArchiveBacking`, when its reads are done.
//!
//! The neutral tree resource_json returns (common.schema.json `value`) keeps every shape the bytes have: integer
//! signedness (`u64:`/`i64:` bit patterns), f32 and f64 bits, binary strings, optional presence, variant
//! discriminants (the stored qualified type hash of the selected type), GUIDs (a nil GUID stays a GUID) and object
//! references, with each node's declared type name. Struct fields are a `map` with `string` keys in field order.
//! One shape is not kept: an empty variant (stored hash 0 at blob offset 0) is kfc's default selection of the base's
//! inner type, as ShroudForge's `Variant.type` reports it too, so its discriminant is that type's hash, not the stored
//! 0, and its value is that type mapped where the empty variant's blob would start. Encoding the tree back writes a
//! selected inner type, not the empty variant (the codec slots' value_encode decides whether to keep that). Strings must be UTF-8
//! (kfc's `as_str`, as in ShroudForge): other bytes fail the reply.
//!
//! Named queries (`item:`, `template:`, `skill:`, `material:`) answer from a named index the startup controller gives the
//! archive (`ArchiveBacking::with_named_index`); a kind without one is UNAVAILABLE, never another generation's or an
//! empty answer.
use std::{
    collections::{BTreeMap, BTreeSet},
    ffi::c_char,
    fs::File,
    io::{Read, Seek, SeekFrom},
    path::{Path, PathBuf},
    sync::{Arc, OnceLock},
};

use kfc::{
    container::{KFCFile, KFCReader, KFCReaderOptions},
    guid::{ContentHash, Guid, ResourceId},
    reflection::{LookupKey, TypeHandle, TypeMetadata, TypeRegistry},
    resource::mapped::{MappedStruct, MappedValue},
};
use kiln_abi::{
    KILN_ERR_BUFFER_TOO_SMALL, KILN_ERR_INTERNAL, KILN_ERR_INVALID_ARGUMENT, KILN_ERR_NOT_FOUND, KILN_ERR_REVOKED,
    KILN_ERR_UNAVAILABLE, KILN_OK, KilnOwner, KilnResult, KilnStr,
    services::{
        kiln_assets::{KilnAssetKey, KilnContentHash},
        kiln_eml_archive::{
            KILN_ARCHIVE_FILTER_GUID, KILN_ARCHIVE_FILTER_PART, KILN_ARCHIVE_FILTER_TYPE, KILN_ARCHIVE_ORDER_CATALOG,
            KILN_ARCHIVE_ORDER_PART, KILN_ARCHIVE_SOURCE_FILE, KILN_ARCHIVE_SOURCE_TRANSACTION, KilnArchiveErrorV1,
            KilnArchiveQuery, KilnArchiveResourceInfo, KilnEmlArchiveView,
        },
    },
};

use super::{
    errors::{self, ErrorOut, overlap},
    registry::{string, u64s},
    view::{self, Generation},
};
use crate::sf_startup::resource_values::{MAX_DEPTH, converter::mapped_value_limit};

const QUERY_SIZE: u32 = std::mem::size_of::<KilnArchiveQuery>() as u32;
const INFO_SIZE: u32 = std::mem::size_of::<KilnArchiveResourceInfo>() as u32;
const KEY_STRIDE: usize = std::mem::size_of::<KilnAssetKey>();
const HASH_STRIDE: usize = std::mem::size_of::<KilnContentHash>();
/// RFC 8878: a zstd block decompresses to at most 128 KiB.
const ZSTD_BLOCK_MAX: u64 = 128 * 1024;
/// SF's text when a resource's type is not in the registry (session/asset_resources.rs `type_for_resource`).
const TYPE_MISSING: &str = "resource type not found in current game registry";

/// The named indexes a boot query can ask (kiln.boot's `item:`, `template:`, `skill:` and `material:` forms).
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum NamedIndex {
    Item,
    Template,
    Skill,
    Material,
}

/// One generation's archive: the reference catalog over its files and the generation's own transaction entries.
/// Immutable once built and shared by every view of the generation (`Arc`); safe on any thread.
pub struct ArchiveBacking {
    /// The reference index (`<stem>.kfc.bak`) and its reader of `<stem>.kfc_resources`.
    reference: KFCReader,
    dir: PathBuf,
    stem: String,
    /// STAGED transaction entries (owned bytes); empty for a LIVE backing.
    transaction: BTreeMap<ResourceId, Arc<[u8]>>,
    /// LIVE: the published index and its resources that are not the reference's.
    live: Option<Live>,
    named: BTreeMap<(NamedIndex, String), ResourceId>,
    indexed: Vec<NamedIndex>,
    /// Per resource chunk: checked once against its file and its zstd frames, before the reader decompresses it.
    checked: Vec<OnceLock<()>>,
}

/// A LIVE backing's published index (`<stem>.kfc`), the keys it reads, and its own per-chunk checks.
struct Live {
    index: KFCReader,
    keys: BTreeSet<ResourceId>,
    checked: Vec<OnceLock<()>>,
}

/// An index's chunk table must not overflow; the reader adds these fields unchecked.
fn chunks_fit(reader: &KFCReader) -> bool {
    reader.file().resource_chunks().iter().all(|c| {
        c.uncompressed_offset.checked_add(c.uncompressed_size).is_some()
            && c.offset.checked_add(c.compressed_size).is_some()
    })
}

/// The chunks a resource entry spans (its first and last, as the reader searches them), or None.
fn spanning(reader: &KFCReader, id: &ResourceId) -> Option<(usize, usize)> {
    let file = reader.file();
    let entry = file.resources().get(id)?;
    let size = reader.resource_size(id)?;
    let end = entry.offset.checked_add(size)?;
    let chunks = file.resource_chunks();
    let first = chunks.iter().position(|c| {
        c.uncompressed_offset <= entry.offset && entry.offset < c.uncompressed_offset + c.uncompressed_size
    })?;
    let last = chunks
        .iter()
        .rposition(|c| c.uncompressed_offset <= end && end <= c.uncompressed_offset + c.uncompressed_size)?;
    (first <= last).then_some((first, last))
}

/// Whether the live index reads `id` exactly as the reference does: the same entry over identical spanning chunks.
fn same_as_reference(reference: &KFCReader, live: &KFCReader, id: &ResourceId) -> bool {
    let (Some(a), Some(b)) = (reference.file().resources().get(id), live.file().resources().get(id)) else {
        return false;
    };
    if (a.offset, a.size) != (b.offset, b.size) || reference.resource_size(id) != live.resource_size(id) {
        return false;
    }
    let (Some(ra), Some(rb)) = (spanning(reference, id), spanning(live, id)) else {
        return false;
    };
    let (ca, cb) = (reference.file().resource_chunks(), live.file().resource_chunks());
    ra.1 - ra.0 == rb.1 - rb.0
        && (0..=ra.1 - ra.0).all(|i| {
            let (x, y) = (&ca[ra.0 + i], &cb[rb.0 + i]);
            (
                x.offset,
                x.size,
                x.compressed_size,
                x.uncompressed_offset,
                x.uncompressed_size,
            ) == (
                y.offset,
                y.size,
                y.compressed_size,
                y.uncompressed_offset,
                y.uncompressed_size,
            )
        })
}

impl std::fmt::Debug for ArchiveBacking {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ArchiveBacking")
            .field("stem", &self.stem)
            .field("resources", &self.reference.file().resources().len())
            .field("contents", &self.reference.file().contents().len())
            .field("transaction", &self.transaction.len())
            .field("live", &self.live.as_ref().map(|live| live.keys.len()))
            .finish()
    }
}

impl ArchiveBacking {
    /// The archive of a generation in `dir`: the reference index `<stem>.kfc.bak` (the captured baseline a startup
    /// pass reads), its resources in `<stem>.kfc_resources` and contents in `<stem>_NNN.dat`, plus the generation's
    /// finalized transaction entries (resource id and the exact bytes staged for it). Only the index is read here; the
    /// data files are opened by each read.
    pub fn open(
        dir: &Path,
        stem: &str,
        transaction: impl IntoIterator<Item = (ResourceId, Vec<u8>)>,
    ) -> anyhow::Result<Self> {
        if stem.is_empty()
            || !stem
                .bytes()
                .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
        {
            anyhow::bail!("not an archive stem: {stem:?}");
        }
        let reference = KFCReader::new_with_options(
            dir,
            stem,
            KFCReaderOptions {
                kfc_extension: "kfc.bak",
                ..Default::default()
            },
        )
        .map_err(|e| anyhow::anyhow!("could not read {stem}.kfc.bak: {e}"))?;
        if !chunks_fit(&reference) {
            anyhow::bail!("{stem}.kfc.bak: a resource chunk's range overflows");
        }
        let checked = (0..reference.file().resource_chunks().len())
            .map(|_| OnceLock::new())
            .collect();
        Ok(Self {
            reference,
            dir: dir.to_path_buf(),
            stem: stem.to_owned(),
            transaction: transaction
                .into_iter()
                .map(|(id, bytes)| (id, Arc::from(bytes)))
                .collect(),
            live: None,
            named: BTreeMap::new(),
            indexed: Vec::new(),
            checked,
        })
    }

    /// The LIVE backing of a generation in `dir` that staged nothing (a cached or unchanged start): the same reference
    /// catalog as `open`, with the transaction entries of the published index `<stem>.kfc` as it is on disk (the keys
    /// it reads differently from the reference). Read-only: nothing is written, locked or held open. Refused when the
    /// live index names another game version than the reference or its chunk table overflows.
    pub fn open_live(dir: &Path, stem: &str) -> anyhow::Result<Self> {
        let mut backing = Self::open(dir, stem, std::iter::empty())?;
        let index = KFCReader::new(dir, stem).map_err(|e| anyhow::anyhow!("could not read {stem}.kfc: {e}"))?;
        if !chunks_fit(&index) {
            anyhow::bail!("{stem}.kfc: a resource chunk's range overflows");
        }
        if index.file().game_version() != backing.game_version() {
            anyhow::bail!(
                "{stem}.kfc is game version {:?}, its reference {:?}",
                index.file().game_version(),
                backing.game_version()
            );
        }
        let keys = index
            .file()
            .resources()
            .keys()
            .iter()
            .filter(|id| !same_as_reference(&backing.reference, &index, id))
            .copied()
            .collect();
        let checked = (0..index.file().resource_chunks().len())
            .map(|_| OnceLock::new())
            .collect();
        backing.live = Some(Live { index, keys, checked });
        Ok(backing)
    }

    /// Adds one named index (name -> resource). A `skill:` name is its `0x` hash text; case does not matter there.
    pub fn with_named_index(
        mut self,
        kind: NamedIndex,
        entries: impl IntoIterator<Item = (String, ResourceId)>,
    ) -> Self {
        for (name, id) in entries {
            let name = if kind == NamedIndex::Skill {
                name.to_ascii_lowercase()
            } else {
                name
            };
            self.named.insert((kind, name), id);
        }
        if !self.indexed.contains(&kind) {
            self.indexed.push(kind);
        }
        self
    }

    /// The reference index's game version (a generation's snapshot names the same one).
    pub fn game_version(&self) -> &str {
        self.reference.file().game_version()
    }

    /// The reference catalog.
    pub fn reference(&self) -> &KFCFile {
        self.reference.file()
    }

    /// Where a resource is in this generation and its complete length (index metadata, no IO).
    fn find(&self, id: &ResourceId) -> Option<Found> {
        if let Some(bytes) = self.transaction.get(id) {
            return Some(Found::Transaction(bytes.clone()));
        }
        if let Some(live) = self.live.as_ref().filter(|live| live.keys.contains(id)) {
            return live.index.resource_size(id).map(Found::Live);
        }
        self.reference.resource_size(id).map(Found::File)
    }

    /// A found resource's complete bytes: the owned transaction copy, or a Source read of the reference.
    fn bytes(&self, id: &ResourceId, found: &Found) -> Result<Vec<u8>, String> {
        match found {
            Found::Transaction(bytes) => Ok(bytes.to_vec()),
            Found::File(size) => self.read_file(&self.reference, &self.checked, id, *size),
            Found::Live(size) => {
                let live = self.live.as_ref().expect("a live entry has its index");
                self.read_file(&live.index, &live.checked, id, *size)
            }
        }
    }

    /// A resource's bytes through `index` (the reference, or the live index), read through EML's `KFCReader` once the
    /// chunks the resource spans were checked.
    fn read_file(
        &self,
        index: &KFCReader,
        checked: &[OnceLock<()>],
        id: &ResourceId,
        size: u64,
    ) -> Result<Vec<u8>, String> {
        let what = || format!("resource {}", key_text(id));
        let file = index.file();
        let entry = file
            .resources()
            .get(id)
            .ok_or_else(|| format!("{} is not in the index", what()))?;
        let end = entry
            .offset
            .checked_add(size)
            .ok_or_else(|| format!("{}: its range overflows", what()))?;
        let chunks = file.resource_chunks();
        // the same chunk search the reader makes (kfc-base container/reader.rs read_resource_into)
        let first = chunks.iter().position(|c| {
            c.uncompressed_offset <= entry.offset && entry.offset < c.uncompressed_offset + c.uncompressed_size
        });
        let last = chunks
            .iter()
            .rposition(|c| c.uncompressed_offset <= end && end <= c.uncompressed_offset + c.uncompressed_size);
        let (Some(first), Some(last)) = (first, last) else {
            return Err(format!("{} lies outside the archive's resource chunks", what()));
        };
        if first > last {
            return Err(format!("{}: its chunks are out of order", what()));
        }
        for (chunk, check) in checked.iter().enumerate().take(last + 1).skip(first) {
            self.check_chunk(index, check, chunk)?;
        }
        let mut data = Vec::new();
        data.try_reserve_exact(usize::try_from(size).map_err(|_| format!("{}: too large", what()))?)
            .map_err(|e| format!("{}: {e}", what()))?;
        let read = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| -> Result<bool, String> {
            let mut cursor = index
                .new_cursor()
                .map_err(|e| format!("could not open {}.kfc_resources: {e}", self.stem))?;
            cursor
                .read_resource_into(id, &mut data)
                .map_err(|e| format!("reading {}: {e}", what()))
        }));
        match read {
            Ok(Ok(true)) if data.len() as u64 == size => Ok(data),
            Ok(Ok(true)) => Err(format!("{}: read {} of its {size} bytes", what(), data.len())),
            Ok(Ok(false)) => Err(format!("{} lies outside the archive's resource chunks", what())),
            Ok(Err(cause)) => Err(cause),
            Err(panic) => Err(format!(
                "{}: a resource chunk is shorter than its index says ({})",
                what(),
                panic_text(panic)
            )),
        }
    }

    /// One resource chunk, checked once per generation before the reader decompresses it: its compressed range lies in
    /// the resources file, and its zstd frames can produce the decompressed length the index claims (the reader
    /// reserves that whole length first). A failure is not kept: the next read checks again.
    fn check_chunk(&self, reader: &KFCReader, checked: &OnceLock<()>, index: usize) -> Result<(), String> {
        if checked.get().is_some() {
            return Ok(());
        }
        let chunk = &reader.file().resource_chunks()[index];
        let name = format!("{}.kfc_resources", self.stem);
        let mut file = File::open(self.dir.join(&name)).map_err(|e| format!("could not open {name}: {e}"))?;
        let length = file.metadata().map_err(|e| format!("{name}: {e}"))?.len();
        let end = chunk.offset + chunk.compressed_size;
        if end > length {
            return Err(format!(
                "resource chunk {index} lies past the end of {name} (bytes {}..{end} of {length}): the archive is truncated",
                chunk.offset
            ));
        }
        let mut compressed = Vec::new();
        compressed
            .try_reserve_exact(chunk.compressed_size as usize)
            .map_err(|e| format!("resource chunk {index}: {e}"))?;
        file.seek(SeekFrom::Start(chunk.offset))
            .and_then(|_| (&mut file).take(chunk.compressed_size).read_to_end(&mut compressed))
            .map_err(|e| format!("reading resource chunk {index} of {name}: {e}"))?;
        if compressed.len() as u64 != chunk.compressed_size {
            return Err(format!(
                "resource chunk {index} of {name} ends early: the archive is truncated"
            ));
        }
        let (bound, most) =
            zstd_output_bound(&compressed).map_err(|e| format!("resource chunk {index} of {name}: {e}"))?;
        if chunk.uncompressed_size > bound {
            return Err(format!(
                "resource chunk {index} of {name} claims {} decompressed bytes; its zstd frames produce at most {bound}",
                chunk.uncompressed_size
            ));
        }
        // The reader reserves the claim in one allocation, and its decoder writes whatever the frames produce (at most
        // `most`) into that buffer, growing it. Neither can fail softly there: where the memory cannot be committed
        // (Windows does not overcommit) the process aborts. Reserve the larger here, fallibly, and refuse a chunk this
        // process cannot hold. This is no budget: on Linux a reservation is not memory, and frames can still write up to
        // `most` (at most 128 KiB per 4 compressed bytes) before the decoder stops them.
        let peak = chunk.uncompressed_size.max(most);
        if !usize::try_from(peak).is_ok_and(|n| Vec::<u8>::new().try_reserve_exact(n).is_ok()) {
            return Err(format!(
                "resource chunk {index} of {name}: its zstd frames can write up to {peak} bytes, more than this process can reserve"
            ));
        }
        let _ = checked.set(());
        Ok(())
    }

    /// `count` bytes of a reference content from `start` (in range: the caller checked), read from its container.
    fn read_content(&self, hash: &ContentHash, start: u64, count: u64) -> Result<Vec<u8>, String> {
        let entry = self
            .reference
            .file()
            .contents()
            .get(hash)
            .ok_or_else(|| format!("content {hash} is not in the index"))?;
        let name = format!("{}_{:03}.dat", self.stem, entry.container_index);
        let mut file = File::open(self.dir.join(&name)).map_err(|e| format!("could not open {name}: {e}"))?;
        let length = file.metadata().map_err(|e| format!("{name}: {e}"))?.len();
        let end = entry.offset.checked_add(u64::from(hash.size()));
        if end.is_none_or(|end| end > length) {
            return Err(format!(
                "content {hash} lies past the end of {name} ({} + {} bytes of {length}): the archive is truncated",
                entry.offset,
                hash.size()
            ));
        }
        let mut data = Vec::new();
        data.try_reserve_exact(count as usize)
            .map_err(|e| format!("content {hash}: {e}"))?;
        file.seek(SeekFrom::Start(entry.offset + start))
            .and_then(|_| (&mut file).take(count).read_to_end(&mut data))
            .map_err(|e| format!("reading content {hash} from {name}: {e}"))?;
        if data.len() as u64 != count {
            return Err(format!("content {hash} in {name} ends early: the archive is truncated"));
        }
        Ok(data)
    }

    fn named(&self, kind: NamedIndex, name: &str) -> Result<Option<ResourceId>, ()> {
        if !self.indexed.contains(&kind) {
            return Err(());
        }
        Ok(self.named.get(&(kind, name.to_owned())).copied())
    }
}

/// Where a resource is: the generation's staged transaction (owned bytes), its live published index (complete length)
/// or the reference catalog (complete length).
enum Found {
    Transaction(Arc<[u8]>),
    Live(u64),
    File(u64),
}

impl Found {
    fn len(&self) -> u64 {
        match self {
            Found::Transaction(bytes) => bytes.len() as u64,
            Found::Live(size) | Found::File(size) => *size,
        }
    }
    fn source(&self) -> u32 {
        match self {
            Found::Transaction(_) | Found::Live(_) => KILN_ARCHIVE_SOURCE_TRANSACTION as u32,
            Found::File(_) => KILN_ARCHIVE_SOURCE_FILE as u32,
        }
    }
}

/// What resource_json materialized for one resource of a generation: the complete reply, NOT_FOUND, or a failure's
/// result and its cause (captured once).
pub(crate) enum Reply {
    Json(Arc<[u8]>),
    NotFound,
    Failed(KilnResult, Arc<[u8]>),
}

fn key_text(id: &ResourceId) -> String {
    format!("{}/{:08x}/{}", id.guid(), id.type_hash(), id.part_index())
}

fn panic_text(panic: Box<dyn std::any::Any + Send>) -> String {
    match panic.downcast::<String>() {
        Ok(text) => *text,
        Err(panic) => panic
            .downcast::<&str>()
            .map(|t| (*t).to_owned())
            .unwrap_or_else(|_| "panic".into()),
    }
}

/// What the zstd frames in `data` can decompress to (RFC 8878 §3.1), from their headers alone: `(whole, most)`.
/// `whole` bounds a complete decode (a frame's declared content size caps it); `most` bounds what a decoder may write
/// before it finishes or fails, trusting no declared size. Per block: a compressed one at most 128 KiB, an RLE one its
/// size up to 128 KiB (a decoder refuses more), a raw one its size; skippable frames add nothing. Truncated or malformed
/// frames are an error.
fn zstd_output_bound(data: &[u8]) -> Result<(u64, u64), String> {
    let truncated = || "the zstd frame is truncated".to_owned();
    let byte = |at: usize| data.get(at).copied().ok_or_else(truncated);
    let mut at = 0usize;
    let (mut whole, mut most) = (0u64, 0u64);
    if data.is_empty() {
        return Err("no zstd frame".into());
    }
    while at < data.len() {
        let magic = u32::from_le_bytes(data.get(at..at + 4).ok_or_else(truncated)?.try_into().unwrap());
        at += 4;
        if magic & 0xffff_fff0 == 0x184d_2a50 {
            let size = u32::from_le_bytes(data.get(at..at + 4).ok_or_else(truncated)?.try_into().unwrap());
            at = at
                .checked_add(4 + size as usize)
                .filter(|&end| end <= data.len())
                .ok_or_else(truncated)?;
            continue;
        }
        if magic != 0xfd2f_b528 {
            return Err(format!("not a zstd frame (magic {magic:#010x})"));
        }
        let descriptor = byte(at)?;
        at += 1;
        if descriptor & 0x08 != 0 {
            return Err("a zstd frame header sets its reserved bit".into());
        }
        let single_segment = descriptor & 0x20 != 0;
        let checksum = descriptor & 0x04 != 0;
        if !single_segment {
            at += 1; // window descriptor
        }
        at += [0usize, 1, 2, 4][(descriptor & 3) as usize];
        let fcs_bytes = match descriptor >> 6 {
            0 => usize::from(single_segment),
            1 => 2,
            2 => 4,
            _ => 8,
        };
        let declared = if fcs_bytes == 0 {
            None
        } else {
            let field = data.get(at..at + fcs_bytes).ok_or_else(truncated)?;
            let mut bytes = [0u8; 8];
            bytes[..fcs_bytes].copy_from_slice(field);
            let value = u64::from_le_bytes(bytes);
            Some(if fcs_bytes == 2 { value + 256 } else { value })
        };
        at += fcs_bytes;
        let mut blocks = 0u64;
        loop {
            let header = data.get(at..at + 3).ok_or_else(truncated)?;
            at += 3;
            let header = u32::from(header[0]) | u32::from(header[1]) << 8 | u32::from(header[2]) << 16;
            let last = header & 1 != 0;
            let size = (header >> 3) as usize;
            let (stored, produced) = match (header >> 1) & 3 {
                0 => (size, size as u64),
                1 => (1, (size as u64).min(ZSTD_BLOCK_MAX)),
                2 => (size, ZSTD_BLOCK_MAX),
                _ => return Err("a zstd block of the reserved type".into()),
            };
            at = at
                .checked_add(stored)
                .filter(|&end| end <= data.len())
                .ok_or_else(truncated)?;
            blocks = blocks.saturating_add(produced);
            if last {
                break;
            }
        }
        if checksum {
            at = at
                .checked_add(4)
                .filter(|&end| end <= data.len())
                .ok_or_else(truncated)?;
        }
        whole = whole.saturating_add(declared.map_or(blocks, |d| d.min(blocks)));
        most = most.saturating_add(blocks);
    }
    Ok((whole, most))
}

// ---- resource_json's neutral tree ----

type Registry = Arc<TypeRegistry>;
type Mapped = MappedValue<Arc<[u8]>, Registry>;

/// The bounded walk of decoded bytes into common.schema.json's tagged `value` (the codec's bounds, as `Converter::mapped`).
/// Type metadata is borrowed from the registry the bytes were mapped with, never copied.
struct Tree<'r> {
    registry: &'r TypeRegistry,
    out: String,
    depth: usize,
    values: usize,
    limit: usize,
}

/// A type's inner (base, element, wrapped) chain, walked once: a faulty registry's cycle would make kfc loop or
/// recurse without end (the codec's `check_chain`, same text).
fn check_chain(registry: &TypeRegistry, r#type: &TypeMetadata) -> Result<(), String> {
    let mut next = r#type.inner_type;
    for _ in 0..=registry.len() {
        match next.and_then(|index| registry.get(index)) {
            Some(t) => next = t.inner_type,
            None => return Ok(()),
        }
    }
    Err(format!("cyclic type chain at `{}`", r#type.qualified_name))
}

impl<'r> Tree<'r> {
    /// A handle's metadata in the walk's registry (the handle was made from it).
    fn meta(&self, r#type: &TypeHandle<Registry>) -> Option<&'r TypeMetadata> {
        let registry: &'r TypeRegistry = self.registry;
        registry.get(r#type.index)
    }

    fn count(&mut self) -> Result<(), String> {
        self.values += 1;
        if self.values > self.limit {
            return Err(format!("the value expands to more than {} values", self.limit));
        }
        Ok(())
    }

    fn open(&mut self, tag: &str, declared: Option<&TypeMetadata>) {
        self.out.push_str("{\"tag\":\"");
        self.out.push_str(tag);
        self.out.push('"');
        if let Some(r#type) = declared {
            self.out.push_str(",\"type\":");
            self.out.push_str(&string(&r#type.qualified_name));
        }
    }

    fn scalar(&mut self, tag: &str, declared: Option<&TypeMetadata>, value: &str) {
        self.open(tag, declared);
        self.out.push_str(",\"value\":");
        self.out.push_str(value);
        self.out.push('}');
    }

    fn nested(&mut self, walk: impl FnOnce(&mut Self) -> Result<(), String>) -> Result<(), String> {
        if self.depth >= MAX_DEPTH {
            return Err(format!("value nested more than {MAX_DEPTH} levels deep"));
        }
        self.depth += 1;
        let result = walk(self);
        self.depth -= 1;
        result
    }

    fn value(&mut self, value: Mapped, declared: Option<&'r TypeMetadata>) -> Result<(), String> {
        self.count()?;
        let u = |v: u64| format!("\"u64:{v:016x}\"");
        let i = |v: i64| format!("\"i64:{:016x}\"", v as u64);
        let guid = |g: &Guid| format!("\"guid:{}\"", hex(g.data()));
        match value {
            MappedValue::None => {
                self.open("nil", declared);
                self.out.push('}');
            }
            MappedValue::Bool(b) => self.scalar("bool", declared, if b { "true" } else { "false" }),
            MappedValue::UInt8(v) => self.scalar("u64", declared, &u(v.into())),
            MappedValue::UInt16(v) => self.scalar("u64", declared, &u(v.into())),
            MappedValue::UInt32(v) => self.scalar("u64", declared, &u(v.into())),
            MappedValue::UInt64(v) => self.scalar("u64", declared, &u(v)),
            MappedValue::SInt8(v) => self.scalar("i64", declared, &i(v.into())),
            MappedValue::SInt16(v) => self.scalar("i64", declared, &i(v.into())),
            MappedValue::SInt32(v) => self.scalar("i64", declared, &i(v.into())),
            MappedValue::SInt64(v) => self.scalar("i64", declared, &i(v)),
            MappedValue::Float32(v) => self.scalar("f32", declared, &format!("\"f32:{:08x}\"", v.to_bits())),
            MappedValue::Float64(v) => self.scalar("f64", declared, &format!("\"f64:{:016x}\"", v.to_bits())),
            // an enum or bitmask is its stored bit pattern, named by its own type (a typedef field names the typedef)
            MappedValue::Enum(e) => {
                let own = declared.or(self.meta(e.r#type()));
                self.scalar("u64", own, &u(e.value()))
            }
            MappedValue::Bitmask(b) => {
                let own = declared.or(self.meta(b.r#type()));
                self.scalar("u64", own, &u(b.value()))
            }
            MappedValue::String(s) => {
                let text = s.as_str().map_err(|e| e.to_string())?;
                self.scalar("string", declared, &format!("\"string:{}\"", hex(text.as_bytes())))
            }
            MappedValue::Guid(g) => self.scalar("guid", declared, &guid(&g)),
            MappedValue::Reference(r) => self.scalar("reference", declared, &guid(r.guid())),
            MappedValue::Struct(s) => self.nested(|tree| tree.structure(s, declared))?,
            MappedValue::Array(array) => self.nested(|tree| {
                // kfc maps every element through the array's inner chain
                check_chain(tree.registry, array.r#type())?;
                let element = tree.meta(array.element_type());
                let own = declared.or(tree.meta(array.r#type()));
                tree.open("array", own);
                tree.out.push_str(",\"items\":[");
                for (n, item) in array.iter().enumerate() {
                    if n > 0 {
                        tree.out.push(',');
                    }
                    tree.value(item.map_err(|e| e.to_string())?, element)?;
                }
                tree.out.push_str("]}");
                Ok(())
            })?,
            MappedValue::Optional(optional) => self.nested(|tree| {
                let own = tree.meta(optional.r#type());
                let registry: &'r TypeRegistry = tree.registry;
                let inner = own.and_then(|t| t.inner_type).and_then(|index| registry.get(index));
                tree.open("optional", declared.or(own));
                match optional.into_value() {
                    None => tree.out.push_str(",\"present\":false}"),
                    Some(value) => {
                        tree.out.push_str(",\"present\":true,\"value\":");
                        tree.value(value, inner)?;
                        tree.out.push('}');
                    }
                }
                Ok(())
            })?,
            MappedValue::Variant(variant) => self.nested(|tree| {
                let base = declared.or(tree.meta(variant.base_type()));
                let selected = tree.meta(variant.variant_type());
                let discriminant = u64::from(variant.variant_type().qualified_hash);
                tree.open("variant", base);
                tree.out
                    .push_str(&format!(",\"discriminant\":\"u64:{discriminant:016x}\",\"value\":"));
                // the selected type's value is the variant's one child: a value of its own
                tree.count()?;
                let value = variant.into_value();
                tree.nested(|tree| tree.structure(value, selected))?;
                tree.out.push('}');
                Ok(())
            })?,
        }
        Ok(())
    }

    fn structure(
        &mut self,
        value: MappedStruct<Arc<[u8]>, Registry>,
        declared: Option<&'r TypeMetadata>,
    ) -> Result<(), String> {
        let registry: &'r TypeRegistry = self.registry;
        // the base chain kfc walks to list the fields, then each field's chain kfc maps it through
        check_chain(registry, value.r#type())?;
        let fields: Vec<(&str, Option<&'r TypeMetadata>)> = value
            .r#type()
            .iter_fields()
            .map(|field| (field.name.as_str(), registry.get(field.r#type)))
            .collect();
        for field_type in fields.iter().filter_map(|(_, t)| *t) {
            check_chain(registry, field_type)?;
        }
        self.open("map", declared.or(self.meta(value.r#type())));
        self.out.push_str(",\"entries\":[");
        for (n, (field, (name, field_type))) in value.iter().zip(fields.iter()).enumerate() {
            let (_, field) = field.map_err(|e| e.to_string())?;
            if n > 0 {
                self.out.push(',');
            }
            self.out.push_str("{\"key\":{\"tag\":\"string\",\"value\":\"string:");
            self.out.push_str(&hex(name.as_bytes()));
            self.out.push_str("\"},\"value\":");
            self.value(field, *field_type)?;
            self.out.push('}');
        }
        self.out.push_str("]}");
        Ok(())
    }
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// `bytes` of the type `index` names as the neutral tree. Errors are their texts; parser panics are contained.
fn neutral_tree(registry: &Registry, r#type: &TypeMetadata, bytes: Vec<u8>) -> Result<String, String> {
    let limit = mapped_value_limit(bytes.len());
    let data: Arc<[u8]> = Arc::from(bytes);
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        // kfc maps optionals and typedefs eagerly: the root's chain first
        check_chain(registry, r#type)?;
        let root = Mapped::from_bytes(registry, r#type, &data).map_err(|e| e.to_string())?;
        let mut tree = Tree {
            registry,
            out: String::new(),
            depth: 0,
            values: 0,
            limit,
        };
        tree.value(root, Some(r#type))?;
        Ok(tree.out)
    }))
    .unwrap_or_else(|panic| Err(format!("decoding failed: {}", panic_text(panic))))
}

// ---- queries ----

enum Query {
    Resource(ResourceId),
    Named(NamedIndex, String),
}

fn hex_run(s: &[u8], n: usize, lower_only: bool) -> bool {
    s.len() == n
        && s.iter()
            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(c) || (!lower_only && (b'A'..=b'F').contains(c)))
}

fn name_ok(s: &[u8], dots: bool) -> bool {
    !s.is_empty()
        && s.len() <= 128
        && s.iter()
            .all(|&c| c.is_ascii_alphanumeric() || c == b'_' || (dots && (c == b'.' || c == b'-')))
        && !s.windows(2).any(|w| w == b"..")
}

/// kiln.boot's query grammar as the header gives it (boot_service.cpp:210-251): `resource:<lowercase canonical
/// GUID>/<8 hex qualified type hash>/<decimal part>` (part <= INT32_MAX, no leading zeros), `skill:0x<8 hex>`,
/// `item:`/`template:` names (1..128 ASCII alnum, underscore, dot, hyphen, no `..`), `material:` names (1..128 ASCII
/// alnum, underscore). Anything else (boot's `names:` forms included) is malformed.
fn parse_query(q: &[u8]) -> Option<Query> {
    let colon = q.iter().position(|&c| c == b':')?;
    let (form, arg) = (&q[..colon], &q[colon + 1..]);
    let text = |s: &[u8]| String::from_utf8(s.to_vec()).ok();
    match form {
        b"item" if name_ok(arg, true) => Some(Query::Named(NamedIndex::Item, text(arg)?)),
        b"template" if name_ok(arg, true) => Some(Query::Named(NamedIndex::Template, text(arg)?)),
        b"material" if name_ok(arg, false) => Some(Query::Named(NamedIndex::Material, text(arg)?)),
        b"skill" if arg.len() == 10 && arg.starts_with(b"0x") && hex_run(&arg[2..], 8, false) => {
            Some(Query::Named(NamedIndex::Skill, text(arg)?.to_ascii_lowercase()))
        }
        b"resource" => {
            let mut parts = arg.splitn(3, |&c| c == b'/');
            let (guid, r#type, part) = (parts.next()?, parts.next()?, parts.next()?);
            let dashed = guid.len() == 36
                && guid.iter().enumerate().all(|(i, &c)| {
                    if matches!(i, 8 | 13 | 18 | 23) {
                        c == b'-'
                    } else {
                        hex_run(&[c], 1, true)
                    }
                });
            if !dashed || !hex_run(r#type, 8, false) {
                return None;
            }
            if part.is_empty()
                || part.len() > 10
                || (part.len() > 1 && part[0] == b'0')
                || !part.iter().all(u8::is_ascii_digit)
            {
                return None;
            }
            let part: u64 = std::str::from_utf8(part).ok()?.parse().ok()?;
            if part > i32::MAX as u64 {
                return None;
            }
            let guid = Guid::parse(std::str::from_utf8(guid).ok()?)?;
            let type_hash = u32::from_str_radix(std::str::from_utf8(r#type).ok()?, 16).ok()?;
            Some(Query::Resource(ResourceId::new(guid, type_hash, part as u32)))
        }
        _ => None,
    }
}

// ---- the C boundary ----

type Span = (usize, usize);

fn span<T>(ptr: *const T, bytes: usize) -> Span {
    (ptr as usize, if ptr.is_null() { 0 } else { bytes })
}

/// The caller's error record and the call's spans, checked before any lookup or IO. An invalid record, or one that
/// overlaps another span, is not written (INVALID_ARGUMENT). Otherwise other overlaps are INVALID_ARGUMENT with the
/// record initialized.
fn checked(error: *mut KilnArchiveErrorV1, inputs: &[Span], outputs: &[Span]) -> Result<ErrorOut, KilnResult> {
    let Ok(record) = (unsafe { ErrorOut::check(error) }) else {
        return Err(KILN_ERR_INVALID_ARGUMENT);
    };
    let mine = record.span();
    if inputs.iter().chain(outputs).any(|&s| overlap(&[s, mine])) {
        return Err(KILN_ERR_INVALID_ARGUMENT);
    }
    let crossing = outputs.iter().enumerate().any(|(n, &out)| {
        outputs[n + 1..]
            .iter()
            .chain(inputs)
            .any(|&other| overlap(&[out, other]))
    });
    if crossing {
        return Err(record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0));
    }
    Ok(record)
}

/// A caller-sized record's declared size (0 for NULL).
unsafe fn declared<T>(record: *const T) -> u32 {
    if record.is_null() {
        0
    } else {
        unsafe { record.cast::<u32>().read_unaligned() }
    }
}

/// A genuine provider failure: `cause` retained once behind a fresh token of the view's owner. A view closed meanwhile
/// has had its diagnostics reclaimed already; this call's token is reclaimed here then.
fn failed(
    record: &ErrorOut,
    result: KilnResult,
    owner: KilnOwner,
    view: KilnEmlArchiveView,
    cause: Arc<[u8]>,
) -> KilnResult {
    let rc = record.fail(result, owner, view, cause);
    if view::live(view).is_err() {
        errors::reclaim_views(&[view]);
    }
    rc
}

fn cause(text: String) -> Arc<[u8]> {
    Arc::from(text.into_bytes())
}

/// The live view's generation and archive for an owner-taking member: unknown view INVALID_ARGUMENT, closed REVOKED,
/// another owner's REVOKED, no archive UNAVAILABLE.
fn target(owner: KilnOwner, view: KilnEmlArchiveView) -> Result<(Arc<Generation>, Arc<ArchiveBacking>), KilnResult> {
    let (holder, generation) = view::live(view)?;
    if holder != owner {
        return Err(KILN_ERR_REVOKED);
    }
    let archive = generation.archive.clone().ok_or(KILN_ERR_UNAVAILABLE)?;
    Ok((generation, archive))
}

/// Runs a member's work with any panic contained: INTERNAL, the record initialized (no retained cause).
fn contained(record: &ErrorOut, work: impl FnOnce() -> KilnResult) -> KilnResult {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(work))
        .unwrap_or_else(|_| record.finish(KILN_ERR_INTERNAL, 0, 0))
}

fn id_of(key: &KilnAssetKey) -> ResourceId {
    ResourceId::new(Guid::new(key.guid), key.r#type, key.part)
}

fn key_of(id: &ResourceId) -> KilnAssetKey {
    KilnAssetKey {
        guid: *id.guid().data(),
        r#type: id.type_hash(),
        part: id.part_index(),
    }
}

fn hash_of(hash: &KilnContentHash) -> ContentHash {
    ContentHash::new(hash.size, hash.hash0, hash.hash1, hash.hash2)
}

pub(super) unsafe extern "C" fn find_resource(
    owner: KilnOwner,
    archive: KilnEmlArchiveView,
    key: *const KilnAssetKey,
    out: *mut KilnArchiveResourceInfo,
    error: *mut KilnArchiveErrorV1,
) -> KilnResult {
    let out_size = unsafe { declared(out) };
    let record = match checked(error, &[span(key, KEY_STRIDE)], &[span(out, out_size as usize)]) {
        Ok(record) => record,
        Err(rc) => return rc,
    };
    contained(&record, || {
        if owner == 0 || key.is_null() || out.is_null() || out_size < INFO_SIZE {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        }
        let (generation, backing) = match target(owner, archive) {
            Ok(found) => found,
            Err(rc) => return record.finish(rc, 0, 0),
        };
        let id = id_of(&unsafe { key.read_unaligned() });
        let Some(found) = backing.find(&id) else {
            return record.finish(KILN_ERR_NOT_FOUND, 0, 0);
        };
        let registry = generation
            .registry
            .as_ref()
            .expect("a generation with an archive keeps its registry");
        let Some(r#type) = registry.get_by_hash(LookupKey::Qualified(id.type_hash())) else {
            return failed(
                &record,
                KILN_ERR_UNAVAILABLE,
                owner,
                archive,
                cause(format!("{TYPE_MISSING}: {}", key_text(&id))),
            );
        };
        let info = KilnArchiveResourceInfo {
            size: out_size,
            schema_version: 1,
            key: key_of(&id),
            source: found.source(),
            reserved: 0,
            type_index: r#type.index.as_usize() as u64,
            byte_length: found.len(),
            generation: generation.pins.generation,
        };
        unsafe { out.write_unaligned(info) };
        record.finish(KILN_OK, 0, 0)
    })
}

pub(super) unsafe extern "C" fn list_resources(
    owner: KilnOwner,
    archive: KilnEmlArchiveView,
    query: *const KilnArchiveQuery,
    first: u64,
    out: *mut KilnAssetKey,
    capacity: usize,
    required: *mut usize,
    error: *mut KilnArchiveErrorV1,
) -> KilnResult {
    let query_size = unsafe { declared(query) };
    let bytes = capacity.checked_mul(KEY_STRIDE);
    let record = match checked(
        error,
        &[span(query, query_size as usize)],
        &[
            span(out, bytes.unwrap_or(usize::MAX)),
            span(required, std::mem::size_of::<usize>()),
        ],
    ) {
        Ok(record) => record,
        Err(rc) => return rc,
    };
    contained(&record, || {
        if owner == 0 || query.is_null() || required.is_null() || (out.is_null() && capacity != 0) || bytes.is_none() {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        }
        if query_size < QUERY_SIZE {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        }
        let q = unsafe { query.read_unaligned() };
        let (guid, r#type, part) = (
            KILN_ARCHIVE_FILTER_GUID as u32,
            KILN_ARCHIVE_FILTER_TYPE as u32,
            KILN_ARCHIVE_FILTER_PART as u32,
        );
        let malformed = q.schema_version != 1
            || q.flags & !(guid | r#type | part) != 0
            || (q.order != KILN_ARCHIVE_ORDER_CATALOG as u32 && q.order != KILN_ARCHIVE_ORDER_PART as u32)
            || (q.order == KILN_ARCHIVE_ORDER_PART as u32 && q.flags & (guid | r#type) != guid | r#type)
            || (q.flags & guid == 0 && q.key.guid != [0; 16])
            || (q.flags & r#type == 0 && q.key.r#type != 0)
            || (q.flags & part == 0 && q.key.part != 0);
        if malformed {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        }
        let (_, backing) = match target(owner, archive) {
            Ok(found) => found,
            Err(rc) => return record.finish(rc, 0, 0),
        };
        let mut selected: Vec<&ResourceId> = backing
            .reference()
            .resources()
            .keys()
            .iter()
            .filter(|id| {
                (q.flags & guid == 0 || *id.guid().data() == q.key.guid)
                    && (q.flags & r#type == 0 || id.type_hash() == q.key.r#type)
                    && (q.flags & part == 0 || id.part_index() == q.key.part)
            })
            .collect();
        if q.order == KILN_ARCHIVE_ORDER_PART as u32 {
            selected.sort_by_key(|id| id.part_index());
        }
        let total = selected.len() as u64;
        if first > total || (q.limit != 0 && first.checked_add(q.limit).is_none()) {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        }
        let rest = total - first;
        let count = if q.limit == 0 { rest } else { q.limit.min(rest) } as usize;
        unsafe { required.write_unaligned(count) };
        if capacity < count {
            return record.finish(KILN_ERR_BUFFER_TOO_SMALL, 0, 0);
        }
        for (n, id) in selected[first as usize..first as usize + count].iter().enumerate() {
            unsafe { out.add(n).write_unaligned(key_of(id)) };
        }
        record.finish(KILN_OK, 0, 0)
    })
}

pub(super) unsafe extern "C" fn read_resource(
    owner: KilnOwner,
    archive: KilnEmlArchiveView,
    key: *const KilnAssetKey,
    out: *mut std::ffi::c_void,
    capacity: usize,
    required: *mut usize,
    error: *mut KilnArchiveErrorV1,
) -> KilnResult {
    let record = match checked(
        error,
        &[span(key, KEY_STRIDE)],
        &[span(out, capacity), span(required, std::mem::size_of::<usize>())],
    ) {
        Ok(record) => record,
        Err(rc) => return rc,
    };
    contained(&record, || {
        if owner == 0 || key.is_null() || required.is_null() || (out.is_null() && capacity != 0) {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        }
        let (_, backing) = match target(owner, archive) {
            Ok(found) => found,
            Err(rc) => return record.finish(rc, 0, 0),
        };
        let id = id_of(&unsafe { key.read_unaligned() });
        let Some(found) = backing.find(&id) else {
            return record.finish(KILN_ERR_NOT_FOUND, 0, 0);
        };
        // the index's length: a short buffer is refused before any IO
        let Ok(length) = usize::try_from(found.len()) else {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        };
        if capacity < length {
            unsafe { required.write_unaligned(length) };
            return record.finish(KILN_ERR_BUFFER_TOO_SMALL, 0, 0);
        }
        let bytes = match backing.bytes(&id, &found) {
            Ok(bytes) => bytes,
            Err(text) => return failed(&record, KILN_ERR_INTERNAL, owner, archive, cause(text)),
        };
        unsafe { required.write_unaligned(bytes.len()) };
        if !bytes.is_empty() {
            unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), out.cast::<u8>(), bytes.len()) };
        }
        record.finish(KILN_OK, 0, 0)
    })
}

#[allow(clippy::too_many_arguments)]
pub(super) unsafe extern "C" fn content_read_range(
    owner: KilnOwner,
    archive: KilnEmlArchiveView,
    hash: *const KilnContentHash,
    start: u64,
    count: u64,
    out: *mut std::ffi::c_void,
    capacity: usize,
    required: *mut usize,
    error: *mut KilnArchiveErrorV1,
) -> KilnResult {
    let record = match checked(
        error,
        &[span(hash, HASH_STRIDE)],
        &[span(out, capacity), span(required, std::mem::size_of::<usize>())],
    ) {
        Ok(record) => record,
        Err(rc) => return rc,
    };
    contained(&record, || {
        if owner == 0 || hash.is_null() || required.is_null() || (out.is_null() && capacity != 0) {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        }
        let hash = hash_of(&unsafe { hash.read_unaligned() });
        let size = u64::from(hash.size());
        if start > size || count > size - start {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        }
        let (_, backing) = match target(owner, archive) {
            Ok(found) => found,
            Err(rc) => return record.finish(rc, 0, 0),
        };
        if !backing.reference().contents().contains_key(&hash) {
            return record.finish(KILN_ERR_NOT_FOUND, 0, 0);
        }
        let length = count as usize;
        if capacity < length {
            unsafe { required.write_unaligned(length) };
            return record.finish(KILN_ERR_BUFFER_TOO_SMALL, 0, 0);
        }
        let bytes = match backing.read_content(&hash, start, count) {
            Ok(bytes) => bytes,
            Err(text) => return failed(&record, KILN_ERR_INTERNAL, owner, archive, cause(text)),
        };
        unsafe { required.write_unaligned(length) };
        if length != 0 {
            unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), out.cast::<u8>(), length) };
        }
        record.finish(KILN_OK, 0, 0)
    })
}

pub(super) unsafe extern "C" fn list_contents(
    owner: KilnOwner,
    archive: KilnEmlArchiveView,
    first: u64,
    out: *mut KilnContentHash,
    capacity: usize,
    required: *mut usize,
    error: *mut KilnArchiveErrorV1,
) -> KilnResult {
    let bytes = capacity.checked_mul(HASH_STRIDE);
    let record = match checked(
        error,
        &[],
        &[
            span(out, bytes.unwrap_or(usize::MAX)),
            span(required, std::mem::size_of::<usize>()),
        ],
    ) {
        Ok(record) => record,
        Err(rc) => return rc,
    };
    contained(&record, || {
        if owner == 0 || required.is_null() || (out.is_null() && capacity != 0) || bytes.is_none() {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        }
        let (_, backing) = match target(owner, archive) {
            Ok(found) => found,
            Err(rc) => return record.finish(rc, 0, 0),
        };
        let contents = backing.reference().contents().keys();
        let total = contents.len() as u64;
        if first > total {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        }
        let count = (total - first) as usize;
        unsafe { required.write_unaligned(count) };
        if capacity < count {
            return record.finish(KILN_ERR_BUFFER_TOO_SMALL, 0, 0);
        }
        for (n, hash) in contents[first as usize..].iter().enumerate() {
            let hash = KilnContentHash {
                size: hash.size(),
                hash0: hash.hash0(),
                hash1: hash.hash1(),
                hash2: hash.hash2(),
            };
            unsafe { out.add(n).write_unaligned(hash) };
        }
        record.finish(KILN_OK, 0, 0)
    })
}

/// resource_json's reply for one resource of a generation (materialized once by `Generation::reply`).
fn materialize(generation: &Generation, backing: &ArchiveBacking, id: &ResourceId) -> Reply {
    let Some(found) = backing.find(id) else {
        return Reply::NotFound;
    };
    let bytes = match backing.bytes(id, &found) {
        Ok(bytes) => bytes,
        Err(text) => return Reply::Failed(KILN_ERR_INTERNAL, cause(text)),
    };
    let registry = generation
        .registry
        .as_ref()
        .expect("a generation with an archive keeps its registry");
    let Some(r#type) = registry.get_by_hash(LookupKey::Qualified(id.type_hash())) else {
        return Reply::Failed(KILN_ERR_UNAVAILABLE, cause(format!("{TYPE_MISSING}: {}", key_text(id))));
    };
    let tree = match neutral_tree(registry, r#type, bytes) {
        Ok(tree) => tree,
        Err(text) => return Reply::Failed(KILN_ERR_INTERNAL, cause(format!("resource {}: {text}", key_text(id)))),
    };
    let json = format!(
        "{{\"schema_version\":1,\"pin\":{},\"resourceId\":\"guid:{}\",\"typeHash\":{},\"part\":{},\"type_index\":\"{}\",\"value\":{tree}}}",
        generation.pin_json,
        hex(id.guid().data()),
        id.type_hash(),
        id.part_index(),
        u64s(r#type.index.as_usize() as u64),
    );
    Reply::Json(Arc::from(json.into_bytes()))
}

pub(super) unsafe extern "C" fn resource_json(
    view: KilnEmlArchiveView,
    query: KilnStr,
    json: *mut c_char,
    capacity: usize,
    required: *mut usize,
    error: *mut KilnArchiveErrorV1,
) -> KilnResult {
    let record = match checked(
        error,
        &[span(query.ptr, query.len)],
        &[span(json, capacity), span(required, std::mem::size_of::<usize>())],
    ) {
        Ok(record) => record,
        Err(rc) => return rc,
    };
    contained(&record, || {
        if required.is_null()
            || (json.is_null() && capacity != 0)
            || query.ptr.is_null()
            || query.len > isize::MAX as usize
        {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        }
        let text = unsafe { std::slice::from_raw_parts(query.ptr.cast::<u8>(), query.len) };
        let Some(parsed) = parse_query(text) else {
            return record.finish(KILN_ERR_INVALID_ARGUMENT, 0, 0);
        };
        let (owner, generation) = match view::live(view) {
            Ok(found) => found,
            Err(rc) => return record.finish(rc, 0, 0),
        };
        let Some(backing) = generation.archive.clone() else {
            return record.finish(KILN_ERR_UNAVAILABLE, 0, 0);
        };
        let id = match parsed {
            Query::Resource(id) => id,
            Query::Named(kind, name) => match backing.named(kind, &name) {
                Err(()) => return record.finish(KILN_ERR_UNAVAILABLE, 0, 0),
                Ok(None) => return record.finish(KILN_ERR_NOT_FOUND, 0, 0),
                Ok(Some(id)) => id,
            },
        };
        let reply = generation.reply(id, || materialize(&generation, &backing, &id));
        match &*reply {
            Reply::NotFound => record.finish(KILN_ERR_NOT_FOUND, 0, 0),
            Reply::Failed(rc, text) => failed(&record, *rc, owner, view, text.clone()),
            Reply::Json(bytes) => {
                unsafe { required.write_unaligned(bytes.len()) };
                if capacity < bytes.len() {
                    return record.finish(KILN_ERR_BUFFER_TOO_SMALL, 0, 0);
                }
                unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), json.cast::<u8>(), bytes.len()) };
                record.finish(KILN_OK, 0, 0)
            }
        }
    })
}

// ---- kiln.eml.values' RESOURCE roots ----

/// The RESOURCE root source kiln.eml.values gives an arena over a generation with an archive: the same find and read
/// as find_resource/read_resource (transaction first, then the reference). Holds the archive and the registry it was
/// authorized with (the arena decodes with its own copy, whose digest equals it).
pub(crate) struct ArenaResources {
    archive: Arc<ArchiveBacking>,
    registry: Arc<TypeRegistry>,
}

impl ArenaResources {
    pub(crate) fn new(archive: Arc<ArchiveBacking>, registry: Arc<TypeRegistry>) -> Self {
        Self { archive, registry }
    }
}

impl crate::archive_values::arena::ResourceSource for ArenaResources {
    fn read(&self, key: &KilnAssetKey) -> Result<Option<(Vec<u8>, usize)>, String> {
        let id = id_of(key);
        let Some(found) = self.archive.find(&id) else {
            return Ok(None);
        };
        // SF reads the bytes first, then looks the type up (session/asset_resources.rs get_lua_value)
        let bytes = self.archive.bytes(&id, &found)?;
        let r#type = self
            .registry
            .get_by_hash(LookupKey::Qualified(id.type_hash()))
            .ok_or_else(|| TYPE_MISSING.to_owned())?;
        Ok(Some((bytes, r#type.index.as_usize())))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(blocks: &[(u32, u32, &[u8])], fcs: Option<u32>) -> Vec<u8> {
        let mut out = 0xfd2f_b528u32.to_le_bytes().to_vec();
        match fcs {
            // FCS flag 2 (4 bytes), single segment
            Some(size) => {
                out.push(0b1010_0000);
                out.extend_from_slice(&size.to_le_bytes());
            }
            None => {
                out.push(0);
                out.push(0x40); // window descriptor
            }
        }
        for (n, (kind, size, body)) in blocks.iter().enumerate() {
            let last = u32::from(n + 1 == blocks.len());
            let header = last | kind << 1 | size << 3;
            out.extend_from_slice(&header.to_le_bytes()[..3]);
            out.extend_from_slice(body);
        }
        out
    }

    // The bound walks every frame and block: raw blocks produce their size, RLE blocks their size from one byte,
    // compressed blocks at most 128 KiB; a declared content size caps it; skippable frames add nothing.
    #[test]
    fn the_zstd_bound_follows_frames_and_blocks() {
        let raw = frame(&[(0, 3, b"abc")], None);
        assert_eq!(zstd_output_bound(&raw), Ok((3, 3)));
        let rle = frame(&[(1, 100_000, b"x"), (0, 2, b"yz")], None);
        assert_eq!(zstd_output_bound(&rle), Ok((100_002, 100_002)));
        // an RLE block's 21-bit size field can say 2 MiB - 1; a decoder refuses more than 128 KiB per block
        let big_rle = frame(&[(1, (1 << 21) - 1, b"x")], None);
        assert_eq!(zstd_output_bound(&big_rle), Ok((ZSTD_BLOCK_MAX, ZSTD_BLOCK_MAX)));
        let compressed = frame(&[(2, 4, b"\0\0\0\0"), (2, 4, b"\0\0\0\0")], None);
        assert_eq!(
            zstd_output_bound(&compressed),
            Ok((2 * ZSTD_BLOCK_MAX, 2 * ZSTD_BLOCK_MAX))
        );
        // a declared content size caps a complete decode, not what a failing one may write first
        let declared = frame(&[(2, 4, b"\0\0\0\0")], Some(10));
        assert_eq!(zstd_output_bound(&declared), Ok((10, ZSTD_BLOCK_MAX)));
        let mut two = raw.clone();
        two.extend_from_slice(&0x184d_2a5fu32.to_le_bytes());
        two.extend_from_slice(&2u32.to_le_bytes());
        two.extend_from_slice(b"..");
        two.extend_from_slice(&rle);
        assert_eq!(zstd_output_bound(&two), Ok((100_005, 100_005)));
        // truncation anywhere, a bad magic, the reserved block type: errors
        for cut in 1..raw.len() {
            assert!(zstd_output_bound(&raw[..cut]).is_err(), "cut at {cut}");
        }
        assert!(zstd_output_bound(b"").is_err());
        assert!(zstd_output_bound(&[1, 2, 3, 4, 5, 6, 7]).is_err());
        assert!(zstd_output_bound(&frame(&[(3, 0, b"")], None)).is_err());
    }

    // The header's grammar, exactly: lowercase canonical GUIDs, 8 hex type hashes, decimal parts without leading zeros
    // up to INT32_MAX, skill hashes, item/template/material names; anything else (boot's names: forms) is malformed.
    #[test]
    fn queries_follow_the_header_grammar() {
        let ok = |q: &str| parse_query(q.as_bytes()).is_some();
        assert!(ok("resource:01234567-89ab-cdef-0123-456789abcdef/0A0b0c0d/0"));
        assert!(ok("resource:01234567-89ab-cdef-0123-456789abcdef/0a0b0c0d/2147483647"));
        assert!(!ok("resource:01234567-89ab-cdef-0123-456789abcdef/0a0b0c0d/2147483648"));
        assert!(
            !ok("resource:01234567-89AB-cdef-0123-456789abcdef/0a0b0c0d/0"),
            "lowercase GUID only"
        );
        assert!(
            !ok("resource:01234567-89ab-cdef-0123-456789abcdef/0a0b0c0d/01"),
            "no leading zeros"
        );
        assert!(
            !ok("resource:01234567-89ab-cdef-0123-456789abcdef/m6.Root/0"),
            "a type hash, not a name"
        );
        assert!(!ok("resource:0123456789abcdef0123456789abcdef/0a0b0c0d/0"));
        assert!(!ok("resource:01234567-89ab-cdef-0123-456789abcdef/0a0b0c0d/"));
        assert!(!ok("resource:01234567-89ab-cdef-0123-456789abcdef/0a0b0c0d/1/2"));
        assert!(ok("skill:0xDEADbeef") && !ok("skill:deadbeef") && !ok("skill:0xdeadbee"));
        assert!(
            ok("item:Sword_01.a-b") && !ok("item:a..b") && !ok("item:") && !ok(&format!("item:{}", "a".repeat(129)))
        );
        assert!(ok("template:x") && ok("material:Iron_2") && !ok("material:Iron.2"));
        assert!(!ok("names:items") && !ok("item:a/b") && !ok("item:a\0b") && !ok("resource") && !ok(""));
        match parse_query(b"skill:0xDEADBEEF") {
            Some(Query::Named(NamedIndex::Skill, name)) => assert_eq!(name, "0xdeadbeef"),
            _ => panic!("skill"),
        }
    }
}
