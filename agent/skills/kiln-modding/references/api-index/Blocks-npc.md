# Blocks (Kiln.Blocks): npc

## AIs — The game's AI tunings.
- `static readonly Heavy -> AIBlock` — A heavy fighter: the Vukah's AI, slower to turn away, holding its ground longer.
- `static readonly Scavenger -> AIBlock` — The scavenger melee fighter.
- `static readonly Still -> AIBlock` — One that stands where it is put and never starts a fight: guards, townsfolk.
- `static readonly Vukah -> AIBlock` — The Vukah: a melee fighter that sees 18 m by day (11 m at night) and keeps its distance while it waits.

## AnimationBlock — How a body moves: the clips of a glTF, which of them the engine's states play, your own states, and what plays over them.
- `Clips { get; init; } -> string?` — The glTF file of the mod (its path in the mod's folder): its armature is the skeleton, its animations the clips.
- `Dead { get; init; } -> Clip?` — Plays when it dies, and holds its last frame.
- `Fall { get; init; } -> Clip?` — The clip the engine's Fall state plays.
- `Hit { get; init; } -> Clip?` — Plays over everything when it is hit (the engine's hit overlay).
- `Idle { get; init; } -> Clip?` — The clip the engine's Idle state plays.
- `IdleCombat { get; init; } -> Clip?` — The clip the engine's IdleCombat state plays.
- `IdleSneak { get; init; } -> Clip?` — The clip the engine's IdleSneak state plays.
- `Key { get; init; } -> string` — How JSON names a premade.
- `Layers { get; init; } -> IReadOnlyList<LayerBlock>?` — More animations over the states.
- `Like { get; init; } -> string?` — In JSON, the premade this one starts from.
- `Retarget { get; init; } -> RetargetBlock?` — The clips are authored on a rig of their own: play them on `AnimationBlock.Skeleton`, a body's, by pairing joints...
- `Run { get; init; } -> Clip?` — The clip the engine's Run state plays.
- `Skeleton { get; init; } -> string?` — The skeleton's name: the file's name without its extension when left out.
- `Sneak { get; init; } -> Clip?` — The clip the engine's Sneak state plays.
- `Sprint { get; init; } -> Clip?` — The clip the engine's Sprint state plays.
- `Stagger { get; init; } -> Clip?` — What a hit reaction, a stun or a parry plays over the whole body: the base creature's hit sequences play it.
- `States { get; init; } -> IDictionary<AnimState, Clip>` — States of your own: what attacks, rules and sequences play by name.
- `Stun { get; init; } -> Clip?` — The clip the engine's Stun state plays.
- `Template { get; init; } -> string?` — A game creature whose graph this is (the premades).
- `Walk { get; init; } -> Clip?` — The clip the engine's Walk state plays.

## Animations — The game's graphs, as animations to give a creature of that body.
- `static readonly Fisher -> AnimationBlock` — The game's fisher trader animation graph.
- `static readonly Scavenger -> AnimationBlock` — The game's scavenger animation graph (the scavenger melee creature's).
- `static readonly Villager -> AnimationBlock` — A villager's graph: the fisher trader's.
- `static readonly Vukah -> AnimationBlock` — The game's Vukah animation graph (the Vukah melee creature's).

## AnimState — A state of an animation graph: what a sequence, an attack or the AI names to make a body play something.
- `AnimState(string name)` — A state of this mod's animation by its name, or the engine's (`"Idle"`), or one of the game's graphs...
- `static Dead { get; } -> AnimState` — The engine's dead state.
- `Equals(AnimState? other) -> bool` — True when both are the same state.
- `static Fall { get; } -> AnimState` — The engine's fall state.
- `static Game(Guid id) -> AnimState` — A state of one of the game's graphs, by the GUID its sequences name.
- `static Idle { get; } -> AnimState` — The engine's idle state: standing still.
- `static IdleCombat { get; } -> AnimState` — The engine's idle state in combat.
- `static IdleSneak { get; } -> AnimState` — The engine's idle state while sneaking.
- `static implicit operator AnimState(string name) -> AnimState` — A state by its name: the same as `new AnimState(name)`.
- `Name { get; } -> string` — What it was given as: a name, an engine state's name, a GUID or an id.
- `static Run { get; } -> AnimState` — The engine's run state.
- `static Sneak { get; } -> AnimState` — The engine's sneak state.
- `static Sprint { get; } -> AnimState` — The engine's sprint state.
- `static Stun { get; } -> AnimState` — The engine's stun state.
- `static Walk { get; } -> AnimState` — The engine's walk state.

