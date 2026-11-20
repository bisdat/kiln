//! Index of the custom names players give objects (chest "Rename" button).
//!
//! The server keeps every custom name in records of the form
//! `{ u64 key; char* text = &key + 0x28; u32 len; u32 pad; u64 capacity = 0x100; ...; char text[256] }`
//! (save-layer "CustomStrings"). We have no stable path to the owning
//! table, so a background thread scans the heap for that self-describing layout.
//! The same key can exist in several copies (live table and save snapshot);
//! the copy whose text changed most recently wins.
//!
//! kiln: (0.2.0-dev.29.19; a player's busy area stuttered every scan with the assistants mod on, on a PC with its page
//! file on a hard drive) a scan reads only the pages in the game's working set: a page that is not was not written since
//! it was paged out, so a record on it stays as the scan before saw it, and the scan never pages the game's memory back
//! in. The background thread runs in Windows' background mode (lower CPU, I/O and memory priority) and rests between
//! short stretches of work, so it never takes a whole core from the game (the player's CPU has four threads).

//! kiln: (0.2.0-dev.29.22; the same player on 29.21: with almost all of the game's 5.9 GB of private memory resident, each
//! background scan read all of it and took 90-155 s with its rests, and the next began 15 s after it ended, so the scan
//! never stopped; a lookup of an unknown key ran the whole sweep on the game thread, at most every 3 s) no sweep runs
//! on a timer any more. A name key is the FNV-1a 64 of its text (docs/re/items.md section 14), so a record whose text
//! hashes to its key never goes stale: the index keeps it (`known`) and a renamed object carries a new key. A sweep
//! runs only when a key the index does not know is asked for (`want`), and then narrowly: first the 4 MiB windows
//! around the records found before (a "warm" sweep), then the whole memory with the allocations that held records
//! first, stopping as soon as the asked keys are found. Asked sweeps never run back to back (`ask_gap`), a key a
//! whole sweep did not find is not asked for again for minutes (`miss_retry`), and the game thread only ever runs
//! a warm sweep of at most 32 MiB. Periodic sweeps remain only while the name component is not known by its type hash
//! (the component index is then learned from the index, as upstream does), backing off. Records whose text does not
//! hash to their key (another game build) keep upstream's rules, and the records the index holds are re-read every 15 s
//! (`verify`). The Kiln build asks kiln.items' string table first (game_api.rs), so it sweeps only when that cannot
//! answer.

use std::collections::{HashMap, HashSet};
use std::sync::{Condvar, Mutex};
use std::time::{Duration, Instant};

use chestsort_common::log;
use windows_sys::Win32::System::Diagnostics::Debug::ReadProcessMemory;
use windows_sys::Win32::System::Memory::{VirtualQuery, MEMORY_BASIC_INFORMATION, MEM_COMMIT, MEM_PRIVATE, PAGE_READWRITE};
use windows_sys::Win32::System::ProcessStatus::{K32QueryWorkingSetEx, PSAPI_WORKING_SET_EX_INFORMATION}; // kiln
use windows_sys::Win32::System::Threading::{GetCurrentProcess, GetCurrentThread, SetThreadPriority, THREAD_MODE_BACKGROUND_BEGIN}; // kiln

const TEXT_OFFSET: usize = 0x28;
const CAPACITY: u64 = 0x100;
const MIN_RESCAN_GAP: Duration = Duration::from_secs(3); // kiln: between two warm sweeps on a caller's thread
const CHUNK: usize = 1 << 20;
const PAGE: usize = 0x1000; // kiln
const RECORD: usize = TEXT_OFFSET + 0x100; // kiln: a record's bytes, header and text
// kiln: a background sweep works at most WORK at a time, then rests: REST while a key is asked for (half a core at most),
// REST_IDLE in a periodic sweep (a quarter); a sweep on a caller's thread never rests but is small (FOREGROUND_BUDGET)
const WORK: Duration = Duration::from_millis(4);
const REST: Duration = Duration::from_millis(4);
const REST_IDLE: Duration = Duration::from_millis(12);
// kiln: scheduling (see next_step)
const TICK: Duration = Duration::from_secs(15); // the thread looks at least this often (verify, asked keys)
const ASK_GAP_MIN: Duration = Duration::from_secs(30); // from the end of a whole sweep to the next asked one, at least
const LEARN_GAP_MIN: Duration = Duration::from_secs(30);
const LEARN_GAP_MAX: Duration = Duration::from_secs(30 * 60);
const LEARN_FACTOR: u32 = 4; // a periodic sweep waits 4x the last one's duration, doubled per quiet sweep
const QUIET_MAX: u32 = 5;
const MISS_RETRY_MIN: Duration = Duration::from_secs(5 * 60);
const MISS_RETRY_MAX: Duration = Duration::from_secs(30 * 60);
const HOME_WINDOW: usize = 4 << 20; // a warm sweep reads these windows around the records found before
const FOREGROUND_BUDGET: usize = 32 << 20;
const MAX_KEYS: usize = 16384; // known, wanted and missed keys, each at most

struct Record {
    key: u64,
    text: String,
    /// scan number in which this copy got its current text
    changed: u64,
}

/// kiln: a record whose text hashes to its key: that text is the key's for good.
struct Known {
    text: String,
    addr: usize,
}

#[derive(Default)]
struct Index {
    by_addr: HashMap<usize, Record>,
    known: HashMap<u64, Known>, // kiln
    allocs: HashSet<usize>,     // kiln: allocation bases that held records
    scans: u64,
    last_scan: Option<Instant>,
    last_verify: Option<Instant>, // kiln
    had: usize, // kiln: the record count of the scan before (for the log)
    sched: Sched, // kiln
}

