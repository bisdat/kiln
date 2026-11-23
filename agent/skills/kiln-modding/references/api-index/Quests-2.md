# Quests (Kiln.Quests): part 2 of 2

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
- `Id { get; init; } -> string` — Lower-case, from the game's achievement name (`wetlands_obelisk_above_fog_read`).
- `Position { get; init; } -> WorldPosition?` — Where it is; null when it is not known.
- `ReadBy(Player player) -> bool?` — Whether this character read it (tick thread; null when it cannot be read).
- `Reading { get; init; } -> ProgressFact` — The fact that a character read it.
- `Region { get; init; } -> string` — The region, from the game's achievement name.

## ProgressFact — A fact as the game stores it: a requirement (a knowledge id compared to a value, or a named query) and where it lives.
- `ProgressFact(KnowledgeQuery Query, ProgressScope Scope)`
- `Holds(EntityId target) -> bool?` — For the world (`target` None) or a player in game.
- `Holds(Player player) -> bool?` — For a player in game, as `ProgressFact.Holds`.
- `Id { get; } -> uint` — The knowledge id (or the named query's id).
- `Query { get; init; } -> KnowledgeQuery` — The requirement as the game stores it: a knowledge object compared to a value, or a named query.
- `Scope { get; init; } -> ProgressScope` — Where the game keeps the fact: the world, each character, or both.

## ProgressScope — Where the game keeps a fact.
enum: World, Character, Both

## QuestAction — A reward or side effect (a quest's rewards and hooks, a conversation option's `do`).
- `At { get; set; } -> WorldPosition?` — Spawn, teleport, waypoint: the place.
- `BindNpc { get; set; } -> string?` — Spawn: the entity becomes this NPC of `npcs` (see `Quests.BindNpc`).
- `Clear { get; set; } -> bool` — Waypoint: true clears it.
- `static Code(Func<QuestActionContext, Task> run) -> QuestAction` — A call action: runs `run` as the owning mod.
- `Count { get; set; } -> int` — Items, spawned entities, or a signal's amount.
- `Exact { get; set; } -> bool` — Spawn: exactly at the place given.
- `static Experience(uint amount) -> QuestAction` — An experience action: `amount` experience.
- `Force { get; set; } -> bool` — Force an advance past unfinished objectives.
- `static GrantKnowledge(string name, string? target = null) -> QuestAction` — A knowledge action: sets the game knowledge `name`.
- `static Item(string item, int count = 1) -> QuestAction` — A give action: `count` of `item` to the holder.
- `Kind { get; init; } -> QuestActionKind` — What this action does.
- `Name { get; set; } -> string?` — Item, knowledge name, quest id, "quest.step", template, dialogue, signal or action name.
- `Node { get; set; } -> string?` — Talk: the node to open at.
- `Offset { get; set; } -> WorldPosition?` — Spawn, teleport, waypoint: an offset in metres from the player, instead of `QuestAction.At`.
- `Run { get; set; } -> Func<QuestActionContext, Task>?` — Call, C#: the code to run.
- `Tag { get; set; } -> string?` — Spawn: the Kiln.Spawn tag.
- `Target { get; set; } -> string?` — Knowledge, revoke: "world" or "player"; default: the player's object if the id has one, else the world's.
- `Text { get; set; } -> UiText?` — Chat, toast: the line to send.
- `ToastKind { get; set; } -> string?` — Toast: info, success, warning, error.
- `Transient { get; set; } -> bool` — Spawn: never written to the save.
- `Value { get; set; } -> uint` — Xp: the amount.
- `Yaw { get; set; } -> double` — Spawn: the entity's facing in degrees about the up axis.

## QuestActionContext — What an action runs for.
- `QuestActionContext(QuestHolder Holder, QuestDefinition? Quest, EntityId Npc, string ModId)`
- `Holder { get; init; } -> QuestHolder` — Whom the action runs for (a player, a character, or the world).
- `ModId { get; init; } -> string` — The id of the mod that defined the action.
- `Npc { get; init; } -> EntityId` — The NPC entity involved; `EntityId.None` when there is none.
- `Player { get; } -> Player?` — The holder's player in game; null when the holder has no entity or kiln.players is unavailable.
- `Quest { get; init; } -> QuestDefinition?` — The quest the action belongs to; null outside a quest (for example a dialogue option).

## QuestActionKind — What a `QuestAction` does.
enum: Give, Take, Xp, Knowledge, Revoke, Start, Complete, Fail, Advance, Reset, Spawn, Despawn, Teleport, Chat, Toast, Waypoint, Talk, Signal, Call

## QuestBook — Everything one holder has done: one per character (per world) and one for the world.
- `Quests { get; set; } -> Dictionary<string, QuestRecord>` — Quest full id -> its record.
- `Revision { get; set; } -> long` — +1 per change.
- `Tracked { get; set; } -> string?` — Old saves' tracked quest.

## QuestCancelableEvent — Base of the quest events that fire before a change.
- `Holder { get; } -> QuestHolder` — Whose state changes.
- `Player { get; } -> Player?` — The player in game (null for the world, offline and synthetic holders).
- `Quest { get; } -> QuestDefinition` — The quest the change is for.

## QuestCompletedEvent — A quest is about to complete (its last step's objectives are done; before that step's `onDone` actions and the rewards).

## QuestCondition — A condition on a holder, checked on the server: a quest's `requires`, an option's `if`.
- `Children { get; set; } -> List<QuestCondition>` — All, Any, Not: the conditions it combines.
- `Count { get; set; } -> int` — Level: at least this level.
- `static HasItem(string item, int count = 1) -> QuestCondition` — Holds when the holder's inventory has at least `count` of `item`.
- `static HasKnowledge(string name) -> QuestCondition` — Holds while the game's knowledge `name` holds.
- `Is { get; set; } -> string?` — Quest: locked, available, active, done, failed.
- `Kind { get; init; } -> QuestConditionKind` — What this condition tests.
- `static Level(int level) -> QuestCondition` — Holds when the holder is at least `level`.
- `Name { get; set; } -> string?` — Knowledge name, quest id, "quest.step", item, or predicate name.
- `Predicate { get; set; } -> Func<QuestContext, bool>?` — Predicate, C#: the function that decides.
- `static Quest(string quest, string state) -> QuestCondition` — Holds when quest `quest` is in `state` (locked, available, active, done, failed or started).
- `static QuestStep(string questDotStep, string state) -> QuestCondition` — Holds when step `questDotStep` (written `quest.step`) is in `state` (pending, active or done).
- `static When(Func<QuestContext, bool> predicate) -> QuestCondition` — Holds when `predicate` returns true.

## QuestConditionKind — What a `QuestCondition` tests.
enum: Knowledge, Level, Quest, Step, Item, All, Any, Not, Predicate

## QuestContext — What a condition or a custom objective is asked about.
- `QuestContext(QuestHolder Holder, QuestDefinition? Quest, EntityId Npc, string ModId)`
- `Holder { get; init; } -> QuestHolder` — Whose state is asked about (a player, a character, or the world).
- `ModId { get; init; } -> string` — The id of the mod whose definition is asked.
- `Npc { get; init; } -> EntityId` — The NPC entity being talked to; `EntityId.None` when there is none.
- `Player { get; } -> Player?` — The holder's player in game; null when the holder has no entity or kiln.players is unavailable.
- `Quest { get; init; } -> QuestDefinition?` — The quest being checked; null outside a quest (for example a dialogue option).

## QuestDefinition — A quest chain: steps that run in order, each with one or more objectives.
- `Abandonable { get; set; } -> bool` — Players may give it up (never a world quest; default true).
- `Auto { get; set; } -> bool` — Starts by itself as soon as it is available (a world quest when the world runs, a player quest while the player is...
- `Description { get; set; } -> UiText?` — A longer description of the quest.
- `FullId { get; } -> string` — `<modId>:<id>`.
- `Giver { get; set; } -> string?` — The NPC who offers it (an id of the same mod's `npcs`, or `mod:npc`).
- `Hidden { get; set; } -> bool` — Not listed until it is active.
- `Id { get; init; } -> string` — Letters, digits, '_' and '-'.
- `Journal { get; set; } -> QuestJournalMode` — Where the quest is shown: the game's own journal (the default) or chat lines.
- `Main { get; set; } -> bool` — Marks it as a main quest (JSON `main`): the game's journal entry gets its main-quest flag.
- `ModId { get; } -> string` — The mod that defined it ("" for code outside a mod).
- `Name { get; set; } -> UiText` — The quest's name in the journal.
- `OnFail { get; set; } -> List<QuestAction>` — Actions that run when the quest fails.
- `OnStart { get; set; } -> List<QuestAction>` — Actions that run when the quest starts.
- `PartyRadius { get; set; } -> double` — For a `QuestScope.Party` quest: the radius in metres within which players share credit (default 60).
- `Priority { get; set; } -> int?` — The game's journal: its place in the list (the game uses 40..100; default 50).
- `Repeatable { get; set; } -> bool` — Can be started again after it is done.
- `Requires { get; set; } -> List<QuestCondition>` — Conditions that must all hold before the quest can start.
- `Retryable { get; set; } -> bool` — Can be started again after it failed or was abandoned (default true).
- `Rewards { get; set; } -> List<QuestAction>` — Actions that run when the quest is completed (items, experience, knowledge...).
- `Scope { get; set; } -> QuestScope` — Who owns the quest's progress (default `QuestScope.Player`).
- `Source { get; set; } -> string?` — The game's journal: the portrait (keen::QuestSourceType: None, Flame, Blacksmith, Alchemist, ...).
- `Step(string id) -> QuestStep?` — The step with this id (case-sensitive); null when there is none.
- `Steps { get; set; } -> List<QuestStep>` — The steps, in order.

## QuestEngineStatus — What the engine holds and has done.
- `QuestEngineStatus(bool Started, bool ServerSide, int Quests, int Dialogues, int Npcs, int Conversations, int Errors, ulong Transitions, ulong Actions, ulong ActionFailures, bool Waypoints, IReadOnlyList<string> LoadErrors)`
- `ActionFailures { get; init; } -> ulong` — The number of those actions that failed.
- `Actions { get; init; } -> ulong` — The number of actions run since the engine started.
- `Channel { get; } -> bool` — Always false: the channel kiln.quests is gone with Kiln's journal and windows.
- `Conversations { get; init; } -> int` — The number of chat conversations open now.
- `Dialogues { get; init; } -> int` — The number of dialogue definitions held.
- `Errors { get; init; } -> int` — The number of load errors (see `LoadErrors`).
- `LoadErrors { get; init; } -> IReadOnlyList<string>` — The load errors as text lines.
- `Npcs { get; init; } -> int` — The number of NPC definitions held.
- `Quests { get; init; } -> int` — The number of quest definitions held.
- `ServerSide { get; init; } -> bool` — This process runs the quests: a dedicated server, or a client hosting a world while it runs.
- `Started { get; init; } -> bool` — The engine has started (`Quests.EnsureStarted` ran).
- `Transitions { get; init; } -> ulong` — The number of quest state changes since the engine started.
- `Waypoints { get; init; } -> bool` — The player-waypoint writer (kiln.questmark) is loaded and its program runs in this world, so waypoints can be set.

## QuestEntry — One quest as a character stands in it.
- `QuestEntry(string Id, string? Name, QuestKind Kind, QuestStatus Status, int Step, int Steps, string? StepId, uint NameTag, JournalQuest? Journal, QuestDefinition? Definition)`
- `Active { get; } -> bool` — True when the quest is `QuestStatus.Active`.
- `Definition { get; init; } -> QuestDefinition?` — The definition of a Kiln quest; null for a vanilla quest.
- `Done { get; } -> bool` — True when the quest is `QuestStatus.Done`.
- `Id { get; init; } -> string` — Vanilla: `quest:0x<entryId>`; Kiln: the quest's full id `<mod>:<id>`.
- `Journal { get; init; } -> JournalQuest?` — The game's journal data for a vanilla quest; null for a Kiln quest.
- `Kind { get; init; } -> QuestKind` — Whether this is a quest of the game's journal or one a mod defined.
- `Name { get; init; } -> string?` — A readable name: vanilla quests the game's named query behind them (`NPC_Alchemist_Hint01_HAS_Shelter_Journal`; null...
- `NameTag { get; init; } -> uint` — Vanilla: the loca tag of the quest's title (the catalog's text).
- `Status { get; init; } -> QuestStatus` — Vanilla quests are Locked (not started), Active or Done; the game has no failed state.
- `Step { get; init; } -> int` — The current step (0-based; the last step when done); -1 when not started.
- `StepId { get; init; } -> string?` — The current step's id: a Kiln quest's step id, or `step<n>` (1-based) for a vanilla quest; null when it has none.
- `Steps { get; init; } -> int` — The number of steps the quest has.

## QuestFailedEvent — A quest is about to fail.
- `Reason { get; } -> string` — Why it fails (may be empty).

## QuestHolder — Who owns a quest's state: a character on this world (the game's own key: account + CharacterId), or the world.
- `QuestHolder()`
- `QuestHolder(string Key, ulong Account, EntityId Entity)`
- `Account { get; init; } -> ulong` — The Steam or account id (where the data was filed before characters); 0 for the world and for a session holder.
- `Character { get; init; } -> CharacterKey` — The character whose book this is; none for the world, synthetic holders and builds without character ids.
- `Entity { get; init; } -> EntityId` — The player's entity while in game; none when offline, for the world, and for synthetic holders.
- `static explicit operator PlayerRef(QuestHolder holder) -> PlayerRef` — This holder as a `PlayerRef`.
- `IsNone { get; } -> bool` — True for the default value (no key): no holder at all.
- `IsWorld { get; } -> bool` — True for `QuestHolder.World`.
- `Key { get; init; } -> string` — "world", "character:\<account>.\<id>", "player:\<account>" (a synthetic holder, an offline account, or a build...
- `static Of(EntityId player) -> QuestHolder` — The holder of a player in game.
- `static Of(Player p) -> QuestHolder` — The holder of a player in game: keyed by their character when the game gives one, else by their account, else by...
- `static OfAccount(ulong account) -> QuestHolder` — A player known only by their account (offline): the character they last played in this world, or the account's book...
- `static OfCharacter(CharacterKey character, ulong legacyAccount = 0) -> QuestHolder` — A character, online or not.
- `static Parse(string who) -> QuestHolder` — "world", "synthetic:\<name>", or the name or entity number of a player in game.
- `static Synthetic(string name) -> QuestHolder` — A holder without an entity for tests and tools: its state is saved like a player's, it has no position or inventory...
- `static World { get; } -> QuestHolder` — The holder of the world's own state: what world-scope quests use.

## QuestJournalMode — Where a quest is shown (JSON `journal`).
enum: Chat, Native

## QuestKind — Whose engine a quest belongs to.
enum: Vanilla, Kiln

## QuestLog — A character's quests (`Quests.Of`), read when it is made.
- `Active { get; } -> IEnumerable<QuestEntry>` — The quests that are active.
- `All { get; } -> IReadOnlyList<QuestEntry>` — Every quest: the game's journal quests, then Kiln quests.
- `Completed { get; } -> IEnumerable<QuestEntry>` — The quests that are done.
- `Find(string idOrName) -> QuestEntry?` — By id (`quest:0x8755c335`, `0x8755c335`, `mymod:tobin`, or a Kiln quest's id within its mod) or name...
- `Kiln { get; } -> IEnumerable<QuestEntry>` — The quests mods defined.
- `Player { get; } -> EntityId` — The player entity this list was made for.
- `StatusOf(string idOrName) -> QuestStatus` — The status of the quest `idOrName` (as for `QuestLog.Find`); `QuestStatus.Locked` when there is no such quest.
- `Vanilla { get; } -> IEnumerable<QuestEntry>` — The game's own journal quests.

## QuestMarker — A step's map marker.
- `QuestMarker(WorldPosition? At = null, double Radius = 0, bool Waypoint = true, UiText? Name = null, string? Base = null)`
- `At { get; init; } -> WorldPosition?` — A fixed place (null: the step's other place decides).
- `Base { get; init; } -> string?` — The marker template to copy.
- `Name { get; init; } -> UiText?` — The marker's name on the map (default the step's text).
- `Radius { get; init; } -> double` — The marker's radius in metres; 0 when none is given.
- `Waypoint { get; init; } -> bool` — Also write the player's waypoint, the game's own map and compass pin (default true).

## QuestNpcDefinition — An NPC a quest or conversation refers to: a game NPC, or a new one (`QuestNpcDefinition.Clone`), found by its template or placed in the world...
- `At { get; set; } -> WorldPosition?` — Where it stands (the JSON's `place.at`): with no entity found, this spot still counts as the NPC.
- `Clone { get; set; } -> bool` — A new NPC: a copy of `QuestNpcDefinition.Template` under its own GUID, one per world.
- `CloneGuid { get; set; } -> Guid?` — The clone's GUID.
- `CloneName { get; set; } -> string?` — The clone's template name; null derives it.
- `Dialogue { get; set; } -> string?` — The conversation it opens.
- `FullId { get; } -> string` — `<modId>:<id>`, or just the id outside a mod.
- `Id { get; init; } -> string` — The NPC's id.
- `ModId { get; } -> string` — The mod that defined it ("" for code outside a mod).
- `Name { get; set; } -> UiText?` — The NPC's display name.
- `Place { get; set; } -> QuestPlace?` — Where Kiln puts it in the world, once it can: on real ground, outside every base unless `QuestPlace.InBase`.
- `TalkRange { get; set; } -> double` — How close a player must be to talk (metres).
- `Template { get; set; } -> string?` — Template name of the entity (of a clone: the base).

## QuestObjective — One thing to do in a step.
- `At { get; set; } -> WorldPosition?` — Reach: the place.
- `static Collect(string item, int count = 1) -> QuestObjective` — A collect objective: hold `count` of `item` (name or id).
- `Count { get; set; } -> int` — How many (kills, items, crafts, signals).
- `CountDestroyed { get; set; } -> bool` — Kill: Kiln.Spawn destroying a matching entity counts too (off: only deaths count).
- `static Custom(string signal, int count = 1) -> QuestObjective` — A custom objective: a counter that `Quests.Signal` calls with `signal` raise to `count`.
- `static Deliver(string item, int count, string to) -> QuestObjective` — A deliver objective: hand `count` of `item` to the NPC `to`.
- `Family { get; set; } -> string?` — Kill: part of the template name ("Scavenger", "Vukah").
- `static HasKnowledge(string name) -> QuestObjective` — A knowledge objective: done while the game's knowledge `name` holds.
- `Item { get; set; } -> string?` — Collect, deliver, craft, build: item name or id.
- `static Kill(string? template = null, string? family = null, string? tag = null, int count = 1) -> QuestObjective` — A kill objective: `count` kills, matched by exact `template`, part of the template name (`family`) or a...
- `Kind { get; init; } -> QuestObjectiveKind` — What this objective asks for.
- `Knowledge { get; set; } -> string?` — Knowledge: the name that must hold ("npc:...", "quest:...", "query:...", "0x...").
- `Line { get; set; } -> string?` — Talk: a game dialogue line ("line:...", "0x..."), or a line of Kiln.Quests/Native.
- `Marker { get; set; } -> string?` — Reach: a template name; the nearest entity of that template is the place.
- `Near { get; set; } -> WorldPosition?` — Kill: only near this place (with `QuestObjective.Radius`).
- `Node { get; set; } -> string?` — Talk: a node of a Kiln conversation that must be reached.
- `Npc { get; set; } -> string?` — Talk: the NPC (an id of `npcs`, a template name, or "npc:\<debugName>").
- `Offset { get; set; } -> WorldPosition?` — Reach: an offset in metres from where the player stood when the step started, instead of `QuestObjective.At`.
- `Progress { get; set; } -> Func<QuestContext, int>?` — Custom, C#: polled about once a second, returns the current count.
- `Radius { get; set; } -> double` — Reach, kill: metres.
- `static Reach(WorldPosition at, double radius = 10) -> QuestObjective` — A reach objective: come within `radius` metres of `at`.
- `Recipe { get; set; } -> string?` — Craft: recipe id ("0x...").
- `Signal { get; set; } -> string?` — Custom: `Quests.Signal` with this name adds to the counter.
- `Site { get; set; } -> bool` — Reach: the step's `QuestStep.Site` is the place.
- `Tag { get; set; } -> string?` — Kill: the Kiln.Spawn tag of the victim.
- `static Talk(string npc, string? node = null, string? line = null) -> QuestObjective` — A talk objective: talk to `npc`, optionally reaching a conversation `node` or saying a dialogue `line`.
- `Template { get; set; } -> string?` — Kill: exact template name.
- `Text { get; set; } -> UiText?` — Shown instead of the step's text when a step has several objectives.
- `To { get; set; } -> string?` — Deliver: the NPC who receives the items.

## QuestObjectiveKind — What an objective asks for.
enum: Kill, Collect, Deliver, Reach, Talk, Craft, Build, Knowledge, Custom

## QuestObjectiveProgressEvent — An objective's counter changed.
- `Holder { get; } -> QuestHolder` — Whose counter changed.
- `Index { get; } -> int` — The objective's place in the step.
- `Objective { get; } -> QuestObjective` — The objective whose counter changed.
- `OldValue { get; } -> int` — The counter before the change.
- `Quest { get; } -> QuestDefinition` — The quest the objective belongs to.
- `Step { get; } -> QuestStep` — The step the objective belongs to.
- `Target { get; } -> int` — The count the objective needs (`QuestObjective.Count`).
- `Value { get; } -> int` — The counter after the change.

## QuestPlace — A place found at run time: on real ground (the voxel surface), outside every player base, not too steep.
- `At { get; set; } -> WorldPosition?` — Search around this place.
- `BaseMargin { get; set; } -> double` — Metres to keep from a base's build zone (default 20).
- `Exact { get; set; } -> bool` — Exactly `QuestPlace.At`: no ground, no base check.
- `InBase { get; set; } -> bool` — A base is fine (a friendly NPC that belongs in one).
- `MaxDistance { get; set; } -> double` — Metres from the centre of the search, at most.
- `MinDistance { get; set; } -> double` — Metres from the centre of the search.
- `NearNpc { get; set; } -> string?` — Search around this NPC of the mod's `npcs`.
- `NearPlayer { get; set; } -> bool` — Search around the player who gets there first (the default when nothing else is given).

## QuestProgress — One holder's standing in one quest.
- `QuestProgress(QuestDefinition Quest, QuestHolder Holder, QuestStatus Status, string? StepId, int StepIndex, IReadOnlyDictionary<string, int> Counters, int Completions)`
- `Completions { get; init; } -> int` — How many times the holder has completed the quest.
- `Counter(int objective = 0) -> int` — The counter of an objective of the current step.
- `Counters { get; init; } -> IReadOnlyDictionary<string, int>` — A copy of the objective counters, keyed `<step>#<objective>` (see `QuestRecord.CounterKey`).
- `Holder { get; init; } -> QuestHolder` — Whose standing this is.
- `Quest { get; init; } -> QuestDefinition` — The quest's definition.
- `Status { get; init; } -> QuestStatus` — Where the quest stands for the holder.
- `StepId { get; init; } -> string?` — The id of the current step; null when the quest is not active.
- `StepIndex { get; init; } -> int` — The current step's 0-based index in `QuestDefinition.Steps`; meaningful while the quest is active.

## QuestRecord — A holder's state in one quest, as stored.
- `Completions { get; set; } -> int` — How many times it has been completed.
- `static CounterKey(string step, int objective) -> string` — The key of an objective's counter in `QuestRecord.Counters`: `<step>#<objective>` (the objective's index in the step).
- `Counters { get; set; } -> Dictionary<string, int>` — "\<step>#\<objective>" -> count.
- `EnteredAt { get; set; } -> long` — When the step started (not saved): a step the player already fulfils stays on screen a moment before it passes.
- `FinishedAt { get; set; } -> long` — When it was completed or failed, in Unix seconds (UTC); 0 while it runs.
- `RewardsPending { get; set; } -> bool` — Rewards still to pay (the player was offline when an admin completed it).
- `StartedAt { get; set; } -> long` — When it started, in Unix seconds (UTC).
- `Status { get; set; } -> QuestStatus` — Where the quest stands for this holder (it starts as `QuestStatus.Active`).
- `StepId { get; set; } -> string?` — The current step (an id, so a changed definition keeps its place); null when not active.
- `StepIndex { get; set; } -> int` — The 0-based index of the current step in the quest's steps.
- `StepOrigin { get; set; } -> double[]?` — Where the player stood when the step started (for reach offsets).

## QuestResult — The answer to a quest change.
- `QuestResult(bool Ok, string Reason, QuestStatus Status)`
- `Ok { get; init; } -> bool` — True when the change happened; false when it was refused.
- `Reason { get; init; } -> string` — Why it was refused; empty when `Ok`.
- `Status { get; init; } -> QuestStatus` — The quest's status after the call (when it was refused: the status it kept).

## Quests — Kiln quests: chains defined by mods (quests/*.json or `Quests.Define`), run by the server for players, parties and the world.
- `static Abandon(string quest, QuestHolder holder) -> QuestResult` — A player gives up a quest: back to available (when it may be retried), else failed.
- `static Advance(string quest, QuestHolder holder, string? step = null, bool force = false) -> Task<QuestResult>` — Finishes a holder's current step: objectives that are still open are refused unless they are talk or custom (a...
- `static Available { get; } -> bool` — Always true: the quest engine is part of Kiln.Quests (see `Quests.Status` for what it holds).
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
- `static Of(Player player) -> QuestLog` — A player's quests, vanilla and Kiln, as `Quests.Of` (tick thread).
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
- `static Status { get; } -> QuestEngineStatus` — What the engine holds and has done.
- `static StatusOf(string quest, QuestHolder holder) -> QuestStatus` — A holder's status in a quest.
- `static TrackedBy(QuestHolder holder) -> string?` — The tracked quest an old save holds.
- `static WaypointCounters { get; } -> (ulong Requests, ulong Written)` — Waypoint requests and writes of kiln.questmark so far.

## QuestScope — Who owns a quest's progress.
enum: Player, Party, World

## QuestSource — Who gives a quest (the game's QuestSourceType: an NPC portrait or a biome).
enum: None, Flame, Blacksmith, Alchemist, Huntress, Farmer, Carpenter, CryptKeeper, Bard, ChineseNewYearTrader, Barber, Fisher, AncientResearcher, Grassland, Deepforest, Steppes, Desert, ColdHeights, Wetlands, AncientLands

## QuestSpawn — Creatures a step brings into the world.
- `Count { get; set; } -> int` — How many to spawn (default 1).
- `Level { get; set; } -> int` — The game's level to spawn at (0: the template's).
- `Place { get; set; } -> QuestPlace?` — Default the step's `QuestStep.Site`; without one, near the player at 30..60 m.
- `Retries { get; set; } -> int` — How many times Kiln spawns them again when the game removes them before they die (default 5).
- `Spread { get; set; } -> double` — Metres between them.
- `Tag { get; set; } -> string?` — The Kiln.Spawn tag: a kill objective can name it.
- `Template { get; init; } -> string` — The template name of the creature to spawn.

## QuestStartedEvent — A quest is about to start for a holder.

## QuestStateChangedEvent — A holder's standing in a quest changed: status or step.
- `Holder { get; } -> QuestHolder` — Whose standing changed.
- `OldStatus { get; } -> QuestStatus` — The status before the change.
- `Quest { get; } -> QuestDefinition` — The quest that changed.
- `Status { get; } -> QuestStatus` — The status after the change.
- `StepId { get; } -> string?` — The current step; null when not at a step (done, failed, reset).
- `StepIndex { get; } -> int` — The 0-based index of the current step in `QuestDefinition.Steps`.

## QuestStatus — Where a quest stands for one holder.
enum: Locked, Available, Active, Done, Failed

## QuestStatusChangedEvent — A player's standing in a quest changed: its status or its current step.
- `NewStatus { get; } -> QuestStatus` — The status after the change.
- `NewStep { get; } -> int` — The 0-based step after the change; -1 when the quest has not started.
- `OldStatus { get; } -> QuestStatus` — The status before the change.
- `OldStep { get; } -> int` — The 0-based step before the change; -1 when the quest had not started.
- `Player { get; } -> Player?` — The player; null for a world quest, or when they left.
- `PlayerEntity { get; } -> EntityId` — The player (None for a Kiln world quest).
- `Quest { get; } -> QuestEntry` — The quest as it stands after the change.

## QuestStep — One step of a quest.
- `Id { get; init; } -> string` — The step's id.
- `Marker { get; set; } -> QuestMarker?` — The map marker and waypoint of the step.
- `NoMarker { get; set; } -> bool` — This step shows no marker, even though it has a place (JSON `"marker": false`).
- `Objectives { get; set; } -> List<QuestObjective>` — What to do in this step.
- `OnDone { get; set; } -> List<QuestAction>` — Actions that run when the step is done.
- `OnEnter { get; set; } -> List<QuestAction>` — Actions that run when the step becomes the current one.
- `RecommendedLevel { get; set; } -> int` — The game's journal: the recommended level shown with the step.
- `Site { get; set; } -> QuestPlace?` — Where the step happens, found at run time on real ground outside every base, once per world.
- `Spawns { get; set; } -> List<QuestSpawn>` — Creatures the step brings into the world.
- `Text { get; set; } -> UiText` — The step's text: the paragraph in the journal.
- `Title { get; set; } -> UiText?` — The game's journal: the step's short name (default its text).
- `Voice { get; set; } -> string?` — The game's journal: the step's voice-over line (a name in the mod's cooked audio, or a GUID).

## QuestStepEvent — A step is about to finish.
- `Next { get; } -> QuestStep?` — The step that follows; null when this is the last.
- `Step { get; } -> QuestStep` — The step that is about to finish.
- `StepId { get; } -> string` — The finishing step's id.

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
- `Shrine { get; } -> FlameShrine` — The shrine or sanctum that was found.

## ShrineKind — What kind of flame structure a `FlameShrine` is.
enum: Shrine, Sanctum

## ShroudRoot — A Shroud Root.
- `ShroudRoot(string Id, string Region, bool ElixirWell, int SkillPoints, WorldPosition? Position, ProgressFact Destruction)`
- `DestroyedBy(Player player) -> bool?` — Whether this character destroyed it (tick thread; null when it cannot be read).
- `Destruction { get; init; } -> ProgressFact` — The fact that a character destroyed it.
- `ElixirWell { get; init; } -> bool` — This root is the one at an Elixir Well (the well counts as cleared when it is destroyed).
- `Id { get; init; } -> string` — Lower-case, from the game's achievement name (`desert_well_09`).
- `Position { get; init; } -> WorldPosition?` — Where it is; null until the bosses and roots are read (see `WorldProgress.BossesReady`).
- `Region { get; init; } -> string` — The region, from the game's achievement name.
- `SkillPoints { get; init; } -> int` — The skill points destroying it gives.

## ShroudRootDestroyedEvent — A character destroyed a Shroud Root (an Elixir Well's when `ShroudRoot.ElixirWell`).
- `Root { get; } -> ShroudRoot` — The root that was destroyed.

## Spire — An Ancient Spire: its fast-travel point, the knowledge the game sets when it is unlocked, the per-character "top reached" knowledge and the beacon on top.
- `Spire(string Id, string Template, uint NameTag, WorldPosition Position, ProgressFact Unlock, ProgressFact? TopReached, ProgressFact? Beacon)`
- `Beacon { get; init; } -> ProgressFact?` — The fact that the beacon on top is lit; null when the game keeps none for this spire.
- `BeaconLit { get; } -> bool?` — The beacon on top is lit in this world.
- `Id { get; init; } -> string` — Lower-case, from the fast-travel marker's template name (`grassland`, `capital`, ...).
- `NameTag { get; init; } -> uint` — Loca tag of the map marker's name (the catalog's displayName).
- `Position { get; init; } -> WorldPosition` — Where the spire's fast-travel point is.
- `Template { get; init; } -> string` — The fast-travel marker's template name.
- `TopReached { get; init; } -> ProgressFact?` — The fact that a character reached the top; null when the game keeps none for this spire.
- `TopReachedBy(Player player) -> bool?` — The character reached the top (its own knowledge; the game's spire achievement reads it).
- `Unlock { get; init; } -> ProgressFact` — The fact that the spire's fast travel is unlocked.
- `Unlocked { get; } -> bool?` — The spire's fast travel is unlocked in this world (tick thread; null when unreadable).
- `UnlockedFor(Player player) -> bool?` — Unlocked as this character sees it (the world's value, or its own twin in personal progression).

## SpireUnlockedEvent — An Ancient Spire's knowledge was set: the world's unlock (`WorldProgressEvent.IsWorld`), a character's twin of it (personal progression), or a character's "top...
- `Spire { get; } -> Spire` — The spire.
- `TopReached { get; } -> bool` — The character's own "top reached" knowledge, not the unlock.

## Survivor — A survivor (an NPC of the game's roster with a rescue requirement).
- `Survivor(string Id, uint NameTag, ProgressFact Rescue)`
- `Id { get; init; } -> string` — The NPC's debug name.
- `NameTag { get; init; } -> uint` — Loca tag of the NPC's name.
- `Rescue { get; init; } -> ProgressFact` — The fact that the survivor is rescued.
- `Rescued { get; } -> bool?` — Whether the survivor is rescued in this world (tick thread; null when it cannot be read).
- `RescuedFor(Player player) -> bool?` — Whether the survivor is rescued as this character sees it (the world's value, or its own twin in personal...

## SurvivorAssignmentChangedEvent `0.2.16` — A survivor's bed changed: by `Survivors.AssignBed`/`Survivors.UnassignBed` (`SurvivorEvent.ByKiln`), or by the game (a player's summon tool, a destroyed bed, the...
- `NewBed { get; } -> EntityId` — The bed after; None when it has none now.
- `OldBed { get; } -> EntityId` — The bed before; None when it had none.

## SurvivorAssignmentOptions `0.2.16` — How a bed assignment is authorized.
- `SurvivorAssignmentOptions(EntityId Actor, SurvivorAuthorization Authorization = SurvivorAuthorization.PlayerPermissions)`
- `Actor { get; init; } -> EntityId` — The player acting (PlayerPermissions); `EntityId.None` with TrustedServer.
- `Authorization { get; init; } -> SurvivorAuthorization` — PlayerPermissions (the default) or TrustedServer.
- `static Trusted { get; } -> SurvivorAssignmentOptions` — TrustedServer with no actor: for server code that is its own authority.

## SurvivorAuthorization `0.2.16` — Who answers for a bed assignment (`SurvivorAssignmentOptions`).
enum: PlayerPermissions, TrustedServer

## SurvivorCompletion `0.2.16` — How a survivor request ended.
enum: Done, NoChange, Refused, TimedOut, Partial, WorldEnded, Indeterminate

## SurvivorEffects `0.2.16` — What a survivor request really wrote (`SurvivorResult.Effects`).
enum: None, KnowledgeWritten, RegistryConfirmed, ToolGiven, EntityCreated, HomeMoved, BedChanged

## SurvivorEvent `0.2.16` — The base of the survivor events: the survivor, its state before and after, the request (0: the game).
- `After { get; } -> SurvivorInfo` — Its state after the change.
- `Before { get; } -> SurvivorInfo` — Its state before the change.
- `ByKiln { get; } -> bool` — True when a Kiln request made the change.
- `Request { get; } -> ulong` — The kiln.survivors request that made it; 0 when the game made it (a player, the game's upkeep).
- `Step { get; } -> ulong` — The server-world step.
- `Survivor { get; } -> SurvivorId` — The survivor.

## SurvivorId `0.2.16` — A survivor: the game's roster id of an NPC (its NpcId, FNV-1a-32 of the NPC template's GUID; the same value as `Villager.NpcId`).
- `SurvivorId()`
- `SurvivorId(uint Value)`
- `Value { get; init; } -> uint` — The NpcId.

## SurvivorInfo `0.2.16` — A roster survivor as the world has it now.
- `SurvivorInfo(SurvivorId Id, string DebugName, bool Unlocked, ItemId SummonTool, EntityId Entity, EntityId Bed, Guid? BaseId, WorldPosition? Home)`
- `BaseId { get; init; } -> Guid?` — The base whose build zone holds its home; null when none.
- `Bed { get; init; } -> EntityId` — Its bed (the NPC's side of the relation); None when it has none or is not in the world.
- `DebugName { get; init; } -> string` — The roster's debug name (the NPC template's name), e.g. "NPC_Workshop_Blacksmith_Assistant_01".
- `Entity { get; init; } -> EntityId` — The live NPC; `EntityId.None` when it is not in the world.
- `Home { get; init; } -> WorldPosition?` — The point it lives around (set by Summon and by the game's summon tool); null when not in the world.
- `Id { get; init; } -> SurvivorId` — Its NpcId.
- `SummonTool { get; init; } -> ItemId` — The item that summons it; `ItemId` 0 when it has none.
- `Unlocked { get; init; } -> bool` — Recruited for the world (the game's NPC registry).

## SurvivorRecruitedEvent `0.2.16` — A survivor was recruited for the world by `Survivors.RecruitWorld` or `Survivors.Summon` (the NPC registry confirmed the unlock).

## SurvivorRecruitOptions `0.2.16` — Options of `Survivors.RecruitWorld`.
- `SurvivorRecruitOptions(Player? GiveToolTo = null)`
- `GiveToolTo { get; init; } -> Player?` — A player in game who gets one summon tool after the unlock; null for none.

## SurvivorResult `0.2.16` — The end of a survivor request.
- `SurvivorResult(bool Ok, string? Reason, SurvivorInfo Before, SurvivorInfo After, bool AfterKnown, SurvivorEffects Effects, SurvivorCompletion Completion, EntityId DisplacedNpc, int Steps)`
- `After { get; init; } -> SurvivorInfo` — A readback at the end (`AfterKnown`), else the last confirmed state.
- `AfterKnown { get; init; } -> bool` — After is a readback.
- `Before { get; init; } -> SurvivorInfo` — The survivor when the request was taken.
- `Completion { get; init; } -> SurvivorCompletion` — How it ended.
- `DisplacedNpc { get; init; } -> EntityId` — AssignBed: the NPC that lost the bed; None when none.
- `Effects { get; init; } -> SurvivorEffects` — What was really written.
- `Ok { get; init; } -> bool` — Done or NoChange.
- `Reason { get; init; } -> string?` — "Code: words" when not Ok (codes in docs/kiln/services/survivors.md).
- `Steps { get; init; } -> int` — World updates from acceptance to the end.

## Survivors `0.2.16` — The game's roster survivors: read, recruit, summon, beds.
- `static AssignBed(SurvivorId id, EntityId bed, SurvivorAssignmentOptions options) -> Task<SurvivorResult>` — Assigns a bed to a survivor: both ends of the game's relation are written in one step (saved and sent to players...
- `const AssignCapability = "survivors.assign" -> string` — The capability of `Survivors.AssignBed` and `Survivors.UnassignBed`.
- `static BaseAt(WorldPosition position) -> Guid?` — The base whose build zone holds `position` (the id `Survivors.Summon` takes); null when no loaded base zone holds it.
- `static BedOccupant(EntityId bed) -> SurvivorId?` — The survivor a bed is assigned to (the bed's side of the relation); null when it is free.
- `static Find(SurvivorId id) -> SurvivorInfo?` — The survivor with this id, live; null when no roster entry has it.
- `static List() -> IReadOnlyList<SurvivorInfo>` — Every roster entry with its live state, in roster order.
- `static Of(EntityId npc) -> SurvivorInfo?` — The survivor a live NPC entity is; null when it is none (not a roster NPC, or no such entity).
- `const ReadCapability = "survivors.read" -> string` — The capability of the reads.
- `const RecruitCapability = "survivors.recruit" -> string` — The capability of `Survivors.RecruitWorld`.
- `static RecruitWorld(SurvivorId id, SurvivorRecruitOptions? options = null) -> Task<SurvivorResult>` — Recruits a survivor for the world, as its rescue does: its unlock is written, the game's NPC registry must confirm...
- `static Summon(SurvivorId id, Guid baseId, WorldPosition position, float yawDegrees = 0) -> Task<SurvivorResult>` — Brings a survivor to a point in a base: a survivor already in the world is moved there and that becomes its home...
- `const SummonCapability = "survivors.summon" -> string` — The capability of `Survivors.Summon`.
- `static UnassignBed(SurvivorId id, SurvivorAssignmentOptions options) -> Task<SurvivorResult>` — Takes a survivor's bed away: both ends are cleared (and any stale bed that still names it).

## SurvivorSummonedEvent `0.2.16` — A survivor was brought into a base (`Survivors.Summon`).

## SurvivorToolGivenEvent `0.2.16` — A survivor's summon tool went into a player's inventory (`Survivors.RecruitWorld`).

## WorldDifficulty — The world's difficulty: the server's preset and the game settings in effect (kiln.settings).
- `WorldDifficulty(string? Preset, IReadOnlyDictionary<string, object> Settings)`
- `IsCustom { get; } -> bool` — True when `WorldDifficulty.Preset` is Custom: the settings were changed by hand.
- `Number(string name) -> double?` — A setting as a number (factors, durations in ns, option values); null when absent or not numeric.
- `Preset { get; init; } -> string?` — Default, Relaxed, Hard, Survival or Custom (the game's GameSettingsPreset); null when unknown.
- `Settings { get; init; } -> IReadOnlyDictionary<string, object>` — Every game setting's current value by name, as `enshrouded_server.json` spells it.

## WorldIndexState — How far the index of the game's data is.
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
- `static For(EntityId player) -> CharacterProgress` — One character's progress, as `WorldProgress.For`.
- `static For(Player player) -> CharacterProgress` — One character's progress (a player in game).
- `static LimitsAt(int level) -> FlameLimits?` — What the game derives from a flame level: how many Flame Altars and water sources the world may have, the Shroud...
- `static Obelisk(string id) -> Obelisk?` — An Ancient Obelisk by id; null when there is none.
- `static Obelisks { get; } -> IReadOnlyList<Obelisk>` — Every Ancient Obelisk (empty until `WorldProgress.PlacesReady`).
- `static PlacesReady { get; } -> bool` — True once the spires, shrines, roots and obelisks are listed.
- `static SetFlameLevel(int level) -> Task<FlameChange>` — Sets the Mother Flame's level, as the game's own admin command does (the `Level` attribute of the world's progress...
- `static Shrine(string id) -> FlameShrine?` — A Flame Shrine or Sanctum by id (`grassland_flameshrine_01`) or by its marker template name; null when there is none.
- `static Shrines { get; } -> IReadOnlyList<FlameShrine>` — Every Flame Shrine and Flame Sanctum the game's achievements count, by region.
- `static ShroudRoot(string id) -> ShroudRoot?` — A Shroud Root by id; null when there is none.
- `static ShroudRoots { get; } -> IReadOnlyList<ShroudRoot>` — Every Shroud Root the game's achievements count (with positions once `WorldProgress.BossesReady`).
- `static Spire(string id) -> Spire?` — A spire by id (`grassland`) or by its marker template name; null when there is none.
- `static Spires { get; } -> IReadOnlyList<Spire>` — Every Ancient Spire (empty until `WorldProgress.PlacesReady`).
- `static Status { get; } -> WorldProgressStatus` — What world progress can read on this server and how far its index of the game's data is.
- `static Survivors { get; } -> IReadOnlyList<Survivor>` — The survivors (NPCs of the game's roster with a rescue requirement) and whether they are rescued (tick thread).
- `static WhenReady(bool bosses = false, TimeSpan? timeout = null) -> Task<bool>` — Waits off the tick thread until the lists are ready (or failed).

## WorldProgressEvent — Base of the progress events whose fact is knowledge: the world's or one character's.
- `IsWorld { get; } -> bool` — True when the world's knowledge changed (`WorldProgressEvent.Target` is None).
- `Player { get; } -> Player?` — The player whose knowledge changed; null for the world, or when they left in the meantime.
- `Step { get; } -> ulong` — The server-world step of the change.
- `Target { get; } -> EntityId` — None: the world's knowledge changed; else the player whose knowledge changed.

## WorldProgressStatus — What world progress can read on this server, and how far its index of the game's data is.
- `WorldProgressStatus(bool Knowledge, bool World, bool Settings, bool Assets, WorldIndexState Places, WorldIndexState Bosses, int Spires, int Shrines, int Roots, int Obelisks, int BossCount, int TrackedBosses, double PlacesSeconds, double BossesSeconds, IReadOnlyList<string> Problems)`
- `Assets { get; init; } -> bool` — kiln.assets is loaded.
- `BossCount { get; init; } -> int` — The number of bosses found.
- `Bosses { get; init; } -> WorldIndexState` — Bosses and root positions (every chunk of the scene and every template).
- `BossesSeconds { get; init; } -> double` — How long reading the bosses took, in seconds.
- `Knowledge { get; init; } -> bool` — kiln.knowledge is loaded.
- `Obelisks { get; init; } -> int` — The number of obelisks found.
- `Places { get; init; } -> WorldIndexState` — Spires, shrines, roots, obelisks (the query db and the scene's global chunk).
- `PlacesSeconds { get; init; } -> double` — How long reading the places took, in seconds.
- `Problems { get; init; } -> IReadOnlyList<string>` — Problems found while reading the game's data, as text lines.
- `Roots { get; init; } -> int` — The number of Shroud Roots found.
- `Settings { get; init; } -> bool` — kiln.settings is loaded.
- `Shrines { get; init; } -> int` — The number of Flame Shrines and Sanctums found.
- `Spires { get; init; } -> int` — The number of spires found.
- `TrackedBosses { get; init; } -> int` — The number of bosses whose kill the game records.
- `World { get; init; } -> bool` — kiln.world is loaded.
