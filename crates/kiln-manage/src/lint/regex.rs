//! A small backtracking matcher for the ECMA-262 `pattern`s in schema/*.schema.json (JSON Schema patterns are
//! unanchored searches). It covers what Kiln's schemas use: classes, \d \w \s and their negations, groups
//! (capturing, `(?:`, lookahead `(?=` / `(?!`, lookbehind `(?<=` / `(?<!` of any length), alternation, `* + ? {n} {n,} {n,m}` (lazy forms parse as greedy:
//! the same strings match), `^ $ .`. Unsupported syntax is an error, so a schema cannot silently mean something else.
//!
//! A repetition of one character (`.*`, `[a-z]+`, `\s{0,64}`) runs as a loop: no recursion per character, so a long
//! value is checked whole. A repetition of a group still recurses once per round, and some patterns take exponential
//! time (a nested quantifier, in a lookbehind too). Each match therefore runs under a budget of [`MAX_STEPS`] steps and
//! of stack, measured below where the match began (so it holds for any frame size, debug build or not): first
//! [`MAX_STACK`] on the caller's thread (a thread with 1 MiB of stack has room to spare); a match that needs more runs
//! once more on a thread of its own with [`DEEP_STACK`] (a repeated group over a long value). A match that exceeds its
//! budget is [`TooCostly`]: the caller says the value was not checked, never that it does not match.

#[derive(Debug, Clone)]
enum Re {
    Char(char),
    Any,
    Class(Vec<(char, char)>, bool),
    Start,
    End,
    Look(Box<Re>, bool),
    /// `(?<=x)` / `(?<!x)` (true = negative): some `x` ends exactly here.
    Behind(Box<Re>, bool),
    Concat(Vec<Re>),
    Alt(Vec<Re>),
    Repeat(Box<Re>, u32, Option<u32>),
}

#[derive(Debug, Clone)]
pub struct Regex {
    re: Re,
}

/// How much stack one match may use below where it began on the caller's thread (a repeated group recurses once per
/// round).
pub const MAX_STACK: usize = 256 * 1024;
/// The stack of the thread a match that needs more runs on; it may use all but 2 MiB of it.
pub const DEEP_STACK: usize = 64 * 1024 * 1024;
/// How many steps one match may take in all.
pub const MAX_STEPS: u32 = 1_000_000;

/// The match was given up: it would take more than [`MAX_STEPS`] steps, or more stack than [`DEEP_STACK`] allows.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TooCostly;

/// How one run of a match ended.
enum Run {
    Done(bool),
    /// Out of steps.
    Spent,
    /// Out of stack.
    Deep,
}

impl Run {
    fn result(self) -> Result<bool, TooCostly> {
        match self {
            Run::Done(found) => Ok(found),
            _ => Err(TooCostly),
        }
    }
}

/// The budget of one match.
struct Budget {
    /// The stack address where the match began (the stack grows down on every platform Kiln builds for).
    base: usize,
    /// How far below `base` it may go.
    stack: usize,
    /// It went that far (and may fit on [`DEEP_STACK`]).
    deep: std::cell::Cell<bool>,
    steps: std::cell::Cell<u32>,
    spent: std::cell::Cell<bool>,
}

impl Regex {
    pub fn new(pattern: &str) -> Result<Regex, String> {
        let chars: Vec<char> = pattern.chars().collect();
        let mut p = P { c: &chars, i: 0 };
        let re = p.alt()?;
        if p.i != chars.len() {
            return Err(format!("unexpected '{}' at {}", chars[p.i], p.i));
        }
        Ok(Regex { re })
    }

    /// Whether the pattern matches anywhere in `s` (JSON Schema `pattern` semantics), or [`TooCostly`] when the
    /// match ran out of its budget (then nothing is known about `s`).
    pub fn is_match(&self, s: &str) -> Result<bool, TooCostly> {
        let chars: Vec<char> = s.chars().collect();
        match self.run(&chars, MAX_STACK) {
            Run::Deep => std::thread::scope(|scope| {
                std::thread::Builder::new()
                    .name("kiln-lint-regex".into())
                    .stack_size(DEEP_STACK)
                    .spawn_scoped(scope, || self.run(&chars, DEEP_STACK - 2 * 1024 * 1024))
                    .ok()
                    .and_then(|t| t.join().ok())
            })
            .map_or(Err(TooCostly), Run::result),
            r => r.result(),
        }
    }

