// SPDX-License-Identifier: GPL-3.0-or-later
//! The stamped startup witness (`mod_loader_lua::startup_host::InputWitness`). A check reads the small parsed
//! inputs again and only the metadata of the large ones (resources, their backup, every container), yet catches a
//! same-length rewrite with a new change time, an identity swap and a file that appears or goes; `open_matching`
//! refuses a changed input; a 5 GiB (sparse) set is checked in under 50 ms; a generation's own added containers are
//! witnessed too.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;

use fixture::{
    TestRoot,
    archive::{ArchiveFixture, STEM},
};
use kfc::container::KFCFile;
use mod_loader::lua::startup_host::{InputWitness, StartupHost};
use std::{
    fs::{self, File, OpenOptions},
    io::{Seek, SeekFrom, Write},
    path::{Path, PathBuf},
    rc::Rc,
    time::{Duration, Instant},
};

const MIB: u64 = 1 << 20;
/// Longer than the change cache's racy window (3 s): a stamp taken later vouches for these files.
const SETTLE: Duration = Duration::from_millis(3300);
/// Lets a coarse change-time clock (one scheduler tick on Windows) move on before a rewrite.
const TICK: Duration = Duration::from_millis(60);

/// A synthetic game folder: small parsed files with real bytes, large ones sparse.
struct Game {
    _root: TestRoot,
    dir: PathBuf,
    containers: usize,
}
impl Game {
    fn new(tag: &str, large: u64, backup: u64, containers: usize) -> Self {
        let root = TestRoot::new(tag);
        let dir = root.0.join("game").into_std_path_buf();
        fs::create_dir(&dir).unwrap();
        let g = Self { _root: root, dir, containers };
        for (ext, len) in [("kfc", 4096), ("kfc.bak", 4096), ("kfc.bak.integrity", 64)] {
            fs::write(g.path(ext), pattern(len, ext.len() as u8)).unwrap();
        }
        sparse(&g.path("kfc_resources"), large);
        sparse(&g.path("kfc_resources.bak"), backup);
        for i in 0..containers {
            sparse(&g.dat(i), large);
        }
        g
    }
    fn path(&self, ext: &str) -> PathBuf {
        self.dir.join(format!("{STEM}.{ext}"))
    }
    fn dat(&self, i: usize) -> PathBuf {
        self.dir.join(format!("{STEM}_{i:03}.dat"))
    }
    fn small_bytes(&self) -> u64 {
        ["kfc", "kfc.bak", "kfc.bak.integrity"].iter().map(|e| fs::metadata(self.path(e)).unwrap().len()).sum()
    }
    fn large(&self) -> Vec<PathBuf> {
        let mut all = vec![self.path("kfc_resources"), self.path("kfc_resources.bak")];
        all.extend((0..self.containers).map(|i| self.dat(i)));
        all
    }
    fn capture(&self) -> InputWitness {
        InputWitness::capture(&self.dir, STEM, self.containers).unwrap()
    }
}

fn pattern(len: usize, seed: u8) -> Vec<u8> {
    (0..len).map(|i| (i as u8).wrapping_mul(31).wrapping_add(seed)).collect()
}

/// A file of `len` bytes that takes (almost) no disk: a few real bytes at the start, a hole after them.
fn sparse(path: &Path, len: u64) {
    let mut f = File::create(path).unwrap();
    #[cfg(windows)]
    set_sparse(&f);
    f.write_all(b"kiln large input").unwrap();
    f.set_len(len).unwrap();
}

