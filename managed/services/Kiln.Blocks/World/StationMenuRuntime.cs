// SPDX-License-Identifier: MIT
// Station menus at run time (chapter 16a). Not written yet.
namespace Kiln;

internal static class StationMenuRuntime
{
    internal sealed class Intent
    {
        public string State = "pending";
        public string Reason = StationMenuCompiler.PendingContent;
    }

    internal static bool AutoTick = true;
    internal static Func<bool> BlockPassDone = () => false;
    internal static Action Declared = StationMenuSync.BlocksDeclared;
    internal static bool IsDeclared => false;
    internal static void Start() { }
    internal static void Pump() { }
    internal static MenuDefinitionResult Define(BlockHandle<StationBlock> station, StationMenuDefinition definition) => throw new NotImplementedException();
    internal static void Dispose(StationMenuRegistration registration) => throw new NotImplementedException();
    internal static Intent? Find(ulong owner, Guid definition) => null;
    internal static void ResetForTests() { }
}
