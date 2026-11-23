// SPDX-License-Identifier: MIT
// Quests and dialogues as blocks: the same chains, steps, objectives, conditions, actions and conversations as quests/*.json and Quests.Define, written as
// block records (C#) or in blocks/*.json ("quests", "dialogues"), and lowered to QuestDefinition and DialogueDefinition (QuestCompiler.cs).
// Where quests/*.json has a key, the block has the same one, so the page that explains a quest (docs/kiln/modding/quests.md) explains a block too.
using System.Text.Json.Serialization;

namespace Kiln;

/// <summary>The blocks of a quest chain: a quest, a dialogue. They are built by the game's own journal and NPC menu, so players need the mod (<c>clients</c> "mod").</summary>
public interface IQuestBlock : IBlock { }

// ------------------------------------------------------------------------------------------------------- quests -----

/// <summary>
/// A quest chain: steps that run in order, each with objectives, and what it pays. The game shows it in its own journal, tracked-quest line and map; the server
/// runs it for each character on each world. The block's name is the quest's id (letters, digits, '_' and '-'): keep it, and the ids of its steps, because
/// players' progress is filed under them. Players need the mod, and a quest changes the game's journal at start: one defined in code is there from the next start
/// (docs/kiln/modding/quests.md "Quests as blocks").
/// </summary>
[BlockClients(ClientsLevel.Mod)]
public sealed record QuestBlock : IQuestBlock
{
    /// <summary>The name players read in the journal: a text, or one per language (<c>{ "en": "..." }</c> in JSON). Required.</summary>
    [JsonConverter(typeof(UiTextConverter))]
    public required UiText Name { get; init; }
    /// <summary>What the quest is about, as text.</summary>
    [JsonConverter(typeof(UiTextConverter))]
    public UiText? Description { get; init; }
    /// <summary>The NPC who offers it: an <see cref="NpcBlock"/> of this mod by its name, an NPC of the mod's quests/*.json, or "other-mod:npc". A quest with a giver
    /// starts through that NPC, an action or code.</summary>
    public string? Giver { get; init; }
    /// <summary>Whose state it is: <see cref="QuestScope.Player"/> (the default) gives each character its own; <see cref="QuestScope.Party"/> shares kills, crafts, builds and signals within <see cref="PartyRadius"/> and pays each player; <see cref="QuestScope.World"/> is one state for the world, and cannot be given up.</summary>
    public QuestScope Scope { get; init; } = QuestScope.Player;
    /// <summary>Party: metres within which players share kills and crafts. Default 60.</summary>
    public double? PartyRadius { get; init; }
    /// <summary>A main quest of the game's journal.</summary>
    public bool Main { get; init; }
    /// <summary>Starts by itself as soon as it is available.</summary>
    public bool Auto { get; init; }
    /// <summary>Can be started again after it is done.</summary>
    public bool Repeatable { get; init; }
    /// <summary>Can be started again after it failed or was given up (default true).</summary>
    public bool Retryable { get; init; } = true;
    /// <summary>Players may give it up (never a world quest; default true).</summary>
    public bool Abandonable { get; init; } = true;
    /// <summary>Not listed until it is active.</summary>
    public bool Hidden { get; init; }
    /// <summary>The journal's portrait (<c>None</c>, <c>Flame</c>, <c>Blacksmith</c>, <c>Alchemist</c>, ...).</summary>
    public string? Source { get; init; }
    /// <summary>The journal's order (the game uses 40..100; default 50).</summary>
    public int? Priority { get; init; }
    /// <summary>It can start once all of these hold: <c>Requires = [Req.Level(8)]</c>.</summary>
    public IReadOnlyList<ConditionBlock>? Requires { get; init; }
    /// <summary>The steps, run in order. At least one; their ids are unique in the quest.</summary>
    public required IReadOnlyList<StepBlock> Steps { get; init; }
    /// <summary>What finishing the quest pays: experience, items, a status.</summary>
    public RewardBlock? Rewards { get; init; }
    /// <summary>Run when it starts.</summary>
    public IReadOnlyList<ActionBlock>? OnStart { get; init; }
    /// <summary>Run when it fails or is given up.</summary>
    public IReadOnlyList<ActionBlock>? OnFail { get; init; }
}

