// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln-eml: which EML mod wrote which archive resource, and where in it: the record behind the EML dialect's change
//! journal (kiln-eml's `sf_startup::journal`, format 2). Never a value.
//!
//! **Who.** The runner runs one mod's entrypoint at a time (`LuaModRunner::run`: `require("mods.<id>")`); while it runs
//! that mod is the writer ([`begin`]), whoever's code it calls (a shared module, another mod's function): the running
//! entrypoint is the writer, as ShroudForge's journal names its packages. A write while no entrypoint runs is
//! unattributed (normally none: the runner runs no Lua between entrypoints).
//!
//! **Where.** Every value wrapper of a resource's data (`MappedStructValue`, `StructValue`, the arrays, the variants) keeps
//! a small log ([`Writes`]) of the writes made through it: a struct field assigned (`Field`), the value itself put in
//! place whole (`Whole`: an array element set or inserted, a variant's value replaced) or a list's membership changed
//! (`Items`: an insertion, removal or clear, or a plain element set). A resource's `data` replaced whole is noted on the
//! resource. Once the pass is over (before its resources are serialized), [`collect`] walks each resource's value tree as
//! it will be written and turns every log into a member path from the resource's root at its final place
//! (`recipes[37].craftingDuration`, `damageSetup`, `input[]`, or `""` for the whole resource), per writer. So:
//! - a write lands in the journal exactly when it can reach the published resource: a value a mod detached from its
//!   resource (replaced, removed) and wrote afterwards is not in the tree any more, and is not named;
//! - a copy assigned into a resource is that resource's write by the assigning mod (the copy's own log starts empty);
//! - list positions are the published ones: an element's index is where it is written, whatever moved it since.
//!
//! **Cost.** Off (one thread-local read) outside a run. In a run, a write costs a thread-local read and a scan of the
//! written value's own log (a few entries: one per writer and member), allocating only for a new entry; nothing walks or
//! compares a value until [`collect`], which walks the materialized values once, as EML's own dirty check before
//! serializing does.
use std::{
    cell::{Cell, RefCell},
    collections::{BTreeMap, BTreeSet, HashMap},
};

use kfc::guid::{ContentHash, ResourceId};

thread_local! {
    /// The running entrypoint's writer number (1-based, `Record::packages`); 0: none.
    static WRITER: Cell<u32> = const { Cell::new(0) };
    /// A run is recording (between `Recording::start` and its drop).
    static RECORDING: Cell<bool> = const { Cell::new(false) };
}

/// What one write changed in the value that logs it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum Member {
    /// The value itself was put in place whole (an array element set or inserted, a variant's value replaced).
    Whole,
    /// A struct field was assigned.
    Field(Box<str>),
    /// A list's membership changed (an insertion, removal or clear, or a plain element set).
    Items,
}

/// A value wrapper's log of the writes made through it: (writer, member), each once. Eight bytes until written.
#[derive(Default)]
pub(crate) struct Writes(Cell<Option<Box<Vec<(u32, Member)>>>>);

impl Writes {
    #[inline]
    pub(crate) fn whole(&self) {
        self.note(|m| *m == Member::Whole, || Member::Whole)
    }
    #[inline]
    pub(crate) fn items(&self) {
        self.note(|m| *m == Member::Items, || Member::Items)
    }
    #[inline]
    pub(crate) fn field(&self, key: &str) {
        self.note(|m| matches!(m, Member::Field(k) if &**k == key), || Member::Field(key.into()))
    }
    fn note(&self, same: impl Fn(&Member) -> bool, make: impl FnOnce() -> Member) {
        if !RECORDING.with(Cell::get) {
            return;
        }
        let writer = WRITER.with(Cell::get);
        let mut log = self.0.take().unwrap_or_default();
        if !log.iter().any(|(w, m)| *w == writer && same(m)) {
            log.push((writer, make()));
        }
        self.0.set(Some(log));
    }
    /// The member path of each logged write, `path` being this value's own path.
    pub(crate) fn emit(&self, path: &str, out: &mut Vec<(u32, String)>) {
        let log = self.0.take();
        if let Some(log) = &log {
            for (writer, member) in log.iter() {
                out.push((*writer, match member {
                    Member::Whole => path.to_owned(),
                    Member::Field(key) => field_path(path, key),
                    Member::Items => format!("{path}[]"),
                }));
            }
        }
        self.0.set(log);
    }
}

/// `path.key` (`key` at the root).
pub(crate) fn field_path(path: &str, key: &str) -> String {
    if path.is_empty() { key.to_owned() } else { format!("{path}.{key}") }
}

