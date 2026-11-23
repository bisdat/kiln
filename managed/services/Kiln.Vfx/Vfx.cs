// SPDX-License-Identifier: MIT
// Kiln.Vfx: play the game's VFX from code (Forge: Level.addParticle, ParticleEngine). Native: kiln.vfx
// (native/core/abi/services/kiln_vfx.h). Docs: docs/kiln/services/vfx.md.
using System.Numerics;
using System.Security.Cryptography;
using System.Text;

namespace Kiln;

/// <summary>Which VFX: one of the game's by name, one of a mod's, or a GUID. What <see cref="Vfx"/> takes. A block's
/// <c>VfxRef</c> field says the same plus where it attaches, and lowers to this.</summary>
public readonly record struct VfxId
{
    private readonly Guid _guid;
    private readonly string? _game;

    /// <summary>A VFX by GUID (the same as <see cref="Of(Guid)"/>).</summary>
    public VfxId(Guid guid) => _guid = guid;
    private VfxId(string game) => _game = game;

    /// <summary>No VFX: the default value. Playing it does nothing.</summary>
    public static readonly VfxId None = default;

    /// <summary>The GUID. A game name resolves on first use, on the thread that may read the game's data; <see cref="Guid.Empty"/> until
    /// then (asked from another thread, it is resolved within a step), or when the game has nothing by that name. <see cref="Vfx"/>'s
    /// calls take care of this themselves.</summary>
    public Guid Guid => _game is null ? _guid : VfxNames.Resolve(_game);

    /// <summary>The name given to <see cref="Game"/>, or null.</summary>
    public string? GameName => _game;

    /// <summary>True for <see cref="None"/>: no game name and an empty GUID.</summary>
    public bool IsNone => _game is null && _guid == Guid.Empty;

    /// <summary>One of the game's VFX by name. VFX have no names of their own, so a name is what uses it: a template that carries it
    /// (<c>Explosion_Player_Spell_Fireball_1</c>), or a sequence that plays it (its first VFX event). Also takes a GUID's text.
    /// The constants the local catalog generates (<c>GameVfx.*</c>) are GUIDs, for <see cref="Of(Guid)"/>.</summary>
    public static VfxId Game(string name)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        return Guid.TryParse(name, out var g) ? new VfxId(g) : new VfxId(name);
    }

    /// <summary>Any VFX by GUID.</summary>
    public static VfxId Of(Guid guid) => new(guid);

    /// <summary>Any VFX by GUID text.</summary>
    public static VfxId Of(string guid) => new(Guid.Parse(guid));

    /// <summary>The calling mod's VFX <paramref name="name"/>: <c>UUIDv5(&lt;mod&gt;:vfx:&lt;name&gt;)</c>, the rule of every block id.</summary>
    public static VfxId Named(ModContext mod, string name)
    {
        ArgumentNullException.ThrowIfNull(mod);
        return Named(mod.Info.Id, name);
    }

    /// <summary>A VFX of the mod <paramref name="modId"/>, whether or not that mod is here. Unknown to the game means it is ignored.</summary>
    public static VfxId Named(string modId, string name) => new(Derive(modId, "vfx", name));

    /// <summary>The game name given to <see cref="Game"/>, or the GUID's text.</summary>
    public override string ToString() => _game ?? _guid.ToString();

    private static Guid Derive(string mod, string kind, string name)
    {
        Span<byte> ns = stackalloc byte[16];
        new Guid("6f6d2a44-53c1-5c52-9d51-0a3c6e1f2b77").TryWriteBytes(ns, bigEndian: true, out _);  // as Kiln.Blocks' BlockIds, Kiln.Quests' NativeIds
        var text = Encoding.UTF8.GetBytes($"{mod}:{kind}:{name}");
        var all = new byte[16 + text.Length];
        ns.CopyTo(all);
        text.CopyTo(all, 16);
        var h = SHA1.HashData(all);
        h[6] = (byte)((h[6] & 0x0f) | 0x50);
        h[8] = (byte)((h[8] & 0x3f) | 0x80);
        return new Guid(h.AsSpan(0, 16), bigEndian: true);
    }
}

/// <summary>A running (or finished) VFX. Kiln's id: never reused, safe to keep.</summary>
/// <param name="Value">The handle's number; 0 is <see cref="None"/>. The top bit marks a server-side handle (<see cref="IsRemote"/>).</param>
public readonly record struct VfxHandle(uint Value)
{
    /// <summary>No VFX: the default value, what a play that did not start returns.</summary>
    public static readonly VfxHandle None = default;
    /// <summary>True for <see cref="None"/>.</summary>
    public bool IsNone => Value == 0;

    /// <summary>A server-side handle (the VFX was relayed to Kiln clients): it can be stopped and set, not observed.</summary>
    public bool IsRemote => (Value & VfxCore.RemoteBit) != 0;

    /// <summary><c>#n</c>, or <c>remote#n</c> for a server-side handle.</summary>
    public override string ToString() => IsRemote ? $"remote#{Value & ~VfxCore.RemoteBit}" : $"#{Value}";
}

