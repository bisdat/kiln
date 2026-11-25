//! Crucible, the modpack platform on frostdev.io, as the launcher uses it (docs/kiln/launcher/crucible.md, "The
//! launcher's Crucible client"). The service: /mnt/Dongus/kiln-wave/dev30/crucible (README, API); the pack format and
//! instances: [`crate::modpack::pack`].
//!
//! - **Receiving** needs no sign-in: a code or link -> `GET /codes/<code>` (or `/packs/<slug>`) -> the version's signed
//!   index from the CDN (`m/<versionId>.json`), opened with a public key compiled in ([`sign`]) -> the parts the Library
//!   lacks downloaded by sha256 from the CDN only (`f/`, `l/`, `b/`), each checked -> [`pack::assemble`] puts a .kilnpack
//!   together and checks every kdh1 -> [`pack::install`] makes the instance.
//! - **Sharing** needs the player's Discord sign-in (the relay session as Bearer): [`pack::build`] from the profile ->
//!   per-mod deterministic archives ([`pack::mod_archive`]) -> `POST /quickshare` (or a new version of the pack this
//!   profile was shared as) -> only the files Crucible lacks go up, to the signed URLs it hands out (single PUT or
//!   multipart) -> `POST /uploads/<id>/complete`. The link and code exist at once.
//! - **Hosts**: the API (credential host), the CDN (plain), upload hosts (R2's, plain). Nothing else is reached; a URL
//!   from the service on another host is refused before anything is sent.
//!
//! State in `<data>/crucible/`: `shared.json` (what each profile was shared as), `trust.json` (creators the player
//! always trusts), `cache/` (archives by sha256, kept for dedupe and re-installs), `packs/` (assembled packs).

pub mod dev;
pub mod links;
/// Pack Studio's publish: a version of a pack source, its credits, link-only or public.
pub mod publish;
mod ownership;
pub mod sign;

use crate::game::GameInstall;
use crate::library::LibraryRef;
use crate::manifest::ModKind;
use crate::modpack::ModpackSource;
use crate::modpack::pack::{self, PackIndex, StoredFile};
use crate::net::{self, Body, Method, Request, Transport};
use crate::paths::DataDirs;
use crate::task::Progress;
use crate::{Error, ErrorCode, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sign::PublicKey;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// Crucible's API.
pub const API: &str = "https://frostdev.io/kiln/crucible/api/v1";
/// Crucible's files (Cloudflare R2 behind cdn.frostdev.io).
pub const CDN: &str = "https://cdn.frostdev.io";
/// Upload URLs Crucible hands out point at R2's S3 endpoint (`<account>.r2.cloudflarestorage.com`).
pub const UPLOAD_SUFFIX: &str = ".r2.cloudflarestorage.com";
/// Public keys of Crucible's index signer (raw Ed25519, base64). The pair is made on the server at the first deploy
/// and its public key goes here once the owner approves it (DESIGN.md section 13.6). Until then this is empty and the
/// launcher shows no Crucible surface ([`Endpoints::enabled`]).
pub const KEYS: &[&str] = &["3wedy1u9hKuyGh6UcRGBe1X8YvAiPyeALoIpVaBXK/s="];
/// The largest pack Crucible takes (2 GiB) and the largest file (512 MiB).
pub const MAX_PACK: u64 = 2 * 1024 * 1024 * 1024;

fn err(code: ErrorCode, msg: impl Into<String>) -> Error {
    Error::new(code, msg)
}

/// "Kiln can't reach Crucible": every network failure, in the player's words.
pub fn offline() -> Error {
    err(ErrorCode::Network, "Kiln can't reach Crucible. Ask your friend for the pack file (Share to Crucible > Save as file).")
}

// ================================================================================================= endpoints

/// Where Crucible is and which keys sign its indexes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Endpoints {
    /// The API's base, no trailing slash.
    pub api: String,
    /// The CDN's origin.
    pub cdn: String,
    /// Hosts upload URLs may point at: a host, or a suffix starting with a dot.
    pub upload_hosts: Vec<String>,
    pub keys: Vec<PublicKey>,
    /// (from, to) origins: a local service's own URLs (http://127.0.0.1:port) turned into the made-up https hosts
    /// [`dev::LocalTransport`] knows. Empty in production.
    pub rewrites: Vec<(String, String)>,
    /// Not the production service.
    pub dev: bool,
}

impl Endpoints {
    pub fn production() -> Endpoints {
        Endpoints {
            api: API.into(),
            cdn: CDN.into(),
            upload_hosts: vec![UPLOAD_SUFFIX.into()],
            keys: KEYS.iter().filter_map(|k| PublicKey::from_base64(k).ok()).collect(),
            rewrites: Vec::new(),
            dev: false,
        }
    }

    /// Crucible is usable: a signing key is known (none: the launcher hides Crucible).
    pub fn enabled(&self) -> bool {
        !self.keys.is_empty()
    }

    /// A URL the service sent, with a dev rewrite applied.
    pub fn rewrite(&self, url: &str) -> String {
        for (from, to) in &self.rewrites {
            if let Some(rest) = url.strip_prefix(from.as_str()) {
                return format!("{to}{rest}");
            }
        }
        url.to_string()
    }

    fn host_of(url: &str) -> Result<String> {
        Ok(net::policy::parse_url(url)?.host)
    }

    fn upload_host_ok(&self, host: &str) -> bool {
        self.upload_hosts.iter().any(|h| if h.starts_with('.') { host.ends_with(h.as_str()) && host.len() > h.len() } else { host == h })
    }
}

// ================================================================================================= service views

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct CrucibleUser {
    pub id: String,
    pub name: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub avatar: Option<String>,
}

/// A pack as the service describes it (packView).
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct CruciblePack {
    pub id: String,
    pub slug: String,
    pub code: String,
    pub url: String,
    pub title: String,
    pub summary: String,
    pub icon: Option<String>,
    pub owner: Option<CrucibleUser>,
    pub visibility: String,
    pub upvotes: u64,
    pub comments: u64,
    pub installs: u64,
    pub mods: u64,
    pub builds: Vec<u64>,
    pub latest: Option<String>,
}

/// A version as the service describes it (versionView).
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct CrucibleVersion {
    pub id: String,
    pub number: u64,
    pub version: String,
    pub changes: Option<String>,
    pub size: u64,
    pub published: Option<String>,
    pub status: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub checks: Option<Value>,
}

/// A presigned upload (storage.mjs plan()).
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Upload {
    pub id: String,
    pub sha256: String,
    pub role: String,
    pub size: u64,
    pub url: Option<String>,
    pub headers: BTreeMap<String, String>,
    pub multipart: bool,
    pub part_size: u64,
    pub parts: Vec<UploadPart>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct UploadPart {
    pub part_number: u32,
    pub url: String,
}

/// What `POST /quickshare` and `POST /packs/<id>/versions` answer.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct ShareAnswer {
    pub pack: Option<CruciblePack>,
    pub version: CrucibleVersion,
    pub uploads: Vec<Upload>,
    pub have: Vec<String>,
}

/// A code or a pack, with its latest ready version (or the one still uploading).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Located {
    pub pack: CruciblePack,
    pub version: Option<CrucibleVersion>,
    /// The creator's upload of the newest version is not finished: (that version, how far, 0..1).
    pub uploading: Option<(String, f64)>,
}

/// `GET /packs/<id>/head`.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Head {
    pub pack: String,
    pub title: String,
    pub version: Option<CrucibleVersion>,
}

// ================================================================================================= targets

/// What a player pasted or a link named.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Target {
    /// A share code, normalised (8 symbols, no dash, upper case).
    Code(String),
    /// A public pack's slug (or a pack id pck_...).
    Pack(String),
    /// A version id (pv_...).
    Version(String),
}

/// The share code's alphabet: no look-alikes (0/O, 1/I/L, ...), as Crucible's ids.mjs.
pub const CODE_ALPHABET: &str = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";

/// A code as typed ("h7qk 3mzp", "H7QK-3MZP"): 8 symbols of [`CODE_ALPHABET`], or None.
pub fn parse_code(text: &str) -> Option<String> {
    let s: String = text.chars().filter(|c| !matches!(c, '-' | ' ' | '_')).map(|c| c.to_ascii_uppercase()).collect();
    (s.len() == 8 && s.chars().all(|c| CODE_ALPHABET.contains(c))).then_some(s)
}

/// A code as shown: H7QK-3MZP.
pub fn show_code(code: &str) -> String {
    if code.len() == 8 { format!("{}-{}", &code[..4], &code[4..]) } else { code.to_string() }
}

fn slug_ok(s: &str) -> bool {
    !s.is_empty() && s.len() <= 80 && s.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-' || b == b'_')
}

