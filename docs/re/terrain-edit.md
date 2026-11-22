# Terrain queries, provenance and block breaking

Static qualification for client Steam **26430625** / svn 1287369,
SHA-256 `5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77`, and dedicated server
Steam **26430253** / svn 1287369, SHA-256 `f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2`.
All addresses below are RVAs; image base is `0x140000000`. Each relevant function was separately decompiled in an
owned, headless Ghidra 12.1.2 project and checked against GNU objdump 2.47 instructions. Kiln's reflection reader
independently extracted the metadata types from **both** executables. Neither executable was run.

EMF 0.6.19 (MIT, matenm) identifies the requested semantics, not the authority for engine addresses or layouts.
`sigdb/symbols/server.d/build-terrain.json` supplies shared exact-build anchors; the existing
`tools/TerrainConfigLookup` and `queries/VoxelRay` anchors are reused. New record entries are **static**, not runtime
verification. Sigdb features describe resolution, not authorization to bypass the build service's write policy.

## 1. Material facts: qualified

| Symbol / role | Client | Server | Win64 contract |
|---|---:|---:|---|
| `tools/TerrainConfigLookup` = TerrainConfigFind | `0xe9f270` | `0x9464b0` | `const byte*(TerraformingEfficiencyRegistry*, u8 terrainId)` |
| preceding terrain exchange-rate getter | `0xe9f210` | `0x946450` | `u32(registry*, u8 terrainId)` |
| `build/BuildingConfigFind` | `0xe8c9b0` | `0x933af0` | `const byte*(TerraformingEfficiencyRegistry*, u8 buildingId)` |
| building coefficient builder | `0x491dd0` | `0x2292f0` | inspection anchor |
| terrain coefficient builder | `0x491ee0` | `0x229400` | inspection anchor |

The registry's `+0x38` pointer names its resource. Its first BlobArray has relative data offset at `+0`, count at
`+4`, terrain stride **0xb8**; the building BlobArray is at `+8`, count `+0xc`, stride **0xb4**. The two finders
return the matching record pointer or null. Input order is **registry in RCX, id in DL**, not reversed. Terrain ids
are raw cell materials 1..127; building ids are raw material minus 128. A building cell's high byte is appearance,
not occupancy: raw material >=128 is building even when its high byte is zero.

The sibling is not a config finder: it returns the **u32 field at config+0x18**, or integer **16** if missing.
The exact EMF pattern's `74 11` branch distinguishes the pointer finder; wildcarding that branch displacement
matches both functions. The existing Kiln pattern additionally includes the pointer-or-null return.

Both reflected config types agree on these fields (also the four `build/block_config.*` constants):

| Field | Offset | Type / interpretation |
|---|---:|---|
| `item` | `0x04` | 16-byte ItemReference GUID; building `materialItem`, terrain `terrainItem` |
| `hardness` | `0x14` | u8 HardnessCategory: Soft=0, SlightlyHard=1, ModeratelyHard=2, Hard=3, VeryHard=4, **Unbreakable=5** |
| `loot_rate` | `0x18` | **u32**, removed blocks/terrain units per loot item; not a float or health |
| `health` | `0x1c` | i32 healthPoints |

The native coefficient builders consume hardness and health at these offsets. Unknown config, malformed facts or
hardness >=5 must never become an unrestricted break. Obtain the **current server world's** registry on its Pre/Post
tick; do not retain config/resource pointers across registry replacement or world teardown. The client executable
contains this server-world code for hosting; that does not authorize edits in its client/prediction worlds.

## 2. Exact box clearing: qualified engine path, not the chosen API writer

`build/VoxelBoxFillAir`: C **0xa9aac0**, S **0xa453c0**:

```cpp
void fill_air(void* editor, void* undo_or_null, void* allocator,
              float* mutable_aligned_box8, const SpanBoxes* exclusions, uint8_t grow);
// box8 = {lo.x,lo.y,lo.z,pad, hi.x,hi.y,hi.z,pad}; SpanBoxes = {pointer,u64 count}
```

The discovery call is in the **building-mining kernel**, not the terrain-only carve kernel. Unique anchor begins at
C `0x496e10` / S `0x22e370`; its call opcode is +28, rel32 displacement +29. Capturing that target avoids a generic
entry prefix that matches **three functions on each build**. This is the call-at/rel32 discovery EMF describes.
The observed caller supplies null undo, the VoxelWorld singleton's first qword as allocator, its second qword as
editor, a mutable 16-byte-aligned box and an empty `{null,0}` exclusions span.

