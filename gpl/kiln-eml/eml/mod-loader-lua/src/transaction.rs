// SPDX-License-Identifier: GPL-3.0-or-later
// kiln-eml: build privately, then publish the header last. While installing data the live header is vanilla,
// whose containers and resource prefix are immutable. A crash at any publication boundary leaves a usable game.
use std::{collections::BTreeMap, fs::{self, File, OpenOptions}, io::{self, Read, Seek, SeekFrom}, path::{Path, PathBuf}, rc::Rc, time::{Duration, Instant}};
use crate::progress::archive_work::{self, Note};
use kfc::{container::{KFCFile, KFCReader, KFCWriter, KFCWriteError}, guid::{ContentHash, ResourceId}, reflection::TypeRegistry};

pub type Writer = KFCWriter<Rc<KFCFile>, Rc<TypeRegistry>>;
pub(crate) const VANILLA_HEADER: &str = ".vanilla-header";
#[cfg(test)]
thread_local! { pub(crate) static BEFORE_PUBLISH: std::cell::Cell<Option<fn(&Path)>> = const { std::cell::Cell::new(None) }; }
#[cfg(test)]
thread_local! { static TEMP_OPEN: std::cell::Cell<Option<fn(&Path)>> = const { std::cell::Cell::new(None) }; }
// kiln-eml: deterministic validation/seal race seam; production has no callback here.
#[cfg(test)]
thread_local! { static AFTER_STAGE_VALIDATED: std::cell::Cell<Option<fn(&Path)>> = const { std::cell::Cell::new(None) }; }
// kiln-eml: between the two copies of a restore (load.rs), for the tests that look at what is held meanwhile
#[cfg(test)]
thread_local! { pub(crate) static DURING_RESTORE: std::cell::Cell<Option<fn(&Path)>> = const { std::cell::Cell::new(None) }; }

pub fn regular(path: &Path) -> io::Result<()> {
    let m = fs::symlink_metadata(path)?;
    if !m.is_file() || m.file_type().is_symlink() {
        return Err(io::Error::other(format!("not a regular file: {}", path.display())));
    }
    #[cfg(windows)] {
        use std::os::windows::fs::MetadataExt;
        if (m.file_attributes() | attributes_by_name(path)?) & 0x400 != 0 { return Err(io::Error::other("reparse point")); }
    }
    Ok(())
}

/// kiln-eml: the attributes of `path` asked by name, under Wine (0 on Windows proper). Wine follows links made on the
/// Linux side even with OPEN_REPARSE_POINT, so the handle-based metadata above shows a plain folder there; asked by
/// name, Wine reports directory links as reparse points (discover.rs does the same). Wine shows file links to neither
/// query. Windows proper reports junctions and links through the handle query already, and a raw by-name query of a
/// path longer than MAX_PATH fails there where std's (verbatim) one works: a deep game folder must not stop every
/// publication.
#[cfg(windows)]
pub(crate) fn attributes_by_name(path: &Path) -> io::Result<u32> {
    use std::os::windows::ffi::OsStrExt;
    if !under_wine() { return Ok(0); }
    #[link(name = "kernel32")]
    unsafe extern "system" { fn GetFileAttributesW(path: *const u16) -> u32; }
    let name: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
    let attributes = unsafe { GetFileAttributesW(name.as_ptr()) };
    if attributes == u32::MAX { return Err(io::Error::last_os_error()); }
    Ok(attributes)
}

/// kiln-eml: a flush to the drive that takes longer than this gets a kiln.log line: a drive busy with another
/// program's writes makes a start slow, and the line says where the time went.
const SLOW_SYNC: Duration = Duration::from_secs(2);

/// kiln-eml: `file.sync_all()`, with that line (`what`: the file, for the line).
pub(crate) fn sync_file(file: &File, what: &Path) -> io::Result<()> {
    let began = Instant::now();
    file.sync_all()?;
    slow_sync(began, what);
    Ok(())
}

fn slow_sync(began: Instant, what: &Path) {
    let took = began.elapsed();
    if took >= SLOW_SYNC {
        let name = what.file_name().map_or_else(|| what.display().to_string(), |n| n.to_string_lossy().into_owned());
        tracing::warn!("Flushing {name} to the drive took {:.1} s: the drive is busy or slow, which makes this start slow", took.as_secs_f64());
    }
}

pub(crate) fn sync_dir(dir: &Path) -> io::Result<()> {
    #[cfg(unix)] { sync_file(&File::open(dir)?, dir)?; }
    #[cfg(windows)] {
        if under_wine() {
            use std::os::windows::fs::OpenOptionsExt;
            // Wine's MoveFileEx ignores WRITE_THROUGH. A writable directory handle maps FlushFileBuffers to
            // fsync(dirfd), making the vanilla-header barrier and each subsequent rename durable on Unix too.
            let handle = OpenOptions::new().read(true).write(true).custom_flags(0x02000000) // FILE_FLAG_BACKUP_SEMANTICS
                .open(dir)?;
            sync_file(&handle, dir)?;
        }
    }
    #[cfg(not(any(unix, windows)))] { let _ = dir; }
    Ok(())
}

#[cfg(windows)]
fn under_wine() -> bool {
    // (asked once and kept, the first time on the boot thread: a later check may run on
    // kiln-sfi's deadline thread while a package's DllMain holds the loader lock, which Wine's GetModuleHandleW and
    // GetProcAddress take)
    use std::sync::atomic::{AtomicU8, Ordering};
    static UNDER_WINE: AtomicU8 = AtomicU8::new(0);
    match UNDER_WINE.load(Ordering::Acquire) {
        1 => return false,
        2 => return true,
        _ => {}
    }
    unsafe extern "system" {
        fn GetModuleHandleW(name: *const u16) -> *mut std::ffi::c_void;
        fn GetProcAddress(module: *mut std::ffi::c_void, name: *const u8) -> *mut std::ffi::c_void;
    }
    let name: Vec<u16> = "ntdll.dll".encode_utf16().chain(Some(0)).collect();
    let ntdll = unsafe { GetModuleHandleW(name.as_ptr()) };
    let wine = !ntdll.is_null() && !unsafe { GetProcAddress(ntdll, c"wine_get_version".as_ptr().cast()) }.is_null();
    UNDER_WINE.store(if wine { 2 } else { 1 }, Ordering::Release);
    wine
}

/// kiln-eml: asks now, on the calling thread, what the checks of a startup witness later ask the system once
/// (whether this is Wine): kiln-eml's early phase calls it on the boot thread, so a check on kiln-sfi's deadline thread
/// while a package's DllMain holds the loader lock resolves no symbol.
pub fn prime_witness_checks() {
    #[cfg(windows)]
    let _ = under_wine();
}

pub fn replace(src: &Path, dst: &Path) -> io::Result<()> {
    if dst.exists() { regular(dst)?; }
    #[cfg(windows)] {
        use std::os::windows::ffi::OsStrExt;
        unsafe extern "system" { fn MoveFileExW(a: *const u16, b: *const u16, flags: u32) -> i32; }
        let a: Vec<_> = src.as_os_str().encode_wide().chain(Some(0)).collect();
        let b: Vec<_> = dst.as_os_str().encode_wide().chain(Some(0)).collect();
        if unsafe { MoveFileExW(a.as_ptr(), b.as_ptr(), 1 | 8) } == 0 { return Err(io::Error::last_os_error()); }
    }
    #[cfg(not(windows))] { fs::rename(src, dst)?; }
    sync_dir(dst.parent().ok_or_else(|| io::Error::other("no parent"))?)
}

/// A same-volume rename that can never replace an existing recovery file.
pub(crate) fn move_new(src: &Path, dst: &Path) -> io::Result<()> {
    regular(src)?;
    sync_file(&OpenOptions::new().read(true).write(true).open(src)?, src)?;
    #[cfg(target_os = "linux")] {
        use std::{ffi::CString, os::unix::ffi::OsStrExt};
        unsafe extern "C" { fn renameat2(a: i32, src: *const std::ffi::c_char, b: i32, dst: *const std::ffi::c_char, flags: u32) -> i32; }
        let src = CString::new(src.as_os_str().as_bytes())?;
        let dst = CString::new(dst.as_os_str().as_bytes())?;
        if unsafe { renameat2(-100, src.as_ptr(), -100, dst.as_ptr(), 1) } != 0 { return Err(io::Error::last_os_error()); }
    }
    #[cfg(windows)] {
        use std::os::windows::ffi::OsStrExt;
        unsafe extern "system" { fn MoveFileExW(a: *const u16, b: *const u16, flags: u32) -> i32; }
        // std produces verbatim absolute paths, including for game folders longer than MAX_PATH.
        let source = fs::canonicalize(src)?;
        let target = fs::canonicalize(dst.parent().ok_or_else(|| io::Error::other("no recovery parent"))?)?.join(dst.file_name().ok_or_else(|| io::Error::other("no recovery name"))?);
        let a: Vec<_> = source.as_os_str().encode_wide().chain(Some(0)).collect();
        let b: Vec<_> = target.as_os_str().encode_wide().chain(Some(0)).collect();
        if unsafe { MoveFileExW(a.as_ptr(), b.as_ptr(), 8) } == 0 { return Err(io::Error::last_os_error()); }
    }
    #[cfg(not(any(target_os = "linux", windows)))]
    return Err(io::Error::other("exclusive recovery rename is unsupported on this platform"));
    sync_dir(dst.parent().ok_or_else(|| io::Error::other("no recovery parent"))?)?;
    sync_dir(src.parent().ok_or_else(|| io::Error::other("no archive parent"))?)
}

pub fn atomic_copy(src: &Path, dst: &Path) -> io::Result<()> { copy_via_temp(src, dst, true) }

/// kiln-eml: atomic_copy into a held stage (`PatchWriter::at`). The stage's own cleanup record covers the temps inside
/// it, so the copy writes no record of its own (on Linux the held path is /proc/self/fd/<fd>, under which record_temp
/// would otherwise create kiln/eml/ownership inside the stage).
pub(crate) fn stage_copy(src: &Path, dst: &Path) -> io::Result<()> { copy_via_temp(src, dst, false) }

fn copy_via_temp(src: &Path, dst: &Path, own_record: bool) -> io::Result<()> {
    regular(src)?;
    if fs::symlink_metadata(dst).is_ok() { regular(dst)?; }
    let tmp = dst.with_extension(format!("kiln-{}", uuid::Uuid::new_v4()));
    let record = if own_record { crate::cleanup::record_temp(dst, &tmp)? } else { None };
    failpoint!(if record.is_some(), "temp.recorded", dst);
    let result = (|| {
        let mut f = OpenOptions::new().write(true).create_new(true).open(&tmp)?;
        #[cfg(test)]
        TEMP_OPEN.with(|check| { if let Some(check) = check.take() { check(&tmp); } });
        copy_counted(&mut File::open(src)?, &mut f)?;
        sync_file(&f, dst)?;
        drop(f);
        failpoint!("temp.written", dst);
        replace(&tmp, dst)?;
        failpoint!("temp.replaced", dst);
        Ok(())
    })();
    let _ = fs::remove_file(&tmp);
    if !tmp.try_exists().unwrap_or(true) {
        if let Some(record) = record {
            // The rename/removal must persist before its write-ahead cleanup proof can disappear.
            sync_dir(dst.parent().unwrap())?;
            fs::remove_file(record)?;
            failpoint!("temp.record-removed", dst);
        }
    }
    result
}

