// SPDX-License-Identifier: MIT
// Kiln.Markers' own code on a fake kiln.markers (chapter 04 §6, the offline list: wrappers, dispatch, the error rule): what
// it passes for a spec and its defaults, how each native answer becomes a MarkerResult or an exception, the binding of a
// mod to its id and its release at unload, and Markers.Changed's delivery (M01 and M14 as the wrapper sees them).
using System.Runtime.InteropServices;
using Kiln;
using Xunit;

namespace MarkersTests;

public sealed class AbiTests
{
    [Fact]
    public unsafe void The_mirror_has_the_headers_v1_layouts()
    {
        Assert.Equal(96, sizeof(MarkersNative.Spec));
        Assert.Equal(280, sizeof(MarkersNative.Result));
        Assert.Equal(320, sizeof(MarkersNative.AnchorResult));
        Assert.Equal(304, sizeof(MarkersNative.Icon));
        Assert.Equal(192, sizeof(MarkersNative.Change));
        Assert.Equal(208, sizeof(MarkersNative.Visibility));
        Assert.Equal(280, sizeof(MarkersNative.CatalogChange));
        Assert.Equal(144, sizeof(MarkersNative.Table));
        Assert.Equal(40, sizeof(MarkersNative.Peer));
        Assert.Equal(40, sizeof(MarkersNative.Envelope));
        Assert.Equal(48, sizeof(MarkersNative.BridgeTable));
        Assert.Equal(8, (int)Marshal.OffsetOf<MarkersNative.Spec>(nameof(MarkersNative.Spec.Key)));
        Assert.Equal(48, (int)Marshal.OffsetOf<MarkersNative.Spec>(nameof(MarkersNative.Spec.Follow)));
        Assert.Equal(80, (int)Marshal.OffsetOf<MarkersNative.Spec>(nameof(MarkersNative.Spec.Text)));
        Assert.Equal(72, (int)Marshal.OffsetOf<MarkersNative.Table>(nameof(MarkersNative.Table.SetLocal)));
        Assert.Equal(128, (int)Marshal.OffsetOf<MarkersNative.Table>(nameof(MarkersNative.Table.Subscribe)));
        Assert.Equal(56, (int)Marshal.OffsetOf<MarkersNative.Change>(nameof(MarkersNative.Change.Owner)));
        Assert.Equal(121, (int)Marshal.OffsetOf<MarkersNative.Change>(nameof(MarkersNative.Change.Key)));
        Assert.Equal(32, (int)Marshal.OffsetOf<MarkersNative.Icon>(nameof(MarkersNative.Icon.Key)));
        Assert.Equal(175, (int)Marshal.OffsetOf<MarkersNative.Icon>(nameof(MarkersNative.Icon.TextureName)));
    }
}

public sealed class WithoutTheServiceTests() : MarkersFixture(table: false)
{
    [Fact]
    public void Every_member_throws_naming_markers_local()
    {
        var mod = Mod("no.table", 77);
        var spec = new MarkerSpec("k", new WorldPosition(1, 2, 3), "t", MarkerIcon.None, MarkerSurface.World);
        foreach (var call in new Func<MarkerResult>[] { () => Markers.Set(spec), () => Markers.Remove("k"), Markers.Clear })
        {
            var e = Assert.Throws<KilnUnavailableException>(() => As(mod, call));
            Assert.Equal("markers.local", e.Capability);
            Assert.Contains("kiln.markers", e.Message);
        }
        Assert.False(Capabilities.Is("markers.compass"));
        // no core in a unit test: the row says why from kiln.markers' own (offline here, service-missing in a game)
        Assert.Contains(Capabilities.Get("markers.map").Reason, new[] { CapabilityReason.ServiceMissing, CapabilityReason.Offline });
        Assert.Contains("kiln.markers", Capabilities.Get("markers.map").ReasonText);
    }

    [Fact]
    public void A_table_published_after_a_refusal_is_used_at_the_next_call()
    {
        var mod = Mod("late.table", 77);
        Assert.Throws<KilnUnavailableException>(() => As(mod, Markers.Clear));
        FakeMarkers.Install();  // the service's table appears (published late); no refresh tick ran
        var r = As(mod, Markers.Clear);
        Assert.True(r.Ok);
        Assert.True(Capabilities.Is("markers.local"));
    }

