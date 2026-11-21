// SPDX-License-Identifier: MIT
//! The DLL Manager selection tests. The bodies live in shared/native_selection.rs so
//! another test package can run the same functions.
#[path = "shared/native_selection.rs"]
mod bodies;

#[test]
fn source_precedence_is_selection_then_profile_then_ini() {
    bodies::source_precedence_is_selection_then_profile_then_ini()
}

#[test]
fn plugin_ini_parses_as_shroudforge_reads_it() {
    bodies::plugin_ini_parses_as_shroudforge_reads_it()
}

#[test]
fn manager_approval_profile_and_manifest_switches_drop_dlls_without_withdrawal() {
    bodies::manager_approval_profile_and_manifest_switches_drop_dlls_without_withdrawal()
}

#[test]
fn targets_follow_the_selection_and_the_manifest() {
    bodies::targets_follow_the_selection_and_the_manifest()
}

#[test]
fn an_unestablished_selected_dll_withdraws_the_package_and_its_dependents() {
    bodies::an_unestablished_selected_dll_withdraws_the_package_and_its_dependents()
}

#[test]
fn exclusions_are_all_or_nothing_like_the_source() {
    bodies::exclusions_are_all_or_nothing_like_the_source()
}

#[test]
fn execution_plan_ranks_order_dlls_and_unplanned_mods_report_their_reason() {
    bodies::execution_plan_ranks_order_dlls_and_unplanned_mods_report_their_reason()
}

#[test]
fn one_record_per_file_and_a_conflicting_mode_withdraws() {
    bodies::one_record_per_file_and_a_conflicting_mode_withdraws()
}

#[test]
fn declared_register_dll_targets_are_established_or_withdraw_the_package() {
    bodies::declared_register_dll_targets_are_established_or_withdraw_the_package()
}

#[test]
fn the_sealed_table_reuses_established_targets_and_never_loads_a_new_one() {
    bodies::the_sealed_table_reuses_established_targets_and_never_loads_a_new_one()
}

#[test]
fn selection_is_pure_and_never_reads_the_filesystem() {
    bodies::selection_is_pure_and_never_reads_the_filesystem()
}

#[test]
fn native_policy_text_is_kilns_own_and_refuses_as_native_mods_do() {
    bodies::native_policy_text_is_kilns_own_and_refuses_as_native_mods_do()
}

#[test]
fn native_policy_always_selects_every_source_as_before() {
    bodies::native_policy_always_selects_every_source_as_before()
}

#[test]
fn native_policy_never_withdraws_every_dll_package_and_its_dependents_with_no_load() {
    bodies::native_policy_never_withdraws_every_dll_package_and_its_dependents_with_no_load()
}

#[test]
fn native_policy_names_itself_for_a_package_whose_dll_also_has_an_issue() {
    bodies::native_policy_names_itself_for_a_package_whose_dll_also_has_an_issue()
}

#[test]
fn native_policy_reports_every_spelling_of_a_refused_file() {
    bodies::native_policy_reports_every_spelling_of_a_refused_file()
}

#[test]
fn plugin_ini_contract() {
    bodies::plugin_ini_contract()
}
