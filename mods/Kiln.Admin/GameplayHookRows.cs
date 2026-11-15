// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
namespace Kiln.Admin;

// Observer flags say installed, not whether zone protection is supported. Preserve the group row's reason for UI.
internal static class GameplayHookRows
{
    internal static JsonObject Snapshot(Func<string, Capability>? get=null)
    {
        get ??= Capabilities.Get;
        var rows=new JsonObject();
        foreach(var group in new[]{"build","terraform","spawn"}) {
            var row=get("gameplay."+group+"_hooks");
            var state=row.Reason==CapabilityReason.Unknown ? "unknown" : row.State==CapabilityState.Available ? "installed" :
                row.State==CapabilityState.Unavailable ? "refused" : row.ReasonText.StartsWith("resolved,",StringComparison.Ordinal) ? "idle" : "waiting";
            rows[group]=new JsonObject { ["state"]=state,["reason"]=row.ReasonText };
        }
        return rows;
    }
}
