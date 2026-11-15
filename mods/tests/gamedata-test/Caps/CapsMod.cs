// SPDX-License-Identifier: MIT
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Kiln;

namespace GamedataCaps;

/// <summary>
/// Kiln's buff room on a real server (chapter 17.2, package 17b S2-S3). OnLoad, before the world: CapsBuff_0000..1999
/// from code (they wait for the world's load, which registers them in one batch). OnReady, the world running: one
/// data-pack document with CapsBuff_2000..4096 (one batch): 4,096 register, the 4,097th is refused "full (4096
/// clones)". Then Caps_Collide, a GUID whose buff id is CapsBuff_0000's: refused, naming it. A hot reload of this mod
/// (control "reload gamedata-caps") defines them all again: no new record, no new header. A restart: all of them again,
/// in one header. PASS / FAIL lines "gamedata-caps: ...", then "gamedata-caps: done n PASS, m FAIL".
/// </summary>
public sealed class CapsMod : Mod
{
    private const int Before = 2000, Total = 4097, Room = 4096;
    private const string Base = "0x5f425faf";  // the game's poisoned debuff, as the research probe (out/caps/probes)
    // FNV-1a over Guid.ToByteArray() is 0x1a176334 for both (found by a birthday search): CapsBuff_0000 takes the id
    private static readonly Guid First = Guid.Parse("615e6c08-7d7e-5de3-a6b3-7fa8f90bd2c1");
    private static readonly Guid Collide = Guid.Parse("392bd680-569c-59c6-9516-160e1bfbadcc");

    private int _pass, _fail;
    private bool _ran;

    private static Guid GuidOf(int i)
    {
        if (i == 0) return First;
        var b = SHA1.HashData(Encoding.UTF8.GetBytes($"kiln-17b-gamedata-caps-{i}")).AsSpan(0, 16).ToArray();
        b[7] = (byte)((b[7] & 0x0f) | 0x50);  // a v5-shaped GUID in the game's byte order
        b[8] = (byte)((b[8] & 0x3f) | 0x80);
        return new Guid(b);
    }

    internal static uint BuffId(Guid g)
    {
        var h = 2166136261u;
        foreach (var x in g.ToByteArray()) h = (h ^ x) * 16777619u;
        return h;
    }

    private static string Name(int i) => $"CapsBuff_{i:D4}";

    private static GameDataClone Clone(int i) => new() { Guid = GuidOf(i), Name = Name(i), Base = Base, Fields = { ["defaultLifeTime"] = 7 } };

    private void Check(bool ok, string what)
    {
        if (ok) _pass++;
        else _fail++;
        Log.Info($"gamedata-caps: {(ok ? "PASS" : "FAIL")} {what}");
    }

    private string Marker() => Path.Combine(Context.DataDirectory, "phase.json");

    private static string ProcessStart() => Process.GetCurrentProcess().StartTime.ToUniversalTime().ToString("O");

    private string Phase()
    {
        if (!File.Exists(Marker())) return "fresh";
        using var doc = JsonDocument.Parse(File.ReadAllText(Marker()));
        var same = doc.RootElement.GetProperty("pid").GetInt32() == Environment.ProcessId &&
                   doc.RootElement.GetProperty("start").GetString() == ProcessStart();
        return same ? "reload" : "restart";
    }

    private void WriteMarker(string phase)
    {
        Directory.CreateDirectory(Context.DataDirectory);
        File.WriteAllText(Marker(), $"{{\"phase\": \"{phase}\", \"pid\": {Environment.ProcessId}, \"start\": \"{ProcessStart()}\"}}");
    }

    public override void OnLoad()
    {
        if (!Content.GameDataAvailable)
        {
            Log.Info("gamedata-caps: FAIL kiln.gamedata is not loaded");
            return;
        }
        var ids = Enumerable.Range(0, Total).Select(i => BuffId(GuidOf(i))).ToHashSet();
        if (ids.Count != Total || ids.Contains(0) || BuffId(First) != BuffId(Collide))
            Log.Info($"gamedata-caps: FAIL the fixture: {ids.Count} unique ids of {Total}, collide pair {BuffId(First):x8}/{BuffId(Collide):x8}");
        var phase = Phase();
        Log.Info($"gamedata-caps: OnLoad ({phase}): defining {Before} buff clones before the world");
        for (var i = 0; i < Before; i++) Content.Buffs.Define(Clone(i));
        Content.OnReady(_ =>
        {
            if (_ran) return;
            _ran = true;
            try
            {
                Run(phase);
            }
            catch (Exception e)
            {
                Check(false, $"aborted: {e}");
            }
            Log.Info($"gamedata-caps: done {_pass} PASS, {_fail} FAIL");
        });
    }

    private static JsonObject BuffCopy() =>
        (JsonNode.Parse(Content.GameDataStatus())?["clones"]?["buffCopy"] as JsonObject) ?? new JsonObject();

