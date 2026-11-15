//! The "before beta" copy and the round trip: a data folder in 0.2.0-dev.28's formats, a test build of dev.29 writes
//! in it, the release reads what it wrote, and the copy puts the release's files back byte for byte.

use super::*;

/// 0.2.0-dev.28's launcher settings, as that build reads them (git show 2301cc5f:launcher/src-tauri/src/commands/app.rs;
/// the same fields, defaults and serde attributes; no `deny_unknown_fields`).
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
#[allow(dead_code)]
struct Dev28Settings {
    #[serde(default)]
    steam_roots: Vec<String>,
    #[serde(default)]
    default_game_id: Option<String>,
    #[serde(default)]
    allow_http_modpacks: bool,
    #[serde(default = "ten")]
    saves_keep: u32,
    #[serde(default = "yes")]
    update_check: bool,
    #[serde(default)]
    update_channel: Option<String>,
    #[serde(default = "yes")]
    animated_fire: bool,
    #[serde(default = "yes")]
    add_to_active_profile: bool,
    #[serde(default)]
    ui_mode: Option<String>,
    #[serde(default = "yes")]
    mod_folder_notes: bool,
    #[serde(default)]
    mod_updates_daily: bool,
}
fn ten() -> u32 {
    10
}
fn yes() -> bool {
    true
}

/// 0.2.0-dev.28's channel choice (selfupdate/feed.rs there): only "stable" and "dev"; anything else follows the build.
fn dev28_channel(setting: Option<&str>, version: &str) -> &'static str {
    match setting {
        Some("stable") => "stable",
        Some("dev") => "dev",
        _ => if version.contains('-') { "dev" } else { "stable" },
    }
}

const DEV28_SETTINGS: &str = r#"{
  "steamRoots": [],
  "defaultGameId": "client-steam",
  "allowHttpModpacks": false,
  "savesKeep": 10,
  "updateCheck": true,
  "animatedFire": true,
  "addToActiveProfile": true,
  "modFolderNotes": true,
  "modUpdatesDaily": false
}
"#;
const DEV28_UPDATE: &str = r#"{
  "format": 1,
  "checked": "2026-11-14T23:00:00Z"
}
"#;
const DEV28_PROFILE: &str = r#"{
  "format": 1,
  "id": "ember-run",
  "name": "Ember run",
  "created": "2026-11-04T10:00:00Z",
  "updated": "2026-11-14T20:00:00Z",
  "generation": 3,
  "notes": "",
  "mods": {
    "kiln": []
  },
  "dropins": []
}
"#;

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn tmp(tag: &str) -> Tmp {
    let p = std::env::temp_dir().join(format!("kiln-snapshot-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    Tmp(p)
}
fn put(root: &Path, rel: &str, text: &str) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, text).unwrap();
}
fn read(root: &Path, rel: &str) -> String {
    std::fs::read_to_string(root.join(rel)).unwrap()
}

/// A data folder as 0.2.0-dev.28 left it: settings, update.json, one profile, one mod in the Library.
fn dev28_folder(root: &Path) {
    put(root, "settings.json", DEV28_SETTINGS);
    put(root, "update.json", DEV28_UPDATE);
    put(root, "profiles/ember-run/meta.json", DEV28_PROFILE);
    put(root, "profiles/ember-run/config/kiln/ember/settings.json", "{\"heat\": 3}\n");
    put(root, "library/kiln/ember/1.0.0+abcd1234/kiln.mod.json", "{\"id\": \"ember\", \"version\": \"1.0.0\"}\n");
}

