// SPDX-License-Identifier: MIT
// Kiln.Quests engine: what a quest chain is. The JSON loader (Json.cs) and C# mods (Quests.Define) both produce these
// objects; the engine (Engine.cs) only reads them. Usage: docs/kiln/services/quests.md, docs/kiln/modding/quests.md.
namespace Kiln;

/// <summary>Where a quest stands for one holder.</summary>
public enum QuestStatus
{
    /// <summary>Its <c>requires</c> does not hold yet.</summary>
    Locked = 0,
    /// <summary>It can be started.</summary>
    Available = 1,
    /// <summary>It was started and is running; the holder is on a step (see <see cref="QuestProgress.StepId"/>).</summary>
    Active = 2,
    /// <summary>It was completed (the rewards are paid or owed).</summary>
    Done = 3,
    /// <summary>It failed (<see cref="Quests.Fail"/> or a <c>fail</c> action) or was given up.</summary>
    Failed = 4,
}

/// <summary>Who owns a quest's progress.</summary>
public enum QuestScope
{
    /// <summary>Each player on their own (the default).</summary>
    Player = 0,
    /// <summary>Each player has their own state, but players within <see cref="QuestDefinition.PartyRadius"/> metres
    /// share kills and other objective credit, and each gets the rewards.</summary>
    Party = 1,
    /// <summary>One shared state for the world.</summary>
    World = 2,
}

/// <summary>Where a quest is shown (JSON <c>journal</c>).</summary>
public enum QuestJournalMode
{
    /// <summary>Chat lines and <c>/quests</c>, for a mod that vanilla clients join (<c>clients: "none"</c>): the game's UI cannot
    /// reach them, because a client builds it from the mod's files.</summary>
    Chat = 0,
    /// <summary>The default: the game's own journal, tracked-quest HUD, notifications and map markers, built at boot from the
    /// mod's files on the server and on every client (<c>clients: "mod"</c>). The engine runs the quest.</summary>
    Native = 2,
}

/// <summary>A quest chain: steps that run in order, each with one or more objectives.</summary>
public sealed class QuestDefinition
{
    /// <summary>Letters, digits, '_' and '-'. Unique within the mod.</summary>
    public required string Id { get; init; }
    /// <summary>The quest's name in the journal.</summary>
    public UiText Name { get; set; } = "";
    /// <summary>A longer description of the quest. Null for none.</summary>
    public UiText? Description { get; set; }
    /// <summary>The NPC who offers it (an id of the same mod's <c>npcs</c>, or <c>mod:npc</c>). A quest with a giver can only be
    /// started through that NPC, or by an action or code.</summary>
    public string? Giver { get; set; }
    /// <summary>Who owns the quest's progress (default <see cref="QuestScope.Player"/>).</summary>
    public QuestScope Scope { get; set; } = QuestScope.Player;
    /// <summary>For a <see cref="QuestScope.Party"/> quest: the radius in metres within which players share credit
    /// (default 60).</summary>
    public double PartyRadius { get; set; } = 60;
    /// <summary>Marks it as a main quest (JSON <c>main</c>): the game's journal entry gets its main-quest flag.</summary>
    public bool Main { get; set; }
    /// <summary>Starts by itself as soon as it is available (a world quest when the world runs, a player quest while
    /// the player is in game).</summary>
    public bool Auto { get; set; }
    /// <summary>Can be started again after it is done.</summary>
    public bool Repeatable { get; set; }
    /// <summary>Can be started again after it failed or was abandoned (default true).</summary>
    public bool Retryable { get; set; } = true;
    /// <summary>Players may give it up (never a world quest; default true).</summary>
    public bool Abandonable { get; set; } = true;
    /// <summary>Not listed until it is active.</summary>
    public bool Hidden { get; set; }
    /// <summary>Where the quest is shown: the game's own journal (the default) or chat lines.</summary>
    public QuestJournalMode Journal { get; set; } = QuestJournalMode.Native;
    /// <summary>The game's journal: the portrait (keen::QuestSourceType: None, Flame, Blacksmith, Alchemist, ...).</summary>
    public string? Source { get; set; }
    /// <summary>The game's journal: its place in the list (the game uses 40..100; default 50).</summary>
    public int? Priority { get; set; }
    /// <summary>Conditions that must all hold before the quest can start. Empty: always available.</summary>
    public List<QuestCondition> Requires { get; set; } = new();
    /// <summary>The steps, in order. The quest needs at least one.</summary>
    public List<QuestStep> Steps { get; set; } = new();
    /// <summary>Actions that run when the quest is completed (items, experience, knowledge...). A failing one is logged
    /// and the rest still run.</summary>
    public List<QuestAction> Rewards { get; set; } = new();
    /// <summary>Actions that run when the quest starts.</summary>
    public List<QuestAction> OnStart { get; set; } = new();
    /// <summary>Actions that run when the quest fails.</summary>
    public List<QuestAction> OnFail { get; set; } = new();

    /// <summary>The mod that defined it ("" for code outside a mod).</summary>
    public string ModId { get; internal set; } = "";
    /// <summary><c>&lt;modId&gt;:&lt;id&gt;</c>.</summary>
    public string FullId => ModId.Length > 0 ? $"{ModId}:{Id}" : Id;
    /// <summary>The step with this id (case-sensitive); null when there is none.</summary>
    public QuestStep? Step(string id) => Steps.Find(s => s.Id.Equals(id, StringComparison.Ordinal));
    /// <summary>The full id, the scope and the number of steps.</summary>
    public override string ToString() => $"{FullId} ({Scope}, {Steps.Count} steps)";
}

