// SPDX-License-Identifier: MIT
//! The pre-select inventory: discovery and classification, authoritative blockers vs warning-only findings, the
//! static Lua scan, the parity catalog answers, and the no-effects / no-ShroudForge-folder rules.
mod sfi_scan_common;
use kiln_manage::manifest::RunSide;
use kiln_manage::sfi_scan::{
    self, Kind, Presence,
    lua::{self, Uncertain},
    parity::CatalogInput,
};
use serde_json::{Value, json};
use sfi_package::parity::{Catalog, CatalogHeader, ExpectedCatalog, Symbol};
use sfi_scan_common::*;
use std::collections::BTreeSet;
use std::fs;
use std::path::Path;

fn roots() -> BTreeSet<String> {
    ["shroudforge", "loader", "integer", "table", "builtin"]
        .iter()
        .map(|s| s.to_string())
        .collect()
}
fn scan(src: &str) -> lua::FileScan {
    let mut budget = lua::MAX_TOKENS;
    lua::scan(&lua::tokenize(src, &mut budget).unwrap(), &roots())
}
fn chains(s: &lua::FileScan) -> Vec<String> {
    s.references.iter().map(|r| r.chain.clone()).collect()
}
fn uncertain(s: &lua::FileScan) -> Vec<Uncertain> {
    s.findings.iter().map(|f| f.class).collect()
}

#[test]
fn comments_and_strings_are_never_references() {
    let s = scan(concat!(
        "-- shroudforge.comment.one()\n",
        "--[[ shroudforge.comment.two() ]]\n",
        "--[==[ ]] shroudforge.comment.three() ]==]\n",
        "local a = \"shroudforge.string.one\" .. 'shroudforge.string.two \\' still'\n",
        "local b = [[shroudforge.string.three\n]] .. [=[ ]] shroudforge.string.four ]=]\n",
        "local c = \"line\\z\n   shroudforge.string.five\"\n",
        "shroudforge.real.call()\n",
    ));
    assert_eq!(chains(&s), ["shroudforge.real.call"]);
    assert_eq!(
        s.references[0].line, 9,
        "line numbers survive long strings, escapes and comments"
    );
    assert!(s.findings.is_empty(), "{:?}", s.findings);
}

#[test]
fn dot_bracket_alias_and_method_chains_resolve() {
    let s = scan(concat!(
        "shroudforge.ui.add('x')\n",
        "shroudforge[\"ui\"]['remove']()\n",
        "local ui = shroudforge.ui\n",
        "ui.show()\n",
        "shroudforge.ui:open()\n",
        "local sf = builtin.shroudforge\n",
        "sf.log.info('hi')\n",
    ));
    let got = chains(&s);
    for want in [
        "shroudforge.ui.add",
        "shroudforge.ui.remove",
        "shroudforge.ui.show",
        "shroudforge.ui",
        "builtin.shroudforge.log.info",
    ] {
        assert!(got.iter().any(|c| c == want), "{want} missing from {got:?}");
    }
    let show = s
        .references
        .iter()
        .find(|r| r.chain == "shroudforge.ui.show")
        .unwrap();
    assert_eq!(show.via.as_deref(), Some("ui"));
    let open = s
        .references
        .iter()
        .find(|r| r.method.as_deref() == Some("open"))
        .unwrap();
    assert_eq!(open.chain, "shroudforge.ui");
    assert_eq!(s.references[0].literal_arg.as_deref(), Some("x"));
}

#[test]
fn require_literals_and_dynamic_requires() {
    let s = scan(
        "require 'a.b'\nrequire(\"c\")\nrequire(name)\nlocal r = require\nrequire('d' .. x)\n",
    );
    assert_eq!(s.requires, [("a.b".to_string(), 1), ("c".to_string(), 2)]);
    assert_eq!(
        uncertain(&s)
            .iter()
            .filter(|u| **u == Uncertain::DynamicRequire)
            .count(),
        3,
        "{:?}",
        s.findings
    );
}

