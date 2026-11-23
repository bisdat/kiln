# Chat (Kiln.Chat)

## Arg — Factories for `ArgumentType`s.
- `static Bool() -> ArgumentType` — true/false, on/off, yes/no, 1/0.
- `static Choice(params string[] options) -> ArgumentType` — One of `options` (any case); the value is the option as declared.
- `static Double(double min = -1.7976931348623157E+308, double max = 1.7976931348623157E+308) -> ArgumentType` — A number (invariant culture, "1.5") in `min`..`max`.
- `static Duration(TimeSpan? min = null, TimeSpan? max = null) -> ArgumentType` — A duration: "90s", "5m", "1h30m", "250ms", "00:05:00", or seconds.
- `static Entities() -> ArgumentType` — Entities: ids, player names or any selector (`@e[type=Enemy_Vukah*,distance=..40]`, `@a`).
- `static Entity() -> ArgumentType` — One entity: an entity id (a number, not checked for existence), `@s`, `@p`, `@r`, or a selector that matches exactly...
- `static Enum(Type enumType) -> ArgumentType` — A name of the enum `enumType` (any case).
- `static Enum<T>() -> ArgumentType` — A name of `T` (any case).
- `static Greedy() -> ArgumentType` — Everything to the end of the line, as typed.
- `static Int(int min = -2147483648, int max = 2147483647) -> ArgumentType` — A whole number in `min`..`max`.
- `static Item() -> ArgumentType` — An item by name, display name or id (Kiln.Items catalog).
- `static Player() -> ArgumentType` — One online player: name (exact, else a unique prefix, any case), entity id, `@s` (the player running the command)...
- `static Players() -> ArgumentType` — Like `Arg.Player`, plus `@a` (every player in game) and filtered selectors (`@a[distance=..30]`, `@r[limit=2]`).
- `static Position() -> ArgumentType` — Three coordinates "x y z" in metres; `~` or `~5` is relative to the player running the command.
- `static String() -> ArgumentType` — One word, or "a quoted text".
- `static TimeOfDay() -> ArgumentType` — A time of day: "7", "7.5", "07:30", "7pm", "noon", "midnight", "dawn", "dusk".
- `static Word() -> ArgumentType` — One word (no spaces).

## ArgumentNode — A typed value; its parsed value is in `CommandContext.Get` under `CommandNode.Name`.
- `Describe(string description) -> ArgumentNode` — Sets the one-line help text of this argument.
- `override Display { get; } -> string` — The node as shown in usage lines: `<name>`.
- `Executes(CommandHandler handler) -> ArgumentNode` — Runs `handler` when the line ends here.
- `Requires(CommandPermission level, string? node = null) -> ArgumentNode` — Going through this node needs `node` (default `<mod>.command.<path>`), which `level` has by default.
- `Then(CommandNode child) -> ArgumentNode` — Adds a child node (subcommand or argument).
- `Type { get; } -> ArgumentType` — The argument's type: how its text is parsed.

## ArgumentType — The type of a command argument: how text becomes a value, what it suggests while typing.
- `abstract Describe() -> string` — For help: "whole number 1..1000".
- `abstract Name { get; } -> string` — Short type name: "int", "double", "bool", "word", "string", "greedy", "enum", "player", "players", "entity"...
- `abstract ValueType { get; } -> Type` — The CLR type of the parsed value (what `CommandContext.Get` returns).

## Chat — The server's text chat (service `kiln.chat`): see players' lines (and drop them), send server lines.
- `static Available { get; } -> bool` — True when the native service is loaded on this game build (capability `kiln.chat`).
- `static Broadcast(string text) -> int` — Sends a server line to every connected player.
- `static FindPlayer(EntityId entity) -> ChatPlayer?` — A connected player by entity id; null when the entity is none, not a connected player, or kiln.chat is not loaded.
- `static FindPlayer(string name) -> ChatPlayer?` — A connected player by name: exact match (ignoring case) first, else a unique prefix.
- `static Players { get; } -> IReadOnlyList<ChatPlayer>` — Connected players (including ones still loading, whose `ChatPlayer.Entity` is none).
- `static Received -> ChatMessageHandler?` — Every line a player types, on the tick thread, before it is shown to anyone.
- `static Say(EntityId speaker, string text, EntityId to = default(EntityId)) -> bool` — Adds a line spoken by `speaker` (as if they typed it, no prefix), shown to everyone or only to `to`.
- `static ServerLines { get; set; } -> ServerLineStyle` — How server lines look in players' chat.
- `static ServerPrefix { get; set; } -> string` — Text put in front of every server line (`Chat.Broadcast`, `Chat.Tell`, command replies).
- `static Status { get; } -> ChatStatus` — The native chat service's counters; all zero or false when kiln.chat is not loaded.
- `static Tell(EntityId player, string text) -> bool` — Sends a server line to one player.
- `static TellIfOnline(PlayerRef player, string text) -> bool` `0.2.2` — Sends a server line to this person, if they are still in the session the ref was captured in (`PlayerRef.IsOnline`)...

