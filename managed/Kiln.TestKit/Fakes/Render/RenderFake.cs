// SPDX-License-Identifier: MIT
// The fake kiln.render (native/core/abi/services/kiln_render.h, v2) under the real Ui.World.Outline of Kiln.Ui (0.2.0-dev.30.3,
// GitHub issue #111): a mod's outline requests are recorded with their entity, colour, pulse, priority and duration, and
// at the start of each step (the next client step, as kiln.render's stage) the native rules decide their state: priority
// then newest per entity, the game's own highlight first unless the player's setting says mods-first, "off" keeps them
// waiting, an entity without a model waits, two colours at once and a third shares the nearest, a duration runs out on the
// harness's virtual time. A test sets the game's side (KilnHarness.Outlines: GameHighlight, HideModel, Setting, Despawn,
// EndWorld) or pins a request's state (SetState). Drawing (RenderFrameEvent) and the camera are not modelled: the draw
// functions do nothing, status and camera answer unavailable, render.camera is offline.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln.TestKit;

internal sealed class RenderFakeState
{
    public sealed class Req
    {
        public ulong Handle, Owner;
        public uint Entity, Rgba, Flags, DurationMs;
        public int Priority;
        public long Seq, MadeMs;
        public uint State, Reason = RenderFake.Pending, Group, Merged;
        public bool Pinned, Forgotten;
    }

    public readonly object Lock = new();
    public readonly List<Req> All = [];
    public readonly HashSet<uint> GameHighlights = [];
    public readonly HashSet<uint> NoModel = [];
    public uint Setting;  // 0 game-first, 1 mods-first, 2 off
    public ulong NextHandle = 1;
    public long Seq;
    public ulong Sets, Steps;
}

internal sealed unsafe class RenderFake : IKitFake
{
    public const string Service = RenderNative.Service;
    public const string OutlineRow = Ui.World.OutlineCapability;
    public const string CameraRow = "render.camera";
    public const uint Waiting = 0, Shown = 1, Game = 2, Ended = 3;
    public const uint Ok = 0, Pending = 1, Outshone = 2, NoModelReason = 3, SwitchedOff = 4, Removed = 16, Despawned = 17,
        World = 18, Expired = 19;
    public const int MaxPerOwner = 1024;

    private static RenderNative.TableV2* _table;

    string IKitFake.Service => Service;

    public void Register()
    {
        if (_table == null)
        {
            var v2 = (RenderNative.TableV2*)NativeMemory.AllocZeroed((nuint)sizeof(RenderNative.TableV2));
            var t = &v2->Base;
            t->Header = new ServiceHeader { Size = (uint)sizeof(RenderNative.TableV2), Version = 2 };
            t->GetStatus = &Status;
            t->AcquireFrames = &AcquireFrames;
            t->Release = &Release;
            t->GetCamera = &Camera;
            t->WorldToScreen = &WorldToScreen;
            t->Line = &Line;
            t->Rect = &Rect;
            t->Circle = &Circle;
            t->Triangle = &Triangle;
            t->Polyline = &Polyline;
            t->Text = &Text;
            t->TextSize = &TextSize;
            t->Image = &Image;
            v2->AddOutline = &AddOutline;
            v2->GetOutlineInfo = &OutlineInfo;
            v2->RemoveOutline = &RemoveOutline;
            v2->ClearOutlines = &ClearOutlines;
            v2->GetOutlineStatus = &OutlineStatus;
            _table = v2;  // process lifetime
        }
        KitBackend.Instance.RegisterTable(Service, (nint)_table);
        FakeTables.Features([OutlineRow]);
        FakeTables.Offline(CameraRow, "the game's camera is not modelled by the test kit's fake of kiln.render");
    }

    public void Start(KilnHarness harness) { }

    // the next client step: kiln.render's stage applies what was asked since
    public void BeforeStep(KilnHarness harness, ulong step)
    {
        var st = KitFakes.State<RenderFakeState>(harness);
        lock (st.Lock)
        {
            st.Steps++;
            Decide(st, harness.VirtualMs);
        }
    }

    public void End(KilnHarness harness)
    {
        var st = KitFakes.State<RenderFakeState>(harness);
        lock (st.Lock)
            foreach (var r in st.All)
                if (r.State != Ended) EndReq(r, World);
    }