#[test]
fn shadowed_reassigned_metatable_and_computed_are_uncertain() {
    let s = scan(concat!(
        "local t = { shroudforge = 1, loader = 2 }\n",
        "shroudforge[key]()\n",
        "local x = _G['shroudforge']\n",
        "setmetatable(t, {})\n",
        "load('return 1')()\n",
        "function shroudforge.ui.add() end\n",
        "shroudforge.ui.remove = nil\n",
        "local ui = shroudforge.ui\n",
        "ui = {}\n",
        "ui.after()\n",
        "local shroudforge = {}\n",
        "shroudforge.after.shadow()\n",
    ));
    let u = uncertain(&s);
    for want in [
        Uncertain::Computed,
        Uncertain::Metatable,
        Uncertain::DynamicLoad,
        Uncertain::Reassigned,
        Uncertain::Shadowed,
    ] {
        assert!(u.contains(&want), "{want:?} missing from {:?}", s.findings);
    }
    // the table-constructor keys are not assignments to the API
    assert!(!s.findings.iter().any(|f| f.line == 1), "{:?}", s.findings);
    // three assignments: function shroudforge.ui.add, shroudforge.ui.remove =, ui =
    assert_eq!(
        u.iter().filter(|u| **u == Uncertain::Reassigned).count(),
        3,
        "{:?}",
        s.findings
    );
    let got = chains(&s);
    assert!(
        !got.iter().any(|c| c.contains("after")),
        "a reassigned alias and a shadowed root resolve nothing: {got:?}"
    );
    assert!(
        !got.iter().any(|c| c == "shroudforge.ui.remove"),
        "an assignment target is not a call: {got:?}"
    );
}

#[test]
fn unterminated_source_and_budget_are_errors_not_panics() {
    let mut budget = lua::MAX_TOKENS;
    for bad in [
        "x = 'open",
        "x = [[open",
        "--[[ open",
        "x = \"a\\q\"",
        "x = \u{7f}",
    ] {
        assert!(lua::tokenize(bad, &mut budget).is_err(), "{bad:?}");
    }
    let mut small = 3;
    assert!(lua::tokenize("a b c d e", &mut small).is_err());
}

#[test]
fn inventory_classifies_every_package_folder() {
    let g = Game::new("classify");
    g.simple("sf-lua", "sf-lua", &["runtime"], json!([]));
    g.lua("sf-lua", "src/mod.lua", "shroudforge.log.info('x')\n");
    g.sf(
        "eml-data",
        json!({"id":"eml-data","name":"E","version":"1.0.0","capabilities":["patch"]}),
        json!({"schemaVersion":1,"enabled":true,"launcher":"EML"}),
    );
    g.sf(
        "off",
        json!({"id":"off","name":"off","version":"1.0.0"}),
        json!({"schemaVersion":1,"enabled":false}),
    );
    g.sf(
        "server-only",
        json!({"id":"server-only","name":"S","version":"1.0.0","capabilities":["runtime"]}),
        json!({"schemaVersion":1,"enabled":true,"targets":["server"]}),
    );
    g.file("broken", "mod.json", "{");
    g.sf(
        "needs-18",
        json!({"id":"needs-18","name":"N","version":"1.0.0"}),
        json!({"schemaVersion":1,"enabled":true,"groups":[{"label":"g","tab":true}]}),
    );
    g.file("not-a-package", "readme.txt", "hello");
    g.file(
        ".hidden",
        "mod.json",
        json!({"id":"h","name":"h","version":"1.0.0"}).to_string(),
    );
    let inv = client(&g);
    assert_eq!(inv.side, "client");
    assert_eq!(inv.mods_root, "mods");
    assert_eq!(inv.dialect, "sf179");
    let dirs: Vec<_> = inv.packages.iter().map(|p| p.directory.as_str()).collect();
    assert_eq!(
        dirs,
        [
            "broken",
            "eml-data",
            "needs-18",
            "off",
            "server-only",
            "sf-lua"
        ]
    );

    let p = pkg(&inv, "sf-lua");
    assert_eq!(
        (p.kind, p.presence, p.eligible(), p.planned),
        (Kind::Shroudforge, Presence::OnSide, true, true)
    );
    assert!(
        p.parts.lua && p.parts.lua_entry && p.parts.runtime && !p.parts.data && !p.parts.native
    );
    assert_eq!(p.targets, ["client", "server"]);

    let p = pkg(&inv, "eml-data");
    assert_eq!(
        (p.kind, p.presence, p.eligible()),
        (Kind::Eml, Presence::OnSide, true)
    );
    assert!(p.parts.data && !p.parts.lua);

    let p = pkg(&inv, "off");
    assert_eq!(
        (p.presence, p.eligible(), p.planned),
        (Presence::Disabled, true, false)
    );
    let p = pkg(&inv, "server-only");
    assert_eq!(
        (p.presence, p.eligible(), p.planned),
        (Presence::OtherSide, true, false)
    );
    let server = run(&g, RunSide::Server, &native(), CatalogInput::Missing);
    assert_eq!(pkg(&server, "server-only").presence, Presence::OnSide);
    assert_eq!(server.side, "server");

    let p = pkg(&inv, "broken");
    assert_eq!(codes(p), ["discovery"]);
    assert!(!p.requires180);
    let p = pkg(&inv, "needs-18");
    assert_eq!(codes(p), ["requires-1.8", "discovery"]);
    assert!(p.requires180 && !p.eligible());
}

