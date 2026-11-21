// SPDX-License-Identifier: MIT
//! Escape-only hardening. Resolve contained symlinks, then use pinned
//! directories and no-follow leaf opens. Never create parents before inspection.
//! SF reference: both pins lua/bindings/io/manage_exports.rs:14-40,91-123,162-190.
use std::{
    fs::{self, File},
    io,
    path::{Path, PathBuf},
};

/// Freeze existing symlink resolution before Lua execution, without creating a root.
pub(super) fn select_root(path: &Path) -> io::Result<PathBuf> {
    if !path.is_absolute() {
        return Err(io::Error::other("Export path must be absolute"));
    }
    let mut ancestor = path.to_path_buf();
    let mut missing = Vec::new();
    loop {
        match fs::symlink_metadata(&ancestor) {
            Ok(_) => {
                let mut selected = ancestor.canonicalize()?;
                for part in missing.into_iter().rev() {
                    selected.push(part);
                }
                return Ok(selected);
            }
            Err(e) if e.kind() == io::ErrorKind::NotFound => {
                missing.push(
                    ancestor
                        .file_name()
                        .ok_or_else(|| io::Error::from(io::ErrorKind::InvalidInput))?
                        .to_os_string(),
                );
                ancestor.pop();
            }
            Err(e) => return Err(e),
        }
    }
}
// A final dangling FILE link can be written by SF if its target parent already
// exists. Never create the link target's missing parents: SF doesn't do that.
fn dangling_file_target(path: &Path, budget: usize) -> io::Result<PathBuf> {
    if budget == 0 {
        return Err(io::Error::other("too many levels of symbolic links"));
    }
    match path.canonicalize() {
        Ok(path) => Ok(path),
        Err(e) if e.kind() == io::ErrorKind::NotFound => {
            if fs::symlink_metadata(path).is_ok_and(|m| m.file_type().is_symlink()) {
                let target = fs::read_link(path)?;
                let text = target.to_string_lossy();
                if text.ends_with(std::path::MAIN_SEPARATOR)
                    || (cfg!(windows) && text.ends_with('/'))
                {
                    return Err(e);
                }
                let target = if target.is_absolute() {
                    target
                } else {
                    path.parent().ok_or(e)?.join(target)
                };
                dangling_file_target(&target, budget - 1)
            } else {
                let parent = path.parent().ok_or(e)?.canonicalize()?;
                let name = path
                    .file_name()
                    .ok_or_else(|| io::Error::from(io::ErrorKind::NotFound))?;
                Ok(parent.join(name))
            }
        }
        Err(e) => Err(e),
    }
}
pub(super) struct Root {
    pub path: PathBuf,
    directory: platform::Directory,
}
impl Root {
    pub fn open(path: &Path, create: bool) -> io::Result<Self> {
        // path was selected before Lua. Do not canonicalize it AGAIN and silently select
        // a different anchor if the configured root/ancestor has since been replaced.
        let directory = platform::Directory::root(path, create, true)?;
        Ok(Self {
            path: path.to_path_buf(),
            directory,
        })
    }
    /// The configured root's parent, for the empty export only: it is not in the root, so
    /// it may be a link (a Wine drive or Steam library link), like the root's own ancestors.
    pub fn anchor(path: &Path) -> io::Result<Self> {
        Ok(Self {
            path: path.to_path_buf(),
            directory: platform::Directory::root(path, true, false)?,
        })
    }
    /// Inspect all existing ancestors before any mutation, including a missing leaf.
    pub fn resolve(&self, relative: &Path) -> io::Result<PathBuf> {
        self.resolve_path(relative, true)
    }
    fn resolve_path(&self, relative: &Path, file_leaf: bool) -> io::Result<PathBuf> {
        let mut path = self.path.clone();
        let mut missing = false;
        let mut components = relative.components().peekable();
        while let Some(c) = components.next() {
            path.push(c);
            if !missing {
                match fs::symlink_metadata(&path) {
                    Ok(metadata) => {
                        path = match path.canonicalize() {
                            Ok(path) => path,
                            Err(e)
                                if e.kind() == io::ErrorKind::NotFound
                                    && metadata.file_type().is_symlink()
                                    && !file_leaf =>
                            {
                                // SF create_dir_all fails on this existing dangling
                                // directory entry (both pins manage_exports.rs:112-114).
                                // Leave it for pinned/no-follow parent creation to return
                                // that native error; never create its missing target.
                                missing = true;
                                path
                            }
                            Err(e)
                                if e.kind() == io::ErrorKind::NotFound
                                    && metadata.file_type().is_symlink()
                                    && components.peek().is_none() =>
                            {
                                dangling_file_target(&path, 40)?
                            }
                            Err(e) => return Err(e),
                        };
                        // Under Wine canonicalize keeps a Unix link in the path: ask Wine.
                        #[cfg(windows)]
                        let outside = !path.starts_with(&self.path) || platform::wine_link(&path)?;
                        #[cfg(not(windows))]
                        let outside = !path.starts_with(&self.path);
                        if outside {
                            return Err(io::Error::new(
                                io::ErrorKind::PermissionDenied,
                                "export path resolves outside the configured export directory",
                            ));
                        }
                    }
                    Err(e)
                        if matches!(
                            e.kind(),
                            io::ErrorKind::NotFound | io::ErrorKind::NotADirectory
                        ) =>
                    {
                        missing = true
                    }
                    Err(e) => return Err(e),
                }
            }
        }
        Ok(path)
    }
    /// Unlike writes/copies, rename replaces the final directory ENTRY (both SF
    /// pins manage_exports.rs:104-123,162-171). Only its parent is dereferenced.
    pub fn rename_destination(&self, relative: &Path) -> io::Result<PathBuf> {
        let name = relative
            .file_name()
            .ok_or_else(|| io::Error::from(io::ErrorKind::IsADirectory))?;
        Ok(self
            .resolve_path(relative.parent().unwrap_or(Path::new("")), false)?
            .join(name))
    }
    pub fn parent(&self, path: &Path, create: bool) -> io::Result<(platform::Directory, PathBuf)> {
        let relative = path
            .strip_prefix(&self.path)
            .map_err(|_| io::Error::from(io::ErrorKind::PermissionDenied))?;
        let name = relative
            .file_name()
            .ok_or_else(|| io::Error::from(io::ErrorKind::IsADirectory))?;
        let parent = relative.parent().unwrap_or(Path::new(""));
        Ok((self.directory.descend(parent, create)?, PathBuf::from(name)))
    }
    pub fn read(&self, path: &Path) -> io::Result<File> {
        let (dir, name) = self.parent(path, false)?;
        dir.read(&name)
    }
    /// Preserve an empty export's trailing separator: the OS refuses it without
    /// creating the configured root as either a file or a directory (Windows SF).
    pub fn write_name(&self, name: &Path, bytes: &[u8]) -> io::Result<()> {
        #[cfg(windows)]
        {
            // Both Windows pins return ERROR_INVALID_NAME for this trailing-root
            // write (root-file.lua). FILE_FLAG_OPEN_REPARSE_POINT changes that
            // Win32 behavior, so refuse before it can accidentally create a file.
            // Keep the native OS error code/message, not a replacement Lua string.
            let _ = (name, bytes);
            Err(io::Error::from_raw_os_error(123))
        }
        #[cfg(not(windows))]
        {
            use io::Write;
            let mut file = self.directory.write(name, false)?;
            file.write_all(bytes)
        }
    }
    pub fn write(&self, path: &Path, bytes: &[u8]) -> io::Result<()> {
        use io::Write;
        let (dir, name) = self.parent(path, true)?;
        let mut file = dir.write(&name, false)?;
        file.write_all(bytes)
    }
    pub fn copy(&self, source: &Path, dest: &Path) -> io::Result<()> {
        let mut input = self.read(source)?;
        let (dir, name) = self.parent(dest, true)?;
        let mut output = dir.write(&name, true)?;
        io::copy(&mut input, &mut output)?;
        output.set_permissions(input.metadata()?.permissions())
    }
    pub fn rename(&self, source: &Path, dest: &Path) -> io::Result<()> {
        let (from, name) = self.parent(source, false)?;
        let (to, target) = self.parent(dest, true)?;
        let path = to.path().join(&target);
        if let Ok(link) = fs::read_link(&path)
            && !path.exists()
        {
            replace_dangling(&to, &target, &link, dest, || {
                from.rename(&name, &to, &target)
            })
        } else {
            from.rename(&name, &to, &target)
        }
    }
    pub fn delete(&self, path: &Path) -> io::Result<()> {
        let (dir, name) = self.parent(path, false)?;
        dir.delete(&name)
    }
    pub fn entries(&self, path: &Path) -> io::Result<(platform::Directory, fs::ReadDir)> {
        let relative = path
            .strip_prefix(&self.path)
            .map_err(|_| io::Error::from(io::ErrorKind::PermissionDenied))?;
        let guard = self.directory.descend(relative, false)?;
        let entries = guard.entries()?;
        // Keep the proc-fd path alive on Linux, and pin Windows ancestors during iteration.
        Ok((guard, entries))
    }
}

