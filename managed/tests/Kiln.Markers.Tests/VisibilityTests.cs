// SPDX-License-Identifier: MIT
// Markers.VisibilityChanged as the wrapper delivers the native payload (chapter 04 §4.6); the admission rules themselves
// are tested natively (native/services/markers/tests/surfaces_test.cpp).
using System.Text;
using Kiln;
using MarkersTests;
using Xunit;

namespace MarkersTests;

public sealed unsafe class VisibilityTests : MarkersFixture
{
    private static void Raise(string owner, string key, uint requested, uint before, uint after, uint map, uint compass, uint world,
        bool initial = false, uint audience = 0, ulong peer = 0)
    {
        var v = new MarkersNative.Visibility
        {
            Size = (uint)sizeof(MarkersNative.Visibility), Audience = audience, Initial = initial ? 1u : 0u, WorldEpoch = 7,
            PeerGeneration = peer, PublisherGeneration = 3, Sequence = 11, Requested = requested, Before = before, After = after,
        };
        v.Reason[0] = map;
        v.Reason[1] = compass;
        v.Reason[2] = world;
        var o = Encoding.UTF8.GetBytes(owner);
        var k = Encoding.UTF8.GetBytes(key);
        for (var i = 0; i < o.Length; i++) v.Owner[i] = o[i];
        for (var i = 0; i < k.Length; i++) v.Key[i] = k[i];
        var fn = (delegate* unmanaged[Cdecl]<MarkersNative.Visibility*, void*, void>)FakeMarkers.VisibilityFn;
        Assert.True(fn != null, "Kiln.Markers subscribed without a visibility callback");
        fn(&v, (void*)FakeMarkers.ChangeUser);
    }

    [Fact]
    public void A_visibility_payload_reaches_the_handler_as_its_mod()
    {
        var mod = Mod("vis.mod", 77);
        var seen = new List<MarkerVisibilityChangedEvent>();
        ModContext? during = null;
        As(mod, () => Markers.VisibilityChanged += e =>
        {
            during = ModContext.CurrentMod;
            seen.Add(e);
        });
        Assert.Equal(1, FakeMarkers.Subscribes);
        Raise("vis.mod", "gate", 3, 0, 2, 1, 0, 13, initial: true);
        var e = Assert.Single(seen);
        Assert.Same(mod, during);
        Assert.Equal(("vis.mod", "gate", MarkerSource.Local, true, MarkerSurface.Map | MarkerSurface.Compass, (MarkerSurface)0, MarkerSurface.Compass),
            (e.Owner, e.Id, e.Audience, e.Initial, e.Requested, e.Before, e.After));
        Assert.Equal((MarkerVisibilityReason.MapCapacity, MarkerVisibilityReason.Ready, MarkerVisibilityReason.NotRequested), (e.Map, e.Compass, e.World));
        Assert.Equal((7ul, 3ul, 11ul, 0ul), (e.WorldEpoch, e.PublisherGeneration, e.Sequence, e.PeerGeneration));
    }

    [Fact]
    public void Every_native_reason_has_its_name_and_a_received_marker_keeps_its_link()
    {
        var mod = Mod("vis.mod", 77);
        var seen = new List<MarkerVisibilityChangedEvent>();
        As(mod, () => Markers.VisibilityChanged += seen.Add);
        for (uint r = 0; r <= 13; r++) Raise("srv", "k" + r, 4, 4, 0, 13, 13, r, audience: 3, peer: 5);
        Assert.Equal(Enum.GetValues<MarkerVisibilityReason>(), seen.Select(e => e.World));
        Assert.All(seen, e => Assert.Equal((MarkerSource.Received, 5ul), (e.Audience, e.PeerGeneration)));
        // a reason this build does not know is reported as the capability being off, never as Ready
        Raise("srv", "x", 4, 0, 0, 13, 13, 99);
        Assert.Equal(MarkerVisibilityReason.CapabilityOff, seen[^1].World);
    }

    [Fact]
    public void A_throwing_handler_is_removed_and_an_unloaded_mod_hears_nothing()
    {
        var mod = Mod("vis.mod", 77);
        var other = Mod("other.mod", 78);
        var calls = 0;
        As(mod, () => Markers.VisibilityChanged += _ => throw new InvalidOperationException("boom"));
        As(other, () => Markers.VisibilityChanged += _ => calls++);
        Raise("vis.mod", "a", 1, 0, 1, 0, 13, 13);
        Raise("vis.mod", "a", 1, 1, 0, 1, 13, 13);
        Assert.Equal(2, calls);
        Assert.Single(Backend.Lines, l => l.Contains("Markers.VisibilityChanged handler threw and was removed", StringComparison.Ordinal));
        other.Retire();
        Raise("vis.mod", "a", 1, 0, 1, 0, 13, 13);
        Assert.Equal(2, calls);
    }
}