/// kiln: what the background thread does next.
#[derive(Default)]
struct Sched {
    /// the name component's index is not known by its type hash: periodic sweeps let game_api learn it
    learning: bool,
    last_full_end: Option<Instant>,
    /// how long the last whole sweep took (wall clock, rests included)
    last_full_took: Duration,
    /// periodic sweeps in a row that found no new name
    quiet: u32,
    /// keys asked for and not in the index: whether a warm sweep looked for them already
    wanted: HashMap<u64, bool>,
    /// keys a whole sweep did not find, and when
    missed: HashMap<u64, Instant>,
}

#[derive(Debug, PartialEq, Clone, Copy)]
enum Step {
    Warm,
    Full { asked: bool },
    Wait(Duration),
}

static INDEX: Mutex<Option<Index>> = Mutex::new(None);
static WAKE: Condvar = Condvar::new(); // kiln

/// kiln: the next background step; `homes`: the index holds records (a warm sweep has somewhere to look).
fn next_step(s: &Sched, homes: bool, now: Instant) -> Step {
    let since = s.last_full_end.map(|t| now.saturating_duration_since(t));
    let due = |gap: Duration| match since {
        Some(d) if d < gap => Step::Wait((gap - d).min(TICK)),
        _ => Step::Full { asked: false },
    };
    if !s.wanted.is_empty() {
        if homes && s.wanted.values().any(|tried| !tried) {
            return Step::Warm;
        }
        return match due(ask_gap(s)) {
            Step::Full { .. } => Step::Full { asked: true },
            wait => wait,
        };
    }
    if s.learning {
        return due(learn_gap(s));
    }
    Step::Wait(TICK)
}

/// kiln: an asked sweep starts at least ASK_GAP_MIN, and at least as long as the last whole sweep took, after it ended.
fn ask_gap(s: &Sched) -> Duration {
    ASK_GAP_MIN.max(s.last_full_took)
}

/// kiln: the wait before a periodic sweep (only while learning): 4x the last sweep's duration, doubled per quiet sweep.
fn learn_gap(s: &Sched) -> Duration {
    (s.last_full_took * LEARN_FACTOR * (1u32 << s.quiet.min(QUIET_MAX))).clamp(LEARN_GAP_MIN, LEARN_GAP_MAX)
}

/// kiln: how long a key a whole sweep did not find stays unasked.
fn miss_retry(s: &Sched) -> Duration {
    (s.last_full_took * 8).clamp(MISS_RETRY_MIN, MISS_RETRY_MAX)
}

/// kiln: FNV-1a 64, the game's name key of a text.
pub(crate) fn fnv1a64(text: &[u8]) -> u64 {
    text.iter().fold(0xcbf2_9ce4_8422_2325, |h, &b| (h ^ b as u64).wrapping_mul(0x100_0000_01b3))
}

pub fn start() {
    std::thread::spawn(|| {
        // kiln: the game's threads come first
        unsafe { SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) };
        loop {
            background_step();
        }
    });
}

/// kiln: one turn of the background thread: re-read the records the index holds (when due), then a sweep or a wait.
fn background_step() {
    let verify_due = {
        let guard = INDEX.lock().unwrap();
        guard.as_ref().is_some_and(|i| !i.by_addr.is_empty() && i.last_verify.is_none_or(|t| t.elapsed() >= TICK))
    };
    if verify_due {
        verify();
    }
    let step = {
        let guard = INDEX.lock().unwrap();
        match guard.as_ref() {
            Some(i) => next_step(&i.sched, !i.known.is_empty() || !i.by_addr.is_empty(), Instant::now()),
            None => Step::Wait(TICK),
        }
    };
    match step {
        Step::Wait(d) => {
            let guard = INDEX.lock().unwrap();
            let _ = WAKE.wait_timeout(guard, d.max(Duration::from_millis(50))).unwrap();
        }
        Step::Warm => run_sweep(Kind::Warm, Some(REST), None),
        Step::Full { asked } => run_sweep(Kind::Full { asked }, Some(if asked { REST } else { REST_IDLE }), None),
    }
}

/// Current name for `key`, if any object carries it.
pub fn lookup(key: u64) -> Option<String> {
    let guard = INDEX.lock().unwrap();
    let index = guard.as_ref()?;
    if let Some(k) = index.known.get(&key) {
        return Some(k.text.clone()); // kiln
    }
    index
        .by_addr
        .values()
        .filter(|r| r.key == key)
        .max_by_key(|r| r.changed)
        .map(|r| r.text.clone())
}

/// kiln: like [`lookup`]; an unknown key is handed to the background thread (`want`), which finds it soon.
pub fn lookup_known(key: u64) -> Option<String> {
    lookup(key).or_else(|| {
        want(key);
        None
    })
}

/// Like [`lookup`], but rescans immediately (rate limited) when the key is unknown,
/// e.g. right after a chest was renamed.
/// kiln: the rescan on the caller's thread is a warm sweep of at most FOREGROUND_BUDGET (before: all of the game's
/// memory); when it does not find the key, the background thread looks further and a later call gets the name.
pub fn lookup_fresh(key: u64) -> Option<String> {
    if let Some(name) = lookup(key) {
        return Some(name);
    }
    let recent = INDEX.lock().unwrap().as_ref().and_then(|i| i.last_scan).is_some_and(|t| t.elapsed() < MIN_RESCAN_GAP);
    if !recent {
        run_sweep(Kind::Warm, None, Some(FOREGROUND_BUDGET));
        if let Some(name) = lookup(key) {
            return Some(name);
        }
    }
    want(key);
    None
}

/// kiln: the name component is not known by its type hash: periodic sweeps fill the index game_api learns it from.
pub fn learning() {
    let mut guard = INDEX.lock().unwrap();
    let s = &mut guard.get_or_insert_with(Index::default).sched;
    if !s.learning {
        s.learning = true;
        WAKE.notify_all();
    }
}

/// kiln: asks the background thread for `key` (not again for a while after a whole sweep missed it).
fn want(key: u64) {
    let mut guard = INDEX.lock().unwrap();
    let index = guard.get_or_insert_with(Index::default);
    if index.known.contains_key(&key) || index.sched.wanted.contains_key(&key) || index.sched.wanted.len() >= MAX_KEYS {
        return;
    }
    let retry = miss_retry(&index.sched);
    if index.sched.missed.get(&key).is_some_and(|t| t.elapsed() < retry) {
        return;
    }
    index.sched.missed.remove(&key);
    index.sched.wanted.insert(key, false);
    WAKE.notify_all();
}

