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
    seq: u64,
    public: std::collections::BTreeSet<String>,
    credits: BTreeMap<String, Value>, // version -> the credits PUT
    patches: Vec<Value>,
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
            "title": title, "summary": "", "icon": null, "owner": { "id": OWNER, "name": "Emberfox" }, "visibility": if s.public.contains(id) { "public" } else { "link" },
            "upvotes": 0, "comments": 0, "installs": s.installs, "mods": 0, "builds": [], "latest": latest })
    }

    fn version_view(s: &State, id: &str) -> Value {
        let (_, number, index, status) = &s.versions[id];
        serde_json::json!({ "id": id, "number": number, "version": index["version"], "changes": index.get("changes"), "status": status,
            "size": Self::files_of(index).iter().map(|f| f.2).sum::<u64>(), "published": null })
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
        let parts: Vec<&str> = p.trim_start_matches('/').split('/').collect();
        let need_auth = matches!((req.method, parts.as_slice()), (Method::Post, ["quickshare"]) | (Method::Post, ["packs"]) | (Method::Post, ["packs", _, "versions"]) | (Method::Post, ["uploads", ..])
            | (Method::Put, _) | (Method::Patch, _));
        if need_auth && !authed {
            return json(401, serde_json::json!({ "error": "signed-out", "message": "Sign in with Discord first." }));
        }
        match (req.method, parts.as_slice()) {
            (Method::Post, ["quickshare"]) | (Method::Post, ["packs"]) => {
                s.seq += 1;
                let id = format!("pck_{}", s.seq);
                let code = format!("H7QK3MZ{}", &CODE_ALPHABET[s.packs.len()..s.packs.len() + 1]);
                s.packs.insert(id.clone(), (code, "ember-run".into(), jbody["title"].as_str().unwrap_or("x").into(), None));
                let (vid, uploads, have) = Self::add_version(&mut s, &id, jbody["index"].clone());
                json(201, serde_json::json!({ "pack": Self::pack_view(&s, &id), "version": Self::version_view(&s, &vid), "uploads": uploads, "have": have }))
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
            (Method::Put, ["versions", vid, "credits"]) => {
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
fn sharing_needs_the_sign_in_and_sends_nothing_without_it() {
    let t = tmp("signin");
    let fake = Fake::new();
    let ep = endpoints(&fake.signer.key);
    let (a, pid) = creator(&t);
    let e = share(&a, &Crucible::new(&ep, &fake, "0"), &ShareRequest { profile: pid, ..Default::default() }, None, &mut NoProgress).unwrap_err();
    assert_eq!(e.code, ErrorCode::AuthFailed);
    assert!(e.message.contains("Your friends don't need to"), "{}", e.message);
    assert!(fake.sent().is_empty());
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
