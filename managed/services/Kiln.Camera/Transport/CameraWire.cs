// SPDX-License-Identifier: MIT
// Kiln.Camera's wire (chapter 03 §4.6): the messages 0-8 of channel "kiln.camera" v1 and the canonical request blob.
// Pure code: no game, no network, no clock. Kiln.Network carries the message id beside the body (NetChannel.SendRaw /
// OnRaw), so every body here starts with its own fields. Little-endian integers, IEEE 754 little-endian floats, no C
// padding, no pointers. Every limit is checked before anything is allocated, and every enum against the exact values of
// kiln_camera.h (out/camera/probes/camera_proposal.h); a body that fails any check decodes to nothing.
using System.Buffers.Binary;
using System.Text;

// Namespace Kiln.CameraTransport, not Kiln.Camera.Transport: a namespace Kiln.Camera would collide with S2's class Kiln.Camera.
namespace Kiln.CameraTransport;

/// <summary>The message ids of channel <c>kiln.camera</c> v1 (§4.6). Fixed: never renumber.</summary>
internal enum CameraMessage : ushort
{
    Hello = 0,      // client -> server
    Welcome = 1,    // server -> client
    Apply = 2,      // server -> client
    Update = 3,     // server -> client
    End = 4,        // server -> client
    Ack = 5,        // client -> server
    Ended = 6,      // client -> server
    Heartbeat = 7,  // server -> client
    Status = 8,     // client -> server
}

/// <summary>The numbers of kiln_camera.h the wire carries (camera_proposal.h).</summary>
internal static class CameraWireAbi
{
    public const ushort Protocol = 1;
    public const int MaxBody = 16384;            // the channel's maximum; the effective one is negotiated below it
    public const int MaxKeys = 256;              // KILN_CAMERA_MAX_KEYS
    public const uint MaxDurationMs = 120000;    // KILN_CAMERA_MAX_DURATION_MS
    public const int MaxDiagnosticBytes = 256;
    public const int KeyBytes = 48;              // at_ms u32, position 3 x f64, rotation 4 x f32, fov f32
    public const int BindingBytes = 44;          // token, server nonce/epoch, client nonce/epoch, player, incarnation
    public const int CommonHeaderBytes = 84;     // binding + owner token, lease, request id, endpoint serial, revision
    public const int BlobFixedBytes = 22;        // parts, policy, duration, blend in/out, priority, lens source

    // parts (KILN_CAMERA_MODE ... LENS)
    public const uint PartMode = 1, PartPath = 2, PartProfile = 4, PartShake = 8, PartLens = 16;
    public const uint KnownParts = PartMode | PartPath | PartProfile | PartShake | PartLens;
    // input policy, lens source, rotation mode, collision, transition, update timing
    public const byte VisualOnly = 0, StockMode = 1, AimToo = 2;
    public const byte LensNone = 0, LensConstant = 1, LensKeys = 2;
    public const byte FreeOrbit = 0, FollowHeading = 1, FixedWorld = 2;
    public const byte CollisionNone = 0;
    public const byte TransitionNone = 0, TransitionBlink = 1, TransitionLerp = 2;
    public const byte TimingPreserve = 0, TimingRestart = 1;
    public const byte MaxCameraId = 45;          // CameraId 0..45
    // capability bits the remote end can negotiate (the local part rows)
    public const ulong CapLocalPose = 1ul << 1, CapLocalFov = 1ul << 2, CapLocalProfile = 1ul << 3, CapLocalShake = 1ul << 4;
    public const ulong RemoteParts = CapLocalPose | CapLocalFov | CapLocalProfile | CapLocalShake;
    // results
    public const byte CommitNone = 0, CommitServerState = 1, CommitClientFrame = 2;
    public const uint FlagMayHaveApplied = 1, FlagAlreadyEnded = 2;
    public const byte MaxEndReason = 13;         // KILN_CAMERA_END_CANCELLED_BEFORE_APPLY
    public const ushort MaxReason = 27;          // KILN_CAMERA_REASON_BODY_TOO_LARGE
}

/// <summary>KilnCameraReason (camera_proposal.h): the values the wire carries in Ack/Ended.</summary>
internal static class CameraReasons
{
    public const ushort None = 0, Busy = 1, AlreadyOwned = 2, UnknownLease = 3, ForeignOwner = 4, WrongEndpoint = 5,
        InputPolicy = 6, RequiresKilnClient = 7, PeerUnavailable = 8, Negotiation = 9, TargetUnavailable = 10,
        NoCameraFrame = 11, StaleWorld = 12, ExpiredUpdate = 13, Collision = 14, TransportTimeout = 15,
        MalformedRemote = 16, ResourceBusy = 17, FrameFault = 18, SnapshotBusy = 19, SnapshotStale = 20, Bounds = 21,
        RateLimit = 22, LeaseKind = 23, Cancelled = 24, ClientCapability = 25, InsufficientLifetime = 26, BodyTooLarge = 27;
}