    fn run(&self, chars: &[char], stack: usize) -> Run {
        let b = Budget { base: stack_address(), stack, deep: false.into(), steps: 0.into(), spent: false.into() };
        let found = (0..=chars.len()).any(|i| !b.spent.get() && mat(&self.re, chars, i, &b, &mut |_| true));
        match (b.spent.get(), b.deep.get()) {
            (false, _) => Run::Done(found),
            (true, true) => Run::Deep,
            (true, false) => Run::Spent,
        }
    }
}

struct P<'a> {
    c: &'a [char],
    i: usize,
}

impl P<'_> {
    fn peek(&self) -> Option<char> {
        self.c.get(self.i).copied()
    }
    fn alt(&mut self) -> Result<Re, String> {
        let mut alts = vec![self.concat()?];
        while self.peek() == Some('|') {
            self.i += 1;
            alts.push(self.concat()?);
        }
        Ok(if alts.len() == 1 { alts.pop().unwrap() } else { Re::Alt(alts) })
    }
    fn concat(&mut self) -> Result<Re, String> {
        let mut items = Vec::new();
        while let Some(c) = self.peek() {
            if c == '|' || c == ')' {
                break;
            }
            let atom = self.atom()?;
            let atom = self.quantifier(atom)?;
            items.push(atom);
        }
        Ok(Re::Concat(items))
    }
    fn quantifier(&mut self, atom: Re) -> Result<Re, String> {
        let (min, max) = match self.peek() {
            Some('*') => (0, None),
            Some('+') => (1, None),
            Some('?') => (0, Some(1)),
            Some('{') => {
                let save = self.i;
                self.i += 1;
                let a = self.int();
                let (lo, hi) = match (a, self.peek()) {
                    (Some(a), Some('}')) => (a, Some(a)),
                    (Some(a), Some(',')) => {
                        self.i += 1;
                        let b = self.int();
                        if self.peek() != Some('}') {
                            return Err("bad {n,m}".into());
                        }
                        (a, b)
                    }
                    _ => {
                        // not a quantifier: a literal '{' (as in JS without the u flag)
                        self.i = save;
                        return Ok(atom);
                    }
                };
                if let Some(h) = hi
                    && h < lo
                {
                    return Err("{n,m} with m < n".into());
                }
                (lo, hi)
            }
            _ => return Ok(atom),
        };
        self.i += 1;
        if self.peek() == Some('?') {
            self.i += 1; // lazy: same set of matching strings
        }
        if matches!(atom, Re::Start | Re::End | Re::Look(..) | Re::Behind(..)) {
            return Err("quantifier after an assertion".into());
        }
        Ok(Re::Repeat(Box::new(atom), min, max))
    }
    fn int(&mut self) -> Option<u32> {
        let s = self.i;
        while self.peek().is_some_and(|c| c.is_ascii_digit()) {
            self.i += 1;
        }
        if s == self.i { None } else { self.c[s..self.i].iter().collect::<String>().parse().ok() }
    }
    fn atom(&mut self) -> Result<Re, String> {
        let c = self.peek().ok_or("unexpected end")?;
        self.i += 1;
        Ok(match c {
            '^' => Re::Start,
            '$' => Re::End,
            '.' => Re::Any,
            '(' => {
                let mut look = None;
                let mut behind = false;
                if self.peek() == Some('?') {
                    match (self.c.get(self.i + 1), self.c.get(self.i + 2)) {
                        (Some(':'), _) => self.i += 2,
                        (Some('='), _) => {
                            self.i += 2;
                            look = Some(false)
                        }
                        (Some('!'), _) => {
                            self.i += 2;
                            look = Some(true)
                        }
                        // `(?<=` / `(?<!`; `(?<name>` (a named group) stays unsupported
                        (Some('<'), Some(c @ ('=' | '!'))) => {
                            look = Some(*c == '!');
                            behind = true;
                            self.i += 3;
                        }
                        _ => return Err("unsupported group".into()),
                    }
                }
                let inner = self.alt()?;
                if self.peek() != Some(')') {
                    return Err("missing )".into());
                }
                self.i += 1;
                match look {
                    Some(neg) if behind => Re::Behind(Box::new(inner), neg),
                    Some(neg) => Re::Look(Box::new(inner), neg),
                    None => inner,
                }
            }
            '[' => self.class()?,
            '\\' => self.escape(false)?,
            '*' | '+' | '?' => return Err(format!("nothing to repeat before '{c}'")),
            ')' => return Err("unbalanced )".into()),
            c => Re::Char(c),
        })
    }
    fn escape(&mut self, in_class: bool) -> Result<Re, String> {
        let c = self.peek().ok_or("trailing \\")?;
        self.i += 1;
        let digit = vec![('0', '9')];
        let word = vec![('a', 'z'), ('A', 'Z'), ('0', '9'), ('_', '_')];
        let space = vec![(' ', ' '), ('\t', '\r'), ('\u{a0}', '\u{a0}'), ('\u{feff}', '\u{feff}'), ('\u{2028}', '\u{2029}'), ('\u{1680}', '\u{1680}'), ('\u{2000}', '\u{200a}'), ('\u{202f}', '\u{202f}'), ('\u{205f}', '\u{205f}'), ('\u{3000}', '\u{3000}')];
        Ok(match c {
            'd' => Re::Class(digit, false),
            'D' => Re::Class(digit, true),
            'w' => Re::Class(word, false),
            'W' => Re::Class(word, true),
            's' => Re::Class(space, false),
            'S' => Re::Class(space, true),
            'n' => Re::Char('\n'),
            'r' => Re::Char('\r'),
            't' => Re::Char('\t'),
            'f' => Re::Char('\u{c}'),
            'v' => Re::Char('\u{b}'),
            '0' => Re::Char('\0'),
            'b' if in_class => Re::Char('\u{8}'),
            'u' => {
                let h: String = self.c.get(self.i..self.i + 4).ok_or("bad \\u")?.iter().collect();
                self.i += 4;
                Re::Char(char::from_u32(u32::from_str_radix(&h, 16).map_err(|_| "bad \\u")?).ok_or("bad \\u")?)
            }
            c if c.is_ascii_alphanumeric() => return Err(format!("unsupported escape \\{c}")),
            c => Re::Char(c),
        })
    }
    fn class(&mut self) -> Result<Re, String> {
        let mut neg = false;
        if self.peek() == Some('^') {
            neg = true;
            self.i += 1;
        }
        let mut ranges: Vec<(char, char)> = Vec::new();
        let mut first = true;
        loop {
            let c = self.peek().ok_or("missing ]")?;
            if c == ']' && !first {
                self.i += 1;
                break;
            }
            first = false;
            self.i += 1;
            let lo = if c == '\\' {
                match self.escape(true)? {
                    Re::Char(x) => x,
                    Re::Class(r, false) => {
                        ranges.extend(r);
                        continue;
                    }
                    Re::Class(r, true) => {
                        // a negated set inside a class: add its complement
                        let mut prev = '\0';
                        let mut sorted = r.clone();
                        sorted.sort();
                        for (a, b) in sorted {
                            if a > prev {
                                ranges.push((prev, char::from_u32(a as u32 - 1).unwrap_or(prev)));
                            }
                            prev = char::from_u32(b as u32 + 1).unwrap_or(b);
                        }
                        ranges.push((prev, char::MAX));
                        continue;
                    }
                    _ => return Err("bad class escape".into()),
                }
            } else {
                c
            };
            if self.peek() == Some('-') && self.c.get(self.i + 1).is_some_and(|x| *x != ']') {
                self.i += 1;
                let h = self.peek().ok_or("missing ]")?;
                self.i += 1;
                let hi = if h == '\\' {
                    match self.escape(true)? {
                        Re::Char(x) => x,
                        _ => return Err("bad range".into()),
                    }
                } else {
                    h
                };
                if hi < lo {
                    return Err("range out of order".into());
                }
                ranges.push((lo, hi));
            } else {
                ranges.push((lo, lo));
            }
        }
        Ok(Re::Class(ranges, neg))
    }
}

