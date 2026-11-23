# Permissions (Kiln.Permissions)

## PermissionDecision — The answer of `Permissions.Explain`: allowed or not, and which rule decided.
- `PermissionDecision(bool Allowed, string Node, PermissionLevel Level, string? Rule, string From, IReadOnlyList<string> Groups)`
- `Allowed { get; init; } -> bool`
- `From { get; init; } -> string`
- `Groups { get; init; } -> IReadOnlyList<string>`
- `Level { get; init; } -> PermissionLevel`
- `Node { get; init; } -> string`
- `Rule { get; init; } -> string?`

## PermissionLevel — Who has a node when no rule in permissions.json mentions it (Forge's `DefaultPermissionLevel`).
enum: Everyone, Admin, Console

## PermissionNode — A registered node.
- `PermissionNode(string Node, PermissionLevel DefaultLevel, string Description, string Owner)`
- `DefaultLevel { get; init; } -> PermissionLevel`
- `Description { get; init; } -> string`
- `Node { get; init; } -> string`
- `Owner { get; init; } -> string`

## Permissions — Permission nodes (Forge's PermissionAPI).
- `static AddPlayerToGroup(ulong steamId, string group, string? name = null) -> void` — Puts a player into a Kiln group (created when missing).
- `static Explain(PermissionSubject subject, string node) -> PermissionDecision` — The decision with the rule that made it (for "/perm check" and the dashboard).
- `static FilePath { get; } -> string` — Full path of permissions.json.
- `static Find(string node) -> PermissionNode?`
- `static GroupsOf(PermissionSubject subject) -> IReadOnlyList<string>` — The Kiln groups that apply to the subject (own, from the user group, "default", inherited).
- `static Has(EntityId player, string node) -> bool` — Whether the online player with this entity has `node`.
- `static Has(PermissionSubject subject, string node) -> bool` — Whether `subject` has `node`.
- `static Has(Player player, string node) -> bool` — Whether an online player has `node`.
- `static IsAdmin(PermissionSubject subject) -> bool` — Whether the subject counts as admin (for `PermissionLevel.Admin` nodes).
- `static MapUserGroup(string userGroup, IEnumerable<string> groups) -> void` — Maps a server user group (enshrouded_server.json name) to Kiln groups (empty: no mapping).
- `static Nodes { get; } -> IReadOnlyList<PermissionNode>` — Every registered node, sorted.
- `static PersonHas(PlayerRef player, string node) -> bool` `0.2.2` — Whether this person has `node`, online or not (`PermissionSubject.ForPerson`: asked by Steam id, so a ref of an...
- `static Register(string node, PermissionLevel defaultLevel = PermissionLevel.Admin, string? description = null) -> PermissionNode` — Registers `node` ("mymod.fly", "mymod.command.home") with the level that has it when permissions.json says nothing.
- `static Reload() -> void` — Reads permissions.json (and the server's user groups) now.
- `static RemoveGroup(string group) -> void` — Removes a Kiln group (and players' memberships of it).
- `static RemovePlayerFromGroup(ulong steamId, string group) -> void`
- `static ServerUserGroups { get; } -> IReadOnlyList<(string Name, PlayerPermissions Rights)>` — The server's user groups as read from enshrouded_server.json (name, rights bits).
- `static SetGroup(string group, bool? admin = null, IEnumerable<string>? inherits = null) -> void` — Creates or changes a Kiln group: admin flag and inherited groups (null = unchanged).
- `static SetGroupNode(string group, string node, bool? allow) -> void` — Grants, denies or clears a node for a Kiln group (created when missing).
- `static SetPlayerNode(ulong steamId, string node, bool? allow, string? name = null) -> void` — Grants (true), denies (false) or clears (null) a node for one player.
- `static ToJson() -> Json.Nodes.JsonObject` — The whole configuration as JSON (groups, user-group mapping, players, server user groups, nodes).
- `static UserGroupOf(PermissionSubject subject) -> string?` — The server user group (enshrouded_server.json "userGroups" name) Kiln maps the subject to, or null.

## PermissionsChangedEvent — permissions.json was reloaded or edited, or a server user group changed.
- `Reason { get; } -> string` — "file", "api" or "userGroups".

## PermissionSubject — Whose permission is asked.
- `AdminOverride { get; init; } -> bool?` — Whether the player counts as admin for `PermissionLevel.Admin` nodes before groups are looked at (Kiln.Chat passes...
- `static Console { get; } -> PermissionSubject`
- `DeclaredAdmin { get; init; } -> bool` — Remote tools: whether they act as admin.
- `Entity { get; init; } -> EntityId` — The player entity (online players); none for offline players and non-players.
- `Equals(PermissionSubject other) -> bool`
- `static ForPerson(PlayerRef player) -> PermissionSubject` `0.2.2` — A person by `PlayerRef`, online or not.
- `static ForPlayer(EntityId entity) -> PermissionSubject` — An online player by entity (looked up through Kiln.Players when asked).
- `static ForPlayer(Player player) -> PermissionSubject` — An online player by entity (looked up through Kiln.Players when asked).
- `static ForSteamId(ulong steamId, string? name = null, PlayerPermissions? gameRights = null, uint? userGroupId = null) -> PermissionSubject` — A player by SteamID64, online or not (the dashboard, tests).
- `GameRights { get; init; } -> PlayerPermissions?` — The user group's rights when known (else looked up from the online player).
- `Kind { get; init; } -> PermissionSubjectKind`
- `Name { get; init; } -> string`
- `static operator !=(PermissionSubject left, PermissionSubject right) -> bool`
- `static operator ==(PermissionSubject left, PermissionSubject right) -> bool`
- `static Remote(string name, bool admin = true) -> PermissionSubject`
- `SteamId { get; init; } -> ulong` — SteamID64; 0 = look it up from `PermissionSubject.Entity`.
- `UserGroupId { get; init; } -> uint?` — The game's user-group id when known.

## PermissionSubjectKind
enum: Console, Player, Remote