/// <summary>The game's attachment slots (keen::AttachmentSlot). A body that has no bone for a slot plays the effect nowhere.
/// The game maps each slot to a joint of the body's skeleton through its animation graph, so which joint a slot is differs per body.</summary>
public enum Slot : byte
{
    /// <summary>The body's root. The default of <see cref="Vfx.PlayOn"/>.</summary>
    Root = 0,
    /// <summary>The head.</summary>
    Head = 1,
    /// <summary>The chest.</summary>
    Chest = 2,
    /// <summary>The pelvis.</summary>
    Pelvis = 3,
    /// <summary>The left foot.</summary>
    FootL = 4,
    /// <summary>The right foot.</summary>
    FootR = 5,
    /// <summary>The game's second left foot slot, the left one of the <c>FootRL</c> / <c>FootRR</c> pair.</summary>
    FootRL = 6,
    /// <summary>The game's second right foot slot, the right one of the <c>FootRL</c> / <c>FootRR</c> pair.</summary>
    FootRR = 7,
    /// <summary>The right hand.</summary>
    HandR = 8,
    /// <summary>The left hand.</summary>
    HandL = 9,
    /// <summary>The right knee.</summary>
    KneeR = 10,
    /// <summary>The left knee.</summary>
    KneeL = 11,
    /// <summary>The left arm. It has no right counterpart.</summary>
    ArmL = 12,
    /// <summary>The game's <c>Block</c> slot (a joint of the player's skeleton).</summary>
    Block = 13,
    /// <summary>The right palm.</summary>
    HandPalmR = 14,
    /// <summary>The left palm.</summary>
    HandPalmL = 15,
    /// <summary>One of the game's 20 generic slots (<c>Generic00</c>). Which joint, if any, it is depends on the body.</summary>
    Generic00 = 16,
    /// <summary>One of the game's 20 generic slots (<c>Generic01</c>). Which joint, if any, it is depends on the body.</summary>
    Generic01 = 17,
    /// <summary>One of the game's 20 generic slots (<c>Generic02</c>). Which joint, if any, it is depends on the body.</summary>
    Generic02 = 18,
    /// <summary>One of the game's 20 generic slots (<c>Generic03</c>). Which joint, if any, it is depends on the body.</summary>
    Generic03 = 19,
    /// <summary>One of the game's 20 generic slots (<c>Generic04</c>). Which joint, if any, it is depends on the body.</summary>
    Generic04 = 20,
    /// <summary>One of the game's 20 generic slots (<c>Generic05</c>). Which joint, if any, it is depends on the body.</summary>
    Generic05 = 21,
    /// <summary>One of the game's 20 generic slots (<c>Generic06</c>). Which joint, if any, it is depends on the body.</summary>
    Generic06 = 22,
    /// <summary>One of the game's 20 generic slots (<c>Generic07</c>). Which joint, if any, it is depends on the body.</summary>
    Generic07 = 23,
    /// <summary>One of the game's 20 generic slots (<c>Generic08</c>). Which joint, if any, it is depends on the body.</summary>
    Generic08 = 24,
    /// <summary>One of the game's 20 generic slots (<c>Generic09</c>). Which joint, if any, it is depends on the body.</summary>
    Generic09 = 25,
    /// <summary>One of the game's 20 generic slots (<c>Generic10</c>). Which joint, if any, it is depends on the body.</summary>
    Generic10 = 26,
    /// <summary>One of the game's 20 generic slots (<c>Generic11</c>). Which joint, if any, it is depends on the body.</summary>
    Generic11 = 27,
    /// <summary>One of the game's 20 generic slots (<c>Generic12</c>). Which joint, if any, it is depends on the body.</summary>
    Generic12 = 28,
    /// <summary>One of the game's 20 generic slots (<c>Generic13</c>). Which joint, if any, it is depends on the body.</summary>
    Generic13 = 29,
    /// <summary>One of the game's 20 generic slots (<c>Generic14</c>). Which joint, if any, it is depends on the body.</summary>
    Generic14 = 30,
    /// <summary>One of the game's 20 generic slots (<c>Generic15</c>). Which joint, if any, it is depends on the body.</summary>
    Generic15 = 31,
    /// <summary>One of the game's 20 generic slots (<c>Generic16</c>). Which joint, if any, it is depends on the body.</summary>
    Generic16 = 32,
    /// <summary>One of the game's 20 generic slots (<c>Generic17</c>). Which joint, if any, it is depends on the body.</summary>
    Generic17 = 33,
    /// <summary>One of the game's 20 generic slots (<c>Generic18</c>). Which joint, if any, it is depends on the body.</summary>
    Generic18 = 34,
    /// <summary>One of the game's 20 generic slots (<c>Generic19</c>). Which joint, if any, it is depends on the body.</summary>
    Generic19 = 35,
}

/// <summary>Where an attached effect sits relative to its slot (keen::ecs::Offset). <paramref name="Local"/> is in the slot's own axes,
/// <paramref name="World"/> in world axes, both in metres. The default is none.</summary>
/// <param name="Local">Metres along the slot's own axes.</param>
/// <param name="World">Metres along the world's axes.</param>
/// <param name="Rotation">The offset's own rotation; null for none.</param>
public readonly record struct Offset(Vector3 Local = default, Vector3 World = default, Quaternion? Rotation = null);

/// <summary>Named parameters of an effect, set when it starts. A name or type the VFX does not have is ignored. Parameters are
/// the VFX's own: <c>radius</c>, <c>duration</c>, <c>color</c>, <c>speed</c>, <c>count</c>, <c>targetPosition</c> are common.</summary>
public sealed class VfxParams
{
    internal readonly List<VfxParam> Items = [];

    /// <summary>Adds a float parameter. Returns this, to chain.</summary>
    public VfxParams Set(string name, float value)
    {
        Items.Add(VfxParam.Of(name, value));
        return this;
    }

    /// <summary>Adds a vector parameter. Returns this, to chain.</summary>
    public VfxParams Set(string name, Vector3 value)
    {
        Items.Add(VfxParam.Of(name, value));
        return this;
    }

    /// <summary>Adds a colour parameter. Returns this, to chain.</summary>
    public VfxParams Set(string name, Color value)
    {
        Items.Add(VfxParam.Of(name, value));
        return this;
    }

    /// <summary>Adds an integer parameter. Returns this, to chain.</summary>
    public VfxParams Set(string name, int value)
    {
        Items.Add(VfxParam.Of(name, value));
        return this;
    }
}