#[derive(Debug, PartialEq, Clone, Copy)]
enum Kind {
    /// the windows around the records found before
    Warm,
    /// all of the game's private read/write memory (`asked`: for wanted keys; stops once they are found)
    Full { asked: bool },
}

/// kiln: a sweep, then its results into the index and the schedule. `rest`: rest between stretches of work;
/// `budget`: stop after reading that much.
fn run_sweep(kind: Kind, rest: Option<Duration>, budget: Option<usize>) {
    let (homes, stop, wanted_before) = {
        let mut guard = INDEX.lock().unwrap();
        let index = guard.get_or_insert_with(Index::default);
        let wanted: HashSet<u64> = index.sched.wanted.keys().copied().collect();
        let stop = if kind == (Kind::Full { asked: true }) { wanted.clone() } else { HashSet::new() };
        (homes_of(index), stop, wanted)
    };
    let out = unsafe { sweep(kind, &homes, rest, budget, &stop) };
    let mut guard = INDEX.lock().unwrap();
    let index = guard.get_or_insert_with(Index::default);
    let merged = merge(index, &out);
    let now = Instant::now();
    index.last_scan = Some(now);
    let s = &mut index.sched;
    let known = |k: &u64| index.known.contains_key(k) || index.by_addr.values().any(|r| r.key == *k);
    s.wanted.retain(|k, _| !known(k));
    match kind {
        Kind::Warm if rest.is_some() => {
            for k in &wanted_before {
                if let Some(tried) = s.wanted.get_mut(k) {
                    *tried = true;
                }
            }
        }
        Kind::Warm => {}
        Kind::Full { asked } => {
            s.last_full_end = Some(now);
            if out.complete {
                s.last_full_took = out.took;
                for k in &wanted_before {
                    if s.wanted.remove(k).is_some() && s.missed.len() < MAX_KEYS {
                        s.missed.insert(*k, now);
                    }
                }
            }
            if !asked {
                s.quiet = if merged.new_keys > 0 { 0 } else { s.quiet.saturating_add(1) };
            }
        }
    }
    s.missed.retain(|k, _| !known(k));
    let count = index.known.len() + index.by_addr.len();
    let had = index.had;
    index.had = count;
    let first = index.scans == 1;
    if kind != Kind::Warm || merged.new_keys > 0 || count != had || first {
        log!(
            "name index: {} sweep{}: {count} name record(s) ({} new), took {} ms (read {} MiB of {} MiB in {} area(s); {} MiB not in memory skipped, {} record(s) kept from before){}",
            match kind { Kind::Warm => "warm", Kind::Full { asked: true } => "asked", Kind::Full { asked: false } => "periodic" },
            if rest.is_none() { " on the caller's thread" } else { "" },
            merged.new_keys,
            out.took.as_millis(),
            out.bytes >> 20,
            out.area_bytes >> 20,
            out.areas,
            out.skipped.iter().map(|(a, b)| b - a).sum::<usize>() >> 20,
            merged.kept,
            if out.complete { "" } else { ", stopped early" }
        );
    }
}

/// kiln: where a warm sweep looks and which allocations a whole sweep reads first.
#[derive(Default, Debug)]
struct Homes {
    /// HOME_WINDOW-aligned windows holding records, the fullest first
    windows: Vec<usize>,
    allocs: HashSet<usize>,
}

fn homes_of(index: &Index) -> Homes {
    let mut count: HashMap<usize, usize> = HashMap::new();
    for addr in index.known.values().map(|k| k.addr).chain(index.by_addr.keys().copied()) {
        *count.entry(addr & !(HOME_WINDOW - 1)).or_default() += 1;
    }
    let mut windows: Vec<(usize, usize)> = count.into_iter().collect();
    windows.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    Homes { windows: windows.into_iter().map(|(w, _)| w).collect(), allocs: index.allocs.clone() }
}

/// kiln: the address ranges a sweep is the authority for: a record of the index in them that the sweep did not find
/// (and not on a page it skipped) is gone.
#[derive(Debug, PartialEq)]
enum Scope {
    All,
    Ranges(Vec<(usize, usize)>),
}

impl Scope {
    fn holds(&self, start: usize, end: usize) -> bool {
        match self {
            Scope::All => true,
            Scope::Ranges(r) => {
                let i = r.partition_point(|&(_, e)| e <= start);
                i < r.len() && r[i].0 <= start && end <= r[i].1
            }
        }
    }
}

/// kiln: what a sweep found and read.
struct Outcome {
    found: Vec<(usize, u64, String)>,
    allocs: HashSet<usize>,
    scope: Scope,
    /// pages not in the working set, sorted
    skipped: Vec<(usize, usize)>,
    bytes: usize,
    area_bytes: usize,
    areas: usize,
    /// every area of the plan was read (no budget or early stop)
    complete: bool,
    took: Duration,
}

/// kiln: a part of a committed private read/write region, and the base of its allocation.
#[derive(Debug, Clone, Copy)]
struct Area {
    base: usize,
    size: usize,
    alloc: usize,
}

/// kiln: the committed private read/write memory in [start, end) (images, mapped files, guard pages and write-combined
/// or uncached GPU memory are not: their type or protection differs).
unsafe fn areas(start: usize, end: usize) -> Vec<Area> {
    let mut out = Vec::new();
    let mut addr = start;
    let mut info: MEMORY_BASIC_INFORMATION = std::mem::zeroed();
    while addr < end && VirtualQuery(addr as *const _, &mut info, std::mem::size_of::<MEMORY_BASIC_INFORMATION>()) != 0 {
        let base = info.BaseAddress as usize;
        let size = info.RegionSize;
        if size == 0 {
            break;
        }
        let (a, b) = (base.max(start), base.saturating_add(size).min(end));
        if info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && info.Protect == PAGE_READWRITE && a < b {
            out.push(Area { base: a, size: b - a, alloc: info.AllocationBase as usize });
        }
        addr = base.saturating_add(size);
    }
    out
}