/// <summary>KilnCameraEndReason values.</summary>
internal static class CameraEndReasons
{
    public const byte None = 0, Released = 1, Expired = 2, TargetLost = 3, PlayerLeft = 4, WorldStopped = 5, Superseded = 6,
        CapabilityLost = 7, OwnerRetired = 8, PeerDisconnected = 9, AuthorityTimeout = 10, SafetyRejected = 11,
        TransportTimeout = 12, CancelledBeforeApply = 13;
}

/// <summary>One key of a path: at_ms, position (double metres), rotation (x, y, z, w), keyed FOV (0 unless LENS_KEYS).</summary>
internal readonly record struct WireKey(uint AtMs, double X, double Y, double Z, float Qx, float Qy, float Qz, float Qw, float Fov);

/// <summary>
/// The canonical request blob's fields, exactly as §4.6 lists them. Internal: the authored records (CameraOverride and
/// the parts of §5.2) belong to S2, and the native request (KilnCameraRequest) to S8.
/// ADAPTER POINT (S8): the server bridge fills this from the KilnCameraRequest native kiln.camera hands to
/// KilnCameraTransport.submit; the client side turns it back into a KilnCameraRequest for apply/update. Nothing else in
/// Transport/ knows either shape.
/// </summary>
internal sealed record CameraWireRequest
{
    public uint Parts { get; init; }
    public byte InputPolicy { get; init; }
    public uint DurationMs { get; init; }
    public uint BlendInMs { get; init; }
    public uint BlendOutMs { get; init; }
    public int Priority { get; init; }
    public byte LensSource { get; init; }
    // MODE (normally invalid on this visual channel, but carried exactly)
    public byte CameraId { get; init; }
    public byte Transition { get; init; }
    // PROFILE
    public uint FollowEntity { get; init; }
    public byte RotationMode { get; init; }
    public byte Collision { get; init; }
    public float Distance { get; init; }
    public float OffsetX { get; init; }
    public float OffsetY { get; init; }
    public float OffsetZ { get; init; }
    public float OffsetWorldY { get; init; }
    public float RotX { get; init; }
    public float RotY { get; init; }
    public float RotZ { get; init; }
    public float RotW { get; init; } = 1f;
    // LENS (constant)
    public float FovDegrees { get; init; }
    // SHAKE
    public float Amplitude { get; init; }
    public float Frequency { get; init; }
    public uint FalloffMs { get; init; }
    public uint Seed { get; init; }
    // PATH
    public WireKey[] Keys { get; init; } = [];

    public bool Has(uint part) => (Parts & part) != 0;

    /// <summary>Same fields, keys compared by value (the record's default compares the array by reference).</summary>
    public bool SameAs(CameraWireRequest? o) => o is not null && this with { Keys = [] } == o with { Keys = [] } && Keys.AsSpan().SequenceEqual(o.Keys);
}

/// <summary>The four ids every binding message carries: token, both worlds' nonces and epochs, the player and its
/// incarnation. Server and client epochs are separate numbers: never compared with each other.</summary>
internal readonly record struct WireBinding(ulong Token, ulong ServerNonce, uint ServerEpoch, ulong ClientNonce, uint ClientEpoch,
    uint Player, ulong Incarnation);

/// <summary>The common header of Apply, Update, End, Ack and Ended (84 bytes).</summary>
internal readonly record struct WireHeader(WireBinding Binding, ulong OwnerToken, ulong Lease, ulong RequestId, ulong Serial, ulong Revision);

internal readonly record struct HelloMessage(ushort Protocol, ulong ClientNonce, uint ClientEpoch, uint LocalEntity, ulong Caps,
    ushort MaxKeys, uint MaxBody);

internal readonly record struct WelcomeMessage(ulong ClientNonce, uint ClientEpoch, ulong ServerNonce, uint ServerEpoch, uint Player,
    ulong Incarnation, ulong Token, ulong AllowedCaps, uint MaxBody, ushort MaxKeys);

/// <summary>Apply (Timing absent), Update (Timing Preserve/Restart) or End (Request null).</summary>
internal sealed record MutationMessage(CameraMessage Kind, WireHeader Header, byte Timing, CameraWireRequest? Request);

internal readonly record struct AckMessage(WireHeader Header, bool Ok, ushort Reason, byte Scope, uint Flags, ulong Frame,
    uint ElapsedMs, uint RemainingMs, string Diagnostic);

internal readonly record struct EndedMessage(WireHeader Header, byte EndReason, ushort Reason, ulong Frame);

internal readonly record struct HeartbeatMessage(WireBinding Binding, ulong Sequence, ulong HighestSerial, ulong Lease, ulong Revision);

