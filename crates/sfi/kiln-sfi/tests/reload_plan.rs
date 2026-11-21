// SPDX-License-Identifier: MIT
// the interpreter's live reload/unload transport on the production plan source.
//! The live reload through the production boundary and plan source (`KilnPlanSource`): the request and result files live in
//! `<game>/kiln/sfi/runtime`; a reload checks each package folder's content digest against the frozen launch plan
//! again (in its smallest form): an unchanged package's Lua is read and run again, a changed one is refused
//! with ShroudForge's failed-to-activate text and its new Lua never runs, the runtime is restored and sfi.json keeps
//! its rows; a package the start withdrew is never admitted by a reload. Nothing below `<game>/shroudforge`.
//! A reload's type registry view (game.types from the granted archive) is released by the end of the reload, also
//! when the reload is refused.
#[path = "support/fake_archive.rs"]
mod fake_archive;
#[path = "support/fake_startup.rs"]
mod fake_startup;
mod support;

use kiln_abi::services::kiln_launch::*;
use kiln_abi::*;
use kiln_sfi::lifecycle;
use kiln_sfi::plan_source::{CHANGED, KilnPlanSource};
use kiln_sfi::runtime::reload::{self, REQUEST_FILE, RESULT_FILE};
use kiln_sfi::state::Lua;
use kiln_sfi::worker::{Worker, WorkerPhase};
use kiln_sfi::{Boundary, InactiveReason, Phase};
use serde_json::{Value, json};
use sfi_policy::digest::package_content_sha256;
use std::path::{Path, PathBuf};
use std::time::Duration;
use support::*;

const WAIT: Duration = Duration::from_secs(300);
const KILN: (i32, i32) = (KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN);

/// A package whose Lua logs `<id>:top<tag>` and `<id>:load<tag>` with shroudforge.log.info.
fn lua(id: &str, tag: &str) -> String {
    format!(
        "shroudforge.log.info('{id}:top{tag}') \
         return {{ on_load = function() shroudforge.log.info('{id}:load{tag}') end, \
                   on_unload = function() shroudforge.log.info('{id}:unload{tag}') end }}"
    )
}
fn put_package(game: &Path, id: &str, text: &str) -> [u8; 32] {
    let dir = game.join("mods").join(id);
    std::fs::create_dir_all(dir.join("src")).unwrap();
    std::fs::write(
        dir.join("mod.json"),
        json!({"id": id, "name": id, "version": "1.0.0", "capabilities": ["runtime"]}).to_string(),
    )
    .unwrap();
    std::fs::write(dir.join("src").join("mod.lua"), text).unwrap();
    package_content_sha256(&dir).unwrap()
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
fn candidate(id: &str, content: [u8; 32]) -> Value {
    json!({"id":id,"version":"1.0.0","kind":"shroudforge","root":format!("mods/{id}"),
      "content_sha256":hex(&content),"config_sha256":"d".repeat(64),"enabled":true,"targets":["client","server"],
      "capabilities":["runtime"],"dependencies":[],"requiredApis":[],"services":[],"requires180":false,
      "saveSensitive":false,"decision":"candidate","reasons":[],
      "data":{"required":false,"admission":"not-needed"},"lua":{"required":true,"admission":"admit"},"dlls":[]})
}
fn with_dll(mut package: Value) -> Value {
    package["dlls"] = json!([{"path":"native/x.dll","sha256":"e".repeat(64),"mode":"library","source":"ini",
        "required":true,"gate":"pass","gateProof_sha256":"f".repeat(64),"bridgeAbi":0}]);
    package
}

struct Boot {
    boundary: &'static Boundary,
    worker: &'static Worker,
    owner: KilnOwner,
    pool: std::sync::Arc<FakePool>,
    runtime: PathBuf,
}
impl Boot {
    fn count(&self, text: &str) -> usize {
        logs(self.owner)
            .iter()
            .filter(|(_, l)| l.ends_with(&format!("] {text}")))
            .count()
    }
    fn wait(&self, text: &str, n: usize) {
        assert!(
            eventually(WAIT, || self.count(text) >= n),
            "{text} x{n}: {:?}",
            logs(self.owner)
        );
    }
    fn request(&self, document: Value) {
        std::fs::create_dir_all(&self.runtime).unwrap();
        let temp = self.runtime.join("request.tmp");
        std::fs::write(&temp, serde_json::to_vec(&document).unwrap()).unwrap();
        std::fs::rename(&temp, self.runtime.join(REQUEST_FILE)).unwrap();
    }
    fn answer(&self, id: &str) -> Value {
        let mut answer = Value::Null;
        assert!(
            eventually(WAIT, || {
                answer = std::fs::read(self.runtime.join(RESULT_FILE))
                    .ok()
                    .and_then(|b| serde_json::from_slice(&b).ok())
                    .unwrap_or(Value::Null);
                answer["requestId"] == id
            }),
            "no answer {id}: {:?}",
            logs(self.owner)
        );
        answer
    }
    fn stop(&self) {
        if let Some(shared) = self.worker.shared() {
            shared.request_stop();
            assert!(self.pool.task(0).wait_finished(WAIT));
        }
    }
}

/// The early phase, Kiln's plugin step and its on_ready, with the production worker factory. kiln-eml's startup seam
/// grants this thread's plan an archive generation (no grant, no runtime), and the fake archive serves it.
fn boot(game: &Path) -> Boot {
    set_launch(Some(KILN));
    set_state_dir(fresh_dir("rl1-state"));
    let worker: &'static Worker =
        Box::leak(Box::new(Worker::new(lifecycle::factory::<KilnPlanSource>)));
    let boundary: &'static Boundary = Box::leak(Box::new(
        Boundary::with_worker(InactiveReason::NegotiationMissing, worker)
            .with_own_loader(|| None)
            .with_startup(fake_startup::resolve),
    ));
    let (early, owner) = (next_owner(), next_owner());
    let pool = FakePool::new(owner, PoolConfig::default());
    let api = Box::leak(api(true));
    fake_archive::wrap(api);
    fake_startup::install(fake_startup::Config::granting_current());
    let (mut early_info, mut info) = (plugin_info(), plugin_info());
    assert_eq!(
        unsafe { boundary.enter(Phase::Early, &*api, early, &mut early_info) },
        KILN_OK
    );
    assert_eq!(
        unsafe { boundary.enter(Phase::Normal, &*api, owner, &mut info) },
        KILN_OK
    );
    assert!(fake_startup::grant_archive(owner));
    unsafe { (info.on_ready.expect("a kiln start"))(info.user) };
    assert!(eventually(WAIT, || worker
        .shared()
        .is_some_and(|s| s.phase() == WorkerPhase::Running)));
    Boot {
        boundary,
        worker,
        owner,
        pool,
        runtime: reload::runtime_dir(game),
    }
}

