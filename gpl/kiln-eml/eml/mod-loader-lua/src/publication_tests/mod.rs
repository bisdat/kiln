// SPDX-License-Identifier: GPL-3.0-or-later
//! The kill/error matrix of the physical publication path (the publication plan, boundaries 1-7).
//!
//! Four scenarios run the production composition (`run_with`: baseline capture, write-ahead scratch, staging, Lua,
//! finalization and read validation, SF handoff, receipt, vanilla-header barrier, displacement, data, header LAST,
//! previous-generation clean-up, change cache) or the no-mod restore, on the synthetic m6 fixture (GPL, no game bytes):
//! - `first`: first publication over vanilla files, with SF's applied state and a foreign `_004.dat` in the way;
//! - `grow`: replacing owned output with a generation that adds a container where a foreign file now lies;
//! - `shrink`: replacing owned output with a generation without the container the previous one added;
//! - `restore`: the no-mod restore of owned output;
//! - `lent`: the SF-dialect startup pass's physical path on the startup facade: over owned output, SF's
//!   applied state and the same foreign `_004.dat` as `grow`, the host's one stage is lent to the VM, a
//!   content is written through the VM's `StageWriter` (as `game.assets.create_content` does), the stage is taken
//!   back from every handle and the edited resource written into it (the asset session's dirty-resource serialization,
//!   `LentStage::prepare_with`), the stage is prepared, the baseline reader is closed and the generation published.
//!   the startup pass composes them; it has no change cache yet, so neither has this step.
//!
//! Each boundary the uninterrupted run passes (failpoint.rs; every repeated file is its own boundary) is then hit once
//! with an injected I/O failure (in this process) and once by killing a child process stopped exactly there. After the
//! interruption the live archive must reopen and validate (every referenced file, size and range) and be provably the
//! OLD set, a SAFE BASE (vanilla data) or the complete NEW set, never a mixed one; an ownership claim, SF's applied
//! claim, the backups and the foreign file must agree with it. Then the next start's recovery runs twice (prune, then the
//! same mods again, or the no-mod restore), and finally a no-mod restore, with a foreign stage child and an unrecorded
//! UUID-named copy planted first: both survive byte-identical, and the foreign `.dat` survives exactly once.
use crate::failpoint::{self, Action, Boundary};
use crate::test_gate;
use camino::{Utf8Path, Utf8PathBuf};
use std::{
    fs,
    process::{Command, Stdio},
    sync::atomic::{AtomicU64, Ordering},
    time::{Duration, Instant},
};

#[allow(dead_code)]
#[path = "../../../../kiln-eml/tests/support/m6_fixture/archive.rs"]
mod archive;
#[allow(dead_code)]
#[path = "../../../../kiln-eml/tests/support/m6_fixture/types.rs"]
mod types;

use archive::{ArchiveFixture, STEM, SemanticSet};

static NEXT_ROOT: AtomicU64 = AtomicU64::new(0);