/// kiln: the plan of a sweep: (scope range, areas) units, in reading order.
unsafe fn plan(kind: Kind, homes: &Homes) -> Vec<((usize, usize), Vec<Area>)> {
    match kind {
        Kind::Warm => homes
            .windows
            .iter()
            .map(|&w| {
                let end = w.saturating_add(HOME_WINDOW + RECORD);
                ((w, end), areas(w, end))
            })
            .collect(),
        Kind::Full { .. } => {
            let mut all = areas(0, usize::MAX);
            // the allocations that held records first (stable: address order otherwise)
            all.sort_by_key(|a| !homes.allocs.contains(&a.alloc));
            all.into_iter().map(|a| ((a.base, a.base + a.size), vec![a])).collect()
        }
    }
}

/// kiln: the sweep itself (no lock held). Stops early when every key of `stop` is found (non-empty) or `budget`
/// bytes were read.
unsafe fn sweep(kind: Kind, homes: &Homes, rest: Option<Duration>, budget: Option<usize>, stop: &HashSet<u64>) -> Outcome {
    let started = Instant::now();
    let units = plan(kind, homes);
    let mut pass = Pass::new(rest);
    let mut scanned = Vec::new();
    let mut complete = true;
    let (mut areas_n, mut area_bytes) = (0, 0);
    for (range, list) in &units {
        if budget.is_some_and(|b| pass.bytes >= b) || (!stop.is_empty() && stop.iter().all(|k| pass.keys.contains(k))) {
            complete = false;
            break;
        }
        for area in list {
            pass.area(area);
            areas_n += 1;
            area_bytes += area.size;
        }
        scanned.push(*range);
    }
    let scope = if complete && matches!(kind, Kind::Full { .. }) { Scope::All } else { Scope::Ranges(merge_ranges(scanned)) };
    Outcome {
        found: pass.found,
        allocs: pass.allocs,
        scope,
        skipped: merge_ranges(pass.skipped),
        bytes: pass.bytes,
        area_bytes,
        areas: areas_n,
        complete,
        took: started.elapsed(),
    }
}

/// kiln: sorts ranges and joins the ones that touch or overlap.
fn merge_ranges(mut r: Vec<(usize, usize)>) -> Vec<(usize, usize)> {
    r.sort_unstable();
    let mut out: Vec<(usize, usize)> = Vec::with_capacity(r.len());
    for (a, b) in r {
        match out.last_mut() {
            Some(last) if a <= last.1 => last.1 = last.1.max(b),
            _ => out.push((a, b)),
        }
    }
    out
}

/// kiln: one sweep's reading state.
struct Pass {
    buf: Vec<u8>,
    ws: Vec<PSAPI_WORKING_SET_EX_INFORMATION>,
    slice: Instant,
    rest: Option<Duration>,
    bytes: usize,
    skipped: Vec<(usize, usize)>,
    found: Vec<(usize, u64, String)>,
    keys: HashSet<u64>,
    allocs: HashSet<usize>,
}

impl Pass {
    fn new(rest: Option<Duration>) -> Self {
        Pass {
            buf: vec![0u8; CHUNK + 0x200],
            ws: Vec::new(),
            slice: Instant::now(),
            rest,
            bytes: 0,
            skipped: Vec::new(),
            found: Vec::new(),
            keys: HashSet::new(),
            allocs: HashSet::new(),
        }
    }

    /// The resident pages of `area`, in 1 MiB chunks (29.19's reading).
    unsafe fn area(&mut self, area: &Area) {
        let process = GetCurrentProcess();
        let mut offset = 0;
        while offset < area.size {
            // overlap chunks so records crossing a boundary are seen whole
            let len = (area.size - offset).min(CHUNK + 0x200);
            // only the resident pages; the chunk's own part (without the overlap) counts as skipped where it is not
            let start = area.base + offset;
            let valid = resident(start, len.div_ceil(PAGE), &mut self.ws);
            add_skipped(&mut self.skipped, &valid, start, (area.size - offset).min(CHUNK));
            for (run, run_len) in resident_runs(&valid, start, len) {
                let mut read = 0usize;
                let ok = ReadProcessMemory(process, run as *const _, self.buf.as_mut_ptr() as *mut _, run_len, &mut read);
                if ok != 0 {
                    self.bytes += read;
                    let before = self.found.len();
                    scan_chunk(&self.buf[..read], run, &mut self.found);
                    for (_, key, _) in &self.found[before..] {
                        self.keys.insert(*key);
                        self.allocs.insert(area.alloc);
                    }
                }
            }
            // rest after each stretch of work (background sweeps only)
            if let Some(rest) = self.rest {
                if self.slice.elapsed() >= WORK {
                    std::thread::sleep(rest);
                    self.slice = Instant::now();
                }
            }
            offset += CHUNK;
        }
    }
}

/// kiln: what a merge changed.
struct Merged {
    new_keys: usize,
    kept: usize,
}

/// kiln: a sweep's records into the index. A record whose text hashes to its key goes to `known`; the others are
/// upstream's address-keyed copies (the copy whose text changed most recently wins), and those of the scan before that
/// lie outside the sweep's scope or on a page it skipped stay as they were.
fn merge(index: &mut Index, out: &Outcome) -> Merged {
    index.scans += 1;
    let scan = index.scans;
    let prev = std::mem::take(&mut index.by_addr);
    let mut next = HashMap::with_capacity(prev.len());
    let mut new_keys = 0;
    for (addr, key, text) in &out.found {
        if fnv1a64(text.as_bytes()) == *key {
            if !index.known.contains_key(key) {
                if index.known.len() >= MAX_KEYS {
                    continue;
                }
                new_keys += 1;
            }
            index.known.insert(*key, Known { text: text.clone(), addr: *addr });
            continue;
        }
        let changed = match prev.get(addr) {
            Some(old) if old.key == *key && old.text == *text => old.changed,
            _ => scan,
        };
        if !prev.values().any(|r| r.key == *key) && !next.values().any(|r: &Record| r.key == *key) {
            new_keys += 1;
        }
        next.insert(*addr, Record { key: *key, text: text.clone(), changed });
    }
    let kept = keep_skipped(&mut next, prev, &out.scope, &out.skipped);
    index.by_addr = next;
    if out.scope == Scope::All {
        index.allocs = out.allocs.clone();
    } else if index.allocs.len() < 4096 {
        index.allocs.extend(out.allocs.iter().copied());
    }
    Merged { new_keys, kept }
}

