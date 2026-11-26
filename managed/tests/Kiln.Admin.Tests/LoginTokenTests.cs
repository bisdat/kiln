// SPDX-License-Identifier: MIT
using Xunit;

namespace Kiln.Admin.Tests;

public sealed class LoginTokenTests
{
    [Fact]
    public void A_secret_expires_after_60_seconds()
    {
        long now = 0;
        var t = new LoginTokens(() => now);
        var (s, _) = t.Mint("kiln-webadmin", null, "launcher");
        now += LoginTokens.TtlMs - 1;
        var (s2, _) = t.Mint("kiln-webadmin", null, "launcher");
        Assert.NotNull(t.Redeem("kiln-webadmin", s2));
        now += 1;
        Assert.Null(t.Redeem("kiln-webadmin", s));
        Assert.Equal(0, t.Live);
    }

    [Fact]
    public void Secrets_are_32_random_bytes_and_a_consumer_keeps_at_most_8()
    {
        var t = new LoginTokens(() => 0);
        var all = Enumerable.Range(0, 10).Select(_ => t.Mint("c", null, "x").Secret).ToList();
        Assert.Equal(10, all.Distinct().Count());
        Assert.All(all, s => Assert.Equal(64, s.Length));
        Assert.Equal(LoginTokens.MaxPerConsumer, t.Live);
        Assert.Null(t.Redeem("c", all[0]));  // the oldest went
        Assert.NotNull(t.Redeem("c", all[9]));
        Assert.Null(t.Redeem("c", "not hex"));
        Assert.Throws<AdminError>(() => t.Mint("Bad Name", null, "x"));
    }

    // The web panel's rename (0.2.0-dev.31.14): launchers 31.2 to 31.13 mint for kiln-webadmin, newer ones for the id of
    // the panel that listens. Kilnwarden redeems for kilnwarden and, refused, once more for kiln-webadmin: a refusal for
    // another consumer must leave the secret live for its own.
    [Fact]
    public void A_secret_refused_for_another_consumer_stays_live_for_its_own()
    {
        var t = new LoginTokens(() => 0);
        Assert.True(LoginTokens.ValidConsumer("kilnwarden"));
        var (old, _) = t.Mint("kiln-webadmin", "Kiln Launcher", "kiln-launcher/0.2.0");
        Assert.Null(t.Redeem("kilnwarden", old));
        Assert.Equal("kiln-webadmin", t.Redeem("kiln-webadmin", old)?.Consumer);
        var (now, _) = t.Mint("kilnwarden", "Kiln Launcher", "kiln-launcher/0.2.0");
        Assert.Null(t.Redeem("kiln-webadmin", now));
        Assert.Equal("kilnwarden", t.Redeem("kilnwarden", now)?.Consumer);
        Assert.Equal(0, t.Live);
    }
}