#[test]
fn shroudforge_plan_issues_are_authoritative_with_their_closure() {
    let g = Game::new("plan");
    g.simple("a", "a", &["runtime"], json!([{"id":"b","version":"^1"}]));
    g.simple("b", "b", &["runtime"], json!([{"id":"a","version":"^1"}]));
    g.simple(
        "c",
        "c",
        &["runtime"],
        json!([{"id":"missing","version":"^1"}]),
    );
    g.simple("d", "d", &["runtime"], json!([{"id":"c","version":"^1"}]));
    g.simple("e", "e", &["runtime"], json!([{"id":"f","version":"^2"}]));
    g.simple("f", "f", &["runtime"], json!([]));
    g.simple(
        "ok",
        "ok",
        &["runtime"],
        json!([{"id":"f","version":"^1"},{"id":"gone","version":"^1","optional":true}]),
    );
    let inv = client(&g);
    assert_eq!(codes(pkg(&inv, "a")), ["plan:dependency-cycle"]);
    assert_eq!(codes(pkg(&inv, "c")), ["plan:missing-dependency"]);
    assert!(!pkg(&inv, "d").eligible());
    assert_eq!(codes(pkg(&inv, "e")), ["plan:dependency-version"]);
    assert!(pkg(&inv, "ok").eligible() && pkg(&inv, "f").eligible());
    assert_eq!(inv.load_order, ["f", "ok"]);
}

#[test]
fn require_closure_findings_are_advisories() {
    let g = Game::new("require");
    g.simple("p", "p", &["runtime"], json!([]));
    g.lua("p", "src/mod.lua", "local a = require('a')\nrequire('x.y')\nrequire('../../../etc/passwd')\nrequire('mods.other.thing')\n");
    g.lua("p", "src/a.lua", "return require('mod')\n");
    g.lua("p", "src/lonely.lua", "-- never required\n");
    let inv = client(&g);
    let p = pkg(&inv, "p");
    assert!(p.eligible(), "{:?}", p.blockers);
    let details: Vec<_> = p
        .advisories
        .iter()
        .filter(|a| a.class == "require")
        .map(|a| a.detail.as_str())
        .collect();
    assert!(
        details
            .iter()
            .any(|d| d.contains("require cycle: src/mod.lua -> src/a.lua -> src/mod.lua")),
        "{details:?}"
    );
    assert!(
        details
            .iter()
            .any(|d| d.contains("src/x/y.lua is not in the package")),
        "{details:?}"
    );
    assert!(
        details
            .iter()
            .any(|d| d.contains("../../../etc/passwd") && d.contains("not followed")),
        "{details:?}"
    );
    assert!(
        details
            .iter()
            .any(|d| d.contains("package other is not installed")),
        "{details:?}"
    );
}