/// kiln: the records of the scan before that the sweep is not the authority for (outside its scope, or even in part on
/// a page it skipped: a page out of the working set was not written since it was paged out) go into `next` as they
/// were. Returns how many.
fn keep_skipped(next: &mut HashMap<usize, Record>, prev: HashMap<usize, Record>, scope: &Scope, skipped: &[(usize, usize)]) -> usize {
    let mut kept = 0;
    for (addr, record) in prev {
        if next.contains_key(&addr) || (scope.holds(addr, addr + RECORD) && !overlaps(skipped, addr, addr + RECORD)) {
            continue;
        }
        next.insert(addr, record);
        kept += 1;
    }
    kept
}

/// kiln: re-reads the address-keyed records (those whose text does not hash to their key): a changed text is taken, a
/// record no longer there is dropped, one on a page out of the working set stays.
fn verify() {
    let addrs: Vec<usize> = {
        let guard = INDEX.lock().unwrap();
        guard.as_ref().map(|i| i.by_addr.keys().copied().collect()).unwrap_or_default()
    };
    let seen: Vec<(usize, Option<Option<(u64, String)>>)> = addrs.iter().map(|&a| (a, unsafe { read_record(a) })).collect();
    let mut guard = INDEX.lock().unwrap();
    let Some(index) = guard.as_mut() else { return };
    index.scans += 1;
    let scan = index.scans;
    for (addr, now) in seen {
        match now {
            None => {}
            Some(None) => {
                index.by_addr.remove(&addr);
            }
            Some(Some((key, text))) => {
                if let Some(r) = index.by_addr.get_mut(&addr) {
                    if r.key != key || r.text != text {
                        *r = Record { key, text, changed: scan };
                    }
                }
            }
        }
    }
    index.last_verify = Some(Instant::now());
}

/// kiln: the record at `addr`: None when its pages are not all resident (not read), Some(None) when no record is there.
unsafe fn read_record(addr: usize) -> Option<Option<(u64, String)>> {
    let first = addr & !(PAGE - 1);
    let pages = (addr + RECORD - first).div_ceil(PAGE);
    if !resident(first, pages, &mut Vec::new()).iter().all(|&v| v) {
        return None;
    }
    let mut buf = [0u8; RECORD];
    let mut read = 0usize;
    if ReadProcessMemory(GetCurrentProcess(), addr as *const _, buf.as_mut_ptr() as *mut _, RECORD, &mut read) == 0 || read != RECORD {
        return Some(None);
    }
    let mut found = Vec::new();
    scan_chunk(&buf, addr, &mut found);
    Some(found.into_iter().next().map(|(_, k, t)| (k, t)))
}

/// kiln: whether [start, end) meets one of `ranges` (sorted, not overlapping).
fn overlaps(ranges: &[(usize, usize)], start: usize, end: usize) -> bool {
    let i = ranges.partition_point(|&(_, e)| e <= start);
    i < ranges.len() && ranges[i].0 < end
}

/// kiln: the runs of pages of [start, start + len) that are in the working set, as (address, length) clipped to the
/// range; `valid[i]`: page i (from `start`, page aligned) is resident.
fn resident_runs(valid: &[bool], start: usize, len: usize) -> Vec<(usize, usize)> {
    let end = start + len;
    let mut runs = Vec::new();
    let mut i = 0;
    while i < valid.len() {
        if !valid[i] {
            i += 1;
            continue;
        }
        let first = i;
        while i < valid.len() && valid[i] {
            i += 1;
        }
        let a = start + first * PAGE;
        let b = (start + i * PAGE).min(end);
        if a < b {
            runs.push((a, b - a));
        }
    }
    runs
}

/// kiln: adds the pages of [start, start + len) that are not resident to `skipped` (merging with its last range).
fn add_skipped(skipped: &mut Vec<(usize, usize)>, valid: &[bool], start: usize, len: usize) {
    let end = start + len;
    for (i, &v) in valid.iter().enumerate() {
        let a = start + i * PAGE;
        if a >= end {
            break;
        }
        if v {
            continue;
        }
        let b = (a + PAGE).min(end);
        match skipped.last_mut() {
            Some(last) if last.1 == a => last.1 = b,
            _ => skipped.push((a, b)),
        }
    }
}

/// kiln: which pages of [start, start + pages * PAGE) are in this process's working set; all of them when Windows
/// cannot say (the scan then reads everything, as upstream's does).
unsafe fn resident(start: usize, pages: usize, ws: &mut Vec<PSAPI_WORKING_SET_EX_INFORMATION>) -> Vec<bool> {
    ws.clear();
    ws.resize_with(pages, || std::mem::zeroed());
    for (i, e) in ws.iter_mut().enumerate() {
        e.VirtualAddress = (start + i * PAGE) as *mut _;
    }
    let bytes = (pages * std::mem::size_of::<PSAPI_WORKING_SET_EX_INFORMATION>()) as u32;
    if K32QueryWorkingSetEx(GetCurrentProcess(), ws.as_mut_ptr() as *mut _, bytes) == 0 {
        return vec![true; pages];
    }
    ws.iter().map(|e| e.VirtualAttributes.Flags & 1 != 0).collect()
}

