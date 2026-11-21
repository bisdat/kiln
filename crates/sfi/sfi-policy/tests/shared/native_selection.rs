// SPDX-License-Identifier: MIT
// Shared native-selection bodies: run by sfi-policy's own tests (tests/native_selection.rs) and by any
// other test package that includes them.
//! ShroudForge's DLL Manager selection on Kiln-owned facts (dll_manager.rs, native_dlls.rs
//! queue, native_plugins.rs register_dll; both pins identical), plus Kiln's whole-package
//! withdrawal and the sealed register_dll table. Pure: no filesystem, nothing loads.
use serde_json::json;
use sfi_policy::native_selection::{
    DllFile, Exclusions, ExecutionPlan, Inputs, Manifest, ManifestTarget, Mode, NativePermission,
    NativePolicy, Package, PluginIni, Profile, RegisterOutcome, Registration, RegistrationTable,
    Selection, Source, Target, select,
};
use std::collections::BTreeMap;

fn package(dir: &str, id: Option<&str>, files: &[&str]) -> Package {
    Package {
        directory: dir.into(),
        manifest: id.map_or(Manifest::Missing, |id| Manifest::Valid {
            id: id.into(),
            enabled: true,
            target: ManifestTarget::Both,
        }),
        ini: PluginIni::Missing,
        profile: None,
        files: files.iter().map(|f| (*f).to_owned()).collect(),
    }
}
fn ini(dll: &str) -> PluginIni {
    PluginIni::Enabled { dll: dll.into() }
}
fn file(path: &str) -> DllFile {
    DllFile {
        path: path.into(),
        mode: Mode::Library,
    }
}
fn selection(dir: &str, files: &[&str]) -> Selection {
    Selection {
        package_directory: dir.into(),
        enabled: true,
        approved: true,
        target: Target::Both,
        compatibility_profile: None,
        files: files.iter().map(|f| file(f)).collect(),
    }
}
fn plan(order: &[&str]) -> ExecutionPlan {
    ExecutionPlan {
        order: order.iter().map(|s| (*s).to_owned()).collect(),
        ..ExecutionPlan::default()
    }
}
struct Case {
    manager: bool,
    server: bool,
    exclusions: Exclusions,
    selections: Vec<Selection>,
    packages: Vec<Package>,
    plan: ExecutionPlan,
    registrations: Vec<Registration>,
    permission: NativePermission,
}
impl Case {
    fn new(packages: Vec<Package>, order: &[&str]) -> Self {
        Self {
            manager: true,
            server: false,
            exclusions: Exclusions::default(),
            selections: Vec::new(),
            packages,
            plan: plan(order),
            registrations: Vec::new(),
            permission: NativePermission::ALWAYS,
        }
    }
    fn inputs(&self) -> Inputs<'_> {
        Inputs {
            manager_enabled: self.manager,
            server: self.server,
            exclusions: &self.exclusions,
            selections: &self.selections,
            packages: &self.packages,
            plan: &self.plan,
            registrations: &self.registrations,
            permission: self.permission,
        }
    }
    fn run(&self) -> sfi_policy::native_selection::NativeSelection {
        select(&self.inputs())
    }
    fn loads(&self) -> Vec<(String, Source)> {
        self.run()
            .entries
            .iter()
            .map(|e| (format!("{}/{}", e.package_directory, e.path), e.source))
            .collect()
    }
}
fn s(text: &str) -> String {
    text.into()
}

pub fn source_precedence_is_selection_then_profile_then_ini() {
    let mut p = package("pkg", Some("a"), &["sel.dll", "prof.dll", "ini.dll"]);
    p.ini = ini("ini.dll");
    p.profile = Some(Profile {
        id: "p1".into(),
        target: Target::Both,
        files: vec![file("prof.dll")],
    });
    let mut case = Case::new(vec![p], &["a"]);
    case.selections = vec![selection("PKG", &["sel.dll"])];
    assert_eq!(case.loads(), [(s("pkg/sel.dll"), Source::Selection)]);
    case.selections[0].files.clear();
    assert_eq!(case.loads(), [(s("pkg/prof.dll"), Source::Profile)]);
    case.packages[0].profile.as_mut().unwrap().files.clear();
    assert_eq!(case.loads(), [(s("pkg/ini.dll"), Source::Ini)]);
    case.packages[0].ini = PluginIni::Disabled;
    assert!(case.loads().is_empty());
    let selected = case.run();
    assert!(selected.issues.is_empty() && selected.withdrawn.is_empty());
    // Status ids keep ShroudForge's shape; the plan names are the launch-plan enum.
    case.packages[0].ini = ini("Sub/INI.dll");
    case.packages[0].files.push("sub/ini.DLL".into());
    let entry = &case.run().entries[0];
    assert_eq!(entry.status_id, "dll:pkg:sub/ini.dll");
    assert_eq!(
        (entry.source.plan_name(), entry.source.source_text()),
        ("ini", "ini")
    );
    assert_eq!(Source::Selection.source_text(), "manual");
    assert_eq!(Source::Registration.plan_name(), "lua-registration");
}