#[cfg(windows)]
fn set_sparse(f: &File) {
    use std::os::windows::io::AsRawHandle;
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn DeviceIoControl(
            handle: *mut std::ffi::c_void, code: u32, input: *const std::ffi::c_void, input_len: u32,
            output: *mut std::ffi::c_void, output_len: u32, returned: *mut u32, overlapped: *mut std::ffi::c_void,
        ) -> i32;
    }
    let mut returned = 0u32;
    // FSCTL_SET_SPARSE
    let ok = unsafe {
        DeviceIoControl(f.as_raw_handle(), 0x0009_00C4, std::ptr::null(), 0, std::ptr::null_mut(), 0, &mut returned,
            std::ptr::null_mut())
    };
    assert_ne!(ok, 0, "FSCTL_SET_SPARSE: {}", std::io::Error::last_os_error());
}

/// Rewrites the first bytes of `path` in place (same length) and puts its last-write time back.
fn rewrite_in_place(path: &Path) {
    let before = fs::metadata(path).unwrap();
    std::thread::sleep(TICK);
    let mut f = OpenOptions::new().write(true).open(path).unwrap();
    f.seek(SeekFrom::Start(0)).unwrap();
    f.write_all(b"KILN LARGE INPUT").unwrap();
    f.set_modified(before.modified().unwrap()).unwrap();
    drop(f);
    let after = fs::metadata(path).unwrap();
    assert_eq!((after.len(), after.modified().unwrap()), (before.len(), before.modified().unwrap()),
        "precondition: length and last-write time are as before");
}

/// Bytes this thread has read through read(2) and friends (Linux per-thread I/O accounting).
#[cfg(target_os = "linux")]
fn thread_rchar() -> u64 {
    let io = fs::read_to_string("/proc/thread-self/io").unwrap();
    io.lines().find_map(|l| l.strip_prefix("rchar: ")).unwrap().trim().parse().unwrap()
}

fn changed(error: std::io::Error, path: &Path) {
    let text = error.to_string();
    assert!(text.contains("archive input changed") && text.contains(&*path.file_name().unwrap().to_string_lossy()),
        "{text}");
}

#[test]
fn the_witness_is_send_sync_clone_and_a_settled_recapture_equals_it() {
    fn shared<T: Send + Sync + Clone + 'static>() {}
    shared::<InputWitness>();
    let g = Game::new("g13p-equal", 8 * MIB, 8 * MIB, 3);
    std::thread::sleep(SETTLE);
    let w = g.capture();
    assert_eq!(g.capture(), w, "nothing changed: the same proofs");
    let (dir, clone) = (g.dir.clone(), w.clone());
    std::thread::spawn(move || clone.check(&dir).unwrap()).join().unwrap();
    // the baseline identity is the backups' bytes: another resource backup is another baseline
    let base = w.baseline().clone();
    rewrite_in_place(&g.path("kfc_resources.bak"));
    std::thread::sleep(SETTLE);
    assert_ne!(g.capture().baseline(), &base);
}

#[cfg(target_os = "linux")]
#[test]
fn a_check_reads_no_byte_of_a_large_file() {
    let g = Game::new("g13p-bytes", 256 * MIB, 64 * MIB, 4);
    std::thread::sleep(SETTLE);
    let before = thread_rchar();
    let w = g.capture();
    let capture = thread_rchar() - before;
    // control: the counter sees reads, and the capture hashes the resource backup once
    assert!(capture >= 64 * MIB, "the capture read {capture} bytes");
    assert!(capture < 64 * MIB + 4 * g.small_bytes() + 64 * 1024, "the capture read {capture} bytes");
    for _ in 0..3 {
        let before = thread_rchar();
        w.check(&g.dir).unwrap();
        let read = thread_rchar() - before;
        // the small parsed files once, plus the few hundred bytes of the counter's own read
        assert!(read <= g.small_bytes() + 4096, "a check read {read} bytes; small inputs are {}", g.small_bytes());
    }
}

