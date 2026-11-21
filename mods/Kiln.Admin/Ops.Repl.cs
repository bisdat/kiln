// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Text.Json.Nodes;

namespace Kiln.Admin;

/// <summary>The network inspector's ops (Kiln.Replication; docs/kiln/services/replication.md): counters per player and
/// channel, a short watch on one entity, a capture file, a component's registry facts. Read only.</summary>
internal static partial class Ops
{
    internal static bool RegisterReplicationSafe(Bridge bridge, Logger log)
    {
        try
        {
            return RegisterReplication(bridge);
        }
        catch (Exception e) when (e is FileNotFoundException or FileLoadException or TypeLoadException or MissingMethodException)
        {
            log.Info($"replication ops off: Kiln.Replication is not available ({e.GetType().Name})");
            return false;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static bool RegisterReplication(Bridge bridge)
    {
        // {off?} -> the counters per peer and channel (the first call turns them on for Kiln.Admin; off: true lets go)
        bridge.Register("repl_stats", req =>
        {
            if (req["off"] is JsonValue ov && ov.TryGetValue<bool>(out var off) && off)
            {
                Replication.StopStats(Replication.OwnerOfCaller());
                return new JsonObject { ["on"] = false };
            }
            var first = !Replication.StatsOn(Replication.OwnerOfCaller());
            var rows = Replication.Stats();
            return new JsonObject
            {
                ["on"] = true,
                ["started"] = first,
                ["rows"] = new JsonArray([.. rows.Select(r => (JsonNode)new JsonObject
                {
                    ["peer"] = r.PeerId, ["name"] = r.Peer?.Name, ["local"] = r.Local, ["fake"] = r.Fake,
                    ["channel"] = r.Channel.ToString(), ["frames"] = r.Frames, ["messages"] = r.Messages, ["bytes"] = r.Bytes,
                    ["refused"] = r.Refused, ["creates"] = r.Creates, ["updates"] = r.Updates, ["destroyLists"] = r.DestroyLists,
                    ["destroyedIds"] = r.DestroyedIds, ["unchanged"] = r.Unchanged, ["tooBig"] = r.TooBig, ["acks"] = r.Acks,
                    ["resets"] = r.Resets, ["dropped"] = r.Dropped,
                })]),
            };
        });
        // {entity, peer? (id or name), seconds? (1..30, default 3)} -> what each peer was sent of the entity meanwhile
        bridge.RegisterAsync("repl_watch", async req =>
        {
            var entity = req["entity"] is JsonValue ev && ev.TryGetValue<uint>(out var e) && e != 0 ? new EntityId(e)
                : throw new AdminError("entity required");
            var seconds = req["seconds"] is JsonValue sv && sv.TryGetValue<int>(out var s) ? s : 3;
            if (seconds is < 1 or > 30) throw new AdminError("seconds is 1 to 30");
            NetPeer? peer = null;
            if (req["peer"] is JsonNode pn)
            {
                var text = pn.ToString();
                peer = Network.Peers.FirstOrDefault(p => string.Equals(p.Name, text, StringComparison.OrdinalIgnoreCase) || p.Id.ToString() == text)
                       ?? throw new AdminError($"no connected peer '{text}'");
            }
            var events = new JsonArray();
            string? ended = null;
            ReplWatch w;
            try
            {
                w = Replication.Watch(entity, peer);
            }
            catch (Exception ex) when (ex is ArgumentException or InvalidOperationException or KilnUnavailableException)
            {
                throw new AdminError(ex.Message);
            }
            using (w)
            {
                w.Sent += m =>
                {
                    if (events.Count >= 500) return;
                    events.Add(new JsonObject
                    {
                        ["peer"] = m.PeerId, ["name"] = m.Peer?.Name, ["local"] = m.Local, ["fake"] = m.Fake, ["channel"] = m.Channel.ToString(),
                        ["kind"] = m.Kind.ToString(), ["tick"] = m.Tick, ["seq"] = m.Seq, ["base"] = m.BaselineTick, ["payloadBytes"] = m.PayloadBytes,
                        ["gap"] = m.Gap,
                        ["components"] = new JsonObject(m.Components.Select(c =>
                            KeyValuePair.Create(c.Key, (JsonNode?)System.Text.Json.Nodes.JsonValue.Create(Convert.ToHexString(c.Value.Span))))),
                        ["packedByGame"] = new JsonArray([.. m.PackedByGame.Select(n => (JsonNode)n)]),
                    });
                };
                w.Ended += x => ended = x.Reason;
                var until = Environment.TickCount64 + seconds * 1000L;
                while (Environment.TickCount64 < until && ended is null) await TickScheduler!.NextTick();
            }
            return new JsonObject { ["entity"] = entity.Value, ["events"] = events, ["ended"] = ended, ["truncated"] = events.Count >= 500 };
        });
        // {peer (id or name), seconds (1..600)} -> a capture file in kiln/logs (JSON lines; tools/replinspect reads it)
        bridge.Register("repl_capture", req =>
        {
            var text = req["peer"]?.ToString() ?? throw new AdminError("peer required");
            var seconds = req["seconds"] is JsonValue sv && sv.TryGetValue<int>(out var s) ? s : throw new AdminError("seconds required");
            try
            {
                return new JsonObject { ["message"] = ReplCommands.CaptureFor(Replication.OwnerOfCaller(), text, seconds) };
            }
            catch (CommandException ex)
            {
                throw new AdminError(ex.Message);
            }
        });
        // {name} -> what the game's registry says about a component
        bridge.Register("repl_component", req =>
        {
            var name = req["name"]?.ToString() ?? throw new AdminError("name required");
            var c = Replication.Component(name);
            return c is null ? new JsonObject { ["found"] = false } : new JsonObject
            {
                ["found"] = true, ["name"] = c.Name, ["index"] = c.Index, ["size"] = c.Size, ["hash"] = c.Hash, ["replicationBit"] = c.ReplicationBit,
                ["versioned"] = c.Versioned, ["customCodec"] = c.CustomCodec, ["clientChannel"] = c.MayBeOnClientChannel,
                ["predictionChannel"] = c.MayBeOnPredictionChannel,
            };
        });
        return true;
    }
}