fn catalog_parts() -> (CatalogHeader, Vec<Symbol>) {
    let v: Value =
        serde_json::from_slice(include_bytes!("fixtures/sfi-scan/catalog-v1.json")).unwrap();
    let header: CatalogHeader = serde_json::from_value(v["header"].clone()).unwrap();
    let mut symbols: Vec<Symbol> = serde_json::from_value(v["symbols"].clone()).unwrap();
    let full = |name: &str, scope: &str| -> Symbol {
        let revisions: Vec<Value> = ["sf179", "sf180"]
            .iter()
            .map(|r| {
                json!({"revision": r, "presence": "callable", "parity": "full",
                    "required": 1, "proven_full": 1, "tests": ["t"], "digest": "0".repeat(64)})
            })
            .collect();
        serde_json::from_value(json!({
            "name": name, "scope": scope, "kind": "function", "introduced": null, "alias_of": null, "composite": [],
            "revisions": revisions,
        }))
        .unwrap()
    };
    symbols.push(full("shroudforge.log.info", "namespace-member"));
    symbols.push(full("shroudforge.Notification", "type-declaration"));
    symbols.push(full("Hidden.member", "type-declaration"));
    symbols.sort_by(|a, b| a.name.cmp(&b.name));
    (header, symbols)
}
fn expected(header: &CatalogHeader) -> ExpectedCatalog {
    ExpectedCatalog {
        final_manifest_sha256: header.final_manifest_sha256.clone(),
        frozen: header.frozen.clone(),
        inventories: header
            .revisions
            .iter()
            .map(|r| (r.id, r.inventory_sha256.clone()))
            .collect(),
        policy_sha256: header.policy_sha256.clone(),
        registry_sha256: header.registry_sha256.clone(),
        backend_build: header.backend_build.clone(),
        evidence_sha256: None,
    }
}
const API_LUA: &str = concat!(
    "shroudforge.log.info('ok')\n",
    "table.insert(t, 1)\n",
    "integer.u8.neg(1)\n",
    "shroudforge.new_api()\n",
    "loader.runtime.register_dll('helper.dll')\n",
    "local n = shroudforge.Notification\n",
    "Hidden.member()\n",
    "shroudforge.never_heard_of()\n",
    "local m = setmetatable({}, {})\n",
);

#[test]
fn catalog_answers_for_sf179_are_warnings_only() {
    let (header, symbols) = catalog_parts();
    let catalog = Catalog::new(header.clone(), symbols).unwrap();
    let bytes = catalog.to_json();
    let exp = expected(&header);
    let g = Game::new("catalog");
    g.simple("p", "p", &["runtime"], json!([]));
    g.lua("p", "src/mod.lua", API_LUA);
    let inv = run(
        &g,
        RunSide::Client,
        &native(),
        CatalogInput::Provided {
            bytes: &bytes,
            expected: &exp,
        },
    );
    assert_eq!(inv.catalog.status, "verified");
    assert_eq!(inv.catalog.revision, "sf179");
    assert_eq!(inv.catalog.digest.as_deref(), Some(catalog.digest()));
    assert_eq!(
        inv.catalog.inventory_sha256.as_deref(),
        Some(header.revisions[0].inventory_sha256.as_str())
    );
    let p = pkg(&inv, "p");
    assert!(p.eligible(), "advisories never block: {:?}", p.blockers);
    let found = |class: &str, needle: &str| {
        p.advisories
            .iter()
            .any(|a| a.class == class && a.detail.contains(needle))
    };
    assert!(
        found("api-unproven", "table.insert"),
        "composite with partial parts: {:?}",
        p.advisories
    );
    assert!(
        found("api-absent", "integer.u8.neg"),
        "absence obligation proven Full is still no callable"
    );
    assert!(
        found("api-1.8-only", "shroudforge.new_api"),
        "callable in sf180, not in sf179: the query is sf179"
    );
    assert!(found("api-excluded", "loader.runtime.register_dll"));
    assert!(found("register-dll", "helper.dll"));
    assert!(
        found("api-not-a-global", "shroudforge.Notification"),
        "a declaration is not a global"
    );
    assert!(
        found("api-coverage-unknown", "1 API reference"),
        "never_heard_of is unknown: {:?}",
        p.advisories
    );
    assert!(found("lua-uncertain:metatable", "setmetatable"));
    assert!(
        !p.advisories
            .iter()
            .any(|a| a.detail.contains("shroudforge.log.info")),
        "a Full callable has no warning"
    );
    assert!(
        !p.advisories.iter().any(|a| a.detail.contains("Hidden")),
        "a declaration-only name is no API root"
    );
    // and register_dll seen in Lua is not promoted to a declared target
    assert!(p.dlls.is_empty());
}

