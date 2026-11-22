# Client hooks for mods (advanced tier, 0.2.0-dev.30)

A mod can hook functions of the game client (`enshrouded.exe`) the same way it hooks the server: typed hooks
(`Kiln.Sigs`), `Hooks.Detour` and `Hooks.Mid`, on the functions Kiln describes or on functions the mod describes in its
own symbol fragment, `sigdb/client.json`. This page says what is allowed, what Kiln does to keep the game safe, and what
it costs you after a game update. The mechanics (descriptors, handlers, faults, threads) are in
[../services/hooks.md](../services/hooks.md#typed-hooks-and-calls-detour_typed-kilnsigs-since-020-dev29).

## First choice: a Kiln API, not a hook

Kiln's rule for mods (the owner, 2026-11-20): a mod should not need to know the game's functions or memory. Kiln names
things, keeps a registry of them, and translates to the game; when the game updates, Kiln is updated once and every mod
that used the API keeps working. For the game's own UI that is `Ui.Screens`, `Ui.Menus`, `Ui.Crafting`, `Ui.Values`,
`Ui.Tooltip`, `Ui.Inventory` (marks and clicks on the game's slots, panels beside the backpack and chests, the chest's
entity: [../ui.md](../ui.md#the-games-inventory-slots-020-dev30)), and `Ui.World.Outline` (an entity's model outlined
with the game's own highlight, since 0.2.0-dev.30.3: [../render.md](../render.md#outlines-since-020-dev303)); for game
data the registries and blocks.

Hooks are the **advanced tier**: for what no Kiln API covers yet. Your manifest says `"advanced": true` (Kiln.Sdk:
`<KilnAdvanced>true</KilnAdvanced>`). **They may stop working after a game
update** until you (for your own fragment) or Kiln (for `Kiln.Sigs` and Kiln's own names) publish an update. If your hook
does something many mods would want, open an issue: that is how a hook becomes a Kiln API.

## What changes after a game update

| You hook | After a game update |
|---|---|
| a `Kiln.Sigs` class (`Kiln.Sigs.Items.GetItemVolume`, ...) or a Kiln symbol (`Symbols.Get("ui/...")`) | Kiln's sigdb is updated with Kiln; your mod works again with the next Kiln, unchanged |
| a function of your own `sigdb/client.json` | you update the fragment (pattern, descriptor) and publish your mod; until then it is refused |

Refused means: Kiln resolves every name against the running exe; a pattern that matches nowhere (or more than once), or a
descriptor that no longer fits the function's code, turns that name's row (`sig/<modid>/<Name>`) off with the reason, and
`Detour` answers `Ok = false` with that reason (a `HookRefusedEvent` too). Nothing is patched on a guess, your other
entries keep working, and the game runs as without your hook. Check the row before you rely on the function, and say in
your log what the player loses (the sample does).

## What a client hook may do

- **Any function of the game client your fragment describes** (or Kiln describes): rendering, UI, input, sound, the
  client's copy of game rules. The client also carries the server-world code (it hosts single player and co-op): a hook of
  such a function runs in a hosting client, not in a client that joined another server (the server computes it there).
- **Its effect stays in this player's game.** A vanilla player, other Kiln players and the server never see it unless your
  mod sends its own message. A client hook cannot make the server accept anything: the server decides inventories, damage,
  loot, positions. Use client hooks for what this game shows or how it feels; for anything that must hold for everyone,
  hook the server (`sigdb/server.json`, side `server` or `both`) and keep the client to asking.
- **No server-world APIs from a client hook.** A hook runs on the thread that calls the function (the render thread, a UI
  or worker thread), not the tick thread: touch the call in flight and your own fields. Read Kiln's services that say
  `[AnyThread]`; post anything else to the tick thread (an event, a queue your tick handler drains). `Invoke` of a
  `tick` descriptor throws off the tick thread.
- **Side.** `Symbols.LoadShipped()` loads `sigdb/client.json` in a game client and `sigdb/server.json` on a server, so one
  mod can ship both. A client-only mod: `"side": "client"`.

## What Kiln does for you (and what it refuses)

- **Only on a measured game build.** The client's policy `verified-only` (kiln.json `"client": {"policy": ...}`;
  [../client.md](../client.md)) installs hooks only on an exe whose sigdb record is verified; an unknown build runs Kiln in
  safe idle, with no mod hook.
- **Checked installation.** A typed hook goes in at a safe point of the client (client Pre for a joined client, the hosted
  world's Pre when hosting) and the first patch of a function is written only after every other thread of the process was
  stopped and checked (none inside the bytes it replaces). If that is not possible the request waits for the next safe
  tick (three tries), then it is refused; never an unchecked patch.
- **Faults.** A managed exception in your handler is caught (the game gets the original's result); a native typed handler
  that faults is unlinked and the original's full result is restored. An access violation in a managed handler outside
  the null page still ends the process (the .NET runtime's rule): keep handlers to the call in flight.
- **Unload.** Your hooks and fragment go with your mod (unload, hot reload, a disabled mod), after handlers in flight return.

## Sample and checks

- `mods/samples/ClientHookSample`: a client-only advanced mod whose `sigdb/client.json` names the client's GetItemVolume by
  its bytes and describes it (`sret: OptS32`, one pointer argument); the build generates
  `Kiln.Sigs.ClientHookSample.ItemVolume`, the mod hooks it and only counts calls. Copy its project file (`KilnServices`
  `Hooks;Sigs`, `KilnAdvanced`, `KilnSide client`, `AllowUnsafeBlocks`) and its fragment layout.
- `mods/samples/TypedHookVolume`: a typed hook of a Kiln-described function (`Kiln.Sigs.Items.GetItemVolume`).
- `mods/samples/HookSample`: raw detours, mid hooks and a byte patch from a fragment (server).
- Check your fragment against a game exe offline: `python3 tools/sigport/sigdb_check.py sigdb/client.json <enshrouded.exe>`
  (every name must say `ok` with one address); add `--fptool <kiln-fptool>` (`tools/sigport/fptool/build.sh`) to compare
  each descriptor with the function's code. The offline tool sees only the file: for a descriptor that names one of
  Kiln.Sigs' structs (`OptS32`, ...) check a copy with that struct under `"structs"` (Kiln's loader knows them). The
  sample's descriptor: `match` on client 26430625.
- Game tests (the owner's dev.30 pass, `tools/client-checks`): `06b-typed-hooks.client` (a typed hook installs and runs in
  a game client) and `ui-hooks.c8` (a client mod's own fragment). Until they pass, client hooks are built and lab-tested,
  not seen in a real game client.
