// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln-eml: the read-only archive inspector behind `kiln-eml-inspect`.
//!
//! It classifies one side's archive set in a game folder before a start, for the launcher's launch-plan producer:
//! the current input (the live header, resource file and referenced containers), the BASE a kiln-mode start builds
//! from (the working baseline an ownership receipt or a backup seal proves, or else the validated current set as
//! first managed), the BASE's origin token, and role-framed SHA-256 digests of both sets.
//!
//! Read only, by construction: files are opened for reading only (sharing everything on Windows, so no other writer is
//! ever blocked), nothing is created, locked, repaired, adopted or recovered, no Lua or package code runs, no type
//! cache is read or written, nothing is logged to a file, and nothing under ShroudForge's folders is read. Integrity
//! is not provenance: a valid archive or an intact seal never makes a set "vanilla"; the origin is
//! `first-managed-unclassified`, `rebaselined-unclassified` or `unrecorded`, never more.
use kfc::container::KFCFile;
use mod_loader::lua::readonly::{self, ReceiptView, SealCrc};
use serde::Serialize;
use sha2::{Digest as _, Sha256};
use std::{
    collections::BTreeMap,
    fs::{self, File, Metadata},
    io::{self, Read},
    path::Path,
    time::SystemTime,
};

pub const SCHEMA_VERSION: u32 = 1;
pub const COMPONENT: &str = "kiln-eml-inspect";
/// The digest framing (README "Archive inspector"); a new framing gets a new name.
pub const FRAMING: &str = "kiln-eml-inspect archive-set v1";
pub const FIRST_MANAGED: &str = "first-managed-unclassified";
pub const FIRST_MANAGED_LABEL: &str =
    "unclassified: game files as found when Kiln first managed this install";
pub const REBASELINED: &str = "rebaselined-unclassified";
pub const REBASELINED_LABEL: &str =
    "unclassified: game files as found when Kiln took a new baseline after the game changed";
