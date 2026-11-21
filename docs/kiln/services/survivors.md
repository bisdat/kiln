# Survivors: recruit, summon, beds (kiln.survivors, since 0.2.0-dev.31)

The game's roster survivors (the NPCs a player rescues, then places with a summon tool and gives a bed) for server
mods. Kiln owns the game side: a mod names a survivor by its roster id and a bed by its entity, never a game function,
field or offset.

| Layer | File |
|---|---|
| Native service | `native/services/quests/survivors.cpp` (in kiln-quests.dll, next to kiln.knowledge), `survivors_core.h` (the game-free rules) |
| ABI | `native/core/abi/services/kiln_survivors.h` (`kiln.survivors` v1, a new table) |
| Layout | `sigdb/symbols/{server,client}.d/dev31-18-survivors.json` (feature `survivors.assignment.layout`) |
| Managed | `managed/services/Kiln.Quests/Survivors.cs`, `SurvivorsTypes.cs`, `SurvivorsEvents.cs` |
| Tests | `native/services/quests/tests/survivors_test.cpp` (host), `managed/tests/Kiln.Quests.Tests/SurvivorsTests.cs`, `mods/tests/survivors-test/` (lab), `mods/tests/client-pass/` checks `18-c18.1`, `18-c18.4` |
| Engine facts | dossier chapter 18; [docs/re/quests.md](../../re/quests.md); [docs/re/creatures.md](../../re/creatures.md) 7.6 |

## The API

```csharp
var smith = Survivors.List().First(s => s.DebugName == "NPC_Workshop_Blacksmith_Assistant_01");
var r = await Survivors.RecruitWorld(smith.Id, new SurvivorRecruitOptions(GiveToolTo: player));
var baseId = Survivors.BaseAt(player.Position!.Value);              // the base the player stands in
if (baseId is { } b) await Survivors.Summon(smith.Id, b, spot);     // created there, or moved there (its home)
await Survivors.AssignBed(smith.Id, bed, new SurvivorAssignmentOptions(player.Entity));   // the player's rights
await Survivors.UnassignBed(smith.Id, SurvivorAssignmentOptions.Trusted);                 // the server mod's own
```

All members are server-side and run on the tick thread.

| Member | Capability | What it does |
|---|---|---|
| `Survivors.Find(id)`, `List()`, `Of(entity)` | `survivors.read` | The roster with live state: `SurvivorInfo(Id, DebugName, Unlocked, SummonTool, Entity, Bed, BaseId, Home)` |
| `Survivors.BaseAt(position)` | `survivors.read` | The base whose build zone holds a point (the id `Summon` takes) |
| `Survivors.BedOccupant(bed)` | `survivors.assign` | The survivor a bed is assigned to (the bed's side), null when free |
| `Survivors.RecruitWorld(id, options)` | `survivors.recruit` | Unlock for the world, registry confirmation, optionally one summon tool |
| `Survivors.Summon(id, baseId, position, yaw)` | `survivors.summon` | Into a base: an NPC in the world is moved (its home too), else created; a locked one is recruited first |
| `Survivors.AssignBed(id, bed, options)`, `UnassignBed(id, options)` | `survivors.assign` | Both ends of the bed relation in one step |

`SurvivorId` is the game's NpcId (FNV-1a-32 of the NPC template's GUID), the same value as `Villager.NpcId`: store it,
never an entity id (entity ids change at every load).

### Results: one rule for every request

Each write is a request that ends in one `SurvivorResult` (`IKilnResult`) within about 30 s:

| Completion | Ok | Meaning |
|---|---|---|
| `Done` | yes | everything asked for happened and was read back |
| `NoChange` | yes | it already was so; nothing written |
| `Refused` | no | refused before anything was written (`Reason`) |
| `TimedOut` | no | the 30 s budget ran out before anything was written |
| `Partial` | no | something was written (`Effects`), then it could not finish |
| `WorldEnded` | no | the world stopped; `Effects` says what was written before |