/// kiln-eml: `io::copy` in 1 MiB steps, each counted for the startup pass's progress (`archive_work`). Windows' std
/// copies 8 KiB at a time, a system call each way (and through Wine's file layer under Proton): the resource backup
/// alone is over 200 MiB on a client.
fn copy_counted(src: &mut File, dst: &mut File) -> io::Result<u64> {
    use std::io::Write;
    let mut buffer = vec![0u8; 1 << 20];
    let mut copied = 0u64;
    loop {
        let n = match src.read(&mut buffer) {
            Ok(0) => return Ok(copied),
            Ok(n) => n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        };
        dst.write_all(&buffer[..n])?;
        archive_work::add(n as u64);
        copied += n as u64;
    }
}

pub fn atomic_write(dst: &Path, bytes: &[u8]) -> io::Result<()> {
    use std::io::Write;
    let tmp = dst.with_extension(format!("kiln-{}", uuid::Uuid::new_v4()));
    let record = crate::cleanup::record_temp(dst, &tmp)?;
    failpoint!(if record.is_some(), "temp.recorded", dst);
    let result = (|| {
        let mut f = OpenOptions::new().write(true).create_new(true).open(&tmp)?;
        f.write_all(bytes)?;
        sync_file(&f, dst)?;
        drop(f);
        failpoint!("temp.written", dst);
        replace(&tmp, dst)?;
        failpoint!("temp.replaced", dst);
        Ok(())
    })();
    let _ = fs::remove_file(&tmp);
    if !tmp.try_exists().unwrap_or(true) {
        if let Some(record) = record {
            // The rename/removal must persist before its write-ahead cleanup proof can disappear.
            sync_dir(dst.parent().unwrap())?;
            fs::remove_file(record)?;
            failpoint!("temp.record-removed", dst);
        }
    }
    result
}

// kiln-eml: identity comes from an open handle, not an interchangeable caller path.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct FileIdentity {
    #[cfg(unix)]
    id: (u64, u64),
    #[cfg(windows)]
    id: (u64, [u8; 16]),
}
impl FileIdentity {
    pub(crate) fn of(file: &File) -> io::Result<Self> {
        #[cfg(unix)] {
            use std::os::unix::fs::MetadataExt;
            let m = file.metadata()?;
            Ok(Self { id: (m.dev(), m.ino()) })
        }
        #[cfg(windows)] {
            use std::os::windows::io::AsRawHandle;
            #[repr(C)]
            struct IdInfo { volume: u64, id: [u8; 16] }
            #[link(name = "kernel32")]
            unsafe extern "system" {
                fn GetFileInformationByHandleEx(handle: *mut std::ffi::c_void, class: i32,
                    info: *mut std::ffi::c_void, size: u32) -> i32;
            }
            let mut info = IdInfo { volume: 0, id: [0; 16] };
            // FileIdInfo (18): volume + 128-bit file id. No pointer or Rust object escapes this call.
            if unsafe { GetFileInformationByHandleEx(file.as_raw_handle(), 18,
                (&mut info as *mut IdInfo).cast(), std::mem::size_of::<IdInfo>() as u32) } == 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(Self { id: (info.volume, info.id) })
        }
        #[cfg(not(any(unix, windows)))]
        { let _ = file; Err(io::Error::other("archive identity unsupported on this platform")) }
    }
    /// A stable text form for durable records (cleanup proof): equal tokens <=> equal identities on this platform.
    pub(crate) fn token(&self) -> String {
        #[cfg(unix)]
        { format!("unix:{:x}:{:x}", self.id.0, self.id.1) }
        #[cfg(windows)]
        { format!("win:{:x}:{}", self.id.0, self.id.1.iter().map(|b| format!("{b:02x}")).collect::<String>()) }
        #[cfg(not(any(unix, windows)))]
        { String::new() }
    }
    pub(crate) fn directory(path: &Path) -> io::Result<Self> {
        let mut options = OpenOptions::new();
        options.read(true);
        #[cfg(windows)] {
            use std::os::windows::fs::OpenOptionsExt;
            options.custom_flags(0x02000000); // FILE_FLAG_BACKUP_SEMANTICS
        }
        Self::of(&options.open(path)?)
    }
}

/// kiln-eml: a directory held open while Kiln works inside it, so its path keeps naming exactly this directory.
/// - Windows (the real target, Wine included): the handle is opened WITHOUT FILE_SHARE_DELETE, so the directory cannot
///   be renamed, moved or deleted while it is held; `path()` is its own path. The handle itself has DELETE access, so
///   `remove` deletes exactly this directory through it.
/// - Linux (host tests and kiln-eml-run only; the plugin never runs natively here): `path()` is /proc/self/fd/<fd>, which
///   reaches the held directory wherever it was moved, so a replacement at the old path is never enumerated or touched.
/// - other unix: the path itself (best available; not a Kiln target).
///
/// The identity comes from the held handle, never from a second open by path.
pub(crate) struct HeldDir {
    file: File,
    path: PathBuf,
}
impl HeldDir {
    pub(crate) fn open(path: &Path) -> io::Result<Self> {
        let mut options = OpenOptions::new();
        options.read(true);
        #[cfg(windows)] {
            use std::os::windows::fs::OpenOptionsExt;
            // GENERIC_READ | DELETE (list and identify it, and delete it through this handle only); FILE_FLAG_BACKUP_SEMANTICS
            // (a directory); share READ | WRITE, deliberately not DELETE
            options.access_mode(0x8000_0000 | 0x0001_0000).custom_flags(0x02000000).share_mode(0x1 | 0x2);
        }
        let file = options.open(path)?;
        if !file.metadata()?.is_dir() { return Err(io::Error::other("not a directory")); }
        #[cfg(target_os = "linux")]
        let via = {
            use std::os::fd::AsRawFd;
            PathBuf::from(format!("/proc/self/fd/{}", file.as_raw_fd()))
        };
        #[cfg(not(target_os = "linux"))]
        let via = path.to_path_buf();
        Ok(Self { file, path: via })
    }
    pub(crate) fn identity(&self) -> io::Result<FileIdentity> { FileIdentity::of(&self.file) }
    pub(crate) fn path(&self) -> &Path { &self.path }
    /// The held directory's children, as paths below `path()`, listed through the hold and never by opening the
    /// directory's path again. Windows: a new handle opened RELATIVE to the held one (an empty name: the held directory
    /// itself, wherever it is), sharing everything, enumerated and closed. `fs::read_dir` cannot be used there: Wine's
    /// FindFirstFileExW (Wine 11.0, GE-Proton11's) opens the directory sharing only READ | WRITE, which this hold's
    /// DELETE access refuses with a sharing violation (Windows proper and Wine 11.20 share DELETE there). A fresh handle
    /// per listing, because Wine keeps a listing per open directory and a restarted scan of the hold itself shows what
    /// was there at its first listing. Linux: `path()` is the held directory's /proc/self/fd link already.
    pub(crate) fn children(&self) -> io::Result<Vec<PathBuf>> {
        #[cfg(windows)]
        { Ok(list_through(&self.file)?.into_iter().map(|name| self.path.join(name)).collect()) }
        #[cfg(not(windows))]
        { fs::read_dir(&self.path)?.map(|e| e.map(|e| e.path())).collect() }
    }
    /// Delete the held (empty) directory through this handle, never by path: a path may name another directory by now.
    /// Windows marks this handle's directory delete-on-close and closes it (fails, keeping it, when it is not empty).
    /// Returns false where no handle-based removal exists (non-Windows host builds): the directory is kept.
    pub(crate) fn remove(self) -> io::Result<bool> {
        #[cfg(windows)] {
            use std::os::windows::io::AsRawHandle;
            #[link(name = "kernel32")]
            unsafe extern "system" {
                fn SetFileInformationByHandle(handle: *mut std::ffi::c_void, class: i32, info: *const std::ffi::c_void, size: u32) -> i32;
            }
            // FileDispositionInfo (4): FILE_DISPOSITION_INFO { BOOLEAN DeleteFile }. Only this handle's directory, at close.
            let delete: u8 = 1;
            if unsafe { SetFileInformationByHandle(self.file.as_raw_handle(), 4, (&delete as *const u8).cast(), 1) } == 0 {
                return Err(io::Error::last_os_error());
            }
            drop(self.file);
            Ok(true)
        }
        #[cfg(not(windows))]
        { drop(self); Ok(false) }
    }
}

/// kiln-eml: the names in the directory `held` is open on (not "." or ".."), through a handle opened relative to it
/// (HeldDir::children).
#[cfg(windows)]
fn list_through(held: &File) -> io::Result<Vec<std::ffi::OsString>> {
    use std::{ffi::c_void, os::windows::{ffi::OsStringExt, io::{AsRawHandle, FromRawHandle}}};
    #[repr(C)]
    struct UnicodeString { length: u16, maximum_length: u16, buffer: *mut u16 }
    #[repr(C)]
    struct ObjectAttributes {
        length: u32, root: *mut c_void, name: *const UnicodeString, attributes: u32, descriptor: *mut c_void, qos: *mut c_void,
    }
    #[repr(C)]
    struct IoStatusBlock { status: usize, information: usize }
    #[link(name = "ntdll", kind = "raw-dylib")]
    unsafe extern "system" {
        fn NtCreateFile(handle: *mut *mut c_void, access: u32, attributes: *const ObjectAttributes, io: *mut IoStatusBlock,
            allocation: *const i64, file_attributes: u32, share: u32, disposition: u32, options: u32, ea: *const c_void,
            ea_length: u32) -> i32;
        fn RtlNtStatusToDosError(status: i32) -> u32;
    }
    #[link(name = "kernel32", kind = "raw-dylib")]
    unsafe extern "system" {
        fn GetFileInformationByHandleEx(handle: *mut c_void, class: i32, info: *mut c_void, size: u32) -> i32;
    }
    let name = UnicodeString { length: 0, maximum_length: 0, buffer: std::ptr::null_mut() };
    let attributes = ObjectAttributes {
        length: size_of::<ObjectAttributes>() as u32, root: held.as_raw_handle(), name: &name, attributes: 0,
        descriptor: std::ptr::null_mut(), qos: std::ptr::null_mut(),
    };
    let mut io_status = IoStatusBlock { status: 0, information: 0 };
    let mut handle = std::ptr::null_mut();
    // FILE_LIST_DIRECTORY | SYNCHRONIZE; FILE_SHARE_READ | WRITE | DELETE; FILE_OPEN; FILE_DIRECTORY_FILE |
    // FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT
    let status = unsafe {
        NtCreateFile(&mut handle, 0x1 | 0x0010_0000, &attributes, &mut io_status, std::ptr::null(), 0, 0x1 | 0x2 | 0x4, 1,
            0x1 | 0x20 | 0x4000, std::ptr::null(), 0)
    };
    if status < 0 { return Err(io::Error::from_raw_os_error(unsafe { RtlNtStatusToDosError(status) } as i32)); }
    let listing = unsafe { File::from_raw_handle(handle) };
    // FILE_FULL_DIR_INFO records: NextEntryOffset at 0, FileNameLength (bytes) at 60, FileName (UTF-16) at 68
    const RESTART: i32 = 15; // FileFullDirectoryRestartInfo
    const NEXT: i32 = 14; // FileFullDirectoryInfo
    const ERROR_NO_MORE_FILES: i32 = 18;
    let mut buffer = vec![0u64; 8192];
    let mut names = Vec::new();
    let mut class = RESTART;
    loop {
        let size = (buffer.len() * 8) as u32;
        if unsafe { GetFileInformationByHandleEx(listing.as_raw_handle(), class, buffer.as_mut_ptr().cast(), size) } == 0 {
            let error = io::Error::last_os_error();
            if error.raw_os_error() == Some(ERROR_NO_MORE_FILES) { return Ok(names); }
            return Err(error);
        }
        class = NEXT;
        let bytes: &[u8] = unsafe { std::slice::from_raw_parts(buffer.as_ptr().cast(), buffer.len() * 8) };
        let field = |at: usize| bytes.get(at..at + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]) as usize);
        let mut at = 0;
        loop {
            let bad = || io::Error::other("a malformed directory listing");
            let (next, length) = (field(at).ok_or_else(bad)?, field(at + 60).ok_or_else(bad)?);
            let wide: Vec<u16> = bytes.get(at + 68..at + 68 + length).ok_or_else(bad)?
                .chunks_exact(2).map(|c| u16::from_le_bytes([c[0], c[1]])).collect();
            if wide != [0x2e] && wide != [0x2e, 0x2e] { names.push(std::ffi::OsString::from_wide(&wide)); }
            if next == 0 { break; }
            at += next;
        }
    }
}