fn scan_chunk(data: &[u8], base: usize, found: &mut Vec<(usize, u64, String)>) {
    let mut i = 0;
    while i + TEXT_OFFSET + 0x100 <= data.len() {
        let addr = base + i;
        let ptr = u64::from_le_bytes(data[i + 8..i + 16].try_into().unwrap());
        if ptr == (addr + TEXT_OFFSET) as u64
            && u64::from_le_bytes(data[i + 0x18..i + 0x20].try_into().unwrap()) == CAPACITY
        {
            let key = u64::from_le_bytes(data[i..i + 8].try_into().unwrap());
            let len = u32::from_le_bytes(data[i + 0x10..i + 0x14].try_into().unwrap()) as usize;
            let text = &data[i + TEXT_OFFSET..i + TEXT_OFFSET + 0x100];
            if key != 0 && (1..0x100).contains(&len) && text[len] == 0 {
                if let Ok(s) = std::str::from_utf8(&text[..len]) {
                    found.push((addr, key, s.to_owned()));
                }
            }
        }
        i += 8;
    }
}

// kiln: host tests (cargo xwin test under Wine, ../../../build.sh --test)
#[cfg(test)]
mod tests {
    use super::*;
    use windows_sys::Win32::System::Memory::{VirtualAlloc, VirtualFree, MEM_RELEASE, MEM_RESERVE};

    fn record(key: u64, text: &str) -> Record {
        Record { key, text: text.into(), changed: 1 }
    }

    /// a record as the game lays it out, at `at` (8-aligned, zeroed memory)
    unsafe fn put(at: *mut u8, key: u64, text: &str) {
        let a = at as usize;
        (at as *mut u64).write_unaligned(key);
        (at.add(8) as *mut u64).write_unaligned((a + TEXT_OFFSET) as u64);
        (at.add(0x10) as *mut u32).write_unaligned(text.len() as u32);
        (at.add(0x18) as *mut u64).write_unaligned(CAPACITY);
        std::ptr::copy_nonoverlapping(text.as_ptr(), at.add(TEXT_OFFSET), text.len());
        at.add(TEXT_OFFSET + text.len()).write(0);
    }

    unsafe fn alloc(size: usize) -> *mut u8 {
        let p = VirtualAlloc(std::ptr::null(), size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) as *mut u8;
        assert!(!p.is_null());
        p
    }

    fn named(text: &str) -> u64 {
        fnv1a64(text.as_bytes())
    }

    #[test]
    fn ranges_and_runs() {
        let r = [(0x1000, 0x3000), (0x8000, 0x9000)];
        assert!(overlaps(&r, 0x2f00, 0x3100) && overlaps(&r, 0x0, 0x1001) && overlaps(&r, 0x8fff, 0x9100));
        assert!(!overlaps(&r, 0x3000, 0x8000) && !overlaps(&r, 0x0, 0x1000) && !overlaps(&r, 0x9000, 0xa000) && !overlaps(&[], 0, 9));
        // pages 0,1 resident, 2 not, 3 resident; the range ends inside page 3
        let valid = [true, true, false, true];
        assert_eq!(resident_runs(&valid, 0x10000, 3 * PAGE + 0x200), vec![(0x10000, 2 * PAGE), (0x13000, 0x200)]);
        assert_eq!(resident_runs(&[false, false], 0x10000, 2 * PAGE), vec![]);
        let mut skipped = vec![(0xe000, 0x10000)];
        add_skipped(&mut skipped, &[false, true, false, false], 0x10000, 4 * PAGE);
        assert_eq!(skipped, vec![(0xe000, 0x11000), (0x12000, 0x14000)]);
        // the chunk's own part only: the overlap page is not counted
        let mut only = Vec::new();
        add_skipped(&mut only, &[true, false], 0x20000, PAGE);
        assert!(only.is_empty());
        assert_eq!(merge_ranges(vec![(9, 12), (0, 4), (4, 6), (10, 20)]), vec![(0, 6), (9, 20)]);
        let scope = Scope::Ranges(vec![(0x1000, 0x2000), (0x8000, 0x9000)]);
        assert!(scope.holds(0x1000, 0x2000) && scope.holds(0x8100, 0x8200) && Scope::All.holds(0, 9));
        assert!(!scope.holds(0x1f00, 0x2100) && !scope.holds(0x3000, 0x3100) && !scope.holds(0x0, 0x1001));
    }

    #[test]
    fn records_on_skipped_pages_or_outside_the_scope_are_kept() {
        let prev = || {
            let mut prev = HashMap::new();
            prev.insert(0x10100, record(7, "Ores"));    // on a skipped page: kept
            prev.insert(0x11f80, record(8, "Food"));    // starts on a read page, ends on a skipped one: kept
            prev.insert(0x30000, record(9, "Gone"));    // on a read page and not found again: dropped (in scope)
            prev.insert(0x40000, record(10, "Again"));  // found again: the scan's copy wins
            prev
        };
        let found = || {
            let mut next = HashMap::new();
            next.insert(0x40000, Record { key: 10, text: "Again".into(), changed: 5 });
            next
        };
        let skipped = [(0x10000, 0x11000), (0x12000, 0x13000)];
        let mut next = found();
        assert_eq!(keep_skipped(&mut next, prev(), &Scope::All, &skipped), 2);
        assert_eq!(next.len(), 3);
        assert_eq!(next[&0x10100].text, "Ores");
        assert_eq!(next[&0x11f80].text, "Food");
        assert_eq!(next[&0x40000].changed, 5);
        assert!(!next.contains_key(&0x30000));
        // a warm sweep that read only 0x40000-0x50000: everything else stays
        let mut next = found();
        assert_eq!(keep_skipped(&mut next, prev(), &Scope::Ranges(vec![(0x40000, 0x50000)]), &[]), 3);
        assert!(next.contains_key(&0x30000));
    }