/// Whether a run is recording on this thread (the resource-level notes below check it).
pub(crate) fn recording() -> bool {
    RECORDING.with(Cell::get)
}
fn writer() -> u32 {
    WRITER.with(Cell::get)
}

/// One run's recording on this thread: on from `start` until the guard is dropped, whatever ends the run.
pub(crate) struct Recording(());
impl Recording {
    pub(crate) fn start() -> Self {
        RECORDING.with(|r| r.set(true));
        WRITER.with(|w| w.set(0));
        Self(())
    }
}
impl Drop for Recording {
    fn drop(&mut self) {
        RECORDING.with(|r| r.set(false));
        WRITER.with(|w| w.set(0));
    }
}

/// The running entrypoint: its writes are its own until the guard is dropped (the entrypoint returned or failed).
pub(crate) struct Entrypoint(());
impl Drop for Entrypoint {
    fn drop(&mut self) {
        WRITER.with(|w| w.set(0));
    }
}

/// What one run noted beside the value logs: its writers in the order their entrypoints ran, and the resource-level
/// writes (a resource's data replaced, a resource or content created).
#[derive(Default)]
pub(crate) struct Record {
    packages: Vec<String>,
    whole: Vec<(u32, ResourceId)>,
    created: Vec<(u32, ResourceId, bool)>,
    contents: Vec<(u32, ContentHash)>,
}

impl Record {
    /// `package`'s entrypoint begins.
    pub(crate) fn begin(record: &RefCell<Record>, package: &str) -> Entrypoint {
        let mut r = record.borrow_mut();
        r.packages.push(package.to_owned());
        let n = r.packages.len() as u32;
        if recording() {
            WRITER.with(|w| w.set(n));
        }
        Entrypoint(())
    }
    /// The running entrypoint replaced `id`'s data.
    pub(crate) fn replaced(record: &RefCell<Record>, id: ResourceId) {
        if recording() {
            record.borrow_mut().whole.push((writer(), id));
        }
    }
    /// The running entrypoint created `id`; `explicit`: with its own GUID and part.
    pub(crate) fn created(record: &RefCell<Record>, id: ResourceId, explicit: bool) {
        if recording() {
            record.borrow_mut().created.push((writer(), id, explicit));
        }
    }
    /// The running entrypoint created the content `hash` (or created it again).
    pub(crate) fn content(record: &RefCell<Record>, hash: ContentHash) {
        if recording() {
            record.borrow_mut().contents.push((writer(), hash));
        }
    }
}

/// One run's writes, per writer: the EML dialect's journal before kiln-eml bounds and formats it. Never a value.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct WriteRecord {
    /// Every mod whose entrypoint ran, in the order they ran (one that wrote nothing too).
    pub packages: Vec<PackageWrites>,
    /// Writes made while no entrypoint ran (normally none).
    pub unattributed: PackageWrites,
}

/// What one mod's entrypoint (or, without an id, everything outside an entrypoint) wrote.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PackageWrites {
    pub id: Option<String>,
    /// In resource-key order; never a resource the same writer created.
    pub changed: Vec<WrittenResource>,
    /// In creation order; ordinals 1, 2, ...
    pub created: Vec<CreatedResource>,
    /// Each once, in first-creation order.
    pub contents: Vec<ContentHash>,
}

/// A resource a writer wrote, and where.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WrittenResource {
    pub id: ResourceId,
    /// The writer (id, ordinal) that created it in this run, when another one did.
    pub created_by: Option<(String, u32)>,
    /// The member paths written, from the resource's root at their published places, sorted, each once; `[""]`: the
    /// whole resource (its data was replaced).
    pub paths: Vec<String>,
}

/// A resource a writer created, the `ordinal`-th (1-based) of its creations.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CreatedResource {
    pub ordinal: u32,
    pub id: ResourceId,
    pub explicit: bool,
}

