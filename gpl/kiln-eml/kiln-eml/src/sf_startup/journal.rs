// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln-eml (R1, the registry map's ruling): the startup change journal of the ShroudForge-dialect pass, kiln-eml's own
//! code. Per package of the committed attempt, which archive resources its entrypoint wrote, which it created (each
//! with a per-package ordinal) and which contents it created; never a value. A later Kiln registry stage diffs the base
//! against the published archive field by field and names each change's writer from this record; effect replay (dev.31
//! D6) names a package's changes by the same keys after a game update.
//!
//! **What counts, exactly.** The pass runs one package's entrypoint at a time ([`Recorder::begin`] / [`Recorder::end`]).
//! Every typed value of a resource carries that resource as its owner (`ValueObjects::owned_by`: the decoded value of
//! `Resource.data`, a value validated into a resource by `create_resource` or the `data` setter, every wrapper read
//! from those, and every copy assigned into them, which belongs to the resource it is assigned into, never to its
//! source). Each write through such a wrapper (an assignment, `table.insert`, `table.remove`, `table.clear`, a
//! variant's `value`) and each `data` assignment is noted for the package whose entrypoint is running, so:
//! - `changed` lists the resources a package wrote, whoever changed them before (a resource two packages edit is
//!   listed for both), also through a reference kept from an earlier package (a shared module) or written by another
//!   package's module function the entrypoint called (the running entrypoint is the writer, as ShroudForge names the
//!   failing package by its entrypoint); a resource a package only read is never listed. A write into a value the
//!   package itself detached from its resource earlier (a removed element, a replaced field kept in a variable) still
//!   names that resource: the journal can over-name a writer, never miss one;
//! - `created` lists the resources a package created, in creation order: the ordinal (1, 2, ...) is the resource's
//!   name across runs (`<package id>:<ordinal>`), since a created resource's random GUID differs per publication; it
//!   holds while the package creates in the same order (the Lua runtime seeds its string hashing per run, so creations
//!   made while walking a string-keyed table can be numbered differently); an explicit GUID is marked as such; a
//!   package's writes into its own creations are not listed again;
//! - `contents` lists each content a package created (`create_content`), once per package, first call first;
//! - code that runs inside an entrypoint is that entrypoint's, whoever wrote it (another package's module function, a
//!   finalizer the collector happens to run then); a write made while no entrypoint runs goes to `unattributed`
//!   (normally empty: the pass runs no Lua between entrypoints).
//!
//! No value is walked or compared here: the cost is one cell comparison per write, plus one set lookup the first time a
//! resource is written in an entrypoint.
//!
//! **Bounds.** At most [`PACKAGE_ENTRIES`] entries (changed + created + contents) per package (and for the unattributed
//! bucket), [`TOTAL_ENTRIES`] in all; what is over is left out and marks the package and the journal `truncated`, never
//! a failure. Created resources keep their ordinals (the ones left out are the last).
//!
//! The record (startup-journal.schema.json v1) is written by the startup controller beside the ownership receipt, bound
//! to the published generation (startup.rs); this module only records and formats.
use std::{
    cell::{Cell, RefCell},
    collections::{BTreeSet, HashMap, HashSet},
    rc::Rc,
};

use kfc::guid::{ContentHash, ResourceId};

use super::resource_values::WriteOwner;

/// The most entries one package (or the unattributed bucket) keeps: changed, created and contents together.
pub const PACKAGE_ENTRIES: usize = 4096;
/// The most entries a journal keeps in all.
pub const TOTAL_ENTRIES: usize = 32768;

/// A resource a package created, the `ordinal`-th (1-based) of its creations.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Created {
    pub ordinal: u32,
    pub id: ResourceId,
    /// The package gave the GUID (and part) itself; else it was drawn at random.
    pub explicit: bool,
}

/// A resource a package wrote; `created_by`: the package (id, ordinal) that created it in this pass, if one did.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Changed {
    pub id: ResourceId,
    pub created_by: Option<(String, u32)>,
}

/// What one package's entrypoint (or, without an id, everything outside an entrypoint) changed.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Changes {
    /// None: the unattributed bucket.
    pub id: Option<String>,
    /// In resource-key order.
    pub changed: Vec<Changed>,
    /// In creation order.
    pub created: Vec<Created>,
    /// In first-creation order.
    pub contents: Vec<ContentHash>,
    pub truncated: bool,
}

/// The journal of one attempt: its packages in the order their entrypoints ran.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Journal {
    pub packages: Vec<Changes>,
    pub unattributed: Changes,
    pub truncated: bool,
}

