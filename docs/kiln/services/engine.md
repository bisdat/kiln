# kiln.engine: exact-build engine profiles and the engine's own gates

`kiln.engine` (`native/services/engine/`, `kiln-engine.dll`) knows which exact game build runs and holds Kiln's per-build
engine profiles. Kiln services whose members read engine memory from the caller's thread, or write it under the engine's
own gates, build on it. The per-build profiles and the exact-image selection are adapted from ShroudForge (MIT,
Copyright (c) 2026 bonsaibauer; see `CREDITS.md`).

It has two tables:

| Table | Header | Who | What |
|---|---|---|---|
| `kiln.engine` v1 | `native/core/abi/services/kiln_engine.h` (public, append-only, in `kiln-abi`) | anyone | `identity`, `gate_stats`; data only, any thread |
| `kiln.internal.engine` v1 | `native/services/engine/include/kiln_engine_native.h` (Kiln-internal) | Kiln's own services only | the selected profile's validated layout; later the owner/ECS views, guarded copies and the engine's gates |

## Exact builds only

A profile applies only when all of these hold (exact executable identity, no structural fallback):

1. the exe file name is the profile's target (`enshrouded.exe`, `enshrouded_server.exe`);
2. the sha256 of the exe file (the core's, `KilnBuildInfo.exe_sha256`) is the profile's;
3. the loaded image's PE `TimeDateStamp` and `SizeOfImage` are the profile's (`SizeOfImage`, not the file length);
4. `<exe>.kfc` starts with `KFC4`;
5. exactly one ported profile matches.

Otherwise `identity().status` is `KILN_ENGINE_UNKNOWN_BUILD` (or `UNIDENTIFIED`/`AMBIGUOUS`), the rows below are
unavailable, and every engine-reading member of every service returns its own unavailable shape. A new game build needs a
new profile; nothing structural stands in.

Capability rows: `engine.profile` (an exact profile matched; text: its id), `engine.ecs_read` (owner and ECS read layouts
validated), `engine.ecs_write` (the original native-thread Scene write gate enabled). Unavailable rows carry
`symbol-missing` and the reason in their text. Per-call readiness (a world loading, the server thread stopping) stays
dynamic: each call says why it refused.

## Profiles

`sigdb/engine/<side>/<steam build>.json` are game-build profiles adapted from ShroudForge (MIT), copied verbatim
(`sigdb/engine/README.md`). `tools/engine/generate.py` turns them into
`native/services/engine/profiles.g.h` (committed; `--check` fails on drift). It applies the profile rules at build time, on
that immutable data: eligibility (`select_resource_profile.cpp:5-13`), the resource layout
(`parse_resource_layout.cpp`) and the engine layout (`configure_engine_layout.cpp`); a profile that breaks a rule fails the
build. `tools/engine/test_generate.py` mutates the real profiles and expects the refusal for each rule. `tools/engine/validate.py <profile> <copy of the exe>` checks a profile against a read-only copy of its
executable (never the installed game, never run): hash, PE stamps, file name, the change tracker's guard bytes at its RVA,
every code RVA a `.pdata` function start, the thread registry in `.data`, vtables in `.rdata`, the thread names.

## Kiln-internal tables

`kiln.internal.engine` follows the Kiln-internal table rule ([README.md](README.md#kiln-internal-tables)): only
kiln/services DLLs of this very build publish or resolve it. Consumers check `header.version` for an exact match,
`header.size` for each member they call and the table's own record sizes (`kiln_engine_native()` does all three).

## Tests

```sh
KILN_OUT=/path/to/build native/services/engine/tests/run.sh          # host: generator, C layout, identity, DLL entry (ASan/UBSan and TSan)
KILN_OUT=/path/to/build native/core/tests/test-service-access.sh     # the core's kiln.internal.* rule (real api.cpp block)
native/build.sh kiln-engine engine-host
KILN_OUT=/path/to/build WINEPREFIX=/abs/prefix native/services/engine/tests/wine.sh   # the real DLL under headless Wine
```

The Wine host proves the Windows facts (PE headers, exe name, `.kfc` header) on a stand-in exe, which is always an
unknown build. Matching the real game builds is proven statically (`validate.py` on the copies); the game itself is the final check.