#[test]
fn a_release_data_folder_round_trips_through_a_test_build() {
    let t = tmp("roundtrip");
    let root = &t.0;
    dev28_folder(root);
    let dirs = kiln_manage::paths::DataDirs::at(root);
    // dev.29 reads the release's formats
    let p = kiln_manage::profile::get(&dirs, "ember-run").unwrap();
    assert_eq!(p.name, "Ember run");

    // the first start of a test build: the copy, before anything is written
    let info = at_start(root, "0.2.0-dev.29", "beta", Some(3)).unwrap().expect("a copy");
    assert_eq!(info.files, ["profiles/ember-run/meta.json", "settings.json"], "update.json is the self-update's own record");
    assert_eq!((info.from.as_str(), info.by.as_str()), ("", "0.2.0-dev.29+beta.3"), "a launcher before dev.29 records no build");
    assert_eq!(super::info(root), Some(info.clone()));
    assert_eq!(read_last(root).unwrap().beta, Some(3));

    // the test build writes: its settings (keepVersions, the beta channel), the profile (its own save), a profile and
    // a mod of its own, the kept versions' state
    let mut s: crate::commands::app::LauncherSettings = serde_json::from_str(DEV28_SETTINGS).unwrap();
    s.update_channel = Some("beta".into());
    s.keep_versions = 3;
    kiln_manage::util::write_json_atomic(&root.join("settings.json"), &s).unwrap();
    let mut p2 = p.clone();
    p2.name = "Ember run (beta)".into();
    kiln_manage::profile::save(&dirs, &p2).unwrap();
    kiln_manage::profile::create(&dirs, "Made in the beta", None).unwrap();
    put(root, "library/kiln-kept.json", "{\"format\":1,\"mods\":[],\"pending\":[]}\n");
    put(root, "library/kiln/beta-only/2.0.0+ffff0000/kiln.mod.json", "{\"id\": \"beta-only\", \"version\": \"2.0.0\"}\n");
    put(root, "update.json", "{\"format\":1,\"updated\":{\"from\":\"0.2.0-dev.29+beta.3\",\"to\":\"0.2.0-dev.28\",\"at\":\"2026-11-15T12:00:00Z\",\"source\":\"frostdev.io\"}}\n");

    // the release reads what the test build wrote: settings (keepVersions ignored, "beta" as no choice: dev), the
    // profile (format 1, the same struct: crates/kiln-manage/src/profile.rs is unchanged since 2301cc5f)
    let back: Dev28Settings = serde_json::from_str(&read(root, "settings.json")).unwrap();
    assert_eq!(dev28_channel(back.update_channel.as_deref(), "0.2.0-dev.28"), "dev");
    let saved: serde_json::Value = serde_json::from_str(&read(root, "profiles/ember-run/meta.json")).unwrap();
    assert_eq!(saved["format"], 1, "0.2.0-dev.28 refuses a profile whose format is not 1");
    let saved: kiln_manage::profile::Profile = serde_json::from_value(saved).unwrap();
    assert_eq!(saved.name, "Ember run (beta)");

    // another test build keeps the first copy
    assert_eq!(at_start(root, "0.2.0-dev.29", "beta", Some(4)).unwrap(), None);
    assert_eq!(super::info(root).unwrap().by, "0.2.0-dev.29+beta.3");

    // the switch back with the restore: the release's files byte for byte; what the beta added stays
    let restored = restore(root, Some("dev")).unwrap();
    assert_eq!(restored, info.files);
    assert_eq!(read(root, "settings.json"), DEV28_SETTINGS);
    assert!(read(root, "update.json").contains("\"to\":\"0.2.0-dev.28\""), "the switch's record for the release's Updated to stays");
    assert_eq!(read(root, "profiles/ember-run/meta.json"), DEV28_PROFILE);
    assert!(kiln_manage::profile::list(&dirs).unwrap().iter().any(|p| p.name == "Made in the beta"), "a profile made in the beta stays");
    assert!(root.join("library/kiln/beta-only/2.0.0+ffff0000/kiln.mod.json").is_file(), "a mod added in the beta stays in the Library");
    assert!(root.join("library/kiln/ember/1.0.0+abcd1234/kiln.mod.json").is_file());
    assert_eq!(read(root, "profiles/ember-run/config/kiln/ember/settings.json"), "{\"heat\": 3}\n", "mod settings are not part of the copy");
    assert!(root.join("library/kiln-kept.json").is_file(), "not in the copy (there was none before the beta): left as it is");
    // and the release reads the result
    let _: Dev28Settings = serde_json::from_str(&read(root, "settings.json")).unwrap();
    assert_eq!(kiln_manage::profile::get(&dirs, "ember-run").unwrap().name, "Ember run");
}

#[test]
fn a_switch_back_lets_the_next_test_build_take_a_new_copy() {
    let t = tmp("again");
    let root = &t.0;
    dev28_folder(root);
    at_start(root, "0.2.0-dev.29", "beta", Some(1)).unwrap().unwrap();
    left_beta(root, "0.2.0-dev.28", "dev").unwrap();
    put(root, "settings.json", "{\"savesKeep\": 4}\n");
    let info = at_start(root, "0.2.0-dev.29", "beta", Some(2)).unwrap().expect("a new copy");
    assert_eq!(info.from, "0.2.0-dev.28");
    assert_eq!(std::fs::read_to_string(root.join(DIR).join("settings.json")).unwrap(), "{\"savesKeep\": 4}\n");
    assert!(!root.join(format!("{DIR}.new")).exists() && !root.join(format!("{DIR}.old")).exists());
}

