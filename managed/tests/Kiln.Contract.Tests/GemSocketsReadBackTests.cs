// SPDX-License-Identifier: MIT
// Gem wave, the owner's real-client check (2026-11-16, a client hosting its world): Gems.AddSocket read the re-created
// ring back as "level 40, Common" where it was 42 and Epic. The re-creation was right: a new item entity is finished by
// the game in the world step after its transaction (lab gems-int probe, dedicated server: at delivery ItemState rarity
// Common, level 0, Level 40/55; one step later Epic, 42, 42/42). kiln.items delivers the result in a step's Pre, and the
// read-back was scheduled from a worker thread for "the next service tick", which could still be that same step's (the
// host's managed tick runs after kiln.items' native Pre): on the hosting client it read 4 ms after delivery, before the
// world stepped. The read-back waits until the world has run a whole step since the call.
using Xunit;

namespace Kiln.Contract.Tests;

public sealed class GemSocketsReadBackTests : ContractTest
{
    [Fact]
    public void AReadBackScheduledBeforeTheStepsServiceTickWaitsForTheNextStep()
    {
        Step(5);  // the last service tick was step 5; step 6's Pre delivers the result before step 6's service tick
        var ran = new List<ulong>();
        Gems.AfterWorldStep(() => ran.Add(ServiceTick.Step));
        Step();   // step 6: the world has not run a step since the delivery
        Assert.Empty(ran);
        Step();   // step 7: world step 6 is done
        Assert.Equal([7ul], ran);
        Step(3);
        Assert.Single(ran);
    }

    [Fact]
    public void AReadBackScheduledAfterTheStepsServiceTickRunsWithinTwoSteps()
    {
        Step(6);  // step 6's service tick already ran when the result's continuation scheduled the read
        var ran = new List<ulong>();
        Gems.AfterWorldStep(() => ran.Add(ServiceTick.Step));
        Step();   // 7
        Step();   // 8
        Assert.Single(ran);
        Assert.True(ran[0] is 7 or 8, $"ran at {ran[0]}");
    }

    [Fact]
    public void AWorldThatStartsAgainRunsAWaitingReadBackAtOnce()
    {
        Step(40);
        var ran = 0;
        Gems.AfterWorldStep(() => ran++);
        ServiceTick.Run(HostAccess.Tick(1, 2, 2, 0, 40.8));  // a new world: its steps count from 1
        Assert.Equal(1, ran);
    }
}
