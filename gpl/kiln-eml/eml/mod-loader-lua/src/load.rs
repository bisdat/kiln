// kiln-eml: preserve preparation causes and paths through the run and player status.
use anyhow::Context;

use std::{fs::File, io::BufReader, rc::Rc};

use kfc::{container::{KFCCursor, KFCFile, KFCReader, KFCReaderOptions, KFCWriteOptions, KFCWriter}, reflection::TypeRegistry};

use crate::{alias::{Path, PathBuf}, log::{debug, error, info, warn}};

pub fn load_kfc_file(
    kfc_path: impl AsRef<Path>
) -> anyhow::Result<Rc<KFCFile>> {
    let kfc_path = kfc_path.as_ref();

    match KFCFile::from_path(kfc_path, false) {
        Ok(file) => Ok(Rc::new(file)),
        Err(e) => {
            error!(
                error = %e,
                path = ?kfc_path,
                "Failed to open KFC file",
            );
            Err(anyhow::Error::new(e).context(format!("load_kfc_file: could not read {kfc_path} as a game archive")))
        }
    }
}

pub fn create_reader(
    dir: &Path,
    file_name: &str,
) -> anyhow::Result<KFCCursor<KFCReader>> {
    // kiln-eml: use display paths, as the plugin does (support reports redact these).
    let header = dir.join(format!("{file_name}.kfc.bak"));
    let resources = dir.join(format!("{file_name}.kfc_resources"));
    match KFCReader::new_with_options(
        dir,
        file_name,
        KFCReaderOptions {
            kfc_extension: "kfc.bak",
            ..Default::default()
        }
    ).and_then(|reader| reader.into_cursor()) {
        Ok(reader) => Ok(reader),
        Err(e) => {
            error!(
                path = ?dir,
                file_name = %file_name,
                error = %e,
                "Failed to create KFC reader",
            );
            Err(anyhow::Error::new(e).context(format!("create_reader: could not open {header} and {resources}")))
        }
    }
}

pub fn create_writer(
    dir: &Path,
    file_name: &str,
    type_registry: &Rc<TypeRegistry>,
    ref_file: &Rc<KFCFile>,
) -> anyhow::Result<crate::transaction::PatchWriter> {
    create_writer_over(dir, file_name, type_registry, ref_file, None)
}

/// kiln-eml: `create_writer` over inputs witnessed earlier (the startup host's): the witness is checked and
/// taken over instead of capturing the inputs again, so a stage does not hash the working baseline's resource backup a
/// second time.
pub(crate) fn create_writer_over(
    dir: &Path,
    file_name: &str,
    type_registry: &Rc<TypeRegistry>,
    ref_file: &Rc<KFCFile>,
    witnessed: Option<&crate::transaction::InputWitness>,
) -> anyhow::Result<crate::transaction::PatchWriter> {
    // kiln-eml: capture a canonical target and its input witness before recording/creating private scratch.
    let canonical = std::fs::canonicalize(dir).with_context(|| format!("create_writer: could not resolve {dir}"))?;
    let canonical = PathBuf::from_path_buf(canonical).map_err(|_| anyhow::anyhow!("create_writer: non UTF-8 target"))?;
    let dir = canonical.as_path();
    let target = match witnessed {
        Some(witness) => {
            if !witness.covers(dir.as_std_path(), file_name, ref_file.containers().len()) {
                anyhow::bail!("create_writer: the input witness was captured for another archive than {dir}/{file_name}");
            }
            witness.check(dir.as_std_path()).with_context(|| format!("create_writer: archive inputs in {dir} changed since they were witnessed"))?;
            crate::transaction::TargetSnapshot::witnessed(witness.clone(), ref_file.clone())
        }
        None => crate::transaction::TargetSnapshot::capture(dir.as_std_path(), file_name, ref_file.clone()),
    }
    .with_context(|| format!("create_writer: could not capture archive inputs in {dir}"))?;
    // kiln-eml: only the private staging directory is writable by the KFC writer.
    let stage = dir.join(format!(".kiln-eml-stage-{}", uuid::Uuid::new_v4()));
    crate::cleanup::record(dir.as_std_path(), file_name, stage.file_name().ok_or_else(|| anyhow::anyhow!("create_writer: invalid stage path {stage}"))?).with_context(|| format!("create_writer: could not prepare stage {stage}"))?;
    failpoint!("stage.recorded", stage);
    std::fs::create_dir(&stage).with_context(|| format!("create_writer: could not create stage {stage}"))?;
    failpoint!("stage.created", stage);
    #[cfg(test)]
    crate::cleanup::test_point(&crate::cleanup::AFTER_STAGE_CREATED, stage.as_std_path());
    // kiln-eml: hold the new stage and bind the write-ahead record to the HELD directory's identity, and only when it is
    // still empty: a directory swapped in after create_dir is not Kiln's. Then the record stays unbound (kept and
    // reported as eml-cleanup-unbound, never deletion authority) and no writer is built on it.
    let stage_name = stage.file_name().ok_or_else(|| anyhow::anyhow!("create_writer: invalid stage path {stage}"))?;
    let held = crate::transaction::HeldDir::open(stage.as_std_path())
        .with_context(|| format!("create_writer: could not hold stage {stage}"))?;
    if !held.children().with_context(|| format!("create_writer: could not list stage {stage}"))?.is_empty() {
        anyhow::bail!("create_writer: stage {stage} was not empty right after it was created; it is not Kiln's, left unbound \
                       ({}) and not used", crate::cleanup::CLEANUP_UNBOUND);
    }
    let identity = held.identity().with_context(|| format!("create_writer: could not capture stage identity {stage}"))?;
    crate::cleanup::bind_stage(dir.as_std_path(), stage_name, &identity)
        .with_context(|| format!("create_writer: could not bind cleanup proof to stage {stage}"))?;
    failpoint!("stage.bound", stage);
    #[cfg(test)]
    crate::cleanup::test_point(&crate::cleanup::AFTER_STAGE_BOUND, stage.as_std_path());
    // kiln-eml: the writer keeps holding the stage until it is dropped. Every staging copy, the archive writer, the checks,
    // the publication's moves and the final clean-up reach it through the hold (PatchWriter::at), never by its path: a
    // directory swapped in at the path after binding is never written, published or cleaned.
    let mut patch = crate::transaction::PatchWriter::staged(
        dir.into(), stage.clone().into_std_path_buf(), file_name.into(), target, held,
    ).with_context(|| format!("create_writer: could not capture stage identity {stage}"))?;
    let at = patch.at().to_path_buf();
    let backup = dir.join(format!("{file_name}.kfc.bak"));
    crate::transaction::stage_copy(backup.as_std_path(),
        &at.join(crate::transaction::VANILLA_HEADER)).with_context(|| format!("create_writer: could not copy {backup} to {stage}"))?;
    crate::transaction::stage_copy(&at.join(crate::transaction::VANILLA_HEADER),
        &at.join(format!("{file_name}.kfc"))).with_context(|| format!("create_writer: could not copy staged header in {stage}"))?;
    let resources_backup = dir.join(format!("{file_name}.kfc_resources.bak"));
    let resources = if resources_backup.exists() { resources_backup } else { dir.join(format!("{file_name}.kfc_resources")) };
    crate::transaction::stage_copy(resources.as_std_path(),
        &at.join(format!("{file_name}.kfc_resources"))).with_context(|| format!("create_writer: could not copy {resources} to {stage}"))?;
    let writer = match KFCWriter::new_incremental_with_options(
        &at,
        file_name,
        ref_file.clone(),
        type_registry.clone(),
        KFCWriteOptions {
            overwrite_containers: true,
            // kiln-eml: also empty the padding .dat files (beyond the game's own and this run's). Without it, a
            // container a previous run filled (a mod that called create_content, since removed) keeps its bytes while
            // the new header lists it as empty, and the game refuses to start ("Container ..._010.dat has wrong size
            // 4'096 (expected: 0)", seen on a lab server, 2026-11-07).
            truncate_containers: true,
            ..Default::default()
        }
    ) {
        Ok(writer) => writer,
        Err(e) => {
            error!(
                path = ?dir,
                file_name = %file_name,
                error = %e,
                "Failed to create KFC writer",
            );
            return Err(anyhow::Error::new(e).context(format!("create_writer: could not create archive writer in {stage}")));
        }
    };

    patch.writer = Some(writer);
    failpoint!("stage.writer", stage);
    Ok(patch)
}

