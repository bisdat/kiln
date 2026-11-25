// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln.TestKit;

/// <summary>One harness's scripted crosshair (KilnHarness.Crosshair). Guarded by its own lock: a mod may read the
/// crosshair from any thread.</summary>
internal sealed class CrosshairFakeState
{
    internal readonly object Lock = new();
    internal CrosshairNative.Hit Sample;  // Sequence 0: nothing aimed at
    internal ulong Sequence;
    internal ulong AgeNs;
    internal bool Faulted;
    internal int Reads;
}

/// <summary>The kit's fake of kiln.client.crosshair (native/core/abi/services/kiln_client_crosshair.h): get answers the
/// harness's scripted sample as the service does: NO_SAMPLE until the test aims, STALE past KILN_CROSSHAIR_FRESH_NS,
/// FAULT after a scripted fault, unavailable while the row is off. Since 0.2.0-dev.31.13.</summary>
internal sealed unsafe class CrosshairFake : IKitFake
{
    private static CrosshairNative.Table* _table;

    public string Service => CrosshairNative.Service;

    public void Register()
    {
        if (_table == null)
        {
            _table = (CrosshairNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(CrosshairNative.Table));
            _table->Header = new() { Size = (uint)sizeof(CrosshairNative.Table), Version = 1 };
            _table->Get = &Get;
        }
        KitBackend.Instance.RegisterTable(Service, (nint)_table);
        FakeTables.Features([Crosshair.Capability]);
    }

    public void Start(KilnHarness harness) { }
    public void BeforeStep(KilnHarness harness, ulong step) { }
    public void End(KilnHarness harness) { }

    public void AddCoverage(List<CoverageRow> rows) => rows.Add(new("Crosshair (since 0.2.0-dev.31.13)", CoverageKind.RecordingFake,
        [Service, Crosshair.Capability],
        "Crosshair.Read and TryGet read the sample the test aims (KilnHarness.Crosshair: Aim, AimAtNothing, Age, Fault, Clear): " +
        "hit or miss, position, normal, distance, entity, voxel material, the next sequence and the harness's step; stale past " +
        "Crosshair.FreshFor. Not modelled: the game's trace (its reach, the held item, what it hits), the client session's start and end. " +
        "In a game the first read of the process answers NoSample while Kiln's hook goes in; the fake answers the aimed sample at once."));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Get(CrosshairNative.Hit* out_, uint* reason)
    {
        if (out_ == null || out_->Size < (uint)sizeof(CrosshairNative.Hit)) return FakeTables.InvalidArgument;
        if (FakeTables.Active(CrosshairNative.Service) is not { } harness || FakeTables.Off(Crosshair.Capability)) return FakeTables.Unavailable;
        var s = KitFakes.State<CrosshairFakeState>(harness);
        uint why;
        lock (s.Lock)
        {
            s.Reads++;
            if (s.Faulted || s.Sample.Sequence == 0)
            {
                *out_ = new CrosshairNative.Hit { Size = (uint)sizeof(CrosshairNative.Hit) };
                why = s.Faulted ? (uint)CrosshairReason.Faulted : (uint)CrosshairReason.NoSample;
            }
            else
            {
                *out_ = s.Sample;
                out_->Size = (uint)sizeof(CrosshairNative.Hit);
                out_->AgeNs = s.AgeNs;
                why = s.AgeNs > CrosshairNative.FreshNs ? (uint)CrosshairReason.Stale : (uint)CrosshairReason.None;
            }
        }
        if (reason != null) *reason = why;
        return 0;
    }
}
