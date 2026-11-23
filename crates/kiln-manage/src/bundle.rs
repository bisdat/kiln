//! Runtime bundles `kiln-runtime-<semver>+<yyyymmdd>.<gitsha>.zip` with `kiln-runtime.json` at the root
//! (schema/kiln-runtime.schema.json). Deterministic: entries sorted bytewise, times fixed to the commit time,
//! no extra attributes, deflate. Created by `kilnctl bundle create` from tools/package's staging folder.
//! [`read`], [`verify`], [`extract`] (so the installer) also take the bundle unpacked into a folder, `kiln-runtime.json`
//! at its root, as the Nexus Mods runtime file carries it: the same checks over both (`Source`).

use crate::manifest::RunSide;
use crate::task::Progress;
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

pub const MANIFEST: &str = "kiln-runtime.json";

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RuntimeManifest {
    pub format: u32,
    pub version: String,
    pub kiln: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub git: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub created: Option<String>,
    pub abi: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub api: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub dotnet: Option<String>,
    pub components: BTreeMap<String, RuntimeComponent>,
    pub files: Vec<RuntimeFile>,
    #[serde(default)]
    pub builds: RuntimeBuilds,
    /// The profile.json forms the runtime's core reads (native/core/src/profile.h KILN_PROFILE_READER, copied in by
    /// tools/package/stage.sh). None: a runtime from before the field, reader 1. profile::render writes a form only
    /// for a runtime that declares it.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub profile_reader: Option<u32>,
}

