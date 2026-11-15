// SPDX-License-Identifier: MIT
// kiln.world v4's game-program members of the kit's kiln.world table (World/WorldTable.cs), answered from the running
// harness's SystemsState as native/services/world/game_programs.cpp and wraps.cpp answer: the same results, the same
// refusals inside a slice, KILN_ERR_UNAVAILABLE for what the kit does not model.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln.TestKit;

internal static unsafe class SystemsTable
{
    public static void Fill(WorldAbi.Table* t)
    {
        t->GameProgramFind = &Find;
        t->GameProgramInfo = &Info;
        t->GameProgramsStatus = &Status;
        t->LabParallel = &LabParallel;
        t->GameProgramWrap = &Wrap;
        t->GameProgramUnwrap = &Unwrap;
        t->GameWrapInfo = &WrapInfo;
        t->GameWrapsRelease = &Release;
        t->GameSliceRunOriginal = &RunOriginal;
        t->WrapOriginal = &WrapOriginal;
        t->InProgram = &InProgram;
        t->GameProgramList = &List;
        t->GameSliceIterate = &Iterate;
        t->GameSliceSlot = &Slot;
        t->GameSliceMark = &Mark;
        t->GameSliceArchetype = &Archetype;
        t->GameArchetypeHas = &Has;
        t->GameSliceAttrChanged = &AttrChanged;
        t->GameSliceAttrValidate = &AttrValidate;
        t->GameWorldSession = &Session;
        t->GameProgramContract = &Contract;
    }

    // The harness that runs now, when kiln.world is not switched off.
    private static SystemsState? State => FakeTables.Active(WorldAbi.Service) is { } h ? KitFakes.State<SystemsState>(h) : null;

    private static bool WrapsOff => FakeTables.Off(SystemsFake.WrapsRow);

    // The slice a stage was handed, on this thread, inside that stage (as kiln.world's frame_of).
    private static SystemsState.Frame? FrameOf(SystemsNative.Slice* s) =>
        SystemsState.Current is { } f && s != null && s == f.Slice && s->Size >= (uint)sizeof(SystemsNative.Slice) ? f : null;

