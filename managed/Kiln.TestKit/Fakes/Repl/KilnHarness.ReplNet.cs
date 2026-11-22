// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessReplNet? _replNet;

    /// <summary>Script and inspect the fake kiln.repl.net service used by Kiln.Replication.</summary>
    public HarnessReplNet ReplNet => _replNet ??= new HarnessReplNet(this);
}

/// <summary>A field or strided array of little-endian item IDs in a scripted replication component.</summary>
public readonly struct FakeReplItemSite(string component, ushort offset, ushort count = 1, ushort stride = 0)
{
    /// <summary>The component registered with <see cref="HarnessRepl.AddComponent"/>.</summary>
    public string Component { get; } = component;

    /// <summary>The first item's byte offset.</summary>
    public ushort Offset { get; } = offset;

    /// <summary>How many 32-bit IDs this site covers.</summary>
    public ushort Count { get; } = count;

    /// <summary>Byte spacing between IDs when the count is greater than one.</summary>
    public ushort Stride { get; } = stride;
}

/// <summary>The fixture and recordings behind Kiln.Replication's filters, touches, item remap and game message hooks.</summary>
public sealed class HarnessReplNet
{
    private readonly KilnHarness _harness;

    internal HarnessReplNet(KilnHarness harness) => _harness = harness;

    private ReplFakeState State => ReplFake.StateOf(_harness);

    /// <summary>Connects a peer to <see cref="KilnHarness.Repl"/> and gives it an identity for replication callbacks and
    /// injection. A peer with <paramref name="vanilla"/> false stands for a ready Kiln client with current content.</summary>
    public NetPeer Connect(uint machine, uint peer, bool vanilla = true)
    {
        var net = new NetPeer(peer) { State = vanilla ? NetPeerState.Vanilla : NetPeerState.Ready };
        _harness.Repl.Connect(machine, peer, net);
        return net;
    }

    /// <summary>Touch requests in call order, including requests for missing entities or entities without a carrier.</summary>
    public IReadOnlyList<EntityId> Touches
    {
        get { var repl = State; lock (repl.Lock) return repl.Net.Touches.ToArray(); }
    }

    /// <summary>Inject attempts in call order, with copies of the requested bytes, including refused requests.</summary>
    public IReadOnlyList<(uint Peer, GameChannel Channel, byte[] Bytes)> Injections
    {
        get
        {
            var repl = State;
            lock (repl.Lock) return repl.Net.Injections.Select(i => (i.Peer, i.Channel, (byte[])i.Bytes.Clone())).ToArray();
        }
    }

    /// <summary>Supplies the test's verdict for injected bytes and send rewrites. Without a verdict, inject answers
    /// unavailable and a send rewrite keeps the original bytes.</summary>
    public Func<GameChannel, ReadOnlyMemory<byte>, bool>? ValidateSend
    {
        get { var repl = State; lock (repl.Lock) return repl.Net.ValidateSend; }
        set { var repl = State; lock (repl.Lock) repl.Net.ValidateSend = value; }
    }

    /// <summary>Replaces the item remap fixture. Register the components first, then step the harness so Kiln resolves
    /// the peer selectors. Use an empty sites list to turn the remap off, and a default item ID as the look-alike for an
    /// item that should be blanked. Unlisted non-game IDs are also blanked.</summary>
    public unsafe void ConfigureItemRemap(IReadOnlyList<FakeReplItemSite> sites, IReadOnlyCollection<ItemId> gameItems,
        IReadOnlyDictionary<ItemId, ItemId> lookAlikes)
    {
        ArgumentNullException.ThrowIfNull(sites);
        ArgumentNullException.ThrowIfNull(gameItems);
        ArgumentNullException.ThrowIfNull(lookAlikes);

        _harness.Run(() =>
        {
            var repl = State;
            var nativeSites = new ReplNetNative.ItemSite[sites.Count];
            lock (repl.Lock)
                for (var i = 0; i < nativeSites.Length; i++)
                {
                    var site = sites[i];
                    ArgumentException.ThrowIfNullOrEmpty(site.Component);
                    var hash = ReplAbi.ComponentHash(site.Component);
                    if (!repl.Registry.ContainsKey(hash))
                        throw new ArgumentException($"register component {site.Component} with Repl.AddComponent first", nameof(sites));
                    nativeSites[i] = new ReplNetNative.ItemSite { ComponentHash = hash, Offset = site.Offset, Count = site.Count, Stride = site.Stride };
                }
            var ids = gameItems.Select(id => id.Value).ToArray();
            var map = lookAlikes.Select(p => new ReplNetNative.ItemMap { From = p.Key.Value, To = p.Value.Value }).ToArray();
            var table = ReplNetNative.T;
            if (table == null) throw ReplicationCaps.Off(ReplicationCaps.ItemRemap, "the scripted item remap is off");
            int result;
            fixed (ReplNetNative.ItemSite* siteData = nativeSites)
            fixed (uint* gameData = ids)
            fixed (ReplNetNative.ItemMap* mapData = map)
                result = table->RemapSet(siteData, (uint)nativeSites.Length, gameData, (uint)ids.Length, mapData, (uint)map.Length);
            if (result == FakeTables.Unavailable) throw ReplicationCaps.Off(ReplicationCaps.ItemRemap, "the scripted item remap is off");
            if (result != FakeTables.Ok)
                throw new ArgumentException("the remap needs in-bounds sites, game IDs, and look-alikes that are game IDs or zero");
        });
    }

    /// <summary>Runs the registered handlers on a scripted game message and returns the resulting bytes, or null if
    /// dropped. Use a connected peer ID on the server and peer zero on a joined-client harness.</summary>
    public byte[]? DriveMessage(uint peer, GameChannel channel, MessageDirection direction, ReadOnlySpan<byte> bytes)
    {
        var copy = bytes.ToArray();
        return _harness.Run(() => ReplNetFake.DriveMessage(_harness, peer, channel, direction, copy));
    }
}
