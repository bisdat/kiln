//! Pack Studio's publish (DESIGN.md 3.6): the next version of a pack source to Crucible. A new pack is made the first
//! time (link-only), later versions go to the same pack; only the files Crucible lacks are sent (as for a share); the
//! credits are confirmed for the version; a public pack is made public once (the service checks credits, a summary
//! and its own analysis again). The built .kilnpack is kept in the source's versions/ folder (a server pack is built
//! from it).

use super::{CruciblePack, CrucibleVersion, Crucible, ShareAnswer, Scaled, archives, err, send};
use crate::game::GameInstall;
use crate::modpack::pack;
use crate::modpack::studio::{self, Published, PublishedVersion, Visibility};
use crate::net::Method;
use crate::paths::DataDirs;
use crate::task::Progress;
use crate::{ErrorCode, Result};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::path::Path;

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PublishRequest {
    /// SemVer; must be new for the pack.
    pub version: String,
    #[serde(default)]
    pub changes: Option<String>,
    #[serde(default)]
    pub visibility: Visibility,
    /// "I have credited every mod and its author, and I may share these files." (public packs)
    #[serde(default)]
    pub attest: bool,
    /// Game builds the creator tested with (default: the source's, else the installed game's).
    #[serde(default)]
    pub game_builds: Vec<u64>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PublishResult {
    pub pack: CruciblePack,
    pub version: CrucibleVersion,
    pub files: usize,
    pub uploaded: u64,
    pub skipped: u64,
    /// The pack is public now.
    pub public: bool,
    /// What Crucible still wants before the pack can be public (empty when public or link-only).
    pub needs: Vec<String>,
    /// The version kept in the source's versions/ folder.
    pub file: String,
}

fn read<T: serde::de::DeserializeOwned>(v: Value) -> Result<T> {
    serde_json::from_value(v).map_err(|_| err(ErrorCode::Network, "Crucible sent an answer Kiln cannot read."))
}

/// What must hold before anything is sent: a new version, and for a public pack the credits, the attestation and a
/// summary. Empty: go.
pub fn precheck(st: &studio::Studio, req: &PublishRequest) -> Vec<String> {
    let mut out = Vec::new();
    if crate::semver::SemVersion::parse(req.version.trim()).is_err() {
        out.push(format!("{} is not a version like 1.2.0", req.version));
    }
    if let Some(p) = &st.source.published {
        if p.versions.iter().any(|v| v.version == req.version.trim()) {
            out.push(format!("version {} is published already: choose a newer one", req.version.trim()));
        }
    }
    if st.mods.is_empty() {
        out.push("the pack has no mods".into());
    }
    if req.visibility == Visibility::Public {
        out.extend(studio::public_needs(st));
        if !req.attest {
            out.push("confirm that you credited every mod and may share these files".into());
        }
    }
    out
}

/// Publishes the next version of the pack source in dir.
pub fn publish(dirs: &DataDirs, c: &Crucible, dir: &Path, req: &PublishRequest, game: Option<&GameInstall>, progress: &mut dyn Progress) -> Result<PublishResult> {
    if !c.signed_in() {
        return Err(err(ErrorCode::AuthFailed, "Sign in with Discord to publish."));
    }
    let st = studio::open(dirs, dir)?;
    let problems = precheck(&st, req);
    if !problems.is_empty() {
        return Err(err(ErrorCode::Refused, format!("Before publishing: {}.", problems.join("; "))).with_detail(problems));
    }
    // the version, its notes and builds go into the source first (the build reads them)
    let mut s = st.source.clone();
    s.version = req.version.trim().to_string();
    s.changes = req.changes.clone().filter(|t| !t.trim().is_empty());
    s.visibility = req.visibility;
    if !req.game_builds.is_empty() {
        s.game_builds = req.game_builds.clone();
    } else if s.game_builds.is_empty() {
        s.game_builds = game.and_then(|g| g.build_id.as_deref()).and_then(|b| b.trim().parse().ok()).into_iter().collect();
    }
    let st = studio::save(dirs, dir, s)?;
    let out = super::root(dirs).join("out");
    let built = pack::build(dirs, &studio::build_request(&st), &out, &mut Scaled { inner: progress, from: 0.0, to: 0.2 })?;
    let result = (|| -> Result<PublishResult> {
        let files = archives(dirs, &built, progress)?;
        let summary = st.source.summary.clone().unwrap_or_default();
        let body = st.source.description.clone().unwrap_or_default();
        progress.report(Some(0.32), "asking Crucible for upload slots");
        let create = || -> Result<ShareAnswer> {
            read(c.call(Method::Post, "/packs", Some(&json!({ "index": files.index, "title": st.source.name, "summary": summary, "body": body, "tags": st.source.tags, "changes": files.index.changes })), true)?)
        };
        let (answer, existing) = match st.source.published.as_ref().map(|p| p.pack.clone()) {
            Some(id) => match c.call(Method::Post, &format!("/packs/{id}/versions"), Some(&json!({ "index": files.index, "changes": files.index.changes })), true) {
                Ok(v) => (read::<ShareAnswer>(v)?, Some(id)),
                // gone on Crucible (deleted, expired): a new pack
                Err(e) if matches!(e.code, ErrorCode::NotFound | ErrorCode::Refused) => (create()?, None),
                Err(e) => return Err(e),
            },
            None => (create()?, None),
        };
        let pack_id = answer.pack.as_ref().map(|p| p.id.clone()).or(existing).ok_or_else(|| err(ErrorCode::Network, "Crucible sent an answer Kiln cannot read."))?;
        let version_id = answer.version.id.clone();
        let uploaded = send(c, &answer.uploads, &files, "canceled: publish again to send the rest", progress)?;
        // the credits of this version (every complete row; public packs have one for every mod)
        let rows: Vec<Value> = files.index.mods.iter().filter_map(|m| m.credit.as_ref().map(|cr| json!({ "kind": m.kind.as_str(), "id": m.id, "author": cr.author, "source": cr.source }))).collect();
        if !rows.is_empty() {
            progress.report(Some(0.96), "confirming the credits");
            c.call(Method::Put, &format!("/versions/{version_id}/credits"), Some(&json!({ "credits": rows })), true)?;
        }
        // visibility, title and texts (Crucible checks credits, a summary and its analysis before going public)
        let mut needs = Vec::new();
        let mut patch = json!({ "title": st.source.name, "summary": summary, "body": body });
        if !st.source.tags.is_empty() {
            patch["tags"] = json!(st.source.tags);
        }
        let current = answer.pack.as_ref().map(|p| p.visibility.clone());
        let was_public = st.source.published.as_ref().is_some_and(|p| p.visibility == Visibility::Public) || current.as_deref() == Some("public");
        match req.visibility {
            Visibility::Public if !was_public => patch["visibility"] = json!("public"),
            Visibility::Link if was_public => patch["visibility"] = json!("link"),
            _ => {}
        }
        let pack_view: CruciblePack = match c.call(Method::Patch, &format!("/packs/{pack_id}"), Some(&patch), true) {
            Ok(v) => read(v.get("pack").cloned().unwrap_or(Value::Null))?,
            Err(e) if e.code == ErrorCode::Refused => {
                needs = e.detail.as_ref().and_then(|d| d.pointer("/service/needs")).and_then(Value::as_array).map(|a| a.iter().filter_map(Value::as_str).map(str::to_string).collect()).unwrap_or_default();
                if needs.is_empty() {
                    needs.push(e.message.clone());
                }
                // the texts without the visibility change
                patch.as_object_mut().map(|m| m.remove("visibility"));
                match c.call(Method::Patch, &format!("/packs/{pack_id}"), Some(&patch), true) {
                    Ok(v) => read(v.get("pack").cloned().unwrap_or(Value::Null))?,
                    Err(_) => answer.pack.clone().unwrap_or_default(),
                }
            }
            Err(e) => return Err(e),
        };
        let version = c.call(Method::Get, &format!("/versions/{version_id}"), None, true)
            .ok()
            .and_then(|v| v.get("version").cloned())
            .and_then(|v| serde_json::from_value::<CrucibleVersion>(v).ok())
            .unwrap_or(answer.version.clone());
        let file = studio::keep_version(dir, &built)?;
        let public = pack_view.visibility == "public";
        let vis = if public { Visibility::Public } else { Visibility::Link };
        let mut rec: Published = st.source.published.clone().filter(|p| p.pack == pack_id).unwrap_or_default();
        rec.pack = pack_id.clone();
        rec.slug = Some(pack_view.slug.clone()).filter(|s| !s.is_empty()).or(rec.slug);
        rec.code = Some(pack_view.code.clone()).filter(|s| !s.is_empty()).or(rec.code);
        rec.url = Some(pack_view.url.clone()).filter(|s| !s.is_empty()).or(rec.url);
        rec.visibility = vis;
        rec.versions.push(PublishedVersion { version: files.index.version.clone(), id: version_id.clone(), published: crate::util::now_rfc3339(), visibility: vis, file: Some(file.clone()) });
        studio::record_published(dir, rec)?;
        progress.report(Some(1.0), &format!("published {} {}", st.source.name, files.index.version));
        let count = super::file_list(&files.index).len();
        Ok(PublishResult { pack: pack_view, version, files: count, uploaded, skipped: files.total.saturating_sub(uploaded), public, needs, file })
    })();
    let _ = std::fs::remove_file(&built.file);
    result
}

/// The pack on Crucible as its owner sees it now (visibility, counts), for the Studio's publish tab.
pub fn pack_state(c: &Crucible, pack_id: &str) -> Result<CruciblePack> {
    let v = c.call(Method::Get, &format!("/packs/{pack_id}"), None, true)?;
    read(v.get("pack").cloned().unwrap_or(v))
}