    [Fact]
    public unsafe void A_table_smaller_than_v1_is_not_used()
    {
        FakeMarkers.Install(size: 136);  // one function short
        MarkersNative.Forget();
        Capabilities.Reset();
        var mod = Mod("small.table", 77);
        Assert.Throws<KilnUnavailableException>(() => As(mod, Markers.Clear));
        Assert.Empty(FakeMarkers.Calls);
    }
}

public sealed class MarkersTests : MarkersFixture
{
    private static readonly WorldPosition Gate = new(3670, 820, 1500);

    [Fact]
    public void A_spec_and_its_defaults_reach_the_service_as_written()
    {
        var mod = Mod("boss.mod", 77);
        var r = As(mod, () => Markers.Set(new MarkerSpec("boss_gate", Gate, "Raid entrance", new MarkerIcon(0xffe5e444))));
        Assert.True(r.Ok);
        var s = Assert.Single(FakeMarkers.Specs);
        Assert.Equal(new FakeSpec(77, 96, 3, "boss_gate", 3670, 820, 1500, 0, 0, 0, 0xffe5e444, 0xffffffff, 0, 0, "Raid entrance"), s);
        As(mod, () => Markers.Set(new MarkerSpec("label", Gate, "Zoë — 東京", MarkerIcon.None, MarkerSurface.World | MarkerSurface.Compass,
            Height: 2.5f, MaxDistance: 300, Priority: -7, Rgba: 0x80ff00ff, MapLabel: MarkerMapLabel.NativeTooltip)));
        var t = FakeMarkers.Specs[1];
        Assert.Equal((6u, 2.5f, 300f, -7, 0x80ff00ffu, 1u, "Zoë — 東京"), (t.Surfaces, t.Height, t.MaxDistance, t.Priority, t.Rgba, t.MapLabel, t.Text));
        Assert.Equal("bind 77 boss.mod", FakeMarkers.Calls[0]);
        Assert.Single(FakeMarkers.Calls, c => c.StartsWith("bind", StringComparison.Ordinal));  // bound once
    }

    [Fact]
    public void M01_results_and_changes_as_the_wrapper_reports_them()
    {
        var mod = Mod("m01.mod", 77);
        var seen = new List<MarkerChangedEvent>();
        As(mod, () => Markers.Changed += seen.Add);
        var a = new MarkerSpec("gate", Gate, "Raid entrance", MarkerIcon.None, MarkerSurface.Compass);
        var b = a with { Text = "Raid entrance (open)" };
        var results = new List<MarkerResult>
        {
            As(mod, () => Markers.Set(a)),
        };
        FakeMarkers.Dispatch();
        results.Add(As(mod, () => Markers.Set(a)));
        FakeMarkers.Dispatch();
        results.Add(As(mod, () => Markers.Set(b)));
        FakeMarkers.Dispatch();
        results.Add(As(mod, () => Markers.Remove("gate")));
        FakeMarkers.Dispatch();
        results.Add(As(mod, () => Markers.Remove("gate")));
        FakeMarkers.Dispatch();
        results.Add(As(mod, Markers.Clear));
        FakeMarkers.Dispatch();
        Assert.Equal(new MarkerResult(true, null, true, 0, 0, 1), results[0]);
        Assert.Equal(new MarkerResult(true, null, true, 0, 0, 0), results[1]);  // identical: no change
        Assert.Equal(new MarkerResult(true, null, true, 0, 0, 1), results[2]);
        Assert.Equal(new MarkerResult(true, null, true, 0, 0, 1), results[3]);
        Assert.False(results[4].Ok);
        Assert.False(results[4].Committed);
        Assert.StartsWith("NotFound", results[4].Reason);
        Assert.Equal(new MarkerResult(true, null, true, 0, 0, 0), results[5]);  // empty clear
        Assert.Equal([MarkerChangeKind.Added, MarkerChangeKind.Updated, MarkerChangeKind.Removed], seen.Select(e => e.Kind));
        var added = seen[0];
        Assert.Equal(("m01.mod", "gate", MarkerSource.Local, EntityId.None, 7ul, 1, "Api"),
            (added.Owner, added.Id, added.Audience, added.Recipient, added.WorldEpoch, added.Affected, added.Cause));
        Assert.True(seen[0].Sequence < seen[1].Sequence && seen[1].Sequence < seen[2].Sequence);
    }

