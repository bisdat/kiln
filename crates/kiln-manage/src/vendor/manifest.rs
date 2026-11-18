//! The manifest of vendor components (`launcher/src/data/vendor-components.json`, `schema/vendor-components.schema.json`)
//! and the vendor licence texts bundled next to it.
//!
//! [`builtin`] is the one Kiln ships; [`parse`] reads any text with the same rules, which go beyond the schema:
//! - ids unique, at most one component per vendor and version (they share the folder `kiln/vendor/<vendor>/<version>/`);
//! - file names plain and unique within a component;
//! - every URL a tag-pinned file on the vendor's own GitHub organisation ([`check_url`]), the tag being the version
//!   or `v<version>`;
//! - sizes from 1 byte to [`MAX_FILE`];
//! - the licence text bundled ([`license_text`]) and its SHA-256 the manifest's ([`license_sha256`]);
//! - a sha256 is 64 lower-case hex digits or [`NOT_PINNED`].
//!
//! A component with any file not pinned is not available in this build: [`Component::pinned`] is false and nothing
//! downloads it.

use crate::net::policy::{parse_url, Url};
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use std::collections::BTreeSet;

/// The format this build reads.
pub const FORMAT: u32 = 1;
/// The sha256 of a file the release process has not pinned yet.
pub const NOT_PINNED: &str = "not-pinned";
/// The largest file a manifest may name (bytes).
pub const MAX_FILE: u64 = 512 * 1024 * 1024;

/// The manifest Kiln ships (`launcher/src/data/vendor-components.json`).
pub const BUILTIN: &str = include_str!("../../../../launcher/src/data/vendor-components.json");

