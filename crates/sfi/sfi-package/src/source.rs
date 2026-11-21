// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Source (both pins): modules/modloader/mod_package_filesystem.rs:69-87,121-225,
// 235-326,379-413,504-517. The pinned filesystem modules are byte-identical.
//! Pure package sources and the immutable package descriptor (moved from sfi-core's
//! `host`; `sfi_core::host` re-exports every item here unchanged).
//!
//! SF provenance (both pins): modules/modloader/mod_package_filesystem.rs:99-135,
//! 300-308,392-400,504-517. Inclusion in a registry is not activation.
//!
//! Sources stay worker-local: the Rc-backed overlay and descriptor are not Send.
//! ```compile_fail
//! fn send<T: Send>() {}
//! send::<sfi_package::source::ProfileOverlaySource>();
//! ```
//! ```compile_fail
//! fn send<T: Send>() {}
//! send::<sfi_package::source::Package>();
//! ```

use std::{
    collections::{HashMap, HashSet},
    fs, io,
    path::{Path, PathBuf},
    rc::Rc,
};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SourceVersion {
    Sf179,
    Sf180,
}

/// A source node's kind. Directory entries preserve symlinks without resolving them;
/// metadata follows contained links and reports their target kind instead.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SourceKind {
    File,
    Directory,
    Symlink,
    /// Neither file nor directory (e.g. a disk FIFO).
    Other,
}

/// Structural metadata, not an exclusive classification for the three predicates.
/// An exact profile override is a File here, but SF also considers it a directory.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SourceMetadata {
    pub kind: SourceKind,
}

/// One immediate child, never an absolute path or a recursive descendant.
/// Original link kinds allow traversal without following links (SF files():247-250).
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SourceEntry {
    pub name: String,
    /// Source-root-relative path with `/` separators. The listing parent is resolved,
    /// but this child's symlink name is preserved (SF read_directory():261-289).
    pub path: String,
    pub kind: SourceKind,
}

/// Worker-local read-only package source, including host-supplied archives/overlays.
/// Paths are source-relative strings; `""` and `"."` both denote the root directory.
/// Implementors enforce containment and preserve original I/O failures, including
/// NotFound, NotADirectory and PermissionDenied. Operations observe current source
/// contents, not a snapshot; successful require results remain cached in the VM.
/// Listing/metadata default to Unsupported for read-file-only hosts. Predicates
/// derive from metadata and suppress errors to false, as SF does; these defaults
/// do not claim any successful listing/metadata support.
pub trait PackageSource {
    fn read_file(&self, path: &str) -> io::Result<Vec<u8>>;

    fn metadata(&self, _path: &str) -> io::Result<SourceMetadata> {
        Err(io::ErrorKind::Unsupported.into())
    }

    /// SF exists():137-148,310-314 (both pins): all path/access errors become false.
    /// Metadata-only sources retain their existing structural classification; sources
    /// with virtual nodes can override these predicates independently.
    fn exists(&self, path: &str) -> bool {
        self.metadata(path).is_ok()
    }

    /// SF is_file():161-165,316-320 (both pins): all path/access errors become false.
    fn is_file(&self, path: &str) -> bool {
        self.metadata(path)
            .is_ok_and(|metadata| metadata.kind == SourceKind::File)
    }

    /// SF is_directory():179-189,322-326 (both pins): errors become false, and this
    /// is NOT the negation of is_file (an exact profile file satisfies both).
    fn is_directory(&self, path: &str) -> bool {
        self.metadata(path)
            .is_ok_and(|metadata| metadata.kind == SourceKind::Directory)
    }

    /// Immediate entries in original source order (no sorting), with per-entry errors
    /// retained alongside successful siblings, as SF's read_directory does (:261-298).
    /// The outer error means the directory could not be opened; an empty directory is
    /// a successful empty vector. A caller applies its own operation's sorting contract.
    fn read_dir(&self, _path: &str) -> io::Result<Vec<io::Result<SourceEntry>>> {
        Err(io::ErrorKind::Unsupported.into())
    }
}

/// Selected profile's virtual files over a worker-local package source.
/// The caller selects/validates the profile; construction only snapshots its UTF-8
/// contents as bytes (SF apply_profile_files():219-225, both pins). No disk writes.
/// SF applies this layer to disk packages only; the host can also wrap injected sources.
/// Paths use component-wise Path equality/prefix rules, not canonical disk paths.
/// In particular, `""` and `"."` open the same disk root but need not list the same
/// virtual names (SF read_directory():75-87 compares the original requested parent).
#[derive(Clone)]
pub struct ProfileOverlaySource {
    source: Rc<dyn PackageSource>,
    files: HashMap<PathBuf, Vec<u8>>,
}
impl ProfileOverlaySource {
    pub fn new(source: Rc<dyn PackageSource>, files: &HashMap<String, String>) -> Self {
        Self {
            source,
            files: files
                .iter()
                .map(|(path, contents)| (PathBuf::from(path), contents.as_bytes().to_vec()))
                .collect(),
        }
    }

    fn has_prefix(&self, path: &str) -> bool {
        self.files.keys().any(|item| item.starts_with(path))
    }
}
impl PackageSource for ProfileOverlaySource {
    // SF open_package_file():125-130: exact overrides bypass disk canonicalization.
    fn read_file(&self, path: &str) -> io::Result<Vec<u8>> {
        match self.files.get(Path::new(path)) {
            Some(bytes) => Ok(bytes.clone()),
            None => self.source.read_file(path),
        }
    }