/// <summary>One parameter as the game takes it: FNV-1a of the name, keen::DsTypeId, the bytes.</summary>
internal readonly record struct VfxParam(uint Hash, uint Type, byte[] Data)
{
    // DsTypeIds in the game's VFX data and setters (docs/re/vfx.md 2.6). A wrong one is ignored by the game.
    private const uint TypeFloat = 0x90b5085d, TypeFloat3 = 0x3afc452a, TypeColor = 0x53039620, TypeInt = 0x5655a2c9;

    public static VfxParam Of(string name, float v) => new(Fnv1a(name), TypeFloat, BitConverter.GetBytes(v));

    public static VfxParam Of(string name, Vector3 v)
    {
        var b = new byte[12];
        BitConverter.TryWriteBytes(b.AsSpan(0), v.X);
        BitConverter.TryWriteBytes(b.AsSpan(4), v.Y);
        BitConverter.TryWriteBytes(b.AsSpan(8), v.Z);
        return new(Fnv1a(name), TypeFloat3, b);
    }

    public static VfxParam Of(string name, Color v) => new(Fnv1a(name), TypeColor, [v.R, v.G, v.B, v.A]);

    public static VfxParam Of(string name, int v) => new(Fnv1a(name), TypeInt, BitConverter.GetBytes(v));

    public static uint Fnv1a(string s)
    {
        var h = 0x811c9dc5u;
        foreach (var b in Encoding.UTF8.GetBytes(s)) h = (h ^ b) * 0x01000193u;
        return h;
    }
}

/// <summary>Where a VFX stands.</summary>
public enum VfxState
{
    /// <summary>Unknown or expired handle (finished handles are kept for at least 60 s).</summary>
    None = 0,
    /// <summary>Queued: starts inside the game's next FxController update.</summary>
    Pending = 1,
    /// <summary>The game's FxController holds it. The first play of a VFX can still be loading its pipelines and shows late.</summary>
    Playing = 2,
    /// <summary>Ended (see <see cref="VfxInfo.Reason"/>).</summary>
    Stopped = 3,
    /// <summary>Never started (see <see cref="VfxInfo.Reason"/>).</summary>
    Failed = 4,
}

/// <summary>Why a VFX is <see cref="VfxState.Stopped"/> or <see cref="VfxState.Failed"/>.</summary>
public enum VfxEndReason
{
    /// <summary>No reason: the VFX has not ended.</summary>
    None = 0,
    /// <summary>The FxController dropped it: its lifetime ran out and the instance went.</summary>
    Finished = 1,
    /// <summary>It was stopped: <see cref="Vfx.Stop"/>, <see cref="Vfx.StopAll"/>, or its mod unloading.</summary>
    Stopped = 2,
    /// <summary><see cref="Vfx.PlayOn"/>: the entity is not in the client world (any more).</summary>
    EntityGone = 3,
    /// <summary>The game has no VFX with that GUID: ignored.</summary>
    UnknownVfx = 4,
    /// <summary>The FxController gave no handle (its pool is full).</summary>
    StartFailed = 5,
    /// <summary>Waited 10 s for an FxController update: there is no client world, or the VFX system isn't ready.</summary>
    TimedOut = 6,
    /// <summary>The client world ended.</summary>
    WorldEnded = 7,
    /// <summary>A game call faulted: kiln.vfx plays nothing more this session.</summary>
    Fault = 8,
    /// <summary>Refused on purpose by a test flag (<c>VfxTestSeams</c>) for a GUID the game knows.</summary>
    Refused = 9,
    /// <summary>No instance after 5 s: the body has no such slot, or the game's instance pool is full.</summary>
    NoInstance = 10,
}

/// <summary>Details of a VFX (client side).</summary>
/// <param name="Handle">The handle asked about.</param>
/// <param name="State">Where it stands.</param>
/// <param name="Reason">Why it ended or never started; <see cref="VfxEndReason.None"/> while it has not ended.</param>
/// <param name="FxHandle">The game's FxController handle for it; 0 for none.</param>
/// <param name="Instance">The game's VFX instance behind it; 0 for none yet (for example an attachment that is not resolved yet).</param>
/// <param name="Thread">The id of the thread that made the game call.</param>
/// <param name="Entity">The entity it was played on (<see cref="Vfx.PlayOn"/>); <see cref="EntityId.None"/> for a play at a position.</param>
/// <param name="Attached">A <see cref="Vfx.PlayOn"/> took effect: the effect is attached to the entity.</param>
/// <param name="Unmatched">The entity could not be verified near the position (a relayed <see cref="Vfx.PlayOn"/>): the effect played at <paramref name="Position"/> without attaching.</param>
/// <param name="StartDelay">Time from the request to the game's start of it; null until it started.</param>
/// <param name="Lifetime">The length the play settled on; <see cref="Vfx.UntilStopped"/> for a loop; null when it is not known (before it started).</param>
/// <param name="Position">The position it plays at.</param>
public sealed record VfxInfo(
    VfxHandle Handle, VfxState State, VfxEndReason Reason, uint FxHandle, uint Instance, int Thread, EntityId Entity, bool Attached, bool Unmatched,
    TimeSpan? StartDelay, TimeSpan? Lifetime, WorldPosition Position);