#[derive(Default)]
struct Bucket {
    id: Option<String>,
    written: BTreeSet<ResourceId>,
    seen: HashSet<ContentHash>,
    creations: u32,
    created: Vec<Created>,
    contents: Vec<ContentHash>,
    truncated: bool,
}
impl Bucket {
    fn entries(&self) -> usize {
        self.written.len() + self.created.len() + self.contents.len()
    }
}

struct State {
    /// Bucket 0 is the unattributed one; each entrypoint gets the next.
    buckets: Vec<Bucket>,
    current: usize,
    /// Every resource created in this pass: its creator's bucket and ordinal.
    creators: HashMap<ResourceId, (usize, u32)>,
    entries: usize,
    truncated: bool,
    closed: bool,
}

/// The pass's recorder: one per asset session (so one per attempt), shared by every resource owner it hands out.
/// Worker-local (`Rc`, `Cell`): the VM's thread only. It holds keys and ids, never a Lua value.
pub struct Recorder {
    state: RefCell<State>,
    /// Moves at every `begin`/`end`: an owner that already noted itself in this epoch skips the set lookup.
    epoch: Cell<u64>,
}

impl Default for Recorder {
    fn default() -> Self {
        Self {
            state: RefCell::new(State {
                buckets: vec![Bucket::default()],
                current: 0,
                creators: HashMap::new(),
                entries: 0,
                truncated: false,
                closed: false,
            }),
            epoch: Cell::new(0),
        }
    }
}

impl State {
    /// Room for one more entry in the current bucket; else it and the journal are marked truncated.
    fn room(&mut self) -> bool {
        let bucket = &mut self.buckets[self.current];
        if bucket.entries() < PACKAGE_ENTRIES && self.entries < TOTAL_ENTRIES {
            self.entries += 1;
            true
        } else {
            bucket.truncated = true;
            self.truncated = true;
            false
        }
    }
}

impl Recorder {
    /// `package`'s entrypoint begins: what is noted from now on is its own.
    pub fn begin(&self, package: &str) {
        let mut state = self.state.borrow_mut();
        if state.closed {
            return;
        }
        state.buckets.push(Bucket {
            id: Some(package.to_owned()),
            ..Bucket::default()
        });
        state.current = state.buckets.len() - 1;
        self.epoch.set(self.epoch.get() + 1);
    }

    /// The running entrypoint returned (or failed): what is noted from now on is unattributed.
    pub fn end(&self) {
        self.state.borrow_mut().current = 0;
        self.epoch.set(self.epoch.get() + 1);
    }

    /// The running entrypoint (or nobody) wrote into `id`'s value or replaced it.
    pub fn wrote(&self, id: ResourceId) {
        let mut state = self.state.borrow_mut();
        let current = state.current;
        if state.closed
            || state.creators.get(&id).is_some_and(|(creator, _)| *creator == current)
            || state.buckets[current].written.contains(&id)
        {
            return;
        }
        if state.room() {
            state.buckets[current].written.insert(id);
        }
    }

    /// The running entrypoint (or nobody) created `id`; `explicit`: with its own GUID.
    pub fn created(&self, id: ResourceId, explicit: bool) {
        let mut state = self.state.borrow_mut();
        if state.closed {
            return;
        }
        let current = state.current;
        let bucket = &mut state.buckets[current];
        bucket.creations = bucket.creations.saturating_add(1);
        let ordinal = bucket.creations;
        state.creators.insert(id, (current, ordinal));
        if state.room() {
            state.buckets[current].created.push(Created { ordinal, id, explicit });
        }
    }

    /// The running entrypoint (or nobody) created the content `hash` (or created it again).
    pub fn content(&self, hash: ContentHash) {
        let mut state = self.state.borrow_mut();
        let current = state.current;
        if state.closed || state.buckets[current].seen.contains(&hash) {
            return;
        }
        if state.room() {
            let bucket = &mut state.buckets[current];
            bucket.seen.insert(hash);
            bucket.contents.push(hash);
        }
    }