#[test]
fn missing_unreadable_and_stale_catalogs_say_coverage_unknown() {
    let (header, symbols) = catalog_parts();
    let catalog = Catalog::new(header.clone(), symbols).unwrap();
    let bytes = catalog.to_json();
    let mut stale = expected(&header);
    stale.backend_build = "another-build".into();
    let g = Game::new("stale");
    g.simple("p", "p", &["runtime"], json!([]));
    g.lua("p", "src/mod.lua", API_LUA);
    let garbage = b"{\"schema_version\":1}".to_vec();
    let exp = expected(&header);
    for (input, status) in [
        (CatalogInput::Missing, "missing"),
        (
            CatalogInput::Provided {
                bytes: &garbage,
                expected: &exp,
            },
            "unreadable",
        ),
        (
            CatalogInput::Provided {
                bytes: &bytes,
                expected: &stale,
            },
            "stale",
        ),
    ] {
        let inv = run(&g, RunSide::Client, &native(), input);
        assert_eq!(inv.catalog.status, status);
        assert!(
            inv.warnings
                .iter()
                .any(|w| w.contains("API coverage is unknown")),
            "{:?}",
            inv.warnings
        );
        let p = pkg(&inv, "p");
        assert!(p.eligible());
        assert!(
            p.advisories
                .iter()
                .any(|a| a.class == "api-coverage-unknown"
                    && a.detail.contains("no verified parity catalog"))
        );
        assert!(
            !p.advisories
                .iter()
                .any(|a| a.class.starts_with("api-") && a.class != "api-coverage-unknown"),
            "{status}: {:?}",
            p.advisories
        );
    }
}

#[test]
fn lua_findings_never_change_eligibility() {
    let g = Game::new("eligibility");
    for (dir, src) in [
        ("plain", "-- nothing\n"),
        (
            "noisy",
            "shroudforge[k]()\nlocal shroudforge = 1\nload(x)\nrequire(y)\nsetmetatable(_G, {})\n",
        ),
    ] {
        g.simple(dir, dir, &["runtime"], json!([]));
        g.lua(dir, "src/mod.lua", src);
    }
    g.lua("noisy", "src/broken.lua", "x = 'open\n");
    let inv = client(&g);
    let (plain, noisy) = (pkg(&inv, "plain"), pkg(&inv, "noisy"));
    assert!(noisy.advisories.len() > plain.advisories.len());
    assert!(
        classes(noisy).iter().any(|c| c == "lua-scan"),
        "the unterminated string is reported: {:?}",
        noisy.advisories
    );
    assert_eq!(
        (plain.blockers.clone(), plain.planned),
        (noisy.blockers.clone(), noisy.planned)
    );
    assert!(noisy.eligible());
}