/// The run's record from `record` and the value logs of `trees` (each resource whose value the run materialized, with the
/// writes its tree logged: (writer, path)).
pub(crate) fn collect(record: &Record, trees: Vec<(ResourceId, Vec<(u32, String)>)>) -> WriteRecord {
    let n = record.packages.len();
    let mut written: Vec<BTreeMap<ResourceId, BTreeSet<String>>> = vec![BTreeMap::new(); n + 1];
    let slot = |w: u32| (w as usize).min(n);
    for (w, id) in &record.whole {
        written[slot(*w)].entry(*id).or_default().insert(String::new());
    }
    for (id, writes) in trees {
        for (w, path) in writes {
            written[slot(w)].entry(id).or_default().insert(path);
        }
    }
    // creations: per writer in creation order, and who created what (the first creation of a key wins)
    let mut created: Vec<Vec<CreatedResource>> = vec![Vec::new(); n + 1];
    let mut creators: HashMap<ResourceId, (usize, u32)> = HashMap::new();
    for (w, id, explicit) in &record.created {
        let w = slot(*w);
        let ordinal = created[w].len() as u32 + 1;
        created[w].push(CreatedResource { ordinal, id: *id, explicit: *explicit });
        creators.entry(*id).or_insert((w, ordinal));
    }
    let mut contents: Vec<Vec<ContentHash>> = vec![Vec::new(); n + 1];
    for (w, hash) in &record.contents {
        let list = &mut contents[slot(*w)];
        if !list.contains(hash) {
            list.push(*hash);
        }
    }
    let name = |w: usize| (w >= 1).then(|| record.packages[w - 1].clone());
    let mut buckets: Vec<PackageWrites> = (0..=n)
        .map(|w| PackageWrites {
            id: name(w),
            changed: std::mem::take(&mut written[w])
                .into_iter()
                .filter(|(id, _)| creators.get(id).is_none_or(|(creator, _)| *creator != w))
                .map(|(id, paths)| WrittenResource {
                    id,
                    created_by: creators.get(&id).and_then(|(creator, ordinal)| Some((name(*creator)?, *ordinal))),
                    paths: if paths.contains("") { vec![String::new()] } else { paths.into_iter().collect() },
                })
                .collect(),
            created: std::mem::take(&mut created[w]),
            contents: std::mem::take(&mut contents[w]),
        })
        .collect();
    let unattributed = buckets.remove(0);
    WriteRecord { packages: buckets, unattributed }
}

#[cfg(test)]
mod tests {
    use super::*;
    use kfc::guid::Guid;

    fn id(n: u32) -> ResourceId {
        let mut bytes = [0u8; 16];
        bytes[..4].copy_from_slice(&n.to_le_bytes());
        ResourceId::new(Guid::from(bytes), 0x1234_5678, 0)
    }

    // A log notes only while a run records, each (writer, member) once; the writer is the running entrypoint's.
    #[test]
    fn a_log_notes_the_running_entrypoint_once_per_member() {
        let w = Writes::default();
        w.field("off");
        let mut out = Vec::new();
        w.emit("root", &mut out);
        assert!(out.is_empty(), "nothing is noted outside a run");
        let record = RefCell::new(Record::default());
        {
            let _run = Recording::start();
            w.items();
            {
                let _a = Record::begin(&record, "a");
                w.field("x");
                w.field("x");
                w.whole();
            }
            let _b = Record::begin(&record, "b");
            w.field("x");
        }
        w.field("after");
        w.emit("root", &mut out);
        assert_eq!(out, [(0, "root[]".into()), (1, "root.x".into()), (1, "root".into()), (2, "root.x".into())]);
    }

    // The record per writer: whole replacements are "", a writer's writes into its own creation are not listed, another's
    // name the creator and ordinal, contents once, unattributed apart.
    #[test]
    fn collect_groups_per_writer_and_names_creators() {
        let record = RefCell::new(Record::default());
        {
            let _run = Recording::start();
            Record::replaced(&record, id(9));
            {
                let _a = Record::begin(&record, "a");
                Record::created(&record, id(100), false);
                Record::created(&record, id(101), true);
                Record::content(&record, ContentHash::from_data(b"x"));
                Record::content(&record, ContentHash::from_data(b"x"));
            }
            let _b = Record::begin(&record, "b");
            Record::replaced(&record, id(1));
        }
        let trees = vec![
            (id(100), vec![(1, "own".to_string()), (2, "other".to_string())]),
            (id(1), vec![(2, "deep.x".to_string())]),
            (id(2), vec![(1, "b".to_string()), (1, "a[3]".to_string())]),
        ];
        let r = collect(&record.borrow(), trees);
        assert_eq!(r.unattributed.changed, [WrittenResource { id: id(9), created_by: None, paths: vec![String::new()] }]);
        let a = &r.packages[0];
        assert_eq!(a.id.as_deref(), Some("a"));
        assert_eq!(a.changed, [WrittenResource { id: id(2), created_by: None, paths: vec!["a[3]".into(), "b".into()] }]);
        assert_eq!(a.created.iter().map(|c| (c.ordinal, c.explicit)).collect::<Vec<_>>(), [(1, false), (2, true)]);
        assert_eq!(a.contents, [ContentHash::from_data(b"x")]);
        let b = &r.packages[1];
        assert_eq!(
            b.changed,
            [
                WrittenResource { id: id(1), created_by: None, paths: vec![String::new()] },
                WrittenResource { id: id(100), created_by: Some(("a".into(), 1)), paths: vec!["other".into()] },
            ]
        );
    }
}
