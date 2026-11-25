//! The Crucible client against an in-process fake of the service (API, CDN, upload host): share, receive by code,
//! install, the creator's next version with only new files uploaded, the update with its plan, undo; and the refusals
//! (no sign-in, no consent, a tampered file, a file on another host, an index signed with another key).

use super::sign::testing::Signer;
use super::*;
use crate::net::{Response, WireRequest};
use crate::task::NoProgress;
use std::sync::Mutex;

const KEY: &str = "sessionkeysessionkeysessionkeysessionkey123"; // 43 characters, as a relay session
const OWNER: &str = "195336310146334720";
const API_BASE: &str = "https://api.crucible.test/kiln/crucible/api/v1";
const CDN_BASE: &str = "https://cdn.crucible.test";

#[derive(Default)]
struct State {
    packs: BTreeMap<String, (String, String, String, Option<String>)>, // id -> (code, slug, title, latest)
    versions: BTreeMap<String, (String, u64, Value, String)>,         // id -> (pack, number, index, status)
    files: BTreeMap<String, Vec<u8>>,                                  // role:sha -> bytes
    uploads: BTreeMap<String, (String, String, u64, Option<Vec<u8>>)>, // id -> (role, sha, size, staged)
    puts: Vec<String>,
    installs: u64,
    tamper: Option<String>,
    foreign_url: bool,
    reject_sessions: bool,
    seq: u64,
    public: std::collections::BTreeSet<String>,
    anonymous_keys: BTreeMap<String, String>,
    upload_packs: BTreeMap<String, String>,
    credits: BTreeMap<String, Value>, // version -> the credits PUT
    patches: Vec<Value>,
    fail_credits: u32,                 // the next PUT /credits calls that answer 503
    gone: std::collections::BTreeSet<String>, // packs that answer 410 (deleted or taken down)
}

struct Fake {
    signer: Signer,
    s: Mutex<State>,
    requests: Mutex<Vec<String>>,
}

fn json(status: u16, v: Value) -> Result<Response> {
    Ok(Response { status, headers: vec![("Content-Type".into(), "application/json".into())], body: v.to_string().into_bytes() })
}

fn body_bytes(b: Body) -> Vec<u8> {
    use std::io::Read;
    match b {
        Body::Empty => Vec::new(),
        Body::Bytes(b) => b,
        Body::File { file, len } => {
            let mut v = Vec::new();
            file.take(len).read_to_end(&mut v).unwrap();
            v
        }
        Body::Reader { reader, len } => {
            let mut v = Vec::new();
            reader.take(len).read_to_end(&mut v).unwrap();
            v
        }
    }
}

impl Fake {
    fn new() -> Fake {
        Fake { signer: Signer::new(3), s: Mutex::new(State::default()), requests: Mutex::new(Vec::new()) }
    }

    fn files_of(index: &Value) -> Vec<(String, String, u64)> {
        let mut v = Vec::new();
        for m in index["mods"].as_array().unwrap() {
            v.push(("mod".into(), m["file"]["sha256"].as_str().unwrap().into(), m["file"]["size"].as_u64().unwrap()));
        }
        if let Some(f) = index.get("layer").and_then(|l| l.get("file")) {
            v.push(("layer".into(), f["sha256"].as_str().unwrap().into(), f["size"].as_u64().unwrap()));
        }
        for o in index.get("overrides").and_then(Value::as_array).cloned().unwrap_or_default() {
            v.push(("blob".into(), o["sha256"].as_str().unwrap().into(), o["size"].as_u64().unwrap()));
        }
        v
    }

    fn pack_view(s: &State, id: &str) -> Value {
        let (code, slug, title, latest) = &s.packs[id];
        serde_json::json!({ "id": id, "slug": slug, "code": show_code(code), "url": format!("https://frostdev.io/kiln/crucible/p/{}", show_code(code)),
            "title": title, "summary": "", "icon": null, "owner": if s.anonymous_keys.contains_key(id) { serde_json::json!({ "id": "", "name": "Anonymous" }) } else { serde_json::json!({ "id": OWNER, "name": "Emberfox" }) }, "visibility": if s.public.contains(id) { "public" } else { "link" },
            "upvotes": 0, "comments": 0, "installs": s.installs, "mods": 0, "builds": [], "latest": latest })
    }

    fn version_view(s: &State, id: &str) -> Value {
        let (_, number, index, status) = &s.versions[id];
        serde_json::json!({ "id": id, "number": number, "version": index["version"], "changes": index.get("changes"), "status": status,
            "size": Self::files_of(index).iter().map(|f| f.2).sum::<u64>(), "published": null,
            "checks": { "state": "done", "full": true, "ok": true, "errors": [], "warnings": ["beta: references networking"], "notes": [],
                "summary": { "errors": 0, "warnings": 1, "notes": 0 } } })
    }

    fn plan(s: &mut State, files: &[(String, String, u64)]) -> (Vec<Value>, Vec<String>) {
        let (mut uploads, mut have) = (Vec::new(), Vec::new());
        for (role, sha, size) in files {
            if s.files.contains_key(&format!("{role}:{sha}")) {
                have.push(sha.clone());
                continue;
            }
            s.seq += 1;
            let id = format!("upl_{}", s.seq);
            s.uploads.insert(id.clone(), (role.clone(), sha.clone(), *size, None));
            uploads.push(serde_json::json!({ "id": id, "sha256": sha, "role": role, "size": size,
                "url": format!("https://up.crucible.test/up/{id}?X-Amz-Signature=abc"), "headers": { "content-type": "application/octet-stream" } }));
        }
        (uploads, have)
    }

    fn finish(s: &mut State) {
        let ready: Vec<String> = s.versions.iter()
            .filter(|(_, v)| v.3 == "uploading" && Self::files_of(&v.2).iter().all(|(r, sha, _)| s.files.contains_key(&format!("{r}:{sha}"))))
            .map(|(k, _)| k.clone()).collect();
        for vid in ready {
            let pack = s.versions[&vid].0.clone();
            s.versions.get_mut(&vid).unwrap().3 = "ready".into();
            s.packs.get_mut(&pack).unwrap().3 = Some(vid);
        }
    }

    fn add_version(s: &mut State, pack: &str, index: Value) -> (String, Vec<Value>, Vec<String>) {
        s.seq += 1;
        let vid = format!("pv{}x", s.seq).replace("pv", "pv_");
        let number = s.versions.values().filter(|v| v.0 == pack).count() as u64 + 1;
        let files = Self::files_of(&index);
        s.versions.insert(vid.clone(), (pack.into(), number, index, "uploading".into()));
        let (uploads, have) = Self::plan(s, &files);
        for u in &uploads { s.upload_packs.insert(u["id"].as_str().unwrap().into(), pack.into()); }
        Self::finish(s);
        (vid, uploads, have)
    }

    fn served(&self, s: &State, vid: &str) -> Vec<u8> {
        let (pack, number, index, _) = &s.versions[vid];
        let (code, slug, _, _) = &s.packs[pack];
        let mut out = index.clone();
        out["crucible"] = serde_json::json!({ "pack": pack, "version": vid, "slug": slug, "code": show_code(code), "number": number });
        let cdn = if s.foreign_url { "https://evil.example" } else { CDN_BASE };
        for m in out["mods"].as_array_mut().unwrap() {
            let sha = m["file"]["sha256"].as_str().unwrap().to_string();
            m["file"]["url"] = format!("{cdn}/f/{sha}.zip").into();
        }
        if let Some(l) = out.get_mut("layer").and_then(|l| l.get_mut("file")) {
            let sha = l["sha256"].as_str().unwrap().to_string();
            l["url"] = format!("{cdn}/l/{sha}.zip").into();
        }
        for o in out.get_mut("overrides").and_then(Value::as_array_mut).map(|v| v.iter_mut().collect::<Vec<_>>()).unwrap_or_default() {
            let sha = o["sha256"].as_str().unwrap().to_string();
            o["url"] = format!("{cdn}/b/{sha}").into();
        }
        self.signer.envelope(out.to_string().as_bytes()).into_bytes()
    }