fn in_class(ranges: &[(char, char)], c: char) -> bool {
    ranges.iter().any(|(a, b)| *a <= c && c <= *b)
}

/// One step under the budget: false (and the budget marked spent) once it is exceeded; every caller then unwinds.
fn mat(re: &Re, s: &[char], i: usize, b: &Budget, k: &mut dyn FnMut(usize) -> bool) -> bool {
    if b.spent.get() {
        return false;
    }
    let steps = b.steps.get() + 1;
    if steps > MAX_STEPS {
        b.spent.set(true);
        return false;
    }
    if b.base.saturating_sub(stack_address()) > b.stack {
        b.deep.set(true);
        b.spent.set(true);
        return false;
    }
    b.steps.set(steps);
    step(re, s, i, b, k)
}

/// An address in the caller's stack frame.
#[inline(never)]
fn stack_address() -> usize {
    let here = 0u8;
    std::hint::black_box(&here) as *const u8 as usize
}

fn step(re: &Re, s: &[char], i: usize, b: &Budget, k: &mut dyn FnMut(usize) -> bool) -> bool {
    match re {
        Re::Char(c) => i < s.len() && s[i] == *c && k(i + 1),
        Re::Any => i < s.len() && !matches!(s[i], '\n' | '\r' | '\u{2028}' | '\u{2029}') && k(i + 1),
        Re::Class(r, neg) => i < s.len() && in_class(r, s[i]) != *neg && k(i + 1),
        Re::Start => i == 0 && k(i),
        Re::End => i == s.len() && k(i),
        Re::Look(inner, neg) => {
            let found = mat(inner, s, i, b, &mut |_| true);
            found != *neg && k(i)
        }
        // ECMA-262 matches a lookbehind backwards; for whether it holds, that is: some start j <= i where the inner
        // pattern matches exactly s[j..i]
        Re::Behind(inner, neg) => {
            let found = (0..=i).rev().any(|j| !b.spent.get() && mat(inner, s, j, b, &mut |end| end == i));
            found != *neg && k(i)
        }
        Re::Concat(items) => seq(items, s, i, b, k),
        Re::Alt(alts) => alts.iter().any(|a| mat(a, s, i, b, k)),
        Re::Repeat(inner, min, max) if one_char(inner) => {
            // greedy, as the recursive form tries it (longest first), as a loop
            let limit = max.map_or(usize::MAX, |m| m as usize);
            let mut run = 0;
            // each character looked at is a step of the budget: an unanchored `.*x` over a long value is quadratic
            while run < limit && i + run < s.len() && charge(b) && step(inner, s, i + run, b, &mut |_| true) {
                run += 1;
            }
            if b.spent.get() || run < *min as usize {
                return false;
            }
            (*min as usize..=run).rev().take_while(|_| charge(b)).any(|n| k(i + n)) && !b.spent.get()
        }
        Re::Repeat(inner, min, max) => rep(inner, *min, *max, 0, s, i, b, k),
    }
}

