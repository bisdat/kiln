// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// 04-markers (dossier chapter 04 §6; dev.31 out/04-markers/CLIENT-CHECKS.md): Kiln.Markers on the game's map, Kiln's
// compass strip, world labels, following an entity and server markers through the hosted loopback. Rows: markers.map
// (04-m15, 04-markers.capacity_restore), markers.compass and markers.world (04-m07), markers.nameplates and the anchors
// (04-m05, 04-m06), markers.send and the built-in receiver (04-m08 hosted half; the joined and vanilla halves, 04-m09 to
// 04-m12, are guided steps of CLIENT-CHECKS.md with mods/tests/markers-send-test). Every check removes its markers at its
// end (Markers.Clear / ClearSent). Orders 3100-3119.
public sealed partial class ClientPassMod
{
    private const string MarkersArea = "Map markers and labels";

    /// <summary>Skips the check (the player cannot pass it) when a row it tests is off here, naming why.</summary>
    private static async Task<bool> NeedOn(CheckContext c, params string[] capabilities)
    {
        var off = capabilities.Where(x => !Capabilities.Is(x)).ToArray();
        if (off.Length == 0) return true;
        c.Log.Info($"client-pass: {c.Id}: {off[0]} is off here: {Capabilities.Why(off[0])}");
        await c.Skip($"{string.Join(", ", off)} off: {Capabilities.Why(off[0])}");
        return false;
    }

    /// <summary>The ground about where the character stands (the third-person camera's pivot is about its head).</summary>
    private static WorldPosition Feet() => Render.Camera.Pivot with { Y = Render.Camera.Pivot.Y - 1.6 };

    [ClientCheck("04-m07", "Kiln's compass strip and a world label: a point 50 m east, facing north, east and west", Area = MarkersArea,
        Since = Dev31, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3100)]
    public async Task MarkersCompass(CheckContext c)
    {
        if (!await NeedOn(c, "markers.compass", "markers.world")) return;
        c.OnEnd(() => Markers.Clear());
        var at = Feet();
        var east = new WorldPosition(at.X + 50, at.Y, at.Z);
        var changes = 0;
        var seen = new List<MarkerVisibilityChangedEvent>();
        Action<MarkerChangedEvent> onChange = _ => Interlocked.Increment(ref changes);
        Action<MarkerVisibilityChangedEvent> onVis = e => { lock (seen) seen.Add(e); c.Log.Info($"04-m07: {e.Id} {e.After} (compass {e.Compass}, world {e.World})"); };
        Markers.Changed += onChange;
        Markers.VisibilityChanged += onVis;
        c.OnEnd(() => { Markers.Changed -= onChange; Markers.VisibilityChanged -= onVis; });
        var a = Markers.Set(new MarkerSpec("m07-east", east, "East 50 m", MarkerIcon.None, MarkerSurface.Compass, Rgba: 0xff5050ff));
        var b = Markers.Set(new MarkerSpec("m07-label", east, "East label", MarkerIcon.None, MarkerSurface.World, Height: 2, Rgba: 0x50c0ffff));
        c.Expect(a.Ok && b.Ok, "both markers stored", $"{a.Reason} {b.Reason}");
        c.Log.Info($"04-m07: camera at {Render.Camera.Position}, the point at {east}");
        c.Expect(await c.Until(() => Volatile.Read(ref changes) >= 2, TimeSpan.FromSeconds(5), "two Added"), "two Added events");
        c.Instruct("Face NORTH (open the map with M to check, then close it). A red diamond 'East 50 m' with its distance " +
                   "should sit on the strip at the top of the screen, a quarter of the strip RIGHT of its centre; a blue " +
                   "'East label' diamond floats over the point to your right. Is that what you see?");
        await c.Verdict();
        c.Instruct("Turn to face EAST. Is the red diamond now in the MIDDLE of the strip, and the blue label in front of you, " +
                   "50 m away?");
        await c.Verdict();
        var before = Volatile.Read(ref changes);
        c.Instruct("Turn to face WEST (your back to the point). Is the red diamond at the LEFT end of the strip (the opposite " +
                   "bearing), and is the blue label gone (it is behind you)?");
        await c.Verdict();
        c.Expect(Volatile.Read(ref changes) == before, "turning raised no logical change", $"{Volatile.Read(ref changes) - before} changes");
        lock (seen)
            c.Expect(seen.Any(e => e.Id == "m07-label" && e.World == MarkerVisibilityReason.BehindCamera),
                "the label's World surface went BehindCamera while the compass kept it");
        c.Instruct("Open the inventory, then the Kiln menu, then the game's map. Do the strip and the label disappear while a " +
                   "menu or the map is open, and come back when you close it?");
        await c.Verdict();
    }