`Effects` lists what was really written: `KnowledgeWritten`, `ToolGiven`, `EntityCreated`, `HomeMoved`,
`BedChanged` (and the observation `RegistryConfirmed`). **Kiln never takes something back to fake a rollback**: an
unlock, a tool or a move that happened stays, and the result says so. A child step (the knowledge grant, the item
transaction) is never cancelled: when the budget runs out while one is in flight, the request waits for it and then ends
with what really happened. Abandoning the task yourself (a timeout of your own) does not cancel the request.

Bad arguments throw `ArgumentException` (id 0, a bed of `EntityId.None`, null options, PlayerPermissions without an
actor, TrustedServer with one, a position or yaw that is not finite, an empty base id); a capability that is off throws
`KilnUnavailableException` naming it; everything the world decides comes back as `Ok = false` with a `Reason` of the
form `Code: words`:

| Code | When |
|---|---|
| `NotARosterNpc` | no roster entry has this id |
| `UnsupportedUnlockRequirement` | the survivor's unlock is a compound condition (Kiln grants only a plain world Bool) |
| `KnowledgeNotApplying` | the world's knowledge update does not run yet (right after the start) |
| `KnowledgeRefused`, `RegistryConfirmationTimeout` | the grant was refused / the NPC registry did not follow within 10 s or 180 updates |
| `NoSummonTool`, `RecipientNotInGame`, `RecipientLeft`, `RecipientSessionChanged`, `ToolRefused`, `ToolNotGiven` | the tool |
| `BaseNotFound`, `BaseAbandoned`, `PositionOutsideBase` | Summon's base and point |
| `NotUnique`, `SpawnFailed`, `MoveRefused`, `Despawned`, `NotConfirmed` | Summon's placement and its readback (home within 0.05 m, position within 1 m, a new NPC alive 3 s) |
| `NotInWorld`, `Locked`, `NotABed`, `BedOutOfHomeRange` | beds: the NPC must be recruited and in the world, the bed a sleeping place within the NPC's bed radius of its home (read from the NPC's template through the game's GetTemplateComponent; when that cannot be read the check is skipped with one warning in kiln.log) |
| `ActorNotInGame`, `ActorSessionChanged`, `PermissionsUnknown`, `PermissionDenied`, `BedOutsideBase` | PlayerPermissions |
| `ChangedByGame` | the readback after a bed write does not show it (the game changed it in between) |
| `WorldChanged`, `QueueTimeout`, `Deadline`, `WorldEnded` | the request's world and budget |

### Authorization of bed changes

