// SPDX-License-Identifier: MIT
// Kiln.SteamMessages: a mod's own Steam peer-to-peer messages (ISteamNetworkingMessages) between game processes, by Steam
// id, separate from the game connection and from Kiln.Network's channels. Native side: kiln.net's steam_* members
// (native/core/abi/services/kiln_net.h; SteamNative.cs). Since 0.2.0-dev.30.3.
using System.Text;

namespace Kiln;

/// <summary>What kiln.net's Steam messages status says (kiln_net.h's <c>KILN_STEAM_MSG_*</c>). Since 0.2.0-dev.30.3.</summary>
[Flags]
public enum SteamMessagesFlags : uint
{
    /// <summary>Nothing.</summary>
    None = 0,

    /// <summary>Steam is initialised in this process and its messages interface resolves.</summary>
    SteamReady = 1u << 0,

    /// <summary>The GameServer interfaces are used (a dedicated server); otherwise the user's.</summary>
    GameServer = 1u << 1,

    /// <summary><see cref="SteamMessagesStatus.LocalSteamId"/> is this process's Steam id.</summary>
    HasIdentity = 1u << 2,

    /// <summary>Steam's session requests are answered: the session callback is registered and the accept policy is
    /// not <see cref="SteamAcceptMode.None"/>.</summary>
    SessionsAnswered = 1u << 3,

    /// <summary>A lease that answers session requests is open (any owner's).</summary>
    SessionLeaseOpen = 1u << 4,

    /// <summary>The game is exiting: every call answers <see cref="SteamMessagesRefusal.ClosedForExit"/>.</summary>
    ClosedForExit = 1u << 5,
}

/// <summary>Whom a session-answering lease accepts (kiln_net.h's <c>KILN_STEAM_ACCEPT_*</c>). Since 0.2.0-dev.30.3.</summary>
public enum SteamAcceptMode : uint
{
    /// <summary>No session request is answered (Steam lets it time out).</summary>
    None = 0,

    /// <summary>Every Steam user is accepted.</summary>
    Any = 1,

    /// <summary>Only the listed Steam ids are accepted.</summary>
    Listed = 2,
}

/// <summary>How a message is sent (Steam's <c>k_nSteamNetworkingSend_*</c>; kiln_net.h's <c>KILN_STEAM_SEND_*</c>). Since
/// 0.2.0-dev.30.3.</summary>
[Flags]
public enum SteamSendFlags : uint
{
    /// <summary>Unreliable: may be lost or arrive out of order.</summary>
    Unreliable = 0,

    /// <summary>Reliable and in order.</summary>
    Reliable = 8,

    /// <summary>A session with the peer that failed is opened again instead of failing every later send.</summary>
    RestartBrokenSession = 32,
}

/// <summary>Why a Steam messages call did not do what it was asked. Since 0.2.0-dev.30.3.</summary>
public enum SteamMessagesRefusal : uint
{
    /// <summary>Not refused.</summary>
    None = 0,

    /// <summary><see cref="SteamMessageLease.Receive"/>: no message waits on the channel.</summary>
    NoMessage = 1,

    /// <summary>Steam is not ready in this process (yet): nothing was sent or taken, a policy other than
    /// <see cref="SteamAcceptMode.None"/> is not in force.</summary>
    SteamNotReady = 2,

    /// <summary><see cref="SteamMessageLease.Send"/>: Steam's answer was not OK (<see cref="SteamSendResult.SteamResult"/>);
    /// <see cref="SteamMessageLease.Accept"/>: Steam did not accept the session.</summary>
    SteamRefused = 3,

    /// <summary><see cref="SteamMessageLease.Receive"/>: Steam's next message on the channel was bigger than the buffer or
    /// had no Steam sender. It was taken off Steam's queue and dropped.</summary>
    Dropped = 4,

    /// <summary>The lease is closed (disposed, or its mod unloaded).</summary>
    LeaseClosed = 5,

    /// <summary>The game is exiting.</summary>
    ClosedForExit = 6,

    /// <summary><see cref="SteamMessages.Open"/>: kiln.net does not open Steam message leases for the calling mod (it
    /// admits a mod whose kiln.mod.json says <c>"steamMessages": true</c>, through the owner Kiln made for it; kiln.log says
    /// <c>steam-messages-owner</c>), or the mod is unloading.</summary>
    NotPermitted = 7,

    /// <summary><see cref="SteamMessages.Open"/>: the channels overlap an open lease, a session-answering lease is open
    /// already, or all <see cref="SteamMessages.MaxLeases"/> leases are open.</summary>
    Taken = 8,

    /// <summary>A Steam call faulted (kiln.log has one line per kind), or kiln.net answered an internal error.</summary>
    Fault = 9,
}