pub fn plugin_ini_parses_as_shroudforge_reads_it() {
    for (text, expected) in [
        ("[Plugin]\nenabled=1\ndll=a.dll", ini("a.dll")),
        (
            "\u{feff}[ plugin ]\r\n; c\n# c\nDLL = \"sub\\a.dll\"",
            ini("sub/a.dll"),
        ),
        ("[Plugin]\nenabled=off", PluginIni::Disabled),
        ("[Plugin]\nenabled=false\ndll=a.dll", PluginIni::Disabled),
        ("[Other]\ndll=a.dll", PluginIni::Invalid),
        ("[Plugin]\ndll=a.dll\ndll=b.dll", PluginIni::Invalid),
        ("[Plugin]\nenabled=maybe\ndll=a.dll", PluginIni::Invalid),
        ("[Plugin]\nenabled=1", PluginIni::Invalid),
        ("[Plugin]\njunk\ndll=a.dll", PluginIni::Invalid),
        (
            "[Plugin]\nenabled=1\nenabled=1\ndll=a.dll",
            PluginIni::Invalid,
        ),
        ("[Other]\njunk\n[Plugin]\ndll='a.dll'", ini("a.dll")),
    ] {
        assert_eq!(PluginIni::parse(text), expected, "{text:?}");
    }
}

pub fn manager_approval_profile_and_manifest_switches_drop_dlls_without_withdrawal() {
    let mut case = Case::new(vec![package("pkg", Some("a"), &["x.dll"])], &["a"]);
    case.packages[0].ini = ini("x.dll");
    assert_eq!(case.loads().len(), 1);
    for change in 0..6 {
        let mut c = Case::new(case.packages.clone(), &["a"]);
        c.selections = vec![selection("pkg", &[])];
        match change {
            0 => c.manager = false,
            1 => c.selections[0].enabled = false,
            2 => c.selections[0].approved = false,
            // A named compatibility profile that was not found deactivates the package's DLLs.
            3 => c.selections[0].compatibility_profile = Some("gone".into()),
            4 => {
                c.packages[0].manifest = Manifest::Valid {
                    id: "a".into(),
                    enabled: false,
                    target: ManifestTarget::Both,
                }
            }
            _ => {
                c.packages[0].manifest = Manifest::Invalid {
                    id: Some("a".into()),
                    error: "bad".into(),
                }
            }
        }
        let selected = c.run();
        assert!(selected.entries.is_empty(), "change {change}");
        assert!(selected.issues.is_empty(), "change {change}");
        assert!(selected.withdrawn.is_empty(), "change {change}");
    }
    // An inactive selection is not a broken one: a missing selected file or an invalid ini
    // behind a disabled or unconfirmed selection raises no issue and withdraws nothing.
    for (enabled, approved) in [(false, true), (true, false)] {
        for broken in [true, false] {
            let mut c = Case::new(case.packages.clone(), &["a"]);
            c.selections = vec![selection("pkg", &[])];
            (c.selections[0].enabled, c.selections[0].approved) = (enabled, approved);
            if broken {
                c.selections[0].files = vec![file("missing.dll")];
            } else {
                c.packages[0].ini = PluginIni::Invalid;
            }
            let selected = c.run();
            assert!(
                selected.issues.is_empty() && selected.withdrawn.is_empty(),
                "enabled={enabled} approved={approved} broken={broken}: {selected:?}"
            );
            // The same facts behind an active selection do withdraw the package.
            (c.selections[0].enabled, c.selections[0].approved) = (true, true);
            assert!(c.run().is_withdrawn("a"));
        }
    }
    // "none" is not a profile name: the selection stays active.
    let mut c = Case::new(case.packages.clone(), &["a"]);
    c.selections = vec![selection("pkg", &[])];
    c.selections[0].compatibility_profile = Some("none".into());
    assert_eq!(c.loads().len(), 1);
    // A package with no DLL file, no ini and no profile is not a native candidate at all,
    // even when its selection lists a file (ShroudForge skips the folder).
    let mut c = Case::new(
        vec![package("pkg", Some("a"), &["readme.txt", ".dll"])],
        &["a"],
    );
    c.selections = vec![selection("pkg", &["x.dll"])];
    let selected = c.run();
    assert!(selected.entries.is_empty() && selected.issues.is_empty());
}