fn yes() -> bool {
    true
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RuntimeComponent {
    pub sides: Vec<RunSide>,
    #[serde(default = "yes")]
    pub default: bool,
    #[serde(default)]
    pub requires: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub description: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RuntimeFile {
    /// Relative to the game folder, '/' separated.
    pub path: String,
    pub size: u64,
    pub sha256: String,
    pub component: String,
    /// User-editable: installed only when missing, never replaced or deleted.
    #[serde(default)]
    pub config: bool,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RuntimeBuilds {
    #[serde(default)]
    pub server: Vec<RuntimeBuild>,
    #[serde(default)]
    pub client: Vec<RuntimeBuild>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RuntimeBuild {
    pub sha256: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub steam_build: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub svn: Option<u64>,
    #[serde(default = "yes")]
    pub verified: bool,
}

/// A bundle the launcher has (in `<data>/runtime/`), for the UI.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RuntimeBundle {
    pub version: String,
    pub kiln: String,
    pub file: PathBuf,
    pub sha256: String,
    pub created: Option<String>,
    pub sides: Vec<RunSide>,
    pub components: Vec<String>,
    /// Every component with its sides and whether it is a default (the UI shows installed versus available parts).
    #[serde(default)]
    pub parts: Vec<BundlePart>,
}

/// One component of a bundle, for the UI (`RuntimeBundle.parts`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BundlePart {
    pub name: String,
    pub sides: Vec<RunSide>,
    pub default: bool,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub requires: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub description: Option<String>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CreateOptions {
    /// Commit time (seconds) for entry times and `created`; SOURCE_DATE_EPOCH.
    pub source_date_epoch: i64,
    pub git: String,
}

/// The zip name of a bundle version: `kiln-runtime-<version>.zip`.
pub fn file_name(version: &str) -> String {
    format!("kiln-runtime-{version}.zip")
}

/// Every side some component of the bundle supports.
pub fn sides(m: &RuntimeManifest) -> Vec<RunSide> {
    let mut v: Vec<RunSide> = Vec::new();
    for c in m.components.values() {
        for s in &c.sides {
            if !v.contains(s) {
                v.push(*s);
            }
        }
    }
    v.sort_by_key(|s| match s {
        RunSide::Server => 0,
        RunSide::Client => 1,
    });
    v
}

/// Windows drops trailing dots and spaces from a file name: `".. "` and `"..."` open the parent folder, `"a."` and
/// `"a "` open `a`. A component like that names a different file there than it does in the archive.
pub fn windows_would_rename(component: &str) -> bool {
    component.ends_with(['.', ' '])
}

/// A zip entry name or manifest path that is safe to join to a folder (zip-slip guard): relative, '/' separated, no
/// empty, `.` or `..` component, no component that ends in a dot or a space (Windows normalises those to another
/// name, `".. "` to `..`), no backslash, drive letter, NUL or control character. Returns it unchanged.
pub fn safe_rel_path(name: &str) -> Result<&str> {
    let bad = |why: &str| Err(Error::new(ErrorCode::Refused, format!("unsafe path {name:?} in the bundle: {why}")));
    if name.is_empty() {
        return bad("empty");
    }
    if name.starts_with('/') {
        return bad("absolute");
    }
    if name.contains('\\') {
        return bad("backslash");
    }
    if name.chars().any(|c| (c as u32) < 32 || c == ':') {
        return bad("drive letter, stream name or control character");
    }
    for part in name.split('/') {
        if part.is_empty() || part == "." || part == ".." {
            return bad("empty, . or .. component");
        }
        if windows_would_rename(part) {
            return bad("a name ending in a dot or a space (Windows would turn it into another name)");
        }
    }
    Ok(name)
}

/// Checks the manifest's own rules: format, paths, components of files, sorted unique paths.
pub fn validate(m: &RuntimeManifest) -> Result<()> {
    if m.format != 1 {
        return Err(Error::new(ErrorCode::Unsupported, format!("kiln-runtime.json format {} (this Kiln reads format 1)", m.format)));
    }
    let mut problems = Vec::new();
    for (i, f) in m.files.iter().enumerate() {
        if let Err(e) = safe_rel_path(&f.path) {
            problems.push(e.message);
        }
        if !m.components.contains_key(&f.component) {
            problems.push(format!("{}: unknown component {}", f.path, f.component));
        }
        if f.sha256.len() != 64 || !f.sha256.bytes().all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase()) {
            problems.push(format!("{}: bad sha256", f.path));
        }
        if i > 0 && m.files[i - 1].path.as_bytes() >= f.path.as_bytes() {
            problems.push(format!("{}: files are not sorted or not unique", f.path));
        }
        if f.path == MANIFEST {
            problems.push(format!("{MANIFEST} cannot be a runtime file"));
        }
    }
    // Windows (and macOS) folders do not tell names apart by case: two paths that differ only in it, or a file that is
    // a folder of another path, would be one name there (the second written over the first, the receipt never
    // matching what is on disk), from the zip or the folder alike
    let mut folded: BTreeMap<String, &str> = BTreeMap::new();
    for f in &m.files {
        if let Some(other) = folded.insert(f.path.to_lowercase(), &f.path)
            && other != f.path
        {
            problems.push(format!("{}: the same name as {other} where case does not count (Windows)", f.path));
        }
    }
    for f in &m.files {
        if let Some(file) = f.path.match_indices('/').find_map(|(i, _)| folded.get(&f.path[..i].to_lowercase())) {
            problems.push(format!("{}: one of its folders is the file {file}", f.path));
        }
    }
    for (name, c) in &m.components {
        for r in &c.requires {
            if !m.components.contains_key(r) {
                problems.push(format!("component {name} requires unknown component {r}"));
            }
        }
        if c.sides.is_empty() {
            problems.push(format!("component {name} has no sides"));
        }
    }
    if !problems.is_empty() {
        return Err(Error::new(ErrorCode::Schema, format!("kiln-runtime.json breaks the format's rules: {}", problems.join("; ")))
            .with_detail(problems));
    }
    Ok(())
}

// (kiln-eml-inspect, kiln-eml's archive inspector: the one runtime file installed executable)
/// The one runtime file that is packed and installed executable: kiln-eml's archive inspector for Linux (component
/// eml, GPL-3), which kiln-manage starts before a start of Kiln's interpreter (crate::sfi_inspection).
/// Every other file is packed 0644 and installed with the system's default mode, whatever mode a zip entry claims (a
/// zip's own modes are never read). Windows has no modes: its helper is the .exe beside it.
pub const EXECUTABLE_HELPER: &str = "kiln/tools/eml-inspect/linux-x86_64/kiln-eml-inspect";
/// The component that brings it.
pub const EXECUTABLE_HELPER_COMPONENT: &str = "eml";

/// Is the runtime file `path` of `component` the executable helper? Exactly that manifest path (already
/// [`safe_rel_path`]: '/' separated, no `.`/`..`, case as written) in exactly that component.
pub fn executable(path: &str, component: &str) -> bool {
    path == EXECUTABLE_HELPER && component == EXECUTABLE_HELPER_COMPONENT
}

/// Gives the installed helper at `path` its execute bits back (0755) when they are missing, and only when it is still
/// the file the receipt lists with `sha256`: opened without following a link (and without blocking on a FIFO), a plain
/// file, its bytes read through that very handle, then chmod on that handle, so nothing swapped in at the path is ever
/// changed. Ok(false): it has them. Err: nothing changed (a link, not a plain file, other bytes, or chmod refused).
#[cfg(unix)]
pub fn restore_executable(path: &Path, sha256: &str) -> std::io::Result<bool> {
    use sha2::Digest as _;
    use std::io::Read as _;
    use std::os::unix::fs::{OpenOptionsExt as _, PermissionsExt as _};
    let mut f = std::fs::OpenOptions::new().read(true).custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK).open(path)?;
    let meta = f.metadata()?;
    if !meta.is_file() {
        return Err(std::io::Error::other("not a plain file"));
    }
    if meta.permissions().mode() & 0o111 == 0o111 {
        return Ok(false);
    }
    let mut h = sha2::Sha256::new();
    let mut buf = vec![0u8; 1 << 16];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    if crate::hash::hex(h.finalize().as_slice()) != sha256 {
        return Err(std::io::Error::other("its bytes are not the installed helper's"));
    }
    f.set_permissions(std::fs::Permissions::from_mode(0o755))?;
    Ok(true)
}

/// What an entry of a bundle is: a zip says it of each entry, a folder of each name below it (never following a link).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Kind {
    File,
    Dir,
    Link,
    /// A FIFO, socket or device (a folder only).
    Special,
}

