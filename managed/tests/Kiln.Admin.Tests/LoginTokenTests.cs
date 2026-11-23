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
}
