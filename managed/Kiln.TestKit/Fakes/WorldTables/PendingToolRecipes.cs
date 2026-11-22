// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
namespace Kiln.TestKit;

/// <summary>A pending tool has no live item to craft yet. Keep its recipe declaration observable in Blocks.Lowered,
/// without pretending that kiln.content or its live registry exists. All other registrations retain their normal
/// offline refusal. This sink lives only on the harness's tick thread.</summary>
internal sealed class PendingToolRecipeSink(Lowered.ILoweredSink inner):Lowered.ILoweredSink
{
    public void Program(ulong owner,Guid guid,byte[] blob)=>inner.Program(owner,guid,blob);
    public void Clone(ulong owner,string section,JsonObject definition)=>inner.Clone(owner,section,definition);
    public void Item(ulong owner,ItemDefinition definition)=>inner.Item(owner,definition);
    public void Recipe(ulong owner,RecipeDefinition definition)
    {
        if(definition.Outputs.Count==1) {
            var output=definition.Outputs[0].Item;
            foreach(var mod in Blocks.AllMods.Where(m=>m.Owner==owner))
                foreach(var handle in mod.Handles.Values)
                    if(handle is TableBlockHandle<BuildingToolBlock> {Status:TableBlockStatus.Pending} tool &&
                        (output==tool.GameName || output==tool.Guid.ToString() || output==tool.Id.ToString(System.Globalization.CultureInfo.InvariantCulture)))return;
        }
        inner.Recipe(owner,definition);
    }
}
