// SPDX-License-Identifier: MIT
// the interpreter's live reload/unload transport.
//! The live reload/unload transport. The bodies live in shared/reload.rs so another test package
//! can run the same functions.
#[path = "shared/reload.rs"]
mod bodies;

#[test]
fn the_result_file_holds_shroudforge_s_documents() {
    bodies::the_result_file_holds_shroudforge_s_documents()
}

#[test]
fn requests_that_cannot_be_read_are_ignored_silently() {
    bodies::requests_that_cannot_be_read_are_ignored_silently()
}

#[test]
fn a_reload_runs_the_lua_again_and_dedupes_on_the_request_id() {
    bodies::a_reload_runs_the_lua_again_and_dedupes_on_the_request_id()
}

#[test]
fn an_unload_runs_on_unload_in_reverse_and_keeps_the_others() {
    bodies::an_unload_runs_on_unload_in_reverse_and_keeps_the_others()
}

#[test]
fn a_package_not_admitted_at_the_start_is_refused_and_the_runtime_restored() {
    bodies::a_package_not_admitted_at_the_start_is_refused_and_the_runtime_restored()
}

#[test]
fn a_changed_package_is_refused_with_the_rebuild_s_reason() {
    bodies::a_changed_package_is_refused_with_the_rebuild_s_reason()
}

#[test]
fn a_restart_only_package_refuses_a_reload_but_not_an_unload() {
    bodies::a_restart_only_package_refuses_a_reload_but_not_an_unload()
}

#[test]
fn a_cancel_during_the_rebuild_leaves_fast_with_no_mod_code() {
    bodies::a_cancel_during_the_rebuild_leaves_fast_with_no_mod_code()
}

#[test]
fn a_stop_during_the_rebuild_leaves_in_order_and_unloads_nothing_twice() {
    bodies::a_stop_during_the_rebuild_leaves_in_order_and_unloads_nothing_twice()
}

#[test]
fn a_cancel_during_the_abandoned_vm_s_leave_closes_no_vm() {
    bodies::a_cancel_during_the_abandoned_vm_s_leave_closes_no_vm()
}

#[test]
fn requests_during_a_long_turn_coalesce_to_the_latest() {
    bodies::requests_during_a_long_turn_coalesce_to_the_latest()
}

#[test]
fn sfi_json_is_updated_after_a_reload_and_a_restore() {
    bodies::sfi_json_is_updated_after_a_reload_and_a_restore()
}

#[test]
fn unload_and_a_failed_restore_clean_up_through_the_cleanup_port() {
    bodies::unload_and_a_failed_restore_clean_up_through_the_cleanup_port()
}

#[test]
fn rebuild_failures_answer_shroudforge_s_texts_and_restore() {
    bodies::rebuild_failures_answer_shroudforge_s_texts_and_restore()
}

#[test]
fn a_reloaded_vm_starts_with_runtime_facts_of_its_own() {
    bodies::a_reloaded_vm_starts_with_runtime_facts_of_its_own()
}

#[test]
fn a_reloaded_runtime_logs_its_mod_errors_again() {
    bodies::a_reloaded_runtime_logs_its_mod_errors_again()
}

#[test]
fn the_result_replaces_the_file_atomically() {
    bodies::the_result_replaces_the_file_atomically()
}

// Each VM's typed values (kiln.eml.values)

#[test]
fn a_vm_s_arena_is_made_before_its_packages_collected_at_the_turn_end_and_drained_at_the_stop() {
    bodies::a_vm_s_arena_is_made_before_its_packages_collected_at_the_turn_end_and_drained_at_the_stop()
}

#[test]
fn a_reload_s_new_vm_gets_a_fresh_arena_and_the_old_one_drains_before_its_vm_goes() {
    bodies::a_reload_s_new_vm_gets_a_fresh_arena_and_the_old_one_drains_before_its_vm_goes()
}

#[test]
fn a_refused_reload_makes_no_arena_and_a_failed_one_drains_the_new_vm_s() {
    bodies::a_refused_reload_makes_no_arena_and_a_failed_one_drains_the_new_vm_s()
}

#[test]
fn a_stop_during_the_rebuild_drains_the_abandoned_vm_s_arena_after_its_on_unload() {
    bodies::a_stop_during_the_rebuild_drains_the_abandoned_vm_s_arena_after_its_on_unload()
}

#[test]
fn a_fast_leave_closes_no_arena_and_releases_no_view() {
    bodies::a_fast_leave_closes_no_arena_and_releases_no_view()
}

#[test]
fn an_interpreter_stop_drains_the_arena_after_the_on_unloads() {
    bodies::an_interpreter_stop_drains_the_arena_after_the_on_unloads()
}

#[test]
fn a_vm_whose_typed_values_cannot_be_set_up_fails_its_setup() {
    bodies::a_vm_whose_typed_values_cannot_be_set_up_fails_its_setup()
}

#[test]
fn an_arena_that_does_not_drain_is_logged_and_keeps_its_view() {
    bodies::an_arena_that_does_not_drain_is_logged_and_keeps_its_view()
}

#[test]
fn a_withheld_archive_row_refuses_the_arena_s_view_before_it_is_acquired() {
    bodies::a_withheld_archive_row_refuses_the_arena_s_view_before_it_is_acquired()
}

#[test]
fn an_arena_admitted_without_a_control_keeps_its_view() {
    bodies::an_arena_admitted_without_a_control_keeps_its_view()
}

#[test]
fn the_watchdog_stops_a_call_that_overran_inside_a_reader_callback_whose_error_was_swallowed() {
    bodies::the_watchdog_stops_a_call_that_overran_inside_a_reader_callback_whose_error_was_swallowed()
}
