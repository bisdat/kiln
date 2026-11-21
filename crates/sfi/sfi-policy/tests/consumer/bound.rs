// SPDX-License-Identifier: MIT
//! Must NOT compile in a default-feature consumer: the OS-bound evaluation and the
//! native-subsystem parser belong to kiln-sfi's resolver (feature `os-binding`).
use sfi_policy::{
    native_contracts::NativeContracts,
    pe_gate::{self, Image},
};

fn main() {
    let registry = NativeContracts::shipped().unwrap();
    let images: [Image<'_>; 0] = [];
    let _ = registry.evaluate_bound("a.dll", &images, &["kernel32.dll"]);
    let _ = pe_gate::parse_system(b"MZ");
}
