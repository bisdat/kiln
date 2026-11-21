// SPDX-License-Identifier: MIT
#[path = "shared/c2_package_diagnostics.rs"]
mod assertions;

#[test]
fn strict_unknown_fields_use_document_names_pluralization_and_declaration_order() {
    assertions::strict_unknown_fields_use_document_names_pluralization_and_declaration_order();
}

#[test]
fn nonobject_documents_name_the_correct_document() {
    assertions::nonobject_documents_name_the_correct_document();
}

#[test]
fn manifest_and_extension_schema_messages_match_pinned_shapes() {
    assertions::manifest_and_extension_schema_messages_match_pinned_shapes();
}

#[test]
fn nested_closed_record_diagnostics_follow_instance_order() {
    assertions::nested_closed_record_diagnostics_follow_instance_order();
}

#[test]
fn settings_schema_messages_keep_the_setting_key_prefix() {
    assertions::settings_schema_messages_keep_the_setting_key_prefix();
}

#[test]
fn locale_schema_messages_include_the_supplied_source_and_document_name() {
    assertions::locale_schema_messages_include_the_supplied_source_and_document_name();
}

#[test]
fn locale_errors_name_the_listed_path_with_the_platform_separator() {
    assertions::locale_errors_name_the_listed_path_with_the_platform_separator();
}

#[test]
fn sf_179_package_documents_match_shroudforge_reader_goldens() {
    assertions::sf_179_package_documents_match_shroudforge_reader_goldens();
}

#[test]
fn sf_180_package_documents_match_shroudforge_reader_goldens() {
    assertions::sf_180_package_documents_match_shroudforge_reader_goldens();
}
