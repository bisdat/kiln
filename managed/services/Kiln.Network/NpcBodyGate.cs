// SPDX-License-Identifier: MIT
// Client and server have to have built the same authored NPC bodies (package 16f, chapter 16f §4.5): a roster NPC with a body of its
// own is a template clone whose model, skeleton, graph and motion sequences each side built at its own start, and a client whose build
// differs (or lacks the body) draws something else or nothing. Having the mod installed is not enough: what counts is what the boot build
// made Ready, and the per-body closure hash it computed (the canonical body and motion spec, the owned files' hashes and the chassis
// layout; native/services/quests). HELLO and WELCOME carry "kiln.npcbodies", the SHA-256 (lower-case hex) of the sorted lines
// "mod:npc:closure" of this side's Ready bodies joined with '\n' (empty when there is none), and "kiln.npcbodies.count"; the list itself
// goes along (at most MaxNames lines and MaxValueBytes bytes) so the refusal can name the first body that differs. A peer that sends no key (an older Kiln)
// counts as having none. A client that defined a body from code after its start (PendingRestart: NpcBodies) says so in
// "kiln.npcbodies.pending" and is refused with a restart-required reason, as the chapter's late client is. The server refuses as
// PlayerGraphGate does: modSync off is the operator's call (a warning in the log). Reads its rows from NpcBodyRows, so it needs no
// reference to Kiln.Blocks.
using System.Globalization;
using System.Security.Cryptography;
using System.Text;

namespace Kiln;

internal static class NpcBodyGate
{
    public const string KeyHash = "kiln.npcbodies", KeyCount = "kiln.npcbodies.count", KeyList = "kiln.npcbodies.list",
        KeyPending = "kiln.npcbodies.pending";
    /// <summary>At most this many lines go into the handshake; the count says how many there are.</summary>
    public const int MaxNames = 64;

    /// <summary>The list values' budget in bytes, as BootGate's: a HELLO or WELCOME value over 4096 bytes is refused by the decoder
    /// (Wire.ReadProps), which drops the whole frame. Whole lines go in until the next would pass it; the count and the hash still cover
    /// every body, and a list shorter than the count is read as incomplete (<see cref="Describe"/>).</summary>
    public const int MaxValueBytes = 2048;

    /// <summary>The sorted "mod:npc:closure" lines of these rows' Ready bodies.</summary>
    internal static List<string> Lines(IEnumerable<NpcBodyRow> rows) =>
        rows.Where(r => r.State == NpcBodyRow.Ready).Select(r => $"{r.Mod}:{r.Npc}:{r.Closure}").Distinct(StringComparer.Ordinal)
            .Order(StringComparer.Ordinal).ToList();

