//! "Send to Kiln support": a support report (kiln_manage::report, the same one "Save report..." writes) is prepared
//! once, shown on the check step (the public post as it will look, the zip's files and size, who sees the zip), and
//! sent exactly as prepared: the zip's bytes and report.json alone are kept here (in memory) between the two, for 30
//! minutes at most. The known problem that matches the report's findings comes from the last copy of the public list,
//! matched here.

use crate::state::blocking;
use kiln_manage::community::known::{KnownAction, KnownList};
use kiln_manage::community::post::{self, PostFinding};
use kiln_manage::community::relay::{Attachment, ZIP_MAX};
use kiln_manage::community::{ProblemKind, ReportDraft, SendFrom, SupportPost, ZipAudience};
use kiln_manage::{Error, ErrorCode};
use serde::Serialize;
use std::collections::HashMap;
use std::sync::{LazyLock, Mutex};
use std::time::{Duration, Instant};

/// A prepared report waits this long for Send.
const KEEP: Duration = Duration::from_secs(30 * 60);

struct Prepared {
    zip: Vec<u8>,
    /// report.json alone (the tool, the findings, the versions): what goes instead of the zip when nobody gets it.
    alone: Vec<u8>,
    name: String,
    from: SendFrom,
    known: Option<String>,
    at: Instant,
}

static PREPARED: LazyLock<Mutex<HashMap<String, Prepared>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FileLine {
    pub path: String,
    pub size: u64,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct KnownView {
    pub id: String,
    pub title: String,
    pub fix: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub copy: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub action: Option<KnownAction>,
    /// The button's words from the list; absent: the launcher's own for the action.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub label: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ReportPreview {
    pub prepared: String,
    pub findings: Vec<PostFinding>,
    pub versions: Vec<(String, String)>,
    /// The system's tag; the kind's is added for the kind the player picks (`kindTags`).
    pub tags: Vec<String>,
    pub kind_tags: HashMap<ProblemKind, Option<&'static str>>,
    pub size: u64,
    pub limit: u64,
    pub files: Vec<FileLine>,
    pub known: Option<KnownView>,
}

fn known_path() -> Result<std::path::PathBuf, Error> {
    Ok(crate::state::dirs()?.root.join("community-known.json"))
}

/// Keeps the last copy of the known-problems list (public) for offline use.
pub fn keep_known(list: &KnownList) -> Result<(), Error> {
    kiln_manage::util::write_json_atomic(&known_path()?, list)
}

pub fn load_known() -> Option<KnownList> {
    let bytes = std::fs::read(known_path().ok()?).ok()?;
    KnownList::parse(&bytes).ok()
}

pub async fn prepare(app: tauri::AppHandle, target: Option<crate::commands::logs::LogTarget>, from: SendFrom) -> Result<ReportPreview, Error> {
    let input = crate::commands::logs::report_input(&app, target)?;
    blocking(move || {
        let r = kiln_manage::report::build(&input);
        // in memory: the zip is never written to a temp folder other users of this PC could read
        let zip = kiln_manage::report::zip_bytes(&r)?;
        let post = post::build(&r, crate::commands::community::version());
        let known = crate::community::report::matching(&r);
        let id = kiln_manage::hash::hex(&kiln_manage::util::random_bytes::<12>()?);
        let preview = ReportPreview {
            prepared: id.clone(),
            findings: post.findings.clone(),
            versions: post.versions.clone(),
            tags: post.tags.clone(),
            kind_tags: [ProblemKind::Noload, ProblemKind::Mod, ProblemKind::Crash, ProblemKind::Launcher, ProblemKind::Server, ProblemKind::Other].into_iter().map(|k| (k, k.tag())).collect(),
            size: zip.len() as u64,
            limit: ZIP_MAX,
            files: r.files.iter().map(|(n, b)| FileLine { path: n.clone(), size: b.len() as u64 }).collect(),
            known: known.as_ref().map(|k| KnownView { id: k.id.clone(), title: k.title.clone(), fix: k.fix.clone(), copy: k.copy.clone(), action: k.action, label: k.label.clone() }),
        };
        let mut map = PREPARED.lock().unwrap();
        map.retain(|_, p| p.at.elapsed() < KEEP);
        // the newest few: a player opening the dialog again and again keeps no pile of zips in memory
        while map.len() >= 3 {
            let oldest = map.iter().min_by_key(|(_, p)| p.at).map(|(k, _)| k.clone());
            match oldest { Some(k) => { map.remove(&k); } None => break }
        }
        map.insert(id, Prepared { zip, alone: kiln_manage::report::public_json(&r), name: kiln_manage::report::file_name(std::time::SystemTime::now()), from, known: known.map(|k| k.id), at: Instant::now() });
        Ok(preview)
    })
    .await
}

/// A prepared report's zip for another send (a tester's results): the zip the screen showed, sent as shown. It stays
/// until [`forget_prepared`], so a send that fails can be tried again with it.
pub fn prepared_zip(prepared: &str) -> Option<(String, Vec<u8>)> {
    let map = PREPARED.lock().unwrap();
    let p = map.get(prepared).filter(|p| p.at.elapsed() < KEEP)?;
    Some((p.name.clone(), p.zip.clone()))
}

pub fn forget_prepared(prepared: &str) {
    PREPARED.lock().unwrap().remove(prepared);
}

/// The known problem the report matches, from the last copy of the public list (kept when the Community page opened:
/// matching it sends nothing).
pub fn matching(r: &kiln_manage::report::Report) -> Option<kiln_manage::community::known::KnownIssue> {
    let list = load_known()?;
    kiln_manage::community::known::best(&list, r, kiln_manage::util::now_unix()).cloned()
}

/// Sends a prepared report with the player's draft. The prepared report stays until it is sent, so a refusal (offline,
/// too many, too big) can be tried again with the same zip and words.
pub fn send(prepared: &str, draft: &ReportDraft) -> Result<SupportPost, Error> {
    let d = draft.checked()?;
    let (wire, bytes, name) = {
        let map = PREPARED.lock().unwrap();
        let p = map.get(prepared).filter(|p| p.at.elapsed() < KEEP)
            .ok_or_else(|| Error::new(ErrorCode::NotFound, "This report was prepared too long ago. Open Send to Kiln support again.").with_detail(serde_json::json!({ "kind": "not_found" })))?;
        let bytes = if d.zip == ZipAudience::None { p.alone.clone() } else { p.zip.clone() };
        (post::wire(&d, p.from, p.known.as_deref()), bytes, p.name.clone())
    };
    let with = if d.zip == ZipAudience::None { Attachment::ReportJson(&bytes) } else { Attachment::Zip(&name, &bytes) };
    let post = crate::commands::community::relay_call(|r| r.report(wire, with))?;
    PREPARED.lock().unwrap().remove(prepared);
    crate::commands::app::launcher_log("community", &format!("support report sent to Kiln support (zip: {})", match d.zip { ZipAudience::Team => "team", ZipAudience::Public => "public", ZipAudience::None => "not sent" }));
    Ok(post)
}