    /// The journal so far; the recorder notes nothing more (writes after the pass, from a finalizer, are not the
    /// generation's: the stage was taken back before its resources were serialized).
    pub fn take(&self) -> Journal {
        let mut state = self.state.borrow_mut();
        state.closed = true;
        let names: Vec<Option<String>> = state.buckets.iter().map(|b| b.id.clone()).collect();
        let creators = std::mem::take(&mut state.creators);
        let mut buckets = std::mem::take(&mut state.buckets).into_iter().map(|b| Changes {
            changed: b
                .written
                .iter()
                .map(|id| Changed {
                    id: *id,
                    created_by: creators
                        .get(id)
                        .and_then(|(creator, ordinal)| Some((names[*creator].clone()?, *ordinal))),
                })
                .collect(),
            id: b.id,
            created: b.created,
            contents: b.contents,
            truncated: b.truncated,
        });
        let unattributed = buckets.next().unwrap_or_default();
        Journal {
            packages: buckets.collect(),
            unattributed,
            truncated: state.truncated,
        }
    }
}

/// One resource's owner for its typed values (`ValueObjects::owned_by`).
pub(crate) struct ResourceOwner {
    id: ResourceId,
    recorder: Rc<Recorder>,
    noted: Cell<u64>,
}

impl ResourceOwner {
    pub(crate) fn new(id: ResourceId, recorder: Rc<Recorder>) -> Self {
        Self {
            id,
            recorder,
            noted: Cell::new(u64::MAX),
        }
    }
}

impl WriteOwner for ResourceOwner {
    fn wrote(&self) {
        let epoch = self.recorder.epoch.get();
        if self.noted.get() != epoch {
            self.noted.set(epoch);
            self.recorder.wrote(self.id);
        }
    }
}

/// What the record says of a package besides its changes: its plan identity.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Identity {
    pub version: Option<String>,
    /// `shroudforge` or `eml` (the plan's kind).
    pub kind: &'static str,
    pub content_sha256: [u8; 32],
    pub settings_sha256: [u8; 32],
}

/// The record's header: the generation it belongs to.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Header<'a> {
    pub stem: &'a str,
    /// kiln-eml's generation digest (`startup::generation_digest`): the outcome's `archiveSha256`.
    pub generation: [u8; 32],
    pub receipt_sha256: [u8; 32],
    pub dialect: &'static str,
    pub attempt: &'static str,
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
fn text(s: &str) -> String {
    serde_json::to_string(s).unwrap_or_else(|_| "\"\"".into())
}
fn key(id: &ResourceId) -> String {
    format!(
        "\"guid\":\"{}\",\"typeHash\":\"{:08x}\",\"part\":{}",
        id.guid(),
        id.type_hash(),
        id.part_index()
    )
}
fn changes(c: &Changes) -> String {
    let changed: Vec<String> = c
        .changed
        .iter()
        .map(|e| {
            let by = match &e.created_by {
                Some((package, ordinal)) => format!("{{\"package\":{},\"ordinal\":{ordinal}}}", text(package)),
                None => "null".into(),
            };
            format!("{{{},\"createdBy\":{by}}}", key(&e.id))
        })
        .collect();
    let created: Vec<String> = c
        .created
        .iter()
        .map(|e| {
            format!(
                "{{\"ordinal\":{},{},\"explicit\":{}}}",
                e.ordinal,
                key(&e.id),
                e.explicit
            )
        })
        .collect();
    let contents: Vec<String> = c
        .contents
        .iter()
        .map(|h| format!("{{\"guid\":\"{h}\",\"size\":{}}}", h.size()))
        .collect();
    format!(
        "\"truncated\":{},\"changed\":[{}],\"created\":[{}],\"contents\":[{}]",
        c.truncated,
        changed.join(","),
        created.join(","),
        contents.join(",")
    )
}

