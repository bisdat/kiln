// SPDX-License-Identifier: MIT
using System.Text.Json;
using System.Text.RegularExpressions;
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// Kiln's interpreter (kiln mode: kiln.json "shroudforge.backend": "kiln", with "preview" naming
// kiln-sfi.interpreter while it is held). The seven guided checks the client gate's row kiln-sfi.interpreter named
// (native/core/gates/sfi.json) while kiln mode was held, and since 0.2.0-dev.30.3 the mod shortcuts (sfi-keybinds, with
// the keys probe mods/tests/sf-keys-probe). Each reads what the interpreter wrote
// for this start (kiln/state/sfi.json, kiln/logs/kiln.log) and asserts it, then asks one verdict. A start that is not a
// kiln start (hosted ShroudForge, the interpreter refused or not installed, a record of an earlier start) is a skip with
// its reason, never a pass. Checks over several starts keep their part in the mod's data folder and end each part but
// the last with skip. docs/kiln/client-pass.md, "Kiln's interpreter". Orders 3900-3907.
public sealed partial class ClientPassMod
{
    private const string SfiSince = "0.2.0-dev.30.2", SfiKeysSince = "0.2.0-dev.30.3";
    private const string SfiArea = "Kiln's interpreter";
    // Quiver & Spellbook 0.8.2: the save-sensitive package of the pass (its row in schema/sfi/sfi-safety-policy.json)
    private const string SfiQuiver = "ammo-slots";
    private const string SfiQuiverLabel = "Quiver & Spellbook (ammo-slots 0.8.2)";
    private const string SfiSaveText = "Do not save while this mod is off; restore it and restart first.";
    private const string SfiSetupHelp = "docs/kiln/client-pass.md, \"Kiln's interpreter\"";
    // the files in the mod's data folder that keep the part a check over several starts has reached
    private const string SfiSettingsPart = "sfi-ui-settings", SfiRoundtripPart = "sfi-backend-roundtrip";
    // the keys probe (mods/tests/sf-keys-probe): a ShroudForge-format package with one keybind setting, F4 by default
    private const string SfiKeysProbe = "kiln-sf-keys-probe";
    // the interpreter's kiln.log line once mod shortcuts read the game window's keys (crates/sfi/kiln-sfi runtime/input.rs)
    private const string SfiKeysStarted = "keyboard shortcuts of ShroudForge-format mods read the game window's keys";

    // ------------------------------------------------------------------------------------- the interpreter's record ---

    /// <summary>One package of kiln/state/sfi.json (schema/sfi/sfi-state.schema.json), the fields the checks read.</summary>
    private sealed record SfiPackage(string Id, string? Version, string? Name, string Intent, bool? RuntimeActive,
        bool SaveSensitive, string? Skip, bool SkipLate, string? WarningTrigger, bool WarningSaveSensitive, string? WarningText)
    {
        public string Label => Name is { Length: > 0 } n ? $"{n} ({Id}{(Version is { } v ? " " + v : "")})" : Id;

        /// <summary>What Kiln's ShroudForge page should say about it, in short.</summary>
        public string State => Intent switch
        {
            "disabled" => "switched off",
            "other-side" => "for the other side, not run here",
            "absent" => "no longer in the profile",
            _ when Skip is { } s => (SkipLate ? "stopped: " : "skipped: ") + (s.Length > 160 ? s[..160] + "..." : s),
            _ => RuntimeActive switch { true => "running", false => "loaded, no runtime running", null => "loaded" },
        };
    }

    /// <summary>kiln/state/sfi.json, the fields the checks read.</summary>
    private sealed record SfiRecord(string Phase, string? Reason, string? Backend, string? LaunchId, long Pid, string? MenuBy,
        string? MenuKey, SfiPackage[] Packages, string[] Carried)
    {
        public SfiPackage? Package(string id) => Packages.FirstOrDefault(p => p.Id == id);
    }