pub fn targets_follow_the_selection_and_the_manifest() {
    let mut p = package("pkg", Some("a"), &["x.dll"]);
    p.ini = ini("x.dll");
    for (manifest, load, server, loads) in [
        (ManifestTarget::Both, Target::Both, false, true),
        (ManifestTarget::Both, Target::Server, false, false),
        (ManifestTarget::Both, Target::Server, true, true),
        (ManifestTarget::Client, Target::Both, false, true),
        (ManifestTarget::Client, Target::Server, false, false),
        (ManifestTarget::Server, Target::Client, true, false),
    ] {
        let mut c = Case::new(vec![p.clone()], &["a"]);
        c.server = server;
        c.packages[0].manifest = Manifest::Valid {
            id: "a".into(),
            enabled: true,
            target: manifest,
        };
        c.selections = vec![selection("pkg", &[])];
        c.selections[0].target = load;
        let selected = c.run();
        assert_eq!(
            !selected.entries.is_empty(),
            loads,
            "{manifest:?} {load:?} server={server}"
        );
        // A target conflict is a UI note in ShroudForge, not a plan issue: no withdrawal.
        assert!(selected.withdrawn.is_empty());
    }
}

pub fn an_unestablished_selected_dll_withdraws_the_package_and_its_dependents() {
    let mut a = package("a-dir", Some("a"), &["lib.dll"]);
    a.ini = ini("lib.dll");
    let mut b = package("b-dir", Some("b"), &["b.dll"]);
    b.ini = ini("b.dll");
    let mut c = package("c-dir", Some("c"), &["c.dll"]);
    c.ini = ini("c.dll");
    let d = package("d-dir", Some("d"), &[]);
    let mut case = Case::new(vec![a, b, c, d], &["a", "b", "c", "d"]);
    case.plan.dependencies = BTreeMap::from([
        (s("b"), vec![s("a")]),
        (s("c"), vec![s("x-absent")]),
        (s("d"), vec![s("b")]),
    ]);
    assert_eq!(case.run().entries.len(), 3);
    for (break_it, text) in [
        (
            0,
            "DLL lib.dll: The system cannot find the file specified. (os error 2)",
        ),
        (1, "DLL is excluded: lib.dll"),
        (2, "DLL path must stay inside one installed mod folder"),
        (3, "native-plugin.ini has an invalid [Plugin] declaration"),
    ] {
        let mut broken = Case::new(case.packages.clone(), &["a", "b", "c", "d"]);
        broken.plan.dependencies = case.plan.dependencies.clone();
        match break_it {
            0 => broken.packages[0].files.clear(),
            1 => broken.exclusions = Exclusions::from_config(Some(&json!(["LIB.dll"]))),
            2 => broken.packages[0].ini = ini("../lib.dll"),
            _ => broken.packages[0].ini = PluginIni::Invalid,
        }
        let selected = broken.run();
        assert!(
            selected.issues.iter().any(|i| i.detail == text),
            "{break_it}: {:?}",
            selected.issues
        );
        let withdrawn: Vec<_> = selected
            .withdrawn
            .iter()
            .map(|w| (w.mod_id.as_str(), w.caused_by.clone()))
            .collect();
        assert_eq!(
            withdrawn,
            [
                ("a", vec![]),
                ("b", vec![s("a")]),
                ("d", vec![s("b"), s("a")])
            ],
            "{break_it}"
        );
        assert!(selected.withdrawn[0].reason.contains(text));
        // The independent package keeps its DLL; nothing of a withdrawn one loads.
        assert_eq!(
            broken.loads(),
            [(s("c-dir/c.dll"), Source::Ini)],
            "{break_it}"
        );
    }
    // A missing folder inside the package is "path not found", as Windows reports it.
    let mut nested = Case::new(case.packages.clone(), &["a", "b", "c", "d"]);
    nested.packages[0].ini = ini("bin/lib.dll");
    assert!(nested.run().issues[0].detail.ends_with("(os error 3)"));
}

pub fn exclusions_are_all_or_nothing_like_the_source() {
    let mut case = Case::new(vec![package("pkg", Some("a"), &["x.dll"])], &["a"]);
    case.packages[0].ini = ini("x.dll");
    for config in [
        json!(["y.dll", "x.dll*"]),
        json!(["sub/x.dll"]),
        json!(["x.txt"]),
        json!("x.dll"),
        json!([7]),
        json!((0..65).map(|i| format!("n{i}.dll")).collect::<Vec<_>>()),
    ] {
        case.exclusions = Exclusions::from_config(Some(&config));
        assert_eq!(case.exclusions.names().count(), 0, "{config}");
        assert_eq!(case.loads().len(), 1, "{config}");
    }
    case.exclusions = Exclusions::from_config(Some(&json!([" X.DLL ", "x.dll"])));
    assert_eq!(case.exclusions.names().collect::<Vec<_>>(), ["x.dll"]);
    assert!(case.loads().is_empty());
    assert!(case.exclusions.excludes("deep\\X.dll"));
    assert!(Exclusions::from_config(None).names().next().is_none());
}

