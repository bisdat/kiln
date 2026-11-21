// SPDX-License-Identifier: MIT
//! Runtime half of native admission. No automatic activation: the admitted host
//! must keep this on its serial worker, poll before dispatch, and wire its Sink
//! to durable state, runtime quarantine and the native/managed save notice.
use crate::{
    native_contracts::{AdmissionReport, AdmissionVerdict, NativeContracts},
    pe_gate::{Image, MAX_IMAGE_BYTES},
};
use std::{
    collections::BTreeSet,
    fs::{File, OpenOptions},
    io::Read,
    path::{Path, PathBuf},
};
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
#[repr(C)]
struct FileId {
    volume: u64,
    index: u64,
    size: u64,
}
pub struct Snapshot {
    path: PathBuf,
    name: String,
    bytes: Vec<u8>,
    digest: String,
    id: FileId,
    folder: FileId,
    _file: File,
    #[cfg(windows)]
    _directories: Vec<File>,
}
impl Snapshot {
    pub fn capture(path: &Path, name: &str) -> Result<Self, String> {
        if !path.is_absolute()
            || std::fs::symlink_metadata(path)
                .map_err(|e| e.to_string())?
                .file_type()
                .is_symlink()
        {
            return Err("native image must be an absolute regular non-link path".into());
        }
        let path = std::fs::canonicalize(path).map_err(|e| e.to_string())?;
        #[cfg(windows)]
        let directories = windows::hold_directories(&path)?;
        let mut options = OpenOptions::new();
        options.read(true);
        #[cfg(windows)]
        {
            use std::os::windows::fs::OpenOptionsExt;
            options.share_mode(1).custom_flags(0x00200000);
        }
        let file = options.open(&path).map_err(|e| e.to_string())?;
        if !file.metadata().map_err(|e| e.to_string())?.is_file() {
            return Err("native image is not a regular file".into());
        }
        #[cfg(windows)]
        {
            use std::os::windows::fs::MetadataExt;
            if file
                .metadata()
                .map_err(|e| e.to_string())?
                .file_attributes()
                & 0x400
                != 0
            {
                return Err("native image is a reparse point".into());
            }
        }
        let id = identity(&file)?;
        let folder = directory_identity(path.parent().ok_or("native image has no folder")?)?;
        if id.size > MAX_IMAGE_BYTES as u64 {
            return Err("native image exceeds byte budget".into());
        }
        let mut bytes = Vec::new();
        (&file)
            .take(MAX_IMAGE_BYTES as u64 + 1)
            .read_to_end(&mut bytes)
            .map_err(|e| e.to_string())?;
        if bytes.len() as u64 != id.size || identity(&file)? != id {
            return Err("native image changed during snapshot".into());
        }
        let digest = NativeContracts::sha256(&bytes);
        Ok(Self {
            path,
            name: name.into(),
            bytes,
            digest,
            id,
            folder,
            _file: file,
            #[cfg(windows)]
            _directories: directories,
        })
    }
    fn revalidate(&self) -> Result<(), String> {
        let now = Self::capture(&self.path, &self.name)?;
        if now.id != self.id || now.folder != self.folder || now.digest != self.digest {
            return Err("native image identity, size or SHA-256 changed after inspection".into());
        }
        Ok(())
    }
    pub fn path(&self) -> &Path {
        &self.path
    }
    /// SHA-256 (lower-case hex) of the bytes captured and held.
    pub fn digest(&self) -> &str {
        &self.digest
    }
    pub(crate) fn bytes(&self) -> &[u8] {
        &self.bytes
    }
}
fn directory_identity(path: &Path) -> Result<FileId, String> {
    let mut options = OpenOptions::new();
    options.read(true);
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        options.share_mode(3).custom_flags(0x02200000);
    }
    let file = options.open(path).map_err(|e| e.to_string())?;
    let mut id = identity(&file)?;
    id.size = 0;
    Ok(id)
}
#[cfg(unix)]
fn identity(file: &File) -> Result<FileId, String> {
    use std::os::unix::fs::MetadataExt;
    let m = file.metadata().map_err(|e| e.to_string())?;
    Ok(FileId {
        volume: m.dev(),
        index: m.ino(),
        size: m.len(),
    })
}
#[cfg(windows)]
fn identity(file: &File) -> Result<FileId, String> {
    windows::file_id(file)
}
#[cfg(not(any(unix, windows)))]
fn identity(_: &File) -> Result<FileId, String> {
    Err("file identity is unsupported on this platform".into())
}

