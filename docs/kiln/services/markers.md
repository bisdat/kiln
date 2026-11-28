# Kiln.Markers: markers on the map, a compass strip and labels in the world

A mod puts markers by key on three surfaces of a player's screen: the **game's own map** (one of the game's marker icons),
**Kiln's compass strip** at the top of the screen and a **label in the world** (a diamond and a line of text above a
point). A marker can **follow an entity** (a nameplate over a creature, a pin on a moving cart). A **server** mod sends
markers to every player or to one player; a player sees them when their game runs Kiln, also without the server mod.
Markers are **session only**: nothing is saved, a player's own markers end with their world, a server's with its world.

| Layer | File |
|---|---|
| C table | `native/core/abi/services/kiln_markers.h` (`kiln.markers` v1; the internal `kiln.markers.bridge` v1) |
| Native | `native/services/markers/` → `kiln/services/kiln-markers.dll` |
| Managed | `managed/services/Kiln.Markers/` → `kiln/host/Kiln.Markers.dll` (`Markers`, `Markers.Icons`, the built-in receiver) |
| Sample | `mods/samples/MarkersSample` |
| Tests | `native/services/markers/tests/run.sh` (host), `managed/tests/Kiln.Markers.Tests`, lab mods `mods/tests/markers-test` (anchors) and `mods/tests/markers-send-test` (server markers, with `tools/nettest/markerstest.py` as a receiving Kiln peer; its demo mode for real clients is switched on by the control file `kiln/control/markers-send-test.demo`) |
| Evidence | dossier chapter 04 (map lists, the icon catalog, anchors, wire v1); `docs/re/map.md` |

## Using it (a player's own markers)

```csharp
// once the icon catalog is ready (Markers.Icons.IsReady, or Markers.CatalogChanged)
var well = Markers.Icons.Resolve("well_plant");            // an alias, or the canonical key "type:ffe5e444"
var r = Markers.Set(new MarkerSpec("boss_gate", new WorldPosition(3670, 820, 1500), "Raid entrance",
    well.Icon!.Icon, MarkerSurface.Map | MarkerSurface.Compass | MarkerSurface.World, Height: 2));
if (!r.Ok) Log.Warn($"marker: {r.Reason}");

Markers.Remove("boss_gate");   // or Markers.Clear() for all of this mod's markers
```

`Set`, `Remove` and `Clear` are client-side and work on any thread. The key is the marker's id within the mod
(1 to 64 characters of `A-Z a-z 0-9 _ . : -`); setting the same key again updates it, setting an identical value does
nothing (`Affected` 0, no event). A mod's markers are bound to its mod id, so two mods never overwrite each other's keys,
and they go when the mod unloads.

The same marker as JSON (`MarkerSpec.FromJson`, `ToJson`), with the same defaults and units:

```json
{"id":"boss_gate","position":{"x":3670,"y":820,"z":1500},"text":"Raid entrance","icon":"well_plant",
 "surfaces":["map","compass","world"],"follow":null,"height":2,"maxDistance":0,"priority":0,"rgba":4294967295,"mapLabel":"none"}
```

## The surfaces
| Surface | What the player sees | Capability |
|---|---|---|
| `Map` | the icon of a game marker type on the game's world map, among the game's own markers. It follows that icon type's fog, zoom and filter rules. No name tooltip in v1 (`MapLabel.None`): `Text` is drawn on the other surfaces only | `markers.map` |
| `Compass` | a coloured diamond on Kiln's own compass strip at the top of the screen (the full 360 degrees, north straight up when you face north), with the text and the distance in metres. It is Kiln's strip, not the game's compass | `markers.compass` |
| `World` | a coloured diamond and the text over the point (`Height` metres above it), at any distance, through walls (no occlusion) | `markers.world` |

- `Icon` is for the map only; without `Map` it must be `MarkerIcon.None`. `Rgba` (0xRRGGBBAA) colours the compass and
  world glyphs and text; the map icon keeps the game's colours.
- Drawn surfaces show only on the play screen (in game, or typing in chat), not in menus or the Kiln menu, and need a
  camera. `MaxDistance` (metres from the camera, 0 = no limit) applies to every surface.
