// SPDX-License-Identifier: MIT
//! Plan source test 8: the exported boundary (what `kiln_plugin_early` and `kiln_plugin_main` enter) binds the
//! production worker; under backend `kiln` its on_ready submits it and the runtime follows the frozen plan.
//! The boundary is a process static, so this file holds one test. kiln-eml's startup seam is the fake one this
//! process registers as kiln-eml's export (off Windows the production resolver finds what was registered); it grants
//! the plan, so the runtime runs with that archive generation.
#[path = "support/fake_archive.rs"]
mod fake_archive;
#[path = "support/fake_startup.rs"]
mod fake_startup;
mod support;

use kiln_abi::services::kiln_launch::*;
use kiln_abi::services::kiln_tasks::*;
use kiln_abi::*;
use kiln_sfi::worker::WorkerPhase;
use serde_json::json;
use std::time::Duration;

/// A liveness bound, not a latency claim: how long a wait for something that must happen holds before the test
/// reports a hang. A passing wait returns as soon as its condition holds, so this sits far above any delay that
/// scheduling alone can cause on a loaded machine (300 s, as worker.rs and the lifecycle bodies).
const WAIT: Duration = Duration::from_secs(300);
use support::*;

#[test]
fn the_exported_boundary_starts_the_production_worker_under_kiln() {
    set_launch(Some((KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN)));
    set_state_dir(fresh_dir("exported"));
    let game = game_dir();
    let dir = game.join("mods").join("a");
    std::fs::create_dir_all(dir.join("src")).unwrap();
    std::fs::write(
        dir.join("mod.json"),
        r#"{"id":"alpha","name":"alpha","version":"1.0.0","capabilities":["runtime"]}"#,
    )
    .unwrap();
    std::fs::write(
        dir.join("src").join("mod.lua"),
        "shroudforge.log.info('alpha:exported')",
    )
    .unwrap();
    let content: String = sfi_policy::digest::package_content_sha256(&dir)
        .unwrap()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    let mut plan = plan_document("client");
    plan["packages"] = json!([{"id":"alpha","version":"1.0.0","kind":"shroudforge","root":"mods/a",
      "content_sha256":content,"config_sha256":"d".repeat(64),"enabled":true,"targets":["client"],
      "capabilities":["runtime"],"dependencies":[],"requiredApis":[],"services":[],"requires180":false,
      "saveSensitive":false,"decision":"candidate","reasons":[],
      "data":{"required":false,"admission":"not-needed"},"lua":{"required":true,"admission":"admit"},"dlls":[]}]);
    plan["loadOrder"] = json!(["alpha"]);
    set_plan(Some(serde_json::to_vec(&plan).unwrap()));
    let (early, normal) = (next_owner(), next_owner());
    let pool = FakePool::new(normal, PoolConfig::default());
    let api = Box::leak(api(true));
    #[cfg(not(windows))]
    assert!(kiln_sfi::startup_seam::register_host_export(
        fake_startup::getter
    ));
    fake_archive::wrap(api);
    fake_startup::install(fake_startup::Config::granting_current());
    let (mut e, mut n) = (plugin_info(), plugin_info());
    assert_eq!(
        unsafe { kiln_sfi::kiln_plugin_early(&*api, early, &mut e) },
        KILN_OK
    );
    assert_eq!(
        unsafe { kiln_sfi::kiln_plugin_main(&*api, normal, &mut n) },
        KILN_OK
    );
    // what kiln-eml's archive serves: the generation its startup outcome granted
    fake_startup::grant_archive(normal);
    let exported = kiln_sfi::exported();
    assert!(
        exported.worker().is_some(),
        "the exported boundary has the production worker"
    );
    assert_eq!(
        n.user,
        (exported as *const kiln_sfi::Boundary).cast_mut().cast()
    );
    unsafe { n.on_ready.expect("kiln installs on_ready")(n.user) };
    assert!(exported.worker_started());
    let submissions = pool.submissions();
    assert_eq!(submissions.len(), 1, "{submissions:?}");
    assert_eq!(
        (submissions[0].owner, submissions[0].name.as_str()),
        (normal, "interpreter")
    );
    assert_eq!(
        submissions[0].flags,
        (KILN_TASK_LONG_RUNNING | KILN_TASK_RESIDENT) as u32
    );
    let worker = exported.worker().unwrap();
    let shared = worker.shared().unwrap().clone();
    // on Windows this process has no kiln-eml.dll: no seam, no grant, and the runtime-create is refused before any Lua
    if cfg!(windows) {
        assert!(eventually(WAIT, || matches!(
            shared.phase(),
            WorkerPhase::Stopping | WorkerPhase::Stopped
        )));
        assert!(
            logs(normal)
                .iter()
                .all(|(_, l)| l != "[alpha] alpha:exported")
        );
        assert!(pool.task(0).wait_finished(WAIT));
        return;
    }
    assert!(eventually(WAIT, || shared.phase() == WorkerPhase::Running));
    let ran = || {
        logs(normal)
            .iter()
            .any(|(_, l)| l == "[alpha] alpha:exported")
    };
    // the early phase attached to kiln-eml's seam and Kiln's plugin step read its outcome once
    let seen = fake_startup::seen().expect("the exported boundary attached");
    assert_eq!(seen.count("attach"), 1);
    assert!(exported.startup_outcome().unwrap().grant.is_some());
    assert!(eventually(WAIT, ran), "{:?}", logs(normal));
    assert_eq!(exported.plan_reads(), 1);
    shared.request_stop();
    assert!(pool.task(0).wait_finished(WAIT));
}
