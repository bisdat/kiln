// SPDX-License-Identifier: MIT
//! The production runtime host through the lifecycle host. The bodies live in
//! shared/runtime_facts.rs so another test package can run the same functions.
#[path = "shared/runtime_facts.rs"]
mod bodies;

#[test]
fn the_production_runtime_host_answers_through_the_lifecycle_sf179() {
    bodies::the_production_runtime_host_answers_through_the_lifecycle_sf179()
}

#[test]
fn the_production_runtime_host_answers_through_the_lifecycle_sf180() {
    bodies::the_production_runtime_host_answers_through_the_lifecycle_sf180()
}

#[test]
fn the_catalog_is_both_pins_runtime_operations_in_order() {
    bodies::the_catalog_is_both_pins_runtime_operations_in_order()
}
