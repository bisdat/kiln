// SPDX-License-Identifier: GPL-3.0-or-later
//! What the startup pass costs on an archive of a real client's size (kiln-eml's first kiln-mode start, a cache miss):
//! a synthetic archive with about the resource file of a client (215 MiB of compressed resources) and as many GiB of
//! contents in 1 GiB containers as `KILN_EML_TIMING_CONTENT_MIB` says (8 GiB by default; a client has 48 GiB). One
//! ShroudForge package edits three resources and creates three contents, as a small real mod does.
//!
//! Ignored by default (it writes the archive, gigabytes, below TMPDIR): run it with TMPDIR on a disk with room,
//! `cargo test --release -p kiln-eml --test m6_startup_timing -- --ignored --nocapture`. It prints one line per step
//! (the startup controller's points) and what the process read and wrote, and fails when the pass reads the game's
//! containers whole: their contents are the baseline's, held by the input witness, and only the stage's own containers
//! are read back.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;
#[path = "support/m6_startup/mod.rs"]
mod startup_support;

use fixture::{
    TestRoot,
    archive::{ArchiveFixture, PhysicalSet, STEM, resource_id},
    types,
};
use kfc::{
    container::{KFCFile, KFCWriteOptions, KFCWriter},
    guid::{ContentHash, Guid, ResourceId},
    hash::fnv,
    reflection::LookupKey,
};
use kiln_eml::{sf_pass::StartupDialect, startup::State};
use startup_support::{Start, edits, native};
use std::{cell::RefCell, collections::BTreeMap, fs, rc::Rc, time::Instant};

