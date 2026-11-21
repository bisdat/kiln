// SPDX-License-Identifier: MIT
//! A finite, read-only reference scanner for ShroudForge package Lua. It tokenizes the source (strings, long
//! strings and comments are recognised, so a name inside them is never a reference), follows member chains rooted at
//! free API globals (`a.b`, `a["b"]`, `a:b`), `local` aliases of such chains and literal `require` calls, and
//! records what it cannot follow: computed keys, shadowed or reassigned roots, metatable and raw access, dynamic
//! `require`/`load`. It never executes a chunk and never claims a whole-program proof: every result is evidence for a
//! warning, nothing here can refuse a package.
//!
//! Scope is approximated per file and forward only: a name declared `local` anywhere (a `local` statement, a
//! parameter, a `for` variable) counts as local from that token on. That errs towards "uncertain", never towards a
//! false reference to the API.
use std::collections::{BTreeMap, BTreeSet};

/// Per-package budgets: past them the scan stops and says so (a warning, never a refusal).
pub const MAX_TOKENS: usize = 1_000_000;
pub const MAX_FINDINGS: usize = 4096;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Tok {
    Name(String),
    Keyword(&'static str),
    /// A string literal; `None` when an escape could not be decoded to UTF-8 text.
    Str(Option<String>),
    Number,
    Op(&'static str),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Token {
    pub tok: Tok,
    pub line: u32,
}

const KEYWORDS: [&str; 22] = [
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
    "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while",
];
const OPS3: [&str; 1] = ["..."];
const OPS2: [&str; 9] = ["..", "==", "~=", "<=", ">=", "<<", ">>", "//", "::"];
const OPS1: [&str; 23] = [
    "+", "-", "*", "/", "%", "^", "#", "&", "~", "|", "<", ">", "=", "(", ")", "{", "}", "[", "]",
    ";", ":", ",", ".",
];

/// Tokenizes Lua 5.4 source. An unterminated string or comment, or a byte Lua does not accept outside a string, is an
/// error with its line: the caller reports the file as not scanned.
pub fn tokenize(src: &str, budget: &mut usize) -> Result<Vec<Token>, String> {
    let b = src.as_bytes();
    let mut i = 0usize;
    let mut line = 1u32;
    let mut out = Vec::new();
    // A first line starting with '#' is skipped by Lua's loader.
    if b.first() == Some(&b'#') {
        while i < b.len() && b[i] != b'\n' {
            i += 1;
        }
    }
    while i < b.len() {
        let c = b[i];
        if c == b'\n' {
            line += 1;
            i += 1;
            continue;
        }
        if c.is_ascii_whitespace() {
            i += 1;
            continue;
        }
        if *budget == 0 {
            return Err("token budget exceeded".into());
        }
        if c == b'-' && b.get(i + 1) == Some(&b'-') {
            i += 2;
            if let Some(level) = long_open(b, i) {
                let start = line;
                let (end, lines) = long_close(b, i + level + 2, level)
                    .ok_or_else(|| format!("line {start}: unterminated long comment"))?;
                line += lines;
                i = end;
            } else {
                while i < b.len() && b[i] != b'\n' {
                    i += 1;
                }
            }
            continue;
        }
        let at = line;
        let tok = if c == b'"' || c == b'\'' {
            let (value, end, lines) = short_string(b, i).map_err(|e| format!("line {at}: {e}"))?;
            line += lines;
            i = end;
            Tok::Str(value)
        } else if c == b'[' && long_open(b, i).is_some() {
            let level = long_open(b, i).unwrap_or(0);
            let mut body = i + level + 2;
            // A newline right after the opening bracket is not part of the string.
            let mut skipped = 0;
            if b.get(body) == Some(&b'\r') {
                body += 1;
            }
            if b.get(body) == Some(&b'\n') {
                body += 1;
                skipped = 1;
            }
            let (end, lines) = long_close(b, body, level)
                .ok_or_else(|| format!("line {at}: unterminated long string"))?;
            let text = std::str::from_utf8(&b[body..end - level - 2])
                .ok()
                .map(str::to_owned);
            line += lines + skipped;
            i = end;
            Tok::Str(text)
        } else if c.is_ascii_alphabetic() || c == b'_' {
            let start = i;
            while i < b.len() && (b[i].is_ascii_alphanumeric() || b[i] == b'_') {
                i += 1;
            }
            let word = &src[start..i];
            match KEYWORDS.iter().find(|k| **k == word) {
                Some(k) => Tok::Keyword(k),
                None => Tok::Name(word.to_owned()),
            }
        } else if c.is_ascii_digit() || (c == b'.' && b.get(i + 1).is_some_and(u8::is_ascii_digit))
        {
            let hex = c == b'0' && matches!(b.get(i + 1), Some(b'x' | b'X'));
            i += if hex { 2 } else { 1 };
            while i < b.len() {
                let d = b[i];
                let exp = if hex {
                    matches!(d, b'p' | b'P')
                } else {
                    matches!(d, b'e' | b'E')
                };
                if exp && matches!(b.get(i + 1), Some(b'+' | b'-')) {
                    i += 2;
                } else if d.is_ascii_alphanumeric() || d == b'.' || d == b'_' {
                    i += 1;
                } else {
                    break;
                }
            }
            Tok::Number
        } else if let Some(op) = OPS3
            .iter()
            .chain(OPS2.iter())
            .chain(OPS1.iter())
            .find(|op| b[i..].starts_with(op.as_bytes()))
        {
            i += op.len();
            Tok::Op(op)
        } else {
            return Err(format!("line {at}: unexpected character"));
        };
        *budget -= 1;
        out.push(Token { tok, line: at });
    }
    Ok(out)
}

/// `[` `=`* `[` at `i`: the level.
fn long_open(b: &[u8], i: usize) -> Option<usize> {
    if b.get(i) != Some(&b'[') {
        return None;
    }
    let mut j = i + 1;
    while b.get(j) == Some(&b'=') {
        j += 1;
    }
    (b.get(j) == Some(&b'[')).then_some(j - i - 1)
}
/// From `i`, the index just past the matching `]` `=`*level `]`, and the newlines crossed.
fn long_close(b: &[u8], mut i: usize, level: usize) -> Option<(usize, u32)> {
    let mut lines = 0;
    while i < b.len() {
        if b[i] == b'\n' {
            lines += 1;
        } else if b[i] == b']'
            && b.len() >= i + level + 2
            && b[i + 1..i + 1 + level].iter().all(|&c| c == b'=')
            && b[i + 1 + level] == b']'
        {
            return Some((i + level + 2, lines));
        }
        i += 1;
    }
    None
}
/// A quoted string from its opening quote: (decoded text, index past the closing quote, newlines crossed).
fn short_string(b: &[u8], start: usize) -> Result<(Option<String>, usize, u32), String> {
    let quote = b[start];
    let mut i = start + 1;
    let mut out: Vec<u8> = Vec::new();
    let mut lines = 0;
    loop {
        let Some(&c) = b.get(i) else {
            return Err("unterminated string".into());
        };
        match c {
            b'\n' | b'\r' => return Err("unterminated string".into()),
            _ if c == quote => break,
            b'\\' => {
                i += 1;
                let Some(&e) = b.get(i) else {
                    return Err("unterminated string".into());
                };
                i += 1;
                match e {
                    b'n' => out.push(b'\n'),
                    b't' => out.push(b'\t'),
                    b'r' => out.push(b'\r'),
                    b'a' => out.push(7),
                    b'b' => out.push(8),
                    b'f' => out.push(12),
                    b'v' => out.push(11),
                    b'\\' | b'"' | b'\'' => out.push(e),
                    b'\n' => {
                        lines += 1;
                        out.push(b'\n');
                        if b.get(i) == Some(&b'\r') {
                            i += 1;
                        }
                    }
                    b'\r' => {
                        lines += 1;
                        out.push(b'\n');
                        if b.get(i) == Some(&b'\n') {
                            i += 1;
                        }
                    }
                    b'z' => {
                        while let Some(&w) = b.get(i) {
                            if w == b'\n' {
                                lines += 1;
                            } else if !w.is_ascii_whitespace() {
                                break;
                            }
                            i += 1;
                        }
                    }
                    b'x' => {
                        let hex = b.get(i..i + 2).and_then(|h| std::str::from_utf8(h).ok());
                        let v = hex
                            .and_then(|h| u8::from_str_radix(h, 16).ok())
                            .ok_or("invalid \\x escape")?;
                        out.push(v);
                        i += 2;
                    }
                    b'u' => {
                        if b.get(i) != Some(&b'{') {
                            return Err("invalid \\u escape".into());
                        }
                        let close = b[i..]
                            .iter()
                            .position(|&c| c == b'}')
                            .ok_or("invalid \\u escape")?
                            + i;
                        let v = std::str::from_utf8(&b[i + 1..close])
                            .ok()
                            .and_then(|h| u32::from_str_radix(h, 16).ok())
                            .ok_or("invalid \\u escape")?;
                        match char::from_u32(v) {
                            Some(ch) => out.extend_from_slice(ch.to_string().as_bytes()),
                            // Lua accepts code points up to 2^31 as raw UTF-8-like bytes; not text for us.
                            None => out.extend_from_slice(&[0xff]),
                        }
                        i = close + 1;
                    }
                    d if d.is_ascii_digit() => {
                        let mut v = u32::from(d - b'0');
                        for _ in 0..2 {
                            match b.get(i) {
                                Some(&n) if n.is_ascii_digit() => {
                                    v = v * 10 + u32::from(n - b'0');
                                    i += 1;
                                }
                                _ => break,
                            }
                        }
                        out.push(u8::try_from(v).map_err(|_| "decimal escape too large")?);
                    }
                    _ => return Err("invalid escape sequence".into()),
                }
            }
            _ => {
                out.push(c);
                i += 1;
            }
        }
    }
    Ok((String::from_utf8(out).ok(), i + 1, lines))
}

/// What the scanner could not follow. All of them are warning-only evidence.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, serde::Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum Uncertain {
    /// `api[expr]`, or `_G`/`_ENV` (the global table itself).
    Computed,
    /// A `local`, parameter or loop variable reuses an API root's name.
    Shadowed,
    /// An API root, one of its members or an alias of one is assigned.
    Reassigned,
    /// setmetatable/getmetatable/rawget/rawset/rawequal/debug.
    Metatable,
    /// `require` with anything but one string literal, or `require` used as a value.
    DynamicRequire,
    /// load/loadstring/loadfile/dofile.
    DynamicLoad,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Reference {
    /// Dotted chain from the root, aliases resolved (`shroudforge.ui.add`).
    pub chain: String,
    /// A trailing `:name` method call on the chain, not part of `chain`.
    pub method: Option<String>,
    pub line: u32,
    /// The local alias the chain went through.
    pub via: Option<String>,
    /// The first argument, when the chain is called with one string literal first.
    pub literal_arg: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Finding {
    pub class: Uncertain,
    pub line: u32,
    pub detail: String,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct FileScan {
    pub references: Vec<Reference>,
    pub findings: Vec<Finding>,
    /// Literal `require` arguments with their lines.
    pub requires: Vec<(String, u32)>,
}

const META: [&str; 6] = [
    "setmetatable",
    "getmetatable",
    "rawget",
    "rawset",
    "rawequal",
    "debug",
];
const LOADERS: [&str; 4] = ["load", "loadstring", "loadfile", "dofile"];
const GLOBAL_TABLES: [&str; 2] = ["_G", "_ENV"];

fn is_op(t: Option<&Token>, op: &str) -> bool {
    matches!(t, Some(Token { tok: Tok::Op(o), .. }) if *o == op)
}
fn is_kw(t: Option<&Token>, kw: &str) -> bool {
    matches!(t, Some(Token { tok: Tok::Keyword(k), .. }) if *k == kw)
}
fn name_of(t: Option<&Token>) -> Option<&str> {
    match t {
        Some(Token {
            tok: Tok::Name(n), ..
        }) => Some(n),
        _ => None,
    }
}

/// Scans one tokenized file. `roots` are the free global names whose member chains are API references.
pub fn scan(tokens: &[Token], roots: &BTreeSet<String>) -> FileScan {
    let mut out = FileScan::default();
    let follows = assignment_follows(tokens);
    let mut locals: BTreeSet<String> = BTreeSet::new();
    let mut aliases: BTreeMap<String, String> = BTreeMap::new();
    // Open brackets, to tell a table-constructor key (`{ name = 1 }`) from an assignment.
    let mut brackets: Vec<&'static str> = Vec::new();
    let mut i = 0;
    let push = |out: &mut FileScan, class, line, detail: String| {
        if out.findings.len() < MAX_FINDINGS {
            out.findings.push(Finding {
                class,
                line,
                detail,
            });
        }
    };
    while i < tokens.len() {
        let t = &tokens[i];
        match &t.tok {
            Tok::Op(o @ ("(" | "{" | "[")) => {
                brackets.push(o);
                i += 1;
                continue;
            }
            Tok::Op(")" | "}" | "]") => {
                brackets.pop();
                i += 1;
                continue;
            }
            Tok::Keyword("local") => {
                if is_kw(tokens.get(i + 1), "function") {
                    if let Some(name) = name_of(tokens.get(i + 2)) {
                        declare(
                            &mut out,
                            &mut locals,
                            &mut aliases,
                            roots,
                            name,
                            tokens[i + 2].line,
                            push,
                        );
                    }
                    i += 3;
                    continue;
                }
                // local a <const>, b = ...
                let mut names = Vec::new();
                let mut j = i + 1;
                while let Some(name) = name_of(tokens.get(j)) {
                    names.push((name.to_owned(), tokens[j].line));
                    j += 1;
                    if is_op(tokens.get(j), "<") {
                        j += 3; // <attrib>
                    }
                    if is_op(tokens.get(j), ",") {
                        j += 1;
                    } else {
                        break;
                    }
                }
                // An alias: `local a = <chain>` that ends the expression.
                let alias = if names.len() == 1 && is_op(tokens.get(j), "=") {
                    chain_at(tokens, j + 1, &locals, &aliases, roots)
                        .filter(|c| c.pure_end && c.method.is_none())
                } else {
                    None
                };
                for (name, line) in &names {
                    declare(
                        &mut out,
                        &mut locals,
                        &mut aliases,
                        roots,
                        name,
                        *line,
                        push,
                    );
                }
                if let Some(c) = alias {
                    aliases.insert(names[0].0.clone(), c.chain);
                }
                i = j;
                continue;
            }
            Tok::Keyword("function") => {
                // function a.b:c(params) / function(params): skip the name, declare the parameters.
                let mut j = i + 1;
                while j < tokens.len() && !is_op(tokens.get(j), "(") {
                    j += 1;
                }
                j += 1;
                while let Some(tok) = tokens.get(j) {
                    match &tok.tok {
                        Tok::Name(n) => declare(
                            &mut out,
                            &mut locals,
                            &mut aliases,
                            roots,
                            n,
                            tok.line,
                            push,
                        ),
                        Tok::Op(")") => break,
                        _ => {}
                    }
                    j += 1;
                }
                // The name part of `function shroudforge.x()` assigns into the API table.
                if let Some(root) = name_of(tokens.get(i + 1))
                    && roots.contains(root)
                    && !locals.contains(root)
                {
                    push(
                        &mut out,
                        Uncertain::Reassigned,
                        t.line,
                        format!("function {root}... assigns into an API table"),
                    );
                }
                i = j + 1;
                continue;
            }
            Tok::Keyword("for") => {
                let mut j = i + 1;
                while let Some(tok) = tokens.get(j) {
                    match &tok.tok {
                        Tok::Name(n) => declare(
                            &mut out,
                            &mut locals,
                            &mut aliases,
                            roots,
                            n,
                            tok.line,
                            push,
                        ),
                        Tok::Op(",") => {}
                        _ => break,
                    }
                    j += 1;
                }
                i = j;
                continue;
            }
            Tok::Name(name) => {
                let member = is_op(i.checked_sub(1).and_then(|p| tokens.get(p)), ".")
                    || is_op(i.checked_sub(1).and_then(|p| tokens.get(p)), ":")
                    || is_op(i.checked_sub(1).and_then(|p| tokens.get(p)), "::")
                    || is_kw(i.checked_sub(1).and_then(|p| tokens.get(p)), "goto");
                if member {
                    i += 1;
                    continue;
                }
                let key = brackets.last() == Some(&"{")
                    && is_op(tokens.get(i + 1), "=")
                    && matches!(
                        i.checked_sub(1).and_then(|p| tokens.get(p)).map(|t| &t.tok),
                        Some(Tok::Op("{" | "," | ";"))
                    );
                if key {
                    i += 1;
                    continue;
                }
                let free = !locals.contains(name.as_str());
                if free && name == "require" {
                    match (
                        tokens.get(i + 1).map(|t| &t.tok),
                        tokens.get(i + 2).map(|t| &t.tok),
                    ) {
                        (Some(Tok::Str(Some(s))), _) => out.requires.push((s.clone(), t.line)),
                        (Some(Tok::Op("(")), Some(Tok::Str(Some(s))))
                            if is_op(tokens.get(i + 3), ")") =>
                        {
                            out.requires.push((s.clone(), t.line))
                        }
                        _ => push(
                            &mut out,
                            Uncertain::DynamicRequire,
                            t.line,
                            "require with a computed argument".into(),
                        ),
                    }
                    i += 1;
                    continue;
                }
                if free && META.contains(&name.as_str()) {
                    push(
                        &mut out,
                        Uncertain::Metatable,
                        t.line,
                        format!("{name} can change what API names resolve to"),
                    );
                    i += 1;
                    continue;
                }
                if free && LOADERS.contains(&name.as_str()) {
                    push(
                        &mut out,
                        Uncertain::DynamicLoad,
                        t.line,
                        format!("{name} runs code the scan cannot see"),
                    );
                    i += 1;
                    continue;
                }
                if free && GLOBAL_TABLES.contains(&name.as_str()) {
                    push(
                        &mut out,
                        Uncertain::Computed,
                        t.line,
                        format!("{name} reaches globals by computed name"),
                    );
                    i += 1;
                    continue;
                }
                let Some(c) = chain_at(tokens, i, &locals, &aliases, roots) else {
                    i += 1;
                    continue;
                };
                let assigned = is_op(tokens.get(c.end), "=")
                    || (is_op(tokens.get(c.end), ",") && follows[c.end]);
                if assigned {
                    let what = c.via.as_deref().unwrap_or(&c.chain);
                    push(
                        &mut out,
                        Uncertain::Reassigned,
                        t.line,
                        format!("{what} is assigned"),
                    );
                    if c.via.is_some() && c.depth == 0 {
                        aliases.remove(name.as_str());
                    }
                } else if let Some(at) = &c.computed_at {
                    push(
                        &mut out,
                        Uncertain::Computed,
                        t.line,
                        format!("{at}[...] is indexed with a computed key"),
                    );
                } else {
                    let literal_arg = match (
                        tokens.get(c.end).map(|t| &t.tok),
                        tokens.get(c.end + 1).map(|t| &t.tok),
                    ) {
                        (Some(Tok::Str(Some(s))), _) => Some(s.clone()),
                        (Some(Tok::Op("(")), Some(Tok::Str(Some(s)))) => Some(s.clone()),
                        _ => None,
                    };
                    out.references.push(Reference {
                        chain: c.chain,
                        method: c.method,
                        line: t.line,
                        via: c.via,
                        literal_arg,
                    });
                }
                i = c.end;
                continue;
            }
            _ => {}
        }
        i += 1;
    }
    out
}

/// `a, b.c = ...`: a comma after a target belongs to an assignment when an `=` follows at the same bracket depth
/// before the statement ends (a `;`, a keyword or a closing bracket of an enclosing level). For every token index, the
/// answer of that forward scan, computed in one backward pass (a scan from each comma would be quadratic on a long
/// comma run). A bracket group is skipped whole; an unclosed one runs to the end of the file, which ends no statement.
fn assignment_follows(tokens: &[Token]) -> Vec<bool> {
    let mut closer = vec![usize::MAX; tokens.len()];
    let mut open: Vec<usize> = Vec::new();
    for (j, t) in tokens.iter().enumerate() {
        match &t.tok {
            Tok::Op("(" | "{" | "[") => open.push(j),
            Tok::Op(")" | "}" | "]") => {
                if let Some(o) = open.pop() {
                    closer[o] = j;
                }
            }
            _ => {}
        }
    }
    let mut follows = vec![false; tokens.len() + 1];
    for j in (0..tokens.len()).rev() {
        follows[j] = match &tokens[j].tok {
            Tok::Op("=") => true,
            Tok::Op(";" | ")" | "}" | "]") | Tok::Keyword(_) => false,
            Tok::Op("(" | "{" | "[") => closer[j] != usize::MAX && follows[closer[j] + 1],
            _ => follows[j + 1],
        };
    }
    follows
}

#[allow(clippy::too_many_arguments)]
fn declare(
    out: &mut FileScan,
    locals: &mut BTreeSet<String>,
    aliases: &mut BTreeMap<String, String>,
    roots: &BTreeSet<String>,
    name: &str,
    line: u32,
    push: impl Fn(&mut FileScan, Uncertain, u32, String),
) {
    if roots.contains(name) || ["require"].contains(&name) {
        push(
            out,
            Uncertain::Shadowed,
            line,
            format!("local {name} hides the API global"),
        );
    }
    aliases.remove(name);
    locals.insert(name.to_owned());
}

struct Chain {
    chain: String,
    method: Option<String>,
    via: Option<String>,
    /// Token index just past the chain.
    end: usize,
    /// Member steps taken after the base name.
    depth: usize,
    /// The chain prefix that was indexed with a computed key (the chain stops there).
    computed_at: Option<String>,
    /// Nothing that continues the expression follows (an alias ends here).
    pure_end: bool,
}

fn chain_at(
    tokens: &[Token],
    i: usize,
    locals: &BTreeSet<String>,
    aliases: &BTreeMap<String, String>,
    roots: &BTreeSet<String>,
) -> Option<Chain> {
    let base = name_of(tokens.get(i))?;
    let (mut chain, via) = if let Some(target) = aliases.get(base) {
        (target.clone(), Some(base.to_owned()))
    } else if roots.contains(base) && !locals.contains(base) {
        (base.to_owned(), None)
    } else {
        return None;
    };
    let mut j = i + 1;
    let mut depth = 0;
    let mut method = None;
    let mut computed_at = None;
    loop {
        if is_op(tokens.get(j), ".") {
            let Some(n) = name_of(tokens.get(j + 1)) else {
                break;
            };
            chain.push('.');
            chain.push_str(n);
            j += 2;
            depth += 1;
        } else if is_op(tokens.get(j), "[") {
            match (
                tokens.get(j + 1).map(|t| &t.tok),
                is_op(tokens.get(j + 2), "]"),
            ) {
                (Some(Tok::Str(Some(s))), true) if !s.is_empty() && !s.contains('.') => {
                    chain.push('.');
                    chain.push_str(s);
                    j += 3;
                    depth += 1;
                }
                _ => {
                    computed_at = Some(chain.clone());
                    break;
                }
            }
        } else if is_op(tokens.get(j), ":") {
            if let Some(n) = name_of(tokens.get(j + 1)) {
                method = Some(n.to_owned());
                j += 2;
            }
            break;
        } else {
            break;
        }
    }
    let pure_end = computed_at.is_none()
        && !matches!(
            tokens.get(j).map(|t| &t.tok),
            Some(Tok::Op(
                "(" | "["
                    | "."
                    | ":"
                    | ".."
                    | "+"
                    | "-"
                    | "*"
                    | "/"
                    | "//"
                    | "%"
                    | "^"
                    | "=="
                    | "~="
                    | "<"
                    | ">"
                    | "<="
                    | ">="
                    | "&"
                    | "|"
                    | "~"
                    | "<<"
                    | ">>"
            )) | Some(Tok::Str(_))
                | Some(Tok::Keyword("and" | "or"))
        );
    Some(Chain {
        chain,
        method,
        via,
        end: j,
        depth,
        computed_at,
        pure_end,
    })
}