    [ClientCheck("04-m15", "Native map icons: two of the game's icons near you, hover and click are harmless, then they go",
        Area = MarkersArea, Since = Dev31, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3101)]
    public async Task MarkersMapIcons(CheckContext c)
    {
        if (!await NeedOn(c, "markers.map", "markers.catalog")) return;
        c.OnEnd(() => Markers.Clear());
        c.Expect(await c.Until(() => Markers.Icons.IsReady, TimeSpan.FromSeconds(30), "the icon catalog"), "the icon catalog is ready");
        var usable = Markers.Icons.All.Where(i => i.Surfaces.HasFlag(MarkerSurface.Map)).ToList();
        var travel = Markers.Icons.All.FirstOrDefault(i => i.Surfaces == 0);
        c.Log.Info($"04-m15: {Markers.Icons.All.Count} icons, {usable.Count} for the map; fast travel refused: {travel?.Alias}");
        c.Expect(Markers.Icons.All.Count == 43 && usable.Count == 37, "43 icons, 37 for the map", $"{Markers.Icons.All.Count}, {usable.Count}");
        if (travel is not null)
        {
            var refused = false;
            try { Markers.Set(new MarkerSpec("m15-travel", Feet(), "", travel.Icon, MarkerSurface.Map)); }
            catch (ArgumentException) { refused = true; }
            c.Expect(refused, "a fast-travel icon is refused on the map", travel.Alias);
        }
        var camp = Markers.Icons.Resolve("special_enemy_camp");
        var well = Markers.Icons.Resolve("well_plant");
        var first = camp.Ok ? camp.Icon! : usable[0];
        var second = well.Ok ? well.Icon! : usable[1];
        var at = Feet();
        var r1 = Markers.Set(new MarkerSpec("m15-a", new WorldPosition(at.X + 40, at.Y, at.Z), "A", first.Icon, MarkerSurface.Map));
        var r2 = Markers.Set(new MarkerSpec("m15-b", new WorldPosition(at.X - 40, at.Y, at.Z), "B", second.Icon, MarkerSurface.Map));
        c.Expect(r1.Ok && r2.Ok, "both map markers stored", $"{r1.Reason} {r2.Reason}");
        c.Log.Info($"04-m15: {first.Alias} 40 m east, {second.Alias} 40 m west of {at}");
        c.Instruct($"Open the map (M) and find your position. There should be a {first.Alias.Replace('_', ' ')} icon 40 m EAST " +
                   $"of you and a {second.Alias.Replace('_', ' ')} icon 40 m WEST (zoom in if the map hides small icons). Are both there?");
        await c.Verdict();
        c.Instruct("Hover each icon with the cursor, then left-click and right-click each. Nothing should happen beyond what an " +
                   "ordinary map marker does: no teleport, no travel prompt, no error, no crash, your own pins unchanged. Right?");
        await c.Verdict();
        Markers.Clear();
        c.Instruct("Keep the map open: both icons should disappear within 3 seconds. Did they?");
        await c.Verdict();
    }

