// SPDX-License-Identifier: MIT
#[path = "shared/c2_package_editor.rs"]
mod assertions;

#[test]
fn all_shared_cases_match_views_decisions_and_exact_bytes() {
    assertions::all_shared_cases_match_views_decisions_and_exact_bytes();
}

#[test]
fn numeric_edits_refuse_distinct_large_options_and_out_of_range_values() {
    assertions::numeric_edits_refuse_distinct_large_options_and_out_of_range_values();
}

#[test]
fn successful_numeric_edits_preserve_equivalence_and_strict_parseability() {
    assertions::successful_numeric_edits_preserve_equivalence_and_strict_parseability();
}

#[test]
fn exact_2_mib_bound_and_refused_edit_do_not_mutate_input() {
    assertions::exact_2_mib_bound_and_refused_edit_do_not_mutate_input();
}
