// SPDX-License-Identifier: MIT
// the interpreter's runtime.ecs registry family tests.
//! The shared runtime.ecs registry bodies (tests/shared/runtime_ecs.rs), which
//! other test packages can run too.
#[path = "shared/runtime_ecs.rs"]
mod bodies;

#[test]
fn the_join_proves_each_row_by_hash_name_and_size_and_is_read_once_per_stamp() {
    bodies::the_join_proves_each_row_by_hash_name_and_size_and_is_read_once_per_stamp()
}

#[test]
fn a_registration_entry_has_shroudforge_s_fields_with_unsigned_hash_and_flag_bits() {
    bodies::a_registration_entry_has_shroudforge_s_fields_with_unsigned_hash_and_flag_bits()
}

#[test]
fn a_size_hash_or_name_mismatch_refuses_the_report_and_resolves_only_proven_rows() {
    bodies::a_size_hash_or_name_mismatch_refuses_the_report_and_resolves_only_proven_rows()
}

#[test]
fn a_registered_owner_type_is_not_its_stored_dynamic_type() {
    bodies::a_registered_owner_type_is_not_its_stored_dynamic_type()
}

#[test]
fn part_a_has_no_template_role_and_a_type_without_a_column_resolves_nothing() {
    bodies::part_a_has_no_template_role_and_a_type_without_a_column_resolves_nothing()
}

#[test]
fn a_missing_selector_and_an_ambiguous_one_answer_shroudforge_s_reasons() {
    bodies::a_missing_selector_and_an_ambiguous_one_answer_shroudforge_s_reasons()
}

#[test]
fn get_component_and_get_registry_give_the_provider_s_reason_when_there_is_no_registry() {
    bodies::get_component_and_get_registry_give_the_provider_s_reason_when_there_is_no_registry()
}

#[test]
fn every_catalog_field_on_its_own() {
    bodies::every_catalog_field_on_its_own()
}

#[test]
fn the_catalog_s_read_and_write_facts_follow_readiness_the_write_context_and_the_denials() {
    bodies::the_catalog_s_read_and_write_facts_follow_readiness_the_write_context_and_the_denials()
}

#[test]
fn the_metadata_calls_answer_without_an_active_lifecycle_or_the_runtime_capability() {
    bodies::the_metadata_calls_answer_without_an_active_lifecycle_or_the_runtime_capability()
}

#[test]
fn get_components_lists_resolved_storage_types_once_ready() {
    bodies::get_components_lists_resolved_storage_types_once_ready()
}

#[test]
fn on_the_production_host_reads_and_writes_wait_for_their_slices_and_the_metadata_answers() {
    bodies::on_the_production_host_reads_and_writes_wait_for_their_slices_and_the_metadata_answers()
}

#[test]
fn the_family_claims_its_four_names_inside_the_ecs_container() {
    bodies::the_family_claims_its_four_names_inside_the_ecs_container()
}
