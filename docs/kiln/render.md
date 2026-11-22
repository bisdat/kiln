# Render events: code every frame, drawing on screen and in the world

Client only. A mod subscribes to `RenderFrameEvent` and draws through it: shapes, text and images in pixels, and
markers, labels, lines, boxes and rings at world positions, projected with the camera the game renders with.
Forge: `RenderGuiEvent` (2D) and `RenderLevelStageEvent` (world). Sample: `mods/samples/RenderSample` (FPS counter,
compass, waypoints). Since 0.2.0-dev.30.3 a mod can also outline an entity's model with the game's own highlight
(`Ui.World.Outline`, [Outlines](#outlines-since-020-dev303)).

| Layer | Where |
|---|---|
| C# | `managed/services/Kiln.Ui/Render.cs` (`RenderFrameEvent`, `RenderDraw`, `WorldDraw`, `RenderCamera`, `Render`), `RenderDispatch.cs`; outlines: `UiWorldOutline.cs` (`Ui.World`, `WorldOutline`, `OutlineOptions`) |
| C table | `native/core/abi/services/kiln_render.h` (`kiln.render` v2: v1's drawing and camera, outlines appended in 0.2.0-dev.30.3), registered by `kiln-ui.dll` |
| Native | `native/services/ui/render.cpp`, projection `render_math.h`; outlines `outline.cpp`, `outline_core.h` |
| Camera hook | `sigdb/symbols/client.d/render.json` (feature `render.camera`); RE: [../re/client-render.md](../re/client-render.md#the-camera) |
| Outline hooks | `sigdb/symbols/client.d/outline.json` (feature `ui.world_outline`); RE: [../re/client-render.md](../re/client-render.md#highlight-groups-outlines) |
| Tests | `managed/tests/Kiln.Ui.Tests` (projection, budget, dispatch, `WorldOutlineTests`), the test kit's fake (`KilnHarness.Outlines`, [modding/unit-tests.md](modding/unit-tests.md#outlines)), `native/services/ui/tests/run.sh` (projection, outlines, host) |

## C#
```csharp
public sealed class Markers : Mod
{
    private UiImage? _pin;
    private WorldPosition _home = new(3790, 846, 1467);

    public override void OnLoad()
    {
        _pin = Ui.LoadImage("pin.png");                      // a PNG in the mod's folder
        Events.On<RenderFrameEvent>(OnFrame);                // or [Subscribe] private void OnFrame(RenderFrameEvent e)
    }

    private void OnFrame(RenderFrameEvent e)                 // the render thread, every frame
    {
        e.Draw.Text(10, 10, $"{e.Fps:F0} fps");             // pixels, top left
        if (!e.Camera.Valid) return;                         // menu, loading
        e.World.Marker(_home, Color.Orange);                 // skipped when behind the camera
        e.World.Label(_home, $"Home {e.World.DistanceTo(_home):F0} m");
        e.World.Circle(_home, 3, Color.Orange);              // a ring on the ground, cut at the camera
        if (!e.World.ToScreenClamped(_home, 40, out var edge, out var angle))
            e.Draw.Arrow(edge, angle, 16, Color.Orange);     // off screen: an arrow on the edge
    }
}
```

### `RenderFrameEvent`
| Member | What |
|---|---|
| `Frame`, `DeltaSeconds`, `Fps` | overlay frame number, seconds since the last frame (at most 0.25), smoothed frames per second |
| `Width`, `Height`, `Scale` | screen size in pixels, UI scale (multiply your pixel sizes by it) |
| `MenuOpen`, `Capturing`, `Screen` | the Kiln menu is open, a Kiln screen has the input, the game's screen (`GameScreen`) |
| `Camera` | `RenderCamera` for this frame (below) |
| `Draw` | `RenderDraw`: pixels |
| `World` | `WorldDraw`: world positions |

`RenderDraw` (origin top left, y down, colours `Color`): `Line`, `Rect` / `FillRect` (rounding), `Circle` / `FillCircle`,
`Triangle` / `FillTriangle`, `Polyline` (closed or not), `FillPolygon` (convex), `Arrow(tip, angle, size)`,
`Text(x, y, text, color, size, TextAlign, TextStyle, middle)` (`Plain`, `Shadow`, `Outline`), `MeasureText`,
`Image(UiImage, x0, y0, x1, y1, tint)`, `ImageCentered`. Images come from `Ui.LoadImage` (a PNG file in the mod or
bytes) and show from the frame after loading; the mod's images are freed when it unloads.

`WorldDraw` (metres, x east, y up, z north): `Ready` (the camera is known), `DistanceTo`, `ToScreen(p, out pixel)`,
`ToScreenClamped(p, margin, out pixel, out angle)` (off screen or behind: on the edge, in its direction),
`Marker(p, color, size, MarkerShape)` (`Diamond`, `Circle`, `Square`, `Triangle`, `Cross`), `Label(p, text)`,
`Icon(p, image, size)`, `Line(a, b)`, `Box(min, max)` / `Box(center, size)`, `Circle(center, radius)`. Points behind the
camera are not drawn; lines, boxes and rings are clipped to the near plane and viewport, so visible segments still show.

### `RenderCamera`
`Valid`, `Position`, `Rotation` (quaternion, camera space to world), `Forward` / `Right` / `Up`, `Heading` (degrees: 0
north, 90 east), `Pitch`, `FovY` (radians, vertical), `Near`, `Far`, `Distance` and `Pivot` (what the third-person
camera looks at: about the character's head), `ToCameraSpace(p)`, `Project(p, width, height, out pixel, out depth)`.
`Render.Camera` reads the latest one from any thread (key handlers, `Client.Tick`).

### `Render`
`Available`, `CameraAvailable`, `Camera`, `HandlerBudgetMs` (default 2), `Stats` (`Handlers`, `Frames`, `LastFrameMs`,
`MaxFrameMs`, `Disabled`, camera state), `FieldOfViewIsHorizontal` (a calibration switch for checking the projection on a
new game build; off).

## Threads
`RenderFrameEvent` is posted on the **game's render thread** (the thread that presents frames), inside Kiln's overlay
frame, not on the tick thread. It runs on the bus's subscriptions: `Events.On`, `[Subscribe]`, priorities, the handler
runs as its mod, and every subscription ends when the mod unloads. In a handler:
- read what your mod keeps (copies, immutable snapshots swapped whole); don't touch game state;
- act with `Client.Post` (client world, main thread) or `Context.Scheduler.Post` (server world when hosting);
- post other bus events with `Events.Enqueue` (`Events.Post` is for the tick thread and throws here); render frame events
  themselves cannot be reposted through either method;
- don't keep `e`, `e.Draw` or `e.World`: they are reused next frame. Drawing outside the handler does nothing.

Managed unsubscribe and unload do not wait for a handler that dispatch has already admitted. `OnUnload` can overlap
that final call, so use snapshots and coordinate disposal of resources you own.

A handler of a base class (`KilnEvent`) does not get render frames; subscribe to `RenderFrameEvent` itself.

## Cost
- Nobody subscribed: render events request no overlay frames. Existing Kiln windows and HUD can still request frames.
  The camera hook still copies the camera on client updates, and the subscription timer still checks periodically.
- Subscribed: one overlay frame per present. A frame in which nothing is drawn records no GPU work; drawing adds one
  render pass over the swapchain image (the same pass Kiln's HUD and windows use).
- Dispatch reuses cached subscription arrays; the overlay's frame demand follows subscriptions within a quarter second.

## Safety: the budget
Nothing can stop a handler that is running, so a slow one is turned off instead:
- over `Render.HandlerBudgetMs` (2 ms) in more than 60 of the last 120 frames (the first 30 calls have grace for JIT and
  initial uploads), or three calls of 100 ms or more, including warm-up and throwing calls;
- five exceptions (like the bus).
The handler is unsubscribed, the mod's log says why, and a toast names the mod. Other handlers keep running. Handler
times show in `Events.Diagnostics.Handlers` like any bus handler.

## Layers
Everything drawn here goes under Kiln's own windows, screens, HUD lines and toasts and over the game's picture,
including the game's HUD and menus. Handlers draw in priority order (a `High` handler's shapes are under a `Normal`
one's). Full-screen colour effects (`ScreenEffects`, [services/screen.md](services/screen.md)) are under all of it.

## The camera
The camera is the local player's view as the client writes it each frame (position, rotation, field of view), read by a
hook on the game's main thread (feature `render.camera`). Points drawn this way follow the scene; camera shake (a glider's
wobble) is not in it, so markers may tremble a little against the scene there. The camera expires 500 ms after its last
write, including when returning to the main menu or loading. On a game build where the camera is not found,
`Render.CameraAvailable` is false, `Camera.Valid` stays false and world drawing does nothing; screen drawing still works.

## C (native plugins)
`kiln_render.h`: draw inside a kiln.ui frame callback (`kiln_ui.h` `subscribe_frame`); call `acquire_frames` so a frame
happens at every present while you draw. Before unloading, unsubscribe the frame callback with `ui->unsubscribe(sub)`
from your cleanup thread (outside callbacks), then call `r->release(frames)`. Unsubscribe cancels pending callbacks and
waits for an in-flight callback to finish; releasing frame demand alone does not unsubscribe it. These service handles
require explicit cleanup; core owner revocation does not remove them. A callback trying to unsubscribe a different
callback that is currently running gets `KILN_ERR_WRONG_THREAD`; retry from the cleanup thread.
```c
#define S(x) (KilnStr){x, sizeof(x) - 1}
const KilnUiApi* ui = api->service(S(KILN_UI_SERVICE), 1);
const KilnRenderApi* r = api->service(S(KILN_RENDER_SERVICE), 1);
r->acquire_frames(owner, &frames);
ui->subscribe_frame(owner, on_frame, NULL, &sub);
static void on_frame(const KilnUiFrame* f, void* user) {
    double home[3] = {3790, 846, 1467};
    float px[3];
    if (r->world_to_screen(home, f->width, f->height, px)) r->circle(px[0], px[1], 6, 0xFF9030FF, 0, 1, 1);
    r->text(10, 10, 0xFFFFFFFF, 0, KILN_RENDER_TEXT_SHADOW, S("hello"));
}
```
`camera(KilnRenderCamera*)` and `world_to_screen` work on any thread.

## Outlines (since 0.2.0-dev.30.3)
Client only. `Ui.World.Outline` lights an entity's model with the game's own highlight, the outline the game draws
around the building cursor's target. Use it to show the player which chests just received items, or where a searched
item lies. It is not drawing: there is no frame event, it works on any thread, and the outline stays until it ends.
GitHub issue #111 (thanks Fuzen777).

```csharp
public sealed class ChestGlow : Mod
{
    private static readonly OutlineOptions Filled = new()
    {
        Color = Color.Green, Pulse = true, Duration = TimeSpan.FromSeconds(8),
    };

    // on the client, when your server half says these chests just received items (a network message)
    private void OnFilled(IReadOnlyList<EntityId> chests)
    {
        if (!Ui.World.OutlinesAvailable) return;     // a server, an older Kiln, a game build without it
        foreach (var chest in chests)
            Ui.World.Outline(chest, Filled);         // ends by itself after 8 s
    }
}
```
Keep the handle when you end the outline yourself:
```csharp
_found?.Dispose();                                   // the previous search result
_found = Ui.World.Outline(target, new OutlineOptions { Priority = 10 });
// later (a tick, a key press): it shows from the next client step
if (_found is { State: OutlineState.Waiting, Reason: OutlineReason.NoModel }) Hud.Toast("Too far away to light up");
```
- `Ui.World.Outline(entity, options)` returns a `WorldOutline`. `Dispose()` takes the outline away; it is idempotent
  and never throws. `Ui.World.ClearOutlines()` takes away every outline of your mod. Your mod's outlines end when it
  unloads.
- The entity is the client world's: a replicated entity has the server's id, so the id your server half sends works.
- `Ui.World.OutlinesAvailable`: the row `ui.world_outline` is on and kiln.render has outlines. `Outline` throws
  `KilnUnavailableException` when it is off, `ArgumentException` for entity 0 or a colour with alpha 0, and
  `InvalidOperationException` when your mod holds 1024 outlines already.
- The outline shows from the next client step, not at once.

### `OutlineOptions`
| Member | What |
|---|---|
| `Color` | the colour; alpha must be above 0. Default `Ui.World.DefaultOutlineColor`, gold (255, 196, 64) |
| `Pulse` | the colour breathes (1.2 s period, 40 % to 100 % brightness) |
| `Priority` | among mods' outlines on the same entity: the highest wins; on a tie, the newest. When more than two colours are in use, the colours of the highest-priority outlines keep the two groups |
| `Duration` | null (the default): until disposed. Otherwise above zero and at most `uint.MaxValue` ms (`ArgumentOutOfRangeException`) |
| `ThroughWalls` | (0.2.0-dev.30.5, mod API 0.2.14) also seen through walls: Kiln draws its own screen-space silhouette on its UI layer every frame, a hull of a box about 1.2 m wide and high at the entity's position, projected with the camera world labels use, as an outline plus a soft fill in `Color`, breathing with `Pulse`, ending with the outline. Row `ui.world_outline.through_walls` (`Ui.World.ThroughWallsAvailable`); while it is off the outline shows without the silhouette |

### States
`WorldOutline.State` and `Reason` ask kiln.render at each read. `Active` is `State != Ended`. `ColorShared`: a
third colour is in use and this one shows with the nearest other colour.

| State | Reasons |
|---|---|
| `Waiting` | `Pending` (not applied yet, or no client world), `Outshone` (another outline on the entity wins), `NoModel` (no model instance now: hidden, not streamed in, a voxel model), `SwitchedOff` (the player's setting is `off`) |
| `Shown` | `None`: the model shows this outline's colour |
| `GameHighlight` | `None`: the game's own highlight (the building cursor's) or another tool's shows instead, for now |
| `Ended` | `Removed` (Dispose, ClearOutlines), `Despawned`, `WorldEnded` (left the game, joined another), `Expired` (the duration ran out), `ModUnloaded`, `Failed` (Kiln's hooks or its stage stopped; kiln.log says why) |

A waiting outline takes over when the reason goes away (the winner ends, the model streams in, the cursor moves on).

### Limits
- **Two colours at once.** Kiln uses two of the game's highlight groups, Group1 and Group2, and colours them itself.
  Each distinct (colour, pulse) pair in use takes one; a third pair shares the group whose colour is nearest
  (`ColorShared`). Group3 and Group4 stay free for other tools. Which groups the game uses:
  [../re/client-render.md](../re/client-render.md#highlight-groups-outlines).
- **The game's highlight wins** over every mod outline, unless the player's kiln.json says
  `"ui": {"outlines": "mods-first"}`. `"off"` turns mod outlines off: requests are accepted and wait
  (`SwitchedOff`). The default is `"game-first"` ([client.md](client.md#kilnjson-per-side)). Another tool's highlight
  (Greeneye's Quick Deposit and Find, The Worst Map Mod) always shows instead, `"mods-first"` included: Kiln never
  takes a group it did not set, other than the building cursor's.
- **Through walls is Kiln's own silhouette.** The game draws its highlight with the model, so what stands in front of
  the entity hides it. `ThroughWalls` (0.2.0-dev.30.5) adds a screen-space silhouette instead: kiln.render v3 reads the
  entity's position (its RenderTransform, as world labels do) for that request at each client step (`outline_info`'s
  position, `KILN_RENDER_OUTLINE_THROUGH_WALLS`), Kiln.Ui projects a box at it and draws the hull over everything. It is a
  box, not the model's shape (a mesh-shaped silhouette needs a pass inside the game's renderer: not planned), and it shows
  where the model is in view too, next to the game's highlight. Nothing is drawn while the camera or the position is not
  known, or a corner is behind the camera.
- **Many at once.** Kiln reads up to 64 entities' models per client step, so a batch of hundreds lights up over a few
  steps. A model that is remade (or hidden) is noticed at the next step; one that appears later (streamed in, shown
  again) is found within 16 steps.
- **Models only.** Entities drawn through `ModelComponent` can be outlined: props, chests, stations, NPCs, creatures,
  dropped items. Voxel models and the terrain cannot (they wait with `NoModel`).
- **Client only.** On a dedicated server `OutlinesAvailable` is false. Nothing is installed for a player whose mods
  never ask: the first request puts kiln.render's hooks in, and they pass the game's colours through again once no
  request is left.
- Tests: the test kit fakes kiln.render's outlines ([modding/unit-tests.md](modding/unit-tests.md#outlines)).

### C table
Appended to `kiln.render` in v2; check `header.size >= offsetof(KilnRenderApi, outline_status) + sizeof(void*)`.
Every function works on any thread.

| Function | What |
|---|---|
| `outline(owner, entity, opts, out)` | a new request; `opts` is `KilnRenderOutline` (`size`, `rgba` 0xRRGGBBAA, `priority`, `flags` `KILN_RENDER_OUTLINE_PULSE`, `duration_ms` 0 = until removed, `reserved` 0). `KILN_ERR_UNAVAILABLE` (a server, the feature off, the mechanism failed), `KILN_ERR_INVALID_ARGUMENT`, `KILN_ERR_BUFFER_TOO_SMALL` (1024 live requests per owner) |
| `outline_info(handle, out)` | `KilnRenderOutlineInfo`: `state` (`KILN_RENDER_OUTLINE_WAITING`, `_SHOWN`, `_GAME`, `_ENDED`), `reason`, `entity`, `group` (4 or 5, 0 when none), `merged`. Also after the request ended, until the owner's next `outline_clear` |
| `outline_remove(handle)` | ends one request |
| `outline_clear(owner)` | ends every request of the owner; call it when your plugin unloads |
| `outline_status(out)` | `KilnRenderOutlineStatus`: available, hooks in, the player's setting, counts, and `detail` (why it is off, in words) |
```c
const KilnRenderApi* r = api->service(S(KILN_RENDER_SERVICE), 1);
if (r && r->header.size >= offsetof(KilnRenderApi, outline_status) + sizeof(void*)) {
    KilnRenderOutline o = {sizeof o, 0x6ED26EFF, 0, KILN_RENDER_OUTLINE_PULSE, 8000, 0};   /* green, 8 s */
    KilnHandle h = 0;
    if (r->outline(owner, chest, &o, &h) == KILN_OK) {
        KilnRenderOutlineInfo info = {sizeof info};
        r->outline_info(h, &info);   /* info.state, info.reason */
    }
}
/* at unload */
r->outline_clear(owner);
```