## AttackBlock — One attack: the state it plays, how near it has to be, how often, and what its hit does.
- `Angle { get; init; } -> double` — Degrees in front of it that its hit reaches.
- `Chance { get; init; } -> double?` — 0..1: how often it is chosen when it could be.
- `Cooldown { get; init; } -> double?` — Seconds after any attack of the creature before this one is chosen again.
- `Delivery { get; init; } -> IDeliveryBlock?` — A ranged attack: what leaves at `AttackBlock.HitAt`, as a spell's delivery.
- `HitAt { get; init; } -> double` — Seconds into it that the hit happens.
- `Length { get; init; } -> double?` — Seconds it lasts.
- `MinRange { get; init; } -> double?` — The shortest.
- `Name { get; init; } -> string?` — What rules, `PickAttack` and `Act.Attack` call it.
- `OnHit { get; init; } -> IReadOnlyList<EffectRef>?` — What a hit does, to everything in front within `AttackBlock.Range`.
- `Plays { get; init; } -> AnimState?` — The state it plays: one the animation has.
- `Range { get; init; } -> double` — The longest distance to its target it starts from.
- `Sequence { get; init; } -> Guid?` — A game sequence this attack plays as it is, on a game body: its GUID.
- `Style { get; init; } -> IReadOnlyList<AttackStyle>?` — When the game considers it.

## AttackStyle — When the game considers an attack (`keen::enemy01::AttackStyle`).
enum: Normal, Retaliate, CounterAttack, StunBreak, Special, Initiate, InitiateFollowUp, IgnoreThreat, Flanking

## BehaviourBlock — What a creature does: attacks and rules, on top of the game's combat AI (blocking, flanking, hit reactions stay).
- `Attacks { get; init; } -> IReadOnlyList<PartRef<AttackBlock>>?` — The attacks, each a block or the name of one defined once.
- `Brain { get; init; } -> Func<NpcBrain>?` — Your code drives it instead of the game's AI: a new `NpcBrain` for each creature.
- `BrainName { get; init; } -> string?` — In JSON: a brain registered by name with `Blocks.Brain`.
- `Key { get; init; } -> string` — How JSON names a premade.
- `Like { get; init; } -> string?` — In JSON, the premade this one starts from.
- `Perception { get; init; } -> NpcPerceptionPolicy` `0.2.3` — How the brain sees: distance and faction (the default), or also voxel line of sight.
- `Phases { get; init; } -> IReadOnlyList<string>?` — The phases its rules name.
- `PickAttack { get; init; } -> Func<NpcView, string?>?` — Decides the attack each time it would choose, on the tick thread: an attack's name, or null for the game's choice.
- `PickAttackName { get; init; } -> string?` — In JSON: an attack picker registered by name with `Blocks.AttackPicker`.
- `PickEvery { get; init; } -> double?` — Seconds between the decisions of `BehaviourBlock.PickAttack` and of the AI's target picker, more than 0 and at most 60.
- `Rules { get; init; } -> IReadOnlyList<PartRef<RuleBlock>>?` — The rules, each a block or the name of one defined once.
- `Template { get; init; } -> string?` — Keeps this game creature's attacks and rules; the block's come after them.

## Behaviours — The game's behaviours.
- `static readonly None -> BehaviourBlock` — Nothing of its own: no attacks, no rules.
- `static readonly ScavengerMelee -> BehaviourBlock` — The scavenger's.
- `static readonly VukahMelee -> BehaviourBlock` — The Vukah's attacks and rules.