/// Reads a code, a share link (https://frostdev.io/kiln/crucible/p/CODE), a public pack link
/// (https://frostdev.io/kiln/crucible/<slug>), a kiln:// link (kiln://crucible/CODE, kiln://crucible/pack/<slug>,
/// kiln://crucible/p/CODE) or a version id.
pub fn parse_target(text: &str) -> Option<Target> {
    let t = text.trim();
    if t.len() > 300 {
        return None;
    }
    if let Some(code) = parse_code(t) {
        return Some(Target::Code(code));
    }
    if t.starts_with("pv_") && t.len() <= 40 && t[3..].bytes().all(|b| b.is_ascii_alphanumeric()) {
        return Some(Target::Version(t.to_string()));
    }
    let rest = t
        .strip_prefix("kiln://crucible/")
        .or_else(|| t.strip_prefix("kiln:crucible/"))
        .or_else(|| t.strip_prefix("https://frostdev.io/kiln/crucible/"))
        .or_else(|| t.strip_prefix("https://www.frostdev.io/kiln/crucible/"))?;
    let rest = rest.split(['?', '#']).next().unwrap_or("").trim_end_matches('/');
    let parts: Vec<&str> = rest.split('/').collect();
    match parts.as_slice() {
        ["p", code] => parse_code(code).map(Target::Code),
        ["pack", slug] if slug_ok(slug) => Some(Target::Pack(slug.to_string())),
        [one] => parse_code(one).map(Target::Code).or_else(|| (slug_ok(one) && !matches!(*one, "api" | "oembed" | "copyright")).then(|| Target::Pack(one.to_string()))),
        _ => None,
    }
}

// ================================================================================================= the client

/// Crucible's API and CDN through the launcher's network layer.
pub struct Crucible<'a> {
    ep: &'a Endpoints,
    transport: &'a dyn Transport,
    session: Option<String>,
    owner_key: Option<String>,
    agent: String,
}

/// The service's refusal, in its own words (`{error, message}`), as a Kiln error.
fn refusal(status: u16, body: &[u8]) -> Error {
    let v: Value = serde_json::from_slice(body).unwrap_or(Value::Null);
    let kind = v.get("error").and_then(Value::as_str).unwrap_or("").to_string();
    let msg = v.get("message").and_then(Value::as_str).map(|s| s.chars().take(300).collect::<String>());
    let code = match (status, kind.as_str()) {
        (401, _) => ErrorCode::AuthFailed,
        (404, _) => ErrorCode::NotFound,
        (409, "uploading") => ErrorCode::Busy,
        // not ready to go public (credits, summary, checks) or hidden while reports are looked at: the message says why
        (409, "not-ready" | "under-review") => ErrorCode::Refused,
        (410, _) | (451, _) | (403, _) => ErrorCode::Refused,
        (413, _) => ErrorCode::Refused,
        (422, _) | (400, _) => ErrorCode::InvalidArgument,
        (429, _) => ErrorCode::Busy,
        _ => ErrorCode::Network,
    };
    let message = match (code, msg) {
        (ErrorCode::Network, _) => "Crucible is having trouble right now. Try again in a minute.".to_string(),
        (_, Some(m)) => m,
        (ErrorCode::AuthFailed, None) => "Sign in with Discord to share. Your friends don't need to.".into(),
        (ErrorCode::NotFound, None) => "This pack is no longer on Crucible.".into(),
        (_, None) => format!("Crucible refused that ({status})."),
    };
    let mut e = Error::new(code, message);
    if !v.is_null() {
        e = e.with_detail(serde_json::json!({ "status": status, "service": v }));
    }
    e
}

