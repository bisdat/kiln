// SPDX-License-Identifier: MIT
//! Windows proof authority. Never treats a loader pathname as mapped identity.
use super::*;
use core::ffi::c_void;
use std::os::windows::{
    ffi::{OsStrExt, OsStringExt},
    fs::{MetadataExt, OpenOptionsExt},
    io::AsRawHandle,
};
use std::{collections::BTreeMap, sync::Arc};
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct OwnerFault {
    owner: u64,
    flags: u32,
    reserved: u32,
}
#[repr(C)]
struct ParkSignal {
    sequence: u64,
    owner: u64,
    all: u32,
    exception: u32,
}
#[repr(C)]
struct Event {
    sequence: u64,
    owner: u64,
    kind: u32,
    reserved: u32,
    module: [u16; 64],
}
#[repr(C)]
struct Binding {
    path: *const u16,
    name: *const u16,
    file: *mut c_void,
    view: *mut c_void,
    flags: u32,
    reserved: u32,
    sha256: [u8; 32],
}
#[repr(C)]
#[derive(Clone, Copy)]
struct CopyProof {
    module: *mut c_void,
    view: *mut c_void,
    sha256: [u8; 32],
}
#[repr(C)]
struct ApiBinding {
    importer: *const u16,
    requested: *const u16,
    host: *const u16,
}
const _: () = {
    assert!(core::mem::size_of::<Event>() == 152);
    assert!(core::mem::offset_of!(Event, module) == 24);
    assert!(core::mem::size_of::<FileId>() == 24);
    assert!(core::mem::size_of::<Binding>() == 72);
    assert!(core::mem::offset_of!(Binding, sha256) == 40);
    assert!(core::mem::size_of::<CopyProof>() == 48);
    assert!(core::mem::size_of::<ApiBinding>() == 24);
};
unsafe extern "C" {
    fn sfi_owner_fault_copy(output: *mut OwnerFault, capacity: u32, required: *mut u32) -> u32;
    fn sfi_mapped_header_matches(
        module: *mut c_void,
        disk: *const u8,
        length: usize,
        matches: *mut u32,
    ) -> u32;
    fn sfi_guard_interpreter(active: u32);
    fn sfi_guard_prepare(error: *mut u32) -> u32;
    fn sfi_guard_wait(
        after: u64,
        milliseconds: u32,
        fault: *mut ParkSignal,
        available: *mut u32,
        error: *mut u32,
    ) -> u32;
    fn sfi_breach_flags() -> u32;
    fn sfi_breach_recover(scan_ok: u32, present: u32) -> u32;
    fn sfi_breach_name_mask(name: *const u16, length: u32) -> u32;
    fn sfi_loaded_banned_mask(mask: *mut u32, error: *mut u32) -> u32;
    fn sfi_monitor_start(error: *mut u32) -> u32;
    fn sfi_monitor_poll(after: u64, event: *mut Event, state: *mut u32) -> u32;
    fn sfi_monitor_owner(owner: u64);
    fn sfi_monitor_epoch() -> u64;
    fn sfi_monitor_modules(
        modules: *mut *mut c_void,
        capacity: u32,
        count: *mut u32,
        error: *mut u32,
    ) -> u32;
    fn sfi_monitor_module_path(
        module: *mut c_void,
        path: *mut u16,
        capacity: u32,
        count: *mut u32,
        error: *mut u32,
    ) -> u32;
    fn sfi_monitor_file_id(file: *mut c_void, id: *mut FileId, error: *mut u32) -> u32;
    fn sfi_image_view(file: *mut c_void, view: *mut *mut c_void, error: *mut u32) -> u32;
    fn sfi_image_unmap(view: *mut c_void) -> u32;
    fn sfi_same_mapping(
        module: *mut c_void,
        view: *mut c_void,
        same: *mut u32,
        error: *mut u32,
    ) -> u32;
    fn sfi_system_path(
        importer: *const u16,
        requested: *const u16,
        path: *mut u16,
        capacity: u32,
        count: *mut u32,
        error: *mut u32,
    ) -> u32;
    fn sfi_verified_load(
        bindings: *const Binding,
        count: u32,
        copies: *const CopyProof,
        copy_count: u32,
        apis: *const ApiBinding,
        api_count: u32,
        owner: u64,
        observed: u64,
        epoch: u64,
        module: *mut *mut c_void,
        foreign: *mut u32,
        may_have_run: *mut u32,
        pin_status: *mut i32,
        failed: *mut u32,
        error: *mut u32,
    ) -> u32;
    fn sfi_image_quarantined(
        module: *mut c_void,
        view: *mut c_void,
        sha256: *const u8,
        quarantined: *mut u32,
        owner: *mut u64,
        pin_status: *mut i32,
        error: *mut u32,
    ) -> u32;
}
/// bindings.c SFI_NOT_INITIALIZED (ERROR_INVALID_MODULETYPE): a proved mapping whose OS loader never initialized it
/// (or whose DllMain failed), or an entry another loader has not finished loading.
const NOT_INITIALIZED: u32 = 190;
/// bindings.c SFI_TOO_MANY_COPIES (a customer code, never a Windows error): a loader watch already holds 8 distinct
/// identical files of this name, from 8 other packages, and holds no ninth.
const TOO_MANY_COPIES: u32 = 0x2000_0008;
fn checked(fault: u32, error: u32) -> Result<(), String> {
    if fault != 0 || error != 0 {
        Err(format!(
            "native OS proof failed: SEH {fault:#x}, status {error:#x}"
        ))
    } else {
        Ok(())
    }
}
/// The NUL-terminated spelling handed to LoadLibraryExW: the plain form of the canonical verbatim path whenever it
/// names the same file (loader_path.rs: native Windows refuses a verbatim kernel32.dll with error 487), or a refusal
/// for a path the loader would map as another file whatever the spelling.
fn loader_path(path: &Path) -> Result<Vec<u16>, String> {
    let verbatim: Vec<u16> = path.as_os_str().encode_wide().collect();
    loader_path::loadable(&verbatim)?;
    let mut spelled = loader_path::plain_spelling(&verbatim).unwrap_or(verbatim);
    spelled.push(0);
    Ok(spelled)
}
fn wide_text(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}
fn digest_bytes(s: &str) -> Result<[u8; 32], String> {
    if s.len() != 64 {
        return Err("invalid snapshot digest".into());
    }
    let mut out = [0; 32];
    for (i, byte) in out.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&s[i * 2..i * 2 + 2], 16).map_err(|e| e.to_string())?;
    }
    Ok(out)
}
pub(super) fn file_id(file: &File) -> Result<FileId, String> {
    let mut id = FileId {
        volume: 0,
        index: 0,
        size: 0,
    };
    let mut error = 0;
    let fault = unsafe { sfi_monitor_file_id(file.as_raw_handle(), &mut id, &mut error) };
    checked(fault, error)?;
    Ok(id)
}
pub(super) fn hold_directories(path: &Path) -> Result<Vec<File>, String> {
    let mut held = Vec::new();
    for directory in path.ancestors().skip(1) {
        let mut file = OpenOptions::new()
            .read(true)
            .share_mode(3)
            .custom_flags(0x02200000)
            .open(directory)
            .map_err(|e| e.to_string())?;
        // GetFinalPathName/canonicalize has resolved links. Remaining reparse anchors
        // include Wine's DOS drives and Unix mount boundaries, not just user symlinks.
        // Hold those anchors without write OR delete sharing so they cannot be retargeted
        // through Windows while the verified path is in use. Package-root authorization
        // (including rejecting untrusted links) remains the frozen-plan producer's job.
        if file
            .metadata()
            .map_err(|e| e.to_string())?
            .file_attributes()
            & 0x400
            != 0
        {
            file = OpenOptions::new()
                .read(true)
                .share_mode(1)
                .custom_flags(0x02200000)
                .open(directory)
                .map_err(|e| e.to_string())?;
        }
        held.push(file);
    }
    Ok(held)
}
struct ImageView {
    view: *mut c_void,
    _file: File,
}
impl ImageView {
    fn open(file: &File) -> Result<Self, String> {
        let retained = file.try_clone().map_err(|e| e.to_string())?;
        let mut view = std::ptr::null_mut();
        let mut error = 0;
        let fault = unsafe { sfi_image_view(retained.as_raw_handle(), &mut view, &mut error) };
        checked(fault, error)?;
        if view.is_null() {
            return Err("empty backing view".into());
        }
        Ok(Self {
            view,
            _file: retained,
        })
    }
}
impl Drop for ImageView {
    fn drop(&mut self) {
        unsafe {
            sfi_image_unmap(self.view);
        }
    }
}
fn same(module: usize, view: *mut c_void) -> Result<bool, String> {
    let (mut result, mut error) = (0, 0);
    let fault = unsafe { sfi_same_mapping(module as *mut c_void, view, &mut result, &mut error) };
    checked(fault, error)?;
    Ok(result != 0)
}
fn module_path(module: usize) -> Result<PathBuf, String> {
    let mut path = vec![0u16; 32768];
    let (mut count, mut error) = (0, 0);
    let fault = unsafe {
        sfi_monitor_module_path(
            module as *mut c_void,
            path.as_mut_ptr(),
            32768,
            &mut count,
            &mut error,
        )
    };
    checked(fault, error)?;
    path.truncate(count as usize);
    Ok(std::ffi::OsString::from_wide(&path).into())
}
fn normalized_name(path: &Path) -> String {
    path.file_name()
        .unwrap_or_default()
        .to_string_lossy()
        .trim_end_matches([' ', '.'])
        .to_ascii_lowercase()
}
struct ParkSource {
    after: u64,
}
impl FaultSource for ParkSource {
    fn wait(&mut self, budget: std::time::Duration) -> Result<Option<ParkedFault>, String> {
        let mut signal = ParkSignal {
            sequence: 0,
            owner: 0,
            all: 0,
            exception: 0,
        };
        let (mut available, mut error) = (0, 0);
        let fault = unsafe {
            sfi_guard_wait(
                self.after,
                budget.as_millis().min(u32::MAX as u128) as u32,
                &mut signal,
                &mut available,
                &mut error,
            )
        };
        checked(fault, error)?;
        if available == 0 {
            return Ok(None);
        }
        self.after = signal.sequence;
        Ok(Some(ParkedFault {
            owner: (signal.owner != 0).then_some(signal.owner),
            all: signal.all != 0,
            reason: format!(
                "native stack guard restoration failed ({:#x}); faulted thread parked for process life",
                signal.exception
            ),
        }))
    }
}
#[derive(Default)]
pub struct WindowsBackend {
    after: u64,
    reported_flags: u32,
    reported_owners: BTreeMap<u64, u32>,
    views: BTreeMap<FileId, ImageView>,
    copies: BTreeMap<usize, CopyProof>,
    systems: BTreeMap<String, Arc<Snapshot>>,
}
impl WindowsBackend {
    pub fn with_owner<T>(owner: u64, call: impl FnOnce() -> T) -> Result<T, String> {
        if owner == 0 {
            return Err("native call needs a package owner".into());
        }
        std::thread_local! {static OWNER:std::cell::Cell<u64>=const{std::cell::Cell::new(0)};}
        struct Scope(u64);
        impl Drop for Scope {
            fn drop(&mut self) {
                OWNER.set(self.0);
                unsafe { sfi_monitor_owner(self.0) }
            }
        }
        let previous = OWNER.replace(owner);
        unsafe { sfi_monitor_owner(owner) };
        let _scope = Scope(previous);
        Ok(call())
    }
    fn paths(&mut self) -> Result<Vec<(usize, PathBuf)>, String> {
        let mut modules = vec![std::ptr::null_mut(); 4096];
        let (mut count, mut error) = (0, 0);
        let fault =
            unsafe { sfi_monitor_modules(modules.as_mut_ptr(), 4096, &mut count, &mut error) };
        checked(fault, error)?;
        modules.truncate(count as usize);
        Ok(modules
            .into_iter()
            .filter_map(|module| {
                module_path(module as usize)
                    .ok()
                    .map(|path| (module as usize, path))
            })
            .collect())
    }
    fn loaded_mask() -> Result<u32, String> {
        let (mut mask, mut error) = (0, 0);
        let fault = unsafe { sfi_loaded_banned_mask(&mut mask, &mut error) };
        checked(fault, error)?;
        Ok(mask)
    }
    fn flag_names(flags: u32) -> Vec<String> {
        let mut names = Vec::new();
        if flags & 1 != 0 {
            names.push("shroudforge-runtime.dll".into());
        }
        if flags & 2 != 0 {
            names.push(concat!("kf", "c-runtime.dll").into());
        }
        names
    }
    fn reconcile(
        &mut self,
        mut out: Vec<Breach>,
        seen: u32,
        lost: bool,
    ) -> Result<Vec<Breach>, String> {
        let mut flags = unsafe { sfi_breach_flags() };
        let unseen = flags & !self.reported_flags & !seen;
        if lost || unseen != 0 {
            let present = match Self::loaded_mask() {
                Ok(mask) => mask,
                Err(error) => {
                    unsafe { sfi_breach_recover(0, 0) };
                    return Err(error);
                }
            };
            flags = unsafe { sfi_breach_recover(1, present) };
            if flags & 3 != 0 && (lost || unseen & 3 != 0) {
                out.push(Breach{owner:None,module:Self::flag_names(flags|present).join(","),reason:"sticky banned-load proof recovered by full module scan; diagnostic details may be lost".into()});
            }
            if flags & 8 != 0 && (lost || unseen & 8 != 0) {
                out.push(Breach {
                    owner: None,
                    module: "lazy dependency".into(),
                    reason: "sticky dependency-binding breach recovered after diagnostic loss"
                        .into(),
                });
            }
        }
        if flags & 4 != 0 {
            return Err("native loader observer failed; enforcement unavailable".into());
        }
        self.reported_flags |= flags | seen;
        Ok(out)
    }
    fn view(&mut self, image: &Snapshot) -> Result<*mut c_void, String> {
        if let std::collections::btree_map::Entry::Vacant(entry) = self.views.entry(image.id) {
            entry.insert(ImageView::open(&image._file)?);
        }
        Ok(self.views[&image.id].view)
    }
    fn reject_quarantined(&mut self, image: &Snapshot, module: usize) -> Result<(), String> {
        let view = self.view(image)?;
        let hash = digest_bytes(&image.digest)?;
        let (mut q, mut owner, mut status, mut error) = (0, 0, 0, 0);
        let fault = unsafe {
            sfi_image_quarantined(
                module as *mut c_void,
                view,
                hash.as_ptr(),
                &mut q,
                &mut owner,
                &mut status,
                &mut error,
            )
        };
        checked(fault, error)?;
        if q != 0 {
            return Err(format!(
                "quarantined failed image {} owned by package {owner}; pin status {status:#x}; never retry or initialize again",
                image.name
            ));
        }
        Ok(())
    }
    fn foreign_copy(
        &mut self,
        module: usize,
        path: &Path,
        image: &Snapshot,
    ) -> Result<bool, String> {
        // The loader path is only a hint for opening a candidate. Kernel section
        // equality must prove that candidate is actually backing this mapped module.
        let file = OpenOptions::new()
            .read(true)
            .share_mode(1)
            .custom_flags(0x00200000)
            .open(path)
            .map_err(|e| format!("unverifiable mapped backing {}: {e}", path.display()))?;
        let id = file_id(&file)?;
        let temporary = ImageView::open(&file)?;
        if !same(module, temporary.view)? {
            return Err(format!(
                "loader pathname is not the backing file of mapped module {}",
                path.display()
            ));
        }
        if id.size != image.id.size {
            return Ok(false);
        }
        let mut data = Vec::new();
        (&file)
            .take(MAX_IMAGE_BYTES as u64 + 1)
            .read_to_end(&mut data)
            .map_err(|e| e.to_string())?;
        if file_id(&file)? != id || data.len() as u64 != id.size {
            return Err("mapped backing changed while hashing".into());
        }
        if NativeContracts::sha256(&data) != image.digest {
            return Ok(false);
        }
        let view = if let Some(cached) = self.views.get(&id) {
            cached.view
        } else {
            let view = temporary.view;
            self.views.insert(id, temporary);
            view
        };
        self.copies.insert(
            module,
            CopyProof {
                module: module as *mut c_void,
                view,
                sha256: digest_bytes(&image.digest)?,
            },
        );
        Ok(true)
    }
    /// Whether the candidate `module`, listed under `path`, maps `image`: the inspected file itself or a proved identical
    /// copy (`Some(true)`), another file (`Some(false)`), or `None` once the module is no longer loaded under that path.
    /// This runs outside the loader lock, so another thread may unload the module between the enumeration and its proof
    /// (NtAreMappedFilesTheSame then fails on the vacated address; midload-churn on Wine). A module that is gone is no
    /// candidate; one that stays loaded under its path and stays unprovable refuses, as before. The window scan proves
    /// every module again under the loader lock.
    fn candidate(
        &mut self,
        module: usize,
        path: &Path,
        image: &Snapshot,
        expected: *mut c_void,
    ) -> Result<Option<bool>, String> {
        let mut attempt = 0;
        loop {
            let proof = match same(module, expected) {
                Ok(true) => Ok(true),
                Ok(false) => self.foreign_copy(module, path, image),
                Err(error) => Err(error),
            };
            let error = match proof {
                Ok(matched) => return Ok(Some(matched)),
                Err(error) => error,
            };
            if module_path(module).ok().as_deref() != Some(path) {
                return Ok(None);
            }
            attempt += 1;
            if attempt == 3 {
                return Err(error);
            }
        }
    }
    fn system_path(importer: &str, requested: &str) -> Result<Option<PathBuf>, String> {
        let i = wide_text(importer);
        let r = wide_text(requested);
        let mut path = vec![0u16; 32768];
        let (mut count, mut error) = (0, 0);
        let fault = unsafe {
            sfi_system_path(
                i.as_ptr(),
                r.as_ptr(),
                path.as_mut_ptr(),
                32768,
                &mut count,
                &mut error,
            )
        };
        if fault == 0 && error == 126 {
            return Ok(None);
        }
        checked(fault, error)?;
        path.truncate(count as usize);
        Ok(Some(std::ffi::OsString::from_wide(&path).into()))
    }
}
impl Backend for WindowsBackend {
    fn modules(&mut self) -> Result<Vec<String>, String> {
        Ok(Self::flag_names(Self::loaded_mask()?))
    }
    fn register(&mut self) -> Result<(), String> {
        let mut error = 0;
        let fault = unsafe { sfi_guard_prepare(&mut error) };
        checked(fault, error)?;
        let fault = unsafe { sfi_monitor_start(&mut error) };
        checked(fault, error)
    }
    fn fault_source(&mut self) -> Result<Option<Box<dyn FaultSource>>, String> {
        Ok(Some(Box::new(ParkSource { after: 0 })))
    }
    fn owner_failures(&mut self) -> Result<Vec<Breach>, String> {
        let mut faults = vec![OwnerFault::default(); 4096];
        let mut required = 0;
        let ok = unsafe { sfi_owner_fault_copy(faults.as_mut_ptr(), 4096, &mut required) };
        if ok == 0 || required > 4096 {
            return Err("owner fault proof unavailable".into());
        }
        faults.truncate(required as usize);
        let mut out = Vec::new();
        for fault in faults {
            let previous = self.reported_owners.entry(fault.owner).or_default();
            if fault.flags & !*previous == 0 {
                continue;
            }
            *previous |= fault.flags;
            out.push(Breach {
                owner: Some(fault.owner),
                module: "native package".into(),
                reason: format!(
                    "process-lifetime owner quarantine proof {:#x}; other packages remain eligible",
                    fault.flags
                ),
            });
        }
        Ok(out)
    }
    fn events(&mut self) -> Result<Vec<Breach>, String> {
        let mut out = Vec::new();
        let mut seen = 0;
        for _ in 0..128 {
            let mut event = Event {
                sequence: 0,
                owner: 0,
                kind: 0,
                reserved: 0,
                module: [0; 64],
            };
            let mut state = 0;
            let fault = unsafe { sfi_monitor_poll(self.after, &mut event, &mut state) };
            checked(fault, 0)?;
            match state {
                0 => return self.reconcile(out, seen, false),
                1 => {
                    self.after = event.sequence;
                    let flags = unsafe { sfi_breach_flags() };
                    let n = event.module.iter().position(|b| *b == 0).unwrap_or(64);
                    let mask = unsafe { sfi_breach_name_mask(event.module.as_ptr(), n as u32) };
                    let reason = match event.kind {
                        1 if flags & mask != 0 => {
                            seen |= mask;
                            "loader notification observed banned module"
                        }
                        _ => continue,
                    };
                    out.push(Breach {
                        owner: (event.owner != 0).then_some(event.owner),
                        module: String::from_utf16_lossy(&event.module[..n]),
                        reason: format!("{reason}; notification {}", event.sequence),
                    });
                }
                2 => {
                    self.after = event.sequence;
                    return self.reconcile(out, seen, true);
                }
                3 => continue,
                _ => return Err("invalid notification state".into()),
            }
        }
        self.reconcile(out, seen, true)
    }
    fn resolve_system(
        &mut self,
        importer: &Snapshot,
        requested: &str,
    ) -> Result<Option<SystemImage>, String> {
        let importer_name = normalized_name(&importer.path);
        let Some(path) = Self::system_path(&importer_name, requested)? else {
            return Ok(None);
        };
        let host = normalized_name(&path);
        if !path.is_file() {
            return Ok(None);
        }
        let system_folder =
            directory_identity(path.parent().ok_or("system binding has no folder")?)?;
        if importer.folder != system_folder
            && !requested.starts_with("api-")
            && !requested.starts_with("ext-")
        {
            let sibling = importer
                .path
                .parent()
                .ok_or("importer has no folder")?
                .join(requested);
            if sibling.exists() {
                return Err(format!(
                    "uninspected package dependency in DLL folder: {}",
                    sibling.display()
                ));
            }
        }
        let snapshot = if let Some(cached) = self.systems.get(&host) {
            if cached.folder != system_folder {
                return Err("OS system directory changed during resolution".into());
            }
            cached.clone()
        } else {
            let image = Arc::new(Snapshot::capture(&path, &host)?);
            self.systems.insert(host.clone(), image.clone());
            image
        };
        Ok(Some(SystemImage {
            snapshot,
            requested: requested.into(),
            importer: importer_name,
            host,
        }))
    }
    fn first_loader(&mut self, image: &Snapshot) -> Result<Option<usize>, String> {
        self.reject_quarantined(image, 0)?;
        let expected = self.view(image)?;
        let mut found = None;
        for (module, path) in self.paths()? {
            let same_name = normalized_name(&path) == normalized_name(&image.path);
            let mut header_match = 0;
            let header_fault = unsafe {
                sfi_mapped_header_matches(
                    module as *mut c_void,
                    image.bytes.as_ptr(),
                    image.bytes.len(),
                    &mut header_match,
                )
            };
            if !same_name && (header_fault != 0 || header_match == 0) {
                continue;
            }
            // Past the header filter a module is a candidate copy of this image whatever its basename: a mapping or
            // backing that cannot be proved refuses the load (it may be this very DLL, already initialized by another
            // loader under an alias whose pathname changed), instead of initializing the DLL a second time.
            let refuse = |error: String| {
                format!(
                    "mapped {} matches {} but its backing cannot be proved ({error}); refusing to initialize it again",
                    path.display(),
                    image.name
                )
            };
            let Some(matched) = self
                .candidate(module, &path, image, expected)
                .map_err(refuse)?
            else {
                continue;
            };
            if matched {
                found = Some(module);
            } else if same_name {
                return Err(format!(
                    "preloaded {} has different backing identity/content",
                    normalized_name(&path)
                ));
            }
        }
        Ok(found)
    }
    fn load(&mut self, _: &Path, _: u64) -> Result<usize, BackendLoadError> {
        Err(loader_refusal(
            "Windows loads require a closure-bound transaction".into(),
        ))
    }
    fn validate_loaded(&mut self, module: usize, image: &Snapshot) -> Result<(), String> {
        self.reject_quarantined(image, module)?;
        let expected = self.view(image)?;
        if same(module, expected)? {
            return Ok(());
        }
        if let Some(copy) = self.copies.get(&module)
            && copy.sha256 == digest_bytes(&image.digest)?
            && same(module, copy.view)?
        {
            return Ok(());
        }
        Err("actual mapped backing does not match the inspected identity/content".into())
    }
    fn load_permit(
        &mut self,
        permit: &LoadPermit<'_>,
        owner: u64,
    ) -> Result<LoadOutcome, BackendLoadError> {
        let epoch = unsafe { sfi_monitor_epoch() };
        self.copies.clear();
        let bound: Vec<_> = permit.bound_files().collect();
        let paths = bound
            .iter()
            .map(|s| loader_path(&s.path))
            .collect::<Result<Vec<_>, _>>()
            .map_err(loader_refusal)?;
        for image in &bound {
            self.first_loader(image).map_err(loader_refusal)?;
        }
        let names: Vec<_> = bound
            .iter()
            .map(|s| wide_text(&normalized_name(&s.path)))
            .collect();
        let mut bindings = Vec::new();
        let mut copies: Vec<_> = self.copies.values().copied().collect();
        for (index, image) in bound.iter().enumerate() {
            let view = self.view(image).map_err(loader_refusal)?;
            let sha256 = digest_bytes(&image.digest).map_err(loader_refusal)?;
            bindings.push(Binding {
                path: paths[index].as_ptr(),
                name: names[index].as_ptr(),
                file: image._file.as_raw_handle(),
                view,
                flags: permit.closure.nodes[index].flags,
                reserved: 0,
                sha256,
            });
            copies.push(CopyProof {
                module: std::ptr::null_mut(),
                view,
                sha256,
            });
        }
        let api_strings: Vec<_> = permit
            .closure
            .apis
            .iter()
            .map(|a| {
                (
                    wide_text(&a.importer),
                    wide_text(&a.requested),
                    wide_text(&a.host),
                )
            })
            .collect();
        let apis: Vec<_> = api_strings
            .iter()
            .map(|(i, r, h)| ApiBinding {
                importer: i.as_ptr(),
                requested: r.as_ptr(),
                host: h.as_ptr(),
            })
            .collect();
        let (mut module, mut foreign, mut may_have_run, mut pin_status, mut failed, mut error) =
            (std::ptr::null_mut(), 0, 0, 0, u32::MAX, 0);
        let fault = Self::with_owner(owner, || unsafe {
            sfi_verified_load(
                bindings.as_ptr(),
                bindings.len() as u32,
                copies.as_ptr(),
                copies.len() as u32,
                apis.as_ptr(),
                apis.len() as u32,
                owner,
                self.after,
                epoch,
                &mut module,
                &mut foreign,
                &mut may_have_run,
                &mut pin_status,
                &mut failed,
                &mut error,
            )
        })
        .map_err(loader_refusal)?;
        if fault != 0 || error != 0 || pin_status < 0 {
            return Err(BackendLoadError::from_load_evidence(
                fault,
                error,
                may_have_run != 0,
                pin_status,
                if let Some(image) = bound.get(failed as usize) {
                    if error == NOT_INITIALIZED {
                        format!(
                            "dependency binding/load refused: {} is mapped but its loader never initialized it (DONT_RESOLVE_DLL_REFERENCES, a failed DllMain, or another loader's load of it still under way)",
                            image.name
                        )
                    } else if error == TOO_MANY_COPIES {
                        format!(
                            "more than 8 identical copies of {} across packages: Kiln watches at most 8 files of one name, so this package's copy is refused before any load",
                            image.name
                        )
                    } else {
                        format!("dependency binding/load refused: {}", image.name)
                    }
                } else {
                    "loader inventory/API-set/observer binding changed or unavailable".into()
                },
            ));
        }
        if module.is_null() {
            return Err(loader_refusal("verified load returned no module".into()));
        }
        Ok(if foreign != 0 {
            LoadOutcome::ForeignOwned(module as usize)
        } else {
            LoadOutcome::Loaded(module as usize)
        })
    }
}
pub(super) fn early_scan() -> Result<Vec<String>, String> {
    WindowsBackend::default().modules()
}
pub(super) fn mark_interpreter(active: bool) {
    unsafe {
        sfi_guard_interpreter(u32::from(active));
    }
}