pub fn create_backup(
    kfc_path: impl AsRef<Path>
) -> anyhow::Result<PathBuf> {
    create_backup_with(kfc_path, crate::ownership::SfHandoff::Live)
}

/// kiln-eml: `create_backup` in ShroudForge handoff mode `mode` (kiln mode: Deferred, which takes no
/// ShroudForge lock; a baseline write changes no live archive, so it owes no handoff either).
pub fn create_backup_with(
    kfc_path: impl AsRef<Path>,
    mode: crate::ownership::SfHandoff,
) -> anyhow::Result<PathBuf> {
    let path = kfc_path.as_ref();
    // kiln-eml: baseline writes happen before final publication. SF's lock/status must protect this shared pair too,
    // or a refused handoff can already have replaced the backups an active SF preparation is using.
    let root = path.parent().ok_or_else(|| anyhow::anyhow!("create_backup: archive has no parent: {path}"))?;
    let stem = path.file_stem().ok_or_else(|| anyhow::anyhow!("create_backup: archive has no name: {path}"))?;
    let _sf = crate::ownership::before_baseline(root.as_std_path(), stem, mode).map_err(|error| {
        error!(%error, "Refusing vanilla backup changes while ShroudForge owns the archive writer");
        anyhow::Error::new(error).context("create_backup: ShroudForge owns the archive writer")
    })?;
    let mut bak_path = path.to_string();
    bak_path.push_str(".bak");
    let bak_path = Path::new(&bak_path);

    // kiln-eml: reject links and malformed data; a version change refreshes the backup atomically.
    crate::transaction::regular(path.as_std_path()).with_context(|| format!("create_backup: archive {path} is not a regular file (links refused)"))?;
    if bak_path.exists() { crate::transaction::regular(bak_path.as_std_path()).with_context(|| format!("create_backup: backup {bak_path} is not a regular file (links refused)"))?; }
    // kiln-eml: the checks below read the backup pair (and a new backup copies the live pair): hundreds of MiB on a real
    // client, so say so before it starts (the run's log would otherwise be silent for as long as it takes)
    let size = |p: &Path| std::fs::metadata(p.as_std_path()).map_or(0, |m| m.len());
    let resources_bak = path.with_extension("kfc_resources.bak");
    if bak_path.exists() {
        info!("Checking the vanilla backup against its seal: {} read, twice", crate::progress::human(size(bak_path) + size(&resources_bak)));
    }
    // A crash can leave the old seal after both new backups were published. Only repair it when both backups
    // are byte-identical to the untouched live files; never bless a mismatching backup from a patched install.
    if bak_path.exists() && !validate_backup(path, bak_path) && backups_equal_live(path, bak_path) {
        write_backup_seal(path, bak_path)?;
    }
    // kiln-eml: another writer replaced the whole pair after a game update and left this seal behind (an installed
    // ShroudForge's Updater does). The backups then have other lengths than the seal recorded: a damaged one keeps
    // its length and stays refused below. Such a pair is checked like an unsealed one (its version is the live
    // one, the live resource prefix equals it) and sealed again; never promoted from a patched header of this build.
    if bak_path.exists() && !validate_backup(path, bak_path) && replaced_since_sealed(path, bak_path)
        && KFCFile::get_version_tag(bak_path).ok() == KFCFile::get_version_tag(path).ok() && validate_unsealed(path, bak_path) {
        warn!("The vanilla backup pair was replaced since it was sealed (another loader after a game update); it matches the live resource file and is sealed again");
        write_backup_seal(path, bak_path)?;
    }
    if bak_path.exists() && !validate_backup(path, bak_path) {
        let old = KFCFile::get_version_tag(bak_path).with_context(|| format!("create_backup: could not read backup {bak_path} as a game archive"))?;
        let current = KFCFile::get_version_tag(path).with_context(|| format!("create_backup: could not read version from {path}"))?;
        // kiln-eml: diagnose only an already refused same-version pair; retain update/recovery behavior.
        if old == current {
            let resources = path.with_extension("kfc_resources.bak");
            if resources.exists() {
                crate::transaction::regular(resources.as_std_path()).with_context(|| format!("create_backup: backup {resources} is not a regular file (links refused)"))?;
                File::open(&resources).with_context(|| format!("create_backup: could not read backup {resources}"))?;
            }
            load_kfc_file(bak_path).context("create_backup")?;
            anyhow::bail!("create_backup: backup pair {bak_path} and {resources} does not match the seal Kiln recorded for the game's own files; verify the game files in Steam, then start through Kiln again");
        }
    }
    if !bak_path.exists() || !validate_backup(path, bak_path) {
        let live = path.with_extension("kfc_resources");
        info!(
            "Backing up the vanilla game data: {} and {} ({}) are copied, then read back for the seal",
            path.file_name().unwrap_or("the header"), live.file_name().unwrap_or("the resource file"),
            crate::progress::human(size(path) + size(&live))
        );
        load_kfc_file(path).context("create_backup")?;
        // Snapshot resources before committing the matching header backup. Existing EML-only backups may lack this.
        let resources = path.with_extension("kfc_resources");
        let resources_backup = path.with_extension("kfc_resources.bak");
        crate::transaction::atomic_copy(resources.as_std_path(), resources_backup.as_std_path()).with_context(|| format!("create_backup: could not copy {resources} to {resources_backup}"))?;
        crate::transaction::atomic_copy(path.as_std_path(), bak_path.as_std_path()).with_context(|| format!("create_backup: could not copy {path} to {bak_path}"))?;
        write_backup_seal(path, bak_path)?;
    } else if path.with_extension("kfc_resources.bak").exists() && !seal_path(path).exists() {
        // Adopt a legacy unsealed pair only after validate_backup checked its live resource prefix.
        write_backup_seal(path, bak_path)?;
    }

    Ok(bak_path.to_path_buf())
}

