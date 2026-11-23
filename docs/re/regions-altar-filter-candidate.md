# Candidate: exclude native-covered Kiln terrain save chunks (World 8 ONLY)

This is a fork-only, **opt-in behavioural experiment**, separate from diagnostic PR #6. It is NOT a general-purpose fix, not a proposal to upstream Kiln, and not ready to merge.

## Reproduction and reason

In a disposable world with one real level-2 Flame Altar (120 m cubical build zone), Base Cleanup removed 39,831 rubble voxels using persisted Kiln.Build writes. Kiln retained 113 16 m chunks and generated 20 synthetic `KILNBILD` save regions. The game showed 21 apparent altars. A read-only fork probe measured:

- An initial original native region of **40 × 40 × 40 m**, and 23/113 chunks with native save coverage;
- 48 ms later, an original region of **120 × 120 × 120 m**, with 113/113 chunks covered;
- The actual `BuildZoneSystem` still contained **one** real base in both observations;
- The old serializer hook appended all **20** synthetic regions regardless.

The game serializes complete 16 m chunks touching a save region, not only the cells inside that region.

## Candidate implementation

`native/services/build/regions.cpp`:

- A marker named `kiln/data/kiln.build/regions/overlap-filter-world` opts a **single world GUID** into the filter. The marker's first line must exactly equal that world's GUID. Other worlds use the original unmodified append algorithm.
- Immediately after the game's original save-region builder, inspect its original region bounds.
- During a **1,500 ms continuous stable-region observation window**, omit synthetic append entries for the opted-in world. This is meant to avoid writing temporary small startup altars into game saves. Each original region-size/geometry change restarts the window.
- When stable, generate synthetic boxes only from kept 16 m chunks that do **not** intersect any original native save region.
- Any inability to read valid native bounds reverts to the pre-existing append behaviour, not a partial guessed filter.
- The source `regions/<world>.json` chunk set is **never filtered, cleared or migrated**. If native zones are later removed or shrink, extra save coverage can be regenerated after stabilization.
- The `diagnose-regions` marker is still supported, recording native region and active base counts alongside actual injected box count.
- The host regression covers startup 40 → 120 m transitions, exact chunk-touching semantics, off-base chunks and unchanged saved JSON.

**Known safety trade-off:** During the 1.5 s stabilization window, genuinely off-base writes in the opted-in world might not be saved by a concurrent game save. This is why the experiment is opt-in and World-8-only; do **not** enable it on any important world or use it for long-term off-base building. The normal game may subsequently save again, but the candidate does not guarantee immediate preservation during this early window.

**Not yet proven:** Preventing new injected regions may not remove already-serialized `KILNBILD` save segments from the game save. Do not assume a pre-existing affected world is healed.

## Host tests

From a checkout of this branch, in the existing `kiln-build` Distrobox:

```bash
distrobox enter kiln-build -- bash native/services/build/tests/test-transient.sh
```

This host suite does not test the Windows hook or in-game save semantics.

## Safe game test: disposable World 8 only

1. Completely exit Enshrouded. Restore the earlier read-only probe DLL using its own restore script if still installed.
2. Build **only** `kiln-build.dll` from the installed Kiln 30.8 release source plus this branch's `regions.cpp`, retaining release-matching build stamp `0.2.0-dev.30.8+20261123.2fa72a6`.
3. Back up the installed service DLL and verify its stamp before replacing it. Preserve World 8's region JSON; do not delete/release any chunks.
4. Write the World 8 GUID as the single line of `overlap-filter-world`. Create the separate read-only `diagnose-regions` marker.
5. Load **only** World 8, remain in-game at least a few seconds, exit to menu, then quit. Do not run a cleanup or other terrain-writing mod during this test.
6. Expected candidate logs: `FILTER ... state=settling ... injected=0` around the short 40 m startup, followed by `FILTER ... state=active ... covered=113 outside=0 injected=0`.
7. Inspect apparent altars **after a save and a second load**, and check that earlier cleanup remains removed in-game.
8. Restore the exact original 30.8 `kiln-build.dll` and remove the test markers. If anything differs, stop; preserve logs and world data for analysis.

**Acceptance target:** one genuine altar displayed, only one live base, rubble removal persisted, and zero synthetic entries once original bounds have stabilized. Failure on any one criterion blocks merging or rollout to a genuine world.

## Important

Do not merge the branch, change upstream, or run this diagnostic on the original important world until World 8 is independently accepted.
