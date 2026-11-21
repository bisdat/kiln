// SPDX-License-Identifier: MIT
// kiln.render (native/core/abi/services/kiln_render.h): drawing on the overlay's background list and the game's camera.
using System.Runtime.InteropServices;

namespace Kiln;

internal static unsafe class RenderNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Camera
    {
        public uint Size;
        public byte Valid, Available, R0, R1;
        public double X, Y, Z;
        public float Qx, Qy, Qz, Qw;
        public float FovY, Aspect, Near, Far, Distance, R2;
        public ulong Frame;
        public uint AgeMs, R3;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Status
    {
        public uint Size;
        public byte Camera, CameraValid, R0, R1;
        public uint FrameHolders;
        public ulong CameraFrames, Draws;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<Status*, int> GetStatus;
        public delegate* unmanaged[Cdecl]<ulong, ulong*, int> AcquireFrames;
        public delegate* unmanaged[Cdecl]<ulong, int> Release;
        public delegate* unmanaged[Cdecl]<Camera*, int> GetCamera;
        public delegate* unmanaged[Cdecl]<double*, float, float, float*, byte> WorldToScreen;
        public delegate* unmanaged[Cdecl]<float, float, float, float, uint, float, void> Line;
        public delegate* unmanaged[Cdecl]<float, float, float, float, uint, float, float, int, void> Rect;
        public delegate* unmanaged[Cdecl]<float, float, float, uint, int, float, int, void> Circle;
        public delegate* unmanaged[Cdecl]<float, float, float, float, float, float, uint, float, int, void> Triangle;
        public delegate* unmanaged[Cdecl]<float*, uint, uint, float, int, int, void> Polyline;
        public delegate* unmanaged[Cdecl]<float, float, uint, float, uint, NativeStr, void> Text;
        public delegate* unmanaged[Cdecl]<NativeStr, float, float*, float*, void> TextSize;
        public delegate* unmanaged[Cdecl]<ulong, float, float, float, float, float, float, float, float, uint, void> Image;
    }

    // ---- outlines (v2, 0.2.0-dev.30.3) ----
    public const uint OutlinePulse = 1;
    public const uint OutlineWaiting = 0, OutlineShown = 1, OutlineGame = 2, OutlineEnded = 3;

    /// <summary>KilnRenderOutline (24 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Outline
    {
        public uint Size, Rgba;
        public int Priority;
        public uint Flags, DurationMs, Reserved;
    }

    /// <summary>KilnRenderOutlineInfo (24 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct OutlineInfo
    {
        public uint Size, State, Reason, Entity, Group, Merged;
    }

    /// <summary>KilnRenderOutlineStatus (208 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct OutlineStatus
    {
        public uint Size;
        public byte Available, Installed, Staged, Setting;
        public uint Requests, Entities, Shown, Game, NoModel;
        public ulong Sets, Steps;
        public fixed byte Detail[160];
    }

    /// <summary>v2: v1's table and the five outline functions (an older kiln-core has v1's 112 bytes: <see cref="T2"/> is
    /// null then).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct TableV2
    {
        public Table Base;
        public delegate* unmanaged[Cdecl]<ulong, uint, Outline*, ulong*, int> AddOutline;
        public delegate* unmanaged[Cdecl]<ulong, OutlineInfo*, int> GetOutlineInfo;
        public delegate* unmanaged[Cdecl]<ulong, int> RemoveOutline;
        public delegate* unmanaged[Cdecl]<ulong, int> ClearOutlines;
        public delegate* unmanaged[Cdecl]<OutlineStatus*, int> GetOutlineStatus;
    }

    // Publish the completed lookup once; another thread must never observe "tried" before the pointer. A new Lazy (tests
    // only) looks again.
    private static Lazy<nint> _tableAddress = new(Lookup);
    private static nint Lookup() => (nint)Services.Get<Table>(Service, 1);
    public const string Service = "kiln.render";
    public static Table* T => (Table*)Volatile.Read(ref _tableAddress).Value;

    public static bool Available => T != null;

    /// <summary>The v2 table (outlines), or null without kiln.render or on a kiln-core with v1 only.</summary>
    public static TableV2* T2
    {
        get
        {
            var t = T;
            return t != null && t->Header.Size >= (uint)sizeof(TableV2) && ((TableV2*)t)->GetOutlineStatus != null ? (TableV2*)t : null;
        }
    }

    /// <summary>The v2 table is here (<see cref="T2"/> is not null).</summary>
    public static bool HasOutlines => T2 != null;

    /// <summary>outline_status, or null without the v2 table or when it fails.</summary>
    public static OutlineStatus? ReadOutlineStatus()
    {
        var t = T2;
        if (t == null) return null;
        OutlineStatus s = default;
        s.Size = (uint)sizeof(OutlineStatus);
        return t->GetOutlineStatus(&s) == 0 ? s : null;
    }

    /// <summary>The status' detail text ("" when all works).</summary>
    public static string DetailOf(in OutlineStatus s)
    {
        fixed (byte* p = s.Detail)
        {
            var n = 0;
            while (n < 160 && p[n] != 0) n++;
            return System.Text.Encoding.UTF8.GetString(p, n);
        }
    }

    internal static void ResetForTests() => Volatile.Write(ref _tableAddress, new Lazy<nint>(Lookup));

    public static Camera ReadCamera()
    {
        Camera c = default;
        c.Size = (uint)sizeof(Camera);
        if (T != null) T->GetCamera(&c);
        return c;
    }

    public static Status? ReadStatus()
    {
        if (T == null) return null;
        Status s = default;
        s.Size = (uint)sizeof(Status);
        return T->GetStatus(&s) == 0 ? s : null;
    }
}