/// A unique root below TMPDIR (the m6 fixture's own helper, which archive.rs expects beside it).
pub struct TestRoot(pub Utf8PathBuf);
impl TestRoot {
    pub fn new(tag: &str) -> Self {
        assert!(tag.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-'));
        let parent = Utf8PathBuf::from_path_buf(std::env::temp_dir()).unwrap();
        loop {
            let id = NEXT_ROOT.fetch_add(1, Ordering::Relaxed);
            let path = parent.join(format!("m6-{tag}-{}-{id}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(e) => panic!("reserve test root: {e}"),
            }
        }
    }
}
impl Drop for TestRoot {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

/// Edits one resource and creates one content blob (a new container).
const WITH_CONTENT: &str = r#"local r = game.assets.get_resource("01234567-89ab-cdef-0123-456789abcdef", game.types.get("m6.Root"))
r.data.child.extra = 202
game.assets.create_content(buffer.wrap("m6 g03 created content"))"#;
/// Edits one resource only.
const EDIT_ONLY: &str = r#"local r = game.assets.get_resource("01234567-89ab-cdef-0123-456789abcdef", game.types.get("m6.Root"))
r.data.child.extra = 303"#;
const FOREIGN: &[u8] = b"m6 g03 foreign container bytes";
/// What the `lent` pass stages through the VM's handle: one content blob (a new container) and one edited resource.
const LENT_CONTENT: &[u8] = b"m6 g08 content staged through the lent writer";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Scenario {
    First,
    Grow,
    Shrink,
    Restore,
    Lent,
    /// The `lent` pass in kiln mode (`SfHandoff::Deferred`, a recorded BASE origin): ShroudForge's folder is
    /// never touched, the handoff it owes is recorded instead.
    Deferred,
    /// The verified BASE in kiln mode (`StartupHost::restore_base`, Deferred) over owned output.
    DeferredRestore,
    /// The startup cache's record (`StartupRecord::write`) replacing an older one; the archive is not touched.
    Record,
}
const SCENARIOS: [Scenario; 8] = [
    Scenario::First,
    Scenario::Grow,
    Scenario::Shrink,
    Scenario::Restore,
    Scenario::Lent,
    Scenario::Deferred,
    Scenario::DeferredRestore,
    Scenario::Record,
];

impl Scenario {
    fn name(self) -> &'static str {
        match self {
            Scenario::First => "first",
            Scenario::Grow => "grow",
            Scenario::Shrink => "shrink",
            Scenario::Restore => "restore",
            Scenario::Lent => "lent",
            Scenario::Deferred => "deferred",
            Scenario::DeferredRestore => "deferred-restore",
            Scenario::Record => "record",
        }
    }
    fn parse(name: &str) -> Self {
        SCENARIOS.into_iter().find(|s| s.name() == name).unwrap()
    }
    /// The mods of the step under test (a `run_with` step); None: the no-mod restore, or the lent pass.
    fn mods(self) -> Option<&'static [&'static str]> {
        match self {
            Scenario::First | Scenario::Grow => Some(&["content"]),
            Scenario::Shrink => Some(&["edit"]),
            Scenario::Restore | Scenario::Lent | Scenario::Deferred | Scenario::DeferredRestore | Scenario::Record => None,
        }
    }
    fn has_sf(self) -> bool {
        !matches!(self, Scenario::Restore | Scenario::Record)
    }
    fn has_foreign(self) -> bool {
        matches!(self, Scenario::First | Scenario::Grow | Scenario::Lent | Scenario::Deferred)
    }
    /// Kiln mode: ShroudForge's folder is never touched.
    fn deferred(self) -> bool {
        matches!(self, Scenario::Deferred | Scenario::DeferredRestore)
    }
}

/// A fixture in the state right before the scenario's step, with what the invariants compare against.
struct Case {
    f: ArchiveFixture,
    vanilla: SemanticSet,
    old: SemanticSet,
    old_owned: bool,
    /// Every file below `<game>/shroudforge` (name and bytes) before the step: kiln mode must leave it as it is.
    sf_tree: Vec<(String, Vec<u8>)>,
    /// The receipt the setup's change journal names (kiln-mode scenarios over a journaled generation).
    journal: Option<String>,
}

fn package(root: &Utf8Path, id: &str, lua: &str) {
    let path = root.join("packages").join(id);
    fs::create_dir_all(path.join("src")).unwrap();
    fs::write(
        path.join("mod.json"),
        serde_json::to_vec(&serde_json::json!({"id": id, "name": id, "version": "1.0.0", "capabilities": ["patch"]}))
            .unwrap(),
    )
    .unwrap();
    fs::write(path.join("src/mod.lua"), lua).unwrap();
}

fn run(root: &Utf8Path, ids: &[&str]) -> anyhow::Result<crate::RunResult> {
    let game = root.join("game");
    let paths = ids
        .iter()
        .map(|id| root.join("packages").join(id))
        .collect();
    let env = mod_loader::ModEnvironment::load_from(&game, game.join("kiln/eml/cache"), paths)
        .unwrap_or_else(|_| panic!("synthetic packages load"));
    let args = crate::RunArgs {
        file_name: STEM.into(),
        options: crate::RunOptions {
            patch: true,
            ..Default::default()
        },
    };
    crate::run_with(&env, args, &crate::Progress::none())
}

/// The no-mod start: restore only output the receipt proves (kiln-eml's runner does the same).
fn restore(root: &Utf8Path) -> bool {
    let game = root.join("game");
    !crate::owns_output(game.as_std_path(), STEM).unwrap_or(false) || crate::restore(&game, STEM)
}

/// The `lent` scenario's pass: the startup facade's one stage lent to the VM's handle, written through it, taken back,
/// prepared and published. The install lock is the caller's in production (kiln-eml's runner); here the pass takes it.
fn lent(root: &Utf8Path) -> anyhow::Result<()> {
    lent_with(root, crate::ownership::SfHandoff::Live)
}

/// The `deferred-restore` step: the verified BASE in kiln mode over owned output.
fn deferred_restore(root: &Utf8Path) -> anyhow::Result<()> {
    use kfc::container::KFCFile;
    use std::rc::Rc;
    let game = root.join("game");
    let _hold = test_gate::lock_hold();
    let lock = fs::OpenOptions::new()
        .create(true)
        .truncate(false)
        .write(true)
        .open(game.join(".kiln-eml.lock"))?;
    lock.try_lock()?;
    let baseline = Rc::new(KFCFile::from_path(game.join(format!("{STEM}.kfc.bak")), false)?);
    let mut host = crate::startup_host::StartupHost::open(game.as_std_path(), STEM, Rc::new(types::registry()), baseline, &lock)?
        .with_handoff(crate::ownership::SfHandoff::Deferred);
    host.restore_base()?;
    Ok(())
}

/// `lent` in ShroudForge handoff mode `handoff`; Deferred also records the first-managed BASE origin.
fn lent_with(root: &Utf8Path, handoff: crate::ownership::SfHandoff) -> anyhow::Result<()> {
    use kfc::{
        container::KFCFile, guid::ContentHash, reflection::LookupKey, resource::value::Value,
    };
    use std::rc::Rc;
    let game = root.join("game");
    // Dropped after the lock file: no kill-matrix child is created meanwhile (test_gate.rs).
    let _hold = test_gate::lock_hold();
    let lock = fs::OpenOptions::new()
        .create(true)
        .truncate(false)
        .write(true)
        .open(game.join(".kiln-eml.lock"))?;
    lock.try_lock()?;
    let registry = Rc::new(types::registry());
    let baseline = Rc::new(KFCFile::from_path(
        game.join(format!("{STEM}.kfc.bak")),
        false,
    )?);
    let mut host = crate::startup_host::StartupHost::open(
        game.as_std_path(),
        STEM,
        registry.clone(),
        baseline,
        &lock,
    )?
    .with_handoff(handoff);
    if handoff == crate::ownership::SfHandoff::Deferred {
        let (token, label) = crate::ownership::RECORDED_ORIGINS[0];
        let origin = crate::startup_host::BaseOrigin::recorded(token, label, host.baseline_identity())?;
        host = host.with_origin(origin)?;
    }
    let stage = host.begin_stage()?.lend();
    let reader = host.reference_reader()?;
    let writer = stage.writer();
    // As the SF pass orders them: contents at once while packages run through the VM's handle, the dirty resources
    // when they are serialized, into the stage taken back from every handle first.
    writer.write_content(&ContentHash::from_data(LENT_CONTENT), LENT_CONTENT)?;
    let mut value = types::root_value();
    if let Value::Struct(fields) = &mut value {
        fields.insert("child".into(), types::child(7, 404));
    }
    let ty = registry
        .get_by_name(LookupKey::Qualified(types::ROOT))
        .unwrap();
    let bytes = value.to_bytes(&registry, ty)?;
    let prepared = stage.prepare_with(|staged| {
        staged.write_resource(&archive::resource_id(0), &bytes)?;
        Ok(())
    })?;
    drop(reader);
    let published = prepared.publish()?;
    if handoff == crate::ownership::SfHandoff::Deferred {
        // kiln mode: the generation's change journal, bound to the receipt the publication left, as kiln-eml's
        // startup controller writes it after its one publication
        let receipt = published
            .receipt_sha256()
            .ok_or_else(|| anyhow::anyhow!("the publication left no receipt"))?
            .to_owned();
        crate::startup_host::StartupJournal::bind(game.as_std_path(), STEM, &lock)?
            .write_for(&receipt, &journal_record(&receipt))?;
    }
    Ok(())
}

/// A change journal as the matrix writes it: only its binding (the receipt it names) matters here; kiln-eml's own
/// tests check the record.
fn journal_record(receipt: &str) -> Vec<u8> {
    format!("{{\"receiptSha256\":\"{receipt}\"}}").into_bytes()
}

/// The receipt the change journal on disk names, None when there is none.
fn journal_names(f: &ArchiveFixture) -> Option<String> {
    let bytes = fs::read(f.game.join(format!("kiln/eml/ownership/{STEM}.change-journal.json"))).ok()?;
    let value: serde_json::Value = serde_json::from_slice(&bytes).expect("the change journal is whole JSON");
    Some(value["receiptSha256"].as_str().expect("the journal names a receipt").to_owned())
}

fn receipt_marker(f: &ArchiveFixture) -> Option<String> {
    crate::ownership::marker(f.game.as_std_path(), STEM).unwrap()
}

/// The setup's change journal for the generation now live (a kiln-mode start journaled it).
fn plant_journal(f: &ArchiveFixture) -> String {
    let receipt = receipt_marker(f).expect("the setup published");
    fs::write(
        f.game.join(format!("kiln/eml/ownership/{STEM}.change-journal.json")),
        journal_record(&receipt),
    )
    .unwrap();
    receipt
}

/// A change journal on disk names the receipt beside it, whose generation is live and owned, published whole: the
/// setup's (the old set still live) or the step's (the new set).
fn journal_bound(case: &Case, state: &str, label: &str) {
    let f = &case.f;
    let Some(named) = journal_names(f) else { return };
    assert_eq!(
        receipt_marker(f).as_deref(),
        Some(named.as_str()),
        "{label}: the change journal names another generation than the receipt's"
    );
    assert!(
        crate::owns_output(f.game.as_std_path(), STEM).unwrap(),
        "{label}: a change journal beside a receipt whose output is not live"
    );
    if case.journal.as_deref() == Some(named.as_str()) {
        assert_eq!(state, "old", "{label}: the setup's change journal outlived its generation");
    } else {
        assert_eq!(state, "new", "{label}: a change journal written before its generation was live");
    }
}

/// The scenario's step: the operation the matrix interrupts.
fn step(scenario: Scenario, root: &Utf8Path) -> bool {
    match scenario.mods() {
        Some(ids) => run(root, ids).is_ok(),
        None if scenario == Scenario::Lent => lent(root).is_ok(),
        None if scenario == Scenario::Deferred => lent_with(root, crate::ownership::SfHandoff::Deferred).is_ok(),
        None if scenario == Scenario::DeferredRestore => deferred_restore(root).is_ok(),
        None if scenario == Scenario::Record => record_write(root, RECORD_NEW).is_ok(),
        None => restore(root),
    }
}

const RECORD_OLD: &[u8] = b"{\"m6\":\"the startup record before\"}";
const RECORD_NEW: &[u8] = b"{\"m6\":\"the startup record after, longer than before\"}";

/// The `record` step: the startup cache's record replaced under the held install lock.
fn record_write(root: &Utf8Path, bytes: &[u8]) -> anyhow::Result<()> {
    let game = root.join("game");
    let _hold = test_gate::lock_hold();
    let lock = fs::OpenOptions::new()
        .create(true)
        .truncate(false)
        .write(true)
        .open(game.join(".kiln-eml.lock"))?;
    lock.try_lock()?;
    let record = crate::startup_host::StartupRecord::bind(game.as_std_path(), &format!("sf-startup-{STEM}.json"), &lock)?;
    record.write(bytes)?;
    Ok(())
}

fn record_bytes(f: &ArchiveFixture) -> Option<Vec<u8>> {
    fs::read(f.game.join(format!("kiln/eml/cache/sf-startup-{STEM}.json"))).ok()
}

/// Every file below `<game>/shroudforge`, by name, with its bytes.
fn sf_tree(f: &ArchiveFixture) -> Vec<(String, Vec<u8>)> {
    fn walk(base: &std::path::Path, dir: &std::path::Path, out: &mut Vec<(String, Vec<u8>)>) {
        let Ok(entries) = fs::read_dir(dir) else { return };
        for e in entries.flatten() {
            let path = e.path();
            let name = path.strip_prefix(base).unwrap().to_string_lossy().into_owned();
            if path.is_dir() {
                out.push((format!("{name}/"), Vec::new()));
                walk(base, &path, out);
            } else {
                out.push((name, fs::read(&path).unwrap()));
            }
        }
    }
    let base = f.game.join("shroudforge").into_std_path_buf();
    let mut out = Vec::new();
    walk(&base, &base, &mut out);
    out.sort();
    out
}

fn owed(f: &ArchiveFixture) -> bool {
    f.game.join(format!("kiln/eml/ownership/{STEM}.sf-handoff-owed.json")).exists()
}

fn foreign_path(f: &ArchiveFixture) -> Utf8PathBuf {
    f.game.join(format!("{STEM}_004.dat"))
}

fn setup(scenario: Scenario) -> Case {
    let f = ArchiveFixture::new(&format!("g03-{}", scenario.name()));
    package(&f.root.0, "content", WITH_CONTENT);
    package(&f.root.0, "edit", EDIT_ONLY);
    let vanilla = f.semantics();
    match scenario {
        Scenario::First => {}
        Scenario::Grow | Scenario::Lent | Scenario::Deferred => assert!(run(&f.root.0, &["edit"]).is_ok()),
        Scenario::Shrink | Scenario::Restore | Scenario::DeferredRestore => assert!(run(&f.root.0, &["content"]).is_ok()),
        Scenario::Record => {
            assert!(run(&f.root.0, &["edit"]).is_ok());
            record_write(&f.root.0, RECORD_OLD).unwrap();
        }
    }
    if scenario.has_sf() {
        crate::transaction::tests::sf_applied(f.game.as_std_path(), STEM);
    }
    if scenario.has_foreign() {
        assert!(
            !foreign_path(&f).exists(),
            "the foreign container must not overwrite owned output"
        );
        fs::write(foreign_path(&f), FOREIGN).unwrap();
    }
    let old = f.semantics();
    let old_owned = crate::owns_output(f.game.as_std_path(), STEM).unwrap();
    let sf_tree = sf_tree(&f);
    // The kiln-mode scenarios and EML's own no-mod restore start over a journaled generation (a kiln-mode start
    // journaled it; the restore runs when the player left kiln mode)
    let journal = (scenario.deferred() || scenario == Scenario::Restore).then(|| plant_journal(&f));
    Case {
        f,
        vanilla,
        old,
        old_owned,
        sf_tree,
        journal,
    }
}

/// The boundaries the uninterrupted step passes, and the semantics it leaves.
fn reference(scenario: Scenario) -> (Vec<Boundary>, SemanticSet) {
    let case = setup(scenario);
    failpoint::record();
    assert!(
        step(scenario, &case.f.root.0),
        "uninterrupted {} step",
        scenario.name()
    );
    let seen = failpoint::take();
    (seen, case.f.semantics())
}

fn sf_status(f: &ArchiveFixture) -> Option<String> {
    let bytes = fs::read(f.game.join("shroudforge/state.json")).ok()?;
    let value: serde_json::Value = serde_json::from_slice(&bytes).ok()?;
    value["assets"]["status"].as_str().map(str::to_owned)
}

fn foreign_copies(f: &ArchiveFixture) -> usize {
    let mut paths = vec![foreign_path(f).into_std_path_buf()];
    if let Ok(entries) = fs::read_dir(f.game.join("kiln/eml/displaced")) {
        paths.extend(
            entries
                .flatten()
                .map(|e| e.path().join(format!("{STEM}_004.dat"))),
        );
    }
    paths
        .iter()
        .filter(|p| fs::read(p).is_ok_and(|b| b == FOREIGN))
        .count()
}

fn stages(f: &ArchiveFixture) -> Vec<std::path::PathBuf> {
    fs::read_dir(&f.game)
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

/// The live set right after an interruption: which proven state it is, never a mixed one. Panics with `label`.
fn interrupted_state(
    case: &Case,
    scenario: Scenario,
    new: &SemanticSet,
    label: &str,
) -> &'static str {
    let f = &case.f;
    let live = archive::semantics(&f.game, STEM, &f.types).unwrap_or_else(|e| {
        panic!("{label}: the live archive does not reopen and validate: {e:#}")
    });
    let state = if live == *new {
        "new"
    } else if live == case.old {
        "old"
    } else if live == case.vanilla {
        "base"
    } else {
        panic!("{label}: a mixed set is live (neither old, safe base nor complete new)")
    };
    let owned = crate::owns_output(f.game.as_std_path(), STEM)
        .unwrap_or_else(|e| panic!("{label}: the receipt cannot be read: {e}"));
    assert!(
        !owned || state == "new" || (state == "old" && case.old_owned),
        "{label}: the receipt claims a set it did not publish ({state})"
    );
    if scenario == Scenario::Record {
        let bytes = record_bytes(f);
        assert!(
            bytes.as_deref() == Some(RECORD_OLD) || bytes.as_deref() == Some(RECORD_NEW),
            "{label}: the startup record is neither the old nor the new one: {bytes:?}"
        );
    }
    if case.journal.is_some() {
        journal_bound(case, state, label);
    }
    if scenario.deferred() {
        // kiln mode: ShroudForge's folder byte for byte as before, and a change of the live pair owes the handoff
        assert!(sf_tree(f) == case.sf_tree, "{label}: kiln mode touched ShroudForge's folder");
        assert!(
            state == "old" || owed(f),
            "{label}: game data changed ({state}) and no ShroudForge handoff is recorded as owed"
        );
    } else if scenario.has_sf() {
        let status = sf_status(f);
        assert!(
            state == "old" || status.as_deref() == Some("failed"),
            "{label}: game data changed ({state}) while ShroudForge's applied claim stands ({status:?})"
        );
    }
    for (name, original) in [
        ("kfc", &f.original.0[&format!("{STEM}.kfc")]),
        (
            "kfc_resources",
            &f.original.0[&format!("{STEM}.kfc_resources")],
        ),
    ] {
        if let Ok(bytes) = fs::read(f.game.join(format!("{STEM}.{name}.bak"))) {
            assert!(
                bytes == *original,
                "{label}: a partial or foreign {name}.bak is in place"
            );
        }
    }
    if scenario.has_foreign() {
        assert_eq!(
            foreign_copies(f),
            1,
            "{label}: the foreign container was lost or duplicated"
        );
    }
    state
}

/// The next starts: twice prune and the same mods (or the no-mod restore), then a no-mod restore, twice.
fn recover(case: &Case, scenario: Scenario, new: &SemanticSet, label: &str) {
    let f = &case.f;
    let planted_child: Vec<_> = stages(f)
        .into_iter()
        .map(|stage| {
            let path = stage.join("zz-foreign-child.txt");
            fs::write(&path, b"not Kiln's").unwrap();
            path
        })
        .collect();
    let unrecorded = f
        .game
        .join(format!("{STEM}.kfc.kiln-{}", uuid::Uuid::new_v4()));
    fs::write(&unrecorded, b"unrecorded copy").unwrap();
    for round in 0..2 {
        crate::prune(f.game.as_std_path(), STEM);
        let redone = match scenario.mods() {
            Some(ids) => Some(run(&f.root.0, ids).map(drop)),
            None if scenario == Scenario::Lent => Some(lent(&f.root.0)),
            None if scenario == Scenario::Deferred => Some(lent_with(&f.root.0, crate::ownership::SfHandoff::Deferred)),
            None => None,
        };
        match redone {
            Some(redone) => {
                if let Err(e) = redone {
                    panic!("{label}: recovery run {round} failed: {e:#}");
                }
                assert_eq!(
                    f.semantics(),
                    *new,
                    "{label}: recovery run {round} did not publish the complete new set"
                );
                assert!(
                    crate::owns_output(f.game.as_std_path(), STEM).unwrap(),
                    "{label}: recovered output not owned"
                );
            }
            None => {
                let restored = if scenario == Scenario::DeferredRestore {
                    deferred_restore(&f.root.0).map_err(|e| eprintln!("{label}: {e:#}")).is_ok()
                } else {
                    restore(&f.root.0)
                };
                assert!(restored, "{label}: recovery restore {round} failed");
                assert_eq!(
                    f.semantics(),
                    case.vanilla,
                    "{label}: recovery restore {round} left non-vanilla data"
                );
            }
        }
    }
    // Kiln mode's recovery never leaves a change journal for another generation than the live one: the redone
    // publication's, bound to its receipt, or none after the verified BASE
    match scenario {
        Scenario::Deferred => {
            let named = journal_names(f).unwrap_or_else(|| panic!("{label}: the recovered generation has no journal"));
            assert_eq!(receipt_marker(f), Some(named), "{label}: the journal names another generation");
            assert!(crate::owns_output(f.game.as_std_path(), STEM).unwrap(), "{label}");
        }
        Scenario::DeferredRestore => {
            assert_eq!(journal_names(f), None, "{label}: a change journal outlived the verified BASE")
        }
        Scenario::Restore => {
            assert_eq!(journal_names(f), None, "{label}: a change journal outlived EML's own restore")
        }
        _ => {}
    }
    for round in 0..2 {
        crate::prune(f.game.as_std_path(), STEM);
        assert!(restore(&f.root.0), "{label}: final restore {round} failed");
        assert_eq!(
            f.semantics(),
            case.vanilla,
            "{label}: final restore {round}"
        );
    }
    // No change journal outlives the no-mod restore that took its generation away (EML's own restore removes it
    // before the first live file changes)
    assert_eq!(journal_names(f), None, "{label}: a change journal outlived the final restore");
    for path in &planted_child {
        assert_eq!(
            fs::read(path).unwrap(),
            b"not Kiln's",
            "{label}: a foreign stage child was deleted"
        );
    }
    assert_eq!(
        fs::read(&unrecorded).unwrap(),
        b"unrecorded copy",
        "{label}: an unrecorded copy was deleted"
    );
    if scenario.has_foreign() {
        assert_eq!(
            foreign_copies(f),
            1,
            "{label}: the foreign container did not survive recovery exactly once"
        );
    }
    if scenario == Scenario::Record {
        // the next start's clean-up removes the record's temp by its name; the record is rewritten whole
        crate::prune(f.game.as_std_path(), STEM);
        let cache = f.game.join("kiln/eml/cache");
        let temps: Vec<String> = fs::read_dir(&cache)
            .unwrap()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|name| crate::ownership::temp_of(name).is_some())
            .collect();
        assert_eq!(temps, Vec::<String>::new(), "{label}: a startup record temp survived the clean-up");
        record_write(&f.root.0, RECORD_NEW).unwrap();
        assert_eq!(record_bytes(f).as_deref(), Some(RECORD_NEW), "{label}");
    }
    if scenario.deferred() {
        // the stale applied claim is owed a handoff until a start that may touch ShroudForge's folder makes it
        assert!(
            sf_status(f).as_deref() != Some("applied") || owed(f),
            "{label}: a stale applied claim with no owed handoff"
        );
        crate::ownership::settle_owed_handoff(f.game.as_std_path(), STEM).unwrap();
        assert!(!owed(f), "{label}: the owed handoff was not settled");
    }
    if scenario.has_sf() {
        assert_ne!(
            sf_status(f).as_deref(),
            Some("applied"),
            "{label}: a stale applied claim came back"
        );
    }
    no_kiln_residue(f, &unrecorded, label);
}

