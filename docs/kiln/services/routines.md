# kiln.routines: villager day plans

Since 0.2.0-dev.31. Native service `kiln.routines` v1 (`kiln-routines.dll`, `native/core/abi/services/kiln_routines.h`,
`native/services/routines/`), managed face `Routines` and `RoutineBlock` in `Kiln.Blocks` (namespace `Kiln`).

A mod gives a villager a **day plan**: windows of the game day, each with one of the game's own villager tasks (idle,
roam, sit, inspect, sleep, or "vanilla": the game's own choice), and a fallback. When a villager with a plan reaches a
decision, the window of the game minute says which task it takes; the game's own code then walks, finds the seat or the
bed, sits, sleeps and plays the animations. Nothing is teleported and nothing is placed: a plan only answers the
question the game asks at a decision. A villager without a plan runs the game's code unchanged.

| Layer | Where |
|---|---|
| Symbols | sigdb feature `routines.decisions`: the villager worker, its task chooser, the state setters, the sleep check, the command handler, the write marks and the context offsets, on the server and on the game client (the hosting client's own world) |
| C table | `native/core/abi/services/kiln_routines.h` |
| Native | `native/services/routines/` -> `kiln/services/kiln-routines.dll` |
| Managed | `managed/services/Kiln.Blocks/Npc/Routine*.cs` (`Routines`, `RoutineBlock`, the events, the saved assignments) |
| Data | the `routines` section of `blocks/*.json` and an NPC block's `routine` ([schemas/kiln-blocks.md](../modding/schemas/kiln-blocks.md#routine)) |
| Test kit | `KilnHarness.Routines` (`managed/Kiln.TestKit/Fakes/Routines/`) |
| Lab test | `mods/tests/routines-test` (PASS/FAIL lines in kiln.log) |
| Sample | `mods/samples/VillagerRoutine` |
| Guided check | `16g-cc16g` (`mods/tests/client-pass/Dev31Routines.cs`) |

## Capabilities

| Row | Means | Off when |
|---|---|---|
| `kiln.routines` | the native table is there | `kiln-routines.dll` is not loaded |
| `routines.decisions` | plans choose villagers' tasks: on a dedicated server, and in the world of a game that hosts | the game build's symbols did not resolve, a hook did not install, or `kiln.world` / its Pre and Post are missing (the managed row is lowered then) |
| `routines.anchors` | "go to this place" tasks | always, in this version (policy) |
| `routines.jobs` | production work at a station | always, in this version (policy) |

`routines.decisions` is not held by the client gate: nothing ships switched off in 0.2.0-dev.31. Its real-game check is
the guided check `16g-cc16g`, "Unconfirmed" until the owner has played it. A joined client never decides for villagers:
the server does, and a game without Kiln that joins sees the villagers as the server moves them.

## A plan

```csharp
var day = Blocks.Define("day", new RoutineBlock
{
    Version = 1,
    Windows =
    [
        new RoutineWindow(6 * 60, 12 * 60, new InspectTask()),
        new RoutineWindow(12 * 60, 14 * 60, new SitTask()),
        new RoutineWindow(14 * 60, 21 * 60, new RoamTask()),
        new RoutineWindow(21 * 60, 6 * 60, new SleepTask(), Priority: 5),  // over midnight
    ],
    Fallback = new IdleTask(),
});
var result = await Routines.Assign(villager, day);
```

The same in `blocks/*.json`:

```json
{
  "format": 1,
  "routines": {
    "day": {
      "version": 1,
      "windows": [
        { "startMinute": 360, "endMinute": 720, "task": { "kind": "inspect" } },
        { "startMinute": 720, "endMinute": 840, "task": { "kind": "sit" } },
        { "startMinute": 840, "endMinute": 1260, "task": { "kind": "roam" } },
        { "startMinute": 1260, "endMinute": 360, "priority": 5, "task": { "kind": "sleep" } }
      ],
      "fallback": { "kind": "idle" }
    }
  }
}
```

A plan from data is found by name: `Blocks.Find("day", "<mod>") as BlockHandle<RoutineBlock>`.

| Type | Members |
|---|---|
| `RoutineBlock` | `Version` (uint, default 1), `Was` (the name it had before a rename), `Windows` (at most 64), `Fallback` (default `VanillaTask`) |
| `RoutineWindow(StartMinute, EndMinute, Task, Priority = 0, AllDay = false)` | minutes of the game day, 0..1439 (0 is midnight); end-exclusive; a start after the end runs over midnight; start = end is refused except `AllDay` with 0 and 0 |
| Tasks | `VanillaTask` (the game's own choice: its sleep check, its social targets, its weighted pick), `IdleTask` (stand, for the game's own idle time), `RoamTask` (walk around home, by the villager's own range), `SleepTask` (the game's own sleep, see below), `SitTask` (find a seat nearby and sit), `InspectTask` (find something nearby to look at or work on). None takes a value |

**Which window.** At each decision the villager takes the window with the highest priority among the windows that
cover the game minute; equal priorities go to the window written first. A minute no window covers runs the
`Fallback` as a task. The fallback is `VanillaTask`, `IdleTask` or `RoamTask` only (a task that can always run).

A changed window takes effect at the villager's next decision, not at once: a villager sitting at 13:59 finishes its
sit before it roams. Villagers decide only near a player: the game pauses a villager's state machine when no player is
in range (about 150 m), and a routine changes nothing while it is paused. It picks up again when a player comes back.

What is wrong with a plan throws `BlockError` (an `ArgumentException`) at `Blocks.Define` or when its JSON file is
read: more than 64 windows, a minute outside 0..1439, start = end without `AllDay`, `Version` 0, a `Was` that is not
a local name, a fallback other than vanilla, idle or roam, and in JSON a `duration`, `radius` or a task kind of a
later version (`goTo` and `job` name `routines.anchors` / `routines.jobs`).

## Sleep and staying awake

- **`SleepTask` is the game's own sleep.** At the decision Kiln asks the game's own sleep check for that villager. When
  the game's sleep window for it allows it (its own sleep and wake times), the game takes FindBed and the villager walks
  to its bed and sleeps. When it does not, the sleep is refused with `NativeSleepNotCurrentlyAllowed` (a task event)
  and the fallback runs until the next decision. A plan cannot make a villager sleep by day.
- **Beds.** `SleepTask` uses the villager's bed through the game's own FindBed (its assigned bed, or a free one in
  range). Routines never assign beds: assign a bed with `Survivors.AssignBed` (package 18).
- **Keep-awake.** While the plan's task is Idle, Roam, Sit or Inspect, the game's own "go to bed" check answers no for
  that villager: it stays up until its next decision, at night too. Vanilla and Sleep tasks leave the game's check
  alone. The game's sleep check runs at most once per decision (a refused Sleep followed by a vanilla fallback does not
  ask twice).

