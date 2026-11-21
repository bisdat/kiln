// SPDX-License-Identifier: MIT
#[path = "shared/c3h_ext.rs"]
mod assertions;

#[test]
fn virtual_file_and_ancestors() {
    assertions::virtual_file_and_ancestors();
}
#[test]
fn shadowing_and_nested_listing() {
    assertions::shadowing_and_nested_listing();
}
#[test]
fn root_empty_and_component_paths() {
    assertions::root_empty_and_component_paths();
}
#[test]
fn metadata_defaults_and_error_suppression() {
    assertions::metadata_defaults_and_error_suppression();
}
#[test]
fn injected_listing_errors_and_independent_predicates() {
    assertions::injected_listing_errors_and_independent_predicates();
}
#[test]
fn disk_predicates_and_overlay_symlinks() {
    assertions::disk_predicates_and_overlay_symlinks();
}
