// SPDX-License-Identifier: MIT
//! The package content digest : the plan's writer and kiln-sfi compute `content_sha256` with this
//! one function, so its framing is pinned here by a known answer.
use sfi_policy::digest::{package_content_sha256, sha256};
use std::path::{Path, PathBuf};

fn tree(name: &str, files: &[(&str, &[u8])]) -> PathBuf {
    let base = option_env!("CARGO_TARGET_TMPDIR")
        .map(Path::new)
        .filter(|base| base.is_dir())
        .map_or_else(std::env::temp_dir, Path::to_path_buf);
    let root = base
        .join("sfi-policy-digest")
        .join(format!("{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    std::fs::create_dir_all(&root).unwrap();
    for (path, bytes) in files {
        let file = root.join(path);
        std::fs::create_dir_all(file.parent().unwrap()).unwrap();
        std::fs::write(file, bytes).unwrap();
    }
    root
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
const FILES: &[(&str, &[u8])] = &[("mod.json", b"{}"), ("src/mod.lua", b"return 1\n")];

#[test]
fn the_framing_is_pinned_by_a_known_answer() {
    // computed independently (Python hashlib) from the module docs' formula
    let root = tree("kat", FILES);
    assert_eq!(
        hex(&package_content_sha256(&root).unwrap()),
        "033f12183b9b2d1e796e380ebab33baf043e63ba0dc616796c1a9dbe22cd4b59"
    );
    assert_eq!(
        hex(&sha256(b"abc")),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
}

#[test]
fn every_file_its_path_and_its_bytes_count_and_nothing_else() {
    let a = package_content_sha256(&tree("same-a", FILES)).unwrap();
    // the same tree written in another order
    let reversed: Vec<_> = FILES.iter().rev().copied().collect();
    assert_eq!(
        a,
        package_content_sha256(&tree("same-b", &reversed)).unwrap()
    );
    // an empty folder adds no file
    let root = tree("empty-dir", FILES);
    std::fs::create_dir_all(root.join("assets/empty")).unwrap();
    assert_eq!(a, package_content_sha256(&root).unwrap());
    for (label, files) in [
        (
            "a changed byte",
            vec![
                ("mod.json", b"{ }".as_slice()),
                ("src/mod.lua", b"return 1\n"),
            ],
        ),
        (
            "a renamed file",
            vec![("mod.json", b"{}"), ("src/main.lua", b"return 1\n")],
        ),
        (
            "a moved file",
            vec![("mod.json", b"{}"), ("mod.lua", b"return 1\n")],
        ),
        (
            "an added file",
            vec![
                ("mod.json", b"{}"),
                ("src/mod.lua", b"return 1\n"),
                ("x.dll", b""),
            ],
        ),
        (
            "a case-only rename",
            vec![("Mod.json", b"{}"), ("src/mod.lua", b"return 1\n")],
        ),
        // the framing: a path/content boundary moved by one byte is another input
        (
            "a split moved",
            vec![("mod.jso", b"n{}"), ("src/mod.lua", b"return 1\n")],
        ),
    ] {
        let label_dir = label.replace(' ', "-");
        assert_ne!(
            a,
            package_content_sha256(&tree(&label_dir, &files)).unwrap(),
            "{label}"
        );
    }
}

/// Explorer's own `desktop.ini` and `Thumbs.db` (any letter case, any depth) are left out on both
/// sides of the plan; anything else of a similar name, a folder of that name and a link of that name still count.
#[test]
fn explorer_files_are_left_out_and_nothing_else() {
    let a = package_content_sha256(&tree("explorer-base", FILES)).unwrap();
    let with: Vec<(&str, &[u8])> = FILES
        .iter()
        .copied()
        .chain([
            ("desktop.ini", b"[.ShellClassInfo]".as_slice()),
            ("src/Thumbs.db", b"\x00\x01"),
            ("assets/deep/DESKTOP.INI", b"x"),
            ("thumbs.DB", b"y"),
        ])
        .collect();
    assert_eq!(
        a,
        package_content_sha256(&tree("explorer-files", &with)).unwrap()
    );
    for (label, extra) in [
        ("a near name", ("desktop.ini.bak", b"x".as_slice())),
        ("another extension", ("thumbs.dbx", b"x")),
        (
            "a file in a folder of that name",
            ("desktop.ini/inside.lua", b"x"),
        ),
    ] {
        let files: Vec<(&str, &[u8])> = FILES.iter().copied().chain([extra]).collect();
        assert_ne!(
            a,
            package_content_sha256(&tree(&label.replace(' ', "-"), &files)).unwrap(),
            "{label}"
        );
    }
    assert!(sfi_policy::digest::explorer_file("Desktop.Ini"));
    assert!(!sfi_policy::digest::explorer_file("desktop.ini "));
    #[cfg(unix)]
    {
        let root = tree("explorer-link", FILES);
        std::os::unix::fs::symlink(root.join("mod.json"), root.join("desktop.ini")).unwrap();
        assert!(
            package_content_sha256(&root).is_err(),
            "a link named desktop.ini is still a link"
        );
    }
}

#[test]
fn links_and_other_objects_are_refused() {
    let root = tree("not-a-folder", &[("file", b"x")]);
    assert!(
        package_content_sha256(&root.join("file")).is_err(),
        "a file is not a package folder"
    );
    assert!(package_content_sha256(&root.join("missing")).is_err());
    #[cfg(unix)]
    {
        let root = tree("link-inside", FILES);
        std::os::unix::fs::symlink(root.join("mod.json"), root.join("alias.json")).unwrap();
        let err = package_content_sha256(&root).unwrap_err();
        assert!(err.contains("alias.json is a link"), "{err}");
        let root = tree("link-folder", FILES);
        std::os::unix::fs::symlink(root.join("src"), root.join("lib")).unwrap();
        assert!(package_content_sha256(&root).is_err());
        // the package folder itself a link
        let real = tree("link-root-real", FILES);
        let link = real.with_file_name(format!("link-root-{}", std::process::id()));
        let _ = std::fs::remove_file(&link);
        std::os::unix::fs::symlink(&real, &link).unwrap();
        assert!(package_content_sha256(&link).is_err());
        let _ = std::fs::remove_file(&link);
    }
}

#[test]
fn a_plain_file_is_opened_through_a_checked_handle() {
    use std::io::Read as _;
    let root = tree("open", &[("plan.json", b"{}")]);
    let mut text = String::new();
    sfi_policy::digest::open_plain_file(&root.join("plan.json"))
        .unwrap()
        .read_to_string(&mut text)
        .unwrap();
    assert_eq!(text, "{}");
    assert!(sfi_policy::digest::plain_folder(&root));
    assert!(
        sfi_policy::digest::open_plain_file(&root).is_err(),
        "a folder is not a plain file"
    );
    #[cfg(unix)]
    {
        std::os::unix::fs::symlink(root.join("plan.json"), root.join("link.json")).unwrap();
        assert!(sfi_policy::digest::open_plain_file(&root.join("link.json")).is_err());
        std::os::unix::fs::symlink(
            &root,
            root.with_file_name(format!("open-link-{}", std::process::id())),
        )
        .unwrap();
        assert!(!sfi_policy::digest::plain_folder(
            &root.with_file_name(format!("open-link-{}", std::process::id()))
        ));
        let _ =
            std::fs::remove_file(root.with_file_name(format!("open-link-{}", std::process::id())));
    }
}

/// Native Windows: the reader's handle shares the file for reading only, so nobody writes, renames or deletes it while
/// it is read; a junction or link as the package folder, or inside it, is refused.
#[cfg(windows)]
#[test]
fn on_windows_the_open_file_is_shared_for_reading_only_and_reparse_points_are_refused() {
    use std::io::Read as _;
    let root = tree(
        "win-share",
        &[("plan.json", b"{}"), ("pkg/mod.json", b"{}")],
    );
    let plan = root.join("plan.json");
    let held = sfi_policy::digest::open_plain_file(&plan).unwrap();
    let sharing = |r: std::io::Result<()>| r.unwrap_err().raw_os_error();
    assert_eq!(
        sharing(
            std::fs::OpenOptions::new()
                .write(true)
                .open(&plan)
                .map(drop)
        ),
        Some(32)
    );
    assert_eq!(sharing(std::fs::remove_file(&plan)), Some(32));
    assert_eq!(
        sharing(std::fs::rename(&plan, root.join("moved.json"))),
        Some(32)
    );
    let mut other = String::new();
    std::fs::File::open(&plan)
        .unwrap()
        .read_to_string(&mut other)
        .unwrap();
    assert_eq!(other, "{}", "another reader may read it");
    drop(held);
    std::fs::OpenOptions::new().write(true).open(&plan).unwrap();
    // a junction (no privilege needed) as the package folder, and inside a package folder
    let junction = |link: &Path, target: &Path| {
        let status = std::process::Command::new("cmd")
            .args(["/C", "mklink", "/J"])
            .arg(link)
            .arg(target)
            .output()
            .unwrap();
        assert!(status.status.success(), "{status:?}");
    };
    let link = root.join("pkg-junction");
    junction(&link, &root.join("pkg"));
    assert!(
        package_content_sha256(&link).is_err(),
        "a junction is not a plain folder"
    );
    assert!(!sfi_policy::digest::plain_folder(&link));
    let inside = tree("win-junction-inside", FILES);
    junction(&inside.join("lib"), &inside.join("src"));
    let err = package_content_sha256(&inside).unwrap_err();
    assert!(err.contains("lib is a link"), "{err}");
    // a file symbolic link, where this account may create one
    match std::os::windows::fs::symlink_file(&plan, root.join("link.json")) {
        Ok(()) => assert!(sfi_policy::digest::open_plain_file(&root.join("link.json")).is_err()),
        Err(e) => println!("SKIP file symlink: {e}"),
    }
}
