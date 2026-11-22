// SPDX-License-Identifier: MIT
// Kiln.Replication in the capability registry (Kiln.Capabilities; docs/kiln/services/caps.md). Reported at the service's
// start (ReplicationStartup, the first service tick); what kiln.repl can do is fixed once it is loaded.
namespace Kiln;

internal static class ReplicationCaps
{
    /// <summary>kiln.repl's table is here (the core's row).</summary>
    public const string Service = "kiln.repl";

    /// <summary>sigdb: what the server sends each peer (server side, also a hosted world).</summary>
    public const string Observe = "repl.observe";

    /// <summary>sigdb (client exe): what a Kiln client receives (Replication.WatchReceived). Its in-game check is
    /// 11-repl.receive-watch.</summary>
    public const string Client = "repl.client";

    /// <summary>sigdb: the loopback harness (<see cref="ReplicationLab"/>); lab servers only: kiln.json's policy "any"
    /// and a kiln.net lab port.</summary>
    public const string Loopback = "repl.loopback";

    /// <summary>kiln.repl.net (since 0.2.0-dev.31): a mod's own values on entities, sent to Kiln clients on Kiln's
    /// channel (Replication.Extension). Needs repl.observe's send step and kiln.net's transport; a Kiln client needs
    /// its receive side too.</summary>
    public const string Sidecar = "repl.sidecar";

    /// <summary>sigdb: per-peer snapshot filters (Replication.Filter).</summary>
    public const string Filter = "repl.filter";

    /// <summary>sigdb + kiln.world's write_other: re-send an entity without changing it (Replication.Touch).</summary>
    public const string Touch = "repl.touch";

    /// <summary>Kiln-only items shown as their look-alike (or an empty slot) to players that lack them
    /// (Replication.RemapItems); repl.filter plus kiln.repl.net's remap.</summary>
    public const string ItemRemap = "repl.itemremap";

    /// <summary>Message hooks and inject on the game's other channels (Replication.OnMessage, Replication.Inject):
    /// sigdb repl.messages.server (or .client) and the session's send functions.</summary>
    public const string Messages = "repl.messages";

    /// <summary>sigdb: the game's own message codec (Replication.Decode / Encode).</summary>
    public const string Schema = "repl.schema";

    /// <summary>The sigdb features behind repl.messages on each side.</summary>
    public const string MessagesServer = "repl.messages.server", MessagesClient = "repl.messages.client";

    internal static readonly ServiceCapRows Rows = new(Service, Report);

    private static void Report()
    {
        var client = Mods.Side == ModSide.Client;
        string[] features = client ? [Observe, Client] : [Observe, Loopback];
        if (!ReplNative.Available)
        {
            ServiceCaps.LowerFeatures(Service, features);
        }
        else
        {
            var caps = ReplNative.Capabilities();
            if ((caps & ReplNative.CapObserve) == 0)
                ServiceCaps.Lower(Observe, CapabilityReason.Faulted, "kiln.repl turned it off (see kiln.log)");
            if (client && (caps & ReplNative.CapClient) == 0)
                ServiceCaps.Lower(Client, CapabilityReason.ServiceOlder, "kiln.repl has no client receive side in this version");
            if (!client && (caps & ReplNative.CapLoopback) == 0)
                ServiceCaps.Lower(Loopback, CapabilityReason.Policy,
                    "lab servers only: needs kiln.json policy \"any\" and kiln.net's lab port (labPort)");
        }
        ReportNet(client);
        // the side a row is not for (sigdb declares it on the other side only)
        if (client) ServiceCaps.Report(Loopback, CapabilityState.Unavailable, CapabilityReason.WrongSide, "the loopback harness runs on lab servers");
        else ServiceCaps.Report(Client, CapabilityState.Unavailable, CapabilityReason.WrongSide, "a game client's receive side");
    }

    /// <summary>kiln.repl.net's rows: a sigdb feature that resolved is lowered while the native part that serves it is
    /// not there (an older kiln-repl.dll, or the part could not start); the managed rows follow their parts.</summary>
    private static void ReportNet(bool client)
    {
        var caps = ReplNetNative.Capabilities();
        string missing = ReplNetNative.Available ? "kiln.repl.net could not start it (see kiln.log)" : "kiln.repl.net is not here (an older kiln-repl.dll)";
        if ((caps & ReplNetNative.CapFilter) == 0) ServiceCaps.Lower(Filter, CapabilityReason.ServiceOlder, missing);
        if ((caps & ReplNetNative.CapTouch) == 0) ServiceCaps.Lower(Touch, CapabilityReason.ServiceOlder, missing);
        if ((caps & ReplNetNative.CapSchema) == 0) ServiceCaps.Lower(Schema, CapabilityReason.ServiceOlder, missing);
        var side = client ? MessagesClient : MessagesServer;
        if ((caps & (ReplNetNative.CapMessagesSend | ReplNetNative.CapMessagesReceive)) == 0)
            ServiceCaps.Lower(side, CapabilityReason.ServiceOlder, missing);
        // managed rows (no sigdb feature of their own)
        var track = client ? ReplNetNative.CapClientTrack : ReplNetNative.CapTrack;
        ServiceCaps.Report(Sidecar, (caps & track) != 0 ? CapabilityState.Available : CapabilityState.Unavailable,
            (caps & track) != 0 ? CapabilityReason.None : CapabilityReason.ServiceOlder, (caps & track) != 0 ? "" : missing);
        ServiceCaps.Report(ItemRemap, (caps & ReplNetNative.CapRemap) != 0 ? CapabilityState.Available : CapabilityState.Unavailable,
            (caps & ReplNetNative.CapRemap) != 0 ? CapabilityReason.None : CapabilityReason.ServiceOlder,
            (caps & ReplNetNative.CapRemap) != 0 ? "" : missing);
        var msgs = ReplNetNative.CapMessagesSend | ReplNetNative.CapMessagesReceive;
        ServiceCaps.Report(Messages, (caps & msgs) != 0 ? CapabilityState.Available : CapabilityState.Unavailable,
            (caps & msgs) != 0 ? CapabilityReason.None : CapabilityReason.ServiceOlder, (caps & msgs) != 0 ? "" : missing);
    }

    /// <summary>The exception a member throws when <paramref name="capability"/> is off; the rows are reported first.</summary>
    public static KilnUnavailableException Off(string capability, string detail)
    {
        Rows.Ensure();
        return ServiceCaps.Unavailable(capability, detail);
    }
}

/// <summary>Started by Kiln (ServiceTick.AutoStart) at its first service tick: the repl.* rows are there before a mod
/// asks for them, not from Kiln.Replication's first use.</summary>
internal static class ReplicationStartup
{
    static ReplicationStartup()
    {
        ReplicationCaps.Rows.Ensure();
        ReplCommands.RegisterSafe();  // repl stats | watch | capture | component
        ReplicationView.Start();      // the Kiln menu's Replication section
        ItemRemap.Start();            // Kiln-only items hidden from peers that lack them (kiln.repl.net)
    }
}