#[test]
fn a_reload_checks_each_package_against_the_plan_and_refuses_a_changed_one() {
    let game = game_dir();
    let good = put_package(&game, "good", &lua("good", ""));
    let edit = put_package(&game, "edit", &lua("edit", ""));
    let dll = put_package(&game, "native", &lua("native", ""));
    let mut document = plan_document("client");
    document["packages"] = json!([
        candidate("good", good),
        candidate("edit", edit),
        with_dll(candidate("native", dll))
    ]);
    document["loadOrder"] = json!(["good", "edit", "native"]);
    set_plan(Some(serde_json::to_vec(&document).unwrap()));
    let b = boot(&game);
    b.wait("edit:load", 1);
    assert_eq!(b.count("native:top"), 0, "withdrawn at the start");

    // unchanged: the Lua is read and run again
    b.request(json!({"requestId": "same", "modIds": ["good", "edit"]}));
    assert_eq!(b.answer("same")["status"], "reloaded");
    b.wait("edit:load", 2);
    assert_eq!(b.count("good:top"), 2);

    // a package the start withdrew (its DLL never loaded) is never admitted by a reload
    b.request(json!({"requestId": "native", "modIds": ["native"]}));
    assert_eq!(
        b.answer("native")["message"],
        reload::failed_to_activate("native", reload::NOT_ADMITTED)
    );
    assert_eq!(b.count("native:top"), 0);

    // the Lua changed on disk after the launcher hashed the folder: refused before anything is unloaded, the
    // running packages are left alone and the new Lua never runs
    std::fs::write(
        game.join("mods").join("edit").join("src").join("mod.lua"),
        lua("edit", "2"),
    )
    .unwrap();
    let (loads, unloads) = (b.count("edit:load"), b.count("edit:unload"));
    b.request(json!({"requestId": "changed", "modIds": ["good"]}));
    let answer = b.answer("changed");
    assert_eq!(answer["status"], "error");
    assert_eq!(
        answer["message"],
        reload::failed_to_activate("edit", CHANGED)
    );
    assert_eq!(
        (b.count("edit:load"), b.count("edit:unload")),
        (loads, unloads),
        "left alone: {:?}",
        logs(b.owner)
    );
    assert_eq!(
        b.count("edit:top2") + b.count("edit:load2"),
        0,
        "{:?}",
        logs(b.owner)
    );
    // sfi.json keeps the running package's row: no skip, still active
    let record = b.boundary.status().record().unwrap();
    let row = record.package("edit").unwrap();
    assert_eq!(
        (row.lua, row.skip.is_none()),
        (Lua::Active, true),
        "{row:?}"
    );
    assert!(!game.join("shroudforge").exists());
    b.stop();
}

