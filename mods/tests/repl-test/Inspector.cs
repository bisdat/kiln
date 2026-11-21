// SPDX-License-Identifier: MIT
// repl-test, dev.31: the network inspector (Replication) on the loopback harness's fakes (ReplTestMod.cs, header).
using Kiln;

namespace ReplTest;

public sealed partial class ReplTestMod
{
    private const uint FakePeerBit = 0x80000000;

    private static uint FakeId(int machine) => FakePeerBit | (uint)machine;

    private static bool Same(ReadOnlyMemory<byte> a, byte[]? b) => b is not null && a.Span.SequenceEqual(b);

    // Every phase on the fakes it gets; returns the fakes the release phase frees.
    private async Task<IReadOnlyList<(int Machine, int Slot)>> Inspector(EntityId chest, EntityId can, IReadOnlyList<(int Machine, int Slot)> fakes,
        int max)
    {
        var watch = await StatsAndWatch(can, fakes, max);
        await Bind(can, fakes);
        fakes = await HookRemoval(chest, can, watch, max);
        fakes = await Cost(chest, can, max);
        await Destroy(chest, can, fakes);
        return fakes;
    }

    private async Task<ReplWatch> StatsAndWatch(EntityId can, IReadOnlyList<(int Machine, int Slot)> fakes, int max)
    {
        // the counters: the first call turns them on (no rows yet); from then on they count what the harness captures
        var l0 = ReplicationLab.LoopbackStats();
        var first = Replication.Stats();
        await Items.Fuel.Set(can, max / 3);
        await Steps(60);
        var before = Replication.Stats().Where(r => r.Fake).ToList();
        long U(IEnumerable<ReplPeerStats> rows, ReplChannel ch) => rows.Where(r => r.Channel == ch).Sum(r => r.Unchanged);
        // touches that change nothing: five tracked writes of the can's own ItemState byte, five water amounts it has already
        var itemState = World.ReadBytes(can, "ItemState");
        var touched = 0;
        if (itemState is { Length: > 0 })
            for (var i = 0; i < 5; i++)
            {
                var w = await World.WriteAt(can, "ItemState", 0, itemState[0]);
                touched += w.Ok ? 1 : 0;
                await Steps(5);
            }
        for (var i = 0; i < 5; i++)
        {
            var f = await Items.Fuel.Set(can, max / 3);
            touched += f.Ok ? 1 : 0;
            await Steps(5);
        }
        await Steps(10);
        var l1 = ReplicationLab.LoopbackStats();
        var rows = Replication.Stats();
        var fakeRows = rows.Where(r => r.Fake).ToList();
        var machines = fakes.Select(f => FakeId(f.Machine)).ToHashSet();
        var bothChannels = fakes.All(f => fakeRows.Any(r => r.PeerId == FakeId(f.Machine) && r.Channel == ReplChannel.Client) &&
                                          fakeRows.Any(r => r.PeerId == FakeId(f.Machine) && r.Channel == ReplChannel.Prediction));
        long sent = fakeRows.Sum(r => r.Messages), captured = l1.Messages - l0.Messages;
        long lists = fakeRows.Sum(r => r.DestroyLists), capturedLists = l1.DestroyLists - l0.DestroyLists;
        var unchanged = U(fakeRows, ReplChannel.Client) - U(before, ReplChannel.Client);
        var unchangedPrediction = U(fakeRows, ReplChannel.Prediction) - U(before, ReplChannel.Prediction);
        foreach (var r in fakeRows) Info($"stats row: fake 0x{r.PeerId:x} {r.Channel}: {r}");
        Result(first.Count == 0 && bothChannels && fakeRows.All(r => machines.Contains(r.PeerId) && r.Peer is null) && sent > 0 && sent == captured &&
               lists == capturedLists && unchangedPrediction > 0 && fakeRows.All(r => r.TooBig == 0 && r.Refused == 0 && r.Dropped == 0),
            "stats",
            $"first call {first.Count} rows; {fakeRows.Count} fake rows (both channels of {fakes.Count} fakes: {bothChannels}); messages counted {sent}, " +
            $"captured by the harness in the same frames {captured}; destroy lists {lists} vs {capturedLists}; Unchanged rose by " +
            $"{unchangedPrediction} on the prediction channel (the game re-checks predicted entities every frame) and by {unchanged} on the " +
            $"client channel over {touched} touches that changed nothing ({fakes.Count} fakes); too big {fakeRows.Sum(r => r.TooBig)}, refused {fakeRows.Sum(r => r.Refused)}, " +
            $"dropped {fakeRows.Sum(r => r.Dropped)}; bytes {fakeRows.Sum(r => r.Bytes)}, acks {fakeRows.Sum(r => r.Acks)}");

        // a watch started mid-session: the next message's full values, no baseline needed
        var events = new List<ReplSent>();
        var watch = Replication.Watch(can);
        watch.Sent += events.Add;
        var value = max / 3 + 17;
        await Items.Fuel.Set(can, value);
        await Steps(15);
        var live = World.ReadBytes(can, "NetworkFuel");
        int equal = 0, decodedEqual = 0, full = 0;
        foreach (var f in fakes)
        {
            var mine = events.Where(e => e.PeerId == FakeId(f.Machine) && e.Channel == ReplChannel.Client).ToList();
            if (mine.Count > 0 && mine[0].Components.ContainsKey("NetworkFuel") && mine[0].Components.ContainsKey("ItemState")) full++;
            var last = mine.LastOrDefault(e => e.Components.ContainsKey("NetworkFuel"));
            if (last is null) continue;
            if (Same(last.Components["NetworkFuel"], live)) equal++;
            var d = ReplicationLab.Decoded(f.Machine, can, "NetworkFuel");
            if (d is not null && d.Tick == last.Tick && last.Components["NetworkFuel"].Span.SequenceEqual(d.Bytes.Span)) decodedEqual++;
        }
        var prediction = events.Count(e => e.Channel == ReplChannel.Prediction);
        var sample = events.FirstOrDefault(e => e.Channel == ReplChannel.Client);
        Result(equal == fakes.Count && decodedEqual == fakes.Count && full == fakes.Count && events.All(e => !e.Gap && e.Fake && e.Entity == can),
            "watch",
            $"{events.Count} events ({prediction} on the prediction channel); per fake: newest NetworkFuel equal to the live bytes {equal}/{fakes.Count}, " +
            $"equal to the fake's decoded state of the same tick {decodedEqual}/{fakes.Count}, first message with full values {full}/{fakes.Count}; " +
            $"gaps {events.Count(e => e.Gap)}; e.g. {(sample is null ? "none" : $"{sample.Kind} tick {sample.Tick} seq {sample.Seq} base {sample.BaselineTick}, {sample.PayloadBytes} B, components {string.Join(",", sample.Components.Keys)}, packed by the game {string.Join(",", sample.PackedByGame)}")}");
        return watch;
    }

