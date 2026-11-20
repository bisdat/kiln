// SPDX-License-Identifier: MIT
// Station menus (chapter 16a): the checks, the MenuKey binding table and the normalized record. Not written yet.
namespace Kiln;

internal static class StationMenuCompiler
{
    internal const int MaxTabs = 15, MaxSets = 28, MaxRecipes = 512, MaxLanguages = 35, MaxTextBytes = 4096, MaxKey = 64, MaxIcon = 14;
    internal const int MaxRecordBytes = 1 << 20;
    internal const string PendingContent = "PendingContent";

    internal sealed class MenuError(string field, string problem) : Exception(problem)
    {
        public string Field { get; } = field;
    }

    internal sealed record Station(string Mod, string Key, Guid WorkshopGuid, uint WorkshopId, uint BaseWorkshopId, bool Own);
    internal sealed record Ref(string Mod, string Key, string Field);
    internal sealed record Set(string Key, Guid Id, IReadOnlyList<KeyValuePair<string, string>>? Heading, IReadOnlyList<Ref> Recipes);
    internal sealed record Tab(string Key, Guid Id, IReadOnlyList<KeyValuePair<string, string>> Name, int Icon, IReadOnlyList<Set> Sets);
    internal sealed record Menu(string Mod, string Key, Guid Id, IReadOnlyList<Tab> Tabs);

    internal static bool ValidKey(string? s) => throw new NotImplementedException();
    internal static string? KeyProblem(string? s, string what) => throw new NotImplementedException();
    internal static string? RefProblem(string? s) => throw new NotImplementedException();
    internal static string LanguageKey(string language) => throw new NotImplementedException();
    internal static Menu Check(string definingMod, string definitionKey, string stationMod, IReadOnlyList<StationMenuTabBlock>? tabs, string path) => throw new NotImplementedException();

    internal sealed record Binding(Guid Workshop, string Mod, string Key, string Name, Guid Recipe, ModBlocks Ledger, ulong Owner)
    {
        public uint Id => BlockIds.Fnv(Recipe);
    }

    internal static List<Binding> LiveBindings() => [];
    internal static void ResetForTests() { }

    internal enum Outcome { Record, Pending, Refused }
    internal readonly record struct Resolution(Outcome Outcome, string Record, string Problem, int Pruned);
    internal static Resolution Resolve(Menu menu, Station station, IReadOnlyList<Binding> live, Func<Guid, string?> stateOf, bool prune) => throw new NotImplementedException();
}
