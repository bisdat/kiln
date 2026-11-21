// SPDX-License-Identifier: MIT
use serde_json::{Value, json};
use sfi_policy::launch_plan::{LaunchPlan, Permission, ValidationContext};
fn base() -> Value {
    let h = "a".repeat(64);
    json!({"format":1,"minReader":1,"plan_id":"12345678-1234-1234-1234-123456789abc","launch_id":null,
        "startupDialect":"none","writer":{"component":"kiln-manage","version":"0.1.0"},
        "profile":{"id":"test","generation":1,"mode":"active","semantic_sha256":h},
        "game":{"side":"client","exe_sha256":h,"archiveBase_sha256":h,"archiveInput_sha256":h},
        "backend":{"requested":"kiln","selected":"kiln","fallbackOrder":[],"dataWriter":"kiln-eml","selectionReason":"test"},
        "contract":{"apiVersion":"1.7.9","sourceCommit":"b".repeat(40),"upstream180Final":false,"inventory_sha256":h,"parity_sha256":h,"serviceContracts_sha256":h},
        "fingerprints":{"packages_sha256":h,"settings_sha256":h,"chain_sha256":h,"loaderInventory_sha256":h,"compat_sha256":h,"releasePolicy_sha256":h,"placementReceipts_sha256":h},
        "packages":[],"loadOrder":[],"warnings":[]})
}
fn context() -> ValidationContext {
    ValidationContext {
        exe_sha256: "a".repeat(64),
        side: "client".into(),
        upstream_180_final: false,
    }
}
fn read(v: &Value) -> Result<LaunchPlan, String> {
    LaunchPlan::parse(&serde_json::to_vec(v).unwrap(), &context())
}
fn package(id: &str) -> Value {
    json!({"id":id,"version":"1.0.0","kind":"shroudforge","root":format!("mods/{id}"),
      "content_sha256":"c".repeat(64),"config_sha256":"d".repeat(64),"enabled":true,"targets":["client"],"capabilities":["runtime"],
      "dependencies":[],"requiredApis":[],"services":[],"requires180":false,"saveSensitive":true,"decision":"candidate","reasons":[],
      "data":{"required":false,"admission":"not-needed"},"lua":{"required":true,"admission":"admit"},"dlls":[]})
}
#[test]
fn valid_plan_only_grants_pending_permission_and_never_starts_runtime() {
    let v = base();
    let plan = read(&v).unwrap();
    assert_eq!(
        plan.plan_id(),
        [
            0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34, 0x56, 0x78,
            0x9a, 0xbc
        ]
    );
    assert_eq!(plan.permission(false), Permission::Held);
    assert_eq!(plan.permission(true), Permission::EligiblePendingAdmission);
    assert!(!plan.runtime_started());
    for backend in ["hosted", "installed"] {
        let mut v = base();
        v["backend"]["requested"] = json!(backend);
        v["backend"]["selected"] = json!(backend);
        v["backend"]["dataWriter"] = json!("shroudforge");
        assert_eq!(read(&v).unwrap().permission(true), Permission::OtherBackend);
    }
}
#[test]
fn versions_uuid_exe_side_unknown_keys_and_duplicates_fail_closed() {
    for key in ["format", "minReader"] {
        let mut v = base();
        v[key] = json!(2);
        assert!(read(&v).is_err());
    }
    let mut v = base();
    v["plan_id"] = json!("not-uuid");
    assert!(read(&v).is_err());
    let mut v = base();
    v["launch_id"] = json!("12345678-1234-1234-1234-123456789abc");
    assert!(read(&v).is_err());
    let mut v = base();
    v["game"]["exe_sha256"] = json!("e".repeat(64));
    assert!(read(&v).is_err());
    let mut v = base();
    v["game"]["side"] = json!("server");
    assert!(read(&v).is_err());
    let mut v = base();
    v["unknown"] = json!(true);
    assert!(read(&v).is_err());
    let s = serde_json::to_string(&base()).unwrap();
    let duplicate = s.replacen("\"format\":1", "\"format\":1,\"format\":1", 1);
    assert!(LaunchPlan::parse(duplicate.as_bytes(), &context()).is_err());
    for text in [b"".as_slice(), b"{", b"null", b"[]"] {
        assert!(LaunchPlan::parse(text, &context()).is_err());
    }
}
#[test]
fn parts_candidates_closure_and_topological_order_are_consistent() {
    let mut v = base();
    v["packages"] = json!([package("dep"), package("mod")]);
    v["loadOrder"] = json!(["dep", "mod"]);
    v["packages"][1]["dependencies"] =
        json!([{"id":"dep","range":"^1.0","optional":false,"included":true}]);
    assert!(read(&v).is_ok());
    let good = v.clone();
    v["loadOrder"] = json!(["mod", "dep"]);
    assert!(read(&v).is_err());
    let mut v = good.clone();
    v["packages"][0]["lua"]["admission"] = json!("refuse");
    assert!(read(&v).is_err());
    let mut v = good.clone();
    v["packages"][1]["dependencies"][0]["included"] = json!(false);
    assert!(read(&v).is_err());
    let mut v = good.clone();
    v["packages"][1]["dependencies"][0]["range"] = json!("^2.0");
    assert!(
        read(&v).is_ok(),
        "reader validates recorded closure, not planner range semantics"
    );
    let mut v = good;
    v["packages"][0]["decision"] = json!("skip");
    assert!(read(&v).is_err());
}
#[test]
fn path_and_case_collisions_and_noncanonical_integer_encodings_fail() {
    for path in [
        "../mods/x",
        "mods/../x",
        "/mods/x",
        "C:/mods/x",
        "mods/x.",
        "mods/CON",
        "mods/a//b",
        "mods/a:b",
        "mods\\x",
    ] {
        let mut v = base();
        let mut p = package("x");
        p["root"] = json!(path);
        v["packages"] = json!([p]);
        v["loadOrder"] = json!(["x"]);
        assert!(read(&v).is_err(), "{path}");
    }
    let mut v = base();
    let mut a = package("a");
    let mut b = package("b");
    a["root"] = json!("mods/Case");
    b["root"] = json!("mods/case");
    v["packages"] = json!([a, b]);
    v["loadOrder"] = json!(["a", "b"]);
    assert!(read(&v).is_err());
    let s = serde_json::to_string(&base()).unwrap();
    for n in ["1.0", "1e0", "-0", "01"] {
        let bad = s.replacen("\"generation\":1", &format!("\"generation\":{n}"), 1);
        assert!(LaunchPlan::parse(bad.as_bytes(), &context()).is_err());
    }
}
#[test]
fn dialect_is_frozen_separately_from_physical_writer_and_release_policy_is_independent() {
    let mut v = base();
    let mut p = package("patcher");
    p["capabilities"] = json!(["patch"]);
    p["data"] = json!({"required":true,"admission":"admit"});
    v["packages"] = json!([p]);
    v["loadOrder"] = json!(["patcher"]);
    assert!(read(&v).is_err());
    v["startupDialect"] = json!("shroudforge");
    assert!(read(&v).is_ok());
    v["packages"][0]["requires180"] = json!(true);
    v["contract"]["upstream180Final"] = json!(true);
    assert!(read(&v).is_err());
    let mut ctx = context();
    ctx.upstream_180_final = true;
    assert!(LaunchPlan::parse(&serde_json::to_vec(&v).unwrap(), &ctx).is_ok());
    v["backend"]["dataWriter"] = json!("shroudforge");
    assert!(LaunchPlan::parse(&serde_json::to_vec(&v).unwrap(), &ctx).is_err());
}
#[test]
fn budgets_nested_duplicates_and_dll_admission_fail_closed() {
    assert!(LaunchPlan::parse(&vec![b' '; 8 * 1024 * 1024 + 1], &context()).is_err());
    let mut v = base();
    v["packages"] = json!(vec![json!({}); 4097]);
    assert!(read(&v).is_err());
    let mut v = base();
    v["profile"]["generation"] = json!(9223372036854775808u64);
    assert!(read(&v).is_err());
    let source = serde_json::to_string(&base()).unwrap();
    let duplicate = source.replacen("\"generation\":1", "\"generation\":1,\"generation\":1", 1);
    assert!(LaunchPlan::parse(duplicate.as_bytes(), &context()).is_err());
    let mut v = base();
    let mut p = package("native");
    let dll = json!({"path":"bin/native.dll","sha256":"e".repeat(64),"mode":"library","source":"ini","required":true,"gate":"pass","gateProof_sha256":"f".repeat(64),"bridgeAbi":0});
    p["dlls"] = json!([dll.clone()]);
    v["packages"] = json!([p]);
    v["loadOrder"] = json!(["native"]);
    assert!(read(&v).is_ok());
    let valid = v.clone();
    v["packages"][0]["dlls"][0]["gate"] = json!("unknown");
    assert!(read(&v).is_err());
    let mut v = valid.clone();
    v["packages"][0]["dlls"][0]["bridgeAbi"] = json!(2);
    assert!(read(&v).is_err());
    let mut v = valid;
    v["packages"][0]["dlls"] = json!([dll.clone(), dll]);
    assert!(read(&v).is_err());
}

