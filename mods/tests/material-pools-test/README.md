# Material pools: coordinator run

This fixture has been built and host-tested, **never run on a server or in a game**. It ships enabled when the executable's reflection resolves. No preview setting or gate entry is needed. A byte dump or TestKit result is not proof that the build menu consumed it.

Use two **new, disposable** labs copied from the same test save. Pick a position inside a flame base with at least 8 m of clear space to its east. The main run writes three small kept cubes there and requests a real game save. The boundary run writes one cube and retains 49 definitions in its own world ledger. Never use a player's save. The runner refuses pre-existing worldtables ledgers and unrelated mods; it stops each lab before copying files. It never clears a world ledger to manufacture free capacity.

The coordinator first builds/deploys the integrated Kiln stack, including the changed `native/services/content/refs.cpp`. This implementer's only Windows cross-build is `kiln-worldtables`; the reference admission change is covered by the content service's host sanitizer test. An older kiln-content DLL will refuse the pool GUID and the binding checks must fail.

From the integrated tree, with `KILN_OUT` pointing at the integrated build and `KILN_LABS` at the lab root:

```sh
bash tools/lab/lab.sh new dev29-17d-pools 28350 --save /path/to/disposable-test-save
bash tools/lab/lab.sh new dev29-17d-pools-boundary 28354 --save /path/to/disposable-test-save
bash tools/deploy-server.sh "$KILN_LABS/dev29-17d-pools" --no-build
bash tools/deploy-server.sh "$KILN_LABS/dev29-17d-pools-boundary" --no-build
```

Build `MaterialPoolsTest.csproj` with the same targeted/offline dotnet flags used in REPORT.md. If using this implementer's outputs, set `KILN_OUT=/home/jame/Work/kiln-codex/d29-17d/build`; its driver lives at `$KILN_OUT/dotnet/bin/MaterialPoolsTest/debug/MaterialPoolsTest.dll`. Before the run, copy this build's worldtables DLL and managed assemblies over the stopped integrated labs:

```sh
for name in dev29-17d-pools dev29-17d-pools-boundary; do
  cp "$KILN_OUT/cmake/bin/services/kiln-worldtables.dll" "$KILN_LABS/$name/kiln/services/"
  cp "$KILN_OUT/dotnet/bin/Kiln.Blocks/debug/Kiln.Blocks.dll" "$KILN_LABS/$name/kiln/host/"
  cp "$KILN_OUT/dotnet/bin/Kiln.Testing/debug/Kiln.Testing.dll" "$KILN_LABS/$name/kiln/host/"
done
/home/jame/Work/kiln-codex/slot python3 -B mods/tests/material-pools-test/run.py \
  --lab dev29-17d-pools --boundary-lab dev29-17d-pools-boundary \
  --port 28351 --boundary-port 28355 --position X Y Z --out "$KILN_OUT/material-pools-lab"
```