/// <summary>One step of a quest. It is done when all its objectives are.</summary>
public sealed record StepBlock
{
    /// <summary>Letters, digits, '_' and '-'; unique in the quest. Keep it: progress is filed under it.</summary>
    public required string Id { get; init; }
    /// <summary>The journal's paragraph.</summary>
    [JsonConverter(typeof(UiTextConverter))]
    public UiText? Text { get; init; }
    /// <summary>The journal's short line (the list, the tracked quest). Default the text.</summary>
    [JsonConverter(typeof(UiTextConverter))]
    public UiText? Title { get; init; }
    /// <summary>A voice-over line in the journal: a name of the mod's cooked audio, or a GUID.</summary>
    public string? Voice { get; init; }
    /// <summary>The recommended level the game's journal shows with the step. 0 or more.</summary>
    public int RecommendedLevel { get; init; }
    /// <summary>The step's objective. <see cref="Objectives"/> adds more.</summary>
    public ObjectiveBlock? Objective { get; init; }
    /// <summary>More objectives, or instead of <see cref="Objective"/>. A step is done when all of them are.</summary>
    public IReadOnlyList<ObjectiveBlock>? Objectives { get; init; }
    /// <summary>The map marker. A step with a place has one by itself; <c>MarkerBlock.None</c> (JSON <c>false</c>) shows nothing.</summary>
    [JsonConverter(typeof(MarkerConverter))]
    public MarkerBlock? Marker { get; init; }
    /// <summary>A place found at run time, on real ground outside every base, once per world.</summary>
    public PlaceBlock? Site { get; init; }
    /// <summary>Creatures the step brings into the world (one, or a list in JSON).</summary>
    [JsonConverter(typeof(OneOrManyConverter<StepSpawnBlock>))]
    public IReadOnlyList<StepSpawnBlock>? Spawn { get; init; }
    /// <summary>Paid when the step is done.</summary>
    public RewardBlock? Rewards { get; init; }
    /// <summary>Run when the step is entered.</summary>
    public IReadOnlyList<ActionBlock>? OnEnter { get; init; }
    /// <summary>Run when the step is done, after its <see cref="Rewards"/> are paid.</summary>
    public IReadOnlyList<ActionBlock>? OnDone { get; init; }
}

/// <summary>The map marker of a step: a fixed place, a radius, a name; the player's own pin follows.</summary>
public sealed record MarkerBlock
{
    /// <summary>No marker for the step (JSON <c>"marker": false</c>).</summary>
    public static MarkerBlock None { get; } = new() { Hidden = true };

    /// <summary>True for <see cref="None"/>: the step shows no marker. Not written in JSON.</summary>
    [JsonIgnore] public bool Hidden { get; init; }
    /// <summary>The marker's place as [x, y, z] in metres. Without it, the step's own place decides.</summary>
    public double[]? At { get; init; }
    /// <summary>The marker's radius in metres. 0 when left out.</summary>
    public double? Radius { get; init; }
    /// <summary>Also write the player's own map and compass pin (default true).</summary>
    public bool Waypoint { get; init; } = true;
    /// <summary>The marker's name on the map. Default the step's text.</summary>
    [JsonConverter(typeof(UiTextConverter))]
    public UiText? Name { get; init; }
    /// <summary>The marker template to copy.</summary>
    public string? Base { get; init; }
}

/// <summary>A place found at run time: on real ground (the voxel surface), outside every base, not too steep. It is kept per world, so everyone doing the step goes
/// to one place. Say what it is near: <see cref="Near"/> ("player", or an NPC), or <see cref="At"/>.</summary>
public record PlaceBlock
{
    /// <summary>A fixed place (or the centre of the search).</summary>
    public double[]? At { get; init; }
    /// <summary>"player" (who gets there first), or an NPC: a block of this mod by its name, or an id of the mod's quests/*.json.</summary>
    public string? Near { get; init; }
    /// <summary>Metres from what it is near: <c>[min, max]</c>. It widens by itself until a good spot is found.</summary>
    public double[]? Distance { get; init; }
    /// <summary>Metres to keep from a base's build zone (default 20).</summary>
    public double? Margin { get; init; }
    /// <summary>A base is fine (a friendly NPC that belongs in one). Enemies never take this: the game pacifies them in bases and may remove them.</summary>
    public bool InBase { get; init; }
    /// <summary>Exactly <see cref="At"/>: no ground, no base check.</summary>
    public bool Exact { get; init; }
}

/// <summary>Creatures a step brings into the world. Kiln checks every second that they exist and spawns them again when the game removed them; they are never saved.
/// Where: the step's <see cref="StepBlock.Site"/>, or the place named here.</summary>
public sealed record StepSpawnBlock : PlaceBlock
{
    /// <summary>A game template: <c>GameTemplates</c> constant or name.</summary>
    public required TemplateRef Template { get; init; }
    /// <summary>How many to spawn. At least 1. Default 1.</summary>
    public int Count { get; init; } = 1;
    /// <summary>The Kiln.Spawn tag: a kill objective can name it.</summary>
    public string? Tag { get; init; }
    /// <summary>Metres between them, 0 or more. Default 4.</summary>
    public double? Spread { get; init; }
    /// <summary>The level to spawn at (0 or none: the template's).</summary>
    public int Level { get; init; }
    /// <summary>How many times Kiln spawns them again when the game removes them before they die (default 5).</summary>
    public int? Retries { get; init; }
}

// ------------------------------------------------------------------------------------------------------ goals -----

