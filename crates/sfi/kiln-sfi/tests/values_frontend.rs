// SPDX-License-Identifier: MIT
//! The typed-value frontend's own contract. The bodies live in shared/runtime_values.rs so
//! another test package can run the same functions.
#[path = "shared/runtime_values.rs"]
mod bodies;

#[test]
fn the_arena_hands_the_provider_a_complete_v1_reader() {
    bodies::the_arena_hands_the_provider_a_complete_v1_reader()
}

#[test]
fn a_refused_arena_or_a_short_table_is_kilns_error() {
    bodies::a_refused_arena_or_a_short_table_is_kilns_error()
}

#[test]
fn reader_requests_run_once_with_sources_reads_in_a_protected_call() {
    bodies::reader_requests_run_once_with_sources_reads_in_a_protected_call()
}

#[test]
fn temporaries_die_with_the_outermost_call_and_retained_tokens_with_their_release() {
    bodies::temporaries_die_with_the_outermost_call_and_retained_tokens_with_their_release()
}

#[test]
fn proxies_are_interned_weakly_and_close_their_lease_once() {
    bodies::proxies_are_interned_weakly_and_close_their_lease_once()
}

#[test]
fn a_finalizer_inside_a_reader_callback_only_closes_its_lease() {
    bodies::a_finalizer_inside_a_reader_callback_only_closes_its_lease()
}

#[test]
fn wake_signals_and_the_worker_collects_with_the_control() {
    bodies::wake_signals_and_the_worker_collects_with_the_control()
}

#[test]
fn wakes_from_many_threads_while_the_worker_calls() {
    bodies::wakes_from_many_threads_while_the_worker_calls()
}

#[test]
fn callbacks_off_the_worker_answer_wrong_thread() {
    bodies::callbacks_off_the_worker_answer_wrong_thread()
}

#[test]
fn the_arena_closes_and_drains_before_the_vm_and_the_worker_go() {
    bodies::the_arena_closes_and_drains_before_the_vm_and_the_worker_go()
}

#[test]
fn an_undrained_arena_keeps_its_reader() {
    bodies::an_undrained_arena_keeps_its_reader()
}

#[test]
fn an_arena_admitted_without_a_control_keeps_its_reader() {
    bodies::an_arena_admitted_without_a_control_keeps_its_reader()
}

#[test]
fn callbacks_contain_panics_and_never_unwind_into_the_provider() {
    bodies::callbacks_contain_panics_and_never_unwind_into_the_provider()
}

#[test]
fn a_proxy_that_cannot_be_made_never_takes_the_provisional() {
    bodies::a_proxy_that_cannot_be_made_never_takes_the_provisional()
}

#[test]
fn the_frozen_packets_parse_and_the_invalid_ones_are_refused() {
    bodies::the_frozen_packets_parse_and_the_invalid_ones_are_refused()
}

#[test]
fn error_packets_raise_the_errors_source_raises() {
    bodies::error_packets_raise_the_errors_source_raises()
}

#[test]
fn a_lua_level_reraises_the_original_error() {
    bodies::a_lua_level_reraises_the_original_error()
}

#[test]
fn sources_typeid_label_and_type_name_are_rendered_from_the_found_value() {
    bodies::sources_typeid_label_and_type_name_are_rendered_from_the_found_value()
}

#[test]
fn packets_copy_with_a_probe_and_one_exact_retry() {
    bodies::packets_copy_with_a_probe_and_one_exact_retry()
}

#[test]
fn refusals_and_unreadable_replies_are_kilns_errors() {
    bodies::refusals_and_unreadable_replies_are_kilns_errors()
}

#[test]
fn a_loaded_root_lives_as_long_as_its_resource_info() {
    bodies::a_loaded_root_lives_as_long_as_its_resource_info()
}

#[test]
fn shapes_and_the_table_port_follow_shroudforge() {
    bodies::shapes_and_the_table_port_follow_shroudforge()
}

#[test]
fn every_class_delegates_its_shroudforge_metamethods_to_the_provider() {
    bodies::every_class_delegates_its_shroudforge_metamethods_to_the_provider()
}