/// <summary>kiln.vfx's state. On a dedicated server <see cref="Native"/> is false and only the relay's numbers count.</summary>
/// <param name="Available">The same as <see cref="Vfx.Available"/>.</param>
/// <param name="Native">kiln.vfx is in this process (a client).</param>
/// <param name="Guard">The guard is hooked and its map layout checked out.</param>
/// <param name="Controller">An FxController update ran and no world has ended since.</param>
/// <param name="SystemReady">The game's VFX system has entries in its map.</param>
/// <param name="Simulated">A simulated game (labs and tests only).</param>
/// <param name="Registered">How many VFX are in the game's map.</param>
/// <param name="MapCapacity">The capacity of the game's VFX map.</param>
/// <param name="Thread">The thread of the FxController update; 0 when not seen yet.</param>
/// <param name="Live">Kiln's handles that are pending or playing.</param>
/// <param name="Played">Kiln's plays that reached the FxController.</param>
/// <param name="Failed">Kiln's plays that never started.</param>
/// <param name="Refused">Requests the guard refused: ones that would have aborted the game.</param>
/// <param name="Updates">FxController updates seen.</param>
/// <param name="Relay">The relay channel <c>kiln:vfx</c> is open.</param>
/// <param name="RelaySent">Relay messages this process has sent.</param>
/// <param name="RelayReceived">Relay messages this process has received.</param>
public sealed record VfxStatus(
    bool Available, bool Native, bool Guard, bool Controller, bool SystemReady, bool Simulated, uint Registered, uint MapCapacity, int Thread,
    uint Live, ulong Played, ulong Failed, ulong Refused, ulong Updates, bool Relay, long RelaySent, long RelayReceived);

/// <summary>
/// Play any VFX from code, at a position or attached to an entity's slot.
/// </summary>
/// <remarks>
/// <para><b>Client</b> (the game client, any thread): the call returns a handle at once; the game is called inside its next
/// FxController update. An unknown GUID is ignored with a log line: the guard around the game's own path means no VFX GUID can abort the
/// client.</para>
/// <para><b>Server</b> (dedicated server, or code on a hosting client's server-world tick thread): <see cref="PlayAt"/>,
/// <see cref="PlayOn"/>, <see cref="PlayFor"/>, <see cref="Stop"/> and <see cref="Set(VfxHandle,string,float)"/> are relayed to Kiln clients
/// on the channel <c>kiln:vfx</c> (vanilla clients see nothing new); a host's own client plays them directly. The handle is
/// <see cref="VfxHandle.IsRemote"/>. Prefer data for what every player must see: an effect started by a replicated entity, sequence or
/// buff also reaches joiners and vanilla clients.</para>
/// <para><b>How long.</b> The game has no "finished" signal. A one-shot (<c>lifetime</c> null on PlayAt) is stopped after the length of the
/// effect: for a VFX block, its delay, duration and longest particle life; otherwise what its resource gives (the larger of its kill time and
/// its duration + lifetime parameters, 10 s if it says nothing, 1 to 60 s). After a stop the
/// instance stays for its kill time at most (10 s for almost every game VFX), then goes whatever is alive. Pass
/// <see cref="UntilStopped"/> to keep a loop going. A mod's VFX stop when it unloads.</para>
/// </remarks>
public static unsafe class Vfx
{
    /// <summary>As a lifetime: runs until <see cref="Stop"/>.</summary>
    public static readonly TimeSpan UntilStopped = Timeout.InfiniteTimeSpan;

    /// <summary>Client: kiln.vfx can reach the game on this build (capabilities <c>kiln.vfx</c>, <c>vfx.play</c>). Server: a Kiln client
    /// has the relay channel (otherwise nobody would see anything; capability <c>vfx.relay</c>, which <see cref="Capabilities.Why"/>
    /// explains). False: calls return <see cref="VfxHandle.None"/> or relay to nobody.</summary>
    public static bool Available => VfxCore.ServerSide ? VfxRelay.HasListeners : VfxCore.LocalAvailable;

    /// <summary>Plays <paramref name="vfx"/> at <paramref name="at"/>. Server: relayed to Kiln clients within
    /// <paramref name="radius"/> metres (0 = 120, negative = every Kiln client).</summary>
    public static VfxHandle PlayAt(VfxId vfx, WorldPosition at, Quaternion? rotation = null, float scale = 1f, VfxParams? parameters = null,
        TimeSpan? lifetime = null, float radius = 0)
    {
        var ns = VfxCore.LifetimeNs(lifetime, loopByDefault: false);
        if (VfxCore.ServerSide)
            return VfxCore.Resolve(vfx, out var g)
                ? VfxRelay.Play(VfxRelay.KindAt, g, at, rotation, scale, EntityId.None, Slot.Root, default, ns, parameters, radius, EntityId.None)
                : VfxHandle.None;
        return VfxCore.PlayLocal(vfx, VfxNative.At, at, rotation, scale, EntityId.None, Slot.Root, default, ns, parameters);
    }

    /// <summary>Attaches <paramref name="vfx"/> to <paramref name="entity"/> at <paramref name="slot"/>. It moves with the entity and ends
    /// with it; without a <paramref name="lifetime"/> it runs until <see cref="Stop"/>. Client: a client-world entity (on a host the ids equal
    /// the server's). Server: relayed with the entity's id and position; a client attaches to its entity of that id when it stands within
    /// 10 m of the position, else plays at the position (ids aren't proven to match on a joined client).</summary>
    public static VfxHandle PlayOn(VfxId vfx, EntityId entity, Slot slot = Slot.Root, Offset offset = default, VfxParams? parameters = null,
        TimeSpan? lifetime = null, float radius = 0)
    {
        if (entity.IsNone) throw new ArgumentException("no entity", nameof(entity));
        var ns = VfxCore.LifetimeNs(lifetime, loopByDefault: true);
        if (VfxCore.ServerSide)
        {
            if (!VfxCore.Resolve(vfx, out var g)) return VfxHandle.None;
            Services.RequireTickThread("Vfx.PlayOn (server)");
            var at = World.Transform(entity)?.Position ?? throw new ArgumentException($"entity {entity} has no position", nameof(entity));
            return VfxRelay.Play(VfxRelay.KindOn, g, at, null, 1f, entity, slot, offset, ns, parameters, radius, EntityId.None);
        }
        return VfxCore.PlayLocal(vfx, VfxNative.On, default, null, 1f, entity, slot, offset, ns, parameters);
    }