/// <summary>One step of a quest. It is done when all its objectives are.</summary>
public sealed class QuestStep
{
    /// <summary>The step's id. Unique within its quest.</summary>
    public required string Id { get; init; }
    /// <summary>The step's text: the paragraph in the journal.</summary>
    public UiText Text { get; set; } = "";
    /// <summary>The game's journal: the step's short name (default its text).</summary>
    public UiText? Title { get; set; }
    /// <summary>The game's journal: the step's voice-over line (a name in the mod's cooked audio, or a GUID).</summary>
    public string? Voice { get; set; }
    /// <summary>The game's journal: the recommended level shown with the step.</summary>
    public int RecommendedLevel { get; set; }
    /// <summary>What to do in this step. The step is done when all its objectives are.</summary>
    public List<QuestObjective> Objectives { get; set; } = new();
    /// <summary>The map marker and waypoint of the step. Null: one is made anyway when the step has a place (a site, a spawn, a
    /// reach or kill place, an NPC to talk to or deliver to). See <see cref="NoMarker"/>.</summary>
    public QuestMarker? Marker { get; set; }
    /// <summary>This step shows no marker, even though it has a place (JSON <c>"marker": false</c>).</summary>
    public bool NoMarker { get; set; }
    /// <summary>Where the step happens, found at run time on real ground outside every base, once per world.</summary>
    public QuestPlace? Site { get; set; }
    /// <summary>Creatures the step brings into the world. Kiln verifies they exist, and spawns them again when the game removed them.</summary>
    public List<QuestSpawn> Spawns { get; set; } = new();
    /// <summary>Actions that run when the step becomes the current one.</summary>
    public List<QuestAction> OnEnter { get; set; } = new();
    /// <summary>Actions that run when the step is done.</summary>
    public List<QuestAction> OnDone { get; set; } = new();
}

/// <summary>A step's map marker.</summary>
/// <param name="At">A fixed place (null: the step's other place decides).</param>
/// <param name="Radius">The marker's radius in metres; 0 when none is given.</param>
/// <param name="Waypoint">Also write the player's waypoint, the game's own map and compass pin (default true).</param>
/// <param name="Name">The marker's name on the map (default the step's text).</param>
/// <param name="Base">The marker template to copy.</param>
public sealed record QuestMarker(WorldPosition? At = null, double Radius = 0, bool Waypoint = true, UiText? Name = null, string? Base = null);

/// <summary>A place found at run time: on real ground (the voxel surface), outside every player base, not too steep. Kiln keeps
/// it per world, so everyone doing that step goes to one place.</summary>
public sealed class QuestPlace
{
    /// <summary>Search around this place.</summary>
    public WorldPosition? At { get; set; }
    /// <summary>Search around this NPC of the mod's <c>npcs</c>.</summary>
    public string? NearNpc { get; set; }
    /// <summary>Search around the player who gets there first (the default when nothing else is given).</summary>
    public bool NearPlayer { get; set; }
    /// <summary>Metres from the centre of the search. It widens by itself until a good spot is found, so a base in the way never fails it.</summary>
    public double MinDistance { get; set; }
    /// <summary>Metres from the centre of the search, at most.</summary>
    public double MaxDistance { get; set; }
    /// <summary>Metres to keep from a base's build zone (default 20).</summary>
    public double BaseMargin { get; set; } = 20;
    /// <summary>A base is fine (a friendly NPC that belongs in one). Enemies never take this: the game pacifies them in bases and may remove them.</summary>
    public bool InBase { get; set; }
    /// <summary>Exactly <see cref="At"/>: no ground, no base check.</summary>
    public bool Exact { get; set; }
}

/// <summary>Creatures a step brings into the world.</summary>
public sealed class QuestSpawn
{
    /// <summary>The template name of the creature to spawn.</summary>
    public required string Template { get; init; }
    /// <summary>How many to spawn (default 1).</summary>
    public int Count { get; set; } = 1;
    /// <summary>Default the step's <see cref="QuestStep.Site"/>; without one, near the player at 30..60 m.</summary>
    public QuestPlace? Place { get; set; }
    /// <summary>The Kiln.Spawn tag: a kill objective can name it.</summary>
    public string? Tag { get; set; }
    /// <summary>Metres between them.</summary>
    public double Spread { get; set; } = 4;
    /// <summary>The game's level to spawn at (0: the template's).</summary>
    public int Level { get; set; }
    /// <summary>How many times Kiln spawns them again when the game removes them before they die (default 5).</summary>
    public int Retries { get; set; } = 5;
}

/// <summary>What an objective asks for. Which fields of <see cref="QuestObjective"/> count depends on it.</summary>
public enum QuestObjectiveKind
{
    /// <summary>Kill creatures, by template, family or <c>Kiln.Spawn</c> tag.</summary>
    Kill,
    /// <summary>Hold items in the inventory.</summary>
    Collect,
    /// <summary>Hand items over to an NPC.</summary>
    Deliver,
    /// <summary>Be at a place.</summary>
    Reach,
    /// <summary>Talk to an NPC, say a game dialogue line, or reach a node of a chat conversation.</summary>
    Talk,
    /// <summary>Craft an item or recipe.</summary>
    Craft,
    /// <summary>Place a build piece.</summary>
    Build,
    /// <summary>Have a game knowledge name set.</summary>
    Knowledge,
    /// <summary>A counter that code or a <c>signal</c> action raises (<see cref="Quests.Signal"/>), or a C#
    /// <see cref="QuestObjective.Progress"/> function.</summary>
    Custom,
}

