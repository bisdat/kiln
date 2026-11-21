// SPDX-License-Identifier: MIT
//! Host infrastructure only: no Lua IO or profile parsing/admission parity claims.
//! Both SF pins: modules/modloader/mod_package_filesystem.rs:69-87,121-199,219-225,
//! 261-326. The pinned files are byte-identical; expectations below are source-derived.
#![allow(dead_code)]
use sfi_package::source::{
    DiskSource, PackageSource, ProfileOverlaySource, SourceEntry, SourceKind, SourceMetadata,
};
use std::{
    collections::HashMap,
    fs, io,
    path::PathBuf,
    rc::Rc,
    sync::atomic::{AtomicU64, Ordering},
};

static SEQ: AtomicU64 = AtomicU64::new(0);
struct Tree(PathBuf);
impl Tree {
    fn new() -> Self {
        let root = std::env::temp_dir().join(format!(
            "c3h-ext-{}-{}",
            std::process::id(),
            SEQ.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&root).unwrap();
        Self(root.canonicalize().unwrap())
    }
    fn disk(&self) -> Rc<DiskSource> {
        Rc::new(DiskSource::new(&self.0).unwrap())
    }
}
impl Drop for Tree {
    fn drop(&mut self) {
        fs::remove_dir_all(&self.0).unwrap();
    }
}
fn overlay(source: Rc<dyn PackageSource>, files: &[(&str, &str)]) -> ProfileOverlaySource {
    ProfileOverlaySource::new(
        source,
        &files
            .iter()
            .map(|(p, v)| (p.to_string(), v.to_string()))
            .collect(),
    )
}
fn entries(source: &dyn PackageSource, path: &str) -> Vec<SourceEntry> {
    source
        .read_dir(path)
        .unwrap()
        .into_iter()
        .collect::<io::Result<_>>()
        .unwrap()
}
fn predicates(source: &dyn PackageSource, path: &str) -> (bool, bool, bool) {
    (
        source.exists(path),
        source.is_file(path),
        source.is_directory(path),
    )
}

pub fn virtual_file_and_ancestors() {
    // SF does not synthesize disk directories just because override prefixes exist.
    for real_src in [false, true] {
        let tree = Tree::new();
        if real_src {
            fs::create_dir(tree.0.join("src")).unwrap();
        }
        let source = overlay(tree.disk(), &[("src/virtual.lua", "return 'profile'\0雪")]);
        assert_eq!(
            source.read_file("src/virtual.lua").unwrap(),
            "return 'profile'\0雪".as_bytes()
        );
        assert_eq!(predicates(&source, "src/virtual.lua"), (true, true, true));
        assert_eq!(
            source.metadata("src/virtual.lua").unwrap().kind,
            SourceKind::File
        );
        assert_eq!(predicates(&source, "src"), (true, false, true));
        assert_eq!(source.metadata("src").unwrap().kind, SourceKind::Directory);
        assert_eq!(predicates(&source, "sr"), (false, false, false)); // components, not string prefix
        assert_eq!(predicates(&source, "src/virtual"), (false, false, false));
        assert_eq!(
            source.read_dir("src/virtual.lua").unwrap_err().kind(),
            io::ErrorKind::NotFound
        );
        if real_src {
            let listed = entries(&source, "src");
            assert_eq!(listed.len(), 1);
            assert_eq!(
                listed[0],
                SourceEntry {
                    name: "virtual.lua".into(),
                    path: "src/virtual.lua".into(),
                    kind: SourceKind::File,
                }
            );
            assert_eq!(entries(&source, "")[0].path, "src");
        } else {
            assert_eq!(
                source.read_dir("src").unwrap_err().kind(),
                io::ErrorKind::NotFound
            );
            assert!(entries(&source, "").is_empty());
        }
    }
}

pub fn shadowing_and_nested_listing() {
    let tree = Tree::new();
    fs::create_dir_all(tree.0.join("src/real-dir")).unwrap();
    fs::write(tree.0.join("src/shadow.lua"), b"disk").unwrap();
    fs::write(tree.0.join("src/keep.lua"), b"\0\xff\r\n").unwrap();
    fs::write(tree.0.join("src/ancestor"), b"disk ancestor").unwrap();
    fs::write(tree.0.join("src/real-dir/child"), b"child").unwrap();
    let disk = tree.disk();
    let original = entries(disk.as_ref(), "src");
    let source = overlay(
        disk.clone(),
        &[
            ("src/shadow.lua", "profile"),
            ("src/virtual.lua", "virtual"),
            ("src/missing/deep.lua", "nested"),
            ("src/real-dir", "directory override"),
            ("src/shadow.lua/child", "file ancestor"),
            ("src/ancestor/child", "virtual child"),
        ],
    );
    assert_eq!(source.read_file("src/shadow.lua").unwrap(), b"profile");
    assert_eq!(disk.read_file("src/shadow.lua").unwrap(), b"disk");
    assert_eq!(source.read_file("src/keep.lua").unwrap(), b"\0\xff\r\n");
    assert_eq!(predicates(&source, "src/ancestor"), (true, true, true));
    assert_eq!(
        source.metadata("src/ancestor").unwrap().kind,
        SourceKind::Directory
    );
    assert_eq!(source.read_file("src/ancestor").unwrap(), b"disk ancestor");
    assert_eq!(source.read_file("src/missing/deep.lua").unwrap(), b"nested");
    assert_eq!(predicates(&source, "src/missing"), (true, false, true));
    assert_eq!(
        source.read_dir("src/missing").unwrap_err().kind(),
        io::ErrorKind::NotFound
    );
    // Exact overrides can shadow disk directories without hiding their children from listing.
    assert_eq!(
        source.read_file("src/real-dir").unwrap(),
        b"directory override"
    );
    assert_eq!(
        source.metadata("src/real-dir").unwrap().kind,
        SourceKind::File
    );
    assert_eq!(predicates(&source, "src/real-dir"), (true, true, true));
    assert_eq!(
        entries(&source, "src/real-dir")[0].path,
        "src/real-dir/child"
    );
    assert_eq!(
        source.read_dir("src/shadow.lua").unwrap_err().kind(),
        io::ErrorKind::NotADirectory
    );
    let listed = entries(&source, "src");
    assert_eq!(&listed[..original.len()], original); // preserve disk order and structural kinds
    assert_eq!(listed.len(), original.len() + 1);
    assert_eq!(listed.last().unwrap().path, "src/virtual.lua");
    assert_eq!(
        listed.iter().filter(|e| e.path == "src/shadow.lua").count(),
        1
    );
    assert!(!listed.iter().any(|e| e.path == "src/missing")); // no fabricated immediate directory
    fs::write(tree.0.join("src/later.lua"), b"later").unwrap();
    assert!(source.is_file("src/later.lua")); // disk is live, not snapshotted
}

pub fn root_empty_and_component_paths() {
    let tree = Tree::new();
    let source = overlay(
        tree.disk(),
        &[("top.lua", "top"), ("nested/deep.lua", "deep")],
    );
    for path in ["", "."] {
        assert_eq!(predicates(&source, path), (true, false, true));
        assert_eq!(source.metadata(path).unwrap().kind, SourceKind::Directory);
        assert_eq!(
            source.read_file(path).unwrap_err().kind(),
            io::ErrorKind::NotFound
        );
    }
    assert_eq!(
        entries(&source, "")
            .iter()
            .map(|e| e.path.as_str())
            .collect::<Vec<_>>(),
        ["top.lua"]
    );
    // SF compares override.parent() to the original request, not the canonical disk parent.
    assert!(entries(&source, ".").is_empty());
    assert!(entries(&source, tree.0.to_str().unwrap()).is_empty());
    assert_eq!(predicates(&source, "./top.lua"), (false, false, false));
    assert_eq!(
        source.read_file("./top.lua").unwrap_err().kind(),
        io::ErrorKind::NotFound
    );
    assert_eq!(source.read_file("nested//./deep.lua").unwrap(), b"deep");
    assert_eq!(
        predicates(&source, "nested//./deep.lua"),
        (true, true, true)
    );
    assert_eq!(
        predicates(&source, "nested/../top.lua"),
        (false, false, false)
    );
    let empty = overlay(tree.disk(), &[]);
    assert!(entries(&empty, "").is_empty());
    assert_eq!(predicates(&empty, "missing"), (false, false, false));

    // Expanded two-pin disk oracle: Path equality normalizes internal separators
    // and dots, but a leading CurDir is significant. Listings retain the ORIGINAL
    // override spelling; component-equivalent physical entries win deduplication.
    fs::create_dir(tree.0.join("assets")).unwrap();
    fs::write(tree.0.join("assets/same.txt"), b"physical").unwrap();
    let source = overlay(
        tree.disk(),
        &[
            ("assets//./a.txt", "a"),
            ("assets/./b.txt", "b"),
            ("assets//./same.txt", "shadow"),
            ("./leading.lua", "leading"),
            ("./assets//./leading.txt", "nested leading"),
        ],
    );
    for parent in ["assets", "assets/.", "assets//"] {
        let listed = entries(&source, parent);
        assert_eq!(listed.len(), 3);
        assert_eq!(listed[0].path, "assets/same.txt");
        let mut appended: Vec<_> = listed[1..].iter().map(|e| e.path.as_str()).collect();
        appended.sort_unstable();
        assert_eq!(appended, ["assets/./b.txt", "assets//./a.txt"]);
    }
    for path in ["assets/a.txt", "assets//./a.txt", "assets/./a.txt"] {
        assert_eq!(source.read_file(path).unwrap(), b"a");
        assert_eq!(predicates(&source, path), (true, true, true));
    }
    assert_eq!(source.read_file("assets/same.txt").unwrap(), b"shadow");
    assert_eq!(predicates(&source, "leading.lua"), (false, false, false));
    for path in ["./leading.lua", ".//./leading.lua"] {
        assert_eq!(source.read_file(path).unwrap(), b"leading");
        assert_eq!(predicates(&source, path), (true, true, true));
    }
    assert_eq!(entries(&source, ".")[0].path, "assets"); // physical entry first
    assert_eq!(entries(&source, ".")[1].path, "./leading.lua");
    let listed = entries(&source, "./assets");
    assert_eq!(listed.len(), 2);
    assert_eq!(listed[0].path, "assets/same.txt");
    assert_eq!(listed[1].path, "./assets//./leading.txt");
}

pub fn metadata_defaults_and_error_suppression() {
    struct MetadataOnly;
    impl PackageSource for MetadataOnly {
        fn read_file(&self, _: &str) -> io::Result<Vec<u8>> {
            Err(io::ErrorKind::Unsupported.into())
        }
        fn metadata(&self, path: &str) -> io::Result<SourceMetadata> {
            match path {
                "file" => Ok(SourceMetadata {
                    kind: SourceKind::File,
                }),
                "directory" => Ok(SourceMetadata {
                    kind: SourceKind::Directory,
                }),
                "symlink" => Ok(SourceMetadata {
                    kind: SourceKind::Symlink,
                }),
                "other" => Ok(SourceMetadata {
                    kind: SourceKind::Other,
                }),
                "denied" => Err(io::ErrorKind::PermissionDenied.into()),
                "not-dir" => Err(io::ErrorKind::NotADirectory.into()),
                "unsupported" => Err(io::ErrorKind::Unsupported.into()),
                _ => Err(io::ErrorKind::NotFound.into()),
            }
        }
    }
    for (path, expected) in [
        ("file", (true, true, false)),
        ("directory", (true, false, true)),
        ("symlink", (true, false, false)),
        ("other", (true, false, false)),
        ("denied", (false, false, false)),
        ("not-dir", (false, false, false)),
        ("unsupported", (false, false, false)),
        ("missing", (false, false, false)),
    ] {
        assert_eq!(predicates(&MetadataOnly, path), expected);
    }
    let source = overlay(Rc::new(MetadataOnly), &[("denied/virtual", "v")]);
    assert_eq!(predicates(&source, "denied/virtual"), (true, true, true));
    assert_eq!(predicates(&source, "denied"), (true, false, true));
    assert_eq!(predicates(&source, ""), (true, false, true));
    assert_eq!(predicates(&source, "."), (false, false, false));
    assert_eq!(predicates(&source, "file"), (true, true, false));
    assert_eq!(source.read_file("denied/virtual").unwrap(), b"v");
    assert_eq!(
        source.metadata("missing").unwrap_err().kind(),
        io::ErrorKind::NotFound
    );
    assert_eq!(
        source.read_dir("denied").unwrap_err().kind(),
        io::ErrorKind::Unsupported
    );
    struct ReadOnly;
    impl PackageSource for ReadOnly {
        fn read_file(&self, _: &str) -> io::Result<Vec<u8>> {
            Ok(vec![0, 255])
        }
    }
    assert_eq!(predicates(&ReadOnly, "anything"), (false, false, false));
    assert_eq!(ReadOnly.read_file("anything").unwrap(), [0, 255]);
}

pub fn injected_listing_errors_and_independent_predicates() {
    struct Injected;
    impl PackageSource for Injected {
        fn read_file(&self, _: &str) -> io::Result<Vec<u8>> {
            Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "read denied",
            ))
        }
        fn metadata(&self, _: &str) -> io::Result<SourceMetadata> {
            Err(io::ErrorKind::Unsupported.into())
        }
        fn exists(&self, path: &str) -> bool {
            path == "independent"
        }
        fn is_file(&self, path: &str) -> bool {
            path == "independent"
        }
        fn is_directory(&self, path: &str) -> bool {
            path == "independent"
        }
        fn read_dir(&self, path: &str) -> io::Result<Vec<io::Result<SourceEntry>>> {
            if !path.is_empty() {
                return Err(io::Error::new(
                    io::ErrorKind::PermissionDenied,
                    "listing denied",
                ));
            }
            Ok(vec![
                Ok(SourceEntry {
                    name: "same".into(),
                    path: "same".into(),
                    kind: SourceKind::Symlink,
                }),
                Err(io::Error::other("entry failed")),
                // Dedup is by full returned path, not the display name.
                Ok(SourceEntry {
                    name: "alias".into(),
                    path: "resolved/alias".into(),
                    kind: SourceKind::File,
                }),
            ])
        }
    }
    let source = overlay(
        Rc::new(Injected),
        &[
            ("same", "shadow"),
            ("new", "new"),
            ("alias", "alias"),
            ("denied/child", "child"),
        ],
    );
    assert_eq!(predicates(&source, "independent"), (true, true, true));
    assert_eq!(source.read_file("same").unwrap(), b"shadow");
    assert_eq!(
        source.read_file("missing").unwrap_err().to_string(),
        "read denied"
    );
    assert_eq!(
        source.read_dir("denied").unwrap_err().to_string(),
        "listing denied"
    );
    let listed = source.read_dir("").unwrap();
    assert_eq!(listed.len(), 5);
    assert_eq!(listed[0].as_ref().unwrap().kind, SourceKind::Symlink);
    assert_eq!(listed[1].as_ref().unwrap_err().to_string(), "entry failed");
    assert_eq!(listed[2].as_ref().unwrap().path, "resolved/alias");
    let mut appended: Vec<_> = listed[3..]
        .iter()
        .map(|e| e.as_ref().unwrap().path.as_str())
        .collect();
    appended.sort_unstable(); // override HashMap iteration is deliberately not a guarantee
    assert_eq!(appended, ["alias", "new"]);
    let files = HashMap::from([("copy".into(), "original".into())]);
    let source = ProfileOverlaySource::new(Rc::new(Injected), &files);
    drop(files); // owned bytes, not a borrowed profile lifetime
    assert_eq!(source.read_file("copy").unwrap(), b"original");
}

