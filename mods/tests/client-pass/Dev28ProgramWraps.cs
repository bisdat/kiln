// SPDX-License-Identifier: MIT
// 06a-program-wraps: the guided check 06a-wraps.client. Program wraps on a game client are on since 0.2.0-dev.29
// (systems.wraps.client, held in dev.28) without a run in a real game; this check is the record of that. In a hosted game
// it wraps the client world's render_transform (a client program: no update in the server exe) and the hosted server
// world's is_actor_in_shape with a Before each that only counts, then asserts what the code can see: each wrap reached
// its own world's slices (the prediction world, which runs the client world's table, is forwarded untouched), the client
// world's slices ran on the game's workers, a third wrap with an archetype filter (WhenArchetype: the client world's
// entity map, read on the workers while entities come and go) decided its slices, no stage faulted or went over its
// budget, and the wraps are gone after Dispose. What only a player can judge is asked: that the game looked and ran as before while they were on. The joined
// half (a client joined to a dedicated server) is not here: a joined check's body runs on the server.
//
// 06a-wraps.waterworks: the product proof (chapter 06a 6, 9.13) in a hosted world with a second player in it, where the
// game itself runs the hosted world's entity programs as tasks on its workers. The check makes its own typed wrap of
// fuel_setup (Systems.Wrap<Ecs.Programs.FuelSetup>, the WaterWorks sample's wrap, x2) and asserts from what only that
// program's own slices can show: its slices ran on the game's workers (fails when the wrap never ran on one), the game's
// own fills were seen in fuel_setup's update and the doubled water is what GetComponent reads afterwards. The player
// judges what they see: a well's water bar refilling faster while the wrap is on.
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

public sealed partial class ClientPassMod
{
    private const int ProgramWrapsOrder = 2000 + (28060 - 28000) * 10;

    private sealed class WrapCount
    {
        public long Slices, Entities, OffWorldThread;
        public readonly ConcurrentDictionary<int, byte> Threads = new();

        public void Add(ref Slice s)
        {
            Interlocked.Increment(ref Slices);
            Interlocked.Add(ref Entities, s.Count);
            if (!s.IsWorldThread) Interlocked.Increment(ref OffWorldThread);
            Threads.TryAdd(Environment.CurrentManagedThreadId, 0);
        }

        public override string ToString() =>
            $"{Interlocked.Read(ref Slices)} slices, {Interlocked.Read(ref Entities)} entities, {Interlocked.Read(ref OffWorldThread)} " +
            $"off the world's thread, on {Threads.Count} thread(s)";
    }