    fn handle(&self, req: WireRequest) -> Result<Response> {
        let path = req.url.path_and_query.split('?').next().unwrap().to_string();
        self.requests.lock().unwrap().push(format!("{} {}{}", req.method.as_str(), req.url.host, path));
        let authed = req.headers.iter().any(|(k, v)| k.eq_ignore_ascii_case("authorization") && v == &format!("Bearer {KEY}"));
        let mut s = self.s.lock().unwrap();
        let host = req.url.host.as_str();
        let body = body_bytes(req.body);
        let jbody: Value = serde_json::from_slice(&body).unwrap_or(Value::Null);
        if host == "up.crucible.test" {
            let id = path.trim_start_matches("/up/").to_string();
            let u = s.uploads.get_mut(&id).expect("upload");
            u.3 = Some(body);
            s.puts.push(id);
            return Ok(Response { status: 200, headers: vec![("ETag".into(), "\"e\"".into())], body: Vec::new() });
        }
        if host == "cdn.crucible.test" {
            if let Some(vid) = path.strip_prefix("/m/").and_then(|p| p.strip_suffix(".json")) {
                return match s.versions.get(vid) {
                    Some(v) if v.3 == "ready" => Ok(Response { status: 200, headers: vec![], body: self.served(&s, vid) }),
                    _ => Ok(Response { status: 404, headers: vec![], body: Vec::new() }),
                };
            }
            let (role, sha) = if let Some(x) = path.strip_prefix("/f/") { ("mod", x.trim_end_matches(".zip")) }
                else if let Some(x) = path.strip_prefix("/l/") { ("layer", x.trim_end_matches(".zip")) }
                else { ("blob", path.trim_start_matches("/b/")) };
            let mut bytes = s.files.get(&format!("{role}:{sha}")).cloned().unwrap_or_default();
            if bytes.is_empty() {
                return Ok(Response { status: 404, headers: vec![], body: Vec::new() });
            }
            if s.tamper.as_deref() == Some(sha) {
                let n = bytes.len() / 2;
                bytes[n] ^= 0xff;
            }
            return Ok(Response { status: 200, headers: vec![], body: bytes });
        }
        assert_eq!(host, "api.crucible.test", "a host outside the fake");
        let p = path.strip_prefix("/kiln/crucible/api/v1").unwrap().to_string();
        let anonymous = p.starts_with("/anonymous/");
        let p = p.strip_prefix("/anonymous").unwrap_or(&p);
        let parts: Vec<&str> = p.trim_start_matches('/').split('/').collect();
        let owner_key = req.headers.iter().find(|(k, _)| k.eq_ignore_ascii_case("authorization")).and_then(|(_, v)| v.strip_prefix("CrucibleOwner "));
        if anonymous {
            let pack = match parts.as_slice() {
                ["packs", id, ..] => Some(*id),
                ["versions", vid, ..] => s.versions.get(*vid).map(|v| v.0.as_str()),
                ["uploads", id, ..] => s.upload_packs.get(*id).map(String::as_str),
                _ => None,
            };
            let ok = if parts.as_slice() == ["quickshare"] { owner_key.is_some_and(|k| k.len() == 64) }
                else { pack.and_then(|id| s.anonymous_keys.get(id)).map(String::as_str) == owner_key && owner_key.is_some() };
            if !ok { return json(403, serde_json::json!({ "error": "refused", "message": "Wrong owner key." })); }
        }
        let need_auth = matches!((req.method, parts.as_slice()), (Method::Post, ["quickshare"]) | (Method::Post, ["packs"]) | (Method::Post, ["packs", _, "versions"]) | (Method::Post, ["uploads", ..])
            | (Method::Put, _) | (Method::Patch, _));
        if need_auth && (!authed || s.reject_sessions) && !anonymous {
            return json(401, serde_json::json!({ "error": "signed-out", "message": "Sign in with Discord first." }));
        }
        match (req.method, parts.as_slice()) {
            (Method::Post, ["quickshare"]) | (Method::Post, ["packs"]) => {
                s.seq += 1;
                let id = format!("pck_{}", s.seq);
                let code = format!("H7QK3MZ{}", &CODE_ALPHABET[s.packs.len()..s.packs.len() + 1]);
                s.packs.insert(id.clone(), (code, "ember-run".into(), jbody["title"].as_str().unwrap_or("x").into(), None));
                if anonymous { s.anonymous_keys.insert(id.clone(), owner_key.unwrap().into()); }
                let (vid, uploads, have) = Self::add_version(&mut s, &id, jbody["index"].clone());
                json(201, serde_json::json!({ "pack": Self::pack_view(&s, &id), "version": Self::version_view(&s, &vid), "uploads": uploads, "have": have }))
            }
            (Method::Post, ["packs", id, "versions"]) if s.gone.contains(*id) => {
                json(410, serde_json::json!({ "error": "gone", "message": "This pack is no longer on Crucible." }))
            }
            (Method::Post, ["packs", id, "versions"]) => {
                let (vid, uploads, have) = Self::add_version(&mut s, id, jbody["index"].clone());
                json(201, serde_json::json!({ "version": Self::version_view(&s, &vid), "uploads": uploads, "have": have }))
            }
            (Method::Post, ["uploads", id, "complete"]) => {
                let (role, sha, size, staged) = s.uploads.get(*id).cloned().expect("upload");
                let staged = staged.expect("PUT before complete");
                if staged.len() as u64 != size || crate::hash::sha256_hex(&staged) != sha {
                    return json(422, serde_json::json!({ "error": "bad-file", "message": "The uploaded file does not match." }));
                }
                s.files.insert(format!("{role}:{sha}"), staged);
                Self::finish(&mut s);
                json(200, serde_json::json!({ "ok": true }))
            }
            (Method::Get, ["codes", code]) => {
                let c = parse_code(code).unwrap_or_default();
                let Some(id) = s.packs.iter().find(|(_, p)| p.0 == c).map(|(k, _)| k.clone()) else {
                    return json(404, serde_json::json!({ "error": "not-found", "message": "We couldn't find that code. Codes look like H7QK-3MZP." }));
                };
                let latest = s.packs[&id].3.clone();
                let up = s.versions.iter().filter(|(_, v)| v.0 == id && v.3 == "uploading").map(|(k, _)| k.clone()).last();
                json(200, serde_json::json!({ "pack": Self::pack_view(&s, &id), "version": latest.map(|v| Self::version_view(&s, &v)),
                    "uploading": up.map(|v| serde_json::json!({ "version": v, "progress": 0.5 })) }))
            }
            (Method::Get, ["versions", vid]) => {
                let pack = s.versions[*vid].0.clone();
                json(200, serde_json::json!({ "version": Self::version_view(&s, vid), "pack": Self::pack_view(&s, &pack) }))
            }
            (Method::Get, ["packs", id]) => json(200, serde_json::json!({ "pack": Self::pack_view(&s, id) })),
            (Method::Get, ["packs", id, "head"]) => {
                let latest = s.packs[*id].3.clone();
                json(200, serde_json::json!({ "pack": id, "title": "x", "version": latest.map(|v| Self::version_view(&s, &v)) }))
            }
            (Method::Post, ["versions", vid, "uploads"]) if anonymous => {
                let pack = s.versions[*vid].0.clone();
                let files = Self::files_of(&s.versions[*vid].2);
                let (uploads, have) = Self::plan(&mut s, &files);
                for u in &uploads { s.upload_packs.insert(u["id"].as_str().unwrap().into(), pack.clone()); }
                json(200, serde_json::json!({ "uploads": uploads, "have": have }))
            }
            (Method::Post, ["uploads"]) => {
                let files: Vec<(String, String, u64)> = jbody["files"].as_array().into_iter().flatten()
                    .map(|f| (f["role"].as_str().unwrap().to_string(), f["sha256"].as_str().unwrap().to_string(), f["size"].as_u64().unwrap_or(0))).collect();
                let (uploads, have) = Self::plan(&mut s, &files);
                json(200, serde_json::json!({ "uploads": uploads, "have": have }))
            }
            (Method::Put, ["versions", vid, "credits"]) => {
                if s.fail_credits > 0 {
                    s.fail_credits -= 1;
                    return json(503, serde_json::json!({ "error": "unavailable", "message": "try again" }));
                }
                s.credits.insert(vid.to_string(), jbody["credits"].clone());
                json(200, serde_json::json!({ "version": Self::version_view(&s, vid), "complete": true }))
            }
            (Method::Patch, ["packs", id]) => {
                s.patches.push(jbody.clone());
                if jbody["visibility"] == "public" {
                    let latest = s.packs[*id].3.clone();
                    let credited = latest.as_ref().and_then(|v| s.credits.get(v)).and_then(Value::as_array).map(|a| a.len()).unwrap_or(0);
                    let mods = latest.as_ref().map(|v| s.versions[v].2["mods"].as_array().map(|a| a.len()).unwrap_or(0)).unwrap_or(usize::MAX);
                    if credited < mods || jbody["summary"].as_str().unwrap_or("").len() < 10 {
                        return json(409, serde_json::json!({ "error": "not-ready", "message": "Before going public: every mod needs a credit.", "needs": ["every mod needs a credit (author and an https link to where it comes from)"] }));
                    }
                    s.public.insert(id.to_string());
                } else if jbody["visibility"] == "link" {
                    s.public.remove(*id);
                }
                json(200, serde_json::json!({ "pack": Self::pack_view(&s, id) }))
            }
            (Method::Post, ["versions", _, "installed"]) => {
                s.installs += 1;
                json(200, serde_json::json!({ "ok": true }))
            }
            other => panic!("the fake has no {other:?}"),
        }
    }

    fn sent(&self) -> Vec<String> {
        self.requests.lock().unwrap().clone()
    }
}

