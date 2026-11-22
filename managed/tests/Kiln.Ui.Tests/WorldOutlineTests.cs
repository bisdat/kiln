// SPDX-License-Identifier: MIT
// Ui.World.Outline (0.2.0-dev.30.3, GitHub issue #111) against a fake kiln.render v2 table: the options' checks, the request
// as kiln.render gets it (colour, pulse, priority, duration), State and Reason read from outline_info, Dispose and
// ClearOutlines, the mod's unload, the error rule, and a v1 table (no outlines) as unavailable. The native rules (groups,
// priority, the game's highlight) are kiln.render's own host test.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Kiln;
using Kiln.FakeUi;
using Xunit;

namespace WorldOutlineTestSuite;

internal static unsafe class FakeRender
{
    public static RenderNative.TableV2* Table;
    public static readonly List<(ulong Owner, uint Entity, RenderNative.Outline Opts, ulong Handle)> Added = [];
    public static readonly Dictionary<ulong, (ulong Owner, RenderNative.OutlineInfo Info)> Infos = [];
    public static readonly List<ulong> Removed = [];
    public static readonly List<ulong> Cleared = [];
    public static int AddRc;
    public static string Detail = "";
    public static ulong NextHandle = 700;

    public static void Install(bool v2 = true)
    {
        Free();
        Table = (RenderNative.TableV2*)NativeMemory.AllocZeroed((nuint)sizeof(RenderNative.TableV2));
        Table->Base.Header = new ServiceHeader
        {
            Size = v2 ? (uint)sizeof(RenderNative.TableV2) : (uint)sizeof(RenderNative.Table), Version = v2 ? 2u : 1u,
        };
        Table->Base.GetStatus = &Status;
        if (v2)
        {
            Table->AddOutline = &Add;
            Table->GetOutlineInfo = &Info;
            Table->RemoveOutline = &Remove;
            Table->ClearOutlines = &Clear;
            Table->GetOutlineStatus = &OutlineStatus;
        }
        Added.Clear();
        Infos.Clear();
        Removed.Clear();
        Cleared.Clear();
        AddRc = 0;
        Detail = "";
        NextHandle = 700;
    }

    public static void Free()
    {
        if (Table != null) NativeMemory.Free(Table);
        Table = null;
    }

