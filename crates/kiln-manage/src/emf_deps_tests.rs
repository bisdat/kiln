//! The EMF dependency check against kiln-emf's own rules (native/plugins/emf/src/mods.cpp): ranges, missing, off,
//! out-of-range and optional dependencies, the cascade, and the folders it reads.

use super::*;

fn dep(id: &str, range: &str, optional: bool) -> Dep {
    Dep { id: id.into(), range: range.into(), optional, url: None }
}

fn emf(entry: Option<&str>, id: &str, version: &str, enabled: bool, deps: Vec<Dep>) -> EmfMod {
    EmfMod { entry: entry.map(str::to_string), id: id.into(), name: id.into(), version: version.into(), enabled, side: String::new(), deps, broken: None }
}

/// PepegaJedi's Mage Skills 1.0.0: `"depends": {"lua": ">=1.36"}` and no lua mod.
fn mage_skills() -> EmfMod {
    emf(Some("mage_skills"), "mage_skills", "1.0.0", true, vec![dep("lua", ">=1.36", false)])
}

#[test]
fn ranges_read_as_emf_reads_them() {
    let yes = |r: &str, v: &str| assert_eq!(satisfies(r, v), Some(true), "{r} should take {v}");
    let no = |r: &str, v: &str| assert_eq!(satisfies(r, v), Some(false), "{r} should refuse {v}");
    yes(">=1.36", "1.36.0"); yes(">=1.36", "1.39"); no(">=1.36", "1.35.9");
    yes(">= 1.36", "2.0.0"); // a space after the operator
    yes("^1.2", "1.9.0"); no("^1.2", "2.0.0"); no("^1.2", "1.1.9");
    yes("^0.2.3", "0.2.9"); no("^0.2.3", "0.3.0");
    yes("~1.2", "1.2.7"); no("~1.2", "1.3.0");
    yes("1.2.x", "1.2.5"); no("1.2.x", "1.3.0");
    yes("1.2", "1.5.0"); // a bare version is ^
    yes(">=1.0 <2", "1.9.9"); no(">=1.0 <2", "2.0.0");
    yes(">=1.0, <2", "1.0.0");
    yes("1.2 - 2", "2.9.9"); no("1.2 - 2", "3.0.0");
    yes("1.2.0 - 2.0.0", "2.0.0"); no("1.2.0 - 2.0.0", "2.0.1");
    yes("^1 || ^3", "3.1.0"); no("^1 || ^3", "2.0.0");
    yes("", "0.0.1"); yes("*", "7");
    yes("=1.2.3", "1.2.3"); no("=1.2.3", "1.2.4");
    yes(">1.2", "1.3.0"); no(">1.2", "1.2.9"); no(">1.2.3", "1.2.3");
    yes("<=1.2", "1.2.9"); no("<=1.2", "1.3.0");
    // what EMF refuses: an empty intersection, words, a version with more than three parts
    assert_eq!(satisfies(">2 <1", "1.5.0"), None);
    assert_eq!(satisfies("latest", "1.0.0"), None);
    assert_eq!(satisfies("^1", "1.0.0.1"), None);
    assert_eq!(satisfies("^1", "1.0.0-beta"), None);
}

#[test]
fn a_missing_required_dependency_is_reported_with_emfs_words_and_how_to_get_it() {
    let out = check(&[mage_skills()], RunSide::Client);
    assert_eq!(out, vec![EmfDepProblem {
        id: "mage_skills".into(), dep: "lua".into(), dep_name: Some("EMF's lua mod".into()), range: ">=1.36".into(), state: DepState::Missing,
        found: None, with_emf: true, url: Some(EMF_PAGE.into()), reason: "needs lua, which is not installed".into(),
    }]);
}

#[test]
fn emfs_own_mods_always_link_emfs_page_and_another_mods_link_is_its_entrys() {
    // a mod.json that names some other page for lua: "Get EMF" still opens EMF's page
    let odd = Dep { url: Some("https://example.com/not-emf".into()), ..dep("lua", ">=1.36", false) };
    let pet = Dep { url: Some("https://www.nexusmods.com/enshrouded/mods/9001".into()), ..dep("pet-core", "^2.0", false) };
    let m = emf(Some("mage_skills"), "mage_skills", "1.0.0", true, vec![odd]);
    let p = emf(Some("pets"), "pets", "1.0.0", true, vec![pet]);
    let out = check(&[m, p], RunSide::Client);
    let by = |id: &str| out.iter().find(|x| x.id == id).unwrap();
    assert_eq!((by("mage_skills").with_emf, by("mage_skills").url.as_deref()), (true, Some(EMF_PAGE)));
    assert_eq!((by("pets").with_emf, by("pets").url.as_deref()), (false, Some("https://www.nexusmods.com/enshrouded/mods/9001")));
}