/// <summary>One read of kiln.net's Steam messages status (<see cref="SteamMessages.Status"/>). Since 0.2.0-dev.30.3.</summary>
/// <param name="Answered">kiln.net filled the status. False when this Kiln has no Steam messages here (no kiln.net, an
/// older Kiln) or the read failed; <paramref name="Reason"/> says which.</param>
/// <param name="Flags">What there is.</param>
/// <param name="LocalSteamId">This process's Steam id with <see cref="SteamMessagesFlags.HasIdentity"/>, else 0.</param>
/// <param name="SessionsAccepted">Session requests the session callback accepted since the game started.</param>
/// <param name="SessionsRefused">Session requests it left unanswered (the policy, a bad identity, the exit, Steam said
/// no).</param>
/// <param name="AcceptMode">The accept policy in force.</param>
/// <param name="ListedIds">Steam ids in a <see cref="SteamAcceptMode.Listed"/> policy, else 0.</param>
/// <param name="Reason">Why the read is not a plain answer (not answered, the game is exiting, a Steam call faulted);
/// null otherwise.</param>
public readonly record struct SteamMessagesStatus(bool Answered, SteamMessagesFlags Flags, ulong LocalSteamId,
    long SessionsAccepted, long SessionsRefused, SteamAcceptMode AcceptMode, int ListedIds, string? Reason)
{
    /// <summary><see cref="Flags"/> has <see cref="SteamMessagesFlags.SteamReady"/>.</summary>
    public bool SteamReady => (Flags & SteamMessagesFlags.SteamReady) != 0;

    /// <summary><see cref="Flags"/> has <see cref="SteamMessagesFlags.ClosedForExit"/>.</summary>
    public bool ClosedForExit => (Flags & SteamMessagesFlags.ClosedForExit) != 0;
}

/// <summary>The header of one received message (<see cref="SteamReceiveResult.Message"/>); its bytes are the first
/// <see cref="Length"/> bytes of the buffer given to <see cref="SteamMessageLease.Receive"/>. Since 0.2.0-dev.30.3.</summary>
/// <param name="Sender">The sender's Steam id.</param>
/// <param name="Channel">The Steam message channel it came on.</param>
/// <param name="Length">Bytes copied into the buffer.</param>
/// <param name="Reliable">It came reliably.</param>
public readonly record struct SteamMessageInfo(ulong Sender, ushort Channel, int Length, bool Reliable);

/// <summary>The answer of <see cref="SteamMessages.Open"/>. Since 0.2.0-dev.30.3.</summary>
/// <param name="Ok">True when the lease is open.</param>
/// <param name="Reason">Why not; null when open.</param>
/// <param name="Refusal">Why not, as a value; <see cref="SteamMessagesRefusal.None"/> when open.</param>
/// <param name="Lease">The lease; null when refused.</param>
public sealed record SteamLeaseResult(bool Ok, string? Reason, SteamMessagesRefusal Refusal, SteamMessageLease? Lease) : IKilnResult;

/// <summary>The answer of <see cref="SteamMessageLease.Send"/> (a struct: the per-message path allocates nothing). Since
/// 0.2.0-dev.30.3.</summary>
/// <param name="Ok">True when Steam took the message (its EResult was 1, OK).</param>
/// <param name="Reason">Why not; null when sent.</param>
/// <param name="Refusal">Why not, as a value.</param>
/// <param name="SteamResult">Steam's EResult when Steam was called (1 = OK), else 0.</param>
public readonly record struct SteamSendResult(bool Ok, string? Reason, SteamMessagesRefusal Refusal, int SteamResult) : IKilnResult;

/// <summary>The answer of <see cref="SteamMessageLease.Receive"/> (a struct: the per-message path allocates nothing). Since
/// 0.2.0-dev.30.3.</summary>
/// <param name="Ok">True when one message was taken and copied into the buffer.</param>
/// <param name="Reason">Why not; null when received.</param>
/// <param name="Refusal">Why not, as a value: <see cref="SteamMessagesRefusal.NoMessage"/> when nothing waits.</param>
/// <param name="Message">The message's header when <paramref name="Ok"/>, else default.</param>
public readonly record struct SteamReceiveResult(bool Ok, string? Reason, SteamMessagesRefusal Refusal, SteamMessageInfo Message) : IKilnResult;

/// <summary>The answer of <see cref="SteamMessageLease.Accept"/>. Since 0.2.0-dev.30.3.</summary>
/// <param name="Ok">True when Steam accepted the session.</param>
/// <param name="Reason">Why not; null when accepted.</param>
/// <param name="Refusal">Why not, as a value.</param>
public readonly record struct SteamAcceptResult(bool Ok, string? Reason, SteamMessagesRefusal Refusal) : IKilnResult;

/// <summary>The answer of <see cref="SteamMessageLease.SetAcceptPolicy"/>. Since 0.2.0-dev.30.3.</summary>
/// <param name="Ok">True when the policy is in force.</param>
/// <param name="Reason">Why not; null when in force.</param>
/// <param name="Refusal">Why not, as a value.</param>
/// <param name="Status">kiln.net's status right after the call.</param>
public sealed record SteamPolicyResult(bool Ok, string? Reason, SteamMessagesRefusal Refusal, SteamMessagesStatus Status) : IKilnResult;