pub const UNRECORDED: &str = "unrecorded";
pub const UNRECORDED_LABEL: &str = "unrecorded: a working baseline from an earlier start; where its files came from was not recorded";
/// The frozen BASE-origin tokens and their exact labels (the launch plan's `game.archiveBaseOrigin`).
pub const ORIGINS: [(&str, &str); 3] = [
    (FIRST_MANAGED, FIRST_MANAGED_LABEL),
    (REBASELINED, REBASELINED_LABEL),
    (UNRECORDED, UNRECORDED_LABEL),
];
/// The note of a receipt whose working baseline is gone over game files that are not Kiln's output from
/// it; the base is then the live set, rebaselined, and a kiln-mode start sets the receipt aside as a recovery copy.
pub const RECEIPT_STALE: &str = "receipt-stale";
/// The note of a backup of this game build that was built over the live header (`classify`): the live set is the base.
pub const BASELINE_STALE: &str = "baseline-stale";
/// A header larger than this is not read into memory (real client headers are a few tens of MiB).
const HEADER_LIMIT: u64 = 512 << 20;
const SEAL_LIMIT: u64 = 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Side {
    Client,
    Server,
}
impl Side {
    pub fn stem(self) -> &'static str {
        match self {
            Side::Client => "enshrouded",
            Side::Server => "enshrouded_server",
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Options {
    pub side: Side,
    /// Hash the referenced containers too (default: their exact lengths only; they are GiBs on a real install).
    pub hash_containers: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Inspection {
    pub schema_version: u32,
    pub inspector: Inspector,
    pub status: Status,
    pub target: Target,
    pub proof: Proof,
    pub input: Option<InputSet>,
    pub base: Option<BaseSet>,
    pub receipt: ReceiptReport,
    pub inventory: Vec<InventoryEntry>,
    pub reasons: Vec<Reason>,
    pub notes: Vec<Reason>,
    pub cost: Cost,
}
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Inspector {
    pub component: &'static str,
    pub version: &'static str,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Status {
    Ok,
    Refused,
}
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Target {
    pub side: Side,
    pub stem: &'static str,
    pub executable: Option<FileDigest>,
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct FileDigest {
    pub file: String,
    pub size: u64,
    pub sha256: String,
}
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Proof {
    pub framing: &'static str,
    pub containers: ProofKind,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum ProofKind {
    Sha256,
    Size,
}
/// One member of a framed set: the archive member it stands for, the physical file that holds it, and its proof.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct SetEntry {
    pub role: &'static str,
    pub member: String,
    pub file: String,
    pub size: u64,
    pub proof: ProofKind,
    pub sha256: Option<String>,
    #[serde(skip)]
    raw: Option<[u8; 32]>,
}
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct InputSet {
    /// `owned-output` (an ownership receipt proves it Kiln's output over the present baseline), `base-copy`
    /// (byte-identical to the BASE) or `unclassified`.
    pub class: &'static str,
    pub sha256: String,
    pub files: Vec<SetEntry>,
}
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct BaseSet {
    /// `receipt-baseline`, `sealed-baseline`, `unsealed-baseline` or `current-set`.
    pub evidence: &'static str,
    pub sha256: String,
    pub origin: Origin,
    /// The working baseline's backup files (size and SHA-256, absent ones null), the identity an origin is keyed to;
    /// null for `current-set` (no backup exists yet).
    pub baseline: Option<BTreeMap<String, Option<Hashed>>>,
    pub files: Vec<SetEntry>,
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, serde::Deserialize)]
pub struct Hashed {
    pub size: u64,
    pub sha256: String,
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Origin {
    pub token: &'static str,
    pub label: &'static str,
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ReceiptReport {
    /// `absent`, `understood` or `unknown`.
    pub state: &'static str,
    pub file: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub size: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub sha256: Option<String>,
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct InventoryEntry {
    pub role: &'static str,
    pub file: String,
    pub size: u64,
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Reason {
    pub code: &'static str,
    pub file: Option<String>,
    pub text: String,
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Cost {
    #[serde(rename = "bytesRead")]
    pub bytes_read: u64,
    #[serde(rename = "filesRead")]
    pub files_read: u64,
}

fn reason(code: &'static str, file: Option<&str>) -> Reason {
    let f = file.unwrap_or("a game file");
    let text = match code {
        "missing" => format!("{f} is missing: verify the game files in Steam, then start again"),
        "invalid" => format!("{f} is not a game archive Kiln can read: verify the game files in Steam, then start again"),
        "link" => format!("{f} is a link or reparse point: Kiln reads only regular files and folders in the game folder"),
        "unreadable" => format!("{f} cannot be read; close programs that hold it, then start again"),
        "changed" => format!("{f} changed while Kiln was reading it; close programs that change the game files, then start again"),
        "receipt-unknown" => format!("{f} is not an ownership receipt this Kiln understands; it is kept as it is for recovery"),
        "baseline-missing" => format!("Kiln's ownership receipt names a working baseline that is no longer in the game folder, and {f} is still Kiln's output built from it: verify the game files in Steam, then start again; Kiln then takes a new baseline from the verified files"),
        "baseline-incomplete" => format!("the working baseline is incomplete ({f} is missing): verify the game files in Steam, then start again"),
        "baseline-corrupt" => "the working baseline does not match its seal or the game's own files: verify the game files in Steam, then start again".into(),
        "baseline-outdated" => "the working baseline is from another game build; the game files as found are the base".into(),
        "baseline-stale" => "the working baseline was built over the game files as found (another mod loader or an earlier start kept modded data under the backup's name): it is not their backup, and the game files as found are the base".into(),
        "seal-outdated" => "the working baseline's seal is out of date; the backups were checked against the game's own files instead".into(),
        "receipt-baseline-mismatch" => "Kiln's ownership receipt names other backups than the ones present (another program replaced them)".into(),
        "receipt-stale" => "Kiln's ownership receipt names a working baseline that is no longer in the game folder, and none of the game files is Kiln's output from it: a Kiln start keeps that receipt as a recovery copy and takes the game files as found as a new baseline".into(),
        "receipt-origin-ignored" => "the origin recorded in Kiln's ownership receipt does not belong to this baseline and is not used".into(),
        "pending-generation" => "Kiln's last publication did not finish; the live files are not counted as Kiln's output".into(),
        "unreferenced-containers" => format!("container files the game header does not list are present ({f}); they are left as they are"),
        _ => code.into(),
    };
    Reason {
        code,
        file: file.map(str::to_owned),
        text,
    }
}

/// Identity and change stamp of an open file: (device, inode, ctime in ns) on Unix, (volume serial, file index,
/// ChangeTime) on Windows. The change time moves on every write or metadata change and an ordinary write cannot put it
/// back (unlike the modification time), so a same-length rewrite in place is seen without reading the file again.
type Identity = (u64, u64, i128);

#[cfg(unix)]
fn identity(file: &File) -> io::Result<Identity> {
    use std::os::unix::fs::MetadataExt;
    let m = file.metadata()?;
    Ok((
        m.dev(),
        m.ino(),
        m.ctime() as i128 * 1_000_000_000 + m.ctime_nsec() as i128,
    ))
}
#[cfg(windows)]
fn identity(file: &File) -> io::Result<Identity> {
    use std::os::windows::io::AsRawHandle;
    use windows::Win32::{
        Foundation::HANDLE,
        Storage::FileSystem::{
            BY_HANDLE_FILE_INFORMATION, FILE_BASIC_INFO, FileBasicInfo, GetFileInformationByHandle,
            GetFileInformationByHandleEx,
        },
    };
    let handle = HANDLE(file.as_raw_handle());
    let mut info = BY_HANDLE_FILE_INFORMATION::default();
    unsafe { GetFileInformationByHandle(handle, &mut info) }.map_err(io::Error::other)?;
    if info.dwFileAttributes & 0x400 != 0 {
        return Err(io::Error::other("reparse point"));
    }
    let mut basic = FILE_BASIC_INFO::default();
    unsafe {
        GetFileInformationByHandleEx(
            handle,
            FileBasicInfo,
            (&raw mut basic).cast(),
            size_of::<FILE_BASIC_INFO>() as u32,
        )
    }
    .map_err(io::Error::other)?;
    Ok((
        info.dwVolumeSerialNumber as u64,
        ((info.nFileIndexHigh as u64) << 32) | info.nFileIndexLow as u64,
        basic.ChangeTime as i128,
    ))
}

/// Opens for reading only. Windows: shares read, write and delete (another writer is never blocked; a change is found
/// by the identity, length and time checks instead) and opens a reparse point itself rather than its target.
fn open_read(path: &Path) -> io::Result<File> {
    let mut o = fs::OpenOptions::new();
    o.read(true);
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        o.share_mode(0x1 | 0x2 | 0x4)
            .custom_flags(0x0020_0000 | 0x0800_0000); // OPEN_REPARSE_POINT | SEQUENTIAL_SCAN
    }
    o.open(path)
}

#[derive(Debug)]
struct Fault {
    code: &'static str,
    file: String,
    /// A reason text that names more than `file` (otherwise `reason`'s text for the code).
    text: Option<String>,
}
fn fault(code: &'static str, file: &str) -> Fault {
    Fault {
        code,
        file: file.to_owned(),
        text: None,
    }
}
/// The working baseline lacks `missing` next to the backup files `present` (a header-only backup of an older EML,
/// say). Refused; the text says how to get a new baseline.
fn incomplete(missing: &str, present: &[&str]) -> Fault {
    let list = match present {
        [] => String::new(),
        [one] => (*one).to_owned(),
        [rest @ .., last] => format!("{} and {last}", rest.join(", ")),
    };
    Fault {
        code: "baseline-incomplete",
        file: missing.to_owned(),
        text: Some(format!(
            "the working baseline is incomplete ({missing} is missing, so {list} cannot be used): verify the game files in Steam, then move {list} out of the game folder and start again; Kiln then takes a new baseline from the verified files"
        )),
    }
}

/// What one file looked like when it was read: checked again, by a fresh open, once everything is read.
struct Seen {
    file: String,
    identity: Identity,
    len: u64,
    modified: Option<SystemTime>,
    /// The SHA-256 of a file whose bytes were parsed (header, backup header, seal, receipt): hashed again at the end,
    /// so a same-length rewrite is caught even where the change time cannot be trusted (FAT, exFAT).
    rehash: Option<[u8; 32]>,
}

struct Read1 {
    size: u64,
    sha256: [u8; 32],
    prefix: Option<[u8; 32]>,
    crc: Option<u64>,
    bytes: Option<Vec<u8>>,
}

struct Reader<'a> {
    game: &'a Path,
    seen: Vec<Seen>,
    bytes: u64,
    files: u64,
    observe: &'a mut dyn FnMut(&str),
}

impl Reader<'_> {
    /// Regular-file metadata of a game-relative file, None when it is missing. Links and reparse points are faults.
    fn meta(&self, file: &str) -> Result<Option<Metadata>, Fault> {
        let path = self.game.join(file);
        if let Err(e) = readonly::checked(self.game, &path) {
            return Err(fault(
                if e.kind() == io::ErrorKind::Other {
                    "link"
                } else {
                    "unreadable"
                },
                file,
            ));
        }
        match fs::symlink_metadata(&path) {
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(None),
            Err(_) => Err(fault("unreadable", file)),
            Ok(m) if m.file_type().is_symlink() => Err(fault("link", file)),
            Ok(m) if !m.is_file() => Err(fault("invalid", file)),
            Ok(m) => {
                #[cfg(windows)]
                {
                    use std::os::windows::fs::MetadataExt;
                    if m.file_attributes() & 0x400 != 0 {
                        return Err(fault("link", file));
                    }
                }
                Ok(Some(m))
            }
        }
    }

    fn exists(&self, file: &str) -> Result<bool, Fault> {
        Ok(self.meta(file)?.is_some())
    }

    /// Opens `file` and checks the handle is the file the path names (no link swapped in) with the same length.
    fn open(&self, file: &str) -> Result<(File, Metadata, Identity), Fault> {
        let before = self.meta(file)?.ok_or_else(|| fault("missing", file))?;
        let handle = open_read(&self.game.join(file)).map_err(|e| match e.kind() {
            io::ErrorKind::NotFound => fault("changed", file),
            _ => fault("unreadable", file),
        })?;
        let id = identity(&handle).map_err(|_| fault("link", file))?;
        let now = handle.metadata().map_err(|_| fault("unreadable", file))?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::MetadataExt;
            if (before.dev(), before.ino()) != (id.0, id.1) {
                return Err(fault("changed", file));
            }
        }
        if !now.is_file()
            || now.len() != before.len()
            || now.modified().ok() != before.modified().ok()
        {
            return Err(fault("changed", file));
        }
        Ok((handle, now, id))
    }

    fn record(&mut self, file: &str, id: Identity, m: &Metadata, rehash: Option<[u8; 32]>) {
        self.seen.push(Seen {
            file: file.to_owned(),
            identity: id,
            len: m.len(),
            modified: m.modified().ok(),
            rehash,
        });
    }

    /// A container by its exact length only: opened (identity), never read.
    fn size(&mut self, file: &str) -> Result<u64, Fault> {
        let (_handle, m, id) = self.open(file)?;
        self.record(file, id, &m, None);
        Ok(m.len())
    }

    /// Reads `file` once: SHA-256, optionally the SHA-256 of its first `prefix` bytes, the seal's CRC-64, and the bytes
    /// themselves (`keep`, bounded by `limit`).
    fn read(
        &mut self,
        file: &str,
        prefix: Option<u64>,
        crc: bool,
        keep: Option<u64>,
    ) -> Result<Read1, Fault> {
        let (mut handle, m, id) = self.open(file)?;
        if keep.is_some_and(|limit| m.len() > limit) {
            return Err(fault("invalid", file));
        }
        let mut hash = Sha256::new();
        let mut crc = crc.then(SealCrc::new);
        let mut prefix_hash = None;
        let mut kept = keep.map(|_| Vec::with_capacity(m.len() as usize));
        let mut size = 0u64;
        let mut buf = vec![0u8; 1 << 20];
        loop {
            let n = match handle.read(&mut buf) {
                Ok(0) => break,
                Ok(n) => n,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(_) => return Err(fault("unreadable", file)),
            };
            let chunk = &buf[..n];
            if let Some(p) = prefix
                && prefix_hash.is_none()
                && size + n as u64 >= p
            {
                let mut h = hash.clone();
                h.update(&chunk[..(p - size) as usize]);
                prefix_hash = Some(h.finalize().into());
            }
            hash.update(chunk);
            if let Some(c) = crc.as_mut() {
                c.update(chunk);
            }
            if let Some(k) = kept.as_mut() {
                k.extend_from_slice(chunk);
            }
            size += n as u64;
            if size > m.len() {
                return Err(fault("changed", file));
            }
        }
        if prefix == Some(0) {
            prefix_hash = Some(Sha256::new().finalize().into());
        }
        let after = handle.metadata().map_err(|_| fault("unreadable", file))?;
        if size != m.len()
            || after.len() != m.len()
            || after.modified().ok() != m.modified().ok()
            || identity(&handle).ok() != Some(id)
        {
            return Err(fault("changed", file));
        }
        drop(handle);
        self.bytes += size;
        self.files += 1;
        let sha256: [u8; 32] = hash.finalize().into();
        self.record(file, id, &m, keep.map(|_| sha256));
        (self.observe)(file);
        Ok(Read1 {
            size,
            sha256,
            prefix: prefix_hash,
            crc: crc.map(SealCrc::finalize),
            bytes: kept,
        })
    }

    /// Every file read or sized is still the same file with the same length, modification and change time (a fresh
    /// open), and every file whose bytes were parsed still has the same SHA-256 (read again: these are small).
    fn recheck(&mut self) -> Result<(), Fault> {
        for i in 0..self.seen.len() {
            let (file, identity, len, modified, rehash) = {
                let s = &self.seen[i];
                (s.file.clone(), s.identity, s.len, s.modified, s.rehash)
            };
            let (mut handle, m, id) = self.open(&file).map_err(|f| {
                if f.code == "missing" {
                    fault("changed", &file)
                } else {
                    f
                }
            })?;
            if id != identity || m.len() != len || m.modified().ok() != modified {
                return Err(fault("changed", &file));
            }
            if let Some(want) = rehash {
                let mut hash = Sha256::new();
                let mut buf = vec![0u8; 1 << 20];
                let mut size = 0u64;
                loop {
                    let n = match handle.read(&mut buf) {
                        Ok(0) => break,
                        Ok(n) => n,
                        Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                        Err(_) => return Err(fault("unreadable", &file)),
                    };
                    hash.update(&buf[..n]);
                    size += n as u64;
                    if size > len {
                        return Err(fault("changed", &file));
                    }
                }
                self.bytes += size;
                let got: [u8; 32] = hash.finalize().into();
                if size != len || got != want {
                    return Err(fault("changed", &file));
                }
            }
        }
        Ok(())
    }
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn entry(
    role: &'static str,
    member: String,
    file: &str,
    size: u64,
    digest: Option<[u8; 32]>,
) -> SetEntry {
    SetEntry {
        role,
        member,
        file: file.to_owned(),
        size,
        proof: if digest.is_some() {
            ProofKind::Sha256
        } else {
            ProofKind::Size
        },
        sha256: digest.map(|d| hex(&d)),
        raw: digest,
    }
}

/// SHA-256 over: the framing name and a newline, the entry count (u64 LE), then per entry sorted by member name
/// (bytewise): the member name's length (u64 LE) and bytes, the proof kind (1 = SHA-256, 2 = length only), the length
/// (u64 LE), and for SHA-256 the 32 raw digest bytes. Members are archive member names (`<stem>.kfc`,
/// `<stem>.kfc_resources`, `<stem>_NNN.dat`), not the physical file that holds them, so a BASE held in backups and an
/// input with the same bytes frame alike.
pub fn frame(files: &[SetEntry]) -> String {
    let mut sorted: Vec<&SetEntry> = files.iter().collect();
    sorted.sort_by(|a, b| a.member.as_bytes().cmp(b.member.as_bytes()));
    let mut h = Sha256::new();
    h.update(FRAMING.as_bytes());
    h.update(b"\n");
    h.update((sorted.len() as u64).to_le_bytes());
    for e in sorted {
        h.update((e.member.len() as u64).to_le_bytes());
        h.update(e.member.as_bytes());
        h.update([match e.raw {
            Some(_) => 1u8,
            None => 2u8,
        }]);
        h.update(e.size.to_le_bytes());
        if let Some(d) = e.raw {
            h.update(d);
        }
    }
    hex(&h.finalize())
}

/// A parsed header with the end of its resource chunks.
struct Header {
    file: KFCFile,
    chunks_end: u64,
}
fn parse(bytes: &[u8]) -> Option<Header> {
    let file = KFCFile::from_reader(&mut io::Cursor::new(bytes), false).ok()?;
    let mut chunks_end = 0u64;
    for c in file.resource_chunks() {
        chunks_end = chunks_end.max(c.offset.checked_add(c.size)?);
    }
    Some(Header { file, chunks_end })
}

/// Inspects one side's archive set in `game`. Never writes; see the module comment.
pub fn inspect(game: &Path, options: &Options) -> Inspection {
    inspect_observed(game, options, &mut |_| {})
}

/// `inspect`, calling `observe` with each game-relative file name right after it was read (a test seam: a test
/// changes files there to prove a change during an inspection refuses it).
pub fn inspect_observed(
    game: &Path,
    options: &Options,
    observe: &mut dyn FnMut(&str),
) -> Inspection {
    let stem = options.side.stem();
    let receipt_file = format!("kiln/eml/ownership/{stem}.json");
    let mut out = Inspection {
        schema_version: SCHEMA_VERSION,
        inspector: Inspector {
            component: COMPONENT,
            version: crate::VERSION,
        },
        status: Status::Ok,
        target: Target {
            side: options.side,
            stem,
            executable: None,
        },
        proof: Proof {
            framing: FRAMING,
            containers: if options.hash_containers {
                ProofKind::Sha256
            } else {
                ProofKind::Size
            },
        },
        input: None,
        base: None,
        receipt: ReceiptReport {
            state: "absent",
            file: receipt_file,
            size: None,
            sha256: None,
        },
        inventory: Vec::new(),
        reasons: Vec::new(),
        notes: Vec::new(),
        cost: Cost {
            bytes_read: 0,
            files_read: 0,
        },
    };
    let mut reader = Reader {
        game,
        seen: Vec::new(),
        bytes: 0,
        files: 0,
        observe,
    };
    let result = classify(&mut reader, options, &mut out).and_then(|()| reader.recheck());
    if let Err(f) = result {
        if out.inventory.is_empty() {
            out.inventory = inventory(game, stem, None).unwrap_or_default();
        }
        out.status = Status::Refused;
        out.input = None;
        out.base = None;
        let mut r = reason(f.code, Some(&f.file));
        if let Some(text) = f.text {
            r.text = text;
        }
        out.reasons.push(r);
    }
    out.cost = Cost {
        bytes_read: reader.bytes,
        files_read: reader.files,
    };
    out
}

fn classify(r: &mut Reader<'_>, options: &Options, out: &mut Inspection) -> Result<(), Fault> {
    let stem = options.side.stem();
    let name = |ext: &str| format!("{stem}.{ext}");
    let container = |i: usize| format!("{stem}_{i:03}.dat");

    let exe = name("exe");
    let read = r.read(&exe, None, false, None)?;
    out.target.executable = Some(FileDigest {
        file: exe,
        size: read.size,
        sha256: hex(&read.sha256),
    });

    // The receipt: its bytes and the identity checked again at the end come from one open (and it is hashed again
    // then). Something at its path that is no receipt this build reads (over the writer's cap, not a regular file) is
    // an unknown receipt: refused and kept as it is.
    let receipt_file = out.receipt.file.clone();
    let bytes = match r.meta(&receipt_file) {
        Ok(None) => None,
        Ok(Some(_)) => match r.read(&receipt_file, None, false, Some(readonly::RECEIPT_LIMIT)) {
            Ok(read) => Some(read.bytes.unwrap_or_default()),
            Err(f) if f.code == "invalid" => Some(Vec::new()),
            Err(f) => return Err(f),
        },
        Err(f) if f.code == "invalid" => Some(Vec::new()),
        Err(f) => return Err(f),
    };
    let receipt = match bytes {
        None => None,
        Some(bytes) => match readonly::receipt(stem, &bytes) {
            Some(view) => {
                out.receipt.state = "understood";
                out.receipt.size = Some(view.size);
                out.receipt.sha256 = Some(view.sha256.clone());
                Some(view)
            }
            None => {
                out.receipt.state = "unknown";
                return Err(fault("receipt-unknown", &receipt_file));
            }
        },
    };

    // The live header, parsed from the very bytes that are hashed.
    let header_name = name("kfc");
    let live_read = r.read(&header_name, None, false, Some(HEADER_LIMIT))?;
    let live = parse(live_read.bytes.as_deref().unwrap_or_default())
        .ok_or_else(|| fault("invalid", &header_name))?;

    // The working baseline's files, by presence first.
    let (bak, res_bak, seal_name) = (
        name("kfc.bak"),
        name("kfc_resources.bak"),
        name("kfc.bak.integrity"),
    );
    let present = (r.exists(&bak)?, r.exists(&res_bak)?, r.exists(&seal_name)?);
    let base_header = if present.0 {
        let read = r.read(&bak, None, true, Some(HEADER_LIMIT))?;
        let parsed = parse(read.bytes.as_deref().unwrap_or_default());
        Some((read, parsed))
    } else {
        None
    };
    let same_build = match &base_header {
        Some((_, Some(h))) => h.file.game_version() == live.file.game_version(),
        Some((_, None)) => return Err(fault("baseline-corrupt", &bak)),
        None => false,
    };
    let base_len = base_header
        .as_ref()
        .and_then(|(_, h)| h.as_ref())
        .map(|h| h.chunks_end);

    // The live resource file (and its prefix the baseline's header covers, for the unsealed-pair rule).
    let resources_name = name("kfc_resources");
    let resources = r.read(
        &resources_name,
        base_len.filter(|_| same_build),
        false,
        None,
    )?;
    if resources.size < live.chunks_end {
        return Err(fault("invalid", &resources_name));
    }
    let live_containers = live.file.containers().len();
    let mut input_files = vec![
        entry(
            "header",
            header_name.clone(),
            &header_name,
            live_read.size,
            Some(live_read.sha256),
        ),
        entry(
            "resources",
            resources_name.clone(),
            &resources_name,
            resources.size,
            Some(resources.sha256),
        ),
    ];
    let mut container_digests: BTreeMap<String, (u64, Option<[u8; 32]>)> = BTreeMap::new();
    for (i, info) in live.file.containers().iter().enumerate() {
        let file = container(i);
        let (size, digest) = container_proof(r, &file, options.hash_containers)?;
        if size != info.size {
            return Err(fault("invalid", &file));
        }
        container_digests.insert(file.clone(), (size, digest));
        input_files.push(entry("container", file.clone(), &file, size, digest));
    }
    out.inventory = inventory(r.game, stem, Some(live_containers))?;
    let retained: Vec<&str> = out
        .inventory
        .iter()
        .filter(|e| e.role == "retained-container")
        .map(|e| e.file.as_str())
        .collect();
    if !retained.is_empty() {
        let list = retained.join(", ");
        out.notes
            .push(reason("unreferenced-containers", Some(&list)));
    }

    // BASE evidence.
    let mut notes = Vec::new();
    let relation = match &base_header {
        Some((_, Some(header))) if same_build => Some(readonly::base_relation(&header.file, &live.file)),
        _ => None,
    };
    let backup_based = match (&base_header, same_build) {
        (None, _) => {
            let left: Vec<&str> = [
                (present.1, res_bak.as_str()),
                (present.2, seal_name.as_str()),
            ]
            .into_iter()
            .filter_map(|(here, name)| here.then_some(name))
            .collect();
            if !left.is_empty() {
                return Err(incomplete(&bak, &left));
            }
            // A receipt whose backups are all gone. Over Kiln's own output, never a base; over
            // files that are not, the live set is a new base (rebaselined) and a kiln-mode start keeps the receipt
            if let Some(view) = &receipt {
                if let Some(file) = kiln_output_live(r, stem, view, &input_files)? {
                    return Err(fault("baseline-missing", &file));
                }
                notes.push(RECEIPT_STALE);
            }
            None
        }
        (Some(_), false) => {
            notes.push("baseline-outdated");
            None
        }
        // A backup of this build built over the live header (an archive writer's output kept under the backup's name,
        // by another mod loader or from an earlier start) is not the base: the live set is (note `baseline-stale`), the
        // writer's rule (load.rs `stale_backup`), unless a live file is still Kiln's output. One that is neither the live
        // header nor built under it does not describe the game's files (`baseline-corrupt`).
        (Some((_, Some(_))), true) if relation != Some(readonly::BaseRelation::Under) => {
            if relation == Some(readonly::BaseRelation::Other) {
                return Err(fault("baseline-corrupt", &bak));
            }
            if let Some(view) = &receipt {
                if let Some(file) = kiln_output_live(r, stem, view, &input_files)? {
                    return Err(fault("baseline-corrupt", &file));
                }
            }
            notes.push(BASELINE_STALE);
            None
        }
        (Some((header_read, Some(header))), true) => {
            if !present.1 {
                let left: Vec<&str> = [(true, bak.as_str()), (present.2, seal_name.as_str())]
                    .into_iter()
                    .filter_map(|(here, name)| here.then_some(name))
                    .collect();
                return Err(incomplete(&res_bak, &left));
            }
            let saved = r.read(&res_bak, Some(header.chunks_end), true, None)?;
            if saved.size < header.chunks_end {
                return Err(fault("baseline-corrupt", &res_bak));
            }
            let unsealed_ok = resources.prefix.is_some() && resources.prefix == saved.prefix;
            let mut seal_digest = None;
            let sealed = if present.2 {
                let seal = r
                    .read(&seal_name, None, false, Some(SEAL_LIMIT))
                    .map_err(|f| {
                        if f.code == "invalid" {
                            fault("baseline-corrupt", &seal_name)
                        } else {
                            f
                        }
                    })?;
                seal_digest = Some(Hashed {
                    size: seal.size,
                    sha256: hex(&seal.sha256),
                });
                let recorded = readonly::seal(seal.bytes.as_deref().unwrap_or_default())
                    .ok_or_else(|| fault("baseline-corrupt", &seal_name))?;
                let actual = (
                    (header_read.size, header_read.crc.unwrap_or_default()),
                    (saved.size, saved.crc.unwrap_or_default()),
                );
                if recorded == actual {
                    true
                } else {
                    // The writer's rules: a pair replaced since it was sealed (other lengths), or one byte-identical to
                    // the live files, is checked like an unsealed pair; anything else is a damaged baseline.
                    let replaced = recorded.0.0 != header_read.size || recorded.1.0 != saved.size;
                    let equal_live =
                        header_read.sha256 == live_read.sha256 && saved.sha256 == resources.sha256;
                    if (replaced || equal_live) && unsealed_ok {
                        notes.push("seal-outdated");
                        false
                    } else {
                        return Err(fault("baseline-corrupt", &res_bak));
                    }
                }
            } else if unsealed_ok {
                false
            } else {
                return Err(fault("baseline-corrupt", &res_bak));
            };
            let mut baseline = BTreeMap::new();
            baseline.insert(
                bak.clone(),
                Some(Hashed {
                    size: header_read.size,
                    sha256: hex(&header_read.sha256),
                }),
            );
            baseline.insert(
                res_bak.clone(),
                Some(Hashed {
                    size: saved.size,
                    sha256: hex(&saved.sha256),
                }),
            );
            baseline.insert(seal_name.clone(), seal_digest);
            let mut files = vec![
                entry(
                    "header",
                    header_name.clone(),
                    &bak,
                    header_read.size,
                    Some(header_read.sha256),
                ),
                entry(
                    "resources",
                    resources_name.clone(),
                    &res_bak,
                    saved.size,
                    Some(saved.sha256),
                ),
            ];
            for (i, info) in header.file.containers().iter().enumerate() {
                let file = container(i);
                let (size, digest) = match container_digests.get(&file) {
                    Some(known) => *known,
                    None => container_proof(r, &file, options.hash_containers)?,
                };
                if size != info.size {
                    return Err(fault("baseline-corrupt", &file));
                }
                files.push(entry("container", file.clone(), &file, size, digest));
            }
            Some((sealed, baseline, files, header.file.containers().len()))
        }
        (Some((_, None)), true) => unreachable!("an unparsable baseline header returned above"),
    };

    let mut input_class = "unclassified";
    let base = match backup_based {
        None => BaseSet {
            evidence: "current-set",
            sha256: frame(&input_files),
            // backups of another build, or a receipt whose backups are gone, mean the install was managed before:
            // not "first managed"
            origin: if notes.contains(&"baseline-outdated") || notes.contains(&RECEIPT_STALE) || notes.contains(&BASELINE_STALE) {
                Origin {
                    token: REBASELINED,
                    label: REBASELINED_LABEL,
                }
            } else {
                Origin {
                    token: FIRST_MANAGED,
                    label: FIRST_MANAGED_LABEL,
                }
            },
            baseline: None,
            files: input_files.clone(),
        },
        Some((sealed, baseline, files, base_containers)) => {
            let listed: BTreeMap<String, (u64, String)> = baseline
                .iter()
                .filter_map(|(n, d)| d.as_ref().map(|d| (n.clone(), (d.size, d.sha256.clone()))))
                .collect();
            let matched = receipt.as_ref().filter(|v| v.backups == listed);
            if receipt.is_some() && matched.is_none() {
                notes.push("receipt-baseline-mismatch");
            }
            let origin = match matched {
                Some(view) => match &view.base_origin {
                    None => Origin {
                        token: UNRECORDED,
                        label: UNRECORDED_LABEL,
                    },
                    Some(recorded) => match recorded_origin(recorded, &baseline) {
                        Some(kept) => kept,
                        None => {
                            notes.push("receipt-origin-ignored");
                            Origin {
                                token: UNRECORDED,
                                label: UNRECORDED_LABEL,
                            }
                        }
                    },
                },
                None => Origin {
                    token: UNRECORDED,
                    label: UNRECORDED_LABEL,
                },
            };
            if let Some(view) = matched {
                if owned(r, view, &input_files)? {
                    input_class = "owned-output";
                } else if !view.unproven.is_empty() || view.previous.is_some() {
                    notes.push("pending-generation");
                }
            }
            if input_class == "unclassified"
                && live_containers == base_containers
                && input_files[0].raw == files[0].raw
                && input_files[1].raw == files[1].raw
            {
                input_class = "base-copy";
            }
            BaseSet {
                evidence: if matched.is_some() {
                    "receipt-baseline"
                } else if sealed {
                    "sealed-baseline"
                } else {
                    "unsealed-baseline"
                },
                sha256: frame(&files),
                origin,
                baseline: Some(baseline),
                files,
            }
        }
    };
    let mut ordered: Vec<Reason> = notes.into_iter().map(|n| reason(n, None)).collect();
    ordered.append(&mut out.notes);
    out.notes = ordered;
    out.input = Some(InputSet {
        class: input_class,
        sha256: frame(&input_files),
        files: input_files,
    });
    out.base = Some(base);
    Ok(())
}

fn container_proof(
    r: &mut Reader<'_>,
    file: &str,
    hash: bool,
) -> Result<(u64, Option<[u8; 32]>), Fault> {
    if hash {
        let read = r.read(file, None, false, None)?;
        Ok((read.size, Some(read.sha256)))
    } else {
        Ok((r.size(file)?, None))
    }
}

/// The rebaseline rule, the rebaseline rule after lost backups, one verdict for the pre-Play inspection and the boot
/// witness (which acts on its note): the first file of the validated live set `input` that is still Kiln's output from
/// `view`, if any. A file is Kiln's output when it equals (size first, then SHA-256; a container listed at its size is
/// read only then) a file the receipt lists for its current or its previous generation, unless those listed bytes are
/// the receipt's own backup of that member (an output file Kiln left as the game's, which the game's verified files
/// are too). Files the live header does not reference are not the live set and are not compared. None: the live set
/// may be a new BASE.
fn kiln_output_live(
    r: &mut Reader<'_>,
    stem: &str,
    view: &ReceiptView,
    input: &[SetEntry],
) -> Result<Option<String>, Fault> {
    let backup_of = |member: &str| {
        let ext = if member == format!("{stem}.kfc") {
            "kfc.bak"
        } else if member == format!("{stem}.kfc_resources") {
            "kfc_resources.bak"
        } else {
            return None;
        };
        view.backups.get(&format!("{stem}.{ext}"))
    };
    for e in input {
        let mut live = e.sha256.clone();
        for generation in std::iter::once(&view.files).chain(view.previous.as_ref()) {
            let Some(listed) = generation.get(&e.file) else {
                continue;
            };
            if listed.0 != e.size || backup_of(&e.member) == Some(listed) {
                continue;
            }
            let digest = match &live {
                Some(d) => d.clone(),
                None => {
                    let d = hex(&r.read(&e.file, None, false, None)?.sha256);
                    live = Some(d.clone());
                    d
                }
            };
            if digest == listed.1 {
                return Ok(Some(e.file.clone()));
            }
        }
    }
    Ok(None)
}

/// The receipt's `baseOrigin` is kept only when it is exactly one of the unclassified tokens with its own exact label
/// (`first-managed-unclassified` or `rebaselined-unclassified`), keyed to this very baseline (every backup file's
/// size and SHA-256, absent ones null). Anything else is ignored; nothing is ever upgraded.
fn recorded_origin(
    recorded: &serde_json::Value,
    baseline: &BTreeMap<String, Option<Hashed>>,
) -> Option<Origin> {
    let o = recorded.as_object()?;
    let keyed = o
        .get("baseline")
        .and_then(|b| serde_json::from_value::<BTreeMap<String, Option<Hashed>>>(b.clone()).ok())
        .is_some_and(|b| &b == baseline);
    if o.len() != 3 || !keyed {
        return None;
    }
    let (token, label) = (o.get("token")?.as_str()?, o.get("label")?.as_str()?);
    [
        (FIRST_MANAGED, FIRST_MANAGED_LABEL),
        (REBASELINED, REBASELINED_LABEL),
    ]
    .into_iter()
    .find(|&(t, l)| t == token && l == label)
    .map(|(token, label)| Origin { token, label })
}

/// The writer's ownership rule over the bytes just read: the receipt's current generation (when nothing in it is
/// unproven) or its previous one is live, file for file. Lengths first; a container's content is read only then.
fn owned(r: &mut Reader<'_>, view: &ReceiptView, input: &[SetEntry]) -> Result<bool, Fault> {
    let generations = [
        view.unproven.is_empty().then_some(&view.files),
        view.previous.as_ref(),
    ];
    for generation in generations.into_iter().flatten() {
        let mut lengths = true;
        for (file, (size, _)) in generation {
            let now = match input.iter().find(|e| &e.file == file) {
                Some(e) => Some(e.size),
                None => r.meta(file)?.map(|m| m.len()),
            };
            lengths &= now == Some(*size);
        }
        if !lengths {
            continue;
        }
        let mut all = true;
        for (file, (_, sha)) in generation {
            let digest = match input
                .iter()
                .find(|e| &e.file == file)
                .and_then(|e| e.sha256.clone())
            {
                Some(d) => d,
                None => hex(&r.read(file, None, false, None)?.sha256),
            };
            all &= &digest == sha;
        }
        if all {
            return Ok(true);
        }
    }
    Ok(false)
}

/// The side's files in the game folder by role (names and lengths only; nothing is read). `referenced`: how many
/// containers the validated live header lists; unknown when the inspection refused before that.
fn inventory(
    game: &Path,
    stem: &str,
    referenced: Option<usize>,
) -> Result<Vec<InventoryEntry>, Fault> {
    let header = format!("{stem}.kfc");
    let mut out = Vec::new();
    let entries = fs::read_dir(game).map_err(|_| fault("unreadable", "."))?;
    for e in entries {
        let e = e.map_err(|_| fault("unreadable", "."))?;
        let Some(file) = e.file_name().to_str().map(str::to_owned) else {
            continue;
        };
        let role = if file == format!("{stem}.exe") {
            "executable"
        } else if file == header {
            "header"
        } else if file == format!("{stem}.kfc_resources") {
            "resources"
        } else if file == format!("{stem}.kfc.bak") {
            "base-header"
        } else if file == format!("{stem}.kfc_resources.bak") {
            "base-resources"
        } else if file == format!("{stem}.kfc.bak.integrity") {
            "base-seal"
        } else if let Some(i) = file
            .strip_prefix(&format!("{stem}_"))
            .and_then(|s| s.strip_suffix(".dat"))
            .filter(|s| !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit()))
            .and_then(|s| s.parse::<usize>().ok())
        {
            match referenced {
                Some(n) if i < n => "container",
                Some(_) => "retained-container",
                None => "container-unverified",
            }
        } else {
            continue;
        };
        let size = fs::symlink_metadata(e.path()).map(|m| m.len()).unwrap_or(0);
        out.push(InventoryEntry { role, file, size });
    }
    let receipt = format!("kiln/eml/ownership/{stem}.json");
    if let Ok(m) = fs::symlink_metadata(game.join(&receipt)) {
        out.push(InventoryEntry {
            role: "receipt",
            file: receipt,
            size: m.len(),
        });
    }
    out.sort_by(|a, b| a.file.cmp(&b.file));
    Ok(out)
}
