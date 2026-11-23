//! Fixed-path, size-bounded registry report I/O. Kept std-only for inexpensive host tests.
use std::{fs, io::{self, Read}, path::Path};

pub const MAX_REPORT_BYTES: u64 = 8 * 1024 * 1024;
// The native writer caps rows, not bytes, so a very large mod set can pass this: say where the whole file is.
const TOO_LARGE: &str = "Registry report exceeds the 8 MiB limit this view reads. Open kiln/state/registry.json in the game folder to read it in full.";

pub fn read_report(game: &Path) -> io::Result<Option<String>> {
    let root = game.canonicalize()?;
    let path = root.join("kiln/state/registry.json");
    let meta = match fs::symlink_metadata(&path) {
        Ok(meta) => meta,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(e),
    };
    if !meta.is_file() {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "Registry report is not a regular file."));
    }
    let canonical = path.canonicalize()?;
    if !canonical.starts_with(&root) {
        return Err(io::Error::new(io::ErrorKind::PermissionDenied, "Registry report is outside the game folder."));
    }
    let file = fs::File::open(canonical)?;
    let meta = file.metadata()?;
    if !meta.is_file() {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "Registry report is not a regular file."));
    }
    if meta.len() > MAX_REPORT_BYTES {
        return Err(io::Error::new(io::ErrorKind::InvalidData, TOO_LARGE));
    }
    // Bound the actual read as well: the producer may replace or grow the file after metadata was read.
    let mut bytes = Vec::new();
    file.take(MAX_REPORT_BYTES + 1).read_to_end(&mut bytes)?;
    if bytes.len() as u64 > MAX_REPORT_BYTES {
        return Err(io::Error::new(io::ErrorKind::InvalidData, TOO_LARGE));
    }
    String::from_utf8(bytes).map(Some).map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "Registry report is not UTF-8."))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{fs, path::PathBuf};
    struct Root(PathBuf);
    impl Root {
        fn new(name: &str) -> Self {
            let root = std::env::var_os("KILN_REGISTRY_TEST_OUT").map(PathBuf::from).unwrap_or_else(std::env::temp_dir)
                .join(format!("registry-reader-{}-{name}", std::process::id()));
            fs::create_dir_all(root.join("kiln/state")).unwrap();
            Self(root)
        }
        fn file(&self) -> PathBuf { self.0.join("kiln/state/registry.json") }
    }
    impl Drop for Root { fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); } }
    #[test] fn missing_is_not_an_io_error() {
        let root = Root::new("missing");
        assert_eq!(read_report(&root.0).unwrap(), None);
    }
    #[test] fn reads_utf8_report_at_the_fixed_path() {
        let root = Root::new("valid");
        fs::write(root.file(), "{\"format\":1}").unwrap();
        assert_eq!(read_report(&root.0).unwrap().as_deref(), Some("{\"format\":1}"));
    }
    #[test] fn caps_before_reading_large_files() {
        let root = Root::new("huge");
        fs::File::create(root.file()).unwrap().set_len(MAX_REPORT_BYTES + 1).unwrap();
        let err = read_report(&root.0).unwrap_err().to_string();
        assert!(err.contains("8 MiB") && err.contains("kiln/state/registry.json"), "{err}");
    }
    #[test] fn rejects_invalid_utf8() {
        let root = Root::new("utf8");
        fs::write(root.file(), [0xff, 0xfe]).unwrap();
        assert!(read_report(&root.0).is_err());
    }
    #[test] fn rejects_directory_instead_of_report() {
        let root = Root::new("directory");
        fs::create_dir(root.file()).unwrap();
        assert!(read_report(&root.0).is_err());
    }
    #[cfg(unix)]
    #[test] fn refuses_symlink_outside_game_folder() {
        let root = Root::new("link");
        std::os::unix::fs::symlink("/dev/zero", root.file()).unwrap();
        assert!(read_report(&root.0).is_err());
    }
}