    private static long N(JsonObject o, string k) => o[k] is JsonValue v && v.TryGetValue<long>(out var n) ? n : -1;

    private static IReadOnlyList<GameDataDefinition> Caps() =>
        Content.Buffs.Definitions().Where(d => d.Name.StartsWith("CapsBuff_", StringComparison.Ordinal) || d.Name == "Caps_Collide").ToList();

    private void Run(string phase)
    {
        var copy = BuffCopy();
        var defs = Caps();
        var loaded = defs.Count(d => d.State == "registered");
        Log.Info($"gamedata-caps: after the world's load ({phase}): {loaded} registered, buffCopy {copy.ToJsonString()}");
        var room = Capabilities.Get("gamedata.expanded_buff_room");
        Check(N(copy, "room") == Room && copy["expanded"]?.GetValue<bool>() == true && room.State == CapabilityState.Available,
            $"buff room: room={N(copy, "room")} expanded={copy["expanded"]} gamedata.expanded_buff_room={room.State} ({room.ReasonText})");
        long headersBefore = N(copy, "headers"), usedBefore = N(copy, "clones");
        if (phase != "reload")
            Check(loaded == Before && headersBefore == 1 && N(copy, "attaches") == 1,
                $"buff headers: the world's load registered {loaded} of {Before} with {headersBefore} header(s), {N(copy, "attaches")} registry swap(s)");

        // one data-pack document while the world runs: one batch
        var add = new JsonArray();
        for (var i = Before; i < Total; i++)
            add.Add(new JsonObject { ["guid"] = GuidOf(i).ToString(), ["name"] = Name(i), ["base"] = Base, ["fields"] = new JsonObject { ["defaultLifeTime"] = 7 } });
        var sw = Stopwatch.StartNew();
        var report = Content.Apply(new JsonObject { ["format"] = 1, ["buffs"] = new JsonObject { ["add"] = add } }.ToJsonString(), "gamedata-caps running pack");
        sw.Stop();
        var after = BuffCopy();
        defs = Caps();
        var registered = defs.Count(d => d.Name.StartsWith("CapsBuff_", StringComparison.Ordinal) && d.State == "registered");
        var full = defs.Count(d => d.Error?.Contains($"full ({Room} clones)", StringComparison.Ordinal) == true);
        Check(defs.Count(d => d.Name.StartsWith("CapsBuff_", StringComparison.Ordinal)) == Total && registered == Room && full == 1,
            $"buff cap: definitions={defs.Count(d => d.Name.StartsWith("CapsBuff_", StringComparison.Ordinal))} registered={registered} full={full} " +
            $"(pack: {report.Errors.Count} error(s), {sw.ElapsedMilliseconds} ms)");
        foreach (var e in report.Errors.Take(3)) Log.Info($"gamedata-caps: pack error: {e}");
        var newHeaders = N(after, "headers") - headersBefore;
        Check(phase == "reload" ? newHeaders == 0 && N(after, "clones") == usedBefore : newHeaders == 1,
            $"buff headers: the running document published {newHeaders} (headers={N(after, "headers")} headerBytes={N(after, "headerBytes")} " +
            $"recordBytes={N(after, "recordBytes")} attaches={N(after, "attaches")} clones={N(after, "clones")})");

        // a GUID whose buff id is CapsBuff_0000's
        var collide = new GameDataClone { Guid = Collide, Name = "Caps_Collide", Base = Base };
        string? why;
        try
        {
            var d = Content.Buffs.Define(collide);
            why = d.State == "error" ? d.Error : null;
        }
        catch (ArgumentException e)
        {
            why = e.Message;
        }
        Check(why?.Contains("CapsBuff_0000", StringComparison.Ordinal) == true && N(BuffCopy(), "clones") == N(after, "clones"),
            $"buff id collision: Caps_Collide (id 0x{BuffId(Collide):x8}) refused: {why}");

        // the content sync's payload at this size: the state a client would be sent, and its round trip on this server
        var serving = ContentSync.Serving;
        Log.Info($"gamedata-caps: content sync: {(serving is { } s ? $"state {s.StateId:x8}, {s.Fields} change(s), {s.Bytes} bytes in {s.Parts} part(s)" : "no state served yet")}");
        var trip = ContentSync.CheckRoundTrip();
        Check(trip.Ok, $"content sync round trip with {Room} buff clones: {trip}");
        if (phase == "restart")
            Check(loaded == Before && N(copy, "headers") == 1, $"restart: the world's load registered {loaded} again with {N(copy, "headers")} header");
        if (phase == "reload")
            Check(loaded == Room && N(after, "clones") == usedBefore, $"reload: {loaded} still registered, no new record (clones={N(after, "clones")})");
        WriteMarker(phase);
    }
}
