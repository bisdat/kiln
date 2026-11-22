// SPDX-License-Identifier: MIT
// Kiln's base rules (BaseRules.cs, docs/kiln/services/build.md "Base rules and base limits"): BaseRulesBlock staged in OnBoot and as
// JSON, refused at staging when a value is out of Kiln's bounds, lowered to the patches collector as "baseRules@<name>" (kiln.content
// composes them: native/services/content/tests/baserules_test.cpp); and Kiln.Build's reader of kiln.base_rules' state.
using System.Numerics;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Blocks.Tests;

[Collection("boot generation")]  // BootGeneration is process-wide
public sealed class BaseRulesTests
{
    private static BootContext Boot() => new("roomy-bases", 1, BootRoles.DedicatedServer, new UnavailableGameData());

    private static string Request(IEnumerable<BootStaged> staged) => new JsonObject
    {
        ["id"] = "roomy-bases", ["clients"] = "mod",
        ["staged"] = new JsonArray(staged.Select(s => (JsonNode)new JsonObject { ["kind"] = s.Kind, ["key"] = s.Key, ["json"] = s.Json }).ToArray()),
    }.ToJsonString();

    [Fact]
    public void A_base_rule_is_staged_as_its_document_and_goes_to_the_patches_collector_as_baseRules_at_its_name()
    {
        var f = Boot();
        var d = BootDefinitions.For(f);
        var staged = d.Define("roomy", new BaseRulesBlock
        {
            MinimumAltars = 8, ExtraAltars = 2, BuildAreaPercent = 150, Sizes = [new AltarSize(1, new Vector3(60, 60.5f, 60)), new AltarSize(4, new Vector3(250))],
        });
        Assert.True(staged.Ok, staged.Reason);
        Assert.Equal(new BootBlockId<BaseRulesBlock>("roomy-bases", "baseRules", "roomy"), staged.Reference);
        var one = Assert.Single(f.Staged);
        Assert.Equal("baseRules", one.Kind);
        // the document kiln.content reads (baserules.hpp parse), keys sorted
        Assert.Equal("""{"buildAreaPercent":150,"extraAltars":2,"minimumAltars":8,"sizes":[{"altarLevel":1,"size":[60,60.5,60]},{"altarLevel":4,"size":[250,250,250]}]}""", one.Json);
        // left-out members are left out of the document
        var g = Boot();
        Assert.True(BootDefinitions.For(g).Define("plain", new BaseRulesBlock { ExtraAltars = 1 }).Ok);
        Assert.Equal("""{"extraAltars":1}""", Assert.Single(g.Staged).Json);
        // the same from JSON (a blocks file's "baseRules" value)
        var h = Boot();
        Assert.True(BootDefinitions.For(h).DefineJson("baseRules", "roomy", """
            { "minimumAltars": 8, "extraAltars": 2, "buildAreaPercent": 150,
              "sizes": [ { "altarLevel": 1, "size": [60, 60.5, 60] }, { "altarLevel": 4, "size": [250, 250, 250] }, ], }
            """).Ok);
        Assert.Equal(one.Json, Assert.Single(h.Staged).Json);
        // lowered: one document for the patches collector, keyed so kiln.content tells it from a resource patch; nothing compiled
        var r = JsonNode.Parse(BootLowering.LowerOwner(Request(f.Staged)))!.AsObject();
        Assert.True(r["ok"]!.GetValue<bool>(), (string?)r["reason"]);
        Assert.Equal(["baseRules/roomy"], r["definitions"]!.AsArray().Select(x => (string)x!));
        var doc = Assert.Single(r["docs"]!["patches"]!.AsArray())!;
        Assert.Equal("baseRules@roomy", (string?)doc["key"]);
        Assert.Equal(8, (int)doc["document"]!["minimumAltars"]!);
        Assert.Empty(r["docs"]!["content"]!.AsArray());
    }

