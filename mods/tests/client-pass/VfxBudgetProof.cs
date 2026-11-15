// SPDX-License-Identifier: MIT
// Pure, shared evidence predicates: compiled into client-pass, its fixture, and Contract.Tests.
using System.Text.Json.Nodes;
using Kiln;
namespace ClientPass;
internal static class VfxBudgetProof
{
    public const string Fixture = "17a-vfx-budget-v2";
    public static readonly string[] Names = ["budget_red", "budget_green", "budget_blue"];
    public static string ProcessKey
    {
        get
        {
            try { using var p = System.Diagnostics.Process.GetCurrentProcess(); return $"{Environment.ProcessId}/{p.StartTime.ToUniversalTime().Ticks}"; }
            catch (Exception e) when (e is InvalidOperationException or NotSupportedException or System.ComponentModel.Win32Exception) { return ""; }
        }
    }
    private static Dictionary<string, JsonObject>? Effects(JsonObject? doc, string process)
    {
        if (process.Length == 0 || (string?)doc?["process"] != process || (string?)doc?["fixture"] != Fixture || doc?["effects"] is not JsonArray effects || effects.Count != 3) return null;
        var map = new Dictionary<string, JsonObject>();
        foreach (var e in effects.OfType<JsonObject>())
        {
            var name = (string?)e["name"];
            if (name == null || !Names.Contains(name) || !map.TryAdd(name,e) ||
                !Guid.TryParse((string?)e["registrationId"],out var id) || id == Guid.Empty ||
                !Guid.TryParse((string?)e["definition"],out var definition) || definition != VfxId.Named("vfx-test",name).Guid) return null;
        }
        var never = doc?["requireRegistered"];
        return map.Count == 3 && (string?)never?["name"] == "budget_never_stored" && never?["ok"]?.GetValue<bool>() == false &&
            (uint?)never?["slots"] == 0 && (string?)never?["code"] is "RegistryClosed" or "CapacityExceeded" ? map : null;
    }
    public static bool Before(JsonObject? doc, string process, uint kilnUsed, out JsonObject? evidence)
    {
        evidence = null;
        try
        {
            var effects = Effects(doc,process);
            if (effects == null || !effects.Values.All(e => e["ok"]?.GetValue<bool>() == true && (string?)e["state"] == "AcceptedNextStart" && (uint?)e["slots"] == 0)) return false;
            evidence = (JsonObject)doc!.DeepClone(); evidence["kilnUsed"] = kilnUsed;
            return true;
        }
        catch (Exception e) when (e is InvalidOperationException or FormatException or OverflowException) { return false; }
    }
    public static bool After(JsonObject? evidence, JsonObject? current, string process, uint kilnUsed)
    {
        try
        {
            var previousProcess = (string?)evidence?["process"];
            if (string.IsNullOrEmpty(previousProcess) || previousProcess == process || evidence?["kilnUsed"] is not { } count) return false;
            if (!Before(evidence,previousProcess,count.GetValue<uint>(),out _)) return false;
            var old = Effects(evidence,previousProcess); var now = Effects(current,process);
            return old != null && now != null && (ulong)count.GetValue<uint>() + 3 == kilnUsed && Names.All(n =>
                (string?)old[n]["registrationId"] == (string?)now[n]["registrationId"] && (string?)old[n]["definition"] == (string?)now[n]["definition"]);
        }
        catch (Exception e) when (e is InvalidOperationException or FormatException or OverflowException) { return false; }
    }
}