/// <summary>
/// A mod's own Steam peer-to-peer messages (Steam's ISteamNetworkingMessages) between game processes, by Steam id:
/// separate from the game connection and from Kiln.Network's channels. Open a <see cref="SteamMessageLease"/> on a range
/// of Steam message channels, then send and receive on it. Since 0.2.0-dev.30.3.
/// </summary>
/// <remarks>
/// <para>Works on a game client (the player's Steam user) and on a dedicated server (the GameServer interfaces). The game
/// pumps Steam's callbacks; Kiln never initialises or pumps Steam.</para>
/// <para>Threads: every member may be called from any thread and never waits; each makes at most one Steam call that moves
/// data, or answers at once. <see cref="Open"/> needs the calling mod (any code Kiln runs has it), from OnLoad on: it
/// throws in OnBoot.</para>
/// <para>Opt-in: only a mod whose kiln.mod.json says <c>"steamMessages": true</c> (<see cref="ModInfo.SteamMessages"/>)
/// opens leases; <see cref="Open"/> throws <see cref="InvalidOperationException"/> for any other mod.</para>
/// <para>Capability <c>net.steam_messages</c>: pending until a call first finds Steam ready in this process, then
/// available. <see cref="Open"/> throws <see cref="KilnUnavailableException"/> only when it is unavailable (safe idle,
/// kiln-net.dll not started, steam_api64.dll missing, a Steam export missing) or this Kiln has no Steam messages; a lease
/// opened while it is pending answers <see cref="SteamMessagesRefusal.SteamNotReady"/> until Steam is ready.</para>
/// <para>Leases never overlap, across every mod and Kiln's own runtimes, and at most <see cref="MaxLeases"/> are open in
/// the process. A lease belongs to the mod that opened it and closes when that mod unloads.</para>
/// <para>Sessions: Steam accepts a session with a peer this process sent to first. To receive from a peer that writes
/// first, open one lease with <c>answerSessions</c> and set its accept policy, or accept a pending request by hand
/// (<see cref="SteamMessageLease.Accept"/>). Only one lease in the process answers session requests.</para>
/// </remarks>
/// <example><code>
/// var open = SteamMessages.Open(4100, 4103);
/// if (!open.Ok) { Log.Info(open.Reason!); return; }
/// using var lease = open.Lease!;
/// lease.Send(friendSteamId, 4100, Encoding.UTF8.GetBytes("hello"));
/// var buffer = new byte[SteamMessages.MaxMessageBytes];
/// for (var r = lease.Receive(4100, buffer); r.Ok; r = lease.Receive(4100, buffer))
///     Log.Info($"{r.Message.Sender}: {Encoding.UTF8.GetString(buffer, 0, r.Message.Length)}");
/// </code></example>
[AnyThread]
public static unsafe class SteamMessages
{
    /// <summary>The capability row: pending until a call finds Steam ready here, then available.</summary>
    public const string Capability = "net.steam_messages";

    /// <summary>The most bytes in one message (kiln_net.h's <c>KILN_STEAM_MESSAGE_MAX</c>), and the receive buffer that
    /// never drops a message for its size.</summary>
    public const int MaxMessageBytes = 524288;

    /// <summary>Open leases in the process, every owner's.</summary>
    public const int MaxLeases = 16;

    /// <summary>Steam ids in a <see cref="SteamAcceptMode.Listed"/> policy.</summary>
    public const int MaxListedIds = 256;

    private static readonly object Gate = new();
    private static readonly List<SteamMessageLease> Leases = [];

    static SteamMessages() => NetworkCaps.Rows.Ensure();

    /// <summary>True when Steam messages work here now: this kiln.net has them, the capability row is not unavailable,
    /// and Steam is ready (while the row is pending this asks kiln.net's status once, which also moves the row on). Any
    /// thread; never throws.</summary>
    public static bool Available
    {
        get
        {
            try
            {
                if (SteamNative.Members == null) return false;
                var row = Capabilities.Get(Capability);
                if (row.State == CapabilityState.Available) return true;
                if (row.State == CapabilityState.Unavailable && row.Reason != CapabilityReason.Unknown) return false;
                return Status.SteamReady;
            }
            catch
            {
                return false;
            }
        }
    }

    /// <summary>kiln.net's Steam messages status, read now. Any thread; never throws.</summary>
    public static SteamMessagesStatus Status
    {
        get
        {
            try
            {
                var t = SteamNative.Members;
                if (t == null)
                    return Missing(SteamNative.Header != null ? "this kiln.net has no Steam messages (a Kiln before 0.2.0-dev.30.3)" : "kiln.net is not here");
                var s = new SteamNative.Status { Size = (uint)sizeof(SteamNative.Status) };
                var rc = t->SteamStatus(&s);
                return rc switch
                {
                    SteamNative.Ok => ToStatus(s, null),
                    SteamNative.Unavailable => ToStatus(s, "the game is exiting"),
                    SteamNative.Internal => ToStatus(s, "a Steam call faulted (kiln.log says which)"),
                    _ => Missing($"kiln.net's Steam status failed ({rc})"),
                };
            }
            catch (Exception e)
            {
                return Missing($"reading kiln.net's Steam status failed: {e.GetType().Name}");
            }
        }
    }