#[test]
fn enabled_optional_targets_cannot_hide_ordering_or_cycles() {
    let mut v = base();
    v["packages"] = json!([package("dep"), package("mod")]);
    v["loadOrder"] = json!(["dep", "mod"]);
    v["packages"][1]["dependencies"] =
        json!([{"id":"dep","range":"*","optional":true,"included":false}]);
    assert!(
        read(&v).is_err(),
        "excluded enabled optional target accepted"
    );
    v["loadOrder"] = json!(["mod", "dep"]);
    assert!(read(&v).is_err(), "reverse optional order accepted");
    v["packages"][0]["dependencies"] =
        json!([{"id":"mod","range":"*","optional":false,"included":true}]);
    assert!(read(&v).is_err(), "excluded optional edge hid a cycle");
    let mut absent = base();
    let mut p = package("mod");
    p["dependencies"] = json!([{"id":"dep","range":"*","optional":true,"included":false}]);
    absent["packages"] = json!([p]);
    absent["loadOrder"] = json!(["mod"]);
    assert!(read(&absent).is_ok());
    let mut disabled = package("dep");
    disabled["enabled"] = json!(false);
    disabled["decision"] = json!("disabled");
    disabled["lua"]["admission"] = json!("refuse");
    absent["packages"].as_array_mut().unwrap().push(disabled);
    assert!(read(&absent).is_ok());
    absent["packages"][0]["dependencies"][0]["id"] = json!("shroudforge-api");
    assert!(read(&absent).is_ok());
}