    public void AddCoverage(List<CoverageRow> rows) =>
        rows.Add(new CoverageRow("Outlines in the world", CoverageKind.RecordingFake, [Service, OutlineRow],
            "Ui.World.Outline's own code on the kit's kiln.render v2: each request is recorded with its entity, colour, pulse, " +
            "priority and duration (KilnHarness.Outlines.All, Live, Of, Showing); at the start of the next step the native rules " +
            "decide its state: on one entity the highest priority, then the newest, shows and the others wait (Outshone); the " +
            "game's own highlight shows instead (GameHighlight) unless the player's setting is mods-first; off keeps every " +
            "request waiting; an entity without a model waits (HideModel); two colours at once and a third shares the nearest " +
            "(ColorShared); a duration ends on the harness's virtual time. Despawn and EndWorld end requests, SetState pins one. " +
            "Offline: the game's highlight itself, RenderFrameEvent's drawing (the draw functions do nothing; Render.Available " +
            "is false without kiln.ui) and the camera (render.camera)."));

    internal static RenderFakeState? State =>
        FakeTables.Active(Service) is { } h ? KitFakes.State<RenderFakeState>(h) : null;

    internal static void EndReq(RenderFakeState.Req r, uint reason)
    {
        r.State = Ended;
        r.Reason = reason;
        r.Group = 0;
        r.Merged = 0;
    }

    /// <summary>kiln.render's stage over the live requests: durations, then per entity who shows, then the colour groups.</summary>
    internal static void Decide(RenderFakeState st, long nowMs)
    {
        foreach (var r in st.All)
            if (r.State != Ended && r.DurationMs != 0 && nowMs - r.MadeMs >= r.DurationMs)
                EndReq(r, Expired);
        var shown = new List<RenderFakeState.Req>();
        foreach (var group in st.All.Where(r => r.State != Ended && !r.Pinned).GroupBy(r => r.Entity))
        {
            var winner = group.OrderByDescending(r => r.Priority).ThenByDescending(r => r.Seq).First();
            foreach (var r in group)
            {
                r.Group = 0;
                r.Merged = 0;
                if (st.Setting == 2) (r.State, r.Reason) = (Waiting, SwitchedOff);
                else if (st.NoModel.Contains(r.Entity)) (r.State, r.Reason) = (Waiting, NoModelReason);
                else if (!ReferenceEquals(r, winner)) (r.State, r.Reason) = (Waiting, Outshone);
                else if (st.Setting == 0 && st.GameHighlights.Contains(r.Entity)) (r.State, r.Reason) = (Game, Ok);
                else
                {
                    (r.State, r.Reason) = (Shown, Ok);
                    shown.Add(r);
                }
            }
        }
        // two groups for mods (4, 5), one per distinct (colour, pulse); a further pair shares the nearest colour's
        var pairs = new List<(uint Rgba, uint Pulse, uint Group)>();
        foreach (var r in shown.OrderBy(r => r.Seq))
        {
            var pulse = r.Flags & RenderNative.OutlinePulse;
            var have = pairs.FindIndex(p => p.Rgba == r.Rgba && p.Pulse == pulse);
            if (have >= 0)
            {
                r.Group = pairs[have].Group;
                continue;
            }
            if (pairs.Count < 2)
            {
                var g = pairs.Count == 0 ? 4u : 5u;
                pairs.Add((r.Rgba, pulse, g));
                r.Group = g;
                continue;
            }
            r.Group = pairs.OrderBy(p => Distance(p.Rgba, r.Rgba)).First().Group;
            r.Merged = 1;
        }
        st.Sets += (ulong)shown.Count;
    }

    private static int Distance(uint a, uint b)
    {
        int dr = (int)(a >> 24) - (int)(b >> 24), dg = (int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF),
            db = (int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF);
        return dr * dr + dg * dg + db * db;
    }