impl<'a> Crucible<'a> {
    /// `version`: the launcher's (the User-Agent is `kiln-launcher/<version>` and nothing else).
    pub fn new(ep: &'a Endpoints, transport: &'a dyn Transport, version: &str) -> Crucible<'a> {
        Crucible { ep, transport, session: None, owner_key: None, agent: format!("kiln-launcher/{version}") }
    }

    /// The relay session key (the player's Discord sign-in): sent to the API only.
    pub fn with_session(mut self, key: Option<String>) -> Crucible<'a> {
        self.session = key;
        self
    }

    pub fn endpoints(&self) -> &Endpoints {
        self.ep
    }

    pub fn signed_in(&self) -> bool {
        self.session.is_some()
    }

    fn api_client(&self) -> Result<net::Client<'a>> {
        let api = Endpoints::host_of(&self.ep.api)?;
        let cdn = Endpoints::host_of(&self.ep.cdn)?;
        let mut policy = net::Policy::new("Crucible").credential_host(&api);
        if cdn != api {
            policy = policy.plain_host(&cdn);
        }
        Ok(net::Client::new(policy, self.transport, &self.agent))
    }

    fn cdn_client(&self) -> Result<net::Client<'a>> {
        Ok(net::Client::new(net::Policy::new("Crucible files").plain_host(&Endpoints::host_of(&self.ep.cdn)?), self.transport, &self.agent))
    }

    /// One API call: JSON in, JSON out.
    pub fn call(&self, method: Method, path: &str, body: Option<&Value>, auth: bool) -> Result<Value> {
        let mut req = Request::new(method, format!("{}{path}", self.ep.api)).header("Accept", "application/json").max_response(4 * 1024 * 1024);
        if path.starts_with("/anonymous/") {
            let key = self.owner_key.as_deref().ok_or_else(|| err(ErrorCode::Refused, "This device does not hold the owner key for this link."))?;
            req = req.header("Authorization", &format!("CrucibleOwner {key}"));
        } else if auth {
            let key = self.session.as_deref().ok_or_else(|| err(ErrorCode::AuthFailed, "Sign in with Discord to update this account-owned share."))?;
            req = req.header("Authorization", &format!("Bearer {key}"));
        }
        if let Some(b) = body {
            req = req.json(b);
        } else if matches!(method, Method::Post | Method::Put | Method::Patch) {
            req = req.json(&serde_json::json!({}));
        }
        let resp = self.api_client()?.send(req).map_err(|e| if e.code == ErrorCode::Network || e.code == ErrorCode::Timeout { offline() } else { e })?;
        if !(200..300).contains(&resp.status) {
            return Err(refusal(resp.status, &resp.body));
        }
        serde_json::from_slice(&resp.body).map_err(|_| err(ErrorCode::Network, "Crucible sent an answer Kiln cannot read."))
    }

    fn get<T: for<'de> Deserialize<'de>>(&self, path: &str, auth: bool) -> Result<T> {
        let v = self.call(Method::Get, path, None, auth)?;
        serde_json::from_value(v).map_err(|_| err(ErrorCode::Network, "Crucible sent an answer Kiln cannot read."))
    }

    fn version_in(v: &Value) -> Option<CrucibleVersion> {
        v.get("version").filter(|x| x.is_object()).and_then(|x| serde_json::from_value(x.clone()).ok())
    }

    /// A pack by code, slug, pack id or version id.
    pub fn locate(&self, target: &Target) -> Result<Located> {
        let v = match target {
            Target::Code(c) => self.call(Method::Get, &format!("/codes/{}", show_code(c)), None, false)?,
            Target::Pack(s) => {
                let p = self.call(Method::Get, &format!("/packs/{s}"), None, self.session.is_some())?;
                let pack: CruciblePack = serde_json::from_value(p.get("pack").cloned().unwrap_or(Value::Null)).unwrap_or_default();
                let version = match &pack.latest {
                    Some(vid) => Some(self.call(Method::Get, &format!("/versions/{vid}"), None, false)?),
                    None => None,
                };
                serde_json::json!({ "pack": pack, "version": version.and_then(|x| x.get("version").cloned()) })
            }
            Target::Version(id) => self.call(Method::Get, &format!("/versions/{id}"), None, self.session.is_some())?,
        };
        let pack: CruciblePack = serde_json::from_value(v.get("pack").cloned().unwrap_or(Value::Null)).map_err(|_| err(ErrorCode::Network, "Crucible sent an answer Kiln cannot read."))?;
        if pack.id.is_empty() {
            return Err(err(ErrorCode::NotFound, "We couldn't find that code. Codes look like H7QK-3MZP."));
        }
        let mut version = Self::version_in(&v);
        let mut uploading = v.get("uploading").and_then(|u| {
            Some((u.get("version")?.as_str()?.to_string(), u.get("progress").and_then(progress_of).unwrap_or(0.0)))
        });
        if let Some(x) = &version
            && x.status == "uploading"
        {
            uploading = Some((x.id.clone(), v.get("progress").and_then(progress_of).unwrap_or(0.0)));
            version = None;
        }
        Ok(Located { pack, version, uploading })
    }

    /// The newest version of a pack (the update check, cached 5 minutes by the service).
    pub fn head(&self, pack_id: &str) -> Result<Head> {
        self.get(&format!("/packs/{pack_id}/head"), false)
    }

    /// A version's signed index, opened and checked (CDN first; the API's copy when the CDN has none yet).
    pub fn index(&self, version_id: &str) -> Result<(PackIndex, Value)> {
        if !version_id.starts_with("pv_") || version_id.len() > 40 || !version_id[3..].bytes().all(|b| b.is_ascii_alphanumeric()) {
            return Err(Error::invalid("not a Crucible version id"));
        }
        let req = Request::get(format!("{}/m/{version_id}.json", self.ep.cdn)).max_response(sign::MAX_ENVELOPE);
        let resp = self.cdn_client()?.send(req).map_err(|e| if e.code == ErrorCode::Network { offline() } else { e })?;
        let body = if resp.status == 200 {
            resp.body
        } else {
            let req = Request::get(format!("{}/versions/{version_id}/index", self.ep.api)).max_response(sign::MAX_ENVELOPE);
            let r = self.api_client()?.send(req).map_err(|e| if e.code == ErrorCode::Network { offline() } else { e })?;
            if r.status != 200 {
                return Err(refusal(r.status, &r.body));
            }
            r.body
        };
        let (ix, raw) = sign::open(&body, &self.ep.keys)?;
        if ix.crucible.as_ref().and_then(|c| c.version.as_deref()) != Some(version_id) {
            return Err(err(ErrorCode::HashMismatch, "This pack's index names another version. Nothing was installed."));
        }
        Ok((ix, raw))
    }

    /// The URL of a stored file on the CDN: the index's (after a dev rewrite) when it gives one, else the CDN's layout
    /// (`f/<sha>.zip`, `l/<sha>.zip`, `b/<sha>`). Refused unless it is on the CDN.
    fn file_url(&self, given: Option<&str>, role: &str, sha256: &str) -> Result<String> {
        let url = match given {
            Some(u) => self.ep.rewrite(u),
            None => match role {
                "mod" => format!("{}/f/{sha256}.zip", self.ep.cdn),
                "layer" => format!("{}/l/{sha256}.zip", self.ep.cdn),
                _ => format!("{}/b/{sha256}", self.ep.cdn),
            },
        };
        let host = Endpoints::host_of(&url)?;
        if host != Endpoints::host_of(&self.ep.cdn)? {
            return Err(err(ErrorCode::Refused, format!("A pack file is on {host}, not on Crucible's CDN. Nothing was downloaded.")));
        }
        Ok(url)
    }

    /// Downloads one stored file into `dest` (through a .part file), checking its size and sha256.
    pub fn download(&self, given_url: Option<&str>, role: &str, f: &StoredFile, dest: &Path, progress: &mut dyn FnMut(u64)) -> Result<()> {
        use sha2::Digest;
        use std::io::Write;
        if f.sha256.len() != 64 || !f.sha256.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)) {
            return Err(err(ErrorCode::HashMismatch, "A pack file is named by something that is no sha256. Nothing was downloaded."));
        }
        let url = self.file_url(given_url, role, &f.sha256)?;
        if let Some(d) = dest.parent() {
            std::fs::create_dir_all(d).map_err(crate::util::io_at(d, "creating"))?;
        }
        let part = crate::util::with_suffix(dest, ".part");
        struct Sink<'p> {
            file: std::fs::File,
            h: sha2::Sha256,
            n: u64,
            progress: &'p mut dyn FnMut(u64),
        }
        impl Write for Sink<'_> {
            fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
                self.file.write_all(b)?;
                self.h.update(b);
                self.n += b.len() as u64;
                (self.progress)(b.len() as u64);
                Ok(b.len())
            }
            fn flush(&mut self) -> std::io::Result<()> {
                self.file.flush()
            }
        }
        let file = std::fs::File::create(&part).map_err(crate::util::io_at(&part, "writing"))?;
        let mut sink = Sink { file, h: sha2::Sha256::new(), n: 0, progress };
        let result = (|| -> Result<()> {
            let resp = self.cdn_client()?.download(Request::get(url).max_response(f.size), &mut sink).map_err(|e| if e.code == ErrorCode::Network { offline() } else { e })?;
            if resp.status != 200 {
                return Err(err(ErrorCode::NotFound, "A file of this pack is missing on Crucible. Ask the creator to share it again."));
            }
            sink.file.sync_all()?;
            let have = crate::hash::hex(sink.h.clone().finalize().as_slice());
            if sink.n != f.size || have != f.sha256 {
                return Err(err(ErrorCode::HashMismatch, "A file of this pack didn't download correctly. Try again."));
            }
            Ok(())
        })();
        drop(sink);
        match result {
            Ok(()) => crate::util::rename_replace(&part, dest),
            Err(e) => {
                let _ = std::fs::remove_file(&part);
                Err(e)
            }
        }
    }

    /// Puts one file where Crucible's upload URL says, then tells Crucible it is done.
    pub fn upload(&self, u: &Upload, path: &Path, sent: &mut dyn FnMut(u64)) -> Result<()> {
        use std::io::{Seek, SeekFrom};
        let (have, size) = crate::hash::sha256_file(path)?;
        if have != u.sha256 || size != u.size {
            return Err(err(ErrorCode::HashMismatch, format!("{} changed since the share started; share again", path.display())));
        }
        let put = |url: &str, body: Body, headers: &BTreeMap<String, String>| -> Result<net::Response> {
            let url = self.ep.rewrite(url);
            let host = Endpoints::host_of(&url)?;
            if !self.ep.upload_host_ok(&host) {
                return Err(err(ErrorCode::Refused, format!("Crucible asked for an upload to {host}, which Kiln does not send files to. Nothing was sent.")));
            }
            let client = net::Client::new(net::Policy::new("Crucible uploads").plain_host(&host), self.transport, &self.agent);
            let mut req = Request::new(Method::Put, url).body(body).max_response(64 * 1024);
            for (k, v) in headers {
                if !k.eq_ignore_ascii_case("content-length") && !k.eq_ignore_ascii_case("host") {
                    req = req.header(k, v);
                }
            }
            let r = client.send(req).map_err(|e| if e.code == ErrorCode::Network { offline() } else { e })?;
            if !(200..300).contains(&r.status) {
                return Err(err(ErrorCode::Network, format!("Uploading to Crucible failed ({}). Try again.", r.status)));
            }
            Ok(r)
        };
        let mut parts = Vec::new();
        if u.multipart {
            let part_size = u.part_size.max(1);
            for p in &u.parts {
                let offset = (p.part_number as u64 - 1) * part_size;
                let len = part_size.min(size.saturating_sub(offset));
                let mut f = std::fs::File::open(path).map_err(crate::util::io_at(path, "reading"))?;
                f.seek(SeekFrom::Start(offset))?;
                let r = put(&p.url, Body::Reader { reader: Box::new(std::io::Read::take(f, len)), len }, &BTreeMap::new())?;
                let etag = r.header("etag").ok_or_else(|| err(ErrorCode::Network, "Crucible's storage did not confirm a part. Try again."))?.to_string();
                parts.push(serde_json::json!({ "partNumber": p.part_number, "etag": etag }));
                sent(len);
            }
        } else {
            let url = u.url.as_deref().ok_or_else(|| err(ErrorCode::Network, "Crucible sent an upload without an address."))?;
            let f = std::fs::File::open(path).map_err(crate::util::io_at(path, "reading"))?;
            put(url, Body::File { file: f, len: size }, &u.headers)?;
            sent(size);
        }
        let body = if parts.is_empty() { serde_json::json!({}) } else { serde_json::json!({ "parts": parts }) };
        let prefix = if self.owner_key.is_some() { "/anonymous" } else { "" };
        self.call(Method::Post, &format!("{prefix}/uploads/{}/complete", u.id), Some(&body), true)?;
        Ok(())
    }

    /// The anonymous install count (only when the player left "Count my installs" on).
    pub fn count_install(&self, version_id: &str) -> Result<()> {
        self.call(Method::Post, &format!("/versions/{version_id}/installed"), None, false).map(|_| ())
    }
}

fn progress_of(v: &Value) -> Option<f64> {
    v.as_f64().or_else(|| {
        let done = v.get("bytes").or_else(|| v.get("done")).and_then(Value::as_f64)?;
        let total = v.get("total").or_else(|| v.get("size")).and_then(Value::as_f64)?;
        Some(if total > 0.0 { done / total } else { 0.0 })
    }).or_else(|| v.get("fraction").and_then(Value::as_f64))
}

// ================================================================================================= local state

fn root(dirs: &DataDirs) -> PathBuf {
    dirs.root.join("crucible")
}