/// The captured input witness is NOT a receipt or a lock. Existing callers retain their install guard.
/// Only the facade additionally borrows/checks that guard; legacy run_with already holds it in its caller.
pub(crate) struct TargetSnapshot {
    root: FileIdentity,
    stage: Option<FileIdentity>,
    input: InputWitness,
    baseline: Rc<KFCFile>,
}
impl TargetSnapshot {
    pub(crate) fn capture(game: &Path, name: &str, baseline: Rc<KFCFile>) -> io::Result<Self> {
        let input = InputWitness::capture(game, name, baseline.containers().len())?;
        Self::witnessed(input, baseline)
    }
    /// kiln-eml: a snapshot over inputs witnessed earlier (`StartupHost::open_matching`, after its check).
    pub(crate) fn witnessed(input: InputWitness, baseline: Rc<KFCFile>) -> io::Result<Self> {
        if input.containers != baseline.containers().len() {
            return Err(io::Error::other("the input witness covers another number of containers than the baseline"));
        }
        Ok(Self { root: input.root.clone(), stage: None, input, baseline })
    }
    pub(crate) fn input(&self) -> &InputWitness { &self.input }
    /// Refuses unless `game` and the stage's own path still name the directories captured and held. Only ever refuses:
    /// Kiln's writes and clean-up go through the held stage (`PatchWriter::at`), not through these paths.
    fn check_directories(&self, game: &Path, stage: &Path, held: Option<&HeldDir>) -> io::Result<()> {
        crate::ownership::checked_path(game, stage)?;
        if FileIdentity::directory(game)? != self.root
            || self.stage.as_ref() != Some(&FileIdentity::directory(stage)?)
            || held.map(HeldDir::identity).transpose()?.is_some_and(|id| self.stage.as_ref() != Some(&id)) {
            return Err(io::Error::other("archive root or private stage identity changed"));
        }
        Ok(())
    }
    fn check(&self, game: &Path, stage: &Path, held: Option<&HeldDir>) -> io::Result<()> {
        self.check_directories(game, stage, held)?;
        self.check_inputs(game)
    }
    fn check_root(&self, game: &Path) -> io::Result<()> {
        if FileIdentity::directory(game)? != self.root { return Err(io::Error::other("archive root identity changed")); }
        Ok(())
    }
    pub(crate) fn check_inputs(&self, game: &Path) -> io::Result<()> { self.input.check(game) }
}
fn optional_digest(root: &Path, path: &Path) -> io::Result<Option<crate::ownership::Digest>> {
    crate::ownership::checked_path(root, path)?;
    match fs::symlink_metadata(path) {
        Ok(_) => crate::ownership::digest(root, path).map(Some),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(error),
    }
}

/// kiln-eml: what the game's archive inputs were when a start looked at them, checked again before every step
/// that relies on them (`StartupHost` opening, staging, preparing and publishing) without reading the large files.
///
/// - The files whose bytes are parsed (`<stem>.kfc`, `.kfc.bak`, `.kfc.bak.integrity`: small) keep their SHA-256
///   and are hashed again at every check.
/// - The large ones (`<stem>.kfc_resources`, `.kfc_resources.bak`, every `<stem>_NNN.dat`) keep a stamp: the file's
///   identity and change time (`cache::stamp`, the change cache's own reader: device, inode and ctime on Unix; volume,
///   128-bit file id and ChangeTime on Windows and under Wine), its length and its last-write time. A check opens them
///   for their metadata only (Linux O_PATH, Windows FILE_READ_ATTRIBUTES), so it cannot read a byte of them.
///   The change time moves on every write, including a same-length rewrite whose last-write time is put back, and on
///   a rename onto the path the identity is another file's.
/// - A large file is hashed instead (and at every check) when no stamp can vouch for it: `KILN_EML_FULL_HASH` is set
///   (the change cache's escape hatch turns every use of the stamp off, this one too), the file system reports no
///   identity or change time, or keeps no reliable one (FAT, exFAT, CDFS, UDF on Windows), or the file changed less
///   than the change cache's racy window before the capture (the file system's clock may not have moved on since, so a
///   change right after the capture could keep the same change time).
/// - An input absent at the capture is witnessed as absent: its presence alone is a change, so a large one that
///   appears is not read to say so.
/// - The working baseline's backups (`BaselineIdentity`) are hashed once, at the capture: the resource backup's SHA-256
///   is taken between two equal stamps, so it belongs to the file the stamp then vouches for.
///
/// Unlike the change cache, the witness runs no scratch-file test of the volume (it writes nothing). Under Wine a host
/// mount of FAT or exFAT can report itself as NTFS (stamp.rs), so its large files are stamped: Linux keeps their change
/// time in memory, where every write moves it, and gives an inode it reads back from the disk a new number, so the
/// file id differs where the change time fell back to the last-write time.
///
/// Not caught: a program written to hide an in-place edit by setting the Windows ChangeTime back (the change cache's
/// stamp has the same limit); one that replaces the file is told by the file id. `Send + Sync + Clone`: `check` runs on
/// any thread.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InputWitness {
    game: PathBuf,
    root: FileIdentity,
    stem: String,
    containers: usize,
    inputs: BTreeMap<PathBuf, Option<Proof>>,
    baseline: crate::startup_host::BaselineIdentity,
    /// kiln-eml: when every large input hashed only because it changed within the racy window before the
    /// capture is out of that window (ns, the change cache's clock); None when no input was hashed for that reason.
    settles: Option<i128>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum Proof {
    /// A small file whose bytes are parsed: hashed again at every check.
    Sha256(crate::ownership::Digest),
    /// A large file: its identity and change time, length and last-write time; never read by a check.
    Stamp { stamp: crate::cache::stamp::Stamp, len: u64, modified: std::time::SystemTime },
}

/// The inputs whose bytes Kiln parses (always hashed) and the large ones (stamped where a stamp can vouch for them).
const PARSED: [&str; 3] = ["kfc", "kfc.bak", "kfc.bak.integrity"];
const LARGE: [&str; 2] = ["kfc_resources", "kfc_resources.bak"];

impl InputWitness {
    /// Witnesses `<stem>.kfc`, `.kfc_resources`, `.kfc.bak`, `.kfc_resources.bak`, `.kfc.bak.integrity` and
    /// `<stem>_000.dat` up to `containers` (the baseline's container count) in `game` (resolved first), and the live
    /// header's own containers past them. Reads the small files and the working baseline's backups in full,
    /// once; stamps the rest.
    pub fn capture(game: &Path, stem: &str, containers: usize) -> io::Result<Self> {
        crate::ownership::check_stem(stem)?;
        let game = fs::canonicalize(game)?;
        let root = FileIdentity::directory(&game)?;
        let stamps = !crate::cache::full_hash_requested(std::env::var_os("KILN_EML_FULL_HASH").as_deref());
        let mut inputs = BTreeMap::new();
        let mut settles: Option<i128> = None;
        for (path, large) in Self::paths(&game, stem, containers) {
            let proof = if large {
                let (proof, racy) = capture_large(&game, &path, stamps)?;
                if let Some(until) = racy { settles = Some(settles.map_or(until, |s| s.max(until))); }
                proof
            } else { optional_digest(&game, &path)?.map(Proof::Sha256) };
            inputs.insert(path, proof);
        }
        let mut files = BTreeMap::new();
        for ext in ["kfc.bak", "kfc_resources.bak", "kfc.bak.integrity"] {
            let path = game.join(format!("{stem}.{ext}"));
            let digest = match inputs.get(&path).and_then(Option::as_ref) {
                None => None,
                Some(Proof::Sha256(digest)) => Some(digest.clone()),
                Some(Proof::Stamp { stamp, len, modified }) => {
                    let digest = crate::ownership::digest(&game, &path)?;
                    if stamped(&path)? != Some((*stamp, *len, *modified)) || digest.size != *len {
                        return Err(io::Error::other(format!("archive input changed while it was read: {}", path.display())));
                    }
                    Some(digest)
                }
            };
            files.insert(ext, digest);
        }
        let witness = Self {
            game,
            root,
            stem: stem.to_owned(),
            containers,
            inputs,
            baseline: crate::startup_host::BaselineIdentity::of(files),
            settles,
        };
        // the root and every small file once more: a change while the large ones were looked at is not missed
        witness.check(&witness.game)?;
        Ok(witness)
    }

    /// Refuses unless `game` still resolves to the folder captured and every input is as witnessed (present or absent,
    /// same proof). Reads the small files only (and a large one hashed at the capture, see above).
    pub fn check(&self, game: &Path) -> io::Result<()> {
        if fs::canonicalize(game)? != self.game || FileIdentity::directory(&self.game)? != self.root {
            return Err(io::Error::other("archive root identity changed"));
        }
        for (path, expected) in &self.inputs {
            let now = match expected {
                Some(Proof::Sha256(_)) => optional_digest(&self.game, path)?.map(Proof::Sha256),
                // a stamped input by its metadata; one absent at the capture by its presence alone
                _ => {
                    crate::ownership::checked_path(&self.game, path)?;
                    match regular(path) {
                        Err(error) if error.kind() == io::ErrorKind::NotFound => None,
                        Err(error) => return Err(error),
                        Ok(()) if expected.is_none() => {
                            return Err(io::Error::other(format!("archive input changed: {}", path.display())));
                        }
                        Ok(()) => stamped(path)?.map(|(stamp, len, modified)| Proof::Stamp { stamp, len, modified }),
                    }
                }
            };
            if now.as_ref() != expected.as_ref() {
                return Err(io::Error::other(format!("archive input changed: {}", path.display())));
            }
        }
        Ok(())
    }