internal readonly record struct StatusMessage(WireBinding Binding, ulong Sequence, ulong Lease, ulong Revision, uint ElapsedMs, uint RemainingMs);

/// <summary>Encoding and decoding of the nine messages. Decode methods return false for any body that is not exactly a
/// valid message (but for the transport's zero padding to 4 bytes, <see cref="EndsAt"/>): too long, truncated, trailing bytes, an unknown enum value, a non-finite float, a part the request does
/// not announce; a refused body leaves the out value default (null for a mutation), never a partly read message. Nothing
/// is allocated for a body before its length and counts were checked.</summary>
internal static class CameraWire
{
    // ---- the transport's padding ----

    /// <summary>True when the body ends at <paramref name="end"/>, or only kiln.net's zero padding follows it. The game
    /// transport pads every frame with zeros to a multiple of 4 bytes (native/services/net/game.cpp send: the game's bit
    /// writers flush 32-bit words), and Kiln.Network's Data frame header is 8 bytes, so a joined peer's body arrives with up
    /// to 3 zero bytes after its end whenever its length is not a multiple of 4 (a fragmented body arrives exact; the lab
    /// transport and the hosting loopback pad nothing). Anything else after the end, more than 3 bytes, or a zero byte
    /// that leaves the length unaligned is refused, as kiln.markers' wire does since 0.2.0-dev.31.12.</summary>
    internal static bool EndsAt(ReadOnlySpan<byte> s, int end)
    {
        if (end < 0 || end > s.Length) return false;
        if (end == s.Length) return true;
        return s.Length % 4 == 0 && s.Length - end <= 3 && s[end..].IndexOfAnyExcept((byte)0) < 0;
    }

    /// <summary>The longest body a decoder looks at for a message of at most <paramref name="max"/> bytes: the transport
    /// may add up to 3 zero bytes. The message itself is still checked against <paramref name="max"/>.</summary>
    private static int Padded(int max) => max > int.MaxValue - 3 ? int.MaxValue : max + 3;

    // ---- the request blob ----

    /// <summary>The blob's size in bytes, or -1 when its structure cannot be encoded (a key count past 65535).</summary>
    public static int BlobSize(CameraWireRequest r)
    {
        long n = CameraWireAbi.BlobFixedBytes;
        if (r.Has(CameraWireAbi.PartMode)) n += 2;
        if (r.Has(CameraWireAbi.PartProfile)) n += 42;
        if (r.Has(CameraWireAbi.PartLens) && r.LensSource == CameraWireAbi.LensConstant) n += 4;
        if (r.Has(CameraWireAbi.PartShake)) n += 16;
        if (r.Has(CameraWireAbi.PartPath))
        {
            if (r.Keys.Length > ushort.MaxValue) return -1;
            n += 2 + (long)r.Keys.Length * CameraWireAbi.KeyBytes;
        }
        return n > int.MaxValue ? -1 : (int)n;
    }