`grow != 0` **mutates the input box** by subtracting/adding 0.5 m and enables special building boundary treatment.
`grow == 0` does not expand it. Conversion clamps coordinates to >=0, multiplies by 2, floors low and ceils high.
The fill kernel clears cells in the resulting lower-inclusive/upper-exclusive integer box; empty exclusions skip
nothing. Use cell-aligned finite boxes and validate coordinate range before entry. Do not mistake clipping of a
negative box for supported negative-world editing, or use a const/unaligned box.

Call path, independently checked on both builds:

| Role | Client | Server |
|---|---:|---:|
| box dispatcher | `0xa89450` | `0xa33cf0` |
| allocation/read preparation | `0xa94a00` | `0xa3f300` |
| clearing kernel | `0xa91760` | `0xa3c060` |
| `build/VoxelWrite` | `0x10bb030` | `0x83fa70` |
| `build/StoreCommit` | `0x10baca0` | `0x83f6e0` |

Thus it **does** reach the same edited store and replication sink as Build.Write. It does **not** pass through
Kiln's Run/residency/zone/base checks, journal, keep-region registration, write fault latch or readback verification.
**Grow=0 is not a no-collateral-write guarantee:** both wrappers unconditionally construct dispatcher flags
`{0,1,0}` (C `0xa9abea`, S `0xa454ea`). Byte 1 enables isolated-terrain cleanup over the **widened staging buffer**,
not just the requested core. A separate isolated filled terrain cell outside the core can therefore also be zeroed
(C `0xa89bd2..0xa89bde`, S `0xa34472..0xa3447e`). Grow=0 qualifies only the core brush's geometry, not the whole
operation's footprint; EMF's stronger exact-box description is incomplete on these builds. This is not a replacement
for a guarded exact-cell transaction. Allocation can fail before writing; preparation ignores the batch reader's
boolean readiness result. The outer function returns void and discards dispatcher success. The store's own lock is
not a build-zone permission gate. Native StoreCommit can skip insertion after compression/allocation failure,
without returning an error, and the block writer commits sequentially: earlier blocks survive later faults/failures.

**Use the existing guarded Build.Write with zero cells for Kiln's exact terrain clear.** Its up-front validation
refuses a denied operation before writes; its commit phase is still not atomic. `vox_commit_block` catches native
faults and disables writes; earlier blocks remain. Even KILN_OK can lack VERIFIED after readback, because a native
void writer can silently fail. Count observed removals, not just planned cells. The shared store saves only retained
regions; reaching StoreCommit alone does not guarantee an arbitrary edit survives reload. See [build.md](build.md)
and `native/services/build/voxel.cpp` for those existing contracts. Raw writer/fill calls belong on the server world's
Pre/Post tick, with no world jobs running, never arbitrary threads merely because store locks exist.

## 3. Player-placed terrain: qualified baked-column reader

There is **no player-placement bit, distinct terrain material range or building flag** in EMF's decision.
EMF `block_rules.cpp:176-183` calls current filled terrain world-owned when **any filled terrain** existed in the
baked cell, even if the material differs. A known baked air/building cell means player-placed terrain; unknown baked
state stays protected. Building provenance instead compares the raw material byte: same baked building material
means world-owned, differing material player-built, unknown protected (`block_rules.cpp:64-69`). This is provenance
relative to shipped terrain, not a historical record of who placed a cell.

`EMF_BREAK_TERRAIN` **only breaks player-placed terrain**, even together with EMF_BREAK_WORLD:
`terrain_sphere` refuses every terrain verdict other than 0 (`block_rules.cpp:255`).
`find_wall_ex(EMF_FIND_TERRAIN)` similarly selects verdict 0, passing over unknown/world terrain
(`block_glue.cpp:717-728`). Unknown is not permission to edit.

| Symbol | Client | Server | Win64 contract |
|---|---:|---:|---|
| `build/BaseColumnAcquire` | `0x10ab270` | `0x82f840` | `Handle*(Handle* out, VoxelWorld* editor, u32 level, u64 packedBrickXZ)` |
| `build/BaseColumnSeekBrick` | `0x10c6170` | `0x84adc0` | low-byte `bool(void* cursor, u32 brickY)` |
| `build/BaseBrickDecode` | `0x10cc7d0` | `0x8514b0` | `byte* (byte result[8], SpanU16* dst, const SpanBytes* src)` |
| `build/BaseColumnRelease` | `0x10cc750` | `0x851430` | `void(void* resourceLoader, u32 generationId)`; **not a handle destructor** |