## Calls

| Member | Answers |
|---|---|
| `Routines.Assign(Villager, BlockHandle<RoutineBlock>)` | `Task<RoutineResult>`: binds the plan, or replaces the calling mod's plan on that villager |
| `Routines.Remove(Villager)` | `Task<RoutineResult>`: takes the calling mod's plan off; the game chooses again. No routine: `Ok`, revision 0, no event |
| `Routines.Resume(Villager)` | `Task<RoutineResult>`: ends a manual suspension (below); a Follow on the villager ends. Not suspended: `Ok`, nothing changes, no event |
| `Routines.Inspect(Villager)` | `RoutineSnapshot?`: the routine on the villager now (any mod's), or null |
| `NpcBlock.Routine` | an NPC block's plan (a routine of the mod by handle or name, or a plan written inline, which becomes the block `<npc block>.routine`): given to that villager at each world start when the world has no saved routine for it. Its owner is the mod |

All are `[ServerOnly]`; the calls are `[TickThreadOnly]`. A mod assigns only the plans it defined itself.

`RoutineResult(Ok, Reason, NpcId, Revision)` says the **assignment** changed (or already was as asked), not that a task
ran: `Ok` also when the villager is paused, not in the world, or its first task cannot start. The change is made at
the start of the next step and the task completes on that step's Post. What the villager's tasks do comes only as
`RoutineTaskChangedEvent`.

`RoutineSnapshot`: `NpcId`, `Owner` (mod id), `Key` (`<mod>:<name>`, the name being `Was` when given),
`PlanVersion`, `Revision`, `Assignment` (`RoutineAssignmentState`: `Assigned`, `ManualSuspended`, `PendingSpawn`,
`VersionMismatch`, `Removed`), `WindowIndex` (-1: the fallback or no task), `Invocation` (the task's serial),
`Task`, `TaskState` (`RoutineTaskState`: `None`, `Chosen`, `Started`, `Completed`, `Refused`, `Suspended`),
`NativeState` (the game's `VillagerState`), `LastReason`.

## Rules

- **One owner per villager.** The first mod whose assign is accepted holds the villager; it may replace its plan. Another
  mod's assign, remove or resume answers `Ok = false`, `Reason = "OwnedBy:<mod>"`, and changes nothing. There is no
  priority between mods. A mod's routines come with its own owner, whether its code assigned them or its blocks files
  (`npc.routine`) did, so after a restart its code can change the routines its data files gave. The owner's unload
  removes its routines from the running world at the next step (event reason `OwnerUnloaded`), but not from the world's
  save: a server stop unloads every mod before its last save, and a mod switched off may come back. A mod reloaded while
  the world runs gets its saved routines bound again as soon as it is back with its plans; at the next world start a
  saved routine is bound again when its mod and plan are there, else removed (`OwnerMissing`, `DefinitionMissing`).
- **Queue.** At most 256 changes wait for the next step; one more answers `QueueFull` before anything changed. A world
  that stops completes what is queued with `WorldStopped`. A mod that unloads is never called back: its queued changes
  are dropped (its tasks are canceled).
- **World start.** Until the world's saved routines are bound again (its data is there and no mod's data files wait for
  their pass, normally within the first steps), `Assign`, `Remove` and `Resume` answer `Ok = false`,
  `Reason = "WorldLoading"`: call again at a later step. Once they can, a call first binds the saved routines, so a
  change made early is never undone by them.
- **Revisions** count the changes of all assignments of a world and never go back, also across a restart.
- **Manual commands suspend.** `Villager.GoTo`, `Teleport`, `Stay` or `Follow` (or any other source of the game's Reset
  or GoTo command that the villager takes: a native plugin, another loader) suspend its routine: the running task ends
  `Suspended`, the assignment becomes `ManualSuspended` (event reason `ManualSuspended`), and the game decides for the
  villager until the owner calls `Routines.Resume`. `StopFollowing` does not resume. `Resume` ends a Follow at the
  call (a GoTo the Follow sent just before is dropped, not taken as a new manual command) and lets the plan decide again
  at the villager's next decision, without a teleport; the snapshot shows no task until then. A command the game ignores
  (a GoTo while the villager is in state `Start`) suspends nothing. `LastReason` is `ManualCommand` while suspended.
- **A player's summon tool suspends too.** The game's command carries no sender: a player re-homing a villager with the
  summon tool sends the same Reset as `Villager.Teleport`, and the routine is suspended (and saved so) until the
  owner's `Resume`. A mod that wants routines to follow re-homing calls `Resume` on `RoutineAssignmentChangedEvent` with
  reason `ManualSuspended` when no code of its own moved the villager. (The intended rule, re-homing keeps the routine,
  needs a hook on the summon tool's own path: not in this version.)
- **Interruptions stay the game's.** Water, the build cursor, dialogue and the game's other safety states interrupt a
  task as without a plan (task state `Suspended`, reason `SafetyInterrupt` or `Interrupted`); the plan decides again at
  the next decision. A task the game cannot do (no seat, no bed, no path) ends `Refused`, and the next decision tries
  again.

## Events

| Event | When | Notes |
|---|---|---|
| `RoutineAssignmentChangedEvent` | an assignment was made, replaced, suspended, resumed, migrated, refused at a restart or removed | `Before` (null for a new one), `After`, `Reason`, `Teardown`. Never dropped |
| `RoutineTaskChangedEvent` | a task was chosen, started, completed, refused or suspended | `NpcId`, `Key`, `PlanVersion`, `Revision`, `Invocation`, `Task`, `From`, `To`, `Reason`, `Teardown`. Can be dropped under load: when one villager changes its task more than 16 times in one step, the newest change is kept and the ones in between are lost (kiln.log counts them) |

Both are `[ServerOnly, AnyThread]` and observe only. They are posted on the tick thread on Post, after the step the
change was made in. When the world stops, every assignment gets one event with `Teardown` set (`After.Assignment`
`PendingSpawn`) and a running task one with `To = Suspended`, reason `WorldStopped`; these come while the world stops
or at the start of the next step: a handler must not use the world (villagers, entities) when `Teardown` is set.

Assignment reasons: `Assigned`, `Replaced`, `ManualSuspended`, `Resumed`, `Removed`, `OwnerUnloaded`, `WorldStopped`,
and at a restart `Migrated`, `VersionNotBumped`, `Downgrade`, `OwnerMissing`, `DefinitionMissing`, `OtherWorld`. Task
reasons: `NativeSleepNotCurrentlyAllowed`, `TaskFailed`, `NoTransition`, `SafetyInterrupt`, `Interrupted`,
`ManualSuspended`, `Replaced`, `Removed`, `WorldStopped`.

## Restart

Assignments are saved with the world (Kiln.Blocks' own world data, written with every world save): per villager its
NpcId, the owner mod, the plan's key, version and content hash, the manual suspension and the revision. Never an entity
id or the task that was running. The mods define their plans in `OnLoad`; when the world is ready, each saved assignment
is decided against the plans as defined now:

| Saved vs defined | Outcome | Event |
|---|---|---|
| same version, same content | bound again at its saved revision (a manual suspension stays) | none |
| a higher `Version` | migrated to the new plan, at a new revision | `Migrated` |
| same version, other content | refused; kept, suspended as `VersionMismatch` | `VersionNotBumped` |
| a lower `Version` | refused; kept, suspended as `VersionMismatch` | `Downgrade` |
| the owner mod is not loaded | removed | `OwnerMissing` |
| the plan is not defined any more | removed | `DefinitionMissing` |
| the saved book is another world's | removed | `OtherWorld` |

**Raise `Version` when you change a plan** (its windows or its fallback). Renaming a plan keeps its saved assignments
when the new block says `Was = "<old name>"`. After the restart the time of day decides: nothing of the time between is
replayed. The routine of an NPC block (`NpcBlock.Routine`) is given at each world start only to a villager the world has
no saved routine for and whose owner did not remove it (`Routines.Remove`: the removal is saved and sticks). A routine
refused at a restart (`VersionMismatch`) is not running; its owner's `Routines.Remove` takes it out of the save.

The hooks' counters (decisions, pauses and resumes, sleep checks kept awake, MarkWritten calls, manual commands) are in
kiln.log as `routines: counters (...)` lines, at a world's stop and about every 10 s while they change.

## Threads and errors

- The calls: tick thread (`InvalidOperationException` otherwise, and from code that is not a mod's). The answers of
  `Assign`, `Remove` and `Resume` complete on the tick thread, on the Post of the next step; their continuations are
  canceled when the calling mod unloads first.
- A refusal (another mod's routine, the queue, a stopped world, a revoked owner, `NotAssigned` for a resume without a
  routine): `Ok = false` with the reason. Never an exception.
- A bad argument: `ArgumentException` (`BlockError` at `Define`; at `Assign` a plan of another mod, or a handle that is
  no longer defined because its mod was reloaded: define it again and use the new handle).
- `routines.decisions` off: `KilnUnavailableException` naming `routines.decisions`, decided from what is true at the call.

## Limits (this version)

- No forced daytime sleep, no sleep or wake times of a plan's own.
- No idle duration or roam radius: the villager's own.
- No "go to a place" (`routines.anchors`) or production-job (`routines.jobs`) tasks. `Villager.GoTo` still works on its
  own, and suspends a routine.
- No bed assignment: assign a bed with `Survivors.AssignBed` (package 18).
- 64 windows per plan, 256 queued changes, 16 task events per villager and step.
- Villagers decide only near a player (about 150 m); a routine changes nothing while the game pauses a villager.

## Testing

**Test kit.** `KilnHarness.Routines` is a fake `kiln.routines` on which Kiln.Blocks' own `Routines` code and both
events run offline: one owner per villager, changes queued and committed at the start of the next step, 256 and then
`QueueFull`, revisions, the refusals, teardown at the harness's end. Nothing chooses by itself; the test plays the game:

| Member | Plays |
|---|---|
| `SetClock(hour, minute = 0)` | the game time the decisions read |
| `Decide(npcId)` | the villager reaching a decision; returns the task its routine took (`"idle"`, `"sleep"`, ...) or null when the game chose |
| `Finish(npcId, ok, reason = null)` | the game ending the current task, done or failed |
| `SetSleepAllowed(npcId, allowed)` | the game's own sleep check: a `SleepTask` is refused while it is false |
| `Queued` | how many changes wait for the next step |

Not modelled offline: the game's worker, chooser, walk, seat and bed; manual suspension by `Villager.GoTo` and the
others; restoring assignments after a restart ([../modding/unit-tests.md](../modding/unit-tests.md)).

**Lab test.** `mods/tests/routines-test` runs on a dedicated test server in two boots of one world: run 1 assigns,
decides, keeps a villager awake at 23:30 without the game's check running, checks the sleep check runs once in a
decision, suspends with `Stay` and resumes; the lab script restarts the server; run 2 finds the assignments again at
their revisions (and the suspension kept) and removes them. The lab has no player, so a lab-only hook in the test mod
asks for each decision (never product code). Lines `routines-test: PASS ...`, `RUN<n> DONE pass=<p> fail=<f>`.

**Guided check.** `16g-cc16g` in the client pass: real villagers in a base, the sample's day plan through the windows
and midnight, keep-awake, manual commands and resume, distance, and a restart, with a game without Kiln joined. Until the
owner has played it, routines in a real game are unconfirmed.