    // kiln.net's lab peer stands for a fake (BIND_MACHINE): it shows as that peer, and a watch for it sees only its fake.
    private async Task Bind(EntityId can, IReadOnlyList<(int Machine, int Slot)> fakes)
    {
        var lab = Network.Peers.FirstOrDefault(p => p.IsLab);
        if (lab is null)
        {
            Info("bind: no kiln.net lab peer connected (tools/nettest): skipped");
            return;
        }
        var machine = fakes[0].Machine;
        var bound = ReplicationLabInternals.BindMachine(lab.Id, machine);
        await Steps(2);
        var rows = Replication.Stats().Where(r => r.Peer is not null && r.Peer.Id == lab.Id).ToList();
        var events = new List<ReplSent>();
        using (var w = Replication.Watch(can, lab))
        {
            w.Sent += events.Add;
            await Items.Fuel.Set(can, 123);
            await Steps(15);
        }
        var only = events.Count > 0 && events.All(e => e.Peer is not null && e.Peer.Id == lab.Id && e.PeerId == lab.Id);
        ReplicationLabInternals.BindMachine(lab.Id, -1);  // 0xFFFFFFFF: the lab peer stands for no fake again
        Result(bound == lab.Id && rows.Count > 0 && rows.All(r => r.Fake) && only, "bind",
            $"lab peer 0x{lab.Id:x} bound to fake machine {machine} (kiln.net answers 0x{bound:x}); stats rows under it {rows.Count}; " +
            $"a watch for it got {events.Count} events, all from it: {only}");
    }

