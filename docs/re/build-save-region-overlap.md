# Experimental native save-region overlap filter (issue #136)

## Status

**Draft, opt-in, no migration.** This patch addresses the **redundant** subset of synthetic `KILNBILD` save regions. It does not redesign off-base save persistence and is **not enabled by default**. It needs maintainer review of the early-save durability tradeoff before any general rollout.

### Observed failure

Kiln's existing persistent terrain writer records all 16 m chunks touched by an edit in `kiln/data/kiln.build/regions/<world GUID>.json`. The `build/SaveRegions` detour appends synthetic boxes covering these chunks to the game's base-save region array. The serializer uses complete chunk intersection with region AABBs. The game world-selection UI then displays the synthetic records as additional Flame Altars.

In a controlled disposable world with one level-2 (120 m) altar, removal of 39,831 rubble voxels inside its genuine base kept 113 chunks and generated 20 synthetic boxes, resulting in 21 displayed altars. A read-only native probe showed that 113/113 chunks overlapped the original 120 m base-save region, so the extra boxes were redundant. The live `BuildZoneSystem` contained one real base.

A startup complication matters: the original region was first 40 m (23/113 chunks covered), then became 120 m (113/113 covered) **48 ms later**.

## Behaviour of this candidate

- Opt in to exactly one world at a time by writing its full world GUID to the first line of `kiln/data/kiln.build/regions/overlap-filter-world`. Leave that file absent for the unmodified serializer behaviour. The source contains no hardcoded world GUID.
- After the game's `build/SaveRegions` builder returns, inspect its original region bounds (before Kiln's own entries).
- Wait for the original region bounds to remain unchanged for **1,500 ms** (with at least two observations). If they change, restart the guard. During the guard, inject **zero** synthetic entries in the opted-in world.
- Once stable, include only retained 16 m chunks that **do not touch any original native region**; greedily merge those chunks into boxes and append the result.
- If original region bounds cannot be validated/read, preserve existing behaviour by injecting the complete legacy box list (fail-safe for persistence).
- For all worlds not matching the marker, the original append logic remains in place.
- Neither `g_chunks` nor the per-world JSON is filtered, removed or migrated. Later base shrinkage/removal can require extra save coverage again.
- Optional marker `kiln/data/kiln.build/regions/diagnose-regions` produces read-only `save regions PROBE` diagnostics. Opted-in filtering logs `save regions FILTER` transitions.

## Verified evidence

In the exact Kiln **0.2.0-dev.30.8** client build, a release-stamped `kiln-build.dll` compiled from the same implementation (fork PR [bisdat/kiln#7](https://github.com/bisdat/kiln/pull/7)) passed **37/37** portable save-region tests. The player reported **one** displayed altar after the change and confirmed the earlier edited terrain remained removed after a reload.

Test filter log:

```text
14:41:52.413 state=settling genuine=1 retained=113 covered=23 outside=90 injected=0
14:41:52.458 state=settling genuine=1 retained=113 covered=113 outside=0 injected=0
14:41:53.968 state=active genuine=1 retained=113 covered=113 outside=0 injected=0
```

Retained JSON SHA256 matched before and after: `19e10e7a31e92ea0b04a90f42c563377c159715e8f4c8e7fcc95b5f69a9a9c5a`.

The source in this upstream candidate has been rebased onto fork `main` (matching upstream `main`), and host tests were extended to check that a **bound** test world actually writes the original JSON before verifying it remains unchanged. This additional test refinement has **not yet been re-run** in Distrobox.

## Test the host code

```bash
bash native/services/build/tests/test-transient.sh
```

The build service's Windows hook is not exercised by those portable tests.

## Outstanding review / important durability risk

- **Early-save vulnerability**: During the 1.5 s stabilization guard no synthetic region is injected, even if a retained chunk is genuinely off-base. An immediate save/crash could lose those edits. This is the main reason for the explicit opt-in. We should resolve that risk or select a stronger guard before enabling by default.
- With truly off-base edits, a synthetic region is still required under the present serializer design. This patch cannot stop all apparent false altars while retaining off-base write persistence; a different representation/path may be needed.
- Multi-base, no-base and out-of-base in-game test cases have **not** been completed. The original important world remains untouched.
- Once a game save already contains synthetic entries, the patch does not directly rewrite saved binary data. The affected disposable world reported a corrected count after the candidate's normal save/reload, but no general migration guarantee is implied.

## Acceptance gates before production

1. Reviewer confirms correct native save-region bounds semantics and early-world lifecycle (40 m → 120 m).
2. Fail-safe handling for genuinely off-base writes during the stabilization window.
3. Host tests on the final changes, Windows service build, game tests across multiple altars and off-base edits.
4. No accidental write to the original `regions/<guid>.json` metadata or loss of previously persisted edits.
5. Rollback with a single original Kiln `kiln-build.dll`.

See [upstream issue #136](https://github.com/frostdev-ops/kiln/issues/136) for full reproduction evidence.
