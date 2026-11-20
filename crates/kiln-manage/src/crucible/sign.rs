//! The signed envelope around a pack index (Crucible's src/sign.mjs): `{format: 2, kind: "signed-pack", alg: "ed25519",
//! keyId, payload, signature}`, payload = the index's UTF-8 JSON bytes (base64), signature = Ed25519 over those bytes
//! (base64), keyId = the first 16 hex characters of sha256(the raw 32-byte public key). The index is served from a
//! public CDN, so the launcher trusts it only through this signature, checked against public keys compiled in
//! (`super::KEYS`; a dev build may add one, never replace the check).

use crate::modpack::pack::{self, PackIndex};
use crate::{Error, ErrorCode, Result};
use base64::Engine;
use serde::Deserialize;

/// The longest envelope accepted (an index is at most 1 MiB on Crucible; base64 adds a third).
pub const MAX_ENVELOPE: u64 = 2 * 1024 * 1024;

/// An Ed25519 public key the launcher trusts for pack indexes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PublicKey {
    /// sha256(raw)[..8] as hex: what an envelope names.
    pub id: String,
    pub raw: [u8; 32],
}

impl PublicKey {
    /// From the raw 32 bytes as base64 (what `node src/main.mjs --public-key` prints).
    pub fn from_base64(b64: &str) -> Result<PublicKey> {
        let bytes = base64::engine::general_purpose::STANDARD
            .decode(b64.trim())
            .map_err(|_| Error::invalid("a Crucible signing key must be base64"))?;
        let raw: [u8; 32] = bytes.try_into().map_err(|_| Error::invalid("a Crucible signing key is 32 bytes (Ed25519)"))?;
        let id = crate::hash::sha256_hex(&raw)[..16].to_string();
        Ok(PublicKey { id, raw })
    }
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Envelope {
    format: u32,
    kind: String,
    alg: String,
    key_id: String,
    payload: String,
    signature: String,
}

fn refused(why: &str) -> Error {
    Error::new(ErrorCode::HashMismatch, format!("This pack's index is not signed by Crucible ({why}). Nothing was installed."))
}

/// Checks an envelope against the trusted keys and returns the index it carries, checked with the pack format's rules
/// (`pack::check_index`), plus the payload's JSON (what the creator's tools wrote, with Crucible's own fields).
pub fn open(envelope: &[u8], keys: &[PublicKey]) -> Result<(PackIndex, serde_json::Value)> {
    if envelope.len() as u64 > MAX_ENVELOPE {
        return Err(refused("it is too large"));
    }
    let env: Envelope = serde_json::from_slice(envelope).map_err(|_| refused("it is no signed envelope"))?;
    if env.format != 2 || env.kind != "signed-pack" {
        return Err(refused("it is no signed pack envelope"));
    }
    if env.alg != "ed25519" {
        return Err(refused("an unknown signature algorithm"));
    }
    let key = keys.iter().find(|k| k.id == env.key_id).ok_or_else(|| refused("signed with a key this Kiln does not know: update Kiln"))?;
    let b64 = base64::engine::general_purpose::STANDARD;
    let payload = b64.decode(env.payload.as_bytes()).map_err(|_| refused("the payload is no base64"))?;
    let sig = b64.decode(env.signature.as_bytes()).map_err(|_| refused("the signature is no base64"))?;
    ring::signature::UnparsedPublicKey::new(&ring::signature::ED25519, &key.raw)
        .verify(&payload, &sig)
        .map_err(|_| refused("the signature does not match"))?;
    let value: serde_json::Value = serde_json::from_slice(&payload).map_err(|_| refused("the payload is no JSON"))?;
    match value.get("format").and_then(|v| v.as_u64()) {
        Some(2) => {}
        Some(n) if n > 2 => return Err(Error::new(ErrorCode::Unsupported, format!("This pack uses format {n}, newer than this Kiln: update Kiln."))),
        _ => return Err(Error::new(ErrorCode::Schema, "This pack's index is not format 2.")),
    }
    let index: PackIndex = serde_json::from_value(value.clone()).map_err(|e| Error::new(ErrorCode::Schema, format!("This pack's index cannot be read: {e}")))?;
    pack::check_index(&index)?;
    Ok((index, value))
}

#[cfg(test)]
pub(crate) mod testing {
    //! A test signer with the same envelope as Crucible's src/sign.mjs.
    use base64::Engine;

    pub struct Signer {
        pair: ring::signature::Ed25519KeyPair,
        pub key: super::PublicKey,
    }

