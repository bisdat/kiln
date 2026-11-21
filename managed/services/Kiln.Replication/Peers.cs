// SPDX-License-Identifier: MIT
// Which peers a replication rule applies to (the dossier's chapter 15 §5.2 "PeerSelector"; DESIGN "Public managed API"):
// a sidecar's peers, a per-peer filter's, the item remap's. A selector is a rule, not a list: it is asked again for every
// peer whenever its owner looks (Kiln's own parts once per step on the tick thread), so a peer that finishes its
// handshake, acknowledges the content state or loses an extension moves between the sets by itself.
namespace Kiln;

/// <summary>
/// The peers a replication rule applies to: <see cref="Peers.All"/>, <see cref="Peers.Vanilla"/>,
/// <see cref="Peers.With"/>, <see cref="Peers.Without"/>, <see cref="Peers.LackingContent"/>. A selector is a rule, asked
/// again for every peer each time Kiln applies it (once per step on the tick thread), so a peer moves in or out by itself
/// when its state changes (it finishes the Kiln handshake, acknowledges the server's content, loses an extension).
/// </summary>
/// <remarks>
/// <para>Besides players' connections a server replicates to two kinds of machine without a <see cref="NetPeer"/>: a hosted
/// world's own client (machine 0: the same process, so it has every extension and all content) and, on lab servers, a
/// loopback fake (ReplicationLab) that no kiln.net lab peer stands for (it has no Kiln: a vanilla peer).</para>
/// </remarks>
public sealed class PeerSelector
{
    private readonly Func<PeerFacts, bool> _match;

    internal PeerSelector(string description, Func<PeerFacts, bool> match)
    {
        Description = description;
        _match = match;
    }

    /// <summary>What the selector picks, in words ("vanilla peers", "peers with bigcan:water@1").</summary>
    public string Description { get; }

    /// <summary>Whether <paramref name="peer"/> is one of the selected peers now (a client connection of this server).</summary>
    /// <exception cref="ArgumentNullException"><paramref name="peer"/> is null.</exception>
    [ServerOnly]
    [TickThreadOnly]
    public bool Matches(NetPeer peer)
    {
        ArgumentNullException.ThrowIfNull(peer);
        return _match(PeerFacts.Of(peer));
    }

    /// <summary>Kiln's own parts: one machine's peer as kiln.repl names it (a lab fake, a hosted world's own client).</summary>
    internal bool Matches(in PeerFacts peer) => _match(peer);

    /// <inheritdoc/>
    public override string ToString() => Description;
}

/// <summary>A machine's peer as a selector sees it: kiln.repl's peer id, its kiln.net peer (null for a hosted world's own
/// client and an unbound lab fake), and whether it is the local client or a lab fake.</summary>
internal readonly record struct PeerFacts(uint Id, NetPeer? Peer, bool Local, bool Fake)
{
    /// <summary>A client connection of this server.</summary>
    internal static PeerFacts Of(NetPeer peer) =>
        new(peer.Id, peer, !peer.IsServer && !peer.IsLab && (peer.Id & 0x7F) == 0, false);

    /// <summary>One machine's peer as kiln.repl reports it (a stats row, a track event): KILN_REPL_PEER_* flags.</summary>
    internal static PeerFacts Of(uint id, byte replFlags, IReadOnlyDictionary<uint, NetPeer> peers)
    {
        var local = (replFlags & ReplNative.PeerLocal) != 0;
        var fake = (replFlags & ReplNative.PeerFake) != 0;
        NetPeer? peer = null;
        if (!local && (!fake || (replFlags & ReplNative.PeerBound) != 0)) peer = peers.GetValueOrDefault(id);
        return new PeerFacts(id, peer, local, fake);
    }

    /// <summary>The peer has Kiln and its handshake is done.</summary>
    internal bool Kiln => Peer is { State: NetPeerState.Ready };
}

/// <summary>The selectors of Kiln's replication rules (<see cref="PeerSelector"/>).</summary>
public static class Peers
{
    /// <summary>Every peer.</summary>
    public static PeerSelector All { get; } = new("all peers", static _ => true);

    /// <summary>Peers without Kiln: clients whose Kiln handshake did not finish (a vanilla client, or one still
    /// connecting) and lab fakes no kiln.net lab peer stands for. A hosted world's own client is never one.</summary>
    public static PeerSelector Vanilla { get; } = new("vanilla peers", static p => !p.Local && !p.Kiln);

    /// <summary>Peers that may lack content only Kiln has (Kiln-only items, templates): every vanilla peer, and every
    /// Kiln client that has not acknowledged the content state the server serves now (kiln:content). A hosted world's own
    /// client shares the server's content and never lacks it.</summary>
    public static PeerSelector LackingContent { get; } = new("peers lacking the server's content", static p => !p.Local && !HasContent(p));

    /// <summary>Peers that have <paramref name="extension"/>: Kiln clients whose handshake advertised the same name,
    /// version, size and layout (<see cref="ReplicatedExtension.IsAvailableFor"/>), and a hosted world's own client
    /// while the extension is registered.</summary>
    /// <exception cref="ArgumentNullException"><paramref name="extension"/> is null.</exception>
    public static PeerSelector With(ReplicatedExtension extension)
    {
        ArgumentNullException.ThrowIfNull(extension);
        return new PeerSelector($"peers with {extension}", p => Has(extension, p));
    }

    /// <summary>Peers that do not have <paramref name="extension"/> (the rest of <see cref="With"/>): vanilla peers, Kiln
    /// clients without it or with another version, size or layout of it.</summary>
    /// <exception cref="ArgumentNullException"><paramref name="extension"/> is null.</exception>
    public static PeerSelector Without(ReplicatedExtension extension)
    {
        ArgumentNullException.ThrowIfNull(extension);
        return new PeerSelector($"peers without {extension}", p => !Has(extension, p));
    }

    private static bool Has(ReplicatedExtension extension, in PeerFacts p) =>
        p.Local ? extension.IsRegistered : p.Kiln && extension.AvailableFor(p.Peer!);

    /// <summary>How a Kiln client's content state is judged (Kiln.Content's acknowledgement, through kiln.net's per-peer
    /// clear): replaced by the part that knows better (tests, the item remap).</summary>
    internal static Func<NetPeer, bool> ContentCurrent = DefaultContentCurrent;

    private static bool HasContent(in PeerFacts p) => p.Kiln && ContentCurrent(p.Peer!);

    /// <summary>A Kiln client is current when it acknowledged the content state served now (kiln.net cleared it for every
    /// Kiln-only template there is: Kiln.Network's PeerHasDefinitions), or when the server serves no content state and has
    /// no Kiln-only template: then there is nothing it could lack and nothing to acknowledge.</summary>
    private static bool DefaultContentCurrent(NetPeer peer)
    {
        var templates = NetNative.ReadGate().KilnTemplates;
        if (peer.Cleared && peer.ClearedTemplates >= templates) return true;
        return templates == 0 && !ServingContent();
    }

    /// <summary>Whether this server serves a content state (Kiln.Content's export); replaced by tests.</summary>
    internal static Func<bool> ServingContent = static () =>
    {
        try
        {
            return Serving();
        }
        catch (Exception e) when (e is TypeLoadException or FileNotFoundException or TypeInitializationException)
        {
            return false;  // no Kiln.Content: no content state either
        }
    };

    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    private static bool Serving() => ContentSync.Serving is not null;
}