/// Recovery really ran: what the interrupted step left of Kiln's own is gone. No stage keeps a file Kiln staged (only
/// the planted foreign child), no recorded game-folder temp and no ownership-folder temp is left (only the planted
/// unrecorded copy), and a scratch record is kept only for a stage it cannot prove (nothing deleted there).
fn no_kiln_residue(f: &ArchiveFixture, unrecorded: &Utf8Path, label: &str) {
    for stage in stages(f) {
        for child in fs::read_dir(&stage).unwrap().flatten() {
            assert_eq!(
                child.file_name(),
                "zz-foreign-child.txt",
                "{label}: Kiln's staged bytes survived recovery in {}",
                stage.display()
            );
        }
    }
    let temps = |dir: &Utf8Path| -> Vec<String> {
        fs::read_dir(dir)
            .unwrap()
            .flatten()
            .filter(|e| e.path() != unrecorded.as_std_path())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|name| crate::ownership::temp_of(name).is_some())
            .collect()
    };
    let ownership = f.game.join("kiln/eml/ownership");
    assert_eq!(
        temps(&f.game),
        Vec::<String>::new(),
        "{label}: a recorded temp survived recovery"
    );
    assert_eq!(
        temps(&ownership),
        Vec::<String>::new(),
        "{label}: an ownership temp survived recovery"
    );
    for e in fs::read_dir(&ownership).unwrap().flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if name.starts_with("cleanup-") {
            let record: serde_json::Value =
                serde_json::from_slice(&fs::read(e.path()).unwrap()).unwrap();
            let target = record["name"].as_str().unwrap_or_default();
            assert!(
                target.starts_with(".kiln-eml-stage-")
                    && record.get("stage").is_none()
                    && f.game.join(target).is_dir(),
                "{label}: recovery kept a scratch record it could act on: {record}"
            );
        }
    }
}