impl Transport for Fake {
    fn exchange(&self, req: WireRequest) -> Result<Response> {
        self.handle(req)
    }
    fn download(&self, req: WireRequest, sink: &mut dyn std::io::Write) -> Result<Response> {
        let max = req.max_response;
        let r = self.handle(req)?;
        if r.status == 200 {
            if r.body.len() as u64 > max {
                return Err(Error::new(ErrorCode::Refused, "too large"));
            }
            sink.write_all(&r.body)?;
        }
        Ok(Response { body: Vec::new(), ..r })
    }
}

fn endpoints(key: &PublicKey) -> Endpoints {
    Endpoints { api: API_BASE.into(), cdn: CDN_BASE.into(), upload_hosts: vec!["up.crucible.test".into()], keys: vec![key.clone()], rewrites: vec![], dev: true }
}

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-crucible-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p.canonicalize().unwrap())
}

fn kiln_mod(root: &Path, id: &str, version: &str) -> PathBuf {
    let d = root.join(format!("src/{id}-{version}"));
    std::fs::create_dir_all(&d).unwrap();
    std::fs::write(d.join("kiln.mod.json"), format!(r#"{{ "id": "{id}", "version": "{version}", "apiVersion": "0.2", "entryAssembly": "M.dll", "side": "both" }}"#)).unwrap();
    std::fs::write(d.join("M.dll"), format!("{id} {version} {}", "x".repeat(2000))).unwrap();
    d
}

fn add_mods(dirs: &DataDirs, pid: &str, src: &Path, ids: &[&str]) {
    let paths: Vec<PathBuf> = ids.iter().map(|id| kiln_mod(src, id, "1.0.0")).collect();
    let r = crate::library::import(dirs, &paths, &mut NoProgress).unwrap();
    let mut p = crate::profile::get(dirs, pid).unwrap();
    for lr in r.imported.iter().chain(&r.existing) {
        p.mods.kiln.push(crate::profile::ProfileMod { id: lr.id.clone(), version: lr.version.clone(), hash: lr.hash.clone(), enabled: true });
    }
    crate::profile::save(dirs, &p).unwrap();
}

fn creator(t: &Tmp) -> (DataDirs, String) {
    let dirs = DataDirs::at(t.0.join("creator"));
    let p = crate::profile::create(&dirs, "Ember run", None).unwrap();
    add_mods(&dirs, &p.id, &t.0, &["alpha", "beta"]);
    (dirs, p.id)
}

#[test]
fn share_receive_update_and_undo() {
    let t = tmp("flow");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (a, pid) = creator(&t);
    let ca = Crucible::new(&ep, &fake, "0.2.0-dev").with_session(Some(KEY.into()));
    let r1 = share(&a, &ca, &ShareRequest { profile: pid.clone(), ..Default::default() }, None, &mut NoProgress).unwrap();
    assert_eq!(r1.files, 2);
    assert!(r1.uploaded > 0 && r1.skipped == 0, "{r1:?}");
    assert_eq!(r1.version.status, "ready");
    assert_eq!(r1.pack.code, "H7QK-3MZ2");
    assert_eq!(fake.s.lock().unwrap().puts.len(), 2);
    assert_eq!(shared(&a, &pid).unwrap().unwrap().changed, None);

    // a friend: no sign-in, the code as typed
    let b = DataDirs::at(t.0.join("friend"));
    let cb = Crucible::new(&ep, &fake, "0.2.0-dev");
    let (p, uploading) = preview(&b, &cb, "h7qk 3mz2", None).unwrap();
    assert!(uploading.is_none());
    assert_eq!((p.missing.len(), p.present.len()), (2, 0));
    assert!(p.download > 0 && p.download == p.size, "{} {}", p.download, p.size);
    assert!(p.kiln.ok && p.game.fits && !p.trusted && p.instance.is_none());
    assert_eq!(p.pack.owner.as_ref().unwrap().name, "Emberfox");
    let before = fake.sent().len();
    let e = install(&b, &cb, None, &InstallRequest { target: p.version.id.clone(), consent: false, ..Default::default() }, None, false, &mut NoProgress).unwrap_err();
    assert_eq!(e.code, ErrorCode::ConsentRequired);
    assert_eq!(fake.sent().len(), before, "nothing fetched without consent");
    let ir = install(&b, &cb, None, &InstallRequest { target: p.version.id.clone(), consent: true, trust_always: true, ..Default::default() }, p.pack.owner.as_ref(), true, &mut NoProgress).unwrap();
    assert_eq!(ir.instance.crucible.as_ref().unwrap().pack.as_deref(), Some(r1.pack.id.as_str()));
    assert_eq!(ir.profile.mods.kiln.iter().map(|m| m.id.as_str()).collect::<Vec<_>>(), ["alpha", "beta"]);
    assert!(ir.downloaded > 0);
    assert_eq!(fake.s.lock().unwrap().installs, 1);
    assert_eq!(trusted(&b).iter().map(|t| t.id.as_str()).collect::<Vec<_>>(), [OWNER]);
    let pid_b = ir.profile.id.clone();
    // the same pack again: nothing left to download, the instance is named
    let (p2, _) = preview(&b, &cb, &r1.pack.url, None).unwrap();
    assert_eq!((p2.missing.len(), p2.download), (0, 0));
    assert_eq!(p2.instance.as_deref(), Some(pid_b.as_str()));
    assert!(p2.trusted);

    // the creator adds a mod: "Pack changed", then the link's next version uploads only the new file
    add_mods(&a, &pid, &t.0, &["gamma"]);
    assert_eq!(shared(&a, &pid).unwrap().unwrap().changed.unwrap().added, ["gamma"]);
    let puts = fake.s.lock().unwrap().puts.len();
    let r2 = share(&a, &ca, &ShareRequest { profile: pid.clone(), ..Default::default() }, None, &mut NoProgress).unwrap();
    assert_eq!((r2.pack.id.as_str(), r2.pack.code.as_str()), (r1.pack.id.as_str(), r1.pack.code.as_str()), "same link");
    assert_eq!(r2.version.number, 2);
    assert_eq!(fake.s.lock().unwrap().puts.len(), puts + 1, "only gamma went up");
    assert!(r2.skipped > 0);
    assert_eq!(shared(&a, &pid).unwrap().unwrap().changed, None);

    // the friend sees the update, plans it, takes it, undoes it
    let ups = updates(&b, &cb).unwrap();
    assert_eq!(ups.len(), 1);
    assert_eq!(ups[0].to.version_id, r2.version.id);
    assert_eq!(ups[0].by.as_deref(), Some("Emberfox"));
    assert_eq!(ups[0].counts, Some(UpdateCounts { added: 1, updated: 0, removed: 0 }));
    let plan = update_plan(&b, &cb, &pid_b, &mut NoProgress).unwrap();
    assert_eq!(plan.plan.mods.added.iter().map(|m| m.id.as_str()).collect::<Vec<_>>(), ["gamma"]);
    let done = update(&b, &cb, &UpdateRequest { profile: pid_b.clone(), version_id: plan.version_id.clone(), consent: true, ..Default::default() }, &mut NoProgress).unwrap();
    assert_eq!(done.instance.version, "1.0.1");
    assert!(done.profile.mods.kiln.iter().any(|m| m.id == "gamma"));
    assert!(updates(&b, &cb).unwrap().is_empty());
    let back = pack::undo(&b, &pid_b).unwrap();
    assert!(!back.mods.kiln.iter().any(|m| m.id == "gamma"));
}

#[test]
fn a_game_client_downloads_no_server_only_mod() {
    let t = tmp("serveronly");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (a, pid) = creator(&t);
    add_mods(&a, &pid, &t.0, &["srv"]);
    let ca = Crucible::new(&ep, &fake, "0").with_session(Some(KEY.into()));
    let shared = share(&a, &ca, &ShareRequest { profile: pid, ..Default::default() }, None, &mut NoProgress).unwrap();
    assert_eq!(shared.files, 3, "Crucible holds the whole pack");
    // the creator marked srv for servers only (Pack Studio's sides): the signed index says so
    {
        let mut s = fake.s.lock().unwrap();
        let ix = &mut s.versions.get_mut(&shared.version.id).unwrap().2;
        for m in ix["mods"].as_array_mut().unwrap() {
            if m["id"] == "srv" {
                m["env"] = serde_json::json!({ "client": "unsupported", "server": "required" });
            }
        }
    }

    let b = DataDirs::at(t.0.join("friend"));
    let cb = Crucible::new(&ep, &fake, "0");
    let (pv, _) = preview(&b, &cb, &shared.pack.code, None).unwrap();
    let srv = pv.index.mods.iter().find(|m| m.id == "srv").unwrap();
    assert_eq!(srv.env().client, pack::Need::Unsupported);
    let srv_sha = srv.file.as_ref().unwrap().sha256.clone();
    assert!(!pv.missing.iter().chain(&pv.present).any(|m| m.id == "srv"), "the preview counts what this PC installs");
    let alpha = pv.index.mods.iter().find(|m| m.id == "alpha").unwrap().file.as_ref().unwrap().size;
    assert!(pv.download < alpha * 2 + srv.file.as_ref().unwrap().size, "the download leaves the server's mod out: {}", pv.download);
    let ir = install(&b, &cb, None, &InstallRequest { target: pv.version.id.clone(), consent: true, ..Default::default() }, None, false, &mut NoProgress).unwrap();
    assert_eq!(ir.profile.mods.kiln.iter().map(|m| m.id.as_str()).collect::<Vec<_>>(), ["alpha", "beta"]);
    assert!(!fake.sent().iter().any(|r| r.contains(&srv_sha)), "the server's mod was not downloaded");
    // an update plan from the same (client) pack reads it too
    assert!(update_plan(&b, &cb, &ir.profile.id, &mut NoProgress).is_ok());
}

#[test]
fn signed_out_sharing_keeps_a_private_owner_key_and_updates_the_same_link() {
    let t = tmp("anonymous");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (a, pid) = creator(&t);
    let c = Crucible::new(&ep, &fake, "0");
    let req = ShareRequest { profile: pid.clone(), ..Default::default() };
    let first = share(&a, &c, &req, None, &mut NoProgress).unwrap();
    assert_eq!(first.pack.owner.as_ref().unwrap().id, "");
    let state = shared(&a, &pid).unwrap().unwrap();
    assert!(!state.requires_sign_in);
    let owner = ownership::load(&a, &ep.api, &pid).unwrap().unwrap();
    let visible = serde_json::to_string(&state).unwrap() + &serde_json::to_string(&first).unwrap();
    assert!(!visible.contains(&owner.key));
    #[cfg(unix)] {
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(std::fs::metadata(ownership::path(&a, &ep.api, &pid)).unwrap().permissions().mode() & 0o777, 0o600);
    }
    let b = DataDirs::at(t.0.join("friend"));
    let (pv, _) = preview(&b, &c, &first.pack.code, None).unwrap();
    let installed = install(&b, &c, None, &InstallRequest { target: pv.version.id, consent: true, trust_always: true, ..Default::default() }, pv.pack.owner.as_ref(), false, &mut NoProgress).unwrap();
    assert_eq!(installed.profile.mods.kiln.len(), 2);
    assert!(trusted(&b).is_empty(), "anonymous authors have no account identity to trust permanently");
    add_mods(&a, &pid, &t.0, &["gamma"]);
    let second = share(&a, &c, &req, None, &mut NoProgress).unwrap();
    assert_eq!(second.pack.code, first.pack.code);
    assert_eq!(second.version.number, 2);
    // Simulate an interrupted upload: resume only that version's files, without creating version 3.
    let mut state = shared_all(&a);
    state.profiles.get_mut(&pid).unwrap().pending = true;
    shared_save(&a, &state).unwrap();
    assert_eq!(share(&a, &c, &req, None, &mut NoProgress).unwrap().version.number, 2);
    let mut wrong = owner;
    wrong.key = "f".repeat(64);
    ownership::save(&a, &ep.api, &pid, &wrong).unwrap();
    add_mods(&a, &pid, &t.0, &["delta"]);
    assert_eq!(share(&a, &c, &req, None, &mut NoProgress).unwrap_err().code, ErrorCode::Refused);
    std::fs::remove_file(ownership::path(&a, &ep.api, &pid)).unwrap();
    assert_eq!(share(&a, &c, &req, None, &mut NoProgress).unwrap_err().code, ErrorCode::Refused);
}

#[test]
fn expired_sessions_allow_new_unlisted_links_but_not_account_owned_updates() {
    let t = tmp("expired-session");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (a, pid) = creator(&t);
    let c = Crucible::new(&ep, &fake, "0").with_session(Some(KEY.into()));
    // Existing authenticated shares must not silently become anonymous when their session expires.
    let legacy = share(&a, &c, &ShareRequest { profile: pid.clone(), ..Default::default() }, None, &mut NoProgress).unwrap();
    assert!(shared(&a, &pid).unwrap().unwrap().requires_sign_in);
    fake.s.lock().unwrap().reject_sessions = true;
    add_mods(&a, &pid, &t.0, &["gamma"]);
    assert_eq!(share(&a, &c, &ShareRequest { profile: pid, ..Default::default() }, None, &mut NoProgress).unwrap_err().code, ErrorCode::AuthFailed);
    let fresh = crate::profile::create(&a, "New private link", None).unwrap();
    add_mods(&a, &fresh.id, &t.0, &["alpha"]);
    let link = share(&a, &c, &ShareRequest { profile: fresh.id.clone(), ..Default::default() }, None, &mut NoProgress).unwrap();
    assert_eq!(link.pack.owner.unwrap().id, "");
    assert!(!shared(&a, &fresh.id).unwrap().unwrap().requires_sign_in);
    assert_ne!(link.pack.code, legacy.pack.code);
}

#[test]
fn a_tampered_file_installs_nothing() {
    let t = tmp("tamper");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (a, pid) = creator(&t);
    let ca = Crucible::new(&ep, &fake, "0").with_session(Some(KEY.into()));
    let r = share(&a, &ca, &ShareRequest { profile: pid, ..Default::default() }, None, &mut NoProgress).unwrap();
    let b = DataDirs::at(t.0.join("friend"));
    let cb = Crucible::new(&ep, &fake, "0");
    let (p, _) = preview(&b, &cb, &r.pack.code, None).unwrap();
    let sha = p.index.mods[0].file.as_ref().unwrap().sha256.clone();
    fake.s.lock().unwrap().tamper = Some(sha);
    let e = install(&b, &cb, None, &InstallRequest { target: p.version.id.clone(), consent: true, ..Default::default() }, None, false, &mut NoProgress).unwrap_err();
    assert_eq!(e.code, ErrorCode::HashMismatch, "{e:?}");
    assert!(e.message.contains("didn't download correctly"), "{}", e.message);
    assert!(pack::instances(&b).unwrap().is_empty());
    assert!(crate::library::list(&b, None).unwrap().is_empty());
}

#[test]
fn files_on_another_host_and_other_keys_are_refused() {
    let t = tmp("hosts");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (a, pid) = creator(&t);
    let ca = Crucible::new(&ep, &fake, "0").with_session(Some(KEY.into()));
    let r = share(&a, &ca, &ShareRequest { profile: pid, ..Default::default() }, None, &mut NoProgress).unwrap();
    let b = DataDirs::at(t.0.join("friend"));
    // an index signed with a key this launcher does not have
    let other = Signer::new(9);
    let ep2 = endpoints(&other.key);
    let e = preview(&b, &Crucible::new(&ep2, &fake, "0"), &r.pack.code, None).unwrap_err();
    assert_eq!(e.code, ErrorCode::HashMismatch);
    // a signed index naming files elsewhere: refused before anything is fetched from there
    fake.s.lock().unwrap().foreign_url = true;
    let cb = Crucible::new(&ep, &fake, "0");
    let (p, _) = preview(&b, &cb, &r.pack.code, None).unwrap();
    let e = install(&b, &cb, None, &InstallRequest { target: p.version.id, consent: true, ..Default::default() }, None, false, &mut NoProgress).unwrap_err();
    assert_eq!(e.code, ErrorCode::Refused, "{e:?}");
    assert!(!fake.sent().iter().any(|r| r.contains("evil.example")));
}

#[test]
fn unknown_codes_and_targets() {
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let t = tmp("codes");
    let b = DataDirs::at(t.0.join("friend"));
    let e = preview(&b, &Crucible::new(&ep, &fake, "0"), "ZZZZ-ZZZZ", None).unwrap_err();
    assert_eq!(e.code, ErrorCode::NotFound);
    assert_eq!(e.message, "We couldn't find that code. Codes look like H7QK-3MZP.");
    let e = preview(&b, &Crucible::new(&ep, &fake, "0"), "hello there", None).unwrap_err();
    assert_eq!(e.code, ErrorCode::NotFound);
}

#[test]
fn targets_are_read_from_codes_and_links() {
    assert_eq!(parse_target("h7qk-3mzp"), Some(Target::Code("H7QK3MZP".into())));
    assert_eq!(parse_target(" H7QK 3MZP "), Some(Target::Code("H7QK3MZP".into())));
    assert_eq!(parse_target("https://frostdev.io/kiln/crucible/p/H7QK-3MZP"), Some(Target::Code("H7QK3MZP".into())));
    assert_eq!(parse_target("kiln://crucible/H7QK-3MZP"), Some(Target::Code("H7QK3MZP".into())));
    assert_eq!(parse_target("kiln://crucible/pack/ashen-frontier"), Some(Target::Pack("ashen-frontier".into())));
    assert_eq!(parse_target("https://frostdev.io/kiln/crucible/ashen-frontier?ref=x"), Some(Target::Pack("ashen-frontier".into())));
    assert_eq!(parse_target("pv_abc123"), Some(Target::Version("pv_abc123".into())));
    for bad in ["", "H7QK-3MZ", "H0QK-3MZP", "https://evil.example/kiln/crucible/p/H7QK-3MZP", "kiln://crucible/../x", "kiln://crucible/api", "javascript:alert(1)"] {
        assert_eq!(parse_target(bad), None, "{bad}");
    }
    assert_eq!(show_code("H7QK3MZP"), "H7QK-3MZP");
}

#[test]
fn dev_rewrites_and_upload_hosts() {
    let mut ep = Endpoints::production();
    assert!(ep.upload_host_ok("abc123.r2.cloudflarestorage.com"));
    assert!(!ep.upload_host_ok("r2.cloudflarestorage.com.evil.com"));
    assert!(!ep.upload_host_ok(".r2.cloudflarestorage.com"));
    assert!(!ep.upload_host_ok("cdn.frostdev.io"));
    assert_eq!(ep.enabled(), !KEYS.is_empty());
    ep.rewrites = vec![("http://127.0.0.1:9000/b".into(), "https://cdn.crucible.test".into())];
    assert_eq!(ep.rewrite("http://127.0.0.1:9000/b/f/x.zip"), "https://cdn.crucible.test/f/x.zip");
    assert_eq!(ep.rewrite("https://cdn.frostdev.io/f/x.zip"), "https://cdn.frostdev.io/f/x.zip");
}

#[test]
fn a_studio_pack_is_published_link_only_then_public_with_its_credits() {
    use crate::modpack::studio::{self, NewPack, Visibility};
    use publish::{PublishRequest, publish};
    let t = tmp("publish");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (a, pid) = creator(&t);
    let st = studio::create(&a, &NewPack { name: "Ember run".into(), from: Some(pid.clone()), id: None }).unwrap();
    let anon = Crucible::new(&ep, &fake, "0.2.0-dev");
    let req = PublishRequest { version: "1.0.0".into(), changes: Some("first".into()), ..Default::default() };
    assert_eq!(publish(&a, &anon, &st.dir, &req, None, &mut NoProgress).unwrap_err().code, ErrorCode::AuthFailed);
    let c = Crucible::new(&ep, &fake, "0.2.0-dev").with_session(Some(KEY.into()));

    // public needs credits, the attestation and a summary: refused before anything is sent
    let before = fake.sent().len();
    let e = publish(&a, &c, &st.dir, &PublishRequest { visibility: Visibility::Public, ..req.clone() }, None, &mut NoProgress).unwrap_err();
    assert_eq!(e.code, ErrorCode::Refused);
    assert!(e.message.contains("credits for") && e.message.contains("summary") && e.message.contains("confirm"), "{}", e.message);
    assert_eq!(fake.sent().len(), before);

    // link-only: no credits needed
    let r1 = publish(&a, &c, &st.dir, &req, None, &mut NoProgress).unwrap();
    assert!(!r1.public && r1.needs.is_empty());
    assert_eq!(r1.version.status, "ready");
    assert_eq!(r1.files, 2);
    assert_eq!(r1.file, "ember-run-1.0.0.kilnpack");
    let s = studio::read_source(&st.dir).unwrap();
    assert_eq!(s.version, "1.0.1", "the next version is proposed");
    let p = s.published.clone().unwrap();
    assert_eq!((p.pack.as_str(), p.versions.len()), (r1.pack.id.as_str(), 1));
    assert!(st.dir.join("versions/ember-run-1.0.0.kilnpack").exists());
    // the same version again is refused
    assert_eq!(publish(&a, &c, &st.dir, &req, None, &mut NoProgress).unwrap_err().code, ErrorCode::Refused);

    // credits for both mods, a summary, the attestation: version 2 goes up and the pack goes public
    let mut credits = BTreeMap::new();
    for id in ["alpha", "beta"] {
        credits.insert(id.to_string(), pack::Credit { author: "frostdev".into(), source: format!("https://github.com/frostdev-ops/{id}"), match_kind: Some("kiln".into()) });
    }
    let st = studio::set_credits(&a, &st.dir, &credits).unwrap();
    let mut s = st.source.clone();
    s.summary = Some("A loot run with friends".into());
    studio::save(&a, &st.dir, s).unwrap();
    let puts = fake.s.lock().unwrap().puts.len();
    let r2 = publish(&a, &c, &st.dir, &PublishRequest { version: "1.0.1".into(), visibility: Visibility::Public, attest: true, ..Default::default() }, None, &mut NoProgress).unwrap();
    assert!(r2.public, "{:?}", r2.needs);
    assert_eq!(r2.pack.id, r1.pack.id, "the same pack");
    assert_eq!(r2.version.number, 2);
    assert_eq!(fake.s.lock().unwrap().puts.len(), puts, "nothing new to send");
    let sent = fake.s.lock().unwrap().credits.get(&r2.version.id).cloned().unwrap();
    assert_eq!(sent.as_array().unwrap().len(), 2);
    assert_eq!(sent[0]["author"], "frostdev");
    let s = studio::read_source(&st.dir).unwrap();
    assert_eq!(s.published.as_ref().unwrap().visibility, Visibility::Public);
    assert_eq!(s.published.as_ref().unwrap().versions.len(), 2);
    assert!(fake.sent().iter().any(|r| r.starts_with("PATCH ")));
}

#[test]
fn a_publish_that_stops_midway_resumes_the_same_pack_and_version() {
    use crate::modpack::studio::{self, NewPack, Visibility};
    use publish::{PublishRequest, publish, set_visibility};
    let t = tmp("publish-resume");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (a, pid) = creator(&t);
    let st = studio::create(&a, &NewPack { name: "Ember run".into(), from: Some(pid), id: None }).unwrap();
    let mut credits = BTreeMap::new();
    for id in ["alpha", "beta"] {
        credits.insert(id.to_string(), pack::Credit { author: "frostdev".into(), source: format!("https://github.com/frostdev-ops/{id}"), match_kind: None });
    }
    studio::set_credits(&a, &st.dir, &credits).unwrap();
    let c = Crucible::new(&ep, &fake, "0.2.0-dev").with_session(Some(KEY.into()));
    // the credits call fails: the pack and the pending version are remembered
    fake.s.lock().unwrap().fail_credits = 1;
    let req = PublishRequest { version: "1.0.0".into(), ..Default::default() };
    assert!(publish(&a, &c, &st.dir, &req, None, &mut NoProgress).is_err());
    let s = studio::read_source(&st.dir).unwrap();
    let p = s.published.clone().expect("remembered at once");
    assert_eq!((p.versions.len(), p.versions[0].pending, s.version.as_str()), (1, true, "1.0.0"));
    // the same version again resumes it: no second pack, no second version
    let r = publish(&a, &c, &st.dir, &req, None, &mut NoProgress).unwrap();
    assert_eq!(r.pack.id, p.pack);
    assert_eq!(r.version.id, p.versions[0].id);
    assert_eq!(fake.s.lock().unwrap().packs.len(), 1);
    assert_eq!(fake.s.lock().unwrap().versions.len(), 1);
    let s = studio::read_source(&st.dir).unwrap();
    assert!(!s.published.as_ref().unwrap().versions[0].pending);
    assert_eq!(s.version, "1.0.1");
    // Make public without a new version (summary + attestation first)
    let mut src = s.clone();
    src.summary = Some("A loot run with friends".into());
    studio::save(&a, &st.dir, src).unwrap();
    assert_eq!(set_visibility(&a, &c, &st.dir, Visibility::Public, false).unwrap_err().code, ErrorCode::Refused);
    let v = set_visibility(&a, &c, &st.dir, Visibility::Public, true).unwrap();
    assert!(v.public && v.needs.is_empty(), "{v:?}");
    assert_eq!(studio::read_source(&st.dir).unwrap().published.unwrap().visibility, Visibility::Public);
    // deleted or taken down on Crucible: refused, never quietly a new pack; fresh makes one
    fake.s.lock().unwrap().gone.insert(p.pack.clone());
    let next = PublishRequest { version: "1.0.1".into(), ..Default::default() };
    let e = publish(&a, &c, &st.dir, &next, None, &mut NoProgress).unwrap_err();
    assert_eq!(e.code, ErrorCode::Refused);
    assert_eq!(e.detail.as_ref().and_then(|d| d.get("gone")), Some(&serde_json::json!(true)));
    assert_eq!(fake.s.lock().unwrap().packs.len(), 1);
    let r2 = publish(&a, &c, &st.dir, &PublishRequest { fresh: true, ..next }, None, &mut NoProgress).unwrap();
    assert_ne!(r2.pack.id, p.pack);
    let s = studio::read_source(&st.dir).unwrap();
    assert_eq!(s.published.as_ref().unwrap().pack, r2.pack.id);
    assert_eq!(s.published.as_ref().unwrap().versions.len(), 2, "the local history stays");
}

#[test]
fn an_update_from_another_crucible_pack_with_the_same_index_id_is_refused() {
    // review: an instance follows its own Crucible pack, not any pack whose creator picked the same index id
    let t = tmp("samepackid");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (a, pid) = creator(&t);
    let ca = Crucible::new(&ep, &fake, "0.2.0-dev").with_session(Some(KEY.into()));
    let r1 = share(&a, &ca, &ShareRequest { profile: pid.clone(), ..Default::default() }, None, &mut NoProgress).unwrap();
    let b = DataDirs::at(t.0.join("friend"));
    let cb = Crucible::new(&ep, &fake, "0.2.0-dev");
    let ir = install(&b, &cb, None, &InstallRequest { target: r1.version.id.clone(), consent: true, ..Default::default() }, None, false, &mut NoProgress).unwrap();
    let other = DataDirs::at(t.0.join("other"));
    let op = crate::profile::create(&other, "Ember run", None).unwrap();
    add_mods(&other, &op.id, &t.0.join("o"), &["delta"]);
    let r2 = share(&other, &ca, &ShareRequest { profile: op.id.clone(), ..Default::default() }, None, &mut NoProgress).unwrap();
    assert_ne!(r2.pack.id, r1.pack.id);
    assert_eq!(cb.index(&r2.version.id).unwrap().0.id, cb.index(&r1.version.id).unwrap().0.id, "the same index id");
    let e = update(&b, &cb, &UpdateRequest { profile: ir.profile.id.clone(), version_id: r2.version.id.clone(), consent: true, ..Default::default() }, &mut NoProgress).unwrap_err();
    assert_eq!(e.code, ErrorCode::Refused, "{e:?}");
    // and the preview of the other pack does not call the friend's instance its own
    let (p, _) = preview(&b, &cb, &r2.pack.url, None).unwrap();
    assert_eq!(p.instance, None);
}

#[test]
fn an_installed_pack_is_named_after_the_pack_and_its_creator_and_never_twice() {
    let t = tmp("naming");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (a, pid) = creator(&t);
    let ca = Crucible::new(&ep, &fake, "0.2.0-dev").with_session(Some(KEY.into()));
    let r = share(&a, &ca, &ShareRequest { profile: pid, ..Default::default() }, None, &mut NoProgress).unwrap();
    // the friend has a profile of the same name as the sharer's ("My mods" twice was the dev.30 report)
    let b = DataDirs::at(t.0.join("friend"));
    crate::profile::create(&b, "Ember run", None).unwrap();
    let cb = Crucible::new(&ep, &fake, "0.2.0-dev");
    let (p, _) = preview(&b, &cb, &r.pack.code, None).unwrap();
    // the service's check result reaches the preview (the launcher's "Checks: ..." line)
    let checks = p.version.checks.clone().expect("checks");
    assert_eq!((checks["state"].as_str(), checks["warnings"].as_array().map(Vec::len)), (Some("done"), Some(1)));
    assert_eq!(serde_json::to_value(&p).unwrap()["version"]["checks"]["state"], "done");
    let req = InstallRequest { target: p.version.id.clone(), consent: true, play: Some(false), ..Default::default() };
    let one = install(&b, &cb, None, &req, p.pack.owner.as_ref(), false, &mut NoProgress).unwrap();
    assert_eq!(one.profile.name, "Ember run (Emberfox's pack)");
    let two = install(&b, &cb, None, &req, p.pack.owner.as_ref(), false, &mut NoProgress).unwrap();
    assert_eq!(two.profile.name, "Ember run (Emberfox's pack) (2)");
    // a name typed in the install step that a profile has already gets a number too; no owner: the pack's own name
    let typed = install(&b, &cb, None, &InstallRequest { name: Some("ember RUN".into()), ..req.clone() }, None, false, &mut NoProgress).unwrap();
    assert_eq!(typed.profile.name, "ember RUN (2)");
    let bare = install(&b, &cb, None, &req, None, false, &mut NoProgress).unwrap();
    assert_eq!(bare.profile.name, "Ember run (3)");
    let names: Vec<String> = crate::profile::list(&b).unwrap().into_iter().map(|s| s.name.to_lowercase()).collect();
    let unique: std::collections::BTreeSet<&String> = names.iter().collect();
    assert_eq!(unique.len(), names.len(), "{names:?}");
}

#[test]
fn a_lost_share_record_keeps_the_owner_key_of_the_earlier_link() {
    let t = tmp("anonymous-lost-record");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (a, pid) = creator(&t);
    let c = Crucible::new(&ep, &fake, "0");
    let req = ShareRequest { profile: pid.clone(), ..Default::default() };
    let first = share(&a, &c, &req, None, &mut NoProgress).unwrap();
    let before = ownership::load(&a, &ep.api, &pid).unwrap().unwrap();
    // shared.json is lost or unreadable: the profile looks never shared, so Share makes a new link...
    std::fs::write(root(&a).join("shared.json"), b"{broken").unwrap();
    let second = share(&a, &c, &req, None, &mut NoProgress).unwrap();
    assert_ne!(second.pack.id, first.pack.id);
    let now = ownership::load(&a, &ep.api, &pid).unwrap().unwrap();
    assert_eq!(now.pack.as_deref(), Some(second.pack.id.as_str()));
    // ...but the first link's capability, which nothing else can recover, is kept rather than overwritten
    let kept = ownership::retired(&a, &ep.api, &pid).unwrap();
    assert!(kept.iter().any(|o| o.key == before.key && o.pack.as_deref() == Some(first.pack.id.as_str())),
        "the earlier link's owner key was overwritten");
    assert!(!serde_json::to_string(&shared(&a, &pid).unwrap().unwrap()).unwrap().contains(&before.key));
}

/// The creator's pack for the repair tests: alpha and beta, gamma optional and off by default, srv for servers only
/// (Pack Studio's switches, as the signed index says them). -> (creator, the share, the friend, the friend's client)
fn repair_fixture<'a>(t: &Tmp, fake: &'a Fake, ep: &'a Endpoints) -> (DataDirs, ShareResult, DataDirs, Crucible<'a>) {
    let (a, pid) = creator(t);
    add_mods(&a, &pid, &t.0, &["gamma", "srv"]);
    let ca = Crucible::new(ep, fake, "0.2.0-dev").with_session(Some(KEY.into()));
    let r = share(&a, &ca, &ShareRequest { profile: pid, ..Default::default() }, None, &mut NoProgress).unwrap();
    {
        let mut s = fake.s.lock().unwrap();
        let ix = &mut s.versions.get_mut(&r.version.id).unwrap().2;
        for m in ix["mods"].as_array_mut().unwrap() {
            if m["id"] == "gamma" {
                m["dep"] = "optional".into();
                m["default"] = false.into();
            }
            if m["id"] == "srv" {
                m["env"] = serde_json::json!({ "client": "unsupported", "server": "required" });
            }
        }
    }
    (a, r, DataDirs::at(t.0.join("friend")), Crucible::new(ep, fake, "0.2.0-dev"))
}