    [ClientCheck("04-markers.capacity_restore", "A map marker that lost its place comes back when there is room", Area = MarkersArea,
        Since = Dev31, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3102)]
    public async Task MarkersCapacityRestore(CheckContext c)
    {
        // 128 local map markers fill Kiln's map budget; one more arrives as a received marker (this mod sends it from the
        // hosted server world through the loopback), ranks after the local ones and loses its place (SurfaceBudget).
        // Removing one local marker gives it the place back without a logical change.
        if (!await NeedOn(c, "markers.map", "markers.send")) return;
        c.OnEnd(() =>
        {
            Markers.Clear();
            Markers.ClearSent();
        });
        c.Expect(await c.Until(() => Markers.Icons.IsReady, TimeSpan.FromSeconds(30), "the icon catalog"), "the icon catalog is ready");
        var icon = Markers.Icons.Resolve("special_enemy_camp") is { Ok: true } r ? r.Icon!.Icon : Markers.Icons.All.First(i => i.Surfaces != 0).Icon;
        var at = Feet();
        for (var i = 0; i < 128; i++)
        {
            var ring = new WorldPosition(at.X + 200 + (i % 16) * 6, at.Y, at.Z + 60 + i / 16 * 6);
            var s = Markers.Set(new MarkerSpec($"cap{i:000}", ring, "", icon, MarkerSurface.Map));
            if (!s.Ok) c.Log.Info($"capacity_restore: cap{i:000}: {s.Reason}");
        }
        var loser = new WorldPosition(at.X + 40, at.Y, at.Z);
        MarkerVisibilityChangedEvent? last = null;
        Action<MarkerVisibilityChangedEvent> onVis = e =>
        {
            if (e.Id != "cap-loser") return;
            Volatile.Write(ref last, e);
            c.Log.Info($"capacity_restore: loser {e.After} (map {e.Map}){(e.Initial ? " initial" : "")}");
        };
        var changes = 0;
        Action<MarkerChangedEvent> onChange = e => { if (e.Id == "cap-loser" && e.Audience == MarkerSource.Received) Interlocked.Increment(ref changes); };
        Markers.VisibilityChanged += onVis;
        Markers.Changed += onChange;
        c.OnEnd(() => { Markers.VisibilityChanged -= onVis; Markers.Changed -= onChange; });
        var sent = Markers.Send(EntityId.None, new MarkerSpec("cap-loser", loser, "", icon, MarkerSurface.Map));
        c.Log.Info($"capacity_restore: sent {sent}");
        c.Expect(sent.Committed, "the server stored the broadcast marker", sent.Reason);
        c.Expect(await c.Until(() => Volatile.Read(ref last) is { Map: MarkerVisibilityReason.SurfaceBudget or MarkerVisibilityReason.MapCapacity },
            TimeSpan.FromSeconds(10), "the received marker refused for room"), "the 129th marker has no room (SurfaceBudget or MapCapacity)");
        c.Instruct("Open the map (M) at your position. There should be a block of 128 camp icons north-east of you and NO camp " +
                   "icon 40 m east of you. Is that so? Keep the map open.");
        await c.Verdict();
        var changesBefore = Volatile.Read(ref changes);
        Markers.Remove("cap000");
        c.Expect(await c.Until(() => Volatile.Read(ref last) is { Map: MarkerVisibilityReason.Ready }, TimeSpan.FromSeconds(3), "recovery"),
            "the received marker got its place back within 3 s");
        c.Expect(Volatile.Read(ref changes) == changesBefore, "no logical change for the recovered marker");
        c.Instruct("One icon of the block is gone. Did a camp icon appear 40 m east of you within 3 seconds, while your own pins stayed?");
        await c.Verdict();
    }