    /// <summary>Opens a lease on the Steam message channels <paramref name="firstChannel"/> to
    /// <paramref name="lastChannel"/> (inclusive) for the calling mod. It calls no Steam function.</summary>
    /// <param name="firstChannel">The lease's first channel.</param>
    /// <param name="lastChannel">The lease's last channel (at least <paramref name="firstChannel"/>).</param>
    /// <param name="answerSessions">This lease answers Steam's session requests through its accept policy
    /// (<see cref="SteamMessageLease.SetAcceptPolicy"/>); one such lease at most in the process.</param>
    /// <returns>The lease, or the refusal: <see cref="SteamMessagesRefusal.Taken"/>,
    /// <see cref="SteamMessagesRefusal.NotPermitted"/>, <see cref="SteamMessagesRefusal.ClosedForExit"/>,
    /// <see cref="SteamMessagesRefusal.Fault"/>.</returns>
    /// <exception cref="ArgumentException"><paramref name="firstChannel"/> is above <paramref name="lastChannel"/>.</exception>
    /// <exception cref="KilnUnavailableException"><see cref="Capability"/> is unavailable here (not when it is pending),
    /// or this Kiln has no Steam messages.</exception>
    /// <exception cref="InvalidOperationException">Called outside mod code or in <see cref="Mod.OnBoot"/>, the mod is
    /// unloading, or its kiln.mod.json does not say <c>"steamMessages": true</c>.</exception>
    public static SteamLeaseResult Open(ushort firstChannel, ushort lastChannel, bool answerSessions = false)
    {
        if (firstChannel > lastChannel)
            throw new ArgumentException($"the first channel ({firstChannel}) is above the last ({lastChannel})", nameof(firstChannel));
        // OnBoot's instance is transient and runs under an owner of its own that kiln.net never admits: say so plainly
        if (Game.InBoot) throw new InvalidOperationException(Game.NotInBoot("SteamMessages.Open"));
        var t = Require();
        var mod = ModContext.CurrentMod
            ?? throw new InvalidOperationException("SteamMessages.Open must be called from mod code (OnLoad, a tick, a client tick, an event handler or a Kiln task)");
        if (!mod.Alive || mod.Unloading) throw new InvalidOperationException($"SteamMessages.Open: mod '{mod.Info.Id}' is unloading");
        if (!mod.Info.SteamMessages)
            throw new InvalidOperationException(
                $"SteamMessages.Open: mod '{mod.Info.Id}' has not opted into Steam peer messages. A mod that talks to Steam users " +
                "directly says so; add \"steamMessages\": true to its kiln.mod.json to allow it.");
        var request = new SteamNative.Open
        {
            Size = (uint)sizeof(SteamNative.Open),
            Flags = answerSessions ? SteamNative.OpenSessions : 0,
            FirstChannel = firstChannel,
            LastChannel = lastChannel,
        };
        ulong handle = 0;
        var rc = t->SteamOpen(mod.Owner, &request, &handle);
        if (rc == SteamNative.Ok && handle != 0)
        {
            var lease = new SteamMessageLease(handle, firstChannel, lastChannel, answerSessions, mod);
            lock (Gate) Leases.Add(lease);
            lease.Watch();
            return new SteamLeaseResult(true, null, SteamMessagesRefusal.None, lease);
        }
        return rc switch
        {
            SteamNative.Unavailable when Status.ClosedForExit => Refused(SteamMessagesRefusal.ClosedForExit, "the game is exiting"),
            SteamNative.Unavailable => Refused(SteamMessagesRefusal.NotPermitted,
                $"kiln.net does not open Steam message leases for mod '{mod.Info.Id}' (kiln.log: steam-messages-owner)"),
            SteamNative.Revoked => Refused(SteamMessagesRefusal.NotPermitted, $"mod '{mod.Info.Id}' is unloading"),
            SteamNative.InvalidArgument => Refused(SteamMessagesRefusal.Taken, TakenReason(firstChannel, lastChannel, answerSessions)),
            _ => Refused(SteamMessagesRefusal.Fault, $"kiln.net could not open the lease ({rc})"),
        };
    }

    private static SteamLeaseResult Refused(SteamMessagesRefusal refusal, string reason) => new(false, reason, refusal, null);