/// <summary>One thing to do in a step: exactly one of <see cref="Kill"/>, <see cref="Collect"/>, <see cref="Deliver"/>, <see cref="Reach"/>, <see cref="Talk"/>,
/// <see cref="Craft"/>, <see cref="Build"/>, <see cref="Knowledge"/> or <see cref="Custom"/>. <c>Goal.Kill(tag: "crates", count: 6)</c>.</summary>
public sealed record ObjectiveBlock
{
    /// <summary>Kill creatures: see <see cref="KillGoal"/>. Exactly one of the objective's kinds is set.</summary>
    public KillGoal? Kill { get; init; }
    /// <summary>Hold items: see <see cref="CollectGoal"/>.</summary>
    public CollectGoal? Collect { get; init; }
    /// <summary>Hand items over: see <see cref="DeliverGoal"/>.</summary>
    public DeliverGoal? Deliver { get; init; }
    /// <summary>Get to a place: see <see cref="ReachGoal"/>.</summary>
    public ReachGoal? Reach { get; init; }
    /// <summary>Talk to an NPC: see <see cref="TalkGoal"/>.</summary>
    public TalkGoal? Talk { get; init; }
    /// <summary>Craft something: see <see cref="CraftGoal"/>.</summary>
    public CraftGoal? Craft { get; init; }
    /// <summary>Place a prop: see <see cref="BuildGoal"/>.</summary>
    public BuildGoal? Build { get; init; }
    /// <summary>The game's knowledge holds it: <c>npc:…</c>, <c>quest:…</c>, <c>query:…</c>, <c>0x…</c>.</summary>
    public string? Knowledge { get; init; }
    /// <summary>A signal name, or <see cref="CustomGoal"/>.</summary>
    [JsonConverter(typeof(CustomGoalConverter))]
    public CustomGoal? Custom { get; init; }
    /// <summary>Shown instead of the step's text when a step has several objectives.</summary>
    [JsonConverter(typeof(UiTextConverter))]
    public UiText? Text { get; init; }
}

/// <summary>A death credited to the player (a world quest: any death): of one template, a family (part of the name) or a Kiln.Spawn tag.</summary>
public sealed record KillGoal
{
    /// <summary>An exact template name of the victim, such as <c>Enemy_Scavenger_Melee01</c>.</summary>
    public string? Template { get; init; }
    /// <summary>Part of the victim's template name (<c>"Scavenger"</c>, <c>"Vukah"</c>).</summary>
    public string? Family { get; init; }
    /// <summary>The Kiln.Spawn tag of the victim.</summary>
    public string? Tag { get; init; }
    /// <summary>How many kills. At least 1. Default 1.</summary>
    public int Count { get; init; } = 1;
    /// <summary>Only near this place, within <see cref="Radius"/>.</summary>
    public double[]? Near { get; init; }
    /// <summary>Metres around <see cref="Near"/>. More than 0; default 10.</summary>
    public double? Radius { get; init; }
    /// <summary>Kiln.Spawn destroying a match counts too (default: only deaths).</summary>
    public bool CountDestroyed { get; init; }
}

/// <summary>What the inventory holds now (it goes down when the items do). Takes nothing.</summary>
public sealed record CollectGoal
{
    /// <summary>The item to hold.</summary>
    public required ItemRef Item { get; init; }
    /// <summary>How many. At least 1. Default 1.</summary>
    public int Count { get; init; } = 1;
}

/// <summary>Finished by an <see cref="ActionBlock.Advance"/> or the NPC's hand-over: takes the items then, and refuses without them.</summary>
public sealed record DeliverGoal
{
    /// <summary>The item to hand over.</summary>
    public required ItemRef Item { get; init; }
    /// <summary>How many. At least 1. Default 1.</summary>
    public int Count { get; init; } = 1;
    /// <summary>The NPC who takes them: an <see cref="NpcBlock"/> of this mod by its name, or an id of the mod's quests/*.json.</summary>
    public string? To { get; init; }
}

/// <summary>Being there, checked twice a second: the step's <see cref="Site"/>, a place, an offset from where the player stood, or the nearest entity of a template.</summary>
public sealed record ReachGoal
{
    /// <summary>The place as [x, y, z] in metres.</summary>
    public double[]? At { get; init; }
    /// <summary>An offset in metres from where the player stood when the step started, in place of <see cref="At"/>.</summary>
    public double[]? Offset { get; init; }
    /// <summary>A template's name: the nearest entity of that template is the place.</summary>
    public string? Marker { get; init; }
    /// <summary>True: the step's <see cref="StepBlock.Site"/> is the place.</summary>
    public bool Site { get; init; }
    /// <summary>Metres around the place. More than 0; default 10.</summary>
    public double? Radius { get; init; }
}

/// <summary>Talking to an NPC; <see cref="Line"/>: one line of a dialogue, <see cref="Node"/>: reaching a node of a chat conversation.</summary>
public sealed record TalkGoal
{
    /// <summary>An <see cref="NpcBlock"/> of this mod by its name, an id of the mod's quests/*.json, a template name, or <c>npc:&lt;debugName&gt;</c>.</summary>
    public required string Npc { get; init; }
    /// <summary>A dialogue line to hear: <c>line:…</c>, <c>0x…</c> or <c>&lt;dialogue&gt;.&lt;node&gt;.&lt;option&gt;</c>.</summary>
    public string? Line { get; init; }
    /// <summary>A node of a chat conversation that must be reached.</summary>
    public string? Node { get; init; }
}