#[test]
fn kiln_profiles_apply_and_the_shroudforge_folder_is_never_touched() {
    let g = Game::new("sfroot");
    g.simple("pkg", "pkg", &["runtime"], json!([]));
    g.lua("pkg", "src/mod.lua", "shroudforge.log.info('x')\n");
    // ShroudForge's own data: a config moving the mods folder, a profile turning pkg off, rules blocking it.
    let sf = g.0.join("shroudforge");
    let other = g.0.join("elsewhere");
    fs::create_dir_all(other.join("ghost")).unwrap();
    fs::write(
        other.join("ghost/mod.json"),
        json!({"id":"ghost","name":"g","version":"1.0.0"}).to_string(),
    )
    .unwrap();
    fs::create_dir_all(sf.join("config/compatibility")).unwrap();
    fs::create_dir_all(sf.join("profiles")).unwrap();
    fs::write(
        sf.join("config/modloader-config.json"),
        json!({"paths":{"mods": other}}).to_string(),
    )
    .unwrap();
    // (a profile cannot switch enabled; ShroudForge drops that key) it moves the package to the server side
    let off = json!({"schemaVersion":1,"id":"off","packageDirectory":"pkg","name":"Off","extendedModJson":{"targets":["server"]}});
    fs::write(sf.join("profiles/off.json"), off.to_string()).unwrap();
    fs::write(sf.join("config/compatibility/rules.json"), "{").unwrap();
    let before = snapshot(&g.0);
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&sf, fs::Permissions::from_mode(0o000)).unwrap();
    }
    let n = native();
    let inv = run(&g, RunSide::Client, &n, CatalogInput::Missing);
    let p = pkg(&inv, "pkg");
    assert_eq!(
        (p.presence, p.eligible()),
        (Presence::OnSide, true),
        "nothing of shroudforge/ applied"
    );
    assert!(!inv.packages.iter().any(|p| p.directory == "ghost"));
    // Handing in ShroudForge's own profile folder is refused, not read.
    let inv = run_with(
        &g,
        RunSide::Client,
        &n,
        &policy(),
        CatalogInput::Missing,
        Some(&sf.join("profiles")),
    );
    assert!(
        inv.warnings
            .iter()
            .any(|w| w.contains("inside the game's shroudforge folder")),
        "{:?}",
        inv.warnings
    );
    assert_eq!(pkg(&inv, "pkg").presence, Presence::OnSide);
    let upper = g.0.join("ShroudForge").join("profiles");
    assert!(
        sfi_scan::inside_shroudforge(&g.0, &upper),
        "letter case is ignored, as on Windows"
    );
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&sf, fs::Permissions::from_mode(0o755)).unwrap();
    }
    // A Kiln-owned profile folder is read like the interpreter reads profiles.
    let kiln_profiles = g.0.join("kiln-profiles");
    fs::create_dir_all(&kiln_profiles).unwrap();
    fs::write(kiln_profiles.join("off.json"), off.to_string()).unwrap();
    let after_runs = snapshot(&g.0);
    assert_eq!(
        before,
        strip(&after_runs, "kiln-profiles"),
        "the inventory wrote nothing"
    );
    let inv = run_with(
        &g,
        RunSide::Client,
        &n,
        &policy(),
        CatalogInput::Missing,
        Some(&kiln_profiles),
    );
    assert_eq!(
        pkg(&inv, "pkg").presence,
        Presence::OtherSide,
        "{:?}",
        inv.warnings
    );
    assert_eq!(snapshot(&g.0), after_runs);
}

#[test]
fn a_missing_mods_folder_is_not_created() {
    let g = Game::new("nomods");
    fs::remove_dir_all(g.0.join("mods")).unwrap();
    let inv = client(&g);
    assert!(inv.packages.is_empty());
    assert!(inv.warnings.iter().any(|w| w.contains("no mods folder")));
    assert!(!g.0.join("mods").exists());
}

#[test]
fn links_out_of_the_mods_folder_are_not_followed() {
    let g = Game::new("links");
    let outside = g.0.join("outside");
    fs::create_dir_all(outside.join("src")).unwrap();
    fs::write(
        outside.join("mod.json"),
        json!({"id":"outside","name":"o","version":"1.0.0"}).to_string(),
    )
    .unwrap();
    fs::write(outside.join("src/secret.lua"), "shroudforge.leak()\n").unwrap();
    g.simple("p", "p", &["runtime"], json!([]));
    g.lua("p", "src/mod.lua", "-- entry\n");
    #[cfg(unix)]
    {
        std::os::unix::fs::symlink(&outside, g.dir("linked")).unwrap();
        std::os::unix::fs::symlink(
            outside.join("src/secret.lua"),
            g.dir("p").join("src/leak.lua"),
        )
        .unwrap();
    }
    let inv = client(&g);
    assert!(!inv.packages.iter().any(|p| p.directory == "linked"));
    let p = pkg(&inv, "p");
    assert!(p.eligible());
    assert!(
        !format!("{:?}", p.advisories).contains("leak"),
        "{:?}",
        p.advisories
    );
}

