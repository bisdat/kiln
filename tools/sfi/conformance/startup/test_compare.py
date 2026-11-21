#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Synthetic tool-contract tests, not SF observations or implementation goldens."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('startup_compare', HERE / 'compare.py')
compare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(compare)


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':'),
                                     ensure_ascii=True, allow_nan=False).encode('ascii')).hexdigest()


def packet():
    return json.loads((HERE / 'fixtures/synthetic.json').read_text())


def match(expected, actual, **options):
    options.setdefault('fixture_path', HERE / 'fixtures/synthetic.input')
    return compare.compare_packets(expected, actual, **options)


def record(value, kind, index=0):
    return [r for r in value['records'] if r['kind'] == kind][index]


def resequence(value):
    for seq, rec in enumerate(value['records'], 1):
        rec['seq'] = seq


def rename_package(value, old, new):
    for participant in value['packages']:
        if participant['id'] == old:
            participant['id'] = new
    for rec in value['records']:
        for key in ('package', 'moduleOwner'):
            if rec.get(key) == old:
                rec[key] = new
    archive = record(value, 'archive_result')
    archive['included'] = [new if name == old else name for name in archive['included']]
    for exclusion in archive['excluded']:
        if exclusion['id'] == old:
            exclusion['id'] = new


def deferred_packet():
    value = packet()
    stage = record(value, 'asset_stage')
    effect = record(value, 'effect', 1)
    value['records'].remove(stage)
    value['records'].remove(effect)
    deferred = []
    for index, owner in enumerate(('alpha', 'beta'), 1):
        staged, written = copy.deepcopy(stage), copy.deepcopy(effect)
        staged.update(phase='deferred-stage', package=owner, key=f'resource:synthetic-{index}')
        written.update(phase='deferred-stage', package=owner, effect=f'effect:{index+1}',
                       target=staged['key'])
        deferred.extend((staged, written))
    publication = record(value, 'publication_step')
    offset = value['records'].index(publication)
    value['records'][offset:offset] = deferred
    resequence(value)
    return value


def retry_packet(attempts):
    value = packet()
    value['packages'] = [value['packages'][1]]
    execution = [rec for rec in value['records']
                 if rec.get('package') == 'beta' and rec['kind'] != 'admission']
    records = [record(value, 'admission', 1)]
    for attempt, outcome in attempts:
        run = copy.deepcopy(execution)
        for rec in run:
            rec['attempt'] = attempt
            if rec['kind'] == 'package_end':
                rec['outcome'] = outcome
            elif rec['kind'] == 'lua_transcript' and outcome == 'failed':
                rec['transcript']['fixture_error'] = {'t': 'nil'}
        records.extend(run)
    archive = record(value, 'archive_result')
    archive['included'] = ['beta']
    # Host bookkeeping has independent attempt numbering, not execution chronology.
    records.extend((record(value, 'publication_step'), archive))
    value['records'] = records
    resequence(value)
    return value