#[cfg(target_os = "linux")]
#[test]
fn a_file_changed_a_moment_before_the_capture_is_hashed_not_stamped() {
    // no settling: the change time is inside the racy window, so the stamp cannot vouch for the file
    let g = Game::new("g13p-racy", 8 * MIB, 8 * MIB, 2);
    let w = g.capture();
    let before = thread_rchar();
    w.check(&g.dir).unwrap();
    let read = thread_rchar() - before;
    assert!(read >= 4 * 8 * MIB, "every large file is hashed again: {read}");
    // and a rewrite at once (no tick in between) is caught by its bytes
    let mut f = OpenOptions::new().write(true).open(g.dat(1)).unwrap();
    f.write_all(b"KILN").unwrap();
    drop(f);
    changed(w.check(&g.dir).unwrap_err(), &g.dat(1));
}

#[test]
fn a_same_length_rewrite_with_a_new_change_time_is_caught() {
    let g = Game::new("g13p-rewrite", 8 * MIB, 8 * MIB, 2);
    std::thread::sleep(SETTLE);
    for path in g.large() {
        let w = g.capture();
        w.check(&g.dir).unwrap();
        rewrite_in_place(&path);
        changed(w.check(&g.dir).unwrap_err(), &path);
    }
    // a small parsed file: hashed again
    let w = g.capture();
    let header = g.path("kfc");
    let mut bytes = fs::read(&header).unwrap();
    bytes[100] ^= 0xff;
    fs::write(&header, bytes).unwrap();
    changed(w.check(&g.dir).unwrap_err(), &header);
}

#[test]
fn an_identity_swap_is_caught() {
    let g = Game::new("g13p-swap", 8 * MIB, 8 * MIB, 2);
    // a copy with the same bytes and last-write time, made before the capture
    let mut twins = Vec::new();
    for path in g.large() {
        let twin = path.with_extension("twin");
        fs::copy(&path, &twin).unwrap();
        File::options().write(true).open(&twin).unwrap().set_modified(fs::metadata(&path).unwrap().modified().unwrap()).unwrap();
        twins.push((path, twin));
    }
    std::thread::sleep(SETTLE);
    for (path, twin) in twins {
        let w = g.capture();
        assert_eq!(fs::read(&path).unwrap(), fs::read(&twin).unwrap());
        fs::rename(&twin, &path).unwrap();
        let m = fs::metadata(&path).unwrap();
        assert_eq!(m.len(), fs::metadata(&g.path("kfc_resources")).unwrap().len());
        changed(w.check(&g.dir).unwrap_err(), &path);
    }
}

#[test]
fn an_input_that_appears_or_goes_is_caught() {
    let g = Game::new("g13p-presence", 8 * MIB, 8 * MIB, 2);
    fs::remove_file(g.path("kfc.bak.integrity")).unwrap();
    std::thread::sleep(SETTLE);
    let w = g.capture();
    fs::write(g.path("kfc.bak.integrity"), b"new").unwrap();
    changed(w.check(&g.dir).unwrap_err(), &g.path("kfc.bak.integrity"));
    fs::remove_file(g.path("kfc.bak.integrity")).unwrap();
    w.check(&g.dir).unwrap();
    fs::remove_file(g.dat(1)).unwrap();
    changed(w.check(&g.dir).unwrap_err(), &g.dat(1));
    // a link in place of a large input is refused
    #[cfg(unix)]
    {
        std::os::unix::fs::symlink(g.dat(0), g.dat(1)).unwrap();
        assert!(w.check(&g.dir).is_err());
    }
}

