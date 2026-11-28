use std::path::PathBuf;

use kfc::reflection::LookupKey;
use mod_loader::ModEnvironment;

use crate::{alias::Path, cache::{CacheDiff, FileStateCache, Fingerprints}, env::{AppFeatures, AppState}, log::info, runner::LuaModRunner};

// kiln-eml: a named publication boundary (failpoint.rs). It exists only in this crate's own unit tests, where a
// test can record the boundaries a run passes or make one of them fail or stop the process; every other build has none.
// Its error converts with `?` into io::Error, anyhow::Error, KFCWriteError and mlua::Error.
macro_rules! failpoint {
    ($point:literal, $path:expr) => {
        #[cfg(test)]
        crate::failpoint::at($point, ::std::convert::AsRef::<::std::path::Path>::as_ref(&$path))?;
    };
    (if $cond:expr, $point:literal, $path:expr) => {
        #[cfg(test)]
        if $cond { crate::failpoint::at($point, ::std::convert::AsRef::<::std::path::Path>::as_ref(&$path))?; }
    };
}

// kiln-eml: what a run spent fingerprinting the game's files (bytes read, files reused), for the log and Kiln's state.
pub use cache::FingerprintStats;
// kiln-eml: who wrote which resource member in a run (env/writes.rs): the EML dialect's change journal.
pub use env::writes::{CreatedResource, PackageWrites, WriteRecord, WrittenResource};
// kiln-eml: what a run is doing, for the people waiting for it (progress.rs).
pub use progress::{Event, Progress, ProgressGuard, Sink, Stage, Timing};
// kiln-eml: the startup pass's progress from the archive work itself (progress.rs)
pub use progress::archive_work;

mod runner;
mod definition;
mod env;
mod util;
mod log;
mod cache;
mod progress;
mod alias;
mod load;
mod transaction;
// kiln-eml: private preparation over the existing physical publisher (no Lua dialect changes).
pub mod startup_host;
mod ownership;
// kiln-eml: Steam's word on the game's asset files before a new working baseline is taken from them.
mod steam_files;
mod cleanup;
mod displacement;
// kiln-eml: restoration needs exact writer ownership, not an input/performance cache.
pub use ownership::{recovery_inventory, owns_output, adopt, prune, retained_containers, recovery_guidance, Adoption};
// (kiln mode's ShroudForge handoff, Deferred, and the handoff it owes)
pub use ownership::{SfHandoff, handoff_owed, settle_owed_handoff, RECORDED_ORIGINS};
// The startup pass's type registry, read as EML's run reads it (the change cache's types file, else the exe),
// and the witness checks' one system question asked on the boot thread
pub use load::load_type_registry;
pub use transaction::prime_witness_checks;
// kiln-eml: read-only receipt/seal views for the archive inspector.
pub use ownership::readonly;
mod lua;
#[cfg(test)]
mod failpoint;
// kiln-eml: the test lock/spawn gate (test_gate.rs): this crate's unit tests, and kiln-eml's through the dev-only
// `test-gate` feature (enabled only by kiln-eml's [dev-dependencies]); no normal build has it.
#[cfg(any(test, feature = "test-gate"))]
#[doc(hidden)]
pub mod test_gate;
#[cfg(test)]
mod publication_tests;

#[derive(Debug, Clone, Default)]
pub struct RunOptions {
    /// If true, it will ignore the cache and re-apply all mods.
    pub skip_cache: bool,
    /// If None, it will auto-detect based on the files within the game directory.
    pub is_server: Option<bool>,
    /// If true, it will apply patches to the game files.
    pub patch: bool,
    /// If true, it will allow mods to use `io.export` to export files to the export directory.
    pub export: bool,
    /// If true, it will allow mods to use `loader.runtime.register_dll` to register DLLs to be loaded at runtime.
    pub runtime: bool,
    /// If None, it will use the default export directory (`<game_dir>/export`).
    pub export_dir: Option<PathBuf>,
}

#[derive(Debug, Clone)]
pub struct RunArgs {
    pub file_name: String,
    /// The features to enable.
    pub options: RunOptions,
}

