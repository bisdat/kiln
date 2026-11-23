# Blocks (Kiln.Blocks): quests

## ActionBlock — An action: a quest's `QuestBlock.OnStart` and `QuestBlock.OnFail`, a step's `StepBlock.OnEnter` and `StepBlock.OnDone`, a node's and an option's `OptionBlock.Do`.
- `Advance { get; init; } -> string?` — Finishes a step: "quest.step".
- `Amount { get; init; } -> int` — For `ActionBlock.Signal`: how many to add.
- `At { get; init; } -> double[]?` — For `ActionBlock.Spawn`: the place as [x, y, z] in metres.
- `BindNpc { get; init; } -> string?` — The spawned entity becomes this NPC.
- `Call { get; init; } -> string?` — A C# function (`Quests.RegisterAction`).
- `Chat { get; init; } -> UiText?` — A chat line for the player.
- `Complete { get; init; } -> string?` — Completes the quest of this id.
- `Count { get; init; } -> int` — Items given or taken, spawned entities.
- `Despawn { get; init; } -> string?` — Removes the creatures spawned with this Kiln.Spawn tag.
- `Exact { get; init; } -> bool` — For `ActionBlock.Spawn`: exactly at the place given.
- `Fail { get; init; } -> string?` — Fails the quest of this id.
- `Flag { get; init; } -> int?` — For `ActionBlock.Revoke`: one flag only.
- `Force { get; init; } -> bool` — For `ActionBlock.Start` and `ActionBlock.Advance`.
- `Item { get; init; } -> ItemRef?` — Gives `ActionBlock.Count` of it (what does not fit lands at the player's feet).
- `Knowledge { get; init; } -> string?` — Grants the game's knowledge: `npc:....unlock`.
- `Node { get; init; } -> string?` — For `ActionBlock.Talk`: the node to open at.
- `Offset { get; init; } -> double[]?` — For `ActionBlock.Spawn` and `ActionBlock.Teleport`: metres added to the place, or to the player's position when...
- `Reset { get; init; } -> string?` — Forgets the holder's record of the quest of this id, as if never started.
- `Revoke { get; init; } -> string?` — Revokes it.
- `Signal { get; init; } -> string?` — Feeds `custom` objectives; `ActionBlock.Amount` times.
- `Spawn { get; init; } -> TemplateRef?` — A one-off creature near the player or at `ActionBlock.At`, on the ground outside bases.
- `Start { get; init; } -> string?` — Starts a quest.
- `Tag { get; init; } -> string?` — For `ActionBlock.Spawn`: the Kiln.Spawn tag a kill objective can name.
- `Take { get; init; } -> ItemRef?` — Takes `ActionBlock.Count` of this item from the player.
- `Talk { get; init; } -> string?` — Opens a chat conversation: a dialogue of this mod, at `ActionBlock.Node`.
- `Target { get; init; } -> string?` — For `ActionBlock.Knowledge` and `ActionBlock.Revoke`: "world" or "player".
- `Teleport { get; init; } -> double[]?` — Moves the player.
- `Transient { get; init; } -> bool` — For `ActionBlock.Spawn`: the creature is never written to the save.
- `Value { get; init; } -> uint?` — For `ActionBlock.Knowledge`: the counter amount or flag number.
- `Xp { get; init; } -> uint?` — Experience, as a quest reward.
- `Yaw { get; init; } -> double?` — For `ActionBlock.Spawn`: the creature's yaw.

## BuildGoal — Placing a prop.
- `Count { get; init; } -> int` — How many props.
- `Prop { get; init; } -> ItemRef` — A prop: a block of this mod by its name, or a game prop.

## CollectGoal — What the inventory holds now (it goes down when the items do).
- `Count { get; init; } -> int` — How many.
- `Item { get; init; } -> ItemRef` — The item to hold.

## ConditionBlock — A condition, checked on the server (also when an option is chosen): exactly one of `ConditionBlock.Knowledge`, `ConditionBlock.Level`, `ConditionBlock.Quest`...
- `All { get; init; } -> IReadOnlyList<ConditionBlock>?` — Holds when every condition in the list holds.
- `Any { get; init; } -> IReadOnlyList<ConditionBlock>?` — Holds when at least one condition in the list holds.
- `Count { get; init; } -> int` — With `ConditionBlock.Item`: how many.
- `Is { get; init; } -> string?` — The state a `ConditionBlock.Quest` or a `ConditionBlock.Step` has to be in.
- `Item { get; init; } -> ItemRef?` — At least `ConditionBlock.Count` of this item are in the inventory.
- `Knowledge { get; init; } -> string?` — The game's knowledge holds it.
- `Level { get; init; } -> int?` — The character's level is at least this.
- `Not { get; init; } -> ConditionBlock?` — Holds when this condition does not.
- `Predicate { get; init; } -> string?` — A C# function (`Quests.RegisterPredicate`).
- `Quest { get; init; } -> string?` — A quest of this mod (or "other-mod:quest"), in the state `ConditionBlock.Is` (default done): locked, available...
- `Step { get; init; } -> string?` — "quest.step" in the state `ConditionBlock.Is` (default done): pending, active, done.

## CraftGoal — A successful craft by the player: of an item, or a recipe (`0x...`).
- `Count { get; init; } -> int` — How many crafts.
- `Item { get; init; } -> ItemRef?` — The item to craft.
- `Recipe { get; init; } -> string?` — A recipe by its id (`0x...`).

## CustomGoal — `Quests.Signal`, the `signal` action, or a C# progress function counts: a signal name, and how many.
- `Count { get; init; } -> int` — How many signals.
- `Signal { get; init; } -> string` — The signal's name.

## DeliverGoal — Finished by an `ActionBlock.Advance` or the NPC's hand-over: takes the items then, and refuses without them.
- `Count { get; init; } -> int` — How many.
- `Item { get; init; } -> ItemRef` — The item to hand over.
- `To { get; init; } -> string?` — The NPC who takes them: an `NpcBlock` of this mod by its name, or an id of the mod's quests/*.json.

## DialogueBlock — A conversation in the game's own NPC menu: nodes of text and options, each with a condition and actions.
- `Farewell { get; init; } -> IReadOnlyList<UiText>?` — Replace the NPC's farewell lines.
- `Greet { get; init; } -> bool?` — The start node's text is a greeting.
- `Greetings { get; init; } -> IReadOnlyList<GreetingBlock>?` — More greetings, shown before the plain one while their condition holds.
- `Hint { get; init; } -> string?` — The marker over the NPC's head on the greeting: Hint, Attention, MainQuest.
- `Nodes { get; init; } -> IDictionary<string, DialogueNodeBlock>` — The nodes by name: text and options.
- `Start { get; init; } -> string` — The first node.
- `Transition { get; init; } -> IReadOnlyList<UiText>?` — Replace the line between topics.

## DialogueHandle — What `Blocks.Define` gives back for a dialogue.
- `Definition { get; } -> DialogueDefinition` — The definition the engine runs: `Dialogues.Define` took this object.
- `static implicit operator DialogueDefinition(DialogueHandle handle) -> DialogueDefinition` — A handle goes where a `DialogueDefinition` does: it converts to its `DialogueHandle.Definition`.

## DialogueNodeBlock — A node of a dialogue: what is said, the options that follow, and what happens when it opens.
- `Do { get; init; } -> IReadOnlyList<ActionBlock>?` — Run when the node opens.
- `Goto { get; init; } -> string?` — A node without options: where "Continue" goes.
- `Hint { get; init; } -> string?` — The marker over the NPC's head on this node's greeting: Hint, Attention or MainQuest.
- `Options { get; init; } -> IReadOnlyList<OptionBlock>?` — Add new options at the end: positions and node names feed the lines' ids.
- `Speaker { get; init; } -> UiText?` — Overrides the NPC's name for this node.
- `Text { get; init; } -> UiText?` — What the NPC says at this node.
- `Voice { get; init; } -> string?` — A voice-over line the game's dialogue plays: a name of the mod's cooked audio, or a GUID.

## Do — Actions, the short way: `Do.Start("lost-crates")`, `Do.Advance("lost-crates.return")`, `Do.Give("Material_T1_Twigs", 5)`.
- `static Advance(string questDotStep, bool force = false) -> ActionBlock` — Finishes a step (`"quest.step"`).
- `static Call(string name) -> ActionBlock` — Calls a C# function registered with `Quests.RegisterAction`.
- `static Chat(UiText text) -> ActionBlock` — A chat line for the player.
- `static Complete(string quest) -> ActionBlock` — Completes a quest.
- `static Fail(string quest) -> ActionBlock` — Fails a quest.
- `static Give(ItemRef item, int count = 1) -> ActionBlock` — Gives `count` of `item`.
- `static Knowledge(string name, string? target = null) -> ActionBlock` — Grants the game's knowledge `name`.
- `static Reset(string quest) -> ActionBlock` — Forgets the holder's record of a quest, as if never started.
- `static Revoke(string name, string? target = null) -> ActionBlock` — Revokes the game's knowledge `name`, from the "world" or the "player" `target`.
- `static Signal(string name, int amount = 1) -> ActionBlock` — Feeds `custom` objectives with the signal `name`, `amount` times.
- `static Start(string quest, bool force = false) -> ActionBlock` — Starts a quest.
- `static Take(ItemRef item, int count = 1) -> ActionBlock` — Takes `count` of `item`.
- `static Talk(string dialogue, string? node = null) -> ActionBlock` — Opens a chat conversation with the dialogue `dialogue`, at `node`.
- `static Teleport(double x, double y, double z) -> ActionBlock` — Moves the player to the place (x, y, z).
- `static Xp(uint amount) -> ActionBlock` — Pays `amount` experience.

## Goal — Objectives, the short way: `Goal.Kill(tag: "crates", count: 6)`, `Goal.Deliver("Material_T1_Wood_Logs", 3, to: "ilse")`.
- `static Build(ItemRef prop, int count = 1) -> ObjectiveBlock` — Place `count` of the prop `prop`.
- `static Collect(ItemRef item, int count = 1) -> ObjectiveBlock` — Hold `count` of `item`.
- `static Craft(ItemRef item, int count = 1) -> ObjectiveBlock` — Craft `count` of `item`.
- `static Custom(string signal, int count = 1) -> ObjectiveBlock` — `count` signals of the name `signal`.
- `static Deliver(ItemRef item, int count, string to) -> ObjectiveBlock` — Hand `count` of `item` to the NPC `to`.
- `static Kill(string? template = null, string? family = null, string? tag = null, int count = 1) -> ObjectiveBlock` — A kill objective of a template, a family or a tag: `Goal.Kill(tag: "crates", count: 6)`.
- `static Knowing(string knowledge) -> ObjectiveBlock` — The game's knowledge holds `knowledge` (`npc:...`, `quest:...`, `query:...`, `0x...`).
- `static Reach(double x, double y, double z, double radius = 10) -> ObjectiveBlock` — Get within `radius` metres of the place (x, y, z).
- `static ReachSite(double radius = 10) -> ObjectiveBlock` — Being at the step's `StepBlock.Site`.
- `static Talk(string npc, string? node = null, string? line = null) -> ObjectiveBlock` — Talk to the NPC `npc`, optionally to hear one `line` or to reach a conversation `node`.

## GreetingBlock — A greeting that wins over the plain one while its condition holds.
- `Hint { get; init; } -> string?` — The marker over the NPC's head on this greeting: Hint, Attention or MainQuest.
- `If { get; init; } -> ConditionBlock?` — The greeting is used only while this holds.
- `Text { get; init; } -> UiText` — The greeting's text.
- `Voice { get; init; } -> string?` — A voice-over line for the greeting: a name of the mod's cooked audio, or a GUID.

## IQuestBlock — The blocks of a quest chain: a quest, a dialogue.

## KillGoal — A death credited to the player (a world quest: any death): of one template, a family (part of the name) or a Kiln.Spawn tag.
- `Count { get; init; } -> int` — How many kills.
- `CountDestroyed { get; init; } -> bool` — Kiln.Spawn destroying a match counts too (default: only deaths).
- `Family { get; init; } -> string?` — Part of the victim's template name (`"Scavenger"`, `"Vukah"`).
- `Near { get; init; } -> double[]?` — Only near this place, within `KillGoal.Radius`.
- `Radius { get; init; } -> double?` — Metres around `KillGoal.Near`.
- `Tag { get; init; } -> string?` — The Kiln.Spawn tag of the victim.
- `Template { get; init; } -> string?` — An exact template name of the victim, such as `Enemy_Scavenger_Melee01`.

## KnowledgeBlock `0.2.14` — The game's tracking of one of the mod's items or recipes, as it tracks its own: `new KnowledgeBlock { Trigger = KnowledgeTrigger.Pickup, Of = "rubble-tool" }`.
- `Identity { get; init; } -> BlockIdentity?` — The id to track when it is not a block of this mod by name: the identity of the item or recipe...
- `Of { get; init; } -> string?` — The item or recipe, a block of this mod by name or handle: an item block (Pickup: the item; Craft: its own recipe)...
- `Scope { get; init; } -> TrackingScope?` — Whose knowledge.
- `Trigger { get; init; } -> KnowledgeTrigger` — What is tracked: `KnowledgeTrigger.Pickup` for an item, `KnowledgeTrigger.Craft` for a recipe.

## KnowledgeTrigger `0.2.14` — What the game tracks: an item found, or a recipe crafted.
enum: Pickup, Craft

## MarkerBlock — The map marker of a step: a fixed place, a radius, a name; the player's own pin follows.
- `At { get; init; } -> double[]?` — The marker's place as [x, y, z] in metres.
- `Base { get; init; } -> string?` — The marker template to copy.
- `Hidden { get; init; } -> bool` — True for `MarkerBlock.None`: the step shows no marker.
- `Name { get; init; } -> UiText?` — The marker's name on the map.
- `static None { get; } -> MarkerBlock` — No marker for the step (JSON `"marker": false`).
- `Radius { get; init; } -> double?` — The marker's radius in metres.
- `Waypoint { get; init; } -> bool` — Also write the player's own map and compass pin (default true).

## ObjectiveBlock — One thing to do in a step: exactly one of `ObjectiveBlock.Kill`, `ObjectiveBlock.Collect`, `ObjectiveBlock.Deliver`, `ObjectiveBlock.Reach`, `ObjectiveBlock.Talk`...
- `Build { get; init; } -> BuildGoal?` — Place a prop: see `BuildGoal`.
- `Collect { get; init; } -> CollectGoal?` — Hold items: see `CollectGoal`.
- `Craft { get; init; } -> CraftGoal?` — Craft something: see `CraftGoal`.
- `Custom { get; init; } -> CustomGoal?` — A signal name, or `CustomGoal`.
- `Deliver { get; init; } -> DeliverGoal?` — Hand items over: see `DeliverGoal`.
- `Kill { get; init; } -> KillGoal?` — Kill creatures: see `KillGoal`.
- `Knowledge { get; init; } -> string?` — The game's knowledge holds it: `npc:...`, `quest:...`, `query:...`, `0x...`.
- `Reach { get; init; } -> ReachGoal?` — Get to a place: see `ReachGoal`.
- `Talk { get; init; } -> TalkGoal?` — Talk to an NPC: see `TalkGoal`.
- `Text { get; init; } -> UiText?` — Shown instead of the step's text when a step has several objectives.

## OptionBlock — An option of a node: a topic in the game's NPC menu, with its condition, its actions and where it leads.
- `Close { get; init; } -> bool` — The NPC plays its outro after the reply.
- `Do { get; init; } -> IReadOnlyList<ActionBlock>?` — Run when it is chosen.
- `Goto { get; init; } -> string?` — The next node: its text is the reply, and its options join the NPC's topics from then on.
- `Hint { get; init; } -> string?` — The topic's marker: Hint, Attention or MainQuest.
- `If { get; init; } -> ConditionBlock?` — Shown only while this holds; checked again when chosen.
- `Once { get; init; } -> bool` — The topic goes away once heard.
- `Reply { get; init; } -> UiText?` — What the NPC answers.
- `Text { get; init; } -> UiText` — The topic's text.
- `Voice { get; init; } -> string?` — The voice-over line of the reply.

## PlaceBlock — A place found at run time: on real ground (the voxel surface), outside every base, not too steep.
- `PlaceBlock(PlaceBlock original)`
- `At { get; init; } -> double[]?` — A fixed place (or the centre of the search).
- `Distance { get; init; } -> double[]?` — Metres from what it is near: `[min, max]`.
- `Exact { get; init; } -> bool` — Exactly `PlaceBlock.At`: no ground, no base check.
- `InBase { get; init; } -> bool` — A base is fine (a friendly NPC that belongs in one).
- `Margin { get; init; } -> double?` — Metres to keep from a base's build zone (default 20).
- `Near { get; init; } -> string?` — "player" (who gets there first), or an NPC: a block of this mod by its name, or an id of the mod's quests/*.json.

## QuestBlock — A quest chain: steps that run in order, each with objectives, and what it pays.
- `Abandonable { get; init; } -> bool` — Players may give it up (never a world quest; default true).
- `Auto { get; init; } -> bool` — Starts by itself as soon as it is available.
- `Description { get; init; } -> UiText?` — What the quest is about, as text.
- `Giver { get; init; } -> string?` — The NPC who offers it: an `NpcBlock` of this mod by its name, an NPC of the mod's quests/*.json, or "other-mod:npc".
- `Hidden { get; init; } -> bool` — Not listed until it is active.
- `Main { get; init; } -> bool` — A main quest of the game's journal.
- `Name { get; init; } -> UiText` — The name players read in the journal: a text, or one per language (`{ "en": "..." }` in JSON).
- `OnFail { get; init; } -> IReadOnlyList<ActionBlock>?` — Run when it fails or is given up.
- `OnStart { get; init; } -> IReadOnlyList<ActionBlock>?` — Run when it starts.
- `PartyRadius { get; init; } -> double?` — Party: metres within which players share kills and crafts.
- `Priority { get; init; } -> int?` — The journal's order (the game uses 40..100; default 50).
- `Repeatable { get; init; } -> bool` — Can be started again after it is done.
- `Requires { get; init; } -> IReadOnlyList<ConditionBlock>?` — It can start once all of these hold: `Requires = [Req.Level(8)]`.
- `Retryable { get; init; } -> bool` — Can be started again after it failed or was given up (default true).
- `Rewards { get; init; } -> RewardBlock?` — What finishing the quest pays: experience, items, a status.
- `Scope { get; init; } -> QuestScope` — Whose state it is: `QuestScope.Player` (the default) gives each character its own; `QuestScope.Party` shares kills...
- `Source { get; init; } -> string?` — The journal's portrait (`None`, `Flame`, `Blacksmith`, `Alchemist`, ...).
- `Steps { get; init; } -> IReadOnlyList<StepBlock>` — The steps, run in order.

## QuestHandle — What `Blocks.Define` gives back for a quest: the definition the engine runs.
- `Definition { get; } -> QuestDefinition` — What it became: `Quests.Define` took this object.
- `FullId { get; } -> string` — `<mod>:<name>`, for `Quests.Start`, `Quests.StatusOf` and the rest of `Quests`.

## ReachGoal — Being there, checked twice a second: the step's `ReachGoal.Site`, a place, an offset from where the player stood, or the nearest entity of a template.
- `At { get; init; } -> double[]?` — The place as [x, y, z] in metres.
- `Marker { get; init; } -> string?` — A template's name: the nearest entity of that template is the place.
- `Offset { get; init; } -> double[]?` — An offset in metres from where the player stood when the step started, in place of `ReachGoal.At`.
- `Radius { get; init; } -> double?` — Metres around the place.
- `Site { get; init; } -> bool` — True: the step's `StepBlock.Site` is the place.

## Req — Conditions, the short way: `Req.Level(8)`, `Req.Quest("lost-crates", "active")`.
- `static All(params ConditionBlock[] all) -> ConditionBlock` — Every one of these holds.
- `static Any(params ConditionBlock[] any) -> ConditionBlock` — At least one of these holds.
- `static Item(ItemRef item, int count = 1) -> ConditionBlock` — At least `count` of `item` are in the inventory.
- `static Knowledge(string name) -> ConditionBlock` — The game's knowledge holds `name`.
- `static Level(int level) -> ConditionBlock` — The character's level is at least `level`.
- `static Not(ConditionBlock not) -> ConditionBlock` — This does not hold.
- `static Predicate(string name) -> ConditionBlock` — A C# function registered with `Quests.RegisterPredicate` returns true.
- `static Quest(string quest, string state = "done") -> ConditionBlock` — A quest is in a state: locked, available, active, done, failed or started.
- `static Step(string questDotStep, string state = "done") -> ConditionBlock` — A step (`"quest.step"`) is in a state: pending, active or done.

## RewardBlock — What a quest or a step pays, all at once: experience, items, a status, knowledge, a loot table.
- `Items { get; init; } -> IDictionary<ItemRef, int>` — Item and count: a game item, or an item this mod defines.
- `Knowledge { get; init; } -> IReadOnlyList<string>?` — The game's knowledge granted: `npc:....unlock` unlocks an NPC.
- `LootTable { get; init; } -> string?` — A loot table, rolled when the reward is paid: the name of a `LootTableBlock` (`lootTables`) of this mod, or...
- `Status { get; init; } -> StatusApply?` — A status put on the player: a `StatusBlock` of this mod, or a game status; the status's own time, or...
- `Xp { get; init; } -> uint?` — Experience, through the game's quest path.

## StepBlock — One step of a quest.
- `Id { get; init; } -> string` — Letters, digits, '_' and '-'; unique in the quest.
- `Marker { get; init; } -> MarkerBlock?` — The map marker.
- `Objective { get; init; } -> ObjectiveBlock?` — The step's objective.
- `Objectives { get; init; } -> IReadOnlyList<ObjectiveBlock>?` — More objectives, or instead of `StepBlock.Objective`.
- `OnDone { get; init; } -> IReadOnlyList<ActionBlock>?` — Run when the step is done, after its `StepBlock.Rewards` are paid.
- `OnEnter { get; init; } -> IReadOnlyList<ActionBlock>?` — Run when the step is entered.
- `RecommendedLevel { get; init; } -> int` — The recommended level the game's journal shows with the step.
- `Rewards { get; init; } -> RewardBlock?` — Paid when the step is done.
- `Site { get; init; } -> PlaceBlock?` — A place found at run time, on real ground outside every base, once per world.
- `Spawn { get; init; } -> IReadOnlyList<StepSpawnBlock>?` — Creatures the step brings into the world (one, or a list in JSON).
- `Text { get; init; } -> UiText?` — The journal's paragraph.
- `Title { get; init; } -> UiText?` — The journal's short line (the list, the tracked quest).
- `Voice { get; init; } -> string?` — A voice-over line in the journal: a name of the mod's cooked audio, or a GUID.

## StepSpawnBlock — Creatures a step brings into the world.
- `Count { get; init; } -> int` — How many to spawn.
- `Level { get; init; } -> int` — The level to spawn at (0 or none: the template's).
- `Retries { get; init; } -> int?` — How many times Kiln spawns them again when the game removes them before they die (default 5).
- `Spread { get; init; } -> double?` — Metres between them, 0 or more.
- `Tag { get; init; } -> string?` — The Kiln.Spawn tag: a kill objective can name it.
- `Template { get; init; } -> TemplateRef` — A game template: `GameTemplates` constant or name.

## TalkGoal — Talking to an NPC; `TalkGoal.Line`: one line of a dialogue, `TalkGoal.Node`: reaching a node of a chat conversation.
- `Line { get; init; } -> string?` — A dialogue line to hear: `line:...`, `0x...` or `<dialogue>.<node>.<option>`.
- `Node { get; init; } -> string?` — A node of a chat conversation that must be reached.
- `Npc { get; init; } -> string` — An `NpcBlock` of this mod by its name, an id of the mod's quests/*.json, a template name, or `npc:<debugName>`.

## TrackingScope `0.2.14` — Whose knowledge it is.
enum: Player, World, Both