## Blends — Blends for `AnimationBlock`'s states: `Walk = Blends.ByDirection("WalkF", "WalkB")`.
- `static ByDirection(string forward, string back, string? left = null, string? right = null) -> Clip` — The engine's move direction picks the animation: forward at 0 deg, back at +/-180 deg, left and right at -90 deg and...
- `static On(string input, params (double At, string Clip)[] clips) -> Clip` — Any input, with an animation at each value.

## BlendSpace — Animations mixed by an input: at each value of the input its animation, blended in between.
- `Circular { get; init; } -> bool?` — The input wraps around, as a direction does (-180 is 180).
- `Clips { get; init; } -> IDictionary<string, string>` — Value of the input -> the glTF animation that plays there.
- `Input { get; init; } -> string` — An input the engine fills (`moveDirection`, `hitDirection`, `lookYaw`, ...), or a name of your own a sequence sets.

## Bodies — The game's bodies.
- `static readonly Fisher -> BodyBlock` — The fisher trader.
- `static readonly Scavenger -> BodyBlock` — The scavenger's body.
- `static readonly Villager -> BodyBlock` — A villager: the fisher's body.
- `static readonly Vukah -> BodyBlock` — The Vukah's body.

## BodyBlock — What a creature is: a model of yours on its skeleton (cooked when the mod builds), or a game creature's body (`Bodies`).
- `Colliders { get; init; } -> IReadOnlyList<ColliderBlock>?` — Its collision: the first shape for hits, the second for what it bumps into; one shape is both.
- `Key { get; init; } -> string` — How JSON names a premade.
- `Like { get; init; } -> string?` — In JSON, the premade this one starts from.
- `Model { get; init; } -> string?` — A skinned glTF of the mod (its path in the mod's folder).
- `Scale { get; init; } -> double?` — Its size for each creature that has it (1 = as modelled).
- `Size { get; init; } -> BodySize?` — How much room it needs to walk: radius and height in metres, for paths and steps.
- `Skeleton { get; init; } -> string?` — The skeleton's name: the model file's name without its extension when left out.
- `Slots { get; init; } -> IDictionary<Slot, string>?` — The game's attachment slots on the joints of its skeleton: where gear hangs and hits come from.
- `Template { get; init; } -> string?` — A game creature whose body this is (the premades).
- `Tool { get; init; } -> string?` — The item that summons a roster NPC of this body, which a `RosterBlock`'s tool starts from.
- `Workshop { get; init; } -> string?` — A game NPC's workshop: what a vendor on this body sells from (`0x...`).

## BodySize — How much room a body needs: what paths are searched with.
- `BodySize(double Radius, double Height)`
- `Height { get; init; } -> double` — Its height in metres, above 0.
- `Radius { get; init; } -> double` — Half its width in metres, 0 or more.

## Capsule — A capsule standing on the ground.
- `static Of(double radius, double height) -> ColliderBlock` — A capsule collider of this radius and height in metres.

## Clip — What a state plays: an animation of the glTF by its name, or a blend of several (`Blends`).
- `Clip()` — An empty clip: set `Clip.Name` or `Clip.Blend`.
- `Clip(string name)` — The glTF animation by its name.
- `Blend { get; init; } -> BlendSpace?` — Several animations mixed by an input, such as the direction a creature walks.
- `BlendIn { get; init; } -> double?` — Seconds it blends in over.
- `Bones { get; init; } -> IReadOnlyList<string>?` — On `AnimationBlock.Hit`: only these joints.
- `static implicit operator Clip(string name) -> Clip` — The glTF animation by its name: the same as `new Clip(name)`.
- `Loop { get; init; } -> bool?` — Plays again from the start.
- `Name { get; init; } -> string?` — The glTF animation.
- `Reference { get; init; } -> string?` — On `AnimationBlock.Hit`: the animation whose pose it is relative to, so it adds to whatever plays.
- `RootMotion { get; init; } -> bool?` — The motion of the root joint moves the creature instead.
- `Speed { get; init; } -> double?` — Plays faster (above 1) or slower.
- `Then { get; init; } -> AnimState?` — Moves on to this state of the same animation when its clip ends.

## ColliderBlock — A collision shape of a body, centred on its root.
- `Height { get; init; } -> double` — A capsule's full height, its caps included.
- `Radius { get; init; } -> double` — The radius in metres.
- `Shape { get; init; } -> string` — `capsule` or `sphere`.

## CreatureBlock — A creature: its body, how it moves, how it sees, what it does.
- `AI { get; init; } -> PartRef<AIBlock>?` — Default: the body's, for a game body; the Vukah's for one of your own.
- `Animation { get; init; } -> PartRef<AnimationBlock>?` — Default: the body's own, for a game body.
- `Behaviour { get; init; } -> PartRef<BehaviourBlock>?` — Default: the body's, for a game body; none for one of your own.
- `Body { get; init; } -> PartRef<BodyBlock>` — The body: a premade (`Bodies`) or one of your own, as a block or the name of one defined once.
- `Drops { get; init; } -> PartRef<LootTableBlock>?` — What it drops when it dies, besides what the game gives it: a `LootTableBlock` of this mod (`"other-mod:name"` for...
- `Name { get; init; } -> string?` — Its name, for logs and Kiln players (vanilla players see the body's own).
- `Spawns { get; init; } -> IReadOnlyList<SpawnBlock>?` — Where it turns up by itself, in place of the game's creatures.
- `Stats { get; init; } -> CreatureStats?` — What it is worth in a fight: its level, its damage and experience factors, and the damage it takes of each kind.
- `Was { get; init; } -> string?` — The name before a rename: ids come from the oldest name.

## CreatureHandle — What `Blocks.Define` gives back for a creature: its variant, template and arsenal, and a way to spawn it.
- `Arsenal { get; } -> Guid?` — Its arsenal, when its behaviour has attacks or rules; null otherwise.
- `Spawn(WorldPosition at, float yawDegrees = 0, CreatureSpawnOptions? options = null) -> Task<EntityId>` — Spawns one with all its parts.
- `States { get; } -> IReadOnlyDictionary<string, uint>` — The state ids its animation answers to, by name.
- `Template { get; } -> string` — The template its creatures are: a game creature's, or a new one for a body of its own.
- `Variant { get; } -> string` — The kiln.creatures variant every creature of this block is: `/creature spawn` takes it.

## CreatureStats — What a creature is worth in a fight.
- `Damage { get; init; } -> double?` — Its share of damage dealt, 1 as the game has it.
- `Experience { get; init; } -> double?` — Its share of experience given, 1 as the game has it.
- `Level { get; init; } -> int?` — 1..60: its health and damage follow from it as the game scales them.
- `Takes { get; init; } -> IDictionary<DamageKind, double>` — Damage it takes of a kind, 1 as normal, 0 immune.

## INpcBlock — The blocks of a creature: what `Blocks.Define` takes for them asks players' games for no more than the parts need.

## LayerBlock — An animation played over the states: always, or each time an overlay fires (the engine fires `hit` on a hit reaction).
- `Blend { get; init; } -> BlendSpace?` — Several animations mixed by an input, in place of `LayerBlock.Clip`.
- `Bones { get; init; } -> IReadOnlyList<string>?` — Only these joints.
- `Clip { get; init; } -> string?` — The glTF animation.
- `FadeIn { get; init; } -> double?` — Seconds it fades in over.
- `FadeOut { get; init; } -> double?` — Seconds it fades out over.
- `Name { get; init; } -> string?` — A name for the layer.
- `Overlay { get; init; } -> string?` — `hit`, or a name of your own a sequence fires.
- `Reference { get; init; } -> string?` — The animation whose pose it is relative to: then it adds to what plays.
- `Speed { get; init; } -> double?` — Plays faster (above 1) or slower.
- `Weight { get; init; } -> double?` — How strongly the layer applies.

## NpcBlock — A friendly NPC: a creature with a conversation (`NpcBlock.Talk`, the quests' `DialogueDefinition`), wares and a place in the roster.
- `AI { get; init; } -> PartRef<AIBlock>?` — Not supported: setting it is refused.
- `Animation { get; init; } -> PartRef<AnimationBlock>?` — Not supported: setting it is refused, as an NPC moves like the game's villagers.
- `Behaviour { get; init; } -> PartRef<BehaviourBlock>?` — Not supported: setting it is refused.
- `Body { get; init; } -> PartRef<BodyBlock>` — The body, as a block or the name of one defined once.
- `Name { get; init; } -> string` — The name players read, in English; `NpcBlock.Names` adds the other languages.
- `Names { get; init; } -> IDictionary<string, string>?` — Its names in other languages: `{ ["de"] = "Tobin der Wanderer" }`.
- `Place { get; init; } -> NpcPlace?` — Where it is put in the world.
- `Roster { get; init; } -> RosterBlock?` — Its place in the game's roster of NPCs.
- `Talk { get; init; } -> DialogueDefinition?` — What it says: a dialogue (`Dialogues.Define`), or the handle of a `DialogueBlock`...
- `TalkName { get; init; } -> string?` — A dialogue by name, in JSON `"talk"`: a `DialogueBlock` of this mod by its block name, or one of the mod's...
- `Vendor { get; init; } -> VendorBlock?` — What it sells, at its workshop.
- `Was { get; init; } -> string?` — The name before a rename: ids come from the oldest name.

## NpcBlocksApi — What creature blocks add to `Blocks` and `Act`.
Extension members (C# 14): call them on the receiver (`Act.X`, `Blocks.X`), not on `NpcBlocksApi`.
- `Blocks.Action(string name, Action<NpcView> run) -> void` — A rule action for data files: `{ "custom": "name" }`.
- `Blocks.AttackPicker(string name, Func<NpcView, string?> pick) -> void` — An attack picker for data files: `"pickAttack": "name"`.
- `Blocks.Brain(string name, Func<NpcBrain> make) -> void` — A brain for blocks in data files to name: `"brain": "name"`.
- `Blocks.Condition(string name, Func<NpcView, bool> check, double every = 1) -> void` — A rule condition for data files: `{ "custom": "name" }`.
- `Act.Effect(EffectRef effect, NpcEffectOn on = NpcEffectOn.Target) -> RuleAction` — Runs an effect block: on the creature's combat target (it did it), or on itself.
- `Blocks.Spawn(string creature, WorldPosition at, float yawDegrees = 0, CreatureSpawnOptions? options = null) -> Task<EntityId>` — Spawns a creature block of this mod (or `"other-mod:name"`) with all its parts.
- `Blocks.Target(string name, Func<NpcView, EntityId> pick) -> void` — A target picker for data files: `"pickTarget": "name"`.

## NpcBrain — Your code in place of the game's AI, for each creature whose behaviour names it: `Behaviour = new BehaviourBlock { Brain = () => new MyBrain() }`.
- `virtual Start(NpcControl npc) -> void` — Once, when it takes the creature over.
- `virtual Stop(NpcControl npc) -> void` — Once, when the creature is gone or the mod unloads.
- `abstract Tick(NpcControl npc) -> void` — Every step while the creature lives.

## NpcControl — One creature as a brain sees and drives it.
- `Alive { get; } -> bool` — True while the creature exists and is not dead.
- `Attack(string name) -> bool` — Starts one of its attacks by name: its sequence plays, and its hit lands where it lands.
- `Creature { get; } -> CreatureRef` — The creature's own changes, as a mod's layers: level, faction, AI values, overrides.
- `Effect(string effect, EntityId? on = null) -> void` — Runs an effect block on what it is after (or on `on`), with this creature as the one who did it.
- `Entity { get; } -> EntityId` — The creature's entity.
- `Face(WorldPosition to) -> void` — Turns toward a place where it stands.
- `MoveTo(WorldPosition to, bool run = false) -> void` — Walks (or runs) toward a place: the body turns and moves as its walk or run state plays.
- `Name { get; } -> string` — The creature block's name.
- `PerceptionPolicy { get; } -> NpcPerceptionPolicy` `0.2.3` — How this creature sees: its behaviour's `BehaviourBlock.Perception` as it was when the creature was spawned.
- `PerceptionStats { get; } -> NpcPerceptionStats` `0.2.3` — What its sight did so far: how many looks, and how each candidate's line of sight ended.
- `Pose(AnimState state) -> void` — Holds a state of its animation while no sequence plays (the state input when nothing else sets one)...
- `Position { get; } -> WorldPosition?` — Where the creature is in metres, or null when it does not exist or has no transform.
- `Seen { get; } -> IReadOnlyList<EntityId>` — Enemies within its sight, nearest first, looked for four times a second.
- `Stop() -> void` — Stands still.
- `Target { get; set; } -> EntityId` — What it is after: the nearest of `NpcControl.Seen` until you set another.
- `TargetDistance { get; } -> double?` — Metres to `NpcControl.Target`, or null.
- `Time { get; } -> double` — Seconds since the brain took over.
- `View { get; } -> NpcView` — The creature as rules see it: health, phase, position.

## NpcEffectOn — Where a rule's effect lands.
enum: Target, Self

## NpcOcclusionMode `0.2.3` — What stands between a brain and what it sees.
enum: DistanceAndFaction, Voxels

## NpcPerceptionPolicy `0.2.3` — How a creature with a brain sees.
- `FromOffset { get; init; } -> WorldPosition` — Added to the creature's position: where it looks from.
- `Occlusion { get; init; } -> NpcOcclusionMode` — What stands between the brain and what it sees.
- `ToOffset { get; init; } -> WorldPosition` — Added to a candidate's position: what of it must be visible.

## NpcPerceptionStats `0.2.3` — What a brain's sight did since the creature was spawned.
- `NpcPerceptionStats(ulong Updates, ulong Checks, ulong Visible, ulong Occluded, ulong NotLoaded, ulong OutOfGrid, ulong BudgetExceeded, ulong OutOfRange, ulong EntityGone, ulong OtherRefused, ulong CapabilityUnavailableUpdates, string? LastRefusal)`
- `BudgetExceeded { get; init; } -> ulong` — Candidates whose line could not be checked because the step's ray budget was used up.
- `CapabilityUnavailableUpdates { get; init; } -> ulong` — Looks made while `queries.line_of_sight` was off.
- `Checks { get; init; } -> ulong` — How many candidates were put to the line-of-sight question, in `NpcOcclusionMode.Voxels`.
- `EntityGone { get; init; } -> ulong` — Candidates that had left the world when the line was checked.
- `LastRefusal { get; init; } -> string?` — The latest look's last refusal, such as `OutOfGrid`, or null.
- `NotLoaded { get; init; } -> ulong` — Candidates whose line could not be checked because the terrain there was not loaded.
- `Occluded { get; init; } -> ulong` — Candidates the line of sight found blocked by terrain or a building.
- `OtherRefused { get; init; } -> ulong` — Candidates whose line was refused for any other reason, or whose ray was degenerate (no length, or not a number).
- `OutOfGrid { get; init; } -> ulong` — Candidates whose line could not be checked because it left the grid the query covers.
- `OutOfRange { get; init; } -> ulong` — Candidates farther than a ray reaches, counted without asking the query.
- `Updates { get; init; } -> ulong` — How many looks the brain made (four a second).
- `Visible { get; init; } -> ulong` — Candidates the line of sight found clear: they are seen.

## NpcPlace — Where an NPC is put: next to the first player, or at a spot.
- `At { get; init; } -> double[]?` — The spot, as x, y and z.
- `Distance { get; init; } -> double[]?` — Metres from the first player, between these two.
- `Exact { get; init; } -> bool` — Exactly `NpcPlace.At`: no ground, no base check.
- `InBase { get; init; } -> bool` — A base is fine (the default): a friendly NPC may stand in one, so it comes as near as `NpcPlace.Distance` asks while...
- `Once { get; init; } -> bool` — Only the first time in a world.

## Price — A price: so many of an item.
- `Price(ItemRef Item, int Count)`
- `Count { get; init; } -> int` — How many of it.
- `Item { get; init; } -> ItemRef` — The item asked for.
- `static Of(ItemRef item, int count = 1) -> Price` — A price of `count` of `item`.

## RetargetBlock — How `AnimationBlock.Clips` authored on another rig play on `AnimationBlock.Skeleton`: joints are paired by name, each paired joint turns as its source turns away from...
- `ByName { get; init; } -> bool?` — Pair joints of the same name that `RetargetBlock.Map` leaves out.
- `Map { get; init; } -> IDictionary<string, string>` — The clip file's joint -> the skeleton's joint.
- `Reference { get; init; } -> RetargetPose?` — A pose of the clip file that shows the skeleton's rest posture, when the file's own rest pose doesn't (an A-pose rig...
- `Scale { get; init; } -> double?` — How far translations go on the skeleton.
- `Translate { get; init; } -> IReadOnlyList<string>?` — The clip file's joints whose movement carries over.

## RetargetPose — A frame of one of the clip file's animations: `new("TPose")`.
- `RetargetPose(string Animation, double At = 0)`
- `Animation { get; init; } -> string` — The name of one of the clip file's animations.
- `At { get; init; } -> double` — Seconds into it, from 0.

## RosterBlock — A place in the game's roster of NPCs: unlocked, summoned with a tool, given a bed.
- `Description { get; init; } -> string?` — The roster page's text about it.
- `RoamingRange { get; init; } -> double?` — How far it wanders from where it is placed.
- `SummonTool { get; init; } -> string?` — What the tool that summons it is called under its name ("Tobin's Whistle").
- `UnlockedBy { get; init; } -> string?` — A knowledge name of the mod's quests that unlocks it; null: always.

## RuleBlock — A rule: on a trigger, every so often, when its conditions hold, do something.
- `Chance { get; init; } -> double` — 0..1.
- `Cooldown { get; init; } -> double` — Seconds before it can fire again.
- `Do { get; init; } -> IReadOnlyList<RuleAction>` — What it does when it fires, in order: kiln.creatures' `Act` actions.
- `Every { get; init; } -> double` — Seconds between looks at the conditions, more than 0 and at most 3600.
- `Name { get; init; } -> string?` — What the rule is called.
- `On { get; init; } -> RuleMode` — When it is looked at: every `RuleBlock.Every` seconds by default, or on entering a phase, starting an attack, ...
- `When { get; init; } -> IReadOnlyList<RuleCondition>` — All of these hold.

## Shape — Where particles head at spawn: `Shape.Up`, `Shape.Sphere`, `Shape.Outward`, or `Shape.Cone`.
- `static Cone(float degrees) -> Shape` — A cone around up: half angle in degrees, 0 to 180.
- `static readonly Outward -> Shape` — Away from the centre of the `ParticleBlock.Shape`: out of a sphere, along a disc's plane, across a line.
- `static readonly Sphere -> Shape` — In any direction.
- `static readonly Up -> Shape` — Straight up along the effect's up.

## SpawnBlock — Where a creature turns up by itself: in place of the game's creatures of a template.
- `Chance { get; init; } -> double` — 0..1 of them.
- `DayOnly { get; init; } -> bool` — Only by day.
- `NightOnly { get; init; } -> bool` — Only at night.
- `Replaces { get; init; } -> string` — A game creature: `GameTemplates` constant or name.

## Sphere — A sphere.
- `static Of(double radius) -> ColliderBlock` — A sphere collider of this radius in metres.

## VendorBlock — What an NPC sells: item -> its price.
- `Gives { get; init; } -> IDictionary<ItemRef, int>?` — How many one trade gives, per item; 1 when left out.
- `Sells { get; init; } -> IDictionary<ItemRef, Price>` — The wares: each item to its price, which becomes a recipe at the workshop.
- `Workshop { get; init; } -> string?` — The workshop (`0x...`).