/// A bundle's files, read one way from either form: its zip (`imp::ZipSource`, feature `zip`) or the same tree unpacked
/// in a folder ([`FolderSource`]). [`verify`] and [`extract`] run the same checks and the same checked copy over both.
trait Source {
    /// The zip or the folder, for messages.
    fn path(&self) -> &Path;
    /// What one of its names is called in a message: a zip's "entry", a folder's "file".
    fn noun(&self) -> &'static str;
    /// Every entry, its name '/' separated (a zip's folder entries may end in '/'), and what it is.
    fn entries(&mut self) -> Result<Vec<(String, Kind)>>;
    /// The bytes of one plain file named by a [`safe_rel_path`]: never through a link, never a special file. A name it
    /// does not have is `Schema`.
    fn open(&mut self, name: &str) -> Result<Box<dyn Read + '_>>;
}

fn missing(at: &Path, name: &str) -> Error {
    Error::new(ErrorCode::Schema, format!("{}: {name} is listed but not in the bundle", at.display()))
}

fn link_refused(at: &Path, name: &str) -> Error {
    Error::new(ErrorCode::Refused, format!("{}: {name} is a link (a bundle folder holds plain files and folders only)", at.display()))
}

/// A bundle unpacked into a folder: `kiln-runtime.json` at its root and every file at its manifest path, as unzipping the
/// bundle gives it. The Nexus Mods runtime file carries the bundle this way (`kiln-runtime-<version>/`): Nexus cannot
/// scan an archive inside an archive. Its file modes are never read, as a zip's are not: the executable helper gets its
/// mode from [`executable`] when it is staged.
struct FolderSource {
    root: PathBuf,
}

impl FolderSource {
    fn new(root: &Path) -> Result<FolderSource> {
        match std::fs::symlink_metadata(root.join(MANIFEST)) {
            Ok(_) => Ok(FolderSource { root: root.to_path_buf() }),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Err(Error::new(
                ErrorCode::Schema,
                format!("{}: no {MANIFEST} at the folder's root: not a runtime bundle (its zip, or the folder it was unpacked to)", root.display()),
            )),
            Err(e) => Err(crate::util::io_at(&root.join(MANIFEST), "reading")(e)),
        }
    }

    fn walk(&self, dir: &Path, out: &mut Vec<(String, Kind)>) -> Result<()> {
        let mut entries: Vec<_> =
            std::fs::read_dir(dir).map_err(crate::util::io_at(dir, "reading"))?.collect::<std::io::Result<Vec<_>>>().map_err(crate::util::io_at(dir, "reading"))?;
        entries.sort_by_key(|e| e.file_name());
        for e in entries {
            let p = e.path();
            let rel = p
                .strip_prefix(&self.root)
                .expect("below the root")
                .components()
                .map(|c| c.as_os_str().to_str().map(str::to_string))
                .collect::<Option<Vec<_>>>()
                .ok_or_else(|| Error::new(ErrorCode::Refused, format!("{}: a name that is not UTF-8 is no bundle file", p.display())))?
                .join("/");
            let ft = std::fs::symlink_metadata(&p).map_err(crate::util::io_at(&p, "reading"))?.file_type();
            let kind = if ft.is_symlink() {
                Kind::Link
            } else if ft.is_dir() {
                Kind::Dir
            } else if ft.is_file() {
                Kind::File
            } else {
                Kind::Special
            };
            out.push((rel, kind));
            if kind == Kind::Dir {
                self.walk(&p, out)?;
            }
        }
        Ok(())
    }
}

impl Source for FolderSource {
    fn path(&self) -> &Path {
        &self.root
    }

    fn noun(&self) -> &'static str {
        "file"
    }

    fn entries(&mut self) -> Result<Vec<(String, Kind)>> {
        let mut out = Vec::new();
        self.walk(&self.root, &mut out)?;
        Ok(out)
    }

    fn open(&mut self, name: &str) -> Result<Box<dyn Read + '_>> {
        let rel = safe_rel_path(name)?;
        let parts: Vec<&str> = rel.split('/').collect();
        // the folders on the way are folders, not links (also one swapped in after the walk)
        let mut path = self.root.clone();
        for (i, part) in parts[..parts.len() - 1].iter().enumerate() {
            path.push(part);
            match std::fs::symlink_metadata(&path) {
                Ok(m) if m.file_type().is_symlink() => return Err(link_refused(&self.root, &parts[..=i].join("/"))),
                Ok(m) if m.is_dir() => {}
                Ok(_) => return Err(missing(&self.root, name)),
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Err(missing(&self.root, name)),
                Err(e) => return Err(crate::util::io_at(&path, "reading")(e)),
            }
        }
        path.push(parts[parts.len() - 1]);
        // the file itself: never followed if it is a link, never blocking on a FIFO (whose open waits for a writer)
        let mut o = std::fs::OpenOptions::new();
        o.read(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt as _;
            o.custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK);
        }
        #[cfg(windows)]
        {
            use std::os::windows::fs::OpenOptionsExt as _;
            o.custom_flags(0x0020_0000); // FILE_FLAG_OPEN_REPARSE_POINT: a link itself, never its target
        }
        let f = match o.open(&path) {
            Ok(f) => f,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Err(missing(&self.root, name)),
            Err(_) if std::fs::symlink_metadata(&path).is_ok_and(|m| m.file_type().is_symlink()) => return Err(link_refused(&self.root, name)),
            Err(e) => return Err(crate::util::io_at(&path, "opening")(e)),
        };
        let m = f.metadata().map_err(crate::util::io_at(&path, "reading"))?;
        if m.file_type().is_symlink() {
            return Err(link_refused(&self.root, name));
        }
        if !m.is_file() {
            return Err(Error::new(ErrorCode::Refused, format!("{}: {name} is not a plain file", self.root.display())));
        }
        Ok(Box::new(f))
    }
}

