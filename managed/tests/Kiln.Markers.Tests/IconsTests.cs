// SPDX-License-Identifier: MIT
// Markers.Icons and CatalogChanged on a fake catalog (made-up types, no game data): warm-up, the immutable list, resolve by
// the error rule, the event, and JSON icon names through the catalog.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Kiln;
using Xunit;

namespace MarkersTests;

internal static unsafe class FakeCatalog
{
    public sealed record Row(uint Id, string Name, string Alias, bool Travel);
    public static List<Row> Rows = new();
    public static ulong Generation;
    public static int Reads;
    public static nint CatalogFn;

    public static void Install()
    {
        FakeMarkers.Table->CatalogCount = &Count;
        FakeMarkers.Table->CatalogAt = &At;
        FakeMarkers.Table->CatalogResolve = &Resolve;
        FakeMarkers.Table->Subscribe = &Subscribe;
        Rows = new();
        Generation = 0;
        Reads = 0;
        CatalogFn = 0;
    }

    public static void Ready(params Row[] rows)
    {
        Rows = rows.OrderBy(r => r.Id).ToList();
        Generation = 1;
    }

    /// <summary>The service's catalog note, delivered through the subscribed callback.</summary>
    public static void Announce(bool ready, string reason = "")
    {
        if (CatalogFn == 0) return;
        var c = new MarkersNative.CatalogChange { Size = (uint)sizeof(MarkersNative.CatalogChange), Role = 1, Ready = ready ? 1u : 0, Count = (uint)Rows.Count, Generation = Generation };
        var b = Encoding.UTF8.GetBytes(reason);
        for (var i = 0; i < b.Length; i++) c.Reason[i] = b[i];
        ((delegate* unmanaged[Cdecl]<MarkersNative.CatalogChange*, void*, void>)CatalogFn)(&c, null);
    }

    private static void Put(byte* dst, int cap, string s)
    {
        var b = Encoding.UTF8.GetBytes(s);
        var n = Math.Min(b.Length, cap - 1);
        for (var i = 0; i < n; i++) dst[i] = b[i];
        dst[n] = 0;
    }

