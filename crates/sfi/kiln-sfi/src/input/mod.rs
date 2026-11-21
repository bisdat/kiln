// SPDX-License-Identifier: MIT
//! The input feed behind ShroudForge's `shroudforge.input` (the base of its key and keybind functions).
//!
//! - [`feed`]: the window-thread handoff from kiln.ui's window input records, the key state (the
//!   ShroudForge fork's host feed, ported) and `was_keybind_pressed`'s press memory.
//! - [`label`]: a keybind setting's chords and its label (`keybind_label`).

pub mod feed;
pub mod label;

pub use feed::{
    Desktop, FeedConfig, FeedCounters, FeedError, InputFeed, KeyFeed, PressTracker, Record,
    StopOutcome, SystemDesktop,
};
pub use label::{Binding, Chord, KeyNames, LabelTable, Trigger};
