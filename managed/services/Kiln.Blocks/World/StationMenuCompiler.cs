// SPDX-License-Identifier: MIT
// Station menus (chapter 16a; dev31 out/16a-station-tabs/DESIGN.md section 2): a menu's checks, the table that binds a recipe's
// MenuKey to the recipe it names, and the normalized record kiln.stationmenus stores (format 1, canonical JSON: keys sorted by
// ordinal, compact, arrays in authored order, lower-case GUIDs, ids as unsigned numbers, texts as a language map; what
// nlohmann::json's dump with ensure_ascii=false writes for the same document, so define, export and import agree byte for byte).
//  - ids: workshop Derive(origin.mod, "workshop", stationKey); definition Derive(definition.mod, "menu-layout", key) (a station's own
//    tabs: "<stationKey>.menu"); tab Derive(definition.mod, "menu-tab", key.tab); set Derive(definition.mod, "menu-set", key.tab.set);
//    a recipe's id is the FNV-1a of the GUID it was lowered to, as before (its name and GUID do not change with a MenuKey).
//  - a reference "key" is of the station's mod, "mod:key" of that mod (case-insensitive, as mod ids are); it binds to the one recipe
//    of that mod with that MenuKey at the station's own workshop (a station's list or an item's recipe at it), never to an output, a
//    name or a GUID. None, two, one of another workshop, or the same one twice: refused.
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace Kiln;

internal static class StationMenuCompiler
{
    internal const int MaxTabs = 15, MaxSets = 28, MaxRecipes = 512, MaxLanguages = 35, MaxTextBytes = 4096, MaxKey = 64, MaxIcon = 14;
    internal const int MaxRecordBytes = 1 << 20;
    internal const string PendingContent = "PendingContent";

    /// <summary>What is wrong with a menu, by field (a path under the menu): a block makes it a BlockError, StationMenus.Define an ArgumentException.</summary>
    internal sealed class MenuError(string field, string problem) : Exception(problem)
    {
        public string Field { get; } = field;
    }

    /// <summary>The station a menu lays out: its mod and stable key (Was, else its name), its own workshop, and the workshop whose menu it is
    /// like (where a tab's icon comes from). Not own: a game workshop, which keeps the game's tabs.</summary>
    internal sealed record Station(string Mod, string Key, Guid WorkshopGuid, uint WorkshopId, uint BaseWorkshopId, bool Own);

    /// <summary>A recipe reference, qualified: the mod part lower-cased.</summary>
    internal sealed record Ref(string Mod, string Key, string Field)
    {
        public string Qualified => $"{Mod.ToLowerInvariant()}:{Key}";
        public override string ToString() => Qualified;
    }

    internal sealed record Set(string Key, Guid Id, IReadOnlyList<KeyValuePair<string, string>>? Heading, IReadOnlyList<Ref> Recipes);
    internal sealed record Tab(string Key, Guid Id, IReadOnlyList<KeyValuePair<string, string>> Name, int Icon, IReadOnlyList<Set> Sets);
    /// <summary>A checked menu: the defining mod, the definition's key and id, the tabs with their ids and texts.</summary>
    internal sealed record Menu(string Mod, string Key, Guid Id, IReadOnlyList<Tab> Tabs);

    // ---- grammar --------------------------------------------------------------------------------------------------

    internal static bool ValidKey(string? s) => s is { Length: > 0 and <= MaxKey } && s.All(c => char.IsAsciiLetterOrDigit(c) || c is '.' or '_' or '-');

    internal static string? KeyProblem(string? s, string what) =>
        ValidKey(s) ? null : $"{what} \"{s}\" is not a key: 1 to 64 letters, digits, '.', '_' and '-' (no colon, no space)";

