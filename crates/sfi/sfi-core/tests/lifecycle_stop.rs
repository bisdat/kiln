// SPDX-License-Identifier: MIT
// the lifecycle's restore when the host stops during a reload.
// Behaviour checked against ShroudForge (MIT, Copyright (c) 2026 bonsaibauer), both pins:
// modules/mod-runtime/ingame/reload_runtime_packages.rs:6-95, restore_runtime_packages.rs:4-22,
// unload_runtime_packages.rs:5-25.
//! A host stop (the worker port's `interrupted`) that arrives while a reload or an unload has already unloaded
//! packages: the restore runs no on_load, records no error and leaves those packages inactive, so the host's orderly
//! leave (`stop(Normal)`) never runs a package's on_unload twice without an on_load between. A package whose
//! restored on_load did start is owed its on_unload once.
use serde_json::json;
use sfi_core::{
    bindings::{self, OwnerBindings},
    host::{HostContext, Package as RuntimePackage, PackageSource, Phase, Side, SourceVersion},
    lifecycle::{
        self, CallbackPort, Cleanup, LifecycleRuntime, Monotonic, OwnerRegistration, OwnerState,
        RuntimeSetup, StopPhase,
    },
    locales::LocaleSource,
    lua::LuaVm,
    package::{
        self,
        discover::{PackageRecord, Registry},
    },
};
use std::{
    cell::{Cell, RefCell},
    collections::BTreeMap,
    io,
    rc::Rc,
    time::Duration,
};

struct Clock;
impl Monotonic for Clock {
    fn now(&self) -> Duration {
        Duration::ZERO
    }
}
impl sfi_core::host::Clock for Clock {
    fn seconds(&self) -> f64 {
        0.0
    }
}
struct Log;
impl sfi_core::host::LogSink for Log {
    fn enabled(&self, _: sfi_core::host::LogLevel) -> bool {
        true
    }
    fn write(&self, _: &str, _: sfi_core::host::LogLevel, _: &[u8]) {}
}
struct Source(String);
impl PackageSource for Source {
    fn read_file(&self, path: &str) -> io::Result<Vec<u8>> {
        if path == "src/mod.lua" {
            Ok(self.0.as_bytes().to_vec())
        } else {
            Err(io::ErrorKind::NotFound.into())
        }
    }
}
struct Locale;
impl LocaleSource for Locale {
    fn locale(&self) -> Option<String> {
        None
    }
}
/// The cleanup port: records each restore.
struct Ports(Rc<RefCell<Vec<String>>>);
impl Cleanup for Ports {
    fn restore(&self, owner: &str) -> Result<(), String> {
        self.0.borrow_mut().push(format!("cleanup:{owner}"));
        Ok(())
    }
    fn release_input_reservations(&self, _: &str) {}
}
/// The worker's port: `stop` is the host's stop in force (cleared for the orderly leave, as kiln-sfi's port
/// ignores its own stop while leaving).
#[derive(Default)]
struct Port {
    stop: Cell<bool>,
}
impl CallbackPort for Port {
    fn enter(&self, _: &str) {}
    fn exit(&self) -> Option<String> {
        None
    }
    fn interrupted(&self) -> bool {
        self.stop.get()
    }
}

struct Run {
    runtime: LifecycleRuntime,
    port: Rc<Port>,
    events: Rc<RefCell<Vec<String>>>,
}
impl Run {
    fn count(&self, text: &str) -> usize {
        self.events.borrow().iter().filter(|e| *e == text).count()
    }
    /// The host's orderly leave: its own stop no longer interrupts.
    fn leave(&mut self) {
        self.port.stop.set(false);
        let report = self.runtime.stop(StopPhase::Normal);
        assert!(report.failures.is_empty(), "{:?}", report.failures);
    }
}