#[test]
fn a_dependency_in_range_is_no_problem() {
    let lua = emf(Some("lua"), "lua", "1.39.0", true, vec![]);
    assert_eq!(check(&[mage_skills(), lua.clone()], RunSide::Client), vec![]);
    // a drop-in in emf/mods counts as well (no profile entry)
    let dropped = EmfMod { entry: None, ..lua };
    assert_eq!(check(&[mage_skills(), dropped], RunSide::Client), vec![]);
}

#[test]
fn a_dependency_out_of_range_says_too_old_or_too_new() {
    let old = check(&[mage_skills(), emf(Some("lua"), "lua", "1.33.0", true, vec![])], RunSide::Client);
    assert_eq!(old.len(), 1);
    assert_eq!(old[0].state, DepState::TooOld);
    assert_eq!(old[0].found.as_deref(), Some("1.33.0"));
    assert_eq!(old[0].dep_name.as_deref(), Some("lua"), "the name its own mod.json gives");
    assert_eq!(old[0].reason, "needs lua >=1.36 (1.36.0 or newer), found 1.33.0: too old (update lua)");
    let caret = emf(Some("pets"), "pets", "1.0.0", true, vec![dep("beastmaster", "^1.2", false)]);
    let new = check(&[caret, emf(Some("beastmaster"), "beastmaster", "2.0.0", true, vec![])], RunSide::Client);
    assert_eq!(new[0].state, DepState::TooNew);
    assert_eq!(new[0].reason, "needs beastmaster ^1.2 (1.2.0 or newer, below 2.0.0), found 2.0.0: too new (look for an update of this mod)");
}

#[test]
fn a_switched_off_dependency_is_off() {
    let out = check(&[mage_skills(), emf(Some("lua"), "lua", "1.39.0", false, vec![])], RunSide::Client);
    assert_eq!(out[0].state, DepState::Off);
    assert_eq!(out[0].reason, "needs lua, which is switched off");
}

#[test]
fn optional_dependencies_are_ignored_when_missing_or_off_but_their_version_counts() {
    let opt = |range: &str| emf(Some("extras"), "extras", "1.0.0", true, vec![dep("lua", range, true)]);
    assert_eq!(check(&[opt(">=1.36")], RunSide::Client), vec![]);
    assert_eq!(check(&[opt(">=1.36"), emf(Some("lua"), "lua", "1.39.0", false, vec![])], RunSide::Client), vec![]);
    // EMF checks a present optional dependency's version too (resolve_order's version_problem)
    let out = check(&[opt(">=1.36"), emf(Some("lua"), "lua", "1.20.0", true, vec![])], RunSide::Client);
    assert_eq!(out[0].state, DepState::TooOld);
}

#[test]
fn a_rejection_reaches_the_mods_that_need_it() {
    let lua = emf(Some("lua"), "lua", "1.39.0", true, vec![dep("emf_core_tools", "*", false)]);
    let out = check(&[mage_skills(), lua], RunSide::Client);
    let by = |id: &str| out.iter().find(|p| p.id == id).unwrap();
    assert_eq!(by("lua").state, DepState::Missing);
    assert!(!by("lua").with_emf && by("lua").url.is_none(), "not one of EMF's own mods: named, no link");
    assert_eq!(by("mage_skills").state, DepState::NotLoading);
    assert_eq!(by("mage_skills").reason, "needs lua, which is not loading (needs emf_core_tools, which is not installed)");
    // a broken mod.json and a client mod on a server do not load either
    let broken = EmfMod { broken: Some("mod.json has no \"id\"".into()), ..emf(Some("lua"), "lua", "1.39.0", true, vec![]) };
    assert_eq!(check(&[mage_skills(), broken], RunSide::Client)[0].state, DepState::NotLoading);
    let client = EmfMod { side: "client".into(), ..emf(Some("lua"), "lua", "1.39.0", true, vec![]) };
    assert_eq!(check(&[mage_skills(), client.clone()], RunSide::Client), vec![]);
    assert_eq!(check(&[mage_skills(), client], RunSide::Server)[0].state, DepState::NotLoading);
}