pub fn restore_backup(
    kfc_path: impl AsRef<Path>,
) -> Result<(), ()> {
    restore_backup_with(kfc_path, crate::ownership::SfHandoff::Live)
}

/// kiln-eml: `restore_backup` in ShroudForge handoff mode `mode` (kiln mode: Deferred).
pub fn restore_backup_with(
    kfc_path: impl AsRef<Path>,
    mode: crate::ownership::SfHandoff,
) -> Result<(), ()> {
    restore_backup_as(kfc_path, mode, crate::ownership::RestoreProof::Receipt)
}

/// kiln-eml: `restore_backup_with` over the proof `proof` that the live pair may be replaced (kiln mode's verified
/// BASE also restores Kiln's bundled ShroudForge's recorded output: `StartupHost::restore_base`).
pub(crate) fn restore_backup_as(
    kfc_path: impl AsRef<Path>,
    mode: crate::ownership::SfHandoff,
    proof: crate::ownership::RestoreProof,
) -> Result<(), ()> {
    let path = kfc_path.as_ref();
    let mut bak_path = path.to_string();
    bak_path.push_str(".bak");
    let bak_path = Path::new(&bak_path);

    if !bak_path.exists() {
        warn!(
            path = ?bak_path,
            "Backup file does not exist, cannot restore.",
        );
        return Err(());
    }

    if !bak_path.is_file() {
        error!(
            path = ?bak_path,
            "Backup path is not a file, please remove it manually.",
        );
        return Err(());
    }

    if !validate_backup(path, bak_path) {
        warn!(
            path = ?path,
            backup_path = ?bak_path,
            "Backup file is not compatible with the current KFC file, cannot restore.",
        );
        return Err(());
    }

    let root = path.parent().ok_or(())?;
    let stem = path.file_stem().ok_or(())?;
    // Proof is rechecked under SF's lock. Hold it through both copies AND container cleanup: SF must not append
    // new containers between reading the restored header's range and removing the old generation's extras.
    let _sf = crate::ownership::before_restore_as(root.as_std_path(), stem, mode, proof).map_err(|error| {
        error!(%error, "Refusing archive restore: ownership or ShroudForge prepared-output state could not be safely checked");
    })?;
    let added = crate::ownership::live_containers(root.as_std_path(), stem).unwrap_or_else(|error| {
        tracing::warn!(%error, "The receipt's containers cannot be read; the restore leaves them in place");
        None
    });
    if let Err(e) = crate::transaction::atomic_copy(bak_path.as_std_path(), path.as_std_path()) {
        error!(
            path = ?path,
            backup_path = ?bak_path,
            error = %e,
            "Failed to restore backup file",
        );
        return Err(());
    } else {
        info!(
            path = ?path,
            backup_path = ?bak_path,
            "Backup file restored successfully",
        );
    }

    #[cfg(test)]
    crate::transaction::DURING_RESTORE.with(|check| { if let Some(check) = check.take() { check(root.as_std_path()); } });
    // kiln-eml: new backups restore the resource file exactly too, after the vanilla header is live.
    let resources_backup = path.with_extension("kfc_resources.bak");
    if resources_backup.exists() {
        crate::transaction::atomic_copy(resources_backup.as_std_path(), path.with_extension("kfc_resources").as_std_path())
            .map_err(|_| ())?;
    }
    if let Some(added) = added && let Err(error) = crate::ownership::remove_unreferenced(root.as_std_path(), stem, &added) {
        tracing::warn!(%error, "The vanilla data is back; container files EML had added were left in place");
    }
    Ok(())
}

/// kiln-eml: the seal is there, and the backups' lengths are not the ones it recorded (a game update changes the header
/// and the resource file; a damaged file keeps its length).
fn replaced_since_sealed(path: &Path, bak: &Path) -> bool {
    let seal = seal_path(path);
    if !seal.metadata().is_ok_and(|m| m.is_file() && m.len() <= 1024) { return false; }
    let Ok(bytes) = std::fs::read(&seal) else { return false };
    let Ok(sealed) = serde_json::from_slice::<BackupSeal>(&bytes) else { return false };
    let len = |p: &Path| std::fs::metadata(p).map(|m| m.len()).ok();
    len(bak).is_some_and(|l| l != sealed.header.0) || len(&path.with_extension("kfc_resources.bak")).is_some_and(|l| l != sealed.resources.0)
}

/// kiln-eml: `validate_backup` for the pair as if it had no seal (the rule for a pair another writer made).
fn validate_unsealed(path: &Path, bak: &Path) -> bool {
    validate(path, bak, false)
}

pub fn validate_backup(
    kfc_path: &Path,
    kfc_path_bak: &Path
) -> bool {
    validate(kfc_path, kfc_path_bak, true)
}