/// Packages `(id, lua)` in plan order, every one admitted, on a VM where `record(text)` notes `<id>:<text>` and
/// `host_stop()` raises the host's stop.
fn start(packages: &[(&str, &str)]) -> Run {
    let events = Rc::new(RefCell::new(Vec::new()));
    let port = Rc::new(Port::default());
    let mut registry = Registry::default();
    for (id, text) in packages {
        let source: Rc<dyn PackageSource> = Rc::new(Source((*text).into()));
        let mut parsed = package::Package::parse(
            json!({"id": id, "name": id, "version": "1.0.0", "capabilities": ["runtime"]})
                .to_string()
                .as_bytes(),
            None,
            package::ParseOptions::default(),
        )
        .unwrap();
        parsed.manifest.enabled = true;
        registry
            .insert(
                PackageRecord::new(
                    (*id).into(),
                    parsed,
                    RuntimePackage::new(*id, source.clone()),
                    source,
                )
                .unwrap(),
            )
            .unwrap();
    }
    let mut context = HostContext::new(Phase::Runtime, Side::Server, SourceVersion::Sf179);
    context.clock = Some(Rc::new(Clock));
    context.log = Some(Rc::new(Log));
    let vm = LuaVm::new(context).unwrap();
    let mut owners = BTreeMap::new();
    for (id, _) in packages {
        let record = registry.get(id).unwrap();
        let mut settings = None;
        vm.add_package(record.runtime().clone(), |lua, env, package| {
            settings = Some(
                bindings::install(
                    lua,
                    env,
                    package,
                    vm.context(),
                    OwnerBindings {
                        manifest: record.parsed().manifest.clone(),
                        locale: Rc::new(Locale),
                        loader: lifecycle::loader_snapshot(&registry.snapshot(), false, false),
                    },
                )?
                .settings,
            );
            let (events, owner) = (events.clone(), id.to_string());
            env.set(
                "record",
                lua.create_function(move |_, text: String| {
                    events.borrow_mut().push(format!("{owner}:{text}"));
                    Ok(())
                })?,
            )?;
            let port = port.clone();
            env.set(
                "host_stop",
                lua.create_function(move |_, ()| {
                    port.stop.set(true);
                    Ok(())
                })?,
            )?;
            Ok(())
        })
        .unwrap();
        owners.insert(
            id.to_string(),
            OwnerRegistration {
                state: Rc::new(OwnerState::default()),
                settings: settings.unwrap(),
            },
        );
    }
    let order: Vec<String> = packages.iter().map(|(id, _)| id.to_string()).collect();
    let runtime = LifecycleRuntime::start_with_port(
        vm,
        &registry,
        RuntimeSetup {
            plan: order.clone(),
            admitted: order.into_iter().collect(),
            owners,
            clock: Rc::new(Clock),
            cleanup: Rc::new(Ports(events.clone())),
        },
        Some(port.clone() as Rc<dyn CallbackPort>),
    )
    .unwrap();
    Run {
        runtime,
        port,
        events,
    }
}

const RECORDER: &str = "return { on_load = function() record('load') end, \
                                 on_unload = function() record('unload') end }";

/// The host stops while a reload builds its new VM (after every package was unloaded): the restore runs no on_load
/// and records no error, and the orderly leave runs no on_unload again.
#[test]
fn a_stop_during_the_rebuild_restores_nothing_and_unloads_nothing_twice() {
    let mut run = start(&[("a", RECORDER), ("b", RECORDER)]);
    let port = run.port.clone();
    let refused = run.runtime.reload(&[], || {
        port.stop.set(true);
        Err("interpreter is stopping".to_owned())
    });
    assert_eq!(refused, Err("interpreter is stopping".to_owned()));
    assert_eq!(
        *run.events.borrow(),
        [
            "a:load",
            "b:load",
            "b:unload",
            "cleanup:b",
            "a:unload",
            "cleanup:a"
        ]
    );
    assert!(
        run.runtime.errors().is_empty(),
        "{:?}",
        run.runtime.errors()
    );
    assert!(run.runtime.active_ids().is_empty());
    assert!(run.runtime.quiescent());
    run.leave();
    assert_eq!((run.count("a:unload"), run.count("b:unload")), (1, 1));
    assert_eq!((run.count("a:load"), run.count("b:load")), (1, 1));
}

/// The host stops inside a restored on_load: that package's on_load started, so the orderly leave owes it one
/// on_unload; the packages after it are not restored and are not unloaded again.
#[test]
fn a_stop_during_a_restored_on_load_owes_that_package_one_on_unload() {
    let mut run = start(&[
        (
            "a",
            "local loads = 0 return { on_load = function() loads = loads + 1 record('load') \
             if loads > 1 then host_stop() end end, on_unload = function() record('unload') end }",
        ),
        ("b", RECORDER),
    ]);
    let refused = run.runtime.reload(&[], || {
        Err("updated mod package discovery failed: x".to_owned())
    });
    assert!(refused.is_err());
    assert_eq!((run.count("a:load"), run.count("b:load")), (2, 1));
    assert!(
        run.runtime.errors().is_empty(),
        "{:?}",
        run.runtime.errors()
    );
    run.leave();
    assert_eq!((run.count("a:unload"), run.count("b:unload")), (2, 1));
    assert!(run.runtime.quiescent());
}

/// The host stops during an unload's on_unload: the packages already unloaded are not restored and not unloaded
/// again by the orderly leave; the interrupted one is the host's to finish (its on_unload runs in the leave).
#[test]
fn a_stop_during_an_unload_restores_nothing_and_unloads_nothing_twice() {
    let mut run = start(&[
        (
            "a",
            "local stopped = false return { on_load = function() record('load') end, \
             on_unload = function() record('unload') if not stopped then stopped = true host_stop() end end }",
        ),
        ("b", RECORDER),
    ]);
    assert!(run.runtime.unload(&["a".into(), "b".into()]).is_err());
    assert_eq!((run.count("a:load"), run.count("b:load")), (1, 1));
    assert!(
        run.runtime.errors().is_empty(),
        "{:?}",
        run.runtime.errors()
    );
    run.leave();
    assert_eq!(run.count("b:unload"), 1, "{:?}", run.events.borrow());
    assert_eq!(run.count("a:unload"), 2, "{:?}", run.events.borrow());
    assert!(run.runtime.quiescent());
}