    [ClientCheck("06a-wraps.client", "Program wraps in a game client: the client world and the hosted world", Area = "Systems",
        Since = Dev29, Needs = ClientCheckNeeds.Hosted, Order = ProgramWrapsOrder + 0)]
    public async Task ProgramWrapsClient(CheckContext c)
    {
        if (!await NeedOpen(c, "systems.wraps", "systems.wraps.client") || !await NeedPlayer(c)) return;
        foreach (var row in new[] { "systems.wraps", "systems.wraps.client" })
            if (!Capabilities.Is(row))
            {
                await c.Skip($"{row} is unavailable: {Capabilities.Why(row)}");
                return;
            }
        var client = new WrapCount();
        var server = new WrapCount();
        var disabled = new ConcurrentQueue<WrapDisabledEvent>();
        var w1 = Systems.Wrap("render_transform", new WrapOptions { World = WorldKind.Client });
        if (!c.Expect(w1.Ok && w1.Wrap is not null, "a wrap of the client world's render_transform", w1.Ok ? "made" : w1.Reason))
        {
            await c.Verdict();
            return;
        }
        var clientWrap = w1.Wrap!;
        c.OnEnd(() => clientWrap.Dispose());
        clientWrap.Before((ref Slice s) => client.Add(ref s));
        clientWrap.Disabled += e => disabled.Enqueue(e);
        var w2 = Systems.Wrap("is_actor_in_shape");
        if (!c.Expect(w2.Ok && w2.Wrap is not null, "a wrap of the hosted world's is_actor_in_shape", w2.Ok ? "made" : w2.Reason))
        {
            await c.Verdict();
            return;
        }
        var serverWrap = w2.Wrap!;
        c.OnEnd(() => serverWrap.Dispose());
        serverWrap.Before((ref Slice s) => server.Add(ref s));
        serverWrap.Disabled += e => disabled.Enqueue(e);
        // The archetype filter on a client: render_transform reads CurrentTransform and runs without StaticTransform, so every
        // slice it runs passes this filter; deciding it reads each slice's first entity in the client world's entity map
        // (statically the server's map, out/06a-program-wraps/client-static) on the game's workers. A slice it cannot
        // decide turns the wrap off with a reason (a Disabled event below).
        var archetype = new WrapCount();
        var w3 = Systems.Wrap("render_transform", new WrapOptions { World = WorldKind.Client });
        if (!c.Expect(w3.Ok && w3.Wrap is not null, "a second wrap of render_transform, with an archetype filter", w3.Ok ? "made" : w3.Reason))
        {
            await c.Verdict();
            return;
        }
        var archetypeWrap = w3.Wrap!;
        c.OnEnd(() => archetypeWrap.Dispose());
        archetypeWrap.WhenArchetype(has: ["CurrentTransform"], lacks: ["StaticTransform"])
            .Before((ref Slice s) => archetype.Add(ref s));
        archetypeWrap.Disabled += e => disabled.Enqueue(e);
        c.Instruct("Two of the game's own programs are wrapped now: the one that places every model where it is drawn (client " +
                   "world) and one that tracks who stands in which area (your hosted world). The wraps only count. Walk and " +
                   "turn around for about 30 seconds, near creatures or other players if you can.");
        await c.Delay(TimeSpan.FromSeconds(30));
        c.Log.Info($"client-pass: 06a-wraps.client render_transform (client world): {client}; stats {clientWrap.Stats}");
        c.Log.Info($"client-pass: 06a-wraps.client is_actor_in_shape (hosted world): {server}; stats {serverWrap.Stats}");
        c.Log.Info($"client-pass: 06a-wraps.client render_transform with WhenArchetype (client world): {archetype}; stats {archetypeWrap.Stats}");
        c.Expect(clientWrap.State == WrapState.Active && serverWrap.State == WrapState.Active, "both wraps are active",
            $"client {clientWrap.State}, hosted {serverWrap.State}");
        c.Expect(Interlocked.Read(ref client.Slices) > 0, "the client world's wrap ran", client.ToString());
        c.Expect(client.Threads.Count > 1 && Interlocked.Read(ref client.OffWorldThread) > 0,
            "the client world's slices ran on the game's workers (a client world always runs its programs as tasks)", client.ToString());
        c.Expect(Interlocked.Read(ref server.Slices) > 0, "the hosted world's wrap ran", server.ToString());
        var all = Interlocked.Read(ref client.Slices);
        var admitted = Interlocked.Read(ref archetype.Slices);
        c.Expect(archetypeWrap.State == WrapState.Active && admitted > 0 && admitted * 100 >= all * 95 &&
                 Interlocked.Read(ref archetype.OffWorldThread) > 0,
            "the archetype filter decided the client world's slices on the workers (every render_transform slice passes it)",
            $"{admitted} of {all} slices admitted, {Interlocked.Read(ref archetype.OffWorldThread)} off the world's thread; {archetypeWrap.State}");
        c.Expect(disabled.IsEmpty && clientWrap.Stats.Faults == 0 && serverWrap.Stats.Faults == 0 && archetypeWrap.Stats.Faults == 0 &&
                 clientWrap.Stats.Overruns == 0 && serverWrap.Stats.Overruns == 0 && archetypeWrap.Stats.Overruns == 0,
            "no stage faulted or went over its budget", disabled.IsEmpty ? "none" : string.Join("; ", disabled.Select(e => $"{e.Program}: {e.Reason}")));
        clientWrap.Dispose();
        serverWrap.Dispose();
        archetypeWrap.Dispose();
        await c.Ticks(10);
        var before = (Interlocked.Read(ref client.Slices), Interlocked.Read(ref server.Slices));
        await c.Ticks(25);
        c.Expect(clientWrap.State == WrapState.Removed && serverWrap.State == WrapState.Removed &&
                 before == (Interlocked.Read(ref client.Slices), Interlocked.Read(ref server.Slices)),
            "both wraps are gone after Dispose: no slice reached them in 25 steps",
            $"client {clientWrap.State}, hosted {serverWrap.State}; slices {before} then " +
            $"({Interlocked.Read(ref client.Slices)}, {Interlocked.Read(ref server.Slices)})");
        c.Instruct("While the wraps were on: did the game look and run as before (models where they belong, no flicker, no " +
                   "stutter or freeze that the game does not have otherwise)?");
        await c.Verdict();
    }

