<!-- SPDX-License-Identifier: MIT -->
# Startup host transcript v1

This is the **one authorized startup host envelope** (the API contract's rulings, item 6), not
another Lua transcript protocol. It is MIT, has no engine/parser dependency,
and runs no game, native DLL, oracle or interpreter. Schema validity and a
synthetic tool-test match establish **no runtime/API parity**. Actual dual-pin SF
observation is O02's separate GPL test-driver work.

## Consumer interface

All paths below are repository-relative. The schema is
`tools/sfi/conformance/startup-transcript.schema.json`. Both hosts emit that
same format. Never generate an expected packet from the implementation being
tested, or use `fixtures/synthetic.json` as an SF/API golden: it is an independently
constructed tool fixture with `provenance.engine = synthetic`.

Use Python in isolated mode, with `TMPDIR` in an output directory of your
own (not under `/tmp`). From the worktree root, these are the tool commands
(run heavy ones one at a time):

```sh
python3 -I tools/sfi/conformance/startup/test_compare.py
python3 -I tools/sfi/conformance/startup/compare.py GOLDEN.json ACTUAL.json --fixture INPUT
python3 -I tools/sfi/conformance/startup/compare.py GOLDEN.json ACTUAL.json --fixture INPUT --physical semantic --allow-provenance
```

The comparator requires both packet files **and an independent fixture input**
(`--fixture`). It hashes the input without executing it and requires that digest
in both packets. A missing or changed fixture cannot pass even when both packets
agree. INPUT is the actual script or a deterministic fixture bundle; if it is an
input manifest, the caller must separately verify its constituent file inventory.
The tool does not discover optional files, supply an empty default, create/update
goldens or change packets on disk. Exit 0 prints one JSON accounting object;
mismatch, invalid packet, absent/empty packet file, duplicate JSON key and
nonfinite JSON number exit 2. Object key order and JSON whitespace are irrelevant;
every payload array remains ordered.

A Python caller can load the file using `importlib.util.spec_from_file_location`
(as the tests do), without changing `sys.path`. Public entry points are:

- `load_packet(path) -> dict`: required JSON input with duplicate/nonfinite checks.
- `validate_packet(packet) -> None`: structural and state/digest validation;
  raises `TranscriptError` on failure. This is not attestation that the producer
  really ran the source revision or hashed the files it claims.
- `compare_packets(expected, actual, *, fixture_path=None, allow_legacy=False,
  allow_provenance=False, physical='strict') -> dict`: directional comparison;
  requires a real independent `fixture_path` (None/missing/hash mismatch raises
  `TranscriptError`), as does any unaccepted difference. Inputs remain untouched.

The successful result contains `matched`, `fixture`, `v2_message_verified`,
`legacy_v1_not_message_verified`, `physical_mode`, `physical_differences` and
`provenance_differences`. Counts are **embedded transcript comparisons**, not
unique API obligations, error-message counts or proof that a missing message was
recoverable. Difference entries retain `path`, `expected`, `actual`.

| Consumer | Emit/consume |
| --- | --- |
| the publication kill matrix | Observe every publication/checkpoint/recovery boundary and its final reopened manifest; an operation error uses a shared failed-call transcript. Kill/restore scenarios must still terminate in a parent-observed coherent `archive_result`. An incomplete capture is not a passing golden. |
| the startup controller | Copy frozen ordered package/admission facts; retain attempts, invocation owner versus dependency-module owner, whole-package failure and final inclusion/exclusion reasons. Copy `executesStartup` from the plan/actual phase rule, not from whether a trace happened to be collected. A nonexecuting admitted runtime-only participant has no invented empty Lua transcript. |
| O02 | Emit actual pinned-source/dependency/contract provenance; use controlled UUIDs, clock/seed and opaque encounter-order node/lease/call/effect symbols. Run the real SF code and unchanged shared prelude. Record raw deviations rather than blessing them as new expected behavior. |
| the parity comparison | Require the exact fixture/contract/policy/baseline/plan identity, compare meaningful order/arguments/results, and inspect explicit physical/provenance/legacy accounting. Missing cases are failures in the caller's fixture inventory, not permission to compare an empty set. |

## Required envelope and provenance

`schemaVersion` is 1 (independent of Lua transcript version). Required fields:

- `fixture: {id, sha256}` identifies the fixture input, not generated expectations.
- `provenance: {engine, sourceRevision, dependencies, contracts, policy,
  preludeSha256, luaSchemaSha256}`. Source revisions are full 40-hex commit ids;
  dependencies are sorted unique `{name, version, sha256}` records, contracts
  sorted unique `{family, sha256}`, and policy is `{id, sha256}`. Producers record
  actual resolved dependencies/lock identity; schema validation cannot infer them.
  Shared prelude/schema hashes must identify the **current unchanged files**.
- `seed`, `planId`, `launchId`, `planDigest`: ids are 16-byte UUIDs in lowercase
  textual form, not PIDs, integers or pointer addresses. Producers inject stable
  fixture UUIDs (symbolic test identities); the comparator does not mask UUID
  differences. Plan and launch identity are distinct fields and compared exactly.
- `target: client|server`, frozen `dialect: shroudforge|eml|none`,
  `build: {id, executableSha256, registrySha256}`,
  `baseline: {setSha256, manifestSha256}`, positive exact-JSON `generation`.
  Dialect is not inferred again from the surviving packages or physical writer.
- `packages`: ordered `{id, version, sha256, executesStartup}` participants copied
  from the selected plan, including nonexecuting participants if observed. Package
  identities everywhere (including module owners and outcomes) preserve SF's exact
  case-sensitive 1–80 ASCII alphanumeric/dot/underscore/hyphen grammar, excluding
  exactly `.` and `..`. Leading punctuation is valid; identities are never renamed.
- `records`: nonempty ordered observations, ending with exactly one final result.

`--allow-provenance` / `allow_provenance=True` permits only **engine, source
revision and dependency** differences and returns every delta explicitly. In
particular the binding differential can compare 0.11.2 versus 0.11.6 without losing the dependency
change from its report. It does not discard changed results/errors/arguments,
contract hashes, policy, fixture, package version/content, build or baseline.
Default comparison rejects even provenance changes.

## Ordered records and typed identity

Every record has contiguous `seq`, `kind`, `phase: host|startup|recovery`, positive
`attempt` and the envelope's `generation`. Only `asset_stage` and archive-write
`effect` observations additionally accept `phase: deferred-stage`, described below.
Package observations name `package` (the **invocation/effect owner**). Events execute
serially; packages do not overlap. Execution attempts never regress across package
begins and preserve selected package order within each attempt. Attempt numbers may
have gaps; host/recovery records do not advance execution chronology. All package
admissions are terminal before execution. This is observable loader accounting,
**not** a new admission planner, service-readiness gate or DLL success inference.

The schema gives the exact fields for these closed variants:

- `admission`: package, admitted/skipped, nonempty reason. Exactly once per
  participant, in selected order, before any package begins.
- `package_begin` / `package_end`: balanced package/phase/attempt, success/failed.
  Executing participants need one `lua_transcript` with records or an original
  `fixture_error` per attempt. An error before the first instrumentation call
  retains that error without inventing Lua records. `fixture_error` is permitted
  only for a failed package; a default empty transcript is never evidence.
  Failed `T.expect` assertions always fail validation, even if both files are
  equally wrong.
- `api_call`: unique `call:N`, `api`, ordered typed `args`, `moduleOwner` separate
  from invocation owner. `api_result`: same call/context and typed `values`.
  Zero returns are `[]`; one nil is `[{"t":"nil"}]`, including trailing nil slots.
- `api_error`: same call/context and `transcript`, an **unaltered shared Lua
  transcript containing one failed call**, labelled with that `call:N` symbol.
  Capture using the shared prelude's protected call, not a separately formatted
  error string. No Python normalizer or oracle-only address mask exists here.
- `lua_transcript`: `transcript` is the unmodified shared v2 object for that
  package/phase/attempt. Optional `links: [{luaSeq, call}]` associates local Lua
  call records with completed host calls. A link does not claim coverage of
  uninstrumented API invocations or arguments.
- `value_lease`: grant/release of unique `lease:N` for opaque `node:N`. A grant
  has relation `root`, `alias`, `clone`, or `retained`; nonroot grants identify a
  live `source` lease. Aliases/retained references preserve the node; clones
  create a different new node. Releases retire only that owner's lease, not all
  aliases. Reused, stale-attempt, wrong-generation and foreign leases fail.
- `value_edge`: attach/detach a child to an opaque `slot` using `parentLease` and
  `childLease`. Detach checks the exact existing edge. The child can remain alive
  through a retained lease after detach and source-lease release. These are
  generic graph observations, not parser structure or mutation implementation.
- `effect`: unique `effect:N`, `scope: local|archive-write|external`, operation and
  target. Local effects require an owned lease and exact before/after revision
  progression. Archive-write/external effects cannot carry local revision fields.
  A successful local edit is **not** evidence that archive or engine bytes changed.
- `asset_stage`: package, opaque resource/content key/type, size, before/after
  digests, created and dirty flags. It records a private staged effect, not live
  publication. No file bytes or type-layout serialization are embedded. Use
  `phase: startup` for immediate in-execution staging (such as content creation).
  Use `phase: deferred-stage` for actual dirty-resource apply/serialization/write
  observations after entrypoints return: retain the contributing `package`, its
  completed execution's `attempt` and the same `generation`. No package may be
  active, that execution must have succeeded, and no later execution attempt may
  have begun. Archive-write `effect` records use this same deferred context. API
  calls/results/errors, Lua transcripts, lease/edge operations and local/external
  effects remain execution-bound; deferred staging cannot invent an execution or
  reauthorize stale local leases. In both SF pins, `runner.run()` completes before
  `commit_asset_changes` (`modules/mod-runtime/startup/run_startup_mods.rs:72-76`);
  the real apply/serialization/write boundary is in
  `modules/mod-runtime/startup/commit_asset_changes.rs:13-35`. Producers must not
  move those observations into earlier package scopes or drop their attribution.
- `checkpoint`, `publication_step`, `recovery`: operation id, bounded relative
  basename and file role, expected generation, size/SHA-256, success/error and
  whether live bytes changed. Basenames use ASCII alphanumeric/dot/underscore/
  hyphen (including hidden names such as `.vanilla-header`), excluding exactly
  `.` and `..`, separators and drive/path syntax. This is separate from package
  identities and the unchanged generic operation/provenance identifier grammar.
  Final file manifests use the same safe-basename definition. Error outcomes
  require one shared failed-call transcript. These are observations, not an
  instruction to perform a syscall.
- `archive_result`: prepared/published/restored/unchanged/refused, ordered
  `included` and `{id,reason}` exclusions forming an exact disjoint partition of
  every participant, ordered `files`, `physicalDigest`, sorted `semanticEntries`,
  `semanticDigest`. Included executing participants need a successful last
  completed execution in record order, not the numerically largest attempt id.

Typed args/results use a lossless neutral sum: nil; boolean; integer with explicit
width/signedness and canonical decimal text (8..512 bits); float with 32/64-bit
lowercase bit text (including negative zero and NaN payloads); binary hex strings;
node/lease references; or `t: lua` with a shared canonical value. Generic shared
Lua tables retain the shared model; identity-sensitive aggregates must use node
observations instead. Integers never pass through a floating JSON number. The
shared Lua model still has its own documented NaN/string representations.

No native address, Lua state, raw game/default bytes or parser layout belongs in
these packets. Opaque encounter-order symbols retain equality relations without
leaking addresses. Binary/scalar fixture payloads are synthetic or mod-provided
API inputs; actual archive/default bytes are represented only by digests. The
schema rejects extra fields; it cannot identify the origin of arbitrary strings,
so producers still enforce the no-game-bytes rule. Root/path/address scrubbing is
not a comparator feature: use bounded basenames and controlled fixture input,
and preserve literal hexadecimal text in error messages and arguments.

## One shared Lua error contract, including historical v1

`prelude.lua` and `transcript.schema.json` in the parent directory are the only
Lua interface. The host schema **references** that sibling schema, never copies
it. v2 errors retain canonical error identity and the prelude's optional protected
message. Strict v2 comparison checks every field, including message presence.
Only the shared prelude knows which formatter-owned pointer value slots can be
masked; literal strings/custom `__tostring`, require ids, hexadecimal values,
argument indexes and traceback/chunk text remain exact. This comparator accepts
already observed messages and does not apply even the shared mask a second time.

Historical v1 is structurally readable, but comparisons require explicit
`--allow-legacy-v1` / `allow_legacy=True`. Following
`sfi_core::conformance::compare_golden`, an expected-v1/actual-v2 comparison changes
only the **in-memory comparison copy's** version and removes only failed-call
`message`. It is counted under `legacy_v1_not_message_verified` even when there
are no errors. Expected v2 is never downgraded. No messages are invented for v1,
and all other payload, nil/arity, canonical errors and order remain strict.

## Physical versus semantic archive identity

File manifests are **ordered** `{role,name,size,sha256}` records with unique
basenames. Producers calculate SHA-256 from actual files; this tool checks the
manifest's self-consistency, not the filesystem. `physicalDigest` is SHA-256 over
canonical JSON of that array, preserving physical file/order identity.

`semanticEntries` is sorted unique `(kind,key)` records with `kind: resource|content`
and a SHA-256 of the producer's lossless semantic content. Keys are neutral opaque
identifiers, not MIT parser layout declarations. The producer must use the same
semantic encoding on both sides. `semanticDigest` hashes canonical JSON of this
sorted array. This tool does not parse archives, derive semantic hashes or sort
API/catalog results.

For both digest calculations, canonical JSON means sorted object keys, compact
`,`/`:` separators, ASCII JSON escaping (`ensure_ascii=True`), no nonfinite JSON
numbers, UTF-8/ASCII bytes and no trailing newline. See `digest()` and the
independently constructed synthetic fixture for executable examples.

Default `physical='strict'` compares everything. Explicit `--physical semantic`
allows differences **only** in final `files`/`physicalDigest`, reports all of them,
and still independently validates each manifest/order digest and semantic sorted
digest. Publication/checkpoint/stage records and meaningful API/catalog order
are not erased. An infrastructure scenario whose operation sequence intentionally
differs needs its own reviewed expected packet, not blanket event filtering.

## Validation and obligation links

`compare.py` implements a closed schema subset patterned after the core validator
(`crates/sfi/sfi-core/src/conformance.rs:223-359`), reading the current schema files
at validation time. Only local references and the shared sibling schema resolve;
there is no network resolver or optional downloaded validator. Additional host
checks cover ordering, owner/context/lease/edge lifetime, ranges, shared record
output shape, no failed test expectations, and digest/accounting consistency.
It is not a general-purpose JSON Schema implementation. A schema keyword outside
its supported set is an error, not a silently ignored constraint.

The tests are tool assertions linked to FINAL obligations; none promotes an API
row to Full. SF179/SF180 below denote the pinned read-only source trees, revisions
`d8a394bcb180ceb5bdfce48048dcc4f2e0108bd8` and
`38ef448dc5d4fc267b5f0928502a864a6b078865` respectively.

| Tool assertions | FINAL obligation / phase / source rationale |
| --- | --- |
| error-message, nil/return, literal-hex and v1/v2 tests | `lua-env-lifecycle` argument/errors/require; both phases; both pins `modules/mod-runtime/lua/errors.rs:16-38,91-95`, `require_mod.rs:70-175`; actual shared prelude/runner regression is authoritative for formatting |
| width/bits/binary/catalog-order tests | `eml-assets-types` typed values and resource enumeration; both phases; both pins `modules/mod-runtime/lua/resource_values/mod.rs:30-74`, assets binding order is preserved rather than globally sorted |
| alias/clone/retained edge and lease tests | `eml-assets-types` Resource.data and typed table operations; local both phases; both pins `modules/mod-runtime/lua/resource_values/mapped_values.rs:385-405,424-468,499-525`; fork owned-setter fix remains a producer expectation, not normalized away |
| effect scope, stage and independent physical/semantic tests | `eml-assets-types` Resource.data/create_resource startup apply versus local edits; both pins `modules/mod-runtime/startup/commit_asset_changes.rs:5-36` and `startup/run_startup_mods.rs:58-94` |
| package/admission/attempt/failure/provenance and disjoint-outcome tests | `package-model` selected order and `lua-env-lifecycle` package execution; startup; both pins `modules/mod-runtime/startup/run_startup_mods.rs:58-76`; the API contract's rulings 2, 6, 7 supersede historical admission assumptions |
| leading-punctuation identity and length/traversal tests | FINAL `package-model` `mod.json:id`; manifest/startup attribution; both pins `modules/modloader/manifest/manifest.schema.json:13-16`, `package_lookup.rs:47-52`, `read_mod_manifest.rs:140-141` |
| hidden-basename and deferred two-package stage tests | startup host observation/infrastructure only; both pins `modules/mod-runtime/startup/run_startup_mods.rs:72-76`, `commit_asset_changes.rs:13-35`; Kiln's private `.vanilla-header` at `gpl/kiln-eml/eml/mod-loader-lua/src/load.rs:69-73`, `transaction.rs:8`; no SF transaction infrastructure is ported |

No SF implementation or GPL/parser code is adapted here. Source citations explain
the observations required from future producers; they are not oracle execution.
This pure tool needs no game-only assumption that a game run would have to prove.