/// Is `bundle` a folder (an unpacked bundle) rather than a zip? Only the path itself is followed when it is a link.
pub fn is_folder(bundle: &Path) -> bool {
    std::fs::metadata(bundle).is_ok_and(|m| m.is_dir())
}

#[cfg(not(feature = "zip"))]
fn no_zip() -> Error {
    Error::new(ErrorCode::Unsupported, "kiln-manage was built without the zip feature")
}

fn open_source(bundle: &Path) -> Result<Box<dyn Source>> {
    if is_folder(bundle) {
        return Ok(Box::new(FolderSource::new(bundle)?));
    }
    #[cfg(feature = "zip")]
    {
        Ok(Box::new(imp::ZipSource::open(bundle)?))
    }
    #[cfg(not(feature = "zip"))]
    {
        Err(no_zip())
    }
}

fn read_manifest(src: &mut dyn Source) -> Result<RuntimeManifest> {
    let at = src.path().to_path_buf();
    let mut text = Vec::new();
    src.open(MANIFEST)?.read_to_end(&mut text).map_err(crate::util::io_at(&at, "reading"))?;
    let m: RuntimeManifest =
        serde_json::from_slice(&text).map_err(|e| Error::new(ErrorCode::Parse, format!("{}: {MANIFEST}: {e}", at.display())))?;
    validate(&m)?;
    Ok(m)
}

/// Files a system or a file manager leaves in folders and zips made of them: Finder's `.DS_Store` and `._*` files and
/// the `__MACOSX/` folder of its zips, Explorer's `Thumbs.db`, `ehthumbs.db` and `desktop.ini`. A bundle refuses them
/// like any other file its manifest does not list (no name is let through); the message only says what they are.
fn os_leftover(name: &str) -> bool {
    let base = name.rsplit('/').next().unwrap_or(name);
    name.starts_with("__MACOSX/")
        || base.starts_with("._")
        || [".DS_Store", "Thumbs.db", "ehthumbs.db", "desktop.ini"].iter().any(|n| base.eq_ignore_ascii_case(n))
}

/// Every entry is safe (zip-slip guard), no link or special file, listed in the manifest (or the manifest itself) and
/// there once, and every listed file is there.
fn check_entries(src: &mut dyn Source, m: &RuntimeManifest) -> Result<()> {
    let at = src.path().to_path_buf();
    let noun = src.noun();
    let wanted: std::collections::BTreeSet<&str> = m.files.iter().map(|f| f.path.as_str()).collect();
    let mut seen = std::collections::BTreeSet::new();
    for (name, kind) in src.entries()? {
        match kind {
            Kind::Link => return Err(Error::new(ErrorCode::Refused, format!("{}: symlink {noun} {name}", at.display()))),
            Kind::Special => return Err(Error::new(ErrorCode::Refused, format!("{}: {name} is not a plain file or folder", at.display()))),
            Kind::Dir => {
                safe_rel_path(name.trim_end_matches('/'))?;
                continue;
            }
            Kind::File => {}
        }
        safe_rel_path(&name)?;
        if name != MANIFEST && !wanted.contains(name.as_str()) {
            let what = if os_leftover(&name) {
                " (the system or a file manager adds such files to folders: delete it; a bundle holds only its own files)"
            } else {
                ""
            };
            return Err(Error::new(ErrorCode::Schema, format!("{}: {noun} {name} is not in {MANIFEST}{what}", at.display())));
        }
        if !seen.insert(name.clone()) {
            return Err(Error::new(ErrorCode::Schema, format!("{}: {noun} {name} is there twice", at.display())));
        }
    }
    if let Some(missing) = wanted.iter().find(|p| !seen.contains(**p)) {
        return Err(Error::new(ErrorCode::Schema, format!("{}: {missing} is listed but not in the bundle", at.display())));
    }
    Ok(())
}

/// Streams one file into `out` (when given) while hashing it; checks size and sha256.
fn copy_checked(src: &mut dyn Source, f: &RuntimeFile, mut out: Option<&mut dyn Write>) -> Result<()> {
    use sha2::Digest as _;
    let at = src.path().to_path_buf();
    let mut e = src.open(&f.path)?;
    let mut h = sha2::Sha256::new();
    let mut buf = vec![0u8; 1 << 16];
    let mut size = 0u64;
    loop {
        let n = e.read(&mut buf).map_err(|err| Error::new(ErrorCode::Parse, format!("{}: {}: {err}", at.display(), f.path)))?;
        if n == 0 {
            break;
        }
        size += n as u64;
        if size > f.size {
            break;
        }
        h.update(&buf[..n]);
        if let Some(o) = out.as_mut() {
            // the staged copy of a DLL that Windows refuses (an antivirus): crate::blocked
            o.write_all(&buf[..n]).map_err(crate::blocked::io_at(Path::new(&f.path), "writing", &f.path))?;
        }
    }
    let sha = crate::hash::hex(h.finalize().as_slice());
    if size != f.size || sha != f.sha256 {
        return Err(Error::new(ErrorCode::HashMismatch, format!("{}: {} does not match {MANIFEST} (size {size}, sha256 {sha})", at.display(), f.path))
            .with_detail(serde_json::json!({ "path": f.path, "expectedSha256": f.sha256, "expectedSize": f.size })));
    }
    Ok(())
}

