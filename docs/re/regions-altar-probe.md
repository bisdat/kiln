# Save-region / Flame Altar overlap probe (fork-only, read-only)

## Why this exists

On an Enshrouded test world containing one genuine 120 m Flame Altar, Base Cleanup v0.3.5 successfully removed terrain voxels via Kiln.Build. Kiln then stored 113 retained 16 m chunks, merged them into 20 synthetic save regions, and the game's menu reported 21 altars. A separate established world reported 2 genuine altars plus 39 synthetic regions. Moving the retained-region JSON aside made the extra boundaries disappear.

A retrospective check using the test world's actual altar centre and 120 m zone found all 113 registered chunks *intersect* the genuine altar AABB. Because the game's serializer persists whole terrain chunks touching each region, those synthetic entries appear redundant for this specific case. This is a candidate explanation, **not** proof that suppressing them fixes every effect.

## Probe implemented on this research branch

`native/services/build/regions.cpp` has an opt-in observer inside `hk_save_regions()`. The observer:

- captures the genuine save-region bounds immediately after the game's original builder returns, **before Kiln appends anything**;
- reports the original region count, the active `Build.Bases` count when callable on the tick thread, retained chunks, and chunks intersecting/lying outside the genuine save-region AABBs;
- logs up to four genuine AABBs and up to four synthetic chunk boxes to inspect the two representations separately;
- logs on signature changes instead of on every call (the builder may run hundreds of times);
- leaves the original region array, all keep/release semantics, world files and region JSON unchanged.

Enable only for a test boot by creating an empty marker at:

```text
<Enshrouded game>/kiln/data/kiln.build/regions/diagnose-regions
```

Bazzite example (with game closed):

```bash
GAME="$HOME/.local/share/Steam/steamapps/common/Enshrouded"
mkdir -p "$GAME/kiln/data/kiln.build/regions"
touch "$GAME/kiln/data/kiln.build/regions/diagnose-regions"
```

Start with the **fork-compiled diagnostic kiln-build.dll**, not an unchanged official release. The marker alone does nothing on an official Kiln binary. Use a disposable world, trigger a normal save, and inspect `kiln/logs/kiln.log` for `save regions PROBE:` lines.

To stop diagnostic logging (preferably with the game closed):

```bash
rm -f "$GAME/kiln/data/kiln.build/regions/diagnose-regions"
```

Do **not** delete any existing world JSON as part of enabling/disabling the probe.

## Interpret results

- `native_regions`: regions produced by the unmodified game builder.
- `live_build_zones`: count returned by Kiln's real `BuildZoneSystem` adapter; `-1` means unavailable on that invocation.
- `touching_native_regions`: Kiln-kept 16 m chunks whose AABB intersects **at least one** game's original save AABB, including touching boundaries.
- `outside_native_regions`: Kiln-kept chunks without such an intersection.
- `injected_boxes`: how many synthetic `KILNBILD` regions were actually appended.

All overlap counts are diagnostic estimates derived from the genuine region AABBs. They are not proof that an individual saved game blob is complete. The helper deliberately treats touching intervals as overlapping, corresponding to the documented serializer behaviour; game validation is still necessary.

**Expected test-world observation after cleanup**: one genuine region, 113 retained chunks, 20 appended boxes; potentially all 113 intersect the genuine region. A discrepancy between `native_regions` and `live_build_zones` is especially useful to narrow the downstream issue.

## Next work (not in this branch)

1. Verify whether the actual active `BuildZoneSystem` list grows, or whether only saved `KILNBILD` region records affect the menu count and visible boundaries.
2. Implement a separately reviewed filter **only for chunk records demonstrably already persisted by the genuine base regions**, preserving all original JSON records. Consider partial overlap and removed/resized altars.
3. Verify normal terrain persistence for edits inside and outside bases, including vanilla starts and restore paths.
4. Test on the disposable world before touching historical saved worlds. An existing world may already contain synthetic region blobs.

This branch makes **no game-behaviour changes** and is not an upstream Kiln submission.