    /// The working baseline these inputs hold, hashed at the capture.
    pub fn baseline(&self) -> &crate::startup_host::BaselineIdentity { &self.baseline }

    /// kiln-eml: how long until a capture again would stamp every large input this one had to hash because it
    /// changed within the change cache's racy window before the capture (a check hashes such an input in full every
    /// time). None: no input was hashed for that reason (`KILN_EML_FULL_HASH` or a file system without stamps hash
    /// for good, and waiting would not change that).
    pub fn settles_in(&self) -> Option<std::time::Duration> {
        let left = self.settles? - crate::cache::now_ns();
        Some(std::time::Duration::from_nanos(left.clamp(0, u64::MAX as i128) as u64))
    }

    /// kiln-eml: size and SHA-256 (lower-case hex) of the parsed input `<stem>.<ext>` (`kfc`, `kfc.bak` or
    /// `kfc.bak.integrity`) as captured; None when it was absent.
    pub fn parsed(&self, ext: &str) -> Option<(u64, &str)> {
        if !PARSED.contains(&ext) { return None; }
        self.sha256(&self.game.join(format!("{}.{ext}", self.stem))).map(|d| (d.size, d.sha256()))
    }

    /// kiln-eml: the archive stem and the container count this witness covers, in the folder it resolved.
    pub fn stem(&self) -> &str { &self.stem }
    pub fn game(&self) -> &Path { &self.game }
    pub fn containers(&self) -> usize { self.containers }

    /// Whether this witness was captured for exactly these inputs: the archive `stem` with `containers` containers in
    /// the folder `game` resolves to now. (`check` says whether they are unchanged.)
    pub fn covers(&self, game: &Path, stem: &str, containers: usize) -> bool {
        self.stem == stem && self.containers == containers && fs::canonicalize(game).is_ok_and(|g| g == self.game)
    }

    /// The SHA-256 captured for `path` when it is one of the parsed (small) inputs; None for a stamped or absent one.
    pub(crate) fn sha256(&self, path: &Path) -> Option<&crate::ownership::Digest> {
        match self.inputs.get(path)? {
            Some(Proof::Sha256(digest)) => Some(digest),
            _ => None,
        }
    }

    fn paths(game: &Path, stem: &str, containers: usize) -> Vec<(PathBuf, bool)> {
        let mut paths = Vec::new();
        for ext in PARSED { paths.push((game.join(format!("{stem}.{ext}")), false)); }
        for ext in LARGE { paths.push((game.join(format!("{stem}.{ext}")), true)); }
        for i in 0..containers { paths.push((game.join(format!("{stem}_{i:03}.dat")), true)); }
        // (a published or cached generation's own containers)
        // The live header's containers past the baseline's (the ones the generation Kiln published added) are stamped
        // as the baseline's are, so a package DLL that rewrites one in place during a kiln start is seen. The live
        // header is a parsed input (hashed at every check), so this list cannot change unseen; a header that cannot be
        // read adds none (its digest still refuses any change).
        let live = read_index(&game.join(format!("{stem}.kfc"))).map_or(0, |index| index.containers().len());
        for i in containers..live { paths.push((game.join(format!("{stem}_{i:03}.dat")), true)); }
        paths
    }
}

/// A large input as captured: its stamp when one can vouch for it (see `InputWitness`; never when `stamps` is false,
/// `KILN_EML_FULL_HASH`), else its SHA-256.
/// kiln-eml: with the time (ns) the input leaves the racy window, when that window alone made it hashed.
fn capture_large(root: &Path, path: &Path, stamps: bool) -> io::Result<(Option<Proof>, Option<i128>)> {
    crate::ownership::checked_path(root, path)?;
    match fs::symlink_metadata(path) {
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok((None, None)),
        Err(error) => return Err(error),
        Ok(_) => regular(path)?,
    }
    let seen = crate::cache::now_ns();
    let file = open_metadata(path)?;
    let mut racy = None;
    if stamps
        && crate::cache::stamp::file_system_is_trustworthy(&file).is_ok()
        && let Some((stamp, len, modified)) = stamp_of(&file)?
    {
        let settles = stamp.changed.saturating_add(crate::cache::racy_window().as_nanos() as i128);
        if settles <= seen {
            return Ok((Some(Proof::Stamp { stamp, len, modified }), None));
        }
        racy = Some(settles);
    }
    crate::ownership::digest(root, path).map(|digest| (Some(Proof::Sha256(digest)), racy))
}

/// The stamp of the regular file at `path`, from a handle that cannot read its bytes; None when the file system reports
/// no identity or change time.
fn stamped(path: &Path) -> io::Result<Option<(crate::cache::stamp::Stamp, u64, std::time::SystemTime)>> {
    stamp_of(&open_metadata(path)?)
}
fn stamp_of(file: &File) -> io::Result<Option<(crate::cache::stamp::Stamp, u64, std::time::SystemTime)>> {
    let meta = file.metadata()?;
    if !meta.is_file() { return Err(io::Error::other("not a regular file")); }
    let Some(stamp) = crate::cache::stamp::read(file) else { return Ok(None) };
    Ok(Some((stamp, meta.len(), meta.modified()?)))
}

/// Opens `path` for its metadata only, never following a link at its last component: Linux O_PATH | O_NOFOLLOW (a
/// read on it fails, and a FIFO swapped in does not block the open); Windows FILE_READ_ATTRIBUTES only, every sharing
/// mode and FILE_FLAG_OPEN_REPARSE_POINT (no data access, so another program's handle that shares nothing does not
/// stop it either).
fn open_metadata(path: &Path) -> io::Result<File> {
    let mut options = OpenOptions::new();
    #[cfg(target_os = "linux")] {
        use std::os::unix::fs::OpenOptionsExt;
        const O_PATH: i32 = 0o1000_0000;
        #[cfg(any(target_arch = "aarch64", target_arch = "arm"))]
        const O_NOFOLLOW: i32 = 0o10_0000;
        #[cfg(not(any(target_arch = "aarch64", target_arch = "arm")))]
        const O_NOFOLLOW: i32 = 0o40_0000;
        options.read(true).custom_flags(O_PATH | O_NOFOLLOW);
    }
    #[cfg(windows)] {
        use std::os::windows::fs::OpenOptionsExt;
        // FILE_READ_ATTRIBUTES; FILE_SHARE_READ | WRITE | DELETE; FILE_FLAG_OPEN_REPARSE_POINT
        options.access_mode(0x80).share_mode(0x1 | 0x2 | 0x4).custom_flags(0x0020_0000);
    }
    #[cfg(not(any(target_os = "linux", windows)))]
    options.read(true);
    options.open(path)
}

pub struct PatchWriter {
    pub(crate) writer: Option<Writer>,
    /// The stage's own path in the game folder: its name, and the path checks that only refuse. Kiln's operations on
    /// the stage go through `at()`.
    pub(crate) stage: PathBuf,
    pub(crate) game: PathBuf,
    pub(crate) name: String,
    pub(crate) target: Option<TargetSnapshot>,
    /// The stage create_writer made, held from right after its creation until this writer is dropped (after `writer`):
    /// every staging copy, write, check, publication move and the final clean-up go through it. None only for the
    /// tests' hand-made stages.
    pub(crate) held: Option<HeldDir>,
}
impl PatchWriter {
    /// `held`: the stage create_writer holds since right after creating it; its identity (from that handle, never a
    /// second open by path, which a directory swapped in meanwhile could answer) is the stage's captured identity.
    pub(crate) fn staged(game: PathBuf, stage: PathBuf, name: String, mut target: TargetSnapshot, held: HeldDir) -> io::Result<Self> {
        target.stage = Some(held.identity()?);
        Ok(Self { writer: None, game, stage, name, target: Some(target), held: Some(held) })
    }
    /// Where Kiln reaches the stage: the held directory (wherever its path points now), else the stage path.
    pub(crate) fn at(&self) -> &Path { self.held.as_ref().map_or(&self.stage, HeldDir::path) }
    /// The stage's children, through the hold (HeldDir::children), else by the stage path (the tests' hand-made stages).
    fn children(&self) -> io::Result<Vec<PathBuf>> {
        match &self.held {
            Some(held) => held.children(),
            None => fs::read_dir(&self.stage)?.map(|e| e.map(|e| e.path())).collect(),
        }
    }
}
impl std::ops::Deref for PatchWriter {
    type Target = Writer;
    fn deref(&self) -> &Writer { self.writer.as_ref().unwrap() }
}
impl std::ops::DerefMut for PatchWriter {
    fn deref_mut(&mut self) -> &mut Writer { self.writer.as_mut().unwrap() }
}
impl Drop for PatchWriter {
    fn drop(&mut self) {
        self.writer.take();
        let held = self.held.take();
        // The record of this stage is in the game folder captured: none of it is touched in a replaced one.
        if let Some(target) = &self.target {
            let unchanged = if held.is_some() { target.check_root(&self.game) } else { target.check_directories(&self.game, &self.stage, None) };
            if unchanged.is_err() { return; }
        }
        // The held stage is this run's own directory wherever its path points now: it is cleaned through the handle, so
        // a directory swapped in at the path is never touched (a hand-made test stage without one goes by its path).
        if let Some(name) = self.stage.file_name().and_then(|n| n.to_str()) { crate::cleanup::finish_stage(&self.game, name, held); }
    }
}
impl PatchWriter {
    /// prepare then publish in one step, for the tests' hand-built writers (run_steps composes them itself, lib.rs).
    #[cfg(test)]
    pub fn finalize(self) -> Result<(), KFCWriteError> {
        self.prepare()?.publish()?;
        Ok(())
    }

    /// kiln-eml: finalize/close/flush/read-validate only. No ownership::prepare, SF access, or live replace.
    pub(crate) fn prepare(mut self) -> Result<PreparedArchive, KFCWriteError> {
        archive_work::mark(Note::Checking);
        self.writer.take().ok_or_else(|| io::Error::other("archive writer already finalized"))?.finalize()?;
        failpoint!("prepare.finalized", self.stage);
        let target = self.target.as_ref().ok_or_else(|| io::Error::other("no captured archive target"))?;
        target.check(&self.game, &self.stage, self.held.as_ref())?;
        let files = self.stage_files()?;
        for path in &files { sync_file(&OpenOptions::new().read(true).write(true).open(path)?, path)?; }
        sync_dir(self.at())?;
        failpoint!("prepare.synced", self.stage);
        // Bind validation to the SAME bytes, not a new digest taken only after the reader passed.
        let mut proof = BTreeMap::new();
        for path in files { proof.insert(path.clone(), crate::ownership::digest(self.at(), &path)?); }
        let index = validate_stage(&self, &target.baseline)?;
        #[cfg(test)]
        AFTER_STAGE_VALIDATED.with(|check| { if let Some(check) = check.take() { check(&self.stage); } });
        failpoint!("prepare.validated", self.stage);
        let prepared = PreparedArchive { patch: self, index, proof };
        prepared.check()?;
        Ok(prepared)
    }

