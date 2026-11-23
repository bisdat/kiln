# Creature and NPC blocks

A creature is four parts: a **body** (what it is), an **animation** (how it moves), an **AI** (how it notices and chases) and a **behaviour** (what
it does). Each part is a block you can define once and reuse, or write inline. The premades (`Bodies.Vukah`, `Animations.Vukah`, `AIs.Vukah`,
`Behaviours.VukahMelee`) are plain instances of the same blocks. An NPC is a body with a conversation, wares and a place in the roster.

Sample: `mods/samples/CreatureBlocks`, a creature made entirely of its own files (a script writes its glTF), a Vukah with a rule of its own, a creature
driven by C#, and a wandering trader.

```csharp
var crush = Blocks.Define("crush", new EffectBlock { Damage = Damage.Blunt(35), Knockback = Knockback.Small });

Blocks.Define("stoneling", new CreatureBlock
{
    Name = "Stoneling",
    Body = "stone-body",                    // blocks/stoneling.json: a model of your own
    Animation = "stone-moves",              // and its clips
    AI = AIs.Vukah with { Sight = 24, Faction = Faction.Vukah },
    Behaviour = new BehaviourBlock
    {
        Attacks = [new AttackBlock { Name = "slam", Plays = "slam", Range = 2.8, HitAt = 0.55, OnHit = [crush] }],
        Rules = [new RuleBlock { Every = 1, When = [Is.HealthBelow(0.5)], Do = [Act.SetPhase("cracked"), Act.Effect(crush)] }],
    },
    Stats = new CreatureStats { Level = 12 },
    Spawns = [new SpawnBlock { Replaces = "Enemy_Vukah_Melee", Chance = 0.1, NightOnly = true }],
});
```
```jsonc
{ "format": 1,
  "animations": { "stone-moves": { "clips": "models/stoneling.glb", "idle": "Idle", "walk": "Walk", "run": "Run", "dead": "Dead",
                                   "hit": { "clip": "Flinch", "reference": "Ref" }, "states": { "slam": { "clip": "Attack" } } } },
  "bodies": { "stone-body": { "model": "models/stoneling.glb", "slots": { "HandR": "hand_r" }, "size": { "radius": 0.5, "height": 2.2 } } },
  "creatures": { "stoneling": { "name": "Stoneling", "body": "stone-body", "animation": "stone-moves",
                                "ai": { "like": "vukah", "sight": 24, "faction": "Vukah" },
                                "behaviour": { "attacks": [ { "name": "slam", "plays": "slam", "range": 2.8, "hitAt": 0.55, "onHit": [ "crush" ] } ] } } } }
```
JSON sections: `creatures` (with `drops`), `npcs`, and for parts `bodies`, `animations`, `ais`, `behaviours`, `attacks`, `rules`. A JSON part starts from a premade with
`"like"`; in C# that is `AIs.Vukah with { … }`.

## Two kinds of creature
| Body and animation | What it is | `clients` |
|---|---|---|
| The game's (`Bodies.Vukah`, the default animation) | The game's creature with a new mind: a variant of it, with your AI values, attacks and rules | `none`; `kiln` once it has attacks of its own (a new sequence) |
| Yours (`Model`, `Clips`) | A new template: the AI's creature, playing your graph and drawing your model | `mod`: every player's game needs the files at start |

The build works out `clients` from the blocks: C# blocks through the analyzer, data files through the manifest task.