#[cfg(any(windows, test))]
mod loader_path;
mod resolution;
mod supervisor;
#[derive(Debug, Clone)]
pub struct ParkedFault {
    pub owner: Option<u64>,
    pub all: bool,
    pub reason: String,
}
pub trait FaultSource: Send {
    fn wait(&mut self, budget: std::time::Duration) -> Result<Option<ParkedFault>, String>;
}
/// A real OS resolver owns construction; callers cannot turn a DLL-name list into
/// a system proof. The snapshot is retained and rechecked like package files.
pub struct SystemImage {
    snapshot: std::sync::Arc<Snapshot>,
    requested: String,
    importer: String,
    host: String,
}
pub(crate) struct VerifiedSystems {
    names: BTreeSet<String>,
}
impl VerifiedSystems {
    pub(crate) fn names(&self) -> impl Iterator<Item = &str> {
        self.names.iter().map(String::as_str)
    }
}
/// Private, non-forgeable permit retaining exactly the snapshots inspected. All
/// package dependencies must be supplied; no raw plan gate flag becomes a permit.
pub struct LoadPermit<'a> {
    entry: &'a Snapshot,
    files: Vec<&'a Snapshot>,
    closure: resolution::Closure,
    report: AdmissionReport,
}
impl<'a> LoadPermit<'a> {
    pub fn inspect(
        entry: &'a Snapshot,
        dependencies: &'a [Snapshot],
        registry: &NativeContracts,
    ) -> Result<Self, String> {
        Self::build(entry, dependencies, registry, |_, _| Ok(None))
    }
    fn build(
        entry: &'a Snapshot,
        dependencies: &'a [Snapshot],
        registry: &NativeContracts,
        system: impl FnMut(&Snapshot, &str) -> Result<Option<SystemImage>, String>,
    ) -> Result<Self, String> {
        let files: Vec<_> = std::iter::once(entry).chain(dependencies.iter()).collect();
        let closure = resolution::resolve(&files, system)?;
        let images: Vec<_> = closure
            .nodes
            .iter()
            .filter_map(|node| match node.source {
                resolution::Source::Package(index) => Some(Image {
                    path: &files[index].name,
                    bytes: &files[index].bytes,
                }),
                resolution::Source::System(_) => None,
            })
            .collect();
        let systems: Vec<_> = closure.verified.names().collect();
        let report = registry.evaluate_bound(&entry.name, &images, &systems);
        if !matches!(
            report.verdict,
            AdmissionVerdict::Pass | AdmissionVerdict::AdmittedWithEnforcement
        ) || !report.requires_enforcement
        {
            return Err(report.reasons.join("; "));
        }
        Ok(Self {
            entry,
            files,
            closure,
            report,
        })
    }
    fn bound_files(&self) -> impl Iterator<Item = &Snapshot> {
        self.closure
            .nodes
            .iter()
            .enumerate()
            .map(|(i, _)| self.closure.snapshot(&self.files, i))
    }
    pub fn report(&self) -> &AdmissionReport {
        &self.report
    }
}
#[derive(Debug, Clone)]
pub struct Breach {
    pub owner: Option<u64>,
    pub module: String,
    pub reason: String,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BackendLoadError {
    LoaderFailure {
        code: u32,
        message: String,
    },
    NativeFault {
        exception: u32,
        loader_error: u32,
        may_have_run: bool,
        pin_status: i32,
    },
}
impl BackendLoadError {
    /// Windows may translate a DllMain exception into DLL_INIT_FAILED; Wine may
    /// preserve the exception value in GetLastError. Do not erase that distinction.
    pub fn from_load_status(exception: u32, loader_error: u32, message: String) -> Self {
        Self::from_load_evidence(exception, loader_error, false, 0, message)
    }
    pub fn from_load_evidence(
        exception: u32,
        loader_error: u32,
        may_have_run: bool,
        pin_status: i32,
        message: String,
    ) -> Self {
        let application_exception = loader_error & 0xe000_0000 == 0xe000_0000;
        let standard_exception = matches!(
            loader_error,
            0xc0000005
                | 0xc0000006
                | 0xc000001d
                | 0xc0000025
                | 0xc0000026
                | 0xc0000094
                | 0xc00000fd
                | 0x80000002
                | 0x80000003
                | 0x80000004
        );
        if exception != 0
            || may_have_run
            || pin_status < 0
            || matches!(loader_error, 998 | 1001 | 1114 | 0xc0000142)
            || application_exception
            || standard_exception
        {
            Self::NativeFault {
                // Preserve provenance: a loader-converted exception code is
                // not evidence that SEH unwound through our own frames.
                exception,
                loader_error,
                may_have_run,
                pin_status,
            }
        } else {
            Self::LoaderFailure {
                code: loader_error,
                message,
            }
        }
    }
}
impl std::fmt::Display for BackendLoadError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::LoaderFailure { code, message } => {
                write!(f, "loader refusal {code:#x}: {message}")
            }
            Self::NativeFault {
                exception,
                loader_error,
                may_have_run,
                pin_status,
            } => write!(
                f,
                "native load fault: SEH {exception:#x}, loader {loader_error:#x}, LOADED={may_have_run}, pin={pin_status:#x}"
            ),
        }
    }
}
fn loader_refusal(message: String) -> BackendLoadError {
    BackendLoadError::LoaderFailure { code: 0, message }
}
pub trait Backend {
    fn modules(&mut self) -> Result<Vec<String>, String>;
    fn register(&mut self) -> Result<(), String>;
    fn fault_source(&mut self) -> Result<Option<Box<dyn FaultSource>>, String> {
        Ok(None)
    }
    fn events(&mut self) -> Result<Vec<Breach>, String>;
    fn owner_failures(&mut self) -> Result<Vec<Breach>, String> {
        Ok(Vec::new())
    }
    fn resolve_system(&mut self, _: &Snapshot, _: &str) -> Result<Option<SystemImage>, String> {
        Ok(None)
    }
    fn first_loader(&mut self, image: &Snapshot) -> Result<Option<usize>, String>;
    fn load(&mut self, path: &Path, owner: u64) -> Result<usize, BackendLoadError>;
    fn validate_loaded(&mut self, module: usize, image: &Snapshot) -> Result<(), String>;
    fn load_permit(
        &mut self,
        permit: &LoadPermit<'_>,
        owner: u64,
    ) -> Result<LoadOutcome, BackendLoadError> {
        // Test backends model this atomic binding step. Windows overrides it
        // with the C/SEH loader-lock transaction, not a check/load race.
        let mut root = None;
        for image in permit.bound_files() {
            if let Some(module) = self.first_loader(image).map_err(loader_refusal)? {
                self.validate_loaded(module, image)
                    .map_err(loader_refusal)?;
                if std::ptr::eq(image, permit.entry) {
                    root = Some(module);
                }
            }
        }
        if let Some(module) = root {
            return Ok(LoadOutcome::ForeignOwned(module));
        }
        self.load(&permit.entry.path, owner)
            .map(LoadOutcome::Loaded)
    }
}
pub trait Sink {
    fn self_hosted(&mut self, reason: &str);
    fn quarantine(&mut self, owner: Option<u64>, reason: &str);
    fn record(&mut self, reason: &str);
    fn save_warning(&mut self, reason: &str);
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LoadOutcome {
    Loaded(usize),
    ForeignOwned(usize),
}
pub struct Enforcer<B, S> {
    backend: B,
    sink: std::sync::Arc<std::sync::Mutex<S>>,
    parked: std::sync::Arc<std::sync::Mutex<supervisor::ParkedState>>,
    _supervisor: Option<supervisor::Supervisor>,
    quarantined: BTreeSet<u64>,
    all: bool,
    breached: bool,
}
impl<B: Backend, S: Sink + Send + 'static> Enforcer<B, S> {
    /// Invoke at the early phase before admission, not after the writer commits.
    /// A pre-existing banned module is wrong backend selection, not late quarantine.
    pub fn early(mut backend: B, mut sink: S) -> Result<Self, String> {
        let names = backend.modules()?;
        if !names.is_empty() {
            let reason = existing_runtime_reason(&names);
            sink.self_hosted(&reason);
            return Err(reason);
        }
        backend.register()?; // Notification is live BEFORE the first package DLL load.
        let names = backend.modules()?; // Close the scan/register race before admission.
        if !names.is_empty() {
            let reason = existing_runtime_reason(&names);
            sink.self_hosted(&reason);
            return Err(reason);
        }
        let source = backend.fault_source()?;
        let sink = std::sync::Arc::new(std::sync::Mutex::new(sink));
        let parked = std::sync::Arc::new(std::sync::Mutex::new(supervisor::ParkedState::default()));
        let observer = source
            .map(|source| supervisor::Supervisor::spawn(source, sink.clone(), parked.clone()))
            .transpose()?;
        let mut this = Self {
            backend,
            sink,
            parked,
            _supervisor: observer,
            quarantined: BTreeSet::new(),
            all: false,
            breached: false,
        };
        let early_events = this.backend.events()?;
        if !early_events.is_empty() {
            let names = early_events
                .iter()
                .map(|event| event.module.clone())
                .collect::<Vec<_>>();
            let reason = existing_runtime_reason(&names);
            this.sink
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .self_hosted(&reason);
            return Err(reason);
        }
        this.poll()?;
        if this.breached {
            return Err("native invariant breached before admission completed".into());
        }
        Ok(this)
    }
    pub fn poll(&mut self) -> Result<(), String> {
        for event in self.backend.owner_failures()? {
            self.record_fault(event, false);
        }
        match self.backend.events() {
            Ok(events) => {
                for event in events {
                    self.breach(event);
                }
            }
            Err(reason) => {
                self.breach(Breach {
                    owner: None,
                    module: "unknown".into(),
                    reason: reason.clone(),
                });
                return Err(reason);
            }
        }
        Ok(())
    }
    fn breach(&mut self, event: Breach) {
        self.record_fault(event, true);
    }
    fn record_fault(&mut self, event: Breach, global: bool) {
        self.breached |= global;
        if let Some(owner) = event.owner {
            self.quarantined.insert(owner);
        } else {
            self.all = true;
        }
        let reason = format!(
            "native invariant breach: {}: {}",
            event.module, event.reason
        );
        self.sink
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .quarantine(event.owner, &reason);
        self.sink
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .record(&reason);
        self.sink.lock().unwrap_or_else(|e|e.into_inner()).save_warning(&format!("{reason}. Startup data may still be applied. Do not save while the mod is off; restore it and restart first."));
    }
    pub fn inspect<'a>(
        &mut self,
        entry: &'a Snapshot,
        dependencies: &'a [Snapshot],
        registry: &NativeContracts,
    ) -> Result<LoadPermit<'a>, String> {
        if self.breached {
            return Err("native enforcement breached".into());
        }
        LoadPermit::build(entry, dependencies, registry, |importer, name| {
            self.backend.resolve_system(importer, name)
        })
    }
    /// One line for the sink's record (kiln.log in production), from the thread that runs
    /// the loads: the native run notes a load that returned after the startup deadline had
    /// concluded the run.
    pub fn record(&self, text: &str) {
        self.sink
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .record(text);
    }
    pub fn is_quarantined(&self, owner: u64) -> bool {
        let parked = self.parked.lock().unwrap_or_else(|e| e.into_inner());
        self.all
            || self.quarantined.contains(&owner)
            || parked.all
            || parked.owners.contains(&owner)
    }
    pub fn load(&mut self, owner: u64, permit: &LoadPermit<'_>) -> Result<LoadOutcome, String> {
        self.load_if(owner, permit, || true)
    }
    /// [`Self::load`] with a commit point: `start` is asked once, after every check and
    /// revalidation and immediately before the loader is called. `false` calls nothing and
    /// records nothing (the startup deadline closed the run before this load began).
    pub fn load_if(
        &mut self,
        owner: u64,
        permit: &LoadPermit<'_>,
        start: impl FnOnce() -> bool,
    ) -> Result<LoadOutcome, String> {
        if owner == 0 {
            return Err("native load needs a package owner".into());
        }
        self.poll()?;
        if self.breached || self.is_quarantined(owner) {
            return Err("native enforcement breached; no further loads or backend fallback".into());
        }
        for image in permit.bound_files() {
            image.revalidate()?;
        }
        if !start() {
            return Err("not started: the native startup deadline passed".into());
        }
        let result = self.backend.load_permit(permit, owner);
        if let Err(
            error @ BackendLoadError::NativeFault {
                exception,
                loader_error,
                pin_status,
                ..
            },
        ) = &result
        {
            let global =
                *exception != 0 || *pin_status < 0 || matches!(*loader_error, 1001 | 0xc00000fd);
            self.record_fault(
                Breach {
                    owner: Some(owner),
                    module: permit.entry.name.clone(),
                    reason: error.to_string(),
                },
                global,
            );
        }
        // Always audit after LoadLibrary, including semantic failure or DllMain fault.
        let scan = self.backend.modules();
        self.poll()?;
        match scan {
            Ok(names) if !names.is_empty() => {
                if !self.breached {
                    self.breach(Breach {
                        owner: None,
                        module: names.join(","),
                        reason: "post-load module-list violation".into(),
                    });
                }
            }
            Err(reason) => {
                self.breach(Breach {
                    owner: None,
                    module: "unknown".into(),
                    reason: reason.clone(),
                });
                return Err(reason);
            }
            _ => (),
        }
        if self.breached || self.is_quarantined(owner) {
            return Err("native package quarantined; modules retained, no retry".into());
        }
        let outcome = match result {
            Ok(outcome) => outcome,
            Err(reason) => {
                self.sink
                    .lock()
                    .unwrap_or_else(|e| e.into_inner())
                    .record(&format!("native load failed before admission: {reason}"));
                return Err(reason.to_string());
            }
        };
        let module = match outcome {
            LoadOutcome::Loaded(module) | LoadOutcome::ForeignOwned(module) => module,
        };
        if let Err(reason) = self.backend.validate_loaded(module, permit.entry) {
            self.breach(Breach {
                owner: Some(owner),
                module: permit.entry.name.clone(),
                reason: reason.clone(),
            });
            return Err(reason);
        }
        Ok(outcome)
    }
}
pub(crate) fn existing_runtime_reason(names: &[String]) -> String {
    format!(
        "ShroudForge's own loader ({}) runs it; Kiln's profile does not decide ShroudForge mods this start",
        names.join(",")
    )
}

#[cfg(windows)]
pub use windows::WindowsBackend;
#[cfg(windows)]
mod windows;
#[cfg(windows)]
pub(crate) fn early_scan() -> Result<Vec<String>, String> {
    windows::early_scan()
}
/// Marks (or unmarks) the calling thread as the interpreter worker's: the worker
/// body sets it on entry and clears it on every way out (`worker::task`).
#[cfg(windows)]
pub(crate) fn mark_interpreter(active: bool) {
    windows::mark_interpreter(active);
}