#[derive(Debug, Clone, Default)]
pub struct RunResult {
    pub dlls: Vec<PathBuf>,
    // kiln-eml: what the run did, for Kiln's state file and log.
    /// No mod or game file changed since the last run: nothing was patched (the game files keep the last patch).
    pub cached: bool,
    /// Resources written by this run (0 when cached or when no mod changed anything).
    pub patched: usize,
    /// What the change cache and the input barriers read of the game's files.
    pub fingerprint: FingerprintStats,
    /// kiln-eml: ShroudForge asset mods (`<id> <version>`) whose prepared output this run's publication replaced.
    pub sf_displaced: Vec<String>,
    /// kiln-eml: the live generation once the run ended well: the SHA-256 of the ownership receipt of the output it
    /// published, or that an unchanged start found live (`Checked::Hit`). None: no output of this run is proven live.
    pub generation: Option<String>,
    /// kiln-eml: who wrote what in the published generation (a patch run that published): its change journal's record.
    pub journal: Option<WriteRecord>,
}

pub fn run(
    env: &ModEnvironment,
    args: RunArgs,
) -> anyhow::Result<RunResult> {
    // kiln-eml: progress lines in the log, no sink
    let progress = Progress::start(None);
    run_with(env, args, &progress)
}

/// The run's fingerprint counters, with what the ownership checks of this run read.
fn stats(prints: &Fingerprints) -> FingerprintStats {
    FingerprintStats { ownership_bytes: ownership::read(), ..prints.stats() }
}

/// `run`, reporting what it does into `progress` (a stage, and while it reads game files the file and the bytes done and
/// to do): see progress.rs.
pub fn run_with(
    env: &ModEnvironment,
    args: RunArgs,
    progress: &Progress,
) -> anyhow::Result<RunResult> {
    info!("Running lua with options: {:?}", args);

    // kiln-eml: one memo of file fingerprints for the whole run (cache/prints.rs): every barrier below re-stats its files
    // and reads only those that changed since they were hashed, so the game data is read once, not four times.
    let mut prints = Fingerprints::new(env.game_dir().as_std_path()).with_progress(progress.clone());
    // what an interrupted run had read, and a journal for this one (removed when it ends well)
    prints.resume_from(env.cache_dir().as_std_path());
    ownership::reset_read();
    let result = run_steps(env, args, progress, &mut prints);
    if result.is_ok() {
        prints.finish();
    }
    result
}

fn run_steps(
    env: &ModEnvironment,
    args: RunArgs,
    progress: &Progress,
    prints: &mut Fingerprints,
) -> anyhow::Result<RunResult> {
    // kiln-eml: run_with is prepared execution, the sole publisher and cache completion, composed as before; the
    // prepared half is shared with `startup_host::StartupHost::prepare_eml`, which stops before publication.
    let planned = match check(env, args, progress, prints) {
        Checked::Hit { receipt } => return Ok(RunResult { cached: true, fingerprint: stats(prints), generation: Some(receipt), ..RunResult::default() }),
        Checked::Run(planned) => *planned,
    };
    let cache_unchanged = planned.cache_diff.is_none();

    // create a new app state

    progress.stage(Stage::Prepare);
    let app_state = match AppState::new(
        env.clone(),
        planned.args.clone(),
        &planned.cache_diff,
    ) {
        Ok(context) => context,
        // kiln-eml: a file another program had open is said plainly (the paths and the system's words are in kiln.log);
        // any other cause in full, in Display (also used by status).
        Err(error) => match transaction::in_use(&error) {
            Some(plain) => anyhow::bail!("{plain}"),
            None => anyhow::bail!("could not prepare the game data: {error:#}"),
        },
    };

    let mut executed = execute(env, app_state, planned, progress, prints)?;

    let mut patched = 0;
    let mut published = None;
    let record = executed.record.take();

    if let Some(staged) = executed.staged.take() {
        // Same error text as EML's `finalize`: an I/O failure of the publisher is a KFCWriteError.
        let prepared = staged.writer.prepare().map_err(load::stage_refusal(&staged.completion.file_name))?;
        let generation = staged.completion.generation;
        published = Some((publish(prepared, generation, env.game_dir().as_std_path(), &staged.completion.file_name, &ownership::Publication::default())
            .map_err(kfc::container::KFCWriteError::from)?, staged.completion));

        info!("Applied {} patches", executed.applied);
        patched = executed.applied;
    }

    // create a new cache file

    let mut generation = None;
    if let Some((published, completion)) = published {
        progress.stage(Stage::Saving);
        completion.finish(&published, env, prints);
        generation = published.receipt.clone();
    }

    let app_state = executed.runner.lua.app_data_ref::<AppState>().unwrap();
    let cached = !app_state.has_feature(AppFeatures::PATCH) && cache_unchanged;

    Ok(RunResult {
        cached,
        patched,
        fingerprint: stats(prints),
        sf_displaced: ownership::displaced(),
        journal: generation.as_ref().and(record),
        generation,
        dlls: app_state.registered_dlls()
            .into_iter()
            .map(|path| path.into_std_path_buf())
            .collect::<Vec<_>>(),
    })
}