fn verify_source(src: &mut dyn Source, progress: &mut dyn Progress) -> Result<RuntimeManifest> {
    let m = read_manifest(src)?;
    check_entries(src, &m)?;
    let total: u64 = m.files.iter().map(|f| f.size).sum::<u64>().max(1);
    let mut done = 0u64;
    for f in &m.files {
        if progress.canceled() {
            return Err(Error::new(ErrorCode::Canceled, "canceled"));
        }
        copy_checked(src, f, None)?;
        done += f.size;
        progress.report(Some(done as f32 / total as f32), &format!("verified {}", f.path));
    }
    Ok(m)
}

fn extract_source(src: &mut dyn Source, files: &[RuntimeFile], dest: &Path, progress: &mut dyn Progress) -> Result<()> {
    let m = read_manifest(src)?;
    let total: u64 = files.iter().map(|f| f.size).sum::<u64>().max(1);
    let mut done = 0u64;
    for f in files {
        if progress.canceled() {
            return Err(Error::new(ErrorCode::Canceled, "canceled"));
        }
        let listed = m.files.iter().find(|x| x.path == f.path);
        if listed != Some(f) {
            return Err(Error::invalid(format!("{} is not a file of this bundle", f.path)));
        }
        let target = dest.join(safe_rel_path(&f.path)?);
        if !target.starts_with(dest) {
            return Err(Error::new(ErrorCode::Refused, format!("{} escapes the staging folder", f.path)));
        }
        if let Some(parent) = target.parent() {
            std::fs::create_dir_all(parent).map_err(crate::util::io_at(parent, "creating"))?;
        }
        // a DLL Windows refuses here is in the staging folder (an antivirus scans it as it is written): crate::blocked
        let mut out = std::fs::File::create(&target).map_err(crate::blocked::io_at(&target, "writing", &f.path))?;
        copy_checked(src, f, Some(&mut out))?;
        // (kiln-eml-inspect: the helper, its bytes and its manifest entry checked, gets 0755 on the
        // handle it was written through; a refusal is said and the install goes on: the helper cannot run there)
        #[cfg(unix)]
        if executable(&f.path, &f.component) {
            use std::os::unix::fs::PermissionsExt as _;
            let made = out.metadata().and_then(|m| {
                if m.is_file() { out.set_permissions(std::fs::Permissions::from_mode(0o755)) } else { Err(std::io::Error::other("not a plain file")) }
            });
            if let Err(e) = made {
                progress.report(None, &format!("{} could not be made executable ({e}): Kiln's archive inspector cannot run from this folder", f.path));
            }
        }
        out.sync_all().map_err(crate::blocked::io_at(&target, "flushing", &f.path))?;
        done += f.size;
        progress.report(Some(done as f32 / total as f32), &format!("staged {}", f.path));
    }
    Ok(())
}

#[cfg(feature = "zip")]
mod imp {
    use super::*;

    fn zip_err(zip: &Path) -> impl Fn(zip::result::ZipError) -> Error + '_ {
        move |e| Error::new(ErrorCode::Parse, format!("{}: {e}", zip.display()))
    }

    /// A bundle zip as a [`Source`]: its own modes are never read.
    pub struct ZipSource {
        a: zip::ZipArchive<std::fs::File>,
        path: PathBuf,
    }

    impl ZipSource {
        pub fn open(zip: &Path) -> Result<ZipSource> {
            let f = std::fs::File::open(zip).map_err(crate::util::io_at(zip, "opening"))?;
            Ok(ZipSource { a: zip::ZipArchive::new(f).map_err(zip_err(zip))?, path: zip.to_path_buf() })
        }
    }

    impl Source for ZipSource {
        fn path(&self) -> &Path {
            &self.path
        }

        fn noun(&self) -> &'static str {
            "entry"
        }

        fn entries(&mut self) -> Result<Vec<(String, Kind)>> {
            let mut out = Vec::with_capacity(self.a.len());
            for i in 0..self.a.len() {
                let e = self.a.by_index_raw(i).map_err(zip_err(&self.path))?;
                let name = e.name().map_err(zip_err(&self.path))?.to_string();
                let kind = if e.is_symlink() {
                    Kind::Link
                } else if e.is_dir() {
                    Kind::Dir
                } else {
                    Kind::File
                };
                out.push((name, kind));
            }
            Ok(out)
        }

        fn open(&mut self, name: &str) -> Result<Box<dyn Read + '_>> {
            match self.a.by_name(name) {
                Ok(e) => Ok(Box::new(e)),
                Err(zip::result::ZipError::FileNotFound) => Err(Error::new(ErrorCode::Schema, format!("{}: no {name}", self.path.display()))),
                Err(e) => Err(zip_err(&self.path)(e)),
            }
        }
    }

    fn zip_time(epoch: i64) -> zip::DateTime {
        let (y, mo, d, h, mi, s) = crate::util::utc_secs(epoch.max(315_532_800)); // 1980-01-01
        zip::DateTime::from_date_and_time(y.min(2107) as u16, mo as u8, d as u8, h as u8, mi as u8, s as u8)
            .unwrap_or_else(|_| zip::DateTime::default_for_write())
    }

    pub fn write_zip(out: &Path, entries: &[(String, Vec<u8>)], epoch: i64,
                     // (kiln-eml-inspect: the entries packed 0755, super::executable)
                     executable: &std::collections::BTreeSet<&str>,
    ) -> Result<()> {
        let tmp = crate::util::with_suffix(out, ".tmp");
        let f = std::fs::File::create(&tmp).map_err(crate::util::io_at(&tmp, "writing"))?;
        let mut w = zip::ZipWriter::new(std::io::BufWriter::new(f));
        let opts = zip::write::SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated)
            .compression_level(Some(6))
            .last_modified_time(zip_time(epoch))
            .unix_permissions(0o644)
            .system(zip::System::Unix)
            .large_file(false);
        let mut sorted: Vec<&(String, Vec<u8>)> = entries.iter().collect();
        sorted.sort_by(|a, b| a.0.as_bytes().cmp(b.0.as_bytes()));
        for (name, data) in sorted {
            let o = if data.len() as u64 >= 0xFFFF_0000 { opts.large_file(true) } else { opts };
            let o = if executable.contains(name.as_str()) { o.unix_permissions(0o755) } else { o };
            w.start_file(name.as_str(), o).map_err(zip_err(out))?;
            w.write_all(data)?;
        }
        let mut inner = w.finish().map_err(zip_err(out))?;
        inner.flush()?;
        inner.into_inner().map_err(|e| Error::from(e.into_error()))?.sync_all()?;
        crate::util::rename_replace(&tmp, out)
    }
}

