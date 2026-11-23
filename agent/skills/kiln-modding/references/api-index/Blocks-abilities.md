# Blocks (Kiln.Blocks): abilities

## AbilityBlock — Something a character casts that is no staff's: on a key, a chat command, an item, or from code with `Abilities.Cast`.
- `Aim { get; init; } -> AbilityTargeting` — What it aims at: where the caster faces, or the nearest enemy within `AbilityBlock.Range` (and no enemy: no cast).
- `Cast { get; init; } -> CastBlock?` — Costs, timing and motion.
- `Command { get; init; } -> string?` — A chat command that casts it, without the prefix.
- `Cooldown { get; init; } -> double` — Seconds, per caster.
- `Delivery { get; init; } -> IDeliveryBlock?` — How it gets there.
- `Description { get; init; } -> string?` — What the ability does, as players read it.
- `Item { get; init; } -> ItemBlock?` — An item that casts it when used.
- `Key { get; init; } -> string?` — The default key ("G", "Shift+F"); players remap it in the Kiln menu.
- `Name { get; init; } -> string` — The ability's name, as players read it.
- `OnCast { get; init; } -> IReadOnlyList<EffectRef>?` — What happens to the caster the moment it leaves.
- `PlayersOnly { get; init; } -> bool` — Keys and commands work for players only.
- `Range { get; init; } -> double` — Metres, for `AbilityBlock.Aim`.
- `SpecialCost { get; init; } -> int` — Special-ability charge, the bar that fills in combat.
- `Was { get; init; } -> string?` — The name this block had before a rename.

## AbilityRef — An ability: what `Blocks.Define` returned, its name in this mod, or `"other-mod:name"`.
- `AbilityRef()`
- `AbilityRef(string Value)`
- `static implicit operator AbilityRef(BlockHandle<AbilityBlock> handle) -> AbilityRef` — The ability that `Blocks.Define` returned.
- `static implicit operator AbilityRef(string value) -> AbilityRef` — An ability by name: its name in this mod, or `"other-mod:name"`.
- `Value { get; init; } -> string` — The ability as written: its name in this mod, or `"other-mod:name"`.

## AroundBlock — Everyone around the caster.
- `Affects { get; init; } -> AreaAffects` — Whom it reaches: enemies of the caster (the default) or its friends, as the game's own enemy and friend checks tell.
- `Effects { get; init; } -> IReadOnlyList<EffectRef>` — What each one around the caster gets.
- `Radius { get; init; } -> float` — Metres.

## BeamBlock — Everyone along a line in front of the caster.
- `Affects { get; init; } -> AreaAffects` — Whom it reaches: enemies of the caster (the default) or its friends, as the game's own enemy and friend checks tell.
- `Effects { get; init; } -> IReadOnlyList<EffectRef>` — What each one along the line gets, at every hit.
- `Range { get; init; } -> float` — Metres.
- `Seconds { get; init; } -> double` — How long it keeps hitting, turning with the caster.
- `TickEvery { get; init; } -> double` — Seconds between hits while it lasts.
- `Width { get; init; } -> float` — Metres across.

## ChainBlock — From the first one in front of the caster, on to the next within reach of each, never the same one twice.
- `Affects { get; init; } -> AreaAffects` — Whom it reaches: enemies of the caster (the default) or its friends, as the game's own enemy and friend checks tell.
- `Angle { get; init; } -> float` — How wide the caster looks for the first, in degrees.
- `Effects { get; init; } -> IReadOnlyList<EffectRef>` — What each one it reaches gets.
- `JumpRange { get; init; } -> float` — Metres from one to the next.
- `Jumps { get; init; } -> int` — How many more after the first: 0 to 8.
- `Range { get; init; } -> float` — Metres to the first.

## ConeBlock — Everyone in a cone in front of the caster.
- `Affects { get; init; } -> AreaAffects` — Whom it reaches: enemies of the caster (the default) or its friends, as the game's own enemy and friend checks tell.
- `Angle { get; init; } -> float` — The cone's whole width, in degrees: 1 to 179.
- `Effects { get; init; } -> IReadOnlyList<EffectRef>` — What each one in the cone gets.
- `Range { get; init; } -> float` — Metres.

## EatRule — When a player can use a consumable.
- `If { get; init; } -> Func<Player, bool>?` — From code: your own check, on the tick thread.
- `OnRefused { get; init; } -> IReadOnlyList<EffectRef>?` — What happens to the player when it is refused.
- `Unless { get; init; } -> IReadOnlyList<PlayerStateRef>?` — Not while the player is in any of these.
- `When { get; init; } -> IReadOnlyList<PlayerStateRef>?` — Only while the player is in one of these.