/// What a profile was shared as (`<data>/crucible/shared.json`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SharedPack {
    pub pack: CruciblePack,
    pub version: CrucibleVersion,
    /// The index as uploaded (the next share compares the profile with it).
    pub index: PackIndex,
    pub shared: String,
    /// Some files were still to upload when the share stopped: the next share sends them.
    #[serde(default)]
    pub pending: bool,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SharedFile {
    #[serde(default)]
    profiles: BTreeMap<String, SharedPack>,
}

fn shared_all(dirs: &DataDirs) -> SharedFile {
    crate::util::read_json(&root(dirs).join("shared.json")).ok().flatten().unwrap_or_default()
}

fn shared_save(dirs: &DataDirs, f: &SharedFile) -> Result<()> {
    crate::util::write_json_atomic(&root(dirs).join("shared.json"), f)
}

/// What changed in a profile since it was shared.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ShareChanges {
    pub added: Vec<String>,
    pub updated: Vec<String>,
    pub removed: Vec<String>,
}

impl ShareChanges {
    pub fn is_empty(&self) -> bool {
        self.added.is_empty() && self.updated.is_empty() && self.removed.is_empty()
    }
    /// "+1 added, 1 updated": the automatic change note of a new version.
    pub fn summary(&self) -> String {
        let mut out = Vec::new();
        for (n, word) in [(self.added.len(), "added"), (self.updated.len(), "updated"), (self.removed.len(), "removed")] {
            if n > 0 {
                out.push(format!("{n} {word}"));
            }
        }
        out.join(", ")
    }
}

/// The share of a profile as the Play page shows it.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SharedState {
    pub profile: String,
    pub pack: CruciblePack,
    pub version: CrucibleVersion,
    /// None: the profile matches what was shared.
    pub changed: Option<ShareChanges>,
    pub pending: bool,
    /// Existing account-owned shares still need their Discord sign-in to update.
    pub requires_sign_in: bool,
}

fn changes_since(profile: &crate::profile::Profile, ix: &PackIndex) -> ShareChanges {
    let mut c = ShareChanges::default();
    let mut now: BTreeMap<(ModKind, String), (&str, bool)> = BTreeMap::new();
    for kind in ModKind::ALL {
        for m in profile.mods.list(kind) {
            now.insert((kind, m.id.clone()), (m.hash.as_str(), m.enabled));
        }
    }
    for m in &ix.mods {
        match now.remove(&(m.kind, m.id.clone())) {
            None => c.removed.push(m.id.clone()),
            Some((hash, enabled)) => {
                if hash != m.hash || enabled != m.on(&BTreeMap::new()) {
                    c.updated.push(m.id.clone());
                }
            }
        }
    }
    c.added = now.into_keys().map(|(_, id)| id).collect();
    c
}

pub fn shared(dirs: &DataDirs, pid: &str) -> Result<Option<SharedState>> {
    let all = shared_all(dirs);
    let Some(rec) = all.profiles.get(pid) else { return Ok(None) };
    let profile = crate::profile::get(dirs, pid)?;
    let c = changes_since(&profile, &rec.index);
    Ok(Some(SharedState { profile: pid.into(), pack: rec.pack.clone(), version: rec.version.clone(), changed: (!c.is_empty()).then_some(c), pending: rec.pending, requires_sign_in: rec.pack.owner.as_ref().is_none_or(|o| !o.id.is_empty()) }))
}

/// A creator the player always trusts.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Trust {
    pub id: String,
    pub name: String,
    pub since: String,
}

pub fn trusted(dirs: &DataDirs) -> Vec<Trust> {
    crate::util::read_json::<Vec<Trust>>(&root(dirs).join("trust.json")).ok().flatten().unwrap_or_default()
}

pub fn set_trust(dirs: &DataDirs, id: &str, name: &str, on: bool) -> Result<()> {
    if id.is_empty() || id.len() > 24 || !id.bytes().all(|b| b.is_ascii_digit()) {
        return Err(Error::invalid("a creator is named by their Discord id"));
    }
    let mut all = trusted(dirs);
    all.retain(|t| t.id != id);
    if on {
        all.push(Trust { id: id.into(), name: name.chars().take(64).collect(), since: crate::util::now_rfc3339() });
    }
    crate::util::write_json_atomic(&root(dirs).join("trust.json"), &all)
}

// ================================================================================================= receiving