/// Staging folder layout for [`create`]: `<stage>/kiln-runtime.json` = the manifest without `files` (plus
/// `"config": ["kiln/kiln.json", ...]`, the user-editable files) and one folder per component holding its files at
/// their game-relative paths: `<stage>/core/dinput8.dll`, `<stage>/core/kiln/kiln-core.dll`,
/// `<stage>/sigdb-server/kiln/sigdb/server.json`, ...
pub const STAGE_CONFIG_KEY: &str = "config";

fn walk_stage(root: &Path, dir: &Path, out: &mut Vec<(String, PathBuf)>) -> Result<()> {
    let mut entries: Vec<_> = std::fs::read_dir(dir).map_err(crate::util::io_at(dir, "reading"))?.collect::<std::io::Result<Vec<_>>>()?;
    entries.sort_by_key(|e| e.file_name());
    for e in entries {
        let p = e.path();
        let ft = std::fs::symlink_metadata(&p)?.file_type();
        if ft.is_symlink() {
            return Err(Error::new(ErrorCode::Refused, format!("{}: symlinks are not bundled", p.display())));
        }
        if ft.is_dir() {
            walk_stage(root, &p, out)?;
        } else if ft.is_file() {
            let rel = p.strip_prefix(root).expect("below root");
            let rel = rel
                .components()
                .map(|c| c.as_os_str().to_str().map(str::to_string))
                .collect::<Option<Vec<_>>>()
                .ok_or_else(|| Error::invalid(format!("{}: name is not UTF-8", p.display())))?
                .join("/");
            safe_rel_path(&rel)?;
            out.push((rel, p));
        }
    }
    Ok(())
}