pub fn execution_plan_ranks_order_dlls_and_unplanned_mods_report_their_reason() {
    let mut first = package("zz-first", Some("first"), &["1.dll", "2.dll"]);
    first.profile = Some(Profile {
        id: "p".into(),
        target: Target::Both,
        files: vec![file("2.dll"), file("1.dll")],
    });
    let mut second = package("aa-second", Some("second"), &["s.dll"]);
    second.ini = ini("s.dll");
    let mut native_only = package("mm-native", None, &["n.dll"]);
    native_only.ini = ini("n.dll");
    let mut off = package("off", Some("off"), &["o.dll"]);
    off.ini = ini("o.dll");
    let mut ghost = package("ghost", Some("ghost"), &["g.dll"]);
    ghost.ini = ini("g.dll");
    let mut case = Case::new(
        vec![second, native_only, first, off, ghost],
        &["first", "second"],
    );
    case.plan
        .issues
        .insert(s("off"), s("Missing dependency: base"));
    // Rank order, declared order inside a package, manifest-less packages last.
    assert_eq!(
        case.loads(),
        [
            (s("zz-first/2.dll"), Source::Profile),
            (s("zz-first/1.dll"), Source::Profile),
            (s("aa-second/s.dll"), Source::Ini),
            (s("mm-native/n.dll"), Source::Ini),
        ]
    );
    let selected = case.run();
    let issues: Vec<_> = selected
        .issues
        .iter()
        .map(|i| (i.status_id.as_str(), i.detail.as_str()))
        .collect();
    assert_eq!(
        issues,
        [
            (
                "dll:ghost:g.dll",
                "Mod is disabled or unavailable in the execution plan"
            ),
            ("dll:off:o.dll", "Missing dependency: base"),
        ]
    );
    // Not planned is not withdrawn: those packages never run anyway.
    assert!(selected.withdrawn.is_empty());
    // An environment that cannot be planned keeps only manifest-less DLLs.
    case.plan.error = Some(s("cycle"));
    assert_eq!(case.loads(), [(s("mm-native/n.dll"), Source::Ini)]);
    let selected = case.run();
    assert_eq!(selected.issues[0].status_id, "dll:plan");
    assert_eq!(
        selected.issues[0].detail,
        "Could not resolve mod dependencies: cycle"
    );
}

pub fn one_record_per_file_and_a_conflicting_mode_withdraws() {
    let mut case = Case::new(vec![package("pkg", Some("a"), &["Sub/X.dll"])], &["a"]);
    case.selections = vec![selection("pkg", &["sub//x.dll", "SUB/X.DLL"])];
    let selected = case.run();
    assert_eq!(selected.entries.len(), 1);
    assert_eq!(selected.entries[0].aliases, ["dll:pkg:sub/x.dll"]);
    assert_eq!(selected.entries[0].key(), "pkg/sub/x.dll");
    // A later Library declaration reuses an earlier dinput8 record ...
    case.selections[0].files[0].mode = Mode::Dinput8;
    let selected = case.run();
    assert_eq!(selected.entries.len(), 1);
    assert_eq!(selected.entries[0].mode, Mode::Dinput8);
    // ... a later different non-library mode refuses, and the package cannot run whole.
    case.selections[0].files[0].mode = Mode::Library;
    case.selections[0].files[1].mode = Mode::Dinput8;
    let selected = case.run();
    assert_eq!(
        selected.issues[0].detail,
        "This DLL was already scheduled with another initialization mode. Restart the game after changing its selection"
    );
    assert_eq!(selected.withdrawn[0].mod_id, "a");
    assert!(selected.entries.is_empty());
}