    /// SF has no metadata operation; this structural projection prefers exact files,
    /// then virtual ancestor directories, then the wrapped metadata/error. It cannot
    /// encode SF's overlapping predicates; consumers must use those independently.
    fn metadata(&self, path: &str) -> io::Result<SourceMetadata> {
        if self.files.contains_key(Path::new(path)) {
            Ok(SourceMetadata {
                kind: SourceKind::File,
            })
        } else if self.has_prefix(path) {
            Ok(SourceMetadata {
                kind: SourceKind::Directory,
            })
        } else {
            self.source.metadata(path)
        }
    }

    // SF read_directory():69-87: append ONLY after the underlying listing succeeds.
    // Keep its entries/errors/kinds/order; dedup full paths, not display names. Do not
    // synthesize directories for deeper overrides, or resolve the requested parent.
    fn read_dir(&self, path: &str) -> io::Result<Vec<io::Result<SourceEntry>>> {
        let mut entries = self.source.read_dir(path)?;
        let mut names: HashSet<PathBuf> = entries
            .iter()
            .filter_map(|entry| entry.as_ref().ok().map(|entry| PathBuf::from(&entry.path)))
            .collect();
        for override_path in self.files.keys() {
            if override_path.parent() == Some(Path::new(path))
                && names.insert(override_path.clone())
            {
                entries.push(Ok(SourceEntry {
                    // Profile paths are UTF-8; a path with a parent has a final component.
                    name: override_path
                        .file_name()
                        .unwrap_or_default()
                        .to_string_lossy()
                        .into_owned(),
                    path: override_path
                        .to_string_lossy()
                        .replace(std::path::MAIN_SEPARATOR, "/"),
                    kind: SourceKind::File,
                }));
            }
        }
        Ok(entries)
    }

    // SF exists():137-148: the disk predicate suppresses errors before the overlay ORs.
    fn exists(&self, path: &str) -> bool {
        self.source.exists(path)
            || self.files.contains_key(Path::new(path))
            || self.has_prefix(path)
    }

    // SF is_file():161-165: an exact override wins even when the disk target is a directory.
    fn is_file(&self, path: &str) -> bool {
        self.files.contains_key(Path::new(path)) || self.source.is_file(path)
    }

    // SF is_directory():179-189: Path::starts_with includes equality, so even an
    // exact virtual FILE is a directory. Disk directories remain visible as well.
    fn is_directory(&self, path: &str) -> bool {
        self.has_prefix(path) || self.source.is_directory(path)
    }
}

/// Canonical disk package source; matching SF's disk source, including in-root symlinks.
/// SF: modules/modloader/mod_package_filesystem.rs:235-240,300-308,504-517 (both pins).
#[derive(Debug)]
pub struct DiskSource {
    root: PathBuf,
}
impl DiskSource {
    pub fn new(root: impl AsRef<Path>) -> io::Result<Self> {
        Ok(Self {
            root: root.as_ref().canonicalize()?,
        })
    }
    pub fn root(&self) -> &Path {
        &self.root
    }
    fn contained_path(&self, path: &Path) -> io::Result<PathBuf> {
        let path = self.root.join(path).canonicalize()?;
        // Under Wine (so Proton) canonicalize keeps a Unix link in the path, so where the path
        // really is comes from Wine: a link out of the root is refused as on every other system.
        if !path.starts_with(&self.root)
            || path.is_symlink()
            || !crate::same_folder::wine_within(&path, &self.root)?
        {
            return Err(io::ErrorKind::PermissionDenied.into());
        }
        Ok(path)
    }
}

fn source_kind(kind: fs::FileType) -> SourceKind {
    if kind.is_file() {
        SourceKind::File
    } else if kind.is_dir() {
        SourceKind::Directory
    } else if kind.is_symlink() {
        SourceKind::Symlink
    } else {
        SourceKind::Other
    }
}
impl PackageSource for DiskSource {
    fn read_file(&self, path: &str) -> io::Result<Vec<u8>> {
        let path = self.contained_path(Path::new(path))?;
        if !path.is_file() {
            return Err(io::ErrorKind::NotFound.into());
        }
        fs::read(path)
    }

    fn metadata(&self, path: &str) -> io::Result<SourceMetadata> {
        let path = self.contained_path(Path::new(path))?;
        Ok(SourceMetadata {
            kind: source_kind(fs::metadata(path)?.file_type()),
        })
    }

    fn read_dir(&self, path: &str) -> io::Result<Vec<io::Result<SourceEntry>>> {
        let path = self.contained_path(Path::new(path))?;
        if !path.is_dir() {
            return Err(io::ErrorKind::NotADirectory.into());
        }
        Ok(fs::read_dir(path)?
            .map(|entry| {
                let entry = entry?;
                let name = entry
                    .file_name()
                    .into_string()
                    .map_err(|_| io::Error::other("package entry name is not valid UTF-8"))?;
                // SF keeps the resolved parent's root-relative path without resolving
                // child link targets (:270-289). Dangling and escaping links stay visible;
                // target operations still contain-check.
                let path = entry.path();
                let path = path.strip_prefix(&self.root).map_err(io::Error::other)?;
                let path = path
                    .to_str()
                    .ok_or_else(|| io::Error::other("package entry path is not valid UTF-8"))?
                    .replace(std::path::MAIN_SEPARATOR, "/");
                let kind = source_kind(entry.file_type()?);
                Ok(SourceEntry { name, path, kind })
            })
            .collect())
    }
}

/// Immutable identity and roots captured by bound functions, never a later caller's identity.
#[derive(Clone)]
pub struct Package {
    id: String,
    pub source: Rc<dyn PackageSource>,
    pub export_root: Option<PathBuf>,
}
impl Package {
    pub fn new(id: impl Into<String>, source: Rc<dyn PackageSource>) -> Self {
        Self {
            id: id.into(),
            source,
            export_root: None,
        }
    }
    pub fn id(&self) -> &str {
        &self.id
    }
}