/// The pack preview (the screen before Install and play).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Preview {
    pub pack: CruciblePack,
    pub version: CrucibleVersion,
    pub index: PackIndex,
    /// Bytes of every part of the pack.
    pub size: u64,
    /// Bytes still to download (what the Library lacks).
    pub download: u64,
    pub missing: Vec<LibraryRef>,
    pub present: Vec<LibraryRef>,
    /// Mods that carry code (native, plugin, any other loader's mod, a Kiln mod with an entryAssembly when known).
    pub code: Vec<String>,
    pub optional: BTreeMap<String, bool>,
    /// An installed instance of this pack (the preview offers its update instead).
    pub instance: Option<String>,
    /// What that instance's profile loads from its installed version and the Library lacks ([`pack::lacking_of`]: an
    /// optional mod switched off and a server-only mod are not missing): the preview's "Download missing mods" ([`repair`]).
    /// None without an instance; `missing` and `download` above are of the version shown, for a new install.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub repair: Option<pack::Lacking>,
    pub game: GameFit,
    pub kiln: KilnFit,
    pub trusted: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GameFit {
    pub builds: Vec<u64>,
    pub current: Option<u64>,
    pub fits: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KilnFit {
    pub needs: Option<String>,
    pub ok: bool,
}

fn kiln_fit(ix: &PackIndex) -> KilnFit {
    let ok = match ix.kiln.as_deref() {
        None => true,
        Some(r) => match (crate::semver::VersionRange::try_parse(r), crate::semver::SemVersion::parse(crate::VERSION)) {
            (Ok(range), Ok(v)) => range.satisfies(&v),
            _ => true,
        },
    };
    KilnFit { needs: ix.kiln.clone(), ok }
}

fn game_fit(ix: &PackIndex, game: Option<&GameInstall>) -> GameFit {
    let builds = ix.game.as_ref().map(|g| g.builds.clone()).unwrap_or_default();
    let current = game.and_then(|g| g.build_id.as_deref()).and_then(|b| b.trim().parse().ok());
    let fits = builds.is_empty() || current.is_none_or(|c| builds.contains(&c));
    GameFit { builds, current, fits }
}

fn part_size(f: Option<&StoredFile>) -> u64 {
    f.map(|f| f.size).unwrap_or(0)
}

/// Opens a code or link: the pack, its newest version's signed index and what installing it would download.
pub fn preview(dirs: &DataDirs, c: &Crucible, target: &str, game: Option<&GameInstall>) -> Result<(Preview, Option<(String, f64)>)> {
    let t = parse_target(target).ok_or_else(|| err(ErrorCode::NotFound, "We couldn't find that code. Codes look like H7QK-3MZP."))?;
    let at = c.locate(&t)?;
    let Some(version) = at.version.clone() else {
        if let Some((vid, p)) = at.uploading.clone() {
            let owner = at.pack.owner.as_ref().map(|o| o.name.clone()).unwrap_or_else(|| "The creator".into());
            return Err(err(ErrorCode::Busy, format!("{owner}'s pack is still uploading ({}%). Try again when it's ready.", (p * 100.0).round() as u64))
                .with_detail(serde_json::json!({ "uploading": { "version": vid, "progress": p }, "pack": at.pack })));
        }
        return Err(err(ErrorCode::NotFound, "This pack is no longer on Crucible."));
    };
    let (index, _) = c.index(&version.id)?;
    Ok((preview_of(dirs, at.pack, version, index, game)?, at.uploading))
}

/// [`preview`] for an index already opened.
pub fn preview_of(dirs: &DataDirs, pack: CruciblePack, version: CrucibleVersion, index: PackIndex, game: Option<&GameInstall>) -> Result<Preview> {
    let lacking: BTreeMap<String, StoredFile> = pack::lacking_for(dirs, &index, &pack::client_skips)?.into_iter().collect();
    let mut missing = Vec::new();
    let mut present = Vec::new();
    let mut size = 0u64;
    let mut download = 0u64;
    let mut code = Vec::new();
    // what this game client takes: a server-only mod is neither downloaded nor installed here
    for m in index.mods.iter().filter(|m| !pack::client_skips(m)) {
        size = size.saturating_add(part_size(m.file.as_ref()));
        let lr = LibraryRef { kind: m.kind, id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone() };
        if let Some(f) = lacking.get(&m.id) {
            download = download.saturating_add(f.size);
            missing.push(lr.clone());
        } else {
            present.push(lr.clone());
        }
        let has_code = match m.kind {
            ModKind::Kiln => crate::library::path_of(dirs, &lr).ok().and_then(|p| crate::manifest::read_kiln(&p).ok()).is_some_and(|k| !k.entry_assembly.is_empty()),
            ModKind::Xhl => false,
            _ => true,
        };
        if has_code {
            code.push(m.id.clone());
        }
    }
    if let Some(l) = &index.layer {
        size = size.saturating_add(part_size(l.file.as_ref()));
        let lr = LibraryRef { kind: ModKind::Kiln, id: l.id.clone(), version: l.version.clone(), hash: l.hash.clone() };
        if let Some(f) = lacking.get(&l.id) {
            download = download.saturating_add(f.size);
            missing.push(lr);
        } else {
            present.push(lr);
        }
    }
    for o in &index.overrides {
        let n = o.size.unwrap_or(0);
        size = size.saturating_add(n);
        download = download.saturating_add(n);
    }
    let optional = index.mods.iter().filter(|m| m.dep == pack::Dep::Optional).map(|m| (m.id.clone(), m.default.unwrap_or(true))).collect();
    // a Crucible instance is this pack's when it came from this Crucible pack (another creator may use the same index id);
    // a pack installed from a file is matched by its index id
    let instance = pack::instances(dirs)?
        .into_iter()
        .find(|(_, i)| match i.crucible.as_ref().and_then(|x| x.pack.as_deref()) {
            Some(p) => p == pack.id,
            None => i.pack == index.id,
        })
        .map(|(p, _)| p);
    // an instance without its base (an older or broken install) has nothing to repair from
    let repair = instance.as_deref().and_then(|pid| pack::lacking_of(dirs, pid).ok());
    let trusted = pack.owner.as_ref().is_some_and(|o| trusted(dirs).iter().any(|t| t.id == o.id));
    Ok(Preview { game: game_fit(&index, game), kiln: kiln_fit(&index), pack, version, index, size, download, missing, present, code, optional, instance, repair, trusted })
}

/// Downloads what the Library lacks of a version and puts the .kilnpack together for this game client
/// (`<data>/crucible/packs/<vid>.client.kilnpack`; reused while it is there): mods only a server uses are neither
/// downloaded nor put in (`pack::client_skips`). Progress 0..1 over the bytes to download.
pub fn fetch(dirs: &DataDirs, c: &Crucible, version_id: &str, progress: &mut dyn Progress) -> Result<(PackIndex, pack::Assembled, u64)> {
    progress.report(Some(0.0), "reading the pack");
    let (ix, raw) = c.index(version_id)?;
    let out = root(dirs).join("packs").join(format!("{version_id}.client.{}", pack::EXTENSION));
    if out.is_file()
        && let Ok(r) = pack::read_partial(&out, None, None, &pack::client_skips)
        && r.index == ix
    {
        return Ok((ix, pack::Assembled { file: out, sha256: r.sha256, size: r.size, ..Default::default() }, 0));
    }
    let lacking = pack::lacking_for(dirs, &ix, &pack::client_skips)?;
    // saturating: sizes come from the index, and a wrapped sum would pass the limit
    let total: u64 = lacking.iter().map(|(_, f)| f.size).chain(ix.overrides.iter().map(|o| o.size.unwrap_or(0))).fold(0u64, u64::saturating_add);
    if total > MAX_PACK {
        return Err(err(ErrorCode::Refused, "This pack is larger than Crucible allows (2 GB)."));
    }
    let mut got = 0u64;
    let (archives, _) = download_parts(dirs, c, &ix, &raw, &lacking, total, &mut got, progress)?;
    let mut blobs = BTreeMap::new();
    let raw_overrides: Vec<Value> = raw.get("overrides").and_then(Value::as_array).cloned().unwrap_or_default();
    for o in &ix.overrides {
        let dest = root(dirs).join("cache").join("b").join(&o.sha256);
        let f = StoredFile { sha256: o.sha256.clone(), size: o.size.unwrap_or(0), url: None };
        let url = raw_overrides.iter().find(|x| x.get("sha256").and_then(Value::as_str) == Some(&o.sha256)).and_then(|x| x.get("url")).and_then(Value::as_str);
        if !(dest.is_file() && crate::hash::sha256_file(&dest).is_ok_and(|(h, _)| h == o.sha256)) {
            if o.size.is_none() {
                return Err(err(ErrorCode::Schema, format!("override {}: the index gives no size", o.path)));
            }
            c.download(url, "blob", &f, &dest, &mut |n| got += n)?;
        }
        blobs.insert(o.sha256.clone(), std::fs::read(&dest).map_err(crate::util::io_at(&dest, "reading"))?);
    }
    progress.report(Some(0.85), "checking the pack");
    let done = pack::assemble_for(dirs, &ix, &pack::client_skips, &archives, &blobs, &out)?;
    progress.report(Some(0.9), "pack checked");
    Ok((ix, done, got))
}

/// Downloads the stored archives of `parts` ((mod or layer id, its file) of the signed index `ix`; `raw`: the index as
/// signed, which names each archive's address) into the cache, each checked against its sha256 and size (a cached one
/// that still is is not downloaded again): (the sha256 -> the archive, the bytes downloaded). `got` counts the bytes of
/// every part, cached or not (progress 0..0.8 of `total`).
#[allow(clippy::too_many_arguments)]
fn download_parts(dirs: &DataDirs, c: &Crucible, ix: &PackIndex, raw: &Value, parts: &[(String, StoredFile)], total: u64, got: &mut u64, progress: &mut dyn Progress)
    -> Result<(BTreeMap<String, PathBuf>, u64)> {
    let mut archives = BTreeMap::new();
    let mut fetched = 0u64;
    // the part with this id AND this stored file: two kinds of mod may share an id (each has its own file)
    let urls = |id: &str, sha: &str| -> Option<String> {
        let mods = raw.get("mods")?.as_array()?;
        let m = mods.iter().find(|m| {
            m.get("id").and_then(Value::as_str) == Some(id) && m.get("file").and_then(|f| f.get("sha256")).and_then(Value::as_str) == Some(sha)
        });
        let f = match m {
            Some(m) => m.get("file"),
            None => raw.get("layer").and_then(|l| l.get("file")),
        }?;
        f.get("url")?.as_str().map(str::to_string)
    };
    for (id, f) in parts {
        if progress.canceled() {
            return Err(err(ErrorCode::Canceled, "canceled"));
        }
        let role = if ix.layer.as_ref().is_some_and(|l| &l.id == id) { "layer" } else { "mod" };
        let dest = root(dirs).join("cache").join(if role == "layer" { "l" } else { "f" }).join(format!("{}.zip", f.sha256));
        let cached = dest.is_file() && crate::hash::sha256_file(&dest).is_ok_and(|(h, n)| h == f.sha256 && n == f.size);
        if !cached {
            let name = ix.mods.iter().find(|m| &m.id == id && m.file.as_ref().is_some_and(|x| x.sha256 == f.sha256)).and_then(|m| m.name.clone()).unwrap_or_else(|| id.clone());
            progress.report(Some(*got as f32 / total.max(1) as f32 * 0.8), &format!("downloading {name}"));
            let mut tick = |n: u64| {
                *got += n;
            };
            c.download(urls(id, &f.sha256).as_deref(), role, f, &dest, &mut tick).map_err(|e| {
                if e.code == ErrorCode::HashMismatch { err(ErrorCode::HashMismatch, format!("{name} didn't download correctly. Try again.")) } else { e }
            })?;
            fetched = fetched.saturating_add(f.size);
        } else {
            *got += f.size;
        }
        archives.insert(f.sha256.clone(), dest);
    }
    Ok((archives, fetched))
}

/// Install and play from Crucible.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct InstallRequest {
    /// The version (pv_...) the preview showed.
    pub target: String,
    pub consent: bool,
    #[serde(default)]
    pub trust_always: bool,
    #[serde(default)]
    pub optional: BTreeMap<String, bool>,
    /// The new profile's name (the preview's name field). Default "<pack> (<creator>'s pack)"; either way a name a
    /// profile has already gets " (2)" (profile::unique_name).
    #[serde(default)]
    pub name: Option<String>,
    /// Activate the instance for the game (the launcher then starts it). Default true. False: "Install" (no save
    /// backup, no activation, nothing written in the game folder: allowed while the game runs).
    #[serde(default)]
    pub play: Option<bool>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct InstallResult {
    pub profile: crate::profile::Profile,
    pub instance: pack::Instance,
    pub imported: Vec<LibraryRef>,
    pub overrides: Vec<String>,
    /// The profile active before (Undo).
    pub previous: Option<String>,
    /// Bytes downloaded.
    pub downloaded: u64,
}

struct Scaled<'p> {
    inner: &'p mut dyn Progress,
    from: f32,
    to: f32,
}

impl Progress for Scaled<'_> {
    fn report(&mut self, fraction: Option<f32>, message: &str) {
        self.inner.report(fraction.map(|f| self.from + f.clamp(0.0, 1.0) * (self.to - self.from)), message);
    }
    fn canceled(&self) -> bool {
        self.inner.canceled()
    }
}