/// <summary>A successful craft by the player: of an item, or a recipe (<c>0x…</c>).</summary>
public sealed record CraftGoal
{
    /// <summary>The item to craft. Needs this or <see cref="Recipe"/>.</summary>
    public ItemRef? Item { get; init; }
    /// <summary>A recipe by its id (<c>0x…</c>). Needs this or <see cref="Item"/>.</summary>
    public string? Recipe { get; init; }
    /// <summary>How many crafts. At least 1. Default 1.</summary>
    public int Count { get; init; } = 1;
}

/// <summary>Placing a prop.</summary>
public sealed record BuildGoal
{
    /// <summary>A prop: a block of this mod by its name, or a game prop.</summary>
    public required ItemRef Prop { get; init; }
    /// <summary>How many props. At least 1. Default 1.</summary>
    public int Count { get; init; } = 1;
}

/// <summary><c>Quests.Signal</c>, the <c>signal</c> action, or a C# progress function counts: a signal name, and how many.</summary>
public sealed record CustomGoal
{
    /// <summary>The signal's name. A name is needed.</summary>
    public required string Signal { get; init; }
    /// <summary>How many signals. At least 1. Default 1.</summary>
    public int Count { get; init; } = 1;
}

// ------------------------------------------------------------------------------------------------- conditions -----

/// <summary>A condition, checked on the server (also when an option is chosen): exactly one of <see cref="Knowledge"/>, <see cref="Level"/>, <see cref="Quest"/>,
/// <see cref="Step"/>, <see cref="Item"/>, <see cref="Predicate"/>, <see cref="All"/>, <see cref="Any"/> or <see cref="Not"/>. <c>Req.Level(8)</c>.</summary>
public sealed record ConditionBlock
{
    /// <summary>The game's knowledge holds it.</summary>
    public string? Knowledge { get; init; }
    /// <summary>The character's level is at least this.</summary>
    public int? Level { get; init; }
    /// <summary>A quest of this mod (or "other-mod:quest"), in the state <see cref="Is"/> (default done): locked, available, active, done, failed, started.</summary>
    public string? Quest { get; init; }
    /// <summary>"quest.step" in the state <see cref="Is"/> (default done): pending, active, done.</summary>
    public string? Step { get; init; }
    /// <summary>At least <see cref="Count"/> of this item are in the inventory.</summary>
    public ItemRef? Item { get; init; }
    /// <summary>A C# function (<c>Quests.RegisterPredicate</c>).</summary>
    public string? Predicate { get; init; }
    /// <summary>The state a <see cref="Quest"/> or a <see cref="Step"/> has to be in. Default <c>done</c>. Only goes with those two.</summary>
    public string? Is { get; init; }
    /// <summary>With <see cref="Item"/>: how many. At least 1. Default 1.</summary>
    public int Count { get; init; } = 1;
    /// <summary>Holds when every condition in the list holds. The list is not empty.</summary>
    public IReadOnlyList<ConditionBlock>? All { get; init; }
    /// <summary>Holds when at least one condition in the list holds. The list is not empty.</summary>
    public IReadOnlyList<ConditionBlock>? Any { get; init; }
    /// <summary>Holds when this condition does not.</summary>
    public ConditionBlock? Not { get; init; }
}

// ---------------------------------------------------------------------------------------------------- actions -----

