// SPDX-License-Identifier: MIT
// A fake kiln.world v4 table: two programs, wraps kept in a list (applied at once), stages called by the test with a
// fake slice, as kiln.world's trampoline calls them. Unmanaged entry points over static state: one test at a time.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln.SystemsTests;

internal sealed unsafe class FakeWorld : IDisposable
{
    internal sealed class FakeWrap
    {
        public ulong Owner, Handle;
        public uint Kind;
        public int Index, Stage, Priority;
        public delegate* unmanaged[Cdecl]<SystemsNative.Slice*, void*, void> Fn;
        public void* User;
        public uint State = SystemsNative.Active, Reason;
        public bool Gone;
        public string Detail = "";
    }

    internal static FakeWorld? Current;
    internal readonly List<FakeWrap> Wraps = new();
    internal readonly string[] Names = ["fuel_setup", "client_program"];
    internal int OriginalRuns, Releases;
    internal uint ReleasePending;  // what game_wraps_release answers
    internal uint[] EntityIds = [101, 102, 103];
    internal nuint Layout = 0x5000;
    internal readonly HashSet<uint> LayoutHas = new();
    internal bool InRunOriginal;
    internal ulong Session = 1;
    internal bool NewDescriptor;
    // per-entity storage of fuel_setup's slots, row r: a Fuel attribute block {root, offset, flags, count, s32 current,
    // s32 max} (16 bytes) and a CurrentTransform (8 bytes; row 2 has none: an optional slot that is absent)
    internal readonly byte* Fuel = (byte*)NativeMemory.AllocZeroed(16 * 8);
    internal readonly byte* Transform = (byte*)NativeMemory.AllocZeroed(8 * 8);
    internal readonly byte* Registry = (byte*)NativeMemory.AllocZeroed(8);
    internal readonly List<(ulong Row, int List, int Index)> Marks = new();
    internal int AttrCalls;
    internal bool FuelIsAttribute = true;
    internal bool RefuseUnwrapInSlice;
    internal int MarkResult = SystemsNative.Ok;
    internal uint FuelSize = 16;
    internal bool NoRegistry;  // fuel_setup does not declare impact::AttributeRegistry
    internal ulong StepNow = 77;  // the step a fake slice says
    internal int IterateResult = SystemsNative.Ok;  // what game_slice_iterate answers (Unavailable: a build that cannot iterate)
    private readonly SystemsNative.Table* _table;
    private readonly nint[] _names;

    private sealed class Backend(nint table, Func<string, bool> feature) : IKilnBackend
    {
        public string Version => "0.0.0";
        public bool IsTickThread => Thread.CurrentThread.ManagedThreadId == TickThread;
        public bool HasFeature(string name) => feature(name);
        public void Log(ulong owner, LogLevel level, string text) => Logs.Add($"[{level}] {text}");
        public nint Service(string name, uint minVersion) => name == SystemsNative.Name ? table : 0;
        public ulong OwnerCreate(string id) => 999;
    }

    internal static int TickThread;
    internal static readonly List<string> Logs = new();
    private readonly IKilnBackend _old;

    internal FakeWorld(Func<string, bool>? feature = null)
    {
        Current = this;
        TickThread = Thread.CurrentThread.ManagedThreadId;
        _table = (SystemsNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(SystemsNative.Table));
        _table->Header = new ServiceHeader { Size = (uint)sizeof(SystemsNative.Table), Version = 4 };
        _table->GameProgramFind = &Find;
        _table->GameProgramInfo = &Info;
        _table->GameProgramList = &List;
        _table->GameProgramWrap = &Wrap;
        _table->GameProgramUnwrap = &Unwrap;
        _table->GameWrapInfo = &WrapInfo;
        _table->GameWrapsRelease = &Release;
        _table->GameSliceRunOriginal = &RunOriginal;
        _table->GameSliceIterate = &Iterate;
        _table->GameSliceArchetype = &Archetype;
        _table->GameArchetypeHas = &Has;
        _table->GameSliceSlot = &Slot;
        _table->GameSliceMark = &Mark;
        _table->GameSliceAttrChanged = &AttrChanged;
        _table->GameSliceAttrValidate = &AttrValidate;
        _table->GameWorldSession = &WorldSession;
        for (var r = 0; r < 8; r++)
        {
            var f = Fuel + 16 * r;
            *(uint*)f = 0x05b0bf75;
            *(ushort*)(f + 4) = 8;
            f[6] = 1;
            f[7] = 2;
            *(int*)(f + 8) = 100 * (r + 1);
            *(int*)(f + 12) = 1000;
            *(long*)(Transform + 8 * r) = 7000 + r;
        }
        _names = new nint[Names.Length];
        for (var k = 0; k < Names.Length; k++)
        {
            var b = System.Text.Encoding.UTF8.GetBytes(Names[k]);
            var p = (byte*)NativeMemory.Alloc((nuint)b.Length);
            b.CopyTo(new Span<byte>(p, b.Length));
            _names[k] = (nint)p;
        }
        _old = Kiln.Backend.Current;
        Kiln.Backend.Current = new Backend((nint)_table, feature ?? (n => n is "systems.wraps"));
        Capabilities.Reset();
        SystemsNative.Reset();
        Logs.Clear();
    }