    /// <summary>Why a recipe reference is not "key" or "mod:key", or null.</summary>
    internal static string? RefProblem(string? s)
    {
        if (string.IsNullOrEmpty(s)) return "a recipe in a menu is named by its menuKey: \"key\", or \"other-mod:key\"";
        var colon = s.IndexOf(':');
        if (colon < 0) return ValidKey(s) ? null : $"\"{s}\" is not a menuKey: 1 to 64 letters, digits, '.', '_' and '-'";
        var mod = s[..colon];
        var key = s[(colon + 1)..];
        if (!ValidKey(mod)) return $"\"{s}\": \"{mod}\" is not a mod id (1 to 64 letters, digits, '.', '_' and '-')";
        if (!ValidKey(key)) return $"\"{s}\": \"{key}\" is not a menuKey (1 to 64 letters, digits, '.', '_' and '-')";
        return null;
    }

    /// <summary>A language key as UiText keeps it: trimmed, '_' as '-', lower-case.</summary>
    internal static string LanguageKey(string language) => (language ?? "").Trim().Replace('_', '-').ToLowerInvariant();

    private static readonly Regex Language = new("^(\\*|[a-z]{2,3}(-[a-z0-9]{2,8})*)$", RegexOptions.CultureInvariant);
    private static readonly UTF8Encoding Strict = new(false, true);

    /// <summary>A text as the record writes it: language -> text, sorted by ordinal. 1..35 languages, each "*" or a lower-case tag, each
    /// text 1..4096 bytes of valid UTF-8. A plain string is { "*": text }.</summary>
    internal static IReadOnlyList<KeyValuePair<string, string>> Text(UiText? text, string field)
    {
        if (text == null) throw new MenuError(field, "a text is needed: a string, or one per language");
        var languages = text.Languages.ToList();
        if (languages.Count is < 1 or > MaxLanguages) throw new MenuError(field, $"{languages.Count} languages: 1 to {MaxLanguages}");
        var list = new List<KeyValuePair<string, string>>(languages.Count);
        foreach (var language in languages.Order(StringComparer.Ordinal))
        {
            if (!Language.IsMatch(language)) throw new MenuError(field, $"\"{language}\" is not a language: \"*\" or a tag such as \"en\" or \"pt-br\"");
            var t = text.For(language);
            int bytes;
            try { bytes = Strict.GetByteCount(t); }
            catch (EncoderFallbackException) { throw new MenuError(field, $"the \"{language}\" text is not valid Unicode"); }
            if (bytes is < 1 or > MaxTextBytes) throw new MenuError(field, $"the \"{language}\" text is {bytes} bytes: 1 to {MaxTextBytes}");
            list.Add(new(language, t));
        }
        return list;
    }

    // ---- a menu's checks ------------------------------------------------------------------------------------------

