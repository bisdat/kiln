// SPDX-License-Identifier: MIT
// kiln.net's Steam peer-to-peer messages members (native/core/abi/services/kiln_net.h, KilnNetApi: 184 bytes, the members
// at 128..176), byte-exact with the header: Kiln.Network.Tests' SteamMessagesTests check every size and offset. Every
// member that takes a lease takes its owner first (the mod's native owner); kiln.net answers another owner's lease as
// a closed one.
using System.Runtime.InteropServices;

namespace Kiln;

internal static unsafe class SteamNative
{
    public const string Service = "kiln.net";

    // KilnResult (kiln_api.h) and kiln_net.h's own code
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, Revoked = 6, Internal = 7;
    public const int ErrInvalidData = 210;  // KILN_STEAM_ERR_INVALID_DATA

    public const uint OpenSessions = 1;     // KILN_STEAM_OPEN_SESSIONS
    public const uint SendReliable = 8;     // KILN_STEAM_SEND_RELIABLE
    public const uint MaxChannel = 65535;

    /// <summary>KilnSteamMessagesStatus (48 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Status
    {
        public uint Size, Flags;
        public ulong LocalSteamId, SessionsAccepted, SessionsRefused;
        public uint AcceptMode, Listed;
        public ulong Reserved;
    }

    /// <summary>KilnSteamMessagesOpen (16 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Open
    {
        public uint Size, Flags, FirstChannel, LastChannel;
    }

    /// <summary>KilnSteamAcceptPolicy (24 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct AcceptPolicy
    {
        public uint Size, Mode;
        public ulong* Ids;
        public uint Count, Reserved;
    }

    /// <summary>KilnSteamMessageInfo (24 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct MessageInfo
    {
        public uint Size, Length;
        public ulong PeerSteamId;
        public uint Channel, Flags;
    }

    /// <summary>KilnNetApi with the Steam messages members (184 bytes). The first fifteen members are NetNative's.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public nint NetStatus, Subscribe, Unsubscribe, Send, Peers, PeerInfo, Kick, PeerOfEntity, GateSet, GateInfo, GateRelease,
            EntityWithheld, TemplateKilnOnly, WithholdSet, LabSelftest;
        public delegate* unmanaged[Cdecl]<Status*, int> SteamStatus;
        public delegate* unmanaged[Cdecl]<ulong, Open*, ulong*, int> SteamOpen;
        // (owner, lease, ...): the owner that opened the lease
        public delegate* unmanaged[Cdecl]<ulong, ulong, int> SteamClose;
        public delegate* unmanaged[Cdecl]<ulong, ulong, AcceptPolicy*, Status*, int> SteamMaintain;
        public delegate* unmanaged[Cdecl]<ulong, ulong, ulong, void*, uint, uint, uint, int*, int> SteamSend;
        public delegate* unmanaged[Cdecl]<ulong, ulong, uint, void*, uint, MessageInfo*, uint*, int> SteamReceive;
        public delegate* unmanaged[Cdecl]<ulong, ulong, ulong, uint*, int> SteamAccept;
    }

    // The table found under a backend (a test swaps backends; a game has one): asked once per backend while it is there.
    private sealed record Found(IKilnBackend Backend, nint Table);
    private static volatile Found? _found;

    /// <summary>kiln.net's table (any size), or null.</summary>
    public static ServiceHeader* Header
    {
        get
        {
            var backend = Backend.Current;
            if (_found is { } found && ReferenceEquals(found.Backend, backend)) return (ServiceHeader*)found.Table;
            var h = Services.Get<ServiceHeader>(Service, 1);
            if (h != null) _found = new Found(backend, (nint)h);
            return h;
        }
    }

    /// <summary>The table when it has the Steam messages members (header.size of at least 184), else null.</summary>
    public static Table* Members
    {
        get
        {
            var h = Header;
            return h != null && h->Size >= (uint)sizeof(Table) ? (Table*)h : null;
        }
    }
}