    // Why kiln.net refused a range that Kiln.Network's own checks let through: a lease of another mod (named), else one of
    // Kiln's own runtimes, else the 16 leases.
    private static string TakenReason(ushort first, ushort last, bool sessions)
    {
        lock (Gate)
        {
            foreach (var l in Leases)
                if (l.IsOpen && first <= l.LastChannel && l.FirstChannel <= last)
                    return $"channels {first}..{last} overlap mod '{l.ModId}''s lease {l.FirstChannel}..{l.LastChannel}";
            if (sessions)
                foreach (var l in Leases)
                    if (l.IsOpen && l.AnswersSessions)
                        return $"mod '{l.ModId}' holds the lease that answers session requests (one at most)";
            var open = Leases.Count(l => l.IsOpen);
            return open >= MaxLeases
                ? $"all {MaxLeases} Steam message leases are open"
                : sessions
                    ? $"channels {first}..{last} overlap an open lease of another part of Kiln, a session-answering lease is open already, or all {MaxLeases} leases are open"
                    : $"channels {first}..{last} overlap an open lease of another part of Kiln, or all {MaxLeases} leases are open";
        }
    }

    internal static void Forget(SteamMessageLease lease)
    {
        lock (Gate) Leases.Remove(lease);
    }

    private static SteamNative.Table* Require()
    {
        NetworkCaps.Rows.Ensure();
        var t = SteamNative.Members;
        if (t == null)
            throw ServiceCaps.Unavailable(Capability, SteamNative.Header != null
                ? "this kiln.net has no Steam messages (a Kiln before 0.2.0-dev.30.3)"
                : "kiln.net is not here");
        var row = Capabilities.Get(Capability);
        if (row.State == CapabilityState.Unavailable && row.Reason != CapabilityReason.Unknown) throw ServiceCaps.Unavailable(Capability);
        return t;
    }

    /// <summary>Reports <see cref="Capability"/> where nothing else can (NetworkCaps' rows): an older kiln.net without the
    /// members, or no Kiln core at all. A row the core or kiln-net.dll reported stands.</summary>
    internal static void ReportRow()
    {
        if (NetNative.TestTransport != null) return;  // unit tests of the frame transport: no table to read
        if (SteamNative.Members != null || Capabilities.TryGetRow(Capability, out _)) return;
        var h = SteamNative.Header;
        if (h != null)
        {
            ServiceCaps.Report(Capability, CapabilityState.Unavailable, CapabilityReason.ServiceOlder,
                $"kiln.net v{h->Version} has a {h->Size}-byte table without Steam messages (a Kiln before 0.2.0-dev.30.3)");
            return;
        }
        var (reason, text) = ServiceCaps.Cause(CapabilityReason.ServiceMissing, "kiln.net is not here", SteamNative.Service);
        ServiceCaps.Report(Capability, CapabilityState.Unavailable, reason, text);
    }

    internal static SteamMessagesStatus ToStatus(in SteamNative.Status s, string? reason) =>
        new(true, (SteamMessagesFlags)s.Flags, s.LocalSteamId, Clamp(s.SessionsAccepted), Clamp(s.SessionsRefused),
            (SteamAcceptMode)s.AcceptMode, (int)Math.Min(s.Listed, int.MaxValue), reason);

    private static SteamMessagesStatus Missing(string reason) => new(false, SteamMessagesFlags.None, 0, 0, 0, SteamAcceptMode.None, 0, reason);

    private static long Clamp(ulong v) => v > long.MaxValue ? long.MaxValue : (long)v;

    /// <summary>kiln.net answered unavailable after the lease's checks: the game is exiting, or Steam is not ready.</summary>
    internal static (SteamMessagesRefusal Refusal, string Reason) ExitOrNotReady() =>
        Status.ClosedForExit
            ? (SteamMessagesRefusal.ClosedForExit, "the game is exiting")
            : (SteamMessagesRefusal.SteamNotReady, "Steam is not ready in this process yet");
}

/// <summary>
/// A lease on an inclusive range of Steam message channels (<see cref="SteamMessages.Open"/>). Send, receive and accept
/// on it from any thread; it never waits. Since 0.2.0-dev.30.3.
/// </summary>
/// <remarks>
/// <para>The lease belongs to the mod that opened it: it closes when that mod unloads, or at <see cref="Dispose"/>
/// (idempotent, any thread). A closed lease answers <see cref="SteamMessagesRefusal.LeaseClosed"/> and never calls
/// Steam.</para>
/// <para>Nothing is buffered by Kiln: a message waits in Steam's queue of its channel until a <see cref="Receive"/> on that
/// channel takes it. Each <see cref="Receive"/> takes at most one; loop until it answers
/// <see cref="SteamMessagesRefusal.NoMessage"/>.</para>
/// </remarks>
[AnyThread]
public sealed unsafe class SteamMessageLease : IDisposable
{
    private ulong _handle;
    private readonly ModContext _mod;
    private readonly Action _retire;

    internal SteamMessageLease(ulong handle, ushort first, ushort last, bool sessions, ModContext mod)
    {
        _handle = handle;
        FirstChannel = first;
        LastChannel = last;
        AnswersSessions = sessions;
        _mod = mod;
        _retire = Close;
    }