    /// <summary>The checks both ends make on a request (§4.4, §4.6): known part bits and at least one, exact enums,
    /// the lens source's structure, key counts, times, priority, finite floats and the context-free numeric limits. The
    /// limits that need the world (250 m from the local player, the selected lens) are native kiln.camera's.
    /// Returns <see cref="CameraReasons.None"/>, or the reason a peer answers with.</summary>
    public static ushort Validate(CameraWireRequest r, int maxKeys = CameraWireAbi.MaxKeys)
    {
        const ushort bad = CameraReasons.MalformedRemote, bounds = CameraReasons.Bounds;
        if (r.Parts == 0 || (r.Parts & ~CameraWireAbi.KnownParts) != 0) return bad;
        if (r.InputPolicy > CameraWireAbi.AimToo || r.LensSource > CameraWireAbi.LensKeys) return bad;
        if (r.DurationMs is 0 or > CameraWireAbi.MaxDurationMs) return bounds;
        if ((ulong)r.BlendInMs + r.BlendOutMs > r.DurationMs) return bounds;
        if (r.Priority is < -1000 or > 1000) return bounds;
        // lens source: NONE without LENS; CONSTANT with LENS; KEYS with LENS and PATH
        var lens = r.Has(CameraWireAbi.PartLens);
        if (!lens && r.LensSource != CameraWireAbi.LensNone) return bad;
        if (lens && r.LensSource == CameraWireAbi.LensNone) return bad;
        if (r.LensSource == CameraWireAbi.LensKeys && !r.Has(CameraWireAbi.PartPath)) return bad;
        if (r.Has(CameraWireAbi.PartPath) && r.Has(CameraWireAbi.PartProfile)) return bad;  // Path OR Profile
        if (r.Has(CameraWireAbi.PartMode) && r.Parts != CameraWireAbi.PartMode) return bad;  // Mode combines with nothing
        if (r.Has(CameraWireAbi.PartMode) && (r.CameraId > CameraWireAbi.MaxCameraId || r.Transition > CameraWireAbi.TransitionLerp)) return bad;
        if (r.Has(CameraWireAbi.PartProfile))
        {
            if (r.RotationMode > CameraWireAbi.FixedWorld || r.Collision != CameraWireAbi.CollisionNone) return bad;
            if (!Finite(r.Distance, r.OffsetX, r.OffsetY, r.OffsetZ, r.OffsetWorldY, r.RotX, r.RotY, r.RotZ, r.RotW)) return bad;
            if (r.Distance is < 0.25f or > 200f || Math.Abs(r.OffsetWorldY) > 100f) return bounds;
            if (Math.Sqrt((double)r.OffsetX * r.OffsetX + (double)r.OffsetY * r.OffsetY + (double)r.OffsetZ * r.OffsetZ) > 100) return bounds;
            if (!UnitQuaternion(r.RotX, r.RotY, r.RotZ, r.RotW)) return bounds;
        }
        if (lens && r.LensSource == CameraWireAbi.LensConstant)
        {
            if (!float.IsFinite(r.FovDegrees)) return bad;
            if (r.FovDegrees is < 5f or > 160f) return bounds;
        }
        if (r.Has(CameraWireAbi.PartShake))
        {
            if (!Finite(r.Amplitude, r.Frequency)) return bad;
            if (r.Amplitude is < 0f or > 0.5f || r.Frequency is < 0.1f or > 30f || r.FalloffMs > r.DurationMs) return bounds;
        }
        if (r.Has(CameraWireAbi.PartPath))
        {
            if (r.Keys.Length < 1 || r.Keys.Length > Math.Min(maxKeys, CameraWireAbi.MaxKeys)) return bounds;
            uint last = 0;
            for (var i = 0; i < r.Keys.Length; i++)
            {
                var k = r.Keys[i];
                if (!double.IsFinite(k.X) || !double.IsFinite(k.Y) || !double.IsFinite(k.Z) || !Finite(k.Qx, k.Qy, k.Qz, k.Qw, k.Fov)) return bad;
                if (Math.Abs(k.X) > 1_000_000 || Math.Abs(k.Y) > 1_000_000 || Math.Abs(k.Z) > 1_000_000) return bounds;
                if (!UnitQuaternion(k.Qx, k.Qy, k.Qz, k.Qw)) return bounds;
                // §4.3: the first key at 0, then strictly increasing exact milliseconds, the last at most Duration
                if ((i == 0 && k.AtMs != 0) || k.AtMs > r.DurationMs || (i > 0 && k.AtMs <= last)) return bounds;
                last = k.AtMs;
                if (r.LensSource == CameraWireAbi.LensKeys ? k.Fov is < 5f or > 160f : k.Fov != 0f) return r.LensSource == CameraWireAbi.LensKeys ? bounds : bad;
            }
        }
        else if (r.Keys.Length != 0) return bad;
        return CameraReasons.None;
    }

    private static bool Finite(params float[] v)
    {
        foreach (var x in v) if (!float.IsFinite(x)) return false;
        return true;
    }

    private static bool UnitQuaternion(float x, float y, float z, float w)
    {
        var n = (double)x * x + (double)y * y + (double)z * z + (double)w * w;
        return n > 0 && Math.Abs(n - 1) <= 0.001;
    }

    private static void WriteBlob(Span<byte> s, ref int at, CameraWireRequest r)
    {
        U32(s, ref at, r.Parts);
        U8(s, ref at, r.InputPolicy);
        U32(s, ref at, r.DurationMs);
        U32(s, ref at, r.BlendInMs);
        U32(s, ref at, r.BlendOutMs);
        I32(s, ref at, r.Priority);
        U8(s, ref at, r.LensSource);
        if (r.Has(CameraWireAbi.PartMode))
        {
            U8(s, ref at, r.CameraId);
            U8(s, ref at, r.Transition);
        }
        if (r.Has(CameraWireAbi.PartProfile))
        {
            U32(s, ref at, r.FollowEntity);
            U8(s, ref at, r.RotationMode);
            U8(s, ref at, r.Collision);
            F32(s, ref at, r.Distance);
            F32(s, ref at, r.OffsetX);
            F32(s, ref at, r.OffsetY);
            F32(s, ref at, r.OffsetZ);
            F32(s, ref at, r.OffsetWorldY);
            F32(s, ref at, r.RotX);
            F32(s, ref at, r.RotY);
            F32(s, ref at, r.RotZ);
            F32(s, ref at, r.RotW);
        }
        if (r.Has(CameraWireAbi.PartLens) && r.LensSource == CameraWireAbi.LensConstant) F32(s, ref at, r.FovDegrees);
        if (r.Has(CameraWireAbi.PartShake))
        {
            F32(s, ref at, r.Amplitude);
            F32(s, ref at, r.Frequency);
            U32(s, ref at, r.FalloffMs);
            U32(s, ref at, r.Seed);
        }
        if (r.Has(CameraWireAbi.PartPath))
        {
            U16(s, ref at, (ushort)r.Keys.Length);
            foreach (var k in r.Keys)
            {
                U32(s, ref at, k.AtMs);
                F64(s, ref at, k.X);
                F64(s, ref at, k.Y);
                F64(s, ref at, k.Z);
                F32(s, ref at, k.Qx);
                F32(s, ref at, k.Qy);
                F32(s, ref at, k.Qz);
                F32(s, ref at, k.Qw);
                F32(s, ref at, k.Fov);
            }
        }
    }