    /// <summary>Checks a menu and derives its ids. <paramref name="stationMod"/>: the mod an unqualified recipe key belongs to (the station's).
    /// <paramref name="path"/>: the field the tabs are under ("menu." in a block, "definition." for StationMenus.Define).</summary>
    internal static Menu Check(string definingMod, string definitionKey, string stationMod, IReadOnlyList<StationMenuTabBlock>? tabs, string path)
    {
        if (KeyProblem(definitionKey, "the definition's key") is { } kp) throw new MenuError(path + "key", kp);
        if (tabs == null || tabs.Count == 0)
            throw new MenuError(path + "tabs", "a menu needs at least one tab: for a menu that lists nothing, give one tab with one empty set");
        if (tabs.Count > MaxTabs) throw new MenuError(path + "tabs", $"{tabs.Count} tabs: at most {MaxTabs}");
        var tabKeys = new HashSet<string>(StringComparer.Ordinal);
        var listed = new Dictionary<string, string>(StringComparer.Ordinal);
        var ids = new HashSet<Guid>();
        var menuId = BlockIds.Derive(definingMod, "menu-layout", definitionKey);
        ids.Add(menuId);
        var outTabs = new List<Tab>(tabs.Count);
        for (var i = 0; i < tabs.Count; i++)
        {
            var t = tabs[i];
            var tp = $"{path}tabs[{i}]";
            if (t == null) throw new MenuError(tp, "a tab is null");
            if (KeyProblem(t.Key, "the tab's key") is { } p) throw new MenuError(tp + ".key", p);
            if (!tabKeys.Add(t.Key)) throw new MenuError(tp + ".key", $"two tabs have the key \"{t.Key}\"");
            var name = Text(t.Name, tp + ".name");
            if (t.IconFromTab is < -1 or > MaxIcon)
                throw new MenuError(tp + ".iconFromTab", $"{t.IconFromTab} is outside -1..{MaxIcon}: the n-th tab (0 the first) of the menu the station is like, -1 for its first");
            if (t.Sets == null || t.Sets.Count == 0)
                throw new MenuError(tp + ".sets", $"tab \"{t.Key}\" needs at least one set: for a tab that lists nothing, give one set with no recipes");
            if (t.Sets.Count > MaxSets) throw new MenuError(tp + ".sets", $"{t.Sets.Count} sets: at most {MaxSets} in a tab");
            var setKeys = new HashSet<string>(StringComparer.Ordinal);
            var count = 0;
            var outSets = new List<Set>(t.Sets.Count);
            for (var j = 0; j < t.Sets.Count; j++)
            {
                var s = t.Sets[j];
                var sp = $"{tp}.sets[{j}]";
                if (s == null) throw new MenuError(sp, "a set is null");
                if (KeyProblem(s.Key, "the set's key") is { } sk) throw new MenuError(sp + ".key", sk);
                if (!setKeys.Add(s.Key)) throw new MenuError(sp + ".key", $"two sets of tab \"{t.Key}\" have the key \"{s.Key}\"");
                if (j == 0 && s.Heading != null) throw new MenuError(sp + ".heading", "the first set of a tab has no heading: the tab's name heads it");
                var heading = s.Heading == null ? null : Text(s.Heading, sp + ".heading");
                if (s.Recipes == null) throw new MenuError(sp + ".recipes", "recipes are needed: an empty list for a set that lists nothing");
                var refs = new List<Ref>(s.Recipes.Count);
                for (var k = 0; k < s.Recipes.Count; k++)
                {
                    var rp = $"{sp}.recipes[{k}]";
                    var v = s.Recipes[k].Value;
                    if (RefProblem(v) is { } bad) throw new MenuError(rp, bad);
                    var colon = v.IndexOf(':');
                    var r = colon < 0 ? new Ref(stationMod, v, rp) : new Ref(v[..colon], v[(colon + 1)..], rp);
                    if (listed.TryGetValue(r.Qualified, out var first)) throw new MenuError(rp, $"\"{r.Qualified}\" is listed twice (at {first} too): a recipe shows once in a menu");
                    listed[r.Qualified] = rp;
                    refs.Add(r);
                }
                count += refs.Count;
                if (count > MaxRecipes) throw new MenuError(sp + ".recipes", $"tab \"{t.Key}\" lists {count} recipes so far: at most {MaxRecipes} in a tab");
                var setId = BlockIds.Derive(definingMod, "menu-set", $"{definitionKey}.{t.Key}.{s.Key}");
                if (!ids.Add(setId)) throw new MenuError(sp + ".key", $"the set's id {setId} is taken in this menu: give it another key");
                outSets.Add(new Set(s.Key, setId, heading, refs));
            }
            var tabId = BlockIds.Derive(definingMod, "menu-tab", $"{definitionKey}.{t.Key}");
            if (!ids.Add(tabId)) throw new MenuError(tp + ".key", $"the tab's id {tabId} is taken in this menu: give it another key");
            outTabs.Add(new Tab(t.Key, tabId, name, t.IconFromTab < 0 ? 0 : t.IconFromTab, outSets));
        }
        return new Menu(definingMod, definitionKey, menuId, outTabs);
    }

    // ---- the MenuKey binding table --------------------------------------------------------------------------------

    /// <summary>A recipe with a MenuKey that runs at a station's own workshop: the mod that owns it, its key, and what it was lowered to.
    /// Live while the ledger that committed it still has the owner it had then (a reload or an unload makes it stale).</summary>
    internal sealed record Binding(Guid Workshop, string Mod, string Key, string Name, Guid Recipe, ModBlocks Ledger, ulong Owner)
    {
        public uint Id => BlockIds.Fnv(Recipe);
        public bool Live => Ledger.Owner == Owner && !Ledger.Revoked;
    }

    private static readonly object Gate = new();
    private static readonly Dictionary<Guid, Binding> Table = new();

