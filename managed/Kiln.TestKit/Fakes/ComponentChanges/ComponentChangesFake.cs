// SPDX-License-Identifier: MIT
// The kit's fake of kiln.component_changes (native/core/abi/services/kiln_component_changes.h, v1 with its appended
// members) under Kiln.World's own consumer (ComponentChanges). The table answers from the running harness's
// ComponentChangesModel, and as the native service does when it is unavailable otherwise.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using N = Kiln.ComponentChangesNative;

namespace Kiln.TestKit;

/// <summary>Plugs the fake kiln.component_changes into the kit (<see cref="IKitFake"/>).</summary>
internal sealed unsafe class ComponentChangesFake : IKitFake
{
    public string Service => N.Service;

    private static N.Table* _table;

    public void Register()
    {
        if (_table == null)
        {
            var t = (N.Table*)NativeMemory.AllocZeroed((nuint)sizeof(N.Table));  // process lifetime, never freed
            t->Header = new ServiceHeader { Size = (uint)sizeof(N.Table), Version = N.Version };
            t->Attach = &Attach;
            t->Detach = &Detach;
            t->Watch = &Watch;
            t->Resume = &Resume;
            t->Status = &Status;
            t->Remove = &Remove;
            t->ReleaseOwner = &ReleaseOwner;
            t->BeginEvents = &BeginEvents;
            t->ReadEvents = &ReadEvents;
            t->AckEvents = &AckEvents;
            t->RecordWrite = &RecordWrite;
            t->CountersFn = &Counters;
            t->Watched = &Watched;
            _table = t;  // window_begin, window_end, window_wanted, window_faulted: the core's, none here
        }
        KitBackend.Instance.RegisterTable(N.Service, (nint)_table);
        FakeTables.Features([ComponentChanges.Capability]);
    }

    // Kiln.World's consumer (ComponentChanges) keeps its table, owner, attachment and last batch in statics, and the kit's
    // backend is one object for every harness, so nothing tells it a harness ended: each harness starts and ends with
    // it reset, or a harness after one that watched stays bound to that one's service and gets no event.
    public void Start(KilnHarness harness)
    {
        ComponentChanges.Reset();
        ModelOf(harness).Start(harness);
    }

    public void BeforeStep(KilnHarness harness, ulong step) => ModelOf(harness).BeforeStep(step);

    public void End(KilnHarness harness)
    {
        ModelOf(harness).End();
        ComponentChanges.Reset();
    }

    public void AddCoverage(List<CoverageRow> rows) => rows.Add(new CoverageRow("Component watches", CoverageKind.RecordingFake,
        [N.Service, ComponentChanges.Capability],
        "Kiln.World's own consumer of ComponentChanges.Watch on the kit's kiln.component_changes, rule for rule the service's: 32 " +
        "watches, 4096 present (entity, component) pairs, 128 roots a pair (a watch that does not fit is refused with the reason, one " +
        "that grows past them is suspended whole), roots that must stay the same lives, the inventory links of " +
        "PlayerInventoryAndItems (InventorySetup and Inventory bytes of the fake world), the origin rule (the kit's tracked writes are " +
        "Kiln's with the mod's id; bytes a test sets are Unknown), the two-frame queue, a world that ends every watch. The compares run " +
        "at the start of each step (the kit has no window calls): a change made in step N is posted in step N+1's Pre, and a new watch's " +
        "baseline is read when it is made. HarnessComponentChanges.SetAttribute gives the typed values of AttributeValueChangedEvent. " +
        "Offline: the game's own writes (a test plays them with FakeWorld.SetBytes), kiln.combat's and kiln.items' writers' tokens, " +
        "program wraps' tokens, the timing of the native window calls"));

    public static ComponentChangesModel ModelOf(KilnHarness harness) => KitFakes.State<ComponentChangesModel>(harness);

    private static KilnHarness? Harness => FakeTables.Active(N.Service);
    private static ComponentChangesModel? Model => Harness is { } h ? ModelOf(h) : null;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Attach(ulong owner)
    {
        try { return Model?.Attach(owner) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Detach(ulong owner)
    {
        try { return Model?.Detach(owner) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Watch(ulong owner, N.WatchRequest* rq, N.WatchStatus* st)
    {
        try { return Model?.Create(owner, rq, st) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Resume(ulong owner, ulong id, N.WatchStatus* st)
    {
        try { return Model?.Resume(owner, id, st) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Status(ulong id, N.WatchStatus* st)
    {
        try { return Model?.Status(id, st) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Remove(ulong owner, ulong id)
    {
        try { return Model?.Remove(owner, id) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ReleaseOwner(ulong owner)
    {
        try { return Model?.ReleaseOwner(owner) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int BeginEvents(ulong owner, ulong* batch, uint* records)
    {
        try { return Model?.BeginEvents(owner, batch, records) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ReadEvents(ulong owner, ulong batch, N.Change* out_, uint capacity, uint stride, uint* required)
    {
        try { return Model?.ReadEvents(owner, batch, out_, capacity, stride, required) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int AckEvents(ulong owner, ulong batch)
    {
        try { return Model?.AckEvents(owner, batch) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RecordWrite(ulong owner, N.KnownWrite* w)
    {
        try { return Model?.RecordWrite(owner, w) ?? FakeTables.Ok; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Counters(N.Counters* c)
    {
        try
        {
            if (c == null || c->Size < (uint)sizeof(N.Counters)) return FakeTables.InvalidArgument;
            if (Model is not { } m) return FakeTables.Unavailable;
            m.Counters(c);
            return FakeTables.Ok;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint Watched(uint entity)
    {
        try { return Model?.Watched(entity) ?? 0; } catch { return 0; }
    }

    /// <summary>The kit's tracked write (World.Write, World.Update ...) changed a component of the running harness.</summary>
    internal static void NoteWrite(KilnHarness harness, uint entity, string component, byte[] before, byte[] after, string mod) =>
        ModelOf(harness).NoteWrite(entity, component, before, after, mod);
}
