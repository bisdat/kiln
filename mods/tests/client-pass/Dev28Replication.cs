// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;

namespace ClientPass;

// 11-replication-observe (dev.31): the network inspector (Kiln.Replication). Orders 3300-3309 (lab port 28130).
//   11-repl.receive-watch   a hosted world: what this client receives of a chest (Replication.WatchReceived, the game's own
//                           decode) equals what its own server sends it (Replication.Watch on machine 0, the local
//                           client), message by message, for every component the game does not pack with a codec of its own.
//   11-repl.stats-real-peer the server's counters for a real player (Replication.Stats): both channels count, acks come
//                           back, and the counts grow while the player moves; on a hosted world the local client's rows.
public sealed partial class ClientPassMod
{
    private const string ReplArea = "Replication (the network inspector)";

    private static EntityId NearestContainer(EntityId player)
    {
        var at = World.Position(player);
        EntityId best = default;
        var bestD = double.MaxValue;
        foreach (var e in Items.FindInventories(InventoryKinds.Containers))
        {
            if (at is null) return e;
            if (World.Position(e) is not { } p) continue;
            var d = Math.Pow(p.X - at.Value.X, 2) + Math.Pow(p.Y - at.Value.Y, 2) + Math.Pow(p.Z - at.Value.Z, 2);
            if (d < bestD) (best, bestD) = (e, d);
        }
        return best;
    }

    [ClientCheck("11-repl.receive-watch", "A chest as this client receives it equals what the server sends it", Area = ReplArea,
        Since = Dev31, Needs = ClientCheckNeeds.Hosted, Order = 3300)]
    public async Task ReplReceiveWatch(CheckContext c)
    {
        if (!Capabilities.Is("repl.client") || !Capabilities.Is("repl.observe"))
        {
            await c.Skip($"repl.client: {Capabilities.Why("repl.client")}; repl.observe: {Capabilities.Why("repl.observe")}");
            return;
        }
        c.Instruct("Host a world (single player is fine). Stand next to a storage chest that holds a few items. Press continue.");
        await c.Verdict();
        var chest = NearestContainer(c.Player);
        if (!c.Expect(!chest.IsNone, "a chest near you", "no container found near the player")) return;
        var gate = new object();
        var sent = new List<ReplSent>();
        var received = new List<ReplReceived>();
        ReplWatch? rx = null;
        var send = Replication.Watch(chest);
        send.Sent += e => { if (e.Local) lock (gate) sent.Add(e); };
        c.OnEnd(send.Dispose);
        Exception? rxError = null;
        Client.Post(() =>
        {
            try
            {
                rx = Replication.WatchReceived(chest);
                rx.Received += e => { lock (gate) received.Add(e); };
            }
            catch (Exception e)
            {
                rxError = e;
            }
        });
        c.OnEnd(() => Client.Post(() => rx?.Dispose()));
        c.Instruct($"Open that chest (entity {chest.Value}) and move one item to another slot of the chest, then take one item " +
                   "out and put it back. Press continue when done.");
        await c.Verdict();
        await c.Until(() => { lock (gate) return received.Count > 0; }, TimeSpan.FromSeconds(10), "the client received the chest");
        c.Expect(rxError is null, "Replication.WatchReceived started on the game's main thread", rxError?.Message);
        int matched = 0, equal = 0, differ = 0;
        string? firstDiffer = null;
        lock (gate)
        {
            foreach (var r in received.Where(r => r.Kind != ReplKind.Destroyed))
            {
                var s = sent.FirstOrDefault(x => x.Tick == r.Tick && x.Channel == ReplChannel.Client && x.Kind == r.Kind);
                if (s is null) continue;
                matched++;
                foreach (var (name, bytes) in r.Components)
                {
                    if (s.PackedByGame.Contains(name) || !s.Components.TryGetValue(name, out var mine)) continue;
                    if (bytes.Span.SequenceEqual(mine.Span)) equal++;
                    else { differ++; firstDiffer ??= $"{name} at tick {r.Tick}"; }
                }
            }
            c.Log.Info($"11-repl.receive-watch: chest {chest.Value}: sent to the local client {sent.Count}, received {received.Count}, " +
                       $"matched by tick {matched}, components equal {equal}, differ {differ}{(firstDiffer is null ? "" : $" (first: {firstDiffer})")}");
            c.Expect(received.Count > 0 && sent.Count > 0, "the chest was both sent and received while you moved items",
                $"sent {sent.Count}, received {received.Count}");
            c.Expect(matched > 0 && equal > 0 && differ == 0, "every received component equals what the server sent in the same message",
                $"matched {matched}, equal {equal}, differ {differ}{(firstDiffer is null ? "" : $", first {firstDiffer}")}");
        }
        c.Instruct("Did the chest behave normally while the check watched it (items where you put them, no flicker)?");
        await c.Verdict();
    }

    [ClientCheck("11-repl.stats-real-peer", "The server's counters for a real player", Area = ReplArea, Since = Dev31,
        Needs = ClientCheckNeeds.Any, Order = 3301)]
    public async Task ReplStatsRealPeer(CheckContext c)
    {
        if (!Capabilities.Is("repl.observe"))
        {
            await c.Skip($"repl.observe: {Capabilities.Why("repl.observe")}");
            return;
        }
        IReadOnlyList<ReplPeerStats> Mine()
        {
            var all = Replication.Stats();
            var peer = Network.PeerOf(c.Player);
            return c.Session == ClientCheckSession.Hosted ? all.Where(r => r.Local).ToList()
                : all.Where(r => peer is not null && r.Peer?.Id == peer.Id).ToList();
        }
        Mine();  // the first call turns the counters on
        c.Instruct("Walk or run around for about twenty seconds (open the map once, pick something up if you like). Press continue.");
        await c.Verdict();
        var a = Mine();
        await c.Delay(TimeSpan.FromSeconds(5));
        var b = Mine();
        foreach (var r in b) c.Log.Info($"11-repl.stats-real-peer: {r}");
        long Sum(IReadOnlyList<ReplPeerStats> rows, ReplChannel ch, Func<ReplPeerStats, long> f) => rows.Where(r => r.Channel == ch).Sum(f);
        c.Expect(b.Any(r => r.Channel == ReplChannel.Client) && b.Any(r => r.Channel == ReplChannel.Prediction),
            "rows for your client and prediction channels", $"{b.Count} rows");
        c.Expect(Sum(b, ReplChannel.Client, r => r.Messages) > 0 && Sum(b, ReplChannel.Client, r => r.Updates) > 0,
            "messages and updates were sent to you", $"client channel: {Sum(b, ReplChannel.Client, r => r.Messages)} messages");
        c.Expect(c.Session == ClientCheckSession.Hosted || Sum(b, ReplChannel.Client, r => r.Acks) > 0,
            "your game acknowledged them (acks counted)", $"acks {Sum(b, ReplChannel.Client, r => r.Acks)}");
        c.Expect(Sum(b, ReplChannel.Client, r => r.Messages) >= Sum(a, ReplChannel.Client, r => r.Messages),
            "the counts grow while you play", $"{Sum(a, ReplChannel.Client, r => r.Messages)} then {Sum(b, ReplChannel.Client, r => r.Messages)}");
        c.Instruct("Did the game run as usual while the counters were on (no stutter you would not have otherwise)?");
        await c.Verdict();
    }
}