    #[test]
    fn name_keys_are_fnv1a_64_of_the_text() {
        assert_eq!(fnv1a64(b""), 0xcbf2_9ce4_8422_2325);
        assert_eq!(fnv1a64(b"a"), 0xaf63_dc4c_8601_ec8c);
        assert_eq!(fnv1a64(b"foobar"), 0x8594_4171_f739_67e8);
    }

    #[test]
    fn merge_keeps_hashed_names_for_good_and_counts_new_ones() {
        let mut index = Index::default();
        let ores = named("Ores");
        let out = |found: Vec<(usize, u64, String)>, scope| Outcome {
            found,
            allocs: HashSet::from([0x100000]),
            scope,
            skipped: Vec::new(),
            bytes: 0,
            area_bytes: 0,
            areas: 0,
            complete: true,
            took: Duration::ZERO,
        };
        let m = merge(&mut index, &out(vec![(0x1000, ores, "Ores".into()), (0x2000, 77, "Odd".into())], Scope::All));
        assert_eq!((m.new_keys, index.known.len(), index.by_addr.len()), (2, 1, 1));
        assert_eq!(index.known[&ores].addr, 0x1000);
        // the record is gone from memory: a hashed name stays, an address-keyed one goes (upstream's rule)
        let m = merge(&mut index, &out(Vec::new(), Scope::All));
        assert_eq!((m.new_keys, index.known.len(), index.by_addr.len()), (0, 1, 0));
        let guard_free = |i: &Index, k: u64| i.known.get(&k).map(|n| n.text.clone());
        assert_eq!(guard_free(&index, ores).as_deref(), Some("Ores"));
        assert_eq!(index.allocs, HashSet::from([0x100000]));
        let homes = homes_of(&index);
        assert_eq!(homes.windows, vec![0]);
    }

    #[test]
    fn schedule_sweeps_only_when_asked_or_learning_and_never_back_to_back() {
        let now = Instant::now();
        let ago = |s: u64| Some(now - Duration::from_secs(s));
        let mut s = Sched::default();
        // nothing asked, the component known: no sweep at all
        assert_eq!(next_step(&s, true, now), Step::Wait(TICK));
        // a key asked for: the windows around known records first, a whole sweep after
        s.wanted.insert(1, false);
        assert_eq!(next_step(&s, true, now), Step::Warm);
        assert_eq!(next_step(&s, false, now), Step::Full { asked: true });
        s.wanted.insert(1, true);
        // the last whole sweep took 100 s and ended 10 s ago: the asked one waits until 100 s after its end
        s.last_full_took = Duration::from_secs(100);
        s.last_full_end = ago(10);
        assert_eq!(next_step(&s, true, now), Step::Wait(TICK));
        s.last_full_end = ago(95);
        assert_eq!(next_step(&s, true, now), Step::Wait(Duration::from_secs(5)));
        s.last_full_end = ago(100);
        assert_eq!(next_step(&s, true, now), Step::Full { asked: true });
        // a fast PC: still 30 s apart
        s.last_full_took = Duration::from_secs(2);
        s.last_full_end = ago(29);
        assert!(matches!(next_step(&s, true, now), Step::Wait(_)));
        assert_eq!(ask_gap(&s), ASK_GAP_MIN);
        // learning (no type hash): periodic sweeps, 4x the duration, doubled per quiet sweep, 30 s to 30 min
        s.wanted.clear();
        s.learning = true;
        assert_eq!(learn_gap(&s), Duration::from_secs(30));
        s.quiet = 3;
        assert_eq!(learn_gap(&s), Duration::from_secs(64));
        s.last_full_took = Duration::from_secs(120);
        s.quiet = 0;
        assert_eq!(learn_gap(&s), Duration::from_secs(480));
        s.quiet = 9;
        assert_eq!(learn_gap(&s), LEARN_GAP_MAX);
        s.last_full_end = ago(1799);
        assert!(matches!(next_step(&s, true, now), Step::Wait(_)));
        s.last_full_end = ago(1800);
        assert_eq!(next_step(&s, true, now), Step::Full { asked: false });
        assert_eq!(miss_retry(&s), Duration::from_secs(960));
        s.last_full_took = Duration::from_secs(1);
        assert_eq!(miss_retry(&s), MISS_RETRY_MIN);
    }

    #[test]
    fn unknown_keys_are_asked_for_once_and_a_missed_one_waits() {
        let key = named("kiln test: asked for");
        assert_eq!(lookup_known(key), None);
        assert_eq!(lookup_known(key), None);
        {
            let mut guard = INDEX.lock().unwrap();
            let s = &mut guard.as_mut().unwrap().sched;
            assert_eq!(s.wanted.get(&key), Some(&false));
            // a whole sweep did not find it: not asked again for a while
            s.wanted.remove(&key);
            s.missed.insert(key, Instant::now());
        }
        assert_eq!(lookup_known(key), None);
        let mut guard = INDEX.lock().unwrap();
        let s = &mut guard.as_mut().unwrap().sched;
        assert!(!s.wanted.contains_key(&key));
        s.missed.remove(&key);
    }

    #[test]
    fn touched_pages_are_read_and_untouched_ones_skipped() {
        const SIZE: usize = 16 << 20;
        unsafe {
            let touched = alloc(SIZE);
            let untouched = alloc(SIZE);
            for i in (0..SIZE).step_by(PAGE) {
                touched.add(i).write_volatile(1);
            }
            let mut ws = Vec::new();
            let t = resident(touched as usize, SIZE / PAGE, &mut ws);
            let u = resident(untouched as usize, SIZE / PAGE, &mut ws);
            let (t_in, u_in) = (t.iter().filter(|&&v| v).count(), u.iter().filter(|&&v| v).count());
            eprintln!("working set: touched {t_in}/{} pages resident, untouched {u_in}/{}", t.len(), u.len());
            assert_eq!(t_in, t.len(), "every touched page is resident");
            assert!(u_in < u.len() / 2, "most untouched pages are not resident (the query works here)");
            let mut skipped = Vec::new();
            add_skipped(&mut skipped, &u, untouched as usize, SIZE);
            assert!(skipped.iter().map(|(a, b)| b - a).sum::<usize>() > SIZE / 2);
            VirtualFree(touched as *mut _, 0, MEM_RELEASE);
            VirtualFree(untouched as *mut _, 0, MEM_RELEASE);
        }
    }