/// A large input absent at the capture that appears later is a change told by its presence alone: the check reads none
/// of it (Linux: the thread's read counter; Windows: it is held by another handle that shares nothing, so a data open
/// would fail with a sharing violation instead of the refusal).
#[test]
fn a_large_input_that_appears_is_caught_without_reading_it() {
    let g = Game::new("g13p-appears", 8 * MIB, 8 * MIB, 2);
    for path in [g.path("kfc_resources.bak"), g.dat(1)] {
        fs::remove_file(&path).unwrap();
    }
    std::thread::sleep(SETTLE);
    let w = g.capture();
    for path in [g.path("kfc_resources.bak"), g.dat(1)] {
        sparse(&path, 64 * MIB);
        #[cfg(windows)]
        let held = {
            use std::os::windows::fs::OpenOptionsExt;
            OpenOptions::new().read(true).write(true).share_mode(0).open(&path).unwrap()
        };
        #[cfg(target_os = "linux")]
        let before = thread_rchar();
        changed(w.check(&g.dir).unwrap_err(), &path);
        #[cfg(target_os = "linux")]
        {
            let read = thread_rchar() - before;
            assert!(read <= g.small_bytes() + 4096, "a check read {read} bytes; small inputs are {}", g.small_bytes());
        }
        #[cfg(windows)]
        drop(held);
        fs::remove_file(&path).unwrap();
        w.check(&g.dir).unwrap();
    }
}

/// A program that keeps its handle open while it writes: the change time moves with the write itself, not when the
/// handle closes (natively NTFS updates the last-write and change time at each WriteFile, as Linux and Wine do at each
/// write), so the very next check refuses.
#[test]
fn a_write_through_a_handle_still_open_is_caught() {
    let g = Game::new("g13p-open-writer", 8 * MIB, 8 * MIB, 2);
    for path in [g.path("kfc_resources"), g.dat(1)] {
        // opened before the capture, written after it
        let mut writer = OpenOptions::new().write(true).open(&path).unwrap();
        std::thread::sleep(SETTLE);
        let w = g.capture();
        w.check(&g.dir).unwrap();
        std::thread::sleep(TICK);
        writer.write_all(b"KILN LARGE INPUT").unwrap();
        assert_eq!(fs::metadata(&path).unwrap().len(), 8 * MIB, "precondition: the same length");
        changed(w.check(&g.dir).unwrap_err(), &path);
        drop(writer);
    }
}

#[test]
fn a_five_gib_sparse_set_is_checked_in_under_50_ms() {
    // resources 1 GiB and four 1 GiB containers; a 16 MiB resource backup
    let g = Game::new("g13p-5gib", 1 << 30, 16 * MIB, 4);
    let total: u64 = g.large().iter().map(|p| fs::metadata(p).unwrap().len()).sum();
    assert!(total >= 5 << 30, "{total}");
    std::thread::sleep(SETTLE);
    let w = g.capture();
    let (mut fastest, mut slowest) = (Duration::MAX, Duration::ZERO);
    for _ in 0..5 {
        let started = Instant::now();
        w.check(&g.dir).unwrap();
        fastest = fastest.min(started.elapsed());
        slowest = slowest.max(started.elapsed());
    }
    eprintln!("checks of a {} GiB set: fastest {fastest:?}, slowest {slowest:?} of 5", total >> 30);
    // every platform: a check that read 5 GiB could never finish in 50 ms
    assert!(fastest < Duration::from_millis(50), "fastest check {fastest:?}");
    // natively all five do. Under Wine every metadata call is a round trip to wineserver (10-30 ms a check), so its
    // slowest gets room for a busy host; Windows' byte-free check is proven by the held-without-sharing tests anyway.
    let limit = Duration::from_millis(if under_wine() { 250 } else { 50 });
    assert!(slowest < limit, "slowest check {slowest:?}");
}

/// Wine's ntdll exports wine_get_version; Windows' does not.
#[cfg(windows)]
fn under_wine() -> bool {
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetModuleHandleW(name: *const u16) -> *mut std::ffi::c_void;
        fn GetProcAddress(module: *mut std::ffi::c_void, name: *const u8) -> *mut std::ffi::c_void;
    }
    let ntdll: Vec<u16> = "ntdll.dll".encode_utf16().chain(Some(0)).collect();
    // SAFETY: a NUL-terminated module name and symbol name; ntdll is loaded in every process.
    unsafe {
        let module = GetModuleHandleW(ntdll.as_ptr());
        !module.is_null() && !GetProcAddress(module, c"wine_get_version".as_ptr().cast()).is_null()
    }
}
#[cfg(not(windows))]
fn under_wine() -> bool {
    false
}

