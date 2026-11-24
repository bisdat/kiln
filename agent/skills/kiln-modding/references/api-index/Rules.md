# Rules (Kiln.Rules)

## CharacterFlightMode — How `PlayerRules.CharacterFlight` lets the character fly.
enum: Off, Unlimited, BuildingArea

## Creative — Creative mode for one player: the `creative` preset (`Rules.Preset`: Kiln's own, or the server's `<configDir>/rules/creative.json`) applied as a Kiln rule set for the...
- `static Effects(EntityId player) -> IReadOnlyList<CreativeEffectState>` `0.2.18` — Fresh status of every named effect, including locked/excluded effects.
- `static IsOn(EntityId player) -> bool` — Whether `player` is in creative mode.
- `const PermissionNode = "kiln.command.creative" -> string` — The command's permission node (level Admin; a hosted world's host and the console have it).
- `static Players { get; } -> IReadOnlyList<EntityId>` — The players in creative mode.
- `const PresetName = "creative" -> string` — The preset's name.
- `static Set(EntityId player, bool on) -> CreativeResult` — Turns creative mode on or off for `player`.
- `static SetEffect(EntityId player, string name, bool on) -> CreativeResult` `0.2.18` — Restores or withdraws one named effect on Creative's own live rule set.
- `static SetFlight(EntityId player, CharacterFlightMode mode) -> CreativeResult` — Sets how `player`'s character flies in creative mode (`PlayerRules.CharacterFlight` on their creative rule set).
- `static Status() -> string` — One line on who is in creative mode and which hooks work on this build.

## CreativeEffectState `0.2.18` — One Creative contribution.
- `CreativeEffectState(string Name, string Label, bool Requested, string Effective, string Restore, string? Locked, bool OtherOwners)`
- `Effective { get; init; } -> string`
- `Label { get; init; } -> string`
- `Locked { get; init; } -> string?`
- `Name { get; init; } -> string`
- `OtherOwners { get; init; } -> bool`
- `Requested { get; init; } -> bool`
- `Restore { get; init; } -> string`

## CreativeResult — What `Creative.Set` did.
- `CreativeResult(bool Ok, bool On, string Message)`
- `Message { get; init; } -> string` — One line for the one who asked.
- `Ok { get; init; } -> bool` — False when it was refused (Message says why).
- `On { get; init; } -> bool` — Whether the player is in creative mode now.

## CreativeTools — Creative mode's tools beyond the rules: taking items from the game's catalogue, the time of day and the day and night cycle.
- `static CanGive(EntityId player) -> bool` — Whether `player` may take items: creative mode is on for them (which only an admin can turn on), and this build can...
- `static Catalog() -> IReadOnlyList<ItemInfo>` — The server's catalogue for the menu: every item of this world (mods' items included), without the game's debug, test...
- `static Found(EntityId player) -> IReadOnlyList<ItemId>?` — The catalogue items `player` has found: the game's item knowledge (keyed by the item id; set when the player picks...
- `static Give(EntityId player, ItemId item, int count = 1, int? level = null, ItemRarity? rarity = null) -> Task<CreativeResult>` — Gives `count` of `item` to the player, as a stack the game makes (gear with a level and rarity when given).
- `static Give(EntityId player, ItemId item, int count, int? level, ItemRarity? rarity, int upgrades, bool gemSlot) -> Task<CreativeResult>` — As `CreativeTools.Give`, for gear also with `upgrades` of its upgrades unlocked (5: all; each raises the level by 1...
- `static Give(EntityId player, ItemId item, int count, int? level, ItemRarity? rarity, int upgrades, bool gemSlot, SlotId? slot) -> Task<CreativeResult>` — As above, into `slot` of the player's own backpack first (the creative menu's backpack grid); the rest goes where...
- `const MaxGive = 9999 -> int` — Items one request may give at most.
- `static MayChangeTime(EntityId player) -> bool` — Whether `player` may change the world's time: they have the creative permission (`Creative.PermissionNode`; admins...
- `static SetCycle(EntityId player, TimeCycle cycle) -> CreativeResult` — Sets the day and night cycle (Normal, Stopped, Fast) for the whole world, for this session.
- `static SetCycle(TimeCycle cycle, string who) -> CreativeResult` — Sets the day and night cycle, with no check of who asks (see `CreativeTools.SetTime`).
- `static SetTime(EntityId player, double hours) -> CreativeResult` — Jumps the world's clock forward to `hours` (0..24).
- `static SetTime(double hours, string who) -> CreativeResult` — Jumps the world's clock forward to `hours`, with no check of who asks: for the console, commands that check their...

