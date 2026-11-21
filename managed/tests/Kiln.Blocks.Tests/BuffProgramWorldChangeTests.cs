// SPDX-License-Identifier: MIT
using Xunit;

namespace Kiln.Blocks.Tests;

/// <summary>#108 offline: ApplyBuff's and RemoveBuff's programs across a world change, on a stand-in for kiln.abilities'
/// contract (it keeps Kiln's programs, the game's ImpactRegistry does not; a second registration puts a program back).
/// The native side is native/services/abilities/tests/programs_test.cpp.</summary>
public sealed class BuffProgramWorldChangeTests
{
    // kiln.abilities as the managed side sees it: Programs.Info answers from the live registry, Programs.Register
    // succeeds for a program Kiln has already (it goes back in) and waits while the world's registry is loading.
    private sealed class Service
    {
        public readonly HashSet<string> Kept = [], Live = [];
        public bool Loading;
        public int Registrations;

        public bool Info(string p) => Live.Contains(p);

        public bool Register(string p)
        {
            Registrations++;
            Kept.Add(p);
            if (Loading) return true;
            Live.Add(p);
            return false;
        }

        public void ChangeWorld(bool loading = false)
        {
            Live.Clear();
            Loading = loading;
        }
    }

    private static void Ensure(Service s, string program, string api) =>
        Abilities.EnsureProgram(() => s.Info(program), () => s.Register(program), api);

    [Fact]
    public void AfterAWorldChangeApplyBuffAndRemoveBuffRegisterTheirProgramsAgainWithoutAnError()
    {
        var s = new Service();
        Ensure(s, "buff", "ApplyBuff");
        Ensure(s, "remove", "RemoveBuff");
        Ensure(s, "buff", "ApplyBuff");  // in the registry: no second registration
        Assert.Equal(2, s.Registrations);

        s.ChangeWorld();  // another world, another character, the same game session
        Ensure(s, "buff", "ApplyBuff");
        Ensure(s, "remove", "RemoveBuff");
        Assert.Equal(4, s.Registrations);
        Assert.True(s.Info("buff") && s.Info("remove"));

        s.ChangeWorld();
        Ensure(s, "remove", "RemoveBuff");
        Assert.True(s.Info("remove") && !s.Info("buff"));  // each program when its call needs it
    }

    [Fact]
    public void WhileTheNextWorldLoadsTheCallSaysSoAndTheNextCallAfterItSucceeds()
    {
        var s = new Service();
        Ensure(s, "buff", "ApplyBuff");
        s.ChangeWorld(loading: true);
        var e = Assert.Throws<InvalidOperationException>(() => Ensure(s, "buff", "ApplyBuff"));
        Assert.Contains("ApplyBuff: the game's ImpactRegistry is not loaded yet", e.Message);
        s.Loading = false;
        Ensure(s, "buff", "ApplyBuff");
        Assert.True(s.Info("buff"));
    }

    [Fact]
    public void ARegistrationTheRegistryDoesNotShowIsAnErrorNotASilentSuccess()
    {
        var e = Assert.Throws<InvalidOperationException>(() =>
            Abilities.EnsureProgram(() => false, () => false, "RemoveBuff"));
        Assert.StartsWith("RemoveBuff:", e.Message);
    }
}