    /// <summary>The interpreter's record (kiln/state/sfi.json), or null with why: missing, or unreadable (then a failed
    /// assertion too). The interpreter replaces it whole while the game runs: read with sharing.</summary>
    private static SfiRecord? ReadSfiRecord(CheckContext c, out string why)
    {
        var path = Path.Combine(KilnFolder(), "state", "sfi.json");
        why = "";
        if (!File.Exists(path))
        {
            why = "kiln/state/sfi.json is not there (no start of Kiln's interpreter has written it)";
            return null;
        }
        try
        {
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var doc = JsonDocument.Parse(stream);
            var r = doc.RootElement;
            var launch = Field(r, "launch");
            var packages = Items(r, "packages").Select(p =>
            {
                var skip = Field(p, "skip");
                var warning = Field(p, "warning");
                return new SfiPackage(Text(p, "id") ?? "?", Text(p, "version"), Text(p, "name"), Text(p, "intent") ?? "?",
                    Flag(p, "runtimeActive"), Flag(Field(p, "saveSensitive"), "value") == true, Text(skip, "text"),
                    Flag(skip, "late") == true, Text(warning, "trigger"), Flag(warning, "saveSensitive") == true, Text(warning, "text"));
            }).ToArray();
            var pid = Field(launch, "pid") is { ValueKind: JsonValueKind.Number } n && n.TryGetInt64(out var v) ? v : -1;
            return new SfiRecord(Text(r, "phase") ?? "?", Text(Field(r, "reason"), "text"), Text(Field(launch, "backend"), "selected"),
                Text(launch, "launchId"), pid, Text(Field(r, "menu"), "by"), Text(Field(r, "menu"), "key"), packages,
                Items(r, "carriedSaveSensitive").Select(e => Text(e, "id") ?? "?").ToArray());
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        {
            why = $"kiln/state/sfi.json cannot be read ({ex.Message})";
            c.Expect(false, "kiln/state/sfi.json can be read", ex.Message);
            return null;
        }

        static JsonElement Field(JsonElement e, string name) =>
            e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) ? v : default;
        static string? Text(JsonElement e, string name) => Field(e, name) is { ValueKind: JsonValueKind.String } s ? s.GetString() : null;
        static bool? Flag(JsonElement e, string name) => Field(e, name).ValueKind switch
        {
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            _ => null,
        };
        static IEnumerable<JsonElement> Items(JsonElement e, string name) =>
            Field(e, name) is { ValueKind: JsonValueKind.Array } a ? a.EnumerateArray().ToArray() : [];
    }

    /// <summary>The interpreter's record when it is this start's (its game process is this one), or null with why.</summary>
    private static SfiRecord? SfiThisStart(CheckContext c, out string why)
    {
        if (ReadSfiRecord(c, out why) is not { } s) return null;
        if (s.Pid != Environment.ProcessId)
        {
            why = $"kiln/state/sfi.json is of an earlier start (game process {s.Pid}, this one is {Environment.ProcessId}): " +
                  "the interpreter wrote nothing this start (is kiln/plugins/kiln-sfi.dll in the game?)";
            return null;
        }
        return s;
    }

    /// <summary>The interpreter's record of this start when this is a kiln start in which it runs. Otherwise names why,
    /// asks for skip and returns null: a hosted start or a refused one is not a failure of what the check covers, and a
    /// skip never opens the gate.</summary>
    private static async Task<SfiRecord?> NeedKilnStart(CheckContext c)
    {
        var s = SfiThisStart(c, out var why);
        if (s is not null && s.Phase is "aside" or "refused")
            why = s.Phase == "aside"
                ? $"this start runs ShroudForge mods with another backend ({s.Backend ?? "none"}), not Kiln's interpreter"
                : $"Kiln's interpreter does not run this start: {s.Reason ?? "(no reason recorded)"}";
        else if (s is not null)
        {
            c.Expect(s.Backend == "kiln", "sfi.json: this start's backend is kiln", s.Backend);
            c.Expect(s.Phase == "running", "sfi.json: phase running", s.Phase);
            var init = BootLog(c, "kiln.log")?.LastOrDefault(l => l.Contains("kiln-sfi: Runtime initialized", StringComparison.Ordinal));
            c.Expect(init is not null, "kiln.log: \"kiln-sfi: Runtime initialized\"", init);
            return s;
        }
        c.Log.Info($"client-pass: {c.Id} needs a start in kiln mode: {why}");
        c.Instruct($"This check needs a start in kiln mode ({why}). Switch the game to Kiln's interpreter ({SfiSetupHelp}), " +
                   "start again and run it there.");
        await c.Skip($"not a kiln start: {why}");
        return null;
    }