/// One line per interruption, for the report (`G03_TABLE_OUT=<dir>`).
fn table(scenario: Scenario, mode: &str, rows: &[String]) {
    if let Some(dir) = std::env::var_os("G03_TABLE_OUT") {
        let dir = std::path::PathBuf::from(dir);
        fs::create_dir_all(&dir).unwrap();
        fs::write(
            dir.join(format!("{}-{mode}.tsv", scenario.name())),
            rows.join("\n") + "\n",
        )
        .unwrap();
    }
}

fn failure_matrix(scenario: Scenario) {
    let (boundaries, new) = reference(scenario);
    assert!(!boundaries.is_empty());
    let mut rows = Vec::new();
    for (at, boundary) in boundaries.iter().enumerate() {
        let label = format!(
            "{} fail #{at} {} {}",
            scenario.name(),
            boundary.point,
            boundary.detail
        );
        let case = setup(scenario);
        failpoint::arm(at, Action::Fail, None);
        let ok = step(scenario, &case.f.root.0);
        let seen = failpoint::take();
        assert!(
            seen.len() > at && seen[at] == *boundary,
            "{label}: boundary not reached at the same index: {:?}",
            seen.get(at)
        );
        let state = interrupted_state(&case, scenario, &new, &label);
        rows.push(format!(
            "{}\tfail\t{at}\t{}\t{}\t{}\t{state}",
            scenario.name(),
            boundary.point,
            boundary.detail,
            if ok { "ok" } else { "error" }
        ));
        recover(&case, scenario, &new, &label);
    }
    table(scenario, "fail", &rows);
}