/// The licence texts Kiln ships, by their path relative to the manifest. Every file in
/// `launcher/src/data/vendor-licenses/` is here, and every manifest licence names one (tested).
pub const LICENSE_TEXTS: &[(&str, &str)] = &[
    ("vendor-licenses/amd-fidelityfx-sdk-v2.4.0-mit.txt", include_str!("../../../../launcher/src/data/vendor-licenses/amd-fidelityfx-sdk-v2.4.0-mit.txt")),
    ("vendor-licenses/intel-xess-v3.0.2-license.txt", include_str!("../../../../launcher/src/data/vendor-licenses/intel-xess-v3.0.2-license.txt")),
];

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct VendorManifest {
    pub format: u32,
    pub components: Vec<Component>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Component {
    /// `amd-fsr`, `intel-xess`, later `nvidia-dlssg`.
    pub id: String,
    /// `amd`, `intel` or `nvidia`: the folder `kiln/vendor/<vendor>/` and the key of the licence agreements.
    pub vendor: String,
    /// The vendor as the page names it (AMD).
    pub vendor_name: String,
    /// The display name (AMD FSR).
    pub name: String,
    /// What it enables, one plain sentence.
    pub enables: String,
    /// The vendor's version (the folder `kiln/vendor/<vendor>/<version>/`).
    pub version: String,
    /// The vendor's release page: shown, opened in the browser, never fetched.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub source: Option<String>,
    pub license: License,
    pub files: Vec<VendorFile>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct License {
    /// The vendor's own name for it (MIT licence; Intel Simplified Software License (October 2022)).
    pub name: String,
    /// The bundled text, relative to the manifest (`vendor-licenses/<name>.txt`).
    pub file: String,
    /// SHA-256 of the bundled text with LF line ends ([`license_sha256`]).
    pub sha256: String,
    /// Where the text is in the vendor's repository.
    pub from: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct VendorFile {
    pub name: String,
    pub url: String,
    pub size: u64,
    /// 64 lower-case hex digits, or [`NOT_PINNED`].
    pub sha256: String,
    /// The git blob SHA-1 at the tag (for the pinning tool's cross-check).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub git_blob: Option<String>,
}

impl VendorFile {
    pub fn pinned(&self) -> bool {
        is_sha256(&self.sha256)
    }
}

impl Component {
    /// Every file has its SHA-256: the component can be downloaded.
    pub fn pinned(&self) -> bool {
        self.files.iter().all(VendorFile::pinned)
    }

    /// Bytes of every file together.
    pub fn size(&self) -> u64 {
        self.files.iter().map(|f| f.size).sum()
    }

    /// The hosts the files come from, in manifest order, each once.
    pub fn hosts(&self) -> Vec<String> {
        let mut out: Vec<String> = Vec::new();
        for f in &self.files {
            if let Ok(u) = parse_url(&f.url)
                && !out.contains(&u.host)
            {
                out.push(u.host);
            }
        }
        out
    }

    pub fn file(&self, name: &str) -> Option<&VendorFile> {
        self.files.iter().find(|f| f.name == name)
    }
}

impl VendorManifest {
    pub fn component(&self, id: &str) -> Option<&Component> {
        self.components.iter().find(|c| c.id == id)
    }
}

/// The manifest Kiln ships, parsed and checked.
pub fn builtin() -> Result<VendorManifest> {
    parse(BUILTIN)
}

/// The bundled text of a licence file named in a manifest (None: not bundled).
pub fn license_text(file: &str) -> Option<&'static str> {
    LICENSE_TEXTS.iter().find(|(n, _)| *n == file).map(|(_, t)| *t)
}

/// SHA-256 of a licence text as the manifest and the agreements record it: UTF-8 with LF line ends (a CR before a LF
/// is dropped, so a checkout that turned the file into CRLF hashes the same).
pub fn license_sha256(text: &str) -> String {
    crate::hash::sha256_hex(text.replace("\r\n", "\n").as_bytes())
}

/// The GitHub organisations a vendor's files may come from (code, not data: a manifest cannot add one).
pub fn vendor_orgs(vendor: &str) -> &'static [&'static str] {
    match vendor {
        "amd" => &["GPUOpen-LibrariesAndSDKs"],
        "intel" => &["intel"],
        "nvidia" => &["NVIDIA", "NVIDIA-RTX"],
        _ => &[],
    }
}

/// The hosts a vendor file may be fetched from, and where each may redirect once (GitHub's documented download hosts):
/// - `raw.githubusercontent.com`: a file at a tag, served directly (no redirect);
/// - `media.githubusercontent.com`: a Git LFS file at a tag (`/media/<org>/<repo>/<tag>/<path>`), served directly;
/// - `github.com`: a release asset (`/<org>/<repo>/releases/download/<tag>/<asset>`), which GitHub answers with one
///   redirect to its asset storage, `release-assets.githubusercontent.com` (since 2025) or
///   `objects.githubusercontent.com` (before).
pub const HOSTS: [(&str, &[&str]); 3] = [
    ("raw.githubusercontent.com", &[]),
    ("media.githubusercontent.com", &[]),
    ("github.com", &crate::updates::github::ASSET_HOSTS),
];

/// The hosts one redirect of a request to `host` may go to (empty: none).
pub fn redirect_hosts(host: &str) -> &'static [&'static str] {
    HOSTS.iter().find(|(h, _)| *h == host).map(|(_, r)| *r).unwrap_or(&[])
}