/// A pattern that takes exactly one character and calls nothing else: its repetition can run as a loop.
fn one_char(re: &Re) -> bool {
    matches!(re, Re::Char(_) | Re::Any | Re::Class(..))
}

/// One step of a loop under the budget (false once it is spent).
fn charge(b: &Budget) -> bool {
    let steps = b.steps.get() + 1;
    if b.spent.get() || steps > MAX_STEPS {
        b.spent.set(true);
        return false;
    }
    b.steps.set(steps);
    true
}

fn seq(items: &[Re], s: &[char], i: usize, b: &Budget, k: &mut dyn FnMut(usize) -> bool) -> bool {
    match items.split_first() {
        None => k(i),
        Some((first, rest)) => mat(first, s, i, b, &mut |j| seq(rest, s, j, b, k)),
    }
}

#[allow(clippy::too_many_arguments)]
fn rep(inner: &Re, min: u32, max: Option<u32>, n: u32, s: &[char], i: usize, b: &Budget, k: &mut dyn FnMut(usize) -> bool) -> bool {
    if max.is_none_or(|m| n < m) && mat(inner, s, i, b, &mut |j| (j != i || n < min) && rep(inner, min, max, n + 1, s, j, b, k)) {
        return true;
    }
    !b.spent.get() && n >= min && k(i)
}

#[cfg(test)]
mod tests {
    use super::Regex;

    fn m(p: &str, s: &str) -> bool {
        Regex::new(p).unwrap().is_match(s).unwrap()
    }