/// On Windows a check opens a large file for its attributes only: another program holding it with no sharing at all
/// does not stop the check (a data open would fail with a sharing violation). Also proves the ChangeTime and file id
/// reach the stamp natively: without them the large files would be hashed, which these handles forbid.
#[cfg(windows)]
#[test]
fn a_large_file_held_without_sharing_is_still_checked() {
    use std::os::windows::fs::OpenOptionsExt;
    let g = Game::new("g13p-held", 8 * MIB, 8 * MIB, 2);
    std::thread::sleep(SETTLE);
    let w = g.capture();
    let held: Vec<File> = g.large().iter()
        .map(|p| OpenOptions::new().read(true).write(true).share_mode(0).open(p).unwrap())
        .collect();
    // control: a data open is refused now
    assert!(File::open(&g.large()[0]).is_err());
    w.check(&g.dir).unwrap();
    drop(held);
}

/// A replacement whose every time is put back, the change time too (on Windows a program can set it: a restore tool
/// that writes back each time it saved does): length, last-write and change time are the witnessed file's, and only
/// the file id tells the two apart. Natively the test proves that precondition; Wine cannot set a change time (it is
/// the host's ctime, which the rename moved), so there it only asserts the refusal.
#[cfg(windows)]
#[test]
fn a_swap_with_every_time_put_back_is_caught_by_the_file_id() {
    let g = Game::new("g13p-idswap", 8 * MIB, 8 * MIB, 2);
    let mut twins = Vec::new();
    for path in g.large() {
        let twin = path.with_extension("twin");
        fs::copy(&path, &twin).unwrap();
        twins.push((path, twin));
    }
    std::thread::sleep(SETTLE);
    for (path, twin) in twins {
        let w = g.capture();
        let (times, id) = (win::times(&path), win::file_id(&path));
        fs::rename(&twin, &path).unwrap();
        let put_back = win::set_times(&path, times);
        if !under_wine() {
            assert!(put_back, "set the times: {}", std::io::Error::last_os_error());
            assert_eq!(win::times(&path), times, "precondition: last-write and change time put back");
            assert_ne!(win::file_id(&path), id, "precondition: another file");
        }
        assert_eq!(fs::metadata(&path).unwrap().len(), 8 * MIB, "precondition: the same length");
        changed(w.check(&g.dir).unwrap_err(), &path);
    }
}

/// FILE_BASIC_INFO's last-write and change time, and FILE_ID_INFO, through handles that open no data.
#[cfg(windows)]
mod win {
    use std::{fs::{File, OpenOptions}, os::windows::{fs::OpenOptionsExt, io::AsRawHandle}, path::Path};

