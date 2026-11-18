//! The vendor licences the player agreed to: `<data>/state/vendor-licenses.json`
//! (`schema/vendor-licenses.schema.json`).
//!
//! Per vendor, every licence text agreed to, by the SHA-256 of the text ([`super::manifest::license_sha256`]). A
//! component whose licence hash is not listed for its vendor needs the dialog again, so a changed text asks again and a
//! vendor with two licences (NVIDIA's SDK licence and Streamline's MIT) keeps both.
//!
//! Why the launcher's data folder and not the game's `kiln.json`: the agreement is the player's, made once on this PC
//! in the launcher, and nothing in the game reads it; `kiln.json` is per game folder (a second install would ask
//! again, a reinstall would forget it) and the game, the player and other tools write it. Losing the file only means
//! the dialog comes again. A file this build cannot read (a newer format, broken JSON) counts as no agreement and is
//! replaced at the next agreement.

use super::manifest::Component;
use crate::paths::DataDirs;
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::PathBuf;

pub const FORMAT: u32 = 1;

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Agreements {
    pub format: u32,
    #[serde(default)]
    pub vendors: BTreeMap<String, Vec<Agreement>>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Agreement {
    /// The licence's name as the dialog showed it.
    pub license: String,
    /// SHA-256 of the text the dialog showed.
    pub sha256: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub component: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub version: Option<String>,
    /// RFC 3339 UTC.
    pub agreed: String,
}

/// `<data>/state/vendor-licenses.json`
pub fn path(dirs: &DataDirs) -> PathBuf {
    dirs.root.join("state").join("vendor-licenses.json")
}

impl Agreements {
    /// The file, or none when it is missing or cannot be read by this build.
    pub fn load(dirs: &DataDirs) -> Agreements {
        match crate::util::read_json::<Agreements>(&path(dirs)) {
            Ok(Some(a)) if a.format == FORMAT => a,
            _ => Agreements { format: FORMAT, vendors: BTreeMap::new() },
        }
    }

    /// The player agreed to this component's licence text (its vendor and hash).
    pub fn agreed(&self, c: &Component) -> bool {
        self.vendors.get(&c.vendor).is_some_and(|l| l.iter().any(|a| a.sha256 == c.license.sha256))
    }

    /// When the player agreed to this component's licence text.
    pub fn agreed_at(&self, c: &Component) -> Option<&str> {
        self.vendors.get(&c.vendor)?.iter().find(|a| a.sha256 == c.license.sha256).map(|a| a.agreed.as_str())
    }
}

/// Records the player's agreement to `c`'s licence. `shown_sha256` is the hash of the text the dialog showed: it must
/// be the manifest's (a text that changed between the dialog and the click is refused and has to be read again).
pub fn record(dirs: &DataDirs, c: &Component, shown_sha256: &str) -> Result<Agreements> {
    if shown_sha256 != c.license.sha256 {
        return Err(Error::new(ErrorCode::ConsentRequired, format!(
            "The {} text of {} is not the one the dialog showed. Read it again before you agree.", c.license.name, c.name)));
    }
    let mut a = Agreements::load(dirs);
    if a.agreed(c) {
        return Ok(a);
    }
    a.vendors.entry(c.vendor.clone()).or_default().push(Agreement {
        license: c.license.name.clone(),
        sha256: c.license.sha256.clone(),
        component: Some(c.id.clone()),
        version: Some(c.version.clone()),
        agreed: crate::util::now_rfc3339(),
    });
    crate::util::write_json_atomic(&path(dirs), &a)?;
    Ok(a)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::vendor::manifest::builtin;

    struct Tmp(PathBuf);
    impl Drop for Tmp {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    fn tmp() -> Tmp {
        let p = std::env::temp_dir().join(format!("kiln-vendor-agree-{}", crate::hash::hex(&crate::util::random_bytes::<6>().unwrap())));
        std::fs::create_dir_all(&p).unwrap();
        Tmp(p)
    }

    #[test]
    fn an_agreement_is_per_vendor_and_licence_text() {
        let t = tmp();
        let dirs = DataDirs::at(&t.0);
        let m = builtin().unwrap();
        let (amd, intel) = (m.component("amd-fsr").unwrap().clone(), m.component("intel-xess").unwrap().clone());
        assert!(!Agreements::load(&dirs).agreed(&amd), "nothing is agreed on a new PC");
        // the dialog showed another text: refused, nothing written
        let e = record(&dirs, &amd, &"0".repeat(64)).unwrap_err();
        assert_eq!(e.code, ErrorCode::ConsentRequired);
        assert!(!path(&dirs).exists());
        let a = record(&dirs, &amd, &amd.license.sha256).unwrap();
        assert!(a.agreed(&amd) && !a.agreed(&intel), "AMD's yes is not Intel's");
        assert!(Agreements::load(&dirs).agreed(&amd));
        assert!(Agreements::load(&dirs).agreed_at(&amd).is_some());
        // agreeing again adds nothing
        record(&dirs, &amd, &amd.license.sha256).unwrap();
        assert_eq!(Agreements::load(&dirs).vendors["amd"].len(), 1);
        // a changed text asks again; agreeing to it keeps the first one too
        let mut changed = amd.clone();
        changed.license.sha256 = "ab".repeat(32);
        assert!(!Agreements::load(&dirs).agreed(&changed));
        record(&dirs, &changed, &changed.license.sha256).unwrap();
        let a = Agreements::load(&dirs);
        assert!(a.agreed(&changed) && a.agreed(&amd));
        // the file is what the schema describes
        let v: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(path(&dirs)).unwrap()).unwrap();
        assert_eq!(v["format"], 1);
        assert_eq!(v["vendors"]["amd"][0]["license"], "MIT licence");
        assert_eq!(v["vendors"]["amd"][0]["component"], "amd-fsr");
    }

    #[test]
    fn a_file_this_build_cannot_read_counts_as_no_agreement() {
        let t = tmp();
        let dirs = DataDirs::at(&t.0);
        let amd = builtin().unwrap().component("amd-fsr").unwrap().clone();
        std::fs::create_dir_all(t.0.join("state")).unwrap();
        for text in ["{", r#"{"format": 2, "vendors": {"amd": []}}"#] {
            std::fs::write(path(&dirs), text).unwrap();
            assert!(!Agreements::load(&dirs).agreed(&amd));
        }
        let planted = format!(r#"{{"format": 2, "vendors": {{"amd": [{{"license": "x", "sha256": "{}", "agreed": "2026-01-01T00:00:00Z"}}]}}}}"#, amd.license.sha256);
        std::fs::write(path(&dirs), planted).unwrap();
        assert!(!Agreements::load(&dirs).agreed(&amd), "a newer format is not read");
        record(&dirs, &amd, &amd.license.sha256).unwrap();
        assert!(Agreements::load(&dirs).agreed(&amd));
    }
}
