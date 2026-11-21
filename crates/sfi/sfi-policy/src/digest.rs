// SPDX-License-Identifier: MIT
//! A package folder's content digest : the `content_sha256` a launch plan
//! pins for each package, computed by the plan's writer (kiln-manage) and computed again by
//! kiln-sfi right before a package's Lua is registered. Both sides call this one function.
//!
//! **What is hashed.** Every regular file below the package folder, at any depth: `mod.json`,
//! `extended.mod.json`, Lua modules, assets, INI files and DLLs alike. Directories count only
//! through the files in them. Each file contributes its path relative to the folder (UTF-8,
//! components joined with `/`, as listed by the file system), its size and its SHA-256. The
//! entries are sorted by path bytes and framed with lengths, so no two different trees give the
//! same input:
//!
//! ```text
//! SHA-256( "kiln.sfi.package-content.v1\0" || u64le(file count)
//!          || for each file: u64le(path length) || path || u64le(size) || SHA-256(bytes) )
//! ```
//!
//! **What is refused** (an `Err`, so the package does not run): a folder that is not a plain
//! directory; any link, junction or other reparse point inside it; anything that is neither a
//! plain file nor a plain directory; a name that is not UTF-8; more than [`MAX_FILES`] files or
//! a depth over [`MAX_DEPTH`]; a file that cannot be read, or that turns out to be another
//! object than the one listed when it is opened (it is opened without following a link and
//! checked through its handle).
//!
//! **What is left out:** a plain file named `desktop.ini` or `Thumbs.db` (any letter case, at any
//! depth): Windows Explorer writes these into folders it shows (a folder's view settings, a picture
//! cache), so a player who only looked at a mod folder would otherwise withdraw that package until the
//! next activation. Neither ShroudForge nor Kiln reads them. Both sides of the plan call this one
//! function, so they agree on the exclusion ([`explorer_file`]). A link or a folder of that name is
//! not left out (a link is refused as anywhere else).
//!
//! The digest says nothing about what a file means; a size or a time stamp alone is never
//! taken as proof.
use sha2::{Digest, Sha256};
use std::fs::{self, File, Metadata};
use std::io::Read;
use std::path::{Path, PathBuf};

/// The most files one package folder may hold.
pub const MAX_FILES: usize = 65_536;
/// The deepest folder nesting below the package folder.
pub const MAX_DEPTH: usize = 64;
const DOMAIN: &[u8] = b"kiln.sfi.package-content.v1\0";

/// A file Windows Explorer writes into a folder it shows, by name (ASCII letter case ignored, as on
/// Windows): `desktop.ini` and `Thumbs.db`. The digest leaves such plain files out.
pub fn explorer_file(name: &str) -> bool {
    name.eq_ignore_ascii_case("desktop.ini") || name.eq_ignore_ascii_case("thumbs.db")
}

/// SHA-256 of `bytes`.
pub fn sha256(bytes: &[u8]) -> [u8; 32] {
    Sha256::digest(bytes).into()
}