    /// <summary>A recipe with a MenuKey was committed (a station's list, an item's recipe, a recipe block): process-wide, by its GUID, so a
    /// recipe compiled again replaces its row.</summary>
    internal static void Bind(Binding b)
    {
        lock (Gate) Table[b.Recipe] = b;
        StationMenuRuntime.BindingsChanged();
    }

    /// <summary>The live rows of the table.</summary>
    internal static List<Binding> LiveBindings()
    {
        lock (Gate) return Table.Values.Where(b => b.Live).ToList();
    }

    internal static void ResetForTests()
    {
        lock (Gate) Table.Clear();
    }

    // ---- resolving to the record ----------------------------------------------------------------------------------

    internal enum Outcome { Record, Pending, Refused }

    /// <summary>The record (Outcome.Record), or why not: <c>PendingContent</c> (a recipe is bound but not registered in the game yet) or a
    /// refusal (the field and what is wrong). <see cref="Pruned"/>: references left out because their recipe is gone.</summary>
    internal readonly record struct Resolution(Outcome Outcome, string Record, string Problem, int Pruned);

    /// <summary>Binds every reference of <paramref name="menu"/> and writes the record. <paramref name="stateOf"/>: a recipe's state in
    /// kiln.content (registered, pending, orphaned, error; null: not there yet). <paramref name="prune"/>: a menu accepted before, being
    /// brought up to date: a reference whose recipe is gone is left out (a key that comes back is bound again), not refused.</summary>
    internal static Resolution Resolve(Menu menu, Station station, IReadOnlyList<Binding> live, Func<Guid, string?> stateOf, bool prune)
    {
        var pruned = 0;
        string? pending = null;
        var ids = new HashSet<uint>();
        var tabs = new List<(Tab Tab, List<(Set Set, List<(string Key, uint Id)> Recipes)> Sets)>(menu.Tabs.Count);
        foreach (var tab in menu.Tabs)
        {
            var sets = new List<(Set, List<(string, uint)>)>(tab.Sets.Count);
            foreach (var set in tab.Sets)
            {
                var recipes = new List<(string, uint)>(set.Recipes.Count);
                foreach (var r in set.Recipes)
                {
                    var hits = live.Where(b => string.Equals(b.Mod, r.Mod, StringComparison.OrdinalIgnoreCase) && b.Key == r.Key).ToList();
                    var here = hits.Where(b => b.Workshop == station.WorkshopGuid).OrderBy(b => b.Name, StringComparer.Ordinal).ToList();
                    if (here.Count > 1)
                        return Refused($"{r.Field}: \"{r.Qualified}\" names {here.Count} recipes at this station: {string.Join(" and ", here.Select(b => b.Name))}; give each its own menuKey");
                    if (here.Count == 0)
                    {
                        if (prune) { pruned++; continue; }
                        if (hits.Count > 0)
                            return Refused($"{r.Field}: \"{r.Qualified}\" is {hits[0].Name}, which runs at another workshop, not at station {station.Mod}:{station.Key}");
                        return Refused($"{r.Field}: no recipe of mod {r.Mod} at station {station.Mod}:{station.Key} has the menuKey \"{r.Key}\" (a menu lists a recipe by its menuKey only, never by what it makes or by its name)");
                    }
                    var b = here[0];
                    switch (stateOf(b.Recipe))
                    {
                        case "registered":
                            break;
                        case "orphaned" or "error" when prune:
                            pruned++;
                            continue;
                        case "orphaned":
                            return Refused($"{r.Field}: {b.Name} is no longer in the game (its mod went)");
                        case "error":
                            return Refused($"{r.Field}: {b.Name} could not be registered in the game");
                        default:
                            pending ??= $"{PendingContent}: {b.Name} is not in the game yet";
                            continue;
                    }
                    if (!ids.Add(b.Id)) return Refused($"{r.Field}: {b.Name} would be in the menu twice");
                    recipes.Add((r.Qualified, b.Id));
                }
                sets.Add((set, recipes));
            }
            tabs.Add((tab, sets));
        }
        if (pending != null) return new(Outcome.Pending, "", pending, pruned);
        var record = Write(menu, station, tabs);
        if (Encoding.UTF8.GetByteCount(record) > MaxRecordBytes) return Refused($"the menu's record is {Encoding.UTF8.GetByteCount(record)} bytes: at most 1 MiB");
        return new(Outcome.Record, record, "", pruned);

        Resolution Refused(string problem) => new(Outcome.Refused, "", problem, pruned);
    }