    [Fact]
    public void A_value_outside_Kilns_bounds_throws_at_staging_and_names_the_member()
    {
        var d = BootDefinitions.For(Boot());
        void Bad(BaseRulesBlock b, string part) => Assert.Contains(part, Assert.Throws<ArgumentException>(() => d.Define("bad", b)).Message);
        Bad(new BaseRulesBlock { MinimumAltars = 17 }, "at most 16");
        Bad(new BaseRulesBlock { MinimumAltars = 0 }, "MinimumAltars");
        Bad(new BaseRulesBlock { ExtraAltars = 16 }, "ExtraAltars");
        Bad(new BaseRulesBlock { ExtraAltars = -1 }, "ExtraAltars");
        Bad(new BaseRulesBlock { BuildAreaPercent = 99 }, "BuildAreaPercent");
        Bad(new BaseRulesBlock { BuildAreaPercent = 201 }, "BuildAreaPercent");
        Bad(new BaseRulesBlock { Sizes = [new AltarSize(0, new Vector3(40))] }, "AltarLevel");
        Bad(new BaseRulesBlock { Sizes = [new AltarSize(6, new Vector3(40))] }, "AltarLevel");
        Bad(new BaseRulesBlock { Sizes = [new AltarSize(1, new Vector3(0.25f, 40, 40))] }, "0.5 to 256");
        Bad(new BaseRulesBlock { Sizes = [new AltarSize(1, new Vector3(40, 256.5f, 40))] }, "0.5 to 256");
        Bad(new BaseRulesBlock { Sizes = [new AltarSize(1, new Vector3(40, 40, 100.25f))] }, "multiple of 0.5");
        Bad(new BaseRulesBlock { Sizes = [new AltarSize(1, new Vector3(float.NaN, 40, 40))] }, "0.5 to 256");
        Bad(new BaseRulesBlock { Sizes = [new AltarSize(1, new Vector3(float.PositiveInfinity))] }, "0.5 to 256");
        Bad(new BaseRulesBlock { Sizes = [new AltarSize(2, new Vector3(40)), new AltarSize(2, new Vector3(50))] }, "named twice");
        Assert.Throws<ArgumentException>(() => d.DefineJson("baseRules", "bad-json", """{ "minimumAltars": 17 }"""));
        Assert.Throws<ArgumentException>(() => d.DefineJson("baseRules", "unknown", """{ "minimumAltar": 4 }"""));
        Assert.Throws<ArgumentException>(() => d.DefineJson("baseRules", "two-axes", """{ "sizes": [ { "altarLevel": 1, "size": [40, 40] } ] }"""));
        // the edges are fine, and a rule that changes nothing is no error
        Assert.True(d.Define("edges", new BaseRulesBlock { MinimumAltars = 16, ExtraAltars = 15, BuildAreaPercent = 200,
            Sizes = [new AltarSize(1, new Vector3(0.5f)), new AltarSize(5, new Vector3(256))] }).Ok);
        Assert.True(d.Define("nothing", new BaseRulesBlock()).Ok);
    }

    [Fact]
    public void The_state_of_kiln_base_rules_reads_as_base_limits()
    {
        const string state = """
            {"table": true, "applied": true, "patchedBy": ["more-flame-altars"],
             "original": {"altars": [1,2,4,6,8,10,11,12,13,14,15,16,16,16,16,16,16,16,16,16,16], "sizes": [[0,0,0],[40,40,40],[80,80,80],[120,120,120],[160,160,160],[160,160,160]]},
             "effective": {"altars": [8,8,9,11,13,15,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16], "sizes": [[0,0,0],[60,60,60],[120,120,120],[180,180,180],[250,250,250],[240,240,240]]},
             "rules": [{"mod": "rules-a", "name": "roomy", "applied": true}, {"mod": "rules-b", "name": "wide", "applied": true}, {"mod": "rules-c", "name": "late", "applied": false}],
             "composed": {"minimumAltars": 8, "extraAltars": 5, "buildAreaPercent": 120, "sizes": []}}
            """;
        var l = BaseLimitsReader.From(state, 2, 3)!;
        Assert.Equal((2, 3, 4, 9), (l.FlameLevel, l.CurrentAltars, l.OriginalCount, l.EffectiveCount));
        Assert.Equal(21, l.OriginalCounts.Count);
        Assert.Equal(16, l.EffectiveCounts[20]);
        Assert.Equal([1, 2, 3, 4, 5], l.EffectiveSizes.Select(s => s.AltarLevel));
        Assert.Equal(new AltarSize(4, new Vector3(250)), l.EffectiveSizes[3]);
        Assert.Equal(new AltarSize(1, new Vector3(40)), l.OriginalSizes[0]);
        Assert.Equal(["rules-a:roomy", "rules-b:wide"], l.AppliedRules);
        Assert.Equal(["more-flame-altars"], l.PatchedBy);
        // a Flame level past the table's end reads its last entry; a state without the table is no limits
        Assert.Equal(16, BaseLimitsReader.From(state, 40, 0)!.EffectiveCount);
        Assert.Null(BaseLimitsReader.From("""{"table": false, "reason": "UnknownType"}""", 1, 0));
        Assert.Null(BaseLimitsReader.From(null, 1, 0));
        Assert.Empty(l.RegistryChanged);
    }

    // Ruling D1: the registry stage composes on the base rules' result and wins; the read side says so per value.
    [Fact]
    public void A_registry_edit_over_a_rule_is_named()
    {
        const string state = """
            {"table": true, "applied": true, "patchedBy": [],
             "original": {"altars": [1, 2, 4], "sizes": [[0,0,0],[40,40,40]]},
             "effective": {"altars": [8, 2, 8], "sizes": [[0,0,0],[60,60,60]]},
             "rules": [{"mod": "rules-a", "name": "roomy", "applied": true}],
             "registryChanged": [{"field": "altarsPerFlameLevel[1]", "afterRules": 8, "effective": 2, "rules": ["rules-a:roomy"]}]}
            """;
        var l = BaseLimitsReader.From(state, 1, 0)!;
        Assert.Equal(2, l.EffectiveCount);
        Assert.Equal(["altarsPerFlameLevel[1]: 8 -> 2 (rules-a:roomy)"], l.RegistryChanged);
    }
}
