// SPDX-License-Identifier: MIT
// The kit does not fake kiln.client's building input or kiln.net's Steam peer messages (0.2.0-dev.30.3): both rows are
// offline, the members throw KilnUnavailableException naming them, and the coverage table says so.
using Xunit;

namespace Kiln.TestKit.Tests;

public sealed class ClientInputSteamOfflineTests
{
    [Fact]
    public void Building_input_is_offline_in_the_kit_and_throws_naming_its_row()
    {
        using var kiln = new KilnHarness();
        Assert.False(BuildingInput.Available);
        Assert.Equal(CapReason.Offline, KitBackend.Instance.Capabilities.Get(BuildingInput.Capability).Reason);
        Assert.Contains("unavailable (offline)", kiln.Capabilities.Why(BuildingInput.Capability));
        var e = Assert.Throws<KilnUnavailableException>(() => BuildingInput.Undo());
        Assert.Equal(BuildingInput.Capability, e.Capability);
        Assert.Throws<InvalidOperationException>(() => kiln.Capabilities.Override(BuildingInput.Capability, true));
        Assert.Equal(CoverageKind.Unavailable, Assert.Single(Coverage.All, r => r.Capabilities.Contains(BuildingInput.Capability)).Kind);
    }

    [Fact]
    public void Steam_messages_are_offline_in_the_kit_and_Open_throws_naming_its_row()
    {
        using var kiln = new KilnHarness();
        Assert.False(SteamMessages.Available);
        Assert.False(SteamMessages.Status.Answered);
        Assert.Equal(CapReason.Offline, KitBackend.Instance.Capabilities.Get(SteamMessages.Capability).Reason);
        var e = Assert.Throws<KilnUnavailableException>(() => SteamMessages.Open(1, 2));
        Assert.Equal(SteamMessages.Capability, e.Capability);
        Assert.Throws<InvalidOperationException>(() => kiln.Capabilities.Override(SteamMessages.Capability, true));
        Assert.Equal(CoverageKind.Unavailable, Assert.Single(Coverage.All, r => r.Capabilities.Contains(SteamMessages.Capability)).Kind);
    }
}