/// The manifest `create` would write (no zip), for tests and `kilnctl bundle create --dry-run`.
pub fn manifest_for_stage(stage: &Path, options: &CreateOptions) -> Result<(RuntimeManifest, Vec<(String, PathBuf)>)> {
    let mpath = stage.join(MANIFEST);
    let mut v: serde_json::Value = crate::util::read_json(&mpath)?
        .ok_or_else(|| Error::not_found(format!("{}: no {MANIFEST} in the staging folder", stage.display())))?;
    let obj = v.as_object_mut().ok_or_else(|| Error::new(ErrorCode::Schema, format!("{}: not an object", mpath.display())))?;
    let config: Vec<String> = match obj.remove(STAGE_CONFIG_KEY) {
        Some(c) => serde_json::from_value(c)?,
        None => Vec::new(),
    };
    let kiln = obj.get("kiln").and_then(|k| k.as_str()).ok_or_else(|| Error::new(ErrorCode::Schema, "kiln-runtime.json: no kiln version"))?;
    if options.git.is_empty() || !options.git.bytes().all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase()) || !(7..=40).contains(&options.git.len()) {
        return Err(Error::invalid(format!("git {:?}: 7-40 lower-case hex chars", options.git)));
    }
    let (y, mo, d, ..) = crate::util::utc_secs(options.source_date_epoch);
    let version = format!("{kiln}+{y:04}{mo:02}{d:02}.{}", options.git);
    obj.insert("version".into(), version.into());
    obj.insert("git".into(), options.git.clone().into());
    obj.insert("created".into(), crate::util::rfc3339(options.source_date_epoch).into());
    obj.insert("files".into(), serde_json::Value::Array(Vec::new()));
    let mut m: RuntimeManifest = serde_json::from_value(v)
        .map_err(|e| Error::new(ErrorCode::Schema, format!("{}: {e}", mpath.display())))?;
    let mut files: Vec<(String, PathBuf)> = Vec::new();
    let mut entries: Vec<_> = std::fs::read_dir(stage).map_err(crate::util::io_at(stage, "reading"))?.collect::<std::io::Result<Vec<_>>>()?;
    entries.sort_by_key(|e| e.file_name());
    for e in entries {
        let name = e.file_name().to_string_lossy().into_owned();
        if name == MANIFEST {
            continue;
        }
        if !e.file_type()?.is_dir() {
            return Err(Error::new(ErrorCode::Schema, format!("{}: unexpected file {name} (components are folders)", stage.display())));
        }
        if !m.components.contains_key(&name) {
            return Err(Error::new(ErrorCode::Schema, format!("{}: folder {name} is not a component in {MANIFEST}", stage.display())));
        }
        let mut found = Vec::new();
        walk_stage(&e.path(), &e.path(), &mut found)?;
        for (rel, src) in found {
            let (sha256, size) = crate::hash::sha256_file(&src)?;
            m.files.push(RuntimeFile { config: config.contains(&rel), path: rel.clone(), size, sha256, component: name.clone() });
            files.push((rel, src));
        }
    }
    m.files.sort_by(|a, b| a.path.as_bytes().cmp(b.path.as_bytes()));
    if let Some(w) = m.files.windows(2).find(|w| w[0].path == w[1].path) {
        return Err(Error::new(ErrorCode::Conflict, format!("{} is in two components ({}, {})", w[0].path, w[0].component, w[1].component)));
    }
    for c in &config {
        if !m.files.iter().any(|f| &f.path == c) {
            return Err(Error::new(ErrorCode::Schema, format!("config file {c} is not in any component")));
        }
    }
    validate(&m)?;
    Ok((m, files))
}

/// Reads `kiln-runtime.json` from a bundle: its zip (feature `zip`) or the folder it was unpacked to.
pub fn read(bundle: &Path) -> Result<RuntimeManifest> {
    if is_folder(bundle) {
        return read_manifest(&mut FolderSource::new(bundle)?);
    }
    // `list` reads every bundle's manifest on each status poll: parsed once per zip while it is unchanged
    static MANIFESTS: std::sync::LazyLock<crate::filecache::Memo<std::sync::Arc<RuntimeManifest>>> =
        std::sync::LazyLock::new(|| crate::filecache::Memo::new(64));
    MANIFESTS.get(bundle, |z| read_manifest(&mut *open_source(z)?).map(std::sync::Arc::new)).map(|m| (*m).clone())
}

/// Reads the manifest and checks the bundle against it: every listed file there with its size and sha256, nothing
/// else in it (but the manifest), no unsafe name, link or special file. A zip (feature `zip`) and an unpacked folder
/// (`kiln-runtime.json` at its root) are held to the same rules.
pub fn verify(bundle: &Path, progress: &mut dyn Progress) -> Result<RuntimeManifest> {
    verify_source(&mut *open_source(bundle)?, progress)
}

/// Unpacks `files` (entries of the bundle's manifest) below `dest`, checking each one's size and sha256 while it is
/// written (zip-slip guarded), from the zip or the unpacked folder. The installer stages with it.
pub fn extract(bundle: &Path, files: &[RuntimeFile], dest: &Path, progress: &mut dyn Progress) -> Result<()> {
    extract_source(&mut *open_source(bundle)?, files, dest, progress)
}

/// The sha256 of a bundle zip, which an install receipt records (`runtime.sha256`); `None` for an unpacked folder,
/// which has no zip to name (its `kiln-runtime.json` still pins every file's sha256).
pub fn zip_sha256(bundle: &Path) -> Result<Option<String>> {
    if is_folder(bundle) {
        return Ok(None);
    }
    Ok(Some(crate::hash::sha256_file(bundle)?.0))
}

/// Builds a deterministic bundle from a staging folder (game-relative layout + kiln-runtime.json without files[]).
/// Returns the zip's path in `out_dir`.
pub fn create(stage: &Path, out_dir: &Path, options: &CreateOptions) -> Result<PathBuf> {
    let (m, files) = manifest_for_stage(stage, options)?;
    #[cfg(feature = "zip")]
    {
        let mut entries: Vec<(String, Vec<u8>)> = Vec::with_capacity(files.len() + 1);
        let mut text = serde_json::to_string_pretty(&m)?;
        text.push('\n');
        entries.push((MANIFEST.to_string(), text.into_bytes()));
        for (rel, src) in files {
            entries.push((rel, std::fs::read(&src).map_err(crate::util::io_at(&src, "reading"))?));
        }
        std::fs::create_dir_all(out_dir).map_err(crate::util::io_at(out_dir, "creating"))?;
        let out = out_dir.join(file_name(&m.version));
        let exec: std::collections::BTreeSet<&str> =
            m.files.iter().filter(|f| executable(&f.path, &f.component)).map(|f| f.path.as_str()).collect();
        imp::write_zip(&out, &entries, options.source_date_epoch,
                       &exec,
        )?;
        Ok(out)
    }
    #[cfg(not(feature = "zip"))]
    {
        let _ = (m, files, out_dir);
        Err(no_zip())
    }
}

