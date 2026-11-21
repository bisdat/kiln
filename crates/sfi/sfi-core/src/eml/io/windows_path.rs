// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
// Sanitizer source (both pins): modules/mod-runtime/lua/bindings/io/manage_exports.rs:217-239.
//! Windows lexical path grammar on every host. No filesystem lookup.
//! SF both pins: lua/bindings/io/path_names.rs:2-52 and manage_exports.rs:217-239.
use std::{io, path::PathBuf};

fn separator(b: u8, verbatim: bool) -> bool {
    b == b'\\' || (!verbatim && b == b'/')
}
#[derive(Clone, Copy)]
struct Component<'a> {
    text: &'a str,
    end: usize,
}
struct Parsed<'a> {
    prefix: usize,
    root: usize,
    verbatim: bool,
    parts: Vec<Component<'a>>,
}
fn parse(path: &str) -> Parsed<'_> {
    let b = path.as_bytes();
    let mut prefix = 0;
    let mut verbatim = false;
    if b.starts_with(b"\\\\?\\") {
        verbatim = true;
        if b.get(4..8) == Some(b"UNC\\") {
            let server = path[8..].find('\\').map(|n| n + 8);
            prefix = server
                .map(|s| {
                    path[s + 1..]
                        .find('\\')
                        .map(|n| s + 1 + n)
                        .unwrap_or(b.len())
                })
                .unwrap_or(b.len());
        } else if b.get(4).is_some_and(u8::is_ascii_alphabetic) && b.get(5) == Some(&b':') {
            prefix = 6;
        } else {
            prefix = path[4..].find('\\').map(|n| n + 4).unwrap_or(b.len());
        }
    } else if b.starts_with(b"\\\\.\\") {
        prefix = path[4..]
            .find(['\\', '/'])
            .map(|n| n + 4)
            .unwrap_or(b.len());
    } else if b.len() >= 2 && separator(b[0], false) && separator(b[1], false) {
        if let Some(server_end) = path[2..].find(['\\', '/']).map(|n| n + 2) {
            let share_end = path[server_end + 1..]
                .find(['\\', '/'])
                .map(|n| server_end + 1 + n)
                .unwrap_or(b.len());
            if server_end > 2 && share_end > server_end + 1 {
                prefix = share_end;
            }
        }
    } else if b.first().is_some_and(u8::is_ascii_alphabetic) && b.get(1) == Some(&b':') {
        prefix = 2;
    }
    let root = if b.get(prefix).is_some_and(|&c| separator(c, verbatim)) {
        prefix + 1
    } else {
        0
    };
    let mut parts = Vec::new();
    let first = prefix.max(root);
    let mut start = first;
    for end in first..=b.len() {
        if end == b.len() || separator(b[end], verbatim) {
            if end > start {
                let text = &path[start..end];
                if text != "." || verbatim || (parts.is_empty() && prefix == 0 && root == 0) {
                    parts.push(Component { text, end });
                }
            }
            start = end + 1;
        }
    }
    Parsed {
        prefix,
        root,
        verbatim,
        parts,
    }
}

pub(super) fn name(path: &str) -> &str {
    let p = parse(path);
    p.parts
        .last()
        .map(|x| x.text)
        .filter(|&s| s != "." && s != "..")
        .unwrap_or("")
}
pub(super) fn stem(path: &str) -> &str {
    let name = name(path);
    match name.rfind('.') {
        Some(i) if i > 0 => &name[..i],
        _ => name,
    }
}
pub(super) fn extension(path: &str) -> &str {
    let name = name(path);
    match name.rfind('.') {
        Some(i) if i > 0 => &name[i + 1..],
        _ => "",
    }
}
pub(super) fn parent(path: &str) -> String {
    let mut p = parse(path);
    if p.parts.pop().is_none() {
        return String::new();
    }
    let end = p
        .parts
        .last()
        .map(|c| c.end)
        .unwrap_or(p.prefix.max(p.root));
    path[..end].replace('\\', "/")
}
pub(super) fn join(parts: &[String]) -> String {
    let mut path = String::new();
    for part in parts {
        let p = parse(part);
        let base = parse(&path);
        if p.prefix > 0 {
            path = part.clone();
        } else if base.verbatim && !part.is_empty() {
            let mut normalized = Vec::new();
            if p.root == 0 {
                for c in &base.parts {
                    if c.text != "." {
                        normalized.push(c.text);
                    }
                }
            }
            for c in &p.parts {
                match c.text {
                    "." => {}
                    ".." => {
                        normalized.pop();
                    }
                    _ => normalized.push(c.text),
                }
            }
            let mut result = path[..base.prefix.max(base.root)].to_string();
            for c in normalized {
                if !result.ends_with('\\') {
                    result.push('\\');
                }
                result.push_str(c);
            }
            path = result;
        } else if p.root > 0 {
            path = format!("{}{}", &path[..base.prefix], part);
        } else {
            let need_sep = !path.is_empty()
                && !path
                    .as_bytes()
                    .last()
                    .is_some_and(|&c| separator(c, base.verbatim))
                && !(base.prefix == path.len() && path.ends_with(':'));
            if need_sep {
                path.push('\\');
            }
            path.push_str(part);
        }
    }
    path.replace('\\', "/")
}
pub(super) fn sanitize(path: &str) -> io::Result<PathBuf> {
    let p = parse(path);
    if p.prefix > 0 || p.root > 0 {
        return Err(io::ErrorKind::PermissionDenied.into());
    }
    let mut result = PathBuf::new();
    for c in p.parts {
        match c.text {
            "." => {}
            ".." => {
                if !result.pop() {
                    return Err(io::ErrorKind::PermissionDenied.into());
                }
            }
            s => result.push(s),
        }
    }
    Ok(result)
}