Use an owned, zeroed **0xa0-byte, 16-byte-aligned** handle (safe storage, not a claim about a reflected sizeof),
level 6, and `packedBrickXZ = u32(floor_div8(x)) | (u64(u32(floor_div8(z)))<<32)`. Coordinates are the build service's
origin-relative cells, not blindly absolute metres times two. Acquire consults editor levels/resources and takes the
read lock at editor+0x174; unlike the merged reader, it never consults the edited store at editor+0x1220. It retains
a loaded generation through the resource loader at editor+0x160. These are resource-owned, **borrowed** bytes.

The `build/base_column.*` constants describe this verified layout:

| Name | Offset / value | Meaning |
|---|---:|---|
| `handle_size` / `decode_count` | `0xa0` / 512 | owned storage / cells in an 8-cube brick |
| `lock` / `lock_mode` | `0` / `8` | pointer to world lock / u8 lock mode |
| `loader` / `id` | `0x10` / `0x18` | loader pointer / u32 generation id |
| `data` / `ready` | `0x20` / `0x90` | resource-span pointer / u8 ready |
| `seek` | `0x30` | cursor passed to SeekBrick |
| `uniform` | `0x80` | u8: selected brick is uniform |
| `material` / `fill` | `0x68` / `0x70` | pointers to uniform material/fill bytes |
| `compressed` / `end` / `size` | `0x50` / `0x60` / `0x88` | compressed base pointer / u32 cursor end / u32 selected size |

Require ready and data; missing/unloaded/outside columns are **unknown**, not known air. On a valid loaded column,
SeekBrick(handle+0x30, brickY)==false means known air above its bricks. For a uniform brick read the material and
fill bytes; a null fill pointer defaults to material?255:0. Validate the material pointer. For a nonuniform brick,
validate size>0, size<=end and address ranges; source is `{compressed+end-size,size}`, destination `{u16[512],512}`.
Only result[0]==0 is success; otherwise unknown. Raw voxel brick indexing is `x+8*y+64*z`.
The native merged reader uses `0xff01` below Y=0; reject negative X/Z as unknown, rather than unsigned-wrapping them.

After **every returned acquire**, even failed/missing reads: if data is nonnull release(loader,id), then unlock the
published lock **only** for mode 1 (InterlockedDecrement) or mode 2 (InterlockedExchangeAdd 0x7fff0000). The engine's
own region reader performs exactly this two-stage cleanup at C `0x10b2631..0x10b2656` (S equivalent in `0x8369c0`).
Release alone leaks the world lock. Acquire publishes the lock pointer before acquisition, mode 1 only afterwards:
a nonnull lock pointer alone is not permission to unlock. Copy brick cells before cleanup; cache copied cells only
within the operation/session, never borrowed pointers. Call on the current server world's Pre/Post tick on both
executables; no client-to-server binary port or client-world ownership assumption is needed.

A fault **inside** acquire/resource-touch/release can leave unpublished lock/ref state. SEH does not prove rollback
or recovery. Latch provenance/edits unavailable with restart-required diagnostics; do not continue writing on an
assumption that finally cleaned every internal native lock. The `build.block_provenance` feature is static layout
qualification, not a promise of fault recovery.

## 4. Damage and drops: qualified primitives, not a complete guarded break implementation

`build/VoxelDamageGet` C **0x4a9cf0** / S **0x2426c0** is `u8(ServerVoxelDamageSystem*, const u32 cell[3])`.
Only AL is the result: 0 for missing/outside/undamaged, 255 fully damaged. Damage is in a sparse brick map, not the
raw voxel's appearance byte. The getter checks brick dimensions at system+0xa0/+0xa4/+0xa8. Its byte indexing is
**x+8*z+64*y**, different from the raw voxel brick order.

`build/VoxelDamageAdd` C **0x4a7c90** / S **0x2405a0** is
`u8(system*, i64 ecsNowNs, u32 ecsTick, const u32 cell[3], u8 increment)`. It allocates a damage brick if needed,
saturates addition at 255, stores expiry now+20,000,000,000 ns at brick+0x200, tick at +0x208 and dirty byte 0 at
+0x20c. Zero increment does nothing. Native kernel callers obtain tick and nanoseconds through context helpers;
these match KilnTickInfo.clock and clock_period*clock (preserve signed sentinel/overflow guards). No local lock
protects these sparse-map operations: serialize with engine jobs on server Pre/Post, not a mod-only mutex.
The native damage replication writer C `0x4a8e00` / S `0x241710` consumes the same dirty/tick fields and emits
all 512 damage bytes quantized as byte>>5 (3 bits/cell), with per-peer state. This is the actual crack-state path.
Healing cadence, visual decals and live replication still need an in-game check.

