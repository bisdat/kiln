#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate/compare authorized host packets. No Lua error normalization lives here.

The shared prelude is the sole message policy; embedded transcripts are never
rewritten on disk. This tool has no engine/parser dependency or network resolver.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SCHEMA = ROOT / 'startup-transcript.schema.json'
SHARED = ROOT / 'transcript.schema.json'


class TranscriptError(ValueError):
    """Invalid input or an observed mismatch; never a successful empty result."""


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True, allow_nan=False)


def digest(value):
    return hashlib.sha256(canonical(value).encode('ascii')).hexdigest()


def file_digest(path):
    try:
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()
    except OSError as error:
        raise TranscriptError(f'{path}: {error}') from error


def require(condition, path, message):
    if not condition:
        raise TranscriptError(f'{path}: {message}')


def _pairs(pairs):
    value = {}
    for key, child in pairs:
        require(key not in value, '$', f'duplicate JSON key {key!r}')
        value[key] = child
    return value


def _constant(value):
    raise TranscriptError(f'$: nonfinite JSON number {value}')


def load_packet(path):
    """Load exactly one required file, rejecting duplicate keys and nonfinite JSON."""
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=_pairs,
                          parse_constant=_constant)
    except (OSError, ValueError, RecursionError) as error:
        raise TranscriptError(f'{path}: {error}') from error


_SCHEMA_KEYWORDS = {'$schema', '$id', 'title', 'description', '$defs', '$ref', 'type',
                    'required', 'additionalProperties', 'properties', 'items', 'enum',
                    'const', 'minimum', 'maximum', 'minItems', 'maxItems', 'minLength',
                    'pattern', 'oneOf', 'not'}


def _check_schema(schema, path='$schema'):
    # Inspect even unused definitions/alternatives before validating data. Otherwise
    # oneOf/not could mistake an unsupported constraint for an ordinary nonmatch.
    require(isinstance(schema, dict), path, 'invalid schema object')
    unsupported = schema.keys() - _SCHEMA_KEYWORDS
    require(not unsupported, path, f'unsupported schema keyword(s): {sorted(unsupported)}')
    if '$ref' in schema:
        document = schema['$ref'].partition('#')[0]
        require(document in ('', 'transcript.schema.json'), path, 'unsupported schema reference')
    if 'type' in schema:
        require(schema['type'] in ('object', 'array', 'string', 'boolean', 'integer'), path,
                'unsupported schema type')
    for keyword in ('$defs', 'properties'):
        for name, child in schema.get(keyword, {}).items():
            _check_schema(child, path + '.' + keyword + '.' + name)
    for keyword in ('items', 'not'):
        if keyword in schema:
            _check_schema(schema[keyword], path + '.' + keyword)
    for index, child in enumerate(schema.get('oneOf', [])):
        _check_schema(child, f'{path}.oneOf[{index}]')


