// SPDX-License-Identifier: MIT
// Replication watch: what the server sends each player of one chest (docs/kiln/services/replication.md).
//   /replwatch            watch the chest nearest to you (from the console: nearest to the first player, or the first
//                         container the world has)
//   /replwatch <entity>   watch that entity
//   /replwatch stop       stop watching
// Each message the game sends a player about the chest comes as one ReplSent event, at the start of the next step, with
// the full component values the game coded (never the delta). The mod logs, per player and channel, the size of the
// Inventory component as sent and how many slots of the chest are used, when either changed since the last message to
// that player. What to take from it: the capability is asked first; the watch is one object to dispose (the unload does
// it too); Ended is the watch's last event; a player is a NetPeer, but a hosted world's own client and a lab's fake
// players come with a null Peer and are told apart by Local and Fake.
using Kiln;

namespace ReplWatchSample;

public sealed class ReplWatchMod : Mod
{
    private const double Reach = 30;  // metres around the player

    private ReplWatch? _watch;

    // per peer and channel: the Inventory bytes and the used slots of the last message (tick thread only)
    private readonly Dictionary<(uint Peer, ReplChannel Channel), (byte[] Inventory, int Used)> _last = [];

    public override void OnUnload()
    {
        // Kiln disposes a mod's watches when it unloads; doing it here says so in the mod's own code
        _watch?.Dispose();
        _watch = null;
    }

    [Command("replwatch", Description = "logs a chest's Inventory as the server sends it to each player: [entity] or stop", Permission = CommandPermission.Admin)]
    private string Watch(CommandContext c, string target = "")
    {
        if (target == "stop")
        {
            if (_watch is null) return "no watch on";
            _watch.Dispose();
            _watch = null;
            return "watch stopped";
        }
        if (!Capabilities.Is("repl.observe")) throw new CommandException($"no network inspector here: {Capabilities.Why("repl.observe")}");
        if (!Items.Available) throw new CommandException("this sample finds chests with kiln.items, which is not available here");
        EntityId chest;
        if (target.Length > 0)
        {
            if (!uint.TryParse(target, out var id) || id == 0) throw new CommandException("an entity id is a number from 1 to 4294967295, or 'stop'");
            chest = new EntityId(id);
        }
        else
        {
            chest = FindChest(c) ?? throw new CommandException("no container in this world");
        }
        _watch?.Dispose();
        _last.Clear();
        _watch = Replication.Watch(chest);  // every player; Replication.Watch(chest, peer) for one
        _watch.Sent += OnSent;
        _watch.Ended += e =>
        {
            Log.Info($"repl-watch: the watch on {e.Entity} ended ({e.Reason})");
            _watch = null;
        };
        return $"watching {chest}: one kiln.log line per player when its Inventory or used slots change ('/replwatch stop' ends it)";
    }

    /// <summary>The container nearest to the player who asked, or to the first player in the world; with no player, the
    /// first container the world has.</summary>
    private static EntityId? FindChest(CommandContext c)
    {
        var containers = Items.FindInventories(InventoryKinds.Containers);
        if (containers.Count == 0) return null;
        EntityId? player = c.Source.IsPlayer ? c.Source.Player : null;
        if (player is null && Players.Available) player = Players.Connected.FirstOrDefault(p => p.InGame)?.Entity;
        if (player is { } p && World.Available && Nearby.Within(containers, p, Reach) is { Count: > 0 } near) return near[0];
        return containers[0];
    }

    private void OnSent(ReplSent e)
    {
        var who = e.Local ? "the host's own client"
            : e.Peer is { } peer ? (peer.Name.Length > 0 ? peer.Name : $"peer 0x{e.PeerId:x}")
            : e.Fake ? $"fake player {e.PeerId & 0x1F}" : $"peer 0x{e.PeerId:x}";
        var channel = e.Channel == ReplChannel.Client ? "client" : "prediction";
        if (e.Kind == ReplKind.Destroyed)
        {
            Log.Info($"repl-watch: {e.Entity} destroyed, as sent to {who} ({channel} channel)");
            return;
        }
        // The values are the ones the message was coded from. The used slots are read now (the start of the next step),
        // from the chest's live inventory: close to the message, not from its bytes.
        var inventory = e.Components.TryGetValue("Inventory", out var bytes) ? bytes.ToArray() : null;
        var used = Items.Read(e.Entity)?.UsedSlots ?? -1;
        var key = (e.PeerId, e.Channel);
        var had = _last.TryGetValue(key, out var before);
        if (inventory is not null) _last[key] = (inventory, used);
        if (inventory is null)
        {
            if (e.Kind == ReplKind.Create) Log.Info($"repl-watch: {e.Entity} created for {who} without an Inventory component ({string.Join(", ", e.Components.Keys)})");
            return;
        }
        if (had && before.Used == used && before.Inventory.AsSpan().SequenceEqual(inventory)) return;
        var slots = had && before.Used != used ? $"{before.Used} -> {used}" : $"{used}";
        Log.Info($"repl-watch: {e.Entity} {e.Kind.ToString().ToLowerInvariant()} tick {e.Tick} as sent to {who} ({channel} channel): " +
                 $"Inventory {inventory.Length} B, used slots {slots}, message {e.PayloadBytes} B{(e.Gap ? " (earlier messages missing)" : "")}");
    }
}