#[test]
fn save_sensitive_follows_the_exact_policy() {
    let g = Game::new("safety");
    let manifest = json!({"id":"qs","name":"Q","version":"0.8.2","capabilities":["patch","runtime-register-dll"]});
    g.sf(
        "qs",
        manifest.clone(),
        json!({"schemaVersion":1,"enabled":true}),
    );
    g.file(
        "qs",
        "native-plugin.ini",
        "[Plugin]\nEnabled=1\nDll=qs.dll\n",
    );
    g.file("qs", "qs.dll", b"not a real dll");
    let sha = |p: &Path| kiln_manage::hash::sha256_hex(&fs::read(p).unwrap());
    let row = |dll_sha: &str, mixed: &str| {
        json!({"format":1,"revision":7,"scope":"every-backend","unknownMixedDataNative":mixed,"packages":[{
            "id":"qs","version":"0.8.2","name":"Q","risk":"save-data-loss","text":"drops ammo","evidence":"synthetic",
            "variants":[{"files":[
                {"path":"mod.json","sha256":sha(&g.dir("qs").join("mod.json")),"size":fs::metadata(g.dir("qs").join("mod.json")).unwrap().len()},
                {"path":"QS.DLL","sha256":dll_sha,"size":14}
            ]}]}]})
        .to_string()
    };
    let n = native();
    let exact = sfi_scan::safety::Policy::parse(
        row(&sha(&g.dir("qs").join("qs.dll")), "save-sensitive").as_bytes(),
    )
    .unwrap();
    let inv = run_with(&g, RunSide::Client, &n, &exact, CatalogInput::Missing, None);
    let p = pkg(&inv, "qs");
    assert_eq!(p.save_sensitive.basis, "policy");
    assert_eq!(p.save_sensitive.text.as_deref(), Some("drops ammo"));
    assert_eq!(
        (inv.policy_revision, inv.policy_scope),
        (7, "every-backend")
    );
    let other = "0".repeat(64);
    let mixed = sfi_scan::safety::Policy::parse(row(&other, "save-sensitive").as_bytes()).unwrap();
    let inv = run_with(&g, RunSide::Client, &n, &mixed, CatalogInput::Missing, None);
    assert_eq!(
        pkg(&inv, "qs").save_sensitive.basis,
        "unknown-mixed",
        "no exact row: data plus native"
    );
    let listed = sfi_scan::safety::Policy::parse(row(&other, "listed-only").as_bytes()).unwrap();
    let inv = run_with(
        &g,
        RunSide::Client,
        &n,
        &listed,
        CatalogInput::Missing,
        None,
    );
    assert_eq!(pkg(&inv, "qs").save_sensitive.basis, "none");
    // The shipped policy is the one in schema/sfi.
    assert_eq!(policy().rows[0].id, "ammo-slots");
}

#[test]
fn the_scan_has_no_execution_load_network_or_write_path() {
    for (name, src) in [
        ("mod.rs", include_str!("../src/sfi_scan/mod.rs")),
        ("lua.rs", include_str!("../src/sfi_scan/lua.rs")),
        ("native.rs", include_str!("../src/sfi_scan/native.rs")),
        ("parity.rs", include_str!("../src/sfi_scan/parity.rs")),
    ] {
        for banned in [
            "mlua",
            "LoadLibrary",
            "libloading",
            "dlopen",
            "Command::new",
            "std::process",
            "TcpStream",
            "UdpSocket",
            "ureq",
            "fs::write",
            "create_dir",
            "remove_file",
            "remove_dir",
            "fs::rename",
            "File::create",
            "OpenOptions",
            "set_permissions",
            "sfi_core",
            "kiln_sfi::",
        ] {
            assert!(!src.contains(banned), "{name} contains {banned}");
        }
    }
}