fn is_sha256(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn bad(why: impl Into<String>) -> Error {
    Error::new(ErrorCode::Schema, format!("The vendor manifest is not valid: {}.", why.into()))
}

fn plain_id(s: &str, max: usize) -> bool {
    !s.is_empty() && s.len() <= max && s.bytes().next().is_some_and(|b| b.is_ascii_lowercase() || b.is_ascii_digit())
        && s.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
}

pub(crate) fn version_ok(v: &str) -> bool {
    let parts: Vec<&str> = v.split('.').collect();
    (2..=4).contains(&parts.len())
        && parts.iter().all(|p| !p.is_empty() && p.len() <= 9 && p.bytes().all(|b| b.is_ascii_digit()) && (p.len() == 1 || !p.starts_with('0')))
}

/// A file name in a version folder: letters, digits, `.`, `_`, `-`, not starting or ending with a dot, at most 128
/// bytes, not a Windows device name (`nul.dll`), and not one of Kiln's own names there. The store also reads the
/// names of a receipt with it, so a receipt on disk cannot point outside its folder.
pub fn file_name_ok(n: &str) -> bool {
    let stem = n.split('.').next().unwrap_or("").to_ascii_lowercase();
    let device = matches!(stem.as_str(), "con" | "prn" | "aux" | "nul")
        || (stem.len() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem.as_bytes()[3].is_ascii_digit());
    !n.is_empty() && n.len() <= 128
        && n.bytes().next().is_some_and(|b| b.is_ascii_alphanumeric())
        && !n.ends_with('.')
        && n.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'_' || b == b'-')
        && !device
        && !n.eq_ignore_ascii_case(super::store::RECEIPT)
        && !n.to_ascii_lowercase().ends_with(super::store::PART_SUFFIX)
        && !n.to_ascii_lowercase().ends_with(".tmp")
}

/// Checks one file URL of a component: https on one of [`HOSTS`], the vendor's own organisation, a tag named after the
/// component's version (`<version>` or `v<version>`), and a path to a file. Returns the parsed URL.
pub fn check_url(url: &str, vendor: &str, version: &str) -> Result<Url> {
    let u = parse_url(url).map_err(|e| bad(format!("{url}: {}", e.message)))?;
    if u.path_and_query.contains('?') {
        return Err(bad(format!("{url} has a query")));
    }
    let segs: Vec<&str> = u.path_and_query.trim_start_matches('/').split('/').collect();
    if segs.iter().any(|s| s.is_empty() || *s == "." || *s == ".." || s.contains('%')) {
        return Err(bad(format!("{url} has an empty, relative or escaped path segment")));
    }
    // (org, repo, tag, rest of the path)
    let (org, tag, rest) = match u.host.as_str() {
        "raw.githubusercontent.com" if segs.len() >= 4 => (segs[0], segs[2], &segs[3..]),
        "media.githubusercontent.com" if segs.len() >= 5 && segs[0] == "media" => (segs[1], segs[3], &segs[4..]),
        "github.com" if segs.len() == 6 && segs[2] == "releases" && segs[3] == "download" => (segs[0], segs[4], &segs[5..]),
        _ => return Err(bad(format!("{url} is not a raw file, an LFS file or a release asset on GitHub"))),
    };
    if !vendor_orgs(vendor).iter().any(|o| o.eq_ignore_ascii_case(org)) {
        return Err(bad(format!("{url} is not on {vendor}'s own GitHub organisation")));
    }
    if tag != version && tag != format!("v{version}") {
        return Err(bad(format!("{url} is not pinned to the tag of version {version}")));
    }
    if rest.is_empty() {
        return Err(bad(format!("{url} names no file")));
    }
    Ok(u)
}