`SurvivorAssignmentOptions` is required:
- **PlayerPermissions** (the default): `Actor` is a player in game with the base-edit permission, and every bed the
  change touches (the new bed, the survivor's old bed, a stale bed that still names it) lies in a loaded, kept base whose
  zone rules ([gameplay.md](gameplay.md)) let that player build there.
- **TrustedServer** (`SurvivorAssignmentOptions.Trusted`): no actor; skips only those player checks, never the
  NPC, bed and range checks. For server code that is its own authority. Never pick it from a client's chat command or
  RPC without your own check.

`RecruitWorld` and `Summon` are trusted server operations: a recipient is checked as a player in game, not as an
authorization. A mod that offers them to players does its own permission check.

## Beds (the contract for NPC bodies and villager routines)

1. **Two-sided, one step.** `AssignBed` writes the NPC's bed and the bed's NPC in one Kiln program run with tracked
   writes (the save and replication see them as the game's own), frees the NPC's old bed and takes the bed from its
   previous survivor (`SurvivorResult.DisplacedNpc`, and an event for that survivor). `UnassignBed` clears both ends and
   any stale bed that still names it. The task completes one update after the write, after a readback of both ends.
   The write runs in Kiln's program `kiln_survivors_beds` (a barrier: alone, after the step's earlier programs, never in
   the game's villager job) and changes only the two relation fields, never the NPC's sleep hours, flags or state.
   `Summon` moves an NPC with the game's Reset (the same command as `Villager.Teleport`): a mod's Summon is a mod-issued
   command, so it suspends a villager routine on that survivor until `Routines.Resume` (the ruling for villager routines).
2. **Persistence.** The bed side is saved by the game; the NPC side is rebuilt from it when the world loads. After a
   restart `Find(id).Bed` is the same bed with a new entity id.
3. **Game-origin changes are events.** A player assigning with the summon tool, a destroyed bed or the game's bed upkeep
   raise `SurvivorAssignmentChangedEvent` with `ByKiln == false` (Kiln compares the roster's beds at most once a second).
4. **What a bed is.** An entity with the game's NPC bed component: a single bed, or each sleeping place of a double bed.
   A double bed's frame is not one (`NotABed`).
5. **Home range.** A bed farther from the NPC's home than its own bed radius (the template's, 50 m for the vanilla
   survivors) is refused (`BedOutOfHomeRange`): the game would never go there. Move the NPC first (`Summon`).
6. **Routines** (villager routines) go through the game's own bed search: they read `Find(id).Bed` and follow
   `SurvivorAssignmentChangedEvent`, and never write a bed link themselves.
7. **Bodies** (a mod's own NPC with a roster entry) are survivors like any other.
8. **Not covered:** station or job assignment (the game keeps no such link), walking or path reachability to the bed,
   sleeping animations, roster entries past the roster's limit.

## Events

`SurvivorRecruitedEvent` (the registry confirmed a Kiln recruit), `SurvivorToolGivenEvent`, `SurvivorSummonedEvent`,
`SurvivorAssignmentChangedEvent` (`OldBed`, `NewBed`, `ByKiln`). All carry `Survivor`, `Before`, `After`,
`Request` (0 = the game) and `Step`, on the tick thread after the change. A player's own rescue raises the knowledge
events (`KnowledgeChangedEvent`, `RosterUnlockChangedEvent`), not `SurvivorRecruitedEvent`.

## Native

`kiln.survivors` v1 (`native/core/abi/services/kiln_survivors.h`): `status`, `count`/`at`/`get`/`of_entity`/
`bed_occupant`/`base_at` (tick), `submit`/`result`/`progress`/`events` (any thread). Results are kept 10
minutes (the last 256), events the last 4096. The header documents every struct; its static asserts are the layout.

## Status

- Lab (dedicated server, no player): see "Verification" below.
- Not seen in a game client: what a player sees of a recruit, the tool in the inventory, the NPC at its new home, the bed
  label, the game's own assignment tool on a Kiln-assigned bed: guided checks `18-c18.1` and `18-c18.4`
  (out/18-survivors/CLIENT-CHECKS.md for the owner's pass).
- A hosting game: the bed program is registered for the server world there too; not run yet.
- A server with players runs the bed program on a game worker: it is a serial, barrier program with its own lock and
  touches only what it declares; not seen on a worker yet.

## Verification

Lab, dedicated server (server build 26430253, no player), 2026-11-20, `mods/tests/survivors-test` over two starts of one
world: run 1 **40 PASS, 0 FAIL**, run 2 (after a graceful stop that saved the world) **4 PASS, 0 FAIL**:
- the four capability rows on; the roster (35 entries) read; the lab survivor (`NPC_Workshop_Blacksmith_Assistant_01`,
  0x672da6c9) locked;
- `RecruitWorld`: Done with KnowledgeWritten and RegistryConfirmed in 2 steps, the event; again: NoChange; a tool for an
  entity that is no player: Refused RecipientNotInGame;
- `BaseAt` finds a spawned altar's base; `Summon` outside its zone: Refused PositionOutsideBase; `Summon`: one entity
  created, home within 0.05 m, its base the altar's, the event; again elsewhere: the same entity moved (HomeMoved);
- beds: assign both ends, NoChange again, reassign frees the old bed, an actor who is not in game Refused (nothing
  written), PlayerPermissions without an actor throws, an altar is NotABed, a bed 86 m from home Refused
  BedOutOfHomeRange (bed radius 50 m read from the NPC's template), a second survivor (`Summon` of a locked one recruits
  it first) takes the first one's bed (displaced, both ends right), unassign and NoChange again;
- a destroyed bed: the game moves the NPC off it (onto the other free bed in range), seen as a game-origin event;
- after the restart: the survivor recruited and in the world, its bed the same bed by position (new entity ids), the bed
  side naming it.

After a load the game restores the NPC's side of the relation when it initializes the bed, which can come later than the
observer's 10 s baseline: a mod may then see one game-origin event "no bed -> its bed" per survivor (seen in run 2).