/// There is no portable compare-and-unlink. Quarantine the observed dangling
/// link with a no-replace move, verify it without following it, then commit with
/// another no-replace move. A substituted regular file is NEVER unlinked, and a
/// new destination appearing during the gap is NEVER overwritten. On a restore
/// conflict, retain the original entry in the named recovery directory, not delete
/// somebody else's data. This is escape/race-only hardening of SF's rename.
pub(super) fn replace_dangling(
    to: &platform::Directory,
    target: &Path,
    expected: &Path,
    destination: &Path,
    commit: impl FnOnce() -> io::Result<()>,
) -> io::Result<()> {
    use std::sync::atomic::{AtomicU64, Ordering};
    static NEXT: AtomicU64 = AtomicU64::new(0);
    let (stage_name, stage_path, stage) = loop {
        let name = format!(
            ".sfi-rename-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        );
        let path = to.path().join(&name);
        let builder = &mut fs::DirBuilder::new();
        #[cfg(target_os = "linux")]
        {
            use std::os::unix::fs::DirBuilderExt;
            builder.mode(0o700);
        }
        match builder.create(&path) {
            Ok(()) => match to.descend(Path::new(&name), false) {
                Ok(stage) => break (name, path, stage),
                Err(e) => {
                    let _ = fs::remove_dir(&path);
                    return Err(e);
                }
            },
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(e),
        }
    };
    let entry = Path::new("entry");
    let result = (|| {
        to.rename(target, &stage, entry)?;
        let staged = stage.path().join(entry);
        let result = if fs::read_link(&staged).is_ok_and(|link| link == expected) {
            commit()
        } else {
            Err(io::Error::new(
                io::ErrorKind::AlreadyExists,
                "destination export entry changed during rename",
            ))
        };
        if result.is_ok() {
            // Only the verified symlink in our private directory is removed.
            stage.delete(entry)?;
        } else if let Err(e) = stage.rename(entry, to, target) {
            return Err(io::Error::other(format!(
                "destination changed during rename; original entry retained at {}: {e}",
                destination
                    .with_file_name(&stage_name)
                    .join(entry)
                    .display()
            )));
        }
        result
    })();
    // Windows directory guards deny deletion, so close the stage before cleanup.
    drop(stage);
    // Never recursively remove: a restore conflict deliberately retains the entry.
    let cleanup = fs::remove_dir(&stage_path);
    result.and(cleanup)
}

/// What the export-root checks ask the loader once per process, asked now (on Windows the Wine lookup of
/// [`platform::wine`]); true under Wine (or Proton). See [`super::prime_export_checks`].
pub(super) fn prime() -> bool {
    #[cfg(windows)]
    return platform::wine().is_some();
    #[cfg(not(windows))]
    false
}

#[cfg(target_os = "linux")]
mod platform {
    use super::*;
    use std::{
        ffi::CString,
        os::{
            fd::{AsRawFd, FromRawFd},
            unix::ffi::OsStrExt,
        },
    };
    // Linux UAPI flags and libc *at calls, not a hidden runtime or a filesystem trait.
    const RDONLY: i32 = 0;
    const WRONLY: i32 = 1;
    const CREAT: i32 = 0o100;
    const EXCL: i32 = 0o200;
    const TRUNC: i32 = 0o1000;
    const DIRECTORY: i32 = 0o200000;
    // O_PATH pins search-only directories without requiring directory-list rights.
    const PATH_ONLY: i32 = 0o10000000;
    const NOFOLLOW: i32 = 0o400000;
    const CLOEXEC: i32 = 0o2000000;
    unsafe extern "C" {
        fn openat(fd: i32, path: *const std::ffi::c_char, flags: i32, ...) -> i32;
        fn mkdirat(fd: i32, path: *const std::ffi::c_char, mode: u32) -> i32;
        fn unlinkat(fd: i32, path: *const std::ffi::c_char, flags: i32) -> i32;
        fn renameat2(
            from: i32,
            name: *const std::ffi::c_char,
            to: i32,
            target: *const std::ffi::c_char,
            flags: u32,
        ) -> i32;
    }
    fn cpath(path: &Path) -> io::Result<CString> {
        CString::new(path.as_os_str().as_bytes())
            .map_err(|_| io::Error::from(io::ErrorKind::InvalidInput))
    }
    fn open(fd: i32, path: &Path, flags: i32) -> io::Result<File> {
        let name = cpath(path)?;
        // SAFETY: counted CString lives across the call; successful descriptor is owned once.
        let fd = unsafe { openat(fd, name.as_ptr(), flags | NOFOLLOW | CLOEXEC, 0o666u32) };
        if fd < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(unsafe { File::from_raw_fd(fd) })
        }
    }
    pub struct Directory(File);
    impl Directory {
        pub fn path(&self) -> PathBuf {
            PathBuf::from(format!("/proc/self/fd/{}", self.0.as_raw_fd()))
        }
        /// `contained` is for Windows (Wine links); the no-follow walk from / covers it here.
        pub fn root(path: &Path, create: bool, contained: bool) -> io::Result<Self> {
            let _ = contained;
            let root = Self(open(-100, Path::new("/"), PATH_ONLY | DIRECTORY)?);
            root.descend(path.strip_prefix("/").map_err(io::Error::other)?, create)
        }
        pub fn descend(&self, path: &Path, create: bool) -> io::Result<Self> {
            let mut dir = Self(self.0.try_clone()?);
            for name in path.components() {
                let name = Path::new(name.as_os_str());
                let next = match open(dir.0.as_raw_fd(), name, PATH_ONLY | DIRECTORY) {
                    Ok(file) => file,
                    Err(e)
                        if create
                            && matches!(
                                e.kind(),
                                io::ErrorKind::NotFound | io::ErrorKind::NotADirectory
                            ) =>
                    {
                        let name_c = cpath(name)?;
                        // SAFETY: valid CString and live directory descriptor; mkdirat cannot follow a leaf.
                        let rc = unsafe { mkdirat(dir.0.as_raw_fd(), name_c.as_ptr(), 0o777) };
                        let original = if rc < 0 {
                            let e = io::Error::last_os_error();
                            if e.kind() != io::ErrorKind::AlreadyExists {
                                return Err(e);
                            }
                            Some(e)
                        } else {
                            None
                        };
                        // Like create_dir_all, keep mkdir's original file-conflict error.
                        open(dir.0.as_raw_fd(), name, PATH_ONLY | DIRECTORY)
                            .map_err(|e| original.unwrap_or(e))?
                    }
                    Err(e) => return Err(e),
                };
                dir = Self(next);
            }
            Ok(dir)
        }
        pub fn read(&self, name: &Path) -> io::Result<File> {
            open(self.0.as_raw_fd(), name, RDONLY)
        }
        pub fn write(&self, name: &Path, new: bool) -> io::Result<File> {
            open(
                self.0.as_raw_fd(),
                name,
                WRONLY | CREAT | if new { EXCL } else { TRUNC },
            )
        }
        pub fn rename(&self, name: &Path, to: &Self, target: &Path) -> io::Result<()> {
            let name = cpath(name)?;
            let target = cpath(target)?;
            // SAFETY: both descriptors and strings live across the call. NOREPLACE avoids
            // clobbering a destination substituted after SF's visible existence check.
            let rc = unsafe {
                renameat2(
                    self.0.as_raw_fd(),
                    name.as_ptr(),
                    to.0.as_raw_fd(),
                    target.as_ptr(),
                    1,
                )
            };
            if rc < 0 {
                Err(io::Error::last_os_error())
            } else {
                Ok(())
            }
        }
        pub fn delete(&self, name: &Path) -> io::Result<()> {
            let name = cpath(name)?;
            // SAFETY: live descriptor and CString. unlinkat removes a link, never its target.
            if unsafe { unlinkat(self.0.as_raw_fd(), name.as_ptr(), 0) } < 0 {
                Err(io::Error::last_os_error())
            } else {
                Ok(())
            }
        }
        pub fn entries(&self) -> io::Result<fs::ReadDir> {
            fs::read_dir(format!("/proc/self/fd/{}", self.0.as_raw_fd()))
        }
    }
}