    #[repr(C)]
    #[derive(Default)]
    struct Basic { created: i64, accessed: i64, written: i64, changed: i64, attributes: u32 }
    #[repr(C)]
    #[derive(Default)]
    struct Id { volume: u64, file: [u8; 16] }

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetFileInformationByHandleEx(file: *mut std::ffi::c_void, class: i32, info: *mut std::ffi::c_void, size: u32) -> i32;
        fn SetFileInformationByHandle(file: *mut std::ffi::c_void, class: i32, info: *const std::ffi::c_void, size: u32) -> i32;
    }

    fn open(path: &Path, access: u32) -> File {
        // every sharing mode
        OpenOptions::new().access_mode(access).share_mode(0x1 | 0x2 | 0x4).open(path).unwrap()
    }
    /// (last-write, change) time, 100 ns units.
    pub fn times(path: &Path) -> (i64, i64) {
        let (file, mut basic) = (open(path, 0x80), Basic::default());
        // SAFETY: FILE_BASIC_INFO (class 0) with its own size; the handle is open for the call.
        let ok = unsafe {
            GetFileInformationByHandleEx(file.as_raw_handle(), 0, (&raw mut basic).cast(), size_of::<Basic>() as u32)
        };
        assert_ne!(ok, 0, "{}", std::io::Error::last_os_error());
        (basic.written, basic.changed)
    }
    pub fn file_id(path: &Path) -> (u64, [u8; 16]) {
        let (file, mut id) = (open(path, 0x80), Id::default());
        // SAFETY: FILE_ID_INFO (class 18) with its own size; the handle is open for the call.
        let ok = unsafe {
            GetFileInformationByHandleEx(file.as_raw_handle(), 18, (&raw mut id).cast(), size_of::<Id>() as u32)
        };
        assert_ne!(ok, 0, "{}", std::io::Error::last_os_error());
        (id.volume, id.file)
    }
    /// Sets the last-write and change time (0 leaves the creation and access time); whether the call succeeded.
    pub fn set_times(path: &Path, (written, changed): (i64, i64)) -> bool {
        let (file, basic) = (open(path, 0x100), Basic { written, changed, ..Basic::default() });
        // SAFETY: FILE_BASIC_INFO (class 0) with its own size, from a FILE_WRITE_ATTRIBUTES handle open for the call.
        unsafe { SetFileInformationByHandle(file.as_raw_handle(), 0, (&raw const basic).cast(), size_of::<Basic>() as u32) != 0 }
    }
}

/// The archive fixture with its working baseline and the held install lock (as m6_startup_vm).
struct Setup {
    f: ArchiveFixture,
    guard: File,
}
impl Setup {
    fn new(tag: &str) -> Self {
        let f = ArchiveFixture::new(tag);
        for ext in ["kfc", "kfc_resources"] {
            fs::copy(f.game.join(format!("{STEM}.{ext}")), f.game.join(format!("{STEM}.{ext}.bak"))).unwrap();
        }
        let guard = File::create(f.game.join(".kiln-eml.lock")).unwrap();
        guard.try_lock().unwrap();
        Self { f, guard }
    }
    fn base(&self) -> Rc<KFCFile> {
        Rc::new(KFCFile::from_path(self.f.game.join(format!("{STEM}.kfc.bak")), false).unwrap())
    }
    fn game(&self) -> &Path {
        self.f.game.as_std_path()
    }
    fn witness(&self) -> InputWitness {
        InputWitness::capture(self.game(), STEM, self.base().containers().len()).unwrap()
    }
    fn open(&self, w: &InputWitness) -> anyhow::Result<StartupHost<'_>> {
        StartupHost::open_matching(self.game(), STEM, self.f.types.clone(), self.base(), &self.guard, w)
    }
}

#[test]
fn open_matching_binds_the_witness_and_refuses_a_changed_input() {
    let s = Setup::new("g13p-open");
    std::thread::sleep(SETTLE);
    let w = s.witness();
    let host = s.open(&w).unwrap();
    assert_eq!(host.input_witness(), &w);
    assert_eq!(&host.baseline_identity(), w.baseline());
    let plain = StartupHost::open(s.game(), STEM, s.f.types.clone(), s.base(), &s.guard).unwrap();
    assert_eq!(plain.baseline_identity(), host.baseline_identity(), "the same baseline as a host that captures itself");
    drop(plain);
    // its stage checks the same proofs
    let mut host = host;
    drop(host.begin_stage().unwrap());
    drop(host);

    // a container rewritten in place, same length and last-write time
    rewrite_in_place(&s.game().join(format!("{STEM}_000.dat")));
    let error = s.open(&w).err().expect("a changed container is refused").to_string();
    assert!(error.contains("changed since they were witnessed") && error.contains("_000.dat"), "{error}");
    std::thread::sleep(SETTLE);
    let w = s.witness();
    s.open(&w).unwrap();
    // the resource backup swapped for a copy of itself
    let backup = s.game().join(format!("{STEM}.kfc_resources.bak"));
    let twin = backup.with_extension("twin");
    fs::copy(&backup, &twin).unwrap();
    File::options().write(true).open(&twin).unwrap().set_modified(fs::metadata(&backup).unwrap().modified().unwrap()).unwrap();
    fs::rename(&twin, &backup).unwrap();
    let error = s.open(&w).err().expect("a swapped backup is refused").to_string();
    assert!(error.contains("kfc_resources.bak"), "{error}");
}

