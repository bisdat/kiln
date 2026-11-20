# Material pools: coordinator run

The lab part runs on dedicated servers: all 18 cases pass on server build 26430253 (2026-11-15; 15 before the repair round added the stored-pool and observer cases). **A game client has never run it**: the build menu, previews, placement and joins are the guided check's (below). It ships enabled when the executable's reflection resolves. No preview setting or gate entry is needed. A byte dump or TestKit result is not proof that the build menu consumed it.

Use two **new, disposable** labs copied from the same test save. The mod selects clear cells inside the supplied base box (default corners `(3006, 902.6, 1530)` and `(3042, 906.6, 1570)`). It reads every proposed box and completes selection before its first write; occupied candidates are skipped. Each chosen box is logged as `mp: selected cells X Y Z SizeX SizeY SizeZ`. No space produces `mp: FAIL completed-without-exception: base has no room ...; nothing written` and no write. The main run writes three small kept cubes there and requests a real game save. The boundary run writes one cube and retains 49 definitions in its own world ledger. Never use a player's save. The runner refuses pre-existing worldtables ledgers and unrelated mods; it stops each lab before copying files. It never clears a world ledger to manufacture free capacity.

Deploy the whole stack of one tree into both labs: `tools/deploy-server.sh` builds what changed (kiln-worldtables and kiln-content among them; an older kiln-content refuses the pool GUID and the binding rows fail). Build the lab driver into the same `KILN_OUT` (the runner copies `$KILN_OUT/dotnet/bin/MaterialPoolsTest/debug/MaterialPoolsTest.dll` into the labs itself), then run the runner from the repository root:

```sh
bash tools/lab/lab.sh new dev29-17d-pools 28350 --save <disposable test save>
bash tools/lab/lab.sh new dev29-17d-pools-boundary 28354 --save <disposable test save>
tools/deploy-server.sh "$KILN_LABS/dev29-17d-pools" --policy any
tools/deploy-server.sh "$KILN_LABS/dev29-17d-pools-boundary" --policy any
dotnet build mods/tests/material-pools-test/MaterialPoolsTest.csproj --artifacts-path "$KILN_OUT/dotnet"
python3 -B mods/tests/material-pools-test/run.py --lab dev29-17d-pools --boundary-lab dev29-17d-pools-boundary \
  --port 28351 --boundary-port 28355 --out "$KILN_OUT/material-pools-lab"
```

In a build wave, put the wave's slot tool in front of the build commands and set `MP_LABSTART` to the wave's lab starter: the runner then starts each lab through it. The driver waits, as `mods/tests/build-test` does, until the game reports the world running before it reads or writes cells: a dedicated server's voxel world refuses edits while its save still loads, even though the tiles already read.

Every `mp: PASS`/`mp: FAIL` row names what it read, and a FAIL row also what it expected: for a pool, the served row's pool id, equipment slot and member item ids in order against the expected ones; for a refusal, the reason the author was given.

Use `--base-box X0 Y0 Z0 X1 Y1 Z1` for another base and vertical search range. `--position X Y Z` remains an optional override (read and checked before writes, with subsequent witnesses 3 m east); omit it for automatic selection. Selection is bounded to 4,096 reads. The runner uses `tools/lab/lab.sh` and the existing `tools/nettest` loopback protocol. Each start has a 480-second deadline. A timeout, missing completion, failed assertion, missing named PASS, wrong snapshot hash, or a mismatching client receiving WELCOME/DATA/FRAG fails the run. The loopback case does not prove a real client's renderer, replication, or menu.

The terminal must show every line below and exit zero:

```text
PASS mp-lab.served
PASS mp-lab.restart
PASS mp-lab.absent
PASS mp-lab.back
PASS mp-lab.late-material-room
PASS mp-lab.handshake-order-before-stream
PASS mp-lab.handshake-binding-before-stream
PASS mp-lab.late
PASS mp-lab.pending-inactive
PASS mp-lab.replay
PASS mp-lab.torn-refused
PASS mp-lab.stored-pool-dropped
PASS mp-lab.stored-pool-gone
PASS mp-lab.restored-main-hash
PASS mp-lab.observer-cost
PASS mp-lab.building-seed
PASS mp-lab.building-retained
PASS mp-lab.terrain-boundary
PASS mp-lab.ALL <16-digit actual main-profile hash>
```

The archived `main/served.log` must include `mp: PASS vanilla-rows-unchanged`, `membership-four/shared/full/roof`, `binding-four/shared/full/roof`, `preview-data-four/shared/full/roof`, `budget-four/shared/full/roof`, `shared-material-one-id`, `no-65-served`, `vanilla-shape-binding`, `material-budgets`, `unknown-pool`, `served-registration`, `served-idempotent`, `65-refused-before-serving`, `duplicate-refused`, `roof-refused`, `unknown-base-refused`, `all-ground-read-before-write`, `cells-written-and-game-saved`, and `restart-materials-and-cells-live` (each prefixed `mp: PASS `). These compare actual pool resource bytes, shape-table rows, and the item's 16-byte GUID reference. The full pool has the original 61 nonzero wall items plus three mod items, in that order. The roof baseline includes an empty slot; it counts as occupied, but cannot be a custom selectable entry.