/// kiln-eml: what a run found before it touched any game data.
pub(crate) enum Checked {
    /// Nothing changed and the live output is the one the ownership receipt with this SHA-256 lists: nothing to run.
    Hit { receipt: String },
    Run(Box<Planned>),
}

/// kiln-eml: a run that goes ahead: its arguments (patching switched off over owned output when only the runtime step
/// is wanted), the cache key and diff, and the mods as they were before anything ran.
pub(crate) struct Planned {
    pub(crate) args: RunArgs,
    cache_key: String,
    pub(crate) cache_diff: CacheDiff,
    mods_before: FileStateCache,
}

/// kiln-eml: the change cache check of EML's `run`, with ownership receipts (unchanged behaviour, moved here so that the
/// prepared run shares it).
pub(crate) fn check(
    env: &ModEnvironment,
    mut args: RunArgs,
    progress: &Progress,
    prints: &mut Fingerprints,
) -> Checked {
    let cache_key = format!("{}|{:?}", args.file_name, args.options);
    let file_name = args.file_name.clone();
    let current_cache = (!args.options.skip_cache).then(|| FileStateCache::read(env.cache_dir()));
    if let Some(saved) = &current_cache {
        prints.seed(saved);
    }
    progress.stage(Stage::Checking);
    let mut mods_before = FileStateCache::default();
    track_mods(&mut mods_before, env, prints);

    // check cache if files have changed

    let cache_diff = if let Some(current_cache) = &current_cache {
        let mut new_cache = FileStateCache::default();
        new_cache.configuration(&cache_key);
        // kiln-eml: another build of the cache (or none): this start patches whatever the files say, so no game file is
        // read to be compared with the saved fingerprints (prints.rs)
        if new_cache.diff(current_cache).build_id_changed() {
            prints.patch_certain();
        }

        new_cache.track_game_files(env.game_dir(), prints);
        track_mods(&mut new_cache, env, prints);

        let cache_diff = new_cache.diff(current_cache);

        // kiln-eml: an unchanged start skips the patch only over output Kiln can prove it published. This cache vouches
        // for it when it recorded that receipt's output and every game file is as it recorded them (nothing is read);
        // otherwise it is checked in full once (SHA-256 of the receipt's files and backups), and this cache records it.
        let checked = cache_diff.is_none() && args.options.patch && !args.options.export;
        let owned = if checked {
            let root = env.game_dir().as_std_path();
            match ownership::vouched(root, &file_name, current_cache.owner()) {
                Ok(Some(marker)) => Some(marker),
                _ => {
                    info!("Nothing changed; checking the game data against its ownership receipt (once, until the next patch)");
                    match owns_output(root, &file_name) {
                        Ok(true) => ownership::marker(root, &file_name).ok().flatten(),
                        _ => None,
                    }
                }
            }
        } else { None };
        new_cache.set_owner(if checked { owned.clone() } else { current_cache.owner().map(str::to_owned) });

        // kiln-eml: nothing changed, but this start learned stamps the saved cache lacks (it predates them, or its
        // files were still too fresh to stamp), or its ownership: save them, so the next start reads nothing either.
        if cache_diff.is_none() && (new_cache.stamps_differ(current_cache) || new_cache.owner() != current_cache.owner()) {
            new_cache.write(env.cache_dir());
        }

        if let Some(receipt) = owned {
            args.options.patch = false;

            info!("No changes detected, skipping patching");

            if !args.options.runtime {
                return Checked::Hit { receipt };
            }
        }

        cache_diff
    } else {
        info!("Skipping cache check");
        prints.patch_certain();
        CacheDiff::new_dirty()
    };

    Checked::Run(Box::new(Planned { args, cache_key, cache_diff, mods_before }))
}

