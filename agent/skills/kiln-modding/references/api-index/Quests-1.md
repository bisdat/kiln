# Quests (Kiln.Quests): part 1 of 2

## Boss — A boss placed in the world: a creature the game gives a boss health bar, placed directly or by a spawner.
- `Boss(string Id, string Template, string? Spawner, uint NameTag, WorldPosition Position, ProgressFact? Kill)`
- `Id { get; init; } -> string` — The boss template's name, lower-case, with `_n` when the world has several of it.
- `Kill { get; init; } -> ProgressFact?` — The fact that records its kill; null when the game keeps none (see `Boss.Tracked`).
- `Killed { get; } -> bool?` — Killed in this world (tick thread); null when the game keeps no record or it is a character fact.
- `KilledBy(Player player) -> bool?` — Whether this character killed it, for a boss whose kill is a character fact (tick thread; null when the game keeps...
- `NameTag { get; init; } -> uint` — Loca tag of the health bar's name.
- `Position { get; init; } -> WorldPosition` — Where it is placed.
- `Spawner { get; init; } -> string?` — The placed spawner's template, when a spawner places it.
- `Template { get; init; } -> string` — The boss template's name.
- `Tracked { get; } -> bool` — The game records this boss's kill.

## BossKilledEvent — A boss died: a creature the game gives a boss health bar.
- `Boss { get; } -> Boss?` — The placed boss it was (the nearest placement of its template), when known.
- `Entity { get; } -> EntityId` — The dead entity; None when only the kill record was seen.
- `Killer { get; } -> EntityId` — Who dealt the killing blow (a player or a creature), when Kiln saw it.
- `KillerPlayer { get; } -> Player?` — The killer as a player; null when the killer is not a player, is not known, or left.
- `Position { get; } -> WorldPosition?` — Where the boss died; null when only the kill record was seen.
- `Recorded { get; } -> bool` — The game's kill record (knowledge) changed for this boss with the kill.
- `Step { get; } -> ulong` — The server-world step of the event.
- `Template { get; } -> string?` — The boss's template name.

## CharacterProgress — One character's progress, read with the game's evaluator for that player (personal progression included).
- `BossesKilled { get; } -> IReadOnlyList<Boss>` — The bosses this character killed, among those whose kill the game records for a character.
- `ObelisksRead { get; } -> IReadOnlyList<Obelisk>` — The Ancient Obelisks this character read.
- `Player { get; } -> EntityId` — The player entity this progress is for.
- `Quests { get; } -> QuestLog` — The character's quests, vanilla and Kiln (`Quests.Of`).
- `RootsDestroyed { get; } -> IReadOnlyList<ShroudRoot>` — The Shroud Roots this character destroyed.
- `ShrinesFound { get; } -> IReadOnlyList<FlameShrine>` — The Flame Shrines and Sanctums this character found.
- `SpiresUnlocked { get; } -> IReadOnlyList<Spire>` — The spires unlocked as this character sees it.
- `SpireTopsReached { get; } -> IReadOnlyList<Spire>` — The spires whose top this character reached.

## ChestLootedEvent — A loaded chest was emptied after its loot was generated.
- `Chest { get; } -> ChestStatus` — The chest that was emptied.
- `NearestPlayer { get; } -> EntityId` — The player nearest to the chest at that time.

## ChestStatus — A loot chest placed in the world and its state.
- `ChestStatus(uint LootId, string Template, WorldPosition Position, EntityId Entity, bool? Opened, bool? Looted)`
- `Entity { get; init; } -> EntityId` — The chest's entity while its area is loaded; `EntityId.None` otherwise.
- `Loaded { get; } -> bool` — True when the chest's entity exists (its area is loaded), so the state can be read.
- `Looted { get; init; } -> bool?` — Opened and emptied.
- `LootId { get; init; } -> uint` — The game's loot container id of the placement (stable for a game build).
- `Opened { get; init; } -> bool?` — The chest's loot was generated (it was opened at least once).
- `Position { get; init; } -> WorldPosition` — Where the chest is placed.
- `Template { get; init; } -> string` — The chest's template name.

## ConversationOptionEvent — A player chose an option of a Kiln conversation.
- `Dialogue { get; } -> DialogueDefinition` — The conversation.
- `Holder { get; } -> QuestHolder` — The player (holder) who chose.
- `Node { get; } -> string` — The id of the node the option is in.
- `Npc { get; } -> EntityId` — The NPC being talked to.
- `Option { get; } -> int` — The option's place in the node's list.
- `RefuseMessage { get; set; } -> string?` — The line to tell the player when you cancel the event.

## ConversationResult — The answer to a conversation request.
- `ConversationResult(bool Ok, string Reason)`
- `Ok { get; init; } -> bool` — True when the request was accepted; false when it was refused.
- `Reason { get; init; } -> string` — Why it was refused; empty when `Ok`.

## ConversationView — What a holder is being shown (tick thread; for tools and tests).
- `ConversationView(long Id, string Dialogue, string Node, UiText? Speaker, UiText Text, IReadOnlyList<(int Index, UiText Text)> Options, EntityId Npc)`
- `Dialogue { get; init; } -> string` — The dialogue's full id.
- `Id { get; init; } -> long` — The conversation's id (unique for each opened conversation).
- `Node { get; init; } -> string` — The id of the node being shown.
- `Npc { get; init; } -> EntityId` — The NPC entity being talked to.
- `Options { get; init; } -> IReadOnlyList<(int Index, UiText Text)>` — The options shown: each one's index in the node's option list and its text.
- `Speaker { get; init; } -> UiText?` — The speaker's name for this node: the node's own, else the NPC's.
- `Text { get; init; } -> UiText` — What the NPC says at this node.

## DialogueAction — What a player did with a dialogue line.
enum: Start, Stop

## DialogueDefinition — A conversation: nodes of text and options, per NPC or per quest.
- `Farewell { get; set; } -> List<UiText>` — The game's menu: replaces the NPC's farewell lines.
- `FullId { get; } -> string` — `<modId>:<id>`, or just the id outside a mod.
- `Greet { get; set; } -> bool?` — The game's menu: the start node's text is a greeting.
- `Greetings { get; set; } -> List<DialogueGreeting>` — The game's menu: more greetings, shown before the plain one while their condition holds.
- `Hint { get; set; } -> string?` — The game's menu: the marker over the NPC's head on the greeting (Hint, Attention, MainQuest).
- `Id { get; init; } -> string` — The dialogue's id.
- `ModId { get; } -> string` — The mod that defined it ("" for code outside a mod).
- `Native { get; set; } -> bool` — The default: its lines are in the game's own NPC menu, with the game's voice playback.
- `Nodes { get; set; } -> Dictionary<string, DialogueNode>` — The conversation's nodes by id.
- `Start { get; set; } -> string` — The id of the node the conversation opens at (default `start`).
- `Transition { get; set; } -> List<UiText>` — The game's menu: replaces the line between topics.

## DialogueGreeting — A greeting that wins over the plain one while its condition holds.
- `Hint { get; set; } -> string?` — The marker over the NPC's head on this greeting (Hint, Attention, MainQuest).
- `If { get; set; } -> QuestCondition?` — Shown only while this holds.
- `Text { get; set; } -> UiText` — The greeting line.
- `Voice { get; set; } -> string?` — A voice-over line for it: a name in the mod's cooked audio, or a GUID.

## DialogueKnowledgeEvent — The knowledge a dialogue line is about to run for a player (its knowledgeUnlockOperations and its "heard" knowledge), held by Kiln for a decision.
- `CheckError { get; } -> bool` — A check could not run (counted as passed).
- `ChecksOn { get; } -> bool` — The server setting checkDialogue is on.
- `DefaultApplies { get; } -> bool` — True when the knowledge runs if no handler decides: false when the `checkDialogue` setting is on and the line's...
- `Distance { get; } -> double?` — Metres between player and NPC, null when unknown.
- `LineId { get; } -> uint` — The dialogue line's option id.
- `Npc { get; } -> EntityId` — The NPC's entity.
- `Operations { get; } -> IReadOnlyList<KnowledgeOperation>` — What would run: the line's operations, then {line id, Bool, 1} ("heard").
- `Player { get; } -> Player?` — The player; null when they left in the meantime.
- `PlayerEntity { get; } -> EntityId` — The player's entity.
- `RequirementFailed { get; } -> bool` — The line's requirement is false for this player on the server (a vanilla client never offers such a line).
- `Step { get; } -> ulong` — The server-world step of the choice.
- `TooFar { get; } -> bool` — The player stands farther from the NPC than the server's dialogueRange.

## DialogueLineEvent — A player picked (or stopped) a dialogue line of an NPC: the game's ServerSelectDialogOptionEvent, read by Kiln's program kiln_knowledge.
- `Action { get; } -> DialogueAction` — Whether the player picked the line or stopped it.
- `IsFarewell { get; } -> bool` — True when `DialogueLineEvent.LineId` is 0: a farewell (the server picks a random farewell line).
- `IsMissingRequirement { get; } -> bool` — True when `DialogueLineEvent.LineId` is 1: the client found the option's requirement missing, and the NPC plays its...
- `LineId { get; } -> uint` — The line's dialogue option id (also its "heard" knowledge id); 0 for a farewell, 1 for an option whose requirement...
- `Npc { get; } -> EntityId` — The NPC's entity.
- `Player { get; } -> Player?` — The player; null when they left in the meantime.
- `PlayerEntity { get; } -> EntityId` — The player's entity.
- `Step { get; } -> ulong` — The server-world step of the choice.

## DialogueNode — One node of a conversation: some text from the NPC and the options the player can pick.
- `Do { get; set; } -> List<QuestAction>` — Actions when the node opens.
- `Goto { get; set; } -> string?` — A node without options: where "Continue" goes (default: the conversation ends).
- `Hint { get; set; } -> string?` — The game's menu: the marker over the NPC's head on this node's greeting.
- `Options { get; set; } -> List<DialogueOption>` — The choices the player can pick at this node.
- `Speaker { get; set; } -> UiText?` — Overrides the NPC's name for this node.
- `Text { get; set; } -> UiText` — What the NPC says at this node.
- `Voice { get; set; } -> string?` — A voice-over line, played by the game's dialogue: a name in the mod's cooked audio, or a GUID (any line of the game).

## DialogueOption — One choice the player can pick at a node.
- `Close { get; set; } -> bool` — The game's menu: the NPC plays its outro after the reply.
- `Do { get; set; } -> List<QuestAction>` — Actions that run when the option is chosen.
- `Goto { get; set; } -> string?` — The next node; "end" or null ends the conversation.
- `Hint { get; set; } -> string?` — The game's menu: the topic's marker (Hint, Attention, MainQuest).
- `If { get; set; } -> QuestCondition?` — Shown only while this holds; checked again when chosen.
- `Once { get; set; } -> bool` — The game's menu: the topic goes away once heard.
- `Reply { get; set; } -> UiText?` — The game's menu: what the NPC answers.
- `Text { get; set; } -> UiText` — The option's line.
- `Voice { get; set; } -> string?` — The game's menu: the reply's voice-over.

## Dialogues — Dialogue graphs: lines in the game's own NPC menu, or chat conversations for mods vanilla clients join.
- `static ApplyOption(EntityId player, EntityId npc, string fullDialogueId, string nodeId, int optionIndex) -> Task<ConversationResult>` — The game's own dialogue chose an option that a mod's quest file defines (Kiln.Quests/Native maps the line to the...
- `static Choose(QuestHolder h, long conversation, int option) -> Task<ConversationResult>` — Runs a choice: `option` is the option's place in the node (-1: continue a node without options).
- `static Close(QuestHolder h) -> void` — Ends a holder's conversation.
- `static Define(DialogueDefinition dialogue) -> DialogueDefinition` — Registers a dialogue as the calling mod's (it goes away when the mod unloads).
- `static Definitions { get; } -> IReadOnlyList<DialogueDefinition>` — Every dialogue defined now.
- `static Find(string id) -> DialogueDefinition?` — A dialogue by full id, or by its own id when only one mod has it.
- `static Open(EntityId player, string dialogue, EntityId npc = default(EntityId), string? node = null) -> ConversationResult` — Opens a dialogue for a player in game (at `node`, default its start).
- `static Talk(QuestHolder h, string? npc = null) -> ConversationResult` — A player talks to the NPC near them (or, for a synthetic holder, the one named): a conversation, or the quests that...
- `static View(QuestHolder h) -> ConversationView?` — What a holder is being shown; null when no conversation is open.

## FlameChange — The outcome of `WorldProgress.SetFlameLevel`.
- `FlameChange(ulong Request, bool Ok, string Status, int Requested, int Before, int After, int Max, bool Clamped, bool Marked, bool Mirrored, int Steps, string Reason)`
- `After { get; init; } -> int` — The stored level after the write (once the game's attribute formula ran).
- `Before { get; init; } -> int` — The stored level before and after the write (after: once the game's attribute formula ran).
- `Clamped { get; init; } -> bool` — The game's formula limited the value.
- `Marked { get; init; } -> bool` — The component was marked written (so the game saves and replicates it).
- `Max { get; init; } -> int` — The game's maximum level (0 when unknown).
- `Mirrored { get; init; } -> bool` — `NetworkServerProgress.level` followed (what players see).
- `Ok { get; init; } -> bool` — The requested write completed (see `FlameChange.Mirrored`).
- `Reason { get; init; } -> string` — Why the write failed or was limited; empty on a plain success.
- `Request { get; init; } -> ulong` — The native request id (0 when refused at once).
- `Requested { get; init; } -> int` — What was asked for.
- `Status { get; init; } -> string` — "ok", "unavailable", "internal", ...
- `Steps { get; init; } -> int` — Steps from the write until it was mirrored.

## FlameLevelChangedEvent — The world's flame level changed (the Flame Altar's upgrade, or an admin's set-flame-level).
- `Flame { get; } -> FlameStatus` — The flame as it stands after the change.
- `NewLevel { get; } -> int` — The level after the change.
- `OldLevel { get; } -> int` — The level before the change.

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
- `Discovery { get; init; } -> ProgressFact` — The fact that a character found it.
- `FoundBy(Player player) -> bool?` — Whether this character found it (tick thread; null when it cannot be read).
- `Id { get; init; } -> string` — Lower-case, from the game's achievement query (`grassland_flameshrine_01`).
- `Kind { get; init; } -> ShrineKind` — A Flame Shrine or a Flame Sanctum.
- `NameTag { get; init; } -> uint` — Loca tag of the map marker's name.
- `Position { get; init; } -> WorldPosition?` — Where it is; null when it is not known.
- `Region { get; init; } -> string` — The region, from the game's achievement name (such as `grassland`).
- `Template { get; init; } -> string?` — The marker's template name; null when it is not known.

## FlameStatus — The world's flame: the Mother Flame's level and the flame altars (the game's NetworkServerProgress on the world's progress entity, the level the Flame Altar menu shows).
- `FlameStatus(int Level, int Altars, int HighestAltarLevel, int WaterSources, EntityId Entity)`
- `Altars { get; init; } -> int` — The number of Flame Altars that exist now (not the permitted maximum: see `WorldProgress.LimitsAt`).
- `Entity { get; init; } -> EntityId` — The world's progress entity.
- `HighestAltarLevel { get; init; } -> int` — The highest level among the existing Flame Altars.
- `Level { get; init; } -> int` — The Mother Flame's level as the game replicates it (the level the Flame Altar menu shows).
- `Max { get; init; } -> int?` — The game's maximum flame level (the second value of the `Level` attribute, `flameLevelMax`).
- `Stored { get; init; } -> int?` — The `Level` attribute of the progress entity: the store the game writes when the flame levels up (and its admin...
- `SystemLevel { get; init; } -> int?` — What the game's own altar, water-source and fog rules read (`ServerProgressSystem.progressLevel`).
- `WaterSources { get; init; } -> int` — The number of water sources that exist now (not the permitted maximum).

## Journal — The game's journal of vanilla quests (server side; the client builds its quest log from its own copy and the replicated knowledge, so granting a step's visibility...
- `static Find(string name) -> JournalQuest?` — A quest by "quest:0x...", "0x..." or its entry id; null when there is none.
- `static Find(uint entryId) -> JournalQuest?` — A quest by its entry id; null when there is none.
- `static Quests() -> IReadOnlyList<JournalQuest>` — Every journal quest (tick thread; empty until the catalog exists, a few steps after the world started).
- `static State(EntityId target, JournalQuest quest) -> JournalQuestState` — The quest's state for the world (`target` 0) or a player (tick thread): every step's visibility and completion query...
- `static State(Player player, JournalQuest quest) -> JournalQuestState` — The quest's state for a player, as `Journal.State` (tick thread).
- `static States(EntityId target) -> IReadOnlyList<JournalQuestState>` — Every quest's state for a target (tick thread).

## JournalQuest — A journal quest (JournalQuestResource).
- `JournalQuest(int Index, uint EntryId, uint NameTag, QuestSource Source, QuestType Type, bool IsMain, bool IsTutorial, bool ForAllPlayers, int Priority, IReadOnlyList<JournalStep> Steps, int Rewards, IReadOnlyList<uint> RewardExperience, IReadOnlyList<uint> RewardItems)`
- `EntryId { get; init; } -> uint` — The quest's journal entry id.
- `ForAllPlayers { get; init; } -> bool` — The game unlocks it for all players (`unlockForAllPlayers`).
- `Index { get; init; } -> int` — The quest's position in the game's journal registry.
- `IsMain { get; init; } -> bool` — The game marks it as a main quest.
- `IsTutorial { get; init; } -> bool` — The game marks it as a tutorial quest.
- `Name { get; } -> string` — "quest:0x\<entryId>".
- `NameTag { get; init; } -> uint` — The loca tag of the quest's name (the text is client data).
- `Priority { get; init; } -> int` — Its place in the journal's list.
- `RewardExperience { get; init; } -> IReadOnlyList<uint>` — The experience value ids (from the game's balancing data) of the first rewards, at most four; empty slots are left out.
- `RewardItems { get; init; } -> IReadOnlyList<uint>` — The item ids of the first rewards, at most four; empty slots are left out.
- `Rewards { get; init; } -> int` — The number of rewards the quest has.
- `Source { get; init; } -> QuestSource` — Who gives the quest: the portrait the journal shows.
- `Steps { get; init; } -> IReadOnlyList<JournalStep>` — The quest's steps, in order.
- `Type { get; init; } -> QuestType` — The game's quest type.

## JournalQuestState — A quest's state for the world (`JournalQuestState.Target` 0: shared progress, as the world's journal entries are driven) or one player.
- `JournalQuestState(JournalQuest Quest, EntityId Target, IReadOnlyList<JournalStepState> Steps, bool? Rewarded)`
- `Completed { get; } -> bool?` — The last step's completion query holds (the game then pays the reward).
- `Quest { get; init; } -> JournalQuest` — The quest this state is for.
- `Rewarded { get; init; } -> bool?` — The player's "rewarded" knowledge (the game paid the quest's XP); null for the world.
- `Started { get; } -> bool` — At least one step is shown in the journal.
- `Steps { get; init; } -> IReadOnlyList<JournalStepState>` — The state of each step, in order.
- `Target { get; init; } -> EntityId` — The player entity the state is for; `EntityId.None` for the world's shared progress.
- `VisibleSteps { get; } -> int` — How many steps are shown in the journal.

## JournalStep — One step of a journal quest (JournalEntryResource): shown while `JournalStep.Visible` holds, done when `JournalStep.Done` holds (in all but one vanilla quest every...
- `JournalStep(int Index, uint EntryId, uint NameTag, uint TextTag, uint MapMarkerTag, KnowledgeQuery Visible, KnowledgeQuery Done, int ProgressSteps, int RecommendedLevel)`
- `Done { get; init; } -> KnowledgeQuery` — The knowledge requirement that completes the step.
- `EntryId { get; init; } -> uint` — The step's journal entry id.
- `Index { get; init; } -> int` — The step's 0-based position in its quest.
- `MapMarkerTag { get; init; } -> uint` — The marker id tag of the step's map marker; 0 for none.
- `NameOf(JournalQuest quest) -> string` — "quest:0x\<quest>.step\<n>" (add ".visible" / ".done" for the requirement names).
- `NameTag { get; init; } -> uint` — The loca tag of the step's short name (the text is client data).
- `ProgressSteps { get; init; } -> int` — How many progress entries the step's counter requirement has.
- `RecommendedLevel { get; init; } -> int` — The level the journal recommends for the step.
- `TextTag { get; init; } -> uint` — The loca tag of the step's paragraph (the text is client data).
- `Visible { get; init; } -> KnowledgeQuery` — The knowledge requirement that shows the step in the journal.

## JournalStepState — A step's state; null where it could not be evaluated (a player query asked for the world).
- `JournalStepState(JournalStep Step, bool? Visible, bool? Done)`
- `Done { get; init; } -> bool?` — Whether the step is done; null when it could not be evaluated.
- `Step { get; init; } -> JournalStep` — The step this state is for.
- `Visible { get; init; } -> bool?` — Whether the step is shown in the journal; null when it could not be evaluated.

## Knowledge — The game's knowledge: every unlock, quest step, line heard and NPC rescued (kiln.knowledge).
- `static Available { get; } -> bool` — True when kiln.knowledge is loaded (see `Knowledge.Status` for its parts).
- `static Catalog(KnowledgeCatalog catalog) -> IReadOnlyList<KnowledgeName>` — The catalog's names (NPC unlocks, journal quests, named queries).
- `static FindNpc(string name) -> KnowledgeNpc?` — The NPC with this debug name ("NPC_Workshop_Bard01") or "npc:" name; null when there is none.
- `static For(EntityId player) -> KnowledgeScope` — One player's knowledge (while they are on the server).
- `static For(Player player) -> KnowledgeScope` — One player's knowledge, as `Knowledge.For`.
- `static Get(EntityId target, uint id) -> KnowledgeState?` — An id in a target's map (tick thread).
- `static Get(Player player, string name) -> KnowledgeState?` — The name's state in the player's map (tick thread), as `KnowledgeScope.Get`.
- `static Grant(EntityId player, string name, uint value = 1) -> Task<KnowledgeResult>` — Grants `name` to a player, as `KnowledgeScope.Grant`.
- `static Grant(Player player, string name, uint value = 1) -> Task<KnowledgeResult>` — Grants `name` to a player, as `KnowledgeScope.Grant`.
- `static GrantUnchecked(EntityId target, uint id) -> Task<KnowledgeResult>` — A grant the game's apply gets even when `id` is in no static map, where the game drops it (`KnowledgeResult.After`...
- `static Has(EntityId target, string name) -> bool` — True when the name's requirement holds for the target (see `KnowledgeScope.Has`).
- `static Has(Player player, string name) -> bool` — True when the name holds for the player (tick thread), as `Knowledge.Has`.
- `static Info(uint id) -> KnowledgeInfo?` — What GameKnowledgeResource says about an id; null when the game does not know it.
- `static List(EntityId target) -> IReadOnlyList<KnowledgeState>` — Every set id of a target (tick thread).
- `static NameOf(uint id, bool query = false) -> string?` — The catalog name of a knowledge id ("npc:....unlock", "quest:0x...") or of a query id ("query:..."); null when it...
- `static Npcs { get; } -> IReadOnlyList<KnowledgeNpc>` — The game's NPC roster with the registry's unlock state (tick thread; empty before the catalog exists).
- `static Query(EntityId target, KnowledgeQuery query, KnowledgeMode mode = KnowledgeMode.Player) -> bool` — Evaluates a requirement with the game's evaluator (named queries, NPC / inventory / flame runtime queries included)...
- `static Resolve(string name) -> KnowledgeName` — Resolves a name: "0x1234abcd" / decimal, "npc:\<debugName>[.unlock]", "quest:0x\<entryId>" (the quest's "rewarded"...
- `static Revoke(EntityId player, string name, int flag = -1) -> Task<KnowledgeResult>` — Revokes `name` for a player, as `KnowledgeScope.Revoke`.
- `static Revoke(Player player, string name, int flag = -1) -> Task<KnowledgeResult>` — Revokes `name` for a player, as `KnowledgeScope.Revoke`.
- `static Status { get; } -> KnowledgeStatus?` — What kiln.knowledge can do on this build, and its counters; null when the service is not loaded.
- `static Submit(KnowledgeOp op, EntityId target, uint id, uint value) -> Task<KnowledgeResult>` — Queues a change (any thread).
- `static TryParseId(string s, out uint id) -> bool` — Parses a raw id: "0x1234abcd" or decimal.
- `static TryQuery(EntityId target, KnowledgeQuery query, KnowledgeMode mode = KnowledgeMode.Player) -> bool?` — Evaluates a requirement; null when it cannot be (no player for a player query, unknown query, part off).
- `static TryResolve(string name, out KnowledgeName result) -> bool` — Resolves a name like `Knowledge.Resolve`, without throwing: false for an unknown name or when the catalog is not...
- `static World { get; } -> KnowledgeScope` — The world's (shared) knowledge.

## Knowledge.Security — The opt-in server checks (kiln/config/knowledge.json; both off by default).
- `static Allow(IEnumerable<uint> ids) -> void` — Ids that pass the client veto at run time (in addition to the file's allowClientKnowledge).
- `static Apply(KnowledgeSecurity settings, bool save = false) -> void` — Applies settings now (until kiln/config/knowledge.json changes).
- `static Current { get; } -> KnowledgeSecurity` — The settings in use.

## KnowledgeCatalog — Catalogs of `Knowledge.Catalog`.
enum: Npcs, Quests, Queries

## KnowledgeChangedEvent — A knowledge value changed in the world map or a player's map: the game (a quest step, a dialogue line, a rescue, loot, a query trigger, ...) or a Kiln change...
- `ByKiln { get; } -> bool` — True when a Kiln request caused the change (`KnowledgeChangedEvent.Request` is not 0).
- `Gained { get; } -> bool` — True when the value went from 0 to a non-zero value.
- `Id { get; } -> uint` — The knowledge id that changed.
- `IsWorld { get; } -> bool` — True when the world's map changed.
- `Lost { get; } -> bool` — True when the value went from non-zero to 0 (a revoke).
- `Name { get; } -> string?` — The catalog name ("npc:....unlock", "quest:0x..."), or null.
- `NewValue { get; } -> uint` — The value after the change.
- `OldValue { get; } -> uint` — The value before the change.
- `Player { get; } -> Player?` — The player (null for the world, or if they left in the meantime).
- `Request { get; } -> ulong` — The Kiln request that caused it (`KnowledgeResult.Request`), 0 = the game.
- `Step { get; } -> ulong` — The server-world step in which the game applied the change.
- `Target { get; } -> EntityId` — 0 = the world map, else the player whose map changed.
- `Type { get; } -> KnowledgeType` — The knowledge object's type.

## KnowledgeCompare — How a requirement compares a knowledge object's value with its compare value.
enum: GreaterThan, LessThan, Equals

## KnowledgeInfo — What GameKnowledgeResource says about one id (`Knowledge.Info`).
- `KnowledgeInfo()`
- `KnowledgeInfo(uint Id, KnowledgeObjectFlags Flags, KnowledgeType WorldType, KnowledgeType PlayerType, uint WorldFlags, uint PlayerFlags, uint ExperienceId, int SkillPoints)`
- `ExperienceId { get; init; } -> uint` — For a player object that pays XP: the game's experience value id; 0 for none.
- `Flags { get; init; } -> KnowledgeObjectFlags` — Which kinds of object the id is (world, player, twin, experience, skill points...).
- `Id { get; init; } -> uint` — The knowledge id.
- `IsPlayer { get; } -> bool` — True when the id is a player object.
- `IsWorld { get; } -> bool` — True when the id is a world object.
- `Known { get; } -> bool` — True when the game knows the id (it has at least one flag).
- `PlayerFlags { get; init; } -> uint` — For a Flags player object: the bits that exist.
- `PlayerType { get; init; } -> KnowledgeType` — The type of the id's player object; `KnowledgeType.None` when it has none.
- `SkillPoints { get; init; } -> int` — For a player object that gives skill points: how many.
- `WorldFlags { get; init; } -> uint` — For a Flags world object: the bits that exist.
- `WorldType { get; init; } -> KnowledgeType` — The type of the id's world object; `KnowledgeType.None` when it has none.

## KnowledgeMode — How a player's requirement reads knowledge that has a world object and a player twin.
enum: Shared, Player, Personal

## KnowledgeName — What a name means (`Knowledge.Resolve`).
- `KnowledgeName(string Text, bool IsQuery, uint Id, KnowledgeQuery Requirement)`
- `Id { get; init; } -> uint` — The knowledge id (or the query id when `IsQuery`).
- `IsQuery { get; init; } -> bool` — The name is a named query (evaluate with `Knowledge.Query`; it cannot be granted).
- `Requirement { get; init; } -> KnowledgeQuery` — The full requirement behind the name: an NPC's unlock requirement, a quest step's query; a plain knowledge name: "is...
- `Text { get; init; } -> string` — The name as it was asked for.

## KnowledgeNpc — An NPC of the game's roster (NpcCollection) with the NPC registry's unlock state.
- `KnowledgeNpc(int Index, string DebugName, uint NpcId, uint NameTag, uint Workshop, uint SummonItem, KnowledgeQuery Unlock, bool? Unlocked, bool CraftingAssistant)`
- `CraftingAssistant { get; init; } -> bool` — Whether the NPC is a crafting assistant.
- `DebugName { get; init; } -> string` — The NPC's debug name, such as `NPC_Workshop_Bard01`.
- `Index { get; init; } -> int` — The NPC's index in the roster.
- `Name { get; } -> string` — "npc:\<debugName>".
- `NameTag { get; init; } -> uint` — The loca tag of the NPC's name.
- `NpcId { get; init; } -> uint` — The game's NPC id (FNV-1a of the NPC template's GUID).
- `SummonItem { get; init; } -> uint` — The item id of the NPC's summon tool.
- `Unlock { get; init; } -> KnowledgeQuery` — The requirement that unlocks the NPC.
- `Unlocked { get; init; } -> bool?` — Whether the game's NPC registry says the NPC is unlocked; null when it is not known.
- `Workshop { get; init; } -> uint` — The NPC's workshop id; 0 for none.

## KnowledgeObjectFlags — What GameKnowledgeResource says about an id.
enum: None, World, Player, Twin, Analytics, Experience, SkillPoints

## KnowledgeOp — A change to the game's knowledge.
enum: Grant, Revoke, Set

## KnowledgeOperation — One knowledge operation (the game's KnowledgeUnlockOperation) and the player it is for.
- `KnowledgeOperation()`
- `KnowledgeOperation(uint Id, KnowledgeType Type, uint Value, EntityId Player)`
- `Id { get; init; } -> uint` — The knowledge id.
- `Player { get; init; } -> EntityId` — The player entity the operation is for.
- `Type { get; init; } -> KnowledgeType` — The knowledge object's type.
- `Value { get; init; } -> uint` — The operation's value, as the game stores it; what it means follows `Type`.

## KnowledgeQuery — A requirement exactly as the game stores it (KnowledgeQueryData): a knowledge object compared to a value, or a named query (`KnowledgeQueryType.Extern`).
- `KnowledgeQuery()`
- `KnowledgeQuery(uint Id, KnowledgeQueryType Type, KnowledgeCompare Compare, uint CompareValue, bool PlayerOnly = false)`
- `Compare { get; init; } -> KnowledgeCompare` — How the object's value is compared with `CompareValue`.
- `CompareValue { get; init; } -> uint` — The value to compare with.
- `Deconstruct(out uint Id, out KnowledgeQueryType Type, out KnowledgeCompare Compare, out uint CompareValue, out bool PlayerOnly) -> void`
- `Equals(KnowledgeQuery other) -> bool`
- `Id { get; init; } -> uint` — The knowledge id to compare, or the named query's id when `Type` is `KnowledgeQueryType.Extern`.
- `IsEmpty { get; } -> bool` — True when `Id` is 0: there is no requirement.
- `static IsSet(uint id) -> KnowledgeQuery` — "this Bool knowledge is set".
- `static operator !=(KnowledgeQuery left, KnowledgeQuery right) -> bool`
- `static operator ==(KnowledgeQuery left, KnowledgeQuery right) -> bool`
- `PlayerOnly { get; init; } -> bool` — Read the player's own object even in shared progression (the game's `isExplicitPlayerKnowledgeQuery`).
- `Type { get; init; } -> KnowledgeQueryType` — The kind of requirement.

## KnowledgeQueryType — The kind of a requirement (the game's KnowledgeQueryDataType).
enum: Extern, SimpleBool, SimpleCount, SimpleFlag

## KnowledgeRequestEvent — A client asked the server for knowledge itself (the player input TriggerKnowledgeAction; the vanilla client does this for some tutorial and UI knowledge).
- `Held { get; } -> bool` — The veto holds it: the decision of the handlers counts.
- `Id { get; } -> uint` — The knowledge id the client asked for.
- `Name { get; } -> string?` — The catalog name of the id (`npc:...`, `quest:0x...`), or null.
- `Player { get; } -> Player?` — The player; null when they left in the meantime.
- `PlayerEntity { get; } -> EntityId` — The entity of the player whose client sent the request.
- `Step { get; } -> ulong` — The server-world step of the request.
- `Type { get; } -> KnowledgeType` — The type of the object the client asked for.
- `Value { get; } -> uint` — The value the client sent.

## KnowledgeResult — What a change did.
- `KnowledgeResult(ulong Request, KnowledgeOp Op, EntityId Target, uint Id, bool Ok, string Status, uint Before, uint After, bool Changed, int Steps, string Reason)`
- `After { get; init; } -> uint` — The target's value after the apply.
- `Before { get; init; } -> uint` — The target's value before the apply.
- `Changed { get; init; } -> bool` — True when the value changed.
- `Id { get; init; } -> uint` — The knowledge id.
- `Ok { get; init; } -> bool` — True when the game applied the request.
- `Op { get; init; } -> KnowledgeOp` — The change that was asked for.
- `Reason { get; init; } -> string` — Why a change was refused or did nothing, in words; empty on a plain success.
- `Request { get; init; } -> ulong` — The id of the request this answers; 0 when it was refused at once.
- `Status { get; init; } -> string` — The result code as text.
- `Steps { get; init; } -> int` — The number of server steps from the request to the apply.
- `Target { get; init; } -> EntityId` — The target's entity; `EntityId.None` for the world.

## KnowledgeScope — The knowledge of one target: the world (`Knowledge.World`) or one player (`Knowledge.For`).
- `Get(string name) -> KnowledgeState?` — The name's state, as `KnowledgeScope.Get` (tick thread).
- `Get(uint id) -> KnowledgeState?` — The id's state (tick thread); null when the id is not an object of this kind of target.
- `Grant(string name, uint value = 1) -> Task<KnowledgeResult>` — Grants a name, as `KnowledgeScope.Grant`.
- `Grant(uint id, uint value = 1) -> Task<KnowledgeResult>` — Grants through the game's own knowledge apply (XP, triggers, replication: the game's).
- `Has(string name) -> bool` — True when the name holds, as `KnowledgeScope.Has`; a query is evaluated (tick thread).
- `Has(uint id) -> bool` — True when the id is set; for a name with a requirement (an NPC's unlock, a quest step, a named query) the...
- `IsWorld { get; } -> bool` — True for the world's knowledge.
- `List() -> IReadOnlyList<KnowledgeState>` — Every set id of this target (tick thread).
- `Revoke(string name, int flag = -1) -> Task<KnowledgeResult>` — Revokes a name, as `KnowledgeScope.Revoke` (tick thread).
- `Revoke(uint id, int flag = -1) -> Task<KnowledgeResult>` — Clears the id (Flags: one flag, or all with -1).
- `Set(string name, uint value) -> Task<KnowledgeResult>` — Writes an exact value for a name, as `KnowledgeScope.Set` (tick thread).
- `Set(uint id, uint value) -> Task<KnowledgeResult>` — Writes an exact value (a direct write: no XP, no query-trigger event).
- `Target { get; } -> EntityId` — 0 = the world.

## KnowledgeSecurity — The opt-in checks (`Security`).
- `KnowledgeSecurity(bool VetoClientKnowledge = false, bool CheckDialogue = false, double DialogueRange = 12, bool LogClientKnowledge = true, int AllowedIds = 0)`
- `AllowedIds { get; init; } -> int` — The number of ids in the client veto's allow list (read-only information: `Security.Apply` ignores it; use...
- `CheckDialogue { get; init; } -> bool` — Hold a dialogue line's knowledge and run it only if the line's requirement holds for the player and the player is...
- `DialogueRange { get; init; } -> double` — The most metres between player and NPC for `CheckDialogue` (default 12; 0 means no distance check).
- `LogClientKnowledge { get; init; } -> bool` — Log each distinct knowledge id a client sends, once per player and session (default true).
- `VetoClientKnowledge { get; init; } -> bool` — Hold every knowledge operation a client sends (TriggerKnowledgeAction) and drop it unless its id is allowed or a...

## KnowledgeSettings — The file's schema (Kiln's config "knowledge").
- `AllowClientKnowledge { get; set; } -> List<string>` — Knowledge ids or names that pass the client veto, such as `0x1234abcd`, `npc:NPC_Workshop_Bard01.unlock` or...
- `CheckDialogue { get; set; } -> bool` — Hold the knowledge of a dialogue line a client picked and run it only if the line's requirement holds for that...
- `DialogueRange { get; set; } -> double` — Metres between player and NPC for `KnowledgeSettings.CheckDialogue` (0: no distance check; default 12).
- `LogClientKnowledge { get; set; } -> bool` — Log each distinct knowledge id a client sends, once per player and session (default on).
- `VetoClientKnowledge { get; set; } -> bool` — Hold every knowledge operation a client sends (`TriggerKnowledgeAction`) and drop it unless its id is in...

## KnowledgeState — One id in one map, read at `KnowledgeState.Step`.
- `KnowledgeState()`
- `KnowledgeState(EntityId Target, uint Id, uint Value, KnowledgeType Type, bool Present, byte NewMask, ulong Step)`
- `HasFlag(int flag) -> bool` — For a Flags object: true when flag number `flag` (0 to 31) is set.
- `Id { get; init; } -> uint` — The knowledge id.
- `IsSet { get; } -> bool` — True when the value is not 0.
- `NewMask { get; init; } -> byte` — The game's "new" markers (bits 32 to 39 of the state); 0xff for an id without an entry.
- `Present { get; init; } -> bool` — The map has an entry (it can hold 0 after a revoke).
- `Step { get; init; } -> ulong` — The server-world step of the read.
- `Target { get; init; } -> EntityId` — 0 = the world map, else the player.
- `Type { get; init; } -> KnowledgeType` — The object's type in this target.
- `Value { get; init; } -> uint` — Bool 0/1, Counter n, Flags mask; 0 = not set.

## KnowledgeStatus — What kiln.knowledge can do on this build, and its counters.
- `KnowledgeStatus(bool Knowledge, bool Changes, bool Journal, bool Npc, bool Dialogue, bool ClientInput, bool Program, bool World, int WorldObjects, int PlayerObjects, int WorldEntries, int Players, int Npcs, int Quests, int Queries, int Pending, ulong Submitted, ulong Applied, ulong Failed, ulong ChangesSeen, ulong ClientOps, ulong ClientVetoed, ulong DialogueEvents, ulong DialogueHeld, ulong DialogueDropped, int QueueMax, string Reason, bool Applying = false)`
- `Applied { get; init; } -> ulong` — The number of requests the game applied.
- `Applying { get; init; } -> bool` — The game's knowledge update has run in this world (the world runs and the save's knowledge is in the maps).
- `Changes { get; init; } -> bool` — The in-step apply works: revoke, set, ordered grants and change events (feature `quests.changes`).
- `ChangesSeen { get; init; } -> ulong` — The number of change events produced.
- `ClientInput { get; init; } -> bool` — The hold on knowledge that clients send works (feature `quests.client_input`).
- `ClientOps { get; init; } -> ulong` — The number of knowledge operations clients sent.
- `ClientVetoed { get; init; } -> ulong` — The number of those operations that were dropped by the client veto.
- `Dialogue { get; init; } -> bool` — Dialogue events and holds work (feature `quests.dialogue`).
- `DialogueDropped { get; init; } -> ulong` — The number of held dialogue knowledge operations that were dropped.
- `DialogueEvents { get; init; } -> ulong` — The number of dialogue events seen.
- `DialogueHeld { get; init; } -> ulong` — The number of dialogue lines whose knowledge was held for a check.
- `Failed { get; init; } -> ulong` — The number of requests that failed.
- `Journal { get; init; } -> bool` — The journal and the query evaluator work (feature `quests.journal`).
- `Knowledge { get; init; } -> bool` — Reads and grants work (feature `quests.knowledge`).
- `Npc { get; init; } -> bool` — The NPC registry's unlock states can be read (feature `quests.npc`).
- `Npcs { get; init; } -> int` — The number of NPCs in the catalog.
- `Pending { get; init; } -> int` — The number of requests waiting for the next apply.
- `PlayerObjects { get; init; } -> int` — The number of static player objects.
- `Players { get; init; } -> int` — The number of player maps loaded now.
- `Program { get; init; } -> bool` — Kiln's knowledge program runs.
- `Queries { get; init; } -> int` — The number of named queries in the catalog.
- `Quests { get; init; } -> int` — The number of quests in the catalog.
- `QueueMax { get; init; } -> int` — The most entries seen in the game's knowledge trigger queue at an apply.
- `Reason { get; init; } -> string` — Why a part is off; empty when all is on.
- `Submitted { get; init; } -> ulong` — The number of requests submitted.
- `World { get; init; } -> bool` — A server world with a knowledge registry exists.
- `WorldEntries { get; init; } -> int` — The number of entries in the world map now.
- `WorldObjects { get; init; } -> int` — The number of static world objects.

## KnowledgeType — The game's knowledge types.
enum: Bool, Counter, Flags, None

## NativeComputed — A knowledge object the engine keeps for what a line's one knowledge check cannot say.
- `NativeComputed(string Mod, uint Id, string Kind, string Target, string Node, int Index, uint Gate, uint Heard)`
- `Gate { get; init; } -> uint` — The knowledge id of the node's gate: the condition is false until it is set.
- `Heard { get; init; } -> uint` — The knowledge id of the line's "heard" object: the condition is false once it is set (a `once` topic).
- `Id { get; init; } -> uint` — The knowledge id of the object.
- `Index { get; init; } -> int` — The option's or greeting's index; -1 when it has none.
- `Kind { get; init; } -> string` — What it stands for: "if" (an option), "greeting", "available" (a quest with requirements) or "ready" (a hand-in).
- `Mod { get; init; } -> string` — The id of the mod that defined it.
- `Node { get; init; } -> string` — The node id, for an option; empty otherwise.
- `Target { get; init; } -> string` — What it belongs to: the dialogue id ("if", "greeting"), the quest id ("available") or `quest.step` ("ready").

## NativeIds — Ids of a mod's native quests, dialogue lines and knowledge objects: FNV-1a-32 of a UUIDv5 of `<mod>:<kind>:<name>`.
- `static Derive(string mod, string kind, string name) -> Guid` — The derived GUID (game byte order = `Guid.ToByteArray`).
- `static Fnv(Guid g) -> uint` — FNV-1a-32 over a GUID's bytes: the game's id of an item, recipe, NPC or loca tag with that GUID.
- `static Knowledge(string mod, string name) -> uint` — A knowledge object the mod declared in `knowledge` (or the generated ones: `q.<quest>.active`, ...).
- `static Line(string mod, string dialogue, string node, int option) -> uint` — A native dialogue line: `<dialogue>.<node>.<option>` (its id is its "heard" knowledge).

## NativeLineInfo — A line in the game's menu.
- `NativeLineInfo(string Mod, string Dialogue, string Npc, string Node, int Option, uint Line, string Auto = "")`
- `Auto { get; init; } -> string` — A topic Kiln made for a giver without a dialogue ("start:quest", "advance:quest.step"); empty for a line from a...
- `Dialogue { get; init; } -> string` — The dialogue's id.
- `Line { get; init; } -> uint` — The game's dialogue line id (also its "heard" knowledge id).
- `Mod { get; init; } -> string` — The id of the mod that defined it.
- `Node { get; init; } -> string` — The id of the node the option is in.
- `Npc { get; init; } -> string` — The NPC the line is for.
- `Option { get; init; } -> int` — The option's index in the node.

## NativeMarkerInfo — A map marker the boot build made for a step.
- `NativeMarkerInfo(string Mod, string Quest, string Step, string Template, Guid Guid, WorldPosition? At)`
- `At { get; init; } -> WorldPosition?` — The marker's fixed position; null when it is moved to the step's place at run time.
- `Guid { get; init; } -> Guid` — The marker's GUID.
- `Mod { get; init; } -> string` — The id of the mod that defined it.
- `Quest { get; init; } -> string` — The quest's id.
- `Step { get; init; } -> string` — The step's id.
- `Template { get; init; } -> string` — The marker's template name.

## NativeModInfo — What the boot build made for one mod.
- `NativeModInfo(string Id, bool Ok, IReadOnlyDictionary<string, uint> Knowledge, IReadOnlyList<NativeQuestInfo> Quests, IReadOnlyList<NativeLineInfo> Lines, IReadOnlyList<NativeNpcInfo> Npcs, IReadOnlyList<NativeMarkerInfo> Markers, IReadOnlyList<string> Errors, IReadOnlyList<NativeComputed> Computed)`
- `Computed { get; init; } -> IReadOnlyList<NativeComputed>` — The knowledge objects the engine keeps for conditions one knowledge check cannot say.
- `Errors { get; init; } -> IReadOnlyList<string>` — Why the mod's data was not built, as text lines.
- `Id { get; init; } -> string` — The mod's id.
- `Knowledge { get; init; } -> IReadOnlyDictionary<string, uint>` — The mod's own knowledge objects: name to knowledge id.
- `Lines { get; init; } -> IReadOnlyList<NativeLineInfo>` — The lines built for the game's menu.
- `Markers { get; init; } -> IReadOnlyList<NativeMarkerInfo>` — The map markers built.
- `Npcs { get; init; } -> IReadOnlyList<NativeNpcInfo>` — The NPCs built or changed.
- `Ok { get; init; } -> bool` — The mod's native data was built.
- `Quests { get; init; } -> IReadOnlyList<NativeQuestInfo>` — The quests built for the game's journal.

## NativeNpcInfo — An NPC the boot build made or changed.
- `NativeNpcInfo(string Mod, string Id, string FullId, string Template, Guid Guid, bool IsClone, uint NpcId, bool Roster, uint Unlock, NativePlacement? Place, string Dialogue, string SummonItem)`
- `Dialogue { get; init; } -> string` — The id of the dialogue it opens; empty when none.
- `FullId { get; init; } -> string` — The NPC's full id, `<mod>:<id>`.
- `Guid { get; init; } -> Guid` — The NPC's GUID (a clone's own).
- `Id { get; init; } -> string` — The NPC's id within its mod.
- `IsClone { get; init; } -> bool` — The NPC is a new one: a copy of its template under its own GUID.
- `Mod { get; init; } -> string` — The id of the mod that defined it.
- `NpcId { get; init; } -> uint` — The game's NPC id; 0 when the NPC has none.
- `Place { get; init; } -> NativePlacement?` — Where the NPC is put in the world; null when the mod does not place it.
- `Roster { get; init; } -> bool` — The NPC is an entry of the game's NPC roster.
- `SummonItem { get; init; } -> string` — The NPC's summon item (a GUID or an item name); empty when none.
- `Template { get; init; } -> string` — The template name of the entity (of a clone: the base).
- `Unlock { get; init; } -> uint` — The knowledge id of the NPC's unlock; 0 when none.

## NativePlacement — Where an NPC is put in the world.
- `NativePlacement(QuestPlace Spec, float Yaw, bool Once, bool? Unlock)`
- `Once { get; init; } -> bool` — Place the NPC only once (JSON `once`).
- `Spec { get; init; } -> QuestPlace` — Where to look for the spot (the same keys as a step's site).
- `Unlock { get; init; } -> bool?` — True: grant the game NPC's unlock first.
- `Yaw { get; init; } -> float` — The facing to place the NPC with.

## NativeQuestInfo — A quest the boot build made for the game's journal.
- `NativeQuestInfo(string Mod, string Id, string FullId, string Scope, uint EntryId, uint Started, uint Active, uint Done, IReadOnlyList<NativeStepInfo> Steps)`
- `Active { get; init; } -> uint` — The knowledge id of the quest's "active" object.
- `Done { get; init; } -> uint` — The knowledge id of the quest's "done" object.
- `EntryId { get; init; } -> uint` — The quest's journal entry id (also its "rewarded" knowledge).
- `FullId { get; init; } -> string` — The quest's full id, `<mod>:<id>`.
- `Id { get; init; } -> string` — The quest's id within its mod.
- `Mod { get; init; } -> string` — The id of the mod that defined it.
- `Scope { get; init; } -> string` — The quest's scope as text: player, party or world.
- `Started { get; init; } -> uint` — The knowledge id of the quest's "started" object.
- `Steps { get; init; } -> IReadOnlyList<NativeStepInfo>` — The quest's steps, in order.

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
- `static Mod(string id) -> NativeModInfo?` — A mod's native data by mod id; null when the mod has none.
- `static Mods { get; } -> IReadOnlyList<NativeModInfo>` — Every mod with native data (a mod with errors is listed with Ok = false and built nothing).
- `static Npc(string fullId) -> NativeNpcInfo?` — An NPC the boot build made or changed, by full id; null when there is none.
- `static Quest(string fullId) -> NativeQuestInfo?` — A quest the boot build made, by full id (`<mod>:<id>`); null when there is none.
- `static Resync(QuestHolder holder, bool quiet = false) -> void` — Writes a holder's knowledge again from the engine's records: every quest, what differs.
- `const RosterPage = 64 -> int` `0.2.4` — The roster's maximum the game's own value allows: 64 entries, the game's included.
- `static ShownByGame(QuestDefinition q) -> bool` — The game's journal shows this quest: it says native and the boot build made it.
- `static Start() -> void` — Starts the mirror, the dialogue options and the placement (the engine calls this; idempotent).
- `static Wanted(string fullQuestId, QuestHolder holder) -> IReadOnlyList<NativeWanted>` — What the game's knowledge should hold for a holder in a quest, as the engine sees it now.

## NativeStepInfo — A step's knowledge objects (the engine mirrors the step's state into them).
- `NativeStepInfo(string Id, uint Visible, uint Active, uint Done, uint Count = 0, int Total = 0, uint Ready = 0)`
- `Active { get; init; } -> uint` — The knowledge id of the step's "active" object: the step is the current one.
- `Count { get; init; } -> uint` — The knowledge id of the counter its journal line shows; 0 when the step has no counter.
- `Done { get; init; } -> uint` — The knowledge id of the step's "done" object.
- `Id { get; init; } -> string` — The step's id.
- `Ready { get; init; } -> uint` — The knowledge id of a hand-in step's "ready" object (the player holds the items); 0 when the step is not a hand-in.
- `Total { get; init; } -> int` — What the counter counts up to (0: none).
- `Visible { get; init; } -> uint` — The knowledge id of the step's "visible" object: the journal shows the step.

## NativeWanted — A knowledge value the engine's state asks for.
- `NativeWanted()`
- `NativeWanted(string Name, uint Id, uint Value)`
- `Id { get; init; } -> uint` — The knowledge id.
- `Name { get; init; } -> string` — The knowledge object's name.
- `Value { get; init; } -> uint` — The value wanted.