/// Every path under `root` with its kind, size and modification time.
fn snapshot(root: &Path) -> Vec<(String, bool, u64, Option<std::time::SystemTime>)> {
    let mut out = Vec::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };
        for e in entries.flatten() {
            let m = fs::symlink_metadata(e.path()).unwrap();
            out.push((
                e.path()
                    .strip_prefix(root)
                    .unwrap()
                    .to_string_lossy()
                    .into_owned(),
                m.is_dir(),
                if m.is_dir() { 0 } else { m.len() },
                m.modified().ok(),
            ));
            if m.is_dir() {
                stack.push(e.path());
            }
        }
    }
    out.sort();
    out
}
fn strip(
    snap: &[(String, bool, u64, Option<std::time::SystemTime>)],
    prefix: &str,
) -> Vec<(String, bool, u64, Option<std::time::SystemTime>)> {
    snap.iter()
        .filter(|e| !e.0.starts_with(prefix))
        .cloned()
        .collect()
}

// Edge cases.

#[test]
fn duplicate_ids_are_discovery_blockers_not_a_1_8_requirement() {
    let g = Game::new("dup");
    g.simple("dup-a", "same", &["runtime"], json!([]));
    g.simple("dup-b", "same", &["runtime"], json!([]));
    let inv = client(&g);
    for dir in ["dup-a", "dup-b"] {
        let p = pkg(&inv, dir);
        assert!(
            !p.requires180,
            "{dir}: a valid 1.7.9 manifest does not need 1.8: {:?}",
            p.blockers
        );
        assert_eq!(codes(p), ["discovery"], "{dir}: {:?}", p.blockers);
        assert!(
            p.blockers[0].detail.contains("duplicate mod ID"),
            "{dir}: {:?}",
            p.blockers
        );
        assert_eq!(p.id.as_deref(), Some("same"));
    }
}

#[test]
fn a_long_comma_run_is_scanned_in_linear_time() {
    // `shroudforge, shroudforge, ...` with no statement end: each comma used to scan to the end of the file.
    let src = "shroudforge, ".repeat(300_000);
    let deep = format!("{}{}", "shroudforge, (".repeat(150_000), "x");
    for text in [src, deep] {
        let started = std::time::Instant::now();
        let mut budget = lua::MAX_TOKENS;
        let tokens = lua::tokenize(&text, &mut budget).unwrap();
        let s = lua::scan(&tokens, &roots());
        assert!(!s.references.is_empty());
        assert!(
            started.elapsed() < std::time::Duration::from_secs(20),
            "took {:?}",
            started.elapsed()
        );
    }
    // The lookahead still tells an assignment list from an expression list.
    assert_eq!(
        uncertain(&scan("shroudforge, x = 1, 2\n")),
        [Uncertain::Reassigned]
    );
    assert!(uncertain(&scan("f(shroudforge, (x), y)\nz = 1\n")).is_empty());
    assert_eq!(
        uncertain(&scan("t[1], shroudforge.a = 1, 2\n")),
        [Uncertain::Reassigned]
    );
}

#[test]
fn a_lua_file_over_the_budget_is_not_read() {
    let g = Game::new("hugelua");
    g.simple("p", "p", &["runtime"], json!([]));
    g.lua("p", "src/mod.lua", "shroudforge.log.info('x')\n");
    let big = g.dir("p").join("src/big.lua");
    // Sparse: 17 MiB of NUL bytes, which the tokenizer would reject if it ever read them.
    fs::File::options()
        .create(true)
        .truncate(true)
        .write(true)
        .open(&big)
        .unwrap()
        .set_len(17 * 1024 * 1024)
        .unwrap();
    let inv = client(&g);
    let p = pkg(&inv, "p");
    assert!(p.eligible());
    let text = format!("{:?}", p.advisories);
    assert!(text.contains("larger than the Lua budget"), "{text}");
    assert!(!text.contains("unexpected character"), "{text}");
    assert!(p.parts.lua_entry, "the small entry is still scanned");
}
