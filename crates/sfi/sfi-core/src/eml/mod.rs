// SPDX-License-Identifier: MIT
//! Game-independent SF EML bindings. Callers install each table with `lua::install_namespace`.
//! Resource codecs belong to the external adapter.
pub mod buffer;
pub mod guid;
pub mod hasher;
pub mod image;
pub mod integer;
pub mod io;
pub mod texture;
