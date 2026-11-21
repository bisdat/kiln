// SPDX-License-Identifier: MIT
//! One file or folder by any of its names, as Windows and Wine name them.
//!
//! A string comparison misses names of one folder: Windows compares names by upper-casing
//! each UTF-16 unit with a table (`ΑΣ` and `ας` are one name, so are `ᾳ` and `ᾼ`), and an
//! 8.3 short name, another drive letter under Wine (`Z:\home\...` beside `C:\...`), a local
//! share (`\\localhost\C$\...`), a link or a junction is another name of the same folder.
//! [`name_key`] joins what the table joins (or more, never less); [`identity`] tells one
//! folder from another whatever it is called. sfi-policy's `same_folder` folds names the same
//! way (kiln-sfi checks the two agree).
//!
//! Under Wine (so Proton) a Unix symbolic link is followed by every open and kept in the path
//! by `canonicalize`, so a `canonicalize` + `starts_with` containment check never sees it;
//! [`wine_within`] asks Wine where a path really is.
use std::{io, path::Path};

/// A file's or folder's identity: (device or volume serial number, inode or file index).
pub type Identity = (u64, u64);

/// `name` with every character [`fold`]ed: two names Windows or Wine treats as one have one key.
pub fn name_key(name: &str) -> String {
    name.chars().map(fold).collect()
}

/// A name's character as NTFS and Wine compare names, or coarser (it may only make more names
/// equal): the upper case of its lower case, each where it is one character. Character by
/// character, never by word (`str::to_lowercase` writes a word-final `Σ` as `ς`).
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

/// The file or folder at `path` (links followed), or `None` when there is none (also when a
/// folder on the way is a file) or its filesystem gives no file number (some network shares
/// give 0 for every file; names alone decide there). Any other error is returned.
pub fn identity(path: &Path) -> io::Result<Option<Identity>> {
    match file_identity(path) {
        Ok((_, 0)) => Ok(None),
        Ok(id) => Ok(Some(id)),
        Err(e)
            if matches!(
                e.kind(),
                io::ErrorKind::NotFound | io::ErrorKind::NotADirectory
            ) =>
        {
            Ok(None)
        }
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
    // FILE_READ_ATTRIBUTES, and FILE_FLAG_BACKUP_SEMANTICS so a folder opens; a reparse point
    // is followed.
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

/// Whether `path` lies at or under `root` where Wine really has them: Wine's own Unix name of
/// each (`wine_get_unix_file_name`, which resolves every link) compared component by component.
/// Always true outside Wine, where `canonicalize` already resolved every link. A path Wine
/// gives no Unix name is an error (the caller refuses).
pub fn wine_within(path: &Path, root: &Path) -> io::Result<bool> {
    #[cfg(windows)]
    if let Some(export) = wine::export() {
        let (path, root) = (
            wine::unix_name(export, path)?,
            wine::unix_name(export, root)?,
        );
        let (path, root) = (wine::trim(&path), wine::trim(&root));
        return Ok(path == root
            || (path.starts_with(root) && (root == b"/" || path.get(root.len()) == Some(&b'/'))));
    }
    let _ = (path, root);
    Ok(true)
}

/// Whether this process runs under Wine (so Proton): there `canonicalize` keeps a Unix link in
/// the path, and only [`wine_within`] says where a path really is.
pub fn under_wine() -> bool {
    #[cfg(windows)]
    return wine::export().is_some();
    #[cfg(not(windows))]
    false
}

#[cfg(windows)]
mod wine {
    use std::{ffi::c_void, io, os::windows::ffi::OsStrExt, path::Path, sync::OnceLock};
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetModuleHandleW(name: *const u16) -> *mut c_void;
        fn GetProcAddress(module: *mut c_void, name: *const std::ffi::c_char) -> *mut c_void;
        fn GetProcessHeap() -> *mut c_void;
        fn HeapFree(heap: *mut c_void, flags: u32, memory: *mut c_void) -> i32;
    }
    pub type UnixName = unsafe extern "C" fn(*const u16) -> *mut std::ffi::c_char;
    /// Wine's `wine_get_unix_file_name` (so: this is Wine, or Proton); `None` on native Windows.
    pub fn export() -> Option<UnixName> {
        static EXPORT: OnceLock<usize> = OnceLock::new();
        let address = *EXPORT.get_or_init(|| {
            let kernel32: Vec<u16> = "kernel32.dll".encode_utf16().chain([0]).collect();
            // SAFETY: NUL-terminated names; kernel32 is loaded in every Win32 process.
            unsafe {
                let module = GetModuleHandleW(kernel32.as_ptr());
                if module.is_null() {
                    0
                } else {
                    GetProcAddress(module, c"wine_get_unix_file_name".as_ptr()) as usize
                }
            }
        });
        // SAFETY: Wine's export, char *CDECL wine_get_unix_file_name(LPCWSTR).
        (address != 0).then(|| unsafe { std::mem::transmute::<usize, UnixName>(address) })
    }
    /// The Unix name Wine gives `path`, every link in it resolved.
    pub fn unix_name(export: UnixName, path: &Path) -> io::Result<Vec<u8>> {
        let mut name: Vec<u16> = path.as_os_str().encode_wide().collect();
        if name.contains(&0) {
            return Err(io::ErrorKind::InvalidInput.into());
        }
        name.push(0);
        // SAFETY: a NUL-terminated name; Wine returns a NUL-terminated string from the process
        // heap (or null), which is copied and then freed with HeapFree, as documented.
        unsafe {
            let result = export(name.as_ptr());
            if result.is_null() {
                return Err(io::Error::new(
                    io::ErrorKind::PermissionDenied,
                    format!("Wine gives {} no Unix name", path.display()),
                ));
            }
            let bytes = std::ffi::CStr::from_ptr(result).to_bytes().to_vec();
            HeapFree(GetProcessHeap(), 0, result.cast());
            Ok(bytes)
        }
    }
    pub fn trim(name: &[u8]) -> &[u8] {
        let mut name = name;
        while name.len() > 1 && name.ends_with(b"/") {
            name = &name[..name.len() - 1];
        }
        name
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn names_fold_character_by_character() {
        assert_eq!(name_key("ΑΣ"), name_key("Ασ"));
        assert_eq!(name_key("ας"), name_key("ΑΣ"));
        assert_eq!(fold('\u{1FB3}'), fold('\u{1FBC}'));
        assert_eq!(name_key("ShroudForge"), name_key("SHROUDFORGE"));
        // Coarser than NTFS, which keeps the Kelvin sign and k apart: a deny check refuses more.
        assert_eq!(fold('\u{212A}'), fold('k'));
    }
    // A path Wine gives no Unix name (a folder on its way is missing) is an error, never "within":
    // every caller refuses on it.
    #[cfg(windows)]
    #[test]
    fn a_path_wine_cannot_name_is_an_error() {
        let base = std::env::temp_dir().join(format!("sfi-same-folder-{}", std::process::id()));
        let missing = base.join("no-such-folder").join("leaf");
        if !under_wine() {
            eprintln!("native Windows: no Wine Unix names to test");
            assert!(wine_within(&missing, &base).unwrap());
            return;
        }
        for (path, root) in [(&missing, &base), (&base, &missing)] {
            let error = wine_within(path, root).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::PermissionDenied, "{error}");
        }
    }
}
