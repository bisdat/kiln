// SPDX-License-Identifier: GPL-3.0-or-later
//! The field path an error names: `.` at the root, else the dotted field names and zero-based indices on the way down.
//!
//! Adapted from ShroudForge 1.7.9 (MIT, Copyright (c) 2026 bonsaibauer; notice in sf_startup/mod.rs):
//! modules/mod-runtime/lua/resource_values/value_path.rs:1-54.
use std::fmt::{Display, Write};

pub struct TreePath {
    stack: Vec<String>,
    len: usize,
}

impl Default for TreePath {
    fn default() -> Self {
        Self::new()
    }
}

impl TreePath {
    #[inline]
    pub fn new() -> Self {
        Self {
            stack: vec![String::with_capacity(32); 16],
            len: 0,
        }
    }

    pub fn push(&mut self, name: &str) {
        if self.len == self.stack.len() {
            self.stack.push(String::with_capacity(32));
        }
        self.stack[self.len].clear();
        self.stack[self.len].push_str(name);
        self.len += 1;
    }

    pub fn push_index(&mut self, index: usize) {
        if self.len == self.stack.len() {
            self.stack.push(String::with_capacity(32));
        }
        self.stack[self.len].clear();
        // Writing to a String cannot fail.
        let _ = write!(self.stack[self.len], "{index}");
        self.len += 1;
    }

    pub fn pop(&mut self) {
        if self.len > 0 {
            self.len -= 1;
        }
    }
}

impl Display for TreePath {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.len == 0 {
            write!(f, ".")
        } else {
            write!(f, "{}", self.stack[..self.len].join("."))
        }
    }
}
