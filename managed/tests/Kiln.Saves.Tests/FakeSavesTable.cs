// SPDX-License-Identifier: MIT
// A stand-in for kiln.saves' native table (kiln_saves.h), with the entry appended in 0.2.0-dev.30: stage_progress, the
// barrier that holds the game's world write until Kiln.Saves' writer has the generation's pending manifest on disk.
// It records what the writer reports; WaitLikeTheWorldWrite does what kiln.saves does on the game's Save2Thread before
// it writes the world file (saves.cpp hk_write, stage_barrier.h).
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

internal sealed unsafe class FakeSavesTable : IDisposable
{
    public const uint StageBarrierFeature = 1u << 6;  // KILN_SAVES_CAN_STAGE_BARRIER

    [StructLayout(LayoutKind.Sequential)]
    private struct Table
    {
        public uint Size, Version;
        public delegate* unmanaged[Cdecl]<SavesNative.Status*, int> GetStatus;
        public delegate* unmanaged[Cdecl]<uint, int> Request;
        public delegate* unmanaged[Cdecl]<ulong, SavesNative.Event*, int> NextEvent;
        public delegate* unmanaged[Cdecl]<ulong, delegate* unmanaged[Cdecl]<uint, void*, void>, void*, ulong*, int> SetPump;
        public delegate* unmanaged[Cdecl]<int, byte*, nuint, nuint*, int> Path;
        public delegate* unmanaged[Cdecl]<uint, int> SetPumpInterval;
        public delegate* unmanaged[Cdecl]<ulong, ulong, int> StageProgress;  // appended (0.2.0-dev.30)
    }

    private static readonly FieldInfo TableField = typeof(SavesNative).GetField("_table", BindingFlags.Static | BindingFlags.NonPublic)!;
    private static readonly FieldInfo LookedField = typeof(SavesNative).GetField("_looked", BindingFlags.Static | BindingFlags.NonPublic)!;
    private static long _issued, _done, _progressCalls;
    private static uint _features;

    private readonly Table* _table;
    private readonly object? _oldTable;
    private readonly object? _oldLooked;

    /// <summary>The newest staging the pump handed its writer, and the newest the writer has on disk.</summary>
    public static ulong Issued => (ulong)Interlocked.Read(ref _issued);
    public static ulong Done => (ulong)Interlocked.Read(ref _done);
    public static long ProgressCalls => Interlocked.Read(ref _progressCalls);

    /// <param name="barrier">The build has the stage barrier (the world write hook is on): KILN_SAVES_CAN_STAGE_BARRIER.</param>
    /// <param name="appended">The table has the stage_progress entry (a kiln.saves of 0.2.0-dev.30 or later).</param>
    public FakeSavesTable(bool barrier = true, bool appended = true)
    {
        _oldTable = TableField.GetValue(null);
        _oldLooked = LookedField.GetValue(null);
        Interlocked.Exchange(ref _issued, 0);
        Interlocked.Exchange(ref _done, 0);
        Interlocked.Exchange(ref _progressCalls, 0);
        _features = SavesNative.CanRequest | SavesNative.CanWrite | SavesNative.CanLoad | SavesNative.CanSnapshotStage |
                    (barrier ? StageBarrierFeature : 0u);
        _table = (Table*)NativeMemory.AllocZeroed((nuint)sizeof(Table));
        _table->Size = appended ? (uint)sizeof(Table) : (uint)(sizeof(Table) - sizeof(nint));
        _table->Version = 1;
        _table->GetStatus = &GetStatus;
        _table->Request = &Request;
        _table->NextEvent = &NextEvent;
        _table->SetPump = &SetPump;
        _table->Path = &PathOf;
        _table->SetPumpInterval = &SetPumpInterval;
        _table->StageProgress = appended ? &StageProgress : null;
        TableField.SetValue(null, Pointer.Box(_table, TableField.FieldType));
        LookedField.SetValue(null, true);
    }

    public void Dispose()
    {
        TableField.SetValue(null, _oldTable);
        LookedField.SetValue(null, _oldLooked);
        NativeMemory.Free(_table);
    }

    /// <summary>What kiln.saves does on the Save2Thread before the game writes the world file: wait until the writer has every
    /// staging handed to it so far on disk. Returns how long that took.</summary>
    public static TimeSpan WaitLikeTheWorldWrite(TimeSpan limit)
    {
        var want = Issued;
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while (Done < want && sw.Elapsed < limit) Thread.Sleep(5);
        if (Done < want) throw new TimeoutException($"the writer has staging {Done} of {want} on disk after {sw.Elapsed.TotalMilliseconds:F0} ms");
        return sw.Elapsed;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int GetStatus(SavesNative.Status* s)
    {
        if (s->Size < (uint)sizeof(SavesNative.Status)) return SavesNative.InvalidArgument;
        *s = new SavesNative.Status { Size = (uint)sizeof(SavesNative.Status), Features = _features, State = 2 };
        return SavesNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Request(uint reason) => SavesNative.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NextEvent(ulong after, SavesNative.Event* ev) => SavesNative.NotFound;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SetPump(ulong owner, delegate* unmanaged[Cdecl]<uint, void*, void> fn, void* user, ulong* last)
    {
        if (last != null) *last = 0;
        return SavesNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int PathOf(int kind, byte* buffer, nuint capacity, nuint* length) => SavesNative.NotFound;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SetPumpInterval(uint steps) => SavesNative.Ok;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int StageProgress(ulong issued, ulong done)
    {
        Interlocked.Increment(ref _progressCalls);
        if (issued != 0) Max(ref _issued, (long)issued);
        if (done != 0) Max(ref _done, (long)done);
        return SavesNative.Ok;
    }

    private static void Max(ref long field, long value)
    {
        long seen;
        while ((seen = Interlocked.Read(ref field)) < value && Interlocked.CompareExchange(ref field, value, seen) != seen)
        {
        }
    }
}