/// <summary>
/// An action: a quest's <see cref="QuestBlock.OnStart"/> and <see cref="QuestBlock.OnFail"/>, a step's <see cref="StepBlock.OnEnter"/> and <see cref="StepBlock.OnDone"/>, a
/// node's and an option's <see cref="OptionBlock.Do"/>. Exactly one verb: <see cref="Xp"/>, <see cref="Item"/>, <see cref="Take"/>, <see cref="Knowledge"/>,
/// <see cref="Revoke"/>, <see cref="Start"/>, <see cref="Complete"/>, <see cref="Fail"/>, <see cref="Advance"/>, <see cref="Reset"/>, <see cref="Spawn"/>,
/// <see cref="Despawn"/>, <see cref="Teleport"/>, <see cref="Chat"/>, <see cref="Talk"/>, <see cref="Signal"/> or <see cref="Call"/>. <c>Do.Start("lost-crates")</c>.
/// In an option's <c>Do</c> the first failing action stops the rest; in rewards and hooks each failure is logged and the rest runs.
/// </summary>
public sealed record ActionBlock
{
    /// <summary>Experience, as a quest reward.</summary>
    public uint? Xp { get; init; }
    /// <summary>Gives <see cref="Count"/> of it (what does not fit lands at the player's feet).</summary>
    public ItemRef? Item { get; init; }
    /// <summary>Takes <see cref="Count"/> of this item from the player.</summary>
    public ItemRef? Take { get; init; }
    /// <summary>Items given or taken, spawned entities.</summary>
    public int Count { get; init; } = 1;
    /// <summary>Grants the game's knowledge: <c>npc:….unlock</c>.</summary>
    public string? Knowledge { get; init; }
    /// <summary>Revokes it. That id only: what the game already did because of it stays.</summary>
    public string? Revoke { get; init; }
    /// <summary>For <see cref="Knowledge"/> and <see cref="Revoke"/>: "world" or "player".</summary>
    public string? Target { get; init; }
    /// <summary>For <see cref="Knowledge"/>: the counter amount or flag number.</summary>
    public uint? Value { get; init; }
    /// <summary>For <see cref="Revoke"/>: one flag only.</summary>
    public int? Flag { get; init; }
    /// <summary>Starts a quest.</summary>
    public string? Start { get; init; }
    /// <summary>Completes the quest of this id.</summary>
    public string? Complete { get; init; }
    /// <summary>Fails the quest of this id.</summary>
    public string? Fail { get; init; }
    /// <summary>Finishes a step: "quest.step". Refused for unfinished kill, collect and reach objectives unless <see cref="Force"/>.</summary>
    public string? Advance { get; init; }
    /// <summary>Forgets the holder's record of the quest of this id, as if never started. Knowledge it granted stays.</summary>
    public string? Reset { get; init; }
    /// <summary>For <see cref="Start"/> and <see cref="Advance"/>.</summary>
    public bool Force { get; init; }
    /// <summary>A one-off creature near the player or at <see cref="At"/>, on the ground outside bases. For creatures a step needs, use the step's spawn.</summary>
    public TemplateRef? Spawn { get; init; }
    /// <summary>Removes the creatures spawned with this Kiln.Spawn tag.</summary>
    public string? Despawn { get; init; }
    /// <summary>For <see cref="Spawn"/>: the place as [x, y, z] in metres. Without it, near the player.</summary>
    public double[]? At { get; init; }
    /// <summary>For <see cref="Spawn"/> and <see cref="Teleport"/>: metres added to the place, or to the player's position when there is no <see cref="At"/>.</summary>
    public double[]? Offset { get; init; }
    /// <summary>For <see cref="Spawn"/>: the creature's yaw. 0 when left out.</summary>
    public double? Yaw { get; init; }
    /// <summary>For <see cref="Spawn"/>: the Kiln.Spawn tag a kill objective can name. Default <c>quest:&lt;mod&gt;</c>.</summary>
    public string? Tag { get; init; }
    /// <summary>The spawned entity becomes this NPC.</summary>
    public string? BindNpc { get; init; }
    /// <summary>For <see cref="Spawn"/>: the creature is never written to the save.</summary>
    public bool Transient { get; init; }
    /// <summary>For <see cref="Spawn"/>: exactly at the place given. Otherwise Kiln puts it on real ground outside every base.</summary>
    public bool Exact { get; init; }
    /// <summary>Moves the player.</summary>
    public double[]? Teleport { get; init; }
    /// <summary>A chat line for the player.</summary>
    [JsonConverter(typeof(UiTextConverter))]
    public UiText? Chat { get; init; }
    /// <summary>Opens a chat conversation: a dialogue of this mod, at <see cref="Node"/>.</summary>
    public string? Talk { get; init; }
    /// <summary>For <see cref="Talk"/>: the node to open at.</summary>
    public string? Node { get; init; }
    /// <summary>Feeds <c>custom</c> objectives; <see cref="Amount"/> times.</summary>
    public string? Signal { get; init; }
    /// <summary>For <see cref="Signal"/>: how many to add. At least 1. Default 1.</summary>
    public int Amount { get; init; } = 1;
    /// <summary>A C# function (<c>Quests.RegisterAction</c>).</summary>
    public string? Call { get; init; }
}

// ---------------------------------------------------------------------------------------------------- rewards -----

/// <summary>
/// What a quest or a step pays, all at once: experience, items, a status, knowledge, a loot table. <c>Rewards = new RewardBlock { Xp = 1500, Items = { ["Weapon_T1_1H_SwordEpic_01"] = 1 } }</c>.
/// Each part is paid on its own: one that cannot be (a full inventory drops the rest at the player's feet) is logged and the others still are. A world quest pays every
/// player in game.
/// </summary>
public sealed record RewardBlock
{
    /// <summary>Experience, through the game's quest path.</summary>
    public uint? Xp { get; init; }
    /// <summary>Item and count: a game item, or an item this mod defines.</summary>
    public IDictionary<ItemRef, int> Items { get; init; } = new Dictionary<ItemRef, int>();
    /// <summary>A status put on the player: a <see cref="StatusBlock"/> of this mod, or a game status; the status's own time, or <see cref="StatusApply.Seconds"/>.</summary>
    public StatusApply? Status { get; init; }
    /// <summary>The game's knowledge granted: <c>npc:….unlock</c> unlocks an NPC.</summary>
    public IReadOnlyList<string>? Knowledge { get; init; }
    /// <summary>
    /// A loot table, rolled when the reward is paid: the name of a <see cref="LootTableBlock"/> (<c>lootTables</c>) of this mod, or <c>"other-mod:name"</c>. What comes
    /// up is given like <see cref="Items"/>. A name that is no loot table refuses the block.
    /// </summary>
    public string? LootTable { get; init; }
}