class Comparator(unittest.TestCase):
    def changed(self, mutate, **options):
        a = packet()
        b = copy.deepcopy(a)
        mutate(b)
        compare.validate_packet(b)
        with self.assertRaises(compare.TranscriptError):
            match(a, b, **options)

    def test_positive_strict_v2_and_input_not_modified(self):
        a = packet()
        before = copy.deepcopy(a)
        result = match(a, a)
        self.assertEqual(result['v2_message_verified'], 3)
        self.assertEqual(result['legacy_v1_not_message_verified'], 0)
        self.assertEqual(result['physical_mode'], 'strict')
        self.assertEqual(a, before)

    def test_identical_userdata_names_do_not_hide_error_message_changes(self):
        self.changed(lambda p: record(p, 'api_error')['transcript']['records'][0].update(message='different'))

    def test_embedded_v2_error_message_is_strict(self):
        self.changed(lambda p: record(p, 'lua_transcript')['transcript']['records'][0].update(message='different'))

    def test_nil_is_not_no_return(self):
        self.changed(lambda p: record(p, 'api_result').update(values=[]))

    def test_signed_integer_bits_are_not_coerced(self):
        self.changed(lambda p: record(p, 'api_call')['args'][0].update(signed=False, value='18446744073709551615'))

    def test_float_negative_zero_is_not_positive_zero(self):
        self.changed(lambda p: record(p, 'api_call')['args'][1].update(bits='0000000000000000'))

    def test_binary_strings_are_lossless(self):
        self.changed(lambda p: record(p, 'api_call')['args'][2].update(hex='00fe'))

    def test_alias_is_not_clone(self):
        def mutate(p):
            record(p, 'value_lease', 1).update(relation='clone', node='node:9')
        self.changed(mutate)

    def test_effect_duplication_is_invalid_even_on_both_sides(self):
        a = packet()
        effect = record(a, 'effect')
        a['records'].insert(effect['seq'], copy.deepcopy(effect))
        resequence(a)
        with self.assertRaisesRegex(compare.TranscriptError, 'effect'):
            match(a, a)

    def test_package_execution_order_is_not_sorted_away(self):
        a = packet()
        b = copy.deepcopy(a)
        b['packages'].reverse()
        admissions = [r for r in b['records'] if r['kind'] == 'admission']
        alpha = [r for r in b['records'] if r.get('package') == 'alpha' and r['kind'] != 'admission']
        beta = [r for r in b['records'] if r.get('package') == 'beta' and r['kind'] != 'admission']
        host = [r for r in b['records'] if 'package' not in r]
        b['records'] = list(reversed(admissions)) + beta + alpha + host
        record(b, 'archive_result')['included'].reverse()
        resequence(b)
        compare.validate_packet(b)
        with self.assertRaises(compare.TranscriptError):
            match(a, b)

    def test_wrong_arguments_and_catalog_order_are_not_normalized(self):
        self.changed(lambda p: record(p, 'api_call').update(api='game.types.get_all'))
        self.changed(lambda p: record(p, 'api_call')['args'].reverse())
        a, b = packet(), packet()
        values = [{'t': 'integer', 'width': 64, 'signed': True, 'value': str(i)} for i in (1, 2)]
        record(a, 'api_result')['values'] = values
        record(b, 'api_result')['values'] = list(reversed(values))
        compare.validate_packet(a)
        compare.validate_packet(b)
        with self.assertRaises(compare.TranscriptError):
            match(a, b)

    def test_retained_removed_child_keeps_identity_after_source_lease_release(self):
        a = packet()
        edges = [r for r in a['records'] if r['kind'] == 'value_edge']
        self.assertEqual([r['operation'] for r in edges], ['attach', 'detach'])
        retained = record(a, 'value_lease', 3)
        self.assertEqual(retained['relation'], 'retained')
        result = record(a, 'api_call')['args'][3]
        self.assertEqual((result['node'], result['lease']), (retained['node'], retained['lease']))
        match(a, a)
        b = copy.deepcopy(a)
        record(b, 'value_edge', 1)['slot'] = 'wrong-slot'
        with self.assertRaisesRegex(compare.TranscriptError, 'edge'):
            match(b, b)

    def test_stale_generation_is_invalid(self):
        a = packet()
        record(a, 'api_call')['generation'] = 6
        with self.assertRaisesRegex(compare.TranscriptError, 'generation'):
            match(a, a)

    def test_physical_identity_is_distinct_from_semantic_identity(self):
        a = packet()
        b = copy.deepcopy(a)
        archive = record(b, 'archive_result')
        archive['files'].reverse()
        archive['physicalDigest'] = digest(archive['files'])
        with self.assertRaises(compare.TranscriptError):
            match(a, b)
        result = match(a, b, physical='semantic')
        self.assertEqual(result['physical_mode'], 'semantic')
        self.assertTrue(result['physical_differences'])
        archive['semanticEntries'][0]['sha256'] = 'f' * 64
        archive['semanticDigest'] = digest(archive['semanticEntries'])
        with self.assertRaises(compare.TranscriptError):
            match(a, b, physical='semantic')

    def test_dependency_provenance_changes_require_explicit_accounting(self):
        a = packet()
        b = copy.deepcopy(a)
        b['provenance']['dependencies'][0]['version'] = '0.11.2'
        with self.assertRaises(compare.TranscriptError):
            match(a, b)
        result = match(a, b, allow_provenance=True)
        self.assertEqual(result['provenance_differences'][0]['path'], '$.provenance.dependencies[0].version')
        record(b, 'api_result')['values'] = []
        with self.assertRaises(compare.TranscriptError):
            match(a, b, allow_provenance=True)

    def test_v1_projection_is_explicit_directional_and_counted(self):
        actual = packet()
        golden = copy.deepcopy(actual)
        for rec in golden['records']:
            if 'transcript' in rec:
                rec['transcript']['version'] = 1
                for lua in rec['transcript']['records']:
                    lua.pop('message', None)
        with self.assertRaises(compare.TranscriptError):
            match(golden, actual)
        result = match(golden, actual, allow_legacy=True)
        self.assertEqual(result['v2_message_verified'], 0)
        self.assertEqual(result['legacy_v1_not_message_verified'], 3)
        with self.assertRaises(compare.TranscriptError):
            match(actual, golden, allow_legacy=True)
        record(actual, 'api_result')['values'] = []
        with self.assertRaises(compare.TranscriptError):
            match(golden, actual, allow_legacy=True)

    def test_nonaddress_hexadecimal_and_argument_numbers_stay_literal(self):
        for first, second in [
            ('mod not found: Buffer: 0x1', 'mod not found: Buffer: 0x2'),
            ('bad argument #2 (integer expected, got Buffer: <address>)',
             'bad argument #1 (integer expected, got Buffer: <address>)'),
            ('literal Buffer: 0x1', 'literal Buffer: 0x2'),
            ('bad argument #2 (integer expected, got Buffer: 0x1)',
             'bad argument #2 (integer expected, got Buffer: 0x2)'),
        ]:
            with self.subTest(first=first):
                a, b = packet(), packet()
                record(a, 'api_error')['transcript']['records'][0]['message'] = first
                record(b, 'api_error')['transcript']['records'][0]['message'] = second
                with self.assertRaises(compare.TranscriptError):
                    match(a, b)