    /// <summary>The lease's first channel.</summary>
    public ushort FirstChannel { get; }

    /// <summary>The lease's last channel (inclusive).</summary>
    public ushort LastChannel { get; }

    /// <summary>This lease answers Steam's session requests through its accept policy.</summary>
    public bool AnswersSessions { get; }

    /// <summary>False once the lease was disposed or its mod unloaded.</summary>
    public bool IsOpen => Interlocked.Read(ref _handle) != 0;

    internal string ModId => _mod.Info.Id;

    /// <summary>Closes the lease when its mod unloads; a mod that began unloading meanwhile closes it at once.</summary>
    internal void Watch()
    {
        _mod.Retiring += _retire;
        if (!_mod.Alive || _mod.Unloading) Close();
    }

    /// <summary>Sends one message to <paramref name="steamId"/> on <paramref name="channel"/>.</summary>
    /// <param name="steamId">The receiver's Steam id.</param>
    /// <param name="channel">A channel of this lease.</param>
    /// <param name="data">0 to <see cref="SteamMessages.MaxMessageBytes"/> bytes.</param>
    /// <param name="flags">How: <see cref="SteamSendFlags.Reliable"/> by default.</param>
    /// <returns>Sent (Steam's EResult 1), or the refusal: <see cref="SteamMessagesRefusal.SteamRefused"/> with Steam's
    /// EResult, <see cref="SteamMessagesRefusal.SteamNotReady"/>, <see cref="SteamMessagesRefusal.LeaseClosed"/>,
    /// <see cref="SteamMessagesRefusal.ClosedForExit"/>, <see cref="SteamMessagesRefusal.Fault"/>.</returns>
    /// <exception cref="ArgumentException"><paramref name="steamId"/> is 0, or <paramref name="flags"/> has another
    /// flag.</exception>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="channel"/> is outside the lease, or
    /// <paramref name="data"/> is longer than <see cref="SteamMessages.MaxMessageBytes"/>.</exception>
    public SteamSendResult Send(ulong steamId, ushort channel, ReadOnlySpan<byte> data, SteamSendFlags flags = SteamSendFlags.Reliable)
    {
        if (steamId == 0) throw new ArgumentException("no Steam id (0)", nameof(steamId));
        CheckChannel(channel);
        if (data.Length > SteamMessages.MaxMessageBytes)
            throw new ArgumentOutOfRangeException(nameof(data), data.Length, $"a Steam message has at most {SteamMessages.MaxMessageBytes} bytes");
        if ((flags & ~(SteamSendFlags.Reliable | SteamSendFlags.RestartBrokenSession)) != 0)
            throw new ArgumentException($"unknown send flags 0x{(uint)flags:x}", nameof(flags));
        var h = Interlocked.Read(ref _handle);
        var t = SteamNative.Members;
        if (h == 0 || t == null) return new SteamSendResult(false, "the lease is closed", SteamMessagesRefusal.LeaseClosed, 0);
        var steamResult = 0;
        int rc;
        fixed (byte* p = data) rc = t->SteamSend(_mod.Owner, h, steamId, p, (uint)data.Length, channel, (uint)flags, &steamResult);
        switch (rc)
        {
            case SteamNative.Ok when steamResult == 1:
                return new SteamSendResult(true, null, SteamMessagesRefusal.None, 1);
            case SteamNative.Ok:
                return new SteamSendResult(false, $"Steam did not take the message (EResult {steamResult})", SteamMessagesRefusal.SteamRefused, steamResult);
            case SteamNative.NotFound:
            case SteamNative.Revoked:  // the mod's owner is gone: its leases are closed
                return new SteamSendResult(false, "the lease is closed", SteamMessagesRefusal.LeaseClosed, 0);
            case SteamNative.Unavailable:
                var (refusal, reason) = SteamMessages.ExitOrNotReady();
                return new SteamSendResult(false, reason, refusal, 0);
            default:
                return new SteamSendResult(false, Fault("send", rc), SteamMessagesRefusal.Fault, 0);
        }
    }