#[test]
fn release_builds_and_empty_folders_take_no_copy() {
    let t = tmp("none");
    let root = &t.0;
    // a fresh data folder: nothing to copy, the build is recorded
    assert_eq!(at_start(root, "0.2.0-dev.29", "beta", Some(1)).unwrap(), None);
    assert_eq!(read_last(root).unwrap(), LastBuild { format: 1, version: "0.2.0-dev.29".into(), channel: "beta".into(), beta: Some(1), without_restore: vec![], pending_release: false, copy_complete: Some(false) });
    assert_eq!(super::info(root), None);
    // a release build never takes one
    dev28_folder(root);
    assert_eq!(at_start(root, "0.2.0-dev.29", "dev", None).unwrap(), None);
    assert_eq!(super::info(root), None);
    assert_eq!(read_last(root).unwrap().channel, "dev");
    assert_eq!(restore(root, Some("dev")).unwrap_err().code, ErrorCode::NotFound);
}

#[test]
fn a_restore_writes_only_inside_the_data_folder() {
    let t = tmp("paths");
    let root = &t.0.join("data");
    dev28_folder(root);
    let info = take(root, "0.2.0-dev.28", "0.2.0-dev.29+beta.1").unwrap().unwrap();
    // a changed snapshot.json that names other paths
    let mut bad = info.clone();
    bad.files = vec!["../outside.json".into(), "profiles/../../x/meta.json".into(), "kiln.json".into(), "settings.json".into()];
    put(&root.join(DIR), "../outside.json", "x");
    kiln_manage::util::write_json_atomic(&root.join(DIR).join(INFO), &bad).unwrap();
    put(root, "settings.json", "{}\n");
    assert_eq!(restore(root, Some("dev")).unwrap(), ["settings.json"]);
    assert_eq!(read(root, "settings.json"), DEV28_SETTINGS);
    assert!(!t.0.join("outside.json").exists());
}

#[test]
fn a_restored_choice_of_the_beta_says_the_release() {
    // the release build saved the player's choice of the beta before the test build's first start took the copy
    let t = tmp("channel");
    let root = &t.0;
    dev28_folder(root);
    put(root, "settings.json", "{\"savesKeep\": 7, \"updateChannel\": \"beta\", \"modUpdatesDaily\": true}\n");
    at_start(root, "0.2.0-dev.29", "beta", Some(1)).unwrap().unwrap();
    put(root, "settings.json", "{\"savesKeep\": 3, \"updateChannel\": \"dev\"}\n");
    restore(root, Some("dev")).unwrap();
    let v: serde_json::Value = serde_json::from_str(&read(root, "settings.json")).unwrap();
    assert_eq!(v, serde_json::json!({"savesKeep": 7, "updateChannel": "dev", "modUpdatesDaily": true}));
    // stable, or no channel given: as asked, or as it was
    restore(root, Some("stable")).unwrap();
    assert_eq!(serde_json::from_str::<serde_json::Value>(&read(root, "settings.json")).unwrap()["updateChannel"], "stable");
    restore(root, None).unwrap();
    assert_eq!(serde_json::from_str::<serde_json::Value>(&read(root, "settings.json")).unwrap()["updateChannel"], "beta");
}

#[test]
fn incomplete_copy_does_not_partly_restore() {
    let t = tmp("incomplete");
    dev28_folder(&t.0);
    take(&t.0, "28", "29+beta.1").unwrap();
    put(&t.0, "profiles/ember-run/meta.json", "beta profile");
    std::fs::remove_file(t.0.join(DIR).join("settings.json")).unwrap();
    assert!(restore(&t.0, Some("dev")).is_err());
    assert_eq!(read(&t.0, "profiles/ember-run/meta.json"), "beta profile");
}

#[cfg(unix)]
#[test]
fn restore_refuses_a_profile_parent_symlink() {
    let t = tmp("symlink");
    let root = t.0.join("data");
    dev28_folder(&root);
    take(&root, "28", "29+beta.1").unwrap();
    let outside = t.0.join("outside");
    std::fs::rename(root.join("profiles/ember-run"), &outside).unwrap();
    put(&outside, "meta.json", "outside profile");
    std::os::unix::fs::symlink(&outside, root.join("profiles/ember-run")).unwrap();
    assert!(restore(&root, Some("dev")).is_err());
    assert_eq!(read(&outside, "meta.json"), "outside profile");
}

#[test]
fn failed_marker_never_replaces_a_whole_copy_with_beta_data() {
    let t = tmp("marker-retry");
    dev28_folder(&t.0);
    put(&t.0, "state", "not a directory");
    assert!(at_start(&t.0, "0.2.0-dev.29", "beta", Some(1)).is_err());
    put(&t.0, "settings.json", "beta data");
    std::fs::remove_file(t.0.join("state")).unwrap();
    at_start(&t.0, "0.2.0-dev.29", "beta", Some(1)).unwrap();
    assert_eq!(read(&t.0.join(DIR), "settings.json"), DEV28_SETTINGS);
}