- Each surface has a budget: at most 128 mod markers on the map per step (and never more than the game's map list has
  room for after its own markers), 128 world labels and 64 compass entries per frame. When there are more, the order is
  exact: `Priority` (higher first), distance from the camera, the player's own markers before received ones, then the
  mod id, then the key. Markers that do not fit come back by themselves when there is room.
- The map icons are the game's 43 marker types that have an icon; 37 of them can be used on the map. The six fast-travel
  types are listed (`Markers.Icons.All`) but refused on the map, so a mod cannot make a fake travel point. Map icons are
  offered only on the game build whose icon catalog Kiln has verified; after a game update `markers.map` stays off until
  Kiln is updated (`CatalogChanged.Reason` says so). Compass and world markers do not need the catalog.
- The game rebuilds its map list at every client step, once per frame; Kiln writes its map icons back after each one,
  so they stay on the map at any frame rate (sigdb feature `markers.client_map_steps`, GitHub #159). On a game build
  where that feature does not resolve they are written at the client's 60 Hz steps only and can flicker above 60 fps;
  kiln.log says so once at start. docs/re/map.md, "The list in a frame".
- A dedicated server has no marker icons of its own: it takes the catalog from the first player whose Kiln sends the
  verified catalog of this game build. Until then `Markers.Icons.IsReady` is false and a server `Send` with the map
  surface is refused with `CatalogNotReady`; wait for `Markers.CatalogChanged`. Compass and world markers can be sent at once.

## Visibility events
`Markers.Changed` reports every accepted change of a marker's value (Added, Updated, Removed, Cleared), at the next step
of its world. `Markers.VisibilityChanged` reports what each marker gets on screen: per surface `Ready` or the reason it is
not shown (`MapCapacity`, `SurfaceBudget`, `CameraUnavailable`, `BehindCamera`, `OutOfRange`, `FollowMissing`,
`AnchorEnded`, `SourceStale`, `InvalidPosition`, `CapabilityOff`, `WorldInactive`), once when a marker is first
evaluated (`Initial`), then only when that changes, and a last time (`Removed`) when it goes. This is admission, not
pixels: a map marker is "Ready" while the map is closed too.

## Following an entity
```csharp
var a = await Markers.CaptureAnchorAsync(creature);         // a lease on this incarnation of the entity
if (a.Ok) Markers.Set(new MarkerSpec("pet", default, "Rex", MarkerIcon.None, MarkerSurface.World, Follow: a.Anchor, Height: 2.2f));
a.Anchor?.Dispose();                                         // the stored marker keeps its own reference

await Markers.SetNameplate("pet", creature, "Rex");          // the same in one call: a world label that follows
```
An anchor follows one incarnation of an entity. When the entity is destroyed, unloaded or its id is reused, the marker
stays but is hidden with `AnchorEnded` and never jumps to whatever gets that id next; capture again and `Set` to follow
the new one. A moment without a position (`FollowMissing`) recovers by itself. A server mod uses
`Markers.CaptureServerAnchor` on the tick thread for markers it sends; the server sends the positions, so a player sees the
marker move even where their game does not have the entity.

## Server markers
```csharp
Markers.Send(EntityId.None, spec);      // every player, also those who join later in this session
Markers.Send(player, other);            // one player: replaces the broadcast value of that key for them
Markers.SuppressSent(player, "k");      // hide a broadcast marker for one player
Markers.Unsend(player, "k");            // remove that player's override: they see the broadcast value again
Markers.ClearSent(player);              // all of this mod's markers for that player; ClearSent() all of them
```
Server sends are `[ServerOnly, TickThreadOnly]`. The result says what happened per player: `Committed` (the server
stored it), `Queued` (players with Kiln whose update is on its way) and `Refused` (connected players who cannot take
it: no Kiln, a surface they do not have, their handshake still running). `Ok` is true only when nobody was refused;
a refusal is not a rollback, and the marker reaches a player as soon as they can take it. Players **without Kiln get
nothing** (no packet is sent to them) and are counted as refused. Received markers are the player's to see, not the
server's to read back: no visibility comes back to the server.

On a dedicated server or a world another player hosts, through 0.2.0-dev.31.11 no joined player's link came up:
the game's transport pads every payload with zeros to a multiple of 4 bytes, and both ends refused the padded messages
("a bad marker message from peer 0x…: the message has bytes after its end", every 5 s, while the client kept saying
HELLO). Since 0.2.0-dev.31.12 both ends take up to 3 zero bytes after a message when its length is a multiple of 4,
and nothing else. Both ends need 0.2.0-dev.31.12: an older client still refuses a newer server's padded messages.
Since 0.2.0-dev.31.13 a client whose HELLO gets no answer (such an older server, or a server whose world is gone while
the connection lingers) says HELLO once a second five times, then after 5, 10, 20 and 40 s, then once a minute; a new
connection, client world or lapsed lease starts over at once a second.

## Limits
Kiln's own policy, not the game's: 128 markers per mod and source, 512 local and 512 received on a client (1024 in all),
64 changes per mod (256 in all) between two client steps (the 65th is `Full`: spread big loads over steps), 4 MiB of
marker text per process and 256 KiB of new text per mod (kept until the game exits; the same text again costs nothing),
64 mod ids per process. Following: 128 anchor leases per mod, 256 per world, 512 in all, 1024 watched entities; 128 local
markers of a client may follow an entity; 64 waiting `CaptureAnchorAsync` calls per mod (256 in all). Received markers may
add at most 1 MiB of new text per link to a server (256 KiB per server mod); a label whose text keeps changing (a countdown)
uses this up, after which that one marker keeps its last text until the player's next link (a rejoin, another server),
while other markers and mods keep updating. A server keeps at most 1024 definitions, 512 broadcast, 128 per mod across all players, 512
effective per player and talks to at most 32 marker peers. Nothing is evicted: a call over a limit is refused with `Full`.

## Threads and errors
`Set`, `Remove`, `Clear`, `Icons` and the events' add/remove work on any thread; events arrive at their world's step
(when a world stops, its last events arrive on the stopping thread: do not call game code then). `CaptureAnchorAsync`
completes on the client's step; `CaptureServerAnchor` and the server sends are tick-thread only. Results follow Kiln's
one rule: a refusal by the current state is `Ok = false` with a reason (`NotFound`, `Full`, `NotReady`,
`CatalogNotReady`, `RequiresClient`, `Partial`), a bad argument throws `ArgumentException`, a capability that is off
throws `KilnUnavailableException` naming it. A spec that asks for a surface whose capability is off is refused whole:
nothing is drawn partly.

## Capabilities
| Row | What it says |
|---|---|
| `kiln.markers` | the table is here |
| `markers.local` | a client's own markers (needs `world.lifecycle`) |
| `markers.catalog` | the icon catalog (`Markers.Icons`); `IsReady` is its warm-up |
| `markers.map` | native map icons: the map list's guards resolved (`markers.client_map`) and the catalog verified for this game build; with `markers.client_map_steps` (not a row of its own) they are written again after every client step |
| `markers.compass`, `markers.world` | the drawn surfaces: kiln.render's camera and kiln.ui's frame and screen state |
| `markers.nameplates` | `markers.world` and `markers.anchors.client` |
| `markers.anchors.client`, `markers.anchors.server` | following an entity of that world |
| `markers.send` | server markers and the built-in receiver's link |
| `markers.map_text` | the game's own map tooltip for a marker: off in v1 |

## What it cannot do (v1)
- Save markers, or read or write the player's own map pins. Markers are session only; a restart starts empty and a server
  mod repopulates its markers itself.
- Show markers to players without Kiln. (A server can place one of the game's own marker templates as an entity with
  `Spawn.Entity`; vanilla players see that template's stock icon on their map: an advanced route, see chapter 04 §8.1.)
- A name tooltip on the map, the game's own compass, dungeon maps, the explored-map mask, renaming bosses or NPCs.
- Game art on the compass or world labels (a diamond and text only).

## Game tests
Real-game checks for the owner's pass (machines L and W): `out/04-markers/CLIENT-CHECKS.md` of the dev.31 wave, and the
`[ClientCheck]` fixtures in `mods/tests/client-pass/Dev31Markers.cs`. Until they pass, these rows are on but unconfirmed.
