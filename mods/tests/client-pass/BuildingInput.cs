// SPDX-License-Identifier: MIT
using System.Numerics;
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// kiln.client's building input (Kiln.BuildingInput; docs/kiln/services/client-building-input.md): the five guided checks
// that the client gate's row client.building_input names (native/core/gates/client-input.json). Passing them all is what
// lets the row be removed. They run with kiln.json "preview" naming client.building_input (or true). Each submits through
// BuildingInput, follows the request to its end, asserts what kiln.client answers, and asks one verdict about what the
// world did. A joined session runs a check's body on the server, so the joined check's input comes from the joining game
// client itself: a key of this mod that exists only where building input is available (BuildingInputKey below).
// Orders 3950-3954.
public sealed partial class ClientPassMod
{
    private const string BuildingSince = "0.2.0-dev.30.3";
    private const string BuildingArea = "Building input";
    private const string RoughWoodBlock = "Block_T1_Wood_Rough_CRAFTING";
    private const string BuildingKey = "Ctrl+Shift+F7";

    /// <summary>True when building input works in this game. Otherwise says how to switch it on, asks for skip and returns
    /// false: a game without the preview is not a failure of what the check covers.</summary>
    private static async Task<bool> NeedBuildingInput(CheckContext c)
    {
        if (BuildingInput.Available) return true;
        var why = Capabilities.Why(BuildingInput.Capability);
        c.Log.Info($"client-pass: {c.Id} needs building input: {why}");
        c.Instruct($"Building input is off in this game ({why}). Add \"preview\": [\"client.building_input\"] to " +
                   "kiln/kiln.json in the game folder (or \"preview\": true), start the game again and run this check there.");
        await c.Skip($"client.building_input is off: {why}");
        return false;
    }

    /// <summary>Where the checks build: 2.5 m ahead of the camera's pivot, level, at the pivot's height less 1 m; the
    /// rotation faces the way the camera looks. Null when the camera is not known.</summary>
    private static (WorldPosition At, Quaternion Rotation)? BuildSpot(double right = 0)
    {
        if (!Render.CameraAvailable) return null;
        var cam = Render.Camera;
        if (!cam.Valid) return null;
        var f = cam.Forward;
        var flat = new Vector3(f.X, 0, f.Z);
        flat = flat.LengthSquared() < 1e-6f ? Vector3.UnitZ : Vector3.Normalize(flat);
        var side = new Vector3(flat.Z, 0, -flat.X);  // to the right of the flat forward (x east, z north, y up)
        var p = cam.Pivot;
        var at = new WorldPosition(p.X + flat.X * 2.5 + side.X * right, p.Y - 1.0, p.Z + flat.Z * 2.5 + side.Z * right);
        var yaw = MathF.Atan2(flat.X, flat.Z);
        return (at, Quaternion.CreateFromAxisAngle(Vector3.UnitY, yaw));
    }

    /// <summary>Follows a submitted request to its end (at most 8 s) and asserts it was dispatched. Returns the last
    /// state.</summary>
    private static async Task<BuildingInputState> Settle(CheckContext c, BuildingInputResult r, string what)
    {
        if (!c.Expect(r.Ok && r.Request is not null, $"{what}: queued", r.Reason ?? r.Refusal.ToString())) return BuildingInputState.Unknown;
        var q = r.Request!;
        var state = q.State;
        await c.Until(() => (state = q.State) is not (BuildingInputState.Queued or BuildingInputState.Pressed),
            TimeSpan.FromSeconds(8), $"{what} ends");
        c.Expect(state == BuildingInputState.Dispatched, $"{what}: dispatched as your own input", state.ToString());
        c.Log.Info($"client-pass: {c.Id}: {q} ended {state}");
        return state;
    }

    [ClientCheck("building-input.select", "Kiln selects a building item for you as if you picked it in the build menu",
        Area = BuildingArea, Since = BuildingSince, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3950)]
    public async Task BuildingInputSelect(CheckContext c)
    {
        if (!await NeedPlayer(c) || !await NeedBuildingInput(c)) return;
        var wood = Items.Lookup(RoughWoodBlock);
        if (!c.Expect(wood is not null, $"the game has {RoughWoodBlock}")) { await c.Verdict(); return; }
        c.Instruct("Take the building hammer and open the building mode, with any building item but Rough Wood Block " +
                   "selected. In 8 seconds Kiln selects Rough Wood Block for your first actionbar slot, as if you picked it " +
                   "in the build menu.");
        await c.Delay(TimeSpan.FromSeconds(8));
        await Settle(c, BuildingInput.Select(wood!.Id, 0), "select Rough Wood Block");
        c.Instruct("Pass only if all of these held: (1) Rough Wood Block became your selected building item, (2) the building " +
                   "preview shows it, (3) nothing was built and nothing else changed. Otherwise fail with a note.");
        await c.Verdict();
    }