fn validate(
    kfc_path: &Path,
    kfc_path_bak: &Path,
    use_seal: bool,
) -> bool {
    if crate::transaction::regular(kfc_path_bak.as_std_path()).is_err() { return false; }
    let version_bak = match KFCFile::from_path(kfc_path_bak, false) {
        Ok(file) => file.game_version().to_owned(),
        Err(_) => return false,
    };

    let version = match KFCFile::get_version_tag(kfc_path) {
        Ok(file) => file,
        Err(_) => return false,
    };

    if version != version_bak { return false; }
    let Ok(backup) = KFCFile::from_path(kfc_path_bak, false) else { return false; };
    let resource_backup = kfc_path.with_extension("kfc_resources.bak");
    if resource_backup.exists() {
        if crate::transaction::regular(resource_backup.as_std_path()).is_err() { return false; }
        let Ok(meta) = resource_backup.metadata() else { return false; };
        if backup.resource_chunks().iter().any(|c| c.offset.checked_add(c.size).is_none_or(|end| end > meta.len())) {
            return false;
        }
        let seal = seal_path(kfc_path);
        if use_seal && seal.exists() {
            if crate::transaction::regular(seal.as_std_path()).is_err() { return false; }
            if !seal.metadata().is_ok_and(|m| m.len() <= 1024) { return false; }
            let Ok(bytes) = std::fs::read(&seal) else { return false; };
            let Ok(expected) = serde_json::from_slice::<BackupSeal>(&bytes) else { return false; };
            if backup_seal(kfc_path, kfc_path_bak).ok().as_ref() != Some(&expected) { return false; }
        } else {
            // Old versions had no checksum. Refuse to replace an intact live prefix with different, unverified
            // bytes. New pairs are sealed before the first patch, including exact file lengths and padding.
            use std::io::Read;
            let len = backup.resource_chunks().iter().map(|c| c.offset + c.size).max().unwrap_or(0);
            let Ok(live) = File::open(kfc_path.with_extension("kfc_resources")) else { return false; };
            let Ok(saved) = File::open(&resource_backup) else { return false; };
            let (Ok(a), Ok(b)) = (fingerprint_reader(live.take(len)), fingerprint_reader(saved.take(len))) else { return false; };
            if a != b || a.0 != len { return false; }
        }
    } else if seal_path(kfc_path).exists() {
        return false; // a sealed pair must never silently degrade to a legacy header-only backup (with or without use_seal)
    }
    true
}

#[derive(serde::Serialize, serde::Deserialize, PartialEq, Eq)]
struct BackupSeal { header: (u64, u64), resources: (u64, u64) }

fn seal_path(path: &Path) -> PathBuf { path.with_extension("kfc.bak.integrity") }

fn fingerprint_reader(mut reader: impl std::io::Read) -> std::io::Result<(u64, u64)> {
    // kiln-eml: same CRC-64, sixteen-way tables (cache/prints.rs); the backups are hundreds of MiB
    let mut digest = crate::cache::CRC64.digest();
    let mut len = 0;
    let mut buf = [0u8; 65536];
    loop {
        let n = reader.read(&mut buf)?;
        if n == 0 { return Ok((len, digest.finalize())); }
        len += n as u64;
        digest.update(&buf[..n]);
    }
}

fn backup_seal(path: &Path, bak: &Path) -> std::io::Result<BackupSeal> {
    Ok(BackupSeal {
        header: fingerprint_reader(File::open(bak)?)?,
        resources: fingerprint_reader(File::open(path.with_extension("kfc_resources.bak"))?)?,
    })
}

fn write_backup_seal(path: &Path, bak: &Path) -> anyhow::Result<()> {
    // kiln-eml: seal failures are preparation failures too; retain the I/O source.
    let seal = seal_path(path);
    let pair = backup_seal(path, bak).with_context(|| format!("create_backup: could not read backup pair {bak} and {}", path.with_extension("kfc_resources.bak")))?;
    let bytes = serde_json::to_vec(&pair).context("create_backup: could not serialize backup seal")?;
    crate::transaction::atomic_write(seal.as_std_path(), &bytes).with_context(|| format!("create_backup: could not write backup seal {seal}"))
}

fn backups_equal_live(path: &Path, bak: &Path) -> bool {
    let resources = path.with_extension("kfc_resources");
    let resource_backup = path.with_extension("kfc_resources.bak");
    [path, bak, &resources, &resource_backup].iter().all(|p| crate::transaction::regular(p.as_std_path()).is_ok())
        && KFCFile::from_path(path, false).is_ok()
        && (|| -> std::io::Result<bool> {
            Ok(fingerprint_reader(File::open(path)?)? == fingerprint_reader(File::open(bak)?)?
                && fingerprint_reader(File::open(resources)?)? == fingerprint_reader(File::open(resource_backup)?)?)
        })().unwrap_or(false)
}

