// SPDX-License-Identifier: MIT
//! One folder by any of its names, as Windows and Wine name folders.
//!
//! A launch plan names package folders by strings, and a string comparison misses two names of one folder: Windows
//! compares names by upper-casing each UTF-16 unit with a table (so `ΑΣ` and `ας` are one name, and so are `ᾳ` and
//! `ᾼ`), and an 8.3 short name (`LONGPA~1`), a link or a junction is another name of the same folder. [`name_key`]
//! joins what the table joins (or more, never less); [`identity`] tells one folder from another whatever it is called.
use std::{io, path::Path};

/// A file's or folder's identity: (device or volume serial number, inode or file index).
pub type Identity = (u64, u64);

/// `name` with every character [`fold`]ed: two names Windows or Wine treats as one have one key.
pub fn name_key(name: &str) -> String {
    name.chars().map(fold).collect()
}

/// A name's character as NTFS and Wine compare names, or coarser (it may only make more names equal): the upper case of
/// its lower case, each where it is one character. Character by character, never by word: `str::to_lowercase` writes a
/// word-final `Σ` as `ς` and any other as `σ`, so `ΑΣ` and `Ασ` (one name to Windows) got two keys. Upper-casing
/// alone misses the Greek letters with a subscript iota, whose full upper case is two letters (`ᾳ` U+1FB3 is `ᾼ`
/// U+1FBC to both). kiln-eml's export root folds names the same way and checks it against the system's own table.
pub fn fold(c: char) -> char {
    let mut lower = c.to_lowercase();
    let lower = if lower.len() == 1 {
        lower.next().unwrap_or(c)
    } else {
        c
    };
    let mut upper = lower.to_uppercase();
    if upper.len() == 1 {
        upper.next().unwrap_or(lower)
    } else {
        lower
    }
}

/// The file or folder at `path` (links followed), or `None` when there is none or its filesystem gives no file number
/// (some network shares give 0 for every file; names alone decide there). Any other error is returned.
pub fn identity(path: &Path) -> io::Result<Option<Identity>> {
    match file_identity(path) {
        Ok((_, 0)) => Ok(None),
        Ok(id) => Ok(Some(id)),
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(e),
    }
}

#[cfg(unix)]
fn file_identity(path: &Path) -> io::Result<Identity> {
    use std::os::unix::fs::MetadataExt as _;
    let meta = std::fs::metadata(path)?;
    Ok((meta.dev(), meta.ino()))
}

#[cfg(windows)]
fn file_identity(path: &Path) -> io::Result<Identity> {
    use std::os::windows::{fs::OpenOptionsExt as _, io::AsRawHandle as _};
    #[repr(C)]
    #[derive(Default)]
    struct FileTime {
        low: u32,
        high: u32,
    }
    /// BY_HANDLE_FILE_INFORMATION
    #[repr(C)]
    #[derive(Default)]
    struct Info {
        attributes: u32,
        created: FileTime,
        accessed: FileTime,
        written: FileTime,
        volume_serial: u32,
        size_high: u32,
        size_low: u32,
        links: u32,
        index_high: u32,
        index_low: u32,
    }
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetFileInformationByHandle(file: *mut core::ffi::c_void, info: *mut Info) -> i32;
    }
    // FILE_READ_ATTRIBUTES, and FILE_FLAG_BACKUP_SEMANTICS so a folder opens; a reparse point is followed.
    let file = std::fs::OpenOptions::new()
        .access_mode(0x80)
        .share_mode(0x1 | 0x2 | 0x4)
        .custom_flags(0x0200_0000)
        .open(path)?;
    let mut info = Info::default();
    // SAFETY: a handle this function holds open, and an out record of the documented layout.
    if unsafe { GetFileInformationByHandle(file.as_raw_handle(), &mut info) } == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok((
        u64::from(info.volume_serial),
        (u64::from(info.index_high) << 32) | u64::from(info.index_low),
    ))
}

#[cfg(not(any(unix, windows)))]
fn file_identity(_: &Path) -> io::Result<Identity> {
    Err(io::ErrorKind::NotFound.into())
}