`build/BlockKernel` C **0x4966e0** / S **0x22dc40**:
`bool(operation*, undo*, stats*, SpanFloat* table256, aligned float centre4[4], float radius, u8 mode)`.
The original building helper C `0x4906a0` / S `0x227bc0` builds health/damage coefficients, calls mode 0 to add
floor(255*clamp(1/table[material],0,1)) damage, then mode 1 to clear cells whose native damage reached 255.
Null operation+0x128 damage-store selects instant removal. The kernel uses operation+0 program context,
+0x120 snapping and +0x130 editor, and its removal call uses grow=1: it can affect a margin beyond the selected cell.
It reads at most 4096 cells and can refuse snapping/geometry; it is not an arbitrary box API or a guarded exact writer.
A valid ECS context, the remaining operation dependencies and write-policy integration must be supplied before
binding this as a new break path. A raw kernel symbol or a fake context is insufficient.

EMF uses these native building passes, but terrain damage is its **own** cell map, not native cracks. Terrain hit
count is `health*250/(damage[hardness]*miningFactor)`, saturating to damage byte 255, with entries forgotten after
20 seconds; Unbreakable stays excluded even for instant mode (`block_rules.h:110-121`, `block_rules.cpp:185-209`).
Expose that compatibility policy above engine facts, not as a claim that terrain health is intrinsically 250 times
its reflected value. A failed/refused clear after damage changes cannot be described as an atomic failed operation.

Drops are **mining yields**, not the building-material item and not one item per voxel. The config GUID/rate above
is the game's mapping. The native terrain yield collector C **0x492980** / S **0x229ea0** calls the native accumulator
C **0x4a6f40** / S **0x23f850**, using per-material 12-byte player state, additional bonus state and native randomness,
then resolves the item and adds it to the transaction. See [terrain-tool-carve.md](terrain-tool-carve.md).
Its accounting is more than changed-cell count; calling it generically without that player/transaction context is
**not qualified** here. Preserve the original tool impact/yield path when native mining accounting is required.

EMF instead uses native GUID/u32 rate plus its own per-material carry: floor((removed+carry)/rate), retaining the
remainder; no item for zero GUID/rate (`block_rules.cpp:143-170`), then deferred loose pickups. This is an explicit
compatibility policy, not invented item values, but is not the full native bonus/random accumulator. Qualifying
metadata does not automatically qualify inventory grants, transactions, dropped counts after a partial clear or
an atomic damage+remove+drops operation. Keep those modes refused until their integration is implemented/tested.

## 5. find_wall: use exact-cell DDA, not the existing native ray

The native `queries/VoxelRay` exists on **both** builds: C **0xa0f770**, S **0x6ac7e0**,
`void*(Hit* out, VoxelGrid*, aligned float from4[4], aligned float to4[4], const u8 options[4])`.
Its options control realm, unavailable handling and thin terrain; it has no baked-provenance predicate or option to
pass natural ground while stopping at placed ground. It mutates realm caches, has no native realm bounds check,
and its expanded hit point is not a reliable exact voxel coordinate. Outside-grid is treated as loaded empty unless
the wrapper validates both endpoints; see [build.md](build.md) section 10. Both new decompiles reproduce its
realm stride 0x3870, terrain/material handling and output-first ABI.

EMF `voxel_read.cpp:42-112` already uses 0.5 m cell DDA, tests the starting cell and endpoint, and returns the hit
cell's centre; ties select X then Y then Z. It reads buildings irrespective of high-byte fill and optionally uses
the baked predicate above for terrain. Read failure ends traversal, not a clear line. A bounded DDA over existing
Build cell reads (prefer batched/cached bricks) matches those semantics, with no ray-cache readiness or hit-to-cell
rounding ambiguity. Preserve coordinate/range validation, the compatibility length cap and unknown-state refusal.

## Owner-guided checks still required

On a disposable copy of a world, compare a natural ground cell, changed natural material, player-raised terrain,
a shipped ruin and a player building. Query provenance; trace through natural ground to a placed terrain wall;
verify unloaded columns refuse. Clear one placed cell beside natural ground and verify no neighbouring change.
Exercise a box crossing a protected base/zone: refusal must leave every cell unchanged. Save/reload and have a
second client observe the retained clear. For any enabled damage/drop implementation, compare native pickaxe
hardness, crack increments, healing after 20 seconds, threshold removal and material yield/remainder; test
Unbreakable, partial writer failure accounting and world teardown cancellation. No such game check was run here.