    // ---- the directory ----

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Find(uint kind, NativeStr name) =>
        kind != 0 || name.Ptr == null || State is not { } st ? -1 : st.Find(name.ToString());

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Info(uint kind, int index, SystemsNative.ProgramInfo* output)
    {
        if (output == null || output->Size < (uint)sizeof(SystemsNative.ProgramInfo)) return FakeTables.InvalidArgument;
        if (kind != 0 || State is not { } st || st.At(index) is not { } p) return FakeTables.NotFound;
        var size = output->Size;
        *output = default;
        output->Size = size;
        output->WorldKind = kind;
        output->Index = p.Index;
        output->Id = (ushort)(p.Index + 1);
        output->Name = new NativeStr { Ptr = (byte*)p.NameUtf8, Len = (nuint)Encoding.UTF8.GetByteCount(p.Name) };
        output->DescFlags = p.Lists.Flags;
        output->UpdateRva = p.Runs ? 0x1000u + 0x10u * (uint)p.Index : 0;
        output->Flags = SystemsNative.ProgramGame | (p.Runs ? 0 : SystemsNative.ProgramNoUpdate);
        output->Wraps = (uint)st.Counts(p.Index).Wraps;
        output->Slices = (ulong)p.Slices;
        output->Entities = (ulong)p.Entities;
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Status(SystemsNative.ProgramsStatus* output)
    {
        if (output == null || output->Size < (uint)sizeof(SystemsNative.ProgramsStatus)) return FakeTables.InvalidArgument;
        var size = output->Size;
        *output = default;
        output->Size = size;
        if (State is not { } st) return FakeTables.Ok;
        output->Directory[0] = 1;
        output->Programs[0] = output->GamePrograms[0] = (uint)st.Count;
        var (active, pending, _) = st.Counts();
        output->WrapsActive = (uint)active;
        output->WrapsPending = (uint)pending;
        var text = "the test kit's fake programs"u8;
        for (var i = 0; i < text.Length; i++) output->Detail[i] = text[i];
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int LabParallel(uint on, uint steps) => FakeTables.Unavailable;  // no players mask, no workers

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int List(uint kind, int index, int list, NativeStr* names, uint capacity)
    {
        if (kind != 0 || State is not { } st || st.At(index) is not { } p || list is < 0 or >= SystemsNative.Lists) return -1;
        var n = p.Names[list].Length;
        for (var i = 0; names != null && i < capacity && i < n; i++) names[i] = p.ListStrs[list * 64 + i];
        return n;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Contract(uint kind, int index, uint* contract, byte* text, uint capacity, uint* size)
    {
        if (size != null) *size = 0;
        if (text != null && capacity > 0) text[0] = 0;
        if (contract == null) return FakeTables.InvalidArgument;
        *contract = 0;
        if (kind != 0 || State is not { } st || st.At(index) is not { } p) return FakeTables.NotFound;
        *contract = p.Contract;
        var b = Encoding.UTF8.GetBytes(p.ContractText);
        if (size != null) *size = (uint)b.Length;
        if (text != null && capacity > 0)
        {
            var n = Math.Min(b.Length, (int)capacity - 1);
            b.AsSpan(0, n).CopyTo(new Span<byte>(text, n));
            text[n] = 0;
        }
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static ulong Session(uint kind) => kind == 0 && State is { } st ? st.Session : 0;

    // ---- wraps ----

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Wrap(ulong owner, uint kind, int index, int stage, int priority,
        delegate* unmanaged[Cdecl]<SystemsNative.Slice*, void*, void> fn, void* user, ulong* handle)
    {
        if (handle != null) *handle = 0;
        if (handle == null || owner == 0 || fn == null || stage is < SystemsNative.Before or > SystemsNative.After) return FakeTables.InvalidArgument;
        if (SystemsState.Current is not null || !FakeTables.OnTickThread) return FakeTables.WrongThread;
        if (State is not { } st || WrapsOff || kind != 0) return FakeTables.Unavailable;  // the kit fakes the server world only
        if (st.At(index) is not { } p) return FakeTables.NotFound;
        if (!p.Runs) return FakeTables.InvalidArgument;
        *handle = st.Wrap(owner, p, stage, priority, fn, user);
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Unwrap(ulong handle)
    {
        if (SystemsState.Current is not null) return FakeTables.WrongThread;  // never inside a slice, as kiln.world
        if (State is not { } st) return FakeTables.NotFound;
        return st.Unwrap(handle) ? FakeTables.Ok : FakeTables.NotFound;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int WrapInfo(ulong handle, SystemsNative.WrapInfo* output)
    {
        if (output == null || output->Size < (uint)sizeof(SystemsNative.WrapInfo)) return FakeTables.InvalidArgument;
        if (State is not { } st || st.StageOf(handle) is not { } s) return FakeTables.NotFound;
        var size = output->Size;
        *output = default;
        output->Size = size;
        output->State = s.State;
        output->Reason = s.Reason;
        output->Stage = s.Kind;
        output->Priority = s.Priority;
        output->ProgramIndex = s.Program;
        if (st.At(s.Program) is { } p)
            output->Program = new NativeStr { Ptr = (byte*)p.NameUtf8, Len = (nuint)Encoding.UTF8.GetByteCount(p.Name) };
        output->Calls = (ulong)s.Calls;
        output->Entities = (ulong)s.Entities;
        output->Threads = s.Calls > 0 ? 1u : 0u;
        output->RemoveQueued = (byte)(s.RemoveQueued ? 1 : 0);
        output->Gone = (byte)(s.Gone ? 1 : 0);
        var d = Encoding.UTF8.GetBytes(s.Detail);
        for (var i = 0; i < d.Length && i < 127; i++) output->Detail[i] = d[i];
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Release(ulong owner, uint* pending)
    {
        if (pending != null) *pending = 0;
        if (SystemsState.Current is not null) return FakeTables.WrongThread;
        State?.Release(owner);
        return FakeTables.Ok;  // every slice runs on the tick thread: no call of the owner's is running now
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RunOriginal(SystemsNative.Slice* s)
    {
        if (FrameOf(s) is not { } f || s->Stage != SystemsNative.Replace || f.OriginalCalled) return FakeTables.InvalidArgument;
        f.Owner.RunUpdate(f);
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static ulong WrapOriginal(nuint entry) => 0;  // no program table entries offline

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint InProgram() => SystemsState.Current is not null ? 1u : 0u;

    // ---- per entity inside a stage ----

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Iterate(SystemsNative.Slice* s, delegate* unmanaged[Cdecl]<ulong*, uint, uint, void*, uint> each, void* user)
    {
        if (FrameOf(s) is not { } f) return FakeTables.WrongThread;
        if (each == null) return FakeTables.InvalidArgument;
        if (f.Iter != null) return FakeTables.WrongThread;  // already inside an iteration of this slice
        ulong* it = stackalloc ulong[2];
        f.Iter = it;
        try
        {
            for (var row = 0; row < f.Ids.Length; row++)
            {
                it[0] = (ulong)row;
                f.Row = row;
                f.Marked.Clear();  // MarkWritten once per entity and component
                if (each(it, (uint)row, f.Ids[row], user) != 0) break;
            }
        }
        finally
        {
            f.Iter = null;
            f.Row = -1;
        }
        return FakeTables.Ok;
    }

    // The current entity's component of list `list`, name `i`, in the fake world.
    private static (EntitiesFake.Slot? Block, string Name) Component(SystemsState.Frame f, int list, int i)
    {
        var name = f.Owner.ListName(f.Program, list, i);
        if (name.Length == 0 || list > SystemsNative.ListWriteOpt || f.Row < 0) return (null, name);
        return (KitBackend.Instance.Harness?.EntitiesFake?.Block(f.Ids[f.Row], WorldAbi.Hash(name)), name);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void* Slot(SystemsNative.Slice* s, ulong* iter, int list, int i, uint* size, byte* writable)
    {
        if (size != null) *size = 0;
        if (writable != null) *writable = 0;
        if (FrameOf(s) is not { } f || iter == null || iter != f.Iter || list is < 0 or > SystemsNative.ListSysWrite) return null;
        var (block, _) = Component(f, list, i);  // singletons (sys lists): none offline
        if (block is not { } b) return null;
        if (size != null) *size = (uint)b.Size;
        if (writable != null) *writable = (byte)(list is SystemsNative.ListWrite or SystemsNative.ListWriteOpt ? 1 : 0);
        return (void*)b.Data;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Mark(SystemsNative.Slice* s, ulong* iter, int list, int i)
    {
        if (FrameOf(s) is not { } f || iter == null || iter != f.Iter) return FakeTables.WrongThread;
        if (list is not (SystemsNative.ListWrite or SystemsNative.ListWriteOpt)) return FakeTables.InvalidArgument;
        var (block, name) = Component(f, list, i);
        if (name.Length == 0 || block is null) return FakeTables.InvalidArgument;  // not a write name, or an optional one absent
        f.Owner.Mark(f, f.Ids[f.Row], WorldAbi.Hash(name), name);
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Archetype(SystemsNative.Slice* s, nuint* layout)
    {
        if (layout != null) *layout = 0;
        if (FrameOf(s) is not { } f) return FakeTables.WrongThread;
        if (layout == null) return FakeTables.InvalidArgument;
        if (f.Ids.Length == 0) return FakeTables.NotFound;
        var l = f.Owner.LayoutOf(f.Ids[0]);
        if (l == 0) return FakeTables.Unavailable;
        *layout = l;
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Has(uint kind, nuint layout, uint hash)
    {
        if (kind != 0 || State is not { } st || st.ComponentsOfLayout(layout) is not { } comps) return -1;
        if (Array.IndexOf(comps, hash) >= 0) return 1;
        return KitBackend.Instance.Harness?.EntitiesFake?.KnowsComponent(hash) == true ? 0 : -1;
    }

    // As kiln.world's game_slice_attr_validate: a write slot whose component is an attribute, and its block's header.
    private static int Validate(SystemsState.Frame f, ulong* iter, int list, int i)
    {
        if (iter == null || iter != f.Iter) return FakeTables.WrongThread;
        if (list is not (SystemsNative.ListWrite or SystemsNative.ListWriteOpt)) return FakeTables.InvalidArgument;
        var (block, name) = Component(f, list, i);
        var world = KitBackend.Instance.Harness?.EntitiesFake;
        if (block is not { } b || world is null || !world.IsAttributeComponent(WorldAbi.Hash(name)) || b.Size < 8) return FakeTables.InvalidArgument;
        var header = new ReadOnlySpan<byte>((void*)b.Data, 8);
        if ((header[6] & 1) == 0) return FakeTables.Ok;  // not initialized: HasValues is readable, formulas refuse it
        var offset = BitConverter.ToUInt16(header[4..]);
        return offset < 8 || offset % 4 != 0 || offset > b.Size || header[7] == 0 || header[7] > (b.Size - offset) / 4
            ? FakeTables.InvalidArgument : FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int AttrValidate(SystemsNative.Slice* s, ulong* iter, int list, int i) =>
        FrameOf(s) is { } f ? Validate(f, iter, list, i) : FakeTables.WrongThread;

    // The game's attribute formulas are not modelled: a valid call changes nothing.
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int AttrChanged(SystemsNative.Slice* s, ulong* iter, int list, int i)
    {
        if (FrameOf(s) is not { } f) return FakeTables.WrongThread;
        var valid = Validate(f, iter, list, i);
        if (valid != FakeTables.Ok) return valid;
        var registry = f.Program.Names[SystemsNative.ListSysRead].Concat(f.Program.Names[SystemsNative.ListSysWrite]).Contains("impact::AttributeRegistry");
        var (block, _) = Component(f, list, i);
        if (!registry || block is not { } b || (((byte*)b.Data)[6] & 1) == 0) return FakeTables.InvalidArgument;
        return FakeTables.Ok;
    }
}
