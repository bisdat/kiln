# Players (Kiln.Players)

## BanEntry `0.2.2` — One entry of Kiln's ban list.
- `BanEntry(ulong SteamId, ulong AccountId, string Name, string Reason, string By, DateTimeOffset Since, DateTimeOffset? Until)`
- `AccountId { get; init; } -> ulong` — The game's account id (the player record's, what `Player.AccountId` says); 0 when unknown.
- `By { get; init; } -> string` — Who banned: a mod id, a player's name, "console".
- `IsActive(DateTimeOffset now) -> bool` — True while the ban holds at `now`.
- `Matches(ulong steamId, ulong accountId) -> bool` — True when the ban names this Steam id or this account id (0 matches nothing).
- `Name { get; init; } -> string` — The player's name when banned (for the list only; names do not identify).
- `Reason { get; init; } -> string` — What the player is told.
- `Since { get; init; } -> DateTimeOffset` — When.
- `SteamId { get; init; } -> ulong` — SteamID64; 0 when the ban names only an account id.
- `Until { get; init; } -> DateTimeOffset?` — When it ends by itself; null: until unbanned.

## Bans `0.2.2` — Kiln's ban list: players refused when they connect, by Steam id or game account id.
- `static Add(PlayerRef player, string reason, TimeSpan? duration = null) -> BanEntry` — Bans a player (online or not).
- `static Add(ulong steamId, string reason, TimeSpan? duration = null) -> BanEntry` — Bans a Steam id (a player who is not here).
- `static All { get; } -> IReadOnlyList<BanEntry>` — The entries that hold now (ended ones are dropped).
- `static Find(ulong steamOrAccountId) -> BanEntry?` — The entry that holds now for this Steam id or account id, or null.
- `static Remove(ulong steamOrAccountId) -> bool` — Removes every entry that names this Steam id or account id.

## ExperienceSource — Where experience came from (the game's ExperienceSourceType; the client's "+XP" line may show it).
enum: None, Exploration, Lore, Quest, Combat, Mining, ItemSetCompleted

## KickResult `0.2.2` — The answer to `Players.Kick`: whether the player's connection was refused.
- `Name { get; } -> string` — The player's name when known; "" otherwise.
- `Ok { get; } -> bool` — True when the player was refused: the game's login failure went out, or the player's game left on Kiln's refusal...
- `Player { get; } -> PlayerRef` — The player that was kicked (or was to be).
- `Reason { get; } -> string?` — Why not; null when `KickResult.Ok`.

## Player — A player as kiln.players saw them at the last server-world step.
- `AccountId { get; init; } -> ulong` — The game's account id for the player; 0 when unknown.
- `Aim { get; } -> PlayerAim?` `0.2.2` — Where the player's camera is and where it looks (`PlayerStates.AimOf`); null before the player's client sent a camera.
- `Buffs { get; } -> IReadOnlyList<PlayerBuff>` — The buffs on the player.
- `Can(PlayerPermissions p) -> bool` — True when the player's user group has every right in `p` (see `Player.PermissionsKnown`).
- `CharacterId { get; } -> Guid` — The game's id of the character this player plays (`keen::CharacterId`: a GUID in the header of `character<N>.ksd`...
- `Check(PlayerStateId state) -> bool?` — True or false, or null when this state cannot be read here.
- `CurrentAction { get; } -> PlayerAction?` — The action the player runs now: null when none.
- `Entity { get; init; } -> EntityId` — The player's entity (1..16 on this build: slot + 1).
- `Equipment { get; } -> PlayerEquipment?` — What the player wears and holds.
- `static explicit operator PlayerRef(Player player) -> PlayerRef` — This player as a `PlayerRef`: the Steam id, the character id and, while the player is in game, the session entity...
- `Handle { get; init; } -> uint` — The game's connection handle (slot | generation \<\< 6); 0 when unknown.
- `Health { get; } -> int?` — The player's health; null when it cannot be read (`Player.Snapshot`).
- `HealthMax { get; } -> int?` — The player's maximum health; null when it cannot be read.
- `HeldItem { get; } -> PlayerItem` — The item in hand.
- `InGame { get; } -> bool` — The player is logged in: `Player.State` is `PlayerState.InGame`.
- `Is(PlayerStateId state) -> bool` — True when the player is in this state; false when not, or when it cannot be read (see `Player.Check`).
- `IsAdmin { get; init; } -> bool` — The game's own admin bit (it gates the admin client's actions).
- `IsAdminKnown { get; init; } -> bool` — True when `Player.IsAdmin` was read from the game.
- `IsAirborne { get; } -> bool` — The player is in the air: not on the ground, and not climbing, swimming or hanging from a hook...
- `IsAttacking { get; } -> bool` — The player runs an action that swings or fires a weapon (`PlayerStateId.Attacking`).
- `IsBlocking { get; } -> bool` — The player is holding a block (`PlayerStateId.Blocking`).
- `IsCasting { get; } -> bool` — The player runs an action that uses a spell, or a wand's own attack (`PlayerStateId.Casting`).
- `IsClimbing { get; } -> bool` — The player is climbing (`PlayerStateId.Climbing`).
- `IsCrouching { get; } -> bool` — The player is crouching, in the game's sneak mode (`PlayerStateId.Sneaking`).
- `IsDead { get; } -> bool` — The player is dead (`PlayerStateId.Dead`).
- `IsDiving { get; } -> bool` — The player is diving (`PlayerStateId.Diving`).
- `IsDodging { get; } -> bool` — The player is in a dodge (`PlayerStateId.Dodging`).
- `IsFalling { get; } -> bool` — The player is falling (`PlayerStateId.LocomotionFalling`).
- `IsGliding { get; } -> bool` — The player is gliding (`PlayerStateId.Gliding`).
- `IsGrappling { get; } -> bool` — The player hangs from a grappling hook (`PlayerStateId.Grappling`).
- `IsGrounded { get; } -> bool` — The player is on the ground (`PlayerStateId.Grounded`).
- `IsInBase { get; } -> bool` — The player is inside a flame altar's build zone (`PlayerStateId.InBase`).
- `IsInCombat { get; } -> bool` — The player is in combat (`PlayerStateId.InCombat`).
- `IsInShroud { get; } -> bool` — The player stands in the Shroud (`PlayerStateId.InShroud`).
- `IsJumping { get; } -> bool` — The player is rising from a jump (`PlayerStateId.Jumping`).
- `IsSitting { get; } -> bool` — The player is sitting on a seat (`PlayerStateId.Sitting`).
- `IsSleeping { get; } -> bool` — The player is sleeping (`PlayerStateId.Sleeping`).
- `IsSprinting { get; } -> bool` — The player is sprinting (`PlayerStateId.Sprinting`).
- `IsSwimming { get; } -> bool` — The player is swimming (`PlayerStateId.Swimming`).
- `IsUnderwater { get; } -> bool` — The player is underwater (`PlayerStateId.Underwater`).
- `IsUsingItem { get; } -> bool` — The player runs an action with a used item that is not a weapon (`PlayerStateId.UsingItem`).
- `JoinedStep { get; init; } -> ulong` — Server-world step when Kiln saw the join; 0 if they were in game before Kiln watched.
- `Mana { get; } -> int?` — The player's mana; null when it cannot be read.
- `ManaMax { get; } -> int?` — The player's maximum mana; null when it cannot be read.
- `Name { get; init; } -> string` — The platform name (what the chat and the game log show).
- `Permissions { get; init; } -> PlayerPermissions` — The user group's rights; `PlayerPermissions.None` when unknown (see `Player.PermissionsKnown`).
- `PermissionsKnown { get; init; } -> bool` — True when `Player.Permissions` was read from the game's player record.
- `Position { get; init; } -> WorldPosition?` — Where the player stands (metres), as of the last step; null when unknown.
- `Progression { get; } -> PlayerProgression` — Level, experience, skill tree and skill points of this player (kiln.progression).
- `ShroudTimeLeft { get; } -> TimeSpan?` — The Shroud timer the game shows.
- `Slot { get; init; } -> int` — The game's player slot (0..15).
- `Snapshot() -> PlayerSnapshot?` — Everything at once, one read: states, action, health and the rest.
- `Stamina { get; } -> int?` — The player's stamina; null when it cannot be read.
- `StaminaMax { get; } -> int?` — The player's maximum stamina; null when it cannot be read.
- `State { get; init; } -> PlayerState` — Whether the player is joining, in game or leaving.
- `SteamId { get; init; } -> ulong` — SteamID64, from the game log; 0 when unknown.
- `UserGroupId { get; init; } -> uint` — The game's user-group id (raw); 0 when unknown.

