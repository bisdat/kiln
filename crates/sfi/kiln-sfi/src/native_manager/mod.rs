// SPDX-License-Identifier: MIT
//! The interpreter's DLL manager.
//!
//! - [`selection`] (pure, in sfi-policy): which DLLs, in which order, and which packages are
//!   withdrawn before anything runs; the sealed `register_dll` table.
//! - [`plan`]: the frozen launch plan's native part as an ordered [`plan::NativePlan`].
//! - [`executor`]: loads that plan serially through the native-load enforcement, after the startup
//!   witness and before data commit, and reports exactly one terminal record per DLL.
//! - [`arming`]: whether the loader is armed at all this start, and its one thread.
//!
//! kiln-sfi's early phase (under backend `kiln`, right after the plan read) makes one
//! [`executor::NativeRun`] per process with Kiln's native.policy as the
//! kiln.launch record carries it: the frozen policy and `build_verified`
//! ([`crate::native_permission`]). [`crate::Boundary::native_permission`] reads the same answer
//! from `KilnApi.build_info()` at Kiln's plugin step and refuses the interpreter when the two
//! disagree. With kiln-eml's startup seam attached the run's witness is the seam's ([`arming`]). Dinput8 mode
//! stops at the missing kiln.proxy table. kiln-manage's producer (not written yet) is to apply
//! the same [`selection::NativePermission`] when it writes the plan. Boot wins when they disagree.
pub mod arming;
pub mod executor;
pub mod plan;
pub use sfi_policy::native_selection as selection;
