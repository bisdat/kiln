//! The web panel mod's two ids (0.2.0-dev.31.14). Kiln Web Admin (`kiln-webadmin`, the 0.1.0 test builds) was renamed
//! Kilnwarden (`kilnwarden`). The id is the mod's data folder (`kiln/data/<id>/`, where its `launcher-endpoint.json`
//! is written while the panel listens) and the consumer its one-time launcher login is minted for. Launchers from
//! 0.2.0-dev.31.2 to 31.13 know only the old id.
//!
//! Kilnwarden comes first. With the old copy still enabled, Kilnwarden stays off (it writes no endpoint and logs why),
//! so the old copy's endpoint is the one found. Kilnwarden moves `kiln/data/kiln-webadmin/` to its own folder when it
//! first starts, so after that only its own endpoint is left.
//!
//! (review of 0.2.0-dev.31.14) A panel removes its file when it stops, but a crash skips that, so a file can be left
//! from an earlier run. When both folders have one and the server's Kiln has the old copy loaded (or loading:
//! `kiln/state/mods.json`), Kilnwarden is off and its file is the stale one: the old copy's is used. Either way the
//! certificate pin is checked before any login is minted, so a stale file only ever costs an error, never a login.

use crate::{Error, ErrorCode, Result};

/// A web panel mod: its id and the name messages use.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PanelMod {
    pub id: &'static str,
    pub name: &'static str,
}

pub const KILNWARDEN: PanelMod = PanelMod {
    id: "kilnwarden",
    name: "Kilnwarden",
};
pub const KILN_WEBADMIN: PanelMod = PanelMod {
    id: "kiln-webadmin",
    name: "Kiln Web Admin",
};

/// In the order the launcher looks.
pub const ALL: [PanelMod; 2] = [KILNWARDEN, KILN_WEBADMIN];

/// The file each one writes while its panel listens: `<data root>/<id>/launcher-endpoint.json`.
pub const ENDPOINT_FILE: &str = "launcher-endpoint.json";

impl PanelMod {
    /// The endpoint relative to a server folder whose data root is `kiln/data`.
    pub fn endpoint(self) -> String {
        format!("kiln/data/{}/{ENDPOINT_FILE}", self.id)
    }

    /// `login_mint`'s arguments: the login is for the mod that listens. Kilnwarden also redeems one minted for the
    /// old id (what launchers before 31.14 mint), but its own id needs no second try and no refusal in Kiln.Admin's log.
    pub fn login_mint_args(self) -> serde_json::Map<String, serde_json::Value> {
        let mut args = serde_json::Map::new();
        args.insert("consumer".into(), self.id.into());
        args.insert("label".into(), "Kiln Launcher".into());
        args
    }
}

/// The mod whose panel listens, with its endpoint's bytes: the first in [`ALL`] whose endpoint `read` gets, except that
/// with both files there and `old_copy_loaded()` (asked only then) the old copy's is taken, as Kilnwarden stays off
/// next to it. `read` failing for every one: no panel listens.
pub fn find<E>(
    mut read: impl FnMut(PanelMod) -> std::result::Result<Vec<u8>, E>,
    old_copy_loaded: impl FnOnce() -> bool,
) -> Result<(PanelMod, Vec<u8>)> {
    let new = read(KILNWARDEN).ok();
    let old = read(KILN_WEBADMIN).ok();
    match (new, old) {
        (Some(new), Some(old)) => Ok(if old_copy_loaded() { (KILN_WEBADMIN, old) } else { (KILNWARDEN, new) }),
        (Some(new), None) => Ok((KILNWARDEN, new)),
        (None, Some(old)) => Ok((KILN_WEBADMIN, old)),
        (None, None) => Err(not_listening()),
    }
}

/// Whether `kiln/state/mods.json` (its bytes) has the old copy loaded or still loading, the states in which Kilnwarden
/// stays off. An unreadable file: false (Kilnwarden first).
pub fn old_copy_in_mods_json(bytes: &[u8]) -> bool {
    let Ok(doc) = serde_json::from_slice::<serde_json::Value>(bytes) else { return false };
    doc["mods"].as_array().is_some_and(|mods| {
        mods.iter().any(|m| m["id"] == KILN_WEBADMIN.id && matches!(m["state"].as_str(), Some("loaded" | "pending")))
    })
}