fn kill_matrix(scenario: Scenario) {
    let (boundaries, new) = reference(scenario);
    let mut rows = Vec::new();
    for (at, boundary) in boundaries.iter().enumerate() {
        let label = format!(
            "{} kill #{at} {} {}",
            scenario.name(),
            boundary.point,
            boundary.detail
        );
        let case = setup(scenario);
        let marker = case.f.root.0.join("killed-at");
        let spec = serde_json::json!({"scenario": scenario.name(), "root": case.f.root.0.as_str(), "at": at, "marker": marker.as_str()});
        let mut command = Command::new(std::env::current_exe().unwrap());
        command
            .args([
                "--exact",
                "publication_tests::kill_child",
                "--ignored",
                "--test-threads=1",
            ])
            .env("KILN_PUBLICATION_KILL_CHILD", spec.to_string())
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        let mut child = test_gate::spawn(&mut command).unwrap();
        let started = Instant::now();
        while !marker.exists() {
            if let Some(status) = child.try_wait().unwrap() {
                panic!("{label}: the child exited ({status}) without reaching the boundary");
            }
            if started.elapsed() > Duration::from_secs(120) {
                let _ = child.kill();
                let _ = child.wait();
                panic!("{label}: the child did not reach the boundary in time");
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        child.kill().unwrap();
        child.wait().unwrap();
        assert_eq!(
            fs::read_to_string(&marker).unwrap(),
            format!("{} {}", boundary.point, boundary.detail),
            "{label}: the child stopped at another boundary"
        );
        fs::remove_file(&marker).unwrap();
        let state = interrupted_state(&case, scenario, &new, &label);
        rows.push(format!(
            "{}\tkill\t{at}\t{}\t{}\tkilled\t{state}",
            scenario.name(),
            boundary.point,
            boundary.detail
        ));
        recover(&case, scenario, &new, &label);
    }
    table(scenario, "kill", &rows);
}

/// The kill matrix's child process: runs the step with boundary `at` armed to stop there until it is killed.
#[test]
#[ignore = "child process of the publication kill matrix; does nothing without KILN_PUBLICATION_KILL_CHILD"]
fn kill_child() {
    let Ok(spec) = std::env::var("KILN_PUBLICATION_KILL_CHILD") else {
        return;
    };
    let spec: serde_json::Value = serde_json::from_str(&spec).unwrap();
    let root = Utf8PathBuf::from(spec["root"].as_str().unwrap());
    let marker = std::path::PathBuf::from(spec["marker"].as_str().unwrap());
    failpoint::arm(
        spec["at"].as_u64().unwrap() as usize,
        Action::Kill,
        Some(&marker),
    );
    step(Scenario::parse(spec["scenario"].as_str().unwrap()), &root);
    // Reaching here means the boundary was never passed: the parent sees the exit without a marker.
}

/// The kill matrix creates its children while the other tests of this process take and release file locks. A lock taken
/// with `File::try_lock` (flock on Unix, as ShroudForge's own fs2 lock) belongs to the open file description, and a new
/// process shares every description its creator has open until its exec closes the copies. A child created while
/// ShroudForge's lock is held so keeps it after its holder released it, and the next start's lock fails "ShroudForge
/// state is in use" (the matrix flake of 2026-11-19). Here the child waits 500 ms before its exec, as a child of a
/// loaded machine can; `test_gate::spawn` must not create it while any lock is held (each round checks that it waits),
/// so a released lock is free. The second round takes the lock again under an outer hold, as a test that locks
/// ShroudForge's file itself around a run does: that take does not wait for a spawn, so no spawn may happen while the
/// outer hold lasts either.
#[cfg(unix)]
#[test]
fn a_child_process_never_keeps_a_released_shroudforge_lock() {
    for outer in [false, true] {
        released_lock_round(outer);
    }
}

#[cfg(unix)]
fn released_lock_round(outer: bool) {
    use std::{
        io::{Read, Write},
        os::unix::process::CommandExt,
        sync::{Arc, atomic::AtomicBool},
    };
    let f = archive::ArchiveFixture::new("g03-spawn");
    crate::transaction::tests::sf_applied(f.game.as_std_path(), STEM);
    let game = f.game.clone().into_std_path_buf();
    let outer_hold = outer.then(test_gate::lock_hold);
    let held = crate::ownership::before_baseline(&game, STEM, crate::ownership::SfHandoff::Live).unwrap();
    assert!(held.is_some(), "ShroudForge's write lock is held (outer={outer})");
    let (mut reader, writer) = std::io::pipe().unwrap();
    let created = Arc::new(AtomicBool::new(false));
    let watcher = {
        let created = created.clone();
        std::thread::spawn(move || {
            let mut byte = [0];
            if reader.read(&mut byte).unwrap_or(0) == 1 {
                created.store(true, Ordering::SeqCst);
            }
        })
    };
    let spawner = std::thread::spawn(move || {
        let mut command = Command::new(std::env::current_exe().unwrap());
        command
            .args(["--exact", "publication_tests::kill_child", "--ignored"])
            .env_remove("KILN_PUBLICATION_KILL_CHILD")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        // SAFETY: between its creation and its exec the child only calls write(2) and nanosleep(2), which are
        // async-signal-safe.
        unsafe {
            command.pre_exec(move || {
                let _ = (&writer).write(b"c");
                std::thread::sleep(Duration::from_millis(500));
                Ok(())
            });
        }
        test_gate::spawn(&mut command).unwrap().wait().unwrap()
    });
    // The spawn is under way: the child exists (and shares this process's descriptors), or its spawn found the lock
    // holders and waits for them.
    let started = Instant::now();
    let spawner_thread = spawner.thread().id();
    while !created.load(Ordering::SeqCst) && !test_gate::blocked(spawner_thread) {
        assert!(started.elapsed() < Duration::from_secs(120), "the spawn never began (outer={outer})");
        std::thread::sleep(Duration::from_millis(1));
    }
    // No process is created while the lock is held (the child would share its description): the spawn waits.
    assert!(!created.load(Ordering::SeqCst), "a process was created while ShroudForge's lock was held (outer={outer})");
    drop(held);
    let again = crate::ownership::before_baseline(&game, STEM, crate::ownership::SfHandoff::Live);
    assert!(
        matches!(again, Ok(Some(_))),
        "a child process keeps the released lock (outer={outer}): {:?}",
        again.err()
    );
    drop(again);
    // The spawn waits for this hold: release it before waiting for the child.
    drop(outer_hold);
    assert!(spawner.join().unwrap().success());
    watcher.join().unwrap();
}

/// While a child process is being created, no thread takes its first file lock: the child's exec, which closes its
/// copies of this process's descriptors, comes first. The child sends its process id and waits 500 ms before its exec;
/// a lock hold taken meanwhile returns only once the child no longer has the descriptors it was created with (here the
/// write end of the pipe it sent its id through).
#[cfg(target_os = "linux")]
#[test]
fn no_file_lock_is_taken_while_a_child_process_is_being_created() {
    use std::{
        io::{Read, Write},
        os::{fd::AsRawFd, unix::process::CommandExt},
    };
    let (mut reader, writer) = std::io::pipe().unwrap();
    let fd = writer.as_raw_fd();
    let pipe = fs::read_link(format!("/proc/self/fd/{fd}")).unwrap();
    let spawner = std::thread::spawn(move || {
        let mut command = Command::new(std::env::current_exe().unwrap());
        command
            .args(["--exact", "publication_tests::kill_child", "--ignored"])
            .env_remove("KILN_PUBLICATION_KILL_CHILD")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        // SAFETY: between its creation and its exec the child only calls getpid(2), write(2) and nanosleep(2), which
        // are async-signal-safe.
        unsafe {
            command.pre_exec(move || {
                let _ = (&writer).write(&std::process::id().to_le_bytes());
                std::thread::sleep(Duration::from_millis(500));
                Ok(())
            });
        }
        test_gate::spawn(&mut command).unwrap().wait().unwrap()
    });
    let mut pid = [0; 4];
    reader.read_exact(&mut pid).unwrap();
    let pid = u32::from_le_bytes(pid);
    let hold = test_gate::lock_hold();
    let copy = fs::read_link(format!("/proc/{pid}/fd/{fd}")).ok();
    assert_ne!(
        copy.as_ref(),
        Some(&pipe),
        "a lock hold was taken while the child still had this process's descriptors"
    );
    drop(hold);
    assert!(spawner.join().unwrap().success());
}

/// The ShroudForge write lock's guard closes its file before it releases its hold: in between, a spawn the release lets
/// through would hand the still-locked description to its child. The check runs right after this thread's last hold is
/// released (`test_gate::on_last_release`): by then the lock must be free (a guard that drops its hold before its file
/// still has the file locked there).
#[test]
fn the_sf_lock_guard_closes_its_file_before_releasing_its_hold() {
    let f = archive::ArchiveFixture::new("g03-hold-order");
    crate::transaction::tests::sf_applied(f.game.as_std_path(), STEM);
    let game = f.game.clone().into_std_path_buf();
    let guard = crate::ownership::before_baseline(&game, STEM, crate::ownership::SfHandoff::Live).unwrap();
    assert!(guard.is_some(), "ShroudForge's write lock is held");
    let free = std::rc::Rc::new(std::cell::Cell::new(None));
    {
        let (free, game) = (free.clone(), game.clone());
        test_gate::on_last_release(move || free.set(Some(!crate::transaction::tests::sf_write_lock_busy(&game))));
    }
    drop(guard);
    assert_eq!(
        free.get(),
        Some(true),
        "the guard's hold was released while its file still held ShroudForge's lock (Some(false)), or never (None)"
    );
}

/// Every file lock of the code compiled into this test binary is taken under a `LockHold` taken earlier in the same
/// function, and every process it creates goes through `test_gate::spawn`: a lock or a process creation that bypasses
/// the gate brings the kill matrix's flake back, and this names it. Scans the sources of mod-loader-lua and of the EML
/// crates next to it (`test_gate::bypasses`); the mutexes below are listed by their receiver. Source text is not on the
/// Windows runners.
#[cfg(not(windows))]
#[test]
fn every_file_lock_and_process_of_this_crate_goes_through_the_gate() {
    use std::path::Path;
    // (path under eml/, receiver): mutexes, not files.
    const MUTEXES: [(&str, &str); 6] = [
        ("mod-loader-lua/src/progress.rs", "m"),
        ("mod-loader-lua/src/test_gate.rs", "GATE"),
        ("mod-loader-base/src/registry/mod.rs", "self.inner.fs"),
        ("kfc-base/src/container/reader.rs", "entry.mutex"),
        // the threaded BC encode: its finished strips and the test hook that holds helper threads back (08cf113a)
        ("kfc-content/src/image/encode.rs", "done"),
        ("kfc-content/src/image/encode.rs", "open"),
    ];
    let eml = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
    let mut files = Vec::new();
    for entry in fs::read_dir(eml).unwrap() {
        let src = entry.unwrap().path().join("src");
        if src.is_dir() {
            test_gate::sources(&src, &mut files);
        }
    }
    assert!(
        files.iter().any(|f| f.ends_with("mod-loader-lua/src/ownership.rs")),
        "the scan reaches this crate's sources"
    );
    let bypass = test_gate::bypasses(eml, &files, &MUTEXES);
    assert!(bypass.is_empty(), "outside the test gate (test_gate.rs):\n{}", bypass.join("\n"));
}

/// The change journal is written only for the receipt that is live now and only under the held install guard; an
/// EML-mode (Live) publication over a journaled generation removes it before its receipt changes, and so does EML's own
/// no-mod restore before the live files change; the next start's clean-up removes a journal temp a crash left.
#[test]
fn the_change_journal_is_bound_to_the_live_receipt_and_goes_before_it_changes() {
    let f = ArchiveFixture::new("r1-journal");
    package(&f.root.0, "content", WITH_CONTENT);
    package(&f.root.0, "edit", EDIT_ONLY);
    assert!(run(&f.root.0, &["edit"]).is_ok());
    let receipt = receipt_marker(&f).unwrap();
    {
        let _hold = test_gate::lock_hold();
        let lock = fs::OpenOptions::new()
            .create(true)
            .truncate(false)
            .write(true)
            .open(f.game.join(".kiln-eml.lock"))
            .unwrap();
        assert!(
            crate::startup_host::StartupJournal::bind(f.game.as_std_path(), STEM, &lock).is_err(),
            "bound without the held install guard"
        );
        lock.try_lock().unwrap();
        let journal = crate::startup_host::StartupJournal::bind(f.game.as_std_path(), STEM, &lock).unwrap();
        assert_eq!(journal.read().unwrap(), None);
        assert!(
            journal.write_for(&"00".repeat(32), &journal_record(&"00".repeat(32))).is_err(),
            "written for a receipt that is not the live one"
        );
        assert_eq!(journal_names(&f), None);
        journal.write_for(&receipt, &journal_record(&receipt)).unwrap();
        assert_eq!(journal.read().unwrap(), Some(journal_record(&receipt)));
    }
    assert!(run(&f.root.0, &["content"]).is_ok());
    assert_ne!(receipt_marker(&f).unwrap(), receipt, "a new generation");
    assert_eq!(journal_names(&f), None, "the old generation's journal outlived its receipt");
    // EML's own no-mod restore over a journaled generation (the player left kiln mode): the journal goes before the
    // live files change, so it never names a receipt whose output is no longer live
    plant_journal(&f);
    assert!(restore(&f.root.0), "the no-mod restore");
    assert!(!crate::owns_output(f.game.as_std_path(), STEM).unwrap(), "the restore took the generation away");
    assert_eq!(journal_names(&f), None, "the journal outlived EML's own restore");
    let temp = f
        .game
        .join(format!("kiln/eml/ownership/{STEM}.change-journal.kiln-{}", uuid::Uuid::new_v4()));
    fs::write(&temp, b"half a journal").unwrap();
    crate::prune(f.game.as_std_path(), STEM);
    assert!(!temp.exists(), "a journal temp survived the clean-up");
}

/// The checkpoint enumeration: every named boundary of the publication plan that a scenario's step passes, in order.
/// Before the failpoints existed this recorded nothing (the red witness).
#[test]
fn every_named_publication_boundary_is_passed_in_order() {
    for scenario in SCENARIOS {
        let (seen, _) = reference(scenario);
        let names: Vec<String> = seen
            .iter()
            .map(|b| format!("{} {}", b.point, b.detail))
            .collect();
        for point in required(scenario) {
            assert!(
                names.iter().any(|n| n == point),
                "{}: boundary `{point}` never passed (recorded: {names:#?})",
                scenario.name()
            );
        }
        if let Some(dir) = std::env::var_os("G03_TABLE_OUT") {
            let dir = std::path::PathBuf::from(dir);
            fs::create_dir_all(&dir).unwrap();
            fs::write(
                dir.join(format!("{}-boundaries.txt", scenario.name())),
                names.join("\n") + "\n",
            )
            .unwrap();
        }
        let expected: Vec<&str> = expected(scenario)
            .lines()
            .map(str::trim)
            .filter(|l| !l.is_empty())
            .collect();
        assert_eq!(names, expected, "{} boundaries", scenario.name());
    }
}

/// The publication plan's named boundaries each scenario must pass (items 1-7), independent of the frozen order.
fn required(scenario: Scenario) -> Vec<&'static str> {
    let staging = [
        "stage.recorded game/.kiln-eml-stage-*",
        "stage.created game/.kiln-eml-stage-*",
        "stage.bound game/.kiln-eml-stage-*",
        "temp.replaced stage/.vanilla-header",
        "temp.replaced stage/enshrouded.kfc_resources",
        "stage.writer game/.kiln-eml-stage-*",
        "stage.resource-written stage/resource",
        "stage.readers-closed stage/readers",
        "prepare.finalized game/.kiln-eml-stage-*",
        "prepare.synced game/.kiln-eml-stage-*",
        "prepare.validated game/.kiln-eml-stage-*",
        "temp.replaced ownership/enshrouded.json",
        "temp.replaced game/enshrouded.kfc",
        "publish.vanilla-header game/enshrouded.kfc",
        "publish.replaced game/enshrouded.kfc_resources",
        "publish.header game/enshrouded.kfc",
        "cleanup.stage-file-removed stage/.vanilla-header",
        "cleanup.stage-removed game/.kiln-eml-stage-*",
        "cleanup.record-removed ownership/cleanup-*.json",
        "temp.replaced cache/files.json",
    ];
    let handoff = [
        "handoff.locked config/.shroudforge-write.lock",
        "temp.replaced ownership/sf-handoff-*.json",
        "temp.replaced shroudforge/state.json",
    ];
    let displacement = [
        "displace.folder displaced/*",
        "temp.replaced */moves.json",
        "displace.moved game/enshrouded_004.dat",
        "publish.replaced game/enshrouded_004.dat",
        "stage.content-written stage/content",
    ];
    let mut points: Vec<&str> = Vec::new();
    match scenario {
        Scenario::First => {
            points.extend([
                "temp.recorded game/enshrouded.kfc_resources.bak",
                "temp.written game/enshrouded.kfc_resources.bak",
                "temp.replaced game/enshrouded.kfc_resources.bak",
                "temp.record-removed game/enshrouded.kfc_resources.bak",
                "temp.replaced game/enshrouded.kfc.bak",
                "temp.replaced game/enshrouded.kfc.bak.integrity",
            ]);
            points.extend(staging);
            points.extend(handoff);
            points.extend(displacement);
        }
        Scenario::Grow => {
            points.extend(staging);
            points.extend(handoff);
            points.extend(displacement);
        }
        Scenario::Shrink => {
            points.extend(staging);
            points.extend(handoff);
            points.push("cleanup.container-removed game/enshrouded_004.dat");
        }
        Scenario::Restore => points.extend([
            // The setup's change journal goes before the restore changes a live file
            "journal.removed ownership/enshrouded.change-journal.json",
            "temp.replaced game/enshrouded.kfc",
            "temp.replaced game/enshrouded.kfc_resources",
            "cleanup.container-removed game/enshrouded_004.dat",
        ]),
        Scenario::Deferred => {
            // the lent pass in kiln mode: no ShroudForge lock or state handoff, the owed handoff recorded instead
            let eml = [
                "stage.resource-written stage/resource",
                "stage.readers-closed stage/readers",
                "stage.content-written stage/content",
                "temp.replaced cache/files.json",
            ];
            points.extend(staging.into_iter().filter(|p| !eml.contains(p)));
            points.extend([
                "stage.lent-content-written stage/content",
                "stage.lent-taken-back stage/writer",
                "stage.serialized-resource-written stage/resource",
                "temp.replaced ownership/enshrouded.sf-handoff-owed.json",
                // The setup's change journal goes before the receipt is replaced; the step's is written last
                "journal.removed ownership/enshrouded.change-journal.json",
                "temp.replaced ownership/enshrouded.change-journal.json",
            ]);
            points.extend(displacement.into_iter().filter(|p| !eml.contains(p)));
        }
        Scenario::Record => points.extend(["temp.written cache/sf-startup-enshrouded.json", "temp.replaced cache/sf-startup-enshrouded.json"]),
        Scenario::DeferredRestore => points.extend([
            "journal.removed ownership/enshrouded.change-journal.json",
            "temp.replaced ownership/enshrouded.sf-handoff-owed.json",
            "temp.replaced game/enshrouded.kfc",
            "temp.replaced game/enshrouded.kfc_resources",
            "cleanup.container-removed game/enshrouded_004.dat",
            "restore.receipt-removed ownership/enshrouded.json",
        ]),
        Scenario::Lent => {
            // EML's own writes and its change cache are not on this path; the lent writer's are.
            let eml = [
                "stage.resource-written stage/resource",
                "stage.readers-closed stage/readers",
                "stage.content-written stage/content",
                "temp.replaced cache/files.json",
            ];
            points.extend(staging.into_iter().filter(|p| !eml.contains(p)));
            points.extend([
                "stage.lent-content-written stage/content",
                "stage.lent-taken-back stage/writer",
                "stage.serialized-resource-written stage/resource",
            ]);
            points.extend(handoff);
            points.extend(displacement.into_iter().filter(|p| !eml.contains(p)));
        }
    }
    points
}

fn expected(scenario: Scenario) -> &'static str {
    match scenario {
        Scenario::First => include_str!("first.txt"),
        Scenario::Grow => include_str!("grow.txt"),
        Scenario::Shrink => include_str!("shrink.txt"),
        Scenario::Restore => include_str!("restore.txt"),
        Scenario::Lent => include_str!("lent.txt"),
        Scenario::Deferred => include_str!("deferred.txt"),
        Scenario::DeferredRestore => include_str!("deferred-restore.txt"),
        Scenario::Record => include_str!("record.txt"),
    }
}