pub fn declared_register_dll_targets_are_established_or_withdraw_the_package() {
    let base = || {
        let mut c = Case::new(
            vec![
                package("a", Some("a"), &["own.dll", "extra.dll"]),
                package("b", Some("b"), &["b.dll"]),
            ],
            &["a", "b"],
        );
        c.packages[0].ini = ini("own.dll");
        c.registrations = vec![
            Registration {
                package_directory: "b".into(),
                path: ".\\b.dll".into(),
            },
            Registration {
                package_directory: "a".into(),
                path: "./extra.dll".into(),
            },
            Registration {
                package_directory: "a".into(),
                path: "OWN.dll".into(),
            },
        ];
        c
    };
    let case = base();
    // Selected DLLs first, then registrations in package rank order; the same file reuses its
    // record (an alias), never a second load.
    assert_eq!(
        case.loads(),
        [
            (s("a/own.dll"), Source::Ini),
            (s("a/extra.dll"), Source::Registration),
            (s("b/b.dll"), Source::Registration),
        ]
    );
    let selected = case.run();
    assert_eq!(selected.entries[0].aliases, ["dll:a:own.dll"]);
    assert_eq!(selected.entries[1].status_id, "dll:a:extra.dll");
    for (change, text) in [
        (0, "EML runtime DLL support is unavailable in this phase"),
        (
            1,
            "The DLL Manager selection targets a different game process",
        ),
        (
            2,
            "Native DLL loading for this mod folder is disabled or not confirmed in DLL Manager. Update the selection and restart the game",
        ),
        (
            3,
            "DLL b.dll: The system cannot find the file specified. (os error 2)",
        ),
    ] {
        let mut c = base();
        match change {
            0 => c.manager = false,
            1 => {
                c.selections = vec![selection("b", &[])];
                c.selections[0].target = Target::Server;
            }
            2 => {
                c.selections = vec![selection("b", &[])];
                c.selections[0].approved = false;
            }
            _ => c.packages[1].files.clear(),
        }
        let selected = c.run();
        assert!(
            selected
                .issues
                .iter()
                .any(|i| i.status_id == "dll:b:b.dll" && i.detail == text),
            "{change}: {:?}",
            selected.issues
        );
        assert!(selected.is_withdrawn("b"), "{change}");
        assert!(
            !selected.entries.iter().any(|e| e.package_directory == "b"),
            "{change}"
        );
    }
    // An unplanned mod never runs its Lua, so its declarations change nothing.
    let mut c = base();
    c.plan = plan(&["a"]);
    c.packages[1].files.clear();
    assert!(c.run().withdrawn.is_empty());
}

pub fn the_sealed_table_reuses_established_targets_and_never_loads_a_new_one() {
    let mut c = Case::new(
        vec![
            package("a", Some("a"), &["own.dll", "late.dll", "sub/x.dll"]),
            package("b", Some("b"), &["b.dll"]),
        ],
        &["a", "b"],
    );
    c.packages[0].ini = ini("own.dll");
    c.selections = vec![selection("b", &[])];
    c.selections[0].enabled = false;
    c.exclusions = Exclusions::from_config(Some(&json!(["bad.dll"])));
    let selected = c.run();
    let table = RegistrationTable::seal(&c.inputs(), &selected);
    assert_eq!(
        table.register("A", true, true, ".\\Own.DLL"),
        RegisterOutcome::Reuse {
            key: "a/own.dll".into(),
            status_id: "dll:a:own.dll".into()
        }
    );
    // Same target as SF would accept, but it was not established before the seal.
    assert!(matches!(
        table.register("a", true, true, "late.dll"),
        RegisterOutcome::NotEstablished(text) if text.contains("late.dll")
    ));
    assert_eq!(
        table.register("a", true, false, "own.dll"),
        RegisterOutcome::Refused("EML runtime DLL support is unavailable in this phase".into())
    );
    assert_eq!(
        table.register("a", false, true, "own.dll"),
        RegisterOutcome::SkippedDisabled
    );
    assert_eq!(
        table.register("a", true, true, "bad.dll"),
        RegisterOutcome::Refused("DLL is excluded: bad.dll".into())
    );
    assert_eq!(
        table.register("a", true, true, "../own.dll"),
        RegisterOutcome::Refused("DLL path must stay inside one installed mod folder".into())
    );
    assert!(matches!(
        table.register("b", true, true, "b.dll"),
        RegisterOutcome::Refused(text) if text.starts_with("Native DLL loading for this mod folder is disabled")
    ));
    // A withdrawn package's entries are not reusable.
    let mut w = Case::new(c.packages.clone(), &["a"]);
    w.packages[0].ini = ini("missing.dll");
    w.registrations = vec![Registration {
        package_directory: "a".into(),
        path: "late.dll".into(),
    }];
    let selected = w.run();
    assert!(selected.is_withdrawn("a"));
    let table = RegistrationTable::seal(&w.inputs(), &selected);
    assert!(matches!(
        table.register("a", true, true, "late.dll"),
        RegisterOutcome::NotEstablished(_)
    ));
}

pub fn selection_is_pure_and_never_reads_the_filesystem() {
    // Facts in, decisions out: the module has no file, directory or environment access.
    let source = include_str!("../../src/native_selection.rs");
    for needle in [
        "std::fs",
        "fs::",
        "File::",
        "read_dir",
        "canonicalize",
        "std::env",
    ] {
        assert!(!source.contains(needle), "native_selection uses {needle}");
    }
    // Same inputs, same answer (the producer and the executor agree).
    let mut case = Case::new(vec![package("pkg", Some("a"), &["x.dll"])], &["a"]);
    case.packages[0].ini = ini("x.dll");
    assert_eq!(case.run(), case.run());
}

