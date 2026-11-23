// SPDX-License-Identifier: MIT
using Xunit;
namespace Kiln;

public sealed class StorageOfferWindowTests
{
    [Fact]
    public void Offer_before_deadline_decides_once_and_hosted_uses_it_immediately()
    {
        var window = new StorageOfferWindow(10);
        Assert.False(window.Poll(11, false));
        Assert.True(window.Offer());
        Assert.True(window.Decided);
        Assert.False(window.Offer());
        Assert.False(window.Poll(1000, false));
    }

    [Fact]
    public void Timeout_decides_server_only_and_late_offer_cannot_swap_live_storage()
    {
        var window = new StorageOfferWindow(10);
        Assert.False(window.Poll(10 + StorageOfferWindow.TimeoutSteps - 1, false));
        Assert.True(window.Poll(10 + StorageOfferWindow.TimeoutSteps, false));
        Assert.True(window.TimedOut);
        Assert.False(window.Offer());
    }

    [Fact]
    public void Vanilla_decides_without_waiting()
    {
        var window = new StorageOfferWindow(10);
        Assert.True(window.Poll(10, true));
        Assert.False(window.TimedOut);
    }
}
