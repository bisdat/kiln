// SPDX-License-Identifier: MIT
//! The placement and patch refusals. The shapes and the lifecycle runs live in
//! shared/runtime_refusals.rs so another test package can run the same
//! functions; this file adds what needs the C-ABI fake services: the production host answers
//! every refusal without calling one Kiln member.
#[path = "support/fake_services/mod.rs"]
mod fake_services;
// fake_services builds on this copy; the bodies keep their own.
#[path = "shared/runtime_refusals.rs"]
mod bodies;
#[allow(clippy::duplicate_mod)]
mod support;

use kiln_abi::KilnApi;
use kiln_sfi::lifecycle::{Watchdog, process_clock};
use kiln_sfi::runtime::availability::{FactsConfig, KilnRuntimeHost, OwnerActivity, RuntimeFacts};
use kiln_sfi::runtime::operation_catalog::CATALOG;
use kiln_sfi::runtime::services::ServiceAdapter;
use kiln_sfi::runtime::{Families, Family, install::CONTAINERS};
use kiln_sfi::worker::Host;
use sfi_core::bindings::runtime::RuntimeHost;
use sfi_core::host::{HostContext, Phase, Side, SourceVersion};
use std::rc::Rc;

#[test]
fn placement_shapes_sf179() {
    bodies::placement_shapes_sf179()
}
#[test]
fn placement_shapes_sf180() {
    bodies::placement_shapes_sf180()
}
#[test]
fn patch_shapes_sf179() {
    bodies::patch_shapes_sf179()
}
#[test]
fn patch_shapes_sf180() {
    bodies::patch_shapes_sf180()
}
#[test]
fn the_families_answer_through_the_lifecycle_sf179() {
    bodies::the_families_answer_through_the_lifecycle_sf179()
}
#[test]
fn the_families_answer_through_the_lifecycle_sf180() {
    bodies::the_families_answer_through_the_lifecycle_sf180()
}
#[test]
fn current_profiles_have_no_world_operations_or_runtime_patches() {
    bodies::current_profiles_have_no_world_operations_or_runtime_patches()
}

struct Active;
impl OwnerActivity for Active {
    fn active(&self) -> bool {
        true
    }
}

/// The production host (catalog, kiln.caps, kiln.world through the real adapter on every
/// fake table) and the functions over it: every shape, allowed and denied, reads no member.
#[test]
fn the_production_host_refuses_without_calling_a_kiln_member() {
    let owner = bodies_owner();
    fake_services::bind_thread(owner);
    let api: &'static KilnApi = Box::leak(fake_services::api(true));
    let adapter = Rc::new(ServiceAdapter::new(
        unsafe { Host::read(api) },
        Rc::new(Watchdog::new(process_clock())),
    ));
    let facts = Rc::new(RuntimeFacts::new(
        adapter,
        &CATALOG,
        FactsConfig {
            global_patch: false,
            global_export: false,
            game_build: bodies::GAME_BUILD.into(),
        },
    ));
    for (capabilities, place, patch) in [
        (
            vec!["runtime"],
            bodies::PLACE_REJECTED.to_owned(),
            "build profile does not resolve runtime.patch.x".to_owned(),
        ),
        (
            vec![],
            "mod 'alpha' requires capabilities: [\"runtime\"]".to_owned(),
            "mod 'alpha' requires capabilities: [\"runtime\"]".to_owned(),
        ),
    ] {
        let manifest = sfi_core::package::Package::parse(
            serde_json::json!({"id": "alpha", "name": "alpha", "version": "1.0.0", "capabilities": capabilities})
                .to_string()
                .as_bytes(),
            None,
            sfi_core::package::ParseOptions::default(),
        )
        .unwrap()
        .manifest;
        let context = HostContext::new(Phase::Runtime, Side::Server, SourceVersion::Sf179);
        let host: Rc<dyn RuntimeHost> = Rc::new(KilnRuntimeHost::new(
            &manifest,
            &context,
            Rc::new(Active),
            facts.clone(),
        ));
        let h = bodies::harness(host, false);
        assert_eq!(
            h.ok("entity.place({1,2,3}, {0,0,0,1}, {0,0,0,1,1,1}, 7, 9)"),
            format!("2:false,{place}")
        );
        let finish = if capabilities.is_empty() {
            place.clone()
        } else {
            bodies::FINISH_REJECTED.to_owned()
        };
        assert_eq!(
            h.ok("entity.finish_building(true)"),
            format!("2:false,{finish}")
        );
        assert!(
            h.ok("entity.destroy({1,2,3}, {0,0,0,1}, {0,0,0,1,1,1}, 7, 9)")
                .starts_with("2:false,")
        );
        assert!(h.ok("entity.destroy(7)").starts_with("2:false,"));
        assert_eq!(h.ok("patch.available('runtime.patch.x')"), "1:false");
        assert_eq!(
            h.ok("patch.set_enabled('runtime.patch.x', true)"),
            format!("2:false,{patch}")
        );
        if !capabilities.is_empty() {
            h.err("entity.place()");
            h.err("patch.set_enabled()");
        }
    }
    assert_eq!(
        fake_services::counters(owner).total_calls(),
        0,
        "{:?}",
        fake_services::counters(owner)
    );
}

/// A fresh owner, apart from the ones the bodies' own support copy hands out.
fn bodies_owner() -> kiln_abi::KilnOwner {
    support::next_owner() + 1_000_000
}

/// The families claim names inside the installer's containers (never a container) and sit
/// beside each other and the types family.
#[test]
fn the_families_claim_only_their_names() {
    let families: Vec<Family> = bodies::families();
    assert_eq!(
        families[0].claims(),
        kiln_sfi::runtime::world::placement::CLAIMS
    );
    assert_eq!(families[1].claims(), kiln_sfi::runtime::patch::CLAIMS);
    for family in &families {
        for claim in family.claims() {
            assert!(!CONTAINERS.contains(claim), "{claim}");
        }
    }
    assert!(Families::new(families).is_ok());
    // A second owner of runtime.patch, or of one of the entity names, is refused.
    let twice = Families::new(vec![
        kiln_sfi::runtime::patch::family(),
        Family::new(
            "other",
            &["runtime.patch.available"],
            |_, _, _, _, _| Ok(()),
        ),
    ]);
    assert!(twice.err().unwrap().contains("already owned by patch"));
    let entity = Families::new(vec![
        kiln_sfi::runtime::world::placement::family(None),
        Family::new("r26", &["runtime.world.entity.destroy"], |_, _, _, _, _| {
            Ok(())
        }),
    ]);
    assert!(
        entity
            .err()
            .unwrap()
            .contains("already owned by world.placement")
    );
}