    [ClientCheck("building-input.place-remove", "Kiln places one block in front of you, then removes it, as your own input",
        Area = BuildingArea, Since = BuildingSince, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3951)]
    public async Task BuildingInputPlaceRemove(CheckContext c)
    {
        if (!await NeedPlayer(c) || !await NeedBuildingInput(c)) return;
        c.Instruct("Use a copied world. Stand on open, flat ground where you may build (your base), in the building mode with " +
                   "Rough Wood Block selected and some Rough Wood in your inventory. Look straight ahead and keep still: in 8 " +
                   "seconds Kiln places one block about 2.5 m in front of you, then removes it 4 seconds later.");
        await c.Delay(TimeSpan.FromSeconds(8));
        if (!c.Expect(BuildSpot() is not null, "the camera is known (where to build)")) { await c.Verdict(); return; }
        var (at, rotation) = BuildSpot()!.Value;
        c.Log.Info($"client-pass: {c.Id}: building at {at}");
        await Settle(c, BuildingInput.Place(at, rotation), "place at " + at);
        await c.Delay(TimeSpan.FromSeconds(4));
        await Settle(c, BuildingInput.Remove(at, rotation), "remove at " + at);
        c.Instruct("Pass only if all of these held: (1) one Rough Wood Block appeared about 2.5 m in front of you, (2) it went " +
                   "away again 4 seconds later as with your own remove action (the wood came back), (3) nothing else was built " +
                   "or removed. Otherwise fail with a note saying what you saw.");
        await c.Verdict();
    }

    [ClientCheck("building-input.undo-dismantle", "Kiln's undo takes back your last block; Kiln dismantles one exact prop",
        Area = BuildingArea, Since = BuildingSince, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3952)]
    public async Task BuildingInputUndoDismantle(CheckContext c)
    {
        if (!await NeedPlayer(c) || !await NeedBuildingInput(c)) return;
        List<EntityId> Near() => World.Position(c.Player) is { } here
            ? World.Find("UsedItem").Where(e => World.Position(e) is { } p && p.DistanceTo(here) < 6).ToList()
            : [];
        c.Instruct("Step 1 of 2. In your base, in the building mode, place one block yourself now. 10 seconds later Kiln " +
                   "presses the game's building undo: the block you placed last goes away.");
        await c.Delay(TimeSpan.FromSeconds(10));
        await Settle(c, BuildingInput.Undo(), "undo");
        var before = Near().ToHashSet();
        c.Instruct("Step 2 of 2. Now place one small prop (a chair, a torch or a chest) within 5 m of you. The check finds it " +
                   "and Kiln dismantles exactly that prop, as your own dismantle action held for 1.2 seconds.");
        EntityId prop = EntityId.None;
        await c.Until(() => !(prop = Near().FirstOrDefault(e => !before.Contains(e))).IsNone, TimeSpan.FromMinutes(3),
            "a new prop within 5 m of you");
        if (c.Expect(!prop.IsNone && World.Transform(prop) is not null, "the new prop and its place are known",
                prop.ToString()))
        {
            var t = World.Transform(prop)!.Value;
            await c.Delay(TimeSpan.FromSeconds(2));
            await Settle(c, BuildingInput.Dismantle(prop, t.Position, t.Rotation), $"dismantle prop {prop}");
            await c.Delay(TimeSpan.FromSeconds(2));
            c.Expect(!World.Exists(prop), "the prop is gone from the world", prop.ToString());
        }
        c.Instruct("Pass only if all of these held: (1) the undo took back the block you placed last, and nothing else, (2) " +
                   "the prop you placed was dismantled (it went back to your inventory as with your own dismantle), (3) no " +
                   "other prop or block near you changed. Otherwise fail with a note.");
        await c.Verdict();
    }

