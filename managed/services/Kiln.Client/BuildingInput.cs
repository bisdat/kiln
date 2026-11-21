// SPDX-License-Identifier: MIT
// Kiln.BuildingInput: the local player's ordinary building input, emitted through the game's own input path (kiln.client's
// building_input_submit / _status / _cancel, native/core/abi/services/kiln_client.h). Since 0.2.0-dev.30.3.
using System.Numerics;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>What one building input request does (kiln_client.h's <c>KILN_BUILDING_ACTION_*</c>). Since
/// 0.2.0-dev.30.3.</summary>
public enum BuildingAction : uint
{
    /// <summary>Selects a building item (and its material) in an actionbar slot. Dispatched at once.</summary>
    Select = 0,

    /// <summary>The primary building action at a transform: held, then released at a later input.</summary>
    Place = 1,

    /// <summary>The secondary building action at a transform: held, then released at a later input.</summary>
    Remove = 2,

    /// <summary>The game's own building undo: held, then released at a later input.</summary>
    Undo = 3,

    /// <summary>Dismantles one exact prop at a transform: held for 1.2 s.</summary>
    Dismantle = 4,
}

/// <summary>Where a building input request is (kiln_client.h's <c>KILN_BUILDING_INPUT_*</c>). Since 0.2.0-dev.30.3.</summary>
/// <remarks><see cref="Dispatched"/>, <see cref="TimedOut"/> and <see cref="Cancelled"/> are final, with one exception: a
/// pressed request that was cancelled and is not released within 5 s of its submit reads <see cref="Cancelled"/>, then
/// <see cref="TimedOut"/> (it is still released).</remarks>
public enum BuildingInputState : uint
{
    /// <summary>Not the mod's latest request (a newer one of the same mod replaced it), or the state cannot be read now
    /// (the mod unloaded, kiln.client is off).</summary>
    Unknown = 0,

    /// <summary>Waiting for the game's input path, at most <see cref="BuildingInput.QueueTimeoutMilliseconds"/>.</summary>
    Queued = 1,

    /// <summary>The action is held in the local player's input; it is released at a later input.</summary>
    Pressed = 2,

    /// <summary>Emitted as the player's ordinary input. This is not the world's answer: the server still checks
    /// permissions, resources, range and placement, as for the player's own input.</summary>
    Dispatched = 3,

    /// <summary>Not emitted within <see cref="BuildingInput.QueueTimeoutMilliseconds"/> of the submit (a pressed one is
    /// still released).</summary>
    TimedOut = 4,

    /// <summary>Cancelled (<see cref="BuildingInputRequest.Cancel"/>), or its mod unloaded, its client world stopped, the
    /// input producer stopped, or a guarded copy of the input faulted.</summary>
    Cancelled = 5,
}

/// <summary>Why a building input submit was refused (<see cref="BuildingInputResult.Refusal"/>). Since 0.2.0-dev.30.3.</summary>
public enum BuildingInputRefusal : uint
{
    /// <summary>Not refused: the request is queued.</summary>
    None = 0,

    /// <summary>A request (this mod's or another's) is still queued or pressed: there is one request slot for the whole
    /// client. Try again after it ends (at most 5 s).</summary>
    Busy = 1,

    /// <summary>No client world is live (the main menu, a loading screen), or the expected session is not the live
    /// one.</summary>
    Stale = 2,

    /// <summary>The client world runs, but the local player is not known yet (not spawned).</summary>
    NoLocalPlayer = 3,

    /// <summary>The calling mod is unloading or has unloaded.</summary>
    ModUnloaded = 4,

    /// <summary>kiln.client refused a field that Kiln.Client's own checks let through (a newer rule of the native side);
    /// <see cref="BuildingInputResult.Reason"/> has its answer.</summary>
    Rejected = 5,
}

/// <summary>The answer of a <see cref="BuildingInput"/> submit. <see cref="Ok"/> means queued, never "built". Since
/// 0.2.0-dev.30.3.</summary>
/// <param name="Ok">True when the request was queued.</param>
/// <param name="Reason">Why not; null when queued.</param>
/// <param name="Refusal">Why not, as a value; <see cref="BuildingInputRefusal.None"/> when queued.</param>
/// <param name="Request">The queued request; null when refused.</param>
public sealed record BuildingInputResult(bool Ok, string? Reason, BuildingInputRefusal Refusal, BuildingInputRequest? Request)
    : IKilnResult;

/// <summary>One submitted building input request (<see cref="BuildingInput"/>). Any thread. Since 0.2.0-dev.30.3.</summary>
[ClientOnly]
public sealed unsafe class BuildingInputRequest
{
    private readonly ulong _owner;

    internal BuildingInputRequest(uint id, BuildingAction action, uint clientSession, ulong owner)
    {
        Id = id;
        Action = action;
        ClientSession = clientSession;
        _owner = owner;
    }

    /// <summary>kiln.client's id of the request; never 0. Ids grow by one for the whole client.</summary>
    public uint Id { get; }

    /// <summary>What the request does.</summary>
    public BuildingAction Action { get; }

    /// <summary>The client world session it was submitted for (<see cref="Client.ClientWorldSession"/>).</summary>
    public uint ClientSession { get; }

    /// <summary>Where the request is now: a copy read from kiln.client (any thread, never waits).
    /// <see cref="BuildingInputState.Unknown"/> once a newer request of the same mod replaced it, and when it cannot be
    /// read (the mod unloaded, kiln.client is off). Never throws.</summary>
    [AnyThread]
    public BuildingInputState State
    {
        get
        {
            var t = BuildingInputNative.Members;
            if (t == null) return BuildingInputState.Unknown;
            uint state = 0;
            var rc = t->Status(_owner, Id, &state);
            return rc == BuildingInputNative.Ok && state <= (uint)BuildingInputState.Cancelled ? (BuildingInputState)state : BuildingInputState.Unknown;
        }
    }

    /// <summary>Cancels the request when it is still the mod's latest. A queued request ends
    /// <see cref="BuildingInputState.Cancelled"/> at once; a pressed one reads Cancelled and is released at its next
    /// input (the request slot stays busy until then); a dispatched one stays <see cref="BuildingInputState.Dispatched"/>.
    /// Any thread, never waits, never throws.</summary>
    /// <returns>True when the id was the mod's latest request (also when it was dispatched already: read
    /// <see cref="State"/>); false for an older id, an unloaded mod, or no kiln.client.</returns>
    [AnyThread]
    public bool Cancel()
    {
        var t = BuildingInputNative.Members;
        if (t == null) return false;
        uint cancelled = 0;
        return t->Cancel(_owner, Id, &cancelled) == BuildingInputNative.Ok && cancelled != 0;
    }

    /// <summary><c>building input 12 (Place, session 3)</c>.</summary>
    public override string ToString() => $"building input {Id} ({Action}, session {ClientSession})";
}

/// <summary>
/// The local player's ordinary building input: select a building item, place, remove, undo, dismantle, emitted through
/// the game's own input path as if the player pressed it. Since 0.2.0-dev.30.3.
/// </summary>
/// <remarks>
/// <para>Game client only, and only on a game build whose input producer Kiln has verified (game client 26430625 in
/// 0.2.0-dev.30.3; capability <c>client.building_input</c>; <see cref="Available"/>). Elsewhere every submit throws
/// <see cref="KilnUnavailableException"/> naming it. Kiln hooks the game's input path at the first submit of the process,
/// never before, so a game without a mod that builds never has the hook; that first submit takes a few milliseconds
/// more. Not checked in a real game yet: tell us what you see. This is input, not a server-side build: the game's server checks
/// permissions, resources, range and placement exactly as for the player's own input, so
/// <see cref="BuildingInputState.Dispatched"/> is not the world's acknowledgement. For world edits the server makes
/// itself, use Kiln.Build.</para>
/// <para>Threads: every member may be called from any thread and never waits for the game. A submit needs the calling
/// mod (any code Kiln runs has it: OnLoad, ticks, client ticks, <see cref="Client.Post"/> work, event handlers, Kiln
/// tasks); outside it, it throws <see cref="InvalidOperationException"/>.</para>
/// <para>One request slot for the whole client: every mod and Kiln's interpreter for ShroudForge mods share it. While any request is
/// queued or pressed a submit answers <see cref="BuildingInputRefusal.Busy"/>. A newer request of the same mod becomes its
/// latest: <see cref="BuildingInputRequest.State"/> and <see cref="BuildingInputRequest.Cancel"/> answer only for the
/// latest. A request ends cancelled when its mod unloads or its client world stops.</para>
/// <para>The player is always the local player (<see cref="Client.LocalPlayer"/>), read with its client world session
/// at each submit; a mod cannot drive another player's input.</para>
/// </remarks>
/// <example><code>
/// var r = BuildingInput.Select(plank, 0);
/// if (!r.Ok) { Log.Info(r.Reason!); return; }
/// while (r.Request!.State is BuildingInputState.Queued or BuildingInputState.Pressed) await Client.NextTick();
/// r = BuildingInput.Place(at, Quaternion.Identity);
/// </code></example>
[ClientOnly]
[AnyThread]
public static unsafe class BuildingInput
{
    /// <summary>The capability row: available only on a game client whose build has a verified input producer (ready for
    /// the first request, or already hooked).</summary>
    public const string Capability = "client.building_input";

    /// <summary>A queued request that the input path has not taken within this many milliseconds ends
    /// <see cref="BuildingInputState.TimedOut"/>.</summary>
    public const int QueueTimeoutMilliseconds = 5000;

    /// <summary>The largest scale component (each must be in (0, <see cref="MaxScale"/>]).</summary>
    public const float MaxScale = 1024f;

    // The rows Kiln.Client reports itself: client.building_input only where nothing else can (an older kiln.client
    // without the members, or no Kiln core at all). The core's row always stands.
    internal static readonly ServiceCapRows Rows = new(BuildingInputNative.Service, ReportRows);

    static BuildingInput() => Rows.Ensure();

    /// <summary>True when building input works here: the capability row is available and this kiln.client has the
    /// members. Any thread; never throws.</summary>
    public static bool Available => BuildingInputNative.Members != null && Capabilities.Is(Capability);

    /// <summary>Selects <paramref name="item"/> (and <paramref name="material"/>) in actionbar slot
    /// <paramref name="slot"/> of the local player. Dispatched at once by the input path.</summary>
    /// <param name="item">The building item; required.</param>
    /// <param name="slot">The actionbar slot, 0 to 255.</param>
    /// <param name="material">The terrain and default building material; none leaves the game's choice.</param>
    /// <param name="expectedSession">0: the live client world session; else a <see cref="Client.ClientWorldSession"/>
    /// the request must still be in (<see cref="BuildingInputRefusal.Stale"/> otherwise).</param>
    /// <returns>Queued, or refused with the reason.</returns>
    /// <exception cref="ArgumentException"><paramref name="item"/> is none.</exception>
    /// <exception cref="KilnUnavailableException"><see cref="Capability"/> is off here.</exception>
    /// <exception cref="InvalidOperationException">Called outside mod code.</exception>
    [RequiresCapability(Capability)]
    public static BuildingInputResult Select(ItemId item, byte slot, ItemId material = default, uint expectedSession = 0)
    {
        if (item.IsNone) throw new ArgumentException("a building item is required", nameof(item));
        var input = NewInput(BuildingAction.Select);
        input.ItemId = item.Value;
        input.MaterialItemId = material.Value;
        input.Slot = slot;
        return Submit(ref input, expectedSession, "BuildingInput.Select");
    }

    /// <summary>The primary building action at the transform (the local player places what is selected).</summary>
    /// <param name="position">World position, metres.</param>
    /// <param name="rotation">Rotation; any length but 0 (kiln.client normalises it).</param>
    /// <param name="scale">Each component in (0, <see cref="MaxScale"/>]; null is (1, 1, 1).</param>
    /// <param name="expectedSession">0: the live client world session; else the session the request must still be in.</param>
    /// <returns>Queued, or refused with the reason.</returns>
    /// <exception cref="ArgumentException">The rotation is not finite or has no length.</exception>
    /// <exception cref="ArgumentOutOfRangeException">A position component is not finite or outside the engine's 32.32
    /// range (|x| &lt; 2^31 m), or a scale component is not finite or outside (0, 1024].</exception>
    /// <exception cref="KilnUnavailableException"><see cref="Capability"/> is off here.</exception>
    /// <exception cref="InvalidOperationException">Called outside mod code.</exception>
    [RequiresCapability(Capability)]
    public static BuildingInputResult Place(WorldPosition position, Quaternion rotation, Vector3? scale = null, uint expectedSession = 0)
    {
        var input = NewInput(BuildingAction.Place);
        SetTransform(ref input, position, rotation, scale);
        return Submit(ref input, expectedSession, "BuildingInput.Place");
    }

    /// <summary>The secondary building action at the transform.</summary>
    /// <param name="position">World position, metres.</param>
    /// <param name="rotation">Rotation; any length but 0.</param>
    /// <param name="scale">Each component in (0, <see cref="MaxScale"/>]; null is (1, 1, 1).</param>
    /// <param name="expectedSession">0: the live client world session; else the session the request must still be in.</param>
    /// <returns>Queued, or refused with the reason.</returns>
    /// <exception cref="ArgumentException">The rotation is not finite or has no length.</exception>
    /// <exception cref="ArgumentOutOfRangeException">A position or scale component is out of range.</exception>
    /// <exception cref="KilnUnavailableException"><see cref="Capability"/> is off here.</exception>
    /// <exception cref="InvalidOperationException">Called outside mod code.</exception>
    [RequiresCapability(Capability)]
    public static BuildingInputResult Remove(WorldPosition position, Quaternion rotation, Vector3? scale = null, uint expectedSession = 0)
    {
        var input = NewInput(BuildingAction.Remove);
        SetTransform(ref input, position, rotation, scale);
        return Submit(ref input, expectedSession, "BuildingInput.Remove");
    }

    /// <summary>The game's own building undo.</summary>
    /// <param name="expectedSession">0: the live client world session; else the session the request must still be in.</param>
    /// <returns>Queued, or refused with the reason.</returns>
    /// <exception cref="KilnUnavailableException"><see cref="Capability"/> is off here.</exception>
    /// <exception cref="InvalidOperationException">Called outside mod code.</exception>
    [RequiresCapability(Capability)]
    public static BuildingInputResult Undo(uint expectedSession = 0)
    {
        var input = NewInput(BuildingAction.Undo);
        return Submit(ref input, expectedSession, "BuildingInput.Undo");
    }

    /// <summary>Dismantles the exact prop <paramref name="prop"/> at the transform (the action is held for 1.2 s).</summary>
    /// <param name="prop">The prop's entity; required.</param>
    /// <param name="position">World position, metres.</param>
    /// <param name="rotation">Rotation; any length but 0.</param>
    /// <param name="scale">Each component in (0, <see cref="MaxScale"/>]; null is (1, 1, 1).</param>
    /// <param name="expectedSession">0: the live client world session; else the session the request must still be in.</param>
    /// <returns>Queued, or refused with the reason.</returns>
    /// <exception cref="ArgumentException"><paramref name="prop"/> is none, or the rotation is not finite or has no
    /// length.</exception>
    /// <exception cref="ArgumentOutOfRangeException">A position or scale component is out of range.</exception>
    /// <exception cref="KilnUnavailableException"><see cref="Capability"/> is off here.</exception>
    /// <exception cref="InvalidOperationException">Called outside mod code.</exception>
    [RequiresCapability(Capability)]
    public static BuildingInputResult Dismantle(EntityId prop, WorldPosition position, Quaternion rotation, Vector3? scale = null,
        uint expectedSession = 0)
    {
        if (prop.IsNone) throw new ArgumentException("the prop to dismantle is required", nameof(prop));
        var input = NewInput(BuildingAction.Dismantle);
        input.TargetEntityId = prop.Value;
        SetTransform(ref input, position, rotation, scale);
        return Submit(ref input, expectedSession, "BuildingInput.Dismantle");
    }

    private static BuildingInputNative.Input NewInput(BuildingAction action) =>
        new() { Size = (uint)sizeof(BuildingInputNative.Input), Action = (uint)action };

    // kiln.client's own rules (client_building_input.cpp's convert), checked here first so that a bad argument throws.
    private static void SetTransform(ref BuildingInputNative.Input input, WorldPosition position, Quaternion rotation, Vector3? scale)
    {
        double[] r = [rotation.X, rotation.Y, rotation.Z, rotation.W];
        var norm = 0.0;
        foreach (var v in r)
        {
            if (!double.IsFinite(v)) throw new ArgumentException("the rotation is not finite", nameof(rotation));
            norm += v * v;
        }
        if (!double.IsFinite(norm) || norm < 1e-12) throw new ArgumentException("the rotation has no length", nameof(rotation));
        var s = scale ?? Vector3.One;
        double[] p = [position.X, position.Y, position.Z];
        double[] k = [s.X, s.Y, s.Z];
        for (var i = 0; i < 3; i++)
        {
            var q = Math.ScaleB(p[i], 32);  // the engine's 32.32 fixed point, strictly inside int64
            if (!double.IsFinite(q) || q <= long.MinValue || q >= long.MaxValue)
                throw new ArgumentOutOfRangeException(nameof(position), position, "each position component must be finite and within ±2^31 m");
            if (!double.IsFinite(k[i]) || k[i] <= 0 || k[i] > MaxScale)
                throw new ArgumentOutOfRangeException(nameof(scale), scale, "each scale component must be finite and in (0, 1024]");
            input.Position[i] = p[i];
            input.Scale[i] = k[i];
        }
        for (var i = 0; i < 4; i++) input.Rotation[i] = r[i];
    }

    private static BuildingInputResult Submit(ref BuildingInputNative.Input input, uint expectedSession, string api)
    {
        var t = Require();
        var mod = ModContext.CurrentMod
            ?? throw new InvalidOperationException($"{api} must be called from mod code (OnLoad, a tick, a client tick, Client.Post work, an event handler or a Kiln task)");
        var action = (BuildingAction)input.Action;
        var state = new BuildingInputNative.State { Size = (uint)sizeof(BuildingInputNative.State) };
        var rc = t->GetState(&state);
        if (rc != BuildingInputNative.Ok) return Refused(BuildingInputRefusal.Stale, $"kiln.client's state could not be read ({rc})");
        if (state.LocalPlayer == 0)
            return state.Session == BuildingInputNative.SessionNone || state.ClientSession == 0
                ? Refused(BuildingInputRefusal.Stale, "no client world is live (the main menu or a loading screen)")
                : Refused(BuildingInputRefusal.NoLocalPlayer, "the local player is not known yet (not spawned)");
        input.Player = state.LocalPlayer;
        var session = expectedSession != 0 ? expectedSession : state.ClientSession;
        uint id = 0;
        fixed (BuildingInputNative.Input* p = &input) rc = t->Submit(mod.Owner, session, p, &id);
        return rc switch
        {
            BuildingInputNative.Ok when id != 0 => new BuildingInputResult(true, null, BuildingInputRefusal.None,
                new BuildingInputRequest(id, action, session, mod.Owner)),
            BuildingInputNative.ErrBusy => Refused(BuildingInputRefusal.Busy,
                "another building input request (this mod's or another's) is still queued or pressed; try again after it ends (at most 5 s)"),
            BuildingInputNative.ErrStale => Refused(BuildingInputRefusal.Stale, expectedSession != 0 && expectedSession != state.ClientSession
                ? $"client world session {expectedSession} is not the live one ({state.ClientSession})"
                : "no client world is live, or its session has stopped"),
            BuildingInputNative.Revoked => Refused(BuildingInputRefusal.ModUnloaded, $"mod '{mod.Info.Id}' is unloading"),
            BuildingInputNative.Unavailable => throw ServiceCaps.Unavailable(Capability, "kiln.client answered unavailable"),
            BuildingInputNative.InvalidArgument when mod.Owner == 0 =>
                throw new InvalidOperationException($"{api}: mod '{mod.Info.Id}' has no native owner (it was not loaded by Kiln's host)"),
            _ => Refused(BuildingInputRefusal.Rejected, $"kiln.client refused the request ({rc})"),
        };
    }

    private static BuildingInputResult Refused(BuildingInputRefusal refusal, string reason) => new(false, reason, refusal, null);

    private static BuildingInputNative.Table* Require()
    {
        Rows.Ensure();
        var t = BuildingInputNative.Members;
        if (t == null)
            throw ServiceCaps.Unavailable(Capability, BuildingInputNative.Header != null
                ? "this kiln.client has no building input (a Kiln before 0.2.0-dev.30.3)"
                : "kiln.client is not here");
        if (!Capabilities.Is(Capability)) throw ServiceCaps.Unavailable(Capability);
        return t;
    }

    private static void ReportRows()
    {
        if (BuildingInputNative.Members != null || Capabilities.TryGetRow(Capability, out _)) return;  // the core's row stands
        var h = BuildingInputNative.Header;
        if (h != null)
        {
            ServiceCaps.Report(Capability, CapabilityState.Unavailable, CapabilityReason.ServiceOlder,
                $"kiln.client v{h->Version} has a {h->Size}-byte table without building input (a Kiln before 0.2.0-dev.30.3)");
            return;
        }
        var (reason, text) = ServiceCaps.Cause(CapabilityReason.ServiceMissing, "kiln.client is not here", BuildingInputNative.Service);
        ServiceCaps.Report(Capability, CapabilityState.Unavailable, reason, text);
    }
}

/// <summary>Started at the first service tick (ServiceTick.AutoStart): Kiln.Client's own capability rows.</summary>
internal static class ClientStartup
{
    static ClientStartup() => BuildingInput.Rows.Ensure();
}

/// <summary>kiln.client v1 with its building input members (native/core/abi/services/kiln_client.h, KilnClientApi: 80
/// bytes; the members at 56, 64 and 72). Byte-exact with the header: Kiln.Api.Tests' BuildingInputTests check every size
/// and offset.</summary>
internal static unsafe class BuildingInputNative
{
    public const string Service = "kiln.client";

    // KilnResult (kiln_api.h) and kiln_client.h's own codes
    public const int Ok = 0, InvalidArgument = 1, Unavailable = 3, Revoked = 6;
    public const int ErrBusy = 200, ErrStale = 201;  // KILN_CLIENT_ERR_BUILDING_BUSY, _STALE
    public const uint SessionNone = 0;               // KILN_SESSION_NONE

    /// <summary>KilnClientState (48 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct State
    {
        public uint Size, Side, Session, Generation, LocalPlayer, Flags, ClientSession, ServerSession;
        public ulong ClientSteps, ServerSteps;
    }

    /// <summary>KilnBuildingInput (112 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Input
    {
        public uint Size, Action, Player, ItemId, MaterialItemId, Slot, TargetEntityId, Reserved;
        public fixed double Position[3];
        public fixed double Rotation[4];
        public fixed double Scale[3];
    }

    /// <summary>KilnClientApi with the building input members (80 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<State*, int> GetState;
        public nint SubscribeTick, Unsubscribe, Post, OnClientThread, ClientWorld;
        public delegate* unmanaged[Cdecl]<ulong, ulong, Input*, uint*, int> Submit;
        public delegate* unmanaged[Cdecl]<ulong, uint, uint*, int> Status;
        public delegate* unmanaged[Cdecl]<ulong, uint, uint*, int> Cancel;
    }

    // The table found under a backend (a test swaps backends; a game has one): asked once per backend while it is there.
    private sealed record Found(IKilnBackend Backend, nint Table);
    private static volatile Found? _found;

    /// <summary>kiln.client's table (any size), or null.</summary>
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

    /// <summary>The table when it has the building input members (header.size of at least 80), else null.</summary>
    public static Table* Members
    {
        get
        {
            var h = Header;
            return h != null && h->Size >= (uint)sizeof(Table) ? (Table*)h : null;
        }
    }
}