## PlayerAction — The action a player runs: the game's action sequence and what started it.
- `PlayerAction(uint Sequence, string SequenceName, PlayerStateId Trigger, PlayerStateId Input, PlayerItem Item, PlayerItem Ammunition, PlayerItem CostItem, ulong Slot, EntityId Host, long TriggerTime, TimeSpan Played, TimeSpan Length)`
- `Ammunition { get; init; } -> PlayerItem` — The ammunition item it uses; none when it uses none.
- `CostItem { get; init; } -> PlayerItem` — The item the action costs; none when it costs none.
- `Host { get; init; } -> EntityId` — The entity of the interaction host (the thing the player interacts with); `EntityId.None` when there is none.
- `Input { get; init; } -> PlayerStateId` — The input behind it, when one: a state such as `PlayerStateId.InputQuickUse0`; `PlayerStateId.None` when none.
- `Item { get; init; } -> PlayerItem` — The item the action uses; `PlayerItem.IsNone` when none.
- `Length { get; init; } -> TimeSpan` — How long the sequence lasts.
- `Played { get; init; } -> TimeSpan` — How far the sequence has played.
- `Sequence { get; init; } -> uint` — The game's id of the action sequence: FNV-1a of the sequence GUID.
- `SequenceName { get; init; } -> string` — The game's name of the sequence ("UseItem_Food_Buff"); empty when it cannot be read.
- `Slot { get; init; } -> ulong` — The inventory slot the item comes from: the inventory entity in the low 32 bits, the slot index above them.
- `SlotIndex { get; } -> int` — The slot's index in that inventory.
- `SlotInventory { get; } -> EntityId` — The slot's inventory entity (0 = no slot).
- `Trigger { get; init; } -> PlayerStateId` — What started it: a state such as `PlayerStateId.ActionPrimary`.
- `TriggerTime { get; init; } -> long` — The game's time when the action started, in nanoseconds.

## PlayerActionEndedEvent — A player's action ended: it finished, was replaced or was canceled.
- `Action { get; } -> PlayerAction` — The action that ended.
- `Canceled { get; } -> bool` — A handler of `PlayerActionEvent` canceled it.
- `Duration { get; } -> TimeSpan` — Wall time it was seen running.
- `Player { get; } -> Player` — The player whose action ended.

## PlayerActionEvent — A player started an action: anything the game runs as an action sequence.
- `Action { get; } -> PlayerAction` — The action: its sequence, trigger, item and timing.
- `CanCancel { get; } -> bool` — False where a cancel has no effect: a joined client, or a build where the game's reset did not resolve.
- `Input { get; } -> PlayerStateId` — The input behind it, when one: `PlayerStateId.InputQuickUse0`, ...
- `Item { get; } -> PlayerItem` — The item it uses; `PlayerItem.IsNone` when none.
- `Player { get; } -> Player` — The player who started the action.
- `SequenceName { get; } -> string` — The game's name of what plays: "UseItem_Food_Buff", "Movement_Tool_HangGlider_Start".
- `Step { get; } -> ulong` — The world step the action was seen starting in.
- `Trigger { get; } -> PlayerStateId` — What started it: `PlayerStateId.ActionPrimary`, `PlayerStateId.ActionEvade`, `PlayerStateId.ActionGlider`, ...

## PlayerAim `0.2.2` — Where a player's camera is and where it looks, as the player's client last sent it.
- `PlayerAim(WorldPosition Origin, Vector3 Direction, Quaternion Rotation, float Distance, float FieldOfView, PlayerStateOrigin Source)`
- `At(double metres) -> WorldPosition` — The point `metres` along the view ray from the camera.
- `Direction { get; init; } -> Vector3` — Where the camera looks: a unit vector, `PlayerAim.Rotation` applied to (0, 0, 1), the game's forward axis.
- `Distance { get; init; } -> float` — The camera's distance setting (the third-person boom), metres.
- `FieldOfView { get; init; } -> float` — The vertical field of view, radians.
- `Origin { get; init; } -> WorldPosition` — The camera's position.
- `PitchDegrees { get; } -> float` — The look direction's pitch in degrees: positive up.
- `Rotation { get; init; } -> Quaternion` — The camera's orientation.
- `Source { get; init; } -> PlayerStateOrigin` — Server: read in the server world (any player); ClientPrediction: a joined client's own player.
- `YawDegrees { get; } -> float` — The look direction's heading about +Y in degrees, 0..360 (0 = +Z, 90 = +X), counted as `EntityTransform.YawDegrees`...

## PlayerBannedEvent `0.2.2` — An entry was added to Kiln's ban list (`Bans.Add`).
- `Ban { get; } -> BanEntry` — The new entry.

## PlayerBuff — A buff on a player, or one an item gives.
- `PlayerBuff(uint Id, string Slot, TimeSpan? Remaining = null, int Level = 0)`
- `Id { get; init; } -> uint` — The game's BuffTypeId (`GameBuffs.Ids.X`).
- `Is(string idText) -> bool` — True for this id, given as a number or as a `GameBuffs.X` constant ("0x..").
- `Is(uint id) -> bool` — True for this id.
- `Level { get; init; } -> int` — Its level, when known.
- `Remaining { get; init; } -> TimeSpan?` — Time left, when known (only on a buff that is on a player).
- `Slot { get; init; } -> string` — The game's BuffSlotType name: "Food", "Buff", "Debuff", "Rested", "TerrainDebuff".

