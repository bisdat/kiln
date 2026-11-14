// SPDX-License-Identifier: MIT
// Mirror of native/core/abi/services/kiln_markers.h (kiln.markers v1 and the internal kiln.markers.bridge v1). Sizes are
// checked by Kiln.Markers.Tests (AbiTests) against the header's v1 layouts.
using System.Runtime.InteropServices;

namespace Kiln;

internal static unsafe class MarkersNative
{
    public const string Service = "kiln.markers";
    public const string BridgeService = "kiln.markers.bridge";
    public const uint Version = 1, BridgeVersion = 1;

    // KilnResult
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, WrongThread = 4, BufferTooSmall = 5;

    // KilnMarkerResult.status
    public const uint Accepted = 0, StatusNotFound = 1, Full = 2, RequiresClient = 3, NotReady = 4, Partial = 5,
        InvalidAnchor = 6, CatalogNotReady = 7, Ambiguous = 8;

    // KilnMarkerChange.cause
    public const uint CauseApi = 0, CauseOwnerRetired = 1, CauseWorldStopped = 2, CausePeerLeft = 3, CauseSnapshot = 4,
        CauseLinkStale = 5;

    [StructLayout(LayoutKind.Sequential)]
    public struct Vec3
    {
        public double X, Y, Z;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Spec
    {
        public uint Size, Surfaces;
        public NativeStr Key;
        public Vec3 Position;
        public ulong Follow;
        public float Height, MaxDistance;
        public uint IconType, Rgba;
        public int Priority;
        public uint MapLabelMode;
        public NativeStr Text;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Result
    {
        public uint Size, Status, Committed, Queued, Refused, Affected;
        public fixed byte Reason[256];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct AnchorResult
    {
        public uint Size, Status;
        public ulong Anchor;
        public ulong WorldEpoch, Incarnation;
        public uint WorldKind, Entity;
        public Vec3 Position;
        public fixed byte Reason[256];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Icon
    {
        public uint Size, Id, Flags, Surfaces;
        public fixed byte TextureGuid[16];
        public fixed byte Key[14];
        public fixed byte Alias[129];
        public fixed byte TextureName[129];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Change
    {
        public uint Size, Kind, Audience, Recipient;
        public ulong WorldEpoch, PeerGeneration, PublisherGeneration, Sequence;
        public uint Affected, Cause;
        public fixed byte Owner[65];
        public fixed byte Key[65];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Visibility
    {
        public uint Size, Audience, Recipient, Initial;
        public ulong WorldEpoch, PeerGeneration, PublisherGeneration, Sequence;
        public uint Requested, Before, After;
        public fixed uint Reason[3];
        public fixed byte Owner[65];
        public fixed byte Key[65];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct CatalogChange
    {
        public uint Size, Role, Ready, Count;
        public ulong Generation;
        public fixed byte Reason[256];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, NativeStr, int> BindOwner;
        public delegate* unmanaged[Cdecl]<ulong, int> ReleaseOwner;
        public delegate* unmanaged[Cdecl]<uint*, ulong*, int> CatalogCount;
        public delegate* unmanaged[Cdecl]<uint, Icon*, int> CatalogAt;
        public delegate* unmanaged[Cdecl]<NativeStr, Icon*, Result*, int> CatalogResolve;
        public delegate* unmanaged[Cdecl]<uint, ulong*, int> WorldEpoch;
        public delegate* unmanaged[Cdecl]<ulong, uint, ulong, uint, AnchorResult*, int> CaptureAnchor;
        public delegate* unmanaged[Cdecl]<ulong, ulong, int> ReleaseAnchor;
        public delegate* unmanaged[Cdecl]<ulong, Spec*, Result*, int> SetLocal;
        public delegate* unmanaged[Cdecl]<ulong, NativeStr, Result*, int> RemoveLocal;
        public delegate* unmanaged[Cdecl]<ulong, Result*, int> ClearLocal;
        public delegate* unmanaged[Cdecl]<ulong, uint, Spec*, Result*, int> Send;
        public delegate* unmanaged[Cdecl]<ulong, uint, NativeStr, Result*, int> Unsend;
        public delegate* unmanaged[Cdecl]<ulong, uint, Result*, int> ClearSent;
        public delegate* unmanaged[Cdecl]<ulong, uint, NativeStr, Result*, int> SuppressSent;
        public delegate* unmanaged[Cdecl]<ulong, delegate* unmanaged[Cdecl]<Change*, void*, void>,
            delegate* unmanaged[Cdecl]<Visibility*, void*, void>, delegate* unmanaged[Cdecl]<CatalogChange*, void*, void>,
            void*, ulong*, int> Subscribe;
        public delegate* unmanaged[Cdecl]<ulong, int> Unsubscribe;
    }

    // ---- kiln.markers.bridge (internal; used by the built-in receiver) ----

    [StructLayout(LayoutKind.Sequential)]
    public struct Peer
    {
        public uint Size, Role, Id, Route, Entity, MaxMessageBytes, Flags, Reserved;  // Id: the header's `peer`
        public ulong Generation;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Envelope
    {
        public uint Size, Role, Peer, Message;
        public ulong PeerGeneration;
        public byte* Bytes;
        public uint Length, Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct BridgeTable
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, int> Attach;
        public delegate* unmanaged[Cdecl]<Peer*, int> PeerUp;
        public delegate* unmanaged[Cdecl]<uint, uint, ulong, int> PeerDown;
        public delegate* unmanaged[Cdecl]<Envelope*, int> Receive;
        public delegate* unmanaged[Cdecl]<uint, uint, delegate* unmanaged[Cdecl]<Envelope*, void*, byte>, void*, uint*, int> Pump;
    }

    private static Table* _table;

    /// <summary>The service's table (header.size covers v1), or null. Looked up again while it is missing.</summary>
    public static Table* T => _table != null ? _table : (_table = Services.Get<Table>(Service, Version));

    /// <summary>Tests: look the table up again.</summary>
    internal static void Forget() => _table = null;

    /// <summary>A NUL-terminated UTF-8 field of at most <paramref name="max"/> bytes.</summary>
    public static string Text(byte* p, int max)
    {
        var n = 0;
        while (n < max && p[n] != 0) n++;
        return n == 0 ? "" : System.Text.Encoding.UTF8.GetString(p, n);
    }
}
