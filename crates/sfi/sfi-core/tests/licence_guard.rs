// SPDX-License-Identifier: MIT
use std::{fs, path::Path, process::Command};

use serde_json::Value;

// Split the forbidden crate token so this guard does not exempt itself from the source scan.
const FORBIDDEN_CRATE: &str = concat!("kf", "c");

fn check_package(name: &str, licence: Option<&str>) -> Result<(), String> {
    for prefix in [FORBIDDEN_CRATE, "mod-loader", "shroudforge", "eml"] {
        if name.to_ascii_lowercase().starts_with(prefix) {
            return Err(format!("forbidden package: {name}"));
        }
    }
    let licence = licence.ok_or_else(|| format!("package has no declared licence: {name}"))?;
    if !permissive_alternative(licence) {
        return Err(format!("forbidden licence for {name}: {licence}"));
    }
    Ok(())
}

// SPDX AND binds more tightly than OR. An OR lets us select one alternative;
// every term of a selected AND must be permissive. Parse the entire expression
// (including unselected alternatives), failing closed on malformed syntax.
// Cargo also retains old slash-separated dual-licence declarations.
fn permissive_alternative(expression: &str) -> bool {
    struct Parser<'a> {
        tokens: Vec<&'a str>,
        position: usize,
    }
    impl<'a> Parser<'a> {
        fn take(&mut self, token: &str) -> bool {
            if self.tokens.get(self.position) == Some(&token) {
                self.position += 1;
                true
            } else {
                false
            }
        }
        fn alternative(&mut self) -> Option<bool> {
            let mut allowed = self.conjunction()?;
            while self.take("OR") || self.take("/") {
                let next = self.conjunction()?;
                allowed |= next;
            }
            Some(allowed)
        }
        fn conjunction(&mut self) -> Option<bool> {
            let mut allowed = self.term()?;
            while self.take("AND") {
                let next = self.term()?;
                allowed &= next;
            }
            Some(allowed)
        }
        fn identifier(&mut self) -> Option<&'a str> {
            let token = *self.tokens.get(self.position)?;
            if matches!(token, "AND" | "OR" | "WITH")
                || !token
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'-' | b'.' | b'+'))
            {
                return None;
            }
            self.position += 1;
            Some(token)
        }
        fn term(&mut self) -> Option<bool> {
            if self.take("(") {
                let allowed = self.alternative()?;
                return self.take(")").then_some(allowed);
            }
            let licence = self.identifier()?;
            let allowed = matches!(
                licence,
                "MIT"
                    | "Apache-2.0"
                    | "ISC"
                    | "Zlib"
                    | "0BSD"
                    | "NCSA"
                    | "CC0-1.0"
                    | "Unlicense"
                    | "Unicode-3.0"
                    | "Unicode-DFS-2016"
                    | "BSD-2-Clause"
                    | "BSD-3-Clause"
                    | "BSD-4-Clause"
                    | "BSD-3-Clause-Clear"
                    | "BSD-2-Clause-Patent"
            );
            if self.take("WITH") {
                let exception = self.identifier()?;
                return Some(allowed && licence == "Apache-2.0" && exception == "LLVM-exception");
            }
            Some(allowed && !licence.to_ascii_uppercase().contains("GPL"))
        }
    }
    let mut tokens = Vec::new();
    let mut start = 0;
    for (index, ch) in expression.char_indices() {
        if ch.is_whitespace() || matches!(ch, '(' | ')' | '/') {
            if start < index {
                tokens.push(&expression[start..index]);
            }
            if !ch.is_whitespace() {
                tokens.push(&expression[index..index + ch.len_utf8()]);
            }
            start = index + ch.len_utf8();
        }
    }
    if start < expression.len() {
        tokens.push(&expression[start..]);
    }
    let mut parser = Parser {
        tokens,
        position: 0,
    };
    parser.alternative() == Some(true) && parser.position == parser.tokens.len()
}

