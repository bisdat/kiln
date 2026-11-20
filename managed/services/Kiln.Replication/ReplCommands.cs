// SPDX-License-Identifier: MIT
// The network inspector's commands (the dossier's chapter 11 §5.2 "Surfaces"): repl stats, repl watch, repl capture,
// repl component. Admin only: they show what other players are sent. Reachable from chat, the server console, the
// control folder (kiln) and Kiln.Admin's "command" op.
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Text;

namespace Kiln;

internal static class ReplCommands
{
    private const int DefaultWatchSeconds = 30;
    private static readonly object Gate = new();
    private static readonly List<(ReplWatch Watch, Timer Timer)> CommandWatches = [];

    /// <summary>Registers the commands once (Kiln.Chat may be missing: then there are none).</summary>
    internal static void RegisterSafe()
    {
        try
        {
            Register();
        }
        catch (Exception e) when (e is FileNotFoundException or FileLoadException or TypeLoadException or MissingMethodException)
        {
            // no Kiln.Chat in this install: no commands
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Register()
    {
        var caller = ModContext.CurrentMod;
        try
        {
            ModContext.CurrentMod = null;  // Kiln's own commands outlive any mod
            var stats = Commands.Literal("stats").Describe("counters per player and channel (turns them on at the first use)")
                .Executes(c => c.Reply(StatsText()))
                .Then(Commands.Literal("off").Describe("turns Kiln's own counters off again").Executes(c => c.Reply(StatsOff())));
            var watch = Commands.Literal("watch").Describe($"logs what each player is sent of one entity, for {DefaultWatchSeconds} s (kiln.log)")
                .Then(Commands.Literal("stop").Describe("stops the watches these commands started").Executes(c => c.Reply(StopWatches())))
                .Then(Commands.Argument("entity", Arg.Int(1, int.MaxValue))
                    .Executes(c => c.Reply(StartWatch(c.Get<int>("entity"), null)))
                    .Then(Commands.Argument("peer", Arg.Word()).Executes(c => c.Reply(StartWatch(c.Get<int>("entity"), c.Get<string>("peer"))))));
            var capture = Commands.Literal("capture").Describe("writes everything one player is sent for some seconds to kiln/logs (JSON lines; tools/replinspect reads them)")
                .Then(Commands.Argument("peer", Arg.Word())
                    .Then(Commands.Argument("seconds", Arg.Int(1, 600)).Executes(c => c.Reply(Capture(c.Get<string>("peer"), c.Get<int>("seconds"))))));
            var component = Commands.Literal("component").Describe("what the game's registry says about a component (index, size, how it travels)")
                .Then(Commands.Argument("name", Arg.Word()).Executes(c => c.Reply(ComponentText(c.Get<string>("name")))));
            var root = Commands.Literal("repl").Describe("the network inspector: what the server sends each player (read only)")
                .Requires(CommandPermission.Admin)
                .Executes(c => c.Reply(Help()))
                .Then(stats).Then(watch).Then(capture).Then(component);
            Commands.Register(root, CommandPermission.Admin);
        }
        finally
        {
            ModContext.CurrentMod = caller;
        }
    }

    private static string Help()
    {
        var on = Replication.StatsOn(Replication.ServiceOwner) ? "on" : "off";
        return $"repl: the network inspector ({Capabilities.Get(ReplicationCaps.Observe).State}); Kiln's counters {on}. " +
               "repl stats [off] | repl watch <entity> [peer] | repl watch stop | repl capture <peer> <seconds> | repl component <name>";
    }

    internal static string StatsText()
    {
        Replication.RequireObserve();
        var first = !Replication.StatsOn(Replication.ServiceOwner);
        var rows = Replication.StatsFor(Replication.ServiceOwner);
        if (first) return "repl: counters on from now (Kiln's own; 'repl stats off' turns them off). Run 'repl stats' again to read them.";
        if (rows.Count == 0) return "repl: nothing sent since the counters went on (no player, or nothing relevant to anyone)";
        var sb = new StringBuilder($"repl: {rows.Count} row(s), per player and channel since the counters went on");
        foreach (var r in rows) sb.Append('\n').Append(Line(r));
        return sb.ToString();
    }

    internal static string Line(ReplPeerStats r) => string.Create(CultureInfo.InvariantCulture,
        $"{PeerName(r.Peer, r.PeerId, r.Local, r.Fake)} {r.Channel.ToString().ToLowerInvariant()}: frames {r.Frames}, messages {r.Messages} " +
        $"({r.Bytes / 1024.0:0.#} KiB), creates {r.Creates}, updates {r.Updates}, destroy lists {r.DestroyLists} ({r.DestroyedIds} ids), " +
        $"unchanged {r.Unchanged}, too big {r.TooBig}, refused {r.Refused}, acks {r.Acks}, resends asked {r.Resets}, dropped {r.Dropped}");

    internal static string PeerName(NetPeer? peer, uint id, bool local, bool fake) =>
        local ? "local client" : peer is not null ? $"{(peer.Name.Length > 0 ? peer.Name : "peer")} (0x{id:x})"
        : fake ? $"fake {id & 0x1F} (0x{id:x})" : $"peer 0x{id:x}";

    private static string StatsOff()
    {
        Replication.StopStats(Replication.ServiceOwner);
        return "repl: Kiln's own counters are off (a mod that holds them keeps them on)";
    }

    /// <summary>A peer by name (case-insensitive) or id (decimal or 0x hex, as 'repl stats' shows it).</summary>
    internal static (uint Id, NetPeer? Peer) ParsePeer(string text)
    {
        var peers = Replication.PeerTable();
        foreach (var p in peers.Values)
            if (string.Equals(p.Name, text, StringComparison.OrdinalIgnoreCase)) return (p.Id, p);
        var hex = text.StartsWith("0x", StringComparison.OrdinalIgnoreCase);
        if (uint.TryParse(hex ? text[2..] : text, hex ? NumberStyles.HexNumber : NumberStyles.Integer, CultureInfo.InvariantCulture, out var id) && id != 0)
            return (id, peers.GetValueOrDefault(id));
        throw new CommandException($"no peer '{text}' (a player's name, or a peer id as 'repl stats' shows it)");
    }

    private static string StartWatch(int entity, string? peerText)
    {
        Replication.RequireObserve();
        uint peerId = ReplNative.AllMachines;
        NetPeer? peer = null;
        if (peerText is not null) (peerId, peer) = ParsePeer(peerText);
        ReplWatch w;
        try
        {
            w = ReplWatch.Start(Replication.ServiceOwner, null, new EntityId((uint)entity), peerId, peer, ReplNative.Send);
        }
        catch (ArgumentException)
        {
            throw new CommandException($"peer {peerText} is not connected");
        }
        w.Sent += e => Log(SentLine(e));
        w.Ended += e => Log($"repl watch: entity {e.Entity.Value} ended ({e.Reason})");
        var timer = new Timer(_ => w.Dispose(), null, TimeSpan.FromSeconds(DefaultWatchSeconds), Timeout.InfiniteTimeSpan);
        lock (Gate)
        {
            CommandWatches.RemoveAll(x => !x.Watch.IsActive);
            CommandWatches.Add((w, timer));
        }
        return $"repl: watching entity {entity} on {(peerText is null ? "every peer" : peerText)} for {DefaultWatchSeconds} s: one line per message in kiln.log ('repl watch stop' ends it)";
    }

    internal static string SentLine(ReplSent e)
    {
        var sb = new StringBuilder(string.Create(CultureInfo.InvariantCulture,
            $"repl watch: entity {e.Entity.Value} to {PeerName(e.Peer, e.PeerId, e.Local, e.Fake)} {e.Channel.ToString().ToLowerInvariant()} " +
            $"{e.Kind.ToString().ToLowerInvariant()} tick {e.Tick} seq {e.Seq}"));
        if (e.BaselineTick != 0) sb.Append(CultureInfo.InvariantCulture, $" against tick {e.BaselineTick}");
        if (e.Kind != ReplKind.Destroyed) sb.Append(CultureInfo.InvariantCulture, $", {e.PayloadBytes} B sent");
        if (e.Gap) sb.Append(" (records missing before this one)");
        foreach (var (name, bytes) in e.Components)
            sb.Append(' ').Append(name).Append(e.PackedByGame.Contains(name) ? "~=" : "=").Append(Convert.ToHexString(bytes.Span));
        return sb.ToString();
    }

    private static string StopWatches()
    {
        List<(ReplWatch Watch, Timer Timer)> all;
        lock (Gate)
        {
            all = [.. CommandWatches];
            CommandWatches.Clear();
        }
        foreach (var (w, t) in all)
        {
            w.Dispose();
            t.Dispose();
        }
        return $"repl: {all.Count} watch(es) stopped";
    }

    private static string Capture(string peerText, int seconds) => CaptureFor(Replication.ServiceOwner, peerText, seconds);

    /// <summary>A capture of <paramref name="peerText"/> for <paramref name="seconds"/> into kiln/logs, held by
    /// <paramref name="owner"/> (it ends early when the owner goes). Tick thread.</summary>
    internal static string CaptureFor(ulong owner, string peerText, int seconds)
    {
        Replication.RequireObserve();
        if (seconds is < 1 or > 600) throw new CommandException("seconds is 1 to 600");
        var (id, peer) = ParsePeer(peerText);
        var name = string.Create(CultureInfo.InvariantCulture, $"repl-{id:x8}-{DateTime.UtcNow:yyyyMMddTHHmmssZ}.jsonl");
        var rc = ReplNative.Capture(owner, id, (uint)seconds, name);
        return rc switch
        {
            ReplNative.Ok => $"repl: capturing what {PeerName(peer, id, false, (id & ReplNative.FakePeer) != 0 && peer is null)} is sent for {seconds} s into kiln/logs/{name}",
            ReplNative.InvalidArgument => throw new CommandException($"peer {peerText} is not connected, or a capture runs already"),
            _ => throw new CommandException($"kiln.repl could not capture ({rc}; see kiln.log)"),
        };
    }

    private static string ComponentText(string name)
    {
        var c = Replication.Component(name);
        if (c is null) return $"repl: no component '{name}' in this world's registry (or no world yet)";
        var travels = (c.MayBeOnClientChannel, c.MayBeOnPredictionChannel) switch
        {
            (true, true) => "both channels",
            (true, false) => "the client channel",
            (false, true) => "the prediction channel",
            _ => "neither channel (not sent)",
        };
        return string.Create(CultureInfo.InvariantCulture,
            $"repl: {c.Name}: index {c.Index}, {c.Size} B, hash 0x{c.Hash:x8}; may travel on {travels}" +
            $"{(c.ReplicationBit ? "; a tracked write queues it for sending" : "; a tracked write alone does not queue it")}" +
            $"{(c.Versioned ? "; versioned" : "")}{(c.CustomCodec ? "; packed by the game's own codec (clients see it rounded)" : "")}");
    }

    private static void Log(string line)
    {
        try
        {
            Backend.Current.Log(Replication.ServiceOwner, LogLevel.Info, line);
        }
        catch
        {
            // logging never unwinds into a watch's delivery
        }
    }
}
