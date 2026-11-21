<!-- SPDX-License-Identifier: MIT -->
# sfi: Kiln's interpreter of ShroudForge mods

This is a **separate MIT Cargo workspace**, not a member of Kiln's root workspace.
It uses resolver 3, Rust edition 2024, a declared Rust minimum of 1.89.0, its own
committed lockfile, and unpublished packages. Always use this workspace's manifest
(or run Cargo from this directory). No root manifest/lockfile change is needed.

- `sfi-core`: the Lua runtime, ShroudForge's mod API bindings, the lifecycle and the
  conformance runner.
- `sfi-package`: the pure package model, discovery and profiles.
- `sfi-policy`: the pure launch-plan reader and the native-DLL policy, also used by
  kiln-manage.
- `kiln-abi`: generated bindings of Kiln's C service tables.
- `kiln-sfi`: the plugin (boundary, worker, runtime families, native manager); see its
  README.

`sfi-core` currently provides the public `conformance` module:

- `run_fixture(source, builder)` creates a fresh Lua VM, loads the shared
  `tools/sfi/conformance/prelude.lua` as `=prelude` with full standard libraries,
  asks the caller for a fixture environment, injects `T`, and loads/calls
  `=fixture` protected. Lua compile/runtime errors become `fixture_error` without
  stringifying table or nil error values; host setup errors remain Rust errors.
  It returns `T.transcript()` unchanged, with no trailing newline.
- `plain_std_environment` is the current `harness` family's builder. This is a
  **trusted test host, not a mod sandbox**: full standard libraries include IO,
  debug, and C-module loading. Future API families must register their SF sandbox
  builders in `sfi-core/tests/conformance.rs`.
- `compare(a, b)` parses JSON and compares structurally, ignoring object key order
  and whitespace. The first differing record reports its sequence, label, and
  both complete values. Missing records and fixture-level errors are differences.
- `validate_transcript` reads the shared `transcript.schema.json` directly and
  checks its current structural keywords, recursively, with JSON-location errors.
  This is deliberately not a general JSON-schema implementation. It does not add
  semantic rules absent from the schema (for example contiguous sequence numbers,
  kind-specific required fields, or successful `T.expect` assertions).

The host contract is `tools/sfi/conformance/prelude.lua:6-13`; the canonical value
model is at lines 23-33. Neither shared contract file is copied into this crate.
The exact `mlua = "=0.11.6"` pin uses the features `lua54`, `vendored`, `error-send`
from `modules/mod-runtime/Cargo.toml:39` in both SF 1.7.9 (`d8a394bc`) and 1.8.0
(`38ef448d`). Their manifests permit 0.11.2 as a minimum, but both committed
`Cargo.lock` files resolve mlua 0.11.6 at lines 2286-2287, mlua-sys 0.10.0 at
2305-2306, and lua-src 550.0.0 at 2182-2183. This workspace matches those three
resolved versions; `tests/dependency_lock.rs` reads its committed lockfile and
rejects version drift or duplicate versions. The GPL kiln-eml workspace keeps
its separate dependency lock unchanged.

## Fixtures and provenance

Each fixture is `sfi-core/tests/conformance/<family>/<case>.lua`, with an optional
sibling `<case>.golden.json`. Each family's `goldens.toml` documents every golden:

```toml
[goldens]
"example.golden.json" = "oracle sf-1.8.0 38ef448d"
# "oracle <pin>": captured with ShroudForge's own mod runtime (a local differential test,
# not published). Or: "contract <file:line>" for a contract-derived expectation; "hand"
# for manual expectations.
```

The runner discovers all fixtures, requires a registered family builder, validates
both transcripts, and compares each available golden. It always counts and lists
fixtures lacking goldens. `SFI_REQUIRE_GOLDENS=1` makes any missing golden fail.
Goldens must have provenance, and provenance entries must have both files.

The eight `harness` fixtures have **hand-written** goldens, not SF oracle results.
They cover exact signed integer limits, floating-point bits (including signed
zero, infinity and NaN), fixture line numbers, non-string errors, cyclic/nested
and shared tables, multiple/zero/trailing-nil returns, fixture-level errors,
byte strings, and every fixture API. The deliberately false expectation in
`values.lua` checks that integer `1` and float `1.0` remain distinguishable; it is
not a claim that an SF API passed parity.

## MIT / GPL boundary

The new code here is MIT; see `LICENSE`. It contains no copied ShroudForge
implementation and no archive/format implementation. ShroudForge semantics and
manifest feature choices are contract citations only. There is no dependency on
ShroudForge, EML, or kfc code. Archive-dependent implementation must stay in
`gpl/kiln-eml` and cross the MIT boundary only through data-only service APIs,
not archive-parser types or source.

`tests/licence_guard.rs` runs `cargo metadata --locked --offline` against this
workspace (including the full resolved, target-independent dependency graph).
It rejects package names starting with `kfc`, `mod-loader`, `shroudforge`, or
`eml`, licence expressions containing `GPL` (including LGPL/AGPL and OR expressions),
and packages without a declared licence. It also scans every Rust source under
this workspace for a reference to the forbidden archive-parser crate, including
comments and tests. Its own forbidden token is split, with no self-exemption.
Normal dependencies are only `mlua` and `serde_json`; TOML parsing is test-only.

## Checks

Run from this directory, through the project's build limiter on a shared machine,
with build output on a data disk (`CARGO_TARGET_DIR`):

```sh
export SFI_REQUIRE_GOLDENS=1
cargo test --locked --offline --workspace
cargo fmt --all -- --check
cargo clippy --locked --offline --workspace --all-targets -- -D warnings
```

`--offline` assumes the declared sources are already cached; do not download
binaries or toolchains as part of these checks. The licence metadata subprocess
inherits the outer test's build slot; it does not claim a second slot.

Windows x64 cross-build with installed `cargo-xwin`, clang-cl/lld-link, the Rust
MSVC target, and an already populated Windows SDK/CRT cache:

```sh
cargo xwin build --locked --offline -p sfi-core --target x86_64-pc-windows-msvc
```

This builds a Rust library, not a runtime DLL, and does not execute Windows code.
A green cross-build is not a Windows runtime test.