/// <summary>One thing to do in a step. Which fields count depends on <see cref="Kind"/>; the factories set them.</summary>
public sealed class QuestObjective
{
    /// <summary>What this objective asks for.</summary>
    public required QuestObjectiveKind Kind { get; init; }
    /// <summary>How many (kills, items, crafts, signals). Reach, talk, knowledge: 1.</summary>
    public int Count { get; set; } = 1;
    /// <summary>Shown instead of the step's text when a step has several objectives.</summary>
    public UiText? Text { get; set; }

    /// <summary>Kill: exact template name.</summary>
    public string? Template { get; set; }
    /// <summary>Kill: part of the template name ("Scavenger", "Vukah").</summary>
    public string? Family { get; set; }
    /// <summary>Kill: the Kiln.Spawn tag of the victim.</summary>
    public string? Tag { get; set; }
    /// <summary>Kill: only near this place (with <see cref="Radius"/>).</summary>
    public WorldPosition? Near { get; set; }
    /// <summary>Kill: Kiln.Spawn destroying a matching entity counts too (off: only deaths count).</summary>
    public bool CountDestroyed { get; set; }

    /// <summary>Collect, deliver, craft, build: item name or id.</summary>
    public string? Item { get; set; }
    /// <summary>Craft: recipe id ("0x…").</summary>
    public string? Recipe { get; set; }
    /// <summary>Deliver: the NPC who receives the items.</summary>
    public string? To { get; set; }

    /// <summary>Reach: the place.</summary>
    public WorldPosition? At { get; set; }
    /// <summary>Reach: an offset in metres from where the player stood when the step started, instead of <see cref="At"/>.</summary>
    public WorldPosition? Offset { get; set; }
    /// <summary>Reach: a template name; the nearest entity of that template is the place.</summary>
    public string? Marker { get; set; }
    /// <summary>Reach: the step's <see cref="QuestStep.Site"/> is the place.</summary>
    public bool Site { get; set; }
    /// <summary>Reach, kill: metres.</summary>
    public double Radius { get; set; } = 10;

    /// <summary>Talk: the NPC (an id of <c>npcs</c>, a template name, or "npc:&lt;debugName&gt;").</summary>
    public string? Npc { get; set; }
    /// <summary>Talk: a game dialogue line ("line:…", "0x…"), or a line of Kiln.Quests/Native.</summary>
    public string? Line { get; set; }
    /// <summary>Talk: a node of a Kiln conversation that must be reached.</summary>
    public string? Node { get; set; }

    /// <summary>Knowledge: the name that must hold ("npc:…", "quest:…", "query:…", "0x…").</summary>
    public string? Knowledge { get; set; }

    /// <summary>Custom: <see cref="Quests.Signal"/> with this name adds to the counter.</summary>
    public string? Signal { get; set; }
    /// <summary>Custom, C#: polled about once a second, returns the current count.</summary>
    public Func<QuestContext, int>? Progress { get; set; }

    /// <summary>A kill objective: <paramref name="count"/> kills, matched by exact <paramref name="template"/>, part of
    /// the template name (<paramref name="family"/>) or a <c>Kiln.Spawn</c> <paramref name="tag"/>.</summary>
    public static QuestObjective Kill(string? template = null, string? family = null, string? tag = null, int count = 1) =>
        new() { Kind = QuestObjectiveKind.Kill, Template = template, Family = family, Tag = tag, Count = count };
    /// <summary>A collect objective: hold <paramref name="count"/> of <paramref name="item"/> (name or id).</summary>
    public static QuestObjective Collect(string item, int count = 1) => new() { Kind = QuestObjectiveKind.Collect, Item = item, Count = count };
    /// <summary>A deliver objective: hand <paramref name="count"/> of <paramref name="item"/> to the NPC
    /// <paramref name="to"/>.</summary>
    public static QuestObjective Deliver(string item, int count, string to) =>
        new() { Kind = QuestObjectiveKind.Deliver, Item = item, Count = count, To = to };
    /// <summary>A reach objective: come within <paramref name="radius"/> metres of <paramref name="at"/>.</summary>
    public static QuestObjective Reach(WorldPosition at, double radius = 10) => new() { Kind = QuestObjectiveKind.Reach, At = at, Radius = radius };
    /// <summary>A talk objective: talk to <paramref name="npc"/>, optionally reaching a conversation
    /// <paramref name="node"/> or saying a dialogue <paramref name="line"/>.</summary>
    public static QuestObjective Talk(string npc, string? node = null, string? line = null) =>
        new() { Kind = QuestObjectiveKind.Talk, Npc = npc, Node = node, Line = line };
    /// <summary>A custom objective: a counter that <see cref="Quests.Signal"/> calls with <paramref name="signal"/> raise
    /// to <paramref name="count"/>.</summary>
    public static QuestObjective Custom(string signal, int count = 1) => new() { Kind = QuestObjectiveKind.Custom, Signal = signal, Count = count };
    /// <summary>A knowledge objective: done while the game's knowledge <paramref name="name"/> holds.</summary>
    public static QuestObjective HasKnowledge(string name) => new() { Kind = QuestObjectiveKind.Knowledge, Knowledge = name };

