# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare source-only, isolated dependency captures. This tool never runs Cargo.

Both copies are the retained pre-alignment baseline plus the same test-only runner child. All real registrars,
argument casts, userdata, sandbox/require and fixture archives stay intact. The candidate differs from the old copy ONLY in
its dependency graph: the workspace manifests and Cargo.lock of the tree this tool is in (the production alignment)
replace the baseline's, so the candidate builds exactly the locked production graph (no lock refresh). When that
graph links the MIT core, crates/sfi and tools/sfi/conformance come from the same repository.
Old observations MUST exist before a candidate can be prepared.
"""
import argparse
import hashlib
import json
import shutil
import tomllib
from pathlib import Path

# The workspace files that carry the dependency graph.
GRAPH = ['Cargo.toml', 'Cargo.lock', 'eml/mod-loader-lua/Cargo.toml', 'kiln-eml/Cargo.toml']


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def versions(lock, expected):
    packages = tomllib.loads(lock.read_text())['package']
    return all([p['version'] for p in packages if p['name'] == name] == [version] for name, version in expected)


def prepare(source, output, mode):
    source, output = source.resolve(), output.resolve()
    current = Path(__file__).resolve().parents[5]  # gpl/kiln-eml of the tree holding this tool
    if source == output or source in output.parents:
        raise ValueError('scratch must be outside the source tree')
    if mode == 'candidate' and not (output / 'old-observations/bindings-v2.json').is_file():
        raise ValueError('capture old results before preparing candidate')
    destination = output / mode
    destination.mkdir(parents=True, exist_ok=False)
    gpl = destination / 'gpl/kiln-eml'
    original = source / 'gpl/kiln-eml'
    if not versions(original / 'Cargo.lock', [('mlua', '0.11.2'), ('mlua-sys', '0.8.3'), ('lua-src', '548.1.1')]):
        raise ValueError('source must be the retained pre-alignment baseline')
    shutil.copytree(original, gpl)
    inputs = Path(__file__).resolve().parent.parent
    runner = gpl / 'eml/mod-loader-lua/src/runner'
    shutil.copy2(inputs / 'eml_dependency/driver.rs', runner / 'm6_dependency_driver.rs')
    support = original / 'kiln-eml/tests/support/m6_fixture'
    shutil.copytree(support, runner / 'm6_fixture')
    fixture_mod = runner / 'm6_fixture/mod.rs'
    # This driver builds in mod-loader-lua: omit only kiln-eml's package/runner helpers.
    fixture_mod.write_text(fixture_mod.read_text().replace('pub mod package;\n', ''))
    shutil.copy2(source / 'tools/sfi/conformance/prelude.lua', runner / 'm6_prelude.lua')
    shutil.copy2(inputs / 'alias.lua', runner / 'm6_alias.lua')
    shutil.copy2(inputs / 'eml_dependency/bindings.lua', runner / 'm6_bindings.lua')
    hook = '\n// Test-only dependency capture in an isolated output copy.\n#[cfg(test)]\nmod m6_dependency_driver;\n'
    with (runner / 'mod.rs').open('a') as file:
        file.write(hook)
    if mode == 'candidate':
        if not versions(current / 'Cargo.lock', [('mlua', '0.11.6'), ('mlua-sys', '0.10.0'), ('lua-src', '550.0.0')]):
            raise ValueError('this tool must run from the aligned production tree')
        for name in GRAPH:
            shutil.copy2(current / name, gpl / name)
        if 'sfi-core' in (gpl / 'kiln-eml/Cargo.toml').read_text():
            repo = current.parents[1]
            shutil.copytree(repo / 'crates/sfi', destination / 'crates/sfi')
            shutil.copytree(repo / 'tools/sfi/conformance', destination / 'tools/sfi/conformance')
    provenance = {
        'mode': mode, 'originalLockSha256': digest(original / 'Cargo.lock'),
        'graphSha256': {name: digest(gpl / name) for name in GRAPH},
        'driverSha256': digest(inputs / 'eml_dependency/driver.rs'),
        'bindingFixtureSha256': digest(inputs / 'eml_dependency/bindings.lua'),
        'preludeSha256': digest(source / 'tools/sfi/conformance/prelude.lua'),
        'sourceHashes': {str(path.relative_to(original)): digest(path)
                         for path in sorted((original / 'eml').rglob('*'))
                         if path.is_file() and path.suffix in ('.rs', '.toml', '.lua')},
        'instrumentation': 'append cfg(test) runner child only; no production binding edits',
        'origin': 'hand-authored synthetic archive; real Brabb3l EML bindings, not SF oracle',
        'phase': 'startup', 'dialect': 'brabb3l-eml',
    }
    (destination / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    print(gpl / 'Cargo.toml')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('mode', choices=['old', 'candidate'])
    args = parser.parse_args()
    prepare(args.source, args.output, args.mode)
