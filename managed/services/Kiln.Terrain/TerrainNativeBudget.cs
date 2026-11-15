// SPDX-License-Identifier: MIT
using System.Runtime.InteropServices;
namespace Kiln;

// Appended kiln.build v1 fields: reserve @0x88, charge @0x90, size 0x98 on Win64. Older callers retain the prefix.
internal static unsafe class TerrainNativeBudget
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Table
    {
        public uint Size, Version;
        public fixed ulong Prefix[16];
        public delegate* unmanaged[Cdecl]<uint, uint, int> Reserve;
        public delegate* unmanaged[Cdecl]<double, void> Charge;
    }
    private static Table* Get() => Services.Get<Table>("kiln.build", 1);
    internal static bool Available { get { var t = Get(); return t != null && t->Reserve != null && t->Charge != null; } }
    internal static bool Reserve(long read, long write)
    {
        var t = Get();
        return t != null && t->Reserve != null && t->Reserve(checked((uint)read), checked((uint)write)) == 0;
    }
    internal static void Charge(double milliseconds)
    {
        var t = Get();
        if (t != null && t->Charge != null) t->Charge(milliseconds);
    }
}
