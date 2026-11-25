// SPDX-License-Identifier: MIT
// What kiln.log says when a world write completes before its player storage is on disk (DurabilityLag, 0.2.0-dev.31.5): a
// slow writer is a delay, not an error. One player's kiln.log had an [E] "persistence degraded" line at nearly every 30 s
// autosave for two hours (204 of them) while Kiln.Saves' writer was only slow (its renames took 6-25 s) and always
// caught up; the launcher counted every one as an error.
using Xunit;

namespace Kiln;

public sealed class DurabilityLagTests
{
    private TimeSpan _now = TimeSpan.FromHours(3);
    private readonly DurabilityLag _lag;

    public DurabilityLagTests() => _lag = new DurabilityLag(() => _now);

    [Fact]
    public void The_first_delay_of_a_session_is_a_warning_and_later_ones_are_counted()
    {
        var first = _lag.Late(10);
        Assert.Equal(LogLevel.Warn, first!.Value.Level);
        Assert.StartsWith("player-storage: world generation 10 was written before its storage reached the disk", first.Value.Text);
        Assert.Contains("a delay, not an error", first.Value.Text);
        Assert.Contains("pairs kept in memory, next save retries", first.Value.Text);
        _now += TimeSpan.FromSeconds(30);
        Assert.Null(_lag.Late(11));
        Assert.True(_lag.IsLate(10) && _lag.IsLate(11));
    }

    [Fact]
    public void Its_first_catch_up_is_said_and_later_ones_are_counted_into_the_summary()
    {
        _lag.Late(10);
        _now += TimeSpan.FromSeconds(14);
        var caught = _lag.Landed(10, success: true);
        Assert.Equal(LogLevel.Info, caught!.Value.Level);
        Assert.StartsWith("player-storage: generation 10's storage reached the disk 14.0 s after its world write", caught.Value.Text);
        Assert.False(_lag.IsLate(10));
        _now += TimeSpan.FromSeconds(16);
        Assert.Null(_lag.Late(11));
        _now += TimeSpan.FromSeconds(20.5);
        Assert.Null(_lag.Landed(11, success: true));
        _now += TimeSpan.FromSeconds(10);
        Assert.Null(_lag.Late(12));
        Assert.Null(_lag.Summary());                                   // under 5 minutes since the last line
        _now = TimeSpan.FromHours(3) + TimeSpan.FromSeconds(14) + TimeSpan.FromMinutes(5);
        var summary = _lag.Summary();
        Assert.Equal(LogLevel.Warn, summary!.Value.Level);
        Assert.Equal("player-storage: in the last 5 min, 2 more world saves were written before their storage reached the disk; 1 reached it " +
                     "later, at worst 20.5 s after its world write; 1 not yet (Kiln.Saves' writer is slow, not failing: a delay, not an error; " +
                     "summed up at most every 5 minutes)", summary.Value.Text);
        Assert.Null(_lag.Summary());                                   // counting starts from zero
        _now += TimeSpan.FromHours(1);
        Assert.Null(_lag.Summary());                                   // nothing new: nothing to say
    }

    [Fact]
    public void A_staging_that_failed_is_not_a_catch_up_and_is_left_to_the_caller_as_an_error()
    {
        _lag.Late(10);
        Assert.Null(_lag.Landed(10, success: false));
        Assert.False(_lag.IsLate(10));
        Assert.Null(_lag.Landed(99, success: true));                   // never late: nothing to say
        _now += TimeSpan.FromMinutes(10);
        Assert.Null(_lag.Summary());
    }

    [Fact]
    public void A_staging_still_not_on_disk_after_two_minutes_is_stuck_once_and_its_landing_is_said()
    {
        _lag.Late(10);
        _now += TimeSpan.FromSeconds(119);
        Assert.Empty(_lag.Stuck());
        _now += TimeSpan.FromSeconds(2);
        var stuck = Assert.Single(_lag.Stuck());
        Assert.Equal(10u, stuck.Generation);
        Assert.Equal(121, stuck.Age.TotalSeconds, 3);
        Assert.Empty(_lag.Stuck());                                    // once
        _now += TimeSpan.FromSeconds(60);
        var landed = _lag.Landed(10, success: true);
        Assert.Equal(LogLevel.Info, landed!.Value.Level);
        Assert.Equal("player-storage: generation 10's storage reached the disk 181.0 s after its world write (reported above as not on disk)", landed.Value.Text);
    }

    [Fact]
    public void A_new_world_session_says_its_first_ones_again()
    {
        _lag.Late(10);
        _lag.Landed(10, true);
        _lag.Late(11);
        _lag.Clear();
        Assert.False(_lag.IsLate(11));
        Assert.Equal(LogLevel.Warn, _lag.Late(20)!.Value.Level);
        Assert.Equal(LogLevel.Info, _lag.Landed(20, true)!.Value.Level);
        _now += TimeSpan.FromHours(1);
        Assert.Null(_lag.Summary());                                   // the old session's counts went with it
    }

    [Fact]
    public void Late_generations_stay_bounded()
    {
        for (uint g = 1; g <= 50; g++) _lag.Late(g);
        Assert.False(_lag.IsLate(1));
        Assert.True(_lag.IsLate(50));
    }
}
