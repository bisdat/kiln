// SPDX-License-Identifier: MIT
// Mirror of native/core/abi/services/kiln_camera.h (kiln.camera v1). The sizes are pinned by Kiln.Camera.Tests to the
// numbers of native/services/camera/abi_asserts.h.
using System.Runtime.InteropServices;

namespace Kiln;

internal static unsafe class CameraNative
{
    public const string Service = "kiln.camera";
    public const uint Version = 1;
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, WrongThread = 4, BufferTooSmall = 5, Revoked = 6;

    // KilnCameraRequest.parts
    public const uint PartMode = 1, PartPath = 2, PartProfile = 4, PartShake = 8, PartLens = 16;
    // KilnCameraRequest.lens_source
    public const uint LensNone = 0, LensConstant = 1, LensKeys = 2;
    // KilnCameraResult.flags
    public const uint MayHaveApplied = 1, AlreadyEnded = 2;
    public const int MaxKeys = 256, MaxDurationMs = 120000;

    [StructLayout(LayoutKind.Sequential)]
    public struct Pose
    {
        public double X, Y, Z;
        public float Qx, Qy, Qz, Qw;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Key
    {
        public uint AtMs, Reserved;
        public Pose Pose;
        public float FovDegrees;
        public uint Reserved2;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Profile
    {
        public uint Size, Follow, RotationMode, Collision;
        public float Distance, OffsetX, OffsetY, OffsetZ, OffsetWorldY, Qx, Qy, Qz, Qw;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Shake
    {
        public float Amplitude, Frequency;
        public uint FalloffMs, Seed;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Request
    {
        public uint Size, Parts, Endpoint, Player;
        public uint InputPolicy, DurationMs, BlendInMs, BlendOutMs;
        public int Priority;
        public uint LensSource;
        public float FovDegrees;
        public byte CameraId, Transition, Reserved0, Reserved1;
        public Profile Profile;
        public Shake Shake;
        public uint KeyCount;
        public Key* Keys;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Result
    {
        public uint Size, Ok, ReasonCode, CommitScope, Flags, Endpoint, Player, Generation;
        public ulong Lease, Revision, Frame;
        public NativeStr Reason;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Change
    {
        public uint Size, Kind, Endpoint, Player, Generation, EndReason, ReasonCode, Reserved;
        public ulong Lease, Revision, Frame;
        public NativeStr Reason;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Visual
    {
        public uint Size, Valid, Generation, Reserved;
        public ulong Frame, TargetSampleStep, CapturedNs;
        public Pose Pose;
        public float FovRadians, Aspect, Near, Far;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<uint, uint, ulong*, int> Capabilities;
        public delegate* unmanaged[Cdecl]<ulong, Request*, ulong*, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, int> Apply;
        public delegate* unmanaged[Cdecl]<ulong, ulong, uint, Request*, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, int> Update;
        public delegate* unmanaged[Cdecl]<ulong, ulong, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, int> End;
        public delegate* unmanaged[Cdecl]<ulong, delegate* unmanaged[Cdecl]<Change*, void*, void>, void*, ulong*, int> Subscribe;
        public delegate* unmanaged[Cdecl]<ulong, int> Unsubscribe;
        public delegate* unmanaged[Cdecl]<Visual*, uint*, int> GetVisual;
        public delegate* unmanaged[Cdecl]<ulong, void*, int> BindTransport;
        public delegate* unmanaged[Cdecl]<ulong, int> UnbindTransport;
        public delegate* unmanaged[Cdecl]<ulong, ulong, ulong, uint, uint, NativeStr, int> TransportEnded;
    }

    /// <summary>KilnCameraTransport: the bridge Kiln.Camera binds (CameraBridgeStartup). 32 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Transport
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, uint, ulong, ulong, Request*, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, int> Submit;
        public delegate* unmanaged[Cdecl]<uint, ulong*, int> Caps;
        public delegate* unmanaged[Cdecl]<ulong, void> Retire;
    }

    public const uint EndpointServerMode = 1, EndpointLocal = 2, EndpointRemote = 3;

    private static Table* _table;

    /// <summary>The service's table, or null. Looked up again while it is missing.</summary>
    public static Table* T => _table != null ? _table : (_table = Services.Get<Table>(Service, Version));
}