## PlayerCharacters — kiln.characters: the game's CharacterId of each connected player.
- `static AccountOf(Player p) -> ulong` — The account key the game files the character under (record +0x48, the world's `known_users` key).
- `static Available { get; } -> bool` — True when the CharacterId can be read on this build.
- `static IdOf(Player p) -> Guid` — The player's character id; `Guid.Empty` when unknown.
- `static Status { get; } -> PlayerCharactersStatus?` — Null when kiln.characters is not loaded.

## PlayerCharactersStatus — What kiln.characters can do on this build.
- `PlayerCharactersStatus(bool Resolved, bool Records, bool Simulated, ulong Scans, int Known, int RecordOffset)`
- `Known { get; init; } -> int` — How many slots have a character id in the snapshot.
- `RecordOffset { get; init; } -> int` — The record offset of the character id.
- `Records { get; init; } -> bool` — kiln.players gave the player records at the last scan.
- `Resolved { get; init; } -> bool` — The record offset of the character id was found on this build.
- `Scans { get; init; } -> ulong` — How many scans were done.
- `Simulated { get; init; } -> bool` — The lab simulation is on (`KILN_CHARACTERS_SIMULATE`).

## PlayerClientActionEvent — A client sent a versioned action: anything the game's menus and input send as a command, named by the game's own field.
- `Action { get; } -> PlayerClientActionInfo` — Which action it is: its number, the game's field name and the size of its struct.
- `CanCancel { get; } -> bool` — True: Kiln holds the action and the game has not run it; a cancel stops it.
- `DecisionId { get; } -> DecisionId` `0.2.4` — The decision this held action is part of when a typed equipment handler decides it as well (`PlayerEquippingEvent`...
- `Name { get; } -> string` — The game's field name: "changeEquipmentAction".
- `Payload { get; } -> byte[]` — The action's struct as the client sent it (layout: the game's reflection of the field's type).
- `Player { get; } -> Player` — The player whose client sent the action.
- `Version { get; } -> uint` — The action's version number as the client sent it.

## PlayerClientActionInfo — An action a client can send.
- `PlayerClientActionInfo(int Index, string Name, int Bytes, bool CanCancel)`
- `Bytes { get; init; } -> int` — The size of its struct.
- `CanCancel { get; init; } -> bool` — A `PlayerClientActionEvent` for it can be canceled on this build: Kiln holds it for the handlers before the game's...
- `Index { get; init; } -> int` — Its number on this build.
- `Name { get; init; } -> string` — The game's field name: "changeEquipmentAction".

## PlayerConsumedEvent — A consumable was really used: the stack in its slot went down during the use.
- `Buffs { get; } -> IReadOnlyList<PlayerBuff>` — Buffs the item gives.
- `Item { get; } -> PlayerItem` — The item that was used.
- `Kind { get; } -> PlayerConsumeKind` — What the consumable is, by the game's sequence for it.
- `Left { get; } -> int` — What is left in the slot.
- `Player { get; } -> Player` — The player who used the consumable.

## PlayerConsumeEvent — A player started eating, drinking or using another consumable.
- `Buffs { get; } -> IReadOnlyList<PlayerBuff>` — Buffs the item gives (its applied buff).
- `IsDrink { get; } -> bool` — The consumable is a drink (`PlayerConsumeEvent.Kind` is `PlayerConsumeKind.Drink`).
- `IsFood { get; } -> bool` — The consumable is food (`PlayerConsumeEvent.Kind` is `PlayerConsumeKind.Food`).
- `Kind { get; } -> PlayerConsumeKind` — What the consumable is, by the game's sequence for it.

## PlayerConsumeKind — What a consumable does, by the game's own sequence for it.
enum: Other, Food, Drink, Potion, Bandage, Scroll

## PlayerEquipment — What a player wears and holds at one moment.
- `Held { get; } -> PlayerItem` — What is in the player's hand: what the running action uses, else the selected action bar slot.
- `HotbarIndex { get; } -> int` — The selected action bar slot; -1 unknown.
- `this[PlayerEquipSlot slot] { get; } -> PlayerItem` — The item in a slot; `PlayerItem.IsNone` when empty.

## PlayerEquipmentChangedEvent — Something changed in a player's equipment: seen within 4 steps.
- `Before { get; } -> PlayerItem` — The item in the slot before; `PlayerItem.IsNone` when it was empty.
- `Equipped { get; } -> bool` — The slot holds an item now (`PlayerEquipmentChangedEvent.Now` is not none).
- `Now { get; } -> PlayerItem` — The item in the slot now; `PlayerItem.IsNone` when it is empty.
- `Player { get; } -> Player` — The player whose equipment changed.
- `Slot { get; } -> PlayerEquipSlot` — The slot that changed.

## PlayerEquipSlot — A slot of a player's equipment, named as the game's SlotSelection.equipment fields.
enum: VanityHead, VanityUpperBody, VanityArms, VanityLowerBody, VanityFeet, ArmourHead, ArmourUpperBody, ArmourArms, ArmourLowerBody, ArmourFeet, Ring0, Ring1, RangedWeapon, Shield, Arrow, Spell, FishingBait, HookShot, Glider, Backpack

## PlayerEvent — A join or leave seen by kiln.players.
- `PlayerEvent(ulong Sequence, bool Joined, ulong Step, Player Player)`
- `Joined { get; init; } -> bool` — True for a join, false for a leave.
- `Player { get; init; } -> Player` — The player, as they were at that moment.
- `Sequence { get; init; } -> ulong` — kiln.players' running event number.
- `Step { get; init; } -> ulong` — The server-world step the event was seen in.

## PlayerGlideEvent — A player started or stopped gliding.
- `Duration { get; } -> TimeSpan` — How long the glide lasted (wall time); zero at the start.
- `Ended { get; } -> bool` — The glide ended (`PlayerGlideEvent.Started` is false).
- `Glider { get; } -> PlayerItem` — The glider in the player's equipment slot.
- `Player { get; } -> Player` — The player who started or stopped gliding.
- `Started { get; } -> bool` — True: the glide started.

## PlayerHotbarChangedEvent — The selected action bar slot or the item in hand changed: seen within 4 steps.
- `Before { get; } -> int` — The selected action bar slot before; -1 when unknown.
- `Held { get; } -> PlayerItem` — The item in hand now.
- `HeldBefore { get; } -> PlayerItem` — The item in hand before.
- `Now { get; } -> int` — The selected action bar slot now; -1 when unknown.
- `Player { get; } -> Player` — The player whose action bar or held item changed.

## PlayerItem — An item a player has or uses.
- `PlayerItem()` — Wraps a game item id.
- `PlayerItem(uint id)` — Wraps a game item id.
- `Category { get; } -> string` — The game's ItemCategory name: "Consumables", "Weapons", ...
- `Equals(PlayerItem other) -> bool` — True when both are the same item id.
- `Id { get; } -> uint` — The game's item id (FNV-1a of its GUID); 0 = none.
- `IsNone { get; } -> bool` — True when this is no item (`PlayerItem.Id` is 0).
- `Name { get; } -> string` — The game's name for it (what `GameItems` constants hold); "" when none or unreadable here.
- `static operator !=(PlayerItem a, PlayerItem b) -> bool` — True when the item ids differ.
- `static operator !=(PlayerItem a, string? name) -> bool` — The negation of the name comparison.
- `static operator !=(PlayerItem a, uint id) -> bool` — True when the item's id is not `id`.
- `static operator !=(string? name, PlayerItem a) -> bool` — The negation of the name comparison.
- `static operator !=(uint id, PlayerItem a) -> bool` — True when the item's id is not `id`.
- `static operator ==(PlayerItem a, PlayerItem b) -> bool` — True when both are the same item id.
- `static operator ==(PlayerItem a, string? name) -> bool` — True when the item's game name equals `name` (ordinal).
- `static operator ==(PlayerItem a, uint id) -> bool` — True when the item's id is `id`.
- `static operator ==(string? name, PlayerItem a) -> bool` — True when the item's game name equals `name` (ordinal).
- `static operator ==(uint id, PlayerItem a) -> bool` — True when the item's id is `id`.
- `SlotType { get; } -> string` — The game's EquipmentSlotType name: "Consumable", "MeleeWeapon", "Glider", ...

## PlayerJoinedEvent — A player logged in (their entity exists).
- `Player { get; } -> Player` — The player who logged in.
- `Sequence { get; } -> ulong` — kiln.players' running event number.
- `Step { get; } -> ulong` — The server-world step of the join.

## PlayerJumpEvent — A player jumped.
- `Count { get; } -> int` — Jumps since the last landing.
- `IsDoubleJump { get; } -> bool` — The jump was a double jump (`PlayerJumpEvent.Type` is one of the `JumpDouble*` states).
- `Player { get; } -> Player` — The player who jumped.
- `Type { get; } -> PlayerStateId` — `PlayerStateId.JumpDefault`, `PlayerStateId.JumpDoubleDefault`, ...

## PlayerKickedEvent `0.2.2` — Kiln kicked a player: `Players.Kick`, a ban, or a `PlayerLoginEvent` handler that refused.
- `Banned { get; } -> bool` — True when Kiln's ban list refused the connection.
- `By { get; } -> string?` — The mod that kicked; null for Kiln (a ban, a login refusal) or the console.
- `Name { get; } -> string` — The player's name, "" when unknown.
- `Player { get; } -> PlayerRef` — The player (Steam id 0 when the connection named only an account).
- `Reason { get; } -> string` — What the player was told.

## PlayerLeftEvent — A player left (disconnect, kick, shutdown).
- `Player { get; } -> Player` — The player who left, as they were last seen in game.
- `Sequence { get; } -> ulong` — kiln.players' running event number.
- `Step { get; } -> ulong` — The server-world step of the leave.

## PlayerLevelChangedEvent — A player's level changed: a natural level-up (the game's player_level), a Kiln change (`PlayerLevelChangedEvent.Request` is its id) or anything else the game did.
- `Entity { get; } -> EntityId` — The player's entity.
- `NewLevel { get; } -> int` — The level after the change.
- `OldLevel { get; } -> int` — The level before the change.
- `Player { get; } -> Player?` — The player (null if they left in the meantime).
- `Request { get; } -> ulong` — The Kiln request that caused it (`ProgressionResult.Request`), 0 = the game.
- `Step { get; } -> ulong` — The server-world step the change was seen in.

## PlayerLoginEvent `0.2.2` — A connection named its player for the first time (its account id or Steam id is known), on the server, while the player is still loading in.
- `AccountId { get; } -> ulong` — The game's account id from the player record, 0 when unknown.
- `Entity { get; } -> EntityId` — The player's entity, `EntityId.None` before the game made it.
- `HasKiln { get; } -> bool` — The connection proved Kiln (a Kiln client) by now.
- `Name { get; } -> string` — The player's name ("" when the game has not named it yet).
- `Reason { get; set; } -> string` — What a refused player is told.
- `SteamId { get; } -> ulong` — SteamID64 when Kiln.Players knows it, else 0.

## PlayerPermissions — The rights of a player's user group (`enshrouded_server.json` "userGroups"), as the game prints them after "logged in with Permissions:".
enum: None, KickBan, AccessInventories, EditBase, EditWorld, ExtendBase, ReceiveExp

## PlayerProgression — A player's progression (`Player.Progression`): reads and the changes an admin needs.
- `AddExperience(uint amount, ExperienceSource source = ExperienceSource.None) -> Task<ProgressionResult>` — Grants experience like a quest reward: the game levels the player up (one level per step).
- `Entity { get; } -> EntityId` — The player's entity.
- `Experience { get; } -> uint` — Experience into the current level; 0 when the entity is not a player in game.
- `ExperienceNeeded { get; } -> uint` — What the current level needs for the next one; 0 when the game has not set it yet or the entity is not a player in game.
- `GiveSkillPoints(int points) -> Task<ProgressionResult>` — Gives unspent skill points as the game's +1 skill-point knowledge (at most `ProgressionSnapshot.GivableSkillPoints`...
- `Level { get; } -> int` — The player's level; 0 when the entity is not a player in game.
- `Lock(uint node, bool force = false) -> Task<ProgressionResult>` — Locks a node (one level less).
- `Read() -> ProgressionSnapshot?` — The progression now, or null when the entity is not a player in game.
- `Respec() -> Task<ProgressionResult>` — Gives every spent point back (free, like the game's own reset).
- `SetExperience(uint experience) -> Task<ProgressionResult>` — Sets the experience into the current level.
- `SetLevel(int level) -> Task<ProgressionResult>` — Sets the level (1..cap) like the game's admin command: experience into the level 0.
- `SkillPoints { get; } -> int` — Unspent skill points.
- `Unlock(uint node, bool force = false) -> Task<ProgressionResult>` — Unlocks a node (one more level for a node with levels).
- `UnlockedNodes { get; } -> IReadOnlyList<UnlockedSkillNode>` — The nodes the player has unlocked (`Progression.UnlockedNodes`).

## PlayerProgressionChangedEvent `0.2.2` — A Kiln progression request (`Progression.AddExperience`, `Progression.SetExperience`, `Progression.GiveSkillPoints`) changed a player's experience or skill points.
- `After { get; } -> long` — The value after.
- `Before { get; } -> long` — The value before.
- `Involves(EntityId entity) -> bool` — Whether the event is about `entity`.
- `Origin { get; } -> string` — The id of the mod that made the request ("" when Kiln made it for no mod).
- `Player { get; } -> EntityId` — The player's entity.
- `Request { get; } -> ulong` — The Kiln request (`ProgressionResult.Request`).
- `What { get; } -> string` — `Experience` or `SkillPoints`.

## Players — Who is on the server (kiln.players).
- `static All { get; } -> IReadOnlyList<Player>` — Players in game, in slot order.
- `static Available { get; } -> bool` — True when kiln.players is loaded and works on this game build (capability `kiln.players`).
- `static Connected { get; } -> IReadOnlyList<Player>` — Every connection: joining, in game and leaving.
- `static Count { get; } -> int` — How many players are in game.
- `static Find(EntityId entity) -> Player?` — The player with this entity id (any state), or null.
- `static Find(string name) -> Player?` — The player with this name, or null.
- `static FindBySteamId(ulong steamId) -> Player?` — The player with this SteamID64, or null.
- `static FindPerson(PlayerRef person) -> Player?` `0.2.2` — The person `person` as a player in game now (their current session, whatever slot it has), or null while they are...
- `static IsPlayer(EntityId entity) -> bool` — True if the entity is a player in game.
- `static Joined -> Action<Player>` — A player logged in (their entity exists).
- `static Kick(PlayerRef player, string reason) -> Task<KickResult>` `0.2.2` — Kicks a player who is in game: Kiln sends a Kiln client the reason (its overlay shows it), then the game's login...
- `static Left -> Action<Player>` — A player left (disconnect, kick, shutdown).
- `static PositionOf(EntityId entity) -> WorldPosition?` — The player's position now (tick thread) or at the last step (other threads); null if the entity is not a player in...
- `static Source { get; } -> PlayerSource` — Where the data comes from on this build.

## Players.Diagnostics — Diagnostics and testing: status, raw records, the log tracker.
- `static ProbePosition(EntityId entity) -> WorldPosition?` — The position path used for players (GetEntityTransform on the captured server world), for any entity: checks it on a...
- `static RawSlot(int slot) -> PlayerSlotRaw?` — One player record as the game holds it.
- `static ReplayLog(IReadOnlyList<string> messages) -> (IReadOnlyList<Player> Players, IReadOnlyList<PlayerEvent> Events)` — Runs a fresh copy of the service's game-log tracker over `messages` (one game log message each; "\n" inside one is...
- `static Status { get; } -> PlayersStatus?` — What the service can do and saw last; null when kiln.players is not loaded.

## PlayerSkillsChangedEvent — A player's unlocked skill nodes changed (seen as a change of the points they spent): the player's own skill tree, a Kiln change or the game's reset.
- `Entity { get; } -> EntityId` — The player's entity.
- `NewPointsSpent { get; } -> int` — The skill points the unlocked nodes cost after the change.
- `OldPointsSpent { get; } -> int` — The skill points the unlocked nodes cost before the change.
- `Player { get; } -> Player?` — The player (null if they left in the meantime).
- `Request { get; } -> ulong` — The Kiln request that caused it (`ProgressionResult.Request`), 0 = the game (or not known).
- `Step { get; } -> ulong` — The server-world step the change was seen in.

## PlayerSlotRaw — One raw player record and its BasicPlayerInfos entry (diagnostics).
- `PlayerSlotRaw()`
- `PlayerSlotRaw(uint Handle, uint Peer, uint State, uint UserGroup, uint Permissions, uint Entity, uint NameLength, ulong AccountId, uint InfoEntity, uint InfoHandle, uint InfoAdminByte, uint InfoNonZeroBytes)`
- `AccountId { get; init; } -> ulong` — The record's account id.
- `Entity { get; init; } -> uint` — The player entity (0 until it is spawned).
- `Handle { get; init; } -> uint` — The record's connection handle.
- `InfoAdminByte { get; init; } -> uint` — The whole admin byte of the BasicPlayerInfos entry.
- `InfoEntity { get; init; } -> uint` — The entity in the BasicPlayerInfos entry of this slot.
- `InfoHandle { get; init; } -> uint` — The handle in the BasicPlayerInfos entry of this slot.
- `InfoNonZeroBytes { get; init; } -> uint` — How many bytes of the BasicPlayerInfos entry are not zero.
- `NameLength { get; init; } -> uint` — The length of the name in the record.
- `Peer { get; init; } -> uint` — The machine (peer) handle of the connection.
- `Permissions { get; init; } -> uint` — The record's permission bits (see `PlayerPermissions`).
- `State { get; init; } -> uint` — The record's state: 0 free, 1 loading, 2 waiting for login, 3 in game, 4 removing entity, 5 removing, 6 failed.
- `UserGroup { get; init; } -> uint` — The record's user group id.

## PlayerSnapshot — What a player was doing at one moment.
- `Action { get; } -> PlayerAction?` — The action it runs now; null when none.
- `Active { get; } -> IEnumerable<PlayerStateId>` — Every state on right now, with raw ids for states this Kiln has no name for.
- `BodyHeat { get; } -> int?` — The player's body heat; null when the vitals could not be read.
- `BodyHeatMax { get; } -> int?` — The player's maximum body heat; null when the vitals could not be read.
- `CanDive { get; } -> bool?` — The player can dive now; null when unknown.
- `CanSwim { get; } -> bool?` — The player can swim now; null when unknown.
- `Check(PlayerStateId state) -> bool?` — True or false, or null when this state cannot be read here: a state this game build does not have, or a source that...
- `Comfort { get; } -> int?` — The player's comfort value as the game holds it; null when the vitals could not be read.
- `Entity { get; } -> EntityId` — The player's entity.
- `Health { get; } -> int?` — The player's health; null when the vitals could not be read.
- `HealthMax { get; } -> int?` — The player's maximum health; null when the vitals could not be read.
- `Is(PlayerStateId state) -> bool` — True when the player is in this state; false when not, or when it is unknown (see `PlayerSnapshot.Check`).
- `JumpCount { get; } -> int?` — Jumps since the last landing.
- `Level { get; } -> int?` — The player's level; null when the vitals could not be read.
- `Mana { get; } -> int?` — The player's mana; null when the vitals could not be read.
- `ManaMax { get; } -> int?` — The player's maximum mana; null when the vitals could not be read.
- `Origin { get; } -> PlayerStateOrigin` — Where the read came from: the server, a client's prediction or a replicated copy.
- `Oxygen { get; } -> int?` — The player's oxygen; null when the vitals could not be read.
- `OxygenMax { get; } -> int?` — The player's maximum oxygen; null when the vitals could not be read.
- `RawValue(PlayerStateSource source) -> ulong?` — The source's raw value: a bit mask or an enum value; null when unknown.
- `ShroudResistance { get; } -> int?` — The game's FogResistance value.
- `ShroudResistanceMax { get; } -> int?` — The highest `PlayerSnapshot.ShroudResistance` the player can have; null when the vitals could not be read.
- `ShroudTimeLeft { get; } -> TimeSpan?` — The Shroud timer the game shows: `PlayerSnapshot.ShroudResistance` in seconds.
- `SpecialAbility { get; } -> int?` — The player's special ability meter; null when the vitals could not be read.
- `SpecialAbilityMax { get; } -> int?` — The special ability meter's maximum; null when the vitals could not be read.
- `Stamina { get; } -> int?` — The player's stamina; null when the vitals could not be read.
- `StaminaMax { get; } -> int?` — The player's maximum stamina; null when the vitals could not be read.
- `WaterDepth { get; } -> float?` — Water depth at the player, metres; null when unknown.

## PlayerSource — Where kiln.players gets its data on this game build.
enum: None, Records, GameLog

## PlayersStatus — What kiln.players can do on this build, and what it saw last.
- `PlayersStatus(PlayerSource Source, bool Records, bool AdminBit, bool Positions, bool GameLog, bool SingletonMapAgrees, bool LayoutConsistent, int Slots, int InGame, int Connected, ulong LastSequence, ulong Scans, ulong LogLines, ulong LogLinesMatched, bool WorldCaptured, bool ServerPlayersCaptured, bool BasicPlayerInfosCaptured, bool Simulated)`
- `AdminBit { get; init; } -> bool` — The game's BasicPlayerInfos was found, so `Player.IsAdmin` can be read.
- `BasicPlayerInfosCaptured { get; init; } -> bool` — The game's `BasicPlayerInfos` singleton was captured.
- `Connected { get; init; } -> int` — How many records were in use at the last scan: joining, in game and leaving.
- `GameLog { get; init; } -> bool` — kiln.players follows the game log.
- `InGame { get; init; } -> int` — How many players were in game at the last scan.
- `LastSequence { get; init; } -> ulong` — The newest event number (0: none yet).
- `LayoutConsistent { get; init; } -> bool` — The last scan found every record consistent.
- `LogLines { get; init; } -> ulong` — How many game log lines were seen.
- `LogLinesMatched { get; init; } -> ulong` — How many log lines the log tracker understood.
- `Positions { get; init; } -> bool` — Positions work (the entity transform and the server world resolved).
- `Records { get; init; } -> bool` — The game's player records were found (the layout resolved and the singleton was captured).
- `Scans { get; init; } -> ulong` — How many scans were done.
- `ServerPlayersCaptured { get; init; } -> bool` — The game's `ServerPlayers` singleton (the first player record) was captured.
- `Simulated { get; init; } -> bool` — The lab simulation is on: players are replayed from a log file.
- `SingletonMapAgrees { get; init; } -> bool` — The world's singleton map gives the same objects the registration hook saw.
- `Slots { get; init; } -> int` — How many player slots the game has (16 on this build).
- `Source { get; init; } -> PlayerSource` — Where the player data comes from on this build.
- `WorldCaptured { get; init; } -> bool` — The server world object was captured.

## PlayerState — Where a player is in joining and leaving.
enum: Joining, InGame, Leaving

## PlayerStateChangedEvent — Any player state changed: `PlayerStateChangedEvent.State` was entered or left.
- `Entered { get; } -> bool` — True: the player entered `PlayerStateChangedEvent.State`.
- `Left { get; } -> bool` — The player left `PlayerStateChangedEvent.State` (`PlayerStateChangedEvent.Entered` is false).
- `Name { get; } -> string` — The game's name of the state.
- `Player { get; } -> Player` — The player whose state changed.
- `Snapshot { get; } -> PlayerSnapshot` — Everything else at that moment (states and action; health only in a read of its own).
- `Source { get; } -> PlayerStateSource` — Where the state comes from: the game's enum and field that hold it.
- `State { get; } -> PlayerStateId` — The state that was entered or left.
- `Step { get; } -> ulong` — The world step the change was seen in.

## PlayerStateId — A state a player can be in.
enum: None, Grounded, Jumping, Blocking, Dodging, ParryOportunity, BlockOpportunity, Exhausted, Dead, Sheltered, Sitting, PlayingInstrument, Sleeping, Spawning, InFog, InShroud, InSafeZone, InSaveZone, Mining, Warm, Wet, Soaked, Cold, Freezing, InSneakMode, Sneaking, Unhappy, HasAggro, InCombat, InRangeCombat, InRangeCombatPreparing, InRangeCombatShooting, InRangeCombatKeepTarget, HeavyAttack, InSnappingLerp, Cooking, Falling, HeadDiving, Climbing, ClimbingTransition, Attached, Gliding, HangGliding, AttachedToHook, Grappling, Flying, InTurnSequence, InStopSequence, InStartSequence, FloatingOnWater, UnderWater, Underwater, Suffocating, CanPayUsageCosts, Invisible, EnemyPrepareAttack, IsAnythingAbove, IsReadyForGameplay, IsActionOpportunityTriggered, WantsToMove, Strafing, IsNearBuildingZone, HandsFree, IsWorking, Petting, IsSequenceTriggerInputActive, IsChargingFishingRod, IsFishing, IsHoldingFish, FreezeLeaningDirection, LocomotionWalking, LocomotionSliding, Sliding, LocomotionFalling, LocomotionFlying, LocomotionHangGliding, LocomotionHookPulling, LocomotionHookSwinging, ClimbingWall, LocomotionClimbingWall, ClimbingLadder, LocomotionClimbingLadder, LocomotionSwimming, Swimming, Diving, LocomotionDiving, LocomotionBuildingFlying, IsSpawned, InFullScreenUi, UiCoverRequest, CanHeadDiveHere, CanTriggerHeadDive, CanTriggerHookShot, CanTriggerGlider, CanAreaLootHere, IsSkyDiving, InBlueprintSnappingMode, InVoxelGridSnappingMode, WantsToSprint, WantsToWalk, IsSprinting, Sprinting, InTargetLockCombatMode, QueueFireRangedShot, HasRangedWeaponFeedbackBeenSent, IsOutsideSafeZone, IsInBigDungeon, IsInDeepSubmergeMaterial, HideHelmet, HideUpperbodyArmour, HideLowerbodyArmour, HideFeetArmour, HideGloves, MotionLveRunning, MotionSprinting, MotionWadingInWater, MotionOffBalance, MotionMovingTowardsHole, MotionAirwolfActive, MotionIsUpdraftDepleted, MotionWantsToMove, MotionOnVoxelStairs, ClientUiIsInMultiToolBuildingMenu, ClientUiIsInBlueprintMenu, ClientUiIsInLogicScrewdriverMode, ClientUiIsInLootingMenu, ClientUiIsInFreeCam, ClientUiIsInFirstPerson, ClientUiIsInBuildingDetailTranslationMode, ClientUiIsInBuildingDetailRotationMode, ClientUiIsLockCombatTargetBindingEnabled, AbilityCanTriggerPrimary, AbilityCanTriggerHeavy, AbilityCanTriggerSecondary, AbilityCanTriggerSpecialAbility, AbilityCanRangedCombatShoot, AbilityCanUseTool, AbilityCanEvade, AbilityCanBlock, AbilityCanUseLongerStunParry, AbilityCanJump, AbilityCanTriggerGlider, AbilityCanInteract, AbilityCanSprint, AbilityCanSneak, AbilityCanStandUp, AbilityCanSit, AbilityCanClimb, AbilityCanTriggerStunBreak, AbilityCanExternallyAbortSequence, AbilityCanAbortSequenceByDeath, AbilityCanShowHitReaction, AbilityCanChangeCombatTargetOrHardLockCandidate, AbilityCanChangeVisceralTarget, AbilityCanChangeInteractionTarget, AbilityCanChangeBuildingCursor, AbilityCanRecharge, AbilityCanMove, AbilityCanRotate, AbilityCanCancel, InputMainhandAction, InputMainhandAction_Tap, InputMainhandAction_Hold, InputMainhandAction_Release, InputSecondaryBuildingAction, InputContextualAction, InputContextualAction_Tap, InputContextualAction_Hold, InputAreaLoot, InputSpecialAbility, InputSpecialAbility_Tap, InputSpecialAbility_Hold, InputDropItem, InputCancelAction, InputQuickUse0, InputQuickUse1, InputQuickUse2, InputQuickUse3, InputQuickUse4, InputQuickUse5, InputQuickUse6, InputQuickUse7, InputQuickUse0_Release, InputQuickUse1_Release, InputQuickUse2_Release, InputQuickUse3_Release, InputQuickUse4_Release, InputQuickUse5_Release, InputQuickUse6_Release, InputQuickUse7_Release, InputBlock, InputEvade, InputJump, InputHeadDive, InputSwimStartDiving, InputDiveAscend, InputDiveDescend, InputGlider, InputRespawn, InputBuildingModeFly, InputBuildingUndo, InputBuildingBlueprintSnapping, InputBuildingVoxelGridSnapping, InputLockCombatTarget, InputCombatTargetSelect1dLeft, InputCombatTargetSelect1dRight, InputCombatTargetSelect2dLeft, InputCombatTargetSelect2dRight, InputCombatTargetSelect2dUp, InputCombatTargetSelect2dDown, InputRangedCombatArm, InputRangedCombatShoot, InputRangedCombatShoot_Tap, InputSprint, InputWalk, InputSneak, ActionInvalid, ActionPrimary, ActionPrimary_Tap, ActionPrimary_Hold, ActionHeavy, ActionSecondary, ActionTool, ActionEvade, ActionBlock, ActionParry, ActionJump, ActionGlider, ActionInteract, ActionInteractHold, ActionDismantle, ActionSpecialAbility, ActionDropItem, ActionSneak, ActionSit, ActionClimb, ActionHitReaction, ActionRespawn, ActionExternalTrigger, ActionParried, ActionExternalAbortAndOverride, ActionDeath, ActionParriedOverride, JumpInvalid, JumpDefault, JumpDoubleDefault, JumpDoubleSprint, JumpDoubleMountain, JumpSprint, JumpDefaultSubmerged, JumpStruggleSubmerged, JumpGlider, JumpHookSwingEnter, JumpHookSwingDefaultExit, JumpHookSwingSprintExit, JumpLadderClimbing, JumpFreeClimbing, JumpMountain, JumpSlide, JumpAttack, JumpAirwolf, JumpNpcMovement, JumpJumpPad, SpeedZero, SpeedSneak, SpeedWalk, SpeedRun, SpeedSprint, SpeedSwim, SpeedDive, SpeedSwimSlow, SpeedDiveSlow, SpeedSwimSprint, SpeedDiveSprint, SpeedCombat, SpeedPatrol, SpeedFlee, SpeedCustom, ColliderDefault, ColliderSitting, ColliderLying, ColliderEvading, ColliderClimbingWall, ColliderDeath, ColliderInvisible, ColliderSwimming, ColliderDivingAlive, ColliderDivingDeath, ColliderFlying, ClimbInvalid, ClimbEnter, ClimbClimb, ClimbExit, ClimbTypeInvalid, ClimbTypeWall, ClimbTypeLadder, HookInvalid, HookEnter, HookUpdate, HookExit, HookTypeNone, HookTypePullToggleEntity, HookTypePullTarget, HookTypePullSelf, HookTypeSwingSelf, HookTypeAuto, RangedHolster, RangedAim, RangedAimAndCharge, RangedFire, RangedFireEaseOut, FishingInvalid, FishingIdle, FishingFishingMinigame, FishingAim, FishingAimAndCharge, FishingWaitForCastAvailable, FishingAbortPreHooked, FishingPreGame, FishingPrepareCastFloat, FishingCastFloat, FishingWaitForFloatToLand, FishingWaitForValidFloatLocation, FishingPreGame_EnsureFloatInWater, FishingReadSpawnTable, FishingSpawnFish, FishingWaitFishSpawned, FishingWaitForFish, FishingWaitForFishAttracted, FishingWaitForFishHooked, FishingFishHooked, FishingConsumeBait, FishingWaitHookInput, FishingAdvancedMinigame, FishingWaitPullCloser, FishingPreparePullCloser, FishingPullCloser, FishingInitMinigame, FishingInitFightStage, FishingDecideOutbreak, FishingOutbreak, FishingFinalHook, FishingStartReelIn, FishingReelIn, FishingFinishReelIn, FishingFloatReturned, FishingVictory, FishingWaitForPlayerInteraction, FishingWaitForLootFish, FishingWaitForDiscardFish, FishingEmergencyExit, CookingFlagReceivedSpawnCookEvent, CookingFlagReceivedStartCookEvent, CookingFlagIsCooking, CookingFlagCanCook, ReviveInvalid, ReviveReviving, ReviveGettingRevived, FogNone, FogNoneTemporary, FogDangerous, FogDeath, TemperatureNormal, TemperatureFreezing, Airborne, Attacking, Casting, UsingItem, InBase, Rested, Fed, HasDebuff, Hungry

## PlayerStateInfo — One state the running game has.
- `PlayerStateInfo(PlayerStateId Id, PlayerStateSource Source, ulong Raw, string GameName, bool IsFlag, bool NamedByKiln)`
- `GameName { get; init; } -> string` — The game's own name of the value.
- `Id { get; init; } -> PlayerStateId` — Use it with `Player.Is`; a state only a newer game build has is a value without a name in the enum.
- `IsFlag { get; init; } -> bool` — True: a bit, any number can be on.
- `NamedByKiln { get; init; } -> bool` — False: this build of Kiln has no constant for it (a newer game).
- `Raw { get; init; } -> ulong` — Its bit number or value in that field, as the running game numbers it.
- `Source { get; init; } -> PlayerStateSource` — The game's enum and field it comes from.

## PlayerStateOrigin — Where a state read came from.
enum: Server, ClientPrediction, Replicated

## PlayerStates — Player state on any entity id.
- `static AimOf(EntityId player) -> PlayerAim?` `0.2.2` — Where a player's camera is and where it looks (`PlayerAim`): read now from the player's `PlayerInput.fromClient.camera`.
- `static All { get; } -> IReadOnlyList<PlayerStateInfo>` — Every state the running game has, read from its reflection, with what this Kiln calls them.
- `static Available { get; } -> bool` — True when kiln.playerstate is loaded and read at least one source from the running game.
- `static BuffsGivenBy(PlayerItem item) -> IReadOnlyList<PlayerBuff>` — The buffs an item gives through its ItemInfo.equipment.appliedBuff.
- `static BuffsOf(EntityId player) -> IReadOnlyList<PlayerBuff>` — Every buff on the player, from the game's buff slots.
- `static Check(EntityId player, PlayerStateId state) -> bool?` — True or false, or null when it cannot be read: no such player, or the state does not exist here.
- `static ClientActions { get; } -> IReadOnlyList<PlayerClientActionInfo>` — The actions a client can send on this build, named by the game's own fields: "changeEquipmentAction"...
- `static EquipmentOf(EntityId player) -> PlayerEquipment?` — What the player has equipped and holds.
- `static Find(string gameName, PlayerStateSource source = PlayerStateSource.None) -> PlayerStateId?` — Finds a state by the game's name, in one source or in all (the first match).
- `static InfoOf(PlayerStateId state) -> PlayerStateInfo?` — The state's row in `PlayerStates.All`; null when the running game does not have it.
- `static Is(EntityId player, PlayerStateId state) -> bool` — True when the player is in the state; false when not, or when unknown.
- `static NameOf(PlayerStateId state) -> string` — The game's name of a state, also for one this Kiln has no constant for.
- `static Raw(PlayerStateSource source, int raw) -> PlayerStateId` — The id of a raw bit or value of a source, with or without a constant in this Kiln.
- `static Read(EntityId player) -> PlayerSnapshot?` — Reads a player now (the world's thread), or gives the last tick's copy (other threads; null until there is one).
- `static SourceOf(PlayerStateId state) -> PlayerStateSource` — Which source a state belongs to.
- `static Status { get; } -> PlayerStateServiceStatus?` — What works on this build.

## PlayerStates.Diagnostics — Diagnostics and testing.
- `static Bench(IReadOnlyList<EntityId> players, bool motion, bool input, bool extra) -> long` — One native read of these players, as the watch does it: nanoseconds.
- `static LastPumpNanoseconds { get; } -> long` — Time of the last full pump of the events, nanoseconds (0 when nothing subscribes).
- `static PumpRuns { get; } -> long` — Ticks in which the pump ran.
- `static PumpTotalNanoseconds { get; } -> long` — The per-tick pump's total time, nanoseconds, native reads included.
- `static Watch(EntityId entity, bool on = true) -> void` — Scans an entity with no player record too (a spawned player character in a test lab), so its changes raise the events.

## PlayerStateServiceStatus — What kiln.playerstate can do on this build.
- `PlayerStateServiceStatus(bool Reflection, bool Cancel, bool SequenceNames, bool ItemDetails, int Sources, ulong Scans, ulong PlayersRead, string Detail)`
- `Cancel { get; init; } -> bool` — A `PlayerActionEvent` can be canceled (the game's reset of a running sequence resolved).
- `ClientActionCancel { get; init; } -> bool` — A `PlayerClientActionEvent` can be canceled: the point right before the game's programs, where Kiln holds a client's...
- `Detail { get; init; } -> string` — What did not resolve, in words.
- `ItemDetails { get; init; } -> bool` — Item details can be read (`PlayerItem.Name`, `PlayerItem.Category`, `PlayerItem.SlotType`).
- `PlayersRead { get; init; } -> ulong` — How many player reads were done.
- `Reflection { get; init; } -> bool` — Field offsets and enum names came from the game's reflection.
- `Scans { get; init; } -> ulong` — How many scans were done.
- `SequenceNames { get; init; } -> bool` — Sequence names can be read (`PlayerAction.SequenceName`).
- `Sources { get; init; } -> int` — How many state sources resolved.

## PlayerStateSource — Where a state comes from.
enum: None, Actor, Locomotion, Player, Motion, ClientUi, Ability, Input, Action, Jump, Speed, Collider, Climb, ClimbType, Hook, HookType, Ranged, Fishing, CookingFlag, Revive, Fog, Temperature, Derived

## PlayerUnbannedEvent `0.2.2` — An entry left Kiln's ban list (`Bans.Remove`; not when one runs out).
- `Ban { get; } -> BanEntry` — The entry that was removed.
- `By { get; } -> string` — Who removed it.

## Progression — Level, experience, skill tree and skill points of the players in game (kiln.progression).
- `static AddExperience(EntityId entity, uint amount, ExperienceSource source = ExperienceSource.None, bool sendEvent = true) -> Task<ProgressionResult>` — Grants experience like a quest reward: the game counts it in its next step and levels the player up, one level per step.
- `static Available { get; } -> bool` — True when any part of kiln.progression works on this build (see `Progression.Status`; capability `kiln.progression`).
- `static FindNode(uint id) -> SkillNode?` — The node with this id, or null.
- `static Get(EntityId entity) -> ProgressionSnapshot?` — A player's progression, or null when the entity is not a player in game.
- `static GiveSkillPoints(EntityId entity, int points) -> Task<ProgressionResult>` — Gives unspent skill points as the game's +1 skill-point knowledge, at most `ProgressionSnapshot.GivableSkillPoints`.
- `static Lock(EntityId entity, uint node, bool force = false) -> Task<ProgressionResult>` — Locks a node for a player (one level less for a node with levels).
- `static ParseNode(string text) -> SkillNode?` — Parses a node given as "0x1234abcd", hex digits ("ffd5f940") or a decimal id; null when it is not in the tree.
- `static Respec(EntityId entity) -> Task<ProgressionResult>` — Locks every node and gives the spent points back, free, like the game's own reset.
- `static SetExperience(EntityId entity, uint experience) -> Task<ProgressionResult>` — Sets a player's experience into the current level.
- `static SetLevel(EntityId entity, int level) -> Task<ProgressionResult>` — Sets a player's level (1 to the cap), like the game's admin command: experience into the level goes to 0.
- `static Status { get; } -> ProgressionStatus?` — What works and the counters; null when kiln.progression is not loaded.
- `static Tree { get; } -> IReadOnlyList<SkillNode>` — The game's skill tree (empty before the server world's first step).
- `static Unlock(EntityId entity, uint node, bool force = false) -> Task<ProgressionResult>` — Unlocks a node for a player (one more level for a node with levels).
- `static UnlockedNodes(EntityId entity) -> IReadOnlyList<UnlockedSkillNode>` — The nodes a player has unlocked (tick thread).

## Progression.Diagnostics — Diagnostics and tests.
- `const AttrExperience = 2379078388 -> uint` — The game's attribute ids of the progression attributes (sigdb players/attr.*).
- `const AttrGranted = 494784770 -> uint` — The game's attribute ids of the progression attributes (sigdb players/attr.*).
- `const AttrLevel = 1917580648 -> uint` — The game's attribute ids of the progression attributes (sigdb players/attr.*).
- `const AttrNeeded = 74596194 -> uint` — The game's attribute ids of the progression attributes (sigdb players/attr.*).
- `static ExperienceForLevel(int level) -> (uint Needed, int Cap)` — The game's own experience table: what `level` needs for the next level, and the level cap.
- `static ProbeAttributes(bool level, uint[] storage, uint setId = 0, uint setValue = 0) -> Progression.Diagnostics.AttrProbe` — The game's attribute lookup and formula re-run on a scratch Experience (`level` false, 3 values) or Level (true, 2...

## Progression.Diagnostics.AttrProbe — What `Diagnostics.ProbeAttributes` found: the root id, the storage after the write, and the storage slot of level / experience / granted / needed (-1: not in this...
- `AttrProbe()`
- `AttrProbe(uint Root, uint[] Storage, int[] Slots)`
- `Root { get; init; } -> uint` — The component's attribute root id.
- `Slots { get; init; } -> int[]` — The storage slot of level, experience, granted and needed, in that order; -1 where the component has none.
- `Storage { get; init; } -> uint[]` — The component's values after the write and the game's formulas re-ran (3 for an Experience component, 2 for a Level...

## ProgressionOp — What a progression request does.
enum: SetLevel, AddExperience, SetExperience, GiveSkillPoints, Unlock, Lock, Respec

## ProgressionResult — What a progression request did.
- `ProgressionResult(ulong Request, ProgressionOp Op, EntityId Entity, bool Ok, string Status, int Applied, int Steps, ProgressionSnapshot? Before, ProgressionSnapshot? After, string Reason)`
- `After { get; init; } -> ProgressionSnapshot?` — The player's progression after the change; null when the game gave none (a request refused before it ran).
- `Applied { get; init; } -> int` — SetLevel: the level set.
- `Before { get; init; } -> ProgressionSnapshot?` — The player's progression before the change; null when the game gave none (a request refused before it ran).
- `Entity { get; init; } -> EntityId` — The player's entity.
- `Ok { get; init; } -> bool` — The request did what it asked.
- `Op { get; init; } -> ProgressionOp` — What the request did.
- `Reason { get; init; } -> string` — Why the request failed, in words.
- `Request { get; init; } -> ulong` — The request's id; 0 when the request was refused before it was queued.
- `Status { get; init; } -> string` — A short code for the outcome: `ok`, `invalid`, `not_found`, `unavailable`, `wrong_thread`, `busy` or `internal`.
- `Steps { get; init; } -> int` — How many server-world steps passed from the request to its result.

## ProgressionSnapshot — A player's progression as the game holds it, read at `ProgressionSnapshot.Step`.
- `ProgressionSnapshot(EntityId Entity, int Level, int LevelCap, uint Experience, uint ExperienceNeeded, uint ExperiencePending, int SkillPoints, int SkillPointsTotal, int SkillPointsSpent, int SkillPointsFromLevel, int SkillPointsFromKnowledge, int SkillPointsPerLevel, int UnlockedNodes, int GivableSkillPoints, bool HasLevel, bool HasSkills, bool HasPoints, bool HasKnowledge, bool AtLevelCap, bool OverBudget, bool IsSnapshot, ulong Step)`
- `AtLevelCap { get; init; } -> bool` — The player is at the level cap: the game discards new experience.
- `Entity { get; init; } -> EntityId` — The player's entity.
- `Experience { get; init; } -> uint` — Experience into the current level.
- `ExperienceNeeded { get; init; } -> uint` — What the current level needs for the next one (0: the game has not set it yet).
- `ExperiencePending { get; init; } -> uint` — Granted experience the game's player_level counts in the next step.
- `GivableSkillPoints { get; init; } -> int` — What `PlayerProgression.GiveSkillPoints` can still add.
- `HasKnowledge { get; init; } -> bool` — The givable skill points are valid.
- `HasLevel { get; init; } -> bool` — The level and experience values are valid.
- `HasPoints { get; init; } -> bool` — The skill point totals are valid.
- `HasSkills { get; init; } -> bool` — The spent points and the node count are valid.
- `IsSnapshot { get; init; } -> bool` — The values come from the last snapshot (read from another thread than the tick thread), not live.
- `Level { get; init; } -> int` — The player's level.
- `LevelCap { get; init; } -> int` — The game's player level cap.
- `OverBudget { get; init; } -> bool` — The unlocked nodes cost more than the player has (`SkillPointsSpent` is above `SkillPointsTotal`): the game resets...
- `SkillPoints { get; init; } -> int` — Unspent skill points: `ProgressionSnapshot.SkillPointsTotal` - `ProgressionSnapshot.SkillPointsSpent` (negative: the...
- `SkillPointsFromKnowledge { get; init; } -> int` — From the player's skill-point knowledge (wells, "additional skill point" rewards).
- `SkillPointsFromLevel { get; init; } -> int` — SkillPointsPerLevel x (Level - 1).
- `SkillPointsPerLevel { get; init; } -> int` — The skill points each level above 1 gives.
- `SkillPointsSpent { get; init; } -> int` — The points the unlocked nodes cost.
- `SkillPointsTotal { get; init; } -> int` — The skill points the player has in all: `SkillPointsFromLevel` + `SkillPointsFromKnowledge`.
- `Step { get; init; } -> ulong` — The server-world step of the read.
- `UnlockedNodes { get; init; } -> int` — How many nodes the player has unlocked (a node with levels counts once).

## ProgressionStatus — What kiln.progression can do on this build.
- `ProgressionStatus(bool Levels, bool Skills, bool SkillKnowledge, bool ExperienceEvents, bool Program, bool World, int TreeNodes, int KnowledgeEntries, int KnowledgePlusOne, int LevelCap, int SkillPointsPerLevel, int Pending, ulong Submitted, ulong Applied, ulong Failed, ulong ProgramUpdates, string Reason)`
- `Applied { get; init; } -> ulong` — Requests applied so far.
- `ExperienceEvents { get; init; } -> bool` — The game's experience-gained event is sent for `Progression.AddExperience`.
- `Failed { get; init; } -> ulong` — Requests that failed so far.
- `KnowledgeEntries { get; init; } -> int` — How many skill-point knowledge entries the game has (+1 and +3).
- `KnowledgePlusOne { get; init; } -> int` — Of those, how many give +1 (what `Progression.GiveSkillPoints` uses).
- `LevelCap { get; init; } -> int` — The game's player level cap.
- `Levels { get; init; } -> bool` — Levels and experience work.
- `Pending { get; init; } -> int` — Requests not applied yet.
- `Program { get; init; } -> bool` — Kiln's progression program runs.
- `ProgramUpdates { get; init; } -> ulong` — How many times the program ran.
- `Reason { get; init; } -> string` — Why something is off, in words.
- `SkillKnowledge { get; init; } -> bool` — Skill-point knowledge works (what `Progression.GiveSkillPoints` uses).
- `SkillPointsPerLevel { get; init; } -> int` — The skill points each level gives.
- `Skills { get; init; } -> bool` — The skill tree works.
- `Submitted { get; init; } -> ulong` — Requests submitted so far.
- `TreeNodes { get; init; } -> int` — How many nodes the skill tree has.
- `World { get; init; } -> bool` — The server world's singletons were found.

## SkillNode — One node of the game's skill tree.
- `SkillNode()`
- `SkillNode(uint Id, SkillNodeType Type, int Cost, int MaxLevel, int LevelIndex, int ClassAlignment, uint ActiveSkillItem, int Incoming, int Outgoing, int Index, float UiX, float UiY)`
- `ActiveSkillItem { get; init; } -> uint` — ItemId of the ability item the node unlocks (0: none).
- `ClassAlignment { get; init; } -> int` — The class the node belongs to, as the game's ClassType: 0 Warrior, 1 Mage, 2 Ranger, 3 Neutral.
- `Cost { get; init; } -> int` — Skill points (per level for nodes with levels).
- `Id { get; init; } -> uint` — The node's id (the game's SkillNodeId, an FNV-1a hash of its editor GUID).
- `Incoming { get; init; } -> int` — How many links lead into the node.
- `Index { get; init; } -> int` — The node's position in the skill tree resource.
- `LevelIndex { get; init; } -> int` — The node's level index in the game's skill tree data.
- `MaxLevel { get; init; } -> int` — 0: one unlock; else levels 1..MaxLevel.
- `Outgoing { get; init; } -> int` — How many links lead out of the node.
- `Type { get; init; } -> SkillNodeType` — What kind of node it is.
- `UiX { get; init; } -> float` — The node's x position in the game's skill tree screen.
- `UiY { get; init; } -> float` — The node's y position in the game's skill tree screen.

## SkillNodeType — The game's skill node types.
enum: Root, Attribute, ActiveSkill, PassiveSkill

## UnlockedSkillNode — A node a player has unlocked.
- `UnlockedSkillNode()`
- `UnlockedSkillNode(uint Id, int Level, int MaxLevel, int Cost)`
- `Cost { get; init; } -> int` — What the node costs now, in skill points.
- `Id { get; init; } -> uint` — The node's id (see `SkillNode.Id`).
- `Level { get; init; } -> int` — The node's level: 1 for a plain node, up to `MaxLevel` for a node with levels.
- `MaxLevel { get; init; } -> int` — The node's highest level (0: a node with one unlock).