    impl Signer {
        pub fn new(seed: u8) -> Signer {
            let pair = ring::signature::Ed25519KeyPair::from_seed_unchecked(&[seed; 32]).expect("seed");
            let raw = ring::signature::KeyPair::public_key(&pair).as_ref().to_vec();
            let key = super::PublicKey::from_base64(&base64::engine::general_purpose::STANDARD.encode(&raw)).expect("key");
            Signer { pair, key }
        }

        pub fn envelope(&self, payload: &[u8]) -> String {
            let b64 = base64::engine::general_purpose::STANDARD;
            serde_json::json!({
                "format": 2, "kind": "signed-pack", "alg": "ed25519", "keyId": self.key.id,
                "payload": b64.encode(payload), "signature": b64.encode(self.pair.sign(payload).as_ref()),
            })
            .to_string()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::testing::Signer;
    use super::*;

    fn index_json() -> Vec<u8> {
        serde_json::json!({
            "format": 2, "kind": "pack", "id": "ember-run", "name": "Ember run", "version": "1.0.0",
            "mods": [{ "kind": "kiln", "id": "alpha", "version": "1.0.0", "path": "mods/kiln/alpha", "hash": format!("kdh1:{}", "a".repeat(64)),
                "file": { "sha256": "b".repeat(64), "size": 10, "url": "https://cdn.frostdev.io/f/x.zip" } }],
            "order": { "kiln": ["alpha"], "late": [] },
            "crucible": { "pack": "pck_1", "version": "pv_1", "slug": "ember-run", "code": "H7QK-3MZP", "number": 1 },
        })
        .to_string()
        .into_bytes()
    }

    #[test]
    fn a_signed_index_opens_with_its_key() {
        let s = Signer::new(7);
        let (ix, raw) = open(s.envelope(&index_json()).as_bytes(), std::slice::from_ref(&s.key)).unwrap();
        assert_eq!(ix.id, "ember-run");
        assert_eq!(ix.crucible.unwrap().code.as_deref(), Some("H7QK-3MZP"));
        assert_eq!(raw["crucible"]["number"], 1);
    }

    #[test]
    fn the_key_id_is_sha256_of_the_raw_key() {
        // the same rule as sign.mjs: sha256(raw 32 bytes) hex, first 16 characters
        let k = PublicKey::from_base64(&base64::engine::general_purpose::STANDARD.encode([1u8; 32])).unwrap();
        assert_eq!(k.id, crate::hash::sha256_hex(&[1u8; 32])[..16]);
        assert!(PublicKey::from_base64("AAAA").is_err());
    }

    #[test]
    fn another_key_or_a_changed_payload_is_refused() {
        let s = Signer::new(7);
        let other = Signer::new(8);
        let env = s.envelope(&index_json());
        assert_eq!(open(env.as_bytes(), std::slice::from_ref(&other.key)).unwrap_err().code, ErrorCode::HashMismatch);
        // the same key id with another key's bytes (an attacker naming the trusted id)
        let mut forged = other.key.clone();
        forged.id = s.key.id.clone();
        let env_other = other.envelope(&index_json()).replace(&other.key.id, &s.key.id);
        assert!(open(env_other.as_bytes(), std::slice::from_ref(&s.key)).is_err());
        // a payload changed after signing
        let mut v: serde_json::Value = serde_json::from_str(&env).unwrap();
        let mut payload = index_json();
        payload.extend_from_slice(b" ");
        v["payload"] = base64::engine::general_purpose::STANDARD.encode(&payload).into();
        assert_eq!(open(v.to_string().as_bytes(), std::slice::from_ref(&s.key)).unwrap_err().code, ErrorCode::HashMismatch);
        // no keys at all: nothing opens
        assert!(open(env.as_bytes(), &[]).is_err());
        let _ = forged;
    }

    #[test]
    fn a_signed_but_broken_index_is_refused_by_the_format_rules() {
        let s = Signer::new(7);
        let bad = serde_json::json!({ "format": 2, "kind": "pack", "id": "../x", "name": "x", "version": "1.0.0", "mods": [] }).to_string();
        assert!(open(s.envelope(bad.as_bytes()).as_bytes(), std::slice::from_ref(&s.key)).is_err());
        let newer = serde_json::json!({ "format": 3, "kind": "pack" }).to_string();
        assert_eq!(open(s.envelope(newer.as_bytes()).as_bytes(), std::slice::from_ref(&s.key)).unwrap_err().code, ErrorCode::Unsupported);
    }
}