fn lib_ref(dirs: &DataDirs, profile: &crate::profile::Profile, id: &str) -> LibraryRef {
    let m = profile.mods.kiln.iter().find(|m| m.id == id).unwrap_or_else(|| panic!("{id} is not in the profile"));
    let _ = dirs;
    LibraryRef { kind: ModKind::Kiln, id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone() }
}

/// The player report: a pack profile, one of its mods removed from the Library, the same code opened again. The
/// preview names what is missing for the installed version; "Download missing mods" fetches exactly that (an optional
/// mod switched off, the server's mod and what the Library has are not asked for), checks it as an install does, and
/// puts it back in the Library; the profile, the instance and its version stay as they were, also with an update
/// waiting (the update stays the update screen's).
#[test]
fn a_pack_profile_gets_its_missing_mods_back_and_nothing_else_changes() {
    let t = tmp("repair");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (a, r1, b, cb) = repair_fixture(&t, &fake, &ep);
    let (pv, _) = preview(&b, &cb, &r1.pack.code, None).unwrap();
    assert_eq!(pv.repair, None, "nothing installed: nothing to repair");
    let ir = install(&b, &cb, None, &InstallRequest { target: pv.version.id.clone(), consent: true, play: Some(false), ..Default::default() }, None, false, &mut NoProgress).unwrap();
    let pid = ir.profile.id.clone();
    assert!(!ir.profile.mods.kiln.iter().find(|m| m.id == "gamma").unwrap().enabled, "gamma is off");
    let (fresh, _) = preview(&b, &cb, &r1.pack.code, None).unwrap();
    assert_eq!(fresh.repair.as_ref().map(|l| l.missing.len()), Some(0), "nothing missing after the install");

    // the player removes alpha and the switched-off gamma from the Library (past the profile that lists them)
    let alpha = lib_ref(&b, &ir.profile, "alpha");
    let gamma = lib_ref(&b, &ir.profile, "gamma");
    for r in [&alpha, &gamma] {
        crate::library::remove_with(&b, r, true, &mut |_, _| Ok(None)).unwrap();
    }
    // and the archives Crucible's install kept are gone too (a cleared cache): the repair has to download
    let _ = std::fs::remove_dir_all(root(&b).join("cache"));
    // the creator shares a second version meanwhile: an update waits
    add_mods(&a, &crate::profile::list(&a).unwrap()[0].id, &t.0.join("v2"), &["delta"]);
    let r2 = share(&a, &Crucible::new(&ep, &fake, "0.2.0-dev").with_session(Some(KEY.into())),
        &ShareRequest { profile: crate::profile::list(&a).unwrap()[0].id.clone(), ..Default::default() }, None, &mut NoProgress).unwrap();
    assert_ne!(r2.version.id, r1.version.id);

    let (p, _) = preview(&b, &cb, &r1.pack.code, None).unwrap();
    assert_eq!(p.instance.as_deref(), Some(pid.as_str()));
    assert_eq!(p.version.id, r2.version.id, "the preview shows the newest version");
    let lack = p.repair.clone().expect("the installed version's lacks");
    assert_eq!(lack.version, "1.0.0");
    assert_eq!(lack.missing.iter().map(|r| r.id.as_str()).collect::<Vec<_>>(), ["alpha"], "gamma is off, srv is the server's");
    let alpha_file = pv.index.mods.iter().find(|m| m.id == "alpha").unwrap().file.clone().unwrap();
    assert_eq!(lack.download, alpha_file.size);
    assert_eq!(pack::lacking_of(&b, &pid).unwrap(), lack);

    let profile_before = crate::profile::get(&b, &pid).unwrap();
    let instance_before = pack::instance(&b, &pid).unwrap().unwrap();
    let base_before = std::fs::read(b.profile(&pid).join("pack/base/kiln-modpack.json")).unwrap();
    let profiles_before = crate::profile::list(&b).unwrap().len();
    let sent = fake.sent().len();
    let done = repair(&b, &cb, &pid, &mut NoProgress).unwrap();
    let asked: Vec<String> = fake.sent()[sent..].to_vec();
    assert_eq!(done.imported.iter().map(|r| r.id.as_str()).collect::<Vec<_>>(), ["alpha"], "{done:?}");
    assert_eq!((done.version.as_str(), done.downloaded, done.left.len()), ("1.0.0", alpha_file.size, 0));
    // only the installed version's index and alpha's archive: nothing of gamma, srv, beta, the layer, the overrides or v2
    assert_eq!(asked, [format!("GET cdn.crucible.test/m/{}.json", r1.version.id), format!("GET cdn.crucible.test/f/{}.zip", alpha_file.sha256)], "{asked:?}");
    assert!(crate::library::path_of(&b, &alpha).unwrap().is_dir(), "alpha is back in the Library");
    assert!(!crate::library::path_of(&b, &gamma).unwrap().exists(), "the switched-off gamma was not downloaded");
    assert_eq!(crate::profile::get(&b, &pid).unwrap(), profile_before, "the profile is untouched");
    assert_eq!(pack::instance(&b, &pid).unwrap().unwrap(), instance_before, "the instance keeps its version");
    assert_eq!(std::fs::read(b.profile(&pid).join("pack/base/kiln-modpack.json")).unwrap(), base_before);
    assert_eq!(crate::profile::list(&b).unwrap().len(), profiles_before, "no new profile");
    assert!(crate::profile::list(&b).unwrap().iter().all(|s| s.active.is_empty()), "nothing activated");
    assert!(pack::lacking_of(&b, &pid).unwrap().is_empty());
    assert_eq!(updates(&b, &cb).unwrap().len(), 1, "the update still waits");
    // nothing missing: nothing is asked of Crucible
    let sent = fake.sent().len();
    let again = repair(&b, &cb, &pid, &mut NoProgress).unwrap();
    assert!(again.imported.is_empty() && again.left.is_empty());
    assert_eq!(fake.sent().len(), sent);
}

