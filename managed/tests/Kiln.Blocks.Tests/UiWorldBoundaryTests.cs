// SPDX-License-Identifier: MIT
// Chapter 09 §4.4: a joined client's renderer never calls the server-bound World, Items or Time facades. Live game values
// reach the block UI runtime only through kiln.ui.snapshot (UiWorldSource), collected on the right world's own thread.
// This reads every source file of the runtime (managed/services/Kiln.Blocks/Ui/*.cs and BlockUi*.cs) with comments and
// string literals blanked (interpolation holes are kept: they are code) and fails on any use of those facades.
using System.Runtime.CompilerServices;
using System.Text;
using System.Text.RegularExpressions;
using Xunit;

namespace Kiln.BlockTests;

public sealed class UiWorldBoundaryTests
{
    private static string Repo([CallerFilePath] string self = "")
    {
        foreach (var start in new[] { AppContext.BaseDirectory, Path.GetDirectoryName(self) ?? "" })
            for (var d = new DirectoryInfo(start); d != null; d = d.Parent)
                if (Directory.Exists(Path.Combine(d.FullName, "managed/services/Kiln.Blocks/Ui"))) return d.FullName;
        throw new DirectoryNotFoundException("managed/services/Kiln.Blocks/Ui not found");
    }

    private static IEnumerable<string> RuntimeFiles(string repo)
    {
        var blocks = Path.Combine(repo, "managed/services/Kiln.Blocks");
        return Directory.GetFiles(Path.Combine(blocks, "Ui"), "*.cs").Concat(Directory.GetFiles(blocks, "BlockUi*.cs")).Order(StringComparer.Ordinal);
    }

    // World., Items., Time. (also Kiln.- and global::-qualified) as the start of a member access: not after a word
    // character, a dot or @ (row.Items.Count, UiWorldSource.Read, DateTime.Now are not the facades).
    private static readonly Regex Facade = new(@"(?<![\w.@$])(?:global::)?(?:Kiln\s*\.\s*)?(?<name>World|Items|Time)\s*\.\s*[A-Za-z_]",
        RegexOptions.CultureInvariant);

    // using static Kiln.World; and using W = Kiln.World;
    private static readonly Regex UsingFacade = new(@"\busing\s+(?:static\s+)?(?:\w+\s*=\s*)?(?:global::)?Kiln\s*\.\s*(?<name>World|Items|Time)\s*;",
        RegexOptions.CultureInvariant);

    /// <summary>Every use of the facades in <paramref name="source"/>, as "line: text".</summary>
    internal static IReadOnlyList<string> ServerBoundUses(string source)
    {
        var code = Code(source);
        var hits = new List<string>();
        foreach (var re in new[] { Facade, UsingFacade })
            foreach (Match m in re.Matches(code))
            {
                var line = 1 + code.AsSpan(0, m.Index).Count('\n');
                hits.Add($"{line}: {m.Value.Trim()}");
            }
        return hits;
    }

    [Fact]
    public void No_block_ui_runtime_file_names_the_server_bound_World_Items_or_Time_facades()
    {
        var files = RuntimeFiles(Repo()).ToList();
        Assert.Contains(files, f => f.EndsWith("UiWorldSnapshotNative.cs", StringComparison.Ordinal));
        Assert.Contains(files, f => f.EndsWith("BlockUi.World.cs", StringComparison.Ordinal));
        var found = files.SelectMany(f => ServerBoundUses(File.ReadAllText(f)).Select(h => $"{Path.GetFileName(f)}:{h}")).ToList();
        Assert.True(found.Count == 0,
            "the block UI runtime must read live game values through kiln.ui.snapshot (UiWorldSource), never the server-bound facades:\n" +
            string.Join("\n", found));
    }