## Bodies and animations with files of their own
A body names a skinned glTF of the mod (`Model`), an animation a glTF whose animations are its clips (`Clips`); one file can be both. They are
**cooked when the mod builds** (`kiln build`, or `kiln cook` in the mod's folder): the cook reads `blocks/*.json` itself, so `assets/cook.json` needs no
steps (`{ "format": 1, "tool": "animcook", "steps": [] }`). It makes the skeleton, the clips, a state machine graph per animation, and the model
([animations.md](animations.md), [models.md](models.md#skinned-models)).

**A body or animation with files is written in a data file.** The cook does not run your code, so it cannot see one written in C#; `Define` refuses
such a block, says where it belongs, and C# names the data file's block instead. Everything else (AI, behaviour, the creature itself) can be in either.

### Animation
| Field | What |
|---|---|
| `Idle` | Needed: the clip it stands in |
| `Walk`, `Run` | Needed when the AI moves it. `Blends.ByDirection("WalkF", "WalkB")` blends by the direction it walks |
| `Sprint`, `IdleCombat`, `Fall`, `IdleSneak`, `Sneak`, `Stun` | The engine's other states. `IdleCombat` and `Fall` stand as in Idle when left out |
| `Dead` | Plays when it dies and holds its last frame |
| `Hit` | Plays over everything when it is hit (the engine's hit overlay). With `Reference` it adds to what plays |
| `Stagger` | What a hit reaction, a stun or a parry plays over the whole body. Default: `Hit`'s clip once, else Idle. `Stun` defaults to it, `Dead` to Idle, held |
| `States` | Your own: `[new AnimState("slam")] = new Clip("Attack") { Speed = 1.2 }`, in JSON `"slam": { "clip": "Attack", "speed": 1.2 }`. `Then` moves on to another state when the clip ends |
| `Layers` | More clips over the states, always or on an overlay |
| `Skeleton` | The skeleton's name, the file's name by default: a body and an animation share a skeleton when they name the same one |
| `Retarget` | The clips are authored on a rig of their own (an animation pack, another tool's skeleton): they play on `Skeleton`, a body's, by pairing joints. See below |

A clip is a glTF animation's name. A state machine takes at most 34 transitions: the cook nests machines past that, and you list states.

**Clips from another rig.** An animation pack rarely uses your body's joint names or proportions. Point `Clips` at the pack, `Skeleton` at the body's
skeleton, and pair the joints:

```json
"animations": { "golem-moves": { "clips": "models/anim-pack.glb", "skeleton": "golem",
    "retarget": { "map": { "mixamorig:Hips": "pelvis", "mixamorig:Spine": "spine", "mixamorig:Head": "head", "mixamorig:RightArm": "arm_r" } },
    "idle": "Idle", "walk": "Walking" } }
```

Joints of the same name pair by themselves. The rigs may differ in bone lengths and rest orientations, as long as both rest poses show the same posture
(a T-pose for a T-pose); when the pack's rest pose doesn't, `reference` names a frame of it that does. Body joints with no pair keep their rest pose. The hips'
travel is scaled by the rigs' hip heights (`scale` sets it). What each field does: [animations.md](animations.md#retargeting).

### Body
`Model`, `Skeleton`, `Scale`, `Slots` (the game's attachment slots on your joints: where gear hangs and where hits come from), `Size` (radius and height,
for paths) and `Colliders` (a capsule or sphere for hits and one for what it bumps into; one shape is both). Its bounds for culling are the model's.
`Cloth` and `Chains` give its model cloth and swinging joints: [Model physics](#model-physics).

### Model physics
Since 0.2.0-dev.31 a body of your own can carry **cloth** (`ModelClothBlock`: a primitive of its model the game simulates as cloth) and **bone
chains** (`ModelChainBlock`: 2 to 8 joints of its skeleton that swing). They are cooked into the model when the mod builds; the game simulates them
on players' machines. The fields, ranges and rules are those of `assets/physics.json` ([models.md](models.md#physics)). Like every body with files,
it is written in blocks/*.json:
```json
"bodies": { "caped": { "model": "models/caped.glb",
  "cloth": [ { "mesh": 0, "primitive": 1, "pin": [1, 1, 1, 0, 0, 0], "massKg": 0.8,
               "colliders": [ { "type": "capsule", "a": { "x": 0, "y": 1.2, "z": 0.1, "radius": 0.15, "joint": "spine" },
                                                   "b": { "x": 0, "y": 0.7, "z": 0.1, "radius": 0.12, "joint": "spine" } } ] } ],
  "chains": [ { "rootJoint": "tail_0", "tipJoint": "tail_3", "particleMassKg": 0.5 } ] } }
```
The same body as C# records (a unit test, or C# that repeats a data file's body):
```csharp
new BodyBlock
{
    Model = "models/caped.glb",
    Cloth = [new ModelClothBlock { Mesh = 0, Primitive = 1, Pin = [1, 1, 1, 0, 0, 0], MassKg = 0.8,
                                   Colliders = [new ClothCapsuleBlock(new(0, 1.2, 0.1, 0.15, "spine"), new(0, 0.7, 0.1, 0.12, "spine"))] }],
    Chains = [new ModelChainBlock { RootJoint = "tail_0", TipJoint = "tail_3", ParticleMassKg = 0.5 }],
}
```
- **Cooked, never at run time.** The cook reads the body in blocks/*.json and writes a spec hash of its cloth and chains into the model's
  `.model.json`. A creature whose body carries cloth or chains is refused at `Define` unless the body's model was cooked with exactly these
  ("cooked without cloth or chains" / "with other cloth or chains"): build the mod again. The body's own fields are checked when it is defined. A body written in C# is not seen by the cook; it can name a model whose cloth comes
  from blocks/*.json or `assets/physics.json`, with the same values. A body without `Cloth` and `Chains` asks nothing (its model may have cloth from
  physics.json).
- **One place per model.** A model's physics come from one body or from physics.json, never both.
- **Scale 1.** A body with cloth or chains keeps `Scale` unset or 1 (the game scales the cloth's colliders by the model's own size).
- **On, not yet confirmed in a game** (since 0.2.0-dev.31): the cook writes them with no switch; guided check `16e-cc16e` is still open. Should a
  release hold the cook features again, the cook writes them only with `"experimental": ["modelcook.cloth"]` (and `"modelcook.chains2"`) in
  `assets/cook.json`, and a mod with such a body logs one warning (`Blocks.Warnings`) while the game reports the row held.
- **Chains need the body's own skeleton**, and no clip of it may animate a swinging joint; a cloth vertex may not be weighted to one.
- A game NPC body keeps its own physics (`Cloth` and `Chains` are refused there: "a game body has its own").
  Authored roster bodies have a separate [boot-publication contract](#authored-roster-bodies-and-motions); authoring
  cloth or chains is not evidence that the roster body can yet be built or rendered.

## What Kiln checks, and why
The game checks none of this; a mistake shows as a creature frozen in a pose, or a blow that never lands. So `Define` refuses, naming block and field:

| Rule | Because |
|---|---|
| A body of your own and its animation share a skeleton, and the cooked model and graph name the same one | The game poses a model only by the skeleton its graph names: otherwise it draws unposed |
| An attack's `Plays` is a state of the animation | A sequence that sets a state the graph lacks plays nothing, and the hit that follows the clip never comes |
| `Plays` is a state of yours, or an engine state with a known GUID (`Run` has none) | A sequence names a state by GUID |
| A creature that moves has `Walk` and `Run` | The engine asks for them by id while it walks |
| A behaviour that keeps a game creature's attacks (`Behaviours.VukahMelee`) needs that creature's animation | Its sequences play that creature's states |
| A game body plays its own graph, a body of yours an animation of yours | The skeleton again |
| A rule's phase or attack is one of the behaviour's | A typo would be a phase nothing enters |

What a body of your own gets from the AI's creature, and what Kiln changes so it can't disagree with your animation:
- the AI's fields that name a state (combat idle, patrol, flee, sleep, swimming) point at your Idle, IdleCombat and Walk;
- its hit reactions, stuns, parries and death are sequences of its own: copies of the base creature's whose state events play your `Stagger` (or
  `Dead` for a death), keeping an engine state your graph has (Stun, Fall, Dead). They are made when content is ready, from the running game's data;
- the base creature's other motions that are sequences of its states (taunt, blocking, call for help, idle turns, sleep, its spawn sequence) are off;
- its gear (`SubEntities`) is left out.

## Behaviour
**Rules first.** A creature keeps the game's combat: blocking, flanking, hit reactions, choosing among its attacks. You add attacks and rules.

- `AttackBlock`: `Plays` a state, `Range`, `MinRange`, `Cooldown`, `Style`, `Chance`, `HitAt`, `OnHit` (what the blow does to everything in front within
  range, through `Angle` degrees) or a `Delivery` (a projectile, a cone, any of the spells' deliveries) for a ranged attack. Kiln makes its sequence: the
  attack's state from the start, the blow at `HitAt`, turning to the target before it. On a game body, `Sequence` plays one of the game's as it is.
  `Angle` is the **whole cone aperture**, not a half-angle: more than 0 and at most 360 degrees. `359` really reaches 359 degrees (a one-degree gap behind);
  `360` hits all around, including directly behind, with no directional test. Angles are never silently narrowed.
  `Range` is more than 0 and at most 200 metres; `MinRange` is at least 0 and strictly less than `Range`.
  `HitAt` is at least 0 and strictly less than `Length` when supplied; `Length` is more than 0 and at most 30 seconds.
  The cooked clip's duration can impose a shorter limit at runtime. `kiln lint` checks the authored bounds and relationships before the block loads.
- `RuleBlock`: `On` (when it is looked at), `Every`, `Cooldown`, `Chance`, `When` (all hold), `Do`. Conditions and actions are kiln.creatures'
  ([creatures.md](../services/creatures.md#rules-and-arsenals)): the game's kinds (`Is.HealthBelow`, `Is.TargetWithin`, `Is.Game(...)`, `Act.Game(...)`),
  Kiln's (`Is.InPhase`, `Is.Night`, `Act.SetPhase`, `Act.Attack`), your code (`Is.Custom`, `Act.Custom`), and `Act.Effect(effect)`: an effect block on the
  creature's target, from the creature, or `NpcEffectOn.Self` on itself.
- **Your code never runs on a job thread.** `Is.Custom` runs on the tick thread every `Every` seconds and the game reads its last answer; `Act.Custom`
  is queued and runs at the next step. In JSON: `{ "custom": "name" }`, with `Blocks.Condition("name", npc => …)` and `Blocks.Action(...)` in `OnLoad`.
- `PickAttack` (an attack's name, or null for the game's choice) and `AIBlock.PickTarget` replace one decision. In JSON `"pickAttack": "name"` with
  `Blocks.AttackPicker`, `"pickTarget"` with `Blocks.Target`.
- `Phases`: the phases its rules may name.
- More kinds (`Is.InCombat`, `Is.TargetIsPlayer`, `Is.EnemiesNear`, `Is.SummonsBelow`, `Act.Flee`, `Act.Immune`, `Act.Summon`, `Act.Cast`, `Act.Heal`, ...): [npc-ai.md](npc-ai.md#behaviourblock-rules).

**`Brain`, opt-in per creature**: `Brain = () => new MyBrain()`, in JSON `"brain": "name"` with `Blocks.Brain("name", () => new MyBrain())`. The game's AI
is off for that creature, and with it the game's perception: `NpcControl` sees for it (`Seen`, enemies within the AI's `Sight` by the game's faction
rule; `Target`, the nearest). `MoveTo`, `Stop`, `Face`, `Attack(name)`, `Pose(state)`, `Effect(...)`; `Creature` for its values. `Tick` runs on the tick
thread every step. You lose what the game's combat states did; a creature still takes hits and plays its sequences.

`MoveTo` is not pathfinding: each step it turns the creature toward the point and walks straight at it, into a wall if
one is in the way. For a route around terrain and buildings, ask the game's path search with `Queries.FindPath`
([queries.md](../services/queries.md#paths)) and give the brain its copied points one after another, each with
`MoveTo`. A route is a copy from the step the game answered: ask again when the creature stops getting closer.

```csharp
sealed class Sentinel : NpcBrain
{
    public override void Tick(NpcControl npc)
    {
        if (npc.Target.IsNone) { npc.Stop(); return; }
        if (npc.TargetDistance < 2.8) npc.Attack("slam");
        else npc.MoveTo(World.Position(npc.Target)!.Value, run: true);
    }
}
```

**Walls.** By default a brain sees through them: `Seen` is distance and faction. A behaviour can ask for voxel line
of sight:

```csharp
Behaviour = Behaviours.None with
{
    Brain = () => new Sentinel(),
    Perception = new NpcPerceptionPolicy { Occlusion = NpcOcclusionMode.Voxels },
}
```

```json
"behaviour": { "brain": "sentinel", "perception": { "occlusion": "voxels", "toOffset": { "x": 0, "y": 0.8, "z": 0 } } }
```

Then a candidate is seen only when `Queries.VoxelLineOfSight` ([services/queries.md](../services/queries.md)) says
the line from the creature (`fromOffset`, 1.5 m up by default) to it (`toOffset`) is clear. Terrain and building voxels
block it; props and other creatures do not. A candidate whose line could not be checked (the area is not loaded, the
step's shared ray budget is used up, `queries.line_of_sight` is off) is **not seen**: a brain that asked for walls never
gets wall vision back. `npc.PerceptionStats` counts what happened (`Visible`, `Occluded`, `NotLoaded`,
`BudgetExceeded`, `CapabilityUnavailableUpdates`, ...) and `LastRefusal` names the latest reason. Many brains with
`Voxels` share the 64 rays of a step with every other mod. The policy is read when a creature is spawned, and it is an
error (`behaviour.perception`) on a behaviour without a brain: the game's own AI sees as the game decides.

## AI
`Sight`, `SightAngle`, `Aggro`, `Leash` (its home zone's radius), `Faction`, `Moves`, `Fights`, `PickTarget`, and `Tune` for any other field of the game's AI
by path (`["combatSetup.preferedDistance"] = "3"`). Night sight, hearing, roaming, fleeing, feature switches and **who it fights** (`Relations`, `SenseFilter`, `Factions.Relate`): [npc-ai.md](npc-ai.md). It starts from a game creature's tuning (`Template`, the Vukah's by default), because hundreds of fields
have no neutral value. A body of your own runs that creature's AI program; a friendly fighter is a creature with `Faction = Faction.PlayerAlly`.
`Sight` is how far it sees a target by day (`combatCommitmentSetup.view.range`, the field the game's sight check reads; at night times the AI's
`nightRangeFactor`, the Vukah's 0.6: 18 m by day, 11 m at night), `SightAngle` half its view cone (`combatCommitmentSetup.view.coneHalfAngle`), `Aggro`
the distance within which a target it sees is engaged at once (`combatCommitmentSetup.gain.inRange.range`). `aggroSetup` is not its sight on this game
build: `viewRange` only sizes the search for candidates (never below 64 m, so `Sight` sets it too beyond that), and `aggroRange` and
`viewConeHalfAngle` are not read. Details: [creatures.md](../services/creatures.md).

## Server and client
A creature's arsenal, variant and spawn rules belong to the server world. A server writes them as the mod loads. A game client loads mods off the tick
thread and has a server world only while it hosts one, so there they are written at the first step of a hosted world, before anything of it spawns; a
client joined to another server never needs them. What a block can't take then is an error of the mod by block and field, as in a data file.
Brains are the server's too: they run on the tick thread of a server world (a dedicated server's, or the one a client hosts), never in a client's
menu or in its world joined to another server.

## Spawning
`Blocks.Spawn("stoneling", position)` or the handle's `Spawn`; `/creature spawn <mod>:<name>` takes the variant. `Spawns` puts it in place of the game's
creatures of a template, by chance, at night or by day.

## Drops
`Drops` names a loot table: what the creature drops when it dies.
```csharp
Blocks.Define("ember-scavenger", new CreatureBlock { Body = Bodies.Vukah, Drops = "ember-cache" });   // a lootTables block, a handle, or one written here
```
```jsonc
{ "creatures": { "ember-scavenger": { "body": "vukah", "drops": "ember-cache" } } }
```
Kiln rolls the table on the server where the creature dies, with or without a player near, and drops the stacks as loose pickups around the spot. The game's own
loot is not touched, so a creature that dies near a player should drop that as well (not yet checked with a player). The table, its fields and how it rolls: [item-blocks.md](item-blocks.md#loot-tables). The creature
asks nothing more of players' games (`clients` stays `none`, unless an item in the table is new).

## NPCs
```csharp
Blocks.Define("tobin", new NpcBlock
{
    Name = "Tobin the Wanderer",
    Body = Bodies.Fisher,
    Talk = tobinTalk,                                        // the handle of a DialogueBlock, or a DialogueDefinition; TalkName names one by its name
    Vendor = new VendorBlock { Sells = { [GameItems.Material_T1_Plant_Fibre] = Price.Of(GameItems.Material_T1_Twigs, 2) } },
    Roster = new RosterBlock { SummonTool = "Tobin's Whistle", Description = "Tobin trades a little." },
    Place = new NpcPlace { Distance = [6, 10] },
});
```
It becomes the quests' NPC ([npcs.md](../services/npcs.md)): a new NPC of its own, cloned from the body's game NPC, with its dialogue, so quests name it as a
`giver` or in `talk`. Its wares are recipes at the body's workshop, which the game's NPC of that workshop shows too. **Its roster entry and lines are built
by the game at start**:
- an NPC block in `blocks/*.json` is read by that start-up build from the mod's folder, so it is there at the **first start**, on a server and on each
  player's game;
- one written in C# is known only once the mod's code runs, so Kiln writes it to `kiln/data/kiln-quests/native/<mod>.blocks.json` (the data folder: a
  redeploy keeps it, and nothing is written into the mod) and it is there from the next start. Write NPCs in data files to have them at once.

`Talk` is the handle `Blocks.Define("tobin-talk", new DialogueBlock { ... })` gives back, or any `DialogueDefinition` from `Dialogues.Define`. In JSON, and in C# as `TalkName`, a
name: a dialogue block of this mod by its block name (`"talk": "tobin-talk"`), compiled first when a data file defines it later, or, when no block has
that name, one of this mod's `quests/*.json`. Another mod's dialogue is refused: the startup build compiles each mod's NPC lines separately. A name that is another kind of block is refused. [quests.md](quests.md) "Quests as blocks" has the dialogue and quest blocks.

`Place` puts it once per world: `At` a spot, or `Distance` metres from the first player in game. A base is fine (`InBase`, default true), so it comes
that near while the player is home; `InBase = false` keeps it 20 m outside every base zone, as a quest's places are. `Exact` takes `At` as it is. The
world remembers the spot, so a change of `Place` shows in a new world. It then walks about its spot within the roster's `RoamingRange`, as the game's
villagers do (the body's own when not set).

A `quests/*.json` of the mod's own that names the same NPC wins over both. Players need the mod.

### Authored roster bodies and motions

New authoring types in **mod API 0.2.17** (dev.31).

The authoring contract accepts a `BodyBlock.Model`, an `AnimationBlock` and `NpcBlock.Motions` (a reusable
`npcMotions` block or an inline `NpcMotionsBlock`). The chassis is Kiln's Fisher/Villager chassis; authors name
Kiln slots and graph states, not game component paths. **Authoring support alone does not build a body:** `Define`
refuses `body.model` with the `blocks.npc_body` dependency reason until `quests.boot_pack` and `quests.npc_body`
are available. The internal per-mod rollback backend is present, but the native body builder is not yet installed.
There is no public bootpack or epoch API. Ordinary `Bodies.Villager` / `Bodies.Fisher` NPCs retain their authoring format.

For an authored roster body:

- Give it a private model/rig. Sharing that file or skeleton with an eager creature, look, unrelated body/animation,
  or recipe graph/model is refused with both users named; the cook never removes the other user's assets.json entries.
- Set `Size` explicitly: radius **0.05–4 m**, height **0.1–8 m**, both finite. The cooked model's floor must be
  within **0.05 m of Y0**, and its bounds must fit the radius-height box. Keep `Scale` unset or **1**, and
  `Colliders` unset or empty. Creature blocks retain their existing scale and collider support.
- Map `Root`, `Head` and `HandR` to joints. Model and graph must use the same cooked skeleton; the graph must
  contain every binding target and each target must have a GUID. Hash-only states such as `Run` cannot be used
  by a sequence binding.
- Each `NpcMotionBinding(Role, Variant, State)` is unique. Sit and Sleep variants **0/1/2** are
  enter/loop/exit: only the middle variant loops. Door variants **0/1/2** are guarded alternatives, not phases.
  Other roles have variant **0**. For Interaction, omit `InteractionKey` or leave it null: Kiln fills the chassis
  key internally. An explicit compatibility key is still accepted only when it matches the supported chassis;
  other roles leave the key null. No game GUID is needed for normal motion authoring.
- Required bindings are **Sit 0–2, Sleep 0–2, Inspect 0, Jump 0, Door 0–2**. Every other missing role needs an
  explicit `OptionalFallback`, which is clamped rather than allowed to run a vanilla motion on an authored graph.
  There is no implicit fallback for a required motion. A complete Fisher vector has **27 bindings**.
- Clip duration, speed and loop information come from the cook, not guesses based on a state GUID. A motion
  target needs a single cooked clip, with finite positive effective duration; blends are refused for these bindings.

The cook writes this group's resource/blob keys, file hashes, canonical bindings and effective durations to
`assets/cooked/npc-bodies.v1.json`, described by `schema/npc-bodies.schema.json`. It removes those entries from the eager
`assets/assets.json` group. This is a machine-local cache, excluded from mod identity and restored with the cooked tree
when a cook fails. The two cook tools merge their own contributions. Output hashes prove this machine's bytes only;
`sourceHash` is derived from authored specs and sources for cross-machine agreement. A server marks renderer rows
`skippedOnServer` with no borrowed output hash. An unfinished or renderer-skipped body remains incomplete.
A shared cache receipt tracks the final two-tool result. None of this is a Ready status or evidence of body publication.
At start the cache counts as a cooked file like any other ([cooking.md](cooking.md)): under kiln.json `"cooked": "local"` a
cache this game's cook did not make (no receipt) refuses the body's mod, under `"warn"` (the default) it is read and `kiln.log` says so.
External glTF buffer/image URIs are percent-decoded and resolved relative to the glTF; `../shared/...` is allowed only
while it stays inside this mod. Escapes, nested links, and a `link/..` walk are refused before source hashing.
A legacy NPC model block missing its animation or motion part stays on the ordinary eager asset-cook path, with a
warning naming that NPC's field. This preserves its other assets; it does not make the incomplete NPC definition valid.

Material-block and texture hashes are included as dependencies, but generated material JSON is not material-resource
publication. The prepared material bridge is still required before that closure can be committed. No hot body or
graph replacement is supported; body resources belong to the process's boot generation. `clients: mod` and
`side: both` are required. Never override a vanilla template GUID to bypass content admission.

On a dedicated server the cook makes no renderer output, so there is no `<model>.model.json` next to the graph. There
the NPC block takes the model's GUID from the `keen::RenderModel` row of `assets/cooked/npc-bodies.v1.json` that is
marked `skippedOnServer` (its file is `assets/cooked/<model path with / as _>.bin`), and it skips the bounds check:
each player's game checks the bounds against its own cooked model. A game client always reads the sidecar.

#### Is the body there? `NpcBodies.Status`

New in **mod API 0.2.17** (dev.31). A handle from `Blocks.Define` never means the body was built: the game builds
roster bodies once, at start. Ask:

```csharp
var status = NpcBodies.Status(tobinHandle);           // or NpcBodies.Status("my-mod:tobin") for a block without a handle
if (!status.Ok) Log.Warn($"{status.BlockKey}: {status.State}: {status.Reason}");
Events.On<NpcBodyBuildChangedEvent>(e => Log.Info($"{e.After.BlockKey}: {e.Before.State} -> {e.After.State}"));
```

| `State` | `Ok` | Meaning |
|---|---|---|
| `Ready` | true | this start's boot build published the body; `Template`, `Model`, `Graph` and `Generation` say what it built |
| `Failed` | false | the boot build refused it, and `Reason` is the refusal (layout, dependency, ownership, roster, network clearance), or it was defined in C# (below). A well-formed key with no build record at all is `Failed` with the reason `NoDefinition` |
| `PendingRestart` | false | not reported in this version (a body defined in C# is `Failed`, below) |
| `RemovalPendingRestart` | false | the mod was unloaded while the body was Ready. What the game built stays until the process ends; nothing live is taken away |
| `Removed` | false | Ready at the previous start (the native ledger `kiln-quests/native/npc-bodies.ledger.json`) and gone now |

`Status` runs on any thread. A key that is not `mod:block` and a handle that is not an NPC block's throw
`ArgumentException`; with `blocks.npc_body` off it throws `KilnUnavailableException` naming `blocks.npc_body`. A bad
block is still the compiler's `BlockError` when you define it: a native refusal is a status, never an exception.

`NpcBodyBuildChangedEvent` comes **once per change of state or generation**, Failed and RemovalPendingRestart
included. What the start built comes as a change from "no record" (`Before` is `Failed`/`NoDefinition`). On a
dedicated server it is posted on the tick thread at the first service tick after the change; on a game client
(`ClientSide` true) on the main thread before the client world's next step, so a handler there must not assume the
server's tick. Observe only.

#### A body defined in C#

An NPC block with a body of its own defined in C# (`Blocks.Define` in `OnLoad`) is not built in this version. Its
definition is checked as any block's (a bad one is still a `BlockError`), but Kiln writes no file for the next start, the
status is `Failed` with the reason *an authored roster body defined in C# is not built in this version (...): define the
NPC block in blocks/\*.json*, and `kiln.log` says the same. Such a body would have no cooked source identity for the join
check and skip the client's material check. Put authored bodies in `blocks/*.json`: they are built at the first start.

#### The join check

Players need the same bodies as the server, built the same way: having the mod installed is not enough. HELLO and
WELCOME carry `kiln.npcbodies`, a hash of each Ready body's per-body closure hash ([network.md](../network.md#npc-bodies-at-the-join)),
and the server refuses a player whose bodies differ, naming the first body that does. A player whose game says a body
waits for its restart (the handshake's `pending` key; this version sends none) is refused with a restart-required reason.
`modSync: off` waives this check, as it waives the player animation graph's: such a player joins and the server only logs
a warning. That player may lack a body the server spawns; keep `modSync` on for servers with authored bodies. A mod's
`NetClientHandshakeEvent` handler cannot waive this check (it may refuse a player for reasons of its own).

Not verified in a real game yet: the server's model GUID from the body manifest against a player's cooked model, the
event on a player's main thread, and a refusal for a different body shown on a player's screen. The tests cover them
offline (`Kiln.Blocks.Tests`, `Kiln.Network.Tests`); the test kit scripts the start's rows with `KilnHarness.NpcBodies`
([unit-tests.md](unit-tests.md)).

## Ids
From the mod id and the block's name: the template, variant (`<mod>:<name>`), arsenal, attacks (`<mod>:attack:<name>`), sequences, phases and states
(`<mod>:state:<name>`, the same id the cook gives the graph's state). The cooked graph is the animation block's name, or `<animation>.<body>` for a body
with slots. `Was` keeps the ids through a rename.

## Limits and costs
| What | Today | What it takes |
|---|---|---|
| A body or animation with files written in C# | Data files only | A source generator that hands the build the block's cook steps: M |
| Dodges and the base's other motions on a body of your own | Off (taunt, blocking, call for help, idle turns, sleep, spawn sequence) | Copies onto your states, as hit reactions are: S per kind |
| `Sight` with line of sight in a brain's own perception | Voxels only, on request (`behaviour.perception`, dev.28): terrain and buildings block, props and creatures do not | A collider ray: L |
| A new FSM state, faction relations | Not built | Hooks on `HandleCondition`/`StateDispatch`, the faction leaf: M each |
| Villager routines | Built in dev.31: an NPC block's `routine`, or `Routines.Assign` ([routines](../services/routines.md)) | - |
| Ragdolls | A `Dead` state | Client physics: XL, not planned |

## Test
`mods/tests/blocks-npc-test`: run `build-assets.py` (it writes the creature's glTF and cooks it), then start a lab twice with the force-ready switch
(`kiln/data/kiln.creatures/force-ready`). It logs `blocks-npc-test: PASS` and `FAIL` lines, and `blocks-npc-test done: N PASS, M FAIL`.

`mods/tests/npc-body-test` (authored roster bodies): `lab.sh bodies` lays Kiln out in a dedicated-server lab, cooks the mod with the
server's files, adds a broken neighbour (the same body whose glTF changed after its cook) and starts three times: the build, a restart
(with `handshake.py`'s fake clients at the join check), and one without the mod. It checks the native `NPC body` lines, the body ledger
and the mod's `npc-body-test: PASS` lines. The sample `mods/samples/NpcBody` is the same kind of body for authors: run `make-body.py`.
