// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family tests.
//! The shared runtime.network bodies (tests/shared/runtime_network.rs), which other
//! test packages can run too.
#[path = "shared/runtime_network.rs"]
mod bodies;

#[test]
fn the_raw_fixture_matches_the_oracle_sf179() {
    bodies::the_raw_fixture_matches_the_oracle_sf179()
}

#[test]
fn the_raw_fixture_matches_the_oracle_sf180() {
    bodies::the_raw_fixture_matches_the_oracle_sf180()
}

#[test]
fn the_mod_message_fixture_matches_the_oracle_sf179() {
    bodies::the_mod_message_fixture_matches_the_oracle_sf179()
}

#[test]
fn the_mod_message_fixture_matches_the_oracle_sf180() {
    bodies::the_mod_message_fixture_matches_the_oracle_sf180()
}

#[test]
fn a_mod_message_goes_from_one_package_to_another_sf179() {
    bodies::a_mod_message_goes_from_one_package_to_another_sf179()
}

#[test]
fn a_mod_message_goes_from_one_package_to_another_sf180() {
    bodies::a_mod_message_goes_from_one_package_to_another_sf180()
}

#[test]
fn the_production_family_installs_through_the_worker_sf179() {
    bodies::the_production_family_installs_through_the_worker_sf179()
}

#[test]
fn the_production_family_installs_through_the_worker_sf180() {
    bodies::the_production_family_installs_through_the_worker_sf180()
}
