// SPDX-License-Identifier: MIT
// Mirror of native/core/abi/services/kiln_routines.h (kiln.routines v1), and the JSON it speaks: the snapshot of one
// villager's routine (inspect, and inside events) and the two events.
using System.Runtime.InteropServices;
using System.Text.Json;

namespace Kiln;

internal static unsafe class RoutineNative
{
    public const string Service = "kiln.routines";
    public const uint Version = 1;
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, WrongThread = 4, BufferTooSmall = 5;

    [StructLayout(LayoutKind.Sequential)]
    public struct Result
    {
        public uint Size, Ok;
        public ulong Ticket, Revision;
        public uint NpcId, ReasonCode;
        public NativeStr Reason;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, uint, NativeStr, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, ulong*, int> Assign;
        public delegate* unmanaged[Cdecl]<ulong, uint, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, ulong*, int> Remove;
        public delegate* unmanaged[Cdecl]<ulong, uint, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, ulong*, int> Resume;
        public delegate* unmanaged[Cdecl]<uint, byte*, nuint, nuint*, int> Inspect;
        public delegate* unmanaged[Cdecl]<ulong, void> RevokeOwner;
        public delegate* unmanaged[Cdecl]<ulong, delegate* unmanaged[Cdecl]<NativeStr, void*, void>, void*, ulong*, int> SubscribeEvents;
        public delegate* unmanaged[Cdecl]<ulong, int> UnsubscribeEvents;
        // since 0.2.0-dev.31 (check HasRestore)
        public delegate* unmanaged[Cdecl]<ulong, uint, NativeStr, ulong, uint, NativeStr, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, ulong*, int> Restore;
        public delegate* unmanaged[Cdecl]<ulong, int> SetRevisionFloor;
    }

    /// <summary>The size of v1's table (the members up to UnsubscribeEvents).</summary>
    public static readonly uint V1Size = (uint)(sizeof(ServiceHeader) + 7 * sizeof(nint));

    /// <summary>The table has restore and set_revision_floor (0.2.0-dev.31).</summary>
    public static bool HasRestore(Table* t) => t != null && t->Header.Size >= (uint)sizeof(Table) && t->Restore != null && t->SetRevisionFloor != null;

    public const uint RestoreSuspended = 1;

    private static Table* _table;

    /// <summary>The service's table, or null. Looked up again while it is missing; a table too small for v1's members is
    /// not this service.</summary>
    public static Table* T
    {
        get
        {
            if (_table != null) return _table;
            var t = Services.Get<Table>(Service, Version);
            return t != null && t->Header.Size >= V1Size ? _table = t : null;
        }
    }

    /// <summary>For the test kit's next harness: look the table up again.</summary>
    internal static void Forget() => _table = null;

    // ---- the JSON ----

    /// <summary>A snapshot object as the service writes it. Throws <see cref="FormatException"/> when it is another shape.</summary>
    public static RoutineSnapshot Snapshot(JsonElement e)
    {
        try
        {
            var task = e.GetProperty("task");
            var last = e.GetProperty("lastReason");
            return new RoutineSnapshot(
                e.GetProperty("npcId").GetUInt32(), e.GetProperty("owner").GetString()!, e.GetProperty("key").GetString()!,
                e.GetProperty("planVersion").GetUInt32(), e.GetProperty("revision").GetUInt64(),
                Enum.Parse<RoutineAssignmentState>(e.GetProperty("assignment").GetString()!),
                e.GetProperty("windowIndex").GetInt32(), e.GetProperty("invocation").GetUInt64(), Task(task),
                Enum.Parse<RoutineTaskState>(e.GetProperty("taskState").GetString()!),
                (VillagerState)e.GetProperty("nativeState").GetByte(), last.ValueKind == JsonValueKind.Null ? null : last.GetString());
        }
        catch (Exception x) when (x is KeyNotFoundException or InvalidOperationException or ArgumentException or OverflowException)
        {
            throw new FormatException($"kiln.routines wrote a snapshot Kiln.Blocks does not read: {x.Message}", x);
        }
    }

    private static RoutineTask? Task(JsonElement e)
    {
        if (e.ValueKind == JsonValueKind.Null) return null;
        return RoutineKinds.TryParse(e.GetString(), out var kind, out var problem) ? RoutineKinds.Task(kind) : throw new FormatException(problem);
    }

    /// <summary>One event document of the service as the event Kiln posts for it.</summary>
    public static KilnEvent Event(string json)
    {
        using var doc = JsonDocument.Parse(json);
        var e = doc.RootElement;
        try
        {
            var teardown = e.GetProperty("teardown").GetBoolean();
            var reason = e.GetProperty("reason");
            switch (e.GetProperty("type").GetString())
            {
                case "assignment":
                    var before = e.GetProperty("before");
                    return new RoutineAssignmentChangedEvent(before.ValueKind == JsonValueKind.Null ? null : Snapshot(before), Snapshot(e.GetProperty("after")),
                        reason.GetString() ?? "", teardown);
                case "task":
                    return new RoutineTaskChangedEvent(e.GetProperty("npcId").GetUInt32(), e.GetProperty("key").GetString()!, e.GetProperty("planVersion").GetUInt32(),
                        e.GetProperty("revision").GetUInt64(), e.GetProperty("invocation").GetUInt64(), Task(e.GetProperty("task")),
                        Enum.Parse<RoutineTaskState>(e.GetProperty("from").GetString()!), Enum.Parse<RoutineTaskState>(e.GetProperty("to").GetString()!),
                        reason.ValueKind == JsonValueKind.Null ? null : reason.GetString(), teardown);
                case var other:
                    throw new FormatException($"kiln.routines sent an event of type \"{other}\"");
            }
        }
        catch (Exception x) when (x is KeyNotFoundException or InvalidOperationException or ArgumentException or OverflowException)
        {
            throw new FormatException($"kiln.routines sent an event Kiln.Blocks does not read: {x.Message}", x);
        }
    }
}
