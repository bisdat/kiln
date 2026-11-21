// SPDX-License-Identifier: MIT
#[path = "shared/c3.rs"]
mod cases;
#[test]
#[cfg(unix)]
fn review_discovery_self_cycle() {
    cases::discovery_self_cycle();
}
#[test]
#[cfg(unix)]
fn review_discovery_ancestor_cycle() {
    cases::discovery_ancestor_cycle();
}
#[test]
#[cfg(unix)]
fn review_discovery_contained_alias() {
    cases::discovery_contained_alias();
}
#[test]
fn review_discovery_invalid_source_utf8() {
    cases::discovery_invalid_source_utf8();
}
#[test]
fn review_profile_locale_directory_quirk() {
    cases::profile_locale_directory_quirk();
}
#[test]
fn review_profile_source_predicates_in_public_traversal() {
    cases::profile_source_predicates_in_public_traversal();
}
#[test]
fn review_profile_directory_listing() {
    cases::profile_directory_listing();
}
#[test]
fn review_profile_source_inference_directory_quirk() {
    cases::profile_source_inference_directory_quirk();
}
#[test]
fn discovery_isolation_and_identity() {
    cases::discovery_isolation_and_identity();
}
#[test]
fn discovery_inference_and_locales() {
    cases::discovery_inference_and_locales();
}
#[test]
fn dependency_matrix() {
    cases::dependency_matrix();
}
#[test]
fn dependency_targets_cycles_and_retention() {
    cases::dependency_targets_cycles_and_retention();
}
#[test]
fn runtime_candidates_and_membership() {
    cases::runtime_candidates_and_membership();
}
#[test]
fn profile_duplicate_fields_are_rejected() {
    cases::profile_duplicate_fields_are_rejected();
}
#[test]
fn injected_source_traversal_order_and_failures() {
    cases::injected_source_traversal_order_and_failures();
}
#[test]
fn ordered_dll_selections_are_not_maps() {
    cases::ordered_dll_selections_are_not_maps();
}
#[test]
fn profiles_validation() {
    cases::profiles_validation();
}
#[test]
fn profiles_selection_overlay_and_catalog() {
    cases::profiles_selection_overlay_and_catalog();
}
#[test]
fn compatibility_rules_and_errors() {
    cases::compatibility_rules_and_errors();
}
#[test]
fn revision_bytes_and_save_proposal() {
    cases::revision_bytes_and_save_proposal();
}
#[test]
fn manifest_revision_raw_and_ordered() {
    cases::manifest_revision_raw_and_ordered();
}
#[test]
fn preparation_bytes_and_version_difference() {
    cases::preparation_bytes_and_version_difference();
}

#[test]
fn compatibility_rules_match_shroudforge_goldens() {
    cases::compatibility_rules_match_shroudforge_goldens();
}
#[test]
fn compatibility_rules_file_matches_shroudforge_goldens() {
    cases::compatibility_rules_file_matches_shroudforge_goldens();
}
#[test]
fn compatibility_rules_file_loader_issue() {
    cases::compatibility_rules_file_loader_issue();
}
#[test]
fn compatibility_rules_file_never_reads_shroudforge_data() {
    cases::compatibility_rules_file_never_reads_shroudforge_data();
}
#[cfg(windows)]
#[test]
fn disk_source_refuses_a_link_out_of_the_package() {
    cases::disk_source_refuses_a_link_out_of_the_package();
}

#[test]
fn profiles_match_shroudforge_goldens() {
    cases::profiles_match_shroudforge_goldens();
}

#[test]
fn wrong_target_roots_are_skipped_silently() {
    cases::wrong_target_roots_are_skipped_silently();
}

#[test]
fn profile_target_applicability_and_unlistable_catalog() {
    cases::profile_target_applicability_and_unlistable_catalog();
}