/// kiln-eml: one run's Lua and staged writes, up to publication and not including it.
pub(crate) struct Executed {
    pub(crate) runner: LuaModRunner,
    /// Patch runs: the staged writer holding this run's resources, with every reader of the vanilla data closed.
    pub(crate) staged: Option<Staged>,
    pub(crate) applied: usize,
    /// kiln-eml: who wrote what (env/writes.rs), taken before the resources were serialized (patch runs).
    pub(crate) record: Option<WriteRecord>,
}

pub(crate) struct Staged {
    pub(crate) writer: transaction::PatchWriter,
    pub(crate) completion: Completion,
}

/// kiln-eml: the Lua pass of EML's `run` and the staging of its resources (unchanged behaviour). Publication and the
/// change cache are the caller's: `run_steps` composes them as before, a prepared run hands them out (startup_host.rs).
pub(crate) fn execute(
    env: &ModEnvironment,
    app_state: AppState,
    planned: Planned,
    progress: &Progress,
    prints: &mut Fingerprints,
) -> anyhow::Result<Executed> {
    let Planned { args, cache_key, mods_before, .. } = planned;
    let file_name = args.file_name;
    let vanilla_containers = app_state.kfc_file().containers().len();
    progress.stage(Stage::Checking);
    let mut inputs_before = FileStateCache::default();
    inputs_before.track_game_inputs(env.game_dir().as_std_path(), &file_name, vanilla_containers, prints);

    // setup and run all mods

    let runner = LuaModRunner::new(app_state)?;

    match runner.setup(env.mod_registry()) {
        Ok(_) => {},
        Err(err) => {
            anyhow::bail!("Failed to setup LuaModRunner: {}", err);
        }
    }

    info!("Running mods...");
    progress.stage(Stage::Mods);

    // TODO: move this somewhere else, maybe to the GameContext?

    runner.run()?;

    let app_state = runner.lua.app_data_ref::<AppState>().unwrap();

    if !app_state.has_feature(AppFeatures::PATCH) {
        drop(app_state);
        return Ok(Executed { runner, staged: None, applied: 0, record: None });
    }

    // kiln-eml: who wrote what, from the values as they will be written (env/writes.rs)
    let record = Some(app_state.write_record()?);

    progress.stage(Stage::Checking);
    let mut inputs_now = FileStateCache::default();
    inputs_now.track_game_inputs(env.game_dir().as_std_path(), &file_name, vanilla_containers, prints);
    if inputs_before.diff(&inputs_now).is_some() {
        anyhow::bail!("Game inputs changed or became unreadable during patch preparation");
    }
    info!("Applying patches...");
    progress.stage(Stage::Writing);

    let mut buf = Vec::new();
    let mut writer = app_state.take_writer();
    let resources = app_state.get_cached_resources();
    let mut applied_resources = 0;

    for resource in resources.iter() {
        let resource_id = &resource.resource_id;
        let value = resource.apply(&runner.lua)
            .map_err(|e| anyhow::anyhow!(
                "Failed to apply resource {resource_id}: {e}"
            ))?;

        if let Some(value) = value {
            let Some(r#type) = app_state.type_registry()
                .get_by_hash(LookupKey::Qualified(resource_id.type_hash())) else {
                anyhow::bail!(
                    "Failed to find type with hash {:#010x} for resource {resource_id}",
                    resource_id.type_hash(),
                );
            };

            buf.clear();

            if let Err(e) = value.write_into(
                app_state.type_registry(),
                r#type,
                &mut buf,
            ) {
                anyhow::bail!(
                    "Failed to serialize resource {resource_id} of type {}: {e}",
                    r#type.qualified_name,
                );
            }

            writer.write_resource(resource_id, &buf)?;
            failpoint!("stage.resource-written", std::path::Path::new("stage/resource"));
            applied_resources += 1;
        }
    }

    // Wine refuses to replace the live resource file while our vanilla reader still has it open.
    // All Lua/resource reads are complete; close it before publishing any game files.
    app_state.close_reader();
    failpoint!("stage.readers-closed", std::path::Path::new("stage/readers"));
    drop(app_state);

    let completion = Completion {
        generation: Generation::new(),
        cache_key,
        file_name,
        vanilla_containers,
        mods_before,
        inputs_before,
    };
    Ok(Executed { runner, staged: Some(Staged { writer, completion }), applied: applied_resources, record })
}

/// kiln-eml: one staged generation's identity, so that a cache completion can only ever be the one of the publication
/// it was prepared with (process-unique, never persisted).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct Generation(u128);
impl Generation {
    pub(crate) fn new() -> Self { Self(uuid::Uuid::new_v4().as_u128()) }
}

/// kiln-eml: what one publication left: its generation, the SHA-256 of the ownership receipt it wrote (read once it
/// finished, after the previous generation was tidied up) and the ShroudForge asset mods whose output it replaced.
pub(crate) struct Published {
    pub(crate) generation: Generation,
    pub(crate) receipt: Option<String>,
    pub(crate) displaced: Vec<String>,
}

/// kiln-eml: the sole publisher (transaction.rs), then the identity of what it published. `publication`: the
/// ShroudForge handoff mode and the BASE origin the receipt records (EML mode: Live, none).
pub(crate) fn publish(prepared: transaction::PreparedArchive, generation: Generation, game: &std::path::Path, file_name: &str,
    publication: &ownership::Publication) -> std::io::Result<Published> {
    prepared.publish_with(publication)?;
    Ok(Published { generation, receipt: ownership::marker(game, file_name).ok().flatten(), displaced: ownership::displaced() })
}

/// kiln-eml: the change cache a staged run may save once its generation is published: the inputs it compares and the
/// fingerprints of the mods and game inputs from before Lua ran (the ABA barrier).
pub(crate) struct Completion {
    pub(crate) generation: Generation,
    cache_key: String,
    pub(crate) file_name: String,
    vanilla_containers: usize,
    mods_before: FileStateCache,
    inputs_before: FileStateCache,
}

impl Completion {
    /// Saves `files.json` for `published` only when it is this run's own generation, its receipt is still the live one
    /// and every mod and game input is as it was before Lua ran; otherwise removes any saved cache (the next start runs
    /// again). Returns whether it saved one.
    pub(crate) fn finish(self, published: &Published, env: &ModEnvironment, prints: &mut Fingerprints) -> bool {
        if published.generation != self.generation {
            let _ = std::fs::remove_file(env.cache_dir().join("files.json"));
            return false;
        }
        let mut mods_after = FileStateCache::default();
        track_mods(&mut mods_after, env, prints);
        let mut new_cache = mods_after.clone();
        new_cache.configuration(&self.cache_key);

        new_cache.track_game_files(env.game_dir(), prints);
        // kiln-eml: these are the fingerprints of the output just published, which its receipt lists exactly
        let owner = ownership::marker(env.game_dir().as_std_path(), &self.file_name).ok().flatten();
        let same_receipt = owner == published.receipt;
        new_cache.set_owner(owner);
        // Reuse the exact fingerprints being saved; a second independent after-snapshot can hide an ABA edit.
        let inputs_after = new_cache.clone().only_game_inputs(env.game_dir().as_std_path(), &self.file_name, self.vanilla_containers);
        if same_receipt && self.mods_before.diff(&mods_after).is_none() && self.inputs_before.diff(&inputs_after).is_none() {
            new_cache.write(env.cache_dir());
            true
        } else {
            let _ = std::fs::remove_file(env.cache_dir().join("files.json"));
            false
        }
    }
}

// kiln-eml: an environment from explicit mod paths (ModEnvironment::load_from) tracks exactly those, so switching a
// mod off in a Kiln profile re-patches too; otherwise EML's `<game>/mods`.
fn track_mods(cache: &mut FileStateCache, env: &ModEnvironment, prints: &mut Fingerprints) {
    if env.mod_paths().is_empty() {
        cache.track_mod_files(env.mods_dir(), prints);
    } else {
        for path in env.mod_paths() {
            cache.track_directory(path, prints);
        }
    }
}

pub fn export_lua_definitions(
    game_dir: impl AsRef<Path>,
    file_name: &str,
    force: bool,
) -> bool {
    let game_dir = game_dir.as_ref();
    let cache_dir = game_dir.join(".cache");
    let (type_registry, is_dirty) = match crate::load::load_type_registry(
        game_dir,
        &cache_dir,
        file_name,
    ) {
        Ok(type_registry) => type_registry,
        Err(_) => return false,
    };

    crate::load::export_lua_definitions(
        &cache_dir,
        &type_registry,
        is_dirty || force,
    );

    true
}

pub fn restore(
    game_dir: impl AsRef<Path>,
    file_name: &str,
) -> bool {
    let kfc_path = game_dir.as_ref()
        .join(format!("{file_name}.kfc"));

    load::restore_backup(&kfc_path).is_ok()
}