    #[test]
    fn basics() {
        assert!(m("^[a-z0-9][a-z0-9._-]{0,63}$", "my-mod"));
        assert!(!m("^[a-z0-9][a-z0-9._-]{0,63}$", "My-mod"));
        assert!(!m("^[a-z0-9][a-z0-9._-]{0,63}$", &"a".repeat(65)));
        assert!(m("[*?]", "Material_*"));
        assert!(!m("[*?]", "Material"));
        assert!(m("\\.[pP][nN][gG]$", "icons/x.PNG"));
        assert!(m("^(\\*|[A-Za-z0-9][A-Za-z0-9_-]{1,11})$", "pt-BR"));
        assert!(m("^(\\*|[A-Za-z0-9][A-Za-z0-9_-]{1,11})$", "*"));
        assert!(!m("^(\\*|[A-Za-z0-9][A-Za-z0-9_-]{1,11})$", "-x"));
        assert!(m("^item:[^.]+(\\..+)?$", "item:Weapon_X.perkReferences[1]"));
        assert!(!m("^item:[^.]+(\\..+)?$", "item:"));
        assert!(m("^0[xX][0-9a-fA-F]{1,8}$", "0xcdbe9c80"));
        assert!(m("a{2}b", "xaab"));
        assert!(m("a{", "a{"));
    }

    #[test]
    fn lookahead() {
        // common.schema.json relPath / gamePath
        let rel = "^(?![\\s\\S]*(^|/)\\.\\.(/|$))(?!/)[^\\\\:*?\"<>|]+$";
        assert!(m(rel, "kiln/isle-icons"));
        assert!(!m(rel, "../x"));
        assert!(!m(rel, "a/../x"));
        assert!(!m(rel, "/abs"));
        assert!(m(rel, "a/..b/c"));
        let file = "^(?![\\s\\S]*(^|[\\\\/])\\.\\.([\\\\/]|$))(?![\\\\/])(?![A-Za-z]:)[\\s\\S]+$";
        assert!(m(file, "cooked/x.bin"));
        assert!(!m(file, "C:/x.bin"));
        assert!(!m(file, "a\\..\\b"));
        assert!(m("^(?!kiln\\.)", "mymod.Ping"));
        assert!(!m("^(?!kiln\\.)", "kiln.Log"));
    }

    #[test]
    fn lookbehind() {
        // kiln-publish.schema.json /github/tag: no '/' at the end
        let tag = "^(?!/)(?!.*//)(?!.*\\.\\.)[A-Za-z0-9._/-]{0,64}\\{version\\}[A-Za-z0-9._/-]{0,64}(?<!/)$";
        assert!(m(tag, "v{version}"));
        assert!(m(tag, "mods/hello/v{version}"));
        assert!(!m(tag, "v{version}/"));
        assert!(!m(tag, "/v{version}"));
        assert!(!m(tag, "a//v{version}"));
        assert!(m("(?<=a)b", "ab") && !m("(?<=a)b", "cb"));
        assert!(m("(?<!a)b", "cb") && !m("(?<!a)b", "ab"));
        assert!(m("^x(?<=^x)$", "x"));
        // a lookbehind of any length, with alternatives
        assert!(m("(?<=foo|ba)r", "bar") && m("(?<=foo|ba)r", "foor") && !m("(?<=foo|ba)r", "xr"));
        assert!(m("(?<![0-9]{2})z", "1z") && !m("(?<![0-9]{2})z", "12z"));
        assert!(Regex::new("(?<=a)+").is_err());
    }

