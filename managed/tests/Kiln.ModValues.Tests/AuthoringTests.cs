// SPDX-License-Identifier: MIT
// mod-values/*.json: the chapter's example reads, gives the same normalized definition as the same options in C#, and
// author errors are refused.
using Kiln;
using Xunit;

namespace ModValuesTests;

public sealed class AuthoringTests
{
    internal const string Example = """
        {"format":1,"values":{
         "xp":{"kind":"experience","label":{"en":"Fishing XP","de":"Angel-EP"},
               "numberFormat":{"*":"N0"},"min":"0","max":"9223372036854775807",
               "initial":"0","scale":0,"thresholds":["0","100","400","900"],
               "persistence":"world","trustClientImports":false,"version":1,
               "carryEpoch":1,"readPolicy":"public","readMods":[],"sync":"readers",
               "allowPartitionMove":false,"resetOnKindChange":false},
         "tokens":{"kind":"currency","label":"Tokens","numberFormat":"N0",
                   "min":"0","max":"1000000","initial":"10","scale":0,
                   "persistence":"world","readPolicy":"ownerOnly","sync":"ownerMod"}}}
        """;

    [Fact]
    public void TheChapterExampleMatchesTheSameOptionsInCSharp()
    {
        var defs = ModValuesAuthoring.Parse("fishing", Example, "example.json");
        Assert.Equal(["xp", "tokens"], defs.Select(d => d.Id));
        var xp = Def.FromOptions("fishing", "xp", new ModValueOptions(ModValueKind.Experience, UiText.Of(("en", "Fishing XP"), ("de", "Angel-EP")),
            Initial: 0, Thresholds: [0, 100, 400, 900], NumberFormat: new UiText("N0"), ReadPolicy: ModValueReadPolicy.Public, Sync: ModValueSyncPolicy.Readers));
        var tokens = Def.FromOptions("fishing", "tokens", new ModValueOptions(ModValueKind.Currency, "Tokens", Max: 1_000_000, Initial: 10, NumberFormat: "N0"));
        Assert.Equal(xp.Canonical, defs[0].Canonical);
        Assert.Equal(tokens.Canonical, defs[1].Canonical);
        Assert.Equal(10, defs[1].Initial);
    }

    [Theory]
    [InlineData("""{"format":1,"values":{"a":{"kind":"counter","label":"A","colour":"red"}}}""", "unknown field")]
    [InlineData("""{"format":2,"values":{}}""", "format")]
    [InlineData("""{"format":1,"values":{"a":{"kind":"counter","label":"A","min":5}}}""", "base-10 string")]
    [InlineData("""{"format":1,"values":{"a":{"kind":"counter","label":"A","max":"9223372036854775808"}}}""", "base-10 string")]
    [InlineData("""{"format":1,"values":{"a":{"kind":"counter","label":"A","numberFormat":"D2"}}}""", "number format")]
    [InlineData("""{"format":1,"values":{"a":{"kind":"coins","label":"A"}}}""", "kind is one of")]
    [InlineData("""{"format":1,"values":{"a":{"kind":"counter","label":"A",}}}""", "not JSON")]
    [InlineData("""{"format":1,"values":{"a":{"kind":"counter","label":"A"},"a":{"kind":"counter","label":"B"}}}""", "twice")]
    [InlineData("""{"format":1,"values":{"Bad":{"kind":"counter","label":"A"}}}""", "not a mod value id")]
    [InlineData("""{"format":1,"values":{"a":{"kind":"currency","label":"A","persistence":"carried","trustClientImports":true}}}""", "never travels")]
    public void AuthorErrors(string json, string expected)
    {
        var e = Assert.Throws<ArgumentException>(() => ModValuesAuthoring.Parse("fishing", json, "bad.json"));
        Assert.Contains(expected, e.Message);
    }

    [Fact]
    public void BigNumbersStayExact()
    {
        var defs = ModValuesAuthoring.Parse("m", """{"format":1,"values":{"big":{"kind":"counter","label":"B","initial":"9007199254740993"}}}""", "b.json");
        Assert.Equal(9007199254740993, defs[0].Initial);
        Assert.Equal("9,007,199,254,740,993", ModValueFormat.Format(defs[0].Initial, 0, "N0", "en-US"));
    }

    [Fact]
    public void FilesOfAModAreReadInNameOrderAndIdsMustDiffer()
    {
        var dir = Directory.CreateTempSubdirectory("mv-").FullName;
        try
        {
            Directory.CreateDirectory(Path.Combine(dir, "mod-values"));
            File.WriteAllText(Path.Combine(dir, "mod-values", "a.json"), Example);
            File.WriteAllText(Path.Combine(dir, "mod-values", "b.json"), """{"format":1,"values":{"xp":{"kind":"counter","label":"again"},"other":{"kind":"counter","label":"O"}}}""");
            var errors = new List<string>();
            var defs = ModValuesAuthoring.ReadMod("fishing", dir, errors.Add);
            Assert.Equal(["xp", "tokens"], defs.Select(d => d.Id));
            Assert.Contains("another file", Assert.Single(errors));
        }
        finally
        {
            Directory.Delete(dir, true);
        }
    }
}