    // Reads a blob from s[at..end] (end = the body's end): the structure first, the key count against the bytes left and
    // the limit before the array is made, then Validate.
    private static CameraWireRequest? ReadBlob(ReadOnlySpan<byte> s, ref int at, int maxKeys)
    {
        if (s.Length - at < CameraWireAbi.BlobFixedBytes) return null;
        uint parts = RU32(s, ref at);
        byte policy = RU8(s, ref at);
        uint duration = RU32(s, ref at), blendIn = RU32(s, ref at), blendOut = RU32(s, ref at);
        int priority = RI32(s, ref at);
        byte lensSource = RU8(s, ref at);
        if (parts == 0 || (parts & ~CameraWireAbi.KnownParts) != 0) return null;
        var r = new CameraWireRequest
        {
            Parts = parts, InputPolicy = policy, DurationMs = duration, BlendInMs = blendIn, BlendOutMs = blendOut, Priority = priority,
            LensSource = lensSource,
        };
        if ((parts & CameraWireAbi.PartMode) != 0)
        {
            if (s.Length - at < 2) return null;
            r = r with { CameraId = RU8(s, ref at), Transition = RU8(s, ref at) };
        }
        if ((parts & CameraWireAbi.PartProfile) != 0)
        {
            if (s.Length - at < 42) return null;
            r = r with
            {
                FollowEntity = RU32(s, ref at), RotationMode = RU8(s, ref at), Collision = RU8(s, ref at), Distance = RF32(s, ref at),
                OffsetX = RF32(s, ref at), OffsetY = RF32(s, ref at), OffsetZ = RF32(s, ref at), OffsetWorldY = RF32(s, ref at),
                RotX = RF32(s, ref at), RotY = RF32(s, ref at), RotZ = RF32(s, ref at), RotW = RF32(s, ref at),
            };
        }
        if ((parts & CameraWireAbi.PartLens) != 0 && lensSource == CameraWireAbi.LensConstant)
        {
            if (s.Length - at < 4) return null;
            r = r with { FovDegrees = RF32(s, ref at) };
        }
        if ((parts & CameraWireAbi.PartShake) != 0)
        {
            if (s.Length - at < 16) return null;
            r = r with { Amplitude = RF32(s, ref at), Frequency = RF32(s, ref at), FalloffMs = RU32(s, ref at), Seed = RU32(s, ref at) };
        }
        if ((parts & CameraWireAbi.PartPath) != 0)
        {
            if (s.Length - at < 2) return null;
            int count = RU16(s, ref at);
            // the count against the limit and against the bytes the body holds, before the array exists
            if (count < 1 || count > Math.Min(maxKeys, CameraWireAbi.MaxKeys) || (long)count * CameraWireAbi.KeyBytes > s.Length - at) return null;
            var keys = new WireKey[count];
            for (var i = 0; i < count; i++)
                keys[i] = new WireKey(RU32(s, ref at), RF64(s, ref at), RF64(s, ref at), RF64(s, ref at), RF32(s, ref at), RF32(s, ref at),
                    RF32(s, ref at), RF32(s, ref at), RF32(s, ref at));
            r = r with { Keys = keys };
        }
        return Validate(r, maxKeys) == CameraReasons.None ? r : null;
    }

    // ---- messages ----

    public static byte[] Hello(in HelloMessage m)
    {
        var b = new byte[2 + 8 + 4 + 4 + 8 + 2 + 4];
        var at = 0;
        U16(b, ref at, m.Protocol);
        U64(b, ref at, m.ClientNonce);
        U32(b, ref at, m.ClientEpoch);
        U32(b, ref at, m.LocalEntity);
        U64(b, ref at, m.Caps);
        U16(b, ref at, m.MaxKeys);
        U32(b, ref at, m.MaxBody);
        return b;
    }