    internal bool IsLevel => Kind is QuestObjectiveKind.Collect or QuestObjectiveKind.Reach or QuestObjectiveKind.Knowledge || Progress is not null;
}

/// <summary>What a <see cref="QuestCondition"/> tests.</summary>
public enum QuestConditionKind
{
    /// <summary>A game knowledge name holds.</summary>
    Knowledge,
    /// <summary>The holder's level is at least a number.</summary>
    Level,
    /// <summary>Another quest is in a given state.</summary>
    Quest,
    /// <summary>A step of a quest is in a given state.</summary>
    Step,
    /// <summary>The holder's inventory has at least a number of an item.</summary>
    Item,
    /// <summary>Every child condition holds.</summary>
    All,
    /// <summary>At least one child condition holds.</summary>
    Any,
    /// <summary>Not all child conditions hold (the negation of <see cref="All"/>).</summary>
    Not,
    /// <summary>A C# function (or one registered by name with <c>Quests.RegisterPredicate</c>) decides.</summary>
    Predicate,
}

/// <summary>A condition on a holder, checked on the server: a quest's <c>requires</c>, an option's <c>if</c>.</summary>
public sealed class QuestCondition
{
    /// <summary>What this condition tests.</summary>
    public required QuestConditionKind Kind { get; init; }
    /// <summary>Knowledge name, quest id, "quest.step", item, or predicate name.</summary>
    public string? Name { get; set; }
    /// <summary>Quest: locked, available, active, done, failed. Step: pending, active, done.</summary>
    public string? Is { get; set; }
    /// <summary>Level: at least this level. Item: at least this many.</summary>
    public int Count { get; set; } = 1;
    /// <summary>All, Any, Not: the conditions it combines.</summary>
    public List<QuestCondition> Children { get; set; } = new();
    /// <summary>Predicate, C#: the function that decides. Null with a <see cref="Name"/>: the predicate registered under
    /// that name.</summary>
    public Func<QuestContext, bool>? Predicate { get; set; }

    /// <summary>Holds while the game's knowledge <paramref name="name"/> holds.</summary>
    public static QuestCondition HasKnowledge(string name) => new() { Kind = QuestConditionKind.Knowledge, Name = name };
    /// <summary>Holds when the holder is at least <paramref name="level"/>.</summary>
    public static QuestCondition Level(int level) => new() { Kind = QuestConditionKind.Level, Count = level };
    /// <summary>Holds when quest <paramref name="quest"/> is in <paramref name="state"/> (locked, available, active,
    /// done, failed or started).</summary>
    public static QuestCondition Quest(string quest, string state) => new() { Kind = QuestConditionKind.Quest, Name = quest, Is = state };
    /// <summary>Holds when step <paramref name="questDotStep"/> (written <c>quest.step</c>) is in
    /// <paramref name="state"/> (pending, active or done).</summary>
    public static QuestCondition QuestStep(string questDotStep, string state) => new() { Kind = QuestConditionKind.Step, Name = questDotStep, Is = state };
    /// <summary>Holds when the holder's inventory has at least <paramref name="count"/> of <paramref name="item"/>.</summary>
    public static QuestCondition HasItem(string item, int count = 1) => new() { Kind = QuestConditionKind.Item, Name = item, Count = count };
    /// <summary>Holds when <paramref name="predicate"/> returns true.</summary>
    public static QuestCondition When(Func<QuestContext, bool> predicate) => new() { Kind = QuestConditionKind.Predicate, Predicate = predicate };
}

/// <summary>What a <see cref="QuestAction"/> does. Which fields of the action count depends on it.</summary>
public enum QuestActionKind
{
    /// <summary>Give items to the holder (what does not fit lands at the player's feet).</summary>
    Give,
    /// <summary>Take items from the holder.</summary>
    Take,
    /// <summary>Give experience (as a quest reward, through kiln.progression).</summary>
    Xp,
    /// <summary>Set a game knowledge name (a flag, or a counter amount).</summary>
    Knowledge,
    /// <summary>Clear a game knowledge id. Only that id is cleared: what the game did because of it stays.</summary>
    Revoke,
    /// <summary>Start another quest.</summary>
    Start,
    /// <summary>Complete another quest.</summary>
    Complete,
    /// <summary>Fail another quest.</summary>
    Fail,
    /// <summary>Advance a quest past a step.</summary>
    Advance,
    /// <summary>Reset another quest: forget the holder's record of it, as if it was never started.</summary>
    Reset,
    /// <summary>Spawn a creature or NPC in the world.</summary>
    Spawn,
    /// <summary>Remove the spawned entities that carry a tag.</summary>
    Despawn,
    /// <summary>Move the player to a place.</summary>
    Teleport,
    /// <summary>Send the player a chat line.</summary>
    Chat,
    /// <summary>The same as <see cref="Chat"/> today: the game has no free-text notification.</summary>
    Toast,
    /// <summary>Set or clear the player's waypoint pin on the map and compass.</summary>
    Waypoint,
    /// <summary>Open a chat conversation.</summary>
    Talk,
    /// <summary>Feed <see cref="QuestObjectiveKind.Custom"/> objectives with a signal.</summary>
    Signal,
    /// <summary>Run C# code (<see cref="QuestAction.Run"/>, or a function registered by name).</summary>
    Call,
}