## Chat.Diagnostics — Tools for tests: inject lines as if a player sent them, read the newest inbox line.
- `static Inject(uint connection, string text) -> void` — Queues `text` as if the player with connection handle `connection` (0 = nobody) typed it.
- `static LastLine() -> ChatLine?` — The newest line in the server's chat inbox, or null when it is empty.

## ChatLine — The newest line in the server's chat inbox (diagnostics).
- `ChatLine()`
- `ChatLine(uint Type, uint Connection, ulong Account, uint ReaderMask, string Text)`
- `Account { get; init; } -> ulong` — The speaker's account id hash.
- `Connection { get; init; } -> uint` — The speaker's connection handle.
- `ReaderMask { get; init; } -> uint` — Bit r set: reader r will not be sent this line (done with it, or it was not addressed to that reader).
- `Text { get; init; } -> string` — The line's text.
- `Type { get; init; } -> uint` — The game's type number of the line.

## ChatMessage — A line a player typed.
- `Cancel { get; set; } -> bool` — Set to true to drop the line: nobody sees it, not even the sender, and no command runs.
- `Connection { get; } -> uint` — The sender's connection handle.
- `Id { get; } -> ulong` — Running number since the server started.
- `IsCommand { get; } -> bool` — The text starts with `Commands.Prefix`: after the handlers it goes to the command registry (and is not broadcast)...
- `Player { get; } -> ChatPlayer?` — The sender, or null when their connection record was not found.
- `Sender { get; } -> EntityId` — The sender's player entity; `EntityId.None` when the sender is not known.
- `SenderName { get; } -> string` — The sender's platform name; empty when the sender is not known.
- `Text { get; } -> string` — The text as typed (UTF-8 from the client).

## ChatMessageEvent — A player typed a chat line (or a tool injected one).
- `IsCommand { get; } -> bool` — The original line starts with the command prefix.
- `Message { get; } -> ChatMessage` — The line as received.
- `Player { get; } -> ChatPlayer?` — The sender; null when their connection record was not found.
- `Sender { get; } -> EntityId` — The sender's player entity; `EntityId.None` when the sender is not known.
- `SenderName { get; } -> string` — The sender's platform name; empty when the sender is not known.
- `Text { get; set; } -> string` — The line as typed; handlers may replace it.
- `TextChanged { get; } -> bool` — True when a handler changed `ChatMessageEvent.Text` from the line as typed.

## ChatMessageHandler — Handles a line a player typed (see `Chat.Received`).
- `Invoke(ChatMessage message) -> void`

## ChatPermissions — Rights of the user group a player joined with (the password in `enshrouded_server.json`).
enum: None, KickBan, AccessInventories, EditBase, EditWorld, ExtendBase, ReceiveExp