    /// Every `pattern` (and `patternProperties` key) of schema/*.schema.json is one kiln lint can run: an unsupported
    /// one would make lint refuse every value of that field (KL109) while ajv and the launcher accept it.
    #[test]
    fn every_schema_pattern_is_supported() {
        fn walk(v: &serde_json::Value, out: &mut Vec<String>) {
            match v {
                serde_json::Value::Object(o) => {
                    for (k, x) in o {
                        if k == "pattern" && let Some(p) = x.as_str() { out.push(p.to_string()); }
                        if k == "patternProperties" && let Some(pp) = x.as_object() { out.extend(pp.keys().cloned()); }
                        walk(x, out);
                    }
                }
                serde_json::Value::Array(a) => a.iter().for_each(|x| walk(x, out)),
                _ => {}
            }
        }
        let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../schema");
        let mut patterns = Vec::new();
        for e in std::fs::read_dir(&dir).unwrap().flatten() {
            let p = e.path();
            if p.to_string_lossy().ends_with(".schema.json") {
                let v: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&p).unwrap()).unwrap();
                walk(&v, &mut patterns);
            }
        }
        assert!(patterns.len() > 50, "{}", patterns.len());
        let bad: Vec<String> = patterns.iter().filter_map(|p| Regex::new(p).err().map(|e| format!("{p}: {e}"))).collect();
        assert!(bad.is_empty(), "{}", bad.join("\n"));
    }

    /// The cases that once aborted the process (a stack overflow) or ran for seconds. Each runs in a child process
    /// (this test binary, this test, `KILN_REGEX_CHILD=<case>`) on a test thread's 2 MiB stack, so an abort shows as the
    /// child's exit status, not as a crash of this run.
    #[test]
    fn long_and_costly_matches_end_in_a_subprocess() {
        const TAG: &str = "^(?!/)(?!.*//)(?!.*\\.\\.)[A-Za-z0-9._/-]{0,64}\\{version\\}[A-Za-z0-9._/-]{0,64}(?<!/)$";
        if let Ok(case) = std::env::var("KILN_REGEX_CHILD") {
            let (pattern, text, want) = match case.as_str() {
                // the shipped kiln-publish tag pattern on 100,000 'a' + "cb": it recursed once per character and
                // overflowed the stack; now its one-character repetitions loop, and the value is checked whole
                "long-input" => (TAG, format!("{}cb", "a".repeat(100_000)), Ok(false)),
                // a repeated group still recurses once per round: the stack bound ends it, the process lives
                "deep-group" => ("^(ab)*$", "ab".repeat(400_000), Err(super::TooCostly)),
                // a nested quantifier in a lookbehind: exponential backtracking, ended by the step budget
                "exponential-lookbehind" => ("(?<=(a+)+)b", format!("{}cb", "a".repeat(24)), Err(super::TooCostly)),
                _ => panic!("unknown case {case}"),
            };
            let started = std::time::Instant::now();
            let r = Regex::new(pattern).unwrap().is_match(&text);
            println!("regex-child: {case} {r:?} in {} ms", started.elapsed().as_millis());
            assert_eq!(r, want, "{case}");
            return;
        }
        for (case, want) in [("long-input", "Ok(false)"), ("deep-group", "Err(TooCostly)"), ("exponential-lookbehind", "Err(TooCostly)")] {
            let out = std::process::Command::new(std::env::current_exe().unwrap())
                .args(["--exact", "lint::regex::tests::long_and_costly_matches_end_in_a_subprocess", "--nocapture", "--test-threads=1"])
                .env("KILN_REGEX_CHILD", case)
                .output()
                .unwrap();
            let (stdout, stderr) = (String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
            assert!(out.status.success(), "{case}: the child ended with {:?}\n{stdout}\n{stderr}", out.status);
            assert!(stdout.contains(&format!("regex-child: {case} {want}")), "{case}: {stdout}");
        }
        // the same pattern on a value of an ordinary size is checked, either way
        assert!(m(TAG, "mods/hello/v{version}"));
        assert!(!m(TAG, &format!("{}v{{version}}", "a".repeat(65))));
        assert!(!m("(?<=(a+)+)b", "cb"));
    }

    #[test]
    fn the_budget_is_deterministic_and_reset_per_match() {
        let re = Regex::new("(?<=(a+)+)b").unwrap();
        let long = format!("{}cb", "a".repeat(24));
        for _ in 0..3 {
            assert_eq!(re.is_match(&long), Err(super::TooCostly));
        }
        // a spent budget belongs to that match only
        assert_eq!(re.is_match("aab"), Ok(true));
        // one-character repetitions loop: a long value is checked whole, with no depth
        assert_eq!(Regex::new("^[a-z]+$").unwrap().is_match(&"a".repeat(100_000)), Ok(true));
        assert_eq!(Regex::new("^[a-z]{0,64}$").unwrap().is_match(&"a".repeat(65)), Ok(false));
        assert_eq!(Regex::new("^a*ab$").unwrap().is_match(&format!("{}b", "a".repeat(10_000))), Ok(true));
        // a repeated group within the bound is checked; past it, it is too costly (never a guess)
        assert_eq!(Regex::new("^(ab)*$").unwrap().is_match(&"ab".repeat(100)), Ok(true));
        assert_eq!(Regex::new("^(ab)*$").unwrap().is_match(&"ab".repeat(400_000)), Err(super::TooCostly));
        // past the caller's stack bound, within the deep thread's: checked there
        assert_eq!(Regex::new("^(ab)*$").unwrap().is_match(&"ab".repeat(5_000)), Ok(true));
    }

    #[test]
    fn rejects_unknown_syntax() {
        assert!(Regex::new("(?<n>x)").is_err());
        assert!(Regex::new("\\p{L}").is_err());
        assert!(Regex::new("*a").is_err());
    }
}