    /// <summary>The key that opens Kiln's ShroudForge page: the binding kiln.log names, else F9.</summary>
    private static string SfiPageKey(CheckContext c, out string? line)
    {
        line = BootLog(c, "kiln.log")?.LastOrDefault(l => l.Contains("toggle Kiln's ShroudForge page", StringComparison.Ordinal));
        var m = line is null ? null : Regex.Match(line, @"key (\S+) and");
        return m is { Success: true } ? m.Groups[1].Value : "F9";
    }

    /// <summary>The mods the page should list, each with its state.</summary>
    private static string SfiList(SfiRecord s)
    {
        var shown = s.Packages.Where(p => p.Intent != "absent").Take(16).Select(p => $"{p.Label}: {p.State}").ToList();
        if (s.Packages.Length > 16) shown.Add($"and {s.Packages.Length - 16} more");
        return shown.Count == 0 ? "(no ShroudForge mod in this start's record)" : string.Join("; ", shown);
    }

    /// <summary>The save-sensitive package this start warns about (Quiver &amp; Spellbook first), or null.</summary>
    private static SfiPackage? SfiSaveWarning(SfiRecord s) =>
        s.Package(SfiQuiver) is { WarningSaveSensitive: true } q ? q : s.Packages.FirstOrDefault(p => p.WarningSaveSensitive);