#[test]
fn open_matching_refuses_a_witness_of_another_archive() {
    let s = Setup::new("g13p-other");
    let containers = s.base().containers().len();
    let fewer = InputWitness::capture(s.game(), STEM, containers - 1).unwrap();
    let error = s.open(&fewer).err().unwrap().to_string();
    assert!(error.contains("another archive"), "{error}");
    fs::copy(s.game().join(format!("{STEM}.kfc")), s.game().join("other.kfc")).unwrap();
    let other = InputWitness::capture(s.game(), "other", containers).unwrap();
    assert!(s.open(&other).err().unwrap().to_string().contains("another archive"));
    let elsewhere = Setup::new("g13p-elsewhere");
    let theirs = elsewhere.witness();
    assert!(s.open(&theirs).err().unwrap().to_string().contains("another archive"));
    s.open(&s.witness()).unwrap();
}

// A published or cached generation's own containers (the live header's past the working
// baseline's) are witnessed as the baseline's are, so a package DLL that rewrites one in place during a kiln start, same
// length and last-write time, is seen by the check; the baseline's container count still names the witness.
#[test]
fn a_generations_added_container_rewritten_in_place_is_caught() {
    let live = ArchiveFixture::new("g14d-added");
    // the working baseline: the same archive with one blob fewer, so the live header has a container of its own
    let types = live.types.clone();
    let root = types.get_by_name(kfc::reflection::LookupKey::Qualified(fixture::types::ROOT)).unwrap();
    let bytes = fixture::types::root_value().to_bytes(&types, root).unwrap();
    let resources: Vec<_> = [9, 0, 3].into_iter().map(|part| (fixture::archive::resource_id(part), bytes.clone())).collect();
    let base = ArchiveFixture::with_contents("g14d-added-base", types, &resources, &fixture::archive::BLOBS[..2]);
    let game = live.game.as_std_path();
    for ext in ["kfc", "kfc_resources"] {
        fs::copy(base.game.join(format!("{STEM}.{ext}")), game.join(format!("{STEM}.{ext}.bak"))).unwrap();
    }
    let (baseline, generation) = (base.index().containers().len(), live.index().containers().len());
    let added: Vec<PathBuf> = (baseline..generation)
        .map(|i| game.join(format!("{STEM}_{i:03}.dat")))
        .filter(|p| fs::metadata(p).unwrap().len() >= 16)
        .collect();
    assert!(!added.is_empty(), "the live header adds a container: {baseline} -> {generation}");
    std::thread::sleep(SETTLE);
    for path in &added {
        let w = InputWitness::capture(game, STEM, baseline).unwrap();
        assert_eq!(w.containers(), baseline);
        assert!(w.covers(game, STEM, baseline));
        w.check(game).unwrap();
        rewrite_in_place(path);
        changed(w.check(game).unwrap_err(), path);
        std::thread::sleep(SETTLE);
    }
    // a check of the stamps reads none of their bytes either
    #[cfg(target_os = "linux")]
    {
        let w = InputWitness::capture(game, STEM, baseline).unwrap();
        let header = fs::metadata(game.join(format!("{STEM}.kfc"))).unwrap().len()
            + ["kfc.bak", "kfc.bak.integrity"]
                .iter()
                .map(|e| fs::metadata(game.join(format!("{STEM}.{e}"))).map_or(0, |m| m.len()))
                .sum::<u64>();
        let before = thread_rchar();
        w.check(game).unwrap();
        assert!(thread_rchar() - before <= header + 64 * 1024, "a check read a container");
    }
}