    // ---- the canonical record ---------------------------------------------------------------------------------------

    private static string Write(Menu menu, Station station, List<(Tab Tab, List<(Set Set, List<(string Key, uint Id)> Recipes)> Sets)> tabs)
    {
        var sb = new StringBuilder(512);
        sb.Append("{\"baseWorkshopId\":").Append(station.BaseWorkshopId.ToString(CultureInfo.InvariantCulture));
        sb.Append(",\"definition\":{\"id\":");
        Str(sb, menu.Id.ToString("D"));
        sb.Append(",\"key\":");
        Str(sb, menu.Key);
        sb.Append(",\"mod\":");
        Str(sb, menu.Mod);
        sb.Append("},\"format\":1,\"origin\":{\"mod\":");
        Str(sb, station.Mod);
        sb.Append(",\"stationKey\":");
        Str(sb, station.Key);
        sb.Append(",\"workshopGuid\":");
        Str(sb, station.WorkshopGuid.ToString("D"));
        sb.Append(",\"workshopId\":").Append(station.WorkshopId.ToString(CultureInfo.InvariantCulture));
        sb.Append("},\"tabs\":[");
        for (var i = 0; i < tabs.Count; i++)
        {
            var (tab, sets) = tabs[i];
            if (i > 0) sb.Append(',');
            sb.Append("{\"iconFromTab\":").Append(tab.Icon.ToString(CultureInfo.InvariantCulture)).Append(",\"id\":");
            Str(sb, tab.Id.ToString("D"));
            sb.Append(",\"key\":");
            Str(sb, tab.Key);
            sb.Append(",\"name\":");
            Map(sb, tab.Name);
            sb.Append(",\"sets\":[");
            for (var j = 0; j < sets.Count; j++)
            {
                var (set, recipes) = sets[j];
                if (j > 0) sb.Append(',');
                sb.Append("{\"heading\":");
                if (set.Heading == null) sb.Append("null");
                else Map(sb, set.Heading);
                sb.Append(",\"id\":");
                Str(sb, set.Id.ToString("D"));
                sb.Append(",\"key\":");
                Str(sb, set.Key);
                sb.Append(",\"recipes\":[");
                for (var k = 0; k < recipes.Count; k++)
                {
                    if (k > 0) sb.Append(',');
                    sb.Append("{\"key\":");
                    Str(sb, recipes[k].Key);
                    sb.Append(",\"recipeId\":").Append(recipes[k].Id.ToString(CultureInfo.InvariantCulture)).Append('}');
                }
                sb.Append("]}");
            }
            sb.Append("]}");
        }
        sb.Append("]}");
        return sb.ToString();
    }

    /// <summary>A language map, its keys in ordinal order of their UTF-8 bytes (ASCII keys: the same as ordinal of the string).</summary>
    private static void Map(StringBuilder sb, IReadOnlyList<KeyValuePair<string, string>> map)
    {
        sb.Append('{');
        var first = true;
        foreach (var (k, v) in map.OrderBy(x => x.Key, StringComparer.Ordinal))
        {
            if (!first) sb.Append(',');
            first = false;
            Str(sb, k);
            sb.Append(':');
            Str(sb, v);
        }
        sb.Append('}');
    }

    /// <summary>A JSON string as nlohmann::json's dump writes it with ensure_ascii=false: quote, backslash and the control characters
    /// escaped (\b \f \n \r \t, else \u00xx in lower-case hex), everything else as it is (raw UTF-8 once encoded).</summary>
    internal static void Str(StringBuilder sb, string s)
    {
        sb.Append('"');
        foreach (var c in s)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\b': sb.Append("\\b"); break;
                case '\f': sb.Append("\\f"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                default:
                    if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                    else sb.Append(c);
                    break;
            }
        }
        sb.Append('"');
    }
}