    /// <summary>The files under &lt;game&gt;/shroudforge with their size and time (hosted ShroudForge's folder: a kiln start
    /// writes nothing there).</summary>
    private static Dictionary<string, (long, DateTime)> SfiHostedFolder()
    {
        var dir = Path.Combine(Path.GetDirectoryName(KilnFolder()) ?? ".", "shroudforge");
        var files = new Dictionary<string, (long, DateTime)>(StringComparer.OrdinalIgnoreCase);
        if (!Directory.Exists(dir)) return files;
        try
        {
            foreach (var f in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories).Take(20000))
            {
                var i = new FileInfo(f);
                files[Path.GetRelativePath(dir, f)] = (i.Length, i.LastWriteTimeUtc);
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        return files;
    }

    /// <summary>Mod ids that two folders of &lt;game&gt;/mods hold (their mod.json or extended.mod.json "id"), each with
    /// its folders.</summary>
    private static string[] SfiDuplicateMods()
    {
        var dir = Path.Combine(Path.GetDirectoryName(KilnFolder()) ?? ".", "mods");
        if (!Directory.Exists(dir)) return [];
        var byId = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);
        foreach (var folder in Directory.EnumerateDirectories(dir))
        {
            foreach (var name in new[] { "mod.json", "extended.mod.json" })
            {
                var manifest = Path.Combine(folder, name);
                if (!File.Exists(manifest)) continue;
                try
                {
                    using var doc = JsonDocument.Parse(File.ReadAllText(manifest),
                        new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
                    if (doc.RootElement.ValueKind == JsonValueKind.Object && doc.RootElement.TryGetProperty("id", out var id) &&
                        id.ValueKind == JsonValueKind.String && id.GetString() is { Length: > 0 } key)
                    {
                        if (!byId.TryGetValue(key, out var list)) byId[key] = list = [];
                        list.Add(Path.GetFileName(folder));
                        break;
                    }
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException) { }
            }
        }
        return byId.Where(kv => kv.Value.Count > 1).Select(kv => $"{kv.Key}: {string.Join(", ", kv.Value)}").ToArray();
    }

    /// <summary>The part a check over several starts has reached, and the launch id of the start it was in.</summary>
    private sealed record SfiPart(int Part, string? LaunchId);

    private static SfiPart SfiReadPart(CheckContext c, string name)
    {
        try
        {
            var parts = File.ReadAllText(Path.Combine(c.Mod.DataDirectory, name + ".part")).Split(' ', 2, StringSplitOptions.TrimEntries);
            if (int.TryParse(parts[0], out var n) && n > 0) return new SfiPart(n, parts.ElementAtOrDefault(1) is { Length: > 0 } id ? id : null);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        return new SfiPart(0, null);
    }

    private static void SfiWritePart(CheckContext c, string name, int part, string? launchId)
    {
        Directory.CreateDirectory(c.Mod.DataDirectory);
        var file = Path.Combine(c.Mod.DataDirectory, name + ".part");
        if (part == 0) File.Delete(file);
        else File.WriteAllText(file, $"{part} {launchId}");
    }

    /// <summary>Asserts that this part runs in a later start than the part before it.</summary>
    private static void SfiNewStart(CheckContext c, SfiPart before, SfiRecord now) =>
        c.Expect(before.LaunchId is null || now.LaunchId != before.LaunchId,
            $"part {before.Part + 1} runs in a new start (another launch id than part {before.Part}'s)", $"{before.LaunchId} -> {now.LaunchId}");

    // ------------------------------------------------------------------------------------------------- the checks ---

    [ClientCheck("sfi-ui-page", "F9 opens Kiln's ShroudForge page in a kiln start, with this start's mods and their state",
        Area = SfiArea, Since = SfiSince, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3900)]
    public async Task SfiUiPage(CheckContext c)
    {
        if (await NeedKilnStart(c) is not { } s) return;
        c.Expect(s.MenuBy == "kiln", "sfi.json: Kiln's ShroudForge page answers ShroudForge's mod-menu key (menu.by kiln)",
            s.MenuBy ?? "(no menu record)");
        var key = SfiPageKey(c, out var line);
        c.Expect(line is not null, "kiln.log: \"ShroudForge has no Modloader window: key ... toggle Kiln's ShroudForge page\"", line);
        c.Instruct($"Press {key} (ShroudForge's mod-menu key; remapped in Kiln's Keys tab if you changed it). Kiln's ShroudForge " +
                   "page opens. It lists this start's ShroudForge mods with what Kiln's interpreter did with each; this start's " +
                   $"record says: {SfiList(s)}. Then press {key} again. Pass only if all of these held: (1) {key} opened Kiln's " +
                   "ShroudForge page, (2) the page listed every mod above with the same state (running, or skipped / stopped " +
                   $"with the same reason, or switched off), (3) {key} closed it again, (4) no \"ShroudForge Modloader\" window " +
                   "appeared at any time in this start. Otherwise fail with a note naming which one.");
        await c.Verdict();
    }

    [ClientCheck("sfi-ui-settings", "A setting changed on Kiln's ShroudForge page is kept and the mod uses it after a restart",
        Area = SfiArea, Since = SfiSince, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3901)]
    public async Task SfiUiSettings(CheckContext c)
    {
        var before = SfiReadPart(c, SfiSettingsPart);
        if (await NeedKilnStart(c) is not { } s) return;
        var mod = s.Packages.FirstOrDefault(p => p.Intent == "selected" && p.Skip is null &&
            (p.Id.Contains("combat", StringComparison.OrdinalIgnoreCase) || (p.Name ?? "").Contains("combat", StringComparison.OrdinalIgnoreCase)));
        var key = SfiPageKey(c, out _);
        if (before.Part == 0)
        {
            if (mod is null)
            {
                c.Instruct("This check needs combat-tweaks (trial mod 02, 0.7.2) running in this kiln start; this start's record " +
                           $"has no running combat-tweaks ({SfiList(s)}). Add it to the profile and start again.");
                await c.Skip("needs combat-tweaks running in a kiln start");
                return;
            }
            SfiWritePart(c, SfiSettingsPart, 1, s.LaunchId);
            c.Instruct($"Part 1 of 2. Press {key}, open {mod.Label}'s settings on Kiln's ShroudForge page and change one value " +
                       "you can see in the game (a damage or stamina value; write down which one, its old and its new value). " +
                       $"Close the page with {key} and open it again. Remember (1): is the new value still there? (2): does the " +
                       "page say the change applies at the next start? Then press skip, quit the game, start it again in kiln " +
                       "mode, host the same world and run this check again.");
            await c.Verdict();
            return;
        }
        SfiNewStart(c, before, s);
        c.Expect(mod is not null, "combat-tweaks runs in this kiln start", SfiList(s));
        SfiWritePart(c, SfiSettingsPart, 0, null);
        c.Instruct($"Part 2 of 2. Press {key} and open {mod?.Label ?? "combat-tweaks"}'s settings: the value you changed in part 1 " +
                   "shows its new value. Then do what makes the value show in the game (hit something, run, use stamina: what the " +
                   "setting changes). Pass only if all of these held: (1) and (2) of part 1, (3) after the restart the page " +
                   "shows the new value, (4) the mod uses the new value in the game. Otherwise fail with a note naming which one " +
                   "and the setting. Set the value back afterwards if you like.");
        await c.Verdict();
    }

    [ClientCheck("sfi-ui-actions-notices", "A mod's action button says Requested and the mod reacts once; its notification is readable",
        Area = SfiArea, Since = SfiSince, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3902)]
    public async Task SfiUiActionsNotices(CheckContext c)
    {
        if (await NeedKilnStart(c) is not { } s) return;
        var key = SfiPageKey(c, out _);
        var folder = SfiHostedFolder();
        var logStart = BootLog(c, "kiln.log")?.Length ?? 0;
        c.OnEnd(() =>
        {
            var after = SfiHostedFolder();
            var changed = after.Where(kv => !folder.TryGetValue(kv.Key, out var was) || was != kv.Value).Select(kv => kv.Key).ToArray();
            c.Expect(changed.Length == 0, "nothing was written to <game>/shroudforge while the check ran",
                string.Join(", ", changed.Take(8)));
            var failed = (BootLog(c, "kiln.log") ?? []).Skip(Math.Max(0, logStart - 1))
                .Where(l => l.Contains("ui action failed", StringComparison.Ordinal)).ToArray();
            c.Expect(failed.Length == 0, "kiln.log: no \"ui action failed\" line while the check ran", failed.FirstOrDefault());
        });
        c.Instruct($"Press {key}. On Kiln's ShroudForge page pick a running mod that has an action button (this start's mods: " +
                   $"{SfiList(s)}). Press the button once. Remember (1): does the button say \"Requested\"? (2): does the mod do " +
                   "its action once (its effect in the game or its line in kiln.log, once, not twice)? Then look for a " +
                   "notification of that mod (on the page or as a Kiln notice). Remember (3): can you read all of it? Close the " +
                   "page. Pass only if (1), (2) and (3) held; the check also asserts, as it ends, that nothing was written to " +
                   "<game>/shroudforge. Otherwise fail with a note naming which one. If no mod of this start has an action " +
                   "button, press skip and add one that has.");
        await c.Verdict();
    }

    [ClientCheck("sfi-save-sensitive", "Quiver & Spellbook off after a start with it on: the warning before Play names it",
        Area = SfiArea, Since = SfiSince, Tags = [Launcher], Needs = ClientCheckNeeds.Hosted, Order = 3903)]
    public async Task SfiSaveSensitive(CheckContext c)
    {
        if (await NeedKilnStart(c) is not { } s) return;
        if (s.Package(SfiQuiver) is not { Intent: "disabled" or "absent" } q)
        {
            var now = s.Package(SfiQuiver) is { } on ? "it is " + on.State + " in this start" : "the record of this start does not list it";
            c.Instruct($"This check needs a kiln start with {SfiQuiverLabel} switched off after a kiln start with it on ({now}). " +
                       "Quit, switch it off in the launcher, press Play (do not save in that start) and run this check there.");
            await c.Skip("Quiver & Spellbook is not off after a start with it on");
            return;
        }
        c.Expect(q.SaveSensitive, "sfi.json: ammo-slots is save-sensitive", q.Label);
        c.Expect(q.WarningTrigger == "save-sensitive-off" && q.WarningSaveSensitive,
            "sfi.json: ammo-slots carries the save-sensitive-off warning", $"{q.WarningTrigger}: {q.WarningText}");
        c.Expect(q.WarningText?.Contains(SfiSaveText, StringComparison.Ordinal) == true,
            $"sfi.json: its warning says \"{SfiSaveText}\"", q.WarningText);
        c.Instruct("Do not save in this start. Think back to the moment you pressed Play. Pass only if all of these held: " +
                   "(1) before the game started, a dialog \"Some mods will not run this start\" appeared, its lead saying " +
                   $"\"Do not save without them\", (2) it had a row for \"{SfiQuiverLabel}\" saying it was active at an " +
                   "earlier start, (3) that row said \"It keeps data in your saves: do not save a world while it is off.\" " +
                   "Otherwise fail with a note naming which one (and what the dialog said instead). Quit without saving and " +
                   "switch it back on afterwards.");
        await c.Verdict();
    }

    [ClientCheck("sfi-three-surface-warning", "The save warning in all three places: before Play, in the game, in sfi.json",
        Area = SfiArea, Since = SfiSince, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3904)]
    public async Task SfiThreeSurfaceWarning(CheckContext c)
    {
        if (await NeedKilnStart(c) is not { } s) return;
        if (SfiSaveWarning(s) is not { } w)
        {
            c.Instruct($"This check needs a kiln start with a save warning: switch {SfiQuiverLabel} off after a kiln start with " +
                       "it on, press Play (do not save in that start) and run it there. This start's record has no save-" +
                       $"sensitive warning ({SfiList(s)}).");
            await c.Skip("this start has no save-sensitive warning");
            return;
        }
        c.Expect(w.WarningText?.Contains("Do not save", StringComparison.Ordinal) == true,
            "sfi.json: the warning says \"Do not save\"", w.WarningText);
        c.Instruct("Do not save in this start. The third place is kiln/state/sfi.json: the check read it, it lists " +
                   $"{w.Label} as save-sensitive with the warning \"{w.WarningText}\". Now look at the game: open the pause " +
                   "menu, then Kiln's Mods page, then leave to the main menu and load the world again. Pass only if all of " +
                   "these held: (1) before this start the launcher's dialog \"Some mods will not run this start\" named " +
                   $"{w.Label} with \"Do not save without them\", (2) at the main menu a banner \"Do not save: ... off this " +
                   "start\" and the footer line \"... mods off: do not save\", (3) the DO NOT SAVE badge on its row in Mods with " +
                   $"\"{SfiSaveText}\", (4) a marker on Kiln's Mods entry in the pause menu, (5) the banner came back on " +
                   "entering the world again. Otherwise fail with a note naming which one. Quit without saving.");
        await c.Verdict();
    }

    [ClientCheck("sfi-warning-accessibility", "The warning before Play works with the keyboard alone and at large text; the banner reads",
        Area = SfiArea, Since = SfiSince, Tags = [Launcher], Needs = ClientCheckNeeds.Hosted, Order = 3905)]
    public async Task SfiWarningAccessibility(CheckContext c)
    {
        if (await NeedKilnStart(c) is not { } s) return;
        if (SfiSaveWarning(s) is not { } w)
        {
            c.Instruct("This check is answered in a kiln start that had the save warning before Play. Next time, with " +
                       $"{SfiQuiverLabel} switched off after a kiln start with it on: set your desktop's (or the Deck's) text " +
                       "size large, press Play and, at the dialog, use the keyboard alone: Cancel has the focus first; Tab reaches " +
                       "Continue and the list; Enter on Cancel cancels; press Play again, Escape cancels; Play again, a click " +
                       "beside the dialog cancels; on a Steam Deck, B cancels; Play once more and Continue. Then run this check " +
                       "in that start.");
            await c.Skip("this start has no save warning before Play");
            return;
        }
        c.Expect(w.WarningSaveSensitive, "sfi.json: this start had a save-sensitive warning", w.Label);
        c.Instruct("Do not save in this start. Think back to the dialog before Play (the steps: large text size; Cancel has " +
                   "the focus first; Tab to Continue and through the list; Enter on Cancel, Escape and a click beside the " +
                   "dialog each cancel; B on a Steam Deck; then Continue). Then read the banner at the main menu. Pass only if " +
                   "all of these held: (1) Cancel had the focus when the dialog opened, (2) Tab reached Continue and every row of " +
                   "the list, (3) Enter on Cancel, Escape and a click beside the dialog each cancelled the start (and B on a Deck " +
                   "if you have one: say in the note if you did not try it), (4) at large text size the dialog still read whole " +
                   "and scrolled, nothing cut off, (5) the in-game banner was readable. Otherwise fail with a note naming which one.");
        await c.Verdict();
    }

    [ClientCheck("sfi-backend-roundtrip", "kiln, then hosted, then kiln on one game: every mod runs, settings and the save warning kept",
        Area = SfiArea, Since = SfiSince, Tags = [Launcher], Needs = ClientCheckNeeds.Hosted, Order = 3906)]
    public async Task SfiBackendRoundtrip(CheckContext c)
    {
        var before = SfiReadPart(c, SfiRoundtripPart);
        var s = SfiThisStart(c, out var why);
        var q = s?.Package(SfiQuiver);
        var kiln = s is { Phase: "running", Backend: "kiln" };
        var hosted = s is { Phase: "aside", Backend: "hosted" };
        var qOn = q is { Intent: "selected", Skip: null };
        var qOff = q is { Intent: "disabled" or "absent" };
        var (want, fits) = before.Part switch
        {
            0 => ($"a kiln start with {SfiQuiverLabel} on", kiln && qOn),
            1 => ("a start with Kiln's bundled ShroudForge (hosted)", hosted),
            2 => ($"a kiln start with {SfiQuiverLabel} off", kiln && qOff),
            _ => ($"a kiln start with {SfiQuiverLabel} on again", kiln && qOn),
        };
        if (!fits)
        {
            var seen = s is null ? why : $"this start: phase {s.Phase}, backend {s.Backend ?? "none"}, " +
                                         $"{(q is null ? "Quiver & Spellbook not listed" : "Quiver & Spellbook " + q.State)}";
            c.Instruct($"Part {before.Part + 1} of 4 needs {want}; {seen}. Press skip to keep the part reached and run this check " +
                       $"again in such a start, or fail to start the check over at part 1 ({SfiSetupHelp}).");
            if (await c.Verdict() == ClientCheckVerdict.Fail) SfiWritePart(c, SfiRoundtripPart, 0, null);
            return;
        }
        if (before.Part > 0) SfiNewStart(c, before, s!);
        if (before.Part == 0)
        {
            await NeedKilnStart(c);  // its assertions: backend kiln, phase running, the runtime line
            SfiWritePart(c, SfiRoundtripPart, 1, s!.LaunchId);
            c.Instruct($"Part 1 of 4, a kiln start, on a COPIED world. Fill the quiver and the spellbook with ammo, save, and quit " +
                       "the game. In the launcher: Settings, ShroudForge card, Kiln's bundled ShroudForge, Save; Play, host the " +
                       "same world and run this check again. Press skip now.");
            await c.Verdict();
            return;
        }
        if (before.Part == 1)
        {
            c.Expect(s!.Carried.Contains(SfiQuiver), "sfi.json: the hosted start carries ammo-slots as save-sensitive",
                string.Join(", ", s.Carried));
            var hostedRecord = Path.Combine(KilnFolder(), "state", "shroudforge.json");
            var started = System.Diagnostics.Process.GetCurrentProcess().StartTime.ToUniversalTime();
            c.Expect(File.Exists(hostedRecord) && File.GetLastWriteTimeUtc(hostedRecord) >= started.AddSeconds(-5),
                "kiln/state/shroudforge.json was written by hosted ShroudForge in this start",
                File.Exists(hostedRecord) ? $"last written {File.GetLastWriteTimeUtc(hostedRecord):O}, game started {started:O}" : "not there");
            SfiWritePart(c, SfiRoundtripPart, 2, s.LaunchId);
            c.Instruct("Part 2 of 4, hosted ShroudForge. Remember (1): do your ShroudForge mods do here what they did in part 1 " +
                       "(unlocked blueprints, combat-tweaks' values, the ammo still in the quiver)? (2): are your mods' settings " +
                       "kept (the launcher and Kiln's Mods page show the values of part 1)? (3): does the launcher's Mods > Engines " +
                       "show ShroudForge's row again? Do not save. Quit, switch Quiver & Spellbook OFF in the launcher, then " +
                       "Settings, ShroudForge card, Kiln's interpreter, Save; Play (a save warning should appear: Continue), " +
                       "host the same world and run this check again. Press skip now.");
            await c.Verdict();
            return;
        }
        if (before.Part == 2)
        {
            await NeedKilnStart(c);
            c.Expect(q!.WarningTrigger == "save-sensitive-off" && q.WarningSaveSensitive,
                "sfi.json: ammo-slots is remembered through the hosted start (save-sensitive-off warning)", $"{q.WarningTrigger}: {q.WarningText}");
            SfiWritePart(c, SfiRoundtripPart, 3, s!.LaunchId);
            c.Instruct("Part 3 of 4, kiln with Quiver & Spellbook off. Remember (4): did the warning before Play name " +
                       $"{SfiQuiverLabel} although the start before was hosted? (5): does the game show the do-not-save banner? " +
                       "DO NOT SAVE. Quit without saving, switch Quiver & Spellbook back ON, Play (kiln), host the same world and " +
                       "run this check again. Press skip now.");
            await c.Verdict();
            return;
        }
        await NeedKilnStart(c);
        var duplicates = SfiDuplicateMods();
        c.Expect(duplicates.Length == 0, "no mod is in two folders of <game>/mods", string.Join("; ", duplicates));
        c.Expect(q!.WarningTrigger is null, "sfi.json: Quiver & Spellbook runs again with no warning", q.WarningText);
        SfiWritePart(c, SfiRoundtripPart, 0, null);
        c.Instruct("Part 4 of 4, kiln again. Look at the quiver and the spellbook and at your mods' settings. Pass only if all " +
                   "of these held: (1) to (3) of part 2, (4) and (5) of part 3, (6) the ammo is still there, (7) your mods' " +
                   "settings are as in part 1, (8) the mods do what they did in part 1. Otherwise fail with a note naming which " +
                   "one.");
        await c.Verdict();
    }

    /// <summary>The keys probe's lines in kiln.log (`[kiln-sf-keys-probe] probe ...`) that start with `what`.</summary>
    private static int SfiProbeLines(CheckContext c, string what) =>
        BootLog(c, "kiln.log")?.Count(l => l.Contains($"[{SfiKeysProbe}] probe {what}", StringComparison.Ordinal)) ?? 0;

    [ClientCheck("sfi-keybinds", "A ShroudForge-format mod's shortcut in a kiln start: pressed, held, released, not while Kiln's menu is open",
        Area = SfiArea, Since = SfiKeysSince, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3907)]
    public async Task SfiKeybinds(CheckContext c)
    {
        if (await NeedKilnStart(c) is not { } s) return;
        if (s.Package(SfiKeysProbe) is not { Intent: "selected", Skip: null })
        {
            var now = s.Package(SfiKeysProbe) is { } p ? "it is " + p.State + " in this start" : "this start's record does not list it";
            c.Instruct("This check needs the keys probe running in this kiln start: copy the folder mods/tests/sf-keys-probe of " +
                       $"the Kiln repository into <game>/mods, switch it on in the launcher and start again ({now}).");
            await c.Skip("needs the keys probe (mods/tests/sf-keys-probe) running in a kiln start");
            return;
        }
        var log = BootLog(c, "kiln.log");
        var started = log?.LastOrDefault(l => l.Contains(SfiKeysStarted, StringComparison.Ordinal));
        c.Expect(started is not null, $"kiln.log: \"kiln-sfi: {SfiKeysStarted}\"", started);
        var loaded = log?.LastOrDefault(l => l.Contains($"[{SfiKeysProbe}] probe loaded: shortcut ", StringComparison.Ordinal));
        c.Expect(loaded?.Contains("setting is not a declared keybind", StringComparison.Ordinal) == true,
            "kiln.log: the probe loaded, and a key that is not one of its keybind settings raised \"setting is not a declared keybind\"", loaded);
        var m = loaded is null ? null : Regex.Match(loaded, @"probe loaded: shortcut (.+?); undeclared");
        var key = m is { Success: true } ? m.Groups[1].Value : "F4";
        c.Expect(key is not ("none" or "") && !key.StartsWith("error", StringComparison.Ordinal),
            "the probe's shortcut has a label (keybind_label)", key);

        var pressed = SfiProbeLines(c, "pressed: ");
        c.Instruct($"In the world, with no menu open, press {key} once (the keys probe's shortcut). A notice \"Keys probe: {key} " +
                   $"pressed (1)\" should appear.");
        await c.Until(() => SfiProbeLines(c, "pressed: ") > pressed, TimeSpan.FromSeconds(120), $"the probe saw {key} pressed");
        var (held, released) = (SfiProbeLines(c, "held: "), SfiProbeLines(c, "released: "));
        c.Instruct($"Now hold {key} for two seconds, then let go.");
        await c.Until(() => SfiProbeLines(c, "released: ") > released && SfiProbeLines(c, "held: ") > held,
            TimeSpan.FromSeconds(120), $"the probe saw {key} held, then released");
        await c.Delay(TimeSpan.FromSeconds(1));
        var beforeMenu = SfiProbeLines(c, "pressed: ");
        c.Instruct($"Open Kiln's menu (Insert), press {key} twice while it is open, close the menu, then press {key} once more " +
                   "in the world.");
        await c.Until(() => SfiProbeLines(c, "pressed: ") > beforeMenu, TimeSpan.FromSeconds(180), $"the probe saw {key} pressed after the menu");
        await c.Delay(TimeSpan.FromSeconds(2));
        c.Instruct("Pass only if all of these held: (1) each press in the world showed the notice \"Keys probe: " + key +
                   " pressed (n)\" with n counting up, (2) holding the key showed one notice, not one per repeat, (3) the " +
                   "two presses while Kiln's menu was open showed no notice. Otherwise fail with a note naming which one.");
        await c.Verdict();
        c.Expect(SfiProbeLines(c, "pressed: ") == beforeMenu + 1,
            "kiln.log: the presses while Kiln's menu was open did not count, the one after it did",
            $"{SfiProbeLines(c, "pressed: ") - beforeMenu} presses counted from the menu step on");
    }
}