pub fn not_listening() -> Error {
    Error::new(
        ErrorCode::AdminUnavailable,
        "Kilnwarden is not listening. Check that the current mod is loaded.",
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn read_from(
        present: &'static [&'static str],
    ) -> impl FnMut(PanelMod) -> std::result::Result<Vec<u8>, ()> {
        move |m| {
            present
                .contains(&m.id)
                .then(|| m.id.as_bytes().to_vec())
                .ok_or(())
        }
    }

    #[test]
    fn kilnwarden_first_then_the_old_id() {
        assert_eq!(ALL.map(|m| m.id), ["kilnwarden", "kiln-webadmin"]);
        let mut asked = Vec::new();
        let (m, bytes) = find(
            |m| {
                asked.push(m.id);
                read_from(&["kiln-webadmin"])(m)
            },
            || unreachable!("one file: mods.json is not needed"),
        )
        .unwrap();
        assert_eq!(
            (m, bytes.as_slice()),
            (KILN_WEBADMIN, b"kiln-webadmin".as_slice())
        );
        assert_eq!(asked, ["kilnwarden", "kiln-webadmin"]);
    }

    #[test]
    fn with_both_files_kilnwarden_wins_unless_the_old_copy_is_loaded() {
        let mut asked = Vec::new();
        let (m, bytes) = find(read_from(&["kilnwarden", "kiln-webadmin"]), || {
            asked.push("mods.json");
            false
        })
        .unwrap();
        assert_eq!((m, bytes.as_slice()), (KILNWARDEN, b"kilnwarden".as_slice()));
        assert_eq!(asked, ["mods.json"]);
        // the old copy loaded: Kilnwarden stays off, so its file is left from an earlier run
        let (m, bytes) = find(read_from(&["kilnwarden", "kiln-webadmin"]), || true).unwrap();
        assert_eq!((m, bytes.as_slice()), (KILN_WEBADMIN, b"kiln-webadmin".as_slice()));
        // Kilnwarden alone: whatever mods.json says
        let (m, _) = find(read_from(&["kilnwarden"]), || unreachable!()).unwrap();
        assert_eq!(m, KILNWARDEN);
    }

    #[test]
    fn the_old_copy_counts_while_loaded_or_loading() {
        let doc = |state: &str| format!(r#"{{"format":1,"pid":1,"mods":[{{"id":"kilnwarden","state":"loaded"}},{{"id":"kiln-webadmin","state":"{state}"}}]}}"#);
        assert!(old_copy_in_mods_json(doc("loaded").as_bytes()));
        assert!(old_copy_in_mods_json(doc("pending").as_bytes()));
        for off in ["disabled", "failed", "skipped", "unloaded"] {
            assert!(!old_copy_in_mods_json(doc(off).as_bytes()), "{off}");
        }
        assert!(!old_copy_in_mods_json(br#"{"mods":[{"id":"kilnwarden","state":"loaded"}]}"#));
        assert!(!old_copy_in_mods_json(br#"{"mods":[{"id":"Kiln-WebAdmin","state":"loaded"}]}"#), "ids are exact");
        for bad in [&b""[..], b"not json", b"[]", br#"{"mods":{}}"#] {
            assert!(!old_copy_in_mods_json(bad));
        }
    }

    #[test]
    fn neither_listening() {
        let e = find(read_from(&[]), || unreachable!()).unwrap_err();
        assert_eq!(e.code, ErrorCode::AdminUnavailable);
        assert!(e.message.starts_with("Kilnwarden is not listening"));
    }

    #[test]
    fn endpoint_paths_and_login_consumer_follow_the_id() {
        assert_eq!(
            KILNWARDEN.endpoint(),
            "kiln/data/kilnwarden/launcher-endpoint.json"
        );
        assert_eq!(
            KILN_WEBADMIN.endpoint(),
            "kiln/data/kiln-webadmin/launcher-endpoint.json"
        );
        assert_eq!(KILNWARDEN.login_mint_args()["consumer"], "kilnwarden");
        assert_eq!(KILN_WEBADMIN.login_mint_args()["consumer"], "kiln-webadmin");
        assert_eq!(KILNWARDEN.login_mint_args()["label"], "Kiln Launcher");
    }
}