/// A missing mod's archive that does not match its sha256 puts nothing into the Library (the install's check).
#[test]
fn a_repair_with_a_tampered_download_puts_nothing_in_the_library() {
    let t = tmp("repair-tampered");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (_a, r1, b, cb) = repair_fixture(&t, &fake, &ep);
    let ir = install(&b, &cb, None, &InstallRequest { target: r1.version.id.clone(), consent: true, play: Some(false), ..Default::default() }, None, false, &mut NoProgress).unwrap();
    let alpha = lib_ref(&b, &ir.profile, "alpha");
    crate::library::remove_with(&b, &alpha, true, &mut |_, _| Ok(None)).unwrap();
    let _ = std::fs::remove_dir_all(root(&b).join("cache"));
    let sha = cb.index(&r1.version.id).unwrap().0.mods.iter().find(|m| m.id == "alpha").unwrap().file.clone().unwrap().sha256;
    fake.s.lock().unwrap().tamper = Some(sha);
    let e = repair(&b, &cb, &ir.profile.id, &mut NoProgress).unwrap_err();
    assert_eq!(e.code, ErrorCode::HashMismatch, "{e:?}");
    assert!(!crate::library::path_of(&b, &alpha).unwrap().exists());
    assert_eq!(pack::lacking_of(&b, &ir.profile.id).unwrap().missing, [alpha]);
}

