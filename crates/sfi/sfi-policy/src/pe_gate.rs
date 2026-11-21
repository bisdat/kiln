// SPDX-License-Identifier: MIT
//! Read-only PE32+ import gate. Never calls the Windows loader or executes code.
//!
//! A negative string scan is NOT a dynamic-dependency
//! proof. Pass requires an explicit trusted review bound to the exact inspected
//! bytes for every package image. Reviews are host policy, never plan booleans or
//! player acknowledgements. System dependency identities/search and scan-to-load
//! file identity still require the later loader's verification. This is not an
//! OS security sandbox or a guarantee against arbitrary native side effects.

use std::collections::{BTreeMap, BTreeSet, VecDeque};

pub const MAX_IMAGE_BYTES: usize = 128 * 1024 * 1024;
const MAX_IMAGES: usize = 1024;
const MAX_SECTIONS: usize = 96;
const MAX_IMPORTS: usize = 16_384;
const MAX_NAMES: usize = 4 * 1024 * 1024;
const MAX_NAME: usize = 4096;
const BANNED: [&str; 2] = ["shroudforge-runtime.dll", concat!("kf", "c-runtime.dll")];
const LOOKUPS: [&str; 10] = [
    "GetProcAddress",
    "LoadLibraryA",
    "LoadLibraryW",
    "LoadLibraryExA",
    "LoadLibraryExW",
    "LdrLoadDll",
    "LdrGetProcedureAddress",
    "shroudforge_create",
    concat!("Kf", "cRuntimeInitialize"),
    "shroudforge-runtime.dll",
];

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ImportKind {
    Ordinary,
    Delay,
    ForwardedExport,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Import {
    pub kind: ImportKind,
    pub module: String,
    pub symbols: Vec<String>,
    /// Names/ordinals of an export that forwards here; empty for import tables.
    pub exported_as: Vec<String>,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PeImports {
    pub imports: Vec<Import>,
    pub evidence: Vec<String>,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PeError(pub String);
impl std::fmt::Display for PeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.0.fmt(f)
    }
}
impl std::error::Error for PeError {}
fn bad(text: &str) -> PeError {
    PeError(text.into())
}
fn add(a: usize, b: usize) -> Result<usize, PeError> {
    a.checked_add(b).ok_or_else(|| bad("integer overflow"))
}
fn radd(a: u32, b: u32) -> Result<u32, PeError> {
    a.checked_add(b).ok_or_else(|| bad("RVA overflow"))
}
fn bytes(b: &[u8], at: usize, size: usize) -> Result<&[u8], PeError> {
    b.get(at..add(at, size)?)
        .ok_or_else(|| bad("truncated file range"))
}
fn u16_at(b: &[u8], at: usize) -> Result<u16, PeError> {
    let b = bytes(b, at, 2)?;
    Ok(u16::from_le_bytes([b[0], b[1]]))
}
fn u32_at(b: &[u8], at: usize) -> Result<u32, PeError> {
    let b = bytes(b, at, 4)?;
    Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}
fn u64_at(b: &[u8], at: usize) -> Result<u64, PeError> {
    let b = bytes(b, at, 8)?;
    Ok(u64::from_le_bytes([
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
    ]))
}

struct Section {
    rva: u32,
    virtual_end: u32,
    raw: usize,
    size: usize,
}
struct Pe<'a> {
    bytes: &'a [u8],
    sections: Vec<Section>,
    headers: usize,
    image_base: u64,
    directories: Vec<(u32, u32)>,
    name_bytes: usize,
    records: usize,
}
impl<'a> Pe<'a> {
    fn new(b: &'a [u8], system: bool) -> Result<Self, PeError> {
        if b.len() > MAX_IMAGE_BYTES || bytes(b, 0, 2)? != b"MZ" {
            return Err(bad("not a bounded DOS/PE image"));
        }
        let pe = u32_at(b, 0x3c)? as usize;
        if pe < 64 || bytes(b, pe, 4)? != b"PE\0\0" {
            return Err(bad("invalid PE signature"));
        }
        let coff = add(pe, 4)?;
        if u16_at(b, coff)? != 0x8664 || u16_at(b, add(coff, 18)?)? & 0x2000 == 0 {
            return Err(bad("requires an AMD64 Windows DLL"));
        }
        let count = u16_at(b, add(coff, 2)?)? as usize;
        if count == 0 || count > MAX_SECTIONS {
            return Err(bad("invalid section count"));
        }
        let optional = add(coff, 20)?;
        let optional_size = u16_at(b, add(coff, 16)?)? as usize;
        if optional_size < 112 || u16_at(b, optional)? != 0x20b {
            return Err(bad("requires PE32+ optional header"));
        }
        bytes(b, optional, optional_size)?;
        let subsystem = u16_at(b, add(optional, 68)?)?;
        if !matches!(subsystem, 2 | 3) && !(system && subsystem == 1) {
            return Err(bad("unsupported Windows subsystem"));
        }
        let dirs = u32_at(b, add(optional, 108)?)? as usize;
        if dirs > 16 || add(112, dirs * 8)? > optional_size {
            return Err(bad("invalid data directory count"));
        }
        let headers = u32_at(b, add(optional, 60)?)? as usize;
        let image_size = u32_at(b, add(optional, 56)?)?;
        let table = add(optional, optional_size)?;
        if headers < add(table, count * 40)? || headers > b.len() || image_size < headers as u32 {
            return Err(bad("invalid header/image size"));
        }
        let mut sections: Vec<Section> = Vec::new();
        for i in 0..count {
            let at = add(table, i * 40)?;
            let rva = u32_at(b, add(at, 12)?)?;
            let raw_size = u32_at(b, add(at, 16)?)? as usize;
            let raw = u32_at(b, add(at, 20)?)? as usize;
            let virtual_size = u32_at(b, add(at, 8)?)?;
            let virtual_end = radd(rva, virtual_size.max(raw_size as u32))?;
            if rva < headers as u32 || virtual_end > image_size {
                return Err(bad("section exceeds image or overlaps headers"));
            }
            if raw_size > 0 {
                if raw < headers {
                    return Err(bad("section file data overlaps headers"));
                }
                bytes(b, raw, raw_size)?;
            }
            for previous in &sections {
                if rva < previous.virtual_end && previous.rva < virtual_end {
                    return Err(bad("overlapping section RVA ranges"));
                }
                if raw_size > 0
                    && previous.size > 0
                    && raw < add(previous.raw, previous.size)?
                    && previous.raw < add(raw, raw_size)?
                {
                    return Err(bad("overlapping section file ranges"));
                }
            }
            sections.push(Section {
                rva,
                virtual_end,
                raw,
                size: raw_size,
            });
        }
        let mut directories = Vec::new();
        for i in 0..dirs {
            let at = add(optional, 112 + i * 8)?;
            directories.push((u32_at(b, at)?, u32_at(b, add(at, 4)?)?));
        }
        Ok(Self {
            bytes: b,
            sections,
            headers,
            image_base: u64_at(b, add(optional, 24)?)?,
            directories,
            name_bytes: 0,
            records: 0,
        })
    }
    fn offset(&self, rva: u32, size: usize) -> Result<usize, PeError> {
        let end = (rva as usize)
            .checked_add(size)
            .ok_or_else(|| bad("RVA range overflow"))?;
        if (rva as usize) < self.headers && end <= self.headers {
            bytes(self.bytes, rva as usize, size)?;
            return Ok(rva as usize);
        }
        for section in &self.sections {
            if rva >= section.rva && (rva as usize - section.rva as usize) < section.size {
                let delta = (rva - section.rva) as usize;
                if add(delta, size)? > section.size {
                    return Err(bad("RVA range leaves file-backed section"));
                }
                let at = add(section.raw, delta)?;
                bytes(self.bytes, at, size)?;
                return Ok(at);
            }
        }
        Err(bad("unmapped or zero-filled RVA"))
    }
    fn mapped(&self, rva: u32, size: u32) -> Result<(), PeError> {
        let end = radd(rva, size)?;
        if (rva as usize) < self.headers && (end as usize) <= self.headers
            || self.sections.iter().any(|section| {
                rva >= section.rva && rva < section.virtual_end && end <= section.virtual_end
            })
        {
            return Ok(());
        }
        Err(bad("export target is outside mapped virtual image ranges"))
    }
    fn reject_clr(&self) -> Result<(), PeError> {
        let Some((rva, size)) = self.directory(14)? else {
            return Ok(());
        };
        if size < 72 {
            return Err(bad("short COR20 header"));
        }
        let at = self.offset(rva, 72)?;
        let header_size = u32_at(self.bytes, at)?;
        if header_size < 72 || header_size > size {
            return Err(bad("invalid COR20 header size"));
        }
        let metadata = u32_at(self.bytes, at + 8)?;
        let metadata_size = u32_at(self.bytes, at + 12)?;
        if metadata == 0 || metadata_size < 4 {
            return Err(bad("invalid CLR metadata directory"));
        }
        let metadata_at = self.offset(metadata, metadata_size as usize)?;
        if bytes(self.bytes, metadata_at, 4)? != b"BSJB" {
            return Err(bad("invalid CLR metadata signature"));
        }
        if u32_at(self.bytes, at + 16)? & 1 != 0 {
            Err(bad("IL-only CLR image is not a native DLL"))
        } else {
            Err(bad(
                "mixed-mode CLR images are unsupported by native admission",
            ))
        }
    }
    fn directory(&self, index: usize) -> Result<Option<(u32, u32)>, PeError> {
        let (rva, size) = self.directories.get(index).copied().unwrap_or((0, 0));
        if rva == 0 && size == 0 {
            return Ok(None);
        }
        if rva == 0 || size == 0 {
            return Err(bad("incomplete data directory"));
        }
        self.offset(rva, size as usize)?;
        Ok(Some((rva, size)))
    }
    fn name(&mut self, rva: u32) -> Result<String, PeError> {
        let mut out = Vec::new();
        for i in 0..MAX_NAME {
            self.name_bytes += 1;
            if self.name_bytes > MAX_NAMES {
                return Err(bad("name budget exceeded"));
            }
            let at = self.offset(radd(rva, i as u32)?, 1)?;
            let b = self.bytes[at];
            if b == 0 {
                if out.is_empty() {
                    return Err(bad("empty import/forwarder name"));
                }
                return String::from_utf8(out).map_err(|_| bad("non-ASCII import name"));
            }
            if !(32..127).contains(&b) {
                return Err(bad("non-ASCII/control byte in import name"));
            }
            out.push(b);
        }
        Err(bad("unterminated or overlong import name"))
    }
    fn count(&mut self) -> Result<(), PeError> {
        self.records += 1;
        if self.records > MAX_IMPORTS {
            return Err(bad("import record budget exceeded"));
        }
        Ok(())
    }
    fn pointer_rva(&self, value: u64, relative: bool) -> Result<u32, PeError> {
        let value = if relative {
            value
        } else {
            value
                .checked_sub(self.image_base)
                .ok_or_else(|| bad("delay VA below image base"))?
        };
        u32::try_from(value).map_err(|_| bad("import pointer is not an image RVA"))
    }
    fn thunks(&mut self, table: u32, relative: bool) -> Result<Vec<String>, PeError> {
        if table == 0 {
            return Err(bad("missing import thunk table"));
        }
        let mut result = Vec::new();
        for i in 0..MAX_IMPORTS {
            let at = self.offset(radd(table, (i * 8) as u32)?, 8)?;
            let value = u64_at(self.bytes, at)?;
            if value == 0 {
                return Ok(result);
            }
            self.count()?;
            if value & (1 << 63) != 0 {
                if value & !((1 << 63) | 0xffff) != 0 {
                    return Err(bad("invalid ordinal import"));
                }
                result.push(format!("#{}", value & 0xffff));
            } else {
                let rva = self.pointer_rva(value, relative)?;
                self.offset(rva, 2)?; // IMAGE_IMPORT_BY_NAME hint
                result.push(self.name(radd(rva, 2)?)?);
            }
        }
        Err(bad("unterminated import thunk table"))
    }
    fn imports(&mut self, delay: bool) -> Result<Vec<Import>, PeError> {
        let Some((rva, size)) = self.directory(if delay { 13 } else { 1 })? else {
            return Ok(Vec::new());
        };
        let stride = if delay { 32 } else { 20 };
        let mut result = Vec::new();
        for i in 0..(size as usize / stride).min(MAX_IMPORTS) {
            let at = self.offset(radd(rva, (i * stride) as u32)?, stride)?;
            if bytes(self.bytes, at, stride)?.iter().all(|b| *b == 0) {
                return Ok(result);
            }
            self.count()?;
            let (name, thunk, relative) = if delay {
                let flags = u32_at(self.bytes, at)?;
                if flags & !1 != 0 {
                    return Err(bad("unknown delay import attributes"));
                }
                let relative = flags == 1;
                let name = self.pointer_rva(u32_at(self.bytes, at + 4)? as u64, relative)?;
                let table = u32_at(self.bytes, at + 16)?;
                let table = if table == 0 {
                    u32_at(self.bytes, at + 12)?
                } else {
                    table
                };
                (name, self.pointer_rva(table as u64, relative)?, relative)
            } else {
                let table = u32_at(self.bytes, at)?;
                (
                    u32_at(self.bytes, at + 12)?,
                    if table == 0 {
                        u32_at(self.bytes, at + 16)?
                    } else {
                        table
                    },
                    true,
                )
            };
            let module = self.name(name)?;
            let symbols = self.thunks(thunk, relative)?;
            result.push(Import {
                kind: if delay {
                    ImportKind::Delay
                } else {
                    ImportKind::Ordinary
                },
                module,
                symbols,
                exported_as: Vec::new(),
            });
        }
        Err(bad("import directory has no bounded null descriptor"))
    }
    fn forwarders(&mut self) -> Result<Vec<Import>, PeError> {
        let Some((rva, size)) = self.directory(0)? else {
            return Ok(Vec::new());
        };
        if size < 40 {
            return Err(bad("short export directory"));
        }
        let at = self.offset(rva, 40)?;
        let count = u32_at(self.bytes, at + 20)? as usize;
        if count > MAX_IMPORTS {
            return Err(bad("export count budget exceeded"));
        }
        let functions = u32_at(self.bytes, at + 28)?;
        if count != 0 && functions == 0 {
            return Err(bad("missing export address table"));
        }
        let ordinal_base = u32_at(self.bytes, at + 16)?;
        let name_count = u32_at(self.bytes, at + 24)? as usize;
        let name_table = u32_at(self.bytes, at + 32)?;
        let ordinal_table = u32_at(self.bytes, at + 36)?;
        if name_count > MAX_IMPORTS {
            return Err(bad("export name count budget exceeded"));
        }
        let mut aliases = vec![Vec::new(); count];
        for i in 0..name_count {
            let name_at = self.offset(radd(name_table, (i * 4) as u32)?, 4)?;
            let ordinal_at = self.offset(radd(ordinal_table, (i * 2) as u32)?, 2)?;
            let index = u16_at(self.bytes, ordinal_at)? as usize;
            if index >= count {
                return Err(bad("export name ordinal outside function table"));
            }
            let name_rva = u32_at(self.bytes, name_at)?;
            aliases[index].push(self.name(name_rva)?);
        }
        let end = radd(rva, size)?;
        let mut result = Vec::new();
        for (i, mut names) in aliases.into_iter().enumerate() {
            let at = self.offset(radd(functions, (i * 4) as u32)?, 4)?;
            let target = u32_at(self.bytes, at)?;
            if target >= rva && target < end {
                self.count()?;
                let forward = self.name(target)?;
                if target as usize + forward.len() + 1 > end as usize {
                    return Err(bad("forwarder string leaves export directory"));
                }
                let (module, symbol) = forward
                    .rsplit_once('.')
                    .ok_or_else(|| bad("invalid export forwarder"))?;
                if module.is_empty() || symbol.is_empty() {
                    return Err(bad("empty export forwarder component"));
                }
                let module = if module.to_ascii_lowercase().ends_with(".dll") {
                    module.to_owned()
                } else {
                    format!("{module}.dll")
                };
                names.push(format!("#{}", radd(ordinal_base, i as u32)?));
                result.push(Import {
                    kind: ImportKind::ForwardedExport,
                    module,
                    symbols: vec![symbol.into()],
                    exported_as: names,
                });
            } else if target != 0 {
                self.mapped(target, 1)?;
            }
        }
        Ok(result)
    }
}

