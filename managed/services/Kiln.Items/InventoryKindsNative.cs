// SPDX-License-Identifier: MIT
// The blittable mirror of kiln.items.inventory_kinds v1 (native/core/abi/services/kiln_items_inventory_kinds.h, served by
// kiln-items.dll; 0.2.0-dev.31, GitHub issue #138): x64 16 bytes, the header at 0, find at 8; rows of 8 bytes
// {u32 entity, u32 kind}.
using System.Runtime.InteropServices;

namespace Kiln;

internal static unsafe class InventoryKindsNative
{
    public const string Service = "kiln.items.inventory_kinds";
    public const uint Version = 1;

    [StructLayout(LayoutKind.Sequential)]
    public struct Row
    {
        public uint Entity, Kind;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<uint, Row*, uint, uint*, int> Find;
    }

    private static Table* _table;
    private static Table* _forTests;
    private static bool _testSeam;
    private static readonly object Lock = new();

    /// <summary>The table, or null when kiln-items.dll does not serve it (an older one, the test kit without its fake).
    /// Looked up until found; a table smaller than v1's is no table.</summary>
    public static Table* T
    {
        get
        {
            if (_testSeam) return _forTests;
            if (_table != null) return _table;
            lock (Lock)
            {
                if (_table != null) return _table;
                var t = Services.Get<Table>(Service, Version);
                return _table = t != null && t->Header.Size >= (uint)sizeof(Table) && t->Find != null ? t : null;
            }
        }
    }

    /// <summary>Tests: answer <paramref name="table"/> instead of the backend's (null: none).</summary>
    internal static void UseForTests(Table* table)
    {
        lock (Lock)
        {
            _forTests = table;
            _testSeam = true;
            _table = null;
        }
    }

    internal static void ResetForTests()
    {
        lock (Lock)
        {
            _forTests = null;
            _testSeam = false;
            _table = null;
        }
    }
}