/// A pack installed from a file (no Crucible): its missing mods come back from the pack file of the installed version;
/// another version's file is refused and changes nothing.
#[test]
fn a_file_pack_is_repaired_from_its_own_file_only() {
    let t = tmp("repair-file");
    let (a, pid) = creator(&t);
    let built = |version: &str| pack::build(&a, &pack::BuildRequest { profile: pid.clone(), version: version.into(), ..Default::default() }, &t.0.join(format!("out-{version}")), &mut NoProgress).unwrap();
    let v1 = built("1.0.0");
    let b = DataDirs::at(t.0.join("friend"));
    let src = |b: &pack::BuiltPack| ModpackSource::Path { path: b.file.clone(), sha256: Some(b.sha256.clone()) };
    let ir = pack::install(&b, None, &pack::InstallRequest { source: src(&v1), consent: true, allow_http: false, name: None, optional: BTreeMap::new(), activate: None }, &mut NoProgress).unwrap();
    let beta = lib_ref(&b, &ir.profile, "beta");
    crate::library::remove_with(&b, &beta, true, &mut |_, _| Ok(None)).unwrap();
    assert_eq!(pack::lacking_of(&b, &ir.profile.id).unwrap().missing, [beta.clone()]);
    // no file and no address it came from: the player picks the file
    let e = pack::repair(&b, &pack::RepairRequest { profile: ir.profile.id.clone(), source: None, allow_http: false }, &mut NoProgress).unwrap_err();
    assert_eq!(e.code, ErrorCode::NotFound, "{e:?}");
    add_mods(&a, &pid, &t.0.join("v2"), &["delta"]);
    let v2 = built("1.1.0");
    // the file preview: what is missing, and whether this file is the installed version that can put it back
    let i1 = pack::inspect(&b, &v1.file, Some(&v1.sha256)).unwrap();
    assert_eq!((i1.instance.as_deref(), i1.installed_version), (Some(ir.profile.id.as_str()), true));
    assert_eq!(i1.repair.map(|l| l.missing), Some(vec![beta.clone()]));
    assert!(!pack::inspect(&b, &v2.file, Some(&v2.sha256)).unwrap().installed_version);
    let e = pack::repair(&b, &pack::RepairRequest { profile: ir.profile.id.clone(), source: Some(src(&v2)), allow_http: false }, &mut NoProgress).unwrap_err();
    assert_eq!(e.code, ErrorCode::Refused, "{e:?}");
    assert!(!crate::library::path_of(&b, &beta).unwrap().exists());
    let before = (crate::profile::get(&b, &ir.profile.id).unwrap(), pack::instance(&b, &ir.profile.id).unwrap());
    let done = pack::repair(&b, &pack::RepairRequest { profile: ir.profile.id.clone(), source: Some(src(&v1)), allow_http: false }, &mut NoProgress).unwrap();
    assert_eq!(done.imported.iter().map(|r| r.id.as_str()).collect::<Vec<_>>(), ["beta"]);
    assert!(done.left.is_empty());
    assert!(crate::library::path_of(&b, &beta).unwrap().is_dir());
    assert_eq!((crate::profile::get(&b, &ir.profile.id).unwrap(), pack::instance(&b, &ir.profile.id).unwrap()), before);
}