/// Parse one immutable file snapshot. Malformed or unsupported layouts are errors,
/// never a reason to invoke the Windows loader as a probe.
pub fn parse(bytes: &[u8]) -> Result<PeImports, PeError> {
    let mut pe = Pe::new(bytes, false)?;
    pe.reject_clr()?;
    let mut imports = pe.imports(false)?;
    imports.extend(pe.imports(true)?);
    imports.extend(pe.forwarders()?);
    let mut evidence = Vec::new();
    for name in LOOKUPS.iter().copied().chain(BANNED) {
        let ascii = name.as_bytes();
        let wide: Vec<u8> = name.encode_utf16().flat_map(u16::to_le_bytes).collect();
        if bytes
            .windows(ascii.len())
            .any(|b| b.eq_ignore_ascii_case(ascii))
            || bytes
                .windows(wide.len())
                .any(|b| b.eq_ignore_ascii_case(&wide))
        {
            evidence.push(name.into());
        }
    }
    evidence.sort();
    evidence.dedup();
    Ok(PeImports { imports, evidence })
}

/// Only the sealed OS resolver may use native-subsystem images (e.g. ntdll).
/// Export forwarders are retained with source names for demand-driven resolution.
/// Behind `os-binding`: kiln-sfi's resolver feeds it loader-verified system snapshots.
#[cfg(feature = "os-binding")]
pub fn parse_system(bytes: &[u8]) -> Result<PeImports, PeError> {
    let mut pe = Pe::new(bytes, true)?;
    pe.reject_clr()?;
    let mut imports = pe.imports(false)?;
    imports.extend(pe.imports(true)?);
    imports.extend(pe.forwarders()?);
    Ok(PeImports {
        imports,
        evidence: Vec::new(),
    })
}
/// True for the two runtime basenames no admitted closure may import, in any spelling.
pub fn banned_module(name: &str) -> bool {
    BANNED.contains(&basename(name).as_str())
}
#[derive(Debug, Clone, Copy)]
pub struct Image<'a> {
    pub path: &'a str,
    pub bytes: &'a [u8],
}
/// Host-owned, externally reviewed contract bound by exact byte equality. This
/// API never manufactures a review from missing strings, a plan's proof field,
/// a user acknowledgement, filename, timestamp or successful previous load.
pub struct ReviewedImage<'a> {
    pub path: &'a str,
    pub bytes: &'a [u8],
    pub contract: &'a str,
    pub dynamic_lookups_reviewed: bool,
}
#[derive(Default)]
pub struct Policy<'a> {
    pub reviews: &'a [ReviewedImage<'a>],
    /// Exact host-approved system module names. The real loader must bind the
    /// actual system search/loaded-image identities; this is not a general list
    /// of names allowed to override package-local DLLs.
    pub system_modules: &'a [&'a str],
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InspectionIssueKind {
    Structural,
    DynamicEvidence,
    UnresolvedDependency,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InspectionIssue {
    pub kind: InspectionIssueKind,
    pub message: String,
}
impl InspectionIssue {
    fn structural(message: String) -> Self {
        Self {
            kind: InspectionIssueKind::Structural,
            message,
        }
    }
    fn dynamic(message: String) -> Self {
        Self {
            kind: InspectionIssueKind::DynamicEvidence,
            message,
        }
    }
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum GateVerdict {
    Pass,
    Banned {
        image: String,
        module: String,
        symbols: Vec<String>,
        kind: ImportKind,
    },
    UnknownDynamic {
        reasons: Vec<InspectionIssue>,
    },
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GateReport {
    pub verdict: GateVerdict,
    pub inspected: Vec<String>,
    pub evidence: Vec<String>,
}

fn basename(name: &str) -> String {
    let name = name
        .rsplit(['/', '\\'])
        .next()
        .unwrap_or(name)
        .trim_end_matches([' ', '.'])
        .to_ascii_lowercase();
    if name.contains('.') {
        name
    } else {
        format!("{name}.dll")
    }
}
/// The lower-case inventory key of a package-relative image path, or `None` when the path
/// is not a canonical ASCII forward-slash path (such paths stay unknown, never matched).
pub fn canonical(path: &str) -> Option<String> {
    if path.is_empty()
        || !path.is_ascii()
        || path.len() > 32768
        || path.contains(['\\', ':', '\0'])
        || path
            .split('/')
            .any(|p| p.is_empty() || p == "." || p == ".." || p.ends_with([' ', '.']))
    {
        return None;
    }
    Some(path.to_ascii_lowercase())
}

/// Inspect the transitive package-local closure without filesystem access. All
/// images are bounded immutable snapshots. A later loader must reopen/revalidate
/// the same identities and dependency search before using this result.
pub fn gate(entry: &str, images: &[Image<'_>], policy: &Policy<'_>) -> GateReport {
    let mut report = GateReport {
        verdict: GateVerdict::Pass,
        inspected: Vec::new(),
        evidence: Vec::new(),
    };
    let mut reasons = Vec::new();
    let mut inventory = BTreeMap::new();
    if images.len() > MAX_IMAGES {
        reasons.push(InspectionIssue::structural(
            "package image-count budget exceeded".into(),
        ));
    }
    let mut total = 0usize;
    for image in images.iter().take(MAX_IMAGES) {
        total = total.saturating_add(image.bytes.len());
        let Some(key) = canonical(image.path) else {
            reasons.push(InspectionIssue::structural(format!(
                "noncanonical package image path: {}",
                image.path
            )));
            continue;
        };
        if inventory.insert(key, image).is_some() {
            reasons.push(InspectionIssue::structural(format!(
                "case-colliding package image: {}",
                image.path
            )));
        }
    }
    if total > 256 * 1024 * 1024 {
        reasons.push(InspectionIssue::structural(
            "package byte budget exceeded".into(),
        ));
    }
    if !reasons.is_empty() {
        report.verdict = GateVerdict::UnknownDynamic { reasons };
        return report;
    }
    let mut pending = VecDeque::from([entry.to_ascii_lowercase()]);
    let mut visited = BTreeSet::new();
    let mut edges = 0usize;
    while let Some(key) = pending.pop_front() {
        if !visited.insert(key.clone()) {
            continue;
        }
        let Some(image) = inventory.get(&key) else {
            reasons.push(InspectionIssue::structural(format!(
                "unresolved package image: {key}"
            )));
            continue;
        };
        let parsed = match parse(image.bytes) {
            Ok(parsed) => parsed,
            Err(error) => {
                reasons.push(InspectionIssue::structural(format!(
                    "{}: invalid/unsupported PE: {error}",
                    image.path
                )));
                continue;
            }
        };
        report.inspected.push(image.path.into());
        report.evidence.extend(
            parsed
                .evidence
                .iter()
                .map(|e| format!("{}: {e}", image.path)),
        );
        let review = policy.reviews.iter().find(|r| {
            canonical(r.path).as_ref() == Some(&key)
                && !r.contract.trim().is_empty()
                && r.bytes == image.bytes
        });
        match review {
            None => reasons.push(InspectionIssue::dynamic(format!("{}: dynamic behavior unproven; no exact-byte reviewed contract (string absence is not proof)", image.path))),
            Some(review) if !parsed.evidence.is_empty() && !review.dynamic_lookups_reviewed => reasons.push(InspectionIssue::dynamic(format!("{}: dynamic lookup evidence requires reviewed behavior contract", image.path))),
            Some(_) => (),
        }
        for import in parsed.imports {
            edges += 1;
            if edges > MAX_IMPORTS {
                reasons.push(InspectionIssue::structural(
                    "package dependency-edge budget exceeded".into(),
                ));
                report.verdict = GateVerdict::UnknownDynamic { reasons };
                return report;
            }
            if BANNED.contains(&basename(&import.module).as_str()) {
                report.verdict = GateVerdict::Banned {
                    image: image.path.into(),
                    module: import.module,
                    symbols: import.symbols,
                    kind: import.kind,
                };
                return report;
            }
            if import.module.contains(['/', '\\', ':']) || !import.module.is_ascii() {
                reasons.push(InspectionIssue::structural(format!(
                    "{}: opaque dependency path/search: {}",
                    image.path, import.module
                )));
                continue;
            }
            let module = basename(&import.module);
            let sibling = match key.rsplit_once('/') {
                Some((dir, _)) => format!("{dir}/{module}"),
                None => module.clone(),
            };
            if inventory.contains_key(&sibling) {
                pending.push_back(sibling);
            } else if !policy
                .system_modules
                .iter()
                .any(|approved| approved.eq_ignore_ascii_case(&module))
            {
                reasons.push(InspectionIssue {
                    kind: InspectionIssueKind::UnresolvedDependency,
                    message: format!(
                        "{}: unresolved dependency/search identity: {}",
                        image.path, import.module
                    ),
                });
            }
        }
    }
    if !reasons.is_empty() {
        report.verdict = GateVerdict::UnknownDynamic { reasons };
    }
    report
}
