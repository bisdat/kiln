// SPDX-License-Identifier: MIT
// The kit's fakes of kiln.decisions and kiln.window_clock (native/core/abi/services/kiln_decisions.h,
// kiln_window_clock.h, both v1) under Kiln.Api's own dispatcher (DecisionBroker). The tables answer from the running
// harness's DecisionsModel, and as the native services do when they are unavailable otherwise. The records are
// Kiln.Api's mirrors of the headers (DecisionsNative); their sizes are pinned to the headers' by the kit's tests.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using N = Kiln.DecisionsNative;

namespace Kiln.TestKit;

/// <summary>Plugs the fake kiln.decisions into the kit (<see cref="IKitFake"/>).</summary>
internal sealed unsafe class DecisionsFake : IKitFake
{
    public string Service => N.Service;

    private static N.Table* _table;

    public void Register()
    {
        if (_table == null)
        {
            var t = (N.Table*)NativeMemory.AllocZeroed((nuint)sizeof(N.Table));  // process lifetime, never freed
            t->Header = new ServiceHeader { Size = (uint)sizeof(N.Table), Version = N.Version };
            t->SupportedKinds = &SupportedKinds;
            t->Attach = &Attach;
            t->ResolveEntity = &ResolveEntity;
            t->Interest = &Interest;
            t->EnqueuePrepared = &EnqueuePrepared;
            t->BeginRequests = &BeginRequests;
            t->ReadRequests = &ReadRequests;
            t->AckRequests = &AckRequests;
            t->Answer = &Answer;
            t->BeginOutcomes = &BeginOutcomes;
            t->ReadOutcomes = &ReadOutcomes;
            t->AckOutcomes = &AckOutcomes;
            t->ReevaluatePickup = &ReevaluatePickup;
            t->Detach = &Detach;
            t->WindowBegin = &Window;
            t->WindowEnd = &Window;
            _table = t;
        }
        KitBackend.Instance.RegisterTable(N.Service, (nint)_table);
    }

    public void Start(KilnHarness harness) => ModelOf(harness).Start(harness);
    public void BeforeStep(KilnHarness harness, ulong step) => ModelOf(harness).BeforeStep(step);
    public void End(KilnHarness harness) => ModelOf(harness).End();

    public void AddCoverage(List<CoverageRow> rows) => rows.Add(new CoverageRow("Decisions", CoverageKind.RecordingFake,
        [N.Service, N.ClockService],
        "Kiln.Api's own dispatcher of decisions on the kit's kiln.decisions and kiln.window_clock: the service's broker rule for rule " +
        "(request ids, frozen batches that a read does not consume, answers that are taken once, the deadlines: two more steps for a " +
        "held action, the same step for a client's input, 4096 open requests, 8192 kept outcomes), one DecisionCompletedEvent per " +
        "decision, and a world that ends (KilnHarness.Decisions.RestartWorld: what is open ends WorldEnded, the next world has the next " +
        "epoch). KilnHarness.Decisions.Outcomes records every decision's end. An entity's identity comes from the harness's fake world " +
        "(a life serial per world and entity; no player sessions). Decisions.ReevaluatePickup forgets the walk-over pickups a handler " +
        "canceled (drop and receiver; once the harness says the build holds walk-over pickups). Offline: the game's side of every " +
        "decision (no death, pickup or equipment change is held here: the events that need one are faked with their services), the " +
        "sampled exposures of walk-over pickups (a receiver leaving and coming back, a changed amount)"));

    public static DecisionsModel ModelOf(KilnHarness harness) => KitFakes.State<DecisionsModel>(harness);

    private static KilnHarness? Harness => FakeTables.Active(N.Service);
    private static DecisionsModel? Model => Harness is { } h ? ModelOf(h) : null;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static ulong SupportedKinds()
    {
        try { return Model?.SupportedKinds ?? 0; } catch { return 0; }
    }

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
    private static int ResolveEntity(uint entity, N.EntityKey* key, double* position, uint* hasPosition)
    {
        try { return Harness is { } h ? ModelOf(h).ResolveEntity(h, entity, key, position, hasPosition) : FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Interest(ulong owner, N.Interest* interest)
    {
        try { return Model?.Interest(owner, interest) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int EnqueuePrepared(ulong owner, N.Request* request, ulong* id)
    {
        try { return Model?.EnqueuePrepared(owner, request, id) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int BeginRequests(ulong owner, ulong* batch, uint* records)
    {
        try { return Model?.BeginRequests(owner, batch, records) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ReadRequests(ulong owner, ulong batch, N.Request* buffer, uint capacity, uint stride, uint* required)
    {
        try { return Model?.ReadRequests(owner, batch, buffer, capacity, stride, required) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int AckRequests(ulong owner, ulong batch)
    {
        try { return Model?.AckRequests(owner, batch) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Answer(ulong owner, N.Answer* answer, N.Receipt* receipt)
    {
        try { return Model?.Answer(owner, answer, receipt) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int BeginOutcomes(ulong owner, ulong* batch, uint* records)
    {
        try { return Model?.BeginOutcomes(owner, batch, records) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ReadOutcomes(ulong owner, ulong batch, N.Outcome* buffer, uint capacity, uint stride, uint* required)
    {
        try { return Model?.ReadOutcomes(owner, batch, buffer, capacity, stride, required) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int AckOutcomes(ulong owner, ulong batch)
    {
        try { return Model?.AckOutcomes(owner, batch) ?? FakeTables.Unavailable; } catch { return FakeTables.Internal; }
    }

    // The walk-over producer's suppressed exposures, as far as the fake knows them (a canceled kind-2 decision).
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ReevaluatePickup(uint drop, uint receiver, uint* reset)
    {
        try
        {
            if (Model is { } m) return m.ReevaluatePickup(drop, receiver, reset);
            if (reset != null) *reset = 0;
            return FakeTables.Unavailable;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    // The core's window calls: the harness's steps run them (DecisionsModel.BeforeStep), nobody calls the table's.
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Window(ulong epoch, ulong step) { }
}

/// <summary>Plugs the fake kiln.window_clock into the kit: the clock of the same model.</summary>
internal sealed unsafe class WindowClockFake : IKitFake
{
    public string Service => N.ClockService;

    private static N.ClockTable* _table;

    public void Register()
    {
        if (_table == null)
        {
            var t = (N.ClockTable*)NativeMemory.AllocZeroed((nuint)sizeof(N.ClockTable));  // process lifetime, never freed
            t->Header = new ServiceHeader { Size = (uint)sizeof(N.ClockTable), Version = N.Version };
            t->Get = &Get;
            _table = t;
        }
        KitBackend.Instance.RegisterTable(N.ClockService, (nint)_table);
    }

    public void Start(KilnHarness harness) { }
    public void BeforeStep(KilnHarness harness, ulong step) { }
    public void End(KilnHarness harness) { }
    public void AddCoverage(List<CoverageRow> rows) { }  // the Decisions row names it

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Get(N.WindowState* state)
    {
        try
        {
            if (state == null) return FakeTables.InvalidArgument;
            if (FakeTables.Active(N.ClockService) is not { } harness)
            {
                *state = default;
                state->Size = (uint)sizeof(N.WindowState);  // no world: phase 0
                return FakeTables.Ok;
            }
            DecisionsFake.ModelOf(harness).Clock(state);
            return FakeTables.Ok;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }
}