class Validation(unittest.TestCase):
    schema_negative_cases = 0

    @classmethod
    def tearDownClass(cls):
        print(f'startup schema/state: {cls.schema_negative_cases} rejected packet cases', flush=True)

    def invalid(self, mutate, text=None):
        a = packet()
        mutate(a)
        type(self).schema_negative_cases += 1
        with self.assertRaises(compare.TranscriptError) as caught:
            compare.validate_packet(a)
        if text:
            self.assertIn(text, str(caught.exception))

    def test_schema_validates_synthetic_fixture(self):
        compare.validate_packet(packet())

    def test_terminal_failure_and_recovery_packets_are_valid_observations(self):
        a = packet()
        record(a, 'package_end')['outcome'] = 'failed'
        record(a, 'lua_transcript')['transcript']['fixture_error'] = {'t': 'nil'}
        archive = record(a, 'archive_result')
        archive.update(included=['beta'], excluded=[{'id': 'alpha', 'reason': 'synthetic failure'}])
        recovery = copy.deepcopy(record(a, 'publication_step'))
        recovery.update(kind='recovery', phase='recovery', operation='restore-baseline')
        a['records'].insert(-1, recovery)
        checkpoint = copy.deepcopy(recovery)
        checkpoint.update(kind='checkpoint', phase='host', operation='recovery-verified')
        a['records'].insert(-1, checkpoint)
        resequence(a)
        match(a, a)

    def test_preentry_failure_keeps_original_error_without_invented_lua_records(self):
        a = packet()
        lua = record(a, 'lua_transcript')
        lua['transcript'] = {'version': 2, 'records': [], 'fixture_error': {'t': 'nil'}}
        lua.pop('links')
        record(a, 'package_end')['outcome'] = 'failed'
        record(a, 'archive_result').update(included=['beta'],
            excluded=[{'id': 'alpha', 'reason': 'synthetic top-level error'}])
        match(a, a)

    def test_nonexecuting_participant_does_not_fabricate_empty_lua_evidence(self):
        a = packet()
        a['packages'].append({'id': 'runtime-only', 'version': '1.0.0', 'sha256': 'f' * 64,
                              'executesStartup': False})
        admission = copy.deepcopy(record(a, 'admission'))
        admission['package'] = 'runtime-only'
        a['records'].insert(2, admission)
        record(a, 'archive_result')['included'].append('runtime-only')
        resequence(a)
        match(a, a)

    def test_deferred_resource_staging_preserves_both_completed_package_contexts(self):
        a = deferred_packet()
        last_end = max(rec['seq'] for rec in a['records'] if rec['kind'] == 'package_end')
        stages = [rec for rec in a['records'] if rec['kind'] == 'asset_stage']
        self.assertEqual([rec['package'] for rec in stages], ['alpha', 'beta'])
        self.assertTrue(all(rec['seq'] > last_end for rec in stages))
        compare.validate_schema(a, compare.load_packet(compare.SCHEMA))
        self.assertTrue(match(a, a)['matched'])
        b = copy.deepcopy(a)
        record(b, 'asset_stage')['package'] = 'beta'
        compare.validate_packet(b)
        with self.assertRaises(compare.TranscriptError):
            match(a, b)

    def test_deferred_staging_rejects_unfinished_failed_stale_and_overlapping_contexts(self):
        def early(a):
            stage = record(a, 'asset_stage')
            a['records'].remove(stage)
            a['records'].insert(2, stage)
            resequence(a)

        def overlap(a):
            stage = record(a, 'asset_stage')
            a['records'].remove(stage)
            offset = a['records'].index(record(a, 'package_end', 1))
            a['records'].insert(offset, stage)
            resequence(a)

        def failed(a):
            record(a, 'package_end')['outcome'] = 'failed'
            record(a, 'archive_result').update(included=['beta'],
                excluded=[{'id': 'alpha', 'reason': 'synthetic failure'}])

        for mutate in (early, overlap, failed,
                       lambda a: record(a, 'asset_stage').update(attempt=2),
                       lambda a: record(a, 'asset_stage').update(generation=6)):
            with self.subTest(mutate=mutate):
                a = deferred_packet()
                mutate(a)
                with self.assertRaises(compare.TranscriptError):
                    match(a, a)

    def test_deferred_context_cannot_run_api_or_local_lease_operations(self):
        base = packet()
        for kind in ('api_call', 'api_result', 'api_error', 'lua_transcript',
                     'value_lease', 'value_edge', 'effect', 'package_begin'):
            with self.subTest(kind=kind):
                a = deferred_packet()
                rec = copy.deepcopy(record(base, kind))
                rec['phase'] = 'deferred-stage'
                a['records'].insert(-2, rec)
                resequence(a)
                with self.assertRaises(compare.TranscriptError):
                    match(a, a)
        a = deferred_packet()
        record(a, 'effect', 1)['scope'] = 'external'
        with self.assertRaises(compare.TranscriptError):
            match(a, a)

    def test_deferred_staging_cannot_return_to_an_older_execution_attempt(self):
        a = deferred_packet()
        # Start a later, successful beta-only retry before the old deferred writes.
        retry = retry_packet([(3, 'success')])
        run = [rec for rec in retry['records'] if rec['phase'] == 'startup']
        offset = a['records'].index(record(a, 'asset_stage'))
        a['records'][offset:offset] = run
        resequence(a)
        with self.assertRaises(compare.TranscriptError):
            match(a, a)

    def test_sf_package_identity_grammar_is_preserved_everywhere(self):
        for identity in ('_alpha', '-alpha', '.alpha', '_', '-', '...', 'A' * 80):
            with self.subTest(identity=identity):
                a = packet()
                rename_package(a, 'alpha', identity)
                compare.validate_schema(a, compare.load_packet(compare.SCHEMA))
                self.assertTrue(match(a, a)['matched'])
                # Cover exclusion IDs as well as participant, owner and included IDs.
                record(a, 'archive_result').update(included=['beta'],
                    excluded=[{'id': identity, 'reason': 'synthetic withdrawal'}])
                self.assertTrue(match(a, a)['matched'])

    def test_invalid_package_ids_are_rejected_without_normalizing(self):
        for identity in ('', '.', '..', 'a' * 81, 'a/b', 'a\\b', 'a:b', 'a b', 'é', 'a\n'):
            with self.subTest(identity=identity):
                a = packet()
                rename_package(a, 'alpha', identity)
                with self.assertRaises(compare.TranscriptError):
                    match(a, a)
        schema = compare.load_packet(compare.SCHEMA)
        for name in ('package', 'exclusion', 'admission', 'package_begin', 'package_end',
                     'api_call', 'api_result', 'api_error', 'lua_transcript',
                     'value_lease', 'value_edge', 'effect', 'asset_stage', 'archive_result'):
            field = 'id' if name in ('package', 'exclusion') else 'package'
            definition = schema['$defs'][name]['properties']
            if name == 'archive_result':
                definition = definition['included']['items']
            else:
                definition = definition[field]
            self.assertEqual(definition, {'$ref': '#/$defs/packageId'})
        self.assertEqual(schema['$defs']['api_call']['properties']['moduleOwner'],
                         {'$ref': '#/$defs/packageId'})

    def test_hidden_transaction_basenames_are_preserved(self):
        for kind in ('checkpoint', 'publication_step', 'recovery'):
            with self.subTest(kind=kind):
                a = packet()
                operation = record(a, 'publication_step')
                operation.update(kind=kind, phase='recovery' if kind == 'recovery' else 'host',
                                 file='.vanilla-header')
                compare.validate_schema(a, compare.load_packet(compare.SCHEMA))
                self.assertTrue(match(a, a)['matched'])
        a = packet()
        archive = record(a, 'archive_result')
        archive['files'][0]['name'] = '.synthetic.dat'
        archive['physicalDigest'] = digest(archive['files'])
        self.assertTrue(match(a, a)['matched'])

    def test_basenames_reject_traversal_and_generic_ids_keep_their_grammar(self):
        schema = compare.load_packet(compare.SCHEMA)
        for name, field in (('file', 'name'), ('checkpoint', 'file'),
                            ('publication_step', 'file'), ('recovery', 'file')):
            definition = schema['$defs'][name]['properties'][field]
            for basename in ('', '.', '..', '../a', 'a/b', 'a\\b', '/a', 'C:a', 'a\n'):
                with self.subTest(name=name, basename=basename):
                    with self.assertRaises(compare.TranscriptError):
                        compare.validate_schema(basename, definition, schema)
        for name, field in (('provenance', 'sourceRevision'), ('publication_step', 'operation')):
            with self.assertRaises(compare.TranscriptError):
                compare.validate_schema('.hidden', schema['$defs'][name]['properties'][field], schema)

    def test_final_included_and_excluded_are_disjoint_even_in_self_comparison(self):
        a = packet()
        record(a, 'archive_result')['excluded'] = [{'id': 'alpha', 'reason': 'synthetic refusal'}]
        with self.assertRaisesRegex(compare.TranscriptError, 'package outcome accounting'):
            compare.validate_packet(a)
        with self.assertRaisesRegex(compare.TranscriptError, 'package outcome accounting'):
            match(a, a)

    def test_ordered_retries_allow_gaps_without_renumbering_host_records(self):
        for attempts in ([(1, 'failed'), (2, 'success')], [(3, 'failed'), (7, 'success')]):
            with self.subTest(attempts=attempts):
                a = retry_packet(attempts)
                self.assertTrue(match(a, a)['matched'])

    def test_regressing_attempt_cannot_hide_the_last_failure(self):
        a = retry_packet([(2, 'success'), (1, 'failed')])
        with self.assertRaisesRegex(compare.TranscriptError, 'attempt'):
            compare.validate_packet(a)
        with self.assertRaisesRegex(compare.TranscriptError, 'attempt'):
            match(a, a)

    def test_last_failed_ordered_retry_cannot_be_included(self):
        a = retry_packet([(1, 'success'), (2, 'failed')])
        with self.assertRaisesRegex(compare.TranscriptError, 'successful completed execution'):
            match(a, a)

    def test_attempt_cannot_regress_between_different_packages(self):
        a = packet()
        for rec in a['records']:
            if rec.get('package') == 'alpha' and rec['phase'] == 'startup':
                rec['attempt'] = 2
        with self.assertRaisesRegex(compare.TranscriptError, 'attempt'):
            match(a, a)

    def test_uuid_and_shared_policy_differences_cannot_hide_as_provenance(self):
        for key in ('planId', 'launchId'):
            a, b = packet(), packet()
            b[key] = '00000000-0000-4000-8000-000000000099'
            with self.assertRaises(compare.TranscriptError):
                match(a, b, allow_provenance=True)
        a, b = packet(), packet()
        b['provenance']['policy']['id'] = 'different-policy'
        with self.assertRaises(compare.TranscriptError):
            match(a, b, allow_provenance=True)

    def test_required_top_level_fields(self):
        for key in packet():
            with self.subTest(key=key):
                self.invalid(lambda p: p.pop(key))

    def test_unknown_fields_versions_uuid_target_dialect(self):
        for mutate in [lambda p: p.update(extra=True), lambda p: p.update(schemaVersion=2),
                       lambda p: p.update(planId='plan:1'), lambda p: p.update(target='both'),
                       lambda p: p.update(dialect='mixed'), lambda p: p.update(generation=True)]:
            self.invalid(mutate)

    def test_no_empty_host_or_default_empty_lua_packet(self):
        self.invalid(lambda p: p.update(records=[]))
        self.invalid(lambda p: record(p, 'lua_transcript')['transcript'].update(records=[]))
        self.invalid(lambda p: p['records'].pop())

    def test_sequences_calls_and_package_boundaries(self):
        self.invalid(lambda p: p['records'][1].update(seq=99))
        self.invalid(lambda p: record(p, 'api_result').update(call='call:99'))
        self.invalid(lambda p: record(p, 'api_result').update(package='beta'))
        self.invalid(lambda p: record(p, 'package_end').update(package='beta'))
        self.invalid(lambda p: record(p, 'package_begin').update(package='unknown'))
        self.invalid(lambda p: record(p, 'admission').update(status='skipped', reason='fixture'))

    def test_no_raw_address_or_byte_dump_fields(self):
        for field in ('address', 'default_bytes', 'gameBytes', 'rawLuaState'):
            self.invalid(lambda p: record(p, 'value_lease').update({field: '0x1234'}))
        self.invalid(lambda p: record(p, 'publication_step').update(file='../game.dat'))

    def test_numeric_ranges_and_canonical_binary(self):
        for value in ('9223372036854775808', '-9223372036854775809', '-0', '+1', '01'):
            self.invalid(lambda p: record(p, 'api_call')['args'][0].update(value=value))
        self.invalid(lambda p: record(p, 'api_call')['args'][1].update(bits='8000'))
        self.invalid(lambda p: record(p, 'api_call')['args'][2].update(hex='0ff'))

    def test_lease_alias_clone_and_retained_lifetimes(self):
        self.invalid(lambda p: record(p, 'value_lease', 1).update(node='node:99'))
        self.invalid(lambda p: record(p, 'value_lease', 2).update(node='node:1'))
        self.invalid(lambda p: record(p, 'api_call')['args'][3].update(lease='lease:99'))
        self.invalid(lambda p: record(p, 'value_lease', 1).update(source='lease:99'))
        self.invalid(lambda p: record(p, 'value_lease', 1).update(attempt=2))

    def test_shared_schema_message_constraints(self):
        self.invalid(lambda p: record(p, 'api_error')['transcript'].update(version=1))
        self.invalid(lambda p: record(p, 'api_error')['transcript']['records'][0].update(message=None))
        self.invalid(lambda p: record(p, 'api_error')['transcript']['records'][0].update(ok=True))
        self.invalid(lambda p: record(p, 'lua_transcript')['transcript'].update(extra=1))

    def test_expect_failures_and_fixture_errors_never_pass_by_equality(self):
        self.invalid(lambda p: record(p, 'lua_transcript')['transcript']['records'].append(
            {'seq': 2, 'kind': 'expect', 'label': 'bad', 'pass': False,
             'values': [{'t': 'nil'}], 'expected': {'t': 'nil'}}))
        self.invalid(lambda p: record(p, 'lua_transcript')['transcript'].update(fixture_error={'t': 'nil'}))

    def test_physical_and_semantic_digests_are_independently_checked(self):
        self.invalid(lambda p: record(p, 'archive_result').update(physicalDigest='f' * 64))
        self.invalid(lambda p: record(p, 'archive_result').update(semanticDigest='f' * 64))
        self.invalid(lambda p: record(p, 'archive_result')['files'].append(
            record(p, 'archive_result')['files'][0]))
        def unsort(p):
            a = record(p, 'archive_result')
            a['semanticEntries'].reverse()
            a['semanticDigest'] = digest(a['semanticEntries'])
        self.invalid(unsort)

    def test_shared_provenance_hashes_are_checked_not_just_compared(self):
        self.invalid(lambda p: p['provenance'].update(preludeSha256='f' * 64))
        self.invalid(lambda p: p['provenance'].update(luaSchemaSha256='f' * 64))

    def test_loader_rejects_duplicate_keys_nonfinite_and_missing_files(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as tmp:
            path = Path(tmp) / 'bad.json'
            for content in ('{"records":[],"records":[]}', '{"x":NaN}', ''):
                path.write_text(content)
                with self.assertRaises(compare.TranscriptError):
                    compare.load_packet(path)
            with self.assertRaises(compare.TranscriptError):
                compare.load_packet(Path(tmp) / 'missing.json')

    def test_closed_schema_keyword_support(self):
        for schema in [
            {'inventedKeyword': True},
            {'oneOf': [{'const': 1}, {'const': 2, 'inventedKeyword': True}]},
            {'$defs': {'unused': {'inventedKeyword': True}}, 'const': 1},
        ]:
            with self.subTest(schema=schema):
                with self.assertRaisesRegex(compare.TranscriptError, 'unsupported'):
                    compare.validate_schema(1, schema)


class CommandLine(unittest.TestCase):
    def run_cli(self, *args):
        args = list(map(str, args))
        if '--fixture' not in args:
            args += ['--fixture', str(HERE / 'fixtures/synthetic.input')]
        return subprocess.run([sys.executable, '-I', str(HERE / 'compare.py'), *args],
                              capture_output=True, text=True, timeout=15)

    def test_cli_strict_success_and_explicit_accounting(self):
        fixture = HERE / 'fixtures/synthetic.json'
        result = self.run_cli(fixture, fixture)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['v2_message_verified'], 3)

    def test_independent_fixture_input_is_required_and_hash_checked(self):
        with self.assertRaises(compare.TranscriptError):
            match(packet(), packet(), fixture_path=None)
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as tmp:
            source = Path(tmp) / 'missing.input'
            fixture = HERE / 'fixtures/synthetic.json'
            with self.assertRaises(compare.TranscriptError):
                match(packet(), packet(), fixture_path=source)
            result = self.run_cli(fixture, fixture, '--fixture', source)
            self.assertEqual(result.returncode, 2)
            self.assertIn('missing.input', result.stderr)
            source.write_text('changed synthetic fixture input')
            with self.assertRaises(compare.TranscriptError):
                match(packet(), packet(), fixture_path=source)
            result = self.run_cli(fixture, fixture, '--fixture', source)
            self.assertEqual(result.returncode, 2)
            self.assertIn('digest mismatch', result.stderr)

    def test_cli_missing_golden_or_fixture_is_nonzero(self):
        fixture = HERE / 'fixtures/synthetic.json'
        for args in ((fixture, fixture.with_name('missing.json')),
                     (fixture.with_name('missing.json'), fixture)):
            result = self.run_cli(*args)
            self.assertEqual(result.returncode, 2)
            self.assertIn('missing.json', result.stderr)


if __name__ == '__main__':
    unittest.main()