    /// <summary>SHA-256 of the lines joined with '\n', lower-case hex; "" for none.</summary>
    internal static string Hash(IReadOnlyCollection<string> lines) =>
        lines.Count == 0 ? "" : Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(string.Join('\n', lines))));

    /// <summary>A hash as received: 64 hex digits, lower-cased; anything else (absent, an older Kiln) is "" (no bodies).</summary>
    internal static string Normal(string? h) => h is { Length: 64 } && h.All(Uri.IsHexDigit) ? h.ToLowerInvariant() : "";

    /// <summary>What goes in HELLO and WELCOME.</summary>
    public static Dictionary<string, string> Properties() => Properties(Lines(NpcBodyRows.All), NpcBodyRows.Late);

    internal static Dictionary<string, string> Properties(IReadOnlyList<string> lines, IReadOnlyList<string> late)
    {
        var d = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            [KeyHash] = Hash(lines),
            [KeyCount] = lines.Count.ToString(CultureInfo.InvariantCulture),
        };
        if (lines.Count > 0) d[KeyList] = Within(lines);
        if (late.Count > 0) d[KeyPending] = Within(late);
        return d;
    }

    /// <summary>The first lines that fit <see cref="MaxValueBytes"/> (at most <see cref="MaxNames"/>), joined as PlayerGraphGate joins.
    /// A line that alone is longer than the budget ends the list there.</summary>
    internal static string Within(IEnumerable<string> lines)
    {
        var kept = new List<string>();
        var bytes = 0;
        foreach (var line in lines.Take(MaxNames))
        {
            var add = Encoding.UTF8.GetByteCount(PlayerGraphGate.Join([line])) + (kept.Count > 0 ? 1 : 0);
            if (bytes + add > MaxValueBytes) break;
            kept.Add(line);
            bytes += add;
        }
        return PlayerGraphGate.Join(kept);
    }

    /// <summary>Null when a client with these handshake properties built the same bodies as this server and has none waiting for a
    /// restart; else what the player reads.</summary>
    public static string? Difference(IReadOnlyDictionary<string, string> client) => Difference(Lines(NpcBodyRows.All), client);

    internal static string? Difference(IReadOnlyList<string> server, IReadOnlyDictionary<string, string> client)
    {
        var pending = PlayerGraphGate.Split(client.GetValueOrDefault(KeyPending, ""));
        if (pending.Count > 0)
            return $"Your game defined the NPC body {pending[0]}{(pending.Count > 1 ? $" and {pending.Count - 1} more" : "")} after it started: " +
                   "NPC bodies are built when the game starts, so it has to restart before it joins (restart required).";
        var theirs = Normal(client.GetValueOrDefault(KeyHash));
        if (theirs == Hash(server)) return null;
        return Describe(server, theirs, client.GetValueOrDefault(KeyList, ""), client.GetValueOrDefault(KeyCount, ""));
    }

    /// <summary>The difference in words, naming the first body (in key order) that differs. Plain text: the handshake tests read it.</summary>
    internal static string Describe(IReadOnlyList<string> server, string clientHash, string clientList, string clientCount)
    {
        var client = clientHash.Length == 0 ? [] : PlayerGraphGate.Split(clientList);
        var total = int.TryParse(clientCount, NumberStyles.None, CultureInfo.InvariantCulture, out var n) && n >= client.Count ? n : client.Count;
        var complete = clientHash.Length == 0 || total == client.Count;
        static (string Name, string Closure) Part(string line)
        {
            var at = line.LastIndexOf(':');
            return at > 0 ? (line[..at], line[(at + 1)..]) : (line, "");
        }
        var mine = server.Select(Part).GroupBy(p => p.Name, StringComparer.Ordinal).ToDictionary(g => g.Key, g => g.First().Closure, StringComparer.Ordinal);
        var theirs = client.Select(Part).GroupBy(p => p.Name, StringComparer.Ordinal).ToDictionary(g => g.Key, g => g.First().Closure, StringComparer.Ordinal);
        string? detail = null;
        foreach (var name in mine.Keys.Union(theirs.Keys).Order(StringComparer.Ordinal))
        {
            var has = theirs.TryGetValue(name, out var c);
            if (!mine.TryGetValue(name, out var s))
            {
                detail = $"your game has the NPC body {name} and this server doesn't";
                break;
            }
            if (!has)
            {
                if (!complete) continue;  // beyond the list the client sent: unknown, not missing
                detail = $"the server has the NPC body {name} and your game doesn't";
                break;
            }
            if (!string.Equals(s, c, StringComparison.Ordinal))
            {
                detail = $"the NPC body {name} is built differently on your game (another version of its mod, or other files)";
                break;
            }
        }
        string Count(int k) => k == 1 ? "1 NPC body" : $"{k} NPC bodies";
        detail ??= $"the server has {Count(server.Count)}, your game has {Count(total)}" +
                   (server.Count > 0 ? $", starting with {Part(server[0]).Name}" : "");
        return $"This server's NPC bodies don't match your game's: {detail}. Join with the same mods, and restart if you just changed them.";
    }
}