`main/restart.log` and `main/back.log` must show `mp: PASS restart-materials-and-cells-live`; `main/absent.log` must show `mp: PASS absent-materials-and-cells-retained`. The saved cells must actually read back with their current GUID-mapped material IDs and full fill. The runner compares identical-profile hashes across restart. `main/late-material.log` must show `mp: PASS late-material-uses-vanilla-room`: the three grouped mod materials consume no vanilla selector places, so a new ungrouped late material can use the still-free room (without changing live counts). The runner verifies and removes only that staged lab definition before restarting. Reordering Four's entries and rebinding Four's shape each produce an actual new native hash; the prior hash is sent with the **same current material IDs** and the first profile's per-pool digests (`kiln.worldtables.pools`), and must be rejected before any stream frame with a refusal that names the pool: `the material pool mp-fixture:four differs` (order) and `mp-fixture:shared differs` (binding).

`main/inactive.log`: `mp: PASS inactive-request-unapplied` and `mp: PASS inactive-file-named`. The original driver is disabled for this start; a separate loaded observer checks that no row/receipt was applied. The runner requires exactly one line naming the original `@pool-<guid>.json` as kept (`kept: its mod is not loaded`, never `unsafe`), and byte-identical pending contents afterwards. The file remains available for the next enabled start.

`main/late.log`: `late-registry-closed`, `late-accepted-zero-slots`, `late-idempotent`, `late-not-live`, `explicit-remove` (a second stored pool removed with `WorldTables.RemoveStoredMaterialPool`, its receipt Removed). There must be exactly one checksummed `kiln/data/worldtables/pending/material-pools-test/@pool-<GUID>.json`, containing both the canonical definition and registration identity. `main/replay.log`: `restart-receipt-registered`, `restart-owner-adopted-before-redefine`, `restart-same-receipt`, `restart-one-row`. The runner then deliberately truncates this lab-owned file; `main/torn.log` must show `torn-envelope-refused`, a NotFound receipt/budget, and the service's torn/invalid envelope error. The runner then writes the valid envelope back: `main/drop.log` (the driver loaded and no longer defining `late`) shows `drop-row-served-this-session` and `drop-world-ran`, and the runner requires the file to be gone after that start (`stored-pool-dropped`); `main/dropped.log` shows `dropped-row-gone`, and its hash must equal the `late` run's (`stored-pool-gone`). It then restores the original main profile/hash. `main/observer-cost.log` (`observer-cost`) logs `mp: COST` with the pool observer's allocations and time a call and the tick thread's allocations a tick, and requires `observer-quiet-when-unchanged` (at most 32 bytes a call).

`boundary/building-seed.log`: `building-49-plus-1`, `boundary-refusal-count`, `cells-written-and-game-saved`. The seed uses roofs so selector room is not the limiting factor. After removing that mod, all 49 records are retained without selectable block items; `boundary/building-retained.log` must show `building-49-plus-1`, one ID refusal, `retained-building-no-selector-items`, and `absent-materials-and-cells-retained`. Building budget is 128/79/0/49. `boundary/terrain.log` must show `terrain-11-plus-1`, one ID refusal, and `terrain-no-selector-work`: 127/116/11/0 with TerrainMaterials still 56 entries. The existing building API always creates block items; the retained-record scenario is how this fixture tests 49+1 without selectable items, without adding an unrelated API.

Every run archives `*-result.json`, `*-served.json`, the log and cell witnesses. `lab-proof.json` (the main-profile hash, the Kiln build and every case) is written **only after all 18 cases pass**. No proof file is shipped in this repository.

# Guided real-client check

Deploy `fixtures/mp-fixture/` plus the driver DLL, manifest and `blocks/served.json` on the server and client. On a client the driver defaults to `observe` and performs no voxel writes. Copy the generated `lab-proof.json` into each `mp-fixture/` folder. Deploy the rebuilt `ClientPass.dll` and **updated Kiln.Testing.dll**. A missing proof, a different profile hash, missing rows, wrong membership or wrong item GUID is a failed automatic requirement, not a passable warning.

Run **`client-pass/17-material-pools`**, `Since = Dev29`, from the client-pass list (Ctrl+Shift+F5). Run it hosted and joined. The shared runner changes a player's Pass to Fail whenever any Expect failed, for every check; there is no opt-in tag. The code checks served bytes; the player must show actual menu ingestion/order, first/last selection in the four and full 64 lists, the shared Ice inventory, roof selection, previews, three-block placement cost, surface and one-own-block-per-cell loot, unchanged vanilla lists, save/reload/rejoin, a second hosted world in the same process, and real matching/mismatching joins. Leaving a world ends the running check; reopen it in the final session and Pass only after the entire instruction was carried out. `17-material-worlds` is outside this package.

For a data-only authoring example, see `mods/samples/MaterialPools/`. It adds no pagination or wider material IDs.