/// <summary>A reward or side effect (a quest's rewards and hooks, a conversation option's <c>do</c>).</summary>
public sealed class QuestAction
{
    /// <summary>What this action does.</summary>
    public required QuestActionKind Kind { get; init; }
    /// <summary>Item, knowledge name, quest id, "quest.step", template, dialogue, signal or action name.</summary>
    public string? Name { get; set; }
    /// <summary>Items, spawned entities, or a signal's amount.</summary>
    public int Count { get; set; } = 1;
    /// <summary>Xp: the amount. Knowledge: the counter amount or flag number. Revoke: the flag number + 1 (0 = all).</summary>
    public uint Value { get; set; } = 1;
    /// <summary>Chat, toast: the line to send.</summary>
    public UiText? Text { get; set; }
    /// <summary>Spawn, teleport, waypoint: the place.</summary>
    public WorldPosition? At { get; set; }
    /// <summary>Spawn, teleport, waypoint: an offset in metres from the player, instead of <see cref="At"/>.</summary>
    public WorldPosition? Offset { get; set; }
    /// <summary>Spawn: the entity's facing in degrees about the up axis.</summary>
    public double Yaw { get; set; }
    /// <summary>Spawn: the Kiln.Spawn tag.</summary>
    public string? Tag { get; set; }
    /// <summary>Spawn: never written to the save.</summary>
    public bool Transient { get; set; }
    /// <summary>Spawn: the entity becomes this NPC of <c>npcs</c> (see <see cref="Quests.BindNpc"/>).</summary>
    public string? BindNpc { get; set; }
    /// <summary>Spawn: exactly at the place given. Otherwise Kiln puts it on real ground outside every base.</summary>
    public bool Exact { get; set; }
    /// <summary>Knowledge, revoke: "world" or "player"; default: the player's object if the id has one, else the world's.</summary>
    public string? Target { get; set; }
    /// <summary>Toast: info, success, warning, error.</summary>
    public string? ToastKind { get; set; }
    /// <summary>Talk: the node to open at.</summary>
    public string? Node { get; set; }
    /// <summary>Waypoint: true clears it.</summary>
    public bool Clear { get; set; }
    /// <summary>Force an advance past unfinished objectives.</summary>
    public bool Force { get; set; }
    /// <summary>Call, C#: the code to run.</summary>
    public Func<QuestActionContext, Task>? Run { get; set; }

    /// <summary>A give action: <paramref name="count"/> of <paramref name="item"/> to the holder.</summary>
    public static QuestAction Item(string item, int count = 1) => new() { Kind = QuestActionKind.Give, Name = item, Count = count };
    /// <summary>An experience action: <paramref name="amount"/> experience.</summary>
    public static QuestAction Experience(uint amount) => new() { Kind = QuestActionKind.Xp, Value = amount };
    /// <summary>A knowledge action: sets the game knowledge <paramref name="name"/>. <paramref name="target"/> is "world"
    /// or "player" (null: the player's object if the id has one, else the world's).</summary>
    public static QuestAction GrantKnowledge(string name, string? target = null) => new() { Kind = QuestActionKind.Knowledge, Name = name, Target = target };
    /// <summary>A call action: runs <paramref name="run"/> as the owning mod.</summary>
    public static QuestAction Code(Func<QuestActionContext, Task> run) => new() { Kind = QuestActionKind.Call, Run = run };
}

/// <summary>What a condition or a custom objective is asked about.</summary>
/// <param name="Holder">Whose state is asked about (a player, a character, or the world).</param>
/// <param name="Quest">The quest being checked; null outside a quest (for example a dialogue option).</param>
/// <param name="Npc">The NPC entity being talked to; <see cref="EntityId.None"/> when there is none.</param>
/// <param name="ModId">The id of the mod whose definition is asked.</param>
public sealed record QuestContext(QuestHolder Holder, QuestDefinition? Quest, EntityId Npc, string ModId)
{
    /// <summary>The holder's player in game; null when the holder has no entity or kiln.players is unavailable.</summary>
    public Player? Player => Holder.Entity.IsNone || !Players.Available ? null : Players.Find(Holder.Entity);
}

/// <summary>What an action runs for.</summary>
/// <param name="Holder">Whom the action runs for (a player, a character, or the world).</param>
/// <param name="Quest">The quest the action belongs to; null outside a quest (for example a dialogue option).</param>
/// <param name="Npc">The NPC entity involved; <see cref="EntityId.None"/> when there is none.</param>
/// <param name="ModId">The id of the mod that defined the action.</param>
public sealed record QuestActionContext(QuestHolder Holder, QuestDefinition? Quest, EntityId Npc, string ModId)
{
    /// <summary>The holder's player in game; null when the holder has no entity or kiln.players is unavailable.</summary>
    public Player? Player => Holder.Entity.IsNone || !Players.Available ? null : Players.Find(Holder.Entity);
    internal QuestContext Condition => new(Holder, Quest, Npc, ModId);
}

