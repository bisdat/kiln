// SPDX-License-Identifier: MIT
using Xunit;

namespace Kiln.Blocks.Tests;

/// <summary>The gear formulas agree between C# and native: Kiln.Content's GearMath (and Kiln.Blocks' Compiler, which forwards
/// to it) give, over the same 10,000 generated inputs, the numbers kiln.content's gear.cpp gives, the solver the registry's
/// formulas call (native/services/content/tests/registry_formulas_test.cpp pins the same hash). A change on one side only
/// fails one of the two tests.</summary>
public sealed class GearAgreementTests
{
    private const ulong VectorsHash = 0xa40975297674a479;

    private static ulong SplitMix(ref ulong s)
    {
        ulong z = s += 0x9e3779b97f4a7c15;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
        z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
        return z ^ (z >> 31);
    }

    private static void Fnv(ref ulong h, ReadOnlySpan<byte> b)
    {
        foreach (byte x in b)
            h = (h ^ x) * 0x100000001b3;
    }

    [Fact]
    public void GearMathGivesTheNativeSolversNumbers()
    {
        ulong s = 0x6b696c6e2d726567, h = 0xcbf29ce484222325;
        Span<byte> buf = stackalloc byte[8];
        for (int i = 0; i < 10000; i++)
        {
            int level = 1 + (int)(SplitMix(ref s) % 55);
            int power = (int)(SplitMix(ref s) % 2000);
            double speed = 0.5 + (SplitMix(ref s) % 200) / 100.0;
            double share = (SplitMix(ref s) % 1000) / 1000.0;
            int armor = (int)(SplitMix(ref s) % 500);
            int bp = Compiler.BasePower(level);
            Assert.Equal(GearMath.BasePower(level), bp);
            double dm = GearMath.DmgModFor(power, level, speed);
            int back = GearMath.PowerOf(dm, speed, level);
            int ar = GearMath.ArmorOf(share, level);
            float asf = (float)GearMath.ArmorShare(armor, level);
            BitConverter.TryWriteBytes(buf, bp); Fnv(ref h, buf[..4]);
            BitConverter.TryWriteBytes(buf, dm); Fnv(ref h, buf[..8]);
            BitConverter.TryWriteBytes(buf, back); Fnv(ref h, buf[..4]);
            BitConverter.TryWriteBytes(buf, ar); Fnv(ref h, buf[..4]);
            BitConverter.TryWriteBytes(buf, asf); Fnv(ref h, buf[..4]);
        }
        Assert.Equal(VectorsHash.ToString("x16"), h.ToString("x16"));
    }

    [Fact]
    public void CompilerForwardsToGearMath()
    {
        for (int level = 1; level <= 55; level++)
            for (int power = 0; power < 600; power += 7)
                Assert.Equal(GearMath.DmgModFor(power, level, 1.0), Compiler.DmgModFor(power, level));
    }
}