    public static bool TryHello(ReadOnlySpan<byte> s, out HelloMessage m)
    {
        m = default;
        if (!EndsAt(s, 32)) return false;
        var at = 0;
        var h = new HelloMessage(RU16(s, ref at), RU64(s, ref at), RU32(s, ref at), RU32(s, ref at), RU64(s, ref at), RU16(s, ref at), RU32(s, ref at));
        if (h.Protocol != CameraWireAbi.Protocol || h.ClientNonce == 0 || h.ClientEpoch == 0 || h.LocalEntity == 0 || h.MaxKeys < 1 || h.MaxBody < 1) return false;
        m = h;
        return true;
    }

    public static byte[] Welcome(in WelcomeMessage m)
    {
        var b = new byte[8 + 4 + 8 + 4 + 4 + 8 + 8 + 8 + 4 + 2];
        var at = 0;
        U64(b, ref at, m.ClientNonce);
        U32(b, ref at, m.ClientEpoch);
        U64(b, ref at, m.ServerNonce);
        U32(b, ref at, m.ServerEpoch);
        U32(b, ref at, m.Player);
        U64(b, ref at, m.Incarnation);
        U64(b, ref at, m.Token);
        U64(b, ref at, m.AllowedCaps);
        U32(b, ref at, m.MaxBody);
        U16(b, ref at, m.MaxKeys);
        return b;
    }

    public static bool TryWelcome(ReadOnlySpan<byte> s, out WelcomeMessage m)
    {
        m = default;
        if (!EndsAt(s, 58)) return false;
        var at = 0;
        var w = new WelcomeMessage(RU64(s, ref at), RU32(s, ref at), RU64(s, ref at), RU32(s, ref at), RU32(s, ref at), RU64(s, ref at),
            RU64(s, ref at), RU64(s, ref at), RU32(s, ref at), RU16(s, ref at));
        if (!(w.Token != 0 && w.ServerNonce != 0 && w.ServerEpoch != 0 && w.Player != 0 && (w.AllowedCaps & ~CameraWireAbi.RemoteParts) == 0 &&
              w.MaxBody is >= 1 and <= CameraWireAbi.MaxBody && w.MaxKeys is >= 1 and <= CameraWireAbi.MaxKeys)) return false;
        m = w;
        return true;
    }

    /// <summary>Apply/Update/End. Null when the body would be longer than <paramref name="maxBody"/> (BodyTooLarge,
    /// decided before the buffer is made).</summary>
    public static byte[]? Mutation(CameraMessage kind, in WireHeader h, byte timing, CameraWireRequest? request, int maxBody)
    {
        if (kind is not (CameraMessage.Apply or CameraMessage.Update or CameraMessage.End)) throw new ArgumentOutOfRangeException(nameof(kind));
        if ((kind == CameraMessage.End) != (request is null)) throw new ArgumentException("End carries no request; Apply and Update carry one");
        long size = CameraWireAbi.CommonHeaderBytes + (kind == CameraMessage.Update ? 1 : 0);
        if (request is not null)
        {
            var blob = BlobSize(request);
            if (blob < 0) return null;
            size += blob;
        }
        if (size > Math.Min(maxBody, CameraWireAbi.MaxBody)) return null;
        var b = new byte[size];
        var at = 0;
        WriteHeader(b, ref at, h);
        if (kind == CameraMessage.Update) U8(b, ref at, timing);
        if (request is not null) WriteBlob(b, ref at, request);
        return b;
    }

    /// <summary>Decodes Apply/Update/End: the body's length against <paramref name="maxBody"/> first.</summary>
    public static bool TryMutation(CameraMessage kind, ReadOnlySpan<byte> s, int maxBody, int maxKeys, out MutationMessage? m)
    {
        m = null;
        var limit = Math.Min(maxBody, CameraWireAbi.MaxBody);
        if (s.Length > Padded(limit) || s.Length < CameraWireAbi.CommonHeaderBytes) return false;
        var at = 0;
        var h = ReadHeader(s, ref at);
        switch (kind)
        {
            case CameraMessage.End:
                if (!EndsAt(s, at)) return false;
                m = new MutationMessage(kind, h, 0, null);
                return true;
            case CameraMessage.Apply:
            case CameraMessage.Update:
                byte timing = 0;
                if (kind == CameraMessage.Update)
                {
                    if (s.Length - at < 1) return false;
                    timing = RU8(s, ref at);
                    if (timing > CameraWireAbi.TimingRestart) return false;
                }
                var r = ReadBlob(s, ref at, maxKeys);
                if (r is null || at > limit || !EndsAt(s, at)) return false;
                m = new MutationMessage(kind, h, timing, r);
                return true;
            default:
                return false;
        }
    }

    /// <summary>Only the common header of a mutation body (to answer a malformed request on a valid binding).</summary>
    public static bool TryHeaderOnly(ReadOnlySpan<byte> s, int maxBody, out WireHeader h)
    {
        h = default;
        if (s.Length > Padded(Math.Min(maxBody, CameraWireAbi.MaxBody)) || s.Length < CameraWireAbi.CommonHeaderBytes) return false;
        var at = 0;
        h = ReadHeader(s, ref at);
        return true;
    }