#[test]
fn oversized_and_symlinked_entries_are_reported_not_silently_skipped() {
    let t = tmp("omissions");
    dev28_folder(&t.0);
    std::fs::OpenOptions::new().write(true).open(t.0.join("settings.json")).unwrap().set_len(MAX_FILE + 1).unwrap();
    let e = take(&t.0, "28", "29+beta.1").unwrap_err();
    assert!(e.message.contains("settings.json"));
    assert!(info(&t.0).is_none());
}

#[cfg(unix)]
#[test]
fn a_symlinked_profile_is_an_explicit_copy_failure() {
    let t = tmp("profile-link");
    dev28_folder(&t.0);
    std::os::unix::fs::symlink(t.0.join("profiles/ember-run"), t.0.join("profiles/linked")).unwrap();
    let e = take(&t.0, "28", "29+beta.1").unwrap_err();
    assert!(e.message.contains("linked"));
}

#[test]
fn opting_out_records_each_reason_and_hides_the_copy() {
    let t = tmp("opt-out");
    dev28_folder(&t.0);
    take(&t.0, "28", "29+beta.1").unwrap();
    continue_without_restore(&t.0, "29", "beta", Some(1), vec!["profiles/x: unreadable".into(), "settings.json: too large".into()]).unwrap();
    assert_eq!(without_restore(&t.0).len(), 2);
    assert!(info(&t.0).is_none());
    at_start(&t.0, "29", "beta", Some(2)).unwrap();
    assert_eq!(without_restore(&t.0).len(), 2);
}

#[test]
fn interrupted_restore_finishes_as_one_transaction_on_next_start() {
    for step in 0..13 {
        let t = tmp(&format!("restore-crash-{step}"));
        dev28_folder(&t.0);
        take(&t.0, "28", "29+beta.1").unwrap();
        put(&t.0, "settings.json", "{\"savesKeep\":3}");
        put(&t.0, "profiles/ember-run/meta.json", "beta profile");
        RESTORE_FAIL.with(|f| f.set(Some(step)));
        let result = restore(&t.0, Some("dev"));
        assert_eq!(result.is_err(), step < 12, "failure hook {step}");
        RESTORE_FAIL.with(|f| f.set(None));
        recover_restore(&t.0).unwrap();
        let settings = read(&t.0, "settings.json");
        let profile = read(&t.0, "profiles/ember-run/meta.json");
        assert!((settings == DEV28_SETTINGS && profile == DEV28_PROFILE)
            || (settings == "{\"savesKeep\":3}" && profile == "beta profile"), "mixed files at interruption {step}");
    }
}

#[test]
fn an_unacknowledged_release_restart_cannot_replace_the_original_copy() {
    let t = tmp("unacknowledged");
    dev28_folder(&t.0);
    at_start(&t.0, "0.2.0-dev.29", "beta", Some(1)).unwrap();
    put(&t.0, "settings.json", "beta data");
    plan_leave_beta(&t.0, "0.2.0-dev.28", "dev").unwrap();
    at_start(&t.0, "0.2.0-dev.29", "beta", Some(2)).unwrap();
    assert_eq!(read(&t.0.join(DIR), "settings.json"), DEV28_SETTINGS);
    // A release that knows the marker acknowledges that it really ran. Only
    // that acknowledgement permits the next beta to capture the release's edits.
    at_start(&t.0, "0.2.0-dev.29", "dev", None).unwrap();
    put(&t.0, "settings.json", "release edits");
    at_start(&t.0, "0.2.0-dev.30", "beta", Some(1)).unwrap();
    assert_eq!(read(&t.0.join(DIR), "settings.json"), "release edits");
}

#[cfg(unix)]
#[test]
fn a_profile_path_that_restore_cannot_use_is_reported_before_copying() {
    let t = tmp("unrestorable-path");
    dev28_folder(&t.0);
    put(&t.0, "profiles/a\\b/meta.json", DEV28_PROFILE);
    let err = take(&t.0, "28", "29+beta.1").unwrap_err();
    assert!(err.message.contains("a\\b"));
    assert!(info(&t.0).is_none());
}

#[test]
fn an_empty_new_release_baseline_never_offers_an_older_cycles_copy() {
    let t = tmp("empty-new-cycle");
    dev28_folder(&t.0);
    at_start(&t.0, "0.2.0-dev.29", "beta", Some(1)).unwrap();
    at_start(&t.0, "0.2.0-dev.29", "dev", None).unwrap();
    std::fs::remove_file(t.0.join("settings.json")).unwrap();
    std::fs::remove_dir_all(t.0.join("profiles")).unwrap();
    assert!(at_start(&t.0, "0.2.0-dev.30", "beta", Some(1)).unwrap().is_none());
    assert!(info(&t.0).is_none());
}