/// <summary>A conversation: nodes of text and options, per NPC or per quest. The server decides what is shown and checks
/// every choice again.</summary>
public sealed class DialogueDefinition
{
    /// <summary>The dialogue's id. Unique within its mod.</summary>
    public required string Id { get; init; }
    /// <summary>The id of the node the conversation opens at (default <c>start</c>).</summary>
    public string Start { get; set; } = "start";
    /// <summary>The conversation's nodes by id.</summary>
    public Dictionary<string, DialogueNode> Nodes { get; set; } = new(StringComparer.Ordinal);
    /// <summary>The default: its lines are in the game's own NPC menu, with the game's voice playback. False: a chat conversation
    /// through <c>/talk</c>, for a mod that vanilla clients join.</summary>
    public bool Native { get; set; } = true;
    /// <summary>The game's menu: the start node's text is a greeting. Default: yes on a clone, no on a game NPC, whose own greetings stay.</summary>
    public bool? Greet { get; set; }
    /// <summary>The game's menu: the marker over the NPC's head on the greeting (Hint, Attention, MainQuest).</summary>
    public string? Hint { get; set; }
    /// <summary>The game's menu: more greetings, shown before the plain one while their condition holds.</summary>
    public List<DialogueGreeting> Greetings { get; set; } = new();
    /// <summary>The game's menu: replaces the NPC's farewell lines.</summary>
    public List<UiText> Farewell { get; set; } = new();
    /// <summary>The game's menu: replaces the line between topics.</summary>
    public List<UiText> Transition { get; set; } = new();
    /// <summary>The mod that defined it ("" for code outside a mod).</summary>
    public string ModId { get; internal set; } = "";
    /// <summary><c>&lt;modId&gt;:&lt;id&gt;</c>, or just the id outside a mod.</summary>
    public string FullId => ModId.Length > 0 ? $"{ModId}:{Id}" : Id;
    /// <summary>The full id and the number of nodes.</summary>
    public override string ToString() => $"{FullId} ({Nodes.Count} nodes)";
}

/// <summary>A greeting that wins over the plain one while its condition holds.</summary>
public sealed class DialogueGreeting
{
    /// <summary>The greeting line.</summary>
    public UiText Text { get; set; } = "";
    /// <summary>Shown only while this holds. Null: always.</summary>
    public QuestCondition? If { get; set; }
    /// <summary>A voice-over line for it: a name in the mod's cooked audio, or a GUID.</summary>
    public string? Voice { get; set; }
    /// <summary>The marker over the NPC's head on this greeting (Hint, Attention, MainQuest).</summary>
    public string? Hint { get; set; }
}

/// <summary>One node of a conversation: some text from the NPC and the options the player can pick.</summary>
public sealed class DialogueNode
{
    /// <summary>Overrides the NPC's name for this node.</summary>
    public UiText? Speaker { get; set; }
    /// <summary>What the NPC says at this node.</summary>
    public UiText Text { get; set; } = "";
    /// <summary>A voice-over line, played by the game's dialogue: a name in the mod's cooked audio, or a GUID (any line of the game).</summary>
    public string? Voice { get; set; }
    /// <summary>The game's menu: the marker over the NPC's head on this node's greeting.</summary>
    public string? Hint { get; set; }
    /// <summary>The choices the player can pick at this node.</summary>
    public List<DialogueOption> Options { get; set; } = new();
    /// <summary>A node without options: where "Continue" goes (default: the conversation ends).</summary>
    public string? Goto { get; set; }
    /// <summary>Actions when the node opens.</summary>
    public List<QuestAction> Do { get; set; } = new();
}

/// <summary>One choice the player can pick at a node.</summary>
public sealed class DialogueOption
{
    /// <summary>The option's line.</summary>
    public UiText Text { get; set; } = "";
    /// <summary>Shown only while this holds; checked again when chosen.</summary>
    public QuestCondition? If { get; set; }
    /// <summary>Actions that run when the option is chosen. The first one that fails stops the rest, and the player gets
    /// the reason in chat.</summary>
    public List<QuestAction> Do { get; set; } = new();
    /// <summary>The next node; "end" or null ends the conversation.</summary>
    public string? Goto { get; set; }
    /// <summary>The game's menu: what the NPC answers. Default the target node's text, else the option's own.</summary>
    public UiText? Reply { get; set; }
    /// <summary>The game's menu: the reply's voice-over. Default the target node's.</summary>
    public string? Voice { get; set; }
    /// <summary>The game's menu: the topic goes away once heard.</summary>
    public bool Once { get; set; }
    /// <summary>The game's menu: the NPC plays its outro after the reply.</summary>
    public bool Close { get; set; }
    /// <summary>The game's menu: the topic's marker (Hint, Attention, MainQuest).</summary>
    public string? Hint { get; set; }
}

/// <summary>An NPC a quest or conversation refers to: a game NPC, or a new one (<see cref="Clone"/>), found by its template or
/// placed in the world (<see cref="Place"/>).</summary>
public sealed class QuestNpcDefinition
{
    /// <summary>The NPC's id. Unique within its mod.</summary>
    public required string Id { get; init; }
    /// <summary>Template name of the entity (of a clone: the base).</summary>
    public string? Template { get; set; }
    /// <summary>A new NPC: a copy of <see cref="Template"/> under its own GUID, one per world. Needs <c>clients: "mod"</c>.</summary>
    public bool Clone { get; set; }
    /// <summary>The clone's GUID. Keep it forever; null derives it from the mod and this id.</summary>
    public Guid? CloneGuid { get; set; }
    /// <summary>The clone's template name; null derives it.</summary>
    public string? CloneName { get; set; }
    /// <summary>Where Kiln puts it in the world, once it can: on real ground, outside every base unless <see cref="QuestPlace.InBase"/>.</summary>
    public QuestPlace? Place { get; set; }
    /// <summary>The conversation it opens.</summary>
    public string? Dialogue { get; set; }
    /// <summary>The NPC's display name. Null: the template's own.</summary>
    public UiText? Name { get; set; }
    /// <summary>Where it stands (the JSON's <c>place.at</c>): with no entity found, this spot still counts as the NPC.</summary>
    public WorldPosition? At { get; set; }
    /// <summary>How close a player must be to talk (metres).</summary>
    public double TalkRange { get; set; } = 8;
    /// <summary>The mod that defined it ("" for code outside a mod).</summary>
    public string ModId { get; internal set; } = "";
    /// <summary><c>&lt;modId&gt;:&lt;id&gt;</c>, or just the id outside a mod.</summary>
    public string FullId => ModId.Length > 0 ? $"{ModId}:{Id}" : Id;
}

