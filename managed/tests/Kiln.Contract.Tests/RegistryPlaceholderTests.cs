// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Text.Json.Nodes;
using Xunit;
namespace Kiln.Contract.Tests;
public sealed class RegistryPlaceholderTests
{
    [Fact]
    public void PlaceholderKeepsStableIdentityEffectiveFieldsAndFlattenedGameBase()
    {
        var type = typeof(Content).Assembly.GetType("Kiln.MissingContent")!;
        var factory = type.GetMethod("Placeholder", BindingFlags.Static | BindingFlags.NonPublic);
        Assert.NotNull(factory);
        var body = JsonNode.Parse("""{"guid":"12345678-1234-5678-9123-123456789abc","name":"Kiln_Registry_items_token","base":"22345678-1234-5678-9123-123456789abc","registryId":"a:token","registryKind":"items","registryFields":{"maxStack":500,"power":70},"loot":true,"drops":[{}]}""")!.AsObject();
        var seenType = type.GetNestedType("SeenItem", BindingFlags.Public)!;
        var seen = Activator.CreateInstance(seenType)!;
        seenType.GetProperty("Name")!.SetValue(seen, "Kiln_Registry_items_token");
        seenType.GetProperty("Mod")!.SetValue(seen, "a");
        var placeholder = (JsonObject)factory.Invoke(null, [Guid.Parse(body["guid"]!.GetValue<string>()), seen, body])!;
        Assert.Equal("a:token", placeholder["registryId"]!.GetValue<string>());
        Assert.Equal("items", placeholder["registryKind"]!.GetValue<string>());
        Assert.Equal(500, placeholder["registryFields"]!["maxStack"]!.GetValue<int>());
        Assert.Equal(body["base"]!.GetValue<string>(), placeholder["base"]!.GetValue<string>());
        Assert.Null(placeholder["loot"]);
        Assert.Null(placeholder["drops"]);
        Assert.NotNull(placeholder["placeholder"]);
        placeholder["registryFields"]!["maxStack"] = 1;
        Assert.Equal(500, body["registryFields"]!["maxStack"]!.GetValue<int>());
    }
}
