//! Ordered durability for the records Kiln recovers from (kept versions' `kiln-kept.json`, a profile's `meta.json`,
//! the placement journal): a record is written only after what it names is on disk (file contents and the directory
//! entries that name them), so a power cut never leaves a record pointing at something the disk lost. Every flush and
//! every durable record goes through this one seam; tests record their order ([`recording`]).
//!
//! Unix: a file is flushed with `fsync`, a folder's entries with an `fsync` of the folder. Windows has no folder flush
//! of that kind: a rename is made durable by `MoveFileExW` with `MOVEFILE_WRITE_THROUGH` (files:
//! [`crate::util::write_atomic_durable_exclusive`]; folders: [`rename_dir`]), and [`flush_dir`] only records.

use crate::util::io_at;
use crate::Result;
use std::path::{Path, PathBuf};

/// One step of [`recording`], in the order it happened.
#[cfg(test)]
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum Event {
    /// A file's contents were flushed.
    File(PathBuf),
    /// A folder's entries were flushed.
    Dir(PathBuf),
    /// A durable record was written: what it names (folders it depends on), its pending changes and restores.
    Journal { path: PathBuf, names: Vec<PathBuf>, pending: usize, restores: usize },
}

#[cfg(test)]
thread_local! {
    static RECORD: std::cell::RefCell<Option<Vec<Event>>> = const { std::cell::RefCell::new(None) };
}

/// Runs `f` and returns what it flushed and wrote durably on this thread, in order.
#[cfg(test)]
pub(crate) fn recording<R>(f: impl FnOnce() -> R) -> (R, Vec<Event>) {
    RECORD.with(|r| *r.borrow_mut() = Some(Vec::new()));
    let out = f();
    let events = RECORD.with(|r| r.borrow_mut().take()).unwrap_or_default();
    (out, events)
}

#[cfg(test)]
fn note(e: Event) {
    RECORD.with(|r| if let Some(v) = r.borrow_mut().as_mut() { v.push(e) });
}

/// Flushes a file's contents.
pub(crate) fn flush_file(path: &Path) -> Result<()> {
    std::fs::File::open(path).and_then(|f| f.sync_all()).map_err(io_at(path, "flushing"))?;
    #[cfg(test)]
    note(Event::File(path.to_path_buf()));
    Ok(())
}

/// Flushes a folder's entries (the names in it). Windows: nothing to do here (see the module's doc).
pub(crate) fn flush_dir(path: &Path) -> Result<()> {
    #[cfg(unix)]
    std::fs::File::open(path).and_then(|d| d.sync_all()).map_err(io_at(path, "flushing"))?;
    #[cfg(not(unix))]
    let _ = path;
    #[cfg(test)]
    note(Event::Dir(path.to_path_buf()));
    Ok(())
}

/// Flushes every folder of the tree at `root`, the deepest first, then `root` itself: what a new folder's name will
/// point to is on disk before that name is.
pub(crate) fn flush_tree(root: &Path) -> Result<()> {
    let mut dirs = Vec::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(d) = stack.pop() {
        for e in std::fs::read_dir(&d).map_err(io_at(&d, "reading"))? {
            let e = e?;
            let meta = std::fs::symlink_metadata(e.path())?;
            if meta.is_dir() && !crate::quick::import_is_link(&meta) { stack.push(e.path()); }
        }
        dirs.push(d);
    }
    for d in dirs.iter().rev() {
        flush_dir(d)?;
    }
    Ok(())
}

/// Renames a folder durably: Unix `rename` and a flush of the folder that holds the new name; Windows `MoveFileExW`
/// with `MOVEFILE_WRITE_THROUGH`.
pub(crate) fn rename_dir(from: &Path, to: &Path) -> Result<()> {
    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStrExt;
        use windows_sys::Win32::Storage::FileSystem::{MoveFileExW, MOVEFILE_WRITE_THROUGH};
        let wide = |p: &Path| -> Vec<u16> { p.as_os_str().encode_wide().chain(std::iter::once(0)).collect() };
        let (src, dst) = (wide(from), wide(to));
        // SAFETY: both are NUL-terminated UTF-16 strings that live until the call returns.
        if unsafe { MoveFileExW(src.as_ptr(), dst.as_ptr(), MOVEFILE_WRITE_THROUGH) } == 0 {
            return Err(io_at(to, "renaming to")(std::io::Error::last_os_error()));
        }
    }
    #[cfg(not(windows))]
    std::fs::rename(from, to).map_err(io_at(to, "renaming to"))?;
    if let Some(parent) = to.parent() { flush_dir(parent)?; }
    Ok(())
}

/// Replaces a file durably (a profile's `meta.json`): an exclusive temporary file, flushed, renamed over it, the
/// folder flushed ([`crate::util::write_atomic_durable_exclusive`]).
pub(crate) fn replace(path: &Path, data: &[u8]) -> Result<()> {
    crate::util::write_atomic_durable_exclusive(path, data)?;
    #[cfg(test)]
    {
        note(Event::File(path.to_path_buf()));
        if let Some(parent) = path.parent() { note(Event::Dir(parent.to_path_buf())); }
    }
    Ok(())
}

/// Writes a record durably ([`replace`]); `names`: the folders it depends on, `pending` and `restores`: what it holds
/// (for the order a test checks).
#[allow(unused_variables)]
pub(crate) fn journal(path: &Path, data: &[u8], names: &[PathBuf], pending: usize, restores: usize) -> Result<()> {
    crate::util::write_atomic_durable_exclusive(path, data)?;
    #[cfg(test)]
    note(Event::Journal { path: path.to_path_buf(), names: names.to_vec(), pending, restores });
    Ok(())
}
