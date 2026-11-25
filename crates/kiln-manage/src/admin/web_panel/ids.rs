//! The web panel mod's two ids (0.2.0-dev.31.14). Kiln Web Admin (`kiln-webadmin`, the 0.1.0 test builds) was renamed
//! Kilnwarden (`kilnwarden`). The id is the mod's data folder (`kiln/data/<id>/`, where its `launcher-endpoint.json`
//! is written while the panel listens) and the consumer its one-time launcher login is minted for. Launchers from
//! 0.2.0-dev.31.2 to 31.13 know only the old id.
//!
//! Kilnwarden comes first. With the old copy still enabled, Kilnwarden stays off (it writes no endpoint and logs why),
//! so the old copy's endpoint is the one found. Kilnwarden moves `kiln/data/kiln-webadmin/` to its own folder when it
//! first starts, so after that only its own endpoint is left.

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

/// The first mod in [`ALL`] whose endpoint `read` gets, with the bytes read. `read` failing for every one: no panel
/// listens.
pub fn find<E>(
    mut read: impl FnMut(PanelMod) -> std::result::Result<Vec<u8>, E>,
) -> Result<(PanelMod, Vec<u8>)> {
    ALL.into_iter()
        .find_map(|m| read(m).ok().map(|bytes| (m, bytes)))
        .ok_or_else(not_listening)
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
        let (m, bytes) = find(|m| {
            asked.push(m.id);
            read_from(&["kiln-webadmin"])(m)
        })
        .unwrap();
        assert_eq!(
            (m, bytes.as_slice()),
            (KILN_WEBADMIN, b"kiln-webadmin".as_slice())
        );
        assert_eq!(asked, ["kilnwarden", "kiln-webadmin"]);
    }

    #[test]
    fn kilnwarden_wins_and_the_old_one_is_not_read() {
        let mut asked = Vec::new();
        let (m, _) = find(|m| {
            asked.push(m.id);
            read_from(&["kilnwarden", "kiln-webadmin"])(m)
        })
        .unwrap();
        assert_eq!(m, KILNWARDEN);
        assert_eq!(asked, ["kilnwarden"]);
    }

    #[test]
    fn neither_listening() {
        let e = find(read_from(&[])).unwrap_err();
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
