# Skills (Kiln.Skills)

## ClientSkillLink `0.2.22` — One link of the client's skill tree (`ClientSkills.Links`).
- `ClientSkillLink()`
- `ClientSkillLink(uint Id, uint From, uint To, bool Bidirectional)`
- `Bidirectional { get; init; } -> bool` — It works both ways.
- `From { get; init; } -> uint` — The node it leads from.
- `Id { get; init; } -> uint` — The link's id.
- `To { get; init; } -> uint` — The node it leads to.

## ClientSkillNode `0.2.22` — One node of the client's skill tree and the local player's level of it (`ClientSkills.Nodes`).
- `ClientSkillNode()`
- `ClientSkillNode(uint Id, int Index, SkillNodeType Type, int ClassAlignment, int Cost, int MaxLevel, int Level, bool Unlocked, bool Shadow, float UiX, float UiY, int Incoming, int Outgoing)`
- `ClassAlignment { get; init; } -> int` — 0 Warrior, 1 Mage, 2 Ranger, 3 Neutral.
- `Cost { get; init; } -> int` — Skill points (per level for a node with levels).
- `Id { get; init; } -> uint` — The node's id (the game's SkillNodeId, as `SkillNode.Id`).
- `Incoming { get; init; } -> int` — Links into it.
- `Index { get; init; } -> int` — Its tree index.
- `Level { get; init; } -> int` — The local player's level: 0 locked, 1 for a plain node, up to `MaxLevel`.
- `MaxLevel { get; init; } -> int` — 0: one unlock; else levels 1..MaxLevel.
- `Outgoing { get; init; } -> int` — Links out of it.
- `Shadow { get; init; } -> bool` — A shadow node past the game's caps (its state is Kiln's table, as the server sent it).
- `Spent { get; } -> int` — What its levels cost now: `ClientSkillNode.Cost` times `ClientSkillNode.Level`.
- `Type { get; init; } -> SkillNodeType` — What kind of node it is.
- `UiX { get; init; } -> float` — Its x position on the game's skill screen.
- `UiY { get; init; } -> float` — Its y position on the game's skill screen.
- `Unlocked { get; init; } -> bool` — The player has it (`Level` above 0).