    public void Dispose()
    {
        foreach (var s in Systems.Live.Keys.ToList()) s.Forget(true);
        ProgramLists.Reset();
        WrapWrites.Reset();
        Kiln.Backend.Current = _old;
        Capabilities.Reset();
        SystemsNative.Reset();
        Current = null;
    }

    // Calls stage `w` as kiln.world's trampoline would, with a fake slice of EntityIds; returns the slice.
    internal SystemsNative.Slice Call(FakeWrap w, uint worldThread = 1)
    {
        fixed (uint* ids = EntityIds)
        {
            var name = System.Text.Encoding.UTF8.GetBytes(Names[w.Index]);
            fixed (byte* n = name)
            {
                var s = new SystemsNative.Slice
                {
                    Size = (uint)sizeof(SystemsNative.Slice), WorldKind = w.Kind, Count = (uint)EntityIds.Length,
                    ThreadIsWorld = worldThread, EntityIds = ids, EntityStride = 4, ProgramIndex = w.Index, Step = StepNow,
                    Program = new NativeStr { Ptr = n, Len = (nuint)name.Length }, Stage = w.Stage,
                };
                if (!w.Gone) w.Fn(&s, w.User);
                return s;
            }
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static ulong WorldSession(uint kind) => F.Session;

    internal void SetTableSize(uint size) => _table->Header.Size = size;

    private static FakeWorld F => Current!;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Find(uint kind, NativeStr name)
    {
        if (kind == 1) return -1;
        var s = name.ToString();
        return Array.IndexOf(F.Names, s);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Info(uint kind, int index, SystemsNative.ProgramInfo* out_)
    {
        if (index < 0 || index >= F.Names.Length || kind == 1) return SystemsNative.NotFound;
        out_->WorldKind = kind;
        out_->Index = index;
        out_->Name = new NativeStr { Ptr = (byte*)F._names[index], Len = (nuint)F.Names[index].Length };
        out_->DescFlags = index == 1 ? 1u : 0u;
        out_->Flags = SystemsNative.ProgramGame | (index == 1 && kind == 0 ? SystemsNative.ProgramNoUpdate : 0);
        out_->UpdateRva = index == 1 && kind == 0 ? 0u : 0x1000u;
        return SystemsNative.Ok;
    }

    private static NativeStr Name(string s)
    {
        var b = System.Text.Encoding.UTF8.GetBytes(s);
        var p = (byte*)NativeMemory.Alloc((nuint)b.Length);
        b.CopyTo(new Span<byte>(p, b.Length));
        return new NativeStr { Ptr = p, Len = (nuint)b.Length };
    }

    // fuel_setup's lists: write Fuel, read_opt CurrentTransform, sys_read impact::AttributeRegistry (unless NoRegistry)
    private static readonly NativeStr FuelName = Name("Fuel"), TransformName = Name("CurrentTransform"),
        RegistryName = Name("impact::AttributeRegistry");

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int List(uint kind, int index, int list, NativeStr* names, uint capacity)
    {
        if (index is not (0 or 1)) return -1;
        if (index == 1) return 0;
        NativeStr? one = list switch
        {
            SystemsNative.ListWrite => FuelName,
            SystemsNative.ListReadOpt => TransformName,
            SystemsNative.ListSysRead when !F.NoRegistry => RegistryName,
            _ => null,
        };
        if (list == SystemsNative.ListWrite && F.NewDescriptor)
        {
            if (capacity > 0) names[0] = TransformName;
            if (capacity > 1) names[1] = FuelName;
            return 2;
        }
        if (one is null) return 0;
        if (capacity > 0) names[0] = one.Value;
        return 1;
    }

    // the iterator's first word is the row (Iterate sets it)
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void* Slot(SystemsNative.Slice* s, ulong* iter, int list, int i, uint* size, byte* writable)
    {
        var row = (int)*iter;
        *writable = (byte)(list is SystemsNative.ListWrite or SystemsNative.ListWriteOpt ? 1 : 0);
        switch (list)
        {
            case SystemsNative.ListWrite when i == (F.NewDescriptor ? 1 : 0):
                *size = F.FuelSize;
                return F.Fuel + 16 * row;
            case SystemsNative.ListWrite when F.NewDescriptor && i == 0:
                *size = 8;
                return F.Transform + 8 * row;
            case SystemsNative.ListReadOpt when i == 0:
                *size = 8;
                return row == 2 ? null : F.Transform + 8 * row;
            case SystemsNative.ListSysRead when i == 0 && !F.NoRegistry:
                *size = 0;
                return F.Registry;
            default:
                return null;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Mark(SystemsNative.Slice* s, ulong* iter, int list, int i)
    {
        if (list is not (SystemsNative.ListWrite or SystemsNative.ListWriteOpt)) return SystemsNative.InvalidArgument;
        if (F.MarkResult != SystemsNative.Ok) return F.MarkResult;
        F.Marks.Add((*iter, list, i));
        return SystemsNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int AttrValidate(SystemsNative.Slice* s, ulong* iter, int list, int i)
    {
        if (list != SystemsNative.ListWrite || i != 0 || !F.FuelIsAttribute || F.FuelSize < 8)
            return SystemsNative.InvalidArgument;
        var p = F.Fuel + 16 * (int)*iter;
        if ((p[6] & 1) == 0) return SystemsNative.Ok;
        var offset = *(ushort*)(p + 4);
        return offset >= 8 && offset % 4 == 0 && offset <= F.FuelSize && p[7] > 0 &&
            p[7] <= (F.FuelSize - offset) / 4 ? SystemsNative.Ok : SystemsNative.InvalidArgument;
    }

    // the game's formulas for Fuel: clamp the current value to the maximum
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int AttrChanged(SystemsNative.Slice* s, ulong* iter, int list, int i)
    {
        if (list is not (SystemsNative.ListWrite or SystemsNative.ListWriteOpt) || F.NoRegistry) return SystemsNative.InvalidArgument;
        F.AttrCalls++;
        var f = F.Fuel + 16 * (int)*iter;
        if ((f[6] & 1) == 0) return SystemsNative.InvalidArgument;
        if (*(int*)(f + 8) > *(int*)(f + 12)) *(int*)(f + 8) = *(int*)(f + 12);
        return SystemsNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Wrap(ulong owner, uint kind, int index, int stage, int priority,
        delegate* unmanaged[Cdecl]<SystemsNative.Slice*, void*, void> fn, void* user, ulong* out_)
    {
        if (owner == 0 || fn == null) return SystemsNative.InvalidArgument;
        var w = new FakeWrap { Owner = owner, Kind = kind, Index = index, Stage = stage, Priority = priority, Fn = fn, User = user, Handle = (ulong)F.Wraps.Count + 1 };
        if (stage == SystemsNative.Replace && F.Wraps.Any(o => o.Index == index && o.Kind == kind && o.Stage == SystemsNative.Replace && o.State == SystemsNative.Active && !o.Gone))
        {
            w.State = SystemsNative.Refused;
            w.Reason = SystemsNative.ReasonReplaceTaken;
            w.Detail = "owner 7 replaces fuel_setup already (wrap 1)";
        }
        F.Wraps.Add(w);
        *out_ = w.Handle;
        return SystemsNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Unwrap(ulong h)
    {
        if (F.RefuseUnwrapInSlice && SliceGuard.Depth > 0) return SystemsNative.WrongThread;
        var w = F.Wraps.FirstOrDefault(x => x.Handle == h);
        if (w is null) return SystemsNative.NotFound;
        w.Gone = true;
        w.State = SystemsNative.Removed;
        w.Reason = SystemsNative.ReasonUnwrapped;
        return SystemsNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int WrapInfo(ulong h, SystemsNative.WrapInfo* out_)
    {
        var w = F.Wraps.FirstOrDefault(x => x.Handle == h);
        if (w is null) return SystemsNative.NotFound;
        out_->State = w.State;
        out_->Reason = w.Reason;
        out_->Stage = w.Stage;
        out_->Gone = (byte)(w.Gone ? 1 : 0);
        var d = System.Text.Encoding.UTF8.GetBytes(w.Detail);
        for (var i = 0; i < d.Length && i < 127; i++) out_->Detail[i] = d[i];
        return SystemsNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Release(ulong owner, uint* pending)
    {
        F.Releases++;
        foreach (var w in F.Wraps.Where(x => x.Owner == owner)) w.Gone = true;
        *pending = F.ReleasePending;
        if (F.ReleasePending > 0) F.ReleasePending--;
        return SystemsNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RunOriginal(SystemsNative.Slice* s)
    {
        if (s->Stage != SystemsNative.Replace || F.InRunOriginal) return SystemsNative.InvalidArgument;
        F.OriginalRuns++;
        F.InRunOriginal = true;
        return SystemsNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Iterate(SystemsNative.Slice* s, delegate* unmanaged[Cdecl]<ulong*, uint, uint, void*, uint> each, void* user)
    {
        if (F.IterateResult != SystemsNative.Ok) return F.IterateResult;
        ulong it = 0;
        for (uint r = 0; r < s->Count; r++)
        {
            it = r;
            if (each(&it, r, s->EntityIds[r], user) != 0) break;
        }
        return SystemsNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Archetype(SystemsNative.Slice* s, nuint* out_)
    {
        *out_ = F.Layout;
        return SystemsNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Has(uint kind, nuint layout, uint hash) => layout == F.Layout ? (F.LayoutHas.Contains(hash) ? 1 : 0) : -1;
}