## ChatPlayer — A connected player as the chat sees them.
- `ChatPlayer(EntityId Entity, string Name, uint Connection, uint Reader, ulong Account, ChatPermissions Permissions)`
- `Account { get; init; } -> ulong` — The game's account id hash.
- `CanKickBan { get; } -> bool` — The user group may kick and ban (Kiln's default meaning of "admin").
- `Connection { get; init; } -> uint` — The game's connection handle (chat lines carry it).
- `Entity { get; init; } -> EntityId` — Player entity; `EntityId.None` while not spawned.
- `static explicit operator PlayerRef(ChatPlayer player) -> PlayerRef` `0.2.2` — This chat player as a `PlayerRef`: the person in game with `ChatPlayer.Entity` now (`PlayerRef.Of`).
- `Name { get; init; } -> string` — The platform name the chat shows as speaker.
- `Permissions { get; init; } -> ChatPermissions` — User-group rights.
- `Reader { get; init; } -> uint` — The player's chat reader slot.

## ChatStatus — Counters of the native service.
- `ChatStatus()`
- `ChatStatus(bool ReceiveHooked, bool SendReady, int Players, int Subscribers, ulong Received, ulong Consumed, ulong Sent, ulong InboxCount)`
- `Consumed { get; init; } -> ulong` — Of those, lines a subscriber consumed.
- `InboxCount { get; init; } -> ulong` — Lines held in the chat system's inbox right now.
- `Players { get; init; } -> int` — Connected players (records with a connection).
- `Received { get; init; } -> ulong` — Lines typed by players that were received.
- `ReceiveHooked { get; init; } -> bool` — Players' lines can be observed and consumed.
- `SendReady { get; init; } -> bool` — The game server and its chat system have been seen, so server lines can be sent.
- `Sent { get; init; } -> ulong` — Server lines added, one per recipient.
- `Subscribers { get; init; } -> int` — Subscribers to players' lines.

## ChoiceAttribute — A string parameter that is one of `ChoiceAttribute.Options`.
- `ChoiceAttribute(params string[] options)`
- `Options { get; } -> string[]` — The values the parameter may take.

## CommandAttribute — Declares a method as a command (or a subcommand: `[Command("zone create")]`).
- `CommandAttribute(string path)`
- `Aliases { get; init; } -> string[]` — Other names of the command (on the first word) or of the subcommand (last word).
- `Description { get; init; } -> string?` — One line of help for the command.
- `Node { get; init; } -> string?` — The permission node (default `<mod>.command.<name>[.<sub>]`).
- `Path { get; } -> string` — "name" or "name sub ...".
- `Permission { get; init; } -> CommandPermission` — Who may run it by default.

## CommandCompletion — Completion of a partly typed line: replace the text from `CommandCompletion.Start` with one of `CommandCompletion.Suggestions`.
- `CommandCompletion(int Start, IReadOnlyList<string> Suggestions)`
- `Start { get; init; } -> int` — Where in the line the replacement starts.
- `Suggestions { get; init; } -> IReadOnlyList<string>` — The words that can replace the text from `Start`.

## CommandContext — One run of a command.
- `Arg(int index) -> string` — Argument `index`; a usage error when missing.
- `ArgOrDefault(int index, string? fallback = null) -> string?` — Argument `index` (0-based, unquoted), or `fallback` when it was not given.
- `Args { get; } -> IReadOnlyList<string>` — Arguments after the name, unquoted.
- `ArgText { get; } -> string` — Everything after the name, as typed.
- `Bool(int index) -> bool` — Argument `index` as on or off: `1`, `true`, `on`, `yes` or `0`, `false`, `off`, `no`.
- `Command { get; } -> CommandInfo` — The command that runs.
- `Count { get; } -> int` — How many arguments were given.
- `Double(int index) -> double` — Argument `index` as a number (invariant culture, `1.5`).
- `Fail(string message) -> void` — Ends the command with an error message: a refusal ("only a player in the world can do this") or a failure.
- `Get<T>(string name) -> T` — Typed commands: the value of argument `name` (see `ArgumentType.ValueType`; a `CommandContext.Player` also converts...
- `GetOrDefault<T>(string name, T fallback) -> T` — Typed commands: the value of argument `name`, or `fallback` when it was not given or does not convert to `T`.
- `Has(string name) -> bool` — Typed commands: whether argument `name` was given.
- `Int(int index) -> int` — Argument `index` as a whole number.
- `KeepLine { get; set; } -> bool` — Chat only: also pass the typed line on to the chat (by default a command line is not shown).
- `Label { get; } -> string` — The name as typed (may be an alias).
- `Node { get; } -> CommandNode?` — Typed commands: the node whose handler runs.
- `Output { get; } -> IReadOnlyList<string>` — Lines sent back so far.
- `Player(int index) -> ChatPlayer` — A connected player by entity id or (unique prefix of) name.
- `Reply(string text) -> void` — Sends a line back to the source (a player gets it in chat).
- `Rest(int index) -> string` — The raw text from argument `index` to the end, as typed (quotes kept).
- `Source { get; } -> CommandSource` — Who runs the command.
- `TryGet<T>(string name, out T value) -> bool` — Typed commands: reads the value of argument `name` as `T` (see `CommandContext.Get`).
- `Usage(string? problem = null) -> CommandException` — A `CommandException` with the usage line appended.
- `Values { get; } -> IReadOnlyDictionary<string, object?>` — Typed commands: the parsed arguments by name.

## CommandException — Thrown by a handler (or `CommandContext.Fail`) to end with an error message for the source.
- `CommandException(string message)`

## CommandExecutedEvent — A command finished (ran, failed to parse, failed or was canceled).
- `Command { get; } -> CommandInfo` — The command.
- `ElapsedMs { get; } -> double` — Time spent in the handler (and parsing).
- `Line { get; } -> string` — The whole line without the prefix.
- `Result { get; } -> CommandResult` — How it ended: output, error and whether it ran.
- `Source { get; } -> CommandSource` — Who ran the command.

## CommandExecutingEvent — A command is about to run: found, the source may use it.
- `Args { get; } -> IReadOnlyList<string>` — The arguments after the name.
- `CancelMessage { get; set; } -> string?` — What the source is told when the run is canceled.
- `Command { get; } -> CommandInfo` — The command that is about to run.
- `Label { get; } -> string` — The name as typed (may be an alias).
- `Line { get; } -> string` — The whole line without the prefix.
- `Source { get; } -> CommandSource` — Who runs the command.

## CommandHandler — Runs a command.
- `Invoke(CommandContext context) -> void`

## CommandInfo — A registered command.
- `Aliases { get; } -> IReadOnlyList<string>` — Other names that run the command.
- `Description { get; } -> string` — One line of help.
- `IsTyped { get; } -> bool` — True when the command was registered as a tree of nodes (`CommandInfo.Tree` is not null).
- `Name { get; } -> string` — The command's name, without the prefix.
- `Owner { get; } -> string` — Id of the mod that registered it ("kiln" for built-ins and host code).
- `Permission { get; } -> CommandPermission` — The default level of `CommandInfo.PermissionNode`.
- `PermissionNode { get; } -> string` — The permission node a player needs (default `<owner>.command.<name>`).
- `Syntax { get; } -> string` — "/name usage".
- `ToJson(CommandSource? visibleTo = null) -> Json.Nodes.JsonObject` — Name, aliases, description, permission, usage and the argument tree (for the dashboard).
- `Tree { get; } -> LiteralNode?` — The tree of a typed command (null for a command registered with a plain handler).
- `Usage { get; } -> string` — Arguments, e.g. "\<hours> [mode]" (typed commands: of the first usage line).
- `UsageLines { get; } -> IReadOnlyList<string>` — Every way to run it, without the prefix ("heal \<target> [amount]").

## CommandNode — A node of a typed command: a `LiteralNode` (a fixed word, the command name or a subcommand) or an `ArgumentNode` (a typed value).
- `Children { get; } -> IReadOnlyList<CommandNode>` — The nodes that can follow this one.
- `Description { get; } -> string?` — One line for help.
- `abstract Display { get; } -> string` — "name" for a literal, "\<name>" for an argument.
- `Executable { get; } -> bool` — A command line may end here.
- `Name { get; } -> string` — The node's word (a literal) or argument name.
- `RequiredLevel { get; } -> CommandPermission` — Default level of `CommandNode.RequiredNode`.
- `RequiredNode { get; } -> string?` — Extra permission needed to go through this node (a subcommand), or null.

## CommandPermission — Who may run a command (the default level of its permission node).
enum: Everyone, Admin, Console

## CommandRegistration — Disposing it unregisters the command.
- `Dispose() -> void` — Unregisters the command; for a shared command it lets go of it instead.
- `Info { get; } -> CommandInfo` — The command this registration made.

## CommandResult — Outcome of `Commands.Execute`.
- `Canceled { get; } -> bool` — A `CommandExecutingEvent` handler canceled it.
- `Denied { get; } -> bool` — The source may not run it.
- `Error { get; } -> string?` — Why the command failed, or null when it ran without error.
- `Found { get; } -> bool` — A command with that name exists.
- `Name { get; } -> string` — The command name as typed ("" for an empty line).
- `Ok { get; } -> bool` — It ran and finished without error.
- `Output { get; } -> IReadOnlyList<string>` — The lines the command sent back.
- `ParseError { get; } -> bool` — The line did not match the command's arguments (typed commands).
- `Reason { get; } -> string?` `0.2.2` — Why the command did not run to its end (`CommandResult.Error`: unknown, denied, canceled, a bad argument, or the...
- `Text { get; } -> string` — Output lines, then the error, one per line.

## Commands — The command registry: "/name args" from chat, and `Commands.Execute` for the console, Kiln.Admin and the control folder.
- `static Allowed(CommandSource source, CommandInfo command) -> bool` — Whether `source` may run `command`: the console always (also when it acts as a hosting game's player); remote tools...
- `static Argument(string name, ArgumentType type) -> ArgumentNode` — A typed argument node; its value is `context.Get<T>(name)`.
- `static Complete(CommandSource source, string text, int max = 50) -> CommandCompletion` — Completions for a partly typed line ("/he", "/heal Al", "/time set "): command names, subcommands, player names...
- `static Describe(CommandSource? visibleTo = null) -> Json.Nodes.JsonArray` — Every command (visible to `visibleTo`) as JSON: name, usage lines, node, tree.
- `static Execute(CommandSource source, string line) -> CommandResult` — Runs a command line ("time set 7" or "/time set 7") as `source`.
- `static Find(string name) -> CommandInfo?` — The command registered under `name` or one of its aliases (case-insensitive); null when there is none.
- `static IsAdmin(CommandSource source) -> bool` — Admin rights of a source: console always; remote as declared; players through the admin check (default: their user...
- `static IsCommandLine(string text) -> bool` — The text starts with the prefix followed by a name character.
- `static List(CommandSource? visibleTo = null) -> IReadOnlyList<CommandInfo>` — Registered commands, sorted; with `visibleTo`, only those it may run.
- `static Literal(string name) -> LiteralNode` — A command word: the root of a typed command (its name) or a subcommand.
- `static Prefix { get; set; } -> string` — What starts a command in chat.
- `static Register(LiteralNode root, CommandPermission permission = CommandPermission.Everyone, string? description = null, params string[] aliases) -> CommandRegistration` — Registers a typed command: `root` is its name (aliases: `LiteralNode.Alias` on the root, or `aliases`).
- `static Register(string name, CommandHandler handler, CommandPermission permission = CommandPermission.Everyone, string? usage = null, string? description = null, params string[] aliases) -> CommandRegistration` — Registers a command owned by the calling mod (removed when it unloads).
- `static RegisterAll(object target) -> IReadOnlyList<CommandRegistration>` — Registers every `CommandAttribute` method of `target` (a `Type`: its static methods).
- `static SetAdminCheck(Func<CommandSource, bool>? check) -> void` — Decides who is an admin for `CommandPermission.Admin` commands.
- `static Tokenize(string text) -> IReadOnlyList<string>` — Splits a command line like a shell: spaces separate, "double" or 'single' quotes group, backslash escapes the next...
- `static UnknownCommands { get; set; } -> UnknownCommandPolicy` — What a chat line naming an unknown command does.
- `static Unregister(string name) -> bool` — Removes a command the calling mod registered (or any, when called outside mod code).

## CommandSource — Who runs a command.
- `static Console { get; } -> CommandSource` — The operator / server code.
- `static ForPlayer(EntityId player, string? name = null) -> CommandSource` — A player (admin rights come from `Commands.IsAdmin`).
- `static ForSteamId(ulong steamId, string? name = null) -> CommandSource` — A player by SteamID64, online or not (a dashboard "run as", tests).
- `Has(string node) -> bool` — Whether the source has a permission node (`Permissions.Has`).
- `static implicit operator PermissionSubject(CommandSource source) -> PermissionSubject` — Converts a source to its `CommandSource.Subject` for Kiln.Permissions calls.
- `IsAdmin { get; } -> bool` — Admin rights: `Commands.IsAdmin`.
- `IsHostConsole { get; } -> bool` — The console of a game that hosts its own world, acting as that game's local player: `CommandSource.Player` is the...
- `IsPlayer { get; } -> bool` — True when a player typed the command (`CommandSourceKind.Player`).
- `Kind { get; } -> CommandSourceKind` — What runs the command: the console, a player or a remote tool.
- `Name { get; } -> string` — The source's name: `console`, the player's name, or the remote tool's name.
- `Player { get; } -> EntityId` — The player (for `CommandSourceKind.Player`), else none.
- `static Remote(string name, bool admin = true) -> CommandSource` — A remote tool (e.g. a dashboard request), with or without admin rights.
- `SteamId { get; } -> ulong` — The player's SteamID64 (looked up through Kiln.Players for online players); 0 when unknown or not a player.
- `Subject { get; } -> PermissionSubject` — The source as Kiln.Permissions sees it.

## CommandSourceKind — What kind of source runs a command.
enum: Console, Player, Remote

## EntitySelector — A parsed target selector: `@s` (the source), `@a` (all players), `@p` (nearest player), `@r` (random player), `@e` (entities), each optionally followed by filters in...
- `Head { get; } -> char` — s, a, p, r or e.
- `static IsSelector(string? text) -> bool` — True when `text` starts like a selector (@ and one of s a p r e).
- `Limit { get; } -> int?` — The `limit=` filter: the most matches to return; null when not given.
- `MaxDistance { get; } -> double?` — The upper bound of `distance=` in metres; null when not given.
- `MinDistance { get; } -> double?` — The lower bound of `distance=` in metres; null when not given.
- `PlayersOnly { get; } -> bool` — Only players can match (@s @a @p @r, or @e[type=player]).
- `Select(CommandSource source, out string error) -> IReadOnlyList<SelectedEntity>` — Evaluates the selector for `source`.
- `Single { get; } -> bool` — At most one match (@s, @p, @r, or limit=1).
- `Sort { get; } -> SelectorSort?` — The `sort=` filter; null when not given.
- `Text { get; } -> string` — The selector as written.
- `static TryParse(string text, out EntitySelector? selector, out string error) -> bool` — Parses a selector; false with a message for a malformed one.
- `X { get; } -> double?` — The `x=` coordinate of the origin; null when not given (the source player's position is used).
- `Y { get; } -> double?` — The `y=` coordinate of the origin; null when not given (the source player's position is used).
- `Z { get; } -> double?` — The `z=` coordinate of the origin; null when not given (the source player's position is used).

## GreedyAttribute — A string parameter that takes the rest of the line.

## LiteralNode — A fixed word: the command name (root) or a subcommand.
- `Alias(params string[] names) -> LiteralNode` — Other words for this subcommand (on the root: the command's aliases).
- `Aliases { get; } -> IReadOnlyList<string>` — The other words for this subcommand.
- `Describe(string description) -> LiteralNode` — Sets the one-line help text of this node.
- `override Display { get; } -> string` — The node as shown in usage lines: its word.
- `Executes(CommandHandler handler) -> LiteralNode` — Runs `handler` when the line ends here.
- `Requires(CommandPermission level, string? node = null) -> LiteralNode` — Going through this node needs `node` (default `<mod>.command.<path>`), which `level` has by default.
- `Then(CommandNode child) -> LiteralNode` — Adds a child node (subcommand or argument).

## SelectedEntity — An entity a selector looked at: its template (null for unknown ones), where it is, and the player record for players.
- `SelectedEntity()`
- `SelectedEntity(EntityId Entity, WorldPosition? Position, string? TemplateName, string? DisplayName, TemplateKind Kind, Player? Player)`
- `DisplayName { get; init; } -> string?` — The template's catalog display name; null when there is none.
- `Entity { get; init; } -> EntityId` — The entity's id.
- `IsPlayer { get; } -> bool` — True for a player (`SelectedEntity.Player` is not null).
- `Kind { get; init; } -> TemplateKind` — The kind of template the entity has.
- `Player { get; init; } -> Player?` — The player record for a player; null for any other entity.
- `Position { get; init; } -> WorldPosition?` — Where the entity is; null when it is not known.
- `TemplateName { get; init; } -> string?` — The entity's template name; null for an unknown one.

## Selectors — Target selectors for mods: the same @s/@a/@p/@r/@e[...] the command arguments accept.
- `static Entities(CommandSource source, string selector) -> IReadOnlyList<EntityId>` — The entity ids of `Selectors.Select`.
- `static Players(CommandSource source, string selector) -> IReadOnlyList<Player>` — The players among `Selectors.Select` (a selector that may match other entities is an error).
- `static Select(CommandSource source, string selector) -> IReadOnlyList<SelectedEntity>` — Entities (players included) matching `selector`, as seen by `source`.

## SelectorSort — Sort order of a selector's matches (`sort=`).
enum: Arbitrary, Nearest, Furthest, Random

## ServerLineStyle — How a server line looks in players' chat.
enum: Notice, Spoken

## TimeOfDayAttribute — A double parameter that is a time of day in hours ("7:30", "7pm", "noon").

## UnknownCommandPolicy — What a chat line naming an unknown command does (`Commands.UnknownCommands`).
enum: Reply, PassThrough

## WordAttribute — A string parameter that is one word (no quotes).