#[test]
fn only_switched_on_profile_entries_are_reported_and_the_first_copy_of_an_id_wins() {
    let off = EmfMod { enabled: false, ..mage_skills() };
    assert_eq!(check(&[off], RunSide::Client), vec![]);
    let dropped = EmfMod { entry: None, ..mage_skills() };
    assert_eq!(check(&[dropped], RunSide::Client), vec![], "a drop-in is no row of the profile");
    // the profile's lua 1.33 comes first; a newer copy in emf/mods is ignored, as kiln-emf ignores it
    let mods = [mage_skills(), emf(Some("lua"), "lua", "1.33.0", true, vec![]), emf(None, "lua", "1.39.0", true, vec![])];
    assert_eq!(check(&mods, RunSide::Client)[0].state, DepState::TooOld);
}

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) { let _ = std::fs::remove_dir_all(&self.0); }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("km-emf-deps-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p)
}
fn put(root: &Path, rel: &str, data: &str) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}

#[test]
fn mod_json_is_read_as_emf_reads_it() {
    let t = tmp("read");
    put(&t.0, "mage/mod.json", r#"{"id": "mage_skills", "name": "Mage Skills", "version": "1.0.0", "emf": 43, "main": "main.lua",
        // EMF reads comments
        "depends": {"LUA": ">=1.36", "beastmaster": {"version": "^1.2", "optional": true, "url": "https://www.nexusmods.com/enshrouded/mods/1"}}}"#);
    let m = read_folder(&t.0.join("mage"), Some("mage-skills"), true).unwrap();
    assert_eq!((m.id.as_str(), m.name.as_str(), m.version.as_str(), m.entry.as_deref()), ("mage_skills", "Mage Skills", "1.0.0", Some("mage-skills")));
    assert_eq!(m.broken, None);
    // in EMF's order: by the key as written ("LUA" before "beastmaster"), the id then in lower case
    assert_eq!(m.deps, vec![
        Dep { id: "lua".into(), range: ">=1.36".into(), optional: false, url: None },
        Dep { id: "beastmaster".into(), range: "^1.2".into(), optional: true, url: Some("https://www.nexusmods.com/enshrouded/mods/1".into()) },
    ]);
    // a list form, and links that are no https page
    put(&t.0, "list/mod.json", r#"{"id": "l", "version": "1", "dependencies": ["lua", {"id": "x", "version": "~2", "url": "file:///c:/x"},
        {"id": "y", "url": "http://example.com/y"}]}"#);
    let l = read_folder(&t.0.join("list"), None, true).unwrap();
    assert_eq!(l.deps, vec![dep("lua", "", false), dep("x", "~2", false), dep("y", "", false)]);
    // what EMF refuses: the mod does not load (its dependents say "not loading")
    for (name, text, why) in [
        ("both", r#"{"id": "b", "version": "1.0.0", "depends": {}, "dependencies": {}}"#, "both \"depends\" and \"dependencies\""),
        ("range", r#"{"id": "r", "version": "1.0.0", "depends": {"lua": "latest"}}"#, "matches no version"),
        ("version", r#"{"id": "v", "version": "1.0.0-beta"}"#, "\"version\" must be"),
        ("id", r#"{"id": "Big Mod", "version": "1.0.0"}"#, "is not 1-48 of"),
    ] {
        put(&t.0, &format!("{name}/mod.json"), text);
        let m = read_folder(&t.0.join(name), None, true).unwrap();
        assert!(m.broken.as_deref().is_some_and(|b| b.contains(why)), "{name}: {:?}", m.broken);
    }
    assert_eq!(read_folder(&t.0.join("nothing"), None, true), None);
}

#[test]
fn the_emf_mods_folder_holds_drop_ins_but_not_kilns_copies() {
    let t = tmp("dropins");
    put(&t.0, "lua/mod.json", r#"{"id": "lua", "name": "Lua Scripts", "version": "1.39.0", "emf": 20}"#);
    put(&t.0, "mage_skills/mod.json", r#"{"id": "mage_skills", "version": "1.0.0", "main": "main.lua", "depends": {"lua": ">=1.36"}}"#);
    put(&t.0, &format!("mage_skills/{}", crate::engine_mods::MARKER), r#"{"format": 1, "kind": "emf", "id": "mage_skills", "version": "1.0.0", "hash": "x"}"#);
    put(&t.0, "Big_Stacks.dll", "MZ");
    put(&t.0, "notes.txt", "");
    let found = dropins(&t.0);
    let ids: Vec<(&str, &str)> = found.iter().map(|m| (m.id.as_str(), m.version.as_str())).collect();
    assert!(crate::engine_mods::marker(&t.0.join("mage_skills")).is_some());
    // Kiln's copy of a profile entry is that entry, not a drop-in; a loose DLL is one, named after its file
    assert_eq!(ids, vec![("big_stacks", "0.0.0"), ("lua", "1.39.0")]);
    assert!(found.iter().all(|m| m.entry.is_none() && m.enabled));
    let mut all = vec![mage_skills()];
    all.extend(found);
    assert_eq!(check(&all, RunSide::Client), vec![]);
}