#[test]
fn patch_requires_startup_data_but_data_only_packages_are_valid() {
    let mut v = base();
    let mut p = package("patcher");
    p["capabilities"] = json!(["patch"]);
    p["data"] = json!({"required":true,"admission":"admit"});
    p["lua"] = json!({"required":false,"admission":"not-needed"});
    v["packages"] = json!([p]);
    v["loadOrder"] = json!(["patcher"]);
    v["startupDialect"] = json!("shroudforge");
    assert!(read(&v).is_ok(), "legitimate data-only patch rejected");
    v["packages"][0]["data"] = json!({"required":false,"admission":"not-needed"});
    assert!(read(&v).is_err(), "patch candidate omitted required data");
}

#[test]
fn overlapping_roots_and_cross_package_dll_aliases_are_rejected() {
    let mut v = base();
    let mut a = package("a");
    let mut b = package("b");
    a["root"] = json!("mods/a");
    b["root"] = json!("mods/a/sub");
    let dll = json!({"path":"sub/Plugin.dll","sha256":"e".repeat(64),"mode":"library","source":"ini","required":true,"gate":"pass","gateProof_sha256":"f".repeat(64),"bridgeAbi":0});
    a["dlls"] = json!([dll.clone()]);
    b["dlls"] = json!([dll]);
    b["dlls"][0]["path"] = json!("plugin.dll");
    v["packages"] = json!([a, b]);
    v["loadOrder"] = json!(["a", "b"]);
    assert!(read(&v).is_err(), "nested roots resolve to same DLL");
    for backend in ["hosted", "installed"] {
        let mut x = v.clone();
        x["backend"]["requested"] = json!(backend);
        x["backend"]["selected"] = json!(backend);
        x["backend"]["dataWriter"] = json!("shroudforge");
        assert!(read(&x).is_err());
    }
    v["packages"][1]["root"] = json!("mods/b");
    assert!(read(&v).is_ok(), "separate package roots were conflated");
}

