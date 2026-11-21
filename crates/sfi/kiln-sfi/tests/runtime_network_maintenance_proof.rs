// SPDX-License-Identifier: MIT
// the interpreter's runtime.network family tests.
//! The shared bodies (tests/shared/runtime_network_maintenance.rs) as kiln-sfi's own tests; other
//! test packages can run the same functions.
#[path = "shared/runtime_network_maintenance.rs"]
mod bodies;

#[test]
fn the_server_log_scan_follows_shroudforge() {
    bodies::the_server_log_scan_follows_shroudforge()
}
#[test]
fn the_log_follower_matches_the_full_scan_as_the_log_grows() {
    bodies::the_log_follower_matches_the_full_scan_as_the_log_grows()
}
#[test]
fn the_log_follower_starts_over_when_the_log_is_replaced() {
    bodies::the_log_follower_starts_over_when_the_log_is_replaced()
}
#[test]
fn the_health_check_follows_shroudforge() {
    bodies::the_health_check_follows_shroudforge()
}
#[test]
fn the_discovery_mapping_follows_shroudforge() {
    bodies::the_discovery_mapping_follows_shroudforge()
}
#[test]
fn the_peers_fixture_matches_the_oracle_sf179() {
    bodies::the_peers_fixture_matches_the_oracle_sf179()
}
#[test]
fn the_peers_fixture_matches_the_oracle_sf180() {
    bodies::the_peers_fixture_matches_the_oracle_sf180()
}