    // Children are checked below the held stage (`at`): on Linux it is reached through /proc/self/fd, a link the game
    // folder's own check would refuse; the stage's own path is checked by `check_directories`.
    fn stage_files(&self) -> io::Result<Vec<PathBuf>> {
        crate::ownership::checked_path(&self.game, &self.stage)?;
        let mut files = Vec::new();
        for path in self.children()? {
            let name = path.file_name().and_then(|n| n.to_str()).ok_or_else(|| io::Error::other("invalid stage filename"))?;
            if name != VANILLA_HEADER && !crate::ownership::valid_name(&self.name, name) {
                return Err(io::Error::other("unexpected private archive child"));
            }
            crate::ownership::checked_path(self.at(), &path)?;
            regular(&path)?;
            files.push(path);
        }
        files.sort();
        Ok(files)
    }

    // Hook is used by crash-boundary tests; production never injects a failure.
    #[cfg(test)]
    fn publish(&self, boundary: impl FnMut(usize) -> io::Result<()>) -> io::Result<()> {
        self.publish_as(&crate::ownership::Publication::default(), boundary)
    }
    /// kiln-eml: the publication in a ShroudForge handoff mode, recording a BASE origin (`Publication`).
    fn publish_as(&self, publication: &crate::ownership::Publication, mut boundary: impl FnMut(usize) -> io::Result<()>) -> io::Result<()> {
        archive_work::mark(Note::Publishing);
        let header = format!("{}.kfc", self.name);
        let mut data = self.children()?;
        data.retain(|p| p.file_name() != Some(std::ffi::OsStr::new(VANILLA_HEADER)));
        data.sort();
        for p in &data { regular(p)?; sync_file(&OpenOptions::new().read(true).write(true).open(p)?, p)?; }
        // SF's locks (when its prepared-output state was handed over) stay held until the patched header is live
        let mut prepared = crate::ownership::prepare_from(&self.game, self.at(), &self.name, &data, publication)?;
        // No old patched header may refer to the containers we are about to replace.
        atomic_copy(&self.at().join(VANILLA_HEADER), &self.game.join(&header))?;
        failpoint!("publish.vanilla-header", self.game.join(&header));
        boundary(0)?;
        let mut n = 1;
        prepared.after_barrier(&self.game, || { boundary(n)?; n += 1; Ok(()) })?;
        for p in data.iter().filter(|p| p.file_name() != Some(std::ffi::OsStr::new(&header))) {
            replace(p, &self.game.join(p.file_name().unwrap()))?;
            failpoint!("publish.replaced", self.game.join(p.file_name().unwrap()));
            boundary(n)?;
            n += 1;
        }
        replace(&self.at().join(&header), &self.game.join(&header))?;
        failpoint!("publish.header", self.game.join(&header));
        // the new set is what the game runs now: the previous generation is no proof any more, and the containers only
        // it had go. A failure here leaves the receipt's previous generation (it can only match if those exact files
        // come back) or those containers (unreferenced) in place; the publication stands.
        if let Err(error) = crate::ownership::committed(&self.game, &self.name) {
            tracing::warn!(%error, "EML output published; tidying up after the output it replaced did not finish");
        }
        boundary(n)
    }
}

/// kiln-eml: a sealed PRIVATE view. Neither construction nor possession claims any live output.
/// No Clone, no writer/path accessor, and publish consumes self; error/drop uses the same recorded cleanup.
pub(crate) struct PreparedArchive {
    patch: PatchWriter,
    index: KFCFile,
    proof: BTreeMap<PathBuf, crate::ownership::Digest>,
}
impl PreparedArchive {
    pub(crate) fn index(&self) -> &KFCFile { &self.index }
    fn check(&self) -> io::Result<()> {
        self.patch.target.as_ref().ok_or_else(|| io::Error::other("missing target witness"))?
            .check(&self.patch.game, &self.patch.stage, self.patch.held.as_ref())?;
        if self.patch.stage_files()? != self.proof.keys().cloned().collect::<Vec<_>>() {
            return Err(io::Error::other("prepared archive file set changed"));
        }
        for (path, expected) in &self.proof {
            if crate::ownership::digest(self.patch.at(), path)? != *expected {
                return Err(io::Error::other("prepared archive bytes changed"));
            }
        }
        Ok(())
    }
    pub(crate) fn read_resource(&self, id: &ResourceId) -> io::Result<Option<Vec<u8>>> {
        self.check()?;
        KFCReader::new(self.patch.at(), &self.patch.name).map_err(io::Error::other)?
            .into_cursor().map_err(io::Error::other)?.read_resource(id)
    }
    pub(crate) fn read_content(&self, id: &ContentHash) -> io::Result<Option<Vec<u8>>> {
        self.check()?;
        read_content(&self.patch, &self.index, id)
    }
    #[cfg(test)]
    pub(crate) fn publish(self) -> io::Result<()> {
        self.publish_with(&crate::ownership::Publication::default())
    }
    /// kiln-eml: `publish` in a ShroudForge handoff mode, recording a BASE origin (`Publication`).
    pub(crate) fn publish_with(self, publication: &crate::ownership::Publication) -> io::Result<()> {
        self.check()?;
        // Existing test seam remains AFTER the last input barrier, as before. This hook models a foreign
        // edit during publication; run_with's later cache barrier still notices it and refuses cache completion.
        #[cfg(test)]
        BEFORE_PUBLISH.with(|check| { if let Some(check) = check.take() { check(&self.patch.game); } });
        self.patch.publish_as(publication, |_| Ok(()))
    }
}

fn range(offset: u64, size: u64, limit: u64) -> io::Result<()> {
    if offset.checked_add(size).is_none_or(|end| end > limit) {
        return Err(io::Error::other("staged archive reference out of bounds"));
    }
    Ok(())
}

// Structure source: kfc-base/src/container/header.rs:9-39. Bound table reads BEFORE parser allocation.
fn read_index(path: &Path) -> io::Result<KFCFile> {
    let mut f = File::open(path)?;
    let mut header = [0u8; 144];
    f.read_exact(&mut header)?;
    let word = |i| u32::from_le_bytes(header[i..i + 4].try_into().unwrap()) as u64;
    if &header[..4] != b"KFC4" || word(4) < 144 || word(4) > f.metadata()?.len() {
        return Err(io::Error::other("invalid staged archive header"));
    }
    for (i, width) in [1, 16, 0, 0, 12, 4, 8, 16, 16, 8, 32, 8, 8, 4, 12, 20].into_iter().enumerate() {
        if width != 0 { let field = 16 + i * 8; range(field as u64 + word(field), word(field + 4) * width, word(4))?; }
    }
    let file = KFCFile::from_path(path, false).map_err(io::Error::other)?;
    validate_map(file.contents())?;
    validate_map(file.resources())?;
    validate_map(file.resource_bundles())?;
    let count = word(60) as usize;
    if count != file.resources().len() { return Err(io::Error::other("staged resource catalog count mismatch")); }
    f.seek(SeekFrom::Start(56 + word(56)))?;
    let mut indices = Vec::with_capacity(count);
    for _ in 0..count {
        let mut bytes = [0; 4];
        f.read_exact(&mut bytes)?;
        let index = u32::from_le_bytes(bytes) as usize;
        if index >= file.resources().len() { return Err(io::Error::other("staged resource catalog index out of range")); }
        indices.push(index);
    }
    let mut covered = vec![false; count];
    for (hash, bundle) in file.resource_bundles().iter() {
        range(bundle.index as u64, bundle.count as u64, count as u64)?;
        for &index in &indices[bundle.index..bundle.index + bundle.count] {
            if covered[index] || file.resources().keys()[index].type_hash() != *hash {
                return Err(io::Error::other("staged resource bundle identity mismatch"));
            }
            covered[index] = true;
        }
    }
    if covered.iter().any(|present| !present) { return Err(io::Error::other("staged resource missing from catalog")); }
    Ok(file)
}
fn validate_map<K: PartialEq + kfc::container::StaticHash, V>(map: &kfc::container::StaticMap<K, V>) -> io::Result<()> {
    // Parser checks bucket spans, but not that lookup reaches every exact entry (or duplicate-key identity).
    for (key, entry) in map.iter() {
        if map.get(key).is_none_or(|found| !std::ptr::eq(found, entry)) {
            return Err(io::Error::other("staged archive lookup bucket mismatch"));
        }
    }
    Ok(())
}

fn container_path(patch: &PatchWriter, index: usize) -> io::Result<PathBuf> {
    let baseline = &patch.target.as_ref().ok_or_else(|| io::Error::other("missing baseline"))?.baseline;
    let name = format!("{}_{index:03}.dat", patch.name);
    let path = if index < baseline.containers().len() {
        if patch.at().join(&name).try_exists()? { return Err(io::Error::other("private output overlaps an immutable baseline DAT")); }
        patch.game.join(name)
    } else { patch.at().join(name) };
    crate::ownership::checked_path(if path.starts_with(patch.at()) { patch.at() } else { &patch.game }, &path)?;
    regular(&path)?;
    Ok(path)
}
fn read_content(patch: &PatchWriter, index: &KFCFile, id: &ContentHash) -> io::Result<Option<Vec<u8>>> {
    let Some(entry) = index.contents().get(id) else { return Ok(None) };
    let info = index.containers().get(entry.container_index).ok_or_else(|| io::Error::other("content container missing"))?;
    range(entry.offset, id.size() as u64, info.size)?;
    let mut f = File::open(container_path(patch, entry.container_index)?)?;
    f.seek(SeekFrom::Start(entry.offset))?;
    let mut bytes = vec![0; id.size() as usize];
    f.read_exact(&mut bytes)?;
    archive_work::add(bytes.len() as u64);
    if ContentHash::from_data(&bytes) != *id { return Err(io::Error::other("staged content hash mismatch")); }
    Ok(Some(bytes))
}

fn same_prefix(a: &Path, b: &Path, len: u64) -> io::Result<()> {
    let (mut a, mut b) = (File::open(a)?.take(len), File::open(b)?.take(len));
    let (mut x, mut y) = ([0; 65536], [0; 65536]);
    loop {
        let n = a.read(&mut x)?;
        b.read_exact(&mut y[..n])?;
        archive_work::add(2 * n as u64);
        if x[..n] != y[..n] { return Err(io::Error::other("staged resources changed the immutable baseline prefix")); }
        if n == 0 { return Ok(()); }
    }
}

