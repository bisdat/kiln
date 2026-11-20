//! Vendor upscaler files the launcher downloads when the player asks for them (AMD FSR, Intel XeSS; later NVIDIA's
//! frame generation): docs/kiln/graphics.md ("Upscaler files"), the contract in docs/kiln/launcher/contracts.md
//! ("Vendor files").
//!
//! | module | what |
//! |---|---|
//! | [`manifest`] | the manifest Kiln ships (`launcher/src/data/vendor-components.json`): components, files, pins, the bundled licence texts, the URL rules |
//! | [`agreements`] | the licences the player agreed to (`<data>/state/vendor-licenses.json`), per vendor and text hash |
//! | [`store`] | a game folder's `kiln/vendor/<vendor>/<version>/` folders, their receipts, the page's status, removal |
//! | [`download`] | fetching a component's files through [`crate::net`], size and SHA-256 checks, resume, the receipt, updates |
//!
//! The owner's rules: nothing is downloaded until the player asks, on a screen that says what will happen; a
//! component with a file whose SHA-256 is not pinned is never downloaded; the files go under the game's `kiln/`
//! folder, never into the game's own folder; a dedicated server has none of this.

pub mod agreements;
pub mod download;
pub mod manifest;
pub mod store;

pub use agreements::Agreements;
pub use download::{download, DownloadReport};
pub use manifest::{builtin, Component, VendorManifest};
pub use store::{ComponentStatus, RemoveReport, Store, VendorState};
