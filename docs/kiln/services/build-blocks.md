# Block queries and controlled breaking

Since 0.2.0-dev.30.6. These are public Kiln APIs, available to any mod through `Kiln.Build` or the appended
`kiln.build` v1 rows. The semantics are informed by EMF v44–v47 (MIT, matenm); EMF compatibility translates onto
these services rather than owning another engine implementation.

## Thread and authority

Call `Build.BlockAt`, `Build.FindWall` and `Build.BreakBox` on the **server tick, in Pre or Post**, not from a world
program or task thread. A dedicated server or a hosting client is authoritative. A client joined to a remote host
cannot read or break that host's world. No implicit task posting, streaming wait, queue or callback is hidden in these
calls. Use the existing `Build.Load` before querying an unloaded area. Native rows return `KILN_ERR_WRONG_THREAD`
outside the tick; managed calls enforce the same thread guard as other Build methods.

## Facts at a point

```csharp
var q = Build.BlockAt(new WorldPosition(3024, 903, 1552));
if (q.Info is { } block)
{
    Log.Info($"{block.Kind}, raw material {block.Material}, {block.Provenance}, health {block.Health}");
}
else if (!q.Ok) Log.Info(q.Reason);
```

`BlockInfo` reports `Kind` (`Air`, `Terrain`, `Building`), raw `Material` and `Fill`, `Center` in world metres,
`Provenance`, nullable `Hardness`, `Health` and `Damage`. Building material ids are **128–255**, not EMF's 0–127.
Terrain ids are 1–127. Building fill is paint (zero paint is still a building block); terrain with zero fill is air.
Hardness is Soft through VeryHard (0–4), or Unbreakable (5). Health is the material config's raw health points.
Unknown mining facts are null, not guessed. Building damage reads the game's native crack state; terrain damage
reports Kiln's accumulated policy value when its session clock is available (no native terrain crack visuals).

Provenance means **comparison with the shipped world**, not an owner id, a paint bit or server/client side:

- `Natural`: a building has the same material as the shipped cell; terrain had any filled terrain in the shipped cell.
- `PlayerPlaced`: a different building material, or terrain where the shipped cell had no filled terrain.
- `Unknown`: the baked column is not loaded, its reader is unqualified, or the origin cannot be established.

A ruin rebuilt with its original material is therefore Natural again. Digging, repainting or refilling natural terrain
never makes it PlayerPlaced. Reads copy baked bricks under the game's own column lifetime and release the loader
reference **and** world lock before returning. Copies are cached only within the operation, never across sessions.
A fault in that lifetime disables further voxel writes and requires a process restart; generic recovery of an engine
lock interrupted inside acquire/release is not claimed.

## First wall

```csharp
BlockQueryResult wall = Build.FindWall(from, to, FindWallFlags.PlayerTerrain);
// wall.Ok && !wall.Found: clear line. !wall.Ok: unavailable/refused, not proof of a clear line.
```

Default: first building cell crossed, including the start cell. `PlayerTerrain` also stops at player-placed terrain,
passing over Natural and Unknown terrain. `Terrain` explicitly includes all terrain. The segment is limited to its
first 64 metres; cell stepping is deterministic in x/y/z tie order, not a qualified game raycast. An unavailable
provenance reader refuses the complete PlayerTerrain mode instead of treating unknown ground as player-placed.

## Breaking an exact box

```csharp
var options = new BlockBreakOptions
{
    Flags = BlockBreakFlags.Instant | BlockBreakFlags.PlayerTerrain,
    Build = new BuildOptions { Flags = BuildFlags.RespectZones | BuildFlags.InBases, Actor = player },
};
BlockBreakResult r = Build.BreakBox(box, options); // at most 4096 loaded cells
if (!r.Ok) Log.Info(r.Reason);
```

`Selection` is an optional bool per cell (x fastest, then y, then z). `Materials` selects raw voxel ids; empty means
all breakable materials. `Damage` is empty or five integers 0–100000 by hardness; empty/all-zero selects the T4 pickaxe
values 100,100,95,60,50. Default flags request damage mode, **not** unrestricted instant removal.

- Default protection spares Natural and Unknown buildings. `WorldBuildings` explicitly includes them.
- `PlayerTerrain` includes only PlayerPlaced terrain. **Natural and Unknown terrain are never removed**, even with
  `WorldBuildings`. Without it all terrain stays.
- Unknown material facts and Unbreakable materials are always spared, including in Instant mode.
- Any requested mode without a qualified implementation refuses the **entire call before mutation**. Drops remain
  unavailable (`SymbolMissing`): no guessed item refunds or unrestricted edit is used.
- Building damage uses native Get/Add crack state, `floor(255 * damage * miningDamageFactor / health)` per hit,
  capped at 255. The game owns its 20-second healing and crack replication. A required setting/clock/symbol that
  cannot be read refuses damage mode, never assumes a mining factor of one.
- Terrain has no native crack state. Kiln maintains a bounded (65536-cell), per-session accumulated damage map, with
  `TerrainHealthScale` (default **1**) multiplying material health and `TerrainHealAfter` (default **20 seconds**)
  controlling inactivity expiry. The caller chooses both; an adapter requiring a 250-fold health scale passes 250
  explicitly. Values expire, clear on world unload/session change/material change, and are forgotten after verified
  removal. There are no terrain crack visuals. Full-map admission can fail; no rollback of prior hits is promised.
- Before any damage mutation, the whole candidate mask is dry-run through the guarded writer's policies. A refused
  preflight damages nothing. Once damage starts, a later failure may leave both damage state and committed cells.