## EffectiveRules — A player's rules after every rule set that applies was combined, and the server's limits held.
- `BuildCost { get; init; } -> double` — The multiplier on what placing takes from the inventory (`PlayerRules.BuildCost`).
- `BuildInShroud { get; init; } -> bool` — The player may build inside the Shroud (`PlayerRules.BuildInShroud`).
- `BuildOutsideBases { get; init; } -> bool` — The player may build outside bases (`PlayerRules.BuildOutsideBases`).
- `BuildRefund { get; init; } -> double` — The multiplier on what dismantling and undo give back (`PlayerRules.BuildRefund`).
- `CharacterFlight { get; init; } -> CharacterFlightMode` — The character flight mode in force (`PlayerRules.CharacterFlight`); `CharacterFlightMode.Off` when none.
- `Denied { get; init; } -> IReadOnlyList<string>` — What the server's limits or unavailable features took away, when they did.
- `DigDamage { get; init; } -> double` — The multiplier on the damage the player's tools deal to terrain and building blocks (`PlayerRules.DigDamage`).
- `FlightCarriesBody { get; init; } -> bool` — The character goes where the player flew and is put on the ground when the flight ends...
- `FlyAnywhere { get; init; } -> bool` — The building fly mode works outside bases too (`PlayerRules.FlyAnywhere`).
- `FreeCrafting { get; init; } -> bool` — Crafting takes no input costs (`PlayerRules.FreeCrafting`).
- `FreezeShroudTimer { get; init; } -> bool` — The Shroud timer does not go down in the Shroud (`PlayerRules.FreezeShroudTimer`).
- `From { get; init; } -> IReadOnlyList<RuleSet>` — The rule sets that applied, in the order they were set.
- `GlideBoost { get; init; } -> GlideBoostRule?` — The glider boost in force (`PlayerRules.GlideBoost`); null when no rule set asks for one.
- `GlideSink { get; init; } -> double` — The multiplier on how fast the glider sinks (`PlayerRules.GlideSink`).
- `GlideSteer { get; init; } -> GlideSteerRule?` — The glider steering in force (`PlayerRules.GlideSteer`); null when no rule set asks for it.
- `IgnoreBasePermissions { get; init; } -> bool` — The player may build in other players' bases without their permission (`PlayerRules.IgnoreBasePermissions`).
- `IgnoreEnemiesNearby { get; init; } -> bool` — The player may build with enemies nearby (`PlayerRules.IgnoreEnemiesNearby`).
- `IgnoreNoBuildZones { get; init; } -> bool` — The player may build inside the game's no-build zones (`PlayerRules.IgnoreNoBuildZones`).
- `Invulnerable { get; init; } -> bool` — The player takes no damage from hits (`PlayerRules.Invulnerable`).
- `IsDefault { get; } -> bool` — True when nothing differs from the game's own rules.
- `KeepBuildsOutsideBases { get; init; } -> bool` — What the player builds outside bases is kept in the save (`PlayerRules.KeepBuildsOutsideBases`).
- `KeepConsumables { get; init; } -> bool` — Used consumables are not removed (`PlayerRules.KeepConsumables`).
- `MinHealth { get; init; } -> double` — The fraction of maximum health the player is kept at least at, 0 to 1 (`PlayerRules.MinHealth`).
- `MinMana { get; init; } -> double` — The fraction of maximum mana the player is kept at least at, 0 to 1 (`PlayerRules.MinMana`).
- `MinShroudTime { get; init; } -> double` — The fraction of its maximum the Shroud timer never drops below, 0 to 1 (`PlayerRules.MinShroudTime`).
- `MinStamina { get; init; } -> double` — The fraction of maximum stamina the player is kept at least at, 0 to 1 (`PlayerRules.MinStamina`).
- `NoClip { get; init; } -> bool` `0.2.2` — The character flies without collision (`PlayerRules.NoClip`).
- `NoFallDamage { get; init; } -> bool` — The player takes no damage from falls (`PlayerRules.NoFallDamage`).
- `static None(EntityId player) -> EffectiveRules` — Rules that change nothing for `player`: the game's own rules.
- `Player { get; init; } -> EntityId` — The player entity these rules are for.
- `SameAs(EffectiveRules o) -> bool` — The same values, ignoring `EffectiveRules.From`.
- `UnlimitedGliding { get; init; } -> bool` — The glider never forces the player down for a gameplay reason (`PlayerRules.UnlimitedGliding`).
- `Water { get; init; } -> WaterRule?` — The water refill in force (`PlayerRules.Water`); null when no rule set asks for one.