    // The create payloads of the can with the hooks on equal those after the hooks are removed (same can, same fuel).
    private async Task<IReadOnlyList<(int Machine, int Slot)>> HookRemoval(EntityId chest, EntityId can, ReplWatch watch, int max)
    {
        await Items.Fuel.Set(can, max / 2);
        await Steps(20);
        var (fakesOn, on) = await FreshCreates(chest, can, 4);
        watch.Dispose();
        Replication.StopStats(Replication.OwnerOfCaller());
        await Steps(3);
        ReplicationLabInternals.HookCost(reset: true);
        var (fakes, off) = await FreshCreates(chest, can, 4);
        var cost = ReplicationLabInternals.HookCost(reset: false);
        int same = 0, differ = 0, missing = 0;
        foreach (var f in fakes)
        {
            if (!on.TryGetValue(f.Machine, out var a) || !off.TryGetValue(f.Machine, out var b)) missing++;
            else if (a.AsSpan().SequenceEqual(b)) same++;
            else differ++;
        }
        Result(same == fakes.Count && differ == 0 && missing == 0 && cost.CallbackCalls == 0 && cost.WriterCalls == 0 && cost.DeltaCalls == 0 && cost.TickCalls == 0,
            "hook removal",
            $"can create payloads with the stats and watch hooks on vs after their removal, per fake: {same} identical, {differ} differ, {missing} missing " +
            $"({string.Join(", ", on.Select(kv => $"{kv.Key}:{kv.Value.Length} B"))}); hook calls after the removal over {cost.Frames} send steps: " +
            $"callback {cost.CallbackCalls}, writer {cost.WriterCalls}, delta {cost.DeltaCalls}, ack {cost.TickCalls}");
        return fakes;
    }

    // New fakes (the old ones go): each one's create of the can.
    private async Task<(IReadOnlyList<(int Machine, int Slot)> Fakes, Dictionary<int, byte[]> Creates)> FreshCreates(EntityId chest, EntityId can, int count)
    {
        ReplicationLab.RemoveFakePeers();
        await Steps(3);
        var fakes = ReplicationLab.AddFakePeers(count);
        foreach (var f in fakes)
        {
            ReplicationLab.ForceRelevant(f.Machine, chest);
            ReplicationLab.ForceRelevant(f.Machine, can);
        }
        var creates = new Dictionary<int, byte[]>();
        for (var i = 0; i < 240 && creates.Count < fakes.Count; i++)
        {
            await Steps(1);
            foreach (var f in fakes)
                if (!creates.ContainsKey(f.Machine) && ReplicationLabInternals.Payload(f.Machine, can, ReplKind.Create) is { } p) creates[f.Machine] = p.Bytes;
        }
        return (fakes, creates);
    }

    // 16 fakes, the stats and a watch on: what kiln.repl's own code costs per frame.
    private async Task<IReadOnlyList<(int Machine, int Slot)>> Cost(EntityId chest, EntityId can, int max)
    {
        ReplicationLab.RemoveFakePeers();
        await Steps(3);
        var fakes = ReplicationLab.AddFakePeers(16);
        foreach (var f in fakes)
        {
            ReplicationLab.ForceRelevant(f.Machine, chest);
            ReplicationLab.ForceRelevant(f.Machine, can);
        }
        Replication.Stats();
        var events = 0L;
        using var watch = Replication.Watch(can);
        watch.Sent += _ => events++;
        await Steps(60);
        ReplicationLabInternals.HookCost(reset: true);
        var rnd = new Random(777);
        Task<FuelResult>? pending = null;
        const int StepsMeasured = 600;
        for (var i = 0; i < StepsMeasured; i++)
        {
            if (pending is null || pending.IsCompleted) pending = Items.Fuel.Set(can, rnd.Next(0, max + 1));
            await Steps(1);
        }
        var c = ReplicationLabInternals.HookCost(reset: false);
        var hookNs = (double)(c.CallbackNs + c.WriterNs + c.DeltaNs + c.TickNs);
        var perFrame = c.Frames > 0 ? hookNs / c.Frames : 0;
        var share = c.FrameIntervalMeanNs > 0 ? perFrame / c.FrameIntervalMeanNs : 1;
        var ofSend = c.SendStepP50Ns > 0 ? perFrame / c.SendStepP50Ns : 0;
        static string Per(ulong ns, ulong calls) => calls > 0 ? $"{(double)ns / calls:0} ns/call" : "-";
        Result(c.Frames > 0 && c.WriterCalls > 0 && c.DeltaCalls > 0 && c.CallbackCalls > 0 && c.Allocations == 0 && share < 0.02 && events > 0, "cost",
            $"16 fakes, stats and a watch on the can, {StepsMeasured} steps ({c.Frames} send steps, window {c.WindowNs / 1e6:0} ms): " +
            $"callback {c.CallbackCalls} calls {Per(c.CallbackNs, c.CallbackCalls)}, writer {c.WriterCalls} {Per(c.WriterNs, c.WriterCalls)}, " +
            $"delta {c.DeltaCalls} {Per(c.DeltaNs, c.DeltaCalls)}, ack/reset {c.TickCalls} {Per(c.TickNs, c.TickCalls)}; " +
            $"{perFrame / 1000:0.0} us per frame = {share * 100:0.000} % of the frame interval (mean {c.FrameIntervalMeanNs / 1e6:0.0} ms, p50 " +
            $"{c.FrameIntervalP50Ns / 1e6:0.0} ms) and {ofSend * 100:0.0} % of the send step's p50 ({c.SendStepP50Ns / 1e3:0} us, mean " +
            $"{c.SendStepMeanNs / 1e3:0} us); allocations inside hook code {c.Allocations}; watch records {c.WatchRecords}, dropped {c.WatchDropped}, events {events}");
        // a 5 s capture of one fake for tools/replinspect (the lab script checks the file); measured after the window
        var captured = ReplCommands.CaptureFor(Replication.OwnerOfCaller(), $"0x{FakeId(fakes[0].Machine):x}", 5);
        Info($"capture: {captured}");
        var end = Environment.TickCount64 + 6_500;
        while (Environment.TickCount64 < end)
        {
            if (pending is null || pending.IsCompleted) pending = Items.Fuel.Set(can, rnd.Next(0, max + 1));
            await Steps(1);
        }
        if (pending is not null) await pending;
        return fakes;
    }