// -------------------------------------------------------------------------------------------------- dialogues -----

/// <summary>
/// A conversation in the game's own NPC menu: nodes of text and options, each with a condition and actions. An <see cref="NpcBlock"/> names it in
/// <see cref="NpcBlock.TalkName"/> (its block name) or takes the handle in <see cref="NpcBlock.Talk"/>; so does the quests/*.json NPC <c>dialogue</c>. The menu is one
/// flat list of topics; docs/kiln/modding/quests.md "Dialogue" says how a graph maps to it. A conversation in chat (<c>native: false</c>) is quests/*.json's.
/// </summary>
[BlockClients(ClientsLevel.Mod)]
public sealed record DialogueBlock : IQuestBlock
{
    /// <summary>The first node.</summary>
    public string Start { get; init; } = "start";
    /// <summary>The nodes by name: text and options. At least one; <see cref="Start"/> names the first.</summary>
    public IDictionary<string, DialogueNodeBlock> Nodes { get; init; } = new Dictionary<string, DialogueNodeBlock>(StringComparer.Ordinal);
    /// <summary>The start node's text is a greeting. Default: yes on a clone, no on a game NPC, whose own greetings stay.</summary>
    public bool? Greet { get; init; }
    /// <summary>The marker over the NPC's head on the greeting: Hint, Attention, MainQuest.</summary>
    public string? Hint { get; init; }
    /// <summary>More greetings, shown before the plain one while their condition holds.</summary>
    public IReadOnlyList<GreetingBlock>? Greetings { get; init; }
    /// <summary>Replace the NPC's farewell lines.</summary>
    [JsonConverter(typeof(UiTextListConverter))]
    public IReadOnlyList<UiText>? Farewell { get; init; }
    /// <summary>Replace the line between topics.</summary>
    [JsonConverter(typeof(UiTextListConverter))]
    public IReadOnlyList<UiText>? Transition { get; init; }
}

/// <summary>A greeting that wins over the plain one while its condition holds.</summary>
public sealed record GreetingBlock
{
    /// <summary>The greeting's text. Required.</summary>
    [JsonConverter(typeof(UiTextConverter))]
    public required UiText Text { get; init; }
    /// <summary>The greeting is used only while this holds.</summary>
    public ConditionBlock? If { get; init; }
    /// <summary>A voice-over line for the greeting: a name of the mod's cooked audio, or a GUID.</summary>
    public string? Voice { get; init; }
    /// <summary>The marker over the NPC's head on this greeting: Hint, Attention or MainQuest.</summary>
    public string? Hint { get; init; }
}

/// <summary>A node of a dialogue: what is said, the options that follow, and what happens when it opens.</summary>
public sealed record DialogueNodeBlock
{
    /// <summary>Overrides the NPC's name for this node.</summary>
    [JsonConverter(typeof(UiTextConverter))]
    public UiText? Speaker { get; init; }
    /// <summary>What the NPC says at this node.</summary>
    [JsonConverter(typeof(UiTextConverter))]
    public UiText? Text { get; init; }
    /// <summary>A voice-over line the game's dialogue plays: a name of the mod's cooked audio, or a GUID.</summary>
    public string? Voice { get; init; }
    /// <summary>The marker over the NPC's head on this node's greeting: Hint, Attention or MainQuest.</summary>
    public string? Hint { get; init; }
    /// <summary>Add new options at the end: positions and node names feed the lines' ids.</summary>
    public IReadOnlyList<OptionBlock>? Options { get; init; }
    /// <summary>A node without options: where "Continue" goes.</summary>
    public string? Goto { get; init; }
    /// <summary>Run when the node opens.</summary>
    public IReadOnlyList<ActionBlock>? Do { get; init; }
}

/// <summary>An option of a node: a topic in the game's NPC menu, with its condition, its actions and where it leads.</summary>
public sealed record OptionBlock
{
    /// <summary>The topic's text. Required.</summary>
    [JsonConverter(typeof(UiTextConverter))]
    public required UiText Text { get; init; }
    /// <summary>Shown only while this holds; checked again when chosen.</summary>
    public ConditionBlock? If { get; init; }
    /// <summary>Run when it is chosen. The first failing action stops the rest.</summary>
    public IReadOnlyList<ActionBlock>? Do { get; init; }
    /// <summary>The next node: its text is the reply, and its options join the NPC's topics from then on. "end" or none: nothing more.</summary>
    public string? Goto { get; init; }
    /// <summary>What the NPC answers. Default the target node's text.</summary>
    [JsonConverter(typeof(UiTextConverter))]
    public UiText? Reply { get; init; }
    /// <summary>The voice-over line of the reply. Default the target node's.</summary>
    public string? Voice { get; init; }
    /// <summary>The topic goes away once heard.</summary>
    public bool Once { get; init; }
    /// <summary>The NPC plays its outro after the reply.</summary>
    public bool Close { get; init; }
    /// <summary>The topic's marker: Hint, Attention or MainQuest.</summary>
    public string? Hint { get; init; }
}

