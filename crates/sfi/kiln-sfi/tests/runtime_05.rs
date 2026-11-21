// SPDX-License-Identifier: MIT
//! game.types / runtime.types and the Type userdata. The bodies live in shared/runtime_types.rs so
//! another test package can run the same functions.
#[path = "shared/runtime_types.rs"]
mod bodies;

#[test]
fn the_loader_reads_the_header_then_whole_pages_and_keeps_both_versions() {
    bodies::the_loader_reads_the_header_then_whole_pages_and_keeps_both_versions()
}

#[test]
fn a_page_that_breaks_the_schema_or_the_registry_is_refused_whole() {
    bodies::a_page_that_breaks_the_schema_or_the_registry_is_refused_whole()
}

#[test]
fn a_registry_that_cannot_be_read_is_a_port_error_with_the_member_s_text() {
    bodies::a_registry_that_cannot_be_read_is_a_port_error_with_the_member_s_text()
}

#[test]
fn the_table_has_shroudforge_s_members_and_count_and_version_are_the_registry_s() {
    bodies::the_table_has_shroudforge_s_members_and_count_and_version_are_the_registry_s()
}

#[test]
fn get_is_string_only_and_coerces_numbers_to_their_text() {
    bodies::get_is_string_only_and_coerces_numbers_to_their_text()
}

#[test]
fn names_resolve_through_their_hash_index_and_must_match_exactly() {
    bodies::names_resolve_through_their_hash_index_and_must_match_exactly()
}

#[test]
fn every_hash_domain_refuses_a_collision_and_reports_a_missing_hash() {
    bodies::every_hash_domain_refuses_a_collision_and_reports_a_missing_hash()
}

#[test]
fn index_bounds_follow_shroudforge_s_unsigned_cast() {
    bodies::index_bounds_follow_shroudforge_s_unsigned_cast()
}

#[test]
fn get_all_includes_ds_types_unless_told_not_to() {
    bodies::get_all_includes_ds_types_unless_told_not_to()
}

#[test]
fn find_filters_on_prefix_attribute_primitive_and_ds() {
    bodies::find_filters_on_prefix_attribute_primitive_and_ds()
}

#[test]
fn invalid_utf8_names_are_conversion_errors() {
    bodies::invalid_utf8_names_are_conversion_errors()
}

#[test]
fn one_type_per_index_per_vm_shared_by_every_package_s_table() {
    bodies::one_type_per_index_per_vm_shared_by_every_package_s_table()
}

#[test]
fn a_finalizer_asking_for_the_type_being_made_keeps_one_type_per_index() {
    bodies::a_finalizer_asking_for_the_type_being_made_keeps_one_type_per_index()
}

#[test]
fn of_answers_through_registered_recognizers_only() {
    bodies::of_answers_through_registered_recognizers_only()
}

#[test]
fn of_answers_a_typed_value_with_its_type_through_the_provider() {
    bodies::of_answers_a_typed_value_with_its_type_through_the_provider()
}

#[test]
fn a_type_has_shroudforge_s_metadata_fields() {
    bodies::a_type_has_shroudforge_s_metadata_fields()
}

#[test]
fn enum_values_keep_their_high_bits() {
    bodies::enum_values_keep_their_high_bits()
}

#[test]
fn attributes_are_text_with_their_type_or_nil() {
    bodies::attributes_are_text_with_their_type_or_nil()
}

#[test]
fn fields_are_base_first_and_a_redeclared_field_keeps_the_base_s_slot() {
    bodies::fields_are_base_first_and_a_redeclared_field_keeps_the_base_s_slot()
}

#[test]
fn is_a_follows_real_inheritance_only() {
    bodies::is_a_follows_real_inheritance_only()
}

#[test]
fn cyclic_and_unresolved_bases_are_errors_at_use() {
    bodies::cyclic_and_unresolved_bases_are_errors_at_use()
}

#[test]
fn read_only_containers_index_count_and_walk_in_order() {
    bodies::read_only_containers_index_count_and_walk_in_order()
}

#[test]
fn an_iterator_keeps_its_own_position_and_refuses_another_state() {
    bodies::an_iterator_keeps_its_own_position_and_refuses_another_state()
}

#[test]
fn a_type_from_another_registry_resolves_only_to_the_same_type() {
    bodies::a_type_from_another_registry_resolves_only_to_the_same_type()
}

#[test]
fn a_type_outlives_its_session_as_metadata_only() {
    bodies::a_type_outlives_its_session_as_metadata_only()
}

#[test]
fn shroudforge_s_own_registry_cases_hold_through_lua() {
    bodies::shroudforge_s_own_registry_cases_hold_through_lua()
}

#[test]
fn the_cross_check_agrees_skips_or_refuses() {
    bodies::the_cross_check_agrees_skips_or_refuses()
}

#[test]
fn kiln_types_is_read_through_the_adapter_with_one_call_per_member() {
    bodies::kiln_types_is_read_through_the_adapter_with_one_call_per_member()
}

#[test]
fn the_wiring_gives_every_package_game_types_runtime_types_and_game_version() {
    bodies::the_wiring_gives_every_package_game_types_runtime_types_and_game_version()
}

#[test]
fn an_unreadable_kiln_types_skips_the_cross_check_with_one_info_line() {
    bodies::an_unreadable_kiln_types_skips_the_cross_check_with_one_info_line()
}

#[test]
fn a_refused_registry_stops_runtime_create_before_any_package_code() {
    bodies::a_refused_registry_stops_runtime_create_before_any_package_code()
}

#[test]
fn the_archive_port_acquires_with_the_grant_checks_identity_and_sizes_every_page() {
    bodies::the_archive_port_acquires_with_the_grant_checks_identity_and_sizes_every_page()
}

#[test]
fn an_identity_the_archive_did_not_grant_is_refused_and_a_lying_view_released() {
    bodies::an_identity_the_archive_did_not_grant_is_refused_and_a_lying_view_released()
}

#[test]
fn a_page_that_grows_after_its_sizing_call_is_refused() {
    bodies::a_page_that_grows_after_its_sizing_call_is_refused()
}

#[test]
fn a_failed_page_carries_the_provider_s_text_copied_and_released_exactly_once() {
    bodies::a_failed_page_carries_the_provider_s_text_copied_and_released_exactly_once()
}

#[test]
fn missing_rows_or_slots_are_unavailable_before_anything_is_acquired() {
    bodies::missing_rows_or_slots_are_unavailable_before_anything_is_acquired()
}

#[test]
fn a_page_beyond_page_max_or_of_zero_bytes_is_refused_before_it_is_read() {
    bodies::a_page_beyond_page_max_or_of_zero_bytes_is_refused_before_it_is_read()
}