#[cfg(windows)]
mod platform {
    use super::*;
    use std::{
        ffi::{OsString, c_void},
        fs::OpenOptions,
        os::windows::{
            ffi::{OsStrExt, OsStringExt},
            fs::{MetadataExt, OpenOptionsExt},
            io::AsRawHandle,
        },
        sync::OnceLock,
    };
    const REPARSE: u32 = 0x00200000;
    const BACKUP: u32 = 0x02000000;
    /// FILE_ATTRIBUTE_REPARSE_POINT
    const REPARSE_POINT: u32 = 0x400;
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetFileAttributesW(name: *const u16) -> u32;
        fn GetModuleHandleW(name: *const u16) -> *mut c_void;
        fn GetProcAddress(module: *mut c_void, name: *const std::ffi::c_char) -> *mut c_void;
        fn GetProcessHeap() -> *mut c_void;
        fn HeapFree(heap: *mut c_void, flags: u32, memory: *mut c_void) -> i32;
        fn GetFileInformationByHandleEx(
            file: *mut c_void,
            class: u32,
            info: *mut c_void,
            size: u32,
        ) -> i32;
        fn DeviceIoControl(
            file: *mut c_void,
            code: u32,
            input: *const c_void,
            input_size: u32,
            output: *mut c_void,
            output_size: u32,
            returned: *mut u32,
            overlapped: *mut c_void,
        ) -> i32;
        fn GetVolumePathNameW(name: *const u16, volume: *mut u16, size: u32) -> i32;
    }
    fn wide(path: &Path) -> io::Result<Vec<u16>> {
        let mut wide: Vec<_> = path.as_os_str().encode_wide().collect();
        if wide.contains(&0) {
            return Err(io::ErrorKind::InvalidInput.into());
        }
        wide.push(0);
        Ok(wide)
    }
    /// The attributes Windows gives `path` by name, or `None` when there is no such name.
    fn attributes(path: &Path) -> io::Result<Option<u32>> {
        let name = wide(path)?;
        // SAFETY: a NUL-terminated UTF-16 name that lives across the call.
        match unsafe { GetFileAttributesW(name.as_ptr()) } {
            u32::MAX => match io::Error::last_os_error() {
                e if e.kind() == io::ErrorKind::NotFound => Ok(None),
                e => Err(e),
            },
            attributes => Ok(Some(attributes)),
        }
    }
    type UnixName = unsafe extern "C" fn(*const u16) -> *mut std::ffi::c_char;
    /// Wine's `wine_get_unix_file_name` (so: this is Wine, or Proton), `None` on native Windows. Looked up once per
    /// process (GetModuleHandleW, GetProcAddress: Wine's loader answers both under the loader lock); a host whose package
    /// DLLs may hold that lock later asks it first ([`super::prime`]).
    pub(super) fn wine() -> Option<UnixName> {
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
    fn unix_name(export: UnixName, path: &Path) -> io::Result<Vec<u8>> {
        let name = wide(path)?;
        // SAFETY: a NUL-terminated name; Wine returns a NUL-terminated string from the
        // process heap (or null), which is copied and then freed with HeapFree, as documented.
        unsafe {
            let result = export(name.as_ptr());
            if result.is_null() {
                return Err(io::Error::new(
                    io::ErrorKind::PermissionDenied,
                    "Wine gives this export path no Unix name",
                ));
            }
            let bytes = std::ffi::CStr::from_ptr(result).to_bytes().to_vec();
            HeapFree(GetProcessHeap(), 0, result.cast());
            Ok(bytes)
        }
    }
    fn trim(name: &[u8]) -> &[u8] {
        let mut name = name;
        while name.len() > 1 && name.ends_with(b"/") {
            name = &name[..name.len() - 1];
        }
        name
    }
    fn unix_parent(name: &[u8]) -> &[u8] {
        let name = trim(name);
        match name.iter().rposition(|b| *b == b'/') {
            Some(0) => &name[..1],
            Some(end) => &name[..end],
            None => b"",
        }
    }
    /// Whether `path` (an entry at or under the export root) is a Unix symbolic link under
    /// Wine (so Proton). Wine follows such a link on every open, FILE_FLAG_OPEN_REPARSE_POINT
    /// or not, and canonicalize keeps it in the path: the handle and the path both look like
    /// a plain folder or file. By name Wine still marks a link to a folder as a reparse point
    /// (0x410, as kiln-eml's mod discovery checks) while the handle shows none, and Wine's own
    /// Unix name of an entry resolves every link, so a link to a file, or to a folder
    /// elsewhere, has a Unix parent other than its folder's. Native Windows: always false (a
    /// link there is a reparse point the handle shows).
    pub(super) fn wine_link(path: &Path) -> io::Result<bool> {
        let Some(export) = wine() else {
            return Ok(false);
        };
        let by_name = attributes(path)?.ok_or_else(|| io::Error::from(io::ErrorKind::NotFound))?;
        if by_name & REPARSE_POINT != 0 {
            let entry = OpenOptions::new()
                .access_mode(0x80)
                .share_mode(7)
                .custom_flags(REPARSE | BACKUP)
                .open(path)?;
            if entry.metadata()?.file_attributes() & REPARSE_POINT == 0 {
                return Ok(true);
            }
        }
        let Some(folder) = path.parent() else {
            return Ok(false);
        };
        Ok(unix_parent(&unix_name(export, path)?) != trim(&unix_name(export, folder)?))
    }
    /// Wine shows a drive root (Z:\) and every Unix mount point as a mount-point reparse
    /// point that has no reparse data: where a volume begins, not a link. A junction (a
    /// mount point with data) or a symbolic link is none of these.
    fn wine_volume_boundary(entry: &File, path: &Path) -> io::Result<bool> {
        #[repr(C)]
        struct AttributeTag {
            attributes: u32,
            tag: u32,
        }
        let mut info = AttributeTag {
            attributes: 0,
            tag: 0,
        };
        // SAFETY: a live handle and an out record of FILE_ATTRIBUTE_TAG_INFO's layout (class 9).
        if unsafe {
            GetFileInformationByHandleEx(
                entry.as_raw_handle(),
                9,
                (&raw mut info).cast(),
                size_of::<AttributeTag>() as u32,
            )
        } == 0
        {
            return Err(io::Error::last_os_error());
        }
        if info.tag != 0xA000_0003 {
            return Ok(false);
        }
        let mut data = [0u8; 1024];
        let mut returned = 0u32;
        // SAFETY: a live handle, no input, and an output buffer of the size given.
        // FSCTL_GET_REPARSE_POINT must fail with ERROR_NOT_A_REPARSE_POINT: no data.
        let read = unsafe {
            DeviceIoControl(
                entry.as_raw_handle(),
                0x0009_00A8,
                std::ptr::null(),
                0,
                data.as_mut_ptr().cast(),
                data.len() as u32,
                &raw mut returned,
                std::ptr::null_mut(),
            )
        };
        if read != 0 || io::Error::last_os_error().raw_os_error() != Some(4390) {
            return Ok(false);
        }
        // And the system names this very entry as where its volume begins.
        let name = wide(path)?;
        let mut volume = vec![0u16; 32768];
        // SAFETY: a NUL-terminated name and an output buffer of the length given.
        if unsafe { GetVolumePathNameW(name.as_ptr(), volume.as_mut_ptr(), volume.len() as u32) }
            == 0
        {
            return Err(io::Error::last_os_error());
        }
        let end = volume.iter().position(|c| *c == 0).unwrap_or(volume.len());
        let volume = PathBuf::from(OsString::from_wide(&volume[..end]));
        Ok(fs::canonicalize(path)? == fs::canonicalize(volume)?)
    }
    // Denying FILE_SHARE_DELETE pins every ancestor against rename/reparse replacement.
    pub struct Directory {
        path: PathBuf,
        _guards: Vec<File>,
    }
    /// `contained`: `path` is the export root or lies under it, so no link may be followed
    /// there. The root's own ancestors may be links (a Steam library, a Wine drive).
    fn guard(
        path: &Path,
        directory: bool,
        write: bool,
        new: bool,
        contained: bool,
    ) -> io::Result<File> {
        // A write creates a name that is not there with CREATE_NEW, which never follows a
        // link. Under Wine a dangling Unix link is no name at all to Win32, and OPEN_ALWAYS
        // would create its target, wherever that is.
        let create_new = new || (write && attributes(path)?.is_none());
        let f = OpenOptions::new()
            .read(!write)
            .write(write)
            .create_new(create_new)
            // Attribute inspection/pinning must not require directory-list rights.
            .access_mode(if directory {
                0x80
            } else if write {
                0x40000000
            } else {
                0x80000000
            })
            .share_mode(if directory { 1 } else { 3 })
            .custom_flags(REPARSE | if directory { BACKUP } else { 0 })
            .open(path)
            .map_err(|e| {
                if create_new && !new && e.kind() == io::ErrorKind::AlreadyExists {
                    // The name Win32 did not show is a dangling link: never write through it.
                    io::Error::new(
                        io::ErrorKind::PermissionDenied,
                        "export path is a link that points nowhere",
                    )
                } else {
                    e
                }
            })?;
        let meta = f.metadata()?;
        if directory {
            if meta.file_attributes() & REPARSE_POINT != 0 {
                // Wine represents drive roots and Unix mount points as reparses.
                // A volume boundary is not a redirector. Its no-follow handle
                // denies write/delete, pinning the reparse itself during target use.
                let boundary = if wine().is_some() {
                    wine_volume_boundary(&f, path)?
                } else {
                    path.canonicalize()? == path && fs::metadata(path)?.is_dir()
                };
                if !boundary {
                    return Err(io::ErrorKind::PermissionDenied.into());
                }
            } else if !meta.is_dir() {
                return Err(io::ErrorKind::PermissionDenied.into());
            }
        } else if meta.file_attributes() & REPARSE_POINT != 0 || !meta.is_file() {
            // Device/reparse handles can bypass the selected filesystem root.
            return Err(io::ErrorKind::PermissionDenied.into());
        }
        if contained && wine_link(path)? {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "export path is a link Wine follows",
            ));
        }
        // Truncate only AFTER opening the non-reparse file and verifying its type.
        if write && !create_new {
            f.set_len(0)?;
        }
        Ok(f)
    }
    fn open_directory(path: &Path, create: bool, contained: bool) -> io::Result<File> {
        match guard(path, true, false, false, contained) {
            Ok(file) => Ok(file),
            Err(_) if create => {
                let original = match fs::create_dir(path) {
                    Ok(()) => None,
                    Err(e) if e.kind() == io::ErrorKind::AlreadyExists => Some(e),
                    Err(e) => return Err(e),
                };
                // Preserve create_dir_all's native file-conflict error, rather
                // than replacing it with the no-follow guard's type refusal.
                guard(path, true, false, false, contained).map_err(|e| original.unwrap_or(e))
            }
            Err(e) => Err(e),
        }
    }
    impl Directory {
        pub fn path(&self) -> PathBuf {
            self.path.clone()
        }
        pub fn root(path: &Path, create: bool, contained: bool) -> io::Result<Self> {
            let mut result = Self {
                path: PathBuf::new(),
                _guards: Vec::new(),
            };
            let last = path.components().count();
            for (i, c) in path.components().enumerate() {
                result.path.push(c);
                // A verbatim drive prefix alone (\\\\?\\C:) is not an openable
                // directory handle. Pin its RootDir, then each normal component.
                if matches!(c, std::path::Component::Prefix(_)) {
                    continue;
                }
                if result.path.is_absolute() {
                    let inside = contained && i + 1 == last;
                    result
                        ._guards
                        .push(open_directory(&result.path, create, inside)?);
                }
            }
            Ok(result)
        }
        pub fn descend(&self, path: &Path, create: bool) -> io::Result<Self> {
            let mut result = Self {
                path: self.path.clone(),
                _guards: self
                    ._guards
                    .iter()
                    .map(File::try_clone)
                    .collect::<io::Result<_>>()?,
            };
            for c in path.components() {
                result.path.push(c);
                result
                    ._guards
                    .push(open_directory(&result.path, create, true)?);
            }
            Ok(result)
        }
        pub fn read(&self, name: &Path) -> io::Result<File> {
            guard(&self.path.join(name), false, false, false, true)
        }
        pub fn write(&self, name: &Path, new: bool) -> io::Result<File> {
            guard(&self.path.join(name), false, true, new, true)
        }
        pub fn rename(&self, name: &Path, to: &Self, target: &Path) -> io::Result<()> {
            #[link(name = "kernel32")]
            unsafe extern "system" {
                fn MoveFileW(source: *const u16, destination: *const u16) -> i32;
            }
            fn argument(path: &Path) -> io::Result<Vec<u16>> {
                let mut wide: Vec<_> = path.as_os_str().encode_wide().collect();
                if wide.contains(&0) {
                    // Match std::fs::rename's Windows conversion (both SF pins
                    // manage_exports.rs:170), but only AFTER parent creation.
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        "file name contained an unexpected NUL byte",
                    ));
                }
                wide.push(0);
                Ok(wide)
            }
            let from = argument(&self.path.join(name))?;
            let to = argument(&to.path.join(target))?;
            // SAFETY: null-terminated UTF-16 buffers live for the call. MoveFileW is no-clobber.
            if unsafe { MoveFileW(from.as_ptr(), to.as_ptr()) } == 0 {
                Err(io::Error::last_os_error())
            } else {
                Ok(())
            }
        }
        pub fn delete(&self, name: &Path) -> io::Result<()> {
            fs::remove_file(self.path.join(name))
        }
        pub fn entries(&self) -> io::Result<fs::ReadDir> {
            fs::read_dir(&self.path)
        }
    }
    #[cfg(test)]
    mod tests {
        use super::*;
        // A path Wine gives no Unix name (a folder on its way is missing) is refused, never taken
        // for an entry that is no link: wine_link passes the error on to every caller.
        #[test]
        fn a_path_wine_cannot_name_is_refused() {
            let Some(export) = wine() else {
                eprintln!("native Windows: no Wine Unix names to test");
                return;
            };
            let missing = std::env::temp_dir()
                .join(format!("sfi-secure-{}", std::process::id()))
                .join("no-such-folder")
                .join("leaf");
            let error = unix_name(export, &missing).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::PermissionDenied, "{error}");
        }
    }
}

#[cfg(not(any(target_os = "linux", windows)))]
compile_error!("sandboxed exports require a handle-relative platform implementation");
