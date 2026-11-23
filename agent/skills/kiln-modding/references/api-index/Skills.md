# Skills (Kiln.Skills)

## GameNodeDefinition — A node for the game's own skill tree (drawn by the game's skill-tree screen, unlocked with the game's skill points, effects run by the game).
- `Class { get; init; } -> string?` — "Warrior", "Mage", "Ranger" or "Neutral" (default: the like node's).
- `Configs { get; init; } -> IReadOnlyDictionary<uint, double>?` — Number configs of the like node to change: config id (0x...) -> value.
- `Cost { get; init; } -> int` — Skill points (per level for nodes with levels).
- `From { get; init; } -> IReadOnlyList<string>` — Nodes that lead to this one ("0x\<id>" of a game node, a node GUID, or a Kiln node's `GameNodeDefinition.Name`).
- `Guid { get; init; } -> Guid`
- `Icon { get; init; } -> Guid?` — A UiTexture GUID (e.g. an icon kiln.assets serves, from assets/assets.json) or null.
- `IconFile { get; init; } -> string?` — A PNG for the icon (absolute, or relative to the kiln folder); kiln.assets serves it on clients.
- `Id { get; } -> uint` — This node's id in the game (FNV-1a of the GUID's bytes).
- `Like { get; init; } -> string?` — A node whose effect (program, config values, tooltip values), icon and texts to copy.
- `MaxLevel { get; init; } -> int?` — 0 = one unlock; 1..7 = levels (default: the like node's).
- `Name { get; init; } -> string` — A debug name (also how other Kiln nodes refer to this one).
- `Near { get; init; } -> string?`
- `Offset { get; init; } -> (float X, float Y)?`
- `Position { get; init; } -> (float X, float Y)?` — Absolute position in the game's tree units (the game's tree spans about 1700..6500); null = near...
- `Program { get; init; } -> Guid?` — An ImpactProgram to run instead of the like node's (tools/impact, a Kiln program).
- `Scales { get; init; } -> IReadOnlyDictionary<uint, double>?` — Scale factors of the like node's scaled configs.
- `Texts { get; init; } -> IReadOnlyDictionary<string, (string Name, string Description, string? PerLevel)>?` — language ("en", "de", "*") -> (name, description, perLevel).
- `To { get; init; } -> IReadOnlyList<string>`
- `Type { get; init; } -> string?` — "PassiveSkill" (default: the like node's), "Attribute" or "ActiveSkill".
- `UnlocksAbility { get; init; } -> Guid?` — The Ability_* item the node unlocks (its activeSkillUnlockRef).

## GameNodeInfo — A definition as kiln.skills holds it.
- `GameNodeInfo(Guid Guid, uint Id, string Name, string Owner, string Source, string State, int Index, int LevelIndex, string? Error)`
- `Error { get; init; } -> string?`
- `Guid { get; init; } -> Guid`
- `Id { get; init; } -> uint`
- `Index { get; init; } -> int`
- `LevelIndex { get; init; } -> int`
- `Name { get; init; } -> string`
- `Owner { get; init; } -> string`
- `Source { get; init; } -> string`
- `State { get; init; } -> string`

## GameNodesRefundedEvent — A player joined with game-tree nodes the tree does not know (a mod that added them is gone): kiln.skills removed them before the game's own check (which would reset...
- `Nodes { get; } -> IReadOnlyList<(uint Id, int Level)>` — The removed node ids and their levels.
- `Player { get; } -> EntityId` — The player (None when Kiln could not tell which player it was).
- `Step { get; } -> ulong`

## GameShadowMessage — Server to client: the local player's shadow levels.
- `GameShadowMessage(Dictionary<string, int> Levels, long Revision)`
- `Levels { get; init; } -> Dictionary<string, int>`
- `Revision { get; init; } -> long`

## GameShadowState — A character's levels of shadow nodes (carried character data "gameShadow" of Kiln.Skills' data owner).
- `Levels { get; set; } -> Dictionary<string, int>` — "0x\<node id>" -> level.
- `Revision { get; set; } -> long`

## GameTree — Nodes in the game's own skill tree.
- `static Available { get; } -> bool` — True when kiln.skills is loaded on this game build (capability `kiln.skills`), or while Kiln.Sdk's build bakes a...
- `static Define(params GameNodeDefinition[] nodes) -> IReadOnlyList<GameNodeInfo>` — Defines (or redefines) nodes as the calling mod's.
- `static DefineJson(string json) -> IReadOnlyList<GameNodeInfo>` — Defines nodes from JSON (one node object, an array, or {"nodes": [...]}), as in skills/*.json.
- `static Definitions { get; } -> IReadOnlyList<GameNodeInfo>` — Every definition (all mods, files and code) with its state and tree index.
- `static Dump(bool addedOnly = false) -> string` — The server world's tree as the game holds it (JSON: nodes by index through the game's own lookup, map counts).
- `static GameRequest(EntityId player, int op, uint node = 0) -> bool` — Asks the game to unlock (0) / unlearn (1) a node or reset the tree (2) for a server-world player entity the way its...
- `static PointsSpent(EntityId player) -> int?` — The game's own PointsSpent for a player's real components (the shadow table answers inside it).
- `static Query(EntityId player, uint node) -> (bool Unlocked, bool Reachable)?` — The game's own IsUnlocked and Reachable for a node of a player (the shadow table answers inside them).
- `static Resource(int which, out string check) -> byte[]` — The resource kiln.skills serves (or would serve now): `which` 0 = expanded, 1 = the game's own, 2 = the copy path...
- `static SetShadowLevels(EntityId player, IReadOnlyDictionary<uint, int> levels) -> void` — Replaces a player's shadow levels (tick thread).
- `static ShadowEffects(EntityId player) -> IReadOnlyList<(uint Node, int Level, EntityId Impact)>` — The effect entities Kiln runs for a player's shadow nodes.
- `static ShadowLevel { get; } -> int` — The shadow table is on (nodes past the game's 256-node / 100-level caps work): 1 server side, 2 with the client's...
- `static ShadowLevels(EntityId player, int world = 0) -> IReadOnlyDictionary<uint, int>` — A player's shadow levels (server world: world 0; a joined client's local player: world 2).
- `static ShadowNodes { get; } -> IReadOnlySet<uint>` — Ids of the shadow nodes (index 256 and up) in the tree Kiln serves now.
- `static Status { get; } -> GameTreeStatus?`

## GameTreeStatus — What kiln.skills does with the game's own tree right now.
- `GameTreeStatus(bool TreeExpansion, bool LoginGuard, bool GameFunctions, bool NodeTexts, int GameNodes, int GameLinks, int GameLevelMax, int Nodes, int Links, int AddedNodes, int AddedLinks, int FreeNodes, int FreeLevelSlots, int Defined, int Refused, int Pending, int WorldsServed, int Builds, ulong Refunded, string Reason, int ShadowNodes = 0, int ShadowLevel = 0)`
- `AddedLinks { get; init; } -> int`
- `AddedNodes { get; init; } -> int`
- `Builds { get; init; } -> int`
- `Defined { get; init; } -> int`
- `FreeLevelSlots { get; init; } -> int`
- `FreeNodes { get; init; } -> int`
- `GameFunctions { get; init; } -> bool`
- `GameLevelMax { get; init; } -> int`
- `GameLinks { get; init; } -> int`
- `GameNodes { get; init; } -> int`
- `Links { get; init; } -> int`
- `LoginGuard { get; init; } -> bool`
- `Nodes { get; init; } -> int`
- `NodeTexts { get; init; } -> bool`
- `Pending { get; init; } -> int`
- `Reason { get; init; } -> string`
- `Refunded { get; init; } -> ulong`
- `Refused { get; init; } -> int`
- `ShadowLevel { get; init; } -> int`
- `ShadowNodes { get; init; } -> int`
- `TreeExpansion { get; init; } -> bool`
- `WorldsServed { get; init; } -> int`

## Skills — Kiln talent trees (and, in `GameTree`, nodes in the game's own skill tree).
- `static ActiveEffects(EntityId player) -> IReadOnlyList<(string Tree, string Node, int Level, IReadOnlyList<EntityId> Entities)>` — The effects running for a player now (diagnostics): tree/node -> level and effect entities.
- `static Available { get; } -> bool`
- `static EnsureStarted() -> void` — Starts Kiln.Skills' channel, /talents and its tick (Kiln does this at its first service tick).
- `static Find(string id) -> TalentTree?`
- `static Grant(string tree, EntityId player, int points) -> TalentPointsInfo` — Gives points of the tree's own (any points source; negative takes back).
- `static Grant(string tree, TalentHolder holder, int points) -> TalentPointsInfo` — Gives points of the tree's own (any points source; negative takes back).
- `static Has(EntityId player, string tree, string node) -> bool` — True when the player has the node (level 1 or more).
- `static HolderOf(CharacterKey character, ulong legacySteamId = 0) -> TalentHolder` — The holder of a character, online or not.
- `static HolderOf(EntityId player) -> TalentHolder` — The holder of a player entity in game: the game's character, the Steam id from kiln.players.
- `static LevelOf(string tree, string node, TalentHolder holder) -> int` — The level of a node for a character (0 = locked).
- `static LoadTrees(string? modDirectory = null) -> IReadOnlyList<TalentTree>` — Loads the "trees" of every skills/*.json in `modDirectory` (default: the calling mod's folder) and registers them as...
- `static Lock(string tree, string node, TalentHolder holder, bool force = false) -> TalentResult` — Gives one level back (the points return).
- `static Points(string tree, TalentHolder holder) -> TalentPointsInfo`
- `static Refresh(string tree, TalentHolder holder) -> void` — Applies the player's state now (instead of at the next maintenance pass).
- `static Register(TalentTree tree) -> TalentTree` — Registers a tree as the calling mod's (it goes away when the mod unloads; players' states stay in that mod's player...
- `static Respec(string tree, TalentHolder holder, bool force = false) -> TalentResult` — Gives every point of the tree back (free).
- `static State(string tree, EntityId player) -> TalentTreeState` — A character's state in a tree (live object; the player need not be online).
- `static State(string tree, TalentHolder holder) -> TalentTreeState` — A character's state in a tree (live object; the player need not be online).
- `static Trees { get; } -> IReadOnlyList<TalentTree>` — Every registered tree.
- `static Unlock(EntityId player, string tree, string node) -> TalentResult` — Takes a node (one more level).
- `static Unlock(string tree, string node, TalentHolder holder, bool force = false) -> TalentResult` — Takes a node (one more level).

## TalentAttributes — The AttributeReference GUIDs the game's attribute nodes use (from its SkillTreeResource, svn 1287369; the names were matched in the lab through each attribute...
- `static readonly Constitution -> Guid`
- `static readonly Dexterity -> Guid`
- `static readonly Endurance -> Guid`
- `static readonly Intelligence -> Guid`
- `static NameOf(Guid g) -> string`
- `static readonly Spirit -> Guid`
- `static readonly Strength -> Guid`

## TalentChange
- `TalentChange(TalentTree Tree, TalentHolder Holder, TalentChangeKind Kind, string? Node, int OldLevel, int NewLevel, int Points)`
- `Holder { get; init; } -> TalentHolder`
- `Kind { get; init; } -> TalentChangeKind`
- `NewLevel { get; init; } -> int`
- `Node { get; init; } -> string?`
- `OldLevel { get; init; } -> int`
- `Points { get; init; } -> int`
- `Tree { get; init; } -> TalentTree`

## TalentChangedEvent — A player's tree changed (after the effects were applied).
- `Change { get; } -> TalentChange`

## TalentChangeKind — What changed in a player's tree.
enum: Unlock, Lock, Respec, Grant, Reset

## TalentContext — Passed to callbacks and effects (server, tick thread).
- `Holder { get; } -> TalentHolder`
- `Level { get; } -> int` — The level being applied (or the one being removed).
- `Node { get; } -> TalentNode`
- `Player { get; } -> EntityId`
- `State { get; set; } -> object?` — Anything the effect wants to keep until its remove (per player and node).
- `Tree { get; } -> TalentTree`

## TalentEffect — What a node does while it is unlocked.
- `static Ability(string abilityId) -> TalentEffect` — A Kiln.Abilities ability (id) that only players with this node can cast.
- `static Attribute(Guid attribute, int amountPerLevel = 1) -> TalentEffect` — The game's own attribute-node program (the "+1 Strength" kind of node) with `amountPerLevel` x level of `attribute`...
- `static Buff(Guid buff, double refreshSeconds = 30) -> TalentEffect` — A buff (a game buff's or a kiln.gamedata buff clone's GUID) kept on the player: applied for `refreshSeconds` + 5 s...
- `static Buff(string buff, double refreshSeconds = 30) -> TalentEffect` — A buff by name: a buff clone's name ("MyMod_Frost") or "0x\<buff id>", resolved through kiln.gamedata when it is...
- `static Callback(Action<TalentContext> apply, Action<TalentContext>? remove = null) -> TalentEffect` — Code (server, tick thread): `apply` at unlock / join / level change, `remove` before a level change, at lock...
- `static GameNode(uint nodeId) -> TalentEffect` — What a node of the game's own tree does, run for this player (its program with that node's config values, as the...
- `static LevelledProgram(Guid program, string levelConfig, IReadOnlyDictionary<string, object>? configs = null) -> TalentEffect` — `TalentEffect.Program`, with the node's level in a float config: `levelConfig` (a config id "0x...", or a variable's...
- `static Program(Guid program, IReadOnlyDictionary<string, object>? configs = null) -> TalentEffect` — Any Impact program kept running on the player (root = the player) with config values.

## TalentHolder — A character as a tree sees it: the game's character (the save key: `TalentHolder.Character`), the Steam id, and the player entity while in game.
- `TalentHolder()`
- `TalentHolder(ulong SteamId, EntityId Entity)`
- `Character { get; init; } -> CharacterKey` — The character whose unlocks these are; none when only the Steam id is known.
- `Entity { get; init; } -> EntityId`
- `static explicit operator PlayerRef(TalentHolder holder) -> PlayerRef` — This holder as a `PlayerRef`.
- `SteamId { get; init; } -> ulong`

## TalentNode — A node of a `TalentTree`.
- `Column { get; init; } -> int` — Grid position (column, row from the top-left).
- `Cost { get; init; } -> int` — Points per level.
- `Description { get; init; } -> UiText?`
- `Effects { get; init; } -> IReadOnlyList<TalentEffect>`
- `Icon { get; init; } -> string?` — A PNG, relative to the tree's mod folder (client).
- `Id { get; init; } -> string` — Unique in the tree; the save key.
- `LevelRequired { get; init; } -> int` — The character level needed (0 = none).
- `ListEffects { get; init; } -> bool` — The tooltip lists what `TalentNode.Effects` do under the description.
- `MaxLevel { get; init; } -> int` — Levels (1 = a single unlock).
- `Name { get; init; } -> UiText`
- `PointsSpentRequired { get; init; } -> int` — Points spent in this tree before this node can be taken (tiers).
- `RequireAll { get; init; } -> bool`
- `Requires { get; init; } -> IReadOnlyList<string>` — Nodes of the same tree that lead here: any one of them unlocked (all, with `TalentNode.RequireAll`).
- `Row { get; init; } -> int`

## TalentPoints — Where a tree's points come from.
- `static Custom(Func<TalentHolder, int> total) -> TalentPoints` — Code decides the total (server, tick thread); spent points are subtracted.
- `static Own(int start = 0, int perLevel = 1) -> TalentPoints` — The tree's own currency: `start` + `perLevel` x (character level - 1) + what code granted (`Skills.Grant`).
- `static ShareOfGame(double share) -> TalentPoints` — A share of the game's skill points: floor(`share` x the game's skill-point total) + granted.

## TalentPointsInfo — A character's points in one tree.
- `TalentPointsInfo()`
- `TalentPointsInfo(int Total, int Spent, int Available)`
- `Available { get; init; } -> int`
- `Spent { get; init; } -> int`
- `Total { get; init; } -> int`

## TalentRequest — Client to server: `TalentRequest.Op` 0 = state, 1 = unlock, 2 = lock, 3 = respec.
- `TalentRequest(int Op, string Tree, string Node)`
- `Node { get; init; } -> string`
- `Op { get; init; } -> int`
- `Tree { get; init; } -> string`

## TalentResult — The answer of an unlock / lock / respec / grant.
- `TalentResult(bool Ok, string Reason, int Level, TalentPointsInfo Points)`
- `Level { get; init; } -> int`
- `Ok { get; init; } -> bool`
- `Points { get; init; } -> TalentPointsInfo`
- `Reason { get; init; } -> string`

## TalentStateMessage — Server to client: a player's state in one tree.
- `TalentStateMessage(string Tree, int Total, int Spent, int Available, Dictionary<string, int> Levels, bool Ok, string Message, long Revision, int CharacterLevel)`
- `Available { get; init; } -> int`
- `CharacterLevel { get; init; } -> int`
- `Levels { get; init; } -> Dictionary<string, int>`
- `Message { get; init; } -> string`
- `Ok { get; init; } -> bool`
- `Revision { get; init; } -> long`
- `Spent { get; init; } -> int`
- `Total { get; init; } -> int`
- `Tree { get; init; } -> string`

## TalentTree — A Kiln talent tree: nodes on a grid, their links (requirements), costs, levels, icons, texts and effects.
- `AllowRespec { get; init; } -> bool` — The respec rule: free (default) or refused.
- `Description { get; init; } -> UiText?`
- `Directory { get; set; } -> string?` — The mod folder icons are read from (set by `Skills.Register` to the calling mod's folder; icon paths are relative to...
- `Id { get; init; } -> string` — Unique id, "\<mod>:\<name>" (letters, digits, '.', '_', '-', ':').
- `Key { get; init; } -> string?` — A key that opens the tree ("K", "Shift+T"); null = none.
- `MenuEntry { get; init; } -> UiText?` — An entry in the game's pause menu that opens the tree (client).
- `MenuOrder { get; init; } -> int` — Also in the main menu?
- `Name { get; init; } -> UiText`
- `Nodes { get; init; } -> IReadOnlyList<TalentNode>`
- `OnChanged { get; init; } -> Action<TalentChange>?` — Called on the server (tick thread) after a player's tree changed (unlock, lock, respec, grant).
- `Points { get; init; } -> TalentPoints` — Where the points come from (default: 1 at level 1, plus 1 per level).

## TalentTreeState — A character's state in one tree, as stored in carried character data ("talents:\<tree id>").
- `Granted { get; set; } -> int` — Points given by code (`Skills.Grant`).
- `LastGameTotal { get; set; } -> int`
- `LastLevel { get; set; } -> int` — The character level / game skill-point total seen last (for offline reads).
- `Levels { get; set; } -> Dictionary<string, int>` — node id -> level (1..MaxLevel); absent = locked.
- `Revision { get; set; } -> long` — +1 per change.

## TalentUnlockingEvent — A player is about to take a node (all checks passed, nothing changed yet).
- `Cost { get; } -> int`
- `Holder { get; } -> TalentHolder`
- `Level { get; } -> int` — The level the node gets.
- `Node { get; } -> TalentNode`
- `Reason { get; set; } -> string`
- `Tree { get; } -> TalentTree`