## ClientSkillPoints `0.2.22` — The local player's skill points as the client's skill screen has them (`ClientSkills.Points`).
- `ClientSkillPoints()`
- `ClientSkillPoints(EntityId Player, int Total, int Spent, int Available, int UnlockedNodes, int TreeNodes, ulong Step, TimeSpan Age)`
- `Age { get; init; } -> TimeSpan` — Since the copy was taken.
- `Available { get; init; } -> int` — Left to spend: `Total` minus `Spent`, at least 0 (the screen's "available").
- `Player { get; init; } -> EntityId` — The local player.
- `Spent { get; init; } -> int` — Spent on unlocked nodes, shadow nodes included.
- `Step { get; init; } -> ulong` — The client world's step of the copy.
- `Total { get; init; } -> int` — Skill points in all (from the level and the skill-point knowledge, as the server counts them).
- `TreeNodes { get; init; } -> int` — Nodes in the client's tree.
- `UnlockedNodes { get; init; } -> int` — Nodes with a level above 0.

## ClientSkills `0.2.22` — The local player's skill tree as the game client itself holds it, read-only: the tree's layout (every node, its position on the skill screen, its links), the player's...
- `static Available { get; } -> bool` — The view works here now (a game client whose build qualifies the screen data, nothing faulted).
- `const Capability = "skills.client_view" -> string` — The capability row: the client's skill tree can be read here (sigdb feature, since 0.2.0-dev.31.13, mod API 0.2.22).
- `static Find(uint id) -> ClientSkillNode?` — The node with this id in the client's tree, or null.
- `static Links { get; } -> IReadOnlyList<ClientSkillLink>` — Every link of the client's tree (node ids; bidirectional links work both ways).
- `static Nodes { get; } -> IReadOnlyList<ClientSkillNode>` — Every node of the client's tree by tree index, with the local player's level of it: the tree's layout and the...
- `static Points { get; } -> ClientSkillPoints?` — The local player's skill points and counts; null before the first copy (no local player, the tree not loaded, or the...
- `static Unlocked { get; } -> IReadOnlyList<ClientSkillNode>` — The nodes the local player has (level above 0), by tree index.

## GameNodeDefinition — A node for the game's own skill tree (drawn by the game's skill-tree screen, unlocked with the game's skill points, effects run by the game).
- `Class { get; init; } -> string?` — "Warrior", "Mage", "Ranger" or "Neutral" (default: the like node's).
- `Configs { get; init; } -> IReadOnlyDictionary<uint, double>?` — Number configs of the like node to change: config id (0x...) -> value.
- `Cost { get; init; } -> int` — Skill points (per level for nodes with levels).
- `From { get; init; } -> IReadOnlyList<string>` — Nodes that lead to this one ("0x\<id>" of a game node, a node GUID, or a Kiln node's `GameNodeDefinition.Name`).
- `Guid { get; init; } -> Guid` — The node's fixed GUID.
- `Icon { get; init; } -> Guid?` — A UiTexture GUID (e.g. an icon kiln.assets serves, from assets/assets.json) or null.
- `IconFile { get; init; } -> string?` — A PNG for the icon (absolute, or relative to the kiln folder); kiln.assets serves it on clients.
- `Id { get; } -> uint` — This node's id in the game (FNV-1a of the GUID's bytes).
- `Like { get; init; } -> string?` — A node whose effect (program, config values, tooltip values), icon and texts to copy.
- `MaxLevel { get; init; } -> int?` — 0 = one unlock; 1..7 = levels (default: the like node's).
- `Name { get; init; } -> string` — A debug name (also how other Kiln nodes refer to this one).
- `Near { get; init; } -> string?` — A node to place this one near, written like the entries of `GameNodeDefinition.From`; used when...
- `Offset { get; init; } -> (float X, float Y)?` — Where to put the node relative to `GameNodeDefinition.Near`, in the game's tree units, when...
- `Position { get; init; } -> (float X, float Y)?` — Absolute position in the game's tree units (the game's tree spans about 1700..6500); null = near...
- `Program { get; init; } -> Guid?` — An ImpactProgram to run instead of the like node's (tools/impact, a Kiln program).
- `Scales { get; init; } -> IReadOnlyDictionary<uint, double>?` — Scale factors of the like node's scaled configs.
- `Texts { get; init; } -> IReadOnlyDictionary<string, (string Name, string Description, string? PerLevel)>?` — language ("en", "de", "*") -> (name, description, perLevel).
- `To { get; init; } -> IReadOnlyList<string>` — Nodes this one leads to, written like the entries of `GameNodeDefinition.From`.
- `Type { get; init; } -> string?` — "PassiveSkill" (default: the like node's), "Attribute" or "ActiveSkill".
- `UnlocksAbility { get; init; } -> Guid?` — The Ability_* item the node unlocks (its activeSkillUnlockRef).

## GameNodeInfo — A definition as kiln.skills holds it.
- `GameNodeInfo(Guid Guid, uint Id, string Name, string Owner, string Source, string State, int Index, int LevelIndex, string? Error)`
- `Error { get; init; } -> string?` — Why the definition is refused; null when it is not.
- `Guid { get; init; } -> Guid` — The node's GUID.
- `Id { get; init; } -> uint` — The node's id in the game (FNV-1a of the GUID's bytes).
- `Index { get; init; } -> int` — The node's index in the game's tree, which is its bit in the game's unlock masks (256 and up are shadow nodes); -1...
- `LevelIndex { get; init; } -> int` — The slot of the node's level byte among the game's 100; -1 for a node without levels, one that is not in a tree, and...
- `Name { get; init; } -> string` — The node's debug name.
- `Owner { get; init; } -> string` — The id of the mod that defined it; empty when no mod did.
- `Source { get; init; } -> string` — Where the definition came from: `"code"` for `GameTree.Define` and `GameTree.DefineJson`; otherwise how kiln.skills...
- `State { get; init; } -> string` — `"defined"`, `"redefined"` (in a `GameTree.Define` report), `"pending"` (waits for the next world), `"in-tree"`...

## GameNodesRefundedEvent — A player joined with game-tree nodes the tree does not know (a mod that added them is gone): kiln.skills removed them before the game's own check (which would reset...
- `Nodes { get; } -> IReadOnlyList<(uint Id, int Level)>` — The removed node ids and their levels.
- `Player { get; } -> EntityId` — The player (None when Kiln could not tell which player it was).
- `Step { get; } -> ulong` — The server world's step the removal happened in; 0 when unknown.

## GameShadowMessage — Server to client: the local player's shadow levels.
- `GameShadowMessage(Dictionary<string, int> Levels, long Revision)`
- `Levels { get; init; } -> Dictionary<string, int>` — The player's shadow levels: `"0x<node id>"` to level.
- `Revision { get; init; } -> long` — Reserved: the server sends 0 and the client does not read it.

## GameShadowState — A character's levels of shadow nodes (carried character data "gameShadow" of Kiln.Skills' data owner).
- `Levels { get; set; } -> Dictionary<string, int>` — "0x\<node id>" -> level.
- `Revision { get; set; } -> long` — Plus 1 for every change to `GameShadowState.Levels`.

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
- `static Status { get; } -> GameTreeStatus?` — What kiln.skills does with the game's tree right now; null when kiln.skills is not on this build or does not answer.

## GameTreeStatus — What kiln.skills does with the game's own tree right now.
- `GameTreeStatus(bool TreeExpansion, bool LoginGuard, bool GameFunctions, bool NodeTexts, int GameNodes, int GameLinks, int GameLevelMax, int Nodes, int Links, int AddedNodes, int AddedLinks, int FreeNodes, int FreeLevelSlots, int Defined, int Refused, int Pending, int WorldsServed, int Builds, ulong Refunded, string Reason, int ShadowNodes = 0, int ShadowLevel = 0)`
- `AddedLinks { get; init; } -> int` — Links Kiln added to the game's.
- `AddedNodes { get; init; } -> int` — Nodes Kiln added to the game's.
- `Builds { get; init; } -> int` — Resources Kiln has built.
- `Defined { get; init; } -> int` — Definitions known, of all mods.
- `FreeLevelSlots { get; init; } -> int` — Level slots still free of the game's 100 (a node with levels needs one).
- `FreeNodes { get; init; } -> int` — Native node slots still free: the game's cap of 256 nodes minus `Nodes`.
- `GameFunctions { get; init; } -> bool` — The game's own skill functions were found on this build; `GameTree.Dump` and the simulation need them.
- `GameLevelMax { get; init; } -> int` — The highest level slot (`levelIndex`) the game's own nodes use (90 on svn 1287369).
- `GameLinks { get; init; } -> int` — Links in the game's own resource.
- `GameNodes { get; init; } -> int` — Nodes in the game's own resource; 0 until the first world has read it.
- `Links { get; init; } -> int` — Links in the resource Kiln serves now.
- `LoginGuard { get; init; } -> bool` — The login guard is on: at a join, node ids the tree does not know are removed from the player's save instead of...
- `Nodes { get; init; } -> int` — Nodes in the resource Kiln serves now: the game's plus the added ones.
- `NodeTexts { get; init; } -> bool` — Node texts reach the game's text tables (needs kiln.content); without it the like node's texts show.
- `Pending { get; init; } -> int` — Definitions made after every live world read its tree; they go in at the next world.
- `Reason { get; init; } -> string` — Why something is off; empty when everything works.
- `Refunded { get; init; } -> ulong` — Node ids the login guard has removed at joins since the game started (see `GameNodesRefundedEvent`).
- `Refused { get; init; } -> int` — Definitions that are not in the tree: errors, and nodes over the game's caps while the shadow table is off.
- `ShadowLevel { get; init; } -> int` — 0 = the shadow hooks are off, 1 = on (server side), 2 = on, with the client's skill-tree screen hook as well.
- `ShadowNodes { get; init; } -> int` — Nodes past the game's caps in the tree Kiln serves (shadow nodes); 0 when the shadow hooks are off.
- `TreeExpansion { get; init; } -> bool` — The tree-loading hook is on, so nodes Kiln defines can enter the game's tree (sigdb feature `skills.tree_load` and...
- `WorldsServed { get; init; } -> int` — Tree loads Kiln has served.

## Skills — Kiln talent trees (and, in `GameTree`, nodes in the game's own skill tree).
- `static ActiveEffects(EntityId player) -> IReadOnlyList<(string Tree, string Node, int Level, IReadOnlyList<EntityId> Entities)>` — The effects running for a player now (diagnostics): tree/node -> level and effect entities.
- `static Available { get; } -> bool` — Always true: Kiln talent trees run in managed code and need no native service.
- `static EnsureStarted() -> void` — Starts Kiln.Skills' channel, /talents and its tick (Kiln does this at its first service tick).
- `static Find(string id) -> TalentTree?` — The registered tree with this id; null when there is none.
- `static Grant(string tree, EntityId player, int points) -> TalentPointsInfo` — Gives a player in game points of the tree's own: `Skills.Grant` with `Skills.HolderOf`.
- `static Grant(string tree, TalentHolder holder, int points) -> TalentPointsInfo` — Gives points of the tree's own (any points source; negative takes back).
- `static Has(EntityId player, string tree, string node) -> bool` — True when the player has the node (level 1 or more).
- `static HolderOf(CharacterKey character, ulong legacySteamId = 0) -> TalentHolder` — The holder of a character, online or not.
- `static HolderOf(EntityId player) -> TalentHolder` — The holder of a player entity in game: the game's character, the Steam id from kiln.players.
- `static LevelOf(string tree, string node, TalentHolder holder) -> int` — The level of a node for a character (0 = locked).
- `static LoadTrees(string? modDirectory = null) -> IReadOnlyList<TalentTree>` — Loads the "trees" of every skills/*.json in `modDirectory` (default: the calling mod's folder) and registers them as...
- `static Lock(string tree, string node, TalentHolder holder, bool force = false) -> TalentResult` — Gives one level back (the points return).
- `static Points(string tree, TalentHolder holder) -> TalentPointsInfo` — A character's points in a tree: total, spent and available.
- `static Refresh(string tree, TalentHolder holder) -> void` — Applies the player's state now (instead of at the next maintenance pass).
- `static Register(TalentTree tree) -> TalentTree` — Registers a tree as the calling mod's (it goes away when the mod unloads; players' states stay in that mod's player...
- `static Respec(string tree, TalentHolder holder, bool force = false) -> TalentResult` — Gives every point of the tree back (free).
- `static State(string tree, EntityId player) -> TalentTreeState` — A character's state in a tree, for a player in game: `Skills.State` with `Skills.HolderOf`.
- `static State(string tree, TalentHolder holder) -> TalentTreeState` — A character's state in a tree (live object; the player need not be online).
- `static Trees { get; } -> IReadOnlyList<TalentTree>` — Every registered tree.
- `static Unlock(EntityId player, string tree, string node) -> TalentResult` — Takes a node for a player in game: `Skills.Unlock` with `Skills.HolderOf` and no force.
- `static Unlock(string tree, string node, TalentHolder holder, bool force = false) -> TalentResult` — Takes a node (one more level).

## TalentAttributes — The AttributeReference GUIDs the game's attribute nodes use (from its SkillTreeResource, svn 1287369; the names were matched in the lab through each attribute...
- `static readonly Constitution -> Guid` — The game's AttributeReference GUID for Constitution.
- `static readonly Dexterity -> Guid` — The game's AttributeReference GUID for Dexterity.
- `static readonly Endurance -> Guid` — The game's AttributeReference GUID for Endurance.
- `static readonly Intelligence -> Guid` — The game's AttributeReference GUID for Intelligence.
- `static NameOf(Guid g) -> string` — The attribute's name ("Strength", "Constitution", "Intelligence", "Spirit", "Dexterity" or "Endurance") for one of...
- `static readonly Spirit -> Guid` — The game's AttributeReference GUID for Spirit.
- `static readonly Strength -> Guid` — The game's AttributeReference GUID for Strength.

## TalentChange — One change of a player's tree, as `TalentTree.OnChanged` and `TalentChangedEvent` report it.
- `TalentChange(TalentTree Tree, TalentHolder Holder, TalentChangeKind Kind, string? Node, int OldLevel, int NewLevel, int Points)`
- `Holder { get; init; } -> TalentHolder` — The character whose tree changed.
- `Kind { get; init; } -> TalentChangeKind` — What happened.
- `NewLevel { get; init; } -> int` — The node's level after; 0 for a respec and for a grant.
- `Node { get; init; } -> string?` — The id of the node that changed; null for `TalentChangeKind.Respec` and `TalentChangeKind.Grant`.
- `OldLevel { get; init; } -> int` — The node's level before; for a respec, the sum of all levels given back; 0 for a grant.
- `Points { get; init; } -> int` — The character's points still available in the tree after the change.
- `Tree { get; init; } -> TalentTree` — The tree that changed.

## TalentChangedEvent — A player's tree changed (after the effects were applied).
- `Change { get; } -> TalentChange` — What changed.

## TalentChangeKind — What changed in a player's tree.
enum: Unlock, Lock, Respec, Grant, Reset

## TalentContext — Passed to callbacks and effects (server, tick thread).
- `Holder { get; } -> TalentHolder` — The character the effect applies to.
- `Level { get; } -> int` — The level being applied (or the one being removed).
- `Node { get; } -> TalentNode` — The node whose effect runs.
- `Player { get; } -> EntityId` — The player's entity (the holder's `TalentHolder.Entity`).
- `State { get; set; } -> object?` — Anything the effect wants to keep until its remove (per player and node).
- `Tree { get; } -> TalentTree` — The tree the node belongs to.

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
- `Entity { get; init; } -> EntityId` — The player entity while the player is in game; none for an offline holder.
- `static explicit operator PlayerRef(TalentHolder holder) -> PlayerRef` — This holder as a `PlayerRef`.
- `SteamId { get; init; } -> ulong` — The player's Steam id; 0 when it is not known.

## TalentNode — A node of a `TalentTree`.
- `Column { get; init; } -> int` — Grid position (column, row from the top-left).
- `Cost { get; init; } -> int` — Points per level.
- `Description { get; init; } -> UiText?` — A text for the node's tooltip and detail panel.
- `Effects { get; init; } -> IReadOnlyList<TalentEffect>` — What the node does while it is unlocked.
- `Icon { get; init; } -> string?` — A PNG, relative to the tree's mod folder (client).
- `Id { get; init; } -> string` — Unique in the tree; the save key.
- `LevelRequired { get; init; } -> int` — The character level needed (0 = none).
- `ListEffects { get; init; } -> bool` — The tooltip lists what `TalentNode.Effects` do under the description.
- `MaxLevel { get; init; } -> int` — Levels (1 = a single unlock).
- `Name { get; init; } -> UiText` — The node's name: on its button, in its tooltip and in the detail panel of the tree's screen.
- `PointsSpentRequired { get; init; } -> int` — Points spent in this tree before this node can be taken (tiers).
- `RequireAll { get; init; } -> bool` — True: every node in `TalentNode.Requires` must be unlocked.
- `Requires { get; init; } -> IReadOnlyList<string>` — Nodes of the same tree that lead here: any one of them unlocked (all, with `TalentNode.RequireAll`).
- `Row { get; init; } -> int` — Grid row, counted from the top (0 to 31).

## TalentPoints — Where a tree's points come from.
- `static Custom(Func<TalentHolder, int> total) -> TalentPoints` — Code decides the total (server, tick thread); spent points are subtracted.
- `static Own(int start = 0, int perLevel = 1) -> TalentPoints` — The tree's own currency: `start` + `perLevel` x (character level - 1) + what code granted (`Skills.Grant`).
- `static ShareOfGame(double share) -> TalentPoints` — A share of the game's skill points: floor(`share` x the game's skill-point total) + granted.

## TalentPointsInfo — A character's points in one tree.
- `TalentPointsInfo()`
- `TalentPointsInfo(int Total, int Spent, int Available)`
- `Available { get; init; } -> int` — `Total` minus `Spent`.
- `Spent { get; init; } -> int` — Points in unlocked levels: each node's cost times its level.
- `Total { get; init; } -> int` — All the points the tree gives the character: its points source plus what code granted.

## TalentRequest — Client to server: `TalentRequest.Op` 0 = state, 1 = unlock, 2 = lock, 3 = respec.
- `TalentRequest(int Op, string Tree, string Node)`
- `Node { get; init; } -> string` — The id of the node for an unlock or lock; not read for the other operations.
- `Op { get; init; } -> int` — 0 = state, 1 = unlock, 2 = lock, 3 = respec.
- `Tree { get; init; } -> string` — The id of the tree.

## TalentResult — The answer of an unlock / lock / respec / grant.
- `TalentResult(bool Ok, string Reason, int Level, TalentPointsInfo Points)`
- `Level { get; init; } -> int` — The node's level after the call (a refusal: the level it stayed at); 0 for a respec and for an unknown node.
- `Ok { get; init; } -> bool` — True when the action happened.
- `Points { get; init; } -> TalentPointsInfo` — The character's points in the tree after the call.
- `Reason { get; init; } -> string` — Why it was refused; empty when `Ok`.

## TalentStateMessage — Server to client: a player's state in one tree.
- `TalentStateMessage(string Tree, int Total, int Spent, int Available, Dictionary<string, int> Levels, bool Ok, string Message, long Revision, int CharacterLevel)`
- `Available { get; init; } -> int` — Points still free (`TalentPointsInfo.Available`).
- `CharacterLevel { get; init; } -> int` — The character level the server saw last (`TalentTreeState.LastLevel`).
- `Levels { get; init; } -> Dictionary<string, int>` — Node id to level; a locked node is absent.
- `Message { get; init; } -> string` — A text for the player: the refusal or error; empty when there is none.
- `Ok { get; init; } -> bool` — False when the request was refused or failed; `Message` says why, and a Kiln client shows it as a toast.
- `Revision { get; init; } -> long` — The count of changes to the character's state in this tree (`TalentTreeState.Revision`: plus 1 per change).
- `Spent { get; init; } -> int` — Points in unlocked levels (`TalentPointsInfo.Spent`).
- `Total { get; init; } -> int` — All the points the tree gives the character (`TalentPointsInfo.Total`).
- `Tree { get; init; } -> string` — The id of the tree.

## TalentTree — A Kiln talent tree: nodes on a grid, their links (requirements), costs, levels, icons, texts and effects.
- `AllowRespec { get; init; } -> bool` — The respec rule: free (default) or refused.
- `Description { get; init; } -> UiText?` — A text under the title on the tree's screen.
- `Directory { get; set; } -> string?` — The mod folder icons are read from (set by `Skills.Register` to the calling mod's folder; icon paths are relative to...
- `Id { get; init; } -> string` — Unique id, "\<mod>:\<name>" (letters, digits, '.', '_', '-', ':').
- `Key { get; init; } -> string?` — A key that opens the tree ("K", "Shift+T"); null = none.
- `MenuEntry { get; init; } -> UiText?` — An entry in the game's pause menu that opens the tree (client).
- `MenuOrder { get; init; } -> int` — Where `TalentTree.MenuEntry` sits among the pause menu's entries: lower comes first (the `order` of `Ui.Menus.Add`).
- `Name { get; init; } -> UiText` — The tree's name: the title of its screen, and the tree's name in its key's description on the Keys page.
- `Nodes { get; init; } -> IReadOnlyList<TalentNode>` — The tree's nodes.
- `OnChanged { get; init; } -> Action<TalentChange>?` — Called on the server (tick thread) after a player's tree changed (unlock, lock, respec, grant).
- `Points { get; init; } -> TalentPoints` — Where the points come from (default: 1 at level 1, plus 1 per level).

## TalentTreeState — A character's state in one tree, as stored in carried character data ("talents:\<tree id>").
- `Granted { get; set; } -> int` — Points given by code (`Skills.Grant`).
- `LastGameTotal { get; set; } -> int` — The game's skill-point total seen last (the base of a `TalentPoints.ShareOfGame` tree, for offline reads).
- `LastLevel { get; set; } -> int` — The character level / game skill-point total seen last (for offline reads).
- `Levels { get; set; } -> Dictionary<string, int>` — node id -> level (1..MaxLevel); absent = locked.
- `Revision { get; set; } -> long` — +1 per change.

## TalentUnlockingEvent — A player is about to take a node (all checks passed, nothing changed yet).
- `Cost { get; } -> int` — The points the unlock costs (the node's `TalentNode.Cost`).
- `Holder { get; } -> TalentHolder` — The character taking it.
- `Level { get; } -> int` — The level the node gets.
- `Node { get; } -> TalentNode` — The node being taken.
- `Reason { get; set; } -> string` — The text the refusal carries when a handler cancels the event.
- `Tree { get; } -> TalentTree` — The tree.