    [Fact]
    public void The_scan_finds_the_facades_in_code_and_holes_and_nowhere_else()
    {
        const string sample = """"
            // World.Position(p) in a comment, /* Items.Get */ too
            /* Time.Hours
               Items.All */
            var a = "World.Position(p)"; var b = @"Items.Get(""x"")"; var c = $"{n} Time.Now {{World.X}}";
            var d = row.Items.Count; var e = UiWorldSource.Read(); var f = DateTime.Now; var g = '"'; var h2 = '\'';
            var r = """World.Raw""";
            var h = World.Position(p);
            var i = $"{Items.Get(x)} and {(k ? "Time.Now" : "z")}";
            var j = Kiln.Time.HoursNow;
            using static Kiln.Items;
            using W = global::Kiln.World;
            """";
        var hits = ServerBoundUses(sample);
        Assert.Equal(new[] { "7: World.P", "8: Items.G", "9: Kiln.Time.H", "10: using static Kiln.Items;", "11: using W = global::Kiln.World;" }, hits);
    }

    // Comments and literal text blanked; interpolation holes kept (they are code). Newlines are kept for line numbers.
    internal static string Code(string s)
    {
        var o = new StringBuilder(s.Length);
        var i = 0;
        Lex(s, ref i, o, hole: false);
        return o.ToString();
    }

    private static bool At(string s, int i, char c) => i < s.Length && s[i] == c;

    private static void Lex(string s, ref int i, StringBuilder o, bool hole)
    {
        var depth = 0;
        while (i < s.Length)
        {
            var c = s[i];
            if (hole)
            {
                if (c == '{') depth++;
                else if (c == '}' && depth-- == 0) return;
            }
            if (c == '/' && At(s, i + 1, '/'))
            {
                while (i < s.Length && s[i] != '\n') i++;
                continue;
            }
            if (c == '/' && At(s, i + 1, '*'))
            {
                var end = s.IndexOf("*/", i + 2, StringComparison.Ordinal);
                var stop = end < 0 ? s.Length : end + 2;
                for (; i < stop; i++) o.Append(s[i] == '\n' ? '\n' : ' ');
                continue;
            }
            if (c == '\'')
            {
                i++;
                while (i < s.Length && s[i] != '\'' && s[i] != '\n') i += s[i] == '\\' ? 2 : 1;
                i++;
                o.Append("' '");
                continue;
            }
            if (c is '"' or '$' or '@')
            {
                var j = i;
                var dollars = 0;
                var verbatim = false;
                while (j < s.Length && s[j] is '$' or '@')
                {
                    if (s[j] == '$') dollars++;
                    else verbatim = true;
                    j++;
                }
                if (At(s, j, '"'))
                {
                    i = j;
                    Literal(s, ref i, o, dollars, verbatim);
                    continue;
                }
            }
            o.Append(c);
            i++;
        }
    }

    private static void Literal(string s, ref int i, StringBuilder o, int dollars, bool verbatim)
    {
        var quotes = 0;
        while (At(s, i + quotes, '"')) quotes++;
        o.Append('"');
        if (quotes >= 3)
        {
            // a raw string: ends at the same run of quotes. Its interpolation holes ({{...}} with $$) are not scanned.
            i += quotes;
            var end = s.IndexOf(new string('"', quotes), i, StringComparison.Ordinal);
            var stop = end < 0 ? s.Length : end + quotes;
            for (; i < stop; i++) if (s[i] == '\n') o.Append('\n');
            o.Append('"');
            return;
        }
        i++;  // the opening quote
        while (i < s.Length)
        {
            var c = s[i];
            if (verbatim && c == '"')
            {
                if (At(s, i + 1, '"')) { i += 2; continue; }
                i++;
                break;
            }
            if (!verbatim)
            {
                if (c == '\\') { i += 2; continue; }
                if (c == '"') { i++; break; }
                if (c == '\n') break;
            }
            if (dollars > 0 && c == '{')
            {
                if (At(s, i + 1, '{')) { i += 2; continue; }
                i++;
                o.Append(" {");
                Lex(s, ref i, o, hole: true);
                if (i < s.Length) i++;  // the hole's closing brace
                o.Append("} ");
                continue;
            }
            if (c == '\n') o.Append('\n');
            i++;
        }
        o.Append('"');
    }
}