fn validate_stage(patch: &PatchWriter, base: &KFCFile) -> io::Result<KFCFile> {
    let target = patch.target.as_ref().ok_or_else(|| io::Error::other("missing baseline"))?;
    let backup = patch.game.join(format!("{}.kfc.bak", patch.name));
    if optional_digest(patch.at(), &patch.at().join(VANILLA_HEADER))?.as_ref()
        != target.input().sha256(&backup) {
        return Err(io::Error::other("private vanilla header differs from captured baseline"));
    }
    let file = read_index(&patch.at().join(format!("{}.kfc", patch.name)))?;
    if file.game_version() != base.game_version() || file.containers().len() < base.containers().len() {
        return Err(io::Error::other("staged archive changed baseline identity"));
    }
    for (i, info) in file.containers().iter().enumerate() {
        if let Some(old) = base.containers().get(i) {
            if (old.size, old.count) != (info.size, info.count) { return Err(io::Error::other("baseline DAT metadata changed")); }
        }
        let actual_size = fs::metadata(container_path(patch, i)?)?.len();
        // Legacy run_with historically tolerates an overlong baseline DAT (including unreferenced padding).
        // Never truncate/rewrite that immutable input here. The new facade requires an exact validated base.
        if actual_size < info.size || (i >= base.containers().len() && actual_size != info.size) {
            return Err(io::Error::other("staged DAT size mismatch"));
        }
        if file.contents().values().iter().filter(|e| e.container_index == i).count() != info.count {
            return Err(io::Error::other("staged DAT count mismatch"));
        }
    }
    let resource_path = patch.at().join(format!("{}.kfc_resources", patch.name));
    let size = fs::metadata(&resource_path)?.len();
    let mut logical_end = 0;
    for chunk in file.resource_chunks() {
        range(chunk.offset, chunk.size, size)?;
        if chunk.compressed_size > chunk.size || chunk.uncompressed_offset != logical_end {
            return Err(io::Error::other("invalid staged resource chunk"));
        }
        logical_end = logical_end.checked_add(chunk.uncompressed_size).ok_or_else(|| io::Error::other("resource size overflow"))?;
    }
    let resource_backup = patch.game.join(format!("{}.kfc_resources.bak", patch.name));
    let source = if resource_backup.try_exists()? { resource_backup } else { patch.game.join(format!("{}.kfc_resources", patch.name)) };
    let prefix = base.resource_chunks().iter().map(|c| c.offset.checked_add(c.size))
        .collect::<Option<Vec<_>>>().ok_or_else(|| io::Error::other("baseline chunk overflow"))?.into_iter().max().unwrap_or(0);
    range(0, prefix, size)?;
    range(0, prefix, fs::metadata(&source)?.len())?;
    same_prefix(&source, &resource_path, prefix)?;
    // Reader caches are local to this validation and drop before a PreparedArchive can be returned.
    let mut reader = KFCReader::new(patch.at(), &patch.name).map_err(io::Error::other)?.into_cursor().map_err(io::Error::other)?;
    for (id, entry) in file.resources().iter() {
        let size = reader.resource_size(id).ok_or_else(|| io::Error::other("resource size missing"))?;
        range(entry.offset, size, logical_end)?;
        let bytes = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| reader.read_resource(id)))
            .map_err(|_| io::Error::other("invalid staged resource data"))??.ok_or_else(|| io::Error::other("resource has no chunk"))?;
        if bytes.len() as u64 != size { return Err(io::Error::other("staged resource decoded size mismatch")); }
        archive_work::add(size);
    }
    // kiln-eml: every content of the stage's own containers is read back and its hash checked. A content of one of the
    // baseline's containers that the baseline index lists at the same place is the game's own: the writer only appends
    // containers (`container_path` refuses a stage file over a baseline one), and those files are held by the input
    // witness, by identity, change time and length, from the capture until the publication (`TargetSnapshot::check`
    // before and after this validation, and again before publishing). Reading them back would read the whole game
    // install (48 GiB on a client) at every publication that is not cached, for no proof the witness does not give: such
    // a content keeps its range check only. Any other content in a baseline container is read and checked.
    for (id, entry) in file.contents().iter() {
        let baseline = entry.container_index < base.containers().len()
            && base.contents().get(id).is_some_and(|b| (b.offset, b.flags, b.container_index) == (entry.offset, entry.flags, entry.container_index));
        if baseline {
            let info = file.containers().get(entry.container_index).ok_or_else(|| io::Error::other("content container missing"))?;
            range(entry.offset, id.size() as u64, info.size)?;
        } else {
            read_content(patch, &file, id)?;
        }
    }
    Ok(file)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    #[test]
    fn killed_resource_temp_has_durable_cleanup_proof_before_any_copy() {
        let game = std::env::temp_dir().join(format!("kiln-temp-proof-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&game).unwrap();
        fs::write(game.join("g.kfc_resources"), "live resources").unwrap();
        TEMP_OPEN.with(|check| check.set(Some(|tmp| {
            let root = tmp.parent().unwrap();
            let records: Vec<_> = fs::read_dir(root.join("kiln/eml/ownership")).unwrap().flatten().collect();
            assert_eq!(records.len(), 1);
            let proof: serde_json::Value = serde_json::from_slice(&fs::read(records[0].path()).unwrap()).unwrap();
            assert_eq!(proof["name"], tmp.file_name().unwrap().to_str().unwrap());
            assert_eq!(fs::metadata(tmp).unwrap().len(), 0, "proof precedes staged bytes");
            fs::write(tmp, "half copied").unwrap();
            panic!("simulated killed copy");
        })));
        let killed = std::panic::catch_unwind(|| atomic_copy(&game.join("g.kfc_resources"), &game.join("g.kfc_resources.bak")));
        assert!(killed.is_err());
        crate::prune(&game, "g");
        assert_eq!(fs::read_dir(game.join("kiln/eml/ownership")).unwrap().count(), 0);
        assert!(crate::recovery_guidance(&game, "g").unwrap().is_empty());
        assert_eq!(fs::read(game.join("g.kfc_resources")).unwrap(), b"live resources");
        assert!(!game.join("g.kfc_resources.bak").exists());
        fs::remove_dir_all(game).unwrap();
    }
    #[test]
    fn directory_barrier_propagates_sync_errors() {
        #[cfg(windows)]
        if !under_wine() { return; } // native Windows uses MoveFileEx's WRITE_THROUGH instead
        let absent = std::env::temp_dir().join(format!("kiln-missing-directory-{}", uuid::Uuid::new_v4()));
        assert!(sync_dir(&absent).is_err());
    }
    #[test]
    fn interrupted_publication_keeps_vanilla_until_header_is_ready() {
        for fail in 0..=3 {
            let game = std::env::temp_dir().join(format!("kiln-transaction-{}", uuid::Uuid::new_v4()));
            let stage = game.join("stage");
            fs::create_dir_all(&stage).unwrap();
            for (n, b) in [("g.kfc", "old patch"), ("g.kfc.bak", "vanilla"), ("g.kfc_resources", "base old")] {
                fs::write(game.join(n), b).unwrap();
            }
            for (n, b) in [("g.kfc", "new patch"), ("g.kfc_resources", "base new"), ("g_002.dat", "new")] {
                fs::write(stage.join(n), b).unwrap();
            }
            fs::write(stage.join(VANILLA_HEADER), "vanilla").unwrap();
            let w = PatchWriter { writer: None, game: game.clone(), stage, name: "g".into(), target: None, held: None };
            assert!(w.publish(|n| if n == fail { Err(io::Error::other("power loss")) } else { Ok(()) }).is_err());
            assert_eq!(fs::read(game.join("g.kfc")).unwrap(), if fail == 3 { b"new patch".as_slice() } else { b"vanilla".as_slice() });
            assert!(fs::read(game.join("g.kfc_resources")).unwrap().starts_with(b"base"));
            assert_eq!(crate::owns_output(&game, "g").unwrap(), fail == 3,
                "a partial publication must never authorize restoring shared backups");
            drop(w);
            fs::remove_dir_all(game).unwrap();
        }
    }
    // The receipt is written ahead of the first change. Until that change happens, the previous (owned) output is what
    // is live: it must stay provable, or a player who then switches EML off keeps the old patch for good.
    #[test]
    fn a_publication_that_fails_before_its_first_change_keeps_the_previous_proof() {
        let game = std::env::temp_dir().join(format!("kiln-generations-{}", uuid::Uuid::new_v4()));
        fs::create_dir_all(&game).unwrap();
        for (n, b) in [("g.kfc", "vanilla"), ("g.kfc.bak", "vanilla"), ("g.kfc_resources", "base"), ("g.kfc_resources.bak", "base")] {
            fs::write(game.join(n), b).unwrap();
        }
        let stage = |tag: &str, vanilla: bool| {
            let stage = game.join(format!("stage-{tag}"));
            fs::create_dir_all(&stage).unwrap();
            for (n, b) in [("g.kfc", "patch"), ("g.kfc_resources", "base+"), ("g_002.dat", "data")] {
                fs::write(stage.join(n), format!("{b} {tag}")).unwrap();
            }
            if vanilla { fs::write(stage.join(VANILLA_HEADER), "vanilla").unwrap(); }
            PatchWriter { writer: None, game: game.clone(), stage, name: "g".into(), target: None, held: None }
        };
        stage("one", true).publish(|_| Ok(())).unwrap();
        assert!(crate::owns_output(&game, "g").unwrap());
        // the first step (the vanilla header) fails: nothing of the new set is live
        assert!(stage("two", false).publish(|_| Ok(())).is_err());
        assert_eq!(fs::read(game.join("g.kfc")).unwrap(), b"patch one");
        assert!(crate::owns_output(&game, "g").unwrap(), "the live previous output is still EML's");
        // a publication that completes replaces the proof: the older set is no longer EML's current output
        stage("three", true).publish(|_| Ok(())).unwrap();
        assert!(crate::owns_output(&game, "g").unwrap());
        for (n, b) in [("g.kfc", "patch one"), ("g.kfc_resources", "base+ one"), ("g_002.dat", "data one")] { fs::write(game.join(n), b).unwrap(); }
        assert!(!crate::owns_output(&game, "g").unwrap(), "only the last published generation is proof");
        fs::remove_dir_all(game).unwrap();
    }
    /// Is ShroudForge's write lock in `game` held by someone (this process's handoff guard, in these tests)?
    pub(crate) fn sf_write_lock_busy(game: &Path) -> bool {
        // The probe's own lock, if it gets one, is held under the test gate too (test_gate.rs).
        let _hold = crate::test_gate::lock_hold();
        OpenOptions::new().read(true).write(true).open(game.join("shroudforge/config/.shroudforge-write.lock"))
            .is_ok_and(|f| f.try_lock().is_err())
    }
    /// ShroudForge's state after it prepared asset mods for `stem` (asset-preparation.schema.json).
    pub(crate) fn sf_applied(game: &Path, stem: &str) {
        fs::create_dir_all(game.join("shroudforge/config")).unwrap();
        fs::write(game.join("shroudforge/state.json"), serde_json::to_vec(&serde_json::json!({"schemaVersion":1,"assets":{
            "schemaVersion":1,"status":"applied","phase":"startup","target":stem,"fingerprint":"a".repeat(64),
            "completedAt":42,"mods":[{"id":"sf-mod","version":"1.0.0"}]}})).unwrap()).unwrap();
    }
    // SF writes `preparing` under its write lock before its own preparation touches an archive: the handoff's lock is
    // held from the state edit through every step of the publication.
    #[test]
    fn the_sf_write_lock_is_held_at_every_publication_boundary() {
        let game = std::env::temp_dir().join(format!("kiln-sf-lock-{}", uuid::Uuid::new_v4()));
        let stage = game.join("stage");
        fs::create_dir_all(&stage).unwrap();
        for (n, b) in [("g.kfc", "old patch"), ("g.kfc.bak", "vanilla"), ("g.kfc_resources", "base old")] {
            fs::write(game.join(n), b).unwrap();
        }
        for (n, b) in [("g.kfc", "new patch"), ("g.kfc_resources", "base new"), ("g_002.dat", "new")] { fs::write(stage.join(n), b).unwrap(); }
        fs::write(stage.join(VANILLA_HEADER), "vanilla").unwrap();
        sf_applied(&game, "g");
        let w = PatchWriter { writer: None, game: game.clone(), stage, name: "g".into(), target: None, held: None };
        let mut seen = Vec::new();
        w.publish(|n| { seen.push((n, sf_write_lock_busy(&game))); Ok(()) }).unwrap();
        assert_eq!(seen, [(0, true), (1, true), (2, true), (3, true)], "held at every boundary");
        assert!(!sf_write_lock_busy(&game), "released once published");
        drop(w);
        fs::remove_dir_all(game).unwrap();
    }
    #[test]
    fn sf_lock_also_covers_missing_state_and_byte_identical_publications() {
        for unchanged in [true, false] {
            let game = std::env::temp_dir().join(format!("kiln-sf-all-writes-{}", uuid::Uuid::new_v4()));
            let stage = game.join("stage");
            fs::create_dir_all(&stage).unwrap();
            for (name, old, new) in [("g.kfc", "old patch", "new patch"), ("g.kfc_resources", "base old", "base new")] {
                fs::write(game.join(name), if unchanged { new } else { old }).unwrap();
                fs::write(stage.join(name), new).unwrap();
            }
            fs::write(game.join("g.kfc.bak"), "vanilla").unwrap();
            fs::write(stage.join(VANILLA_HEADER), "vanilla").unwrap();
            fs::create_dir_all(game.join("shroudforge")).unwrap();
            if unchanged { sf_applied(&game, "g"); }
            let state = game.join("shroudforge/state.json");
            let original = fs::read(&state).ok();
            let w = PatchWriter { writer: None, game: game.clone(), stage, name: "g".into(), target: None, held: None };
            w.publish(|n| {
                assert!(sf_write_lock_busy(&game), "SF lock missing at boundary {n}, unchanged={unchanged}");
                Ok(())
            }).unwrap();
            assert_eq!(fs::read(&state).ok(), original, "no state invented or unchanged claim invalidated");
            assert!(!sf_write_lock_busy(&game));
            drop(w);
            fs::remove_dir_all(game).unwrap();
        }
    }
    #[test]
    fn busy_sf_without_state_refuses_before_any_archive_write() {
        // This test locks ShroudForge's file itself: no kill-matrix child is created meanwhile (test_gate.rs).
        let _locks = crate::test_gate::lock_hold();
        let game = std::env::temp_dir().join(format!("kiln-sf-first-start-{}", uuid::Uuid::new_v4()));
        let stage = game.join("stage");
        fs::create_dir_all(&stage).unwrap();
        fs::create_dir_all(game.join("shroudforge/config")).unwrap();
        let lock = File::create(game.join("shroudforge/config/.shroudforge-write.lock")).unwrap();
        lock.lock().unwrap();
        for name in ["g.kfc", "g.kfc_resources"] {
            fs::write(game.join(name), "old").unwrap();
            fs::write(stage.join(name), "new").unwrap();
        }
        fs::write(game.join("g.kfc.bak"), "vanilla").unwrap();
        fs::write(stage.join(VANILLA_HEADER), "vanilla").unwrap();
        let w = PatchWriter { writer: None, game: game.clone(), stage, name: "g".into(), target: None, held: None };
        assert!(w.publish(|_| panic!("busy SF allowed an archive write")).is_err());
        assert_eq!(fs::read(game.join("g.kfc")).unwrap(), b"old");
        drop((w, lock));
        fs::remove_dir_all(game).unwrap();
    }
    // kiln-eml: validation reads the stage's own containers back and checks every content's hash there; a content of one
    // of the baseline's containers that the baseline lists at the same place is the game's own, held by the witness, and
    // is not read again (a client's containers hold 48 GiB).
    #[test]
    fn validation_reads_the_stages_own_containers_back_and_not_the_baselines() {
        use kfc::guid::ContentHash;
        let game = std::env::temp_dir().join(format!("kiln-validate-reads-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&game).unwrap();
        File::create(game.join("g.kfc")).unwrap();
        File::create(game.join("g.kfc_resources")).unwrap();
        let types = Rc::new(TypeRegistry::default());
        let mut vanilla = Writer::new(&game, "g", types.clone(), "fixture").unwrap();
        vanilla.write_content(&ContentHash::from_data(b"original"), b"original").unwrap();
        vanilla.finalize().unwrap();
        // a byte of the game's own container that its index does not vouch for (never so in a real install: here it
        // shows whether validation reads that container back)
        let dat = game.join("g_000.dat");
        let mut bytes = fs::read(&dat).unwrap();
        bytes[0] ^= 0xff;
        fs::write(&dat, &bytes).unwrap();
        let dir = camino::Utf8Path::from_path(&game).unwrap();
        crate::load::create_backup(dir.join("g.kfc")).unwrap();
        let base = Rc::new(KFCFile::from_path(game.join("g.kfc.bak"), false).unwrap());
        let mut patch = crate::load::create_writer(dir, "g", &types, &base).unwrap();
        patch.write_content(&ContentHash::from_data(b"new content"), b"new content").unwrap();
        patch.writer.take().unwrap().finalize().unwrap();
        let index = validate_stage(&patch, &base).expect("the baseline's container is not read back");
        assert_eq!(index.contents().len(), 2);
        // the stage's own container is read back: a byte changed there is refused
        let own = patch.at().join("g_001.dat");
        let mut staged = fs::read(&own).unwrap();
        staged[0] ^= 0xff;
        fs::write(&own, &staged).unwrap();
        let error = validate_stage(&patch, &base).unwrap_err();
        assert!(error.to_string().contains("staged content hash mismatch"), "{error}");
        drop(patch);
        fs::remove_dir_all(game).unwrap();
    }
    #[test]
    fn real_kfc_is_readable_at_every_publication_boundary() {
        use kfc::{container::KFCReader, guid::ContentHash};
        for fail in 0..=3 {
            let game = std::env::temp_dir().join(format!("kiln-real-transaction-{}", uuid::Uuid::new_v4()));
            fs::create_dir(&game).unwrap();
            File::create(game.join("g.kfc")).unwrap();
            File::create(game.join("g.kfc_resources")).unwrap();
            let types = Rc::new(TypeRegistry::default());
            let mut vanilla = Writer::new(&game, "g", types.clone(), "fixture").unwrap();
            vanilla.write_content(&ContentHash::from_data(b"original"), b"original").unwrap();
            vanilla.finalize().unwrap();
            let header = fs::read(game.join("g.kfc")).unwrap();
            let resources = fs::read(game.join("g.kfc_resources")).unwrap();
            let dir = camino::Utf8Path::from_path(&game).unwrap();
            crate::load::create_backup(dir.join("g.kfc")).unwrap();
            let base = Rc::new(KFCFile::from_path(game.join("g.kfc.bak"), false).unwrap());
            let mut patch = crate::load::create_writer(dir, "g", &types, &base).unwrap();
            patch.write_content(&ContentHash::from_data(b"new content"), b"new content").unwrap();
            patch.writer.take().unwrap().finalize().unwrap();
            // The barrier must use the validated private snapshot, even if the public backup changes mid-run.
            fs::write(game.join("g.kfc.bak"), b"damaged after preparation").unwrap();
            assert!(patch.publish(|n| if n == fail { Err(io::Error::other("interrupted")) } else { Ok(()) }).is_err());
            let live = KFCFile::from_path(game.join("g.kfc"), false).unwrap();
            for (index, info) in live.containers().iter().enumerate() {
                assert_eq!(game.join(format!("g_{index:03}.dat")).metadata().unwrap().len(), info.size);
            }
            assert!(fs::read(game.join("g.kfc_resources")).unwrap().starts_with(&resources));
            let mut reader = KFCReader::new(&game, "g").unwrap().into_cursor().unwrap();
            assert_eq!(reader.read_content(&ContentHash::from_data(b"original")).unwrap().unwrap(), b"original");
            if fail < 3 { assert_eq!(fs::read(game.join("g.kfc")).unwrap(), header); }
            else { assert_eq!(reader.read_content(&ContentHash::from_data(b"new content")).unwrap().unwrap(), b"new content"); }
            drop(reader);
            let stage = patch.stage.clone();
            drop(patch.held.take()); // abrupt exit: the process's handles close, no PatchWriter::drop
            std::mem::forget(patch);
            crate::prune(&game, "g");
            assert!(crate::cleanup::tests::reclaimed(&stage), "restart reclaims staged bytes at publication boundary {fail}");
            assert!(crate::recovery_guidance(&game, "g").unwrap().is_empty());
            fs::remove_dir_all(game).unwrap();
        }
    }
    fn collision_patch() -> PatchWriter {
        use kfc::{container::KFCWriteOptions, guid::ContentHash};
        let game = std::env::temp_dir().join(format!("kiln-displace-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&game).unwrap();
        File::create(game.join("g.kfc")).unwrap();
        File::create(game.join("g.kfc_resources")).unwrap();
        let types = Rc::new(TypeRegistry::default());
        let mut original = Writer::new_with_options(&game, "g", types.clone(), "fixture", KFCWriteOptions { max_container_size: 1, ..Default::default() }).unwrap();
        for blob in [b"vanilla one", b"vanilla two"] { original.write_content(&ContentHash::from_data(blob), blob).unwrap(); }
        original.finalize().unwrap();
        crate::load::create_backup(camino::Utf8Path::from_path(&game).unwrap().join("g.kfc")).unwrap();
        let base = Rc::new(KFCFile::from_path(game.join("g.kfc.bak"), false).unwrap());
        let mut patch = crate::load::create_writer(camino::Utf8Path::from_path(&game).unwrap(), "g", &types, &base).unwrap();
        patch.write_content(&ContentHash::from_data(b"new content"), b"new content").unwrap();
        patch.writer.take().unwrap().finalize().unwrap();
        fs::write(game.join("g_002.dat"), b"foreign content").unwrap();
        fs::write(game.join("g_003.dat"), b"").unwrap();
        patch
    }
    #[test]
    fn staged_index_rejects_misrouted_content_buckets() {
        use kfc::guid::ContentHash;
        let game = std::env::temp_dir().join(format!("kiln-bucket-validation-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&game).unwrap();
        File::create(game.join("g.kfc")).unwrap();
        File::create(game.join("g.kfc_resources")).unwrap();
        let mut writer = Writer::new(&game, "g", Rc::new(TypeRegistry::default()), "fixture").unwrap();
        for data in [b"one", b"two"] { writer.write_content(&ContentHash::from_data(data), data).unwrap(); }
        writer.finalize().unwrap();
        let mut bytes = fs::read(game.join("g.kfc")).unwrap();
        // Swap the two valid bucket spans: counts/ranges still pass the parser, lookup no longer matches the keys.
        let start = 64 + u32::from_le_bytes(bytes[64..68].try_into().unwrap()) as usize;
        assert_eq!(u32::from_le_bytes(bytes[68..72].try_into().unwrap()), 2);
        for i in 0..8 { bytes.swap(start + i, start + 8 + i); }
        fs::write(game.join("g.kfc"), bytes).unwrap();
        let rejected = read_index(&game.join("g.kfc")).is_err();
        fs::remove_dir_all(game).unwrap();
        assert!(rejected, "parsed content keys with invalid lookup buckets must not become a prepared index");
    }
    #[test]
    fn staged_index_rejects_out_of_range_resource_catalog_entries() {
        let game = std::env::temp_dir().join(format!("kiln-index-validation-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&game).unwrap();
        File::create(game.join("g.kfc")).unwrap();
        File::create(game.join("g.kfc_resources")).unwrap();
        Writer::new(&game, "g", Rc::new(TypeRegistry::default()), "fixture").unwrap().finalize().unwrap();
        let mut bytes = fs::read(game.join("g.kfc")).unwrap();
        // resource_indices (header.rs:23-24): a single index 0, but there are NO resources.
        bytes[56..60].copy_from_slice(&0u32.to_le_bytes());
        bytes[60..64].copy_from_slice(&1u32.to_le_bytes());
        fs::write(game.join("g.kfc"), bytes).unwrap();
        let rejected = read_index(&game.join("g.kfc")).is_err();
        fs::remove_dir_all(game).unwrap();
        assert!(rejected, "a parsed but invalid resource catalog must not become a prepared index");
    }
    #[test]
    fn changed_bytes_between_read_validation_and_sealing_never_get_a_prepared_token() {
        use kfc::guid::ContentHash;
        let game = std::env::temp_dir().join(format!("kiln-seal-race-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&game).unwrap();
        File::create(game.join("g.kfc")).unwrap();
        File::create(game.join("g.kfc_resources")).unwrap();
        let types = Rc::new(TypeRegistry::default());
        Writer::new(&game, "g", types.clone(), "fixture").unwrap().finalize().unwrap();
        let dir = camino::Utf8Path::from_path(&game).unwrap();
        crate::load::create_backup(dir.join("g.kfc")).unwrap();
        let base = Rc::new(KFCFile::from_path(game.join("g.kfc.bak"), false).unwrap());
        let mut patch = crate::load::create_writer(dir, "g", &types, &base).unwrap();
        patch.write_content(&ContentHash::from_data(b"new bytes"), b"new bytes").unwrap();
        let live = fs::read(game.join("g.kfc")).unwrap();
        AFTER_STAGE_VALIDATED.with(|check| check.set(Some(|stage| {
            use std::io::Write;
            // A same-size in-place change after the complete reader passed.
            OpenOptions::new().write(true).open(stage.join("g_001.dat")).unwrap().write_all(b"bad bytes").unwrap();
        })));
        assert!(patch.prepare().is_err(), "unvalidated replacement bytes acquired a prepared token");
        assert_eq!(fs::read(game.join("g.kfc")).unwrap(), live);
        assert!(!crate::owns_output(&game, "g").unwrap());
        fs::remove_dir_all(game).unwrap();
    }
    fn move_folders(game: &Path) -> Vec<PathBuf> {
        fs::read_dir(game.join("kiln/eml/displaced")).unwrap().flatten().map(|e| e.path()).collect()
    }
    #[test]
    fn displacement_is_recoverable_at_every_move_and_archive_publication_boundary() {
        use kfc::{container::KFCReader, guid::ContentHash};
        for fail in 0..=6 {
            let patch = collision_patch();
            let (game, stage) = (patch.game.clone(), patch.stage.clone());
            sf_applied(&game, "g");
            let vanilla = fs::read(game.join("g.kfc.bak")).unwrap();
            assert!(patch.publish(|n| {
                assert!(sf_write_lock_busy(&game), "lock missing at displacement boundary {n}");
                let folders = move_folders(&game);
                assert_eq!(folders.len(), 1);
                let journal: serde_json::Value = serde_json::from_slice(&fs::read(folders[0].join("moves.json")).unwrap()).unwrap();
                assert_eq!(journal["vanillaContainers"], 2);
                assert_eq!(journal["files"]["g_002.dat"]["size"], 15);
                if n < 6 { assert_eq!(fs::read(game.join("g.kfc")).unwrap(), vanilla, "moves and data writes use the vanilla barrier"); }
                if n == fail { Err(io::Error::other("killed publication")) } else { Ok(()) }
            }).is_err());
            let folder = move_folders(&game).pop().unwrap();
            for (n, name, original) in [(1, "g_002.dat", b"foreign content".as_slice()), (2, "g_003.dat", b"".as_slice())] {
                let path = if fail >= n { folder.join(name) } else { game.join(name) };
                assert_eq!(fs::read(path).unwrap(), original, "original preserved at boundary {fail}");
            }
            let mut reader = KFCReader::new(&game, "g").unwrap().into_cursor().unwrap();
            for blob in [b"vanilla one", b"vanilla two"] { assert_eq!(reader.read_content(&ContentHash::from_data(blob)).unwrap().unwrap(), blob); }
            drop(reader);
            let mut patch = patch;
            drop(patch.held.take());
            std::mem::forget(patch);
            crate::prune(&game, "g");
            assert!(crate::cleanup::tests::reclaimed(&stage));
            // A following enabled start can finish from the baseline; saved originals are never consumed or pruned.
            let types = Rc::new(TypeRegistry::default());
            let base = Rc::new(KFCFile::from_path(game.join("g.kfc.bak"), false).unwrap());
            let mut next = crate::load::create_writer(camino::Utf8Path::from_path(&game).unwrap(), "g", &types, &base).unwrap();
            next.write_content(&ContentHash::from_data(b"next patch"), b"next patch").unwrap();
            next.finalize().unwrap();
            assert!(crate::owns_output(&game, "g").unwrap());
            for (name, original) in [("g_002.dat", b"foreign content".as_slice()), ("g_003.dat", b"".as_slice())] {
                let saved: Vec<_> = move_folders(&game).iter().map(|d| d.join(name)).filter(|p| p.exists()).map(|p| fs::read(p).unwrap()).collect();
                assert_eq!(saved, [original], "each foreign original is saved exactly once after restart, boundary {fail}");
            }
            assert!(crate::restore(camino::Utf8Path::from_path(&game).unwrap(), "g"));
            assert_eq!(fs::read(game.join("g.kfc")).unwrap(), vanilla);
            fs::remove_dir_all(game).unwrap();
        }
    }
    #[test]
    fn an_interrupted_move_never_overwrites_recovery_or_publishes_more_archives() {
        for stop in [0, 1] {
            let patch = collision_patch();
            let game = patch.game.clone();
            let resources = fs::read(game.join("g.kfc_resources")).unwrap();
            let name = if stop == 0 { "g_002.dat" } else { "g_003.dat" };
            let result = patch.publish(|n| {
                if n == stop { fs::write(move_folders(&game)[0].join(name), "existing recovery").unwrap(); }
                Ok(())
            });
            assert!(result.is_err(), "an existing recovery destination must stop publication");
            assert_eq!(fs::read(move_folders(&game)[0].join(name)).unwrap(), b"existing recovery");
            assert!(game.join(name).exists(), "failed move leaves source in place");
            assert_eq!(fs::read(game.join("g.kfc_resources")).unwrap(), resources, "no data archive was written after a failed move");
            assert_eq!(fs::read(game.join("g.kfc")).unwrap(), fs::read(game.join("g.kfc.bak")).unwrap());
            drop(patch);
            fs::remove_dir_all(game).unwrap();
        }
    }
    #[test]
    fn a_written_ahead_receipt_cannot_claim_identical_unmoved_foreign_bytes() {
        let patch = collision_patch();
        let game = patch.game.clone();
        for name in ["g.kfc", "g.kfc_resources", "g_002.dat", "g_003.dat"] { fs::copy(patch.stage.join(name), game.join(name)).unwrap(); }
        let files: Vec<_> = ["g.kfc", "g.kfc_resources", "g_002.dat", "g_003.dat"].iter().map(|n| patch.stage.join(n)).collect();
        let prepared = crate::ownership::prepare(&game, "g", &files).unwrap();
        drop(prepared); // killed after receipt, before the vanilla barrier or any move
        assert!(!crate::owns_output(&game, "g").unwrap(), "future digests cannot claim untouched foreign bytes");
        let original = fs::read(game.join("g_002.dat")).unwrap();
        let prepared = crate::ownership::prepare(&game, "g", &files).unwrap();
        drop(prepared);
        let receipt: serde_json::Value = serde_json::from_slice(&fs::read(game.join("kiln/eml/ownership/g.json")).unwrap()).unwrap();
        assert_eq!(receipt["unproven"].as_array().unwrap().len(), 2, "even identical empty padding still needs displacement");
        assert_eq!(fs::read(game.join("g_002.dat")).unwrap(), original);
        patch.publish(|_| Ok(())).unwrap();
        assert!(crate::owns_output(&game, "g").unwrap());
        let saved: Vec<_> = move_folders(&game).iter().map(|d| d.join("g_002.dat")).filter(|p| p.exists()).map(|p| fs::read(p).unwrap()).collect();
        assert_eq!(saved, [original]);
        drop(patch);
        fs::remove_dir_all(game).unwrap();
    }
    #[test]
    fn a_destination_referenced_by_the_vanilla_header_is_never_displaced() {
        let patch = collision_patch();
        let game = patch.game.clone();
        fs::copy(game.join("g_000.dat"), patch.stage.join("g_000.dat")).unwrap();
        let original = fs::read(game.join("g_000.dat")).unwrap();
        assert!(patch.publish(|_| panic!("must refuse before even the vanilla barrier")).is_err());
        assert_eq!(fs::read(game.join("g_000.dat")).unwrap(), original);
        assert!(!game.join("kiln/eml/displaced").exists());
        drop(patch);
        fs::remove_dir_all(game).unwrap();
    }
    // A game folder deep enough that its stage files pass MAX_PATH (Steam libraries in long folder names).
    #[cfg(windows)]
    #[test]
    fn a_long_game_folder_path_passes_the_link_checks() {
        let mut game = std::env::temp_dir().join(format!("kiln-long-{}", uuid::Uuid::new_v4()));
        while game.as_os_str().len() < 300 { game = game.join("a-rather-long-steam-library-folder-name"); }
        fs::create_dir_all(&game).unwrap();
        let file = game.join(".kiln-eml-stage-0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0").join("enshrouded.kfc_resources");
        fs::create_dir_all(file.parent().unwrap()).unwrap();
        fs::write(&file, "staged").unwrap();
        regular(&file).unwrap();
        crate::ownership::tests::checked(&game, &file).unwrap();
        let saved = file.with_extension("saved");
        move_new(&file, &saved).unwrap();
        fs::write(&file, "another original").unwrap();
        assert!(move_new(&file, &saved).is_err(), "even a long recovery path must never be overwritten");
        assert_eq!(fs::read(&saved).unwrap(), b"staged");
        assert_eq!(fs::read(&file).unwrap(), b"another original");
        let mut top = game.as_path();
        while top.parent() != Some(&std::env::temp_dir()) { top = top.parent().unwrap(); }
        fs::remove_dir_all(top).unwrap();
    }
    #[cfg(unix)]
    #[test]
    fn atomic_copy_rejects_symlink_backup() {
        let d = std::env::temp_dir().join(format!("kiln-copy-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&d).unwrap();
        fs::write(d.join("src"), "original").unwrap();
        fs::write(d.join("victim"), "keep").unwrap();
        std::os::unix::fs::symlink(d.join("victim"), d.join("dst")).unwrap();
        assert!(atomic_copy(&d.join("src"), &d.join("dst")).is_err());
        assert_eq!(fs::read(d.join("victim")).unwrap(), b"keep");
        fs::remove_dir_all(d).unwrap();
    }
}
