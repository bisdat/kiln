// SPDX-License-Identifier: MIT
//! Closed ordinary/lazy graph. System proofs can only come from the OS backend;
//! a module-name allow-list cannot manufacture one.
use super::*;
use crate::pe_gate::{self, ImportKind};
use std::collections::{BTreeMap, BTreeSet, VecDeque};
#[derive(Clone, Copy, Debug)]
pub(super) enum Source {
    Package(usize),
    System(usize),
}
pub(super) struct Node {
    pub source: Source,
    pub flags: u32,
    pub requested: BTreeSet<String>,
}
pub(super) struct Api {
    pub importer: String,
    pub requested: String,
    pub host: String,
}
pub(super) struct Closure {
    pub systems: Vec<SystemImage>,
    pub nodes: Vec<Node>,
    pub apis: Vec<Api>,
    pub verified: VerifiedSystems,
}
impl Closure {
    pub fn snapshot<'a>(&'a self, packages: &'a [&Snapshot], index: usize) -> &'a Snapshot {
        match self.nodes[index].source {
            Source::Package(i) => packages[i],
            Source::System(i) => &self.systems[i].snapshot,
        }
    }
}
fn module_name(raw: &str) -> Result<String, String> {
    if raw.is_empty()
        || !raw.is_ascii()
        || raw.contains(['/', '\\', ':', '\0'])
        || raw.ends_with([' ', '.'])
    {
        return Err(format!("noncanonical dependency name: {raw}"));
    }
    Ok(if raw.contains('.') {
        raw.to_ascii_lowercase()
    } else {
        format!("{}.dll", raw.to_ascii_lowercase())
    })
}
fn api_name(raw: &str) -> bool {
    raw.starts_with("api-") || raw.starts_with("ext-")
}
fn basename(snapshot: &Snapshot) -> Result<String, String> {
    snapshot
        .path
        .file_name()
        .and_then(|s| s.to_str())
        .map(str::to_ascii_lowercase)
        .ok_or_else(|| "unverifiable native filename".into())
}
pub(super) fn resolve(
    packages: &[&Snapshot],
    mut system: impl FnMut(&Snapshot, &str) -> Result<Option<SystemImage>, String>,
) -> Result<Closure, String> {
    if packages.is_empty() || packages.len() > 1024 {
        return Err("native package inventory count exceeded".into());
    }
    let mut package_names = BTreeMap::new();
    for (i, p) in packages.iter().enumerate() {
        let key = pe_gate::canonical(&p.name)
            .ok_or_else(|| format!("noncanonical native image: {}", p.name))?;
        if package_names.insert(key, i).is_some() {
            return Err("case-colliding native image inventory".into());
        }
        if p.name.rsplit('/').next().unwrap_or("").to_ascii_lowercase() != basename(p)? {
            return Err("logical image name does not match its physical DLL name".into());
        }
    }
    let mut out = Closure {
        systems: Vec::new(),
        nodes: vec![Node {
            source: Source::Package(0),
            flags: 2 | 8,
            requested: BTreeSet::new(),
        }],
        apis: Vec::new(),
        verified: VerifiedSystems {
            names: BTreeSet::new(),
        },
    };
    let mut package_nodes = BTreeMap::from([(0usize, 0usize)]);
    let mut system_nodes: BTreeMap<String, usize> = BTreeMap::new();
    let mut pending = VecDeque::from([0usize]);
    let mut queued = BTreeSet::from([0usize]);
    let mut cache: Vec<Option<pe_gate::PeImports>> = vec![None];
    let mut edges = 0usize;
    let mut bytes_total: usize = packages.iter().map(|p| p.bytes.len()).sum();
    while let Some(index) = pending.pop_front() {
        queued.remove(&index);
        if out.nodes.len() > 512 || bytes_total > 512 * 1024 * 1024 {
            return Err("native closure binding/byte budget exceeded".into());
        }
        if cache[index].is_none() {
            let snapshot = out.snapshot(packages, index);
            cache[index] = Some(
                match out.nodes[index].source {
                    Source::Package(_) => pe_gate::parse(&snapshot.bytes),
                    Source::System(_) => pe_gate::parse_system(&snapshot.bytes),
                }
                .map_err(|e| format!("{}: {e}", snapshot.name))?,
            );
        }
        let parsed = cache[index]
            .as_ref()
            .ok_or("missing parsed native image")?
            .clone();
        for import in parsed.imports {
            // Trusted OS DLLs own their optional lazy contracts. Expanding them
            // invents package requirements (and walks hundreds of system DLLs).
            // The package's own ordinary/delay imports remain mandatory proofs.
            if matches!(out.nodes[index].source, Source::System(_))
                && import.kind == ImportKind::Delay
            {
                continue;
            }
            let selected_export = import
                .exported_as
                .iter()
                .any(|name| out.nodes[index].requested.contains(name));
            if matches!(out.nodes[index].source, Source::System(_))
                && import.kind == ImportKind::ForwardedExport
                && !selected_export
            {
                continue;
            }
            edges += 1;
            if edges > 16384 {
                return Err("native dependency edge budget exceeded".into());
            }
            if pe_gate::banned_module(&import.module) {
                return Err(format!(
                    "banned {:?} dependency {} [{}]",
                    import.kind,
                    import.module,
                    import.symbols.join(",")
                ));
            }
            let name = module_name(&import.module)?;
            let source_name = out.snapshot(packages, index).name.clone();
            let physical_importer = basename(out.snapshot(packages, index))?;
            let sibling = match source_name.rsplit_once('/') {
                Some((dir, _)) => format!("{dir}/{name}"),
                None => name.clone(),
            };
            let package_target =
                if matches!(out.nodes[index].source, Source::Package(_)) && !api_name(&name) {
                    package_names.get(&sibling).copied()
                } else {
                    None
                };
            let target = if let Some(p) = package_target {
                if packages[p].folder != packages[0].folder {
                    return Err("dependency outside the DLL's folder (ShroudForge would not find it either)".into());
                }
                if let Some(node) = package_nodes.get(&p) {
                    *node
                } else {
                    let node = out.nodes.len();
                    out.nodes.push(Node {
                        source: Source::Package(p),
                        flags: 0,
                        requested: BTreeSet::new(),
                    });
                    cache.push(None);
                    package_nodes.insert(p, node);
                    node
                }
            } else {
                let proof=system(out.snapshot(packages,index),&name)?.ok_or_else(||format!("unresolved dependency {name} imported by {source_name}; no package snapshot or verified OS binding"))?;
                if !proof.requested.eq_ignore_ascii_case(&name)
                    || !proof.importer.eq_ignore_ascii_case(&physical_importer)
                {
                    return Err("OS proof is for a different import context".into());
                }
                let host = proof.host.to_ascii_lowercase();
                if pe_gate::banned_module(&host) {
                    return Err("OS resolution returned a banned module".into());
                }
                out.verified.names.insert(name.clone());
                if api_name(&name)
                    && !out.apis.iter().any(|a| {
                        a.importer == physical_importer && a.requested == name && a.host == host
                    })
                {
                    out.apis.push(Api {
                        importer: physical_importer.clone(),
                        requested: name.clone(),
                        host: host.clone(),
                    });
                }
                if let Some(existing) = system_nodes.get(&host) {
                    let old = out.snapshot(packages, *existing);
                    if old.id != proof.snapshot.id || old.digest != proof.snapshot.digest {
                        return Err("OS binding changed during closure resolution".into());
                    }
                    *existing
                } else {
                    bytes_total = bytes_total
                        .checked_add(proof.snapshot.bytes.len())
                        .ok_or("native byte budget overflow")?;
                    let source = out.systems.len();
                    out.systems.push(proof);
                    let node = out.nodes.len();
                    out.nodes.push(Node {
                        source: Source::System(source),
                        flags: 1,
                        requested: BTreeSet::new(),
                    });
                    cache.push(None);
                    system_nodes.insert(host, node);
                    node
                }
            };
            let forward_lazy = import.kind == ImportKind::ForwardedExport && !selected_export;
            let flags = if import.kind == ImportKind::Delay || forward_lazy {
                4
            } else {
                out.nodes[index].flags & (2 | 4)
            };
            let before = out.nodes[target].flags;
            out.nodes[target].flags |= flags;
            let mut changed = before != out.nodes[target].flags;
            for symbol in import.symbols {
                changed |= out.nodes[target].requested.insert(symbol);
            }
            if changed && queued.insert(target) {
                pending.push_back(target);
            }
        }
    }
    let mut basenames = BTreeMap::new();
    for index in 0..out.nodes.len() {
        let image = out.snapshot(packages, index);
        let name = basename(image)?;
        if let Some(digest) = basenames.insert(name.clone(), image.digest.clone())
            && digest != image.digest
        {
            return Err(format!(
                "same-basename bindings have different inspected bytes: {name}"
            ));
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn image(import: Option<(&str, bool)>) -> Vec<u8> {
        let mut b = vec![0u8; 0x600];
        b[..2].copy_from_slice(b"MZ");
        for (at, value) in [
            (0x3c, 0x80u32),
            (0x98 + 56, 0x2000),
            (0x98 + 60, 0x200),
            (0x188 + 8, 0x400),
            (0x188 + 12, 0x1000),
            (0x188 + 16, 0x400),
            (0x188 + 20, 0x200),
        ] {
            b[at..at + 4].copy_from_slice(&value.to_le_bytes());
        }
        b[0x80..0x84].copy_from_slice(b"PE\0\0");
        for (at, value) in [
            (0x84, 0x8664u16),
            (0x86, 1),
            (0x94, 240),
            (0x96, 0x2000),
            (0x98, 0x20b),
            (0x98 + 68, 3),
        ] {
            b[at..at + 2].copy_from_slice(&value.to_le_bytes());
        }
        if let Some((name, delay)) = import {
            b[0x98 + 108..0x98 + 112].copy_from_slice(&16u32.to_le_bytes());
            let dir = 0x98 + 112 + if delay { 13 * 8 } else { 8 };
            b[dir..dir + 4].copy_from_slice(&0x1000u32.to_le_bytes());
            b[dir + 4..dir + 8].copy_from_slice(&(if delay { 64u32 } else { 40 }).to_le_bytes());
            let entries: Vec<(usize, u32)> = if delay {
                vec![
                    (0x200, 1),
                    (0x204, 0x1080),
                    (0x20c, 0x1100),
                    (0x210, 0x1100),
                ]
            } else {
                vec![(0x200, 0x1100), (0x20c, 0x1080), (0x210, 0x1100)]
            };
            for (at, value) in entries {
                b[at..at + 4].copy_from_slice(&value.to_le_bytes());
            }
            b[0x280..0x280 + name.len()].copy_from_slice(name.as_bytes());
            b[0x300..0x308].copy_from_slice(&((1u64 << 63) | 1).to_le_bytes());
        }
        b
    }
    #[test]
    fn trusted_system_delay_imports_do_not_expand_the_package_closure() {
        use std::sync::{
            Arc,
            atomic::{AtomicUsize, Ordering},
        };
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let root = std::env::temp_dir().join(format!(
            "sfi-system-delay-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&root).unwrap();
        let run = |delay| {
            let entry_path = root.join("entry.dll");
            let os_path = root.join("os.dll");
            std::fs::write(&entry_path, image(Some(("os.dll", false)))).unwrap();
            std::fs::write(&os_path, image(Some(("ext-ms-optional-l1-1-0.dll", delay)))).unwrap();
            let entry = Snapshot::capture(&entry_path, "entry.dll").unwrap();
            let os = Arc::new(Snapshot::capture(&os_path, "os.dll").unwrap());
            resolve(&[&entry], |importer, name| {
                Ok((name == "os.dll").then(|| SystemImage {
                    snapshot: os.clone(),
                    requested: name.into(),
                    importer: basename(importer).unwrap(),
                    host: "os.dll".into(),
                }))
            })
            .map(|closure| closure.nodes.len())
        };
        assert_eq!(run(true).unwrap(), 2);
        assert!(run(false).is_err());
        std::fs::remove_dir_all(root).unwrap();
    }
}