    #[test]
    fn a_scan_finds_a_record_in_this_process() {
        // a record in a page of its own (committed, private, read/write, touched: resident)
        let mut page = vec![0u8; 3 * PAGE].into_boxed_slice();
        let at = (page.as_ptr() as usize + PAGE) & !(PAGE - 1);
        let key: u64 = 0x5eed_0000_0000_0101;
        unsafe { put(page.as_mut_ptr().add(at - page.as_ptr() as usize), key, "Gems") };
        let valid = unsafe { resident(at, 1, &mut Vec::new()) };
        assert_eq!(valid, vec![true]);
        let out = unsafe { sweep(Kind::Full { asked: false }, &Homes::default(), None, None, &HashSet::new()) };
        assert!(out.found.iter().any(|(a, k, t)| *a == at && *k == key && t == "Gems"));
        assert!(out.bytes > 0 && out.complete && out.scope == Scope::All);
        // the same record re-read where it was (verify)
        assert_eq!(unsafe { read_record(at) }, Some(Some((key, "Gems".to_string()))));
        assert_eq!(unsafe { read_record(at + 8) }, Some(None));
        std::hint::black_box(&page);
    }

    #[test]
    fn warm_and_asked_sweeps_read_near_known_records_first() {
        const SIZE: usize = 2 << 20;
        unsafe {
            let base = alloc(SIZE);
            let a = named("kiln test: Ores");
            put(base.add(0x1000), a, "kiln test: Ores");
            // a whole sweep finds it and remembers where
            let mut index = Index::default();
            let out = sweep(Kind::Full { asked: false }, &Homes::default(), None, None, &HashSet::new());
            merge(&mut index, &out);
            assert_eq!(index.known[&a].addr, base as usize + 0x1000);
            assert!(index.allocs.contains(&(base as usize)));
            // a chest named later: its record lands near the other one; a warm sweep finds it, reading only the windows
            let b = named("kiln test: Food");
            put(base.add(0x3000), b, "kiln test: Food");
            let homes = homes_of(&index);
            let warm = sweep(Kind::Warm, &homes, None, None, &HashSet::new());
            assert!(warm.found.iter().any(|(_, k, t)| *k == b && t == "kiln test: Food"));
            assert!(warm.bytes <= homes.windows.len() * (HOME_WINDOW + RECORD) && warm.bytes < out.bytes);
            merge(&mut index, &warm);
            assert!(index.known.contains_key(&b));
            // an asked whole sweep reads the allocations that held records first and stops once the key is found
            let c = named("kiln test: Gems");
            put(base.add(0x5000), c, "kiln test: Gems");
            let asked = sweep(Kind::Full { asked: true }, &homes_of(&index), None, None, &HashSet::from([c]));
            assert!(asked.found.iter().any(|(_, k, _)| *k == c));
            assert!(!asked.complete && matches!(asked.scope, Scope::Ranges(_)));
            assert!(asked.bytes < out.bytes, "asked read {} of {} bytes", asked.bytes, out.bytes);
            // a budget stops a caller's warm sweep
            let none = sweep(Kind::Warm, &homes_of(&index), None, Some(0), &HashSet::new());
            assert_eq!((none.bytes, none.complete), (0, false));
            std::ptr::write_bytes(base, 0, 0x6000);
            VirtualFree(base as *mut _, 0, MEM_RELEASE);
        }
    }

    /// A synthetic heap: 512 MiB of touched private memory beside one allocation with the name records. Prints what a
    /// whole sweep and a warm sweep read and how long they take (no rests), as the release-note numbers.
    #[test]
    fn benchmark_whole_against_warm_sweep() {
        const BIG: usize = 512 << 20;
        unsafe {
            let big = alloc(BIG);
            for i in (0..BIG).step_by(PAGE) {
                big.add(i).write_volatile(1);
            }
            let home = alloc(1 << 20);
            let key = named("kiln bench: Wood");
            put(home.add(0x2000), key, "kiln bench: Wood");
            let full = sweep(Kind::Full { asked: false }, &Homes::default(), None, None, &HashSet::new());
            let at = home as usize + 0x2000;
            assert!(full.found.iter().any(|(a, k, _)| *a == at && *k == key));
            // where the index would look: this record's window and allocation (other tests' records left out)
            let homes = Homes { windows: vec![at & !(HOME_WINDOW - 1)], allocs: HashSet::from([home as usize]) };
            let next = named("kiln bench: Stone");
            put(home.add(0x4000), next, "kiln bench: Stone");
            let warm = sweep(Kind::Warm, &homes, None, None, &HashSet::new());
            let asked = sweep(Kind::Full { asked: true }, &homes, None, None, &HashSet::from([next]));
            let rested = sweep(Kind::Warm, &homes, Some(REST), None, &HashSet::new());
            eprintln!(
                "bench: whole sweep {} MiB in {} ms; warm sweep {} KiB in {} us; asked sweep {} KiB in {} us (stopped early: {}); warm with rests {} us",
                full.bytes >> 20,
                full.took.as_millis(),
                warm.bytes >> 10,
                warm.took.as_micros(),
                asked.bytes >> 10,
                asked.took.as_micros(),
                !asked.complete,
                rested.took.as_micros()
            );
            assert!(full.bytes >= BIG);
            assert!(warm.found.iter().any(|(_, k, _)| *k == next) && asked.found.iter().any(|(_, k, _)| *k == next));
            assert!(warm.bytes <= HOME_WINDOW + RECORD && asked.bytes < BIG);
            std::ptr::write_bytes(home, 0, 0x5000);
            VirtualFree(big as *mut _, 0, MEM_RELEASE);
            VirtualFree(home as *mut _, 0, MEM_RELEASE);
        }
    }
}
