// SPDX-License-Identifier: MIT
//! Strict ordered JSON with an immutable byte source and duplicate-preserving token spans.
//! SF both pins: modules/modloader/read_mod_manifest.rs:9-24; serde_json preserve_order, kept
//! here by [`Value`] (document order without the feature).
use crate::json::Value;
use std::ops::Range;

#[derive(Debug, Clone)]
pub struct Document {
    text: String,
    value: Value,
    pub root: Node,
}
impl Document {
    /// No launcher file-size policy here: SF's strict reader has no 2 MiB bound.
    pub fn parse(bytes: &[u8]) -> Result<Self, String> {
        let text = std::str::from_utf8(bytes)
            .map_err(|e| format!("strict JSON requires UTF-8: {e}"))?
            .to_owned();
        let value =
            serde_json::from_str(&text).map_err(|e| format!("strict JSON is required: {e}"))?;
        // Tokenization only runs after serde has checked the complete document, including
        // every duplicate, string escape, number, delimiter and recursion limit.
        let root = Scanner { text: &text, at: 0 }.node();
        Ok(Self { text, value, root })
    }
    /// A package file as SF's reader reads it: `serde_json::from_reader`, whose errors
    /// (column, invalid-UTF-8 text) differ from `from_str`, reported as `<file>: <error>`
    /// (read_mod_manifest.rs:9-24, read_mod_locale_catalogs.rs:45-50, both pins).
    pub fn read(name: &str, bytes: &[u8]) -> Result<Self, String> {
        let value: Value = serde_json::from_reader(bytes)
            .map_err(|e| format!("{name}: {}", pinned_reader_error(bytes, &e)))?;
        // from_reader validates every string as UTF-8 and allows only JSON syntax and
        // whitespace around it, so an accepted document is UTF-8 text.
        let text = String::from_utf8(bytes.to_vec()).map_err(|e| format!("{name}: {e}"))?;
        let root = Scanner { text: &text, at: 0 }.node();
        Ok(Self { text, value, root })
    }
    pub fn bounded(bytes: &[u8]) -> Result<Self, String> {
        if bytes.len() > super::MAX_FILE_BYTES {
            return Err("file is larger than 2 MiB".into());
        }
        Self::parse(bytes)
    }
    pub fn bytes(&self) -> &[u8] {
        self.text.as_bytes()
    }
    pub fn text(&self) -> &str {
        &self.text
    }
    pub fn value(&self) -> &Value {
        &self.value
    }
}
/// Both pins lock serde_json 1.0.151, whose reader reports an invalid UTF-8 string at
/// the closing quote it has just consumed (read.rs:869 as_str after IoRead::parse_str);
/// later serde_json (1.0.154 here) points at the first invalid byte instead. The failing
/// string holds the document's first invalid byte (every earlier byte parsed), and its
/// closing quote ends it (strings hold no raw newline), so the pinned position is that
/// quote's line and 1-based byte column.
pub(crate) fn pinned_reader_error(bytes: &[u8], error: &serde_json::Error) -> String {
    let text = error.to_string();
    if !text.starts_with("invalid unicode code point at line ") {
        return text;
    }
    let Err(utf8) = std::str::from_utf8(bytes) else {
        return text;
    };
    let mut at = utf8.valid_up_to();
    while let Some(&byte) = bytes.get(at) {
        match byte {
            b'\\' => at += 2,
            b'"' => break,
            _ => at += 1,
        }
    }
    if at >= bytes.len() {
        return text;
    }
    let line = 1 + bytes[..at].iter().filter(|&&b| b == b'\n').count();
    let start = bytes[..at]
        .iter()
        .rposition(|&b| b == b'\n')
        .map_or(0, |newline| newline + 1);
    format!(
        "invalid unicode code point at line {line} column {}",
        at - start + 1
    )
}
#[derive(Debug, Clone)]
pub struct Node {
    pub span: Range<usize>,
    pub members: Vec<(String, Node)>,
    pub elements: Vec<Node>,
}
impl Node {
    pub fn get(&self, key: &str) -> Option<&Self> {
        self.members
            .iter()
            .rev()
            .find(|(k, _)| k == key)
            .map(|(_, v)| v)
    }
    pub fn raw<'a>(&self, text: &'a str) -> &'a str {
        &text[self.span.clone()]
    }
}
struct Scanner<'a> {
    text: &'a str,
    at: usize,
}
impl Scanner<'_> {
    fn skip(&mut self) {
        while self
            .text
            .as_bytes()
            .get(self.at)
            .is_some_and(u8::is_ascii_whitespace)
        {
            self.at += 1;
        }
    }
    fn string(&mut self) -> String {
        let start = self.at;
        self.at += 1;
        loop {
            match self.text.as_bytes()[self.at] {
                b'\\' => self.at += 2,
                b'"' => {
                    self.at += 1;
                    break;
                }
                _ => self.at += 1,
            }
        }
        serde_json::from_str(&self.text[start..self.at]).expect("validated string token")
    }
    fn node(&mut self) -> Node {
        self.skip();
        let start = self.at;
        let mut node = Node {
            span: start..start,
            members: vec![],
            elements: vec![],
        };
        match self.text.as_bytes()[self.at] {
            b'{' => {
                self.at += 1;
                self.skip();
                while self.text.as_bytes()[self.at] != b'}' {
                    let key = self.string();
                    self.skip();
                    self.at += 1;
                    node.members.push((key, self.node()));
                    self.skip();
                    if self.text.as_bytes()[self.at] != b',' {
                        break;
                    }
                    self.at += 1;
                    self.skip();
                }
                self.at += 1;
            }
            b'[' => {
                self.at += 1;
                self.skip();
                while self.text.as_bytes()[self.at] != b']' {
                    node.elements.push(self.node());
                    self.skip();
                    if self.text.as_bytes()[self.at] != b',' {
                        break;
                    }
                    self.at += 1;
                }
                self.at += 1;
            }
            b'"' => {
                self.string();
            }
            _ => {
                while self
                    .text
                    .as_bytes()
                    .get(self.at)
                    .is_some_and(|b| !b.is_ascii_whitespace() && !b",]}".contains(b))
                {
                    self.at += 1;
                }
            }
        }
        node.span.end = self.at;
        node
    }
}

