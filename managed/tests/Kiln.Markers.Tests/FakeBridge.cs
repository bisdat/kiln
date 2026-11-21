// SPDX-License-Identifier: MIT
// A fake kiln.markers.bridge table and a fake transport for the built-in receiver's own code (Bridge.cs): they record what
// the bridge registers, receives and sends, and let a test hand packets to its send callback the way the native pump does.
// The protocol itself is tested natively (native/services/markers/tests/link_test.cpp).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Kiln;

namespace MarkersTests;

internal static unsafe class FakeBridge
{
    public static MarkersNative.BridgeTable* Table;
    public static readonly List<MarkersNative.Peer> Ups = new();
    public static readonly List<(uint Role, uint Peer, ulong Gen)> Downs = new();
    public static readonly List<(uint Role, uint Peer, ulong Gen, uint Message, byte[] Bytes)> Received = new();
    // what the next pump of a role hands to the send callback: (role, peer, generation, message, bytes)
    public static readonly List<(uint Role, uint Peer, ulong Gen, uint Message, byte[] Bytes)> Outgoing = new();
    public static readonly List<(uint Role, byte Result)> PumpResults = new();
    public static readonly List<uint> Pumps = new();
    public static ulong AttachedOwner;
    public static int Attaches;

    public static void Install()
    {
        Free();
        Table = (MarkersNative.BridgeTable*)NativeMemory.AllocZeroed((nuint)sizeof(MarkersNative.BridgeTable));
        Table->Header.Size = (uint)sizeof(MarkersNative.BridgeTable);
        Table->Header.Version = 1;
        Table->Attach = &Attach;
        Table->PeerUp = &PeerUp;
        Table->PeerDown = &PeerDown;
        Table->Receive = &Receive;
        Table->Pump = &Pump;
        Ups.Clear();
        Downs.Clear();
        Received.Clear();
        Outgoing.Clear();
        PumpResults.Clear();
        Pumps.Clear();
        AttachedOwner = 0;
        Attaches = 0;
    }

    public static void Free()
    {
        if (Table != null) NativeMemory.Free(Table);
        Table = null;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Attach(ulong owner)
    {
        Attaches++;
        AttachedOwner = owner;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int PeerUp(MarkersNative.Peer* p)
    {
        Ups.Add(*p);
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int PeerDown(uint role, uint peer, ulong gen)
    {
        Downs.Add((role, peer, gen));
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Receive(MarkersNative.Envelope* e)
    {
        Received.Add((e->Role, e->Peer, e->PeerGeneration, e->Message, new ReadOnlySpan<byte>(e->Bytes, (int)e->Length).ToArray()));
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Pump(uint role, uint max, delegate* unmanaged[Cdecl]<MarkersNative.Envelope*, void*, byte> send, void* user, uint* processed)
    {
        Pumps.Add(role);
        uint done = 0;
        foreach (var o in Outgoing.Where(x => x.Role == role).ToList())
        {
            Outgoing.Remove(o);
            fixed (byte* b = o.Bytes)
            {
                var e = new MarkersNative.Envelope
                {
                    Size = (uint)sizeof(MarkersNative.Envelope), Role = o.Role, Peer = o.Peer, Message = o.Message, PeerGeneration = o.Gen,
                    Bytes = b, Length = (uint)o.Bytes.Length,
                };
                var r = send(&e, user);
                PumpResults.Add((role, r));
                if (r == 1) done++;
            }
        }
        if (processed != null) *processed = done;
        return 0;
    }
}

internal sealed class FakeTransport : IMarkersTransport
{
    public bool Available { get; set; } = true;
    public int Limit = 16384;
    public MarkerRawHandler? Handler;
    public Action? ClientTick;
    public readonly List<MarkerPeerView> Peers = new();
    public MarkerPeerView? Link;
    public readonly List<(object Peer, ushort Message, byte[] Body)> Sent = new();
    public bool SendOk = true;
    public int Opens;

    public int Open(MarkerRawHandler onRaw)
    {
        Opens++;
        Handler = onRaw;
        return Limit;
    }

    public void HookClientTick(Action pump) => ClientTick = pump;
    public IReadOnlyList<MarkerPeerView> ServerPeers() => Peers.ToList();
    public MarkerPeerView? ServerLink() => Link;

    public bool Send(object peer, ushort message, ReadOnlySpan<byte> body)
    {
        Sent.Add((peer, message, body.ToArray()));
        return SendOk;
    }

    public bool IsClient { get; set; }
    public bool IsHosting { get; set; }
    public uint LocalPlayer { get; set; }
}