/// "Load it" on a pack profile whose mod left the Library (all the preview offered before Download missing mods):
/// in Copy mode (a game folder Windows cannot name the data folder from) the activation stops with "not in the library";
/// with direct paths (Windows, a Proton prefix that reaches the data folder) it succeeds and writes the game's profile
/// file naming the missing mod's Library folder: the game starts without the mod, the only sign the resolve preview's
/// warning. Hence the preview's "missing" line before Load it.
#[cfg(unix)]
#[test]
fn loading_a_pack_profile_with_a_missing_mod() {
    let t = tmp("repair-load");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (_a, r1, b, cb) = repair_fixture(&t, &fake, &ep);
    let ir = install(&b, &cb, None, &InstallRequest { target: r1.version.id.clone(), consent: true, play: Some(false), ..Default::default() }, None, false, &mut NoProgress).unwrap();
    let alpha = lib_ref(&b, &ir.profile, "alpha");
    crate::library::remove_with(&b, &alpha, true, &mut |_, _| Ok(None)).unwrap();
    let game = t.0.join("game");
    std::fs::create_dir_all(game.join("kiln")).unwrap();
    std::fs::write(game.join("enshrouded.exe"), b"MZ").unwrap();
    let pfx = t.0.join("pfx");
    std::fs::create_dir_all(pfx.join("dosdevices")).unwrap();
    std::os::unix::fs::symlink("/", pfx.join("dosdevices/z:")).unwrap();
    let at = |prefix: Option<PathBuf>, mode| crate::game::GameInstall {
        id: "g-test".into(), kind: crate::game::GameKind::Client, path: game.clone(), exe: "enshrouded.exe".into(),
        app_id: crate::CLIENT_APP_ID, library: None, platform: crate::game::Platform::Proton, prefix, build_id: None,
        exe_sha256: None, steam_flavor: None, path_mode: mode, source: "manual".into(),
    };
    let e = crate::profile::activate(&b, &ir.profile.id, &at(None, crate::game::PathMode::Copy)).unwrap_err();
    assert_eq!(e.code, ErrorCode::NotFound, "{e:?}");
    assert!(e.message.contains("alpha") && e.message.contains("not in the library"), "{e:?}");
    let a = crate::profile::activate(&b, &ir.profile.id, &at(Some(pfx), crate::game::PathMode::Z)).unwrap();
    assert_eq!(a.profile_id, ir.profile.id);
    let f = crate::profile::read_game_profile(&game).unwrap().unwrap();
    assert!(f.mods.kiln.iter().any(|m| m.id == "alpha" && m.enabled), "the game's file names the missing mod: {:?}", f.mods.kiln);
    let rp = crate::profile::resolve_preview(&b, &crate::profile::get(&b, &ir.profile.id).unwrap(), crate::manifest::RunSide::Client).unwrap();
    assert!(rp.warnings.iter().any(|w| w.contains("alpha") && w.contains("not in the library")), "{:?}", rp.warnings);
    // after the repair the same activation has nothing to warn about
    repair(&b, &cb, &ir.profile.id, &mut NoProgress).unwrap();
    let rp = crate::profile::resolve_preview(&b, &crate::profile::get(&b, &ir.profile.id).unwrap(), crate::manifest::RunSide::Client).unwrap();
    assert!(!rp.warnings.iter().any(|w| w.contains("not in the library")), "{:?}", rp.warnings);
}

