// SPDX-License-Identifier: MIT
//! The registry proof after a game update (registry P2 slice 4; BRIEF section 6.1): what `kilnctl update` and
//! `kilnctl regproof` check before a regmap record says a Kiln field is mapped on a new game build.
pub mod formula;
pub mod ids;
pub mod refl;
pub mod regmap;
pub mod roundtrip;
pub mod values;