## GameTreePlace — Where a talent sits in the game's own tree.
- `Class { get; init; } -> string` — The screen's colour: Warrior, Mage, Ranger or Neutral.
- `From { get; init; } -> IReadOnlyList<string>?` — Game nodes by `"0x<id>"` that lead here, as well as what the talent's `TalentBlock.Requires` says.
- `Offset { get; init; } -> double[]?` — [x, y] from the node it hangs on, in the tree's units.
- `Position { get; init; } -> double[]?` — [x, y] in the tree's units, about 1700 to 6500.
- `To { get; init; } -> IReadOnlyList<string>?` — Links out of it, to game nodes.

## HomingBlock — A projectile that turns toward what it was aimed at: what the game's own homing projectiles do.
- `Delay { get; init; } -> double` — Seconds it flies straight first.
- `SeekRange { get; init; } -> float` — Metres along its aim it looks for what to turn to, when it was given no target.
- `TurnSpeed { get; init; } -> float` — How fast it turns; the game's own run from 20 to 80.

## PerkBlock — An enhancement on an item, in its `Perks`.
- `Description { get; init; } -> string?` — The perk's description in its tooltip.
- `Icon { get; init; } -> string?` — The icon, when it is not the starter's: a `GamePerks` constant, whose icon it takes, or a PNG in the mod's folder...
- `Modifies { get; init; } -> IDictionary<Stat, double>` — Stats changed on whoever wears or wields it: `Modifies = { [Stat.MaxHealth] = 40 }`.
- `Name { get; init; } -> string` — The perk's name, as players read it in its tooltip.
- `OnHit { get; init; } -> HitsRef?` — What every hit they land does to what was hit.
- `OnHitTaken { get; init; } -> HitsRef?` — What every hit they take does to whoever dealt it.
- `OnKill { get; init; } -> HitsRef?` — What a hit that kills does to what it killed.
- `Tweak { get; init; } -> FieldValues` — Game fields this block does not cover, set on the perk it makes.
- `Value { get; init; } -> string?` — The number the tooltip shows in front of the name: `"5%"`, `"12"`, `"2.5"`, `"8s"`.
- `Was { get; init; } -> string?` — The name this block had before a rename.

## PlacedDelivery — A delivery that is not a flight.
- `PlacedDelivery(PlacedDelivery original)`
- `Look { get; init; } -> ProjectileLook` — On a spell, what shows in the hand.
- `Tweak { get; init; } -> FieldValues` — Game fields this block does not cover, set on the template it is placed as.
- `Vfx { get; init; } -> VfxRef?` — A VFX where it happens, facing where it goes.

## PlayerStateRef — Something a player does or is, which any of the game's states and actions can say: a `PlayerStateId` in C#, the same name or the game's own in JSON (`"Gliding"`...
- `PlayerStateRef()`
- `PlayerStateRef(string Name)`
- `static readonly Eaten -> PlayerStateRef` — The consumable has really gone.
- `static readonly Eating -> PlayerStateRef` — A player has started to use a consumable.
- `static implicit operator PlayerStateRef(PlayerStateId state) -> PlayerStateRef` — One of the game's states, by its id; the name it gets is the id as `0x` and four hex digits.
- `static implicit operator PlayerStateRef(string name) -> PlayerStateRef` — A state or action by name: the game's own (`"Gliding"`), `PlayerStateRef.Eating` or `PlayerStateRef.Eaten`.
- `Name { get; init; } -> string` — The state as written: the game's name (`"Gliding"`), a state id as `0x` and four hex digits, `"eating"` or `"eaten"`.

## SelfBlock — Only the caster: a buff, a heal, a dash.
- `Effects { get; init; } -> IReadOnlyList<EffectRef>` — What happens to the caster.

## SummonBlock — Something spawned in front of the caster: a `GameTemplates` constant or a template's GUID, as a child of the caster, at its level and of its faction.
- `Ahead { get; init; } -> float` — Metres in front of the caster.
- `Count { get; init; } -> int` — 1 to 16, side by side.
- `Seconds { get; init; } -> double?` — When they are taken away.
- `Template { get; init; } -> TemplateRef` — What is spawned: a `GameTemplates` constant or a template's GUID.

