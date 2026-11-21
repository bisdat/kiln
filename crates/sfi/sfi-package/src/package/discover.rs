// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sources (both pins): modules/modloader/find_installed_mods.rs:10-159,
// read_mod_manifest.rs:15-126; read_mod_locale_catalogs.rs:7-62.
//! Source registry, not activation or frozen engine admission. All roots are host inputs.
use super::{
    Package as ParsedPackage, PackageDocuments, ParseOptions,
    document::Document,
    manifest::Manifest,
    profiles::{self, ProfileCatalog},
    settings::infer_api_contract,
};
use crate::source::{
    DiskSource, Package as RuntimePackage, PackageSource, ProfileOverlaySource, SourceEntry,
    SourceKind,
};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs, io,
    path::PathBuf,
    rc::Rc,
};

#[derive(Clone, Debug)]
pub struct PackageRoots {
    pub mods: PathBuf,
    pub config: PathBuf,
    pub profiles: PathBuf,
}
/// A parsed package and its original runtime descriptor are composed, not conflated.
#[derive(Clone)]
pub struct PackageRecord {
    directory: String,
    parsed: ParsedPackage,
    runtime: RuntimePackage,
    raw_source: Rc<dyn PackageSource>,
}
impl PackageRecord {
    pub fn new(
        directory: String,
        parsed: ParsedPackage,
        runtime: RuntimePackage,
        raw_source: Rc<dyn PackageSource>,
    ) -> Result<Self, String> {
        if parsed.manifest.id != runtime.id() {
            return Err("parsed and runtime package IDs disagree".into());
        }
        Ok(Self {
            directory,
            parsed,
            runtime,
            raw_source,
        })
    }
    pub fn directory(&self) -> &str {
        &self.directory
    }
    pub fn parsed(&self) -> &ParsedPackage {
        &self.parsed
    }
    pub fn runtime(&self) -> &RuntimePackage {
        &self.runtime
    }
    pub fn raw_source(&self) -> &Rc<dyn PackageSource> {
        &self.raw_source
    }
}
#[derive(Clone, Default)]
pub struct Registry {
    packages: BTreeMap<String, PackageRecord>,
}
impl Registry {
    pub fn get(&self, id: &str) -> Option<&PackageRecord> {
        self.packages.get(id)
    }
    pub fn values(&self) -> impl Iterator<Item = &PackageRecord> {
        self.packages.values()
    }
    pub fn insert(&mut self, record: PackageRecord) -> Result<(), String> {
        let id = record.parsed.manifest.id.clone();
        if self.packages.contains_key(&id) {
            return Err(format!("duplicate mod ID: {id}"));
        }
        self.packages.insert(id, record);
        Ok(())
    }
    /// Read-only copied metadata for the bindings. Includes disabled and other-target packages;
    /// deliberately has no admitted/active bit, VM handle, epoch or callback mutator.
    pub fn snapshot(&self) -> RegistrySnapshot {
        RegistrySnapshot {
            manifests: self
                .packages
                .iter()
                .map(|(id, p)| (id.clone(), p.parsed.manifest.clone()))
                .collect(),
        }
    }
}
#[derive(Clone, Debug, Default)]
pub struct RegistrySnapshot {
    manifests: BTreeMap<String, Manifest>,
}
impl RegistrySnapshot {
    pub fn has_mod(&self, id: &str) -> bool {
        self.manifests.contains_key(id)
    }
    pub fn get(&self, id: &str) -> Option<&Manifest> {
        self.manifests.get(id)
    }
    pub fn ids(&self) -> impl Iterator<Item = &str> {
        self.manifests.keys().map(String::as_str)
    }
}
#[derive(Debug)]
pub struct DiscoveryIssue {
    pub directory: String,
    pub mod_id: Option<String>,
    pub detail: String,
}
#[derive(Default)]
pub struct DiscoveryReport {
    pub registry: Registry,
    pub errors: Vec<DiscoveryIssue>,
    pub profile_errors: Vec<String>,
}
/// Disk convenience adapter. There are no implicit game/config/SF state paths.
/// SF creates the mods root before reading it; only that explicit root is created.
pub fn discover(
    roots: &PackageRoots,
    options: ParseOptions,
    selections: &[profiles::DllSelection],
) -> io::Result<DiscoveryReport> {
    fs::create_dir_all(&roots.mods)?;
    let source = DiskSource::new(&roots.mods)?;
    // SF profile_for_package suppresses catalog-directory errors; retain them as
    // diagnostics, without turning unrelated valid packages into rejected packages.
    let catalog = match DiskSource::new(&roots.profiles).and_then(|s| profiles::read_catalog(&s)) {
        Ok(c) => c,
        Err(e) if e.kind() == io::ErrorKind::NotFound => ProfileCatalog::default(),
        Err(e) => ProfileCatalog {
            profiles: Vec::new(),
            errors: vec![e.to_string()],
        },
    };
    discover_source(
        &source,
        |e| Ok(Rc::new(DiskSource::new(roots.mods.join(&e.path))?) as Rc<dyn PackageSource>),
        options,
        &catalog,
        selections,
    )
}
/// Injected sources preserve root-entry order and each package's own source identity.
/// Archives may be represented by the opener, but root non-directories never open.
pub fn discover_source(
    root: &dyn PackageSource,
    mut open: impl FnMut(&SourceEntry) -> io::Result<Rc<dyn PackageSource>>,
    options: ParseOptions,
    catalog: &ProfileCatalog,
    selections: &[profiles::DllSelection],
) -> io::Result<DiscoveryReport> {
    let mut report = DiscoveryReport {
        profile_errors: catalog.errors.clone(),
        ..Default::default()
    };
    let mut duplicate_ids = BTreeSet::new();
    for entry in root.read_dir("")? {
        let entry = match entry {
            Ok(e) => e,
            Err(e) => {
                report.errors.push(DiscoveryIssue {
                    directory: String::new(),
                    mod_id: None,
                    detail: e.to_string(),
                });
                continue;
            }
        };
        if entry.name.starts_with('.') || entry.kind != SourceKind::Directory {
            continue;
        }
        let load = (|| {
            let source = open(&entry).map_err(|e| e.to_string())?;
            // SF's exists test ignores folders without a manifest, even for a native-only package.
            match source.metadata("mod.json") {
                Ok(_) => {}
                Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(None),
                Err(e) => return Err(e.to_string()),
            }
            let selected = profiles::selection_for_package(selections, &entry.name);
            read_package(&entry.name, source, options, catalog, selected).map(Some)
        })();
        match load {
            Ok(Some(record)) => {
                let id = record.parsed.manifest.id.clone();
                if report.registry.get(&id).is_some() {
                    duplicate_ids.insert(id.clone());
                    report.errors.push(DiscoveryIssue {
                        directory: entry.name,
                        mod_id: Some(id.clone()),
                        detail: format!("duplicate mod ID: {id}"),
                    });
                } else {
                    report
                        .registry
                        .insert(record)
                        .expect("checked unique package ID");
                }
            }
            Ok(None) => {}
            Err(detail) => report.errors.push(DiscoveryIssue {
                directory: entry.name,
                mod_id: None,
                detail,
            }),
        }
    }
    for id in duplicate_ids {
        report.registry.packages.remove(&id);
    }
    Ok(report)
}
pub fn read_package(
    directory: &str,
    raw_source: Rc<dyn PackageSource>,
    options: ParseOptions,
    catalog: &ProfileCatalog,
    selected: Option<&str>,
) -> Result<PackageRecord, String> {
    // SF reads and parses mod.json before it opens extended.mod.json, each error named
    // by its file (read_mod_manifest.rs:9-24, both pins).
    let manifest = raw_source
        .read_file("mod.json")
        .map_err(|e| format!("mod.json: {e}"))
        .and_then(|bytes| Document::read("mod.json", &bytes))?;
    let extension = match raw_source.read_file("extended.mod.json") {
        Ok(bytes) => Some(Document::read("extended.mod.json", &bytes)?),
        Err(e) if e.kind() == io::ErrorKind::NotFound => None,
        Err(e) => return Err(format!("extended.mod.json: {e}")),
    };
    let documents = PackageDocuments {
        manifest,
        extension,
    };
    let mut m = documents.manifest.value().clone();
    let mut e = documents.extension.as_ref().map(|d| d.value().clone());
    let profile = profiles::select_profile(
        &catalog.profiles,
        directory,
        m.get("id").and_then(crate::json::Value::as_str),
        selected,
    );
    let source = if let Some(profile) = profile {
        profiles::apply_overlay(&mut m, &mut e, profile);
        // Preserve original key spellings; the shared overlay supplies SF's Path
        // equality, listings and overlapping file/directory predicates. Raw source
        // remains separate for revisions/fingerprints.
        Rc::new(ProfileOverlaySource::new(
            raw_source.clone(),
            &profile
                .package_files
                .iter()
                .map(|(p, v)| (p.clone(), v.clone()))
                .collect(),
        )) as Rc<dyn PackageSource>
    } else {
        raw_source.clone()
    };
    let effective_m = serde_json::to_vec(&m).map_err(|e| e.to_string())?;
    let effective_e = e
        .as_ref()
        .map(serde_json::to_vec)
        .transpose()
        .map_err(|e| e.to_string())?;
    let mut parsed = ParsedPackage::parse(&effective_m, effective_e.as_deref(), options)?;
    // Raw documents and effective normalized values intentionally remain distinct.
    parsed.documents = documents;
    let mut lua = String::new();
    collect_lua_sources(source.as_ref(), "src", &mut lua)?;
    infer_api_contract(&mut parsed.manifest, &lua);
    let locales = locale_files(source.as_ref())?
        .into_iter()
        .map(|entry| {
            let shown = listed_path_display(&entry.path);
            source
                .read_file(&entry.path)
                .map(|b| (shown.clone(), b))
                .map_err(|e| format!("{shown}: {e}"))
        })
        .collect::<Result<Vec<_>, _>>()?;
    parsed.set_locales(
        locales
            .iter()
            .map(|(name, b)| (name.as_str(), b.as_slice())),
    )?;
    let runtime = RuntimePackage::new(&parsed.manifest.id, source);
    PackageRecord::new(directory.into(), parsed, runtime, raw_source)
}
/// DFS in original source listing order, no inserted sorting. Failed file opens
/// are skipped, but invalid UTF-8 or a non-NotFound directory failure rejects.
pub fn collect_lua_sources(
    source: &dyn PackageSource,
    directory: &str,
    output: &mut String,
) -> Result<(), String> {
    collect_lua_sources_inner(source, directory, output, &mut BTreeSet::new())
}
fn collect_lua_sources_inner(
    source: &dyn PackageSource,
    directory: &str,
    output: &mut String,
    active: &mut BTreeSet<PathBuf>,
) -> Result<(), String> {
    // Parity exception (no hang reaches Lua): both pins read_mod_manifest.rs:104-126 can
    // recurse forever through a contained directory symlink. Source entries use
    // resolved-parent paths, so a cycle repeats a traversal path. Guard only the
    // active branch: legitimate sibling aliases must still be traversed again.
    let identity = PathBuf::from(directory);
    if !active.insert(identity.clone()) {
        return Err(format!("source directory cycle: {directory}"));
    }
    let result = (|| {
        let entries = match source.read_dir(directory) {
            Ok(e) => e,
            Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(()),
            Err(e) => return Err(e.to_string()),
        };
        for entry in entries.into_iter().flatten() {
            // The SF wrapper's is_directory treats even exact override keys as
            // directories (both pins mod_package_filesystem.rs:179-186). Do not
            // silently start inferring from virtual files when merging listings.
            if source.is_directory(&entry.path) {
                collect_lua_sources_inner(source, &entry.path, output, active)?;
            } else if std::path::Path::new(&entry.path)
                .extension()
                .is_some_and(|ext| ext == "lua")
            {
                let Ok(bytes) = source.read_file(&entry.path) else {
                    continue;
                };
                output.push_str(std::str::from_utf8(&bytes).map_err(|_| {
                    io::Error::new(
                        io::ErrorKind::InvalidData,
                        "stream did not contain valid UTF-8",
                    )
                    .to_string()
                })?);
                output.push('\n');
            }
        }
        Ok(())
    })();
    active.remove(&identity);
    result
}
/// Shared immediate sorted lowercase-.json selection for locale load/raw revision.
/// A listed package path as SF prints it in an error. SF's reader builds listing paths
/// with the platform separator: the disk listing strips the canonical root from
/// std::fs::read_dir paths and the archive listing joins with camino's Utf8Path::join
/// (mod_package_filesystem.rs:261-298,379-390, both pins), so on Windows a catalog prints
/// as `locales\de.json`. Source entries always use `/`. Profile overrides never reach this
/// (SF's is_directory is true for every override path, so they are never catalogs).
fn listed_path_display(path: &str) -> String {
    if cfg!(windows) {
        path.replace('/', "\\")
    } else {
        path.to_owned()
    }
}
pub(crate) fn locale_files(source: &dyn PackageSource) -> Result<Vec<SourceEntry>, String> {
    // Both pins read_mod_locale_catalogs.rs:8-24 use the independent predicates,
    // not metadata: an exact profile override is both a file and a directory.
    if !source.exists("locales") {
        return Ok(Vec::new());
    }
    if !source.is_directory("locales") {
        return Err("locales must be a directory".into());
    }
    let mut files = source
        .read_dir("locales")
        .map_err(|e| format!("locales: {e}"))?
        .into_iter()
        .collect::<io::Result<Vec<_>>>()
        .map_err(|e| format!("locales: {e}"))?;
    files.retain(|e| {
        std::path::Path::new(&e.path)
            .extension()
            .is_some_and(|ext| ext == "json")
            && !source.is_directory(&e.path)
    });
    files.sort_by(|a, b| a.path.cmp(&b.path));
    Ok(files)
}