    /// <summary>Server: plays <paramref name="vfx"/> at <paramref name="at"/> for one player (their Kiln client). Client: plays only when
    /// <paramref name="player"/> is the local player.</summary>
    public static VfxHandle PlayFor(EntityId player, VfxId vfx, WorldPosition at, VfxParams? parameters = null, TimeSpan? lifetime = null)
    {
        var ns = VfxCore.LifetimeNs(lifetime, loopByDefault: false);
        if (VfxCore.ServerSide)
            return VfxCore.Resolve(vfx, out var g)
                ? VfxRelay.Play(VfxRelay.KindFor, g, at, null, 1f, EntityId.None, Slot.Root, default, ns, parameters, 0, player)
                : VfxHandle.None;
        return player == Client.LocalPlayer ? VfxCore.PlayLocal(vfx, VfxNative.At, at, null, 1f, EntityId.None, Slot.Root, default, ns, parameters) : VfxHandle.None;
    }

    /// <summary>Stops a VFX (any thread on a client; tick thread for server handles). The game's stop: it goes at once when its kill time is
    /// 0, otherwise after that many seconds at most. <paramref name="immediate"/>: the instance is destroyed at the next update.</summary>
    public static void Stop(VfxHandle h, bool immediate = false)
    {
        if (h.IsNone) return;
        if (h.IsRemote)
        {
            VfxRelay.Stop(h, immediate);
            return;
        }
        var t = VfxNative.T;
        if (t != null) t->Stop(h.Value, immediate ? 1 : 0);
    }

    /// <summary>Sets a named parameter of a running VFX. A name or type the VFX does not have is ignored.</summary>
    public static void Set(VfxHandle h, string parameter, float value) => VfxCore.Set(h, VfxParam.Of(parameter, value));

    /// <inheritdoc cref="Set(VfxHandle,string,float)"/>
    public static void Set(VfxHandle h, string parameter, Vector3 value) => VfxCore.Set(h, VfxParam.Of(parameter, value));

    /// <inheritdoc cref="Set(VfxHandle,string,float)"/>
    public static void Set(VfxHandle h, string parameter, Color value) => VfxCore.Set(h, VfxParam.Of(parameter, value));

    /// <inheritdoc cref="Set(VfxHandle,string,float)"/>
    public static void Set(VfxHandle h, string parameter, int value) => VfxCore.Set(h, VfxParam.Of(parameter, value));

    /// <summary>Client: holds a reference on the VFX for the session, so its first real play does not start late (pipelines load in the
    /// background on first use). No effect on a server.</summary>
    public static void Preload(VfxId vfx)
    {
        var t = VfxNative.T;
        if (t == null) return;
        if (vfx.Guid == Guid.Empty && vfx.GameName is { } name)
        {
            VfxNames.WhenResolved(name, Send);
            return;
        }
        Send(vfx.Guid);

        void Send(Guid g)
        {
            if (g == Guid.Empty) return;
            Span<byte> b = stackalloc byte[16];
            g.TryWriteBytes(b);
            fixed (byte* p = b) t->Preload(p);
        }
    }

    /// <summary>The handle's state (client).</summary>
    [AnyThread]
    public static VfxState State(VfxHandle h)
    {
        var t = VfxNative.T;
        if (t == null || h.IsNone || h.IsRemote) return VfxState.None;
        return (VfxState)t->State(h.Value);
    }

    /// <summary>True while the VFX is queued or held by the FxController (false once it ended, after <see cref="Stop"/>, for unknown handles
    /// and for server handles).</summary>
    [AnyThread]
    public static bool IsPlaying(VfxHandle h) => State(h) is VfxState.Pending or VfxState.Playing;

    /// <summary>Details of a handle (client), null when unknown.</summary>
    [AnyThread]
    public static VfxInfo? Info(VfxHandle h)
    {
        var t = VfxNative.T;
        if (t == null || h.IsNone || h.IsRemote) return null;
        var i = new VfxNative.Info { Size = (uint)sizeof(VfxNative.Info) };
        if (t->GetInfo(h.Value, &i) != VfxNative.Ok) return null;
        return new VfxInfo(h, (VfxState)i.State, (VfxEndReason)i.Reason, i.FxHandle, i.Instance, (int)i.Thread, new EntityId(i.Entity), (i.Flags & 1) != 0, (i.Flags & 2) != 0,
            i.StartedMs != 0 ? TimeSpan.FromMilliseconds(i.StartedMs - i.QueuedMs) : null,
            i.LifetimeNs > 0 ? TimeSpan.FromTicks(i.LifetimeNs / 100) : i.LifetimeNs < 0 ? UntilStopped : null,
            i.Position.ToPosition());
    }

    /// <summary>Stops every VFX the calling mod started (client).</summary>
    public static int StopAll(bool immediate = false)
    {
        var t = VfxNative.T;
        return t == null ? 0 : (int)t->StopOwner(Services.CallerOwner, immediate ? 1 : 0);
    }

    /// <summary>Client: the game has this VFX registered. False for an unknown one and when the VFX system isn't known yet
    /// (see <see cref="VfxStatus.SystemReady"/>).</summary>
    public static bool IsKnown(VfxId vfx)
    {
        var t = VfxNative.T;
        if (t == null || vfx.IsNone) return false;
        var g = vfx.Guid;
        if (g == Guid.Empty) return false;
        Span<byte> b = stackalloc byte[16];
        g.TryWriteBytes(b);
        fixed (byte* p = b) return t->IsRegistered(p) == 1;
    }

