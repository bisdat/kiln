// SPDX-License-Identifier: GPL-3.0-or-later
// kiln-eml's own path comparison.
// Shared by the export root (sf_startup/plan.rs), the install lock and the running-game check (runner.rs).
//! One file or folder by any of its names.
//!
//! Names alone are not enough on Windows. Wine (so Proton) gives back the spelling a path was asked for, not the
//! folder's own, from `fs::canonicalize` and from a process's image name: another case (`<game>\Mods` for `mods`), an
//! 8.3 short name (`<game>\SHRO~CSB` for `shroudforge`, `...\ENSHRO~1\mods` for the game folder) or another drive
//! letter for the same folder (`Z:\home\...` beside `S:\...` or `C:\...`) each give a name that differs from the
//! folder's own. Native Windows gives a folder's own spelling, but a share of a local folder (`\\localhost\C$\...`)
//! resolves to `\\?\UNC\...` all the same. So each test here holds by name OR by identity (Unix: device and inode;
//! Windows: volume serial number and file index, through every link and junction): either one is enough, so neither
//! can miss what the other finds.
use std::{fs, io, path::Path};

/// A file's or folder's identity: (device or volume serial number, inode or file index).
pub(crate) type Identity = (u64, u64);

/// Whether `path` is `base` or lies inside it: by name, component by component ([`names_within`]), or by identity
/// (`path` or one of its ancestors is the folder `base` is).
///
/// A missing `base` contains nothing, and an ancestor of `path` that does not exist is passed over (a protected folder
/// not made yet); any other error is returned, and the caller refuses.
pub(crate) fn within(path: &Path, base: &Path) -> io::Result<bool> {
    if names_within(path, base) {
        return Ok(true);
    }
    let Some(base) = identity(base)? else {
        return Ok(false);
    };
    for ancestor in path.ancestors() {
        if identity(ancestor)? == Some(base) {
            return Ok(true);
        }
    }
    Ok(false)
}

/// Whether `a` and `b` name one file or folder, as known: by the exact name, or by the identities the caller read
/// (`None`: none known, the exact name alone decides). Never by the folded name ([`names_equal`]): the fold may join
/// more than the system does (the Kelvin sign is `K` to it), and a "same" here is an acceptance (the install lock counts
/// as held), so it joins only what is surely one folder. Every other name of a folder has its identity, unless its
/// volume has no serial number: Wine gives 0 for every mount it knows no serial for (btrfs subvolumes, tmpfs), so two
/// folders on two such mounts can have one identity, and a 0 serial counts as unknown here.
pub(crate) fn same(a: &Path, a_id: Option<Identity>, b: &Path, b_id: Option<Identity>) -> bool {
    a == b || (a_id.is_some_and(|(volume, _)| volume != 0) && a_id == b_id)
}

/// The file or folder at `path` (links followed), or `None` when there is none or its filesystem gives no file number
/// (some network shares give 0 for every file; names alone decide there).
pub(crate) fn identity(path: &Path) -> io::Result<Option<Identity>> {
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
    let meta = fs::metadata(path)?;
    Ok((meta.dev(), meta.ino()))
}
#[cfg(windows)]
fn file_identity(path: &Path) -> io::Result<Identity> {
    use std::os::windows::{fs::OpenOptionsExt as _, io::AsRawHandle as _};
    use windows::Win32::{
        Foundation::HANDLE,
        Storage::FileSystem::{BY_HANDLE_FILE_INFORMATION, GetFileInformationByHandle},
    };
    // FILE_READ_ATTRIBUTES, and FILE_FLAG_BACKUP_SEMANTICS so a folder opens; a reparse point is followed. Shared for
    // reading, writing and deleting, so a running executable opens too.
    let file = fs::OpenOptions::new()
        .access_mode(0x80)
        .share_mode(0x1 | 0x2 | 0x4)
        .custom_flags(0x0200_0000)
        .open(path)?;
    let mut info = BY_HANDLE_FILE_INFORMATION::default();
    unsafe { GetFileInformationByHandle(HANDLE(file.as_raw_handle()), &mut info) }.map_err(io::Error::other)?;
    Ok((
        info.dwVolumeSerialNumber as u64,
        ((info.nFileIndexHigh as u64) << 32) | info.nFileIndexLow as u64,
    ))
}
#[cfg(not(any(unix, windows)))]
fn file_identity(_: &Path) -> io::Result<Identity> {
    Err(io::ErrorKind::NotFound.into())
}