    [ClientCheck("04-m05", "A label follows: over your own character, and over a pickup the server follows until it is gone",
        Area = MarkersArea, Since = Dev31, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3103)]
    public async Task MarkersFollow(CheckContext c)
    {
        if (!await NeedOn(c, "markers.nameplates", "markers.anchors.server", "markers.send")) return;
        c.OnEnd(() =>
        {
            Markers.Clear();
            Markers.ClearSent();
        });
        var changes = new List<MarkerChangedEvent>();
        Action<MarkerChangedEvent> onChange = e => { lock (changes) changes.Add(e); c.Log.Info($"04-m05: {e.Kind} {e.Audience} {e.Id}"); };
        Action<MarkerVisibilityChangedEvent> onVis = e => c.Log.Info($"04-m05: {e.Id} {e.After} (world {e.World})");
        Markers.Changed += onChange;
        Markers.VisibilityChanged += onVis;
        c.OnEnd(() => { Markers.Changed -= onChange; Markers.VisibilityChanged -= onVis; });

        // 1. a nameplate over the local player (its id is the same in the hosted client and server worlds)
        var plate = await Markers.SetNameplate("m05-me", Client.LocalPlayer, "This is you");
        c.Expect(plate.Ok, "the nameplate is set", plate.Reason);
        c.Instruct("A label 'This is you' should float over your character. Walk, run and jump around for a few seconds: does it " +
                   "stay over your character and move with it, smoothly?");
        await c.Verdict();
        int updates;
        lock (changes) updates = changes.Count(e => e.Id == "m05-me" && e.Kind == MarkerChangeKind.Updated);
        c.Expect(updates == 0, "moving raised no logical Updated", $"{updates}");

        // 2. a marker the server follows: a pickup in front of you, sent to you with the server's anchor
        var at = Feet();
        var pickup = await Spawn.Entity("Base_LootPickup_Generic_Gravity", new WorldPosition(at.X + 3, at.Y + 1.5, at.Z));
        c.OnEnd(() => Spawn.Destroy(pickup));
        var anchor = Markers.CaptureServerAnchor(pickup);
        c.Expect(anchor.Ok, "the server captured the pickup", anchor.Reason);
        if (!anchor.Ok) { await c.Verdict(); return; }
        var spec = new MarkerSpec("m05-pickup", default, "Pickup", MarkerIcon.None, MarkerSurface.World | MarkerSurface.Compass,
            Follow: anchor.Anchor, Height: 1, Rgba: 0xffd040ff);
        var sent = Markers.Send(c.Player, spec);
        c.Log.Info($"04-m05: sent {sent}");
        c.Instruct("A yellow 'Pickup' label should be over the loot pickup 3 m in front of you (it may drop to the ground: the " +
                   "label goes with it). Is it there, over the pickup?");
        await c.Verdict();
        Spawn.Destroy(pickup);
        c.Instruct("The pickup was destroyed. Did its label disappear within a second (and not move to anything else)?");
        await c.Verdict();
        lock (changes)
            c.Expect(!changes.Any(e => e.Id == "m05-pickup" && e.Kind == MarkerChangeKind.Removed), "the marker was kept (AnchorEnded, no Removed)");
        var again = await Spawn.Entity("Base_LootPickup_Generic_Gravity", new WorldPosition(at.X - 3, at.Y + 1.5, at.Z));
        c.OnEnd(() => Spawn.Destroy(again));
        var second = Markers.CaptureServerAnchor(again);
        c.Expect(second.Ok, "the server captured the new pickup", second.Reason);
        if (second.Ok) Markers.Send(c.Player, spec with { Follow = second.Anchor });
        c.Instruct("A new pickup appeared 3 m BEHIND you. Did the 'Pickup' label come back over THAT one?");
        await c.Verdict();
    }