#[test]
fn an_injected_failure_at_every_first_publication_boundary_is_old_base_or_new_and_recovers() {
    failure_matrix(Scenario::First);
}
#[test]
fn an_injected_failure_at_every_growing_replacement_boundary_is_old_base_or_new_and_recovers() {
    failure_matrix(Scenario::Grow);
}
#[test]
fn an_injected_failure_at_every_shrinking_replacement_boundary_is_old_base_or_new_and_recovers() {
    failure_matrix(Scenario::Shrink);
}
#[test]
fn an_injected_failure_at_every_restore_boundary_is_old_base_or_new_and_recovers() {
    failure_matrix(Scenario::Restore);
}
#[test]
fn a_kill_at_every_first_publication_boundary_is_old_base_or_new_and_recovers() {
    kill_matrix(Scenario::First);
}
#[test]
fn a_kill_at_every_growing_replacement_boundary_is_old_base_or_new_and_recovers() {
    kill_matrix(Scenario::Grow);
}
#[test]
fn a_kill_at_every_shrinking_replacement_boundary_is_old_base_or_new_and_recovers() {
    kill_matrix(Scenario::Shrink);
}
#[test]
fn a_kill_at_every_restore_boundary_is_old_base_or_new_and_recovers() {
    kill_matrix(Scenario::Restore);
}
#[test]
fn an_injected_failure_at_every_lent_pass_boundary_is_old_base_or_new_and_recovers() {
    failure_matrix(Scenario::Lent);
}
#[test]
fn a_kill_at_every_lent_pass_boundary_is_old_base_or_new_and_recovers() {
    kill_matrix(Scenario::Lent);
}
#[test]
fn an_injected_failure_at_every_deferred_lent_pass_boundary_is_old_base_or_new_and_recovers() {
    failure_matrix(Scenario::Deferred);
}
#[test]
fn a_kill_at_every_deferred_lent_pass_boundary_is_old_base_or_new_and_recovers() {
    kill_matrix(Scenario::Deferred);
}
#[test]
fn an_injected_failure_at_every_deferred_restore_boundary_is_old_base_or_new_and_recovers() {
    failure_matrix(Scenario::DeferredRestore);
}
#[test]
fn a_kill_at_every_deferred_restore_boundary_is_old_base_or_new_and_recovers() {
    kill_matrix(Scenario::DeferredRestore);
}
#[test]
fn an_injected_failure_at_every_startup_record_boundary_is_old_or_new_and_recovers() {
    failure_matrix(Scenario::Record);
}
#[test]
fn a_kill_at_every_startup_record_boundary_is_old_or_new_and_recovers() {
    kill_matrix(Scenario::Record);
}