/// Two kinds of mod under one id (a Kiln and a SET "alpha"): the repair tells them apart by kind and stored file, so
/// only the one that left the Library is downloaded and imported.
#[test]
fn a_repair_tells_two_kinds_of_mod_with_one_id_apart() {
    let t = tmp("repair-same-id");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (a, pid) = creator(&t);
    let set = t.0.join("src/set/alpha");
    std::fs::create_dir_all(&set).unwrap();
    std::fs::write(set.join("init.lua"), b"print('alpha')").unwrap();
    let r = crate::library::import(&a, &[set], &mut NoProgress).unwrap();
    let lr = r.imported.iter().chain(&r.existing).next().unwrap().clone();
    let mut p = crate::profile::get(&a, &pid).unwrap();
    p.mods.set.push(crate::profile::ProfileMod { id: lr.id.clone(), version: lr.version.clone(), hash: lr.hash.clone(), enabled: true });
    crate::profile::save(&a, &p).unwrap();
    let ca = Crucible::new(&ep, &fake, "0").with_session(Some(KEY.into()));
    let shared = share(&a, &ca, &ShareRequest { profile: pid, ..Default::default() }, None, &mut NoProgress).unwrap();
    let b = DataDirs::at(t.0.join("friend"));
    let cb = Crucible::new(&ep, &fake, "0");
    let ir = install(&b, &cb, None, &InstallRequest { target: shared.version.id.clone(), consent: true, play: Some(false), ..Default::default() }, None, false, &mut NoProgress).unwrap();
    let set_alpha = ir.profile.mods.set.iter().find(|m| m.id == "alpha").map(|m| LibraryRef { kind: ModKind::Set, id: m.id.clone(), version: m.version.clone(), hash: m.hash.clone() }).unwrap();
    let kiln_alpha = lib_ref(&b, &ir.profile, "alpha");
    crate::library::remove_with(&b, &set_alpha, true, &mut |_, _| Ok(None)).unwrap();
    let _ = std::fs::remove_dir_all(root(&b).join("cache"));
    assert_eq!(pack::lacking_of(&b, &ir.profile.id).unwrap().missing, [set_alpha.clone()]);
    let ix = cb.index(&shared.version.id).unwrap().0;
    let set_file = ix.mods.iter().find(|m| m.id == "alpha" && m.kind == ModKind::Set).unwrap().file.clone().unwrap();
    let sent = fake.sent().len();
    let done = repair(&b, &cb, &ir.profile.id, &mut NoProgress).unwrap();
    assert_eq!(done.imported, [set_alpha.clone()], "{done:?}");
    assert_eq!((done.downloaded, done.left.len()), (set_file.size, 0));
    assert_eq!(fake.sent()[sent..], [format!("GET cdn.crucible.test/m/{}.json", shared.version.id), format!("GET cdn.crucible.test/f/{}.zip", set_file.sha256)]);
    assert!(crate::library::path_of(&b, &set_alpha).unwrap().exists() && crate::library::path_of(&b, &kiln_alpha).unwrap().is_dir());
}

/// Two kinds of mod with one id (a player's pack had an EMF and a ShroudLink "expeditions"): each part downloads its own
/// file. Before, the download took the first part's URL with the second part's size and failed "larger than allowed".
#[test]
fn two_mods_of_different_kinds_with_one_id_each_get_their_own_file() {
    let t = tmp("same-id");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (a, pid) = creator(&t);
    // a SET mod also called "alpha" (init.lua only), smaller than the Kiln "alpha"
    let set = t.0.join("src/set/alpha");
    std::fs::create_dir_all(&set).unwrap();
    std::fs::write(set.join("init.lua"), b"print('alpha')").unwrap();
    let r = crate::library::import(&a, &[set], &mut NoProgress).unwrap();
    let lr = r.imported.iter().chain(&r.existing).next().unwrap().clone();
    assert_eq!((lr.kind, lr.id.as_str()), (ModKind::Set, "alpha"));
    let mut p = crate::profile::get(&a, &pid).unwrap();
    p.mods.set.push(crate::profile::ProfileMod { id: lr.id.clone(), version: lr.version.clone(), hash: lr.hash.clone(), enabled: true });
    crate::profile::save(&a, &p).unwrap();
    let ca = Crucible::new(&ep, &fake, "0").with_session(Some(KEY.into()));
    let shared = share(&a, &ca, &ShareRequest { profile: pid, ..Default::default() }, None, &mut NoProgress).unwrap();
    let b = DataDirs::at(t.0.join("friend"));
    let cb = Crucible::new(&ep, &fake, "0");
    let (pv, _) = preview(&b, &cb, &shared.pack.code, None).unwrap();
    let alphas: Vec<_> = pv.index.mods.iter().filter(|m| m.id == "alpha").collect();
    assert_eq!(alphas.len(), 2, "{:?}", pv.index.mods);
    assert_ne!(alphas[0].file.as_ref().unwrap().size, alphas[1].file.as_ref().unwrap().size);
    // the preview counts each part's own file once
    let preview_ok = pv.missing.iter().filter(|m| m.id == "alpha").count() == 2
        && pv.download == pv.index.mods.iter().filter_map(|m| m.file.as_ref()).map(|f| f.size).sum::<u64>();
    install(&b, &cb, None, &InstallRequest { target: pv.version.id.clone(), consent: true, ..Default::default() }, None, false, &mut NoProgress).unwrap();
    assert!(preview_ok, "the preview counts each part's own file once: {:?} {}", pv.missing, pv.download);
    let have = crate::library::list(&b, None).unwrap();
    assert!(have.iter().any(|e| e.kind == ModKind::Set && e.id == "alpha"), "{have:?}");
    assert!(have.iter().any(|e| e.kind == ModKind::Kiln && e.id == "alpha"), "{have:?}");
}
