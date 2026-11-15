// SPDX-License-Identifier: MIT
using Xunit;
using static Kiln.Blocks.Tests.RawGateTests;

namespace Kiln.Blocks.Tests;

/// <summary>Typed native declarations (since 0.2.0-dev.29, <see cref="Abilities.Natives.DeclareTyped"/>):
/// the refusals that need no game. The verifier rule and the per-call check are the core's
/// (native/services/abilities/tests).</summary>
[Collection("raw gates")]
public sealed class NativesTypedTests
{
    [Fact]
    public void BadDeclarationsAreArgumentExceptionsBeforeTheCoreIsAsked()
    {
        As(Plain, () =>
        {
            Assert.Throws<ArgumentException>(() => Abilities.Natives.DeclareTyped("", new NativeArg("sint32")));
            Assert.Throws<ArgumentNullException>(() => Abilities.Natives.DeclareTyped("plain-mod.X", (NativeArg[])null!));
            Assert.Throws<ArgumentException>(() => Abilities.Natives.DeclareTyped("NoDot", new NativeArg("sint32")));
            Assert.Throws<ArgumentException>(() => Abilities.Natives.DeclareTyped("plain-mod.X", new NativeArg("")));
            Assert.Throws<ArgumentException>(() => Abilities.Natives.DeclareTyped("plain-mod.X", new NativeArg("sint32", Size: 70000)));
            Assert.Throws<ArgumentException>(() => Abilities.Natives.DeclareTyped("plain-mod.X", Enumerable.Repeat(new NativeArg("bool"), 32).ToArray()));
            // a good declaration reaches the capability: off in a test process, named
            var off = Assert.Throws<KilnUnavailableException>(() =>
                Abilities.Natives.DeclareTyped("plain-mod.X", new NativeArg("sint32", NativeArgDirection.Out), new NativeArg("any", Raw: true)));
            Assert.Equal("abilities.natives.sync", off.Capability);
        });
    }

    [Fact]
    public void KnownTypeSizesAreTheCoresAndTheTypeTables()
    {
        // tools/impact/data/types.json; native/services/abilities/natives_json.h impact_type_size
        foreach (var (type, size) in new[] { ("sint32", 4), ("uint32", 4), ("float32", 4), ("bool", 1), ("Time", 8),
                     ("ecs.EntityId", 4), ("WorldTransform", 56), ("float3", 12), ("impact.DamageSetup", 52), ("AttributeReference", 0) })
            Assert.Equal(size, Abilities.Natives.ImpactTypeSize(type));
        Assert.Equal(1, (int)NativeArgDirection.Out);
        Assert.Equal(2, (int)NativeArgDirection.InOut);
    }
}
