// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Api.Tests;

public sealed class SteppedConfig
{
    [Range(-0.35, 1.05, 0.1)] public double Fraction { get; set; } = 0.123;
    [Range(3, 20, 4)] public int Count { get; set; } = 8;
    [Range(0, 1)] public double Legacy { get; set; } = 0.123;
    [Range(0, 1, double.NaN)] public double Invalid { get; set; } = 0.123;
    [Range(0, 1, 0)] public double Zero { get; set; }
    [Range(0, 1, -1)] public double Negative { get; set; }
    [Range(0, 1, double.PositiveInfinity)] public double Infinite { get; set; }
    [Range(0, 1, 2)] public double TooLarge { get; set; }
    [Range(0, 10, 0.5)] public int FractionalInt { get; set; }
    [Range(0.5, 10, 2)] public int FractionalIntMin { get; set; } = 1;
    [Range(1, 1, 1)] public double Fixed { get; set; } = 1;
    [Range(1e20, 2e20, 1)] public double TooSmall { get; set; } = 1e20;
    [Range(0, 10, 1)] public List<int> List { get; set; } = [3];
}

public sealed class SteppedCommands
{
    [Command("stepped")]
    public double Run([Range(-1, 1, 0.25)] double value) => value;
    [Command("badstep")]
    public double Invalid([Range(-1, 1, double.NaN)] double value) => value;
}

public sealed class ConfigRangeStepTests : KilnTest
{
    [Fact]
    public void The_range_and_entry_contract_is_additive()
    {
        Assert.NotNull(typeof(RangeAttribute).GetConstructor([typeof(double), typeof(double)]));
        Assert.Null(new RangeAttribute(-1, 1).Step);
        Assert.Equal(0.1, new RangeAttribute(-1, 1, 0.1).Step);
        var entry = new ConfigEntry("value", "float", null, 0.0, 0.123, 0.123, -1, 1, [], false, false);
        var (path, type, comment, def, value, fileValue, min, max, options, restart, synced) = entry;
        Assert.Equal(("value", "float", null, 0.0, 0.123, 0.123, -1.0, 1.0, false, false),
            (path, type, comment, def, value, fileValue, min, max, restart, synced));
        Assert.Empty(options);
        Assert.Null(entry.Step);
        Assert.Equal(0.25, (entry with { Step = 0.25 }).Step);
        Assert.Single(typeof(ConfigEntry).GetMethods().Where(m => m.Name == "Deconstruct" && m.GetParameters().Length == 11));
        Assert.Single(typeof(ConfigEntry).GetConstructors().Where(c => c.GetParameters().Length == 11));
    }

    [Fact]
    public void Metadata_reaches_live_and_offline_schemas_without_changing_values_or_validation()
    {
        var cfg = As(NewMod("stepped"), () => Config.Register<SteppedConfig>());
        Assert.Equal(0.1, cfg.Entries.Single(e => e.Path == "fraction").Step);
        Assert.Equal(4, cfg.Entries.Single(e => e.Path == "count").Step);
        Assert.All(cfg.Entries.Where(e => e.Path is not ("fraction" or "count")), e => Assert.Null(e.Step));
        foreach (var schema in new[] { cfg.Schema(), cfg.SchemaSnapshot(), JsonNode.Parse(File.ReadAllText(cfg.SnapshotPath))!.AsObject() })
        {
            var entries = schema["entries"]!.AsArray().ToDictionary(e => (string)e!["path"]!, e => e!.AsObject());
            Assert.Equal(0.1, (double)entries["fraction"]["step"]!);
            Assert.Equal(4, (double)entries["count"]["step"]!);
            Assert.All(entries.Where(e => e.Key is not ("fraction" or "count")), e => Assert.False(e.Value.ContainsKey("step")));
        }
        Assert.Equal(0.123, cfg.Value.Fraction);
        Assert.True(cfg.TrySet("fraction", 0.127, out var error), error);
        Assert.Equal(0.127, cfg.Value.Fraction);
        Assert.True(cfg.TrySet("count", 9, out error), error);
        Assert.Equal(9, cfg.Value.Count);
        Assert.False(cfg.TrySet("fraction", 2, out _));
        File.WriteAllText(cfg.FilePath, "{\"fraction\": 0.128, \"count\": 10}");
        cfg.Reload();
        Assert.Equal(0.128, cfg.Value.Fraction);
        Assert.Equal(10, cfg.Value.Count);
        File.WriteAllText(cfg.FilePath, "{\"fraction\": 2}");
        cfg.Reload();
        Assert.Equal(1.05, cfg.Value.Fraction); // bounds still clamp, never the slider grid
    }

    [Fact]
    public void Command_arguments_keep_bounds_only_even_with_step_metadata()
    {
        As(NewMod("stepcommands"), () => Commands.RegisterAll(new SteppedCommands()));
        foreach (var command in new[] { "stepped", "badstep" })
        {
            Assert.True(Commands.Execute(CommandSource.Console, command + " 0.123").Ok);
            Assert.False(Commands.Execute(CommandSource.Console, command + " 1.1").Ok);
        }
    }
}