pub fn load_type_registry(
    game_dir: &Path,
    cache_dir: &Path,
    file_name: &str,
) -> anyhow::Result<(TypeRegistry, bool)> {
    if let Err(e) = std::fs::create_dir_all(cache_dir) {
        warn!(
            error = %e,
            path = ?cache_dir,
            "Failed to create cache directory, unable to load or save type registry",
        );
        return Err(anyhow::Error::new(e).context(format!("load_type_registry: could not create cache directory {cache_dir}")));
    }

    let types_path = cache_dir.join(format!("{file_name}-types.json"));
    let exe_path = game_dir.join(file_name).with_extension("exe");
    let kfc_path = game_dir.join(file_name).with_extension("kfc");

    let type_registry = match File::open(&types_path) {
        Ok(file) => {
            let reader = BufReader::new(file);

            match serde_json::from_reader::<_, TypeRegistry>(reader) {
                Ok(registry) => Some(registry),
                Err(e) => {
                    debug!(
                        error = %e,
                        path = ?types_path,
                        "Failed to read type registry from file, attempting to extract types...",
                    );

                    None
                },
            }
        }
        Err(e) => {
            debug!(
                error = %e,
                path = ?types_path,
                "Failed to read type registry from file, attempting to extract types...",
            );

            None
        },
    };

    let version_tag = match KFCFile::get_version_tag(&kfc_path) {
        Ok(tag) => Some(tag),
        Err(e) => {
            warn!(
                error = %e,
                path = ?types_path,
                kfc_path = ?kfc_path,
                "Failed to get version tag from KFC file, assuming types are outdated, attempting to extract types...",
            );

            None
        }
    };

    let type_registry = match type_registry {
        Some(type_registry) => {
            if let Some(version_tag) = &version_tag {
                if version_tag != &type_registry.version {
                    warn!(
                        path = ?types_path,
                        kfc_path = ?kfc_path,
                        "Type registry is outdated, attempting to extract types again..."
                    );

                    None
                } else {
                    Some(type_registry)
                }
            } else {
                None
            }
        },
        None => None
    };

    let (type_registry, is_dirty) = match type_registry {
        Some(type_registry) => (type_registry, false),
        None => match {
            // kiln-eml: reads the whole exe and takes a moment (once per game version): say so
            let bytes = std::fs::metadata(exe_path.as_std_path()).map_or(0, |m| m.len());
            info!("Reading the game's reflection types from {} ({}), once per game version", exe_path.file_name().unwrap_or("the exe"), crate::progress::human(bytes));
            TypeRegistry::load_from_executable(&exe_path)
        } {
            Ok(mut registry) => {
                if let Some(version_tag) = version_tag {
                    registry.version = version_tag;

                    match serde_json::to_string(&registry) {
                        Ok(json) => {
                            if let Err(e) = std::fs::write(&types_path, json) {
                                warn!(
                                    error = %e,
                                    path = ?types_path,
                                    "Failed to write type registry to file",
                                );
                            }
                        }
                        Err(e) => {
                            warn!(
                                error = %e,
                                path = ?types_path,
                                "Failed to serialize type registry to JSON",
                            );
                        }
                    }
                } else {
                    warn!(
                        path = ?types_path,
                        kfc_path = ?kfc_path,
                        "No version tag found, type registry cannot be saved",
                    );
                }

                (registry, true)
            }
            Err(e) => {
                error!(
                    error = %e,
                    path = ?types_path,
                    exe_path = ?exe_path,
                    "Failed to load type registry",
                );

                return Err(anyhow::Error::new(e).context(format!("load_type_registry: could not extract types from {exe_path}")));
            }
        }
    };

    info!(
        path = ?types_path,
        exe_path = ?exe_path,
        kfc_path = ?kfc_path,
        version = type_registry.version,
        is_dirty = is_dirty,
        "Type registry loaded successfully",
    );

    Ok((type_registry, is_dirty))
}

pub fn export_lua_definitions(
    cache_dir: &Path,
    type_registry: &TypeRegistry,
    force: bool,
) {
    let lua_cache_dir = cache_dir.join("lua");

    if let Err(e) = std::fs::create_dir_all(&lua_cache_dir) {
        warn!(
            error = %e,
            path = ?lua_cache_dir,
            "Failed to create lua cache directory, unable to generate lua definitions",
        );
    } else {
        let lua_type_def_path = lua_cache_dir.join("types.lua");
        let lua_base_def_path = lua_cache_dir.join("base.lua");

        if force || !lua_type_def_path.exists() {
            let def = crate::definition::generator::generate(type_registry);

            match std::fs::write(&lua_type_def_path, def) {
                Ok(_) => info!(
                    path = ?lua_type_def_path,
                    "Lua type definition file has been generated",
                ),
                Err(e) => {
                    warn!(
                        error = %e,
                        path = ?lua_type_def_path,
                        "Failed to write lua type definition file",
                    );
                }
            }
        }

        if force || !lua_base_def_path.exists() {
            match std::fs::write(&lua_base_def_path, crate::definition::DEFINITION_FILE) {
                Ok(_) => info!(
                    path = ?lua_base_def_path,
                    "Lua base definition file has been generated",
                ),
                Err(e) => {
                    warn!(
                        error = %e,
                        path = ?lua_base_def_path,
                        "Failed to write lua base definition file",
                    );
                }
            }
        }
    }
}

