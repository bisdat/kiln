// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Text.Json.Nodes;
using Xunit;
namespace Kiln.BlockTests;
public sealed class SyncedMoveTests
{
    private static readonly Guid Clone = Guid.Parse("12345678-1234-4234-8234-123456789012"), State = Guid.Parse("22345678-1234-4234-8234-123456789012");
    private static readonly GameDataDefinition Def = new(GameDataSection.Sequences, Clone, 1, "mod.weapon.primary.r0123456789ab", "base", "registered", "server sync", null, false, null, [], "{}");

    // Reflection keeps the red test executable before the new internal receiving-side guard exists.
    private static void Filter(JsonObject doc, string status, bool present, PlayerAnimStateKind kind, List<string> logs,
        bool unreadable = false, bool inheritedMissing = false, bool missingChain = false)
    {
        var type = typeof(ContentSync).Assembly.GetType("Kiln.SyncedMoves");
        Assert.NotNull(type);
        var method = type.GetMethod("Filter", BindingFlags.Static | BindingFlags.NonPublic);
        Assert.NotNull(method);
        var extra = Def with { Guid = Guid.Parse("32345678-1234-4234-8234-123456789012"), Name = "mod.weapon.primary.2.r0123456789ab" };
        method.Invoke(null, [doc, missingChain ? new[] { Def, extra } : new[] { Def },
            (Func<Guid, IReadOnlyDictionary<string, (string State, string? Error)>>)(_ => new Dictionary<string, (string, string?)> { ["client"] = (status, "registry unavailable") }),
            (Func<Guid, IReadOnlyList<(Guid State, PlayerAnimStateKind Kind)>?>)(g => missingChain && g == extra.Guid ? [(Guid.Empty, PlayerAnimStateKind.State)] : unreadable ? null : inheritedMissing ? [(State, PlayerAnimStateKind.State), (Guid.Empty, PlayerAnimStateKind.State)] : [(State, PlayerAnimStateKind.State)]),
            (Func<Guid, IReadOnlyList<AnimationReferenceAnswer>>)(g => present && g == State ? [new(kind, true, "")] : []),
            (Action<string>)logs.Add]);
    }

    private static JsonObject Document(bool array = false) => new()
    {
        ["itemdata"] = new JsonObject
        {
            ["weapon"] = array ? new JsonObject { ["sequences"] = new JsonArray(new JsonObject { ["sequence"] = Clone.ToString() }), ["power"] = 7 }
                : new JsonObject { ["sequences[0].sequence"] = Clone.ToString(), ["sequences[1].sequence"] = Clone.ToString(), ["power"] = 7 },
            ["other"] = new JsonObject { ["power"] = 9 },
        },
        ["buffs"] = new JsonObject { ["x"] = new JsonObject { ["duration"] = 3 } },
    };

    [Theory]
    [InlineData("pending", true, PlayerAnimStateKind.State)]
    [InlineData("failed", true, PlayerAnimStateKind.State)]
    [InlineData("registered", false, PlayerAnimStateKind.State)]
    [InlineData("registered", true, PlayerAnimStateKind.Overlay)]
    public void A_receiver_refuses_only_the_weapons_slots(string status, bool present, PlayerAnimStateKind kind)
    {
        var doc = Document(); var logs = new List<string>();
        Filter(doc, status, present, kind, logs);
        Assert.Null(doc["itemdata"]!["weapon"]!["sequences"]);
        Assert.Equal(1, (int)doc["itemdata"]!["weapon"]!["sequences[0].sequence"]!["mul"]!);
        Assert.Equal(7, (int)doc["itemdata"]!["weapon"]!["power"]!);
        Assert.Equal(9, (int)doc["itemdata"]!["other"]!["power"]!);
        Assert.NotNull(doc["buffs"]);
        Assert.Contains("weapon", Assert.Single(logs));
    }

    [Fact]
    public void Fallback_uses_supported_reference_leaves_not_an_unwritable_list_header()
    {
        var doc = Document(); var logs = new List<string>();
        Filter(doc, "failed", true, PlayerAnimStateKind.State, logs);
        Assert.Null(doc["itemdata"]!["weapon"]!["sequences"]);
        Assert.Equal(1, (int)doc["itemdata"]!["weapon"]!["sequences[0].sequence"]!["mul"]!);
        Assert.Equal(1, (int)doc["itemdata"]!["weapon"]!["sequences[1].sequence"]!["mul"]!);
    }

    [Fact]
    public void Valid_synced_slots_remain_selected()
    {
        var doc = Document(); var logs = new List<string>();
        Filter(doc, "registered", true, PlayerAnimStateKind.State, logs);
        Assert.Equal(Clone.ToString(), (string)doc["itemdata"]!["weapon"]!["sequences[0].sequence"]!);
        Assert.Empty(logs);
    }

    [Fact]
    public void A_missing_state_on_a_later_chain_link_refuses_the_whole_weapons_slots()
    {
        var doc = Document(); var logs = new List<string>();
        Filter(doc, "registered", true, PlayerAnimStateKind.State, logs, missingChain: true);
        Assert.Equal(1, (int)doc["itemdata"]!["weapon"]!["sequences[0].sequence"]!["mul"]!);
        Assert.Single(logs);
    }

    [Theory]
    [InlineData(true, false, false)]
    [InlineData(false, true, false)]
    [InlineData(false, false, true)]
    public void Whole_lists_unreadable_sequences_and_inherited_missing_states_are_checked(bool array, bool unreadable, bool inherited)
    {
        var doc = Document(array); var logs = new List<string>();
        Filter(doc, array ? "failed" : "registered", true, PlayerAnimStateKind.State, logs, unreadable, inherited);
        Assert.Equal(1, (int)doc["itemdata"]!["weapon"]!["sequences[0].sequence"]!["mul"]!);
        Assert.Single(logs);
    }
}
