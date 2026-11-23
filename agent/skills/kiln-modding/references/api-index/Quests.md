# Quests (Kiln.Quests)

## Boss — A boss placed in the world: a creature the game gives a boss health bar, placed directly or by a spawner.
- `Boss(string Id, string Template, string? Spawner, uint NameTag, WorldPosition Position, ProgressFact? Kill)`
- `Id { get; init; } -> string` — The boss template's name, lower-case, with `_n` when the world has several of it.
- `Kill { get; init; } -> ProgressFact?`
- `Killed { get; } -> bool?` — Killed in this world (tick thread); null when the game keeps no record or it is a character fact.
- `KilledBy(Player player) -> bool?`
- `NameTag { get; init; } -> uint` — Loca tag of the health bar's name.
- `Position { get; init; } -> WorldPosition`
- `Spawner { get; init; } -> string?` — The placed spawner's template, when a spawner places it.
- `Template { get; init; } -> string`
- `Tracked { get; } -> bool` — The game records this boss's kill.

## BossKilledEvent — A boss died: a creature the game gives a boss health bar.
- `Boss { get; } -> Boss?` — The placed boss it was (the nearest placement of its template), when known.
- `Entity { get; } -> EntityId` — The dead entity; None when only the kill record was seen.
- `Killer { get; } -> EntityId` — Who dealt the killing blow (a player or a creature), when Kiln saw it.
- `KillerPlayer { get; } -> Player?`
- `Position { get; } -> WorldPosition?`
- `Recorded { get; } -> bool` — The game's kill record (knowledge) changed for this boss with the kill.
- `Step { get; } -> ulong`
- `Template { get; } -> string?` — The boss's template name.

## CharacterProgress — One character's progress, read with the game's evaluator for that player (personal progression included).
- `BossesKilled { get; } -> IReadOnlyList<Boss>`
- `ObelisksRead { get; } -> IReadOnlyList<Obelisk>`
- `Player { get; } -> EntityId`
- `Quests { get; } -> QuestLog` — The character's quests, vanilla and Kiln (`Quests.Of`).
- `RootsDestroyed { get; } -> IReadOnlyList<ShroudRoot>`
- `ShrinesFound { get; } -> IReadOnlyList<FlameShrine>`
- `SpiresUnlocked { get; } -> IReadOnlyList<Spire>`
- `SpireTopsReached { get; } -> IReadOnlyList<Spire>`

## ChestLootedEvent — A loaded chest was emptied after its loot was generated.
- `Chest { get; } -> ChestStatus`
- `NearestPlayer { get; } -> EntityId`

## ChestStatus — A loot chest placed in the world and its state.
- `ChestStatus(uint LootId, string Template, WorldPosition Position, EntityId Entity, bool? Opened, bool? Looted)`
- `Entity { get; init; } -> EntityId`
- `Loaded { get; } -> bool`
- `Looted { get; init; } -> bool?` — Opened and emptied.
- `LootId { get; init; } -> uint`
- `Opened { get; init; } -> bool?` — The chest's loot was generated (it was opened at least once).
- `Position { get; init; } -> WorldPosition`
- `Template { get; init; } -> string`

## ConversationOptionEvent — A player chose an option of a Kiln conversation.
- `Dialogue { get; } -> DialogueDefinition`
- `Holder { get; } -> QuestHolder`
- `Node { get; } -> string`
- `Npc { get; } -> EntityId`
- `Option { get; } -> int` — The option's place in the node's list.
- `RefuseMessage { get; set; } -> string?`

## ConversationResult — The answer to a conversation request.
- `ConversationResult(bool Ok, string Reason)`
- `Ok { get; init; } -> bool`
- `Reason { get; init; } -> string`

## ConversationView — What a holder is being shown (tick thread; for tools and tests).
- `ConversationView(long Id, string Dialogue, string Node, UiText? Speaker, UiText Text, IReadOnlyList<(int Index, UiText Text)> Options, EntityId Npc)`
- `Dialogue { get; init; } -> string`
- `Id { get; init; } -> long`
- `Node { get; init; } -> string`
- `Npc { get; init; } -> EntityId`
- `Options { get; init; } -> IReadOnlyList<(int Index, UiText Text)>`
- `Speaker { get; init; } -> UiText?`
- `Text { get; init; } -> UiText`

## DialogueAction
enum: Start, Stop

## DialogueDefinition — A conversation: nodes of text and options, per NPC or per quest.
- `Farewell { get; set; } -> List<UiText>` — The game's menu: replaces the NPC's farewell lines.
- `FullId { get; } -> string`
- `Greet { get; set; } -> bool?` — The game's menu: the start node's text is a greeting.
- `Greetings { get; set; } -> List<DialogueGreeting>` — The game's menu: more greetings, shown before the plain one while their condition holds.
- `Hint { get; set; } -> string?` — The game's menu: the marker over the NPC's head on the greeting (Hint, Attention, MainQuest).
- `Id { get; init; } -> string`
- `ModId { get; } -> string`
- `Native { get; set; } -> bool` — The default: its lines are in the game's own NPC menu, with the game's voice playback.
- `Nodes { get; set; } -> Dictionary<string, DialogueNode>`
- `Start { get; set; } -> string`
- `Transition { get; set; } -> List<UiText>` — The game's menu: replaces the line between topics.

## DialogueGreeting — A greeting that wins over the plain one while its condition holds.
- `Hint { get; set; } -> string?`
- `If { get; set; } -> QuestCondition?`
- `Text { get; set; } -> UiText`
- `Voice { get; set; } -> string?`

## DialogueKnowledgeEvent — The knowledge a dialogue line is about to run for a player (its knowledgeUnlockOperations and its "heard" knowledge), held by Kiln for a decision.
- `CheckError { get; } -> bool` — A check could not run (counted as passed).
- `ChecksOn { get; } -> bool` — The server setting checkDialogue is on.
- `DefaultApplies { get; } -> bool`
- `Distance { get; } -> double?` — Metres between player and NPC, null when unknown.
- `LineId { get; } -> uint`
- `Npc { get; } -> EntityId`
- `Operations { get; } -> IReadOnlyList<KnowledgeOperation>` — What would run: the line's operations, then {line id, Bool, 1} ("heard").
- `Player { get; } -> Player?`
- `PlayerEntity { get; } -> EntityId`
- `RequirementFailed { get; } -> bool` — The line's requirement is false for this player on the server (a vanilla client never offers such a line).
- `Step { get; } -> ulong`
- `TooFar { get; } -> bool` — The player stands farther from the NPC than the server's dialogueRange.

## DialogueLineEvent — A player picked (or stopped) a dialogue line of an NPC: the game's ServerSelectDialogOptionEvent, read by Kiln's program kiln_knowledge.
- `Action { get; } -> DialogueAction`
- `IsFarewell { get; } -> bool`
- `IsMissingRequirement { get; } -> bool`
- `LineId { get; } -> uint`
- `Npc { get; } -> EntityId` — The NPC's entity.
- `Player { get; } -> Player?`
- `PlayerEntity { get; } -> EntityId`
- `Step { get; } -> ulong`

## DialogueNode
- `Do { get; set; } -> List<QuestAction>` — Actions when the node opens.
- `Goto { get; set; } -> string?` — A node without options: where "Continue" goes (default: the conversation ends).
- `Hint { get; set; } -> string?` — The game's menu: the marker over the NPC's head on this node's greeting.
- `Options { get; set; } -> List<DialogueOption>`
- `Speaker { get; set; } -> UiText?` — Overrides the NPC's name for this node.
- `Text { get; set; } -> UiText`
- `Voice { get; set; } -> string?` — A voice-over line, played by the game's dialogue: a name in the mod's cooked audio, or a GUID (any line of the game).

## DialogueOption
- `Close { get; set; } -> bool` — The game's menu: the NPC plays its outro after the reply.
- `Do { get; set; } -> List<QuestAction>`
- `Goto { get; set; } -> string?` — The next node; "end" or null ends the conversation.
- `Hint { get; set; } -> string?` — The game's menu: the topic's marker (Hint, Attention, MainQuest).
- `If { get; set; } -> QuestCondition?` — Shown only while this holds; checked again when chosen.
- `Once { get; set; } -> bool` — The game's menu: the topic goes away once heard.
- `Reply { get; set; } -> UiText?` — The game's menu: what the NPC answers.
- `Text { get; set; } -> UiText`
- `Voice { get; set; } -> string?` — The game's menu: the reply's voice-over.

