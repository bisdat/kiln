# kiln.chat: chat lines and commands

Players' typed lines reach mods before anyone sees them (and can be dropped); mods send server lines to everyone or to
one player; a command registry runs "/name args" from chat and from code (Kiln.Admin, the control folder).
Evidence and offsets: [docs/re/chat.md](../../re/chat.md).

| Layer | File |
|---|---|
| C table | `native/core/abi/services/kiln_chat.h` (`kiln.chat` v1) |
| Native | `native/services/chat/chat.cpp` -> `kiln/services/kiln-chat.dll` |
| Symbols | `sigdb/symbols/server.d/chat.json` (features `chat.receive`, `chat.send`, `chat.players`) |
| Managed | `managed/services/Kiln.Chat` -> `kiln/host/Kiln.Chat.dll` (`Kiln.Chat`, `Kiln.Commands`) |
| Test | `mods/tests/chat-test` |

Server config: `"enableTextChat": true` in `enshrouded_server.json` (clients only send and show text chat then).

## How it looks in game
The game takes a chat line only with a known speaker. A typed line shows as "<speaker name>: <text>"; a notice (the
type of the game's own "Player X joined." lines) shows its text alone. A server line (`Broadcast`, `Tell`, command
replies) is added once per recipient as a notice with that recipient as speaker, sent only to them: each player sees
**"[Server] text"**, with no name in front. `Chat.ServerPrefix` (default "[Server] ") is put in front of it.
`Chat.ServerLines = ServerLineStyle.Spoken` gives the older look, "<their own name>: [Server] text".
`Chat.Say(speaker, text)` adds a line spoken by a player (no prefix), as if they had typed it.

## Receiving
```csharp
Chat.Received += m =>
{
    // tick thread, before the line is shown; m.Player (entity, name, permissions), m.Text, m.IsCommand
    if (m.Text.Contains("badword")) m.Cancel = true;   // nobody sees it, not even the sender
};
```
A player's line arrives in Pre of the server frame it was received in (the chat send step runs after the sim step),
so a non-cancelled line reaches everyone with no extra delay. Handlers are removed when their mod unloads; a handler
that throws 5 times is removed.

## Sending (tick thread)
```csharp
int n = Chat.Broadcast("Restart in 5 minutes");   // players addressed
bool ok = Chat.Tell(player, "Only you see this");  // false: not connected
Chat.Say(player, "I typed this", to: other);        // spoken by player, only `other` sees it
IReadOnlyList<ChatPlayer> online = Chat.Players;    // entity, name, connection, reader, account, permissions
ChatPlayer? p = Chat.FindPlayer("Ali");             // exact name, else unique prefix
```
Text is cut to 1024 UTF-8 bytes per line; "\n" splits it into several lines.

The contract (dev.27, [contract.md](../modding/contract.md)): Kiln.Chat is server side (`[ServerOnly]`). Without the
service the sends throw `KilnUnavailableException` naming `kiln.chat` (a `NotSupportedException`, as before), and
Kiln.Chat lowers the sigdb features `chat.send`, `chat.receive` and `chat.players`; a send before the game server was
seen keeps throwing `InvalidOperationException`. `Chat.TellIfOnline(PlayerRef, text)` reaches a person only while the session
the ref was captured in lasts (false otherwise), so a line sent after an `await` never lands with whoever has that
entity slot now; `(PlayerRef)chatPlayer` gives the person behind a `ChatPlayer`. `CommandResult` implements
`IKilnResult` (`Reason` is its `Error`). `Chat.Received` is not a second event source: its handlers run first in the
dispatch that then posts `ChatMessageEvent`, which starts canceled when one of them canceled the line.

## Hooks and other mods on the same function
`kiln.chat` hooks the game's `ChatLogAdd` and the server frame's receive loop as links of the [kiln.hooks v2](hooks.md)
chain. A player's line is the call that comes **from the receive loop**: the hook tells it by `caller()`, the return
address of the game's call, so another hook on `ChatLogAdd` can sit in front of it without breaking it. (Before dev.21 a
second hook placed in front of `kiln.chat`'s, such as `kiln-emf`'s, made it see that hook's return address, and it no
longer recognised players' lines.) `kiln-emf`'s client hook is ordered after `kiln.chat`'s. An EMF mod's `hook("ChatLogAdd", ...)` or a Shroudtopia mod's
`CreateDetour` is a link of its own and runs before both.

