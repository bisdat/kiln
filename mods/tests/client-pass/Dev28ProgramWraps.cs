// SPDX-License-Identifier: MIT
// 06a-program-wraps: the guided check 06a-wraps.client. Program wraps on a game client are on since 0.2.0-dev.29
// (systems.wraps.client, held in dev.28) without a run in a real game; this check is the record of that. In a hosted game
// it wraps the client world's render_transform (a client program: no update in the server exe) and the hosted server
// world's is_actor_in_shape with a Before each that only counts, then asserts what the code can see: each wrap reached
// its own world's slices (the prediction world, which runs the client world's table, is forwarded untouched), the client
// world's slices ran on the game's workers, a third wrap with an archetype filter (WhenArchetype: the client world's
// entity map, read on the workers while entities come and go) decided its slices, no stage faulted or went over its
// budget, and the wraps are gone after Dispose. It also prints the core's world-stop guarantee per world kind
// (world.stop_drain.*, since 0.2.0-dev.29: the game's WaitWorldJobs on the server and the client world before their
// STOPPING) and expects all three available in a client; kiln.log's stop lines at leave, rejoin and exit are the
// proof of the stop itself (docs/kiln/services/systems.md). What only a player can judge is asked: that the game looked and ran as before while they were on. The joined
// half (a client joined to a dedicated server) is not here: a joined check's body runs on the server.
//
// 06a-wraps.waterworks: the product proof (chapter 06a 6, 9.13) in a hosted world with a second player in it, where the
// game itself runs the hosted world's entity programs as tasks on its workers. The check makes its own typed wrap of
// fuel_setup (Systems.Wrap<Ecs.Programs.FuelSetup>, the WaterWorks sample's wrap, x2) and asserts from what only that
// program's own slices can show: its slices ran on the game's workers (fails when the wrap never ran on one), the game's
// own fills were seen in fuel_setup's update and the doubled water is what GetComponent reads afterwards. The player
// judges what they see: a well's water bar refilling faster while the wrap is on.
//
// 06a-define.hosted: Systems.Define (S8) in a hosted world with a second player. kiln.world reserved the pass's own program
// (ClientPassFuelQuery, from this mod's kiln.programs.json) when the game started its hosted world; the check binds it,
// asserts it ran for the hosted world's water containers with Fuel read through the query on the game's workers (fails
// when it never ran on one), and that nothing calls it after Dispose. The player judges whether the game ran as before.
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
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
        var drains = new[] { "world.stop_drain.server", "world.stop_drain.client", "world.stop_drain.prediction" }
            .Select(Capabilities.Get).ToArray();
        foreach (var d in drains)
            c.Log.Info($"client-pass: 06a-wraps.client {d.Name}: {d.State} ({d.Reason}): {d.ReasonText}");
        c.Expect(drains.All(d => d.IsAvailable),
            "the core drains every world kind's jobs before that world's stop (world.stop_drain.server, .client, .prediction)",
            string.Join("; ", drains.Select(d => $"{d.Name} {d.State}: {d.ReasonText}")));
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
        // slice it runs passes this filter. A client slice never reads the entity map: the first read asks for kiln.world's
        // scoped slice-record link around the program runner and that slice is left to the game (Stats.ArchetypeSkips)
        // until the link is in, a Pre later; from then on each slice's archetype comes from the engine's own slice record on
        // the game's workers. A build where the link cannot be had turns the wrap off with the reason (a Disabled event
        // below).
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
            $"{admitted} of {all} slices admitted, {Interlocked.Read(ref archetype.OffWorldThread)} off the world's thread, " +
            $"{archetypeWrap.Stats.ArchetypeSkips} left to the game; {archetypeWrap.State}");
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

    [ClientCheck("06a-define.hosted", "A program of a mod's own (Systems.Define) in a hosted world with a second player", Area = "Systems",
        Since = Dev29, Tags = [TwoPlayers], Needs = ClientCheckNeeds.Hosted, Order = ProgramWrapsOrder + 2)]
    public async Task ProgramDefineHosted(CheckContext c)
    {
        if (!await NeedOpen(c, "systems.define") || !await NeedPlayer(c)) return;
        if (!Capabilities.Is("systems.define"))
        {
            await c.Skip($"systems.define is unavailable: {Capabilities.Why("systems.define")}");
            return;
        }
        long calls = 0, fuel = 0;
        var threads = new ConcurrentDictionary<string, byte>();
        var made = Systems.Define(ClientPassFuelQuery.Key, new ProgramOptions(), (ref ClientPassFuelQuery q) =>
        {
            Interlocked.Increment(ref calls);
            if (q.Context.TryGet<Ecs.Fuel>(out _)) Interlocked.Increment(ref fuel);
            threads.TryAdd(Waterworks.ThreadName(), 0);
        });
        if (!c.Expect(made.Ok, "the program kiln.world reserved at boot from client-pass's kiln.programs.json is bound", made.Reason ?? made.Key))
        {
            await c.Verdict();
            return;
        }
        c.OnEnd(() => made.Dispose());
        c.Instruct("A program of the pass's own now runs over your hosted world's water containers (barrels, wells, cans) and " +
                   "only counts them. Keep the second player in the world and stay near a few containers for about 30 seconds.");
        await c.Delay(TimeSpan.FromSeconds(30));
        var seen = $"{Interlocked.Read(ref calls)} entity calls, Fuel read on {Interlocked.Read(ref fuel)}, threads {string.Join(", ", threads.Keys.Order())}";
        c.Log.Info($"client-pass: 06a-define.hosted client_pass_fuel: {seen}");
        c.Expect(Interlocked.Read(ref calls) > 0 && Interlocked.Read(ref fuel) == Interlocked.Read(ref calls),
            "it ran for the hosted world's containers and read their Fuel through the query", seen);
        c.Expect(threads.Keys.Any(n => n.Contains("Task-Default", StringComparison.Ordinal)),
            "it ran on the game's workers (Task-Default), as the game runs its own entity programs with a player in the world", seen);
        made.Dispose();
        var before = Interlocked.Read(ref calls);
        await c.Ticks(25);
        c.Expect(Interlocked.Read(ref calls) == before, "nothing calls it after Dispose (25 steps)",
            $"{before} then {Interlocked.Read(ref calls)}");
        c.Instruct("While it ran: did the game run as before (no stutter or freeze that the game does not have otherwise)?");
        await c.Verdict();
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
        c.Expect(w.OtherProgram is null && Interlocked.Read(ref w.Slices) > 0, "the wrap ran in fuel_setup's own slices",
            w.OtherProgram is null ? $"{Interlocked.Read(ref w.Slices)} slices" : $"a slice of {w.OtherProgram}");
        c.Expect(Interlocked.Read(ref w.WorkerSlices) > 0 && w.Threads.Count > 1,
            "fuel_setup's slices ran on the game's worker threads (the game spreads the hosted world's programs once players are in it)",
            $"{Interlocked.Read(ref w.WorkerSlices)} of {Interlocked.Read(ref w.Slices)} slices off the world's thread, threads [{names}]");
        c.Expect(Interlocked.Read(ref w.Fills) > 0 && Interlocked.Read(ref w.Applied) > 0,
            "the game's own fills were seen in fuel_setup's update and doubled",
            $"{Interlocked.Read(ref w.Fills)} fills, the game added {Interlocked.Read(ref w.GameAdded)}, the wrap {Interlocked.Read(ref w.Applied)}");
        c.Expect(read > 0 && same * 10 >= read * 9, "GetComponent reads the doubled water after the step (9 of 10 at least)",
            $"{same} of {read}");
        c.Expect(disabled.IsEmpty && stats.Faults == 0 && stats.Overruns == 0, "no stage faulted or went over its budget",
            disabled.IsEmpty ? $"{stats.Overruns} overruns" : string.Join("; ", disabled.Select(e => $"{e.Stage}: {e.Reason}")));
        wrap.Dispose();
        c.Instruct("The wrap is off again: the well fills at the game's own speed from now on. Did its water bar fill about twice " +
                   "as fast while the wrap was on, and did the game run as before (no stutter, no error)?");
        await c.Verdict();
    }

    // The server world's players mask (0xFFFF: not known): bit (id - 1) mod 16 per player in the world.