## Dialogues — Dialogue graphs: lines in the game's own NPC menu, or chat conversations for mods vanilla clients join.
- `static ApplyOption(EntityId player, EntityId npc, string fullDialogueId, string nodeId, int optionIndex) -> Task<ConversationResult>` — The game's own dialogue chose an option that a mod's quest file defines (Kiln.Quests/Native maps the line to the...
- `static Choose(QuestHolder h, long conversation, int option) -> Task<ConversationResult>` — Runs a choice: `option` is the option's place in the node (-1: continue a node without options).
- `static Close(QuestHolder h) -> void` — Ends a holder's conversation.
- `static Define(DialogueDefinition dialogue) -> DialogueDefinition` — Registers a dialogue as the calling mod's (it goes away when the mod unloads).
- `static Definitions { get; } -> IReadOnlyList<DialogueDefinition>`
- `static Find(string id) -> DialogueDefinition?` — A dialogue by full id, or by its own id when only one mod has it.
- `static Open(EntityId player, string dialogue, EntityId npc = default(EntityId), string? node = null) -> ConversationResult` — Opens a dialogue for a player in game (at `node`, default its start).
- `static Talk(QuestHolder h, string? npc = null) -> ConversationResult` — A player talks to the NPC near them (or, for a synthetic holder, the one named): a conversation, or the quests that...
- `static View(QuestHolder h) -> ConversationView?` — What a holder is being shown; null when no conversation is open.

## FlameChange — The outcome of `WorldProgress.SetFlameLevel`.
- `FlameChange(ulong Request, bool Ok, string Status, int Requested, int Before, int After, int Max, bool Clamped, bool Marked, bool Mirrored, int Steps, string Reason)`
- `After { get; init; } -> int`
- `Before { get; init; } -> int` — The stored level before and after the write (after: once the game's attribute formula ran).
- `Clamped { get; init; } -> bool` — The game's formula limited the value.
- `Marked { get; init; } -> bool` — The component was marked written (so the game saves and replicates it).
- `Max { get; init; } -> int` — The game's maximum level (0 when unknown).
- `Mirrored { get; init; } -> bool` — `NetworkServerProgress.level` followed (what players see).
- `Ok { get; init; } -> bool` — The requested write completed (see `FlameChange.Mirrored`).
- `Reason { get; init; } -> string`
- `Request { get; init; } -> ulong` — The native request id (0 when refused at once).
- `Requested { get; init; } -> int` — What was asked for.
- `Status { get; init; } -> string` — "ok", "unavailable", "internal", ...
- `Steps { get; init; } -> int` — Steps from the write until it was mirrored.

## FlameLevelChangedEvent — The world's flame level changed (the Flame Altar's upgrade, or an admin's set-flame-level).
- `Flame { get; } -> FlameStatus`
- `NewLevel { get; } -> int`
- `OldLevel { get; } -> int`

## FlameLimits — What the game derives from a flame level (the balancing table's per-level lists and the fog rule).
- `FlameLimits(int Level, int Altars, int WaterSources, int FogResistance, int? FogRemovalLevel, bool? FogRemoved)`
- `Altars { get; init; } -> int` — How many Flame Altars the world may have (`altarsPerFlameLevel`).
- `FogRemovalLevel { get; init; } -> int?` — The server setting `flameLevelFogRemoval` (null when kiln.settings is not ready).
- `FogRemoved { get; init; } -> bool?` — The level reaches `FlameLimits.FogRemovalLevel`.
- `FogResistance { get; init; } -> int` — The Shroud resistance of a player: `playerBaseFogResistance + playerFogResistancePerFlameLevel * level`.
- `Level { get; init; } -> int` — The flame level asked about.
- `WaterSources { get; init; } -> int` — How many water sources it may have (`waterSourcesPerFlameLevel`).

## FlameShrine — A Flame Shrine or Flame Sanctum.
- `FlameShrine(string Id, ShrineKind Kind, string Region, string? Template, uint NameTag, WorldPosition? Position, ProgressFact Discovery)`
- `Discovery { get; init; } -> ProgressFact`
- `FoundBy(Player player) -> bool?`
- `Id { get; init; } -> string` — Lower-case, from the game's achievement query (`grassland_flameshrine_01`).
- `Kind { get; init; } -> ShrineKind`
- `NameTag { get; init; } -> uint`
- `Position { get; init; } -> WorldPosition?`
- `Region { get; init; } -> string`
- `Template { get; init; } -> string?`

## FlameStatus — The world's flame: the Mother Flame's level and the flame altars (the game's NetworkServerProgress on the world's progress entity, the level the Flame Altar menu shows).
- `FlameStatus(int Level, int Altars, int HighestAltarLevel, int WaterSources, EntityId Entity)`
- `Altars { get; init; } -> int`
- `Entity { get; init; } -> EntityId`
- `HighestAltarLevel { get; init; } -> int`
- `Level { get; init; } -> int`
- `Max { get; init; } -> int?` — The game's maximum flame level (the second value of the `Level` attribute, `flameLevelMax`).
- `Stored { get; init; } -> int?` — The `Level` attribute of the progress entity: the store the game writes when the flame levels up (and its admin...
- `SystemLevel { get; init; } -> int?` — What the game's own altar, water-source and fog rules read (`ServerProgressSystem.progressLevel`).
- `WaterSources { get; init; } -> int`

## Journal — The game's journal of vanilla quests (server side; the client builds its quest log from its own copy and the replicated knowledge, so granting a step's visibility...
- `static Find(string name) -> JournalQuest?` — A quest by "quest:0x...", "0x..." or its entry id; null when there is none.
- `static Find(uint entryId) -> JournalQuest?` — A quest by "quest:0x...", "0x..." or its entry id; null when there is none.
- `static Quests() -> IReadOnlyList<JournalQuest>` — Every journal quest (tick thread; empty until the catalog exists, a few steps after the world started).
- `static State(EntityId target, JournalQuest quest) -> JournalQuestState` — The quest's state for the world (`target` 0) or a player (tick thread): every step's visibility and completion query...
- `static State(Player player, JournalQuest quest) -> JournalQuestState` — The quest's state for the world (`target` 0) or a player (tick thread): every step's visibility and completion query...
- `static States(EntityId target) -> IReadOnlyList<JournalQuestState>` — Every quest's state for a target (tick thread).

## JournalQuest — A journal quest (JournalQuestResource).
- `JournalQuest(int Index, uint EntryId, uint NameTag, QuestSource Source, QuestType Type, bool IsMain, bool IsTutorial, bool ForAllPlayers, int Priority, IReadOnlyList<JournalStep> Steps, int Rewards, IReadOnlyList<uint> RewardExperience, IReadOnlyList<uint> RewardItems)`
- `EntryId { get; init; } -> uint`
- `ForAllPlayers { get; init; } -> bool`
- `Index { get; init; } -> int`
- `IsMain { get; init; } -> bool`
- `IsTutorial { get; init; } -> bool`
- `Name { get; } -> string` — "quest:0x\<entryId>".
- `NameTag { get; init; } -> uint`
- `Priority { get; init; } -> int`
- `RewardExperience { get; init; } -> IReadOnlyList<uint>`
- `RewardItems { get; init; } -> IReadOnlyList<uint>`
- `Rewards { get; init; } -> int`
- `Source { get; init; } -> QuestSource`
- `Steps { get; init; } -> IReadOnlyList<JournalStep>`
- `Type { get; init; } -> QuestType`

## JournalQuestState — A quest's state for the world (`JournalQuestState.Target` 0: shared progress, as the world's journal entries are driven) or one player.
- `JournalQuestState(JournalQuest Quest, EntityId Target, IReadOnlyList<JournalStepState> Steps, bool? Rewarded)`
- `Completed { get; } -> bool?` — The last step's completion query holds (the game then pays the reward).
- `Quest { get; init; } -> JournalQuest`
- `Rewarded { get; init; } -> bool?` — The player's "rewarded" knowledge (the game paid the quest's XP); null for the world.
- `Started { get; } -> bool` — At least one step is shown in the journal.
- `Steps { get; init; } -> IReadOnlyList<JournalStepState>`
- `Target { get; init; } -> EntityId`
- `VisibleSteps { get; } -> int`

## JournalStep — One step of a journal quest (JournalEntryResource): shown while `JournalStep.Visible` holds, done when `JournalStep.Done` holds (in all but one vanilla quest every...
- `JournalStep(int Index, uint EntryId, uint NameTag, uint TextTag, uint MapMarkerTag, KnowledgeQuery Visible, KnowledgeQuery Done, int ProgressSteps, int RecommendedLevel)`
- `Done { get; init; } -> KnowledgeQuery`
- `EntryId { get; init; } -> uint`
- `Index { get; init; } -> int`
- `MapMarkerTag { get; init; } -> uint`
- `NameOf(JournalQuest quest) -> string` — "quest:0x\<quest>.step\<n>" (add ".visible" / ".done" for the requirement names).
- `NameTag { get; init; } -> uint`
- `ProgressSteps { get; init; } -> int`
- `RecommendedLevel { get; init; } -> int`
- `TextTag { get; init; } -> uint`
- `Visible { get; init; } -> KnowledgeQuery`

## JournalStepState — A step's state; null where it could not be evaluated (a player query asked for the world).
- `JournalStepState(JournalStep Step, bool? Visible, bool? Done)`
- `Done { get; init; } -> bool?`
- `Step { get; init; } -> JournalStep`
- `Visible { get; init; } -> bool?`

## Knowledge — The game's knowledge: every unlock, quest step, line heard and NPC rescued (kiln.knowledge).
- `static Available { get; } -> bool` — True when kiln.knowledge is loaded (see `Knowledge.Status` for its parts).
- `static Catalog(KnowledgeCatalog catalog) -> IReadOnlyList<KnowledgeName>` — The catalog's names (NPC unlocks, journal quests, named queries).
- `static FindNpc(string name) -> KnowledgeNpc?` — The NPC with this debug name ("NPC_Workshop_Bard01") or "npc:" name; null when there is none.
- `static For(EntityId player) -> KnowledgeScope` — One player's knowledge (while they are on the server).
- `static For(Player player) -> KnowledgeScope` — One player's knowledge (while they are on the server).
- `static Get(EntityId target, uint id) -> KnowledgeState?` — An id in a target's map (tick thread).
- `static Get(Player player, string name) -> KnowledgeState?` — An id in a target's map (tick thread).
- `static Grant(EntityId player, string name, uint value = 1) -> Task<KnowledgeResult>`
- `static Grant(Player player, string name, uint value = 1) -> Task<KnowledgeResult>`
- `static GrantUnchecked(EntityId target, uint id) -> Task<KnowledgeResult>` — Queues a change (any thread).
- `static Has(EntityId target, string name) -> bool` — True when the name's requirement holds for the target (see `KnowledgeScope.Has`).
- `static Has(Player player, string name) -> bool` — True when the name's requirement holds for the target (see `KnowledgeScope.Has`).
- `static Info(uint id) -> KnowledgeInfo?` — What GameKnowledgeResource says about an id; null when the game does not know it.
- `static List(EntityId target) -> IReadOnlyList<KnowledgeState>` — Every set id of a target (tick thread).
- `static NameOf(uint id, bool query = false) -> string?` — The catalog name of a knowledge id ("npc:....unlock", "quest:0x...") or of a query id ("query:..."); null when it...
- `static Npcs { get; } -> IReadOnlyList<KnowledgeNpc>` — The game's NPC roster with the registry's unlock state (tick thread; empty before the catalog exists).
- `static Query(EntityId target, KnowledgeQuery query, KnowledgeMode mode = KnowledgeMode.Player) -> bool` — Evaluates a requirement with the game's evaluator (named queries, NPC / inventory / flame runtime queries included)...
- `static Resolve(string name) -> KnowledgeName` — Resolves a name: "0x1234abcd" / decimal, "npc:\<debugName>[.unlock]", "quest:0x\<entryId>" (the quest's "rewarded"...
- `static Revoke(EntityId player, string name, int flag = -1) -> Task<KnowledgeResult>`
- `static Revoke(Player player, string name, int flag = -1) -> Task<KnowledgeResult>`
- `static Status { get; } -> KnowledgeStatus?`
- `static Submit(KnowledgeOp op, EntityId target, uint id, uint value) -> Task<KnowledgeResult>`
- `static TryParseId(string s, out uint id) -> bool` — Parses a raw id: "0x1234abcd" or decimal.
- `static TryQuery(EntityId target, KnowledgeQuery query, KnowledgeMode mode = KnowledgeMode.Player) -> bool?` — Evaluates a requirement; null when it cannot be (no player for a player query, unknown query, part off).
- `static TryResolve(string name, out KnowledgeName result) -> bool`
- `static World { get; } -> KnowledgeScope` — The world's (shared) knowledge.

## Knowledge.Security — The opt-in server checks (kiln/config/knowledge.json; both off by default).
- `static Allow(IEnumerable<uint> ids) -> void` — Ids that pass the client veto at run time (in addition to the file's allowClientKnowledge).
- `static Apply(KnowledgeSecurity settings, bool save = false) -> void` — Applies settings now (until kiln/config/knowledge.json changes).
- `static Current { get; } -> KnowledgeSecurity` — The settings in use.

## KnowledgeCatalog — Catalogs of `Knowledge.Catalog`.
enum: Npcs, Quests, Queries

## KnowledgeChangedEvent — A knowledge value changed in the world map or a player's map: the game (a quest step, a dialogue line, a rescue, loot, a query trigger, ...) or a Kiln change...
- `ByKiln { get; } -> bool`
- `Gained { get; } -> bool`
- `Id { get; } -> uint`
- `IsWorld { get; } -> bool`
- `Lost { get; } -> bool`
- `Name { get; } -> string?` — The catalog name ("npc:....unlock", "quest:0x..."), or null.
- `NewValue { get; } -> uint`
- `OldValue { get; } -> uint`
- `Player { get; } -> Player?` — The player (null for the world, or if they left in the meantime).
- `Request { get; } -> ulong` — The Kiln request that caused it (`KnowledgeResult.Request`), 0 = the game.
- `Step { get; } -> ulong`
- `Target { get; } -> EntityId` — 0 = the world map, else the player whose map changed.
- `Type { get; } -> KnowledgeType`

## KnowledgeCompare
enum: GreaterThan, LessThan, Equals

## KnowledgeInfo — What GameKnowledgeResource says about one id (`Knowledge.Info`).
- `KnowledgeInfo()`
- `KnowledgeInfo(uint Id, KnowledgeObjectFlags Flags, KnowledgeType WorldType, KnowledgeType PlayerType, uint WorldFlags, uint PlayerFlags, uint ExperienceId, int SkillPoints)`
- `ExperienceId { get; init; } -> uint`
- `Flags { get; init; } -> KnowledgeObjectFlags`
- `Id { get; init; } -> uint`
- `IsPlayer { get; } -> bool`
- `IsWorld { get; } -> bool`
- `Known { get; } -> bool`
- `PlayerFlags { get; init; } -> uint`
- `PlayerType { get; init; } -> KnowledgeType`
- `SkillPoints { get; init; } -> int`
- `WorldFlags { get; init; } -> uint`
- `WorldType { get; init; } -> KnowledgeType`

## KnowledgeMode — How a player's requirement reads knowledge that has a world object and a player twin.
enum: Shared, Player, Personal

## KnowledgeName — What a name means (`Knowledge.Resolve`).
- `KnowledgeName(string Text, bool IsQuery, uint Id, KnowledgeQuery Requirement)`
- `Id { get; init; } -> uint`
- `IsQuery { get; init; } -> bool` — The name is a named query (evaluate with `Knowledge.Query`; it cannot be granted).
- `Requirement { get; init; } -> KnowledgeQuery` — The full requirement behind the name: an NPC's unlock requirement, a quest step's query; a plain knowledge name: "is...
- `Text { get; init; } -> string`

## KnowledgeNpc — An NPC of the game's roster (NpcCollection) with the NPC registry's unlock state.
- `KnowledgeNpc(int Index, string DebugName, uint NpcId, uint NameTag, uint Workshop, uint SummonItem, KnowledgeQuery Unlock, bool? Unlocked, bool CraftingAssistant)`
- `CraftingAssistant { get; init; } -> bool`
- `DebugName { get; init; } -> string`
- `Index { get; init; } -> int`
- `Name { get; } -> string` — "npc:\<debugName>".
- `NameTag { get; init; } -> uint`
- `NpcId { get; init; } -> uint`
- `SummonItem { get; init; } -> uint`
- `Unlock { get; init; } -> KnowledgeQuery`
- `Unlocked { get; init; } -> bool?`
- `Workshop { get; init; } -> uint`

## KnowledgeObjectFlags — What GameKnowledgeResource says about an id.
enum: None, World, Player, Twin, Analytics, Experience, SkillPoints

## KnowledgeOp
enum: Grant, Revoke, Set

## KnowledgeOperation — One knowledge operation (the game's KnowledgeUnlockOperation) and the player it is for.
- `KnowledgeOperation()`
- `KnowledgeOperation(uint Id, KnowledgeType Type, uint Value, EntityId Player)`
- `Id { get; init; } -> uint`
- `Player { get; init; } -> EntityId`
- `Type { get; init; } -> KnowledgeType`
- `Value { get; init; } -> uint`

## KnowledgeQuery — A requirement exactly as the game stores it (KnowledgeQueryData): a knowledge object compared to a value, or a named query (`KnowledgeQueryType.Extern`).
- `KnowledgeQuery()`
- `KnowledgeQuery(uint Id, KnowledgeQueryType Type, KnowledgeCompare Compare, uint CompareValue, bool PlayerOnly = false)`
- `Compare { get; init; } -> KnowledgeCompare`
- `CompareValue { get; init; } -> uint`
- `Deconstruct(out uint Id, out KnowledgeQueryType Type, out KnowledgeCompare Compare, out uint CompareValue, out bool PlayerOnly) -> void`
- `Equals(KnowledgeQuery other) -> bool`
- `Id { get; init; } -> uint`
- `IsEmpty { get; } -> bool`
- `static IsSet(uint id) -> KnowledgeQuery` — "this Bool knowledge is set".
- `static operator !=(KnowledgeQuery left, KnowledgeQuery right) -> bool`
- `static operator ==(KnowledgeQuery left, KnowledgeQuery right) -> bool`
- `PlayerOnly { get; init; } -> bool`
- `Type { get; init; } -> KnowledgeQueryType`

## KnowledgeQueryType — The kind of a requirement (the game's KnowledgeQueryDataType).
enum: Extern, SimpleBool, SimpleCount, SimpleFlag

## KnowledgeRequestEvent — A client asked the server for knowledge itself (the player input TriggerKnowledgeAction; the vanilla client does this for some tutorial and UI knowledge).
- `Held { get; } -> bool` — The veto holds it: the decision of the handlers counts.
- `Id { get; } -> uint`
- `Name { get; } -> string?`
- `Player { get; } -> Player?`
- `PlayerEntity { get; } -> EntityId`
- `Step { get; } -> ulong`
- `Type { get; } -> KnowledgeType`
- `Value { get; } -> uint`

## KnowledgeResult — What a change did.
- `KnowledgeResult(ulong Request, KnowledgeOp Op, EntityId Target, uint Id, bool Ok, string Status, uint Before, uint After, bool Changed, int Steps, string Reason)`
- `After { get; init; } -> uint`
- `Before { get; init; } -> uint`
- `Changed { get; init; } -> bool`
- `Id { get; init; } -> uint`
- `Ok { get; init; } -> bool`
- `Op { get; init; } -> KnowledgeOp`
- `Reason { get; init; } -> string`
- `Request { get; init; } -> ulong`
- `Status { get; init; } -> string`
- `Steps { get; init; } -> int`
- `Target { get; init; } -> EntityId`

## KnowledgeScope — The knowledge of one target: the world (`Knowledge.World`) or one player (`Knowledge.For`).
- `Get(string name) -> KnowledgeState?` — The id's state (tick thread); null when the id is not an object of this kind of target.
- `Get(uint id) -> KnowledgeState?` — The id's state (tick thread); null when the id is not an object of this kind of target.
- `Grant(string name, uint value = 1) -> Task<KnowledgeResult>` — Grants through the game's own knowledge apply (XP, triggers, replication: the game's).
- `Grant(uint id, uint value = 1) -> Task<KnowledgeResult>` — Grants through the game's own knowledge apply (XP, triggers, replication: the game's).
- `Has(string name) -> bool` — True when the id is set; for a name with a requirement (an NPC's unlock, a quest step, a named query) the...
- `Has(uint id) -> bool` — True when the id is set; for a name with a requirement (an NPC's unlock, a quest step, a named query) the...
- `IsWorld { get; } -> bool`
- `List() -> IReadOnlyList<KnowledgeState>` — Every set id of this target (tick thread).
- `Revoke(string name, int flag = -1) -> Task<KnowledgeResult>` — Clears the id (Flags: one flag, or all with -1).
- `Revoke(uint id, int flag = -1) -> Task<KnowledgeResult>` — Clears the id (Flags: one flag, or all with -1).
- `Set(string name, uint value) -> Task<KnowledgeResult>` — Writes an exact value (a direct write: no XP, no query-trigger event).
- `Set(uint id, uint value) -> Task<KnowledgeResult>` — Writes an exact value (a direct write: no XP, no query-trigger event).
- `Target { get; } -> EntityId` — 0 = the world.

## KnowledgeSecurity — The opt-in checks (`Security`).
- `KnowledgeSecurity(bool VetoClientKnowledge = false, bool CheckDialogue = false, double DialogueRange = 12, bool LogClientKnowledge = true, int AllowedIds = 0)`
- `AllowedIds { get; init; } -> int`
- `CheckDialogue { get; init; } -> bool` — Hold a dialogue line's knowledge and run it only if the line's requirement holds for the player and the player is...
- `DialogueRange { get; init; } -> double`
- `LogClientKnowledge { get; init; } -> bool`
- `VetoClientKnowledge { get; init; } -> bool` — Hold every knowledge operation a client sends (TriggerKnowledgeAction) and drop it unless its id is allowed or a...

## KnowledgeSettings — The file's schema (Kiln's config "knowledge").
- `AllowClientKnowledge { get; set; } -> List<string>`
- `CheckDialogue { get; set; } -> bool`
- `DialogueRange { get; set; } -> double`
- `LogClientKnowledge { get; set; } -> bool`
- `VetoClientKnowledge { get; set; } -> bool`

## KnowledgeState — One id in one map, read at `KnowledgeState.Step`.
- `KnowledgeState()`
- `KnowledgeState(EntityId Target, uint Id, uint Value, KnowledgeType Type, bool Present, byte NewMask, ulong Step)`
- `HasFlag(int flag) -> bool`
- `Id { get; init; } -> uint`
- `IsSet { get; } -> bool`
- `NewMask { get; init; } -> byte`
- `Present { get; init; } -> bool` — The map has an entry (it can hold 0 after a revoke).
- `Step { get; init; } -> ulong`
- `Target { get; init; } -> EntityId` — 0 = the world map, else the player.
- `Type { get; init; } -> KnowledgeType`
- `Value { get; init; } -> uint` — Bool 0/1, Counter n, Flags mask; 0 = not set.

## KnowledgeStatus — What kiln.knowledge can do on this build, and its counters.
- `KnowledgeStatus(bool Knowledge, bool Changes, bool Journal, bool Npc, bool Dialogue, bool ClientInput, bool Program, bool World, int WorldObjects, int PlayerObjects, int WorldEntries, int Players, int Npcs, int Quests, int Queries, int Pending, ulong Submitted, ulong Applied, ulong Failed, ulong ChangesSeen, ulong ClientOps, ulong ClientVetoed, ulong DialogueEvents, ulong DialogueHeld, ulong DialogueDropped, int QueueMax, string Reason, bool Applying = false)`
- `Applied { get; init; } -> ulong`
- `Applying { get; init; } -> bool`
- `Changes { get; init; } -> bool`
- `ChangesSeen { get; init; } -> ulong`
- `ClientInput { get; init; } -> bool`
- `ClientOps { get; init; } -> ulong`
- `ClientVetoed { get; init; } -> ulong`
- `Dialogue { get; init; } -> bool`
- `DialogueDropped { get; init; } -> ulong`
- `DialogueEvents { get; init; } -> ulong`
- `DialogueHeld { get; init; } -> ulong`
- `Failed { get; init; } -> ulong`
- `Journal { get; init; } -> bool`
- `Knowledge { get; init; } -> bool`
- `Npc { get; init; } -> bool`
- `Npcs { get; init; } -> int`
- `Pending { get; init; } -> int`
- `PlayerObjects { get; init; } -> int`
- `Players { get; init; } -> int`
- `Program { get; init; } -> bool`
- `Queries { get; init; } -> int`
- `Quests { get; init; } -> int`
- `QueueMax { get; init; } -> int`
- `Reason { get; init; } -> string`
- `Submitted { get; init; } -> ulong`
- `World { get; init; } -> bool`
- `WorldEntries { get; init; } -> int`
- `WorldObjects { get; init; } -> int`

## KnowledgeType — The game's knowledge types.
enum: Bool, Counter, Flags, None

## NativeComputed — A knowledge object the engine keeps for what a line's one knowledge check cannot say.
- `NativeComputed(string Mod, uint Id, string Kind, string Target, string Node, int Index, uint Gate, uint Heard)`
- `Gate { get; init; } -> uint`
- `Heard { get; init; } -> uint`
- `Id { get; init; } -> uint`
- `Index { get; init; } -> int`
- `Kind { get; init; } -> string`
- `Mod { get; init; } -> string`
- `Node { get; init; } -> string`
- `Target { get; init; } -> string`

## NativeIds — Ids of a mod's native quests, dialogue lines and knowledge objects: FNV-1a-32 of a UUIDv5 of `<mod>:<kind>:<name>`.
- `static Derive(string mod, string kind, string name) -> Guid` — The derived GUID (game byte order = `Guid.ToByteArray`).
- `static Fnv(Guid g) -> uint` — FNV-1a-32 over a GUID's bytes: the game's id of an item, recipe, NPC or loca tag with that GUID.
- `static Knowledge(string mod, string name) -> uint` — A knowledge object the mod declared in `knowledge` (or the generated ones: `q.<quest>.active`, ...).
- `static Line(string mod, string dialogue, string node, int option) -> uint` — A native dialogue line: `<dialogue>.<node>.<option>` (its id is its "heard" knowledge).

## NativeLineInfo — A line in the game's menu.
- `NativeLineInfo(string Mod, string Dialogue, string Npc, string Node, int Option, uint Line, string Auto = "")`
- `Auto { get; init; } -> string`
- `Dialogue { get; init; } -> string`
- `Line { get; init; } -> uint`
- `Mod { get; init; } -> string`
- `Node { get; init; } -> string`
- `Npc { get; init; } -> string`
- `Option { get; init; } -> int`

## NativeMarkerInfo
- `NativeMarkerInfo(string Mod, string Quest, string Step, string Template, Guid Guid, WorldPosition? At)`
- `At { get; init; } -> WorldPosition?`
- `Guid { get; init; } -> Guid`
- `Mod { get; init; } -> string`
- `Quest { get; init; } -> string`
- `Step { get; init; } -> string`
- `Template { get; init; } -> string`

## NativeModInfo
- `NativeModInfo(string Id, bool Ok, IReadOnlyDictionary<string, uint> Knowledge, IReadOnlyList<NativeQuestInfo> Quests, IReadOnlyList<NativeLineInfo> Lines, IReadOnlyList<NativeNpcInfo> Npcs, IReadOnlyList<NativeMarkerInfo> Markers, IReadOnlyList<string> Errors, IReadOnlyList<NativeComputed> Computed)`
- `Computed { get; init; } -> IReadOnlyList<NativeComputed>`
- `Errors { get; init; } -> IReadOnlyList<string>`
- `Id { get; init; } -> string`
- `Knowledge { get; init; } -> IReadOnlyDictionary<string, uint>`
- `Lines { get; init; } -> IReadOnlyList<NativeLineInfo>`
- `Markers { get; init; } -> IReadOnlyList<NativeMarkerInfo>`
- `Npcs { get; init; } -> IReadOnlyList<NativeNpcInfo>`
- `Ok { get; init; } -> bool`
- `Quests { get; init; } -> IReadOnlyList<NativeQuestInfo>`

## NativeNpcInfo
- `NativeNpcInfo(string Mod, string Id, string FullId, string Template, Guid Guid, bool IsClone, uint NpcId, bool Roster, uint Unlock, NativePlacement? Place, string Dialogue, string SummonItem)`
- `Dialogue { get; init; } -> string`
- `FullId { get; init; } -> string`
- `Guid { get; init; } -> Guid`
- `Id { get; init; } -> string`
- `IsClone { get; init; } -> bool`
- `Mod { get; init; } -> string`
- `NpcId { get; init; } -> uint`
- `Place { get; init; } -> NativePlacement?`
- `Roster { get; init; } -> bool`
- `SummonItem { get; init; } -> string`
- `Template { get; init; } -> string`
- `Unlock { get; init; } -> uint`

## NativePlacement
- `NativePlacement(QuestPlace Spec, float Yaw, bool Once, bool? Unlock)`
- `Once { get; init; } -> bool`
- `Spec { get; init; } -> QuestPlace`
- `Unlock { get; init; } -> bool?`
- `Yaw { get; init; } -> float`

## NativeQuestInfo
- `NativeQuestInfo(string Mod, string Id, string FullId, string Scope, uint EntryId, uint Started, uint Active, uint Done, IReadOnlyList<NativeStepInfo> Steps)`
- `Active { get; init; } -> uint`
- `Done { get; init; } -> uint`
- `EntryId { get; init; } -> uint`
- `FullId { get; init; } -> string`
- `Id { get; init; } -> string`
- `Mod { get; init; } -> string`
- `Scope { get; init; } -> string`
- `Started { get; init; } -> uint`
- `Steps { get; init; } -> IReadOnlyList<NativeStepInfo>`

## NativeQuests — What was built at boot from the mods' quests/*.json, and the run-time glue.
- `static Counters { get; } -> (ulong Mirrored, ulong Lines, ulong Placed)` — Mirrored knowledge changes, dialogue options run, NPCs and markers placed.
- `static GetClientRosterWorld() -> RosterWorldResult` `0.2.4` — This process's client world as a token for `NativeQuests.GetClientUnlock` and `NativeQuests.GetRosterBudget`; as...
- `static GetClientUnlock(uint npcId, CapacityWorldToken clientWorld) -> NpcUnlockResult` `0.2.4` — The unlock state of `npcId` in the client world `clientWorld` names; as `NativeQuests.GetUnlock`.
- `static GetRosterBudget(CapacityWorldToken world) -> CapacityQueryResult` `0.2.4` — The roster entries of the world `world` names: `CapacityInfo.Capacity` 64, or 128 where Kiln's second page is...
- `static GetServerRosterWorld() -> RosterWorldResult` `0.2.4` — This process's server world (a dedicated server's, or a hosting client's) as a token for `NativeQuests.GetUnlock`...
- `static GetUnlock(uint npcId, CapacityWorldToken serverWorld) -> NpcUnlockResult` `0.2.4` — The unlock state of the roster entry `npcId` in the server world `serverWorld` names, from the snapshot of that...
- `static Holds(NativeComputed c, QuestHolder holder) -> bool` — What the engine says now of a line's computed knowledge, for a holder (`NativeModInfo.Computed`).
- `static KnowledgeId(string mod, string name) -> uint?` — A mod's native object by name ("met_ilse", "q.lost_letter.active"); null when the build does not have it.
- `static ManifestJson { get; } -> string` — The manifest as the service wrote it (JSON).
- `static Mod(string id) -> NativeModInfo?`
- `static Mods { get; } -> IReadOnlyList<NativeModInfo>` — Every mod with native data (a mod with errors is listed with Ok = false and built nothing).
- `static Npc(string fullId) -> NativeNpcInfo?`
- `static Quest(string fullId) -> NativeQuestInfo?`
- `static Resync(QuestHolder holder, bool quiet = false) -> void` — Writes a holder's knowledge again from the engine's records: every quest, what differs.
- `const RosterPage = 64 -> int` `0.2.4` — The roster's maximum the game's own value allows: 64 entries, the game's included.
- `static ShownByGame(QuestDefinition q) -> bool` — The game's journal shows this quest: it says native and the boot build made it.
- `static Start() -> void` — Starts the mirror, the dialogue options and the placement (the engine calls this; idempotent).
- `static Wanted(string fullQuestId, QuestHolder holder) -> IReadOnlyList<NativeWanted>` — What the game's knowledge should hold for a holder in a quest, as the engine sees it now.

## NativeStepInfo — A step's knowledge.
- `NativeStepInfo(string Id, uint Visible, uint Active, uint Done, uint Count = 0, int Total = 0, uint Ready = 0)`
- `Active { get; init; } -> uint`
- `Count { get; init; } -> uint`
- `Done { get; init; } -> uint`
- `Id { get; init; } -> string`
- `Ready { get; init; } -> uint`
- `Total { get; init; } -> int`
- `Visible { get; init; } -> uint`

## NativeWanted — A knowledge value the engine's state asks for.
- `NativeWanted()`
- `NativeWanted(string Name, uint Id, uint Value)`
- `Id { get; init; } -> uint`
- `Name { get; init; } -> string`
- `Value { get; init; } -> uint`

## NpcUnlockResult `0.2.4` — One roster entry's unlock state in one world's snapshot.
- `NpcUnlockResult(bool Ok, string? Reason, uint NpcId, bool? Unlocked, ushort? KnowledgeVersion, CapacityWorldToken World, ulong SnapshotRevision)`
- `KnowledgeVersion { get; init; } -> ushort?` — The engine's knowledge version the snapshot was computed at; null when not `Ok`.
- `NpcId { get; init; } -> uint` — The NpcId asked for (FNV-1a-32 of the NPC template's GUID).
- `Ok { get; init; } -> bool` — True when the snapshot answered: `Unlocked` and `KnowledgeVersion` are set.
- `Reason { get; init; } -> string?` — Null when `Ok`; else `WorldEnded` (the token's world stopped, or another world of that kind runs now), `NotReady`...
- `SnapshotRevision { get; init; } -> ulong` — kiln.knowledge's revision of that snapshot (it rises whenever a published roster answer changes).
- `Unlocked { get; init; } -> bool?` — The game's unlock state of the entry; null when not `Ok`.
- `World { get; init; } -> CapacityWorldToken` — The world the request named, also when it failed.

## Obelisk — An Ancient Obelisk (reading one reveals its region's shrines).
- `Obelisk(string Id, string Region, WorldPosition? Position, ProgressFact Reading)`
- `Id { get; init; } -> string`
- `Position { get; init; } -> WorldPosition?`
- `ReadBy(Player player) -> bool?`
- `Reading { get; init; } -> ProgressFact`
- `Region { get; init; } -> string`

## ProgressFact — A fact as the game stores it: a requirement (a knowledge id compared to a value, or a named query) and where it lives.
- `ProgressFact(KnowledgeQuery Query, ProgressScope Scope)`
- `Holds(EntityId target) -> bool?` — For the world (`target` None) or a player in game.
- `Holds(Player player) -> bool?` — For the world (`target` None) or a player in game.
- `Id { get; } -> uint` — The knowledge id (or the named query's id).
- `Query { get; init; } -> KnowledgeQuery`
- `Scope { get; init; } -> ProgressScope`

## ProgressScope — Where the game keeps a fact.
enum: World, Character, Both

## QuestAction — A reward or side effect (a quest's rewards and hooks, a conversation option's `do`).
- `At { get; set; } -> WorldPosition?` — Spawn, teleport, waypoint: the place.
- `BindNpc { get; set; } -> string?` — Spawn: the entity becomes this NPC of `npcs` (see `Quests.BindNpc`).
- `Clear { get; set; } -> bool` — Waypoint: true clears it.
- `static Code(Func<QuestActionContext, Task> run) -> QuestAction`
- `Count { get; set; } -> int` — Items, spawned entities, or a signal's amount.
- `Exact { get; set; } -> bool` — Spawn: exactly at the place given.
- `static Experience(uint amount) -> QuestAction`
- `Force { get; set; } -> bool` — Force an advance past unfinished objectives.
- `static GrantKnowledge(string name, string? target = null) -> QuestAction`
- `static Item(string item, int count = 1) -> QuestAction`
- `Kind { get; init; } -> QuestActionKind`
- `Name { get; set; } -> string?` — Item, knowledge name, quest id, "quest.step", template, dialogue, signal or action name.
- `Node { get; set; } -> string?` — Talk: the node to open at.
- `Offset { get; set; } -> WorldPosition?` — Spawn, teleport, waypoint: an offset in metres from the player, instead of `QuestAction.At`.
- `Run { get; set; } -> Func<QuestActionContext, Task>?` — Call, C#: the code to run.
- `Tag { get; set; } -> string?` — Spawn: the Kiln.Spawn tag.
- `Target { get; set; } -> string?` — Knowledge, revoke: "world" or "player"; default: the player's object if the id has one, else the world's.
- `Text { get; set; } -> UiText?`
- `ToastKind { get; set; } -> string?` — Toast: info, success, warning, error.
- `Transient { get; set; } -> bool` — Spawn: never written to the save.
- `Value { get; set; } -> uint` — Xp: the amount.
- `Yaw { get; set; } -> double`

## QuestActionContext — What an action runs for.
- `QuestActionContext(QuestHolder Holder, QuestDefinition? Quest, EntityId Npc, string ModId)`
- `Holder { get; init; } -> QuestHolder`
- `ModId { get; init; } -> string`
- `Npc { get; init; } -> EntityId`
- `Player { get; } -> Player?`
- `Quest { get; init; } -> QuestDefinition?`

## QuestActionKind
enum: Give, Take, Xp, Knowledge, Revoke, Start, Complete, Fail, Advance, Reset, Spawn, Despawn, Teleport, Chat, Toast, Waypoint, Talk, Signal, Call

## QuestBook — Everything one holder has done: one per character (per world) and one for the world.
- `Quests { get; set; } -> Dictionary<string, QuestRecord>` — Quest full id -> its record.
- `Revision { get; set; } -> long` — +1 per change.
- `Tracked { get; set; } -> string?` — Old saves' tracked quest.

## QuestCancelableEvent — Base of the quest events that fire before a change.
- `Holder { get; } -> QuestHolder`
- `Player { get; } -> Player?` — The player in game (null for the world, offline and synthetic holders).
- `Quest { get; } -> QuestDefinition`

## QuestCompletedEvent — A quest is about to complete (its last step's objectives are done; before that step's `onDone` actions and the rewards).

## QuestCondition — A condition on a holder, checked on the server: a quest's `requires`, an option's `if`.
- `Children { get; set; } -> List<QuestCondition>`
- `Count { get; set; } -> int` — Level: at least this level.
- `static HasItem(string item, int count = 1) -> QuestCondition`
- `static HasKnowledge(string name) -> QuestCondition`
- `Is { get; set; } -> string?` — Quest: locked, available, active, done, failed.
- `Kind { get; init; } -> QuestConditionKind`
- `static Level(int level) -> QuestCondition`
- `Name { get; set; } -> string?` — Knowledge name, quest id, "quest.step", item, or predicate name.
- `Predicate { get; set; } -> Func<QuestContext, bool>?`
- `static Quest(string quest, string state) -> QuestCondition`
- `static QuestStep(string questDotStep, string state) -> QuestCondition`
- `static When(Func<QuestContext, bool> predicate) -> QuestCondition`

## QuestConditionKind
enum: Knowledge, Level, Quest, Step, Item, All, Any, Not, Predicate

## QuestContext — What a condition or a custom objective is asked about.
- `QuestContext(QuestHolder Holder, QuestDefinition? Quest, EntityId Npc, string ModId)`
- `Holder { get; init; } -> QuestHolder`
- `ModId { get; init; } -> string`
- `Npc { get; init; } -> EntityId`
- `Player { get; } -> Player?`
- `Quest { get; init; } -> QuestDefinition?`

## QuestDefinition — A quest chain: steps that run in order, each with one or more objectives.
- `Abandonable { get; set; } -> bool` — Players may give it up (never a world quest; default true).
- `Auto { get; set; } -> bool` — Starts by itself as soon as it is available (a world quest when the world runs, a player quest while the player is...
- `Description { get; set; } -> UiText?`
- `FullId { get; } -> string` — `<modId>:<id>`.
- `Giver { get; set; } -> string?` — The NPC who offers it (an id of the same mod's `npcs`, or `mod:npc`).
- `Hidden { get; set; } -> bool` — Not listed until it is active.
- `Id { get; init; } -> string` — Letters, digits, '_' and '-'.
- `Journal { get; set; } -> QuestJournalMode`
- `Main { get; set; } -> bool`
- `ModId { get; } -> string` — The mod that defined it ("" for code outside a mod).
- `Name { get; set; } -> UiText`
- `OnFail { get; set; } -> List<QuestAction>`
- `OnStart { get; set; } -> List<QuestAction>`
- `PartyRadius { get; set; } -> double`
- `Priority { get; set; } -> int?` — The game's journal: its place in the list (the game uses 40..100; default 50).
- `Repeatable { get; set; } -> bool` — Can be started again after it is done.
- `Requires { get; set; } -> List<QuestCondition>`
- `Retryable { get; set; } -> bool` — Can be started again after it failed or was abandoned (default true).
- `Rewards { get; set; } -> List<QuestAction>`
- `Scope { get; set; } -> QuestScope`
- `Source { get; set; } -> string?` — The game's journal: the portrait (keen::QuestSourceType: None, Flame, Blacksmith, Alchemist, ...).
- `Step(string id) -> QuestStep?`
- `Steps { get; set; } -> List<QuestStep>`

## QuestEngineStatus — What the engine holds and has done.
- `QuestEngineStatus(bool Started, bool ServerSide, int Quests, int Dialogues, int Npcs, int Conversations, int Errors, ulong Transitions, ulong Actions, ulong ActionFailures, bool Waypoints, IReadOnlyList<string> LoadErrors)`
- `ActionFailures { get; init; } -> ulong`
- `Actions { get; init; } -> ulong`
- `Channel { get; } -> bool` — Always false: the channel kiln.quests is gone with Kiln's journal and windows.
- `Conversations { get; init; } -> int`
- `Dialogues { get; init; } -> int`
- `Errors { get; init; } -> int`
- `LoadErrors { get; init; } -> IReadOnlyList<string>`
- `Npcs { get; init; } -> int`
- `Quests { get; init; } -> int`
- `ServerSide { get; init; } -> bool`
- `Started { get; init; } -> bool`
- `Transitions { get; init; } -> ulong`
- `Waypoints { get; init; } -> bool`

## QuestEntry — One quest as a character stands in it.
- `QuestEntry(string Id, string? Name, QuestKind Kind, QuestStatus Status, int Step, int Steps, string? StepId, uint NameTag, JournalQuest? Journal, QuestDefinition? Definition)`
- `Active { get; } -> bool`
- `Definition { get; init; } -> QuestDefinition?`
- `Done { get; } -> bool`
- `Id { get; init; } -> string` — Vanilla: `quest:0x<entryId>`; Kiln: the quest's full id `<mod>:<id>`.
- `Journal { get; init; } -> JournalQuest?`
- `Kind { get; init; } -> QuestKind`
- `Name { get; init; } -> string?` — A readable name: vanilla quests the game's named query behind them (`NPC_Alchemist_Hint01_HAS_Shelter_Journal`; null...
- `NameTag { get; init; } -> uint` — Vanilla: the loca tag of the quest's title (the catalog's text).
- `Status { get; init; } -> QuestStatus` — Vanilla quests are Locked (not started), Active or Done; the game has no failed state.
- `Step { get; init; } -> int` — The current step (0-based; the last step when done); -1 when not started.
- `StepId { get; init; } -> string?`
- `Steps { get; init; } -> int`

## QuestFailedEvent — A quest is about to fail.
- `Reason { get; } -> string`

## QuestHolder — Who owns a quest's state: a character on this world (the game's own key: account + CharacterId), or the world.
- `QuestHolder()`
- `QuestHolder(string Key, ulong Account, EntityId Entity)`
- `Account { get; init; } -> ulong` — The Steam or account id (where the data was filed before characters); 0 for the world and for a session holder.
- `Character { get; init; } -> CharacterKey` — The character whose book this is; none for the world, synthetic holders and builds without character ids.
- `Entity { get; init; } -> EntityId` — The player's entity while in game; none when offline, for the world, and for synthetic holders.
- `static explicit operator PlayerRef(QuestHolder holder) -> PlayerRef` — This holder as a `PlayerRef`.
- `IsNone { get; } -> bool`
- `IsWorld { get; } -> bool`
- `Key { get; init; } -> string` — "world", "character:\<account>.\<id>", "player:\<account>" (a synthetic holder, an offline account, or a build...
- `static Of(EntityId player) -> QuestHolder` — The holder of a player in game.
- `static Of(Player p) -> QuestHolder` — The holder of a player in game.
- `static OfAccount(ulong account) -> QuestHolder` — A player known only by their account (offline): the character they last played in this world, or the account's book...
- `static OfCharacter(CharacterKey character, ulong legacyAccount = 0) -> QuestHolder` — A character, online or not.
- `static Parse(string who) -> QuestHolder` — "world", "synthetic:\<name>", or the name or entity number of a player in game.
- `static Synthetic(string name) -> QuestHolder` — A holder without an entity for tests and tools: its state is saved like a player's, it has no position or inventory...
- `static World { get; } -> QuestHolder`

## QuestJournalMode — Where a quest is shown (JSON `journal`).
enum: Chat, Native

## QuestKind
enum: Vanilla, Kiln

## QuestLog — A character's quests (`Quests.Of`), read when it is made.
- `Active { get; } -> IEnumerable<QuestEntry>`
- `All { get; } -> IReadOnlyList<QuestEntry>`
- `Completed { get; } -> IEnumerable<QuestEntry>`
- `Find(string idOrName) -> QuestEntry?` — By id (`quest:0x8755c335`, `0x8755c335`, `mymod:tobin`, or a Kiln quest's id within its mod) or name...
- `Kiln { get; } -> IEnumerable<QuestEntry>`
- `Player { get; } -> EntityId`
- `StatusOf(string idOrName) -> QuestStatus`
- `Vanilla { get; } -> IEnumerable<QuestEntry>`

## QuestMarker — A step's map marker.
- `QuestMarker(WorldPosition? At = null, double Radius = 0, bool Waypoint = true, UiText? Name = null, string? Base = null)`
- `At { get; init; } -> WorldPosition?`
- `Base { get; init; } -> string?`
- `Name { get; init; } -> UiText?`
- `Radius { get; init; } -> double`
- `Waypoint { get; init; } -> bool`

## QuestNpcDefinition — An NPC a quest or conversation refers to: a game NPC, or a new one (`QuestNpcDefinition.Clone`), found by its template or placed in the world...
- `At { get; set; } -> WorldPosition?` — Where it stands (the JSON's `place.at`): with no entity found, this spot still counts as the NPC.
- `Clone { get; set; } -> bool` — A new NPC: a copy of `QuestNpcDefinition.Template` under its own GUID, one per world.
- `CloneGuid { get; set; } -> Guid?` — The clone's GUID.
- `CloneName { get; set; } -> string?` — The clone's template name; null derives it.
- `Dialogue { get; set; } -> string?` — The conversation it opens.
- `FullId { get; } -> string`
- `Id { get; init; } -> string`
- `ModId { get; } -> string`
- `Name { get; set; } -> UiText?`
- `Place { get; set; } -> QuestPlace?` — Where Kiln puts it in the world, once it can: on real ground, outside every base unless `QuestPlace.InBase`.
- `TalkRange { get; set; } -> double` — How close a player must be to talk (metres).
- `Template { get; set; } -> string?` — Template name of the entity (of a clone: the base).

## QuestObjective — One thing to do in a step.
- `At { get; set; } -> WorldPosition?` — Reach: the place.
- `static Collect(string item, int count = 1) -> QuestObjective`
- `Count { get; set; } -> int` — How many (kills, items, crafts, signals).
- `CountDestroyed { get; set; } -> bool` — Kill: Kiln.Spawn destroying a matching entity counts too (off: only deaths count).
- `static Custom(string signal, int count = 1) -> QuestObjective`
- `static Deliver(string item, int count, string to) -> QuestObjective`
- `Family { get; set; } -> string?` — Kill: part of the template name ("Scavenger", "Vukah").
- `static HasKnowledge(string name) -> QuestObjective`
- `Item { get; set; } -> string?` — Collect, deliver, craft, build: item name or id.
- `static Kill(string? template = null, string? family = null, string? tag = null, int count = 1) -> QuestObjective`
- `Kind { get; init; } -> QuestObjectiveKind`
- `Knowledge { get; set; } -> string?` — Knowledge: the name that must hold ("npc:...", "quest:...", "query:...", "0x...").
- `Line { get; set; } -> string?` — Talk: a game dialogue line ("line:...", "0x..."), or a line of Kiln.Quests/Native.
- `Marker { get; set; } -> string?` — Reach: a template name; the nearest entity of that template is the place.
- `Near { get; set; } -> WorldPosition?` — Kill: only near this place (with `QuestObjective.Radius`).
- `Node { get; set; } -> string?` — Talk: a node of a Kiln conversation that must be reached.
- `Npc { get; set; } -> string?` — Talk: the NPC (an id of `npcs`, a template name, or "npc:\<debugName>").
- `Offset { get; set; } -> WorldPosition?` — Reach: an offset in metres from where the player stood when the step started, instead of `QuestObjective.At`.
- `Progress { get; set; } -> Func<QuestContext, int>?` — Custom, C#: polled about once a second, returns the current count.
- `Radius { get; set; } -> double` — Reach, kill: metres.
- `static Reach(WorldPosition at, double radius = 10) -> QuestObjective`
- `Recipe { get; set; } -> string?` — Craft: recipe id ("0x...").
- `Signal { get; set; } -> string?` — Custom: `Quests.Signal` with this name adds to the counter.
- `Site { get; set; } -> bool` — Reach: the step's `QuestStep.Site` is the place.
- `Tag { get; set; } -> string?` — Kill: the Kiln.Spawn tag of the victim.
- `static Talk(string npc, string? node = null, string? line = null) -> QuestObjective`
- `Template { get; set; } -> string?` — Kill: exact template name.
- `Text { get; set; } -> UiText?` — Shown instead of the step's text when a step has several objectives.
- `To { get; set; } -> string?` — Deliver: the NPC who receives the items.

## QuestObjectiveKind
enum: Kill, Collect, Deliver, Reach, Talk, Craft, Build, Knowledge, Custom

## QuestObjectiveProgressEvent — An objective's counter changed.
- `Holder { get; } -> QuestHolder`
- `Index { get; } -> int` — The objective's place in the step.
- `Objective { get; } -> QuestObjective`
- `OldValue { get; } -> int`
- `Quest { get; } -> QuestDefinition`
- `Step { get; } -> QuestStep`
- `Target { get; } -> int`
- `Value { get; } -> int`

## QuestPlace — A place found at run time: on real ground (the voxel surface), outside every player base, not too steep.
- `At { get; set; } -> WorldPosition?` — Search around this place.
- `BaseMargin { get; set; } -> double` — Metres to keep from a base's build zone (default 20).
- `Exact { get; set; } -> bool` — Exactly `QuestPlace.At`: no ground, no base check.
- `InBase { get; set; } -> bool` — A base is fine (a friendly NPC that belongs in one).
- `MaxDistance { get; set; } -> double`
- `MinDistance { get; set; } -> double` — Metres from the centre of the search.
- `NearNpc { get; set; } -> string?` — Search around this NPC of the mod's `npcs`.
- `NearPlayer { get; set; } -> bool` — Search around the player who gets there first (the default when nothing else is given).

## QuestProgress — One holder's standing in one quest.
- `QuestProgress(QuestDefinition Quest, QuestHolder Holder, QuestStatus Status, string? StepId, int StepIndex, IReadOnlyDictionary<string, int> Counters, int Completions)`
- `Completions { get; init; } -> int`
- `Counter(int objective = 0) -> int` — The counter of an objective of the current step.
- `Counters { get; init; } -> IReadOnlyDictionary<string, int>`
- `Holder { get; init; } -> QuestHolder`
- `Quest { get; init; } -> QuestDefinition`
- `Status { get; init; } -> QuestStatus`
- `StepId { get; init; } -> string?`
- `StepIndex { get; init; } -> int`

## QuestRecord — A holder's state in one quest, as stored.
- `Completions { get; set; } -> int`
- `static CounterKey(string step, int objective) -> string`
- `Counters { get; set; } -> Dictionary<string, int>` — "\<step>#\<objective>" -> count.
- `EnteredAt { get; set; } -> long` — When the step started (not saved): a step the player already fulfils stays on screen a moment before it passes.
- `FinishedAt { get; set; } -> long`
- `RewardsPending { get; set; } -> bool` — Rewards still to pay (the player was offline when an admin completed it).
- `StartedAt { get; set; } -> long`
- `Status { get; set; } -> QuestStatus`
- `StepId { get; set; } -> string?` — The current step (an id, so a changed definition keeps its place); null when not active.
- `StepIndex { get; set; } -> int`
- `StepOrigin { get; set; } -> double[]?` — Where the player stood when the step started (for reach offsets).

## QuestResult — The answer to a quest change.
- `QuestResult(bool Ok, string Reason, QuestStatus Status)`
- `Ok { get; init; } -> bool`
- `Reason { get; init; } -> string`
- `Status { get; init; } -> QuestStatus`

## Quests — Kiln quests: chains defined by mods (quests/*.json or `Quests.Define`), run by the server for players, parties and the world.
- `static Abandon(string quest, QuestHolder holder) -> QuestResult` — A player gives up a quest: back to available (when it may be retried), else failed.
- `static Advance(string quest, QuestHolder holder, string? step = null, bool force = false) -> Task<QuestResult>` — Finishes a holder's current step: objectives that are still open are refused unless they are talk or custom (a...
- `static Available { get; } -> bool`
- `static BindNpc(string fullNpcId, EntityId entity) -> void` — Says that this entity is that NPC (it wins over the search by template).
- `static CanStart(string quest, QuestHolder holder) -> bool` — A quest can be started now: never started, or done and repeatable, or failed and retryable, and its requirements hold.
- `static ChatLanguage { get; set; } -> string` — Text language of chat lines (vanilla clients do not tell the server theirs).
- `static Complete(string quest, QuestHolder holder) -> QuestResult` — Completes a quest for a holder now (rewards paid; past the remaining steps).
- `static Define(QuestDefinition quest) -> QuestDefinition` — Registers a quest as the calling mod's (it goes away when the mod unloads; states stay).
- `static DefineNpc(QuestNpcDefinition npc) -> QuestNpcDefinition` — Registers an NPC a quest or conversation can refer to, as the calling mod's.
- `static Definitions { get; } -> IReadOnlyList<QuestDefinition>` — Every quest defined now.
- `static EnsureStarted() -> void` — Starts the engine (Kiln does this at its first service tick; a mod may call it earlier).
- `static Evaluate(QuestCondition condition, QuestContext context) -> bool` — Evaluates a condition the way the engine does.
- `static Fail(string quest, QuestHolder holder, string reason = "") -> QuestResult` — Fails an active quest for a holder.
- `static Find(string id, string? modId = null) -> QuestDefinition?` — A quest by full id, or by its own id when only one mod has it.
- `static LineResolver { get; set; } -> Func<string, string, uint?>?` — Maps a talk objective's `line` to a line id when the knowledge names do not know it (modId, text)...
- `static NpcDefinitions { get; } -> IReadOnlyList<QuestNpcDefinition>` — Every NPC defined now.
- `static Of(EntityId player) -> QuestLog` — A player's quests, vanilla and Kiln (tick thread).
- `static Of(Player player) -> QuestLog` — A player's quests, vanilla and Kiln (tick thread).
- `static PlaceOf(string quest, string step) -> WorldPosition?` — The place a step found: on the ground, outside every base.
- `static Progress(string quest, QuestHolder holder) -> QuestProgress?` — A holder's standing in a quest (null when never started).
- `static ProgressOf(EntityId player) -> IReadOnlyList<QuestProgress>` — Every quest a player in game has a record of (`Quests.ProgressOf`).
- `static ProgressOf(QuestHolder holder) -> IReadOnlyList<QuestProgress>` — Every quest a holder has a record of, world quests included.
- `static RecentActions { get; } -> IReadOnlyList<string>` — The last action outcomes: rewards paid, items taken, what failed and why (newest last).
- `static RegisterAction(string name, Func<QuestActionContext, Task> action) -> void` — A code action for JSON's `{"call": "name"}`.
- `static RegisterPredicate(string name, Func<QuestContext, bool> predicate) -> void` — A code condition for JSON's `{"predicate": "name"}`.
- `static Reload() -> int` — Reads quests/*.json of every loaded mod again and keeps what code defined.
- `static Reset(string quest, QuestHolder holder) -> QuestResult` — Forgets a holder's record of a quest (as if never started).
- `static SetStep(string quest, QuestHolder holder, string step) -> QuestResult` — Puts a holder at a step: active there with the step's counters at 0 (an admin's tool: no rewards, no hooks of...
- `static Signal(string name, QuestHolder? holder = null, int amount = 1) -> void` — Adds to every open custom objective with this signal: the holder's quests (and party's, and world quests); with no...
- `static SpawnedBy(string quest, string step, QuestHolder holder) -> IReadOnlyList<EntityId>` — The creatures a step's `spawn` holds in the world for a holder now.
- `static SpawnGaveUp(string quest, string step, QuestHolder holder) -> bool` — A step's `spawn` used up its retries: the game kept removing them (kiln.log has the error).
- `static Start(string quest, QuestHolder holder, bool force = false) -> QuestResult` — Starts a quest for a holder (a party quest also for players near them who can start it).
- `static Status { get; } -> QuestEngineStatus`
- `static StatusOf(string quest, QuestHolder holder) -> QuestStatus` — A holder's status in a quest.
- `static TrackedBy(QuestHolder holder) -> string?` — The tracked quest an old save holds.
- `static WaypointCounters { get; } -> (ulong Requests, ulong Written)` — Waypoint requests and writes of kiln.questmark so far.

## QuestScope — Who owns a quest's progress.
enum: Player, Party, World

## QuestSource — Who gives a quest (the game's QuestSourceType: an NPC portrait or a biome).
enum: None, Flame, Blacksmith, Alchemist, Huntress, Farmer, Carpenter, CryptKeeper, Bard, ChineseNewYearTrader, Barber, Fisher, AncientResearcher, Grassland, Deepforest, Steppes, Desert, ColdHeights, Wetlands, AncientLands

## QuestSpawn — Creatures a step brings into the world.
- `Count { get; set; } -> int`
- `Level { get; set; } -> int` — The game's level to spawn at (0: the template's).
- `Place { get; set; } -> QuestPlace?` — Default the step's `QuestStep.Site`; without one, near the player at 30..60 m.
- `Retries { get; set; } -> int` — How many times Kiln spawns them again when the game removes them before they die (default 5).
- `Spread { get; set; } -> double` — Metres between them.
- `Tag { get; set; } -> string?` — The Kiln.Spawn tag: a kill objective can name it.
- `Template { get; init; } -> string`

## QuestStartedEvent — A quest is about to start for a holder.

## QuestStateChangedEvent — A holder's standing in a quest changed: status or step.
- `Holder { get; } -> QuestHolder`
- `OldStatus { get; } -> QuestStatus`
- `Quest { get; } -> QuestDefinition`
- `Status { get; } -> QuestStatus`
- `StepId { get; } -> string?` — The current step; null when not at a step (done, failed, reset).
- `StepIndex { get; } -> int`

## QuestStatus — Where a quest stands for one holder.
enum: Locked, Available, Active, Done, Failed

## QuestStatusChangedEvent — A player's standing in a quest changed: its status or its current step.
- `NewStatus { get; } -> QuestStatus`
- `NewStep { get; } -> int`
- `OldStatus { get; } -> QuestStatus`
- `OldStep { get; } -> int`
- `Player { get; } -> Player?`
- `PlayerEntity { get; } -> EntityId` — The player (None for a Kiln world quest).
- `Quest { get; } -> QuestEntry`

## QuestStep — One step of a quest.
- `Id { get; init; } -> string`
- `Marker { get; set; } -> QuestMarker?` — The map marker and waypoint of the step.
- `NoMarker { get; set; } -> bool` — This step shows no marker, even though it has a place (JSON `"marker": false`).
- `Objectives { get; set; } -> List<QuestObjective>`
- `OnDone { get; set; } -> List<QuestAction>`
- `OnEnter { get; set; } -> List<QuestAction>`
- `RecommendedLevel { get; set; } -> int` — The game's journal: the recommended level shown with the step.
- `Site { get; set; } -> QuestPlace?` — Where the step happens, found at run time on real ground outside every base, once per world.
- `Spawns { get; set; } -> List<QuestSpawn>` — Creatures the step brings into the world.
- `Text { get; set; } -> UiText`
- `Title { get; set; } -> UiText?` — The game's journal: the step's short name (default its text).
- `Voice { get; set; } -> string?` — The game's journal: the step's voice-over line (a name in the mod's cooked audio, or a GUID).

## QuestStepEvent — A step is about to finish.
- `Next { get; } -> QuestStep?` — The step that follows; null when this is the last.
- `Step { get; } -> QuestStep`
- `StepId { get; } -> string`

## QuestType — The game's QuestType.
enum: Auto, WorldQuest, PlayerQuest

## RosterUnlockChangedEvent `0.2.4` — A roster entry's unlock state changed between two snapshots of one world (a knowledge version change alone, and a world's first snapshot, are not one).
- `After { get; } -> bool` — Unlocked now.
- `Before { get; } -> bool` — Unlocked in the previous snapshot.
- `KnowledgeVersion { get; } -> ushort` — The engine's knowledge version of the new snapshot.
- `NpcId { get; } -> uint` — The entry's NpcId.
- `SnapshotRevision { get; } -> ulong` — The new snapshot's revision.
- `World { get; } -> CapacityWorldToken` — The world whose roster changed.

## RosterWorldResult `0.2.4` — Which world's roster a later read names: the answer of `NativeQuests.GetServerRosterWorld` and `NativeQuests.GetClientRosterWorld`.
- `RosterWorldResult(bool Ok, string? Reason, CapacityWorldToken? World)`
- `Ok { get; init; } -> bool` — True when the world has a complete snapshot: `World` names it.
- `Reason { get; init; } -> string?` — Null when `Ok`; `NoWorld` (no such world in this process now) or `NotReady` (the world has no complete snapshot yet...
- `World { get; init; } -> CapacityWorldToken?` — The world's token while it lives; null when not `Ok`.

## ShrineFoundEvent — A character found a Flame Shrine or Flame Sanctum (the game's record of it; exploration XP is paid with it).
- `Shrine { get; } -> FlameShrine`

## ShrineKind
enum: Shrine, Sanctum

## ShroudRoot — A Shroud Root.
- `ShroudRoot(string Id, string Region, bool ElixirWell, int SkillPoints, WorldPosition? Position, ProgressFact Destruction)`
- `DestroyedBy(Player player) -> bool?`
- `Destruction { get; init; } -> ProgressFact`
- `ElixirWell { get; init; } -> bool` — This root is the one at an Elixir Well (the well counts as cleared when it is destroyed).
- `Id { get; init; } -> string`
- `Position { get; init; } -> WorldPosition?`
- `Region { get; init; } -> string`
- `SkillPoints { get; init; } -> int`

## ShroudRootDestroyedEvent — A character destroyed a Shroud Root (an Elixir Well's when `ShroudRoot.ElixirWell`).
- `Root { get; } -> ShroudRoot`

## Spire — An Ancient Spire: its fast-travel point, the knowledge the game sets when it is unlocked, the per-character "top reached" knowledge and the beacon on top.
- `Spire(string Id, string Template, uint NameTag, WorldPosition Position, ProgressFact Unlock, ProgressFact? TopReached, ProgressFact? Beacon)`
- `Beacon { get; init; } -> ProgressFact?`
- `BeaconLit { get; } -> bool?` — The beacon on top is lit in this world.
- `Id { get; init; } -> string` — Lower-case, from the fast-travel marker's template name (`grassland`, `capital`, ...).
- `NameTag { get; init; } -> uint` — Loca tag of the map marker's name (the catalog's displayName).
- `Position { get; init; } -> WorldPosition`
- `Template { get; init; } -> string`
- `TopReached { get; init; } -> ProgressFact?`
- `TopReachedBy(Player player) -> bool?` — The character reached the top (its own knowledge; the game's spire achievement reads it).
- `Unlock { get; init; } -> ProgressFact`
- `Unlocked { get; } -> bool?` — The spire's fast travel is unlocked in this world (tick thread; null when unreadable).
- `UnlockedFor(Player player) -> bool?` — Unlocked as this character sees it (the world's value, or its own twin in personal progression).

## SpireUnlockedEvent — An Ancient Spire's knowledge was set: the world's unlock (`WorldProgressEvent.IsWorld`), a character's twin of it (personal progression), or a character's "top...
- `Spire { get; } -> Spire`
- `TopReached { get; } -> bool` — The character's own "top reached" knowledge, not the unlock.

## Survivor — A survivor (an NPC of the game's roster with a rescue requirement).
- `Survivor(string Id, uint NameTag, ProgressFact Rescue)`
- `Id { get; init; } -> string`
- `NameTag { get; init; } -> uint`
- `Rescue { get; init; } -> ProgressFact`
- `Rescued { get; } -> bool?`
- `RescuedFor(Player player) -> bool?`

## WorldDifficulty — The world's difficulty: the server's preset and the game settings in effect (kiln.settings).
- `WorldDifficulty(string? Preset, IReadOnlyDictionary<string, object> Settings)`
- `IsCustom { get; } -> bool`
- `Number(string name) -> double?` — A setting as a number (factors, durations in ns, option values); null when absent or not numeric.
- `Preset { get; init; } -> string?` — Default, Relaxed, Hard, Survival or Custom (the game's GameSettingsPreset); null when unknown.
- `Settings { get; init; } -> IReadOnlyDictionary<string, object>` — Every game setting's current value by name, as `enshrouded_server.json` spells it.

## WorldIndexState
enum: NotStarted, Building, Ready, Failed

## WorldProgress — What the game stores about this world's progress.
- `static Available { get; } -> bool` — The knowledge service is loaded (most facts are knowledge).
- `static Boss(string id) -> Boss?` — A boss by id (`enemy_boss_fog_dragon`), or the first of a template's placements by template name.
- `static Bosses { get; } -> IReadOnlyList<Boss>` — Every boss placed in the world (empty until `WorldProgress.BossesReady`).
- `static BossesNear(WorldPosition at, double radius) -> IReadOnlyList<Boss>` — The bosses within `radius` metres of a point, nearest first.
- `static BossesReady { get; } -> bool` — True once the bosses are listed (every chunk of the world read, a few seconds after the first call).
- `static Chest(WorldPosition at, double radius = 3) -> ChestStatus?` — The loot chest placed nearest to `at` within `radius` metres (horizontally), with its state when its entity is loaded.
- `static Chest(uint lootId, WorldPosition near) -> ChestStatus?` — A chest by its loot container id, searched around `near` (its chunk and the ones next to it).
- `static ChestOf(EntityId entity) -> ChestStatus?` — The chest state of a loaded entity (a chest's live entity, e.g. from `World.Find`); null when it is not a chest.
- `static Chests(WorldPosition at, double radius) -> IReadOnlyList<ChestStatus>` — Every loot chest placed within `radius` metres of a point, nearest first.
- `static Difficulty { get; } -> WorldDifficulty?` — The server's preset and the game settings in effect (tick thread).
- `static ElixirWells { get; } -> IReadOnlyList<ShroudRoot>` — The roots at Elixir Wells.
- `static Flame { get; } -> FlameStatus?` — The world's flame (tick thread).
- `static FlameLevel { get; } -> int?` — The Mother Flame's level (tick thread); null when unknown.
- `static For(EntityId player) -> CharacterProgress` — One character's progress (a player in game).
- `static For(Player player) -> CharacterProgress` — One character's progress (a player in game).
- `static LimitsAt(int level) -> FlameLimits?` — What the game derives from a flame level: how many Flame Altars and water sources the world may have, the Shroud...
- `static Obelisk(string id) -> Obelisk?`
- `static Obelisks { get; } -> IReadOnlyList<Obelisk>`
- `static PlacesReady { get; } -> bool` — True once the spires, shrines, roots and obelisks are listed.
- `static SetFlameLevel(int level) -> Task<FlameChange>` — Sets the Mother Flame's level, as the game's own admin command does (the `Level` attribute of the world's progress...
- `static Shrine(string id) -> FlameShrine?`
- `static Shrines { get; } -> IReadOnlyList<FlameShrine>` — Every Flame Shrine and Flame Sanctum the game's achievements count, by region.
- `static ShroudRoot(string id) -> ShroudRoot?`
- `static ShroudRoots { get; } -> IReadOnlyList<ShroudRoot>` — Every Shroud Root the game's achievements count (with positions once `WorldProgress.BossesReady`).
- `static Spire(string id) -> Spire?`
- `static Spires { get; } -> IReadOnlyList<Spire>` — Every Ancient Spire (empty until `WorldProgress.PlacesReady`).
- `static Status { get; } -> WorldProgressStatus`
- `static Survivors { get; } -> IReadOnlyList<Survivor>` — The survivors (NPCs of the game's roster with a rescue requirement) and whether they are rescued (tick thread).
- `static WhenReady(bool bosses = false, TimeSpan? timeout = null) -> Task<bool>` — Waits off the tick thread until the lists are ready (or failed).

## WorldProgressEvent — Base of the progress events whose fact is knowledge: the world's or one character's.
- `IsWorld { get; } -> bool`
- `Player { get; } -> Player?`
- `Step { get; } -> ulong`
- `Target { get; } -> EntityId` — None: the world's knowledge changed; else the player whose knowledge changed.

## WorldProgressStatus — What world progress can read on this server, and how far its index of the game's data is.
- `WorldProgressStatus(bool Knowledge, bool World, bool Settings, bool Assets, WorldIndexState Places, WorldIndexState Bosses, int Spires, int Shrines, int Roots, int Obelisks, int BossCount, int TrackedBosses, double PlacesSeconds, double BossesSeconds, IReadOnlyList<string> Problems)`
- `Assets { get; init; } -> bool`
- `BossCount { get; init; } -> int`
- `Bosses { get; init; } -> WorldIndexState` — Bosses and root positions (every chunk of the scene and every template).
- `BossesSeconds { get; init; } -> double`
- `Knowledge { get; init; } -> bool`
- `Obelisks { get; init; } -> int`
- `Places { get; init; } -> WorldIndexState` — Spires, shrines, roots, obelisks (the query db and the scene's global chunk).
- `PlacesSeconds { get; init; } -> double`
- `Problems { get; init; } -> IReadOnlyList<string>`
- `Roots { get; init; } -> int`
- `Settings { get; init; } -> bool`
- `Shrines { get; init; } -> int`
- `Spires { get; init; } -> int`
- `TrackedBosses { get; init; } -> int`
- `World { get; init; } -> bool`