// ---- Kiln's native.policy in kiln mode ----

const NEVER: &str = r#"kiln.json native.policy is "never""#;
const UNVERIFIED: &str =
    r#"kiln.json native.policy is "verified-only" and this game build is not verified"#;

fn permission(policy: NativePolicy, verified: bool) -> NativePermission {
    NativePermission { policy, verified }
}

/// Every source a package DLL can come from, two Lua-only packages that depend on DLL packages (one only through the
/// other), a Lua-only package that depends on nothing native, and a manifest-less DLL folder.
fn every_source() -> Case {
    let mut ini_pkg = package("ini-dir", Some("ini"), &["i.dll"]);
    ini_pkg.ini = ini("i.dll");
    let mut profile_pkg = package("profile-dir", Some("prof"), &["p.dll"]);
    profile_pkg.profile = Some(Profile {
        id: "p1".into(),
        target: Target::Both,
        files: vec![file("p.dll")],
    });
    let sel_pkg = package("sel-dir", Some("sel"), &["s.dll"]);
    let reg_pkg = package("reg-dir", Some("reg"), &["r.dll"]);
    let lua = package("lua-dir", Some("lua"), &[]);
    let lua2 = package("lua2-dir", Some("lua2"), &[]);
    let free = package("free-dir", Some("free"), &[]);
    let mut bare = package("bare-dir", None, &["b.dll"]);
    bare.ini = ini("b.dll");
    let order = ["ini", "prof", "sel", "reg", "lua", "lua2", "free"];
    let mut case = Case::new(
        vec![
            ini_pkg,
            profile_pkg,
            sel_pkg,
            reg_pkg,
            lua,
            lua2,
            free,
            bare,
        ],
        &order,
    );
    case.selections = vec![selection("sel-dir", &["s.dll"])];
    case.registrations = vec![Registration {
        package_directory: "reg-dir".into(),
        path: "r.dll".into(),
    }];
    case.plan.dependencies = BTreeMap::from([
        (s("lua"), vec![s("ini")]),
        (s("lua2"), vec![s("lua")]),
        (s("free"), vec![]),
    ]);
    case
}
fn withdrawn(
    selected: &sfi_policy::native_selection::NativeSelection,
) -> Vec<(&str, &str, Vec<String>)> {
    selected
        .withdrawn
        .iter()
        .map(|w| (w.mod_id.as_str(), w.reason.as_str(), w.caused_by.clone()))
        .collect()
}

pub fn native_policy_text_is_kilns_own_and_refuses_as_native_mods_do() {
    use NativePolicy::*;
    assert_eq!(permission(Always, false).refusal(), None);
    assert_eq!(permission(Always, true).refusal(), None);
    assert_eq!(permission(VerifiedOnly, true).refusal(), None);
    assert_eq!(permission(VerifiedOnly, false).refusal(), Some(UNVERIFIED));
    assert_eq!(permission(Never, true).refusal(), Some(NEVER));
    assert_eq!(permission(Never, false).refusal(), Some(NEVER));
    assert_eq!(NativePermission::ALWAYS.refusal(), None);
    // kiln.launch's KILN_LAUNCH_NATIVE_POLICY_* values and kiln.json's words
    assert_eq!(NativePolicy::from_launch(0), Some(Always));
    assert_eq!(NativePolicy::from_launch(1), Some(VerifiedOnly));
    assert_eq!(NativePolicy::from_launch(2), Some(Never));
    assert_eq!(NativePolicy::from_launch(3), None);
    assert_eq!(NativePolicy::from_launch(u32::MAX), None);
    assert_eq!(
        [Always.name(), VerifiedOnly.name(), Never.name()],
        ["always", "verified-only", "never"]
    );
}

pub fn native_policy_always_selects_every_source_as_before() {
    let case = every_source();
    assert_eq!(
        case.loads(),
        [
            (s("ini-dir/i.dll"), Source::Ini),
            (s("profile-dir/p.dll"), Source::Profile),
            (s("sel-dir/s.dll"), Source::Selection),
            (s("bare-dir/b.dll"), Source::Ini),
            (s("reg-dir/r.dll"), Source::Registration),
        ]
    );
    let selected = case.run();
    assert!(selected.withdrawn.is_empty() && selected.issues.is_empty());
    // "always" with or without a verified build, and verified-only on a verified one: the same answer.
    for p in [
        permission(NativePolicy::Always, true),
        permission(NativePolicy::VerifiedOnly, true),
    ] {
        let mut other = every_source();
        other.permission = p;
        assert_eq!(other.run(), selected, "{p:?}");
    }
}