#[test]
fn hosted_and_installed_reject_a_lone_nested_root() {
    for backend in ["hosted", "installed"] {
        let mut v = base();
        v["backend"]["requested"] = json!(backend);
        v["backend"]["selected"] = json!(backend);
        v["backend"]["dataWriter"] = json!("shroudforge");
        v["packages"] = json!([package("a")]);
        v["loadOrder"] = json!(["a"]);
        assert!(read(&v).is_ok(), "direct-child {backend} root rejected");
        v["packages"][0]["root"] = json!("mods/a/sub");
        assert!(read(&v).is_err(), "lone nested {backend} root accepted");
    }
}

#[test]
fn fake_file_reader_rejects_short_invalid_and_mismatched_plans() {
    let directory = std::env::temp_dir().join(format!("sfi-plan-test-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let path = directory.join("plan.json");
    std::fs::write(&path, serde_json::to_vec(&base()).unwrap()).unwrap();
    assert!(LaunchPlan::read(&path, &context()).is_ok());
    std::fs::write(&path, b"{").unwrap();
    assert!(LaunchPlan::read(&path, &context()).is_err());
    std::fs::remove_file(&path).unwrap();
    std::fs::remove_dir(&directory).unwrap();
}

#[test]
fn the_typed_view_reads_back_what_parse_checked() {
    use sfi_policy::launch_plan::{Decision, Dialect, Kind, PartAdmission, Severity};
    let mut v = base();
    let mut dep = package("dep");
    dep["content_sha256"] = json!("0123456789abcdef".repeat(4));
    dep["capabilities"] = json!(["runtime", "runtime-register-dll"]);
    let mut m = package("mod");
    m["dependencies"] = json!([{"id":"dep","range":"^1.0","optional":false,"included":true},
        {"id":"absent","range":"*","optional":true,"included":false}]);
    m["dlls"] = json!([{"path":"bin/x.dll","sha256":"e".repeat(64),"mode":"library","source":"ini","required":true,
        "gate":"pass","gateProof_sha256":"f".repeat(64),"bridgeAbi":0}]);
    let mut off = package("off");
    off["decision"] = json!("disabled");
    off["enabled"] = json!(false);
    off["lua"]["admission"] = json!("refuse");
    off["saveSensitive"] = json!(false);
    v["packages"] = json!([dep, m, off]);
    v["loadOrder"] = json!(["dep", "mod"]);
    v["warnings"] = json!([{"code":"save-sensitive-off","message":"off now","causedBy":["off"],"severity":"save-warning"}]);
    let plan = read(&v).unwrap();
    assert_eq!(plan.load_order(), ["dep", "mod"]);
    assert_eq!(plan.startup_dialect(), Dialect::None);
    assert_eq!(plan.game_side(), "client");
    let ids: Vec<&str> = plan.packages().map(|p| p.id).collect();
    assert_eq!(ids, ["dep", "mod", "off"]);
    let dep = plan.package("dep").unwrap();
    assert_eq!(dep.content_sha256[..4], [0x01, 0x23, 0x45, 0x67]);
    assert_eq!(
        (dep.kind, dep.decision, dep.folder()),
        (Kind::Shroudforge, Decision::Candidate, "dep")
    );
    assert!(
        dep.has_native(),
        "runtime-register-dll counts as native code"
    );
    assert_eq!(
        (dep.lua.required, dep.lua.admission),
        (true, PartAdmission::Admit)
    );
    assert_eq!(
        (dep.data.required, dep.data.admission),
        (false, PartAdmission::NotNeeded)
    );
    let m = plan.package("mod").unwrap();
    assert!(m.has_native() && m.dlls.len() == 1);
    assert_eq!(m.dependencies.len(), 2);
    assert!(
        m.dependencies[0].included && !m.dependencies[1].included && m.dependencies[1].optional
    );
    let off = plan.package("off").unwrap();
    assert_eq!(
        (off.decision, off.enabled, off.save_sensitive),
        (Decision::Disabled, false, false)
    );
    assert!(plan.package("absent").is_none());
    let warnings = plan.warnings();
    assert_eq!(warnings.len(), 1);
    assert_eq!(
        (
            warnings[0].code,
            warnings[0].severity,
            warnings[0].caused_by.as_slice()
        ),
        (
            "save-sensitive-off",
            Severity::SaveWarning,
            ["off"].as_slice()
        )
    );
}

/// `game.archiveBaseOrigin` (kiln-eml's inspector's token for the archive base) is optional for a reader and
/// closed when present: one of three tokens, never "vanilla", and no other key joins `game`.
#[test]
fn the_archive_base_origin_is_optional_and_closed() {
    let plan = read(&base()).unwrap();
    assert_eq!(plan.archive_base_origin(), None, "a plan that does not say");
    for token in sfi_policy::launch_plan::ARCHIVE_BASE_ORIGINS {
        let mut v = base();
        v["game"]["archiveBaseOrigin"] = json!(token);
        let plan = read(&v).unwrap_or_else(|e| panic!("{token}: {e}"));
        assert_eq!(plan.archive_base_origin(), Some(token));
    }
    for bad in [
        json!("vanilla"),
        json!("first-managed"),
        json!("Unrecorded"),
        json!(""),
        json!(null),
        json!(1),
        json!({"token": "unrecorded"}),
    ] {
        let mut v = base();
        v["game"]["archiveBaseOrigin"] = bad.clone();
        assert!(read(&v).is_err(), "{bad} accepted");
    }
    let mut v = base();
    v["game"]["archiveBaseLabel"] = json!("unrecorded");
    assert!(read(&v).is_err(), "another key in game accepted");
    let mut v = base();
    v["game"]
        .as_object_mut()
        .unwrap()
        .remove("archiveBase_sha256");
    v["game"]["archiveBaseOrigin"] = json!("unrecorded");
    assert!(
        read(&v).is_err(),
        "the origin stands in for a missing digest"
    );
}

/// The shared launch-plan vectors (schema/fixtures/sfi/launch-plan): every plan in accept/ parses, every plan in
/// refuse/ is refused. kiln-eml's own reader can run the same vectors once it moves onto this reader.
#[test]
fn the_shared_accept_and_refuse_vectors() {
    // SFI_LAUNCH_PLAN_VECTORS: where a test binary runs away from the source tree (the native-Windows runner)
    let root = std::env::var_os("SFI_LAUNCH_PLAN_VECTORS").map_or_else(
        || {
            std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../../../schema/fixtures/sfi/launch-plan")
        },
        std::path::PathBuf::from,
    );
    // each refused vector is refused for its own reason, not for a slip elsewhere in the file
    let because = |name: &str| match name {
        "case-colliding-roots.json" => "case-colliding package roots",
        "device-name-root.json" => "unsafe path component/device name",
        "duplicate-id.json" | "virtual-id.json" => "duplicate or virtual package id",
        "root-two-folders.json" => "package root must be game-relative mods/<single folder>",
        "skip-without-reason.json" => "skipped package has no actionable reason",
        "unknown-decision.json" | "base-origin-vanilla.json" => "unknown enum value",
        other => panic!("{other}: a refused vector without its expected reason here"),
    };
    let mut seen = 0;
    for (folder, accept) in [("accept", true), ("refuse", false)] {
        let mut files: Vec<_> = std::fs::read_dir(root.join(folder))
            .unwrap()
            .map(|e| e.unwrap().path())
            .collect();
        files.sort();
        assert!(!files.is_empty(), "{folder}");
        for file in files {
            let bytes = std::fs::read(&file).unwrap();
            let result = LaunchPlan::parse(&bytes, &context());
            assert_eq!(
                result.is_ok(),
                accept,
                "{}: {:?}",
                file.display(),
                result.as_ref().err()
            );
            if let Err(why) = &result {
                let name = file.file_name().unwrap().to_str().unwrap();
                assert!(why.contains(because(name)), "{name}: {why}");
            }
            seen += 1;
        }
    }
    assert!(seen >= 8);
}

/// Package roots and DLL paths are compared as Windows compares names, character by character: `str::to_lowercase`
/// writes a word-final `Σ` as `ς` and any other as `σ`, so two spellings of one folder had two keys.
#[test]
fn roots_that_are_one_name_to_windows_collide() {
    let two = |a: &str, b: &str| {
        let mut v = base();
        let (mut x, mut y) = (package("a"), package("b"));
        x["root"] = json!(format!("mods/{a}"));
        y["root"] = json!(format!("mods/{b}"));
        v["packages"] = json!([x, y]);
        v["loadOrder"] = json!(["a", "b"]);
        read(&v)
    };
    for (a, b, why) in [
        ("ΑΣ", "Ασ", "a word-final capital sigma"),
        ("ας", "ασ", "the final and the medial small sigma"),
        ("\u{1FB3}", "\u{1FBC}", "a subscript iota's two cases"),
        ("Ä", "ä", "non-ASCII case"),
    ] {
        assert_eq!(
            two(a, b).unwrap_err(),
            "case-colliding package roots",
            "{why}"
        );
    }
    assert!(two("ασ", "αβ").is_ok(), "two names");
    let mut v = base();
    let mut p = package("a");
    let dll = json!({"path":"x","sha256":"e".repeat(64),"mode":"library","source":"ini","required":true,"gate":"pass","gateProof_sha256":"f".repeat(64),"bridgeAbi":0});
    p["dlls"] = json!([dll.clone(), dll]);
    p["dlls"][0]["path"] = json!("plugins/ΑΣ.dll");
    p["dlls"][1]["path"] = json!("plugins/Ασ.dll");
    v["packages"] = json!([p]);
    v["loadOrder"] = json!(["a"]);
    assert_eq!(
        read(&v).unwrap_err(),
        "case-colliding resolved DLL paths across packages"
    );
}

/// Two package folders that are one folder under two names (a link; under Wine and on Windows an 8.3 short name) are
/// overlapping roots: `check_folders` knows them by identity, which no string comparison can.
#[test]
fn two_roots_that_are_one_folder_are_refused() {
    let game = std::env::temp_dir().join(format!("sfi-policy-folders-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&game);
    // a long name (it has an 8.3 name) with a Greek letter with a subscript iota
    let long = "Long package \u{1FB3} folder";
    std::fs::create_dir_all(game.join("mods").join(long)).unwrap();
    std::fs::create_dir_all(game.join("mods").join("other")).unwrap();
    let two = |a: &str, b: &str| {
        let mut v = base();
        let (mut x, mut y) = (package("a"), package("b"));
        x["root"] = json!(format!("mods/{a}"));
        y["root"] = json!(format!("mods/{b}"));
        v["packages"] = json!([x, y]);
        v["loadOrder"] = json!(["a", "b"]);
        read(&v).unwrap_or_else(|e| panic!("{a} and {b}: {e}"))
    };
    assert_eq!(two(long, "other").check_folders(&game), Ok(()));
    assert_eq!(
        two(long, "missing").check_folders(&game),
        Ok(()),
        "a missing folder is checked by name only"
    );
    let mut names: Vec<(String, &str)> = Vec::new();
    #[cfg(unix)]
    {
        std::os::unix::fs::symlink(game.join("mods").join(long), game.join("mods").join("link"))
            .unwrap();
        names.push(("link".into(), "a link"));
    }
    #[cfg(windows)]
    match windows_names::short(&game.join("mods").join(long)) {
        Some(short) => names.push((short, "the 8.3 name")),
        None => eprintln!("no 8.3 name for {long} on this volume"),
    }
    for (name, why) in &names {
        assert!(
            std::fs::metadata(game.join("mods").join(name)).is_ok(),
            "{name} is there ({why})"
        );
        let refused = two(long, name).check_folders(&game);
        assert_eq!(
            refused,
            Err("overlapping package roots: a and b are one folder".into()),
            "{why}"
        );
    }
    std::fs::remove_dir_all(&game).unwrap();
}

#[cfg(windows)]
mod windows_names {
    use std::{
        ffi::OsString,
        os::windows::ffi::{OsStrExt as _, OsStringExt as _},
        path::{Path, PathBuf},
    };
    #[link(name = "kernel32", kind = "raw-dylib")]
    unsafe extern "system" {
        fn GetShortPathNameW(long: *const u16, short: *mut u16, len: u32) -> u32;
    }
    /// The 8.3 name of `path`'s last component, if the volume made one that differs.
    pub fn short(path: &Path) -> Option<String> {
        let long: Vec<u16> = path.as_os_str().encode_wide().chain([0]).collect();
        let mut buf = vec![0u16; 4096];
        let len = unsafe { GetShortPathNameW(long.as_ptr(), buf.as_mut_ptr(), buf.len() as u32) }
            as usize;
        let short = (len > 0 && len < buf.len())
            .then(|| PathBuf::from(OsString::from_wide(&buf[..len])))?;
        let name = short.file_name()?.to_str()?.to_owned();
        (Some(name.as_str()) != path.file_name().and_then(|n| n.to_str())).then_some(name)
    }
}