`X Y Z` must be the chosen base position, not literal letters. The runner uses `tools/lab/lab.sh` and the existing `tools/nettest` loopback protocol. Each start has a 480-second deadline. A timeout, missing completion, failed assertion, missing named PASS, wrong snapshot hash, or a mismatching client receiving WELCOME/DATA/FRAG fails the run. The loopback case does not prove a real client's renderer, replication, or menu.

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
PASS mp-lab.replay
PASS mp-lab.torn-refused
PASS mp-lab.restored-main-hash
PASS mp-lab.building-seed
PASS mp-lab.building-retained
PASS mp-lab.terrain-boundary
PASS mp-lab.ALL <16-digit actual main-profile hash>
```

The archived `main/served.log` must include `mp: PASS vanilla-rows-unchanged`, `membership-four/shared/full/roof`, `binding-four/shared/full/roof`, `preview-data-four/shared/full/roof`, `budget-four/shared/full/roof`, `shared-material-one-id`, `no-65-served`, `vanilla-shape-binding`, `material-budgets`, `unknown-pool`, `served-registration`, `served-idempotent`, `65-refused-before-serving`, `duplicate-refused`, `roof-refused`, `unknown-base-refused`, `cells-written-and-game-saved`, and `restart-materials-and-cells-live` (each prefixed `mp: PASS `). These compare actual pool resource bytes, shape-table rows, and the item's 16-byte GUID reference. The full pool has the original 61 nonzero wall items plus three mod items, in that order. The roof baseline includes an empty slot; it counts as occupied, but cannot be a custom selectable entry.

`main/restart.log` and `main/back.log` must show `mp: PASS restart-materials-and-cells-live`; `main/absent.log` must show `mp: PASS absent-materials-and-cells-retained`. The saved cells must actually read back with their current GUID-mapped material IDs and full fill. The runner compares identical-profile hashes across restart. `main/late-material.log` must show `mp: PASS late-material-uses-vanilla-room`: the three grouped mod materials consume no vanilla selector places, so a new ungrouped late material can use the still-free room (without changing live counts). The runner verifies and removes only that staged lab definition before restarting. Reordering Four's entries and rebinding Four's shape each produce an actual new native hash; the prior hash is sent with the **same current material IDs** and must be rejected before any stream frame.

`main/late.log`: `late-registry-closed`, `late-accepted-zero-slots`, `late-idempotent`, `late-not-live`. There must be exactly one checksummed `kiln/data/worldtables/pending/material-pools-test/@pool-<GUID>.json`, containing both the canonical definition and registration identity. `main/replay.log`: `restart-receipt-registered`, `restart-same-receipt`, `restart-one-row`. The runner then deliberately truncates this lab-owned file; `main/torn.log` must show `torn-envelope-refused`, a NotFound receipt/budget, and the service's torn/invalid envelope error. It removes only this test's envelope and restores the original main profile/hash.

`boundary/building-seed.log`: `building-49-plus-1`, `boundary-refusal-count`, `cells-written-and-game-saved`. The seed uses roofs so selector room is not the limiting factor. After removing that mod, all 49 records are retained without selectable block items; `boundary/building-retained.log` must show `building-49-plus-1`, one ID refusal, `retained-building-no-selector-items`, and `absent-materials-and-cells-retained`. Building budget is 128/79/0/49. `boundary/terrain.log` must show `terrain-11-plus-1`, one ID refusal, and `terrain-no-selector-work`: 127/116/11/0 with TerrainMaterials still 56 entries. The existing building API always creates block items; the retained-record scenario is how this fixture tests 49+1 without selectable items, without adding an unrelated API.

Every run archives `*-result.json`, `*-served.json`, the log and cell witnesses. `lab-proof.json` is written **only after all 14 cases pass**. No proof file is shipped in this repository.

# Guided real-client check

Deploy `fixtures/mp-fixture/` plus the driver DLL, manifest and `blocks/served.json` on the server and client. On a client the driver defaults to `observe` and performs no voxel writes. Copy the generated `lab-proof.json` into each `mp-fixture/` folder. Deploy the rebuilt `ClientPass.dll` and **updated Kiln.Testing.dll**. A missing proof, a different profile hash, missing rows, wrong membership or wrong item GUID is a failed automatic requirement, not a passable warning.

Run **`client-pass/17-material-pools`**, `Since = Dev29`, from the client-pass list (Ctrl+Shift+F5). Run it hosted and joined. Its `required-expectations` tag means any failed Expect changes the player's Pass to Fail; older unrelated checks retain their verdict semantics. The code checks served bytes; the player must show actual menu ingestion/order, first/last selection in the four and full 64 lists, the shared Ice inventory, roof selection, previews, three-block placement cost, surface and one-own-block-per-cell loot, unchanged vanilla lists, save/reload/rejoin, a second hosted world in the same process, and real matching/mismatching joins. Leaving a world ends the running check; reopen it in the final session and Pass only after the entire instruction was carried out. `17-material-worlds` is outside this package.

For a data-only authoring example, see `mods/samples/MaterialPools/`. It adds no pagination or wider material IDs.