/// Downloads, checks and installs a version as an instance. `owner`: the creator (from the preview), remembered as
/// always trusted when the player asked. `count`: send the anonymous install count.
pub fn install(dirs: &DataDirs, c: &Crucible, game: Option<&GameInstall>, req: &InstallRequest, owner: Option<&CrucibleUser>, count: bool, progress: &mut dyn Progress) -> Result<InstallResult> {
    if !req.consent {
        return Err(err(ErrorCode::ConsentRequired, "This pack contains mods, including code, from other people: confirm first."));
    }
    let previous = game.and_then(|g| crate::profile::active(dirs, g).ok().flatten()).map(|a| a.profile_id);
    let (ix, done, downloaded) = fetch(dirs, c, &req.target, &mut Scaled { inner: progress, from: 0.0, to: 0.6 })?;
    if !kiln_fit(&ix).ok {
        return Err(err(ErrorCode::Unsupported, format!("This pack needs Kiln {}. Update Kiln first.", ix.kiln.clone().unwrap_or_default())));
    }
    let r = pack::install(
        dirs,
        game,
        &pack::InstallRequest {
            source: ModpackSource::Path { path: done.file.clone(), sha256: Some(done.sha256.clone()) },
            consent: true,
            allow_http: false,
            // named after the pack and its creator ("My mods (Felup's pack)"), and never a name a profile has already
            // (pack::install): the sharer's profile name alone is often the player's own ("My mods")
            name: req.name.clone().filter(|n| !n.trim().is_empty()).or_else(|| owner.filter(|o| !o.name.trim().is_empty()).map(|o| format!("{} ({}'s pack)", ix.name, o.name.trim()))),
            optional: req.optional.clone(),
            // false: Install without playing. Only the data folder changes (Library, the new profile): no save backup,
            // no activation, nothing in the game folder, so it may run while the game does
            activate: Some(game.is_some() && req.play != Some(false)),
        },
        &mut Scaled { inner: progress, from: 0.6, to: 1.0 },
    )?;
    if req.trust_always
        && let Some(o) = owner.filter(|o| !o.id.is_empty())
    {
        let _ = set_trust(dirs, &o.id, &o.name, true);
    }
    if count {
        let _ = c.count_install(&req.target);
    }
    Ok(InstallResult { profile: r.profile, instance: r.instance, imported: r.imported, overrides: r.overrides, previous, downloaded })
}

/// "Download missing mods": downloads from Crucible what an instance's profile loads from its installed version and the
/// Library lacks ([`pack::lacking_of`]), the archives of that version only, each checked against its sha256 as an install
/// does; puts them together with the layer and the installed overrides as a partial pack ([`pack::assemble_for`]: every
/// kdh1 checked) and imports them into the Library ([`pack::repair_from`]). The instance keeps its version (an update
/// waiting stays an update: the update screen), its profile and settings; nothing is activated or started, nothing in a
/// game folder changes, so it may run while the game does. Nothing missing: nothing is asked of Crucible.
pub fn repair(dirs: &DataDirs, c: &Crucible, pid: &str, progress: &mut dyn Progress) -> Result<pack::RepairResult> {
    let inst = pack::instance(dirs, pid)?.ok_or_else(|| Error::not_found(format!("profile {pid} is no Crucible pack")))?;
    let vid = inst.crucible.as_ref().and_then(|x| x.version.clone()).ok_or_else(|| Error::not_found("this pack was not installed from Crucible"))?;
    let parts = pack::repair_parts(dirs, pid)?;
    if parts.lacking.is_empty() {
        progress.report(Some(1.0), "nothing is missing");
        return Ok(pack::RepairResult { profile: pid.into(), version: inst.version, imported: Vec::new(), downloaded: 0, left: Vec::new() });
    }
    progress.report(Some(0.0), "reading the pack");
    let (ix, raw) = c.index(&vid)?;
    same_crucible_pack(&inst, &ix)?;
    if ix.version != parts.index.version {
        return Err(err(ErrorCode::Refused, "Crucible's copy of this version is not the one installed."));
    }
    // the archives as the signed index names them (the same sha256 as the installed index; refused below otherwise)
    let mut lacking = Vec::new();
    for (id, f) in &parts.lacking {
        // the part with this id AND this stored file (two kinds of mod may share an id)
        let signed = ix.mods.iter().filter(|m| &m.id == id).filter_map(|m| m.file.clone())
            .chain(ix.layer.iter().filter(|l| &l.id == id).filter_map(|l| l.file.clone()))
            .find(|s| s.sha256 == f.sha256)
            .ok_or_else(|| err(ErrorCode::Refused, format!("{id}: Crucible's copy of this version is not the one installed")))?;
        lacking.push((id.clone(), signed));
    }
    let total = lacking.iter().map(|(_, f)| f.size).fold(0u64, u64::saturating_add);
    let mut got = 0u64;
    let (archives, fetched) = download_parts(dirs, c, &ix, &raw, &lacking, total, &mut got, &mut Scaled { inner: progress, from: 0.0, to: 0.7 })?;
    progress.report(Some(0.6), "checking the files");
    let out = root(dirs).join("packs").join(format!("{vid}.repair.{}", pack::EXTENSION));
    let done = pack::assemble_for(dirs, &ix, &parts.skip(), &archives, &parts.blobs, &out);
    let r = done.and_then(|a| pack::repair_from(dirs, pid, &a.file, Some(&a.sha256), &mut Scaled { inner: progress, from: 0.6, to: 1.0 }));
    let _ = std::fs::remove_file(&out);
    Ok(pack::RepairResult { downloaded: fetched, ..r? })
}