`ProtectedSphere = new BlockBreakSphere(center, radius, margin, minimumRadius)` selects a sphere and protects nearby
ruins by shrinking the **whole building radius** away from the nearest eligible Natural/Unknown building cell within
radius+margin. The reduced radius is min(original, distance-margin); below minimumRadius no buildings are hit.
The margin and minimum default to zero; callers choose their policy. The scan ignores the selection mask but respects
material/hardness/damage filters. Terrain retains the original sphere, even when buildings are skipped. WorldBuildings
bypasses shrinking, not the original sphere selection. The protection scan must fit 4096 loaded cells. This follows
EMF's `block_rules.cpp:71-92` policy, with caller-controlled parameters rather than compatibility constants in core.

Removal uses kiln.build's guarded exact-cell writer, not `VoxelBoxFillAir`. Both reach the game's block writer and
store commit, but the guarded writer preserves Kiln's base/zone checks, transient journal and save regions, requests
loaded tiles and keeps every unselected cell unchanged. The raw fill helper can clean isolated neighbouring terrain
outside its nominal box even without growth, and is deliberately not exposed as a shortcut.

The existing Build flags, actor, region persistence and transient rules apply. `Unloaded` and `AllowAltars` are invalid
for this API. Policy preflight refuses the whole box. Managed calls post the existing Write editing/edit events and
journal observable changes for session undo. Cancellation happens before the native call. A native caller uses the
same writer policies but does not invoke managed events.

### Results, partial commits and refusal

`BlockBreakResult` has Considered, Damaged, Removed, Dropped, ProtectedCells, UnknownCells, UnbreakableCells and
ExcludedCells. Spared cells are counted, not silently treated as removed. `BlocksWritten` and `Kept` mirror the
writer's report. `Refusal` distinguishes wrong side, missing symbol, offline, no session, wrong thread, bad arguments,
unloaded tiles, zones, bases, faults and failed verification.

**No rollback or compensation is promised.** A writer may commit earlier blocks before a fault, and its void engine
commit can return without storing the requested cells. `MayHaveChanged` means the writer or damage mutator was entered and partial
changes may remain; never automatically repeat the whole operation or give drops from planned counts.
`Removed` counts only selected cells read back as air; `Verified` says that readback completed, not that every planned
removal succeeded. Without Verified, Removed is only a lower bound. Failed readback or incomplete removal returns
Unverified; a game fault returns Faulted. Inspect these results even when the native return is not OK. Native
`result.write.cells_changed` may be a **planned** count after failure, not an observed count. Kept is not proof that
partially committed blocks after a fault acquired save-region bookkeeping.

## Capabilities

| Row | Meaning |
|---|---|
| `build.block_info` | raw loaded-cell query, server-session row |
| `build.block_metadata` | qualified material hardness and health lookup |
| `build.block_provenance` | qualified baked-world reader and balanced lifetime |
| `build.find_wall` | cell-stepped first-wall query, server-session row |
| `build.break_building` | instant building removal with qualified metadata and guarded writer; default protection also needs provenance |
| `build.break_damage` | native building damage/cracks, caller-policy terrain accumulation and mining settings; server-session row |
| `build.break_drops` | qualified material drops; currently SymbolMissing |
| `build.break_terrain` | instant player-terrain removal with provenance, metadata and guarded writer |

Session rows report WrongSide for a joined client, Offline without a game provider and Session while waiting for a
running authoritative world. Static engine prerequisites report SymbolMissing when unqualified; a provenance-lifetime
fault reports Faulted/restart-required. Optional flags can need more capabilities than a method's base attribute.
Missing/older native tables refuse without reading beyond their header.

Native layout: `block_at` at 160, `find_wall` at 168, `break_box` at 176; table 184 bytes, version still 1. Check header
size before each row. Records: `KilnBuildBlockInfo` 64 bytes, `KilnBuildBreak` 184, `KilnBuildBreakResult` 136 (Windows
x64). No mod API version stamps are set by these additions.

## Guided game check

The owner runs these checks in a disposable copied world; these APIs have not been verified in a running game here.

1. Query unpainted player buildings, a ruin, natural ground and terrain placed in air. Compare raw material, paint,
   center, hardness/health and provenance. A repainted/refilled natural-ground cell must remain Natural.
2. Trace through ground to a building: default skips terrain, PlayerTerrain hits only placed terrain, Terrain hits
   natural ground. Check the start cell, diagonal boundaries, 64-metre limit and an unloaded area.
3. Instant-break a masked box containing player building, ruin, placed terrain and natural ground. Verify only eligible
   selected cells disappear; check an isolated neighbouring terrain cell is unchanged. Repeat without InBases and with
   a denying RespectZones actor: the complete edit must be refused.
4. Observe removal from a second vanilla client, save/restart and verify kept changes. Confirm surrounding cells and
   props remain; a failed/partial result must never claim rollback. Session undo restores only unchanged journal cells.
5. Hit a player building repeatedly: byte damage floors per hit, native cracks replicate, incomplete damage heals
   after about 20 seconds, and removal occurs only at 255. Repeat with a changed miningDamageFactor. Protected zones
   must refuse before cracks change. For placed terrain use two different health scales/expiries: accumulation differs,
   heals at the chosen interval, has no crack visuals and resets on world unload. Drops requests must still refuse
   before any hit/removal. A remote joined client must refuse server operations; stopping/rehosting a world must not
   reuse old provenance copies or terrain damage. ProtectedSphere must spare player buildings beyond a ruin while
   terrain still receives the full original sphere.