#[cfg(test)]
mod kiln_tests {
    use super::*;
    use std::io::Write;
    fn fixture() -> PathBuf {
        let dir = PathBuf::from_path_buf(std::env::temp_dir()).unwrap().join(format!("kiln-backup-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir(&dir).unwrap();
        File::create(dir.join("g.kfc")).unwrap();
        File::create(dir.join("g.kfc_resources")).unwrap();
        KFCWriter::<Rc<KFCFile>, _>::new(&dir, "g", Rc::new(TypeRegistry::default()), "fixture")
            .unwrap().finalize().unwrap();
        dir
    }
    #[test]
    fn backup_restores_exact_header_and_resources() {
        let dir = fixture();
        let kfc = dir.join("g.kfc");
        let resources = dir.join("g.kfc_resources");
        let original = std::fs::read(&kfc).unwrap();
        let original_resources = std::fs::read(&resources).unwrap();
        create_backup(&kfc).unwrap();
        std::fs::OpenOptions::new().append(true).open(&kfc).unwrap().write_all(b"patch tail").unwrap();
        std::fs::OpenOptions::new().append(true).open(&resources).unwrap().write_all(b"patch tail").unwrap();
        // The restore entry now rechecks the runner's authorization while holding SF's lock. Record these exact
        // test-patch bytes as an owned publication; merely sharing a backup must not authorize their replacement.
        crate::ownership::prepare(dir.as_std_path(), "g", &[kfc.clone().into_std_path_buf(), resources.clone().into_std_path_buf()]).unwrap();
        restore_backup(&kfc).unwrap();
        assert_eq!(std::fs::read(&kfc).unwrap(), original);
        assert_eq!(std::fs::read(&resources).unwrap(), original_resources);
        std::fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn corrupt_same_version_backup_is_neither_restored_nor_refreshed_from_a_patch() {
        let dir = fixture();
        let kfc = dir.join("g.kfc");
        let bak = create_backup(&kfc).unwrap();
        let original = std::fs::read(&kfc).unwrap();
        let mut bad = std::fs::read(&bak).unwrap();
        bad[24..28].copy_from_slice(&0x7fffffffu32.to_le_bytes()); // invalid container table, intact version
        std::fs::write(&bak, &bad).unwrap();
        assert_eq!(KFCFile::get_version_tag(&bak).unwrap(), "fixture");
        assert!(!validate_backup(&kfc, &bak));
        assert!(create_backup(&kfc).is_err());
        assert!(restore_backup(&kfc).is_err());
        assert_eq!(std::fs::read(&kfc).unwrap(), original);
        assert_eq!(std::fs::read(&bak).unwrap(), bad);
        std::fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn corrupt_resource_backup_is_rejected_without_changing_live_files() {
        let dir = fixture();
        let kfc = dir.join("g.kfc");
        create_backup(&kfc).unwrap();
        let resources = dir.join("g.kfc_resources");
        let original = std::fs::read(&resources).unwrap();
        let mut bad = original.clone();
        bad[0] ^= 1; // still the same length, with all header chunk bounds intact
        std::fs::write(dir.join("g.kfc_resources.bak"), bad).unwrap();
        assert!(!validate_backup(&kfc, &dir.join("g.kfc.bak")));
        assert!(create_backup(&kfc).is_err());
        assert!(restore_backup(&kfc).is_err());
        assert_eq!(std::fs::read(resources).unwrap(), original);
        std::fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn interrupted_backup_seal_publication_can_be_recovered_before_patching() {
        let dir = fixture();
        let kfc = dir.join("g.kfc");
        let bak = create_backup(&kfc).unwrap();
        // Simulate a new game's complete backup pair with the preceding generation's seal still present.
        KFCWriter::<Rc<KFCFile>, _>::new(&dir, "g", Rc::new(TypeRegistry::default()), "updated fixture")
            .unwrap().finalize().unwrap();
        std::fs::copy(&kfc, &bak).unwrap();
        assert!(!validate_backup(&kfc, &bak));
        create_backup(&kfc).unwrap();
        assert!(validate_backup(&kfc, &bak));
        std::fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn a_changed_game_input_cannot_be_published_or_cached() {
        let dir = fixture();
        let cache = dir.join("cache");
        std::fs::create_dir(&cache).unwrap();
        let mut types = TypeRegistry::default(); types.version = "fixture".into();
        std::fs::write(cache.join("g-types.json"), serde_json::to_vec(&types).unwrap()).unwrap();
        let mod_dir = dir.join("mods/a");
        std::fs::create_dir_all(mod_dir.join("src")).unwrap();
        std::fs::write(mod_dir.join("mod.json"), r#"{"id":"a","name":"a","version":"1.0.0","capabilities":["patch","export"]}"#).unwrap();
        std::fs::write(mod_dir.join("src/mod.lua"), r#"io.export("eml.json", "{}")"#).unwrap();
        let env = mod_loader::ModEnvironment::load_from(&dir, &cache, vec![mod_dir]).unwrap();
        let original = std::fs::read(dir.join("g.kfc")).unwrap();
        let args = crate::RunArgs { file_name: "g".into(), options: crate::RunOptions {
            patch: true, export: true, export_dir: Some(dir.clone().into_std_path_buf()), ..Default::default() } };
        let error = crate::run(&env, args).unwrap_err().to_string();
        assert!(error.contains("Game inputs changed"), "{error}");
        assert_eq!(std::fs::read(dir.join("g.kfc")).unwrap(), original);
        assert!(!cache.join("files.json").exists());
        std::fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn patch_cache_is_bound_to_the_selected_game_file() {
        let dir = fixture();
        let cache = dir.join("cache");
        std::fs::create_dir(&cache).unwrap();
        let mut types = TypeRegistry::default(); types.version = "fixture".into();
        std::fs::write(cache.join("g-types.json"), serde_json::to_vec(&types).unwrap()).unwrap();
        let mod_dir = dir.join("mods/a");
        std::fs::create_dir_all(mod_dir.join("src")).unwrap();
        std::fs::write(mod_dir.join("mod.json"), r#"{"id":"a","name":"a","version":"1.0.0","capabilities":["patch"]}"#).unwrap();
        std::fs::write(mod_dir.join("src/mod.lua"), "-- fixture").unwrap();
        let env = mod_loader::ModEnvironment::load_from(&dir, &cache, vec![mod_dir]).unwrap();
        let args = crate::RunArgs { file_name: "g".into(), options: crate::RunOptions { patch: true, ..Default::default() } };
        crate::transaction::BEFORE_PUBLISH.with(|check| check.set(Some(|game| {
            let resources = game.join("g.kfc_resources");
            #[cfg(target_os = "linux")]
            for fd in std::fs::read_dir("/proc/self/fd").unwrap() {
                assert_ne!(std::fs::read_link(fd.unwrap().path()).ok().as_ref(), Some(&resources),
                    "the vanilla reader must close its live file before publication");
            }
            #[cfg(windows)] {
                use std::os::windows::fs::OpenOptionsExt;
                // Windows/Wine sharing rules provide a direct check for any lingering reader.
                std::fs::OpenOptions::new().read(true).share_mode(0).open(resources).unwrap();
            }
        })));
        assert!(!crate::run(&env, args.clone()).unwrap().cached);
        assert!(crate::run(&env, args.clone()).unwrap().cached);
        let other = crate::RunArgs { file_name: "other".into(), ..args };
        assert!(crate::run(&env, other).is_err(), "another target must not reuse g's completed patch");
        std::fs::remove_dir_all(dir).unwrap();
    }

    // kiln-eml: a game folder whose one mod patches (nothing, but the run publishes), and the run's arguments.
    fn patch_fixture() -> (PathBuf, mod_loader::ModEnvironment, crate::RunArgs) {
        let dir = fixture();
        let cache = dir.join("cache");
        std::fs::create_dir(&cache).unwrap();
        let mut types = TypeRegistry::default(); types.version = "fixture".into();
        std::fs::write(cache.join("g-types.json"), serde_json::to_vec(&types).unwrap()).unwrap();
        let mod_dir = dir.join("mods/a");
        std::fs::create_dir_all(mod_dir.join("src")).unwrap();
        std::fs::write(mod_dir.join("mod.json"), r#"{"id":"a","name":"a","version":"1.0.0","capabilities":["patch"]}"#).unwrap();
        std::fs::write(mod_dir.join("src/mod.lua"), "-- fixture").unwrap();
        let env = mod_loader::ModEnvironment::load_from(&dir, &cache, vec![mod_dir]).unwrap();
        let args = crate::RunArgs { file_name: "g".into(), options: crate::RunOptions { patch: true, ..Default::default() } };
        (dir, env, args)
    }

    // kiln-eml: exercise real filesystem and parser failures, without a game install.
    #[test]
    fn preparation_cause_later_steps() {
        let dir = fixture();
        let bad = dir.join("broken.kfc.bak");
        std::fs::write(&bad, b"KFC").unwrap();
        let text = format!("{:#}", load_kfc_file(&bad).err().unwrap());
        assert!(text.contains("load_kfc_file:") && text.contains("broken.kfc.bak"), "{text}");
        assert!(text.contains("IO error: failed to fill whole buffer"), "{text}");
        std::fs::remove_file(&bad).unwrap();
        let text = format!("{:#}", load_kfc_file(&bad).err().unwrap());
        assert!(text.contains("load_kfc_file:") && text.contains("os error") && text.contains("broken.kfc.bak"), "{text}");
        let text = format!("{:#}", create_reader(&dir, "missing").err().unwrap());
        assert!(text.contains("create_reader:") && text.contains("missing.kfc.bak") && text.contains("os error"), "{text}");
        let base = load_kfc_file(dir.join("g.kfc")).unwrap();
        let text = format!("{:#}", create_writer(&dir, "missing", &Rc::new(TypeRegistry::default()), &base).err().unwrap());
        assert!(text.contains("create_writer:") && text.contains("missing.kfc.bak") && text.contains("os error"), "{text}");
        let text = format!("{:#}", load_type_registry(&dir, &dir.join("cache"), "missing").err().unwrap());
        assert!(text.contains("load_type_registry:") && text.contains("missing.exe") && text.contains("os error"), "{text}");
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn preparation_cause_busy_writer_and_copy() {
        // This test locks ShroudForge's file itself: no kill-matrix child is created meanwhile (test_gate.rs).
        let _locks = crate::test_gate::lock_hold();
        let dir = fixture();
        let config = dir.join("shroudforge/config");
        std::fs::create_dir_all(&config).unwrap();
        let lock = File::create(config.join(".shroudforge-write.lock")).unwrap();
        lock.lock().unwrap();
        let text = format!("{:#}", create_backup(dir.join("g.kfc")).unwrap_err());
        assert!(text.contains("create_backup:") && text.contains("ShroudForge") && text.contains("lock"), "{text}");
        drop(lock);
        std::fs::remove_file(dir.join("g.kfc_resources")).unwrap();
        let text = format!("{:#}", create_backup(dir.join("g.kfc")).unwrap_err());
        assert!(text.contains("create_backup:") && text.contains("g.kfc_resources") && text.contains("os error"), "{text}");
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn preparation_cause_unreadable_backup() {
        use std::os::unix::fs::PermissionsExt;
        let (dir, env, args) = patch_fixture();
        let bak = create_backup(dir.join("g.kfc")).unwrap();
        std::fs::set_permissions(&bak, std::fs::Permissions::from_mode(0)).unwrap();
        assert!(File::open(&bak).is_err(), "test requires an unprivileged host user");
        let text = crate::run(&env, args).unwrap_err().to_string();
        std::fs::set_permissions(&bak, std::fs::Permissions::from_mode(0o600)).unwrap();
        assert!(text.contains("create_backup:") && text.contains("g.kfc.bak") && text.contains("Permission denied") && text.contains("os error"), "{text}");
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn preparation_cause_truncated_backup() {
        let (dir, env, args) = patch_fixture();
        let bak = create_backup(dir.join("g.kfc")).unwrap();
        std::fs::write(&bak, b"KFC").unwrap();
        let text = crate::run(&env, args).unwrap_err().to_string();
        assert!(text.contains("create_backup:") && text.contains("g.kfc.bak") && text.contains("failed to fill whole buffer"), "{text}");
        std::fs::remove_dir_all(dir).unwrap();
    }

    // kiln-eml: preparation failures must reach the run's public error.
    #[test]
    fn preparation_cause_seal() {
        let (dir, env, args) = patch_fixture();
        create_backup(dir.join("g.kfc")).unwrap();
        let bak = dir.join("g.kfc_resources.bak");
        let mut bytes = std::fs::read(&bak).unwrap();
        bytes[0] ^= 1;
        std::fs::write(&bak, bytes).unwrap();
        let text = crate::run(&env, args).unwrap_err().to_string();
        assert!(text.contains("could not prepare the game data: create_backup:"), "{text}");
        assert!(text.contains("does not match the seal") && text.contains("g.kfc_resources.bak"), "{text}");
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn preparation_cause_link() {
        for name in ["g.kfc", "g.kfc.bak", "g.kfc_resources.bak"] {
            let (dir, env, args) = patch_fixture();
            if name == "g.kfc_resources.bak" { create_backup(dir.join("g.kfc")).unwrap(); }
            let target = dir.join("link-target");
            if dir.join(name).exists() {
                std::fs::rename(dir.join(name), &target).unwrap();
            } else {
                std::fs::copy(dir.join("g.kfc"), &target).unwrap();
            }
            std::os::unix::fs::symlink(&target, dir.join(name)).unwrap();
            let text = crate::run(&env, args).unwrap_err().to_string();
            assert!(text.contains("could not prepare the game data: create_backup:"), "{text}");
            assert!(text.contains("link") && text.contains(name), "{text}");
            std::fs::remove_dir_all(dir).unwrap();
        }
    }

    #[test]
    fn a_start_that_changes_nothing_reads_none_of_the_game_data() {
        // the fixture's files are seconds old: let their stamps count at once (the recent-change rule is tested in cache)
        crate::cache::RACY_WINDOW_OVERRIDE.with(|w| w.set(Some(std::time::Duration::ZERO)));
        let (dir, env, args) = patch_fixture();
        // bytes of the game folder's own files (the ones the change cache reads) and of the mod
        let game_bytes = || std::fs::read_dir(&dir).unwrap().map(|e| e.unwrap())
            .filter(|e| e.file_type().unwrap().is_file()).map(|e| e.metadata().unwrap().len()).sum::<u64>();
        let mod_bytes = || ["mods/a/mod.json", "mods/a/src/mod.lua"].iter().map(|n| std::fs::metadata(dir.join(n)).unwrap().len()).sum::<u64>();

        // a file is read once per version of it: the game files before the patch, what the run publishes after
        let before = game_bytes();
        let first = crate::run(&env, args.clone()).unwrap();
        assert!(!first.cached && env.cache_dir().join("files.json").exists());
        assert!(first.fingerprint.hashed_bytes <= before + game_bytes() + mod_bytes(), "{}", first.fingerprint);
        assert!(first.fingerprint.reused_files > 0, "the later steps reuse the earlier ones: {}", first.fingerprint);

        // nothing changed: a hit that reads nothing
        let hit = crate::run(&env, args.clone()).unwrap();
        assert!(hit.cached);
        assert_eq!(hit.fingerprint.hashed_bytes, 0, "{}", hit.fingerprint);
        assert_eq!(hit.fingerprint.hashed_files, 0);
        // and the ownership proof of the unchanged output is vouched for by that cache, not read again
        assert_eq!(hit.fingerprint.ownership_bytes, 0, "{}", hit.fingerprint.brief());
        assert!(hit.fingerprint.reused_files >= 5, "{}", hit.fingerprint);

        // a mod changed: the game data that did not change is still not read; the mod and what the run publishes are
        std::fs::write(dir.join("mods/a/src/mod.lua"), "-- fixture, changed").unwrap();
        let patched_before = game_bytes();
        let patch = crate::run(&env, args.clone()).unwrap();
        assert!(!patch.cached);
        let backups = ["g.kfc.bak", "g.kfc_resources.bak", "g.kfc.bak.integrity"].iter().map(|n| std::fs::metadata(dir.join(n)).unwrap().len()).sum::<u64>();
        assert!(patch.fingerprint.hashed_bytes <= patched_before - backups + game_bytes() - backups + mod_bytes(), "{}", patch.fingerprint);
        assert!(patch.fingerprint.reused_files >= 5, "{}", patch.fingerprint);
        println!("first: {}\nhit:   {}\npatch: {}", first.fingerprint, hit.fingerprint, patch.fingerprint);
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn an_unchanged_start_checks_ownership_in_full_once_when_the_cache_does_not_vouch_for_it() {
        crate::cache::RACY_WINDOW_OVERRIDE.with(|w| w.set(Some(std::time::Duration::ZERO)));
        let (dir, env, args) = patch_fixture();
        assert!(!crate::run(&env, args.clone()).unwrap().cached);
        // a cache saved without the receipt's mark (an earlier build of this cache, or a hit that could not check)
        let files = env.cache_dir().join("files.json");
        let mut saved: serde_json::Value = serde_json::from_slice(&std::fs::read(&files).unwrap()).unwrap();
        assert!(saved.as_object_mut().unwrap().remove("owner").is_some(), "the patch run recorded its receipt");
        std::fs::write(&files, serde_json::to_vec(&saved).unwrap()).unwrap();
        let first = crate::run(&env, args.clone()).unwrap();
        assert!(first.cached && first.fingerprint.ownership_bytes > 0, "checked in full once: {}", first.fingerprint.brief());
        let next = crate::run(&env, args.clone()).unwrap();
        assert!(next.cached && next.fingerprint.ownership_bytes == 0, "then vouched for: {}", next.fingerprint.brief());
        // a receipt that is not the one the cache vouched for (and does not match the live output): the patch runs
        let receipt = dir.join("kiln/eml/ownership/g.json");
        let mut value: serde_json::Value = serde_json::from_slice(&std::fs::read(&receipt).unwrap()).unwrap();
        value["files"]["g.kfc"]["sha256"] = "0".repeat(64).into();
        std::fs::write(&receipt, serde_json::to_vec(&value).unwrap()).unwrap();
        assert!(!crate::run(&env, args).unwrap().cached);
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn a_first_start_does_not_read_the_containers_and_a_change_to_one_is_found_without_reading_it() {
        crate::cache::RACY_WINDOW_OVERRIDE.with(|w| w.set(Some(std::time::Duration::ZERO)));
        let (dir, env, args) = patch_fixture();
        const MIB: usize = 1 << 20;
        for i in 0..3 {
            std::fs::write(dir.join(format!("g_{i:03}.dat")), vec![i as u8 + 1; 2 * MIB]).unwrap();
        }
        let progress = crate::Progress::none();

        // no cache at all: the containers are recorded by their stamps and never read
        let first = crate::run_with(&env, args.clone(), &progress).unwrap();
        assert!(!first.cached && env.cache_dir().join("files.json").exists());
        assert!(first.fingerprint.hashed_bytes < MIB as u64, "no container is read: {}", first.fingerprint);
        assert!(first.fingerprint.recorded_files >= 3 && first.fingerprint.recorded_bytes >= 6 * MIB as u64, "{}", first.fingerprint);
        let saved = std::fs::read_to_string(env.cache_dir().join("files.json")).unwrap();
        assert!(saved.contains("\"unread\""), "the cache file says which fingerprints are stamps");
        assert_eq!(progress.snapshot().map(|e| e.stage), Some(crate::Stage::Saving), "the run reported its stages");

        let hit = crate::run(&env, args.clone()).unwrap();
        assert!(hit.cached && hit.fingerprint.hashed_bytes == 0, "{}", hit.fingerprint);

        // a container changes (same length, last-write time put back): the patch runs again, and no container is read
        std::thread::sleep(std::time::Duration::from_millis(30));
        let container = dir.join("g_001.dat");
        let modified = std::fs::metadata(&container).unwrap().modified().unwrap();
        let mut bytes = std::fs::read(&container).unwrap();
        bytes[0] ^= 0x55;
        let mut f = std::fs::OpenOptions::new().write(true).open(&container).unwrap();
        f.write_all(&bytes).unwrap();
        f.set_modified(modified).unwrap();
        drop(f);
        let again = crate::run(&env, args.clone()).unwrap();
        assert!(!again.cached, "a changed container is a change");
        assert!(again.fingerprint.hashed_bytes < MIB as u64, "{}", again.fingerprint);
        assert!(crate::run(&env, args).unwrap().cached);
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn an_input_edited_in_place_after_the_last_barrier_is_never_cached() {
        crate::cache::RACY_WINDOW_OVERRIDE.with(|w| w.set(Some(std::time::Duration::ZERO)));
        let (dir, env, args) = patch_fixture();
        // after the barrier before publication, an edit of the same size with the last-write time restored
        crate::transaction::BEFORE_PUBLISH.with(|check| check.set(Some(|game| {
            std::thread::sleep(std::time::Duration::from_millis(30));
            let backup = game.join("g.kfc.bak");
            let modified = std::fs::metadata(&backup).unwrap().modified().unwrap();
            let mut bytes = std::fs::read(&backup).unwrap();
            let last = bytes.len() - 1;
            bytes[last] ^= 0x55;
            let mut f = std::fs::OpenOptions::new().write(true).open(&backup).unwrap();
            f.write_all(&bytes).unwrap();
            f.set_modified(modified).unwrap();
        })));
        let result = crate::run(&env, args).unwrap();
        assert!(result.fingerprint.reused_files > 0, "the earlier steps were reused, so the edit had to be noticed by its stamp");
        assert!(!env.cache_dir().join("files.json").exists(), "the changed input must not be cached");
        std::fs::remove_dir_all(dir).unwrap();
    }

}