/// A bundle's sha256, remembered while the zip is unchanged (crate::filecache; across launcher starts too): `list`
/// runs on every status poll, and hashing each ~100 MB zip every time was most of a status's cost.
fn cached_sha(zip: &Path) -> Result<String> {
    crate::filecache::sha256(zip)
}

fn describe(zip: &Path, m: &RuntimeManifest) -> Result<RuntimeBundle> {
    // when it was built: the manifest's `created`, else the zip's mtime (orders builds of one version and date)
    let created = m.created.clone().or_else(|| {
        let t = std::fs::metadata(zip).and_then(|md| md.modified()).ok()?;
        Some(crate::util::rfc3339(t.duration_since(std::time::UNIX_EPOCH).ok()?.as_secs() as i64))
    });
    Ok(RuntimeBundle {
        version: m.version.clone(),
        kiln: m.kiln.clone(),
        file: zip.to_path_buf(),
        sha256: cached_sha(zip)?,
        created,
        sides: sides(m),
        components: m.components.keys().cloned().collect(),
        parts: m
            .components
            .iter()
            .map(|(name, c)| BundlePart {
                name: name.clone(),
                sides: c.sides.clone(),
                default: c.default,
                requires: c.requires.clone(),
                description: c.description.clone(),
            })
            .collect(),
    })
}

/// Which bundle is newer: the version (semver, then the build date), then when it was built (`created`). Two builds
/// of one version on one day differ only there; the git id in the version says nothing about order.
pub fn compare(a: &RuntimeBundle, b: &RuntimeBundle) -> std::cmp::Ordering {
    compare_versions(&a.version, &b.version).then_with(|| a.created.cmp(&b.created))
}

/// The bundles in `<data>/runtime/` (unreadable zips are skipped), newest first (`compare`).
pub fn list(dirs: &crate::paths::DataDirs) -> Result<Vec<RuntimeBundle>> {
    let dir = dirs.runtime();
    let mut out = Vec::new();
    for e in std::fs::read_dir(&dir).into_iter().flatten().flatten() {
        let p = e.path();
        // zips only: a folder of that name is no bundle the launcher keeps
        if p.extension().is_some_and(|x| x == "zip")
            && p.is_file()
            && let Ok(m) = read(&p)
        {
            out.push(describe(&p, &m)?);
        }
    }
    out.sort_by(|a, b| compare(b, a));
    crate::filecache::flush();
    Ok(out)
}

/// Bundle versions `<semver>+<yyyymmdd>.<git>`: by the semver, then by the date.
pub fn compare_versions(a: &str, b: &str) -> std::cmp::Ordering {
    let split = |v: &str| -> (Option<crate::semver::SemVersion>, String) {
        let (base, build) = v.split_once('+').unwrap_or((v, ""));
        (crate::semver::SemVersion::parse(base).ok(), build.split('.').next().unwrap_or("").to_string())
    };
    let (va, da) = split(a);
    let (vb, db) = split(b);
    match (va, vb) {
        (Some(x), Some(y)) => x.compare(&y).then(da.cmp(&db)),
        _ => a.cmp(b),
    }
}

/// Copies a bundle zip into `<data>/runtime/` after `verify`. An unpacked folder is refused: the launcher keeps zips.
pub fn import(dirs: &crate::paths::DataDirs, zip: &Path) -> Result<RuntimeBundle> {
    if is_folder(zip) {
        return Err(Error::new(
            ErrorCode::Unsupported,
            format!("{}: an unpacked bundle folder; the launcher keeps runtime bundles as zips in {} (import the bundle's zip, \
                     or install the folder with kilnctl bundle install)", zip.display(), dirs.runtime().display()),
        ));
    }
    let m = verify(zip, &mut crate::task::NoProgress)?;
    let dir = dirs.runtime();
    std::fs::create_dir_all(&dir).map_err(crate::util::io_at(&dir, "creating"))?;
    let dst = dir.join(file_name(&m.version));
    if std::fs::canonicalize(zip).ok() != std::fs::canonicalize(&dst).ok() {
        let tmp = crate::util::with_suffix(&dst, ".tmp");
        std::fs::copy(zip, &tmp).map_err(crate::util::io_at(&tmp, "copying to"))?;
        crate::util::rename_replace(&tmp, &dst)?;
    }
    describe(&dst, &m)
}

/// Copies the bundle a launcher ships into `<data>/runtime/`, unless that version is already there and readable.
/// `None`: nothing to do.
pub fn import_shipped(dirs: &crate::paths::DataDirs, zip: &Path) -> Result<Option<RuntimeBundle>> {
    let m = read(zip)?;
    if read(&dirs.runtime().join(file_name(&m.version))).is_ok() {
        return Ok(None);
    }
    import(dirs, zip).map(Some)
}

/// The bundle of `version` in `<data>/runtime/`.
pub fn find(dirs: &crate::paths::DataDirs, version: &str) -> Result<RuntimeBundle> {
    list(dirs)?
        .into_iter()
        .find(|b| b.version == version)
        .ok_or_else(|| Error::not_found(format!("no runtime bundle {version} in {}", dirs.runtime().display())))
}