/// [`within`] by name: on Windows each component compared with [`fold`], elsewhere exactly (`Path::starts_with`).
#[cfg(windows)]
pub(crate) fn names_within(path: &Path, base: &Path) -> bool {
    let (path, base) = (key(path), key(base));
    path.len() >= base.len() && path[..base.len()] == base[..]
}
#[cfg(not(windows))]
pub(crate) fn names_within(path: &Path, base: &Path) -> bool {
    path.starts_with(base)
}

/// One name for the same path: on Windows each component compared with [`fold`], elsewhere exactly. Coarser than the
/// system on Windows, so only for a refusal (the running-game check), never for an acceptance (see [`same`]).
#[cfg(windows)]
pub(crate) fn names_equal(a: &Path, b: &Path) -> bool {
    key(a) == key(b)
}
#[cfg(not(windows))]
pub(crate) fn names_equal(a: &Path, b: &Path) -> bool {
    a == b
}

#[cfg(windows)]
fn key(p: &Path) -> Vec<String> {
    p.components().map(|c| c.as_os_str().to_string_lossy().chars().map(fold).collect()).collect()
}

/// A name's character as NTFS and Wine compare names, or coarser (it may only make more names equal): the upper case of
/// its lower case, each where it is one character. NTFS and Wine upper-case each UTF-16 unit by a table; upper-casing
/// alone misses the Greek letters with a subscript iota, whose full upper case is two letters (`ᾳ` U+1FB3 is `ᾼ`
/// U+1FBC to both; `ᾀ` U+1F80 is `ᾈ` U+1F88). Lower-casing first joins them. The test
/// `the_name_fold_joins_what_the_system_upcase_table_joins` checks this against the running system's table. Coarser
/// than the table for a few letters (the Kelvin sign is `K` here, its own letter to NTFS).
pub(crate) fn fold(c: char) -> char {
    let mut lower = c.to_lowercase();
    let lower = if lower.len() == 1 { lower.next().unwrap_or(c) } else { c };
    let mut upper = lower.to_uppercase();
    if upper.len() == 1 { upper.next().unwrap_or(lower) } else { lower }
}