    /// <summary>kiln.vfx's and the relay's state.</summary>
    [AnyThread]
    public static VfxStatus Status()
    {
        VfxStartup.Ensure();
        var relay = VfxRelay.Open;
        var t = VfxNative.T;
        if (t == null)
            return new VfxStatus(Available, false, false, false, false, false, 0, 0, 0, 0, 0, 0, 0, 0, relay, VfxRelay.Sent, VfxRelay.ReceivedCount);
        var s = new VfxNative.Status { Size = (uint)sizeof(VfxNative.Status) };
        t->GetStatus(&s);
        return new VfxStatus(Available, true, (s.Flags & VfxNative.HasGuard) != 0, (s.Flags & VfxNative.Controller) != 0, (s.Flags & VfxNative.SystemReady) != 0,
            (s.Flags & VfxNative.Sim) != 0, s.Registered, s.MapCapacity, (int)s.Thread, s.Live, s.Played, s.Failed, s.Refused, s.Updates, relay, VfxRelay.Sent, VfxRelay.ReceivedCount);
    }

    /// <summary>
    /// Client: the game's VFX resources for this process, all mods together (chapter 17's budget). The game's VFX system holds a
    /// fixed number of them, and registering one more than that aborts the game, so Kiln refuses a mod that would not fit.
    /// <see cref="CapacityInfo.Capacity"/> is that number as kiln.vfx reads it from the game's own table (2,048 on the game client
    /// 26430625); <see cref="CapacityInfo.GameUsed"/> the game's own VFX (1,575 there, counted in its archive);
    /// <see cref="CapacityInfo.KilnUsed"/> Kiln's VFX of loaded mods that the game's table holds with the bytes Kiln served;
    /// <see cref="CapacityInfo.Reserved"/> the rest of the table: VFX of a mod that is no longer loaded (they stay until the
    /// process ends) and other loaders' resources. <see cref="CapacityInfo.Available"/> is what is left for new VFX, which the
    /// game lists once per start: a new one is there from the next start (<c>Blocks.Define(name, VfxBlock, options)</c>).
    /// <para>Ready once kiln.vfx has read the table, at the first update of a client world (the main menu has none); before that
    /// <c>NotReady</c>, with no counts: nothing is assumed. <c>UnknownExternalUsage</c> when the game's own count could not be
    /// read from its archive, <c>AccountingMismatch</c> when the table's counts do not add up. A change posts
    /// <see cref="CapacityChangedEvent"/> (<c>Quota</c> <c>vfx.budget</c>) at a later step while something listens.</para>
    /// <para>A snapshot kiln.vfx keeps: any thread, never a read of the game's memory. Not run in a game client yet (guided check
    /// <c>17-vfx-budget.before</c> and <c>17-vfx-budget.after</c>).</para>
    /// </summary>
    /// <exception cref="KilnUnavailableException"><c>vfx.budget</c>: no kiln.vfx here (a dedicated server has no VFX), one older
    /// than 0.2.0-dev.29, or one that cannot read the game's VFX table on this game build.</exception>
    [ClientOnly]
    [AnyThread]
    [RequiresCapability(VfxCaps.BudgetRow)]
    public static CapacityQueryResult ResourceBudget
    {
        get
        {
            var t = VfxNative.Has(VfxNative.Member.ResourceBudget);
            if (t == null)
                throw VfxCaps.Off(VfxCaps.BudgetRow, VfxNative.T == null
                    ? "kiln.vfx is not here (a dedicated server has no VFX)"
                    : "this kiln.vfx has no resource budget (update Kiln)");
            var n = KilnCapacityInfo.New();
            var rc = t->ResourceBudget(&n);
            if (rc == VfxNative.Unavailable) throw VfxCaps.Off(VfxCaps.BudgetRow, "kiln.vfx cannot read the game's VFX table on this game build");
            if (rc != VfxNative.Ok) throw new InvalidOperationException($"Vfx.ResourceBudget: kiln.vfx answered {rc}");
            return VfxBudgetWatch.Seen(CapacityMapping.ToResult(n));
        }
    }

    /// <summary>Diagnostics as JSON (status and recent handles, client).</summary>
    [AnyThread]
    public static string StatusJson()
    {
        var t = VfxNative.T;
        if (t == null) return "{\"available\":false}";
        nuint n = 0;
        t->StatusJson(null, 0, &n);
        var buf = new byte[(int)n + 256];
        fixed (byte* p = buf)
            if (t->StatusJson(p, (nuint)buf.Length, &n) != VfxNative.Ok) return "{}";
        return Encoding.UTF8.GetString(buf, 0, (int)n);
    }
}