/// game.types from the archive generation kiln-eml's startup outcome granted: each runtime's types family reads the
/// registry through a view that is released once the registry is read. A reload's new VM opens one of its own and it
/// is released by the end of the reload; a reload refused before the new VM (a changed package) opens none; a reload
/// refused after the new VM's types family opened one (its registry read failed) releases it. None is held at the
/// leave. Each VM's typed-value arena follows its VM: the reload's new one is made, the old one drains, the
/// refused rebuild's drains, and the stop drains the last; each arena's view is released after it drained.
#[test]
fn a_reload_releases_its_type_registry_view_also_when_refused() {
    let game = game_dir();
    let typed = put_package(&game, "typed", &lua("typed", ""));
    let mut document = plan_document("client");
    document["packages"] = json!([candidate("typed", typed)]);
    document["loadOrder"] = json!(["typed"]);
    set_plan(Some(serde_json::to_vec(&document).unwrap()));
    let b = boot(&game);
    b.wait("typed:load", 1);
    // (type registry views opened, every one released exactly once); each VM's typed-value arena holds a view of its
    // own besides, counted apart: (arenas made, arenas still open, the open ones' views held, every other
    // arena view released exactly once and only after its arena drained)
    let held = || {
        let seen = fake_archive::seen(b.owner);
        let arenas: Vec<_> = fake_archive::arenas(b.owner)
            .iter()
            .map(|a| a.view)
            .collect();
        let types: Vec<_> = seen.views.iter().filter(|v| !arenas.contains(*v)).collect();
        let released: Vec<_> = seen
            .released
            .iter()
            .filter(|v| !arenas.contains(*v))
            .collect();
        (types.len(), released == types)
    };
    let arenas = || {
        let all = fake_archive::arenas(b.owner);
        let released = fake_archive::seen(b.owner).released;
        let open = all.iter().filter(|a| !a.closed).count();
        let settled = all.iter().all(|a| {
            let times = released.iter().filter(|v| **v == a.view).count();
            if a.closed {
                a.drained && times == 1
            } else {
                times == 0
            }
        });
        (all.len(), open, settled)
    };
    assert_eq!(held(), (1, true), "runtime-create's view");
    assert_eq!(arenas(), (1, 1, true), "runtime-create's arena");

    b.request(json!({"requestId": "ok"}));
    assert_eq!(b.answer("ok")["status"], "reloaded");
    b.wait("typed:load", 2);
    assert_eq!(held(), (2, true), "the reload's own view");
    assert_eq!(
        arenas(),
        (2, 1, true),
        "the reload's own arena; the old one drained"
    );

    // refused before the new VM: the rebuild's wiring never opens a view
    let source = game.join("mods").join("typed").join("src").join("mod.lua");
    std::fs::write(&source, lua("typed", "2")).unwrap();
    b.request(json!({"requestId": "changed"}));
    assert_eq!(
        b.answer("changed")["message"],
        reload::failed_to_activate("typed", CHANGED)
    );
    assert_eq!(held(), (2, true), "{:?}", fake_archive::seen(b.owner).calls);
    assert_eq!(arenas(), (2, 1, true), "no arena for a refused reload");
    std::fs::write(&source, lua("typed", "")).unwrap();

    // refused after the new VM's types family opened its view (its header page then needs more bytes than the
    // sizing call said): the view is released with the refused rebuild
    let identity = fake_startup::served().expect("the start's identity");
    let mut failing = fake_archive::Grant::new(identity, fake_startup::pages(identity));
    failing.grow = 5;
    fake_archive::grant(b.owner, failing);
    let loads = b.count("typed:load");
    b.request(json!({"requestId": "broken"}));
    let answer = b.answer("broken");
    assert_eq!(answer["status"], "error");
    assert!(
        answer["message"]
            .as_str()
            .unwrap()
            .starts_with("updated runtime initialization failed: "),
        "{answer}"
    );
    b.wait("typed:load", loads + 1);
    assert_eq!(held(), (3, true), "{:?}", fake_archive::seen(b.owner).calls);
    assert_eq!(
        arenas(),
        (3, 1, true),
        "the refused rebuild's arena drained: {:?}",
        fake_archive::values_events(b.owner)
    );
    b.stop();
    assert_eq!(held(), (3, true));
    assert_eq!(arenas(), (3, 0, true));
}