/// Other names of one path for the Windows tests: its 8.3 name, and the same folder by another drive letter or share.
#[cfg(all(test, windows))]
pub(crate) mod windows_names {
    use std::{
        ffi::{CStr, OsString, c_char, c_void},
        fs,
        os::windows::ffi::{OsStrExt as _, OsStringExt as _},
        path::{Path, PathBuf},
    };
    #[link(name = "kernel32", kind = "raw-dylib")]
    unsafe extern "system" {
        fn GetShortPathNameW(long: *const u16, short: *mut u16, len: u32) -> u32;
        fn GetModuleHandleW(name: *const u16) -> *mut c_void;
        fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
    }
    fn wide(s: &std::ffi::OsStr) -> Vec<u16> {
        s.encode_wide().chain([0]).collect()
    }
    /// The 8.3 name of `path`, if the volume made one that differs.
    pub(crate) fn short(path: &Path) -> Option<PathBuf> {
        let long = wide(path.as_os_str());
        let mut buf = vec![0u16; 4096];
        let len = unsafe { GetShortPathNameW(long.as_ptr(), buf.as_mut_ptr(), buf.len() as u32) } as usize;
        let short = (len > 0 && len < buf.len()).then(|| PathBuf::from(OsString::from_wide(&buf[..len])))?;
        (short != path).then_some(short)
    }
    /// `path` by another way to its volume: under Wine Z:\<its Unix path>, on Windows \\localhost\<drive>$\... (when
    /// that share answers).
    pub(crate) fn other_path(path: &Path) -> Option<PathBuf> {
        let full = fs::canonicalize(path).ok()?;
        let full = full.to_str()?.strip_prefix(r"\\?\")?.to_owned();
        let kernel32 = unsafe { GetModuleHandleW(wide("kernel32".as_ref()).as_ptr()) };
        let unix_name = unsafe { GetProcAddress(kernel32, c"wine_get_unix_file_name".as_ptr().cast()) };
        let other = if unix_name.is_null() {
            let (drive, rest) = full.split_once(r":\")?;
            PathBuf::from(format!(r"\\localhost\{drive}$\{rest}"))
        } else {
            // SAFETY: Wine's kernel32 export: char *CDECL wine_get_unix_file_name(LPCWSTR)
            let unix_name: unsafe extern "C" fn(*const u16) -> *const c_char = unsafe { std::mem::transmute(unix_name) };
            let unix = unsafe { unix_name(wide(full.as_ref()).as_ptr()) };
            if unix.is_null() {
                return None;
            }
            let unix = unsafe { CStr::from_ptr(unix) }.to_str().ok()?.replace('/', r"\");
            PathBuf::from(format!("Z:{unix}"))
        };
        (other.as_os_str() != full.as_str() && fs::metadata(&other).is_ok()).then_some(other)
    }
}

/// `same` is an acceptance (a held install lock counts for the other name), so it joins only what is surely one folder.
#[cfg(test)]
mod same_tests {
    use super::same;
    use std::path::Path;

    #[test]
    fn only_the_exact_name_or_a_known_identity_is_the_same_folder() {
        let (a, b) = (Path::new("/games/K"), Path::new("/games/\u{212A}"));
        assert!(same(a, None, a, None), "the exact name");
        assert!(!same(a, None, b, None), "the fold is not enough (the Kelvin sign is its own letter to NTFS)");
        assert!(same(a, Some((7, 5)), b, Some((7, 5))), "one volume, one file number");
        assert!(!same(a, Some((7, 5)), b, Some((8, 5))), "two volumes");
        assert!(!same(a, None, b, Some((7, 5))) && !same(a, Some((7, 5)), b, None), "one identity unknown");
        // Wine gives volume serial 0 for every mount it has no serial for (btrfs subvolumes, tmpfs: `Z:\home` and
        // `Z:\var\log` are both (0, 0x100) on a btrfs system), so a 0 serial does not tell two folders apart
        assert!(!same(a, Some((0, 0x100)), b, Some((0, 0x100))), "a volume without a serial");
    }
}

#[cfg(all(test, windows))]
mod tests {
    #[link(name = "ntdll", kind = "raw-dylib")]
    unsafe extern "system" {
        fn RtlUpcaseUnicodeChar(c: u16) -> u16;
    }

    /// Two names NTFS (or Wine) treats as one must have one key: for every UTF-16 unit, its upper case by the system's
    /// own table folds to what the unit folds to. (Units above U+FFFF are surrogate pairs, compared exactly by both.)
    #[test]
    fn the_name_fold_joins_what_the_system_upcase_table_joins() {
        let mut apart = Vec::new();
        for unit in 0..=0xFFFFu16 {
            let upper = unsafe { RtlUpcaseUnicodeChar(unit) };
            let (Some(c), Some(u)) = (char::from_u32(unit as u32), char::from_u32(upper as u32)) else {
                continue;
            };
            if super::fold(c) != super::fold(u) {
                apart.push(format!("{unit:04X}->{upper:04X}"));
            }
        }
        assert!(apart.is_empty(), "one name to the system, two keys here: {apart:?}");
        // and it is no wider than it must be for the protected names
        assert!(!super::names_within(std::path::Path::new(r"C:\game\ｍｏｄｓ"), std::path::Path::new(r"C:\game\mods")));
    }
}