/// <summary>Who owns a quest's state: a character on this world (the game's own key: account + CharacterId), or the world.</summary>
/// <param name="Key">"world", "character:&lt;account&gt;.&lt;id&gt;", "player:&lt;account&gt;" (a synthetic holder, an offline
/// account, or a build without character ids) or "session:&lt;entity&gt;".</param>
/// <param name="Account">The Steam or account id (where the data was filed before characters); 0 for the world and for a session holder.</param>
/// <param name="Entity">The player's entity while in game; none when offline, for the world, and for synthetic holders.</param>
public readonly record struct QuestHolder(string Key, ulong Account, EntityId Entity)
{
    /// <summary>The character whose book this is; none for the world, synthetic holders and builds without character ids.</summary>
    public CharacterKey Character { get; init; }

    /// <summary>The holder of the world's own state: what world-scope quests use.</summary>
    public static QuestHolder World { get; } = new("world", 0, EntityId.None);
    /// <summary>True for <see cref="World"/>.</summary>
    public bool IsWorld => Key == "world";
    /// <summary>True for the default value (no key): no holder at all.</summary>
    public bool IsNone => string.IsNullOrEmpty(Key);

    /// <summary>The save key of the hosting player of a local game when the game gives no Steam or account id.</summary>
    internal const ulong HostKey = 1;

    /// <summary>The holder of a player in game.</summary>
    public static QuestHolder Of(EntityId player)
    {
        if (player.IsNone) return default;
        var p = Players.Available ? Players.Find(player) : null;
        return p is null ? new QuestHolder($"session:{player.Value}", 0, player) : Of(p);
    }

    /// <summary>The holder of a player in game: keyed by their character when the game gives one, else by their account,
    /// else by their session.</summary>
    public static QuestHolder Of(Player p)
    {
        var account = p.SteamId != 0 ? p.SteamId : p.AccountId != 0 ? p.AccountId
            : Client.Available && Client.IsHosting && p.Slot == 0 ? HostKey : 0;
        var c = CharacterData.KeyOf(p.Entity);
        if (c is { IsNone: false, IsAccountOnly: false }) return new QuestHolder($"character:{c.Text}", account, p.Entity) { Character = c };
        return account == 0 ? new QuestHolder($"session:{p.Entity.Value}", 0, p.Entity) : new QuestHolder($"player:{account}", account, p.Entity);
    }

    /// <summary>A character, online or not. <paramref name="legacyAccount"/>: the Steam or account id its book may still be
    /// filed under from before characters (it moves to the first character that asks).</summary>
    public static QuestHolder OfCharacter(CharacterKey character, ulong legacyAccount = 0) =>
        new($"character:{character.Text}", legacyAccount, EntityId.None) { Character = character };

    /// <summary>A player known only by their account (offline): the character they last played in this world, or the account's
    /// book from before characters.</summary>
    public static QuestHolder OfAccount(ulong account) => new($"player:{account}", account, EntityId.None);

    /// <summary>A holder without an entity for tests and tools: its state is saved like a player's, it has no position
    /// or inventory of its own (<see cref="QuestInput"/> feeds them), and actions that need a player are skipped and logged.</summary>
    public static QuestHolder Synthetic(string name)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        var h = 0xcbf29ce484222325ul;
        foreach (var b in System.Text.Encoding.UTF8.GetBytes(name)) h = (h ^ b) * 0x100000001b3ul;
        return new QuestHolder($"player:{h | 0x8000000000000000ul}", h | 0x8000000000000000ul, EntityId.None);
    }

    /// <summary>"world", "synthetic:&lt;name&gt;", or the name or entity number of a player in game. Throws
    /// <see cref="ArgumentException"/> for anything else. Tick thread.</summary>
    public static QuestHolder Parse(string who)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(who);
        if (who.Equals("world", StringComparison.OrdinalIgnoreCase)) return World;
        if (who.StartsWith("synthetic:", StringComparison.OrdinalIgnoreCase)) return Synthetic(who["synthetic:".Length..]);
        var p = !Players.Available ? null : uint.TryParse(who, out var e) ? Players.Find(new EntityId(e)) : Players.Find(who);
        return p is { InGame: true } ? Of(p) : throw new ArgumentException($"'{who}' is not \"world\", \"synthetic:<name>\" or a player in game");
    }

    /// <summary>The holder's key, with the entity when there is one.</summary>
    public override string ToString() => Entity.IsNone ? Key : $"{Key} (entity {Entity})";

    /// <summary>This holder as a <see cref="PlayerRef"/>. With an entity: <see cref="PlayerRef.Of"/> of it (the session
    /// stamped as Kiln.Players stamps it), only when both character ids match, or (a character id is unknown) the
    /// holder's non-zero account matches the live Steam id. Otherwise gives the offline ref of <see cref="Character"/>'s
    /// id, never an unchecked occupant of a reused slot. Without an entity: that offline
    /// ref, read on any thread. <see cref="Account"/> is not copied (it may be an account id rather than a Steam id), so a
    /// holder without entity and character (the world, a synthetic or account-only holder) gives
    /// <see cref="PlayerRef.None"/>. <see cref="PlayerRef.Resolve"/> finds the current session.</summary>
    /// <exception cref="InvalidOperationException">The holder has an entity and this is not the tick thread.</exception>
    /// <exception cref="KilnUnavailableException">The holder has an entity and kiln.players cannot answer
    /// (<c>identity.players</c>).</exception>
    [TickThreadOnly]
    public static explicit operator PlayerRef(QuestHolder holder)
    {
        var offline = new PlayerRef(0, holder.Character.Id, EntityRef.None);
        if (holder.IsNone || holder.IsWorld || holder.Entity.IsNone) return offline;
        var live = PlayerRef.Of(holder.Entity);
        if (live.IsNone) return offline;
        if (holder.Character.Id != Guid.Empty && live.HasCharacter)
            return holder.Character.Id == live.CharacterId ? live : offline;
        return holder.Account != 0 && holder.Account == live.SteamId ? live : offline;
    }
}

