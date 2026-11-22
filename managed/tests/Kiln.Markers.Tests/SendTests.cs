// SPDX-License-Identifier: MIT
// Markers.Send, Unsend, SuppressSent and ClearSent on a fake kiln.markers (chapter 04 §5): what each passes, and how the
// server layers' answers become MarkerResults (a partial delivery is Ok false with Committed, never an exception).
using Kiln;
using Xunit;

namespace MarkersTests;

public sealed class SendTests() : MarkersFixture
{
    private static readonly MarkerSpec Gate = new("gate", new WorldPosition(3670, 820, 1500), "Raid entrance", MarkerIcon.None,
        MarkerSurface.Compass | MarkerSurface.World);

    [Fact]
    public void Send_passes_the_recipient_and_the_spec_under_the_mods_namespace()
    {
        var mod = Mod("boss.mod", 11);
        var r = As(mod, () => Markers.Send(EntityId.None, Gate));
        Assert.True(r.Ok);
        Assert.True(r.Committed);
        Assert.Equal(1, r.Affected);
        Assert.Contains("bind 11 boss.mod", FakeMarkers.Calls);
        Assert.Contains("send 11 0 gate", FakeMarkers.Calls);
        var spec = FakeMarkers.Specs.Last();
        Assert.Equal(6u, spec.Surfaces);
        Assert.Equal("Raid entrance", spec.Text);
        Assert.Equal(3670, spec.X);
        As(mod, () => Markers.Send(new EntityId(7), Gate with { Id = "only_you" }));
        Assert.Contains("send 11 7 only_you", FakeMarkers.Calls);
    }

    [Fact]
    public void A_partial_delivery_is_not_ok_but_committed_with_its_counts()
    {
        var mod = Mod("boss.mod", 11);
        FakeMarkers.SendAnswer = (MarkersNative.Partial, 1, 1, 1, 1, "Partial: stored; 1 recipient(s) queued, 1 refused");
        var r = As(mod, () => Markers.Send(EntityId.None, Gate));
        Assert.False(r.Ok);
        Assert.True(r.Committed);
        Assert.Equal(1, r.Queued);
        Assert.Equal(1, r.Refused);
        Assert.StartsWith("Partial", r.Reason);
        FakeMarkers.SendAnswer = (MarkersNative.RequiresClient, 1, 0, 2, 0, "RequiresClient: stored; no recipient can show it now");
        r = As(mod, () => Markers.Send(EntityId.None, Gate));
        Assert.False(r.Ok);
        Assert.True(r.Committed);
        Assert.Equal(2, r.Refused);
        FakeMarkers.SendAnswer = (MarkersNative.StatusNotFound, 0, 0, 0, 0, "NotFound: player 9 is not connected");
        r = As(mod, () => Markers.Send(new EntityId(9), Gate));
        Assert.False(r.Ok);
        Assert.False(r.Committed);
        Assert.StartsWith("NotFound", r.Reason);
    }

    [Fact]
    public void The_error_rule_names_the_capability_and_the_argument()
    {
        var mod = Mod("boss.mod", 11);
        FakeMarkers.Next = (MarkersNative.Unavailable, 0, "markers.anchors.server");
        var e = Assert.Throws<KilnUnavailableException>(() => As(mod, () => Markers.Send(EntityId.None, Gate)));
        Assert.Equal("markers.anchors.server", e.Capability);
        FakeMarkers.Next = (MarkersNative.InvalidArgument, 0, "icon_type 0x00000999 is not in the icon catalog");
        var a = Assert.Throws<ArgumentException>(() => As(mod, () => Markers.Send(EntityId.None, Gate)));
        Assert.Contains("icon catalog", a.Message);
        FakeMarkers.Next = (MarkersNative.WrongThread, 0, "send runs on the server's tick thread");
        Assert.Throws<InvalidOperationException>(() => As(mod, () => Markers.Send(EntityId.None, Gate)));
        Assert.Throws<ArgumentNullException>(() => As(mod, () => Markers.Send(EntityId.None, null!)));
        Assert.Throws<ArgumentNullException>(() => As(mod, () => Markers.Unsend(EntityId.None, null!)));
        Assert.Throws<InvalidOperationException>(() => Markers.Send(EntityId.None, Gate));  // not from a mod
    }

    [Fact]
    public void Unsend_SuppressSent_and_ClearSent_pass_their_layer()
    {
        var mod = Mod("boss.mod", 11);
        Assert.True(As(mod, () => Markers.Unsend(EntityId.None, "gate")).Ok);
        Assert.True(As(mod, () => Markers.Unsend(new EntityId(4), "gate")).Ok);
        Assert.True(As(mod, () => Markers.SuppressSent(new EntityId(4), "gate")).Ok);
        Assert.True(As(mod, () => Markers.ClearSent(new EntityId(4))).Ok);
        Assert.True(As(mod, () => Markers.ClearSent()).Ok);
        Assert.Contains("unsend 11 0 gate", FakeMarkers.Calls);
        Assert.Contains("unsend 11 4 gate", FakeMarkers.Calls);
        Assert.Contains("suppress 11 4 gate", FakeMarkers.Calls);
        Assert.Contains("clear_sent 11 4", FakeMarkers.Calls);
        Assert.Contains("clear_sent 11 0", FakeMarkers.Calls);
        // suppression is per player: the broadcast is removed with Unsend
        var x = Assert.Throws<ArgumentException>(() => As(mod, () => Markers.SuppressSent(EntityId.None, "gate")));
        Assert.Equal("player", x.ParamName);
        FakeMarkers.SendAnswer = (MarkersNative.Accepted, 1, 0, 0, 0, "");
        var empty = As(mod, () => Markers.ClearSent());
        Assert.True(empty.Ok);
        Assert.Equal(0, empty.Affected);
    }
}

public sealed class SendWithoutTheServiceTests() : MarkersFixture(table: false)
{
    [Fact]
    public void Every_send_member_throws_naming_markers_send()
    {
        var mod = Mod("no.table", 77);
        var spec = new MarkerSpec("k", new WorldPosition(1, 2, 3), "t", MarkerIcon.None, MarkerSurface.World);
        foreach (var call in new Func<MarkerResult>[]
                 {
                     () => Markers.Send(EntityId.None, spec), () => Markers.Unsend(EntityId.None, "k"),
                     () => Markers.SuppressSent(new EntityId(3), "k"), () => Markers.ClearSent(),
                 })
        {
            var e = Assert.Throws<KilnUnavailableException>(() => As(mod, call));
            Assert.Equal("markers.send", e.Capability);
        }
    }
}
