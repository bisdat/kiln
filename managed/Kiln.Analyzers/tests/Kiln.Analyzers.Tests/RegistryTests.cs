// SPDX-License-Identifier: MIT
using Xunit;
using static Kiln.Analyzers.Tests.Harness;

namespace Kiln.Analyzers.Tests;

public sealed class RegistryTests
{
    private static readonly RegistryAnalyzer Analyzer = new();
    [Fact]
    public async Task DeclaringPointsWithoutARealOverrideWarns()
    {
        Ids(await Run(Analyzer, """
            #pragma warning disable KILNX031
            [RegistryEntryPoints(RegistryEntryPoint.WorldLoad)]
            public class M : Mod { public new void OnRegistry(RegistryContext context) { } }
            """), "KILN122");
    }
    [Theory]
    [InlineData("0")]
    [InlineData("16")]
    [InlineData("32")]
    [InlineData("-1")]
    [InlineData("17")]
    public async Task InvalidAutomaticFlagsWarn(string flags)
    {
        Ids(await Run(Analyzer, $$"""
            #pragma warning disable KILNX031
            [RegistryEntryPoints((RegistryEntryPoint)({{flags}}))]
            public class M : Mod { public override void OnRegistry(RegistryContext context) { } }
            """), "KILN123");
    }
    [Fact]
    public async Task AnAbstractDeclarationCanBeImplementedByTheConcreteMod()
    {
        Ids(await Run(Analyzer, """
            #pragma warning disable KILNX031
            [RegistryEntryPoints(RegistryEntryPoint.WorldLoad)]
            public abstract class Base : Mod { }
            public class M : Base { public override void OnRegistry(RegistryContext context) { } }
            """));
    }
    [Fact]
    public async Task AConcreteModWithoutAnOverrideIsCheckedAgainstItsInheritedDeclaration()
    {
        var diagnostics = await Run(Analyzer, """
            #pragma warning disable KILNX031
            [RegistryEntryPoints(RegistryEntryPoint.WorldLoad)]
            public abstract class Base : Mod { }
            public class ConcreteAuthor : Base { }
            """);
        Ids(diagnostics, "KILN122");
        Assert.Contains("ConcreteAuthor", Assert.Single(diagnostics).GetMessage());
    }
    [Fact]
    public async Task DefaultAndInheritedOverridesAndValidCombinationsAreFine()
    {
        Ids(await Run(Analyzer, """
            #pragma warning disable KILNX031
            public abstract class Base : Mod { public override void OnRegistry(RegistryContext context) { } }
            [RegistryEntryPoints(RegistryEntryPoint.WorldLoad | RegistryEntryPoint.ModLoad | RegistryEntryPoint.HotReload)]
            public class M : Base { }
            public class Default : Mod { public override void OnRegistry(RegistryContext context) { } }
            """));
    }
}