// -------------------------------------------------------------------------------------------------- helpers -----

/// <summary>Objectives, the short way: <c>Goal.Kill(tag: "crates", count: 6)</c>, <c>Goal.Deliver("Material_T1_Wood_Logs", 3, to: "ilse")</c>.</summary>
public static class Goal
{
    /// <summary>A kill objective of a template, a family or a tag: <c>Goal.Kill(tag: "crates", count: 6)</c>.</summary>
    public static ObjectiveBlock Kill(string? template = null, string? family = null, string? tag = null, int count = 1) =>
        new() { Kill = new KillGoal { Template = template, Family = family, Tag = tag, Count = count } };
    /// <summary>Hold <paramref name="count"/> of <paramref name="item"/>.</summary>
    public static ObjectiveBlock Collect(ItemRef item, int count = 1) => new() { Collect = new CollectGoal { Item = item, Count = count } };
    /// <summary>Hand <paramref name="count"/> of <paramref name="item"/> to the NPC <paramref name="to"/>.</summary>
    public static ObjectiveBlock Deliver(ItemRef item, int count, string to) => new() { Deliver = new DeliverGoal { Item = item, Count = count, To = to } };
    /// <summary>Get within <paramref name="radius"/> metres of the place (x, y, z). Default 10.</summary>
    public static ObjectiveBlock Reach(double x, double y, double z, double radius = 10) => new() { Reach = new ReachGoal { At = [x, y, z], Radius = radius } };
    /// <summary>Being at the step's <see cref="StepBlock.Site"/>.</summary>
    public static ObjectiveBlock ReachSite(double radius = 10) => new() { Reach = new ReachGoal { Site = true, Radius = radius } };
    /// <summary>Talk to the NPC <paramref name="npc"/>, optionally to hear one <paramref name="line"/> or to reach a conversation <paramref name="node"/>.</summary>
    public static ObjectiveBlock Talk(string npc, string? node = null, string? line = null) => new() { Talk = new TalkGoal { Npc = npc, Node = node, Line = line } };
    /// <summary>Craft <paramref name="count"/> of <paramref name="item"/>.</summary>
    public static ObjectiveBlock Craft(ItemRef item, int count = 1) => new() { Craft = new CraftGoal { Item = item, Count = count } };
    /// <summary>Place <paramref name="count"/> of the prop <paramref name="prop"/>.</summary>
    public static ObjectiveBlock Build(ItemRef prop, int count = 1) => new() { Build = new BuildGoal { Prop = prop, Count = count } };
    /// <summary>The game's knowledge holds <paramref name="knowledge"/> (<c>npc:…</c>, <c>quest:…</c>, <c>query:…</c>, <c>0x…</c>).</summary>
    public static ObjectiveBlock Knowing(string knowledge) => new() { Knowledge = knowledge };
    /// <summary><paramref name="count"/> signals of the name <paramref name="signal"/>.</summary>
    public static ObjectiveBlock Custom(string signal, int count = 1) => new() { Custom = new CustomGoal { Signal = signal, Count = count } };
}

/// <summary>Conditions, the short way: <c>Req.Level(8)</c>, <c>Req.Quest("lost-crates", "active")</c>.</summary>
public static class Req
{
    /// <summary>The character's level is at least <paramref name="level"/>.</summary>
    public static ConditionBlock Level(int level) => new() { Level = level };
    /// <summary>A quest is in a state: locked, available, active, done, failed or started. Default done.</summary>
    public static ConditionBlock Quest(string quest, string state = "done") => new() { Quest = quest, Is = state };
    /// <summary>A step (<c>"quest.step"</c>) is in a state: pending, active or done. Default done.</summary>
    public static ConditionBlock Step(string questDotStep, string state = "done") => new() { Step = questDotStep, Is = state };
    /// <summary>At least <paramref name="count"/> of <paramref name="item"/> are in the inventory.</summary>
    public static ConditionBlock Item(ItemRef item, int count = 1) => new() { Item = item, Count = count };
    /// <summary>The game's knowledge holds <paramref name="name"/>.</summary>
    public static ConditionBlock Knowledge(string name) => new() { Knowledge = name };
    /// <summary>A C# function registered with <c>Quests.RegisterPredicate</c> returns true.</summary>
    public static ConditionBlock Predicate(string name) => new() { Predicate = name };
    /// <summary>Every one of these holds.</summary>
    public static ConditionBlock All(params ConditionBlock[] all) => new() { All = all };
    /// <summary>At least one of these holds.</summary>
    public static ConditionBlock Any(params ConditionBlock[] any) => new() { Any = any };
    /// <summary>This does not hold.</summary>
    public static ConditionBlock Not(ConditionBlock not) => new() { Not = not };
}