    [Fact]
    public void M14_a_change_made_in_a_handler_arrives_with_the_next_step()
    {
        var mod = Mod("m14.mod", 77);
        var seen = new List<string>();
        As(mod, () => Markers.Changed += e =>
        {
            seen.Add(e.Kind + " " + e.Id);
            if (e.Id == "first") Markers.Set(new MarkerSpec("second", Gate, "", MarkerIcon.None, MarkerSurface.World));  // runs as the mod
        });
        As(mod, () => Markers.Set(new MarkerSpec("first", Gate, "", MarkerIcon.None, MarkerSurface.World)));
        As(mod, () => Markers.Remove("first"));
        FakeMarkers.Dispatch();
        Assert.Equal(["Added first", "Removed first"], seen);
        FakeMarkers.Dispatch();
        Assert.Equal(["Added first", "Removed first", "Added second"], seen);
        As(mod, Markers.Clear);
        FakeMarkers.Dispatch();
        Assert.Equal("Cleared ", seen[^1]);
    }

    [Fact]
    public void The_error_rule_bad_argument_capability_off_refusal()
    {
        var mod = Mod("rule.mod", 77);
        var spec = new MarkerSpec("k", Gate, "t", MarkerIcon.None, MarkerSurface.Compass);
        FakeMarkers.Next = (1, 0, "priority must be in [-32768, 32767]");
        var bad = Assert.Throws<ArgumentException>(() => As(mod, () => Markers.Set(spec)));
        Assert.Contains("priority", bad.Message);
        FakeMarkers.Next = (3, 0, "markers.compass");
        var off = Assert.Throws<KilnUnavailableException>(() => As(mod, () => Markers.Set(spec)));
        Assert.Equal("markers.compass", off.Capability);
        FakeMarkers.Next = (3, 0, "");
        Assert.Equal("markers.local", Assert.Throws<KilnUnavailableException>(() => As(mod, () => Markers.Set(spec))).Capability);
        FakeMarkers.Next = (0, 2, "Full: namespace 'rule.mod' has 128 local markers (the most)");
        var full = As(mod, () => Markers.Set(spec));
        Assert.Equal(new MarkerResult(false, "Full: namespace 'rule.mod' has 128 local markers (the most)", false, 0, 0, 0), full);
        FakeMarkers.Next = (0, 4, "NotReady: there is no client world");
        Assert.StartsWith("NotReady", As(mod, Markers.Clear).Reason);
        FakeMarkers.Next = (0, 7, "CatalogNotReady: the icon catalog is still warming up");
        Assert.Equal("CatalogNotReady: the icon catalog is still warming up", As(mod, () => Markers.Set(spec)).Reason);
        Assert.Throws<ArgumentNullException>(() => As(mod, () => Markers.Set(null!)));
        Assert.Throws<ArgumentNullException>(() => As(mod, () => Markers.Set(spec with { Id = null! })));
        Assert.Throws<ArgumentNullException>(() => As(mod, () => Markers.Set(spec with { Text = null! })));
        Assert.Throws<ArgumentNullException>(() => As(mod, () => Markers.Remove(null!)));
        Assert.IsAssignableFrom<IKilnResult>(full);
    }

    [Fact]
    public void Outside_a_mod_or_after_its_unload_is_an_invalid_operation()
    {
        var spec = new MarkerSpec("k", Gate, "t", MarkerIcon.None, MarkerSurface.Compass);
        Assert.Throws<InvalidOperationException>(() => As(null, () => Markers.Set(spec)));
        var gone = Mod("gone.mod", 77);
        gone.Retire();
        Assert.Throws<InvalidOperationException>(() => As(gone, () => Markers.Set(spec)));
        var noCore = Mod("no.core", 0);
        Assert.Throws<InvalidOperationException>(() => As(noCore, () => Markers.Set(spec)));
    }

