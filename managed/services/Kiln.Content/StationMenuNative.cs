// SPDX-License-Identifier: MIT
// Mirror of native/core/abi/services/kiln_station_menus.h (kiln.stationmenus v1, since 0.2.0-dev.31). Blittable only;
// field order is the ABI (the native side static_asserts the same sizes and offsets).
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln;

internal static unsafe class StationMenuNative
{
    public const string Service = "kiln.stationmenus";
    public const uint Version = 1;

    public const int Ok = ContentNative.Ok, InvalidArgument = ContentNative.InvalidArgument, NotFound = ContentNative.NotFound,
        Unavailable = ContentNative.Unavailable, BufferTooSmall = ContentNative.BufferTooSmall, Revoked = ContentNative.Revoked;

    public static string Message(int rc) => ContentNative.Message(rc);

    [StructLayout(LayoutKind.Sequential)]
    public struct Info  // 216 bytes
    {
        public uint Size;
        public uint Store;
        public uint Render;
        public uint RenderReason;
        public ulong Generation;
        public ulong RemoteGeneration;
        public uint Layers;
        public uint Effective;
        public uint Remote;
        public uint Pending;
        public uint Materialized;
        public uint Reserved;
        public fixed byte Detail[160];

        public readonly string DetailText
        {
            get { fixed (byte* p = Detail) return ContentNative.Text(p, 160); }
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<Info*, int> GetInfo;
        public delegate* unmanaged[Cdecl]<ulong, NativeStr, ulong*, ulong*, byte*, nuint, nuint*, int> Define;
        public delegate* unmanaged[Cdecl]<ulong, ulong, ulong*, int> Remove;
        public delegate* unmanaged[Cdecl]<byte*, nuint, nuint*, int> ExportLayouts;
        public delegate* unmanaged[Cdecl]<ulong, NativeStr, byte*, nuint, nuint*, int> RemoteReplace;
        public delegate* unmanaged[Cdecl]<ulong, int> RemoteClear;
        public delegate* unmanaged[Cdecl]<uint, int> SetPending;
        public delegate* unmanaged[Cdecl]<byte*, nuint, nuint*, int> DrainApplied;
    }

    private static Table* _table;
    private static bool _looked;

    internal static Table* T
    {
        get
        {
            if (_table == null && !_looked)
            {
                try
                {
                    var t = (Table*)Backend.Current.Service(Service, Version);
                    _table = t != null && t->Header.Size >= (uint)sizeof(Table) ? t : null;
                }
                catch (Exception)
                {
                    _table = null;
                }
                _looked = _table != null;  // keep looking until it is there (the content DLL may load later)
            }
            return _table;
        }
    }

    internal static readonly IStationMenuBackend Instance = new NativeBackend();

    private sealed class NativeBackend : IStationMenuBackend
    {
        public bool Available => T != null;

        public (int Rc, ulong Handle, ulong Generation, string State, string Error) Define(ulong owner, string record)
        {
            var t = T;
            if (t == null) return (Unavailable, 0, 0, "", "kiln.stationmenus is not loaded");
            const int cap = 4096;
            var buf = stackalloc byte[cap];
            nuint len = 0;
            ulong handle = 0, generation = 0;
            int rc;
            using (var s = new ContentNative.Utf8(record)) rc = t->Define(owner, s.Str, &handle, &generation, buf, cap, &len);
            var report = Encoding.UTF8.GetString(buf, (int)Math.Min(len, (nuint)(cap - 1)));
            var (state, error) = ReadReport(report);
            return (rc, handle, generation, state, error);
        }

        public (int Rc, ulong Generation) Remove(ulong owner, ulong handle)
        {
            var t = T;
            if (t == null) return (Unavailable, 0);
            ulong generation = 0;
            var rc = t->Remove(owner, handle, &generation);
            return (rc, generation);
        }

        public string? Export()
        {
            var t = T;
            if (t == null) return null;
            var text = NativeText.Read((b, cap, len) => t->ExportLayouts(b, cap, len), out var rc);
            return rc == Ok ? text : null;
        }

        public (int Rc, string Error) RemoteReplace(ulong owner, string snapshot)
        {
            var t = T;
            if (t == null) return (Unavailable, "kiln.stationmenus is not loaded");
            const int cap = 4096;
            var buf = stackalloc byte[cap];
            nuint len = 0;
            int rc;
            using (var s = new ContentNative.Utf8(snapshot)) rc = t->RemoteReplace(owner, s.Str, buf, cap, &len);
            var (_, error) = ReadReport(Encoding.UTF8.GetString(buf, (int)Math.Min(len, (nuint)(cap - 1))));
            return (rc, error);
        }

        public int RemoteClear(ulong owner)
        {
            var t = T;
            return t != null ? t->RemoteClear(owner) : Unavailable;
        }

        public int SetPending(bool on)
        {
            var t = T;
            return t != null ? t->SetPending(on ? 1u : 0u) : Unavailable;
        }

        public string? DrainApplied()
        {
            var t = T;
            if (t == null) return null;
            var text = NativeText.Read((b, cap, len) => t->DrainApplied(b, cap, len), out var rc);
            return rc == Ok ? text : null;
        }

        public Info? Info()
        {
            var t = T;
            if (t == null) return null;
            var info = new Info { Size = (uint)sizeof(Info) };
            return t->GetInfo(&info) == Ok ? info : null;
        }
    }

    /// <summary>The report JSON of define / remote_replace: its state and error ("" when absent or unreadable).</summary>
    internal static (string State, string Error) ReadReport(string report)
    {
        if (report.Length == 0) return ("", "");
        try
        {
            using var doc = System.Text.Json.JsonDocument.Parse(report);
            var root = doc.RootElement;
            var state = root.TryGetProperty("state", out var s) && s.ValueKind == System.Text.Json.JsonValueKind.String ? s.GetString() ?? "" : "";
            var error = root.TryGetProperty("error", out var e) && e.ValueKind == System.Text.Json.JsonValueKind.String ? e.GetString() ?? "" : "";
            return (state, error);
        }
        catch (System.Text.Json.JsonException)
        {
            return ("", report);
        }
    }
}
