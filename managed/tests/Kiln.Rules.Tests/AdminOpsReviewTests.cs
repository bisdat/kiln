// SPDX-License-Identifier: MIT
// Compile the production admin handlers against a registration-only bridge: malformed requests must fail before
// item lookup/submission, even when the native game services are absent.
extern alias rulesapi;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Admin;

internal sealed class AdminError(string text) : Exception(text);
internal sealed class Logger { public void Info(string _) { } }
internal sealed class Bridge
{
    public readonly Dictionary<string, Func<JsonObject, Task<JsonNode?>>> Async = new();
    public readonly Dictionary<string, Func<JsonObject, JsonNode?>> Sync = new();
    public void Register(string name, Func<JsonObject, JsonNode?> op) => Sync[name] = op;
    public void RegisterAsync(string name, Func<JsonObject, Task<JsonNode?>> op) => Async[name] = op;
}
internal static partial class Ops
{
    private static Player RequirePlayer(JsonObject req) => new() { Entity = new EntityId(req["entity"]?.GetValue<uint>() ?? 3), Name = "test" };
    // The test namespace Kiln.Rules.Tests shadows the public Rules type when production admin source is linked.
    private static class Rules
    {
        public static RulesClient ClientOf(EntityId e) => rulesapi::Kiln.Rules.ClientOf(e);
        public static EffectiveRules Of(EntityId e) => rulesapi::Kiln.Rules.Of(e);
        // Kiln.Rules' own: with the test kit referenced, Kiln.Creatures' obsolete Kiln.RulesStatus can be in reach too
        public static rulesapi::Kiln.RulesStatus? Status() => rulesapi::Kiln.Rules.Status();
        public static IReadOnlyList<string> Presets => rulesapi::Kiln.Rules.Presets;
        public static IReadOnlyList<RuleSet> Active => rulesapi::Kiln.Rules.Active;
    }
}
public sealed class AdminOpsReviewTests
{
    [Theory]
    [InlineData("{}", "effect requires an explicit boolean on")]
    [InlineData("{\"on\":\"off\"}", "effect requires an explicit boolean on")]
    [InlineData("{\"on\":false,\"effect\":12}", "effect must be a Creative effect name")]
    [InlineData("{\"on\":false,\"effect\":\"typo\"}", "Unknown Creative effect")]
    public void InvalidAdminEffectNeverFallsThroughToWholeCreativeToggle(string json, string error)
    {
        var bridge = new Bridge();
        Assert.True(Ops.RegisterRulesSafe(bridge, new Logger()));
        var request = (JsonObject)JsonNode.Parse(json)!;
        if (!request.ContainsKey("effect")) request["effect"] = "stamina";
        var e = Assert.Throws<AdminError>(() => bridge.Sync["creative"](request));
        Assert.StartsWith(error, e.Message);
        Assert.False(Creative.IsOn(new EntityId(3)));
    }

    [Theory]
    [InlineData("count", "\"oops\"", "count must be an integer")]
    [InlineData("count", "1.5", "count must be an integer")]
    [InlineData("level", "{}", "level must be an integer")]
    [InlineData("rarity", "\"255\"", "rarity is")]
    [InlineData("rarity", "\"typo\"", "rarity is")]
    [InlineData("rarity", "255", "rarity is")]
    public async Task InvalidAdminGiveFieldsNeverBecomeDefaults(string field, string json, string error)
    {
        var bridge = new Bridge();
        Assert.True(Ops.RegisterRulesSafe(bridge, new Logger()));
        var request = new JsonObject { ["item"] = "Wood" };
        request[field] = JsonNode.Parse(json);
        var e = await Assert.ThrowsAsync<AdminError>(() => bridge.Async["creative_give"](request));
        Assert.StartsWith(error, e.Message);
    }
}