impl Journal {
    /// The record (startup-journal.schema.json v1), compact, every field in the schema's order (a typed serializer).
    /// `identity`: each package's plan identity (a package the plan does not name has none, which the startup controller never
    /// passes: every package of the pass is a plan member).
    pub fn json(&self, header: &Header<'_>, identity: &dyn Fn(&str) -> Option<Identity>) -> String {
        let packages: Vec<String> = self
            .packages
            .iter()
            .map(|c| {
                let id = c.id.as_deref().unwrap_or_default();
                let identity = identity(id).unwrap_or(Identity {
                    version: None,
                    kind: "shroudforge",
                    content_sha256: [0; 32],
                    settings_sha256: [0; 32],
                });
                format!(
                    "{{\"id\":{},\"version\":{},\"kind\":\"{}\",\"contentSha256\":\"{}\",\"settingsSha256\":\"{}\",{}}}",
                    text(id),
                    identity.version.as_deref().map_or_else(|| "null".into(), text),
                    identity.kind,
                    hex(&identity.content_sha256),
                    hex(&identity.settings_sha256),
                    changes(c)
                )
            })
            .collect();
        format!(
            "{{\"format\":1,\"stem\":{},\"generation\":\"{}\",\"receiptSha256\":\"{}\",\"dialect\":\"{}\",\
             \"attempt\":\"{}\",\"truncated\":{},\"packages\":[{}],\"unattributed\":{{{}}}}}",
            text(header.stem),
            hex(&header.generation),
            hex(&header.receipt_sha256),
            header.dialect,
            header.attempt,
            self.truncated,
            packages.join(","),
            changes(&self.unattributed)
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use kfc::guid::Guid;

    fn id(n: u32) -> ResourceId {
        let mut bytes = [0u8; 16];
        bytes[..4].copy_from_slice(&n.to_le_bytes());
        ResourceId::new(Guid::from(bytes), 0x1234_5678, n % 3)
    }

    // Attribution by the running entrypoint: a write before any entrypoint is unattributed; two packages writing one
    // resource are both named; a package's writes into its own creation are not listed, another's name its creator
    // and ordinal; repeated writes and contents count once; ordinals count creations, explicit ones too; nothing is
    // noted after take.
    #[test]
    fn writes_are_named_for_the_running_entrypoint() {
        let r = Rc::new(Recorder::default());
        let owner = |n| ResourceOwner::new(id(n), r.clone());
        let early = owner(9);
        early.wrote();
        r.begin("a");
        let zero = owner(0);
        zero.wrote();
        zero.wrote();
        r.created(id(100), false);
        r.created(id(101), true);
        ResourceOwner::new(id(100), r.clone()).wrote();
        r.content(ContentHash::from_data(b"one"));
        r.content(ContentHash::from_data(b"one"));
        r.end();
        r.begin("b");
        zero.wrote();
        ResourceOwner::new(id(101), r.clone()).wrote();
        r.end();
        r.begin("c");
        r.end();
        let journal = r.take();
        zero.wrote();
        r.created(id(7), false);
        let names: Vec<_> = journal.packages.iter().map(|p| p.id.clone().unwrap()).collect();
        assert_eq!(names, ["a", "b", "c"]);
        let a = &journal.packages[0];
        assert_eq!(
            a.changed,
            [Changed {
                id: id(0),
                created_by: None
            }]
        );
        assert_eq!(
            a.created,
            [
                Created {
                    ordinal: 1,
                    id: id(100),
                    explicit: false
                },
                Created {
                    ordinal: 2,
                    id: id(101),
                    explicit: true
                }
            ]
        );
        assert_eq!(a.contents, [ContentHash::from_data(b"one")]);
        let b = &journal.packages[1];
        assert_eq!(
            b.changed,
            [
                Changed {
                    id: id(0),
                    created_by: None
                },
                Changed {
                    id: id(101),
                    created_by: Some(("a".into(), 2))
                }
            ]
        );
        assert_eq!(
            journal.packages[2],
            Changes {
                id: Some("c".into()),
                ..Changes::default()
            }
        );
        assert_eq!(
            journal.unattributed.changed,
            [Changed {
                id: id(9),
                created_by: None
            }]
        );
        assert!(!journal.truncated);
        assert_eq!(r.take().packages.len(), 0, "taken once");
    }

    // Over the bounds: a package keeps its first PACKAGE_ENTRIES entries and is truncated, ordinals keep counting, the
    // journal stops at TOTAL_ENTRIES; nothing fails.
    #[test]
    fn over_the_bounds_the_journal_is_truncated_never_failed() {
        let r = Recorder::default();
        r.begin("big");
        for n in 0..(PACKAGE_ENTRIES as u32 + 10) {
            r.created(id(n), false);
        }
        r.wrote(id(1_000_000));
        r.end();
        let mut n = 2_000_000u32;
        for p in 0..(TOTAL_ENTRIES / PACKAGE_ENTRIES + 1) {
            r.begin(&format!("p{p}"));
            for _ in 0..PACKAGE_ENTRIES {
                r.wrote(id(n));
                n += 1;
            }
            r.end();
        }
        let journal = r.take();
        let big = &journal.packages[0];
        assert!(big.truncated && journal.truncated);
        assert_eq!(big.created.len(), PACKAGE_ENTRIES);
        assert_eq!(big.created.last().unwrap().ordinal, PACKAGE_ENTRIES as u32);
        assert!(big.changed.is_empty());
        let total: usize = journal
            .packages
            .iter()
            .map(|p| p.changed.len() + p.created.len() + p.contents.len())
            .sum();
        assert_eq!(total, TOTAL_ENTRIES);
        assert!(journal.packages.last().unwrap().truncated);
        assert!(journal.packages.last().unwrap().changed.is_empty());
    }
}