    public static void Set(ulong handle, uint state, uint reason, uint merged = 0)
    {
        var (owner, info) = Infos[handle];
        info.State = state;
        info.Reason = reason;
        info.Merged = merged;
        info.Group = state == RenderNative.OutlineShown ? 4u : 0u;
        Infos[handle] = (owner, info);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Status(RenderNative.Status* s) => 3;  // the camera is not this test's

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Add(ulong owner, uint entity, RenderNative.Outline* o, ulong* h)
    {
        if (AddRc != 0) return AddRc;
        if (owner == 0 || entity == 0 || o == null || o->Size < 24 || (o->Rgba & 0xFF) == 0 || o->Reserved != 0 || (o->Flags & ~1u) != 0)
            return 1;
        var handle = NextHandle++;
        Added.Add((owner, entity, *o, handle));
        Infos[handle] = (owner, new RenderNative.OutlineInfo { Size = 24, State = 0, Reason = 1, Entity = entity });
        *h = handle;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Info(ulong h, RenderNative.OutlineInfo* o)
    {
        if (!Infos.TryGetValue(h, out var e)) return 2;
        *o = e.Info;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Remove(ulong h)
    {
        if (!Infos.TryGetValue(h, out var e)) return 2;
        Removed.Add(h);
        e.Info.State = 3;
        e.Info.Reason = 16;
        Infos[h] = e;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Clear(ulong owner)
    {
        Cleared.Add(owner);
        foreach (var h in Infos.Where(kv => kv.Value.Owner == owner).Select(kv => kv.Key).ToArray()) Infos.Remove(h);
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int OutlineStatus(RenderNative.OutlineStatus* s)
    {
        var bytes = Encoding.UTF8.GetBytes(Detail);
        for (var i = 0; i < bytes.Length && i < 159; i++) s->Detail[i] = bytes[i];
        return 0;
    }

    public sealed class Backend(HashSet<string> features, bool withTable = true) : IKilnBackend
    {
        public string Version => "test";
        public bool IsTickThread => true;
        public bool HasFeature(string name) => features.Contains(name);
        public void Log(ulong owner, LogLevel level, string text) { }
        public nint Service(string name, uint minVersion) => name == "kiln.render" && withTable ? (nint)Table : 0;
        public ulong OwnerCreate(string id) => 99;
    }
}

[Collection(UiSerial.Name)]
public sealed unsafe class WorldOutlineTests : IDisposable
{
    private readonly IKilnBackend _previous;
    private static readonly HashSet<string> Rows = [Ui.World.OutlineCapability];
    private static readonly EntityId Chest = new(4100);

    public WorldOutlineTests()
    {
        _previous = Backend.Current;
        FakeRender.Install();
        Use(new FakeRender.Backend(Rows));
    }

    private static void Use(IKilnBackend b)
    {
        Backend.Current = b;
        RenderNative.ResetForTests();
        WorldOutlines.ResetForTests();
    }

    public void Dispose()
    {
        WorldOutlines.ResetForTests();
        Backend.Current = _previous;
        RenderNative.ResetForTests();
        FakeRender.Free();
    }

    private static ModContext NewMod(ulong owner) => HostAccess.CreateContext(
        new ModInfo("outline-" + Guid.NewGuid().ToString("N"), "Outline test", "1.0.0", Path.GetTempPath()), owner, Path.GetTempPath());

    private static T As<T>(ModContext mod, Func<T> call)
    {
        var previous = HostAccess.Current;
        HostAccess.Current = mod;
        try { return call(); }
        finally { HostAccess.Current = previous; }
    }

    [Fact]
    public void Options_check_their_duration_and_default_to_gold()
    {
        var o = new OutlineOptions();
        Assert.Equal(new Color(255, 196, 64, 255), o.Color);
        Assert.Equal(Ui.World.DefaultOutlineColor, o.Color);
        Assert.Equal((false, 0, (TimeSpan?)null, 0u), (o.Pulse, o.Priority, o.Duration, o.DurationMs));
        Assert.Throws<ArgumentOutOfRangeException>(() => new OutlineOptions { Duration = TimeSpan.Zero });
        Assert.Throws<ArgumentOutOfRangeException>(() => new OutlineOptions { Duration = TimeSpan.FromSeconds(-1) });
        Assert.Throws<ArgumentOutOfRangeException>(() => new OutlineOptions { Duration = TimeSpan.FromMilliseconds(uint.MaxValue + 1.0) });
        Assert.Throws<ArgumentOutOfRangeException>(() => o with { Duration = TimeSpan.MaxValue });
        Assert.Equal(1u, new OutlineOptions { Duration = TimeSpan.FromTicks(1) }.DurationMs);  // a part of a millisecond is one, never "until removed"
        Assert.Equal(uint.MaxValue, new OutlineOptions { Duration = TimeSpan.FromMilliseconds(uint.MaxValue) }.DurationMs);
        Assert.Equal(8000u, (o with { Duration = TimeSpan.FromSeconds(8) }).DurationMs);
    }

    [Fact]
    public void Entity_zero_and_an_invisible_colour_are_ArgumentExceptions()
    {
        Assert.Throws<ArgumentException>(() => Ui.World.Outline(EntityId.None));
        Assert.Throws<ArgumentException>(() => Ui.World.Outline(Chest, new OutlineOptions { Color = Color.Green.WithAlpha(0) }));
        Assert.Empty(FakeRender.Added);
    }

    [Fact]
    public void Outline_sends_the_request_and_Dispose_removes_it_once()
    {
        Assert.True(Ui.World.OutlinesAvailable);
        var outline = Ui.World.Outline(Chest, new OutlineOptions
        {
            Color = Color.Green, Pulse = true, Priority = 3, Duration = TimeSpan.FromSeconds(8),
        });
        var (owner, entity, opts, handle) = Assert.Single(FakeRender.Added);
        Assert.NotEqual(0ul, owner);  // Kiln.Ui's own owner: no mod called
        Assert.Equal(Chest.Value, entity);
        Assert.Equal((24u, Color.Green.Rgba, 3, RenderNative.OutlinePulse, 8000u, 0u),
            (opts.Size, opts.Rgba, opts.Priority, opts.Flags, opts.DurationMs, opts.Reserved));
        Assert.Equal((Chest, Color.Green), (outline.Entity, outline.Options.Color));
        Assert.Equal((OutlineState.Waiting, OutlineReason.Pending, true), (outline.State, outline.Reason, outline.Active));
        Assert.Equal(1, WorldOutlines.LiveCount);

        outline.Dispose();
        outline.Dispose();
        Assert.Equal([handle], FakeRender.Removed);
        Assert.Equal((OutlineState.Ended, OutlineReason.Removed, false), (outline.State, outline.Reason, outline.Active));
        Assert.Equal(0, WorldOutlines.LiveCount);
    }

    [Fact]
    public void A_default_outline_is_gold_steady_and_until_removed()
    {
        using var outline = Ui.World.Outline(Chest);
        var opts = Assert.Single(FakeRender.Added).Opts;
        Assert.Equal((new Color(255, 196, 64).Rgba, 0, 0u, 0u), (opts.Rgba, opts.Priority, opts.Flags, opts.DurationMs));
    }

    [Fact]
    public void State_reason_and_shared_colour_are_read_from_outline_info()
    {
        var outline = Ui.World.Outline(Chest);
        var h = outline.Handle;
        FakeRender.Set(h, RenderNative.OutlineShown, 0, merged: 1);
        Assert.Equal((OutlineState.Shown, OutlineReason.None, true), (outline.State, outline.Reason, outline.ColorShared));
        FakeRender.Set(h, RenderNative.OutlineWaiting, 2);
        Assert.Equal((OutlineState.Waiting, OutlineReason.Outshone, false), (outline.State, outline.Reason, outline.ColorShared));
        FakeRender.Set(h, RenderNative.OutlineGame, 0);
        Assert.Equal(OutlineState.GameHighlight, outline.State);
        FakeRender.Set(h, RenderNative.OutlineWaiting, 4);
        Assert.Equal(OutlineReason.SwitchedOff, outline.Reason);
        FakeRender.Set(h, RenderNative.OutlineEnded, 17);  // the chest despawned
        Assert.Equal((OutlineState.Ended, OutlineReason.Despawned), (outline.State, outline.Reason));
        Assert.Equal(0, WorldOutlines.LiveCount);  // ended there: nothing held
        outline.Dispose();
        Assert.Empty(FakeRender.Removed);
        Assert.Equal(OutlineReason.Despawned, outline.Reason);
    }

    [Fact]
    public void Dispose_of_an_outline_that_ended_there_keeps_its_reason()
    {
        var outline = Ui.World.Outline(Chest, new OutlineOptions { Duration = TimeSpan.FromSeconds(1) });
        FakeRender.Set(outline.Handle, RenderNative.OutlineEnded, 19);
        outline.Dispose();
        Assert.Equal((OutlineState.Ended, OutlineReason.Expired), (outline.State, outline.Reason));
    }

    [Fact]
    public void The_enums_are_the_native_values()
    {
        Assert.Equal([0, 1, 2, 3], Enum.GetValues<OutlineState>().Select(v => (int)v));
        Assert.Equal(["Waiting", "Shown", "GameHighlight", "Ended"], Enum.GetNames<OutlineState>());
        var reasons = new Dictionary<OutlineReason, int>
        {
            [OutlineReason.None] = 0, [OutlineReason.Pending] = 1, [OutlineReason.Outshone] = 2, [OutlineReason.NoModel] = 3,
            [OutlineReason.SwitchedOff] = 4, [OutlineReason.Removed] = 16, [OutlineReason.Despawned] = 17,
            [OutlineReason.WorldEnded] = 18, [OutlineReason.Expired] = 19, [OutlineReason.ModUnloaded] = 20, [OutlineReason.Failed] = 21,
        };
        Assert.Equal(reasons.Count, Enum.GetValues<OutlineReason>().Length);
        foreach (var (r, v) in reasons) Assert.Equal(v, (int)r);
        Assert.Equal(24, sizeof(RenderNative.Outline));
        Assert.Equal(24, sizeof(RenderNative.OutlineInfo));
        Assert.Equal(208, sizeof(RenderNative.OutlineStatus));
        Assert.Equal(112, sizeof(RenderNative.Table));
        Assert.Equal(152, sizeof(RenderNative.TableV2));
    }

    [Fact]
    public void A_handle_kiln_render_forgot_reads_as_ended()
    {
        var outline = Ui.World.Outline(Chest);
        FakeRender.Infos.Remove(outline.Handle);
        Assert.Equal((OutlineState.Ended, OutlineReason.Removed), (outline.State, outline.Reason));
        outline.Dispose();
        Assert.Empty(FakeRender.Removed);
    }

    [Fact]
    public void A_v1_table_no_table_or_an_off_row_is_KilnUnavailable()
    {
        FakeRender.Install(v2: false);
        Use(new FakeRender.Backend(Rows));
        Assert.False(Ui.World.OutlinesAvailable);
        var e = Assert.Throws<KilnUnavailableException>(() => Ui.World.Outline(Chest));
        Assert.Equal(Ui.World.OutlineCapability, e.Capability);
        Ui.World.ClearOutlines();  // never throws
        Assert.Equal(CapabilityReason.ServiceOlder, Capabilities.Get(Ui.World.OutlineCapability).Reason);

        Use(new FakeRender.Backend(Rows, withTable: false));
        Assert.False(Ui.World.OutlinesAvailable);
        Assert.Equal(Ui.World.OutlineCapability, Assert.Throws<KilnUnavailableException>(() => Ui.World.Outline(Chest)).Capability);

        FakeRender.Install();
        Use(new FakeRender.Backend([]));
        Assert.False(Ui.World.OutlinesAvailable);
        Assert.Throws<KilnUnavailableException>(() => Ui.World.Outline(Chest));
        Assert.Empty(FakeRender.Added);
    }

    [Fact]
    public void Native_refusals_throw_and_keep_nothing()
    {
        FakeRender.AddRc = 3;
        FakeRender.Detail = "feature ui.world_outline is off";
        var e = Assert.Throws<KilnUnavailableException>(() => Ui.World.Outline(Chest));
        Assert.Contains("feature ui.world_outline is off", e.Message);
        FakeRender.AddRc = 5;
        Assert.Throws<InvalidOperationException>(() => Ui.World.Outline(Chest));
        FakeRender.AddRc = 1;
        Assert.Throws<ArgumentException>(() => Ui.World.Outline(Chest));
        Assert.Equal(0, WorldOutlines.LiveCount);
    }

    [Fact]
    public void ClearOutlines_ends_the_calling_mods_outlines_only()
    {
        var a = NewMod(72);
        var b = NewMod(73);
        var a1 = As(a, () => Ui.World.Outline(Chest));
        var a2 = As(a, () => Ui.World.Outline(new EntityId(4101), new OutlineOptions { Color = Color.Cyan }));
        var b1 = As(b, () => Ui.World.Outline(Chest, new OutlineOptions { Priority = 5 }));
        Assert.Equal([72ul, 72ul, 73ul], FakeRender.Added.Select(x => x.Owner));
        As(a, () => { Ui.World.ClearOutlines(); return 0; });
        Assert.Equal([72ul], FakeRender.Cleared);
        Assert.Equal((OutlineState.Ended, OutlineReason.Removed), (a1.State, a1.Reason));
        Assert.Equal((OutlineState.Ended, OutlineReason.Removed), (a2.State, a2.Reason));
        Assert.True(b1.Active);
        Assert.Equal(1, WorldOutlines.LiveCount);
        a1.Dispose();
        Assert.DoesNotContain(FakeRender.Added[0].Handle, FakeRender.Removed.Skip(2));  // ended already: no second remove
    }

    [Fact]
    public void A_mods_unload_ends_its_outlines_and_clears_its_owner()
    {
        var mod = NewMod(72);
        var outline = As(mod, () => Ui.World.Outline(Chest, new OutlineOptions { Pulse = true }));
        mod.Retire();
        Assert.Contains(72ul, FakeRender.Cleared);
        Assert.Equal((OutlineState.Ended, OutlineReason.ModUnloaded, false), (outline.State, outline.Reason, outline.Active));
        var removed = FakeRender.Removed.Count;
        outline.Dispose();  // after the unload: a no-op
        Assert.Equal(removed, FakeRender.Removed.Count);
        Assert.Equal(0, WorldOutlines.LiveCount);
        Assert.Throws<InvalidOperationException>(() => As(mod, () => Ui.World.Outline(Chest)));
    }
}