pub fn native_policy_never_withdraws_every_dll_package_and_its_dependents_with_no_load() {
    for (p, text) in [
        (permission(NativePolicy::Never, true), NEVER),
        (permission(NativePolicy::VerifiedOnly, false), UNVERIFIED),
    ] {
        let mut case = every_source();
        case.permission = p;
        let selected = case.run();
        assert!(selected.entries.is_empty(), "{p:?}: {:?}", selected.entries);
        assert_eq!(
            withdrawn(&selected),
            [
                ("ini", text, vec![]),
                ("prof", text, vec![]),
                ("sel", text, vec![]),
                ("reg", text, vec![]),
                ("lua", "depends on withdrawn package ini", vec![s("ini")]),
                (
                    "lua2",
                    "depends on withdrawn package lua",
                    vec![s("lua"), s("ini")]
                ),
            ],
            "{p:?}"
        );
        assert!(!selected.is_withdrawn("free"), "{p:?}");
        // Each refused DLL says why in its own status, the manifest-less folder's too (it has no package to withdraw).
        let mut refused: Vec<_> = selected
            .issues
            .iter()
            .filter(|i| i.detail == text)
            .map(|i| i.status_id.as_str())
            .collect();
        refused.sort_unstable();
        assert_eq!(
            refused,
            [
                "dll:bare-dir:b.dll",
                "dll:ini-dir:i.dll",
                "dll:profile-dir:p.dll",
                "dll:reg-dir:r.dll",
                "dll:sel-dir:s.dll"
            ],
            "{p:?}"
        );
        // The sealed register_dll table reuses nothing, and says why in Kiln's words.
        let table = RegistrationTable::seal(&case.inputs(), &selected);
        assert_eq!(
            table.register("reg-dir", true, true, "r.dll"),
            RegisterOutcome::NotEstablished(format!("Native DLL r.dll does not load: {text}")),
            "{p:?}"
        );
        assert_eq!(
            table.register("reg-dir", false, true, "r.dll"),
            RegisterOutcome::SkippedDisabled
        );
        assert_eq!(
            table.register("reg-dir", true, true, "../r.dll"),
            RegisterOutcome::Refused("DLL path must stay inside one installed mod folder".into())
        );
    }
}

pub fn native_policy_names_itself_for_a_package_whose_dll_also_has_an_issue() {
    // A package whose declared DLL is missing would be withdrawn anyway; under the policy its reason is the policy (the
    // DLL's own status keeps the issue), so the player sees one reason for every package that ships a DLL.
    let mut case = every_source();
    case.packages[0].files.clear();
    case.permission = permission(NativePolicy::Never, false);
    let selected = case.run();
    assert_eq!(withdrawn(&selected)[0], ("ini", NEVER, vec![]));
    assert!(
        selected
            .issues
            .iter()
            .any(|i| i.status_id == "dll:ini-dir:i.dll"
                && i.detail
                    == "DLL i.dll: The system cannot find the file specified. (os error 2)")
    );
    // With "always" the issue's own reason, as before.
    case.permission = NativePermission::ALWAYS;
    assert!(
        case.run().withdrawn[0]
            .reason
            .starts_with("native DLL cannot load: DLL i.dll")
    );
    // The manager switched off: declared registrations fail; under the policy the package still names the policy.
    let mut off = every_source();
    off.manager = false;
    off.permission = permission(NativePolicy::Never, false);
    let selected = off.run();
    assert!(selected.entries.is_empty());
    assert_eq!(withdrawn(&selected), [("reg", NEVER, vec![])]);
}

pub fn native_policy_reports_every_spelling_of_a_refused_file() {
    // One file declared twice (one process record and an alias): under the policy both status rows say why.
    let mut case = Case::new(vec![package("pkg", Some("a"), &["Sub/X.dll"])], &["a"]);
    case.selections = vec![selection("pkg", &["sub//x.dll", "SUB/X.DLL"])];
    assert_eq!(case.run().entries[0].aliases, ["dll:pkg:sub/x.dll"]);
    case.permission = permission(NativePolicy::Never, true);
    let selected = case.run();
    assert!(selected.entries.is_empty(), "{:?}", selected.entries);
    let mut refused: Vec<_> = selected
        .issues
        .iter()
        .map(|i| (i.status_id.as_str(), i.detail.as_str()))
        .collect();
    refused.sort_unstable();
    assert_eq!(
        refused,
        [("dll:pkg:sub//x.dll", NEVER), ("dll:pkg:sub/x.dll", NEVER)]
    );
    assert_eq!(withdrawn(&selected), [("a", NEVER, vec![])]);
}