    // ---- 06a-wraps.waterworks ----

    private sealed class Waterworks
    {
        public long Slices, WorkerSlices, Fills, GameAdded, Applied;
        public readonly ConcurrentDictionary<int, string> Threads = new();   // managed thread id -> the OS thread's name
        public readonly ConcurrentQueue<(uint Entity, int Value)> Written = new();
        public string? OtherProgram;

        [DllImport("kernel32.dll")] private static extern nint GetCurrentThread();
        [DllImport("kernel32.dll")] private static extern int GetThreadDescription(nint thread, out nint description);
        [DllImport("kernel32.dll")] private static extern nint LocalFree(nint mem);

        // The name the game gave the OS thread ("keen Task-Default03"); "?" where it cannot be read.
        public static string ThreadName()
        {
            try
            {
                if (GetThreadDescription(GetCurrentThread(), out var p) < 0 || p == 0) return "?";
                var name = Marshal.PtrToStringUni(p) ?? "?";
                LocalFree(p);
                return name.Length > 0 ? name : "?";
            }
            catch (Exception) { return "?"; }
        }
    }

    [ClientCheck("06a-wraps.waterworks", "Program wraps with a second player: a well refills twice as fast", Area = "Systems",
        Since = Dev29, Tags = [TwoPlayers, Visual], Needs = ClientCheckNeeds.Hosted, Order = ProgramWrapsOrder + 1)]
    public async Task ProgramWrapsWaterworks(CheckContext c)
    {
        if (!await NeedOpen(c, "systems.wraps", "systems.wraps.typed") || !await NeedPlayer(c)) return;
        foreach (var row in new[] { "systems.wraps", "systems.wraps.typed" })
            if (!Capabilities.Is(row))
            {
                await c.Skip($"{row} is unavailable: {Capabilities.Why(row)}");
                return;
            }
        c.Instruct("Have a second player join your world and stay in it (they can stand anywhere). Then go to a well or a " +
                   "water barrel that is not full: you will watch its water bar.");
        for (var waited = 0; Players.Count < 2 && waited < 120; waited++) await c.Delay(TimeSpan.FromSeconds(1));
        if (!c.Expect(Players.Count >= 2, "a second player is in the hosted world", $"{Players.Count} player(s)"))
        {
            await c.Verdict();
            return;
        }
        var w = new Waterworks();
        var disabled = new ConcurrentQueue<WrapDisabledEvent>();
        var made = Systems.Wrap<Ecs.Programs.FuelSetup>();
        if (!c.Expect(made.Ok && made.Wrap is not null, "a typed wrap of the hosted world's fuel_setup (its live contract is the view's)",
                made.Ok ? $"contract 0x{Ecs.Programs.FuelSetup.Contract:X8}" : made.Reason))
        {
            await c.Verdict();
            return;
        }
        var wrap = made.Wrap!;
        c.OnEnd(() => wrap.Dispose());
        wrap.Disabled += e => disabled.Enqueue(e);
        // the slices: counted only when they are fuel_setup's (a stage of another program would say so), with the thread
        wrap.Before((ref Slice s) =>
        {
            if (s.Program != "fuel_setup") { w.OtherProgram ??= s.Program; return; }
            Interlocked.Increment(ref w.Slices);
            if (!s.IsWorldThread) Interlocked.Increment(ref w.WorkerSlices);
            var id = Environment.CurrentManagedThreadId;
            if (!w.Threads.ContainsKey(id)) w.Threads.TryAdd(id, Waterworks.ThreadName());
        });
        // x2: what the game's own update added to a well, once more (the WaterWorks sample's wrap)
        wrap.AroundEach(
            capture: (in Ecs.Programs.FuelSetup well) => well.FuelValues.Length > 0 ? well.FuelValues[0] : int.MinValue,
            apply: (ref Ecs.Programs.FuelSetup well, in int before) =>
            {
                if (before == int.MinValue) return;
                var added = well.FuelValues[0] - before;
                if (added <= 0) return;
                Interlocked.Increment(ref w.Fills);
                Interlocked.Add(ref w.GameAdded, added);
                var fuel = well.Fuel;
                fuel[0] += added;
                fuel.Changed();
                Interlocked.Add(ref w.Applied, fuel[0] - before - added);
                w.Written.Enqueue((well.Entity.Value, fuel[0]));
            });
        c.Instruct("The game's well refill (the program fuel_setup) is wrapped now: whatever the game adds to a well or water " +
                   "barrel, the wrap adds once more. Watch the water bar of a well that is not full for about 90 seconds: it " +
                   "should fill about twice as fast as it does without the wrap.");
        int read = 0, same = 0;
        for (var until = DateTime.UtcNow + TimeSpan.FromSeconds(90); DateTime.UtcNow < until;)
        {
            await c.Ticks(1);
            // what the apply wrote, read back on the tick thread after the step (GetComponent)
            while (w.Written.TryDequeue(out var x))
            {
                if (Ecs.Get<Ecs.Fuel>(new EntityId(x.Entity)) is not { } f) continue;
                read++;
                if (f.Values[0] == x.Value) same++;
            }
        }
        var stats = wrap.Stats;
        var names = string.Join(", ", w.Threads.Values.Distinct().OrderBy(n => n));
        c.Log.Info($"client-pass: 06a-wraps.waterworks fuel_setup: {Interlocked.Read(ref w.Slices)} slices, " +
                   $"{Interlocked.Read(ref w.WorkerSlices)} on the game's workers, threads [{names}]; {Interlocked.Read(ref w.Fills)} fills, " +
                   $"the game added {Interlocked.Read(ref w.GameAdded)}, the wrap {Interlocked.Read(ref w.Applied)}; {same} of {read} read " +
                   $"back as written; stats {stats}; players mask 0x{LabSystemsMask():x4}");
        var ok = c.Expect(w.OtherProgram is null && Interlocked.Read(ref w.Slices) > 0, "the wrap ran in fuel_setup's own slices",
            w.OtherProgram is null ? $"{Interlocked.Read(ref w.Slices)} slices" : $"a slice of {w.OtherProgram}");
        ok &= c.Expect(Interlocked.Read(ref w.WorkerSlices) > 0 && w.Threads.Count > 1,
            "fuel_setup's slices ran on the game's worker threads (the game spreads the hosted world's programs once players are in it)",
            $"{Interlocked.Read(ref w.WorkerSlices)} of {Interlocked.Read(ref w.Slices)} slices off the world's thread, threads [{names}]");
        ok &= c.Expect(Interlocked.Read(ref w.Fills) > 0 && Interlocked.Read(ref w.Applied) > 0,
            "the game's own fills were seen in fuel_setup's update and doubled",
            $"{Interlocked.Read(ref w.Fills)} fills, the game added {Interlocked.Read(ref w.GameAdded)}, the wrap {Interlocked.Read(ref w.Applied)}");
        ok &= c.Expect(read > 0 && same * 10 >= read * 9, "GetComponent reads the doubled water after the step (9 of 10 at least)",
            $"{same} of {read}");
        ok &= c.Expect(disabled.IsEmpty && stats.Faults == 0 && stats.Overruns == 0, "no stage faulted or went over its budget",
            disabled.IsEmpty ? $"{stats.Overruns} overruns" : string.Join("; ", disabled.Select(e => $"{e.Stage}: {e.Reason}")));
        wrap.Dispose();
        // the runner cannot fail a check by itself: a failed assertion is a fail of this check, and the player is told so
        c.Instruct(ok
            ? "The wrap is off again: the well fills at the game's own speed from now on. Did its water bar fill about twice " +
              "as fast while the wrap was on, and did the game run as before (no stutter, no error)?"
            : "The wrap is off again. The check's own assertions failed (see the list: the wrap did not run in fuel_setup on " +
              "the game's workers, or its writes did not land): answer Fail, whatever the water bar did.");
        await c.Verdict();
    }

    // The server world's players mask (0xFFFF: not known): bit (id - 1) mod 16 per player in the world.
#pragma warning disable KILNX001
    private static int LabSystemsMask() => LabSystems.PlayersMask ?? 0xFFFF;
#pragma warning restore KILNX001
}