    private static void Fill(Row r, MarkersNative.Icon* o)
    {
        o->Id = r.Id;
        o->Flags = 1u | (r.Travel ? 32u : 0u);
        o->Surfaces = r.Travel ? 0u : 1u;
        for (var i = 0; i < 16; i++) o->TextureGuid[i] = (byte)(r.Id + i);
        Put(o->Key, 14, $"type:{r.Id:x8}");
        Put(o->Alias, 129, r.Alias);
        Put(o->TextureName, 129, r.Name);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Count(uint* count, ulong* generation)
    {
        *count = Generation == 0 ? 0 : (uint)Rows.Count;
        *generation = Generation;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int At(uint index, MarkersNative.Icon* o)
    {
        Reads++;
        if (Generation == 0 || index >= Rows.Count) return 2;
        Fill(Rows[(int)index], o);
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Resolve(NativeStr name, MarkersNative.Icon* o, MarkersNative.Result* r)
    {
        var n = name.ToString();
        void Say(string s) => Put(r->Reason, 256, s);
        if (n.Length == 0 || n.Trim() != n) return 1;
        if (Generation == 0)
        {
            r->Status = 7;
            Say("CatalogNotReady: the icon catalog is still warming up");
            return 0;
        }
        var low = n.ToLowerInvariant();
        var hits = Rows.Where(x => x.Alias == low || $"type:{x.Id:x8}" == low).ToList();
        if (hits.Count == 1)
        {
            Fill(hits[0], o);
            r->Status = 0;
            return 0;
        }
        r->Status = hits.Count == 0 ? 1u : 8u;
        Say(hits.Count == 0 ? $"NotFound: no icon '{n}'" : $"Ambiguous: '{n}' names {hits.Count} icons; use its canonical key");
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Subscribe(ulong owner, delegate* unmanaged[Cdecl]<MarkersNative.Change*, void*, void> changed,
        delegate* unmanaged[Cdecl]<MarkersNative.Visibility*, void*, void> visibility, delegate* unmanaged[Cdecl]<MarkersNative.CatalogChange*, void*, void> catalog,
        void* user, ulong* h)
    {
        FakeMarkers.Subscribes++;
        FakeMarkers.ChangeFn = (nint)changed;
        CatalogFn = (nint)catalog;
        *h = 1;
        return 0;
    }
}

public sealed class IconsTests : MarkersFixture
{
    public IconsTests()
    {
        FakeCatalog.Install();
        Markers.Icons.Forget();
    }

    private static readonly FakeCatalog.Row Well = new(0xffe5e444, "mapmarker_Well_Plant", "well_plant", false);
    private static readonly FakeCatalog.Row Travel = new(0x1095684e, "mapmarker_fastTravel", "fasttravel", true);
    private static readonly FakeCatalog.Row Twin = new(0x00000007, "icon_twin", "twin", false);
    private static readonly FakeCatalog.Row Twin2 = new(0x00000008, "mapmarker_twin", "twin", false);

    [Fact]
    public void Warm_up_then_ready_with_an_immutable_list()
    {
        Assert.False(Markers.Icons.IsReady);
        Assert.Empty(Markers.Icons.All);
        var warming = Markers.Icons.Resolve("well_plant");
        Assert.Equal((false, null as MarkerIconInfo), (warming.Ok, warming.Icon));
        Assert.StartsWith("CatalogNotReady", warming.Reason);
        FakeCatalog.Ready(Well, Travel);
        Assert.True(Markers.Icons.IsReady);
        var all = Markers.Icons.All;
        Assert.Equal([0x1095684eu, 0xffe5e444u], all.Select(i => i.Icon.Id));
        var well = all[1];
        Assert.Equal(("type:ffe5e444", "well_plant", "mapmarker_Well_Plant", MarkerSurface.Map, 1u), (well.Key, well.Alias, well.TextureName, well.Surfaces, well.Flags));
        Assert.Equal(new Guid(Enumerable.Range(0, 16).Select(i => (byte)(0x44 + i)).ToArray()), well.Texture);
        Assert.Equal((MarkerSurface)0, all[0].Surfaces);  // fast travel: listed, not for the map
        var reads = FakeCatalog.Reads;
        Assert.Same(all, Markers.Icons.All);  // cached per generation
        Assert.Equal(reads, FakeCatalog.Reads);
        Assert.Throws<NotSupportedException>(() => ((IList<MarkerIconInfo>)all).Add(well));
    }

    [Fact]
    public void Resolve_by_the_error_rule()
    {
        FakeCatalog.Ready(Well, Travel, Twin, Twin2);
        var r = Markers.Icons.Resolve("WELL_PLANT");
        Assert.True(r.Ok);
        Assert.Null(r.Reason);
        Assert.Equal(new MarkerIcon(0xffe5e444), r.Icon!.Icon);
        Assert.True(Markers.Icons.Resolve("type:ffe5e444").Ok);
        Assert.StartsWith("NotFound", Markers.Icons.Resolve("nothing").Reason);
        Assert.StartsWith("Ambiguous", Markers.Icons.Resolve("twin").Reason);
        Assert.Throws<ArgumentException>(() => Markers.Icons.Resolve(" well_plant"));
        Assert.Throws<ArgumentNullException>(() => Markers.Icons.Resolve(null!));
        Assert.IsAssignableFrom<IKilnResult>(r);
    }

    [Fact]
    public void CatalogChanged_reaches_a_mods_handler_until_it_unloads()
    {
        var mod = Mod("icons.mod", 77);
        var seen = new List<MarkerCatalogChangedEvent>();
        As(mod, () => Markers.CatalogChanged += seen.Add);
        FakeCatalog.Ready(Well, Travel);
        FakeCatalog.Announce(true);
        var e = Assert.Single(seen);
        Assert.Equal((1u, true, 1ul, 2, null as string), (e.Role, e.Ready, e.Generation, e.Count, e.Reason));
        FakeCatalog.Announce(true, "native map icons stay off: the catalog digest is not verified for this build");
        Assert.Equal("native map icons stay off: the catalog digest is not verified for this build", seen[^1].Reason);
        mod.Retire();
        FakeCatalog.Announce(true);
        Assert.Equal(2, seen.Count);
    }

    [Fact]
    public void Json_icon_names_go_through_the_catalog()
    {
        const string json = """{"id":"boss_gate","position":{"x":3670,"y":820,"z":1500},"text":"Raid entrance","icon":"well_plant","surfaces":["map","compass","world"],"follow":null,"height":2,"maxDistance":0,"priority":0,"rgba":4294967295,"mapLabel":"none"}""";
        Assert.Throws<InvalidOperationException>(() => MarkerSpec.FromJson(json));  // before the catalog is ready
        FakeCatalog.Ready(Well, Twin, Twin2);
        var spec = MarkerSpec.FromJson(json);
        Assert.Equal(new MarkerSpec("boss_gate", new WorldPosition(3670, 820, 1500), "Raid entrance", new MarkerIcon(0xffe5e444),
            MarkerSurface.Map | MarkerSurface.Compass | MarkerSurface.World, null, 2, 0, 0, 0xffffffff, MarkerMapLabel.None), spec);
        Assert.Equal(spec, MarkerSpec.FromJson(spec.ToJson()));  // the canonical key round-trips
        Assert.Contains("\"icon\":\"type:ffe5e444\"", spec.ToJson());
        Assert.Throws<ArgumentException>(() => MarkerSpec.FromJson(json.Replace("well_plant", "twin")));     // ambiguous
        Assert.Throws<ArgumentException>(() => MarkerSpec.FromJson(json.Replace("well_plant", "missing")));  // not found
    }
}
