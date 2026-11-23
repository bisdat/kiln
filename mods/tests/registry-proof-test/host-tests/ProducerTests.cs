// SPDX-License-Identifier: MIT
// Host-only tests: compile the actual never-shipped producer against its real managed API dependencies.
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json.Nodes;
using Kiln;
using RegistryProofTest;
using Xunit;

public class ProducerTests
{
    // Coordinator ruling (2026-11-23): evidence only for the config kinds the build has, so the producer counts them; an
    // entry that holds the value but whose kind cannot be read counts as "unknown", never as absent.
    [Theory]
    [InlineData(ContentValueType.Int, true, "sint32")]
    [InlineData(ContentValueType.Float, true, "float")]
    [InlineData(ContentValueType.Int, false, "sint32")]
    [InlineData(ContentValueType.Bool, true, "unknown")]
    [InlineData(null, true, "unknown")]
    [InlineData(null, false, null)]
    public void Config_kinds_are_counted_and_unreadable_ones_are_unknown(ContentValueType? type, bool holds, string? expected)
    {
        var method = typeof(RegistryProofTestMod).GetMethod("ConfigKind", BindingFlags.Static | BindingFlags.NonPublic)!;
        Assert.Equal(expected, (string?)method.Invoke(null, new object?[] { type, holds }));
    }

    // review2 N4: an impactConfig row naming a config Kiln has no id for gets no probe, never StaminaCost's id.
    [Theory]
    [InlineData("ManaCost", "0xaf236ece")]
    [InlineData("StaminaCost", "0x727d7f78")]
    [InlineData("HealthCost", null)]
    [InlineData(null, null)]
    public void Config_ids_refuse_unknown_names(string? config, string? expected)
    {
        var method = typeof(RegistryProofTestMod).GetMethod("ConfigId", BindingFlags.Static | BindingFlags.NonPublic)!;
        Assert.Equal(expected, (string?)method.Invoke(null, new object?[] { config }));
    }

    [Theory]
    [InlineData(2.0)]
    [InlineData(2.5)]
    [InlineData(0.0)]
    public void Float_config_probe_is_fractional_and_changes_the_value(double before)
    {
        var probe = typeof(RegistryProofTestMod).GetMethod("Probe", BindingFlags.Static | BindingFlags.NonPublic)!;
        var value = (JsonNode)probe.Invoke(null, new object[] { JsonValue.Create(before)!,
            new JsonObject { ["conv"] = "impactConfig" }, Array.Empty<string>() })!;
        var number = value.GetValue<double>();
        Assert.NotEqual(Math.Floor(number), number);
        Assert.NotEqual(before, number);
    }

    // Reflection exposes a struct with one scalar (GemSlotOverride {isSet}) at the struct's own path, so the raw read of
    // items.socket's "gemSlotOverride.isSet" falls back to "gemSlotOverride" (only for a Boolean, see Raw).
    [Theory]
    [InlineData("gemSlotOverride.isSet", "gemSlotOverride.isSet|gemSlotOverride")]
    [InlineData("armorSetup.isSet", "armorSetup.isSet|armorSetup")]
    [InlineData("impactValues", "impactValues")]
    [InlineData("isSet", "isSet")]
    public void Raw_paths_include_the_flattened_one_flag_struct(string path, string expected)
    {
        var method = typeof(RegistryProofTestMod).GetMethod("RawPaths", BindingFlags.Static | BindingFlags.NonPublic)!;
        Assert.Equal(expected, string.Join("|", (IEnumerable<string>)method.Invoke(null, new object[] { path })!));
    }

    [Theory]
    [InlineData(2, "not_exact: value cannot be represented", true)]
    [InlineData(0, "not_exact: value cannot be represented", false)]
    [InlineData(2, "not_exact:value cannot be represented", false)]
    [InlineData(2, "unproven: no mapping", false)]
    [InlineData(2, "value cannot be represented exactly", false)]
    public void Exact_refusal_needs_both_the_outcome_and_the_stable_token(int outcome, string reason, bool expected)
    {
        var type = typeof(RegistryProofTestMod);
        var reply = type.GetNestedType("Reply", BindingFlags.NonPublic)!;
        var method = type.GetMethod("IsExactRefusal", BindingFlags.NonPublic | BindingFlags.Static)!;
        var memory = Marshal.AllocHGlobal(232);
        try
        {
            Marshal.Copy(new byte[232], 0, memory, 232);
            Marshal.WriteInt32(memory, 4, outcome);
            var bytes = Encoding.UTF8.GetBytes(reason);
            Marshal.Copy(bytes, 0, memory + 40, bytes.Length);
            Assert.Equal(expected, (bool)method.Invoke(null, new[] { Marshal.PtrToStructure(memory, reply) })!);
        }
        finally { Marshal.FreeHGlobal(memory); }
    }
}