def validate_schema(value, schema, root=None, path='$'):
    """Closed structural subset, like sfi-core/src/conformance.rs:223-359.

    Read the actual shared schema, not a copied value/record definition. Local
    refs and that one sibling document are the only resolvable references. Fail
    closed if either schema grows an unsupported keyword; never fetch a URI.
    This is not a general-purpose draft-2020-12 validator.
    """
    if root is None:
        _check_schema(schema)
        root = schema
    if '$ref' in schema:
        document, _, fragment = schema['$ref'].partition('#')
        require(document in ('', 'transcript.schema.json'), path, 'unsupported schema reference')
        target_root = load_packet(SHARED) if document else root
        if document:
            _check_schema(target_root)
        target = target_root
        if fragment:
            require(fragment.startswith('/'), path, 'unsupported schema fragment')
            for part in fragment[1:].split('/'):
                key = part.replace('~1', '/').replace('~0', '~')
                require(isinstance(target, dict) and key in target, path, 'unresolved schema reference')
                target = target[key]
        validate_schema(value, target, target_root, path)
    if 'const' in schema:
        require(canonical(value) == canonical(schema['const']), path, 'wrong constant')
    if 'enum' in schema:
        require(any(canonical(value) == canonical(x) for x in schema['enum']), path, 'not in enum')
    if 'type' in schema:
        tests = {'object': isinstance(value, dict), 'array': isinstance(value, list),
                 'string': isinstance(value, str), 'boolean': type(value) is bool,
                 'integer': type(value) is int}
        require(schema['type'] in tests, path, 'unsupported schema type')
        require(tests[schema['type']], path, 'expected ' + schema['type'])
    if type(value) in (int, float):
        for keyword, ok in [('minimum', lambda bound: value >= bound),
                            ('maximum', lambda bound: value <= bound)]:
            if keyword in schema:
                require(ok(schema[keyword]), path, keyword + ' violated')
    if isinstance(value, str):
        if 'minLength' in schema:
            require(len(value) >= schema['minLength'], path, 'string too short')
        if 'pattern' in schema:
            # fullmatch also refuses a trailing newline after a schema $ anchor.
            require(re.fullmatch(schema['pattern'], value) is not None, path, 'pattern mismatch')
    if isinstance(value, dict):
        for key in schema.get('required', []):
            require(key in value, path, 'missing required field ' + key)
        properties = schema.get('properties', {})
        for key, child in value.items():
            if key in properties:
                validate_schema(child, properties[key], root, path + '.' + key)
            else:
                require(schema.get('additionalProperties') is not False, path, 'unexpected field ' + key)
    if isinstance(value, list):
        require(len(value) >= schema.get('minItems', 0), path, 'too few items')
        require(len(value) <= schema.get('maxItems', len(value)), path, 'too many items')
        if 'items' in schema:
            for index, child in enumerate(value):
                validate_schema(child, schema['items'], root, f'{path}[{index}]')
    if 'not' in schema:
        try:
            validate_schema(value, schema['not'], root, path)
        except TranscriptError:
            pass
        else:
            raise TranscriptError(f'{path}: forbidden schema match')
    if 'oneOf' in schema:
        matches = 0
        for variant in schema['oneOf']:
            try:
                validate_schema(value, variant, root, path)
                matches += 1
            except TranscriptError:
                pass
        require(matches == 1, path, 'expected exactly one schema variant')


def _lua_value(value, path):
    """Output-shape checks in addition to the shared structural schema."""
    kind = value['t']
    fields = {'nil': {'t'}, 'boolean': {'t', 'v'}, 'integer': {'t', 'v'},
              'float': {'t', 'v'}, 'string': {'t', 'v'}, 'table': {'t', 'v', 'mt'},
              'cycle': {'t'}, 'function': {'t'}, 'userdata': {'t', 'name'}, 'thread': {'t'}}
    require(value.keys() == fields[kind], path, 'not a canonical shared Lua value')
    payload = value.get('v')
    if kind == 'boolean':
        require(type(payload) is bool, path, 'not a Lua boolean')
    elif kind == 'integer':
        require(isinstance(payload, str) and re.fullmatch(r'0|-?[1-9][0-9]*', payload), path,
                'not canonical Lua integer text')
        require(-2**63 <= int(payload) < 2**63, path, 'Lua integer outside signed 64-bit range')
    elif kind == 'float':
        require(isinstance(payload, str) and re.fullmatch(r'[0-9a-f]{16}|inf|-inf|nan', payload),
                path, 'not canonical shared float bits')
    elif kind == 'string':
        require(isinstance(payload, str) and len(payload) >= 2 and payload[0] == payload[-1] == '"',
                path, 'not Lua %q string text')
    elif kind == 'table':
        require(isinstance(payload, list), path, 'not a canonical Lua table')
        for index, pair in enumerate(payload):
            for child in pair:
                _lua_value(child, f'{path}.v[{index}]')