/// The rest of `read_plugin_ini` (dll_manager.rs:571-623, identical at both pins) and
/// the `native-plugin.ini` declaration inside the selection: other sections are ignored
/// whole, keys and section names are ASCII case-insensitive, unknown keys are ignored, both
/// quote kinds are trimmed, inline comments are part of the value, a recognized key repeated
/// in a second `[Plugin]` section invalidates, a BOM is stripped from every line, and the
/// Enabled tokens and defaults. Then: an invalid declaration is an issue unless a nonempty
/// selection or profile supersedes it, and a disabled one never suppresses those.
pub fn plugin_ini_contract() {
    for (text, expected) in [
        ("[PLUGIN]\nDll=a.dll", ini("a.dll")),
        ("[Plugin]\nunknown=1\ndll=a.dll", ini("a.dll")),
        ("[Other]\nno equals here\n[Plugin]\ndll=a.dll", ini("a.dll")),
        ("[Plugin]\ndll=\"'a.dll'\"", ini("a.dll")),
        ("[Plugin]\ndll=a.dll ; note", ini("a.dll ; note")),
        (
            "[Plugin]\ndll=a.dll\n[Plugin]\ndll=b.dll",
            PluginIni::Invalid,
        ),
        (
            "[Plugin]\nenabled=1\n[Plugin]\nenabled=1\ndll=a.dll",
            PluginIni::Invalid,
        ),
        ("[Plugin]\nenabled=0", PluginIni::Disabled),
        ("[Plugin]\nenabled=NO\ndll=", PluginIni::Disabled),
        ("[Plugin]\nenabled=yes\ndll=a.dll", ini("a.dll")),
        ("[Plugin]\nenabled=On\ndll=a.dll", ini("a.dll")),
        ("[Plugin]\nenabled=TRUE\ndll=a.dll", ini("a.dll")),
        ("[Plugin]\nenabled=2\ndll=a.dll", PluginIni::Invalid),
        ("[Plugin]\ndll=", PluginIni::Invalid),
        ("[Plugin]\nenabled=yes\ndll=", PluginIni::Invalid),
        ("[Plugin]\nenabled=yes\ndll=\"\"", PluginIni::Invalid),
        ("\u{feff}[Plugin]\r\n\u{feff}dll=a.dll\r\n", ini("a.dll")),
        ("", PluginIni::Invalid),
        ("dll=a.dll", PluginIni::Invalid),
        ("[Plugin]\n  # c\n\t; c\n dll = a\\b.dll ", ini("a/b.dll")),
        ("[Plugin]\ndll=a.dll\n[Other]\ndll=b.dll", ini("a.dll")),
    ] {
        assert_eq!(PluginIni::parse(text), expected, "{text:?}");
    }
    let invalid_issue = |case: &Case| {
        case.run()
            .issues
            .iter()
            .filter(|i| i.detail == "native-plugin.ini has an invalid [Plugin] declaration")
            .map(|i| i.status_id.clone())
            .collect::<Vec<_>>()
    };
    let mut p = package("Pkg", Some("a"), &["sel.dll", "prof.dll"]);
    p.ini = PluginIni::Invalid;
    let mut case = Case::new(vec![p], &["a"]);
    // No selection and no profile: the invalid declaration is the package's issue.
    assert_eq!(invalid_issue(&case), [s("dll:pkg:ini")]);
    assert!(case.loads().is_empty());
    // A nonempty selection supersedes it.
    case.selections = vec![selection("pkg", &["sel.dll"])];
    assert!(invalid_issue(&case).is_empty());
    assert_eq!(case.loads(), [(s("Pkg/sel.dll"), Source::Selection)]);
    // So does a nonempty profile (with an empty selection file list).
    case.selections[0].files.clear();
    case.packages[0].profile = Some(Profile {
        id: "p".into(),
        target: Target::Both,
        files: vec![file("prof.dll")],
    });
    assert!(invalid_issue(&case).is_empty());
    assert_eq!(case.loads(), [(s("Pkg/prof.dll"), Source::Profile)]);
    // A disabled declaration never suppresses a selection's files, nor a profile's (the
    // catalog file list): both take precedence.
    case.packages[0].profile = None;
    case.packages[0].ini = PluginIni::Disabled;
    case.selections[0].files = vec![file("sel.dll")];
    assert_eq!(case.loads(), [(s("Pkg/sel.dll"), Source::Selection)]);
    case.selections[0].files.clear();
    case.packages[0].profile = Some(Profile {
        id: "p".into(),
        target: Target::Both,
        files: vec![file("prof.dll")],
    });
    assert_eq!(case.loads(), [(s("Pkg/prof.dll"), Source::Profile)]);
    case.packages[0].profile = None;
    // An enabled declaration always loads in library mode.
    case.selections.clear();
    case.packages[0].ini = ini("sel.dll");
    let selected = case.run();
    assert_eq!(selected.entries.len(), 1);
    assert_eq!(selected.entries[0].mode, Mode::Library);
    assert_eq!(selected.entries[0].source, Source::Ini);
}