    public static byte[] Ack(in AckMessage m)
    {
        var text = Diagnostic(m.Diagnostic);
        var b = new byte[CameraWireAbi.CommonHeaderBytes + 1 + 2 + 1 + 4 + 8 + 4 + 4 + 2 + text.Length];
        var at = 0;
        WriteHeader(b, ref at, m.Header);
        U8(b, ref at, m.Ok ? (byte)1 : (byte)0);
        U16(b, ref at, m.Reason);
        U8(b, ref at, m.Scope);
        U32(b, ref at, m.Flags);
        U64(b, ref at, m.Frame);
        U32(b, ref at, m.ElapsedMs);
        U32(b, ref at, m.RemainingMs);
        U16(b, ref at, (ushort)text.Length);
        text.CopyTo(b.AsSpan(at));
        return b;
    }

    public static bool TryAck(ReadOnlySpan<byte> s, out AckMessage m)
    {
        m = default;
        const int fixedBytes = CameraWireAbi.CommonHeaderBytes + 1 + 2 + 1 + 4 + 8 + 4 + 4 + 2;
        if (s.Length < fixedBytes || s.Length > Padded(fixedBytes + CameraWireAbi.MaxDiagnosticBytes)) return false;
        var at = 0;
        var h = ReadHeader(s, ref at);
        byte ok = RU8(s, ref at);
        ushort reason = RU16(s, ref at);
        byte scope = RU8(s, ref at);
        uint flags = RU32(s, ref at);
        ulong frame = RU64(s, ref at);
        uint elapsed = RU32(s, ref at), remaining = RU32(s, ref at);
        int len = RU16(s, ref at);
        if (ok > 1 || reason > CameraWireAbi.MaxReason || scope > CameraWireAbi.CommitClientFrame ||
            (flags & ~(CameraWireAbi.FlagMayHaveApplied | CameraWireAbi.FlagAlreadyEnded)) != 0 || len > CameraWireAbi.MaxDiagnosticBytes ||
            !EndsAt(s, at + len)) return false;
        string text;
        try
        {
            text = new UTF8Encoding(false, true).GetString(s.Slice(at, len));
        }
        catch (DecoderFallbackException)
        {
            return false;
        }
        m = new AckMessage(h, ok == 1, reason, scope, flags, frame, elapsed, remaining, text);
        return true;
    }

    public static byte[] Ended(in EndedMessage m)
    {
        var b = new byte[CameraWireAbi.CommonHeaderBytes + 1 + 2 + 8];
        var at = 0;
        WriteHeader(b, ref at, m.Header);
        U8(b, ref at, m.EndReason);
        U16(b, ref at, m.Reason);
        U64(b, ref at, m.Frame);
        return b;
    }

    public static bool TryEnded(ReadOnlySpan<byte> s, out EndedMessage m)
    {
        m = default;
        if (!EndsAt(s, CameraWireAbi.CommonHeaderBytes + 11)) return false;
        var at = 0;
        var e = new EndedMessage(ReadHeader(s, ref at), RU8(s, ref at), RU16(s, ref at), RU64(s, ref at));
        if (e.EndReason is CameraEndReasons.None or > CameraWireAbi.MaxEndReason || e.Reason > CameraWireAbi.MaxReason) return false;
        m = e;
        return true;
    }

    public static byte[] Heartbeat(in HeartbeatMessage m)
    {
        var b = new byte[CameraWireAbi.BindingBytes + 32];
        var at = 0;
        WriteBinding(b, ref at, m.Binding);
        U64(b, ref at, m.Sequence);
        U64(b, ref at, m.HighestSerial);
        U64(b, ref at, m.Lease);
        U64(b, ref at, m.Revision);
        return b;
    }

    public static bool TryHeartbeat(ReadOnlySpan<byte> s, out HeartbeatMessage m)
    {
        m = default;
        if (!EndsAt(s, CameraWireAbi.BindingBytes + 32)) return false;
        var at = 0;
        var hb = new HeartbeatMessage(ReadBinding(s, ref at), RU64(s, ref at), RU64(s, ref at), RU64(s, ref at), RU64(s, ref at));
        if ((hb.Lease == 0) != (hb.Revision == 0)) return false;
        m = hb;
        return true;
    }

    public static byte[] Status(in StatusMessage m)
    {
        var b = new byte[CameraWireAbi.BindingBytes + 32];
        var at = 0;
        WriteBinding(b, ref at, m.Binding);
        U64(b, ref at, m.Sequence);
        U64(b, ref at, m.Lease);
        U64(b, ref at, m.Revision);
        U32(b, ref at, m.ElapsedMs);
        U32(b, ref at, m.RemainingMs);
        return b;
    }