## TalentBlock — A node of a skill tree: in the game's own tree with `TalentBlock.Game`, or in a `TalentTreeBlock`'s `TalentTreeBlock.Nodes`.
- `CharacterLevel { get; init; } -> int` — In a tree: the character level needed.
- `Column { get; init; } -> int?` — In a tree: the grid cell.
- `Cost { get; init; } -> int` — Points per level.
- `Description { get; init; } -> string?` — Null: what `TalentBlock.Modifies` says, per level.
- `Game { get; init; } -> GameTreePlace?` — A place in the game's own tree.
- `Icon { get; init; } -> string?` — A PNG in the mod's folder.
- `Keeps { get; init; } -> StatusRef?` — A status they have for as long as they have the talent.
- `MaxLevel { get; init; } -> int` — Levels.
- `Modifies { get; init; } -> IDictionary<Stat, double>` — Stats changed, per level: `Modifies = { [Stat.Strength] = 2 }` is +2 a level.
- `Name { get; init; } -> string` — The talent's name, as players read it in the tree.
- `OnHit { get; init; } -> HitsRef?` — What every hit they land does to what was hit.
- `OnHitTaken { get; init; } -> HitsRef?` — What every hit they take does to whoever dealt it.
- `OnKill { get; init; } -> HitsRef?` — What a hit that kills does to what it killed.
- `OnStart { get; init; } -> IReadOnlyList<EffectRef>?` — What happens to them when it starts to work: at the unlock, and at each join.
- `PointsSpent { get; init; } -> int` — In a tree: points spent in it first.
- `RequireAll { get; init; } -> bool` — In a tree: all of `TalentBlock.Requires`, not any one.
- `Requires { get; init; } -> IReadOnlyList<string>?` — What leads here.
- `Row { get; init; } -> int?` — In a tree: the grid row.
- `Texts { get; init; } -> IReadOnlyDictionary<string, ItemTexts>?` — Texts per language: `["de"] = new ItemTexts { Name = "...", Description = "..." }`.
- `Unlocks { get; init; } -> AbilityRef?` — An ability only those with this talent can cast.
- `Was { get; init; } -> string?` — The name this block had before a rename.

## TalentPointsBlock — Where a tree's points come from.
- `PerLevel { get; init; } -> int` — More per character level.
- `ShareOfGame { get; init; } -> double?` — 0..1: a share of the game's skill points instead.
- `Start { get; init; } -> int` — What a level 1 character has.

## TalentTreeBlock — A whole tree of its own, on Kiln's screen in the game's look, opened from the pause menu or a key.
- `Description { get; init; } -> string?` — A text shown with the tree's name.
- `Key { get; init; } -> string?` — A key that opens it.
- `Menu { get; init; } -> string?` — Its entry in the pause menu.
- `Name { get; init; } -> string` — The tree's name, shown on its screen.
- `Nodes { get; init; } -> IDictionary<string, PartRef<TalentBlock>>` — Node name to talent, inline, by handle or by block name.
- `Points { get; init; } -> TalentPointsBlock?` — Where its points come from.
- `Respec { get; init; } -> bool` — Everything can be given back for free.
- `Was { get; init; } -> string?` — The name this block had before a rename.

## TriggerBlock — Any state or action of a player drives effects.
- `Cooldown { get; init; } -> double` — Seconds, per player, before it can happen again.
- `Do { get; init; } -> IReadOnlyList<EffectRef>?` — What happens to the player.
- `If { get; init; } -> Func<Player, bool>?` — From code: your own check, on the tick thread.
- `Item { get; init; } -> ItemRef?` — Only with this item: what an action uses, what is eaten, and for a state what is in hand or worn.
- `On { get; init; } -> PlayerStateRef` — A state entered, an action started, or `PlayerStateRef.Eating` and `PlayerStateRef.Eaten`.
- `OnEnd { get; init; } -> IReadOnlyList<EffectRef>?` — What happens to the player when they leave the state.
- `Stop { get; init; } -> bool` — Stops the action, or the eating.
- `Unless { get; init; } -> IReadOnlyList<PlayerStateRef>?` — Not while the player is in any of these.
- `Was { get; init; } -> string?` — The name this block had before a rename.
- `When { get; init; } -> IReadOnlyList<PlayerStateRef>?` — Only while the player is in one of these.

## ZoneBlock — A place that keeps hitting whoever is in it.
- `Affects { get; init; } -> AreaAffects` — Whom it reaches: enemies of the caster (the default) or its friends, as the game's own enemy and friend checks tell.
- `Ahead { get; init; } -> float` — Metres in front of the caster.
- `Effects { get; init; } -> IReadOnlyList<EffectRef>` — What each one in the place gets, at every hit.
- `Radius { get; init; } -> float` — Metres.
- `Seconds { get; init; } -> double` — How long the place keeps hitting, in seconds.
- `TickEvery { get; init; } -> double` — Seconds between hits on whoever is in it.
