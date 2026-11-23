// SPDX-License-Identifier: MIT
// A roster NPC with a body of its own (docs/kiln/modding/npc-blocks.md, "Authored roster bodies and motions"). Everything about the body is
// data, in blocks/npc-body.json: the game builds roster NPCs once, at start, from the mod's files, before any code runs. A body defined
// from C# is not built in this version. So the code only asks whether the start built it, and says why when it did not.
using Kiln;

namespace NpcBody;

public sealed class NpcBodyMod : Mod
{
    public override void OnLoad()
    {
        // once per change of state: what this start built comes as a change from "no record"
        Events.On<NpcBodyBuildChangedEvent>(e => Log.Info($"{e.After.BlockKey}: {e.Before.State} -> {e.After.State} {e.After.Reason}"));
        if (!Capabilities.Is("blocks.npc_body"))
        {
            Log.Warn($"authored NPC bodies are off here: {Capabilities.Why("blocks.npc_body")}");
            return;
        }
        var status = NpcBodies.Status("npc-body:demo");
        if (status.Ok) Log.Info($"the demo has its own body: template {status.Template}, model {status.Model}, graph {status.Graph}");
        else Log.Warn($"the demo's body was not built ({status.State}): {status.Reason}");
    }
}