#pragma warning disable KILNX001
    private static int LabSystemsMask() => LabSystems.PlayersMask ?? 0xFFFF;
#pragma warning restore KILNX001

    // ---- 06a-wraps.client-stop and 06a-wraps.client-exit: a stop with wraps registered (Codex's closing read, "Real-client
    // evidence required"). 06a-wraps.client disposes its own wraps, so it cannot show a world stopping under an active wrap.
    // These keep a client-world wrap with an archetype filter (render_transform) and a hosted-world wrap (is_actor_in_shape)
    // registered from their first run until the game exits: they go back to Pending at each stop and are applied again by
    // name in the next world. The checks then read kiln.log (and kiln.prev.log for the exit) and require, at every client
    // world stop after they were made, the core's and kiln.world's lines in the review's order.
    private static ProgramWrap? _stopClient, _stopServer;
    private static readonly WrapCount StopClientCount = new(), StopServerCount = new();
    private static readonly ConcurrentQueue<WrapDisabledEvent> StopDisabled = new();
    private const string StopMarker = "client-pass: 06a-wraps.client-stop: wraps registered for the stop proof";

    private void MakeStopWraps(CheckContext c)
    {
        var w1 = Systems.Wrap("render_transform", new WrapOptions { World = WorldKind.Client });
        var w2 = Systems.Wrap("is_actor_in_shape");
        if (!c.Expect(w1.Ok && w2.Ok, "the stop proof's wraps (client render_transform with WhenArchetype, hosted is_actor_in_shape)",
                w1.Ok ? w2.Reason ?? "made" : w1.Reason))
            return;
        _stopClient = w1.Wrap!;
        _stopClient.WhenArchetype(has: ["CurrentTransform"], lacks: ["StaticTransform"]).Before((ref Slice s) => StopClientCount.Add(ref s));
        _stopClient.Disabled += e => StopDisabled.Enqueue(e);
        _stopServer = w2.Wrap!;
        _stopServer.Before((ref Slice s) => StopServerCount.Add(ref s));
        _stopServer.Disabled += e => StopDisabled.Enqueue(e);
        c.Log.Info(StopMarker);  // not disposed: they stay registered through leave, rejoin and exit
    }

    // The ordered lines of one client world stop, from its "stopping" line on (Codex's list). Null when all are there.
    private static string? StopSequence(string[] log, int at, bool wrapsLive)
    {
        var world = Regex.Match(log[at], @"tick: client world (0x[0-9a-f]+) stopping \(session (\d+)");
        if (!world.Success) return "not a client world's stopping line";
        var w = world.Groups[1].Value;
        var want = new List<(string Text, bool Required)>
        {
            ($"tick: client world {w}: waiting for world jobs before STOPPING (WaitWorldJobs)", true),
            ($"tick: client world {w}: its jobs are drained by the engine (WaitWorldJobs, ", true),
            ("tick: prediction world 0x", false),  // only if the prediction world started
            ("world 2 stops: WhenArchetype read ", false),
            ("world 2 stops: its wraps are taken out and wait for the next world (drained: 0 calls in flight at the stop, 0 after ", wrapsLive),
        };
        // the sequence ends where the next client world starts (or the log ends)
        var next = Array.FindIndex(log, at + 1, l => l.Contains("tick: client world 0x", StringComparison.Ordinal) && l.Contains(" started (session ", StringComparison.Ordinal));
        var i = at + 1;
        foreach (var (text, required) in want)
        {
            var found = i < log.Length ? Array.FindIndex(log, i, l => l.Contains(text, StringComparison.Ordinal)) : -1;
            if (found < 0 || (next >= 0 && found > next))
            {
                if (required) return $"missing in order: \"{text}\"";
                continue;
            }
            if (text.StartsWith("world 2 stops: WhenArchetype", StringComparison.Ordinal) &&
                !Regex.IsMatch(log[found], @"read [1-9]\d* slices from the engine's slice record and 0 from the entity map \(0 records did not match"))
                return $"the read counts: {log[found].Trim()}";
            if (text.StartsWith("world 2 stops: its wraps", StringComparison.Ordinal) &&
                !log[found].Contains("the engine drains the world's jobs first (world.stop_drain.client)", StringComparison.Ordinal))
                return $"the stop line does not name the engine's drain: {log[found].Trim()}";
            i = found + 1;
        }
        return null;
    }

    // What no stop proof may show (the review's list); the link's own fault line is checked apart.
    private static readonly string[] StopForbidden =
    [
        "STOPPING without the engine's wait", "wrap drain timed out", "wrap calls were in flight although",
        "its tick hook faulted", "tick: exception in",
    ];

    private static void StopChecks(CheckContext c, string[] log, int from, string what)
    {
        var lines = log.Skip(from).ToArray();
        c.Expect(lines.Any(l => l.Contains("hooks: typed link \"world/slice-record\" on rva ", StringComparison.Ordinal)) ||
                 log.Take(from).Any(l => l.Contains("hooks: typed link \"world/slice-record\" on rva ", StringComparison.Ordinal)),
            $"{what}: the scoped link is on the runner (hooks: typed link \"world/slice-record\" on rva ...)", "");
        c.Expect(log.Any(l => l.Contains("WhenArchetype reads each slice's archetype from the engine's slice record (a scoped checked link", StringComparison.Ordinal)),
            $"{what}: kiln.world's scoped success line", "");
        foreach (var bad in StopForbidden)
        {
            var hit = lines.FirstOrDefault(l => l.Contains(bad, StringComparison.Ordinal));
            c.Expect(hit is null, $"{what}: no \"{bad}\" line", hit?.Trim() ?? "none");
        }
        var fault = lines.FirstOrDefault(l => l.Contains("typed link \"world/slice-record\"", StringComparison.Ordinal) && l.Contains("faulted", StringComparison.Ordinal));
        c.Expect(fault is null, $"{what}: the slice-record link never faulted", fault?.Trim() ?? "none");
        var waits = lines.Count(l => l.Contains("waiting for world jobs before STOPPING (WaitWorldJobs)", StringComparison.Ordinal));
        var dones = lines.Count(l => l.Contains("its jobs are drained by the engine (WaitWorldJobs, ", StringComparison.Ordinal));
        c.Expect(waits == dones, $"{what}: every wait for a world's jobs returned (no unmatched wait line)", $"{waits} wait(s), {dones} completion(s)");
    }

    [ClientCheck("06a-wraps.client-stop", "Program wraps in a game client: the stops of worlds with wraps registered", Area = "Systems",
        Since = Dev29, Needs = ClientCheckNeeds.Hosted, Order = ProgramWrapsOrder + 5)]
    public async Task ProgramWrapsClientStop(CheckContext c)
    {
        if (!await NeedOpen(c, "systems.wraps", "systems.wraps.client") || !await NeedPlayer(c)) return;
        if (BootLog(c, "kiln.log") is not { } log)
        {
            await c.Verdict();
            return;
        }
        var from = Array.FindIndex(log, l => l.Contains(StopMarker, StringComparison.Ordinal));
        if (_stopClient is null || from < 0)
        {
            MakeStopWraps(c);
            c.Instruct("Two wraps are registered now and stay so until the game exits. Walk for 10 s, then: leave to the main " +
                       "menu, host this world again, walk 10 s; leave again and join a server (a friend's or a Kiln lab) for 10 s, " +
                       "leave, host again, walk 10 s; then run this check again. Later, exit the game from a world (Quit to desktop) " +
                       "and run 06a-wraps.client-exit after the next start.");
            await c.Skip("the stop proof's wraps are registered now; leave and rejoin as said, then run this check again");
            return;
        }
        var stops = Enumerable.Range(from, log.Length - from)
            .Where(i => log[i].Contains("tick: client world 0x", StringComparison.Ordinal) && log[i].Contains(" stopping (session ", StringComparison.Ordinal))
            .ToList();
        var starts = log.Skip(from).Where(l => l.Contains("tick: client world 0x", StringComparison.Ordinal) && l.Contains(" started (session ", StringComparison.Ordinal)).ToList();
        if (stops.Count < 2 || starts.Count < 2)
        {
            await c.Skip($"{stops.Count} client world stop(s) and {starts.Count} start(s) since the wraps were made; leave and rejoin twice");
            return;
        }
        foreach (var i in stops)
            c.Expect(StopSequence(log, i, wrapsLive: true) is null, $"the stop at \"{log[i].Trim()}\" has the review's lines in order",
                StopSequence(log, i, wrapsLive: true) ?? "all there");
        var sessions = starts.Select(l => Regex.Match(l, @"\(session (\d+)")).Where(m => m.Success).Select(m => uint.Parse(m.Groups[1].Value)).ToList();
        c.Expect(sessions.Zip(sessions.Skip(1)).All(p => p.Second > p.First), "each rejoin is a new client world session", string.Join(", ", sessions));
        StopChecks(c, log, from, "leave and rejoin");
        if (GameOwnLog(c) is { } game)
            c.Expect(!game.Any(l => l.Contains(QueueNotIdle, StringComparison.Ordinal)), "the game's own log never says \"[task] TaskQueue '…' is not idle during destroy!\"",
                game.FirstOrDefault(l => l.Contains(QueueNotIdle, StringComparison.Ordinal)) ?? "none");
        // fresh callbacks in this world (the wraps were applied again by name)
        var before = (Interlocked.Read(ref StopClientCount.Slices), Interlocked.Read(ref StopServerCount.Slices));
        c.Instruct("Walk and turn around for about 15 seconds.");
        await c.Delay(TimeSpan.FromSeconds(15));
        var after = (Interlocked.Read(ref StopClientCount.Slices), Interlocked.Read(ref StopServerCount.Slices));
        c.Log.Info($"client-pass: 06a-wraps.client-stop render_transform {StopClientCount}; stats {_stopClient.Stats}; is_actor_in_shape {StopServerCount}");
        c.Expect(_stopClient.State == WrapState.Active && _stopServer!.State == WrapState.Active && after.Item1 > before.Item1 && after.Item2 > before.Item2,
            "both wraps were applied again in this world and run (fresh callbacks)", $"client {before.Item1} -> {after.Item1}, hosted {before.Item2} -> {after.Item2}");
        c.Expect(StopDisabled.IsEmpty && _stopClient.Stats.Faults == 0 && _stopServer.Stats.Faults == 0 && _stopClient.Stats.Overruns == 0,
            "no stage faulted, went over its budget or was turned off", StopDisabled.IsEmpty ? "none" : string.Join("; ", StopDisabled.Select(e => $"{e.Program}: {e.Reason}")));
        c.Instruct("Through leaving, joining and hosting again: did the game run as before, models drawn where they belong, no " +
                   "hang at a leave? Pass if it did.");
        await c.Verdict();
    }

    [ClientCheck("06a-wraps.client-exit", "Program wraps in a game client: the exit of a world with wraps registered", Area = "Systems",
        Since = Dev29, Needs = ClientCheckNeeds.Hosted, Order = ProgramWrapsOrder + 6)]
    public async Task ProgramWrapsClientExit(CheckContext c)
    {
        if (!await NeedOpen(c, "systems.wraps", "systems.wraps.client") || !await NeedPlayer(c)) return;
        if (BootLog(c, "kiln.prev.log") is not { } prev || Array.FindLastIndex(prev, l => l.Contains(StopMarker, StringComparison.Ordinal)) is var from && from < 0)
        {
            c.Instruct("Run 06a-wraps.client-stop in a hosted world, then exit the game from that world (Quit to desktop). Start the " +
                       "game again, host a world and run this check (it reads kiln.prev.log, the last start's log).");
            await c.Skip("the last start did not register the stop proof's wraps");
            return;
        }
        var last = Array.FindLastIndex(prev, l => l.Contains("tick: client world 0x", StringComparison.Ordinal) && l.Contains(" stopping (session ", StringComparison.Ordinal));
        c.Expect(last > from, "the last start's client world stopped at the exit, after the wraps were made", last >= 0 ? prev[last].Trim() : "no stop");
        if (last > from)
            c.Expect(StopSequence(prev, last, wrapsLive: true) is null, "the exit's client world stop has the review's lines in order",
                StopSequence(prev, last, wrapsLive: true) ?? "all there");
        var server = Array.FindLastIndex(prev, l => l.Contains("tick: server world 0x", StringComparison.Ordinal) && l.Contains(" stopping (session ", StringComparison.Ordinal));
        c.Expect(server > from && prev.Skip(server).Any(l => l.Contains("tick: server world 0x", StringComparison.Ordinal) && l.Contains("its jobs are drained by the engine (WaitWorldJobs, ", StringComparison.Ordinal)) &&
                 prev.Skip(server).Any(l => l.Contains("world 0 stops: its wraps are taken out", StringComparison.Ordinal) && l.Contains("(world.stop_drain.server)", StringComparison.Ordinal)),
            "the hosted world's stop at the exit: the engine's drain, then kiln.world's stop line naming it", server >= 0 ? prev[server].Trim() : "no stop");
        StopChecks(c, prev, from, "the last start, up to its exit");
        c.Instruct("Did the game close normally at that exit (no crash report, no hang)? Pass if it did.");
        await c.Verdict();
    }
}

// 06a-define.hosted's program: the hosted world's water containers (template FuelSetup, read Fuel). kiln.programs.json beside
// this file reserves it (what Kiln.Sdk's KilnPrograms task writes for it; this mod is built without the Kiln SDK).
[Query(Key = Key, Template = ["FuelSetup"], Read = ["Fuel"])]
internal ref struct ClientPassFuelQuery : IQuery<ClientPassFuelQuery>
{
    public const string Key = "client_pass_fuel";
    public EcsProgramContext Context;
    public static ClientPassFuelQuery Bind(EcsProgramContext context) => new() { Context = context };
}
