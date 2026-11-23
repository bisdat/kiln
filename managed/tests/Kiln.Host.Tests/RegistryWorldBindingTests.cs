// SPDX-License-Identifier: MIT
#pragma warning disable KILNX031
using Kiln;
using Kiln.Host;
using Xunit;

namespace Kiln.Host.Tests;

public sealed class RegistryWorldBindingTests
{
    [Fact]
    public void EitherReadinessOrderWaitsForBothInputs()
    {
        var binding = new RegistryWorldBinding();
        var world = new RegistryWorld(1, WorldKind.Server, "a");
        Assert.False(binding.Bind(world));
        binding.Signal("a"); Assert.True(binding.Bind(world));
        Assert.True(binding.Matches(world));
        var other = new RegistryWorldBinding();
        other.Signal("a"); Assert.False(other.Bind(null)); Assert.True(other.Bind(world));
    }
    [Fact]
    public void ANewSaveIdCannotBindToTheOldNativeWorld()
    {
        var binding = new RegistryWorldBinding(); binding.Signal("a"); binding.Bind(new(1, WorldKind.Server, "a"));
        binding.Signal("b");
        Assert.False(binding.Bind(new(1, WorldKind.Server, "b")));
        Assert.False(binding.Matches(new(1, WorldKind.Server, "b")));
        Assert.True(binding.Bind(new(2, WorldKind.Server, "b")));
    }
    [Fact]
    public void ANewNativeWorldCannotReuseAnUnannouncedOldSave()
    {
        var binding = new RegistryWorldBinding(); binding.Signal("a"); binding.Bind(new(1, WorldKind.Server, "a"));
        Assert.False(binding.Bind(new(2, WorldKind.Server, "a")));
        binding.Signal(null); Assert.False(binding.Bind(new(2, WorldKind.Server, "a")));
        binding.Signal("a"); Assert.True(binding.Bind(new(2, WorldKind.Server, "a"))); // reload of the same save
    }
}