/// <summary>Test seams, for mods/tests/vfx-test. The guard ones skip Kiln's own check on purpose, so the guard in the game's path has to
/// catch the GUID; both refuse a GUID the game knows. Not for mods: a mod has <see cref="Vfx"/>.</summary>
[System.Diagnostics.CodeAnalysis.Experimental("KILNX001", UrlFormat = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/modding/analyzers.md#kilnx001")]
[System.ComponentModel.EditorBrowsable(System.ComponentModel.EditorBrowsableState.Never)]
public static unsafe class VfxTestSeams
{
    /// <summary>Through the FxController, the way data-driven VFX go: the guard at the start must empty it.</summary>
    public static VfxHandle GuardThroughFxController(Guid unknown, WorldPosition at) =>
        VfxCore.PlayLocal(unknown, VfxNative.At | VfxNative.TestFx, at, null, 1f, EntityId.None, Slot.Root, default, 0, null);

    /// <summary>Straight into RenderCreateVfx, the way the callers without a controller go: the guard on VfxCreateInstance must return 0.</summary>
    public static VfxHandle GuardThroughCreateInstance(Guid unknown) =>
        VfxCore.PlayLocal(unknown, VfxNative.TestCreate, default, null, 1f, EntityId.None, Slot.Root, default, 0, null);

    /// <summary>The mirror's sizes, which must equal the C header's.</summary>
    public static (int Play, int Info, int Status, int Table) NativeSizes => VfxNative.Sizes;

    /// <summary>The relay's receive side in this process: encodes a PlayOn as the server would, decodes it and plays it. Returns the local handle.</summary>
    public static VfxHandle LoopbackPlay(Guid vfx, WorldPosition at, float scale, EntityId entity, Slot slot, Offset offset, long lifetimeNs, VfxParams? parameters) =>
        VfxRelay.Loopback(vfx, at, scale, entity, slot, offset, lifetimeNs, parameters);

    /// <summary>The relay's stop, on a handle from <see cref="LoopbackPlay"/>.</summary>
    public static void LoopbackStop(VfxHandle local) => VfxRelay.LoopbackStop(local);
}

/// <summary>Shared plumbing of Vfx and the relay.</summary>
internal static unsafe class VfxCore
{
    public const uint RemoteBit = 0x8000_0000;
    private static readonly object Lock = new();
    private static readonly HashSet<ModContext> Watched = new(ReferenceEqualityComparer.Instance);
    private static int _warnedOff;

    static VfxCore() => VfxStartup.Ensure();

    /// <summary>Client playback works: the guard is on and play reaches the game.</summary>
    public static bool LocalAvailable
    {
        get
        {
            var t = VfxNative.T;
            if (t == null) return false;
            var s = new VfxNative.Status { Size = (uint)sizeof(VfxNative.Status) };
            return t->GetStatus(&s) == VfxNative.Ok && (s.Flags & VfxNative.HasGuard) != 0 && (s.Flags & VfxNative.HasPlay) != 0;
        }
    }

    private static bool Simulated
    {
        get
        {
            var t = VfxNative.T;
            if (t == null) return false;
            var s = new VfxNative.Status { Size = (uint)sizeof(VfxNative.Status) };
            return t->GetStatus(&s) == VfxNative.Ok && (s.Flags & VfxNative.Sim) != 0;
        }
    }

    /// <summary>The call is server code: a dedicated server, or a hosting client's server-world tick thread. A simulated game (labs) counts as a client.</summary>
    public static bool ServerSide => !Simulated && (Client.IsDedicatedServer || (Client.IsHosting && Game.IsTickThread && !Client.IsClientThread));

    public static long LifetimeNs(TimeSpan? t, bool loopByDefault)
    {
        if (t is null) return loopByDefault ? -1 : 0;
        if (t == Vfx.UntilStopped) return -1;
        return Math.Max(1, t.Value.Ticks * 100);
    }

    /// <summary>The GUID of <paramref name="vfx"/>, with one warning when a name finds nothing.</summary>
    public static bool Resolve(VfxId vfx, out Guid guid)
    {
        guid = vfx.Guid;
        return guid != Guid.Empty;
    }

    /// <summary>Plays on this client. A name only another thread can resolve doesn't hold the caller up: the handle comes at once and the
    /// entry waits for its GUID, as every request waits for the game's next update.</summary>
    public static VfxHandle PlayLocal(VfxId vfx, uint flags, WorldPosition at, Quaternion? rotation, float scale, EntityId entity, Slot slot, Offset offset,
        long lifetimeNs, VfxParams? parameters)
    {
        var g = vfx.Guid;
        if (g != Guid.Empty || vfx.GameName is not { } name)
            return g == Guid.Empty ? VfxHandle.None : PlayLocal(g, flags, at, rotation, scale, entity, slot, offset, lifetimeNs, parameters);
        if (!VfxNames.OnlyElsewhere(name)) return VfxHandle.None;  // this thread may ask, and the answer was no
        var h = PlayLocal(Guid.Empty, flags | VfxNative.WaitGuid, at, rotation, scale, entity, slot, offset, lifetimeNs, parameters);
        if (!h.IsNone)
            VfxNames.WhenResolved(name, resolved =>
            {
                var t = VfxNative.T;
                if (t == null) return;
                Span<byte> b = stackalloc byte[16];
                resolved.TryWriteBytes(b);
                fixed (byte* p = b) t->BindGuid(h.Value, p);
            });
        return h;
    }

    public static ulong Owner()
    {
        var mod = ModContext.CurrentMod;
        if (mod is null) return 0;
        lock (Lock)
        {
            if (Watched.Add(mod))
                mod.Retiring += () =>
                {
                    var t = VfxNative.T;
                    if (t != null)
                    {
                        var n = t->StopOwner(mod.Owner, 0);
                        if (n > 0) VfxLog.Info($"{mod.Info.Id} unloaded: {n} VFX stopped");
                    }
                    VfxRelay.Retire(mod);
                    lock (Lock) Watched.Remove(mod);
                };
        }
        return mod.Owner;
    }

    public static VfxHandle PlayLocal(Guid g, uint flags, WorldPosition at, Quaternion? rotation, float scale, EntityId entity, Slot slot, Offset offset,
        long lifetimeNs, VfxParams? parameters, float verifyRadius = 0)
    {
        var t = VfxNative.T;
        if (t == null || !LocalAvailable)
        {
            if (Interlocked.Exchange(ref _warnedOff, 1) == 0)
                VfxLog.Warn("kiln.vfx is not available here (service off, or its signatures did not resolve on this game build): nothing plays");
            return VfxHandle.None;
        }
        // Parameters go into the block the VFX starts with (kiln.vfx), so what it spawns at once has them. The play waits until they are all
        // queued: the update that starts it may run on another thread between Play and SetParam. A play that already waits for its GUID
        // (a name resolved at the next tick) is released later anyway.
        var hold = parameters is { Items.Count: > 0 } && (flags & VfxNative.WaitGuid) == 0 && g != Guid.Empty;
        var p = new VfxNative.Play
        {
            Size = (uint)sizeof(VfxNative.Play),
            Flags = hold ? flags | VfxNative.WaitGuid : flags,
            Position = new VfxNative.Vec3(at),
            Scale = scale,
            Entity = entity.Value,
            Slot = (uint)slot,
            VerifyRadius = verifyRadius,
            LifetimeNs = lifetimeNs,
        };
        VfxNative.WriteGuid(g, p.Vfx);
        if (rotation is { } r)
        {
            p.Rotation[0] = r.X;
            p.Rotation[1] = r.Y;
            p.Rotation[2] = r.Z;
            p.Rotation[3] = r.W;
        }
        p.Local[0] = offset.Local.X;
        p.Local[1] = offset.Local.Y;
        p.Local[2] = offset.Local.Z;
        p.World[0] = offset.World.X;
        p.World[1] = offset.World.Y;
        p.World[2] = offset.World.Z;
        if (offset.Rotation is { } o)
        {
            p.OffsetRotation[0] = o.X;
            p.OffsetRotation[1] = o.Y;
            p.OffsetRotation[2] = o.Z;
            p.OffsetRotation[3] = o.W;
        }
        var h = new VfxHandle(t->Play(Owner(), &p));
        if (!h.IsNone && parameters is not null)
        {
            foreach (var param in parameters.Items) SetLocal(h, param);
            if (hold) t->BindGuid(h.Value, p.Vfx);
        }
        return h;
    }

    public static void Set(VfxHandle h, VfxParam param)
    {
        if (h.IsNone) return;
        if (h.IsRemote) VfxRelay.Set(h, param);
        else SetLocal(h, param);
    }

    public static void SetLocal(VfxHandle h, VfxParam param)
    {
        var t = VfxNative.T;
        if (t == null) return;
        fixed (byte* d = param.Data) t->SetParam(h.Value, param.Hash, param.Type, d, (nuint)param.Data.Length);
    }
}

/// <summary>Game names to GUIDs. VFX have no names of their own (docs/re/vfx.md section 1). The game's data answers only on its world's
/// thread, so a question from any other thread waits for the next service tick.</summary>
internal static class VfxNames
{
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<string, Guid> Known = new();
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<string, byte> Warned = new();
    private static readonly System.Collections.Concurrent.ConcurrentQueue<(string Name, Action<Guid>? Then)> Asked = new();

    /// <summary>The GUID when it's known or this thread may look it up. Off the data's thread: <see cref="Guid.Empty"/>, and the name is
    /// looked up at the next tick.</summary>
    public static Guid Resolve(string name)
    {
        if (Known.TryGetValue(name, out var g)) return g;
        g = Lookup(name, out var wrongThread);
        if (wrongThread)
        {
            VfxStartup.Ensure();
            Asked.Enqueue((name, null));
            return Guid.Empty;
        }
        if (g != Guid.Empty) Known[name] = g;
        else if (GameDataMirror.Available && Warned.TryAdd(name, 0))
            VfxLog.Warn($"no game VFX named '{name}' (a template carrying one, or a sequence playing one), or the game's data isn't there yet");
        return g;
    }

    /// <summary>True when this thread can't tell: only the data's thread can answer for <paramref name="name"/>.</summary>
    public static bool OnlyElsewhere(string name)
    {
        if (Known.ContainsKey(name)) return false;
        Lookup(name, out var wrongThread);
        return wrongThread;
    }

    /// <summary>Calls <paramref name="then"/> with the GUID (<see cref="Guid.Empty"/>: no such name) at the next tick, on the tick's thread.</summary>
    public static void WhenResolved(string name, Action<Guid> then)
    {
        VfxStartup.Ensure();
        Asked.Enqueue((name, then));
    }

    /// <summary>A service tick: on the thread the game's data answers on.</summary>
    public static void Drain(in TickInfo tick)
    {
        while (Asked.TryDequeue(out var q))
        {
            var g = Resolve(q.Name);
            if (g == Guid.Empty && OnlyElsewhere(q.Name))
            {
                Asked.Enqueue(q);  // not this tick's thread after all (a client between worlds); a waiting play times out by itself
                return;
            }
            try
            {
                q.Then?.Invoke(g);
            }
            catch (Exception e)
            {
                VfxLog.Warn($"name '{q.Name}': {e.Message}");
            }
        }
    }

    private static Guid Lookup(string name, out bool wrongThread)
    {
        wrongThread = false;
        try
        {
            var json = GameDataMirror.Read(GameDataMirror.SectionTemplates, name, "VfxComponentResource.vfx", out var rc);
            if (rc == GameDataMirror.WrongThread)
            {
                wrongThread = true;
                return Guid.Empty;
            }
            var g = FromJson(json);
            return g != Guid.Empty ? g : FromJson(GameDataMirror.Read(GameDataMirror.SectionSequences, name, "events[VfxNotifierEvent#0].vfx", out _));
        }
        catch (Exception e)
        {
            VfxLog.Debug($"name lookup '{name}': {e.Message}");
            return Guid.Empty;
        }
    }

    // kiln.gamedata's answer: [{ "path":…, "type":…, "value": "<guid>" … }]. An empty reference is no VFX.
    private static Guid FromJson(string? json)
    {
        if (string.IsNullOrEmpty(json)) return Guid.Empty;
        using var doc = System.Text.Json.JsonDocument.Parse(json);
        var e = doc.RootElement;
        if (e.ValueKind == System.Text.Json.JsonValueKind.Array)
        {
            if (e.GetArrayLength() == 0) return Guid.Empty;
            e = e[0];
        }
        if (e.ValueKind != System.Text.Json.JsonValueKind.Object || !e.TryGetProperty("value", out var v)) return Guid.Empty;
        return v.ValueKind == System.Text.Json.JsonValueKind.String && Guid.TryParse(v.GetString(), out var g) ? g : Guid.Empty;
    }
}

internal static class VfxLog
{
    public static void Info(string text) => Backend.Current.Log(0, LogLevel.Info, "vfx: " + text);
    public static void Warn(string text) => Backend.Current.Log(0, LogLevel.Warn, "vfx: " + text);
    public static void Debug(string text) => Backend.Current.Log(0, LogLevel.Debug, "vfx: " + text);
}
