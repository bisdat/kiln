// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Kiln.TestKit;
namespace Kiln.Sdk.Bake;

/// <summary>The opt-in world-table bake records declarations, not live content registrations. The compiler still
/// validates and lowers items, recipes and clones, but no game exists to instantiate their bases. They are rebuilt
/// by the mod at runtime. Programs can use the existing offline asset recorder.</summary>
internal sealed class BakeDeclarations:IDisposable
{
    private readonly KilnHarness _harness;
    private readonly Lowered.ILoweredSink? _previous;
    internal BakeDeclarations(KilnHarness harness)
    {
        _harness=harness;
        _previous=harness.Run(()=> {
            var previous=Lowered._sinkOverride;
            Lowered._sinkOverride=new DeclarationSink(Lowered.Sink);
            return previous;
        });
    }
    public void Dispose()=>_harness.Run(()=>Lowered._sinkOverride=_previous);
    private sealed class DeclarationSink(Lowered.ILoweredSink programs):Lowered.ILoweredSink
    {
        public void Program(ulong owner,Guid guid,byte[] blob)=>programs.Program(owner,guid,blob);
        public void Clone(ulong owner,string section,JsonObject definition) { }
        public void Item(ulong owner,ItemDefinition definition) { }
        public void Recipe(ulong owner,RecipeDefinition definition) { }
    }
}
