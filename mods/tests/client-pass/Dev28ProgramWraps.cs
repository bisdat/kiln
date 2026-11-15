// SPDX-License-Identifier: MIT
// 06a-program-wraps: the guided check 06a-wraps.client. Program wraps on a game client are on since 0.2.0-dev.29
// (systems.wraps.client, held in dev.28) without a run in a real game; this check is the record of that. In a hosted game
// it wraps the client world's render_transform (a client program: no update in the server exe) and the hosted server
// world's is_actor_in_shape with a Before each that only counts, then asserts what the code can see: each wrap reached
// its own world's slices (the prediction world, which runs the client world's table, is forwarded untouched), the client
// world's slices ran on the game's workers, no stage faulted or went over its budget, and both wraps are gone after
// Dispose. What only a player can judge is asked: that the game looked and ran as before while they were on. The joined
// half (a client joined to a dedicated server) is not here: a joined check's body runs on the server.
using System.Collections.Concurrent;
using Kiln;
using Kiln.Testing;

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
        c.Instruct("Two of the game's own programs are wrapped now: the one that places every model where it is drawn (client " +
                   "world) and one that tracks who stands in which area (your hosted world). The wraps only count. Walk and " +
                   "turn around for about 30 seconds, near creatures or other players if you can.");
        await c.Delay(TimeSpan.FromSeconds(30));
        c.Log.Info($"client-pass: 06a-wraps.client render_transform (client world): {client}; stats {clientWrap.Stats}");
        c.Log.Info($"client-pass: 06a-wraps.client is_actor_in_shape (hosted world): {server}; stats {serverWrap.Stats}");
        c.Expect(clientWrap.State == WrapState.Active && serverWrap.State == WrapState.Active, "both wraps are active",
            $"client {clientWrap.State}, hosted {serverWrap.State}");
        c.Expect(Interlocked.Read(ref client.Slices) > 0, "the client world's wrap ran", client.ToString());
        c.Expect(client.Threads.Count > 1 && Interlocked.Read(ref client.OffWorldThread) > 0,
            "the client world's slices ran on the game's workers (a client world always runs its programs as tasks)", client.ToString());
        c.Expect(Interlocked.Read(ref server.Slices) > 0, "the hosted world's wrap ran", server.ToString());
        c.Expect(disabled.IsEmpty && clientWrap.Stats.Faults == 0 && serverWrap.Stats.Faults == 0,
            "no stage faulted or went over its budget", disabled.IsEmpty ? "none" : string.Join("; ", disabled.Select(e => $"{e.Program}: {e.Reason}")));
        clientWrap.Dispose();
        serverWrap.Dispose();
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
}