/// An instance whose pack has a newer version on Crucible.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateAvailable {
    pub profile: String,
    pub pack: String,
    pub name: String,
    pub from: UpdateFrom,
    pub to: UpdateTo,
    /// The creator's name (the banner's "by Emberfox").
    pub by: Option<String>,
    /// What the new version changes in mods (from the signed indexes, nothing downloaded): the banner's counts.
    pub counts: Option<UpdateCounts>,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateCounts {
    pub added: usize,
    pub updated: usize,
    pub removed: usize,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateFrom {
    pub version: String,
    pub number: Option<u64>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateTo {
    pub version_id: String,
    pub version: String,
    pub number: u64,
}

/// Every Crucible instance with a newer version (one `head` call per pack; a pack gone or unreachable is skipped).
pub fn updates(dirs: &DataDirs, c: &Crucible) -> Result<Vec<UpdateAvailable>> {
    let mut out = Vec::new();
    for (pid, inst) in pack::instances(dirs)? {
        let Some(cr) = &inst.crucible else { continue };
        let (Some(pack_id), Some(vid)) = (cr.pack.as_deref(), cr.version.as_deref()) else { continue };
        let head = match c.head(pack_id) {
            Ok(h) => h,
            Err(e) if e.code == ErrorCode::Network => return Err(e),
            Err(_) => continue,
        };
        let Some(v) = head.version else { continue };
        if v.id != vid {
            let base: Option<PackIndex> = crate::util::read_json(&dirs.profile(&pid).join(pack::INSTANCE_DIR).join("base").join(crate::modpack::MANIFEST)).ok().flatten();
            let counts = base.zip(c.index(&v.id).ok()).map(|(old, (new, _))| {
                let d = pack::diff(&old, &new);
                UpdateCounts { added: d.added.len(), updated: d.updated.len(), removed: d.removed.len() }
            });
            let by = c.get::<Value>(&format!("/packs/{pack_id}"), false).ok()
                .and_then(|p| p.get("pack")?.get("owner")?.get("name")?.as_str().map(str::to_string));
            out.push(UpdateAvailable {
                profile: pid,
                pack: pack_id.into(),
                name: inst.name.clone(),
                from: UpdateFrom { version: inst.version.clone(), number: None },
                to: UpdateTo { version_id: v.id, version: v.version, number: v.number },
                by,
                counts,
            });
        }
    }
    Ok(out)
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CrucibleUpdatePlan {
    pub version_id: String,
    pub download: u64,
    pub plan: pack::UpdatePlan,
}

/// The newest version of an instance's pack, downloaded and planned (nothing written to the profile).
pub fn update_plan(dirs: &DataDirs, c: &Crucible, pid: &str, progress: &mut dyn Progress) -> Result<CrucibleUpdatePlan> {
    let inst = pack::instance(dirs, pid)?.ok_or_else(|| Error::not_found(format!("profile {pid} is no Crucible pack")))?;
    let pack_id = inst.crucible.as_ref().and_then(|x| x.pack.clone()).ok_or_else(|| Error::not_found("this pack was not installed from Crucible"))?;
    let head = c.head(&pack_id)?;
    let v = head.version.ok_or_else(|| err(ErrorCode::NotFound, "This pack is no longer on Crucible."))?;
    let (ix, done, download) = fetch(dirs, c, &v.id, progress)?;
    same_crucible_pack(&inst, &ix)?;
    let plan = pack::plan_update(dirs, pid, &done.file, Some(&done.sha256), &BTreeMap::new())?;
    Ok(CrucibleUpdatePlan { version_id: v.id, download, plan })
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateRequest {
    pub profile: String,
    pub version_id: String,
    pub consent: bool,
    #[serde(default)]
    pub optional: BTreeMap<String, bool>,
    #[serde(default)]
    pub resolve: BTreeMap<String, crate::modpack::merge::Pick>,
    #[serde(default)]
    pub all: Option<crate::modpack::merge::Pick>,
    /// Player-layer fields to let go ("Drop my change"): `plan.local.overlaps[].key`.
    #[serde(default)]
    pub drop: Vec<String>,
}

/// A version may update an instance only when it is of the same pack: the same index id and, for an instance installed
/// from Crucible, the same Crucible pack (the index id is the creator's choice; two creators can pick the same one).
fn same_crucible_pack(inst: &pack::Instance, ix: &PackIndex) -> Result<()> {
    let want = inst.crucible.as_ref().and_then(|x| x.pack.as_deref());
    let got = ix.crucible.as_ref().and_then(|x| x.pack.as_deref());
    if ix.id != inst.pack || (want.is_some() && got != want) {
        return Err(err(ErrorCode::Refused, "That version belongs to another pack."));
    }
    Ok(())
}

pub fn update(dirs: &DataDirs, c: &Crucible, req: &UpdateRequest, progress: &mut dyn Progress) -> Result<pack::UpdateResult> {
    if !req.consent {
        return Err(err(ErrorCode::ConsentRequired, "This pack contains mods, including code, from other people: confirm first."));
    }
    let inst = pack::instance(dirs, &req.profile)?.ok_or_else(|| Error::not_found(format!("profile {} is no Crucible pack", req.profile)))?;
    let (ix, done, _) = fetch(dirs, c, &req.version_id, &mut Scaled { inner: progress, from: 0.0, to: 0.6 })?;
    same_crucible_pack(&inst, &ix)?;
    pack::update(
        dirs,
        &pack::UpdateRequest {
            profile: req.profile.clone(),
            source: ModpackSource::Path { path: done.file.clone(), sha256: Some(done.sha256.clone()) },
            consent: true,
            allow_http: false,
            optional: req.optional.clone(),
            resolve: req.resolve.clone(),
            all: req.all,
            drop: req.drop.clone(),
        },
        &mut Scaled { inner: progress, from: 0.6, to: 1.0 },
    )
}

// ================================================================================================= sharing

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ShareRequest {
    pub profile: String,
    /// The pack's name on Crucible (default: the profile's).
    #[serde(default)]
    pub title: Option<String>,
    /// Settings files to include ("include my settings"; reviewed by the build's allowlist and secret scan).
    #[serde(default)]
    pub overrides: Vec<pack::OverrideSpec>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ShareResult {
    pub pack: CruciblePack,
    pub version: CrucibleVersion,
    /// Files in the pack.
    pub files: usize,
    /// Bytes sent.
    pub uploaded: u64,
    /// Bytes Crucible had already.
    pub skipped: u64,
    /// The link was copied (by the UI; always false here).
    pub copied: bool,
}

/// A local file for each (role, sha256) of an upload index.
pub(crate) struct ShareFiles {
    pub(crate) index: PackIndex,
    pub(crate) paths: BTreeMap<String, PathBuf>,
    pub(crate) total: u64,
}

fn next_version(prev: Option<&str>) -> String {
    match prev.and_then(|p| crate::semver::SemVersion::parse(p).ok()) {
        Some(v) => format!("{}.{}.{}", v.major, v.minor, v.patch + 1),
        None => "1.0.0".into(),
    }
}

/// Builds the pack of a profile and the archives Crucible stores (one per mod, the layer, each override).
fn prepare(dirs: &DataDirs, req: &ShareRequest, rec: Option<&SharedPack>, game: Option<&GameInstall>, progress: &mut dyn Progress) -> Result<ShareFiles> {
    let out = root(dirs).join("out");
    let builds: Vec<u64> = game.and_then(|g| g.build_id.as_deref()).and_then(|b| b.trim().parse().ok()).into_iter().collect();
    let changes = rec.and_then(|r| crate::profile::get(dirs, &req.profile).ok().map(|p| changes_since(&p, &r.index).summary())).filter(|s| !s.is_empty());
    let built = pack::build(
        dirs,
        &pack::BuildRequest {
            profile: req.profile.clone(),
            id: rec.map(|r| r.index.id.clone()),
            name: req.title.clone().filter(|t| !t.trim().is_empty()),
            version: next_version(rec.map(|r| r.index.version.as_str())),
            changes,
            overrides: req.overrides.clone(),
            game_builds: builds,
            ..Default::default()
        },
        &out,
        &mut Scaled { inner: progress, from: 0.0, to: 0.2 },
    )?;
    let result = archives(dirs, &built, progress);
    let _ = std::fs::remove_file(&built.file);
    result
}

/// The archives Crucible stores for a built pack (one per mod, the layer, each override), cached by sha256, and the
/// upload index naming them.
pub(crate) fn archives(dirs: &DataDirs, built: &pack::BuiltPack, progress: &mut dyn Progress) -> Result<ShareFiles> {
    let out = root(dirs).join("out");
    (|| -> Result<ShareFiles> {
        let mut ix = built.index.clone();
        let mut paths = BTreeMap::new();
        let mut total = 0u64;
        let cache = root(dirs).join("cache");
        let n = ix.mods.len().max(1);
        for (i, m) in ix.mods.iter_mut().enumerate() {
            progress.report(Some(0.2 + i as f32 / n as f32 * 0.1), &format!("packing {}", m.name.clone().unwrap_or_else(|| m.id.clone())));
            let tmp = cache.join("f").join(format!("new-{}-{}.zip", m.kind.as_str(), crate::hash::hex(&crate::util::random_bytes::<4>()?)));
            let lr = LibraryRef { kind: m.kind, id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone() };
            let f = pack::mod_archive(dirs, &lr, &tmp)?;
            let dest = cache.join("f").join(format!("{}.zip", f.sha256));
            crate::util::rename_replace(&tmp, &dest)?;
            total += f.size;
            paths.insert(format!("mod:{}", f.sha256), dest);
            m.file = Some(f);
        }
        let read = if ix.layer.is_some() {
            let staging = crate::library::TempDir::new(&out, "share-layer")?;
            let r = pack::read(&built.file, Some(&built.sha256), Some(staging.path()))?;
            let tmp = cache.join("l").join(format!("new-{}.zip", crate::hash::hex(&crate::util::random_bytes::<4>()?)));
            let (f, hash) = pack::folder_archive(ModKind::Kiln, &staging.path().join(pack::LAYER_PATH), &tmp)?;
            if Some(&hash) != ix.layer.as_ref().map(|l| &l.hash) {
                return Err(err(ErrorCode::Internal, "the pack layer's archive hashes differently from its index"));
            }
            let dest = cache.join("l").join(format!("{}.zip", f.sha256));
            crate::util::rename_replace(&tmp, &dest)?;
            total += f.size;
            paths.insert(format!("layer:{}", f.sha256), dest);
            if let Some(l) = ix.layer.as_mut() {
                l.file = Some(f);
            }
            r
        } else {
            pack::read(&built.file, Some(&built.sha256), None)?
        };
        for o in ix.overrides.iter_mut() {
            let bytes = read.overrides.get(&(o.side, o.path.clone())).ok_or_else(|| err(ErrorCode::Internal, format!("override {} is not in the pack", o.path)))?;
            let dest = cache.join("b").join(&o.sha256);
            if let Some(d) = dest.parent() {
                std::fs::create_dir_all(d).map_err(crate::util::io_at(d, "creating"))?;
            }
            crate::util::write_atomic(&dest, bytes)?;
            o.size = Some(bytes.len() as u64);
            total += bytes.len() as u64;
            paths.insert(format!("blob:{}", o.sha256), dest);
        }
        Ok(ShareFiles { index: ix, paths, total })
    })()
}

/// Sends what Crucible asked for (progress 0.35..0.95); the bytes it asked for.
pub(crate) fn send(c: &Crucible, uploads: &[Upload], files: &ShareFiles, canceled: &str, progress: &mut dyn Progress) -> Result<u64> {
    let to_send: u64 = uploads.iter().map(|u| u.size).sum();
    let mut sent = 0u64;
    for u in uploads {
        if progress.canceled() {
            return Err(err(ErrorCode::Canceled, canceled));
        }
        let path = files.paths.get(&format!("{}:{}", u.role, u.sha256)).ok_or_else(|| err(ErrorCode::Internal, format!("Crucible asked for a file this pack does not have ({})", u.sha256)))?;
        progress.report(Some(0.35 + sent as f32 / to_send.max(1) as f32 * 0.6), &format!("uploading {} of {}", crate::util::human_size(sent), crate::util::human_size(to_send)));
        c.upload(u, path, &mut |n| sent += n)?;
    }
    Ok(to_send)
}

/// Every file of an upload index for `POST /uploads`.
fn file_list(ix: &PackIndex) -> Vec<Value> {
    let mut v: Vec<Value> = ix.mods.iter().filter_map(|m| m.file.as_ref()).map(|f| serde_json::json!({ "sha256": f.sha256, "size": f.size, "role": "mod" })).collect();
    if let Some(f) = ix.layer.as_ref().and_then(|l| l.file.as_ref()) {
        v.push(serde_json::json!({ "sha256": f.sha256, "size": f.size, "role": "layer" }));
    }
    for o in &ix.overrides {
        v.push(serde_json::json!({ "sha256": o.sha256, "size": o.size.unwrap_or(0), "role": "blob" }));
    }
    v
}

/// Share to Crucible: the profile as a link-only pack (or the next version of the pack it was shared as), only the files
/// Crucible lacks uploaded. New signed-out shares use a native-only owner capability; existing account shares keep
/// their authenticated route. Neither creating an anonymous share nor receiving it needs a Discord session.
pub fn share(dirs: &DataDirs, c: &Crucible, req: &ShareRequest, game: Option<&GameInstall>, progress: &mut dyn Progress) -> Result<ShareResult> {
    static SHARING: std::sync::Mutex<()> = std::sync::Mutex::new(());
    let _lock = SHARING.lock().map_err(|_| err(ErrorCode::Busy, "Another share stopped unexpectedly. Restart Kiln and try again."))?;
    let mut all = shared_all(dirs);
    let rec = all.profiles.get(&req.profile).cloned();
    let anonymous = rec.as_ref().map(|r| r.pack.owner.as_ref().is_some_and(|o| o.id.is_empty())).unwrap_or(!c.signed_in());
    let mut owner = if anonymous {
        let kept = ownership::load(dirs, &c.ep.api, &req.profile)?;
        let kept = kept.filter(|o| o.pack.as_deref() == rec.as_ref().map(|r| r.pack.id.as_str()));
        match kept {
            Some(o) => Some(o),
            None if rec.is_some() => return Err(err(ErrorCode::Refused, "This device no longer holds the owner key for this link. The link still works for receivers.")),
            None => {
                let o = ownership::Owner { pack: None, key: crate::hash::hex(&crate::util::random_bytes::<32>()?) };
                // Persist before the first request, so a failed local write never creates an unmanageable share.
                ownership::retire(dirs, &c.ep.api, &req.profile)?;
                ownership::save(dirs, &c.ep.api, &req.profile, &o)?;
                Some(o)
            }
        }
    } else {
        if !c.signed_in() { return Err(err(ErrorCode::AuthFailed, "Sign in with Discord to update this account-owned share.")); }
        None
    };
    let scoped = Crucible { ep: c.ep, transport: c.transport, session: if anonymous { None } else { c.session.clone() }, owner_key: owner.as_ref().map(|o| o.key.clone()), agent: c.agent.clone() };
    let c = &scoped;
    let prefix = if anonymous { "/anonymous" } else { "" };
    let mut expired_fallback = None;
    let profile = crate::profile::get(dirs, &req.profile)?;
    // the same profile, unchanged, with files still to send: finish that share
    let resume = rec.as_ref().filter(|r| r.pending && changes_since(&profile, &r.index).is_empty());
    let (files, answer) = if let Some(r) = resume {
        // the same profile, unchanged, zips to the same archives (they are deterministic): only their paths are needed
        let files = prepare(dirs, req, None, game, progress)?;
        let plan = if anonymous {
            c.call(Method::Post, &format!("/anonymous/versions/{}/uploads", r.version.id), Some(&serde_json::json!({})), true)?
        } else {
            c.call(Method::Post, "/uploads", Some(&serde_json::json!({ "files": file_list(&r.index) })), true)?
        };
        let mut answer: ShareAnswer = serde_json::from_value(plan).map_err(|_| err(ErrorCode::Network, "Crucible sent an answer Kiln cannot read."))?;
        answer.pack = Some(r.pack.clone());
        answer.version = r.version.clone();
        (ShareFiles { index: r.index.clone(), paths: files.paths, total: files.total }, answer)
    } else {
        let files = prepare(dirs, req, rec.as_ref(), game, progress)?;
        let body = serde_json::json!({ "index": files.index, "title": req.title.clone().filter(|t| !t.trim().is_empty()).unwrap_or_else(|| files.index.name.clone()) });
        progress.report(Some(0.32), "asking Crucible for upload slots");
        let answer = match &rec {
            Some(r) => match c.call(Method::Post, &format!("{prefix}/packs/{}/versions", r.pack.id), Some(&serde_json::json!({ "index": files.index, "changes": files.index.changes })), true) {
                Ok(v) => {
                    let mut a: ShareAnswer = serde_json::from_value(v).map_err(|_| err(ErrorCode::Network, "Crucible sent an answer Kiln cannot read."))?;
                    a.pack = Some(r.pack.clone());
                    a
                }
                // the pack was deleted or expired on Crucible: share it as a new one
                Err(e) if !anonymous && matches!(e.code, ErrorCode::NotFound | ErrorCode::Refused) => {
                    serde_json::from_value(c.call(Method::Post, "/quickshare", Some(&body), true)?).map_err(|_| err(ErrorCode::Network, "Crucible sent an answer Kiln cannot read."))?
                }
                Err(e) => return Err(e),
            },
            None => {
                let value = match c.call(Method::Post, &format!("{prefix}/quickshare"), Some(&body), true) {
                    Ok(v) => v,
                    // Only a rejected session on a NEW private link may fall back. Bans, moderation, budgets and
                    // all existing account-owned updates keep their refusal; no anonymous reset of an old pack.
                    Err(e) if !anonymous && e.code == ErrorCode::AuthFailed => {
                        let o = ownership::Owner { pack: None, key: crate::hash::hex(&crate::util::random_bytes::<32>()?) };
                        ownership::retire(dirs, &c.ep.api, &req.profile)?;
                        ownership::save(dirs, &c.ep.api, &req.profile, &o)?;
                        let ac = Crucible { ep: c.ep, transport: c.transport, session: None, owner_key: Some(o.key.clone()), agent: c.agent.clone() };
                        let v = ac.call(Method::Post, "/anonymous/quickshare", Some(&body), true)?;
                        owner = Some(o);
                        expired_fallback = Some(ac);
                        v
                    }
                    Err(e) => return Err(e),
                };
                serde_json::from_value(value).map_err(|_| err(ErrorCode::Network, "Crucible sent an answer Kiln cannot read."))?
            },
        };
        (files, answer)
    };
    let c = expired_fallback.as_ref().unwrap_or(c);
    let pack_view = answer.pack.clone().ok_or_else(|| err(ErrorCode::Network, "Crucible sent an answer Kiln cannot read."))?;
    if let Some(o) = owner.as_mut() {
        o.pack = Some(pack_view.id.clone());
        ownership::save(dirs, &c.ep.api, &req.profile, o)?;
    }
    // remember it at once: the link works when the upload finishes, and a stopped upload resumes from here
    all.profiles.insert(req.profile.clone(), SharedPack { pack: pack_view.clone(), version: answer.version.clone(), index: files.index.clone(), shared: crate::util::now_rfc3339(), pending: !answer.uploads.is_empty() });
    shared_save(dirs, &all)?;
    let to_send = send(c, &answer.uploads, &files, "canceled: the share continues the next time you press Share", progress)?;
    let version = c.call(Method::Get, &format!("/versions/{}", answer.version.id), None, false)
        .ok()
        .and_then(|v| v.get("version").cloned())
        .and_then(|v| serde_json::from_value::<CrucibleVersion>(v).ok())
        .unwrap_or(answer.version.clone());
    if let Some(r) = all.profiles.get_mut(&req.profile) {
        r.pending = false;
        r.version = version.clone();
    }
    shared_save(dirs, &all)?;
    progress.report(Some(1.0), &format!("shared as {} ({})", pack_view.url, pack_view.code));
    let count = file_list(&files.index).len();
    Ok(ShareResult { pack: pack_view, version, files: count, uploaded: to_send, skipped: files.total.saturating_sub(to_send), copied: false })
}


#[cfg(test)]
mod tests;