/// <summary>One holder's standing in one quest.</summary>
/// <param name="Quest">The quest's definition.</param>
/// <param name="Holder">Whose standing this is. For a world quest, the world.</param>
/// <param name="Status">Where the quest stands for the holder.</param>
/// <param name="StepId">The id of the current step; null when the quest is not active.</param>
/// <param name="StepIndex">The current step's 0-based index in <see cref="QuestDefinition.Steps"/>; meaningful while the
/// quest is active.</param>
/// <param name="Counters">A copy of the objective counters, keyed <c>&lt;step&gt;#&lt;objective&gt;</c> (see
/// <see cref="QuestRecord.CounterKey"/>). Use <see cref="Counter"/> for the current step.</param>
/// <param name="Completions">How many times the holder has completed the quest.</param>
public sealed record QuestProgress(QuestDefinition Quest, QuestHolder Holder, QuestStatus Status, string? StepId, int StepIndex,
    IReadOnlyDictionary<string, int> Counters, int Completions)
{
    /// <summary>The counter of an objective of the current step.</summary>
    public int Counter(int objective = 0) => StepId is not null && Counters.TryGetValue(QuestRecord.CounterKey(StepId, objective), out var v) ? v : 0;
    /// <summary>The quest's full id, its status and the current step.</summary>
    public override string ToString() => $"{Quest.FullId}: {Status}{(StepId is null ? "" : $" at {StepId}")}";
}

/// <summary>The answer to a quest change. Refusals are not exceptions: <see cref="Reason"/> says why.</summary>
/// <param name="Ok">True when the change happened; false when it was refused.</param>
/// <param name="Reason">Why it was refused; empty when <paramref name="Ok"/>.</param>
/// <param name="Status">The quest's status after the call (when it was refused: the status it kept).</param>
public sealed record QuestResult(bool Ok, string Reason, QuestStatus Status) : IKilnResult
{
    // IKilnResult's reason is null when the action happened; this record's own Reason keeps its text ("" then).
    string? IKilnResult.Reason => Ok ? null : Reason;

    internal static QuestResult No(string why, QuestStatus status = QuestStatus.Locked) => new(false, why, status);
    /// <summary>"ok" and the status, or "refused:" and the reason.</summary>
    public override string ToString() => Ok ? $"ok ({Status})" : $"refused: {Reason}";
}

/// <summary>What the engine holds and has done.</summary>
/// <param name="Started">The engine has started (<see cref="Quests.EnsureStarted"/> ran).</param>
/// <param name="ServerSide">This process runs the quests: a dedicated server, or a client hosting a world while it runs.</param>
/// <param name="Quests">The number of quest definitions held.</param>
/// <param name="Dialogues">The number of dialogue definitions held.</param>
/// <param name="Npcs">The number of NPC definitions held.</param>
/// <param name="Conversations">The number of chat conversations open now.</param>
/// <param name="Errors">The number of load errors (see <paramref name="LoadErrors"/>).</param>
/// <param name="Transitions">The number of quest state changes since the engine started.</param>
/// <param name="Actions">The number of actions run since the engine started.</param>
/// <param name="ActionFailures">The number of those actions that failed.</param>
/// <param name="Waypoints">The player-waypoint writer (kiln.questmark) is loaded and its program runs in this world, so
/// waypoints can be set.</param>
/// <param name="LoadErrors">The load errors as text lines.</param>
public sealed record QuestEngineStatus(bool Started, bool ServerSide, int Quests, int Dialogues, int Npcs, int Conversations,
    int Errors, ulong Transitions, ulong Actions, ulong ActionFailures, bool Waypoints, IReadOnlyList<string> LoadErrors)
{
    /// <summary>Always false: the channel kiln.quests is gone with Kiln's journal and windows. For Kiln.Admin's status op, until it drops the field.</summary>
    public bool Channel => false;
}