def _transcript(value, path):
    # Structural validation already followed the unchanged shared $ref.
    require(bool(value['records']) or 'fixture_error' in value, path,
            'empty Lua transcript without an original error is not evidence')
    for seq, rec in enumerate(value['records'], 1):
        require(rec['seq'] == seq, path, 'noncontiguous Lua sequence')
        kind = rec['kind']
        required = {'call': {'seq', 'label', 'kind', 'ok'},
                    'value': {'seq', 'label', 'kind', 'values'},
                    'expect': {'seq', 'label', 'kind', 'values', 'expected', 'pass'},
                    'note': {'seq', 'label', 'kind', 'text'}}[kind]
        allowed = set(required)
        if kind == 'call':
            require(type(rec.get('ok')) is bool, path, 'call lacks protected-call outcome')
            required |= {'values'} if rec['ok'] else {'error'}
            allowed |= {'values'} if rec['ok'] else {'error', 'message'}
        require(required <= rec.keys() <= allowed, path, 'invalid shared record fields')
        if kind in ('value', 'expect'):
            require(len(rec['values']) == 1, path, 'single-value record has wrong arity')
        if kind == 'expect':
            require(rec['pass'] is True, path, 'failed fixture expectation')
        for child in rec.get('values', []):
            _lua_value(child, path)
        for key in ('error', 'expected'):
            if key in rec:
                _lua_value(rec[key], path)
    if 'fixture_error' in value:
        _lua_value(value['fixture_error'], path)


def _failed_call(value, path):
    require('fixture_error' not in value and len(value['records']) == 1, path,
            'error observation must contain exactly one shared failed call')
    rec = value['records'][0]
    require(rec['kind'] == 'call' and rec.get('ok') is False, path, 'expected shared failed call')