    [ClientCheck("04-m06", "A captured entity's id reused: the old anchor never follows the new entity", Area = MarkersArea,
        Since = Dev31, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3104)]
    public async Task MarkersReusedIds(CheckContext c)
    {
        if (!await NeedOn(c, "markers.nameplates", "markers.anchors.client")) return;
        c.OnEnd(() => Markers.Clear());
        var at = Feet();
        var spot = new WorldPosition(at.X + 3, at.Y + 1.5, at.Z);
        var first = await Spawn.Entity("Base_LootPickup_Generic_Gravity", spot);
        c.OnEnd(() => Spawn.Destroy(first));
        await c.Ticks(10);  // let the client world have it
        var lease = await Markers.CaptureAnchorAsync(first);
        c.Log.Info($"04-m06: server entity {first}, client capture {lease}");
        c.Expect(lease.Ok, "the client captured the pickup by its id", lease.Reason);
        if (!lease.Ok) { await c.Verdict(); return; }
        c.Expect(lease.Anchor!.Entity == first, "the anchor names that entity");
        var set = Markers.Set(new MarkerSpec("m06", default, "First", MarkerIcon.None, MarkerSurface.World, Follow: lease.Anchor, Height: 1));
        lease.Anchor.Dispose();
        c.Expect(set.Ok, "the follow marker is stored", set.Reason);
        c.Instruct("A label 'First' should be over the loot pickup 3 m in front of you. Is it?");
        await c.Verdict();
        Spawn.Destroy(first);
        var second = await Spawn.Entity("Base_LootPickup_Generic_Gravity", spot);
        c.OnEnd(() => Spawn.Destroy(second));
        c.Log.Info($"04-m06: destroyed {first}, spawned {second} at the same spot ({(second == first ? "the same id" : "another id")})");
        c.Instruct("The pickup was destroyed and a new one spawned at the same spot. The label 'First' must NOT be over the new " +
                   "pickup (it belonged to the old one) and must not appear anywhere else. Is it gone?");
        await c.Verdict();
    }

    [ClientCheck("04-m08", "Server markers through the hosted loopback: they arrive once, as received markers", Area = MarkersArea,
        Since = Dev31, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3105)]
    public async Task MarkersLoopback(CheckContext c)
    {
        if (!await NeedOn(c, "markers.send", "markers.compass", "markers.world")) return;
        c.OnEnd(() => Markers.ClearSent());
        var received = new List<MarkerChangedEvent>();
        Action<MarkerChangedEvent> onChange = e =>
        {
            if (e.Audience != MarkerSource.Received) return;
            lock (received) received.Add(e);
            c.Log.Info($"04-m08: received {e.Kind} {e.Owner}/{e.Id} peer {e.PeerGeneration}");
        };
        Markers.Changed += onChange;
        c.OnEnd(() => Markers.Changed -= onChange);
        var at = Feet();
        var r = Markers.Send(EntityId.None, new MarkerSpec("m08-broadcast", new WorldPosition(at.X, at.Y, at.Z + 30), "From the server",
            MarkerIcon.None, MarkerSurface.Compass | MarkerSurface.World, Height: 2, Rgba: 0x60ff60ff));
        c.Log.Info($"04-m08: broadcast {r}");
        c.Expect(r.Committed, "the server stored it", r.Reason);
        c.Expect(await c.Until(() => { lock (received) return received.Count > 0; }, TimeSpan.FromSeconds(10), "the received Added"),
            "it arrived as a received marker");
        await c.Delay(TimeSpan.FromSeconds(3));
        lock (received)
            c.Expect(received.Count(e => e.Id == "m08-broadcast" && e.Kind == MarkerChangeKind.Added) == 1, "it arrived exactly once (counted once)",
                $"{received.Count} received changes");
        c.Instruct("A green 'From the server' label should float 30 m NORTH of where you stood, and a green diamond be on the " +
                   "compass strip. Is there exactly one of each (not two on top of each other)?");
        await c.Verdict();
        Markers.ClearSent();
        c.Instruct("The server cleared its markers. Did the green label and diamond disappear within 3 seconds?");
        await c.Verdict();
    }