/// <summary>Actions, the short way: <c>Do.Start("lost-crates")</c>, <c>Do.Advance("lost-crates.return")</c>, <c>Do.Give("Material_T1_Twigs", 5)</c>.</summary>
public static class Do
{
    /// <summary>Pays <paramref name="amount"/> experience.</summary>
    public static ActionBlock Xp(uint amount) => new() { Xp = amount };
    /// <summary>Gives <paramref name="count"/> of <paramref name="item"/>. What does not fit lands at the player's feet.</summary>
    public static ActionBlock Give(ItemRef item, int count = 1) => new() { Item = item, Count = count };
    /// <summary>Takes <paramref name="count"/> of <paramref name="item"/>.</summary>
    public static ActionBlock Take(ItemRef item, int count = 1) => new() { Take = item, Count = count };
    /// <summary>Grants the game's knowledge <paramref name="name"/>. <paramref name="target"/> is "world" or "player"; left out, the player's object if the id has one, else the world's.</summary>
    public static ActionBlock Knowledge(string name, string? target = null) => new() { Knowledge = name, Target = target };
    /// <summary>Revokes the game's knowledge <paramref name="name"/>, from the "world" or the "player" <paramref name="target"/>. That id only.</summary>
    public static ActionBlock Revoke(string name, string? target = null) => new() { Revoke = name, Target = target };
    /// <summary>Starts a quest. Refused when it cannot start, unless <paramref name="force"/>.</summary>
    public static ActionBlock Start(string quest, bool force = false) => new() { Start = quest, Force = force };
    /// <summary>Completes a quest.</summary>
    public static ActionBlock Complete(string quest) => new() { Complete = quest };
    /// <summary>Fails a quest.</summary>
    public static ActionBlock Fail(string quest) => new() { Fail = quest };
    /// <summary>Finishes a step (<c>"quest.step"</c>). Refused for unfinished kill, collect and reach objectives unless <paramref name="force"/>.</summary>
    public static ActionBlock Advance(string questDotStep, bool force = false) => new() { Advance = questDotStep, Force = force };
    /// <summary>Forgets the holder's record of a quest, as if never started. Knowledge it granted stays.</summary>
    public static ActionBlock Reset(string quest) => new() { Reset = quest };
    /// <summary>Feeds <c>custom</c> objectives with the signal <paramref name="name"/>, <paramref name="amount"/> times.</summary>
    public static ActionBlock Signal(string name, int amount = 1) => new() { Signal = name, Amount = amount };
    /// <summary>A chat line for the player.</summary>
    public static ActionBlock Chat(UiText text) => new() { Chat = text };
    /// <summary>Opens a chat conversation with the dialogue <paramref name="dialogue"/>, at <paramref name="node"/>.</summary>
    public static ActionBlock Talk(string dialogue, string? node = null) => new() { Talk = dialogue, Node = node };
    /// <summary>Calls a C# function registered with <c>Quests.RegisterAction</c>.</summary>
    public static ActionBlock Call(string name) => new() { Call = name };
    /// <summary>Moves the player to the place (x, y, z).</summary>
    public static ActionBlock Teleport(double x, double y, double z) => new() { Teleport = [x, y, z] };
}

/// <summary>What <see cref="Blocks.Define(string, QuestBlock)"/> gives back for a quest: the definition the engine runs.</summary>
public sealed class QuestHandle : BlockHandle<QuestBlock>
{
    internal QuestHandle(string mod, string name, Guid guid, string gameName, QuestBlock block, QuestDefinition definition) : base(mod, name, guid, gameName, block) => Definition = definition;

    /// <summary>What it became: <see cref="Quests.Define"/> took this object. Its <see cref="QuestDefinition.FullId"/> is what <c>Quests.Start</c> takes.</summary>
    public QuestDefinition Definition { get; }
    /// <summary><c>&lt;mod&gt;:&lt;name&gt;</c>, for <c>Quests.Start</c>, <c>Quests.StatusOf</c> and the rest of <see cref="Quests"/>.</summary>
    public string FullId => $"{Mod}:{Name}";
}

/// <summary>What <see cref="Blocks.Define(string, DialogueBlock)"/> gives back for a dialogue. It converts to the <see cref="DialogueDefinition"/> the engine runs,
/// so it goes where one does: <c>new NpcBlock { Talk = handle }</c>.</summary>
public sealed class DialogueHandle : BlockHandle<DialogueBlock>
{
    internal DialogueHandle(string mod, string name, Guid guid, string gameName, DialogueBlock block, DialogueDefinition definition) : base(mod, name, guid, gameName, block) => Definition = definition;

    /// <summary>The definition the engine runs: <see cref="Dialogues.Define"/> took this object.</summary>
    public DialogueDefinition Definition { get; }

    /// <summary>A handle goes where a <see cref="DialogueDefinition"/> does: it converts to its <see cref="Definition"/>.</summary>
    public static implicit operator DialogueDefinition(DialogueHandle handle) => handle.Definition;
}