fn env(name: &str, default: u64) -> u64 {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

/// xorshift64*: incompressible bytes, fast enough for gigabytes.
struct Bytes(u64);
impl Bytes {
    fn fill(&mut self, out: &mut [u8]) {
        for chunk in out.chunks_mut(8) {
            self.0 ^= self.0 >> 12;
            self.0 ^= self.0 << 25;
            self.0 ^= self.0 >> 27;
            let v = self.0.wrapping_mul(0x2545_f491_4f6c_dd1d).to_le_bytes();
            chunk.copy_from_slice(&v[..chunk.len()]);
        }
    }
}

/// What this process read and wrote so far (all reads, the page cache's included).
fn io_counters() -> (u64, u64) {
    #[cfg(target_os = "linux")]
    {
        let text = fs::read_to_string("/proc/self/io").unwrap_or_default();
        let field = |name: &str| {
            text.lines()
                .find_map(|l| l.strip_prefix(name))
                .and_then(|v| v.trim().parse().ok())
                .unwrap_or(0)
        };
        (field("rchar:"), field("wchar:"))
    }
    #[cfg(windows)]
    {
        #[repr(C)]
        #[derive(Default)]
        struct IoCounters {
            read_ops: u64,
            write_ops: u64,
            other_ops: u64,
            read_bytes: u64,
            write_bytes: u64,
            other_bytes: u64,
        }
        #[link(name = "kernel32")]
        unsafe extern "system" {
            fn GetCurrentProcess() -> *mut std::ffi::c_void;
            fn GetProcessIoCounters(process: *mut std::ffi::c_void, counters: *mut IoCounters) -> i32;
        }
        let mut c = IoCounters::default();
        unsafe { GetProcessIoCounters(GetCurrentProcess(), &mut c) };
        (c.read_bytes, c.write_bytes)
    }
    #[cfg(not(any(target_os = "linux", windows)))]
    {
        (0, 0)
    }
}

/// Drops the page cache's copy of every file below `dir` (Linux; a no-op elsewhere): the next read comes from the disk,
/// as on a start whose game files are not in memory.
fn evict(dir: &std::path::Path) {
    #[cfg(target_os = "linux")]
    {
        use std::os::fd::AsRawFd;
        unsafe extern "C" {
            fn posix_fadvise(fd: i32, offset: i64, len: i64, advice: i32) -> i32;
        }
        for e in fs::read_dir(dir).unwrap().flatten() {
            let p = e.path();
            if p.is_dir() {
                evict(&p);
            } else if let Ok(f) = fs::File::open(&p) {
                let _ = f.sync_all();
                unsafe { posix_fadvise(f.as_raw_fd(), 0, 0, 4) }; // POSIX_FADV_DONTNEED
            }
        }
    }
    #[cfg(not(target_os = "linux"))]
    let _ = dir;
}

/// The archive (see the module docs): the three `m6.Root` resources the package edits, filler resources of the same type
/// (about 2.4:1 compressible, as a client's are) up to `resource_mib` compressed, and `content_mib` of contents averaging
/// 412 KiB.
fn large(tag: &str, resource_mib: u64, content_mib: u64) -> (ArchiveFixture, usize, u64) {
    let types = Rc::new(types::registry());
    let root = TestRoot::new(tag);
    let game = root.0.join("game");
    fs::create_dir(&game).unwrap();
    fs::File::create(game.join(format!("{STEM}.kfc"))).unwrap();
    fs::File::create(game.join(format!("{STEM}.kfc_resources"))).unwrap();
    let mut writer = KFCWriter::<Rc<KFCFile>, _>::new_with_options(
        &game,
        STEM,
        types.clone(),
        &types.version,
        KFCWriteOptions {
            max_container_size: 1 << 30,
            truncate_containers: true,
            ..Default::default()
        },
    )
    .unwrap();
    let ty = types.get_by_name(LookupKey::Qualified(types::ROOT)).unwrap();
    let bytes = types::root_value().to_bytes(&types, ty).unwrap();
    for part in [9, 0, 3] {
        writer.write_resource(&resource_id(part), &bytes).unwrap();
    }
    let mut rng = Bytes(0x9e37_79b9_7f4a_7c15);
    // 5200 bytes each: 2100 random bytes, then that run again (zstd keeps one copy): about 2.4:1
    let fillers = (resource_mib << 20) / 2150;
    let mut buf = vec![0u8; 5200];
    for _ in 0..fillers {
        rng.fill(&mut buf[..2100]);
        let (head, tail) = buf.split_at_mut(2100);
        for (j, b) in tail.iter_mut().enumerate() {
            *b = head[j % 2100];
        }
        // random ids, as the game's are: the index's lookup buckets hash the first bytes of the GUID
        let mut id = [0u8; 16];
        rng.fill(&mut id);
        let hex: String = id.iter().map(|b| format!("{b:02x}")).collect();
        let guid = Guid::parse(&format!("{}-{}-{}-{}-{}", &hex[..8], &hex[8..12], &hex[12..16], &hex[16..20], &hex[20..])).unwrap();
        writer.write_resource(&ResourceId::new(guid, fnv(types::ROOT), 0), &buf).unwrap();
    }
    let mut contents = 0usize;
    let mut written = 0u64;
    let mut blob = vec![0u8; 724 << 10];
    while written < content_mib << 20 {
        let size = (100 << 10) + ((contents as u64 * 7919) % (624 << 10)) as usize;
        rng.fill(&mut blob[..size]);
        writer.write_content(&ContentHash::from_data(&blob[..size]), &blob[..size]).unwrap();
        written += size as u64;
        contents += 1;
    }
    writer.finalize().unwrap();
    let cache = game.join("kiln/eml/cache");
    fs::create_dir_all(&cache).unwrap();
    fs::write(cache.join(format!("{STEM}-types.json")), serde_json::to_vec(&*types).unwrap()).unwrap();
    (ArchiveFixture { root, game, types, original: PhysicalSet(BTreeMap::new()) }, contents, written)
}

fn mib(bytes: u64) -> String {
    format!("{:.1} MiB", bytes as f64 / (1u64 << 20) as f64)
}

#[test]
#[ignore = "writes gigabytes below TMPDIR; run by hand (module docs)"]
fn a_first_kiln_mode_start_on_a_client_sized_archive_reads_the_stage_not_the_game() {
    let resource_mib = env("KILN_EML_TIMING_RESOURCE_MIB", 215);
    let content_mib = env("KILN_EML_TIMING_CONTENT_MIB", 8192);
    let cold = env("KILN_EML_TIMING_COLD", 1) == 1;
    let built = Instant::now();
    let (f, contents, content_bytes) = large("timing", resource_mib, content_mib);
    let header = fs::metadata(f.game.join(format!("{STEM}.kfc"))).unwrap().len();
    let resources = fs::metadata(f.game.join(format!("{STEM}.kfc_resources"))).unwrap().len();
    let containers = KFCFile::from_path(f.game.join(format!("{STEM}.kfc")), false).unwrap().containers().len();
    println!(
        "fixture: header {}, resources {}, {contents} contents in {containers} containers ({}), built in {:.1} s",
        mib(header), mib(resources), mib(content_bytes), built.elapsed().as_secs_f64()
    );
    fs::write(f.game.join(format!("{STEM}.exe")), startup_support::EXE).unwrap();
    fs::create_dir_all(f.game.join("mods")).unwrap();
    let s = Start::over(f);
    let lua = |v: u32| [edits(0, v, "one"), edits(3, v + 1, "two"), edits(9, v + 2, "three")].join("\n");
    s.patch("skeleton", &[], &[], &lua(41));
    let mut runs = Vec::new();
    for (n, value) in [(1, 41), (2, 51)] {
        if n == 2 {
            // the next start with another setting of the package: a cache miss over the output the first one published
            s.patch("skeleton", &[], &[], &lua(value));
        }
        let t = Instant::now();
        let plan = s.plan(StartupDialect::Shroudforge, &[]);
        let planned = t.elapsed();
        if cold {
            evict(s.game());
        }
        let (r0, w0) = io_counters();
        let t = Instant::now();
        let w = s.witness(&plan).unwrap_or_else(|r| panic!("witness refused: {r:?}"));
        let witnessed = t.elapsed();
        let (r1, w1) = io_counters();
        if cold {
            evict(s.game());
        }
        let points: RefCell<Vec<(String, Instant)>> = RefCell::default();
        let (r2, w2) = io_counters();
        let began = Instant::now();
        let out = s.run_with(&w, &plan, &native(), &|_| Ok(None), &|p| points.borrow_mut().push((format!("{p:?}"), Instant::now())));
        let ran = began.elapsed();
        let (r3, w3) = io_counters();
        assert_eq!(out.state, State::Committed, "start {n}: {out:?}");
        let mut last = began;
        let steps: Vec<String> = points
            .borrow()
            .iter()
            .map(|(p, at)| {
                let line = format!("{p} +{:.0} ms", at.duration_since(last).as_secs_f64() * 1000.0);
                last = *at;
                line
            })
            .collect();
        println!(
            "start {n} ({}): plan {:.0} ms; witness {:.0} ms, read {}, wrote {}; pass {:.0} ms, read {}, wrote {}",
            if cold { "cold" } else { "warm" },
            planned.as_secs_f64() * 1000.0,
            witnessed.as_secs_f64() * 1000.0, mib(r1 - r0), mib(w1 - w0),
            ran.as_secs_f64() * 1000.0, mib(r3 - r2), mib(w3 - w2)
        );
        println!("start {n} steps: {}", steps.join(", "));
        runs.push((n, r3 - r2));
    }
    for (n, read) in runs {
        // the stage's own files are copied, hashed and compared a few times: a multiple of the resource file whatever
        // the contents, never the containers on top
        assert!(
            read < 12 * resources + (64 << 20),
            "start {n} read {} (contents {}, resource file {}): the pass read the game's containers",
            mib(read), mib(content_bytes), mib(resources)
        );
    }
}