pub fn disk_predicates_and_overlay_symlinks() {
    let tree = Tree::new();
    fs::write(tree.0.join("file"), b"disk").unwrap();
    let disk = tree.disk();
    assert_eq!(predicates(disk.as_ref(), "file"), (true, true, false));
    assert_eq!(predicates(disk.as_ref(), ""), (true, false, true));
    assert_eq!(
        predicates(disk.as_ref(), "file/child"),
        (false, false, false)
    );
    #[cfg(unix)]
    {
        use std::os::unix::fs::symlink;
        let tree = Tree::new();
        fs::create_dir(tree.0.join("package")).unwrap();
        fs::create_dir(tree.0.join("package/real")).unwrap();
        fs::write(tree.0.join("package/real/child"), b"disk").unwrap();
        fs::write(tree.0.join("outside"), b"outside").unwrap();
        symlink("real", tree.0.join("package/alias")).unwrap();
        symlink("../outside", tree.0.join("package/escape")).unwrap();
        symlink("missing", tree.0.join("package/dangling")).unwrap();
        let disk = Rc::new(DiskSource::new(tree.0.join("package")).unwrap());
        assert_eq!(
            predicates(disk.as_ref(), "alias/child"),
            (true, true, false)
        );
        assert_eq!(predicates(disk.as_ref(), "alias"), (true, false, true));
        for path in ["escape", "dangling", "missing", "real/child/nope"] {
            assert_eq!(predicates(disk.as_ref(), path), (false, false, false));
        }
        let source = overlay(
            disk,
            &[("alias/child", "alias override"), ("escape", "safe bytes")],
        );
        assert_eq!(source.read_file("alias/child").unwrap(), b"alias override");
        assert_eq!(source.read_file("real/child").unwrap(), b"disk");
        let listed = entries(&source, "alias");
        assert_eq!(
            listed.iter().map(|e| e.path.as_str()).collect::<Vec<_>>(),
            ["real/child", "alias/child"]
        );
        assert_eq!(predicates(&source, "escape"), (true, true, true));
        assert_eq!(source.read_file("escape").unwrap(), b"safe bytes");
        assert_eq!(
            source.read_dir("escape").unwrap_err().kind(),
            io::ErrorKind::PermissionDenied
        );
    }
}

// These assertions prove host plumbing only. None implements the Lua IO rows
// or the profile-selection/validation row, so no FINAL obligation is promoted.