    [ClientCheck("building-input.own-input", "Nothing is emitted while your own building action is held",
        Area = BuildingArea, Since = BuildingSince, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3953)]
    public async Task BuildingInputOwnInput(CheckContext c)
    {
        if (!await NeedPlayer(c) || !await NeedBuildingInput(c)) return;
        c.Instruct("In your base, in the building mode with Rough Wood Block selected and Rough Wood in your inventory, look " +
                   "at the ground and hold your own build button down (left mouse) now, and keep holding it until the list " +
                   "says to let go (about 10 seconds). Kiln asks for one block 1.5 m to the right of where you look, but must " +
                   "wait while your own building action is held.");
        await c.Delay(TimeSpan.FromSeconds(5));
        if (!c.Expect(BuildSpot(1.5) is not null, "the camera is known (where to build)")) { await c.Verdict(); return; }
        var (at, rotation) = BuildSpot(1.5)!.Value;
        var r = BuildingInput.Place(at, rotation);
        if (c.Expect(r.Ok && r.Request is not null, "place: queued", r.Reason ?? r.Refusal.ToString()))
        {
            var q = r.Request!;
            var held = true;
            for (var i = 0; i < 12; i++)
            {
                held &= q.State == BuildingInputState.Queued;
                await c.Delay(TimeSpan.FromMilliseconds(250));
            }
            c.Expect(held, "while your build button was held the request stayed queued (nothing emitted)", q.State.ToString());
            c.Instruct("Let go of the build button now.");
            var state = q.State;
            await c.Until(() => (state = q.State) is not (BuildingInputState.Queued or BuildingInputState.Pressed),
                TimeSpan.FromSeconds(8), "the request ends after you let go");
            c.Expect(state == BuildingInputState.Dispatched, "after you let go it was dispatched", state.ToString());
        }
        c.Instruct("Pass only if all of these held: (1) while you held the button, only your own blocks were built (Kiln built " +
                   "nothing to the right), (2) after you let go, one block appeared 1.5 m to the right of where you looked, " +
                   "(3) your own building worked normally throughout. Otherwise fail with a note.");
        await c.Verdict();
    }

    [ClientCheck("building-input.joined", "A joined game client's building input: Kiln places and removes one block on the server",
        Area = BuildingArea, Since = BuildingSince, Tags = [Visual], Needs = ClientCheckNeeds.Joined, Order = 3954)]
    public async Task BuildingInputJoined(CheckContext c)
    {
        c.Instruct("Your game client needs \"preview\" naming client.building_input in its own kiln/kiln.json (then this " +
                   $"mod gives it the key {BuildingKey}). On this server, stand where you may build, in the building mode with " +
                   $"Rough Wood Block selected and Rough Wood in your inventory. Look straight ahead and press {BuildingKey}: your " +
                   "game places one block about 2.5 m in front of you through Kiln's building input, removes it 4 seconds " +
                   "later, and writes each step to its own kiln/logs/kiln.log as 'client-pass: BUILDING-INPUT-JOINED'.");
        c.Instruct("Pass only if all of these held: (1) the block appeared on your screen and the server kept it (another player " +
                   "or the server's own view sees it; it did not vanish at once), (2) it was removed 4 seconds later, (3) your " +
                   "kiln.log shows 'place ... Dispatched' and 'remove ... Dispatched', (4) nothing else was built or removed. " +
                   "Otherwise fail with a note and your kiln.log lines.");
        await c.Verdict();
    }

    // building-input.joined: on a game client where building input is available, the key places one block 2.5 m ahead and
    // removes it 4 s later, in a joined session only (hosting has the checks above). Started at load; a game without the
    // preview (or a dedicated server) never gets the key.
    private bool _buildingKeyStop;

    private async Task BuildingInputKey()
    {
        if (!Client.IsClient || !BuildingInput.Available) return;
        using var key = KeyBindings.Register("client-pass.building-input", BuildingKey, null,
            "guided check building-input.joined: place one block ahead, remove it 4 s later", "Client pass");
        while (!_buildingKeyStop)
        {
            await Context.Scheduler.NextTick();
            if (key.TakePresses() == 0) continue;
            if (Client.State != ClientState.Remote)
            {
                Log.Info("client-pass: BUILDING-INPUT-JOINED: not in a joined game; hosting has the building-input checks");
                continue;
            }
            if (BuildSpot() is not { } spot)
            {
                Log.Info("client-pass: BUILDING-INPUT-JOINED: the camera is not known; nothing sent");
                continue;
            }
            await JoinedStep("place", () => BuildingInput.Place(spot.At, spot.Rotation), spot.At);
            await Task.Delay(TimeSpan.FromSeconds(4));
            await JoinedStep("remove", () => BuildingInput.Remove(spot.At, spot.Rotation), spot.At);
        }
    }

    private async Task JoinedStep(string what, Func<BuildingInputResult> submit, WorldPosition at)
    {
        BuildingInputResult r;
        try
        {
            r = submit();
        }
        catch (Exception e) when (e is KilnUnavailableException or ArgumentException or InvalidOperationException)
        {
            Log.Info($"client-pass: BUILDING-INPUT-JOINED {what} at {at}: refused ({e.GetType().Name}: {e.Message})");
            return;
        }
        if (!r.Ok || r.Request is not { } q)
        {
            Log.Info($"client-pass: BUILDING-INPUT-JOINED {what} at {at}: not queued ({r.Refusal}: {r.Reason})");
            return;
        }
        var state = q.State;
        for (var i = 0; i < 400 && state is (BuildingInputState.Queued or BuildingInputState.Pressed); i++)
        {
            await Context.Scheduler.NextTick();
            state = q.State;
        }
        Log.Info($"client-pass: BUILDING-INPUT-JOINED {what} at {at}: {q} ended {state}");
    }
}