    // ---- frames and camera: not modelled ----
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Status(RenderNative.Status* s) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int AcquireFrames(ulong owner, ulong* h)
    {
        if (h != null) *h = 0;
        return FakeTables.Unavailable;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Release(ulong h) => FakeTables.NotFound;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Camera(RenderNative.Camera* c)
    {
        if (c != null && c->Size >= (uint)sizeof(RenderNative.Camera))
        {
            var size = c->Size;
            *c = default;
            c->Size = size;
        }
        return FakeTables.Unavailable;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte WorldToScreen(double* world, float w, float h, float* o) => 0;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Line(float x0, float y0, float x1, float y1, uint rgba, float th) { }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Rect(float x0, float y0, float x1, float y1, uint rgba, float rounding, float th, int filled) { }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Circle(float x, float y, float r, uint rgba, int segments, float th, int filled) { }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint rgba, float th, int filled) { }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Polyline(float* xy, uint count, uint rgba, float th, int closed, int filled) { }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Text(float x, float y, uint rgba, float size, uint flags, NativeStr text) { }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void TextSize(NativeStr text, float size, float* w, float* h)
    {
        if (w != null) *w = 0;
        if (h != null) *h = 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Image(ulong image, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint tint) { }

    // ---- outlines ----
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int AddOutline(ulong owner, uint entity, RenderNative.Outline* o, ulong* h)
    {
        try
        {
            if (h != null) *h = 0;
            if (owner == 0 || entity == 0 || o == null || h == null || o->Size < (uint)sizeof(RenderNative.Outline) ||
                (o->Rgba & 0xFF) == 0 || (o->Flags & ~RenderNative.OutlinePulse) != 0 || o->Reserved != 0)
                return FakeTables.InvalidArgument;
            if (FakeTables.Off(OutlineRow) || KitBackend.Instance.Harness is not { } harness || State is not { } st)
                return FakeTables.Unavailable;
            lock (st.Lock)
            {
                if (st.All.Count(r => r.Owner == owner && r.State != Ended) >= MaxPerOwner) return FakeTables.BufferTooSmall;
                var r = new RenderFakeState.Req
                {
                    Handle = (0x52ul << 56) | st.NextHandle++, Owner = owner, Entity = entity, Rgba = o->Rgba, Flags = o->Flags,
                    DurationMs = o->DurationMs, Priority = o->Priority, Seq = ++st.Seq, MadeMs = harness.VirtualMs,
                };
                st.All.Add(r);
                *h = r.Handle;
            }
            return FakeTables.Ok;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    private static RenderFakeState.Req? Find(RenderFakeState st, ulong handle) =>
        st.All.FirstOrDefault(r => r.Handle == handle && !r.Forgotten);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int OutlineInfo(ulong handle, RenderNative.OutlineInfo* o)
    {
        if (o == null || o->Size < (uint)sizeof(RenderNative.OutlineInfo)) return FakeTables.InvalidArgument;
        if (State is not { } st) return FakeTables.NotFound;
        lock (st.Lock)
        {
            if (Find(st, handle) is not { } r) return FakeTables.NotFound;
            o->State = r.State;
            o->Reason = r.Reason;
            o->Entity = r.Entity;
            o->Group = r.Group;
            o->Merged = r.Merged;
        }
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RemoveOutline(ulong handle)
    {
        if (State is not { } st) return FakeTables.NotFound;
        lock (st.Lock)
        {
            if (Find(st, handle) is not { } r) return FakeTables.NotFound;
            if (r.State != Ended) EndReq(r, Removed);
        }
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ClearOutlines(ulong owner)
    {
        if (owner == 0) return FakeTables.InvalidArgument;
        if (State is not { } st) return FakeTables.Ok;
        lock (st.Lock)
            foreach (var r in st.All)
            {
                if (r.Owner != owner) continue;
                if (r.State != Ended) EndReq(r, Removed);
                r.Forgotten = true;  // outline_info answers NOT_FOUND from now on
            }
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int OutlineStatus(RenderNative.OutlineStatus* s)
    {
        if (s == null || s->Size < (uint)sizeof(RenderNative.OutlineStatus)) return FakeTables.InvalidArgument;
        var size = s->Size;
        *s = default;
        s->Size = size;
        var off = FakeTables.Off(OutlineRow);
        var st = State;
        s->Available = (byte)(off || st is null ? 0 : 1);
        var detail = off ? "ui.world_outline is switched off by the test" : st is null ? "no harness runs" : "";
        if (st is not null)
            lock (st.Lock)
            {
                var live = st.All.Where(r => r.State != Ended).ToArray();
                s->Installed = s->Staged = (byte)(live.Length > 0 ? 1 : 0);
                s->Setting = (byte)st.Setting;
                s->Requests = (uint)live.Length;
                s->Entities = (uint)live.Select(r => r.Entity).Distinct().Count();
                s->Shown = (uint)live.Where(r => r.State == Shown).Select(r => r.Entity).Distinct().Count();
                s->Game = (uint)live.Where(r => r.State == Game).Select(r => r.Entity).Distinct().Count();
                s->NoModel = (uint)live.Where(r => r.Reason == NoModelReason).Select(r => r.Entity).Distinct().Count();
                s->Sets = st.Sets;
                s->Steps = st.Steps;
            }
        var bytes = Encoding.UTF8.GetBytes(detail);
        for (var i = 0; i < bytes.Length && i < 159; i++) s->Detail[i] = bytes[i];
        return FakeTables.Ok;
    }
}