    // Two fakes kept for a minute with the chest relevant, for the lab script's console checks of the repl commands
    // ("repl stats", "repl watch", "repl capture", "repl component"); then the mod lets go of its counters, so its
    // unload can show the hooks go (the lab script unloads it).
    private async Task Hold(EntityId chest)
    {
        var fakes = ReplicationLab.AddFakePeers(2);
        foreach (var f in fakes) ReplicationLab.ForceRelevant(f.Machine, chest);
        Info($"holding 2 fakes for the console checks: chest {chest.Value}, fake peers {string.Join(", ", fakes.Select(f => $"0x{FakeId(f.Machine):x}"))}");
        var until = Environment.TickCount64 + 60_000;
        while (Environment.TickCount64 < until) await Steps(10);
        ReplicationLab.RemoveFakePeers();
        Info("hold over");
    }

    // Taking the can out of the chest: a destroy list naming it (list A) to each fake on both channels.
    private async Task Destroy(EntityId chest, EntityId can, IReadOnlyList<(int Machine, int Slot)> fakes)
    {
        var events = new List<ReplSent>();
        ReplWatchEnded? ended = null;
        using var watch = Replication.Watch(can);
        watch.Sent += events.Add;
        watch.Ended += e => ended = e;
        await Steps(10);
        var before = Replication.Stats().Where(r => r.Fake).ToDictionary(r => (r.PeerId, r.Channel));
        var slot = Items.Read(chest)?.Slots.FirstOrDefault(s => s.ItemEntity == can);
        var take = slot is null ? null : await Items.TakeSlot(chest, slot);
        for (var i = 0; i < 60 && ended is null; i++) await Steps(1);
        var after = Replication.Stats().Where(r => r.Fake).ToList();
        var destroyed = events.Where(e => e.Kind == ReplKind.Destroyed).ToList();
        var clientAll = fakes.All(f => destroyed.Any(e => e.PeerId == FakeId(f.Machine) && e.Channel == ReplChannel.Client));
        var predictionAll = fakes.All(f => destroyed.Any(e => e.PeerId == FakeId(f.Machine) && e.Channel == ReplChannel.Prediction));
        var listsRose = after.Count(r => before.TryGetValue((r.PeerId, r.Channel), out var b) && r.DestroyLists > b.DestroyLists && r.DestroyedIds > b.DestroyedIds);
        var took = take is not null && take.Done > 0;
        Result(took && clientAll && ended?.Reason == "destroyed" && listsRose >= fakes.Count && !watch.IsActive, "destroy",
            $"took the can out of the chest ({take?.Status}, {take?.Done} done); Destroyed events {destroyed.Count}: every fake on the client channel {clientAll}, " +
            $"on the prediction channel {predictionAll}; then Ended({ended?.Reason ?? "none"}); rows whose destroy lists and ids rose {listsRose} " +
            $"({fakes.Count} fakes x 2 channels)");
    }
}