## GlideBoostRule — Gliding like a plane (`PlayerRules.GlideBoost`): the glider's path takes its pitch from where the player looks (a view within about 5 degrees of level flies level)...
- `Climb { get; init; } -> double` — The fastest the glider climbs while the player looks up, m/s (default 15; 0 = it never climbs: looking up then only...
- `Coast { get; init; } -> double` — After letting go of sprint the glider slows down at most this fast, m/s2 (default 8: about 2-3 s back to a plain...
- `static Default { get; } -> GlideBoostRule` — The default boost: every value at its default.
- `Lift { get; init; } -> double` — Upward acceleration while boosting, m/s2 (default 0: the boost goes where the glider points; a "jetpack" might use...
- `MaxSpeed { get; init; } -> double` — The boost accelerates up to this speed, m/s (default 35).
- `const SpeedLimit = 120 -> double` — The fastest any rule may make a boosted glider, m/s.
- `Thrust { get; init; } -> double` — Acceleration along the glider's heading (it turns toward where the player looks) while sprint is held, m/s2.

## GlideSteerRule — Steering the glider's own nose (`PlayerRules.GlideSteer`): S raises it, a centred stick holds the pitch, W dives as in the game.
- `const AngleLimit = 85 -> double` — The highest `GlideSteerRule.MaxClimbAngle`, degrees: never straight up, where the game's pitch measure folds over.
- `static Default { get; } -> GlideSteerRule` — The default steering: `GlideSteerRule.MaxClimbAngle` at its default.
- `MaxClimbAngle { get; init; } -> double` — How far above level the nose may rise, degrees (0..85; default 85; 0 = up to level flight).

## NoClip `0.2.2` — NoClip: a player's character flies through terrain, blocks and props (`PlayerRules.NoClip`).
- `static Available { get; } -> bool` — True when this build can do it (the capability `NoClip.Capability`).
- `const Capability = "rules.noclip" -> string` — The capability: this game build has the collision hook and character flight's hooks.
- `static Of(EntityId player) -> NoClipState` — How NoClip stands for `player` now.
- `const PermissionNode = "kiln.command.noclip" -> string` — The command's permission node (level Admin; a hosted world's host and the console have it).
- `static Set(EntityId player, bool on) -> NoClipResult` — Turns Kiln's NoClip switch for `player` on or off.

## NoClipEnd `0.2.2` — Why NoClip ended for a player (`PlayerNoClipEndedEvent`).
enum: RuleEnded, Death, Teleport, Left `0.2.3`

## NoClipResult `0.2.2` — What `NoClip.Set` did.
- `NoClipResult(bool Ok, bool On, string? Reason, string Message)`
- `Message { get; init; } -> string` — One line for the one who asked.
- `Ok { get; init; } -> bool` — False when turning it on was refused.
- `On { get; init; } -> bool` — Whether the player's rules carry NoClip now.
- `Reason { get; init; } -> string?` — Null when `Ok`; else `policy` (the server's rules.json), `client` (the player's game) or `denied` (another rule set...

## NoClipState `0.2.2` — NoClip for one player (`NoClip.Of`).
- `NoClipState(EntityId Player, bool On, bool Active, ulong StepsSkipped, WorldPosition? LastFree, bool Restoring)`
- `Active { get; init; } -> bool` — The character is moving without collision right now: its last collision step was skipped.
- `LastFree { get; init; } -> WorldPosition?` — Where the character last was in open space while NoClip was on (where it started, at first).
- `On { get; init; } -> bool` — The player's rules carry NoClip (`EffectiveRules.NoClip`).
- `Player { get; init; } -> EntityId` — The player.
- `Restoring { get; init; } -> bool` — NoClip was turned off inside something: the rules no longer carry it (`On` is false), but the character is being...
- `StepsSkipped { get; init; } -> ulong` — Collision steps skipped for the character since the world started.

## PlayerNoClipEndedEvent `0.2.2` — NoClip ended for a player and collision is back.
- `Player { get; } -> EntityId` — The player.
- `Reason { get; } -> NoClipEnd` — Why it ended.
- `RestoredTo { get; } -> WorldPosition?` — Where Kiln put the character before collision returned, when it was inside something; else null.

## PlayerRules — What a rule set asks of the game for the players it applies to.
- `BuildCost { get; init; } -> double?` — x what placing blocks, terrain and props takes from the inventory (0 = free; free also lifts the cursor's "missing...
- `BuildInShroud { get; init; } -> bool?` — Build inside the Shroud.
- `BuildOutsideBases { get; init; } -> bool?` — Build outside bases (the game's "outside a building zone").
- `BuildRefund { get; init; } -> double?` — x what dismantling and undo give back.
- `CharacterFlight { get; init; } -> CharacterFlightMode?` — The character itself flies (not the building spark): airborne, it takes off; jump climbs, sneak sinks, sprint is...
- `static Creative { get; } -> PlayerRules` — Kiln's built-in "creative" preset: free flight and building anywhere outside other players' bases, nothing costs or...
- `static CreativeExtras { get; } -> PlayerRules` — What `/creative` requests in addition to the preset where it says nothing.
- `DigDamage { get; init; } -> double?` — x the damage the player's tools deal to terrain and building blocks (1000 breaks anything breakable in one hit...
- `FlightCarriesBody { get; init; } -> bool?` — The character goes where the player flew: when the flight ends (the fly key, a hit, or the rule ending outside a...
- `FlyAnywhere { get; init; } -> bool?` — The game's building fly mode outside bases too (the game allows it only inside a base, for players with the building...
- `FreeCrafting { get; init; } -> bool?` — Requests crafting without input costs.
- `FreezeShroudTimer { get; init; } -> bool?` — The Shroud timer does not go down while the player is in the Shroud, and the Shroud cannot kill them (the game kills...
- `static FromJson(string json) -> PlayerRules` — Reads rules from JSON (camelCase members, comments allowed).
- `GlideBoost { get; init; } -> GlideBoostRule?` — The glider flies like a plane: it climbs and dives where the player looks (climbing costs speed, diving gives it)...
- `GlideSink { get; init; } -> double?` — x how fast the glider sinks (1 = the game's; 0 = holds its altitude).
- `GlideSteer { get; init; } -> GlideSteerRule?` — The glider's own nose follows the movement keys: holding S raises it (up to `GlideSteerRule.MaxClimbAngle` above...
- `IgnoreBasePermissions { get; init; } -> bool?` — Build in other players' bases without their permission.
- `IgnoreEnemiesNearby { get; init; } -> bool?` — Build with enemies nearby.
- `IgnoreNoBuildZones { get; init; } -> bool?` — Build inside the game's no-build zones (around points of interest, dungeons, altars).
- `Invulnerable { get; init; } -> bool?` — No damage from hits (Kiln.Combat's invulnerability).
- `KeepBuildsOutsideBases { get; init; } -> bool?` — Keep what the player builds outside bases in the save (the game saves terrain only inside bases): Kiln adds those...
- `KeepConsumables { get; init; } -> bool?` — Used consumables are not removed: fired arrows, thrown items and staff ammunition, food, drinks, potions and...
- `MinHealth { get; init; } -> double?` — Health is kept at least at this fraction of its max (1 = always full).
- `MinMana { get; init; } -> double?` — Mana is kept at least at this fraction of its max.
- `MinShroudTime { get; init; } -> double?` — The Shroud timer never drops below this fraction of its max (1 = always full): below it, it recovers at the game's...
- `MinStamina { get; init; } -> double?` — Stamina is kept at least at this fraction of its max (1 = never runs out).
- `NoClip { get; init; } -> bool?` `0.2.2` — The character moves through terrain, blocks and props: it flies as with `PlayerRules.CharacterFlight` (at once, from...
- `NoFallDamage { get; init; } -> bool?` — No damage from falls, from any height, and no hard-landing stagger (Kiln.Combat's fall event, canceled on the server).
- `Problems() -> IReadOnlyList<string>` — Out-of-range values, by member name; empty when the rules are fine.
- `ToJson() -> string` — These rules as indented JSON with camelCase members; members that are not set are left out.
- `UnlimitedGliding { get; init; } -> bool?` — The glider never forces the player down for a gameplay reason: gliding costs no stamina and does not end when...
- `Water { get; init; } -> WaterRule?` — The player's water containers get water back (Kiln.Items' refill policies).
- `With(PlayerRules other) -> PlayerRules` — These rules with every member `other` sets taking its place.

## PlayerRulesChangedEvent — A player's rules changed (a rule set applied, updated, ended, or an area entered or left).
- `After { get; } -> EffectiveRules` — The player's rules after the change.
- `Before { get; } -> EffectiveRules` — The player's rules before the change.
- `Player { get; } -> EntityId` — The player entity whose rules changed.

## Rules — Per-player rules: what the game lets a player do (fly, build where, at what cost, how fast they mine) and what they never run out of (health, stamina, mana, water).
- `static Active { get; } -> IReadOnlyList<RuleSet>` — Every active rule set, in the order they were applied.
- `static Apply(EntityId player, PlayerRules rules, TimeSpan? duration = null, string? label = null) -> RuleSet` — Applies `rules` to one player (`RuleScope.ForPlayer`).
- `static Apply(RuleScope scope, PlayerRules rules, TimeSpan? duration = null, string? label = null) -> RuleSet` — Applies `rules` to `scope` until the returned handle is disposed, the calling mod unloads, `duration` runs out, or...
- `static Available { get; } -> bool` — True when the native kiln.rules service is loaded (its hooks may still be off: see `Rules.Status`).
- `const ChannelName = "kiln:rules" -> string` — The network channel a player's game applies its rules from (flight and the build cursor).
- `static ClientOf(EntityId player) -> RulesClient` — Whether a player's game applies the client-side half of the rules (flight, the build cursor).
- `static DefinePreset(string name, PlayerRules rules) -> IDisposable` — Defines a preset for as long as the calling mod is loaded (a server file of the same name wins).
- `static Of(EntityId player) -> EffectiveRules` — A player's rules as they stand now (every rule set that applies, combined, the server's limits held).
- `static Policy { get; } -> RulesPolicy` — The server's limits on everyone's rules (`<configDir>/rules.json`; edits apply within a second).
- `static Preset(string name) -> PlayerRules?` — A named set of rules: `<configDir>/rules/<name>.json` on this server first, then one a mod defined...
- `static Presets { get; } -> IReadOnlyList<string>` — Names of every preset: the server's files, the mods', Kiln's.
- `static Status() -> RulesStatus?` — The native hooks' state and counters (null when kiln.rules is not loaded).

## RulesClient — Whether a player's game applies the client-side half of the rules.
enum: Unknown, Host, Kiln, KilnWithoutRules, Vanilla, Pending, Simulated

## RuleScope — Who a rule set applies to: one player, everyone, or the players inside an area (optionally only one of them).
- `static Box(WorldPosition min, WorldPosition max) -> RuleScope` — The players inside the box between two corners (given in any order).
- `Covers(EntityId player, WorldPosition? at) -> bool` — Whether the scope covers `player` at `at` (an area needs a position).
- `static Cylinder(WorldPosition center, double radius, double minY, double maxY) -> RuleScope` — An upright cylinder: `radius` around `center` (x, z), from `minY` to `maxY`.
- `static Everyone { get; } -> RuleScope` — Every player in the world.
- `static ForPlayer(EntityId player) -> RuleScope` — Only the player with this entity id.
- `IsArea { get; } -> bool` — True for an area scope: a sphere, a box or a cylinder.
- `Only(EntityId player) -> RuleScope` — This area, for one player only.
- `Player { get; } -> EntityId?` — Only this player entity (a player's entity id goes to the next player who joins after they leave: a player rule set...
- `static Sphere(WorldPosition center, double radius) -> RuleScope` — The players within `radius` metres of `center`.

## RuleSet — One mod's rules for a scope.
- `Active { get; } -> bool` — True until the set ends: it was disposed, it expired, or it was removed with its mod or its player.
- `Dispose() -> void` — Takes the rules away.
- `Expires { get; } -> DateTime?` — When it ends by itself (UTC), or null.
- `Id { get; } -> long` — A number that identifies this rule set, unique in the process.
- `Label { get; } -> string` — A name for the set, for logs and listings; empty when none was given.
- `Owner { get; } -> string` — Id of the mod that applied it ("kiln" outside mod code).
- `Rules { get; } -> PlayerRules` — The rules this set asks for.
- `Scope { get; } -> RuleScope` — Who the rules apply to.
- `Update(PlayerRules rules) -> void` — Changes the rules (same scope, owner and end).

## RulesPolicy — The server's limits on everyone's rules: `<configDir>/rules.json`.
- `AllowBuildInShroud { get; set; } -> bool` — Players may build inside the Shroud (`PlayerRules.BuildInShroud`).
- `AllowBuildOutsideBases { get; set; } -> bool` — Players may build outside bases (`PlayerRules.BuildOutsideBases`).
- `AllowCharacterFlight { get; set; } -> bool` — The character itself may fly for a player (`PlayerRules.CharacterFlight`).
- `AllowFlyAnywhere { get; set; } -> bool` — Players may get the building fly mode outside bases (`PlayerRules.FlyAnywhere`).
- `AllowFreeCrafting { get; set; } -> bool` — Allows requesting `PlayerRules.FreeCrafting`.
- `AllowFreezeShroudTimer { get; set; } -> bool` — The Shroud timer may be frozen for a player (`PlayerRules.FreezeShroudTimer`).
- `AllowGlideBoost { get; set; } -> bool` — Holding sprint may push a glider along the view (`PlayerRules.GlideBoost`).
- `AllowGlideSteer { get; set; } -> bool` — The glider's own nose may be steered for a player (`PlayerRules.GlideSteer`).
- `AllowIgnoreBasePermissions { get; set; } -> bool` — Players may build in other players' bases without their permission (`PlayerRules.IgnoreBasePermissions`).
- `AllowIgnoreEnemiesNearby { get; set; } -> bool` — Players may build with enemies nearby (`PlayerRules.IgnoreEnemiesNearby`).
- `AllowIgnoreNoBuildZones { get; set; } -> bool` — Players may build in the game's no-build zones around points of interest, dungeons and altars...
- `AllowInvulnerable { get; set; } -> bool` — Players may be made invulnerable (`PlayerRules.Invulnerable`).
- `AllowKeepConsumables { get; set; } -> bool` — Allows requesting `PlayerRules.KeepConsumables`: arrows, ammunition, food and potions are not used up.
- `AllowNoClip { get; set; } -> bool` `0.2.2` — A player may move through terrain, blocks and props (`PlayerRules.NoClip`, also `/noclip`).
- `AllowNoFallDamage { get; set; } -> bool` — Falls may do no damage for a player (`PlayerRules.NoFallDamage`).
- `AllowUnlimitedGliding { get; set; } -> bool` — Gliding may cost no stamina for a player (`PlayerRules.UnlimitedGliding`).
- `AllowWater { get; set; } -> bool` — Water containers may get water back (`PlayerRules.Water`).
- `MaxDigDamage { get; set; } -> double` — The highest dig damage multiplier any rule may give, 1 to 1000000 (1000 breaks anything breakable in one hit).
- `MaxGlideSpeed { get; set; } -> double` — The fastest a boosted glider may go, m/s, 0 to 120.
- `MaxGlideSteerAngle { get; set; } -> double` — How far above level a steered glider's nose may rise, degrees, 0 to 85 (85 is almost straight up, 0 is level at most).
- `MaxHealthFloor { get; set; } -> double` — The highest health floor any rule may keep, 0 to 1 (1 is full).
- `MaxManaFloor { get; set; } -> double` — The highest mana floor any rule may keep, 0 to 1 (1 is full).
- `MaxShroudFloor { get; set; } -> double` — The highest Shroud timer floor any rule may keep, 0 to 1 (1 is full).
- `MaxStaminaFloor { get; set; } -> double` — The highest stamina floor any rule may keep, 0 to 1 (1 is full).
- `MinBuildCost { get; set; } -> double` — The lowest building cost multiplier any rule may give, 0 to 1000 (0 allows free building).

## RulesStatus — The state of kiln.rules' hooks and counters (diagnostics).
- `RulesStatus(bool Flight, bool Placement, bool Costs, bool Mining, int Players, ulong FlyAllowed, ulong CursorCleared, ulong CostsScaled, ulong RefundsScaled, ulong DigsScaled, ulong Faults, string Detail)`
- `BuildingZone { get; init; } -> bool` — The server cursor's building-zone hook (BuildOutsideBases: the hammer's "Needs a Flame Altar nearby").
- `CharacterFlight { get; init; } -> bool` — Character flight's hooks (CharacterFlight): the locomotion state, gravity and the flying mover.
- `ConsumablesKept { get; init; } -> ulong` — Consume removals skipped so far.
- `Costs { get; init; } -> bool` — The material cost hooks work (BuildCost, BuildRefund).
- `CostsScaled { get; init; } -> ulong` — Material lists scaled when placing.
- `CraftButtons { get; init; } -> bool` — Game client: the crafting menu's craftable amount (craft buttons stay enabled with FreeCrafting).
- `CraftsFree { get; init; } -> ulong` — Recipe payments answered "paid" so far.
- `CursorCleared { get; init; } -> ulong` — Cursor evaluations where a refusal flag was cleared.
- `Detail { get; init; } -> string` — What is switched off and why, as text; empty when nothing is.
- `DigsScaled { get; init; } -> ulong` — Damage values scaled.
- `Faults { get; init; } -> ulong` — Hook bodies that faulted; the game's own path ran instead.
- `Flight { get; init; } -> bool` — The fly-gate hook works (FlyAnywhere).
- `FlyAllowed { get; init; } -> ulong` — Fly-gate decisions turned to allowed.
- `FreeCrafting { get; init; } -> bool` — The recipe-payment hooks (FreeCrafting): crafts pay nothing inside the game's own transaction.
- `GlideChanged { get; init; } -> ulong` — Glider steps changed (sink, stamina, boost).
- `GlideSteer { get; init; } -> bool` — The glider's pitch step (GlideSteer; actor_rotation).
- `GlideSteered { get; init; } -> ulong` — Glider pitch steps steered so far.
- `Gliding { get; init; } -> bool` — The glider's hooks (UnlimitedGliding, GlideSink, GlideBoost).
- `KeptConsumables { get; init; } -> bool` — The two consume removals (KeepConsumables: fired arrows, thrown items, staff ammunition).
- `KeptFoodAndPotions { get; init; } -> bool` — The buff item's consume removal (KeepConsumables: food, drinks, potions, bandages).
- `MapReveal { get; init; } -> bool` — Game client: the fog of war's discover call (the creative menu's map reveal).
- `Mining { get; init; } -> bool` — The dig damage hook works (DigDamage).
- `NoClip { get; init; } -> bool` `0.2.2` — NoClip's hook on the character's collision step (NoClip; only ever on with `RulesStatus.CharacterFlight`).
- `Placement { get; init; } -> bool` — The build cursor's hook works (the placement rules).
- `Players { get; init; } -> int` — Server entities that have rules set.
- `RefundsScaled { get; init; } -> ulong` — Material lists scaled when dismantling and undoing.
- `ShroudHeld { get; init; } -> ulong` — Shroud timer steps held or recovered instead of draining.
- `ShroudTimer { get; init; } -> bool` — The Shroud timer's hook (FreezeShroudTimer, MinShroudTime).
- `ZoneForced { get; init; } -> ulong` — Building-zone tests turned to "near a base".

## WaterRule — Water a player's containers get back (Kiln.Items' refill policies, on the player's inventories).
- `ChargesPerSecond { get; init; } -> double` — Charges per second of game time; `Infinity` (the default) keeps the containers full.
- `Items { get; init; } -> IReadOnlyList<string>?` — The items refilled: debug names (`Tool_T1_WateringCan`, a mod's `<mod>.<block>`) or ids (`0x8ec15337`).
- `static KeepFull(params string[] items) -> WaterRule` — A rule that keeps the containers full.
- `KeepsFull { get; } -> bool` — True when the water never runs out: `WaterRule.ChargesPerSecond` is infinite.
- `static Rate(double chargesPerSecond, params string[] items) -> WaterRule` — A rule that gives water back at a fixed rate.