/// Parses a manifest and checks every rule of the module.
pub fn parse(text: &str) -> Result<VendorManifest> {
    #[derive(Deserialize)]
    struct Head {
        format: u32,
    }
    let head: Head = serde_json::from_str(text).map_err(|e| Error::new(ErrorCode::Parse, format!("The vendor manifest is not JSON Kiln can read: {e}.")))?;
    if head.format > FORMAT {
        return Err(Error::new(ErrorCode::Unsupported, format!("The vendor manifest has format {}; this Kiln reads format {FORMAT}.", head.format)));
    }
    let m: VendorManifest = serde_json::from_str(text).map_err(|e| Error::new(ErrorCode::Parse, format!("The vendor manifest is not JSON Kiln can read: {e}.")))?;
    let mut ids = BTreeSet::new();
    let mut folders = BTreeSet::new();
    for c in &m.components {
        if !plain_id(&c.id, 32) {
            return Err(bad(format!("{:?} is not a component id", c.id)));
        }
        if !ids.insert(c.id.clone()) {
            return Err(bad(format!("{} is listed twice", c.id)));
        }
        if vendor_orgs(&c.vendor).is_empty() {
            return Err(bad(format!("{}: {:?} is not a vendor Kiln knows", c.id, c.vendor)));
        }
        if !version_ok(&c.version) {
            return Err(bad(format!("{}: {:?} is not a version", c.id, c.version)));
        }
        if !folders.insert((c.vendor.clone(), c.version.clone())) {
            return Err(bad(format!("{} shares kiln/vendor/{}/{}/ with another component", c.id, c.vendor, c.version)));
        }
        for (what, v) in [("vendorName", &c.vendor_name), ("name", &c.name), ("enables", &c.enables), ("license.name", &c.license.name)] {
            if v.trim().is_empty() {
                return Err(bad(format!("{}: {what} is empty", c.id)));
            }
        }
        if c.vendor != "nvidia" && c.name.to_ascii_lowercase().contains("dlss") {
            return Err(bad(format!("{}: only NVIDIA's own files may be called DLSS", c.id)));
        }
        if let Some(s) = &c.source
            && !s.starts_with("https://github.com/")
        {
            return Err(bad(format!("{}: the source page is not on GitHub", c.id)));
        }
        if !c.license.from.starts_with("https://github.com/") {
            return Err(bad(format!("{}: the licence's origin is not on GitHub", c.id)));
        }
        let text = license_text(&c.license.file).ok_or_else(|| bad(format!("{}: the licence text {} is not bundled", c.id, c.license.file)))?;
        if license_sha256(text) != c.license.sha256 {
            return Err(bad(format!("{}: the licence text {} does not have the manifest's SHA-256", c.id, c.license.file)));
        }
        if c.files.is_empty() {
            return Err(bad(format!("{} has no files", c.id)));
        }
        let mut names = BTreeSet::new();
        for f in &c.files {
            if !file_name_ok(&f.name) {
                return Err(bad(format!("{}: {:?} is not a plain file name", c.id, f.name)));
            }
            if !names.insert(f.name.to_ascii_lowercase()) {
                return Err(bad(format!("{}: {} is listed twice", c.id, f.name)));
            }
            check_url(&f.url, &c.vendor, &c.version).map_err(|e| Error::new(e.code, format!("{} ({}) {}", c.id, f.name, e.message)))?;
            if f.size == 0 || f.size > MAX_FILE {
                return Err(bad(format!("{}: {} has a size outside 1 byte to 512 MiB", c.id, f.name)));
            }
            if f.sha256 != NOT_PINNED && !is_sha256(&f.sha256) {
                return Err(bad(format!("{}: the sha256 of {} is neither 64 lower-case hex digits nor \"{NOT_PINNED}\"", c.id, f.name)));
            }
            if let Some(b) = &f.git_blob
                && !(b.len() == 40 && b.bytes().all(|x| x.is_ascii_digit() || (b'a'..=b'f').contains(&x)))
            {
                return Err(bad(format!("{}: the gitBlob of {} is not 40 lower-case hex digits", c.id, f.name)));
            }
        }
    }
    Ok(m)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn one(patch: impl FnOnce(&mut serde_json::Value)) -> Result<VendorManifest> {
        let mut v: serde_json::Value = serde_json::from_str(BUILTIN).unwrap();
        patch(&mut v);
        parse(&v.to_string())
    }

    #[test]
    fn the_builtin_manifest_parses_and_pins_every_file() {
        let m = builtin().unwrap();
        assert_eq!(m.components.iter().map(|c| c.id.as_str()).collect::<Vec<_>>(), ["amd-fsr", "intel-xess"]);
        let amd = m.component("amd-fsr").unwrap();
        assert_eq!((amd.vendor.as_str(), amd.version.as_str(), amd.license.name.as_str()), ("amd", "2.4.0", "MIT licence"));
        assert_eq!(amd.files.iter().map(|f| (f.name.as_str(), f.size)).collect::<Vec<_>>(), [
            ("amd_fidelityfx_loader_vk.dll", 39_184), ("amd_fidelityfx_upscaler_vk.dll", 36_135_696), ("amd_fidelityfx_framegeneration_vk.dll", 36_806_928)]);
        assert_eq!(amd.size(), 72_981_808);
        let intel = m.component("intel-xess").unwrap();
        assert_eq!((intel.vendor.as_str(), intel.version.as_str()), ("intel", "3.0.2"));
        assert!(intel.license.name.starts_with("Intel Simplified Software License"));
        assert_eq!(intel.files.len(), 1);
        assert_eq!((intel.files[0].name.as_str(), intel.files[0].size), ("libxess.dll", 77_795_704));
        // pinned (tools/release/vendor_pin.py) from the vendors' files at the tags, each checked by its git blob
        let pins: Vec<(&str, &str)> = m.components.iter().flat_map(|c| c.files.iter().map(|f| (f.name.as_str(), f.sha256.as_str()))).collect();
        assert_eq!(pins, [
            ("amd_fidelityfx_loader_vk.dll", "38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9"),
            ("amd_fidelityfx_upscaler_vk.dll", "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7"),
            ("amd_fidelityfx_framegeneration_vk.dll", "d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4"),
            ("libxess.dll", "251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7"),
        ]);
        for c in &m.components {
            assert!(c.pinned(), "{} is not pinned", c.id);
            assert!(c.files.iter().all(|f| f.git_blob.is_some()));
            assert_eq!(c.hosts(), ["raw.githubusercontent.com"]);
            assert!(!c.name.to_ascii_lowercase().contains("dlss"));
        }
    }

    #[test]
    fn a_component_with_one_file_not_pinned_is_not_pinned() {
        let mut c = builtin().unwrap().component("amd-fsr").unwrap().clone();
        assert!(c.pinned());
        c.files[1].sha256 = NOT_PINNED.into();
        assert!(!c.pinned() && c.files[0].pinned() && !c.files[1].pinned());
        // and a manifest saying so parses
        let mut v: serde_json::Value = serde_json::from_str(BUILTIN).unwrap();
        v["components"][0]["files"][1]["sha256"] = NOT_PINNED.into();
        let m = parse(&v.to_string()).unwrap();
        assert!(!m.component("amd-fsr").unwrap().pinned() && m.component("intel-xess").unwrap().pinned());
    }

    #[test]
    fn every_bundled_licence_text_is_named_and_hashed() {
        let m = builtin().unwrap();
        let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../launcher/src/data/vendor-licenses");
        let mut on_disk: Vec<String> = std::fs::read_dir(&dir).unwrap().map(|e| format!("vendor-licenses/{}", e.unwrap().file_name().to_string_lossy())).collect();
        on_disk.sort();
        let mut bundled: Vec<String> = LICENSE_TEXTS.iter().map(|(n, _)| n.to_string()).collect();
        bundled.sort();
        assert_eq!(on_disk, bundled, "every file in vendor-licenses/ is bundled, and nothing else");
        for c in &m.components {
            let text = license_text(&c.license.file).unwrap();
            assert_eq!(license_sha256(text), c.license.sha256);
            // the text on disk is the bundled one, with LF line ends
            let disk = std::fs::read_to_string(dir.join(c.license.file.trim_start_matches("vendor-licenses/"))).unwrap();
            assert_eq!(disk, text);
            assert!(!text.contains('\r'));
        }
        // the vendors' own words: AMD's MIT section and Intel's licence
        let amd = license_text(&m.component("amd-fsr").unwrap().license.file).unwrap();
        assert!(amd.starts_with("Copyright (C)\u{202f}\u{202f}Advanced Micro Devices, Inc. All rights reserved."));
        assert!(amd.contains("to deal in the Software without restriction"));
        let intel = license_text(&m.component("intel-xess").unwrap().license.file).unwrap();
        assert!(intel.starts_with("Intel Simplified Software License (Version October 2022)"));
        assert!(intel.contains("without modification"));
        // a CRLF copy hashes the same
        assert_eq!(license_sha256(&intel.replace('\n', "\r\n")), license_sha256(intel));
    }

    #[test]
    fn urls_are_tag_pinned_files_on_the_vendors_own_organisation() {
        let ok = [
            ("https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/a.dll", "amd", "2.4.0"),
            ("https://raw.githubusercontent.com/intel/xess/3.0.2/bin/libxess.dll", "intel", "3.0.2"),
            ("https://media.githubusercontent.com/media/NVIDIA/DLSS/v310.9.16/lib/x.dll", "nvidia", "310.9.16"),
            ("https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip", "nvidia", "2.14.16"),
        ];
        for (u, v, ver) in ok {
            check_url(u, v, ver).unwrap_or_else(|e| panic!("{u}: {}", e.message));
        }
        let bad = [
            // a branch, not the tag
            ("https://raw.githubusercontent.com/intel/xess/main/bin/libxess.dll", "intel", "3.0.2"),
            // another version's tag
            ("https://raw.githubusercontent.com/intel/xess/v3.0.1/bin/libxess.dll", "intel", "3.0.2"),
            // another organisation (a fork)
            ("https://raw.githubusercontent.com/someone/xess/v3.0.2/bin/libxess.dll", "intel", "3.0.2"),
            // another vendor's organisation
            ("https://raw.githubusercontent.com/intel/xess/v3.0.2/bin/libxess.dll", "amd", "3.0.2"),
            // not https, another host, a query, a fragment, a port
            ("http://raw.githubusercontent.com/intel/xess/v3.0.2/bin/libxess.dll", "intel", "3.0.2"),
            ("https://example.com/intel/xess/v3.0.2/bin/libxess.dll", "intel", "3.0.2"),
            ("https://raw.githubusercontent.com/intel/xess/v3.0.2/bin/libxess.dll?x=1", "intel", "3.0.2"),
            ("https://raw.githubusercontent.com/intel/xess/v3.0.2/bin/libxess.dll#a", "intel", "3.0.2"),
            ("https://raw.githubusercontent.com:8443/intel/xess/v3.0.2/bin/libxess.dll", "intel", "3.0.2"),
            // GitHub's asset storage is a redirect target only, never a manifest URL
            ("https://objects.githubusercontent.com/intel/xess/v3.0.2/bin/libxess.dll", "intel", "3.0.2"),
            // a page, not a file; a release asset path of the wrong shape
            ("https://github.com/intel/xess/blob/v3.0.2/bin/libxess.dll", "intel", "3.0.2"),
            ("https://github.com/intel/xess/releases/download/v3.0.2/a/b.dll", "intel", "3.0.2"),
            // no file, a relative or escaped segment
            ("https://raw.githubusercontent.com/intel/xess/v3.0.2", "intel", "3.0.2"),
            ("https://raw.githubusercontent.com/intel/xess/v3.0.2/../main/x.dll", "intel", "3.0.2"),
            ("https://raw.githubusercontent.com/intel/xess/v3.0.2/bin/%2e%2e/x.dll", "intel", "3.0.2"),
            ("https://raw.githubusercontent.com/intel/xess/v3.0.2//x.dll", "intel", "3.0.2"),
        ];
        for (u, v, ver) in bad {
            assert!(check_url(u, v, ver).is_err(), "accepted {u} for {v} {ver}");
        }
        assert_eq!(redirect_hosts("github.com"), ["release-assets.githubusercontent.com", "objects.githubusercontent.com"]);
        assert!(redirect_hosts("raw.githubusercontent.com").is_empty());
        assert!(redirect_hosts("example.com").is_empty());
    }

    #[test]
    fn a_manifest_that_breaks_a_rule_is_refused() {
        let cases: Vec<(&str, Box<dyn FnOnce(&mut serde_json::Value)>)> = vec![
            ("a newer format", Box::new(|v| v["format"] = 2.into())),
            ("an id twice", Box::new(|v| v["components"][1]["id"] = "amd-fsr".into())),
            ("an id with capitals", Box::new(|v| v["components"][0]["id"] = "AMD".into())),
            ("an unknown vendor", Box::new(|v| v["components"][0]["vendor"] = "acme".into())),
            ("one folder for two components", Box::new(|v| { v["components"][1]["vendor"] = "amd".into(); v["components"][1]["version"] = "2.4.0".into(); })),
            ("a version that is no version", Box::new(|v| v["components"][0]["version"] = "latest".into())),
            ("a DLSS name for AMD's files", Box::new(|v| v["components"][0]["name"] = "DLSS for AMD".into())),
            ("an empty sentence", Box::new(|v| v["components"][0]["enables"] = " ".into())),
            ("a licence not bundled", Box::new(|v| v["components"][0]["license"]["file"] = "vendor-licenses/nope.txt".into())),
            ("a licence hash that is not the text's", Box::new(|v| v["components"][0]["license"]["sha256"] = "0".repeat(64).into())),
            ("a file name with a folder", Box::new(|v| v["components"][0]["files"][0]["name"] = "sub/x.dll".into())),
            ("Kiln's own receipt as a file name", Box::new(|v| v["components"][0]["files"][0]["name"] = "kiln-vendor.json".into())),
            ("a part file as a file name", Box::new(|v| v["components"][0]["files"][0]["name"] = "a.dll.part".into())),
            ("a file name twice", Box::new(|v| v["components"][0]["files"][1]["name"] = "AMD_fidelityfx_loader_vk.dll".into())),
            ("a file name ending with a dot (Windows drops it)", Box::new(|v| v["components"][0]["files"][0]["name"] = "a.dll.".into())),
            ("a Windows device name", Box::new(|v| v["components"][0]["files"][0]["name"] = "NUL.dll".into())),
            ("a Windows port name", Box::new(|v| v["components"][0]["files"][0]["name"] = "com1.dll".into())),
            ("a branch URL", Box::new(|v| v["components"][1]["files"][0]["url"] = "https://raw.githubusercontent.com/intel/xess/main/bin/libxess.dll".into())),
            ("a size of 0", Box::new(|v| v["components"][1]["files"][0]["size"] = 0.into())),
            ("a size over the cap", Box::new(|v| v["components"][1]["files"][0]["size"] = (MAX_FILE + 1).into())),
            ("an upper-case hash", Box::new(|v| v["components"][1]["files"][0]["sha256"] = "A".repeat(64).into())),
            ("a short hash", Box::new(|v| v["components"][1]["files"][0]["sha256"] = "abc".into())),
            ("no files", Box::new(|v| v["components"][1]["files"] = serde_json::json!([]))),
            ("a source off GitHub", Box::new(|v| v["components"][1]["source"] = "https://example.com/".into())),
        ];
        for (what, patch) in cases {
            assert!(one(patch).is_err(), "accepted {what}");
        }
        // unknown keys are ignored; one file not pinned makes its component not pinned
        let m = one(|v| {
            v["later"] = true.into();
            v["components"][1]["files"][0]["sha256"] = NOT_PINNED.into();
        }).unwrap();
        assert!(!m.component("intel-xess").unwrap().pinned());
        assert!(m.component("amd-fsr").unwrap().pinned());
        assert_eq!(parse("{").unwrap_err().code, ErrorCode::Parse);
    }
}