#[cfg(test)]
mod tests {
    use super::Document;
    // Expected texts are serde_json 1.0.151's own from_reader output for these bytes
    // (a scratch build against the pins' locked version, 2026-11-19).
    #[test]
    fn reader_errors_keep_the_pinned_serde_json_positions() {
        for (bytes, expected) in [
            (
                b"{\"id\":\"example\",\"name\":\"\xff\",\"version\":\"1.0.0\"}".as_slice(),
                "invalid unicode code point at line 1 column 26",
            ),
            (
                b"{\n  \"a\": \"x\\\"\xff y\"\n}",
                "invalid unicode code point at line 2 column 15",
            ),
            (
                b"{\n\n   \"k\xe2\x82\": 1}",
                "invalid unicode code point at line 3 column 8",
            ),
            (
                b"[\"ok\", \"\xc3\xa9\", \"\\u00e9\xed\xa0\x80z\"]",
                "invalid unicode code point at line 1 column 25",
            ),
            (
                b"{\"a\":\"\xff\x01\"}",
                "control character (\\u0000-\\u001F) found while parsing a string at line 1 column 8",
            ),
            (
                b"{\"a\":\"\xff",
                "EOF while parsing a string at line 1 column 7",
            ),
            (
                b"\"\xf0\x9f\x98\"",
                "invalid unicode code point at line 1 column 5",
            ),
        ] {
            assert_eq!(
                Document::read("f.json", bytes).unwrap_err(),
                format!("f.json: {expected}"),
                "{}",
                String::from_utf8_lossy(bytes)
            );
        }
        let doc = Document::read("f.json", "{\"a\":\"\u{e9}\"}".as_bytes()).unwrap();
        assert_eq!(doc.value()["a"], "\u{e9}");
    }
}