    [Fact]
    public void A_refused_binding_is_a_refusal_not_an_exception()
    {
        var spec = new MarkerSpec("k", Gate, "t", MarkerIcon.None, MarkerSurface.Compass);
        FakeMarkers.BindAnswer = 1;
        var inUse = As(Mod("taken.mod", 77), () => Markers.Set(spec));
        Assert.False(inUse.Ok);
        Assert.StartsWith("NamespaceInUse", inUse.Reason);
        FakeMarkers.BindAnswer = 5;
        var full = As(Mod("late.mod", 78), () => Markers.Set(spec));
        Assert.StartsWith("Full", full.Reason);
        Assert.DoesNotContain(FakeMarkers.Calls, c => c.StartsWith("set", StringComparison.Ordinal));
    }

    [Fact]
    public void Unloading_a_mod_releases_its_records_and_drops_its_handlers()
    {
        var mod = Mod("leaving.mod", 77);
        var other = Mod("staying.mod", 78);
        var mine = new List<MarkerChangedEvent>();
        var theirs = new List<MarkerChangedEvent>();
        As(mod, () => Markers.Changed += mine.Add);
        As(other, () => Markers.Changed += theirs.Add);
        As(mod, () => Markers.Set(new MarkerSpec("a", Gate, "", MarkerIcon.None, MarkerSurface.World)));
        As(mod, () => Markers.Set(new MarkerSpec("b", Gate, "", MarkerIcon.None, MarkerSurface.World)));
        FakeMarkers.Dispatch();
        Assert.Equal(2, mine.Count);
        mod.Retire();
        Assert.Contains(77ul, FakeMarkers.Released);
        FakeMarkers.Dispatch();
        Assert.Equal(2, mine.Count);  // its handler went with it
        var cleared = theirs[^1];
        Assert.Equal((MarkerChangeKind.Cleared, "leaving.mod", 2, "OwnerRetired"), (cleared.Kind, cleared.Owner, cleared.Affected, cleared.Cause));
        // a new instance of the same mod binds again
        var again = Mod("leaving.mod", 79);
        Assert.True(As(again, () => Markers.Set(new MarkerSpec("a", Gate, "", MarkerIcon.None, MarkerSurface.World))).Ok);
        Assert.Contains("bind 79 leaving.mod", FakeMarkers.Calls);
    }

    [Fact]
    public void A_handler_that_throws_is_removed_and_the_others_still_run()
    {
        var mod = Mod("noisy.mod", 77);
        var calls = 0;
        As(mod, () => Markers.Changed += _ => throw new InvalidOperationException("boom"));
        Markers.Changed += _ => calls++;  // Kiln's own (no mod)
        As(mod, () => Markers.Set(new MarkerSpec("a", Gate, "", MarkerIcon.None, MarkerSurface.World)));
        As(mod, () => Markers.Set(new MarkerSpec("b", Gate, "", MarkerIcon.None, MarkerSurface.World)));
        FakeMarkers.Dispatch();
        Assert.Equal(2, calls);
        Assert.Single(Backend.Lines, l => l.Contains("Markers.Changed handler threw and was removed", StringComparison.Ordinal));
        Assert.Equal(1, FakeMarkers.Subscribes);  // one native subscription for the process
        Assert.Equal(4242ul, FakeMarkers.SubscribedOwner);  // Kiln's own owner, not a mod's
    }

    [Fact]
    public void A_disposed_follow_lease_is_refused_and_a_live_one_is_passed()
    {
        var mod = Mod("follow.mod", 77);
        var lease = new MarkerAnchor(0x1234, 77, new EntityId(2022), 2, 5);
        var spec = new MarkerSpec("f", Gate, "", MarkerIcon.None, MarkerSurface.World, Follow: lease);
        As(mod, () => Markers.Set(spec));
        Assert.Equal(0x1234ul, FakeMarkers.Specs[^1].Follow);
        lease.Dispose();
        lease.Dispose();  // idempotent
        Assert.Single(FakeMarkers.Calls, c => c == "release_anchor 77 4660");
        Assert.Throws<ArgumentException>(() => As(mod, () => Markers.Set(spec)));
    }
}