    public static bool TryStatus(ReadOnlySpan<byte> s, out StatusMessage m)
    {
        m = default;
        if (!EndsAt(s, CameraWireAbi.BindingBytes + 32)) return false;
        var at = 0;
        var st = new StatusMessage(ReadBinding(s, ref at), RU64(s, ref at), RU64(s, ref at), RU64(s, ref at), RU32(s, ref at), RU32(s, ref at));
        if ((st.Lease == 0) != (st.Revision == 0)) return false;
        m = st;
        return true;
    }

    /// <summary>A diagnostic text cut to 256 UTF-8 bytes at a character boundary.</summary>
    public static byte[] Diagnostic(string? text)
    {
        var b = Encoding.UTF8.GetBytes(text ?? "");
        if (b.Length <= CameraWireAbi.MaxDiagnosticBytes) return b;
        var n = CameraWireAbi.MaxDiagnosticBytes;
        while (n > 0 && (b[n] & 0xC0) == 0x80) n--;  // never split a character
        return b[..n];
    }

    // ---- the binding and the common header ----

    private static void WriteBinding(Span<byte> s, ref int at, in WireBinding b)
    {
        U64(s, ref at, b.Token);
        U64(s, ref at, b.ServerNonce);
        U32(s, ref at, b.ServerEpoch);
        U64(s, ref at, b.ClientNonce);
        U32(s, ref at, b.ClientEpoch);
        U32(s, ref at, b.Player);
        U64(s, ref at, b.Incarnation);
    }

    private static WireBinding ReadBinding(ReadOnlySpan<byte> s, ref int at) =>
        new(RU64(s, ref at), RU64(s, ref at), RU32(s, ref at), RU64(s, ref at), RU32(s, ref at), RU32(s, ref at), RU64(s, ref at));

    private static void WriteHeader(Span<byte> s, ref int at, in WireHeader h)
    {
        WriteBinding(s, ref at, h.Binding);
        U64(s, ref at, h.OwnerToken);
        U64(s, ref at, h.Lease);
        U64(s, ref at, h.RequestId);
        U64(s, ref at, h.Serial);
        U64(s, ref at, h.Revision);
    }

    private static WireHeader ReadHeader(ReadOnlySpan<byte> s, ref int at) =>
        new(ReadBinding(s, ref at), RU64(s, ref at), RU64(s, ref at), RU64(s, ref at), RU64(s, ref at), RU64(s, ref at));

    // ---- primitives (little-endian) ----

    private static void U8(Span<byte> s, ref int at, byte v) => s[at++] = v;
    private static void U16(Span<byte> s, ref int at, ushort v) { BinaryPrimitives.WriteUInt16LittleEndian(s[at..], v); at += 2; }
    private static void U32(Span<byte> s, ref int at, uint v) { BinaryPrimitives.WriteUInt32LittleEndian(s[at..], v); at += 4; }
    private static void I32(Span<byte> s, ref int at, int v) { BinaryPrimitives.WriteInt32LittleEndian(s[at..], v); at += 4; }
    private static void U64(Span<byte> s, ref int at, ulong v) { BinaryPrimitives.WriteUInt64LittleEndian(s[at..], v); at += 8; }
    private static void F32(Span<byte> s, ref int at, float v) { BinaryPrimitives.WriteSingleLittleEndian(s[at..], v); at += 4; }
    private static void F64(Span<byte> s, ref int at, double v) { BinaryPrimitives.WriteDoubleLittleEndian(s[at..], v); at += 8; }
    private static byte RU8(ReadOnlySpan<byte> s, ref int at) => s[at++];
    private static ushort RU16(ReadOnlySpan<byte> s, ref int at) { var v = BinaryPrimitives.ReadUInt16LittleEndian(s[at..]); at += 2; return v; }
    private static uint RU32(ReadOnlySpan<byte> s, ref int at) { var v = BinaryPrimitives.ReadUInt32LittleEndian(s[at..]); at += 4; return v; }
    private static int RI32(ReadOnlySpan<byte> s, ref int at) { var v = BinaryPrimitives.ReadInt32LittleEndian(s[at..]); at += 4; return v; }
    private static ulong RU64(ReadOnlySpan<byte> s, ref int at) { var v = BinaryPrimitives.ReadUInt64LittleEndian(s[at..]); at += 8; return v; }
    private static float RF32(ReadOnlySpan<byte> s, ref int at) { var v = BinaryPrimitives.ReadSingleLittleEndian(s[at..]); at += 4; return v; }
    private static double RF64(ReadOnlySpan<byte> s, ref int at) { var v = BinaryPrimitives.ReadDoubleLittleEndian(s[at..]); at += 8; return v; }
}