#[test]
fn resolved_workspace_is_independent_and_licence_clean() {
    let workspace = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
    // cargo test is already run through the shared build slot; metadata stays in that slot.
    let result = Command::new(env!("CARGO"))
        .current_dir(workspace)
        .args(["metadata", "--format-version", "1", "--locked", "--offline"])
        .arg("--manifest-path")
        .arg(workspace.join("Cargo.toml"))
        .output()
        .expect("run cargo metadata");
    assert!(
        result.status.success(),
        "cargo metadata failed:\n{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let metadata: Value = serde_json::from_slice(&result.stdout).unwrap();
    assert_eq!(
        Path::new(metadata["workspace_root"].as_str().unwrap()),
        workspace
    );
    let packages = metadata["packages"].as_array().unwrap();
    assert!(!packages.is_empty());
    // The pure facade is a workspace member, so every check below covers it too.
    assert!(
        packages
            .iter()
            .any(|package| package["name"] == "sfi-package"),
        "sfi-package must be inside the guarded workspace"
    );
    for package in packages {
        for dependency in package["dependencies"].as_array().unwrap() {
            if package["name"] == "sfi-core" {
                assert_ne!(
                    dependency["name"], "kiln-sfi",
                    "core cannot depend on the runtime plugin"
                );
            }
        }
    }
    let failures: Vec<_> = packages
        .iter()
        .filter_map(|package| {
            check_package(
                package["name"].as_str().unwrap(),
                package["license"].as_str(),
            )
            .err()
        })
        .collect();
    assert!(failures.is_empty(), "{}", failures.join("\n"));
    eprintln!(
        "licence guard: {} resolved packages checked",
        packages.len()
    );
}

fn check_sources(root: &Path) -> Vec<String> {
    let mut failures = Vec::new();
    for entry in fs::read_dir(root).unwrap() {
        let entry = entry.unwrap();
        let path = entry.path();
        let kind = entry.file_type().unwrap();
        if kind.is_dir() {
            failures.extend(check_sources(&path));
        } else if path.extension().is_some_and(|ext| ext == "rs") {
            let source = fs::read_to_string(&path).unwrap();
            for (line, text) in source.lines().enumerate() {
                if text.to_ascii_lowercase().contains(FORBIDDEN_CRATE) {
                    failures.push(format!(
                        "{}:{}: forbidden crate reference",
                        path.display(),
                        line + 1
                    ));
                }
            }
        }
    }
    failures
}

#[test]
fn rust_sources_do_not_reference_the_forbidden_crate() {
    let workspace = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
    let failures = check_sources(workspace);
    assert!(failures.is_empty(), "{}", failures.join("\n"));
}

#[test]
fn guard_rejects_forbidden_package_prefixes_and_all_gpl_variants() {
    for name in [
        format!("{FORBIDDEN_CRATE}-base"),
        "mod-loader-lua".into(),
        "shroudforge-api".into(),
        "eml-runtime".into(),
    ] {
        assert!(check_package(&name, Some("MIT")).is_err());
    }
    for licence in [
        "GPL-3.0-only",
        "LGPL-2.1-or-later",
        "AGPL-3.0-only",
        "MIT AND GPL-3.0-only",
        "GPL-3.0-only OR LGPL-2.1-or-later OR AGPL-3.0-only",
        "(MIT OR Apache-2.0) AND LGPL-2.1-or-later",
        "GPL-3.0-only OR (MIT AND LGPL-2.1-or-later)",
        "(GPL-3.0-only OR MIT) AND AGPL-3.0-only",
        "gpl-3.0",
    ] {
        assert!(check_package("test-dependency", Some(licence)).is_err());
    }
    assert!(check_package("test-dependency", None).is_err());
    assert!(check_package("sfi-core", Some("MIT")).is_ok());
    assert!(check_package("dependency", Some("MIT OR Apache-2.0")).is_ok());
}

#[test]
fn guard_selects_an_entirely_permissive_spdx_alternative() {
    for licence in [
        "MIT OR GPL-3.0-only",
        "MIT OR Apache-2.0 OR LGPL-2.1-or-later",
        "LGPL-2.1-or-later OR Apache-2.0",
        "AGPL-3.0-only OR (MIT AND BSD-3-Clause)",
        "(GPL-3.0-only OR MIT) AND Apache-2.0",
        "(MIT OR Apache-2.0) AND Unicode-3.0",
        "(MIT OR Apache-2.0) AND NCSA",
        "Apache-2.0 WITH LLVM-exception OR MIT",
        "Apache-2.0/MIT",
        "MIT/Apache-2.0",
        "0BSD OR MIT OR Apache-2.0",
        "ISC AND Zlib",
        "CC0-1.0 OR Unlicense",
    ] {
        assert!(
            check_package("dependency", Some(licence)).is_ok(),
            "{licence}"
        );
    }
    for licence in [
        "",
        "Proprietary",
        "MPL-2.0",
        "MIT AND Proprietary",
        "MIT OR",
        "MIT OR (GPL-3.0-only",
        "MIT GPL-3.0-only",
        "MIT WITH unknown-exception",
        "MIT OR GPL-3.0-only AND",
        "MIT OR Apache-2.0 WITH AND",
        "MIT OR ???",
        "MIT OR (Apache-2.0 WITH )",
    ] {
        assert!(
            check_package("dependency", Some(licence)).is_err(),
            "{licence}"
        );
    }
}
