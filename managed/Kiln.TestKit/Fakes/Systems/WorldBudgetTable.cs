// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln.TestKit;

internal static unsafe class WorldBudgetTable
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct Table
    {
        internal ServiceHeader Header;
        internal delegate* unmanaged[Cdecl]<uint, void*, int> ProgramBudget;
        internal delegate* unmanaged[Cdecl]<NativeStr, NativeStr, byte*, uint, uint*, int> ProgramReservation;
    }
    private static Table* _table;
    internal static void Register()
    {
        if (_table == null)
        {
            _table = (Table*)NativeMemory.AllocZeroed((nuint)sizeof(Table));
            _table->Header = new ServiceHeader { Size = (uint)sizeof(Table), Version = 1 };
            _table->ProgramBudget = &Budget;
            _table->ProgramReservation = &Reservation;
        }
        KitBackend.Instance.RegisterTable("kiln.world.budget", (nint)_table);
        FakeTables.Offline("world.program_budget", "the kit does not model the game's program slot budgets");
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Budget(uint kind, void* reply) => FakeTables.Unavailable;
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Reservation(NativeStr mod, NativeStr key, byte* text, uint capacity, uint* size)
    {
        if (size != null) *size = 0;
        return FakeTables.Unavailable; // systems.define remains explicitly offline
    }
}
