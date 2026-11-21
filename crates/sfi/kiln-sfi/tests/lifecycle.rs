// SPDX-License-Identifier: MIT
//! The lifecycle host tests. The bodies live in shared/lifecycle.rs so
//! another test package can run the same functions.
#[path = "shared/lifecycle.rs"]
mod bodies;

#[test]
fn mod_log_honours_shroudforge_s_info_default() {
    bodies::mod_log_honours_shroudforge_s_info_default()
}

#[test]
fn a_busy_lua_loop_is_interrupted_on_stop_within_one_hook_interval() {
    bodies::a_busy_lua_loop_is_interrupted_on_stop_within_one_hook_interval()
}

#[test]
fn a_pcall_loop_is_re_interrupted() {
    bodies::a_pcall_loop_is_re_interrupted()
}

#[test]
fn a_coroutine_cannot_evade_the_hook() {
    bodies::a_coroutine_cannot_evade_the_hook()
}

#[test]
fn a_stop_during_runtime_create_interrupts_the_top_level() {
    bodies::a_stop_during_runtime_create_interrupts_the_top_level()
}

#[test]
fn a_call_over_10_s_of_lua_time_stops_that_mod_only() {
    bodies::a_call_over_10_s_of_lua_time_stops_that_mod_only()
}

#[test]
fn a_call_just_under_10_s_of_lua_time_is_allowed() {
    bodies::a_call_just_under_10_s_of_lua_time_is_allowed()
}

#[test]
fn a_caught_watchdog_error_is_raised_again() {
    bodies::a_caught_watchdog_error_is_raised_again()
}

#[test]
fn a_coroutine_does_not_hide_a_call_from_the_watchdog() {
    bodies::a_coroutine_does_not_hide_a_call_from_the_watchdog()
}

#[test]
fn native_waits_and_hop_waits_do_not_count_but_ordinary_calls_do() {
    bodies::native_waits_and_hop_waits_do_not_count_but_ordinary_calls_do()
}

#[test]
fn each_call_has_its_own_account() {
    bodies::each_call_has_its_own_account()
}

#[test]
fn a_module_top_level_over_10_s_fails_that_module_and_create_goes_on() {
    bodies::a_module_top_level_over_10_s_fails_that_module_and_create_goes_on()
}

#[test]
fn a_create_longer_than_15_s_logs_sf_s_line_once() {
    bodies::a_create_longer_than_15_s_logs_sf_s_line_once()
}

#[test]
fn an_orderly_stop_runs_sf_s_stop_and_closes_the_state() {
    bodies::an_orderly_stop_runs_sf_s_stop_and_closes_the_state()
}

#[test]
fn a_fast_stop_runs_no_mod_code() {
    bodies::a_fast_stop_runs_no_mod_code()
}

#[test]
fn at_exit_the_worker_returns_within_100_ms_even_inside_lua() {
    bodies::at_exit_the_worker_returns_within_100_ms_even_inside_lua()
}

#[test]
fn a_quarantine_sets_faulted_and_keeps_the_subscriptions_alive() {
    bodies::a_quarantine_sets_faulted_and_keeps_the_subscriptions_alive()
}

#[test]
fn an_interpreter_stop_leaves_in_order_then_drains_the_owner_from_the_tick() {
    bodies::an_interpreter_stop_leaves_in_order_then_drains_the_owner_from_the_tick()
}

#[test]
fn a_hook_stop_is_found_through_mlua_s_error_chain_and_is_no_mod_failure() {
    bodies::a_hook_stop_is_found_through_mlua_s_error_chain_and_is_no_mod_failure()
}

#[test]
fn pushes_are_refused_from_the_stop_request_and_the_stopping_phase_on() {
    bodies::pushes_are_refused_from_the_stop_request_and_the_stopping_phase_on()
}

#[test]
fn a_stopping_for_a_session_never_seen_starting_is_accepted() {
    bodies::a_stopping_for_a_session_never_seen_starting_is_accepted()
}

#[test]
fn a_mod_that_catches_the_watchdog_error_and_returns_is_still_stopped() {
    bodies::a_mod_that_catches_the_watchdog_error_and_returns_is_still_stopped()
}

#[test]
fn a_call_that_overran_inside_a_native_call_is_stopped_when_it_returns() {
    bodies::a_call_that_overran_inside_a_native_call_is_stopped_when_it_returns()
}

#[test]
fn a_module_top_level_that_catches_the_watchdog_error_is_not_activated() {
    bodies::a_module_top_level_that_catches_the_watchdog_error_is_not_activated()
}

#[test]
fn a_cancel_during_the_orderly_leave_closes_no_state() {
    bodies::a_cancel_during_the_orderly_leave_closes_no_state()
}

#[test]
fn a_stop_and_a_cancel_together_leave_fast() {
    bodies::a_stop_and_a_cancel_together_leave_fast()
}

#[test]
fn a_mod_that_catches_the_stop_and_raises_its_own_error_is_no_mod_failure() {
    bodies::a_mod_that_catches_the_stop_and_raises_its_own_error_is_no_mod_failure()
}

#[test]
fn a_pcall_loop_that_keeps_re_entering_is_interrupted_at_every_entry() {
    bodies::a_pcall_loop_that_keeps_re_entering_is_interrupted_at_every_entry()
}

#[test]
fn a_gc_finalizer_runs_without_the_count_hook() {
    bodies::a_gc_finalizer_runs_without_the_count_hook()
}

#[test]
fn the_tick_polls_the_drain_until_the_owner_is_quiet() {
    bodies::the_tick_polls_the_drain_until_the_owner_is_quiet()
}