    /// <summary>Takes at most one message off Steam's queue of <paramref name="channel"/> and copies it into
    /// <paramref name="buffer"/>. A message bigger than the buffer is taken and dropped
    /// (<see cref="SteamMessagesRefusal.Dropped"/>): a buffer of <see cref="SteamMessages.MaxMessageBytes"/> never drops for
    /// its size.</summary>
    /// <param name="channel">A channel of this lease.</param>
    /// <param name="buffer">Where the message's bytes go: <c>buffer[..result.Message.Length]</c>.</param>
    /// <returns>Received, or the refusal: <see cref="SteamMessagesRefusal.NoMessage"/> (nothing waits),
    /// <see cref="SteamMessagesRefusal.Dropped"/>, <see cref="SteamMessagesRefusal.SteamNotReady"/>,
    /// <see cref="SteamMessagesRefusal.LeaseClosed"/>, <see cref="SteamMessagesRefusal.ClosedForExit"/>,
    /// <see cref="SteamMessagesRefusal.Fault"/>.</returns>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="channel"/> is outside the lease.</exception>
    public SteamReceiveResult Receive(ushort channel, Span<byte> buffer)
    {
        CheckChannel(channel);
        var h = Interlocked.Read(ref _handle);
        var t = SteamNative.Members;
        if (h == 0 || t == null) return new SteamReceiveResult(false, "the lease is closed", SteamMessagesRefusal.LeaseClosed, default);
        var info = new SteamNative.MessageInfo { Size = (uint)sizeof(SteamNative.MessageInfo) };
        uint got = 0;
        int rc;
        fixed (byte* p = buffer) rc = t->SteamReceive(_mod.Owner, h, channel, p, (uint)buffer.Length, &info, &got);
        switch (rc)
        {
            case SteamNative.Ok when got == 1:
                return new SteamReceiveResult(true, null, SteamMessagesRefusal.None,
                    new SteamMessageInfo(info.PeerSteamId, (ushort)info.Channel, (int)info.Length, (info.Flags & SteamNative.SendReliable) != 0));
            case SteamNative.Ok:
                return new SteamReceiveResult(false, "no message waits on the channel", SteamMessagesRefusal.NoMessage, default);
            case SteamNative.ErrInvalidData:
                return new SteamReceiveResult(false, $"Steam's next message on channel {channel} was bigger than the {buffer.Length}-byte buffer or had no Steam sender: dropped",
                    SteamMessagesRefusal.Dropped, default);
            case SteamNative.NotFound:
            case SteamNative.Revoked:
                return new SteamReceiveResult(false, "the lease is closed", SteamMessagesRefusal.LeaseClosed, default);
            case SteamNative.Unavailable:
                var (refusal, reason) = SteamMessages.ExitOrNotReady();
                return new SteamReceiveResult(false, reason, refusal, default);
            default:
                return new SteamReceiveResult(false, Fault("receive", rc), SteamMessagesRefusal.Fault, default);
        }
    }

    /// <summary>Accepts a pending session request of <paramref name="steamId"/> by hand (any lease may).</summary>
    /// <param name="steamId">The peer's Steam id.</param>
    /// <returns>Accepted, or the refusal: <see cref="SteamMessagesRefusal.SteamRefused"/> (no request of it is pending, or
    /// Steam said no), <see cref="SteamMessagesRefusal.SteamNotReady"/>, <see cref="SteamMessagesRefusal.LeaseClosed"/>,
    /// <see cref="SteamMessagesRefusal.ClosedForExit"/>, <see cref="SteamMessagesRefusal.Fault"/>.</returns>
    /// <exception cref="ArgumentException"><paramref name="steamId"/> is 0.</exception>
    public SteamAcceptResult Accept(ulong steamId)
    {
        if (steamId == 0) throw new ArgumentException("no Steam id (0)", nameof(steamId));
        var h = Interlocked.Read(ref _handle);
        var t = SteamNative.Members;
        if (h == 0 || t == null) return new SteamAcceptResult(false, "the lease is closed", SteamMessagesRefusal.LeaseClosed);
        uint accepted = 0;
        var rc = t->SteamAccept(_mod.Owner, h, steamId, &accepted);
        switch (rc)
        {
            case SteamNative.Ok when accepted != 0:
                return new SteamAcceptResult(true, null, SteamMessagesRefusal.None);
            case SteamNative.Ok:
                return new SteamAcceptResult(false, $"Steam did not accept a session with {steamId} (no request of it is pending, or Steam said no)",
                    SteamMessagesRefusal.SteamRefused);
            case SteamNative.NotFound:
            case SteamNative.Revoked:
                return new SteamAcceptResult(false, "the lease is closed", SteamMessagesRefusal.LeaseClosed);
            case SteamNative.Unavailable:
                var (refusal, reason) = SteamMessages.ExitOrNotReady();
                return new SteamAcceptResult(false, reason, refusal);
            default:
                return new SteamAcceptResult(false, Fault("accept", rc), SteamMessagesRefusal.Fault);
        }
    }