    [ClientCheck("04-m09", "Server markers for everyone and for one player: two players with Kiln see their own layers",
        Area = MarkersArea, Since = Dev31, Tags = [TwoPlayers, Visual], Needs = ClientCheckNeeds.Joined, Order = 3106)]
    public async Task MarkersLayers(CheckContext c)
    {
        // The body runs on the server. P = the player who runs the check, Q = the other player with Kiln (both in sight
        // of the spot). Broadcast A, a target override B for P, then back.
        if (!await NeedOn(c, "markers.send")) return;
        c.OnEnd(() => Markers.ClearSent());
        var at = Players.PositionOf(c.Player) ?? new WorldPosition(0, 0, 0);
        var spot = new WorldPosition(at.X, at.Y, at.Z + 20);
        var a = new MarkerSpec("m09", spot, "Layer A (everyone)", MarkerIcon.None, MarkerSurface.World | MarkerSurface.Compass, Height: 2, Rgba: 0x60ff60ff);
        var b = a with { Text = "Layer B (only P)", Rgba = 0xff60ffff };
        void Step(string what, MarkerResult r) => c.Log.Info($"04-m09: {what}: {r}");
        Step("broadcast A", Markers.Send(EntityId.None, a));
        c.Instruct("Both players: look 20 m NORTH of P (who runs this check). P and Q should both see a green 'Layer A (everyone)' " +
                   "label and a green diamond on the compass strip. Do both see it?");
        await c.Verdict();
        Step("target B for P", Markers.Send(c.Player, b));
        c.Instruct("P should now see a pink 'Layer B (only P)' instead; Q still the green 'Layer A'. Is it so on both screens?");
        await c.Verdict();
        Step("unsend P", Markers.Unsend(c.Player, "m09"));
        c.Instruct("P sees the green 'Layer A' again, Q unchanged. Right?");
        await c.Verdict();
        Step("suppress for P", Markers.SuppressSent(c.Player, "m09"));
        c.Instruct("P sees NO label there now, Q still sees the green one. Right?");
        await c.Verdict();
        Step("clear P's layer", Markers.ClearSent(c.Player));
        c.Instruct("P sees the green label again. Right?");
        await c.Verdict();
        Step("clear all", Markers.ClearSent());
        c.Instruct("The server cleared everything: within 3 seconds, has the label gone on both screens?");
        await c.Verdict();
    }

    [ClientCheck("04-m10", "A player without Kiln gets no server markers and is counted as refused", Area = MarkersArea,
        Since = Dev31, Tags = [VanillaClient, Visual], Needs = ClientCheckNeeds.Joined, Order = 3107)]
    public async Task MarkersVanilla(CheckContext c)
    {
        // The body runs on the server. P (Kiln) runs the check; V (no Kiln at all) is connected and stands near P.
        if (!await NeedOn(c, "markers.send")) return;
        c.OnEnd(() => Markers.ClearSent());
        var at = Players.PositionOf(c.Player) ?? new WorldPosition(0, 0, 0);
        var r = Markers.Send(EntityId.None, new MarkerSpec("m10", new WorldPosition(at.X, at.Y, at.Z + 20), "Kiln players only",
            MarkerIcon.None, MarkerSurface.World | MarkerSurface.Compass, Height: 2));
        c.Log.Info($"04-m10: broadcast with a vanilla player connected: {r}");
        c.Expect(r.Committed && r.Queued >= 1 && r.Refused >= 1 && !r.Ok, "stored, queued for P, the vanilla player refused (Partial)",
            $"{r}");
        var again = Markers.Send(EntityId.None, new MarkerSpec("m10", new WorldPosition(at.X, at.Y, at.Z + 20), "Kiln players only",
            MarkerIcon.None, MarkerSurface.World | MarkerSurface.Compass, Height: 2));
        c.Log.Info($"04-m10: identical retry: {again}");
        c.Expect(again.Committed && again.Affected == 0, "an identical retry changes nothing", $"{again}");
        c.Instruct("P (Kiln) should see a 'Kiln players only' label 20 m north of P. V (no Kiln) must see nothing new, no error " +
                   "message, no stutter, and must stay connected. Is that so on both screens?");
        await c.Verdict();
        c.Instruct("V: leave the world and join again (wait until you are back in). Is V back in without an error, still " +
                   "seeing nothing of it, while P still sees the label?");
        await c.Verdict();
    }
}