/// The content digest of the package folder `root` (see the module docs).
pub fn package_content_sha256(root: &Path) -> Result<[u8; 32], String> {
    let meta = fs::symlink_metadata(root).map_err(|e| format!("{}: {e}", root.display()))?;
    if !meta.is_dir() || reparse(&meta) {
        return Err(format!("{} is not a plain folder", root.display()));
    }
    let mut files: Vec<(String, PathBuf, Metadata)> = Vec::new();
    let mut folders = vec![(root.to_path_buf(), String::new(), 0usize)];
    while let Some((dir, prefix, depth)) = folders.pop() {
        let entries = fs::read_dir(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
        for entry in entries {
            let entry = entry.map_err(|e| format!("{}: {e}", dir.display()))?;
            let name = entry
                .file_name()
                .into_string()
                .map_err(|_| format!("{}: a name that is not UTF-8", dir.display()))?;
            let relative = if prefix.is_empty() {
                name
            } else {
                format!("{prefix}/{name}")
            };
            let path = entry.path();
            // symlink_metadata: a link is seen as a link, never as its target
            let meta = fs::symlink_metadata(&path).map_err(|e| format!("{relative}: {e}"))?;
            if reparse(&meta) {
                return Err(format!("{relative} is a link or another reparse point"));
            }
            if meta.is_dir() {
                if depth + 1 > MAX_DEPTH {
                    return Err(format!(
                        "{relative}: folders nested deeper than {MAX_DEPTH}"
                    ));
                }
                folders.push((path, relative, depth + 1));
            } else if meta.is_file() {
                if explorer_file(&name_of(&relative)) {
                    continue;
                }
                if files.len() == MAX_FILES {
                    return Err(format!("more than {MAX_FILES} files"));
                }
                files.push((relative, path, meta));
            } else {
                return Err(format!("{relative} is neither a file nor a folder"));
            }
        }
    }
    files.sort_by(|a, b| a.0.as_bytes().cmp(b.0.as_bytes()));
    let mut total = Sha256::new();
    total.update(DOMAIN);
    total.update((files.len() as u64).to_le_bytes());
    for (relative, path, listed) in &files {
        let (size, digest) = hash_file(path, listed).map_err(|e| format!("{relative}: {e}"))?;
        total.update((relative.len() as u64).to_le_bytes());
        total.update(relative.as_bytes());
        total.update(size.to_le_bytes());
        total.update(digest);
    }
    Ok(total.finalize().into())
}

/// The last component of a `/`-joined relative path.
fn name_of(relative: &str) -> String {
    relative.rsplit('/').next().unwrap_or(relative).to_owned()
}

/// `path` is a plain folder: not a link, a junction or any other reparse point.
pub fn plain_folder(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok_and(|m| m.is_dir() && !reparse(&m))
}

/// Opens the plain file at `path` for reading, as the core opens a launch plan: never a link, a
/// junction or another reparse point (the last component is checked, then opened without
/// following a link), and the handle is checked to be that same plain file, so nothing can be
/// swapped in between the check and the read. On Windows the file is shared for reading only
/// while it is open (FILE_SHARE_READ): nobody writes or deletes it under the reader.
pub fn open_plain_file(path: &Path) -> Result<File, String> {
    let listed = fs::symlink_metadata(path).map_err(|e| e.to_string())?;
    if !listed.is_file() || reparse(&listed) {
        return Err("it is not a plain file".into());
    }
    open_listed(path, &listed)
}

fn open_listed(path: &Path, listed: &Metadata) -> Result<File, String> {
    let file = open_no_follow(path).map_err(|e| e.to_string())?;
    let held = file.metadata().map_err(|e| e.to_string())?;
    if !held.is_file() || reparse(&held) || !same_file(listed, &held) {
        return Err("it changed while it was opened".into());
    }
    Ok(file)
}

/// Size and SHA-256 of the file listed as `listed`, read through one handle that is checked to
/// be that same plain file.
fn hash_file(path: &Path, listed: &Metadata) -> Result<(u64, [u8; 32]), String> {
    let mut reader = open_listed(path, listed)?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0u8; 64 * 1024];
    let mut size = 0u64;
    loop {
        let n = reader.read(&mut buffer).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        size += n as u64;
        hasher.update(&buffer[..n]);
    }
    Ok((size, hasher.finalize().into()))
}

/// A link, a junction or any other reparse point (a cloud placeholder, for one).
#[cfg(windows)]
fn reparse(meta: &Metadata) -> bool {
    use std::os::windows::fs::MetadataExt as _;
    meta.file_type().is_symlink() || meta.file_attributes() & 0x400 != 0 // FILE_ATTRIBUTE_REPARSE_POINT
}
#[cfg(not(windows))]
fn reparse(meta: &Metadata) -> bool {
    meta.file_type().is_symlink()
}

/// Opens `path` for reading without following a link at its last component and, on Windows,
/// without letting anyone write or delete it while it is read (FILE_FLAG_OPEN_REPARSE_POINT,
/// FILE_SHARE_READ only).
#[cfg(windows)]
fn open_no_follow(path: &Path) -> std::io::Result<File> {
    use std::os::windows::fs::OpenOptionsExt as _;
    fs::OpenOptions::new()
        .read(true)
        .share_mode(0x1) // FILE_SHARE_READ
        .custom_flags(0x0020_0000) // FILE_FLAG_OPEN_REPARSE_POINT
        .open(path)
}
#[cfg(not(windows))]
fn open_no_follow(path: &Path) -> std::io::Result<File> {
    File::open(path)
}

/// The opened file is the one listed. Windows opens without following a link and the handle is
/// checked on its own; elsewhere the device and inode must be the listed entry's.
#[cfg(unix)]
fn same_file(listed: &Metadata, held: &Metadata) -> bool {
    use std::os::unix::fs::MetadataExt as _;
    (listed.dev(), listed.ino()) == (held.dev(), held.ino())
}
#[cfg(not(unix))]
fn same_file(_: &Metadata, _: &Metadata) -> bool {
    true
}