    /// <summary>Sets whom this lease accepts when a Steam user asks for a session. <see cref="SteamAcceptMode.None"/> is
    /// in force at once; <see cref="SteamAcceptMode.Any"/> and <see cref="SteamAcceptMode.Listed"/> only once Steam is
    /// ready (else <see cref="SteamMessagesRefusal.SteamNotReady"/> and nothing changed: call again later). The first
    /// policy that is not None registers Steam's session callback, once per process. Closing the lease sets None.</summary>
    /// <param name="mode">Whom to accept.</param>
    /// <param name="steamIds">With <see cref="SteamAcceptMode.Listed"/>: up to <see cref="SteamMessages.MaxListedIds"/>
    /// nonzero Steam ids (copied). Empty or null with the other modes.</param>
    /// <returns>In force, or the refusal: <see cref="SteamMessagesRefusal.SteamNotReady"/>,
    /// <see cref="SteamMessagesRefusal.LeaseClosed"/>, <see cref="SteamMessagesRefusal.ClosedForExit"/>,
    /// <see cref="SteamMessagesRefusal.Fault"/> (the callback could not be registered, or Steam faulted).</returns>
    /// <exception cref="InvalidOperationException">The lease was opened without <c>answerSessions</c>.</exception>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="mode"/> is not a mode, or there are more than
    /// <see cref="SteamMessages.MaxListedIds"/> ids.</exception>
    /// <exception cref="ArgumentException">Ids with a mode other than Listed, or an id of 0.</exception>
    public SteamPolicyResult SetAcceptPolicy(SteamAcceptMode mode, IReadOnlyCollection<ulong>? steamIds = null)
    {
        if (mode > SteamAcceptMode.Listed) throw new ArgumentOutOfRangeException(nameof(mode), mode, "not an accept mode");
        if (!AnswersSessions)
            throw new InvalidOperationException("only the lease opened with answerSessions sets the accept policy");
        var ids = steamIds is null ? Array.Empty<ulong>() : steamIds.ToArray();
        if (mode != SteamAcceptMode.Listed && ids.Length > 0)
            throw new ArgumentException("Steam ids are listed only with SteamAcceptMode.Listed", nameof(steamIds));
        if (ids.Length > SteamMessages.MaxListedIds)
            throw new ArgumentOutOfRangeException(nameof(steamIds), ids.Length, $"at most {SteamMessages.MaxListedIds} Steam ids");
        if (Array.IndexOf(ids, 0UL) >= 0) throw new ArgumentException("a Steam id of 0", nameof(steamIds));
        var h = Interlocked.Read(ref _handle);
        var t = SteamNative.Members;
        if (h == 0 || t == null) return new SteamPolicyResult(false, "the lease is closed", SteamMessagesRefusal.LeaseClosed, SteamMessages.Status);
        var status = new SteamNative.Status { Size = (uint)sizeof(SteamNative.Status) };
        int rc;
        fixed (ulong* p = ids)
        {
            var policy = new SteamNative.AcceptPolicy
            {
                Size = (uint)sizeof(SteamNative.AcceptPolicy),
                Mode = (uint)mode,
                Ids = ids.Length > 0 ? p : null,
                Count = (uint)ids.Length,
            };
            rc = t->SteamMaintain(_mod.Owner, h, &policy, &status);
        }
        var now = SteamMessages.ToStatus(status, null);
        return rc switch
        {
            SteamNative.Ok when mode == SteamAcceptMode.None || now.SteamReady => new SteamPolicyResult(true, null, SteamMessagesRefusal.None, now),
            SteamNative.Ok => new SteamPolicyResult(false, "Steam is not ready in this process yet: the policy is not in force (call again later)",
                SteamMessagesRefusal.SteamNotReady, now),
            SteamNative.NotFound or SteamNative.Revoked => new SteamPolicyResult(false, "the lease is closed", SteamMessagesRefusal.LeaseClosed, SteamMessages.Status),
            SteamNative.Unavailable => new SteamPolicyResult(false, "the game is exiting", SteamMessagesRefusal.ClosedForExit,
                now with { Reason = "the game is exiting" }),
            _ => new SteamPolicyResult(false, Fault("accept policy", rc), SteamMessagesRefusal.Fault, now with { Reason = Fault("accept policy", rc) }),
        };
    }

    /// <summary>Closes the lease. Idempotent; any thread. A session-answering lease sets the accept policy to
    /// <see cref="SteamAcceptMode.None"/> as it closes.</summary>
    public void Dispose() => Close();

    private void Close()
    {
        var h = Interlocked.Exchange(ref _handle, 0);
        if (h == 0) return;
        try
        {
            var t = SteamNative.Members;
            if (t != null) t->SteamClose(_mod.Owner, h);
        }
        finally
        {
            _mod.Retiring -= _retire;
            SteamMessages.Forget(this);
        }
    }

    private void CheckChannel(ushort channel)
    {
        if (channel < FirstChannel || channel > LastChannel)
            throw new ArgumentOutOfRangeException(nameof(channel), channel, $"channel {channel} is outside the lease ({FirstChannel}..{LastChannel})");
    }

    private static string Fault(string what, int rc) =>
        rc == SteamNative.Internal ? $"a Steam call faulted in {what} (kiln.log says which)" : $"kiln.net's {what} failed ({rc})";

    /// <summary><c>Steam lease 4100..4103 (sessions)</c>; <c>(closed)</c> once closed.</summary>
    public override string ToString()
    {
        var sb = new StringBuilder($"Steam lease {FirstChannel}..{LastChannel}");
        if (AnswersSessions) sb.Append(" (sessions)");
        if (!IsOpen) sb.Append(" (closed)");
        return sb.ToString();
    }
}
