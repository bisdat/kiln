// SPDX-License-Identifier: GPL-3.0-or-later
//! A prepared EML run over one captured StartupHost generation, then the sole publisher, then cache completion.
//! Brabb3l EML bindings only (no SF Lua parity). The legacy run_with composition is covered by the unchanged fixture
//! witnesses (m6_eml_witness.rs); this file adds the prepared path and compares its game data with the legacy run's.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;

use fixture::archive::{self, ArchiveFixture, STEM};
use kfc::{container::KFCFile, guid::ContentHash};
use mod_loader::{
    ModEnvironment,
    lua::{
        Progress, RunArgs, RunOptions,
        startup_host::{BaseOrigin, PreparedEml, PreparedEmlRun, StartupHost},
    },
};
use std::{collections::BTreeMap, fs, path::Path, rc::Rc};

const CONTENT: &str = r#"game.assets.create_content(buffer.wrap("m6 created content"))"#;
const FAILING: &str = r#"error("m6 prepared failure")"#;
const UNCLASSIFIED: &str = "unclassified: game files as found when Kiln first managed this install";

/// The caller's validated baseline: an exact copy, deliberately WITHOUT a seal. A second baseline initialization
/// (create_backup) inside the prepared run would seal it and change the captured input witness.
fn baseline(f: &ArchiveFixture) -> Rc<KFCFile> {
    for ext in ["kfc", "kfc_resources"] {
        fs::copy(
            f.game.join(format!("{STEM}.{ext}")),
            f.game.join(format!("{STEM}.{ext}.bak")),
        )
        .unwrap();
    }
    Rc::new(KFCFile::from_path(f.game.join(format!("{STEM}.kfc.bak")), false).unwrap())
}
fn guard(f: &ArchiveFixture) -> fs::File {
    let file = fs::File::create(f.game.join(".kiln-eml.lock")).unwrap();
    file.try_lock().unwrap();
    file
}
fn host<'g>(f: &ArchiveFixture, base: &Rc<KFCFile>, guard: &'g fs::File) -> StartupHost<'g> {
    StartupHost::open(
        f.game.as_std_path(),
        STEM,
        f.types.clone(),
        base.clone(),
        guard,
    )
    .unwrap()
}
fn env(f: &ArchiveFixture, ids: &[&str]) -> ModEnvironment {
    let paths = ids
        .iter()
        .map(|id| f.root.0.join("packages").join(id))
        .collect();
    ModEnvironment::load_from(&f.game, f.game.join("kiln/eml/cache"), paths)
        .unwrap_or_else(|_| panic!("synthetic packages load"))
}
fn args(f: &ArchiveFixture, export: bool, skip_cache: bool) -> RunArgs {
    let export_dir = f.root.0.join("export");
    fs::create_dir_all(&export_dir).unwrap();
    RunArgs {
        file_name: STEM.into(),
        options: RunOptions {
            skip_cache,
            is_server: None,
            patch: true,
            export,
            runtime: false,
            export_dir: Some(export_dir.into_std_path_buf()),
        },
    }
}
fn witness(tag: &str) -> ArchiveFixture {
    let f = ArchiveFixture::new(tag);
    let package = f.package(
        "witness",
        include_str!("fixtures/m6/witness.lua"),
        &["patch", "export"],
    );
    fs::write(
        package.join("src/alias.lua"),
        include_str!("fixtures/m6/alias.lua"),
    )
    .unwrap();
    f
}
fn staged(prepared: PreparedEml<'_>) -> PreparedEmlRun<'_> {
    match prepared {
        PreparedEml::Staged(run) => *run,
        PreparedEml::Cached(_) => panic!("expected a staged generation, got a cache hit"),
    }
}
fn stages(game: &Path) -> Vec<std::path::PathBuf> {
    fs::read_dir(game)
        .unwrap()
        .flatten()
        .map(|e| e.path())
        .filter(|p| {
            p.file_name()
                .unwrap()
                .to_string_lossy()
                .starts_with(".kiln-eml-stage-")
        })
        .collect()
}
/// Every byte of the game folder except write-ahead scratch records, stages and the fingerprint journal.
fn snapshot(root: &Path) -> BTreeMap<String, Vec<u8>> {
    let mut out = BTreeMap::new();
    fn visit(root: &Path, path: &Path, out: &mut BTreeMap<String, Vec<u8>>) {
        for e in fs::read_dir(path).unwrap() {
            let e = e.unwrap();
            let name = e.file_name().to_string_lossy().into_owned();
            if name.starts_with(".kiln-eml-stage-") || name == ".kiln-eml.lock" {
                continue;
            }
            if e.file_type().unwrap().is_dir() {
                visit(root, &e.path(), out);
            } else {
                let key = e
                    .path()
                    .strip_prefix(root)
                    .unwrap()
                    .to_string_lossy()
                    .replace('\\', "/");
                if !key.starts_with("kiln/eml/ownership/cleanup-")
                    && key != "kiln/eml/cache/files.partial.json"
                {
                    out.insert(key, fs::read(e.path()).unwrap());
                }
            }
        }
    }
    visit(root, root, &mut out);
    out
}
fn owner(f: &ArchiveFixture) -> Option<String> {
    let cache: serde_json::Value =
        serde_json::from_slice(&fs::read(f.change_cache()).ok()?).ok()?;
    cache["owner"].as_str().map(str::to_owned)
}

#[test]
fn prepared_writes_stay_private_until_publish_and_the_cache_until_matching_completion() {
    let f = witness("prepared-run");
    let base = baseline(&f);
    let guard = guard(&f);
    let before = snapshot(f.game.as_std_path());
    let mut h = host(&f, &base, &guard);
    let run = staged(
        h.prepare_eml(
            &env(&f, &["witness"]),
            args(&f, true, false),
            &Progress::none(),
        )
        .unwrap(),
    );
    assert_eq!(
        run.patched(),
        2,
        "the witness edits one resource and creates one"
    );
    assert_eq!(
        snapshot(f.game.as_std_path()),
        before,
        "prepared Lua/resource/content writes changed live data, backups, receipt or cache"
    );
    assert!(!mod_loader::lua::owns_output(f.game.as_std_path(), STEM).unwrap());
    assert_eq!(
        fs::read(f.root.0.join("export/completed.txt")).unwrap(),
        b"m6 old-eml witness complete",
        "exports run during the prepared Lua pass, as in run_with"
    );
    let created = ContentHash::from_data(archive::NEW_CONTENT);
    assert_eq!(
        run.archive().read_content(&created).unwrap().unwrap(),
        archive::NEW_CONTENT,
        "the private generation holds the run's content"
    );
    let (prepared, commit) = run.into_parts();
    let published = prepared.publish().unwrap();
    archive::validate(&f.game, STEM).unwrap();
    archive::assert_original_prefix(&f);
    assert!(mod_loader::lua::owns_output(f.game.as_std_path(), STEM).unwrap());
    assert!(
        !f.change_cache().exists(),
        "publication alone must not save a completed change cache"
    );
    let receipt = published.receipt_sha256().unwrap().to_owned();
    commit.finish_after_publish(&published).unwrap();
    assert_eq!(owner(&f).as_deref(), Some(receipt.as_str()));

    // Exports are never silently skipped: unchanged inputs with export on run the Lua pass again (run_with's rule).
    fs::remove_file(f.root.0.join("export/completed.txt")).unwrap();
    let mut exporting = host(&f, &base, &guard);
    let run = staged(
        exporting
            .prepare_eml(
                &env(&f, &["witness"]),
                args(&f, true, false),
                &Progress::none(),
            )
            .unwrap(),
    );
    assert!(f.root.0.join("export/completed.txt").exists());
    drop(run);
}

#[test]
fn a_verified_cache_hit_returns_the_published_identity_and_stages_nothing() {
    let f = ArchiveFixture::new("prepared-hit");
    f.package("patch", CONTENT, &["patch"]);
    let base = baseline(&f);
    let guard = guard(&f);
    let mut h = host(&f, &base, &guard);
    let (prepared, commit) = staged(
        h.prepare_eml(
            &env(&f, &["patch"]),
            args(&f, false, false),
            &Progress::none(),
        )
        .unwrap(),
    )
    .into_parts();
    let published = prepared.publish().unwrap();
    let receipt = published.receipt_sha256().unwrap().to_owned();
    commit.finish_after_publish(&published).unwrap();
    let live = snapshot(f.game.as_std_path());
    let stages_before = stages(f.game.as_std_path());
    let mut again = host(&f, &base, &guard);
    match again
        .prepare_eml(
            &env(&f, &["patch"]),
            args(&f, false, false),
            &Progress::none(),
        )
        .unwrap()
    {
        PreparedEml::Cached(hit) => assert_eq!(hit.receipt_sha256(), Some(receipt.as_str())),
        PreparedEml::Staged(_) => panic!("unchanged inputs over owned output must be a cache hit"),
    }
    assert_eq!(
        stages(f.game.as_std_path()),
        stages_before,
        "a hit creates no stage"
    );
    assert_eq!(
        snapshot(f.game.as_std_path()),
        live,
        "a hit rewrote game data or its cache"
    );
}

#[test]
fn prepared_and_legacy_runs_publish_the_same_game_data() {
    let legacy = witness("prepared-vs-legacy-a");
    assert_eq!(legacy.run_packages(&["witness"]).mods[0].state, "loaded");
    let f = witness("prepared-vs-legacy-b");
    let base = baseline(&f);
    let guard = guard(&f);
    let mut h = host(&f, &base, &guard);
    let (prepared, commit) = staged(
        h.prepare_eml(
            &env(&f, &["witness"]),
            args(&f, true, false),
            &Progress::none(),
        )
        .unwrap(),
    )
    .into_parts();
    let published = prepared.publish().unwrap();
    commit.finish_after_publish(&published).unwrap();
    assert_eq!(f.semantics(), legacy.semantics());
    archive::validate(&f.game, STEM).unwrap();
    let names = |root: &Path| {
        let mut names: Vec<_> = fs::read_dir(root.join("export/errors"))
            .unwrap()
            .map(|e| e.unwrap().file_name())
            .collect();
        names.sort();
        names
    };
    assert_eq!(
        names(f.root.0.as_std_path()),
        names(legacy.root.0.as_std_path())
    );
    assert_eq!(
        fs::read(f.root.0.join("export/completed.txt")).unwrap(),
        fs::read(legacy.root.0.join("export/completed.txt")).unwrap()
    );
}

#[test]
fn wrong_generation_or_changed_inputs_never_save_a_reusable_cache() {
    for change in ["wrong-generation", "receipt", "mod-file", "game-input"] {
        let f = ArchiveFixture::new("prepared-refused");
        let package = f.package("patch", CONTENT, &["patch"]);
        let base = baseline(&f);
        let guard = guard(&f);
        let mut first = host(&f, &base, &guard);
        let (prepared, commit) = staged(
            first
                .prepare_eml(
                    &env(&f, &["patch"]),
                    args(&f, false, true),
                    &Progress::none(),
                )
                .unwrap(),
        )
        .into_parts();
        let published = prepared.publish().unwrap();
        let published = match change {
            "wrong-generation" => {
                // Another generation published over the same target: its identity does not complete this commit.
                let mut second = host(&f, &base, &guard);
                let (other, other_commit) = staged(
                    second
                        .prepare_eml(
                            &env(&f, &["patch"]),
                            args(&f, false, true),
                            &Progress::none(),
                        )
                        .unwrap(),
                )
                .into_parts();
                let other = other.publish().unwrap();
                assert!(commit.finish_after_publish(&other).is_err());
                assert!(
                    !f.change_cache().exists(),
                    "a foreign generation completed this cache"
                );
                other_commit.finish_after_publish(&other).unwrap();
                assert!(
                    f.change_cache().exists(),
                    "the matching completion saves it"
                );
                drop(published);
                continue;
            }
            "receipt" => {
                // Same generation, but its receipt is no longer the bytes the publication left.
                let receipt: serde_json::Value =
                    serde_json::from_slice(&fs::read(f.receipt()).unwrap()).unwrap();
                fs::write(f.receipt(), serde_json::to_vec_pretty(&receipt).unwrap()).unwrap();
                published
            }
            "mod-file" => {
                fs::write(package.join("src/mod.lua"), format!("{CONTENT}\n-- edited")).unwrap();
                published
            }
            _ => {
                fs::write(f.game.join("eml.json"), b"{}").unwrap();
                published
            }
        };
        assert!(
            commit.finish_after_publish(&published).is_err(),
            "{change}: completion accepted changed inputs"
        );
        assert!(
            !f.change_cache().exists(),
            "{change}: a reusable cache was saved"
        );
    }
}

#[test]
fn an_aborted_or_unpublished_generation_leaves_live_data_and_cache_alone() {
    for case in ["lua-error", "changed-before-publish", "dropped"] {
        let f = ArchiveFixture::new("prepared-abort");
        f.package(
            "patch",
            if case == "lua-error" {
                FAILING
            } else {
                CONTENT
            },
            &["patch"],
        );
        let base = baseline(&f);
        let guard = guard(&f);
        let before = snapshot(f.game.as_std_path());
        let mut h = host(&f, &base, &guard);
        let prepared = h.prepare_eml(
            &env(&f, &["patch"]),
            args(&f, false, true),
            &Progress::none(),
        );
        match case {
            "lua-error" => {
                let error = format!(
                    "{:#}",
                    prepared.err().expect("a failing package aborts the stage")
                );
                assert!(error.contains("m6 prepared failure"), "{error}");
                assert!(
                    h.prepare_eml(
                        &env(&f, &["patch"]),
                        args(&f, false, true),
                        &Progress::none()
                    )
                    .is_err(),
                    "one generation per captured host, even after an aborted stage"
                );
            }
            "changed-before-publish" => {
                let (prepared, commit) = staged(prepared.unwrap()).into_parts();
                fs::write(f.game.join(format!("{STEM}.kfc_resources.bak")), b"foreign").unwrap();
                let changed = snapshot(f.game.as_std_path());
                assert!(
                    prepared.publish().is_err(),
                    "a changed baseline was published over"
                );
                assert_eq!(snapshot(f.game.as_std_path()), changed);
                drop(commit);
                assert!(!f.change_cache().exists());
                continue;
            }
            _ => drop(staged(prepared.unwrap())),
        }
        assert_eq!(snapshot(f.game.as_std_path()), before, "{case}");
        assert!(!f.change_cache().exists(), "{case}");
        for stage in stages(f.game.as_std_path()) {
            assert!(
                fs::read_dir(&stage).unwrap().next().is_none(),
                "{case}: the aborted stage kept bytes"
            );
        }
    }
}

#[test]
fn one_writer_one_baseline_and_no_open_archive_reader_after_preparation() {
    let f = ArchiveFixture::new("prepared-single-writer");
    f.package("patch", CONTENT, &["patch"]);
    let base = baseline(&f);
    let guard = guard(&f);
    let mut h = host(&f, &base, &guard);
    let run = staged(
        h.prepare_eml(
            &env(&f, &["patch"]),
            args(&f, false, true),
            &Progress::none(),
        )
        .unwrap(),
    );
    assert_eq!(
        stages(f.game.as_std_path()).len(),
        1,
        "exactly one private writer"
    );
    assert!(
        !f.backup_seal().exists(),
        "the prepared run initialized a second baseline (create_backup sealed the caller's pair)"
    );
    let live = [
        f.game.join(format!("{STEM}.kfc_resources")),
        f.game.join(format!("{STEM}.kfc.bak")),
        f.game.join(format!("{STEM}.kfc")),
    ];
    #[cfg(target_os = "linux")]
    for fd in fs::read_dir("/proc/self/fd").unwrap().flatten() {
        if let Ok(path) = fs::read_link(fd.path()) {
            assert!(
                !live.iter().any(|l| path == l.as_std_path()),
                "an archive reader is still open after preparation: {path:?}"
            );
        }
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        for path in &live {
            fs::OpenOptions::new()
                .read(true)
                .write(true)
                .share_mode(0)
                .open(path)
                .unwrap();
        }
    }
    let _ = &live;
    let (prepared, commit) = run.into_parts();
    let published = prepared.publish().unwrap();
    commit.finish_after_publish(&published).unwrap();
    assert!(
        !f.backup_seal().exists(),
        "publication initialized a baseline"
    );
}

#[test]
fn base_origin_is_carried_verbatim_through_capture_publish_cache_and_restore() {
    let f = ArchiveFixture::new("prepared-origin");
    f.package("patch", CONTENT, &["patch"]);
    let base = baseline(&f);
    let guard = guard(&f);
    let identity = host(&f, &base, &guard).baseline_identity();
    let origin = BaseOrigin::new(UNCLASSIFIED, identity.clone());
    // keyed to the exact captured baseline: another pair's identity is refused
    let other = ArchiveFixture::new("prepared-origin-other");
    let other_base = baseline(&other);
    fs::write(
        other.game.join(format!("{STEM}.kfc_resources.bak")),
        b"another pair",
    )
    .unwrap();
    let other_guard = self::guard(&other);
    let foreign = host(&other, &other_base, &other_guard).baseline_identity();
    assert_ne!(foreign, identity);
    assert!(
        host(&f, &base, &guard)
            .with_origin(BaseOrigin::new(UNCLASSIFIED, foreign))
            .is_err(),
        "an origin keyed to another baseline was accepted"
    );

    let mut h = host(&f, &base, &guard).with_origin(origin.clone()).unwrap();
    let run = staged(
        h.prepare_eml(
            &env(&f, &["patch"]),
            args(&f, false, false),
            &Progress::none(),
        )
        .unwrap(),
    );
    assert_eq!(run.archive().origin(), Some(&origin));
    let (prepared, commit) = run.into_parts();
    let published = prepared.publish().unwrap();
    assert_eq!(published.origin(), Some(&origin));
    assert_eq!(
        published.origin().unwrap().label(),
        UNCLASSIFIED,
        "never upgraded by publication"
    );
    commit.finish_after_publish(&published).unwrap();
    // Repeat capture after publication: the same baseline identity, so the same origin; the hit carries it.
    let mut again = host(&f, &base, &guard).with_origin(origin.clone()).unwrap();
    assert_eq!(
        again.baseline_identity(),
        identity,
        "publication changed the captured baseline"
    );
    match again
        .prepare_eml(
            &env(&f, &["patch"]),
            args(&f, false, false),
            &Progress::none(),
        )
        .unwrap()
    {
        PreparedEml::Cached(hit) => assert_eq!(hit.origin(), Some(&origin)),
        PreparedEml::Staged(_) => panic!("expected a cache hit"),
    }
    drop(again);
    // A restore puts the live pair back from that baseline and never rewrites it: the origin still applies.
    assert!(mod_loader::lua::restore(&f.game, STEM));
    let restored = host(&f, &base, &guard);
    assert_eq!(restored.baseline_identity(), identity);
    assert_eq!(
        restored
            .with_origin(origin.clone())
            .unwrap()
            .baseline_identity(),
        origin.baseline().clone()
    );
}