def validate_packet(packet):
    """Validate structure and host accounting. Does not establish runtime parity."""
    validate_schema(packet, load_packet(SCHEMA))
    for name, file in [('preludeSha256', ROOT / 'prelude.lua'), ('luaSchemaSha256', SHARED)]:
        require(packet['provenance'][name] == file_digest(file),
                '$.provenance.' + name, 'does not identify the current shared interface')
    for key, identity in [('dependencies', 'name'), ('contracts', 'family')]:
        names = [x[identity] for x in packet['provenance'][key]]
        require(names == sorted(set(names)), '$.provenance.' + key, 'must be sorted and unique')
    packages = [x['id'] for x in packet['packages']]
    executes = {x['id']: x['executesStartup'] for x in packet['packages']}
    require(len(packages) == len(set(packages)), '$.packages', 'duplicate package')
    admissions, calls, leases, nodes, effects = {}, {}, {}, set(), set()
    order, begun, completed, transcripts = {}, set(), {}, {}
    active, latest_attempt = None, 0
    revisions, edges = {}, {}

    def lease(token, rec, path):
        require(token in leases and leases[token]['live'], path, 'unknown or released lease')
        entry = leases[token]
        require(entry['context'] == (rec['phase'], rec['attempt'], rec['generation']), path,
                'stale lease phase/attempt/generation')
        return entry

    def typed(value, rec, path):
        if value['t'] == 'integer':
            bits = value['width'] - int(value['signed'])
            minimum = -2**bits if value['signed'] else 0
            require(minimum <= int(value['value']) < 2**bits, path, 'integer outside declared width')
        elif value['t'] == 'float':
            require(len(value['bits']) == value['width'] // 4, path, 'wrong floating bit width')
        elif value['t'] == 'node':
            entry = lease(value['lease'], rec, path)
            require(entry['node'] == value['node'] and entry['owner'] == rec['package'], path,
                    'node/owner does not match issued lease')
        elif value['t'] == 'lua':
            _lua_value(value['value'], path)

    for seq, rec in enumerate(packet['records'], 1):
        path = f'$.records[{seq-1}]'
        require(rec['seq'] == seq, path, 'noncontiguous host sequence')
        require(rec['generation'] == packet['generation'], path, 'stale archive generation')
        kind = rec['kind']
        package = rec.get('package')
        context = (rec['phase'], rec['attempt'], rec['generation'])
        if package is not None:
            require(package in packages, path, 'unknown package')
        if kind == 'admission':
            require(rec['phase'] == 'host' and not begun and package not in admissions, path,
                    'admission must precede execution and occur once per package')
            require(package == packages[len(admissions)], path, 'admission package order differs from plan')
            admissions[package] = rec['status']
        elif kind == 'package_begin':
            require(rec['phase'] == 'startup' and active is None and len(admissions) == len(packages), path,
                    'package begins before terminal admissions or overlaps another package')
            require(admissions[package] == 'admitted' and executes[package], path,
                    'skipped or nonexecuting package executed')
            require(rec['attempt'] >= latest_attempt, path, 'execution attempt regressed')
            latest_attempt = rec['attempt']
            key = (rec['attempt'], package)
            require(key not in begun, path, 'duplicate package execution within attempt')
            index = packages.index(package)
            require(index > order.get(rec['attempt'], -1), path, 'execution package order differs from plan')
            order[rec['attempt']] = index
            begun.add(key)
            active = (package, context)
        elif kind == 'package_end':
            require(active == (package, context), path, 'unbalanced package end')
            require(not any(not c['done'] and c['owner'] == package for c in calls.values()), path,
                    'package ended with unfinished API call')
            key = (rec['attempt'], package)
            require(key in transcripts, path, 'package lacks nonempty Lua transcript')
            require(not transcripts[key] or rec['outcome'] == 'failed', path,
                    'fixture_error in successful package')
            completed[package] = {'context': context, 'outcome': rec['outcome']}
            active = None
        elif rec['phase'] == 'deferred-stage':
            # Both SF pins finish runner.run() before commit_asset_changes applies
            # dirty resources. Retain the contributor without inventing execution.
            require(kind == 'asset_stage' or (kind == 'effect' and rec['scope'] == 'archive-write'),
                    path, 'deferred stage only permits asset staging and archive-write effects')
            execution = completed.get(package)
            require(active is None and rec['attempt'] == latest_attempt and execution and
                    execution['context'] == ('startup', rec['attempt'], rec['generation']) and
                    execution['outcome'] == 'success', path,
                    'deferred stage lacks current successful completed execution')
        elif package is not None:
            require(active == (package, context), path, 'package observation outside its execution')
        else:
            require(active is None and rec['phase'] in ('host', 'recovery'), path,
                    'host observation inside package execution')

        if 'transcript' in rec:
            _transcript(rec['transcript'], path + '.transcript')
        if kind == 'api_call':
            require(rec['moduleOwner'] in packages, path, 'unknown dependency-module owner')
            require(rec['call'] not in calls, path, 'duplicate call symbol')
            calls[rec['call']] = {'owner': package, 'context': context, 'done': False}
            for value in rec['args']:
                typed(value, rec, path + '.args')
        elif kind in ('api_result', 'api_error'):
            call = calls.get(rec['call'])
            require(call and not call['done'] and call['owner'] == package and call['context'] == context,
                    path, 'unmatched, duplicate or stale API completion')
            call['done'] = True
            if kind == 'api_result':
                for value in rec['values']:
                    typed(value, rec, path + '.values')
            else:
                _failed_call(rec['transcript'], path)
                require(rec['transcript']['records'][0]['label'] == rec['call'], path,
                        'error label must match call symbol')
        elif kind == 'lua_transcript':
            key = (rec['attempt'], package)
            require(key not in transcripts, path, 'duplicate package/phase/attempt transcript')
            transcripts[key] = 'fixture_error' in rec['transcript']
            links = set()
            for link in rec.get('links', []):
                require(link['luaSeq'] not in links, path, 'duplicate Lua call link')
                links.add(link['luaSeq'])
                call = calls.get(link['call'])
                require(call and call['done'] and call['owner'] == package and call['context'] == context,
                        path, 'invalid Lua-to-host call link')
                records = rec['transcript']['records']
                require(link['luaSeq'] <= len(records) and records[link['luaSeq']-1]['kind'] == 'call',
                        path, 'link does not identify a Lua call record')
        elif kind == 'value_lease':
            token, node = rec['lease'], rec['node']
            if rec['action'] == 'release':
                entry = lease(token, rec, path)
                require(entry['node'] == node and entry['owner'] == package, path, 'foreign lease release')
                require('source' not in rec and 'relation' not in rec, path, 'release has grant fields')
                entry['live'] = False
            else:
                require(token not in leases and 'relation' in rec, path, 'reused lease or missing relation')
                relation = rec['relation']
                if relation == 'root':
                    require(node not in nodes and 'source' not in rec, path, 'root node must be new')
                else:
                    require('source' in rec, path, 'relation lacks source lease')
                    source = lease(rec['source'], rec, path)
                    require((node not in nodes and node != source['node']) if relation == 'clone'
                            else node == source['node'], path, 'alias/clone/retained relation mismatch')
                nodes.add(node)
                leases[token] = {'node': node, 'owner': package, 'context': context, 'live': True}
        elif kind == 'value_edge':
            parent = lease(rec['parentLease'], rec, path)
            child = lease(rec['childLease'], rec, path)
            require(parent['owner'] == child['owner'] == package, path, 'foreign graph edge lease')
            key = (parent['node'], rec['slot'])
            if rec['operation'] == 'attach':
                require(key not in edges, path, 'graph edge already attached')
                edges[key] = child['node']
            else:
                require(edges.get(key) == child['node'], path, 'graph edge detach mismatch')
                del edges[key]
        elif kind == 'effect':
            require(rec['effect'] not in effects, path, 'duplicate effect symbol')
            effects.add(rec['effect'])
            if rec['scope'] == 'local':
                require({'lease', 'beforeRevision', 'afterRevision'} <= rec.keys(), path,
                        'local effect lacks lease/revisions')
                entry = lease(rec['lease'], rec, path)
                require(entry['owner'] == package and entry['node'] == rec['target'], path,
                        'local effect targets foreign node')
                before = revisions.get(entry['node'], 0)
                require(rec['beforeRevision'] == before and rec['afterRevision'] == before + 1,
                        path, 'local effect revision mismatch')
                revisions[entry['node']] = before + 1
            else:
                require(not {'lease', 'beforeRevision', 'afterRevision'} & rec.keys(), path,
                        'nonlocal effect has local graph fields')
        elif kind in ('checkpoint', 'publication_step', 'recovery'):
            require(rec['expectedGeneration'] == packet['generation'], path, 'stale expected generation')
            if rec['status'] == 'error':
                require('transcript' in rec, path, 'operation error lacks shared observation')
                _failed_call(rec['transcript'], path)
            else:
                require('transcript' not in rec, path, 'successful operation has error transcript')
        elif kind == 'archive_result':
            require(seq == len(packet['records']) and active is None, path, 'archive result must be terminal')
            names = [file['name'] for file in rec['files']]
            require(len(set(names)) == len(names), path, 'duplicate physical filename')
            require(rec['physicalDigest'] == digest(rec['files']), path, 'physical manifest/order digest mismatch')
            keys = [(x['kind'], x['key']) for x in rec['semanticEntries']]
            require(keys == sorted(set(keys)), path, 'semantic entries must be sorted and unique')
            require(rec['semanticDigest'] == digest(rec['semanticEntries']), path, 'semantic digest mismatch')
            included, excluded = rec['included'], [x['id'] for x in rec['excluded']]
            require(len(included) + len(excluded) == len(packages) and
                    set(included + excluded) == set(packages), path, 'incomplete/duplicate package outcome accounting')
            require(included == [p for p in packages if p in included] and
                    excluded == [p for p in packages if p in excluded], path, 'final package order differs from plan')
            require(all(admissions.get(p) == 'admitted' for p in included), path, 'skipped package included')
            require(all(not executes[p] or completed.get(p, {}).get('outcome') == 'success'
                        for p in included), path, 'included package lacks successful completed execution')
    require(packet['records'][-1]['kind'] == 'archive_result', '$.records', 'missing terminal archive result')
    require(len(admissions) == len(packages) and active is None and all(c['done'] for c in calls.values()),
            '$.records', 'incomplete admission/package/API accounting')


def _differences(a, b, path='$'):
    if type(a) is not type(b):
        return [{'path': path, 'expected': a, 'actual': b}]
    if isinstance(a, dict) and a.keys() == b.keys():
        return [item for key in sorted(a) for item in _differences(a[key], b[key], path + '.' + key)]
    if isinstance(a, list) and len(a) == len(b):
        return [item for i, (x, y) in enumerate(zip(a, b)) for item in _differences(x, y, f'{path}[{i}]')]
    return [] if a == b else [{'path': path, 'expected': a, 'actual': b}]


def compare_packets(expected, actual, *, fixture_path=None, allow_legacy=False,
                    allow_provenance=False, physical='strict'):
    """Directional golden comparison; explicit exceptions are returned as accounting.

    Legacy projection is exactly sfi-core::conformance::compare_golden: only v2
    -> expected v1 and only failed-call `message` removal. No text masking here.
    """
    require(physical in ('strict', 'semantic'), '$', 'invalid physical comparison mode')
    require(fixture_path is not None, '$.fixture', 'independent fixture_path is required')
    fixture_sha256 = file_digest(fixture_path)
    for side, packet in [('golden', expected), ('actual', actual)]:
        try:
            validate_packet(packet)
            require(packet['fixture']['sha256'] == fixture_sha256, '$.fixture.sha256',
                    'independent fixture input digest mismatch')
        except TranscriptError as error:
            raise TranscriptError(f'invalid {side}: {error}') from error
    a, b = copy.deepcopy(expected), copy.deepcopy(actual)
    result = {'matched': True, 'fixture': a['fixture']['id'], 'v2_message_verified': 0,
              'legacy_v1_not_message_verified': 0, 'physical_mode': physical,
              'physical_differences': [], 'provenance_differences': []}
    if allow_provenance:
        for key in ('engine', 'sourceRevision', 'dependencies'):
            result['provenance_differences'] += _differences(
                a['provenance'][key], b['provenance'][key], '$.provenance.' + key)
            b['provenance'][key] = a['provenance'][key]
    for index, (left, right) in enumerate(zip(a['records'], b['records'])):
        if 'transcript' in left and 'transcript' in right:
            x, y = left['transcript'], right['transcript']
            if x['version'] == 1:
                require(allow_legacy, f'$.records[{index}]', 'legacy v1 requires explicit allow_legacy')
                result['legacy_v1_not_message_verified'] += 1
                if y['version'] == 2:
                    y['version'] = 1
                    for rec in y['records']:
                        if rec['kind'] == 'call' and rec['ok'] is False:
                            rec.pop('message', None)
            else:
                result['v2_message_verified'] += 1
        if physical == 'semantic' and left['kind'] == right['kind'] == 'archive_result':
            for key in ('files', 'physicalDigest'):
                result['physical_differences'] += _differences(left[key], right[key], f'$.records[{index}].{key}')
                right[key] = left[key]
    differences = _differences(a, b)
    require(not differences, '$', 'transcripts differ: ' + canonical(differences[0] if differences else {}))
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('golden', type=Path)
    parser.add_argument('actual', type=Path)
    parser.add_argument('--fixture', type=Path, required=True, help='independent fixture input to hash, never execute')
    parser.add_argument('--allow-legacy-v1', action='store_true', help='count v1 matches as NOT message-verified')
    parser.add_argument('--allow-provenance', action='store_true', help='report engine/revision/dependency deltas explicitly')
    parser.add_argument('--physical', choices=('strict', 'semantic'), default='strict')
    args = parser.parse_args(argv)
    try:
        result = compare_packets(load_packet(args.golden), load_packet(args.actual),
                                 fixture_path=args.fixture, allow_legacy=args.allow_legacy_v1, allow_provenance=args.allow_provenance,
                                 physical=args.physical)
        print(canonical(result))
        return 0
    except (TranscriptError, RecursionError) as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
