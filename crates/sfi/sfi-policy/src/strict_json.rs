// SPDX-License-Identifier: MIT
//! Bounded JSON with duplicate-key rejection before serde_json can overwrite a
//! restriction. Protocols here contain canonical nonnegative integers, not floats.
use serde_json::{Map, Value};
use std::collections::BTreeSet;

pub(crate) fn parse(bytes: &[u8], limit: usize) -> Result<Value, String> {
    if bytes.is_empty() || bytes.len() > limit {
        return Err("empty or oversized JSON".into());
    }
    let mut scanner = Scanner { bytes, at: 0 };
    scanner.value(0)?;
    scanner.space();
    if scanner.at != bytes.len() {
        return Err("trailing JSON content".into());
    }
    serde_json::from_slice(bytes).map_err(|e| format!("invalid JSON: {e}"))
}
struct Scanner<'a> {
    bytes: &'a [u8],
    at: usize,
}
impl Scanner<'_> {
    fn space(&mut self) {
        while self.bytes.get(self.at).is_some_and(u8::is_ascii_whitespace) {
            self.at += 1;
        }
    }
    fn take(&mut self, b: u8) -> Result<(), String> {
        self.space();
        if self.bytes.get(self.at) != Some(&b) {
            return Err("unexpected JSON token".into());
        }
        self.at += 1;
        Ok(())
    }
    fn string(&mut self) -> Result<String, String> {
        self.space();
        let start = self.at;
        self.take(b'"')?;
        loop {
            match self.bytes.get(self.at) {
                None => return Err("truncated JSON string".into()),
                Some(b'"') => {
                    self.at += 1;
                    break;
                }
                Some(b'\\') => {
                    self.at += 2;
                }
                Some(_) => self.at += 1,
            }
            if self.at > self.bytes.len() {
                return Err("truncated JSON escape".into());
            }
        }
        serde_json::from_slice(&self.bytes[start..self.at])
            .map_err(|e| format!("invalid JSON string: {e}"))
    }
    fn value(&mut self, depth: usize) -> Result<(), String> {
        if depth > 64 {
            return Err("JSON depth exceeds 64".into());
        }
        self.space();
        match self.bytes.get(self.at) {
            Some(b'{') => {
                self.at += 1;
                self.space();
                let mut keys = BTreeSet::new();
                if self.bytes.get(self.at) == Some(&b'}') {
                    self.at += 1;
                    return Ok(());
                }
                loop {
                    let key = self.string()?;
                    if !keys.insert(key) {
                        return Err("duplicate JSON object key".into());
                    }
                    self.take(b':')?;
                    self.value(depth + 1)?;
                    self.space();
                    if self.bytes.get(self.at) == Some(&b'}') {
                        self.at += 1;
                        break;
                    }
                    self.take(b',')?;
                }
            }
            Some(b'[') => {
                self.at += 1;
                self.space();
                if self.bytes.get(self.at) == Some(&b']') {
                    self.at += 1;
                    return Ok(());
                }
                loop {
                    self.value(depth + 1)?;
                    self.space();
                    if self.bytes.get(self.at) == Some(&b']') {
                        self.at += 1;
                        break;
                    }
                    self.take(b',')?;
                }
            }
            Some(b'"') => {
                self.string()?;
            }
            Some(_) => {
                let start = self.at;
                while self
                    .bytes
                    .get(self.at)
                    .is_some_and(|b| !b.is_ascii_whitespace() && !b",]}".contains(b))
                {
                    self.at += 1;
                }
                let token = &self.bytes[start..self.at];
                if token.is_empty() {
                    return Err("missing JSON value".into());
                }
                if ![b"true".as_slice(), b"false", b"null"].contains(&token)
                    && (!token.iter().all(u8::is_ascii_digit)
                        || (token.len() > 1 && token[0] == b'0')
                        || std::str::from_utf8(token)
                            .ok()
                            .and_then(|s| s.parse::<u64>().ok())
                            .is_none())
                {
                    return Err("noncanonical or out-of-range integer".into());
                }
            }
            None => return Err("truncated JSON".into()),
        }
        Ok(())
    }
}

pub(crate) fn object<'a>(v: &'a Value, fields: &[&str]) -> Result<&'a Map<String, Value>, String> {
    let object = v.as_object().ok_or("expected JSON object")?;
    if object.len() != fields.len() || fields.iter().any(|key| !object.contains_key(*key)) {
        return Err(format!(
            "missing or unknown fields; expected {}",
            fields.join(",")
        ));
    }
    Ok(object)
}
pub(crate) fn text(v: &Value) -> Result<&str, String> {
    let s = v.as_str().ok_or("expected string")?;
    if s.is_empty() || s.contains('\0') {
        return Err("empty or NUL-containing string".into());
    }
    Ok(s)
}
pub(crate) fn hash(v: &Value) -> Result<&str, String> {
    let s = text(v)?;
    if s.len() != 64
        || !s
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
    {
        return Err("invalid lowercase SHA-256".into());
    }
    Ok(s)
}