A line that nobody consumes (an ordinary line, or a slash line naming no Kiln command) is added back in the same Pre,
before the frame's send step, with every original argument (text, type, tags, connection, account and the game's time)
through the rest of the chain, so the game gets exactly what it would have got without Kiln (since 0.2.0-dev.29.17 the
time too; before, an empty one). The line is held rather than passed at once because whether it is a Kiln command, and
whether a handler cancels or rewrites it, is decided by managed code, which runs in Pre and not inside the game's
receive loop. The links after `kiln.chat`'s on `ChatLogAdd` see it then, once; links in front of it saw it when the
game called, and not again. A link that reads `caller()` in that call gets the
record of the earlier game call on the same thread, not a new one. `state/hooks.json` lists the links: labels `chat/ChatLogAdd` and `chat/ChatReceive`.

Evidence: `native/services/chat/tests/run.sh` (Wine, no game) builds the real `kiln.chat` and `kiln-emf` hook code on one
function in the real chain, in four install orders (including an EMF mod's hook in front, added first or last): a typed line is held back and
delivered, other call sites are not, 4 threads x 50 lines are told apart per thread, and an unknown slash line that
the subscriber leaves alone reaches the game in the same Pre with the same arguments as the same line with nobody
subscribed (a link in front sees it once). Managed: `Kiln.Api.Tests` `CommandsTests` (pass-through, the setting, known
commands and usage errors) and `Kiln.TestKit.Tests` `ChatFakeTests` (end to end with fake players).

## Commands
```csharp
Commands.Register("time", ctx =>
{
    if (ctx.Count == 0) { ctx.Reply($"it is {Time.State.Clock}"); return; }
    if (!Time.TryParse(ctx.Arg(0), out var h)) ctx.Fail("try 7:30 or noon");   // ends with an error message
    Time.Set(h);
    ctx.Reply($"time set to {Time.Format(h)}");
}, CommandPermission.Admin, usage: "[time]", description: "shows or sets the time of day", "t");
```
- Chat: "/time 7:30" runs the handler as the player; the line itself is not shown (set `ctx.KeepLine = true` to let it
  through). Replies go to the player. A known command (name or alias) with wrong arguments gets its usage error, and
  the line is not shown. `/help [command]` is built in.
- A slash line naming **no Kiln command** passes through untouched (default since 0.2.0-dev.29.17): the game's chat and
  whatever else reads chat (EMF Lua mods, Shroudtopia, Flame, a native mod) get it exactly as if Kiln weren't there, with
  no reply from Kiln. Kiln never swallows other loaders' commands. Kiln's "Unknown command '/x'. Type /help for a
  list." reply, which eats the line, is a server setting: `"unknownCommands": "reply"` in `kiln/config/chat.json`
  ([Server settings](#server-settings)). `Commands.UnknownCommands` (`UnknownCommandPolicy.PassThrough` /
  `Reply`) reads the value in effect; a mod may set it in code, but the server's chat.json, when it sets the key, wins.
  Commands run from the console, the control folder or code (`Commands.Execute`) still report unknown names as errors.
- Code: `CommandResult r = Commands.Execute(CommandSource.Console, "time 7:30");` (tick thread). `r.Found`, `r.Denied`,
  `r.Ok`, `r.Error`, `r.Output`, `r.Text`. Sources: `CommandSource.Console` (admin), `CommandSource.Remote("dashboard",
  admin: true)`, `CommandSource.ForPlayer(entity)`.
- Parsing: spaces separate, "double" / 'single' quotes group, backslash escapes. `ctx.Args`, `ctx.Arg(i)`,
  `ctx.ArgOrDefault(i)`, `ctx.Int(i)`, `ctx.Double(i)`, `ctx.Bool(i)`, `ctx.Player(i)` (id or name), `ctx.Rest(i)` (raw
  text from argument i), `ctx.ArgText`. Bad arguments raise a usage message ("...; usage: /time [time]").
- Permissions: `Everyone`, `Admin`, `Console` (never from chat). A player is admin when
  `Commands.SetAdminCheck(src => ...)` says so (the players service or Kiln.Admin should install one, e.g. from
  `Players` admin flags); without one, when their user group has `canKickBan`.
- Ownership: commands, handlers and the admin check belong to the mod that registered them and are dropped when it
  unloads (hot reload included: `reload-test chat-test 5` -> 0 leaked contexts). Names are case-insensitive; another
  mod's name throws, the same mod registering again replaces it. `Register` returns a disposable registration.
- Kiln's shared commands: a service's command tree that mods ask for (`CreatureAdminCommands.Register()`,
  `BuildCommands.Register()`) belongs to Kiln (owner `kiln`, node `kiln.command.<name>`). It is added once, however
  many mods ask, stays while one of their registrations is held, and goes when the last one is disposed or its mod
  unloads. Its handlers run as Kiln.
- Handlers run on the tick thread in their mod's context (Scheduler, `await`, services attribute work to that mod).

## Server settings
`kiln/config/chat.json` (the config folder: `kiln/config`, or the active profile's configDir). Optional; unknown keys
are ignored; read when first needed and again whenever the file changes, so an edit applies without a restart.

```json
{ "unknownCommands": "reply" }
```

| Key | Values | Default | What it does |
|---|---|---|---|
| `unknownCommands` | `"passThrough"`, `"reply"` | `"passThrough"` | What a chat line naming no Kiln command does. `passThrough`: it reaches the game and other mods untouched, no reply. `reply`: Kiln eats it and tells the player "Unknown command" (the behaviour before 0.2.0-dev.29.17); other loaders' slash commands then never reach them. |

A bad value is logged (`kiln.chat: .../chat.json: unknownCommands must be "reply" or "passThrough" ...; ignored`) and
the default applies.

## C (native plugins)
```c
const KilnChatApi* c = api->service(KilnStr{"kiln.chat", 9}, 1);
bool KILN_CALL on_line(const KilnChatMessage* m, void* user) { return false; /* true = consume */ }
c->subscribe(owner, &on_line, NULL, &handle);
c->send(0 /* everyone */, KilnStr{"[Server] hi", 11}, &delivered);   // tick thread
c->say(speaker, text, 0);
c->players(buf, 64, &count);
```
`inject(connection, text)` and `last_line(...)` exist for tests (a line queued as if typed, the newest inbox line).

## Client checklist
Lab: `tools/lab/lab.sh start chat` (port 25720, text chat on), join with the Admin password from its
`enshrouded_server.json`. chat-test registers `/kt-say`, `/kt-all`, `/kt-time`, `/kt-echo`, `/kt-keep`.
1. Type "hello": everyone (you too) sees it, unchanged, at once.
2. `/kt-say hi`: you see "<you>: [Server] you said: hi"; the "/kt-say hi" line is shown to nobody.
3. `/kt-all hi` with two players: both see "<own name>: [Server] <you> says: hi".
4. `/nope`: "[Server] Unknown command '/nope'. Type /help for a list."; `/help` lists the commands.
5. `/kt-time 22:00` as Admin works; as Friend/Guest: "You may not use /kt-time."
6. `/kt-keep x`: the line is shown to everyone and you get "[Server] kept".

## In a game client (hosted worlds)
- `client.d/chat.json` re-cuts the receive-call pattern for the client build (it also captures `chat/gs.chat`).
- When the game closes the server world, the GameServer pointer and lines queued for that world are dropped; ChatReceive
  gives the next world's GameServer at its first call. In an Offline session the game has no text chat at all.
- Commands then come through the console folder (`kiln/control/console/*.cmd`), whose lines run as the console acting as
  the host's own character once it is in the world, so commands that need a player work there
  ([commands.md](../commands.md#console-folder-and-kilnctl-cmd)).
