// SPDX-License-Identifier: MIT
// Lab-only: the live half of the registry proof producer (kilnctl regproof; regmap/README.md "The proof"). Never shipped
// (tools/package/stage.sh builds no mods/tests) and never a mod-author example: it reaches kiln.registry.live through the same
// internal bridge as registry-live-native-test, and reads Kiln's regmap, which no mod should.
//
// With <kiln>/data/content/registry/proof/request.json, at the world's first content ready, for every mapped field of the live
// registries (items, recipes, loot, drops) it takes one entry that reads, and:
//   - reads the value through the schema (kiln.registry.live read), the row's members raw (Content.* by the game's member
//     paths), and the SHA-256 of the entry's resource bytes (kiln.assets);
//   - writes a probe value through kiln.registry.live (begin, submit, commit: the public mapping guard), reads it back
//     through the schema and raw again;
//   - reverts the change through kiln.registry.live and hashes the resource bytes again;
// then writes content/registry/proof/roundtrip-<side>-live.json (registry-roundtrip format 1, generation live, regmapDigest).
// Read-only fields are read only. A field it finds no probe for gets no sample (kilnctl calls it unproven, never proven), and
// the log says why no entry gave one ("registry-proof-test: no sample of <field>": read refusals, no probe...).
// The boot generation and the values export are kiln.content's (registry/proof.cpp, the same request file).
using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Kiln;

namespace RegistryProofTest;

public sealed unsafe class RegistryProofTestMod : Mod
{
    [StructLayout(LayoutKind.Sequential)] private struct Str { public byte* Ptr; public nuint Len; }
    [StructLayout(LayoutKind.Sequential)] private struct Reply
    {
        public uint Size, Outcome, Accepted, Refused, Skipped, Reserved;
        public ulong World, Generation;
        public fixed byte Reason[192];
    }
    private const uint Manual = 4, Applied = 0, Refused = 2, Reverted = 5;
    private static readonly string[] Live = { "items", "recipes", "loot", "drops" };
    private byte* _table;
    private ulong _owner;
    private int _pass, _fail;
    private bool _ran;
    private string _kiln = "";

    private void Check(bool ok, string text)
    {
        if (ok) ++_pass; else ++_fail;
        Log.Info($"registry-proof-test: {(ok ? "PASS" : "FAIL")} {text}");
    }

    public override void OnLoad() => Content.OnReady(e =>
    {
        if (_ran || e.Reload) return;
        _ran = true;
        try { Run(); }
        catch (Exception x) { Check(false, x.ToString()); }
        finally { Log.Info($"registry-proof-test: SUMMARY registry-proof-test: {_pass} PASS, {_fail} FAIL"); }
    });

    // ---- the native table ----
    private static Str S(byte[] b, byte* p) => new() { Ptr = p, Len = (nuint)b.Length };

    private string? Read(string registry, string id, string field) => Read(registry, id, field, out _);

    private string? Read(string registry, string id, string field, out int rc)
    {
        var r = Encoding.UTF8.GetBytes(registry); var i = Encoding.UTF8.GetBytes(id); var f = Encoding.UTF8.GetBytes(field);
        var fn = *(delegate* unmanaged[Cdecl]<ulong, Str, Str, Str, byte*, nuint, nuint*, int>*)(_table + 40);
        rc = -1;
        for (var cap = 4096; cap <= 1 << 24; cap *= 8)
        {
            var buf = new byte[cap];
            nuint len = 0;
            fixed (byte* rp = r, ip = i, fp = f, bp = buf) rc = fn(_owner, S(r, rp), S(i, ip), S(f, fp), bp, (nuint)cap, &len);
            if (rc == 0) return Encoding.UTF8.GetString(buf, 0, (int)Math.Min(len, (nuint)(cap - 1)));
            if (len < (nuint)cap) return null; // an error, not a short buffer
        }
        return null;
    }

    private (bool Ok, string Why, bool ExactRefusal) Change(string registry, string id, string field, JsonNode value, out ulong handle)
    {
        handle = 0;
        ulong h = 0;
        var begin = *(delegate* unmanaged[Cdecl]<ulong, uint, ulong*, int>*)(_table + 8);
        var rc = begin(_owner, Manual, &h);
        if (rc != 0 || h == 0) return (false, $"begin rc={rc}", false);
        var ir = new JsonObject { ["ir"] = 1, ["mod"] = "registry-proof-test", ["stage"] = "alter", ["source"] = "registry-proof",
            ["edits"] = new JsonArray(new JsonObject { ["registry"] = registry, ["select"] = id, ["field"] = field, ["op"] = value.DeepClone() }) };
        var bytes = Encoding.UTF8.GetBytes(ir.ToJsonString());
        var reply = new Reply { Size = (uint)sizeof(Reply) };
        var submit = *(delegate* unmanaged[Cdecl]<ulong, ulong, Str, Reply*, byte*, nuint, nuint*, int>*)(_table + 16);
        fixed (byte* p = bytes) rc = submit(_owner, h, S(bytes, p), &reply, null, 0, null);
        if (rc != 0 || reply.Accepted != 1) { Finish(h, true); return (false, $"submit rc={rc} accepted={reply.Accepted} refused={reply.Refused}: {Why(reply)}", IsExactRefusal(reply)); }
        var commit = Finish(h, false);
        if (commit.Outcome != Applied) { Finish(h, true); return (false, $"commit outcome={commit.Outcome}: {Why(commit)}", IsExactRefusal(commit)); }
        handle = h;
        return (true, "", false);
    }

    private Reply Finish(ulong handle, bool revert)
    {
        var reply = new Reply { Size = (uint)sizeof(Reply) };
        var fn = *(delegate* unmanaged[Cdecl]<ulong, ulong, Reply*, int>*)(_table + (revert ? 32 : 24));
        fn(_owner, handle, &reply);
        return reply;
    }

    private static bool IsExactRefusal(Reply r) => r.Outcome == Refused && Why(r).StartsWith("not_exact: ", StringComparison.Ordinal);

    private static string Why(Reply r) => Encoding.UTF8.GetString(r.Reason, 192).TrimEnd('\0');

    // ---- the game's side ----
    private static ContentRegistry RegistryOf(string name) => name switch
    {
        "items" => Content.Items, "recipes" => Content.Recipes, "loot" => Content.Loot, _ => Content.Drops,
    };

    private static AssetKey? ResourceOf(string registry, ContentEntry e)
    {
        return registry switch
        {
            "items" => new AssetKey(e.Guid, "keen::ItemInfo"),
            "drops" => new AssetKey(e.Guid, "keen::ecs::DefaultInventoryResource"),
            "recipes" => First("keen::RecipeRegistryResource"),
            _ => First("keen::LootableItemsResource"),
        };
        static AssetKey? First(string type)
        {
            var keys = Assets.ListResources(Assets.TypeHash(type));
            return keys.Length > 0 ? keys[0] : null;
        }
    }

    private static string? BytesSha(AssetKey? key)
    {
        if (key is not { } k) return null;
        var b = Assets.ReadResource(k);
        return b is null ? null : Convert.ToHexStringLower(SHA256.HashData(b));
    }

    /// <summary>A member read raw by the game's path: a number, a bool, an enum's name, a GUID; null when the path is not
    /// a scalar the content reader takes (then the sample says so and kilnctl does not count it as compared).</summary>
    private static JsonNode? Raw(ContentEntry e, string path)
    {
        var tries = RawPaths(path);
        for (var i = 0; i < tries.Count; ++i)
        {
            ContentValue v;
            try { v = e.Get(tries[i]); }
            catch (Exception) { continue; }
            if (i > 0 && v.Type != ContentValueType.Bool) return null; // the fallback only stands for a one-flag struct
            return RawValue(v);
        }
        return null;
    }

    /// <summary>The paths a member is read raw by, in order: the path itself, and for "<c>x.isSet</c>" also "<c>x</c>",
    /// where reflection exposes a struct whose only scalar is that flag (GemSlotOverride {isSet}, items.socket), as
    /// kiln.registry.live binds it.</summary>
    private static IReadOnlyList<string> RawPaths(string path)
    {
        const string flag = ".isSet";
        return path.EndsWith(flag, StringComparison.Ordinal) && path.Length > flag.Length
            ? new[] { path, path[..^flag.Length] } : new[] { path };
    }

    private static JsonNode? RawValue(ContentValue v)
    {
        try
        {
            return v.Type switch
            {
                ContentValueType.Bool => JsonValue.Create(v.Bool),
                ContentValueType.Int when v.Name is { } n => n.Contains(',') ? new JsonArray(n.Split(',').Select(x => (JsonNode)JsonValue.Create(x.Trim())!).ToArray()) : JsonValue.Create(n),
                ContentValueType.Int => JsonValue.Create(v.Integer),
                ContentValueType.Float => JsonValue.Create(v.Number),
                ContentValueType.Time => JsonValue.Create((long)Math.Round(v.Number * 1e9)),
                ContentValueType.Guid => JsonValue.Create(v.Guid.ToString()),
                _ => null,
            };
        }
        catch (Exception) { return null; }
    }

    /// <summary>A count of a recipe's first input or output stack (<c>input[0]</c>, <c>output[0]</c>), which Content.Get does
    /// not address: the stacks as the game has them now (ContentEntry.Inputs / Outputs, kiln.content's read of the game's
    /// lists); null for any other member (not read, never guessed).</summary>
    private static JsonNode? StackRaw(ContentEntry e, string list, string leaf)
    {
        try
        {
            var stacks = list switch { "input" => e.Inputs, "output" => e.Outputs, _ => null };
            return stacks is { Count: > 0 } && leaf == "count" ? JsonValue.Create((long)stacks[0].Count) : null;
        }
        catch (Exception) { return null; }
    }

    // ---- probes: another value the field takes, in Kiln's terms ----
    private static readonly HashSet<string> Compared = new() { "int", "float", "bool", "time", "enum", "flags", "optionalEnum", "formula" };

    private static JsonNode? Probe(JsonNode? v, JsonObject row, IReadOnlyList<string> otherIds)
    {
        var conv = (string?)row["conv"] ?? "";
        var values = (row["values"] as JsonObject)?.Select(p => p.Key).ToList() ?? new List<string>();
        switch (conv)
        {
            case "int": case "stackCount": case "formula":
                return v is JsonValue n && n.TryGetValue<double>(out var d) ? JsonValue.Create((long)d + 1) : null;
            case "impactConfig": // float-config probe: fractional even when the current value happens to be whole
                return v is JsonValue ic && ic.TryGetValue<double>(out var c) ? JsonValue.Create(Math.Floor(c) + (c == Math.Floor(c) + 0.5 ? 1.5 : 0.5)) : null;
            case "float":
                return v is JsonValue f && f.TryGetValue<double>(out var x) ? JsonValue.Create(x == 0 ? 1.0 : Math.Round(x * 1.25, 4)) : null;
            case "time":
                return v is JsonValue t && t.TryGetValue<double>(out var s) ? JsonValue.Create(s + 1) : null;
            case "bool":
                return v is JsonValue b && b.TryGetValue<bool>(out var bb) ? JsonValue.Create(!bb) : null;
            case "enum":
                return values.FirstOrDefault(k => k != (string?)v) is { } other ? JsonValue.Create(other) : null;
            case "optionalEnum":
                return (string?)v == "none" ? (values.FirstOrDefault(k => k != "none") is { } o ? JsonValue.Create(o) : null) : JsonValue.Create("none");
            case "flags":
            {
                var have = (v as JsonArray)?.Select(x => (string?)x).Where(x => x != null).Cast<string>().ToList() ?? new List<string>();
                var add = values.FirstOrDefault(k => !have.Contains(k));
                var next = add != null ? have.Append(add).ToList() : have.Skip(1).ToList();
                return new JsonArray(next.Select(x => (JsonNode)JsonValue.Create(x)!).ToArray());
            }
            case "unlock":
                return v is JsonValue u && u.TryGetValue<string>(out var us) ? JsonValue.Create(us == "always" ? "game" : "always") : null;
            case "ref": // another id of the referenced table (items, input categories); a reference without ids at hand gets no probe
                return v is JsonValue rv && rv.TryGetValue<string>(out var cur) && otherIds.FirstOrDefault(i => i != cur) is { } r ? JsonValue.Create(r) : null;
            case "refList": // the same list with its last reference swapped for another of the table (a count change is not probed)
            {
                if (v is not JsonArray a || a.Count == 0 || a[^1] is not JsonValue lv || !lv.TryGetValue<string>(out var last)) return null;
                if (otherIds.FirstOrDefault(i => i != last) is not { } swap) return null;
                var copy = (JsonArray)a.DeepClone();
                copy[^1] = JsonValue.Create(swap);
                return copy;
            }
            case "list": case "inventoryWalk":
            {
                if (v is not JsonArray a || a.Count == 0) return null;
                var copy = (JsonArray)a.DeepClone();
                if (copy[0] is JsonObject el)
                {
                    foreach (var (k, leaf) in el.ToList())
                        if (leaf is JsonValue lv && lv.TryGetValue<double>(out var num) && k is "count" or "min" or "max" or "weight" or "chance")
                        {
                            el[k] = num == Math.Floor(num) ? JsonValue.Create((long)num + 1) : JsonValue.Create(Math.Round(num * 1.25, 4));
                            return copy;
                        }
                    return null;
                }
                if (copy[0] is not JsonValue ev || !ev.TryGetValue<double>(out var e0)) return null;
                copy[0] = e0 == Math.Floor(e0) ? JsonValue.Create((long)e0 + 1) : JsonValue.Create(Math.Round(e0 * 1.25, 4));
                return copy;
            }
            default: return null; // anything else: no probe here (unproven, never guessed)
        }
    }

    private static string Digest(string common)
    {
        var sb = new StringBuilder();
        foreach (var f in Directory.GetFiles(common, "*.json").Select(Path.GetFileName).Order(StringComparer.Ordinal))
            sb.Append(f).Append('\n').Append(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(Path.Combine(common, f!))))).Append('\n');
        return Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(sb.ToString())));
    }

    private void Run()
    {
        _kiln = Path.GetFullPath(Path.Combine(Context.Info.Directory, "..", ".."));
        var proof = Path.Combine(_kiln, "data", "content", "registry", "proof");
        if (!File.Exists(Path.Combine(proof, "request.json")))
        {
            Log.Info("registry-proof-test: no content/registry/proof/request.json: nothing to prove (lab only)");
            return;
        }
        var bridge = typeof(Content).Assembly.GetType("Kiln.BlocksBridge");
        _table = (byte*)(nint)(bridge?.GetMethod("ServiceTable", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)?
            .Invoke(null, new object[] { "kiln.registry.live", 1u }) ?? (nint)0);
        _owner = (ulong)(typeof(ModContext).GetProperty("Owner", BindingFlags.NonPublic | BindingFlags.Instance)?.GetValue(Context) ?? 0UL);
        Check(_table != null && *(uint*)_table >= 56 && _owner != 0, "kiln.registry.live v1 and the mod's owner");
        if (_table == null || *(uint*)_table < 56 || _owner == 0) return;
        var stateFile = Path.Combine(_kiln, "state", "registry.json");
        if (!File.Exists(stateFile)) { Log.Info("registry-proof-test: unproven: no registry state yet"); return; }
        var state = JsonNode.Parse(File.ReadAllText(stateFile))!;
        var side = (string?)state["regmap"]?["side"] ?? "server";
        var exe = (string?)state["regmap"]?["exeSha256"] ?? "";
        var idsFile = Path.Combine(_kiln, "data", "content", "registry", $"ids-{side}-{exe}.json");
        if (!File.Exists(idsFile)) { Log.Info("registry-proof-test: unproven: no id table yet"); return; }
        var ids = JsonNode.Parse(File.ReadAllText(idsFile))!["registries"]!.AsObject();
        Check(exe.Length == 64, $"the running build {exe}");
        var samples = new JsonArray();
        foreach (var registry in Live)
        {
            var map = JsonNode.Parse(File.ReadAllText(Path.Combine(_kiln, "regmap", "common", registry + ".json")))!;
            var table = ids[registry]?.AsObject();
            if (table is null) { Log.Info($"registry-proof-test: unproven: {registry}: no id table yet"); continue; }
            // the registry's entries with their Kiln ids (GUID, else "#<32-bit id>")
            var entries = new List<(ContentEntry E, string Id)>();
            var seen = 0;
            foreach (var e in RegistryOf(registry))
            {
                var id = (string?)table[e.Guid.ToString()] ?? (string?)table[BytesText(e.Guid)] ?? (string?)table["#" + e.Id.ToString(CultureInfo.InvariantCulture)];
                if (id != null) entries.Add((e, id));
                ++seen;
            }
            Log.Info($"registry-proof-test: {registry}: {seen} game entries, {entries.Count} with a Kiln id (of {table.Count})");
            foreach (var (field, rowNode) in map["rows"]!.AsObject())
            {
                var row = rowNode!.AsObject();
                var paths = row["paths"]?.AsArray().Select(p => (string)p!).ToList() ?? new List<string>();
                if ((string?)row["state"] == "unmapped" || paths.Count == 0) continue;
                var backend = (string?)row["backend"] ?? "";
                // Never infer the config's representation from whether its current value is whole.
                foreach (var mode in (string?)row["conv"] == "impactConfig" ? new[] { "sint32", "sint32-refusal", "float" } : new[] { "" })
                {
                    var sample = SampleOf(registry, field, row, paths, backend, entries, map["rows"]!.AsObject(), ids, mode);
                    if (sample != null) samples.Add(sample);
                }
            }
        }
        var doc = new JsonObject { ["format"] = 1, ["kind"] = "registry-roundtrip", ["side"] = side, ["exeSha256"] = exe, ["generation"] = "live",
            ["regmapDigest"] = Digest(Path.Combine(_kiln, "regmap", "common")), ["constants"] = Constants(ids), ["samples"] = samples };
        Directory.CreateDirectory(proof);
        var path = Path.Combine(proof, $"roundtrip-{side}-live.json");
        File.WriteAllText(path + ".tmp", doc.ToJsonString());
        File.Move(path + ".tmp", path, true);
        Log.Info($"registry-proof-test: {(samples.Count == 0 ? "unproven: no evidence; " : "")}wrote {samples.Count} live samples to {path}");
    }

    private JsonObject Constants(JsonObject ids)
    {
        var o = new JsonObject();
        var bal = ids["balance"]?.AsObject().Select(p => (string?)p.Value).FirstOrDefault();
        if (bal is null) return o;
        foreach (var k in new[] { "gear.seed", "gear.creep", "gear.armorOffset", "gear.armorGradient" })
            if (Read("balance", bal, k) is { } t && double.TryParse(t, NumberStyles.Float, CultureInfo.InvariantCulture, out var d)) o[k] = d;
        return o;
    }

    /// <summary>One sample of a field: the first entry that reads (and, for a writable field, takes a probe).</summary>
    private JsonObject? SampleOf(string registry, string field, JsonObject row, List<string> paths, string backend,
                                 List<(ContentEntry E, string Id)> entries, JsonObject rows, JsonObject idTables, string mode = "")
    {
        var q = $"{registry}.{field}";
        var element = field.Contains("[].");
        var list = element ? field[..field.IndexOf("[].", StringComparison.Ordinal)] : "";
        var listPath = element ? ((string?)rows[list]?["paths"]?[0] ?? "") : "";
        var rawPaths = paths.Select(p => !element ? p : registry == "drops" ? $"[0].{p}" : $"{listPath}[0].{p}").ToList();
        var conv = (string?)row["conv"] ?? "";
        var leafName = element ? field[(field.IndexOf("[].", StringComparison.Ordinal) + 3)..] : field;
        // the table a reference names (the schema's target): items, input categories, perks; others get no probe
        var table = conv is "ref" or "refList" ? leafName switch { "item" => "items", "category" => "inputCategories", "perks" => "perks", _ => "" } : "";
        var ids = table.Length > 0 ? idTables[table]?.AsObject().Select(p => (string?)p.Value).Where(v => v != null).Cast<string>().Take(64).ToList() ?? new() : new List<string>();
        var config = (string?)row["config"];
        var configPath = paths[0] + "." + (config == "ManaCost" ? "0xaf236ece" : "0x727d7f78") + ".value";
        // a structured value (list, reference, unlock...) is not one scalar member: its raw view is the entry's resource bytes;
        // a scalar of a recipe's list element (outputs[].count), which Content.Get does not address, from the game's stacks
        JsonArray RawOf(ContentEntry e, AssetKey? key) =>
            new(rawPaths.Select((p, i) => Raw(e, conv == "impactConfig" ? configPath : p) ?? (registry == "recipes" && element && Compared.Contains(conv) ? StackRaw(e, listPath, paths[i]) : null)
                ?? (Compared.Contains(conv) ? null : JsonValue.Create("resource:" + BytesSha(key)))).ToArray());
        var tries = 0;
        var why = new SortedDictionary<string, int>(StringComparer.Ordinal); // why the entries tried gave no sample (the log, when none did)
        void Skip(string reason) => why[reason] = why.GetValueOrDefault(reason) + 1;
        var candidates = entries.Take(400);
        if (backend != "live" && conv == "bool") candidates = candidates.OrderByDescending(x => Read(registry, x.Id, element ? list : field) == "true");
        foreach (var (e, id) in candidates)
        {
            if (mode.Length > 0)
            {
                ContentValueType kind;
                try { kind = e.Get(configPath).Type; }
                catch (Exception) { Skip("config kind unreadable"); continue; }
                if (kind != (mode == "float" ? ContentValueType.Float : ContentValueType.Int)) continue;
            }
            var readText = Read(registry, id, element ? list : field, out var rc);
            if (readText is null) { Skip($"read rc={rc}"); continue; }
            var whole = JsonNode.Parse(readText);
            JsonNode? read = whole;
            if (element)
            {
                if (whole is not JsonArray a || a.Count == 0 || a[0] is not JsonObject el) { Skip("an empty list"); continue; }
                read = Leaf(el, leafName);
                if (read is null) { Skip("element 0 has none"); continue; }
            }
            if (read is null || (read is JsonArray ra && ra.Count == 0)) { Skip("null or empty"); continue; }
            var s = new JsonObject { ["field"] = q, ["entry"] = id, ["element"] = element ? 0 : null, ["read"] = read.DeepClone() };
            var key = ResourceOf(registry, e);
            s["rawBefore"] = RawOf(e, key);
            var before = BytesSha(key);
            s["bytesBefore"] = before;
            if (backend != "live") return s; // read-only (and none): read only
            var probe = mode == "sint32" && read is JsonValue iv && iv.TryGetValue<double>(out var integer)
                ? JsonValue.Create(integer + (integer < int.MaxValue ? 1 : -1)) : Probe(read, row, ids);
            if (mode.Length > 0) s["configKind"] = mode == "float" ? "float" : "sint32";
            if (probe is null || JsonNode.DeepEquals(probe, read)) { Skip("no probe"); continue; } // another entry may take one
            JsonNode op = probe;
            if (element)
            {
                var copy = (JsonArray)whole!.DeepClone();
                SetLeaf((JsonObject)copy[0]!, leafName, probe);
                op = copy;
            }
            var (ok, refusal, exactRefusal) = Change(registry, id, element ? list : field, op, out var handle);
            s["probe"] = probe.DeepClone();
            if (mode == "sint32-refusal")
            {
                s["expectRefused"] = true;
                s["refused"] = !ok && exactRefusal;
                s["refusalCode"] = exactRefusal ? "not_exact" : "";
                s["refusalReason"] = refusal;
                s["readBack"] = JsonNode.Parse(Read(registry, id, field) ?? "null");
                s["rawAfter"] = RawOf(e, key);
                if (ok) s["reverted"] = Finish(handle, true).Outcome == Reverted;
                s["bytesAfter"] = BytesSha(key);
                return s; // accepting 2.5 on Sint32 is changed, even if its rounded read-back happens to equal the base
            }
            if (!ok)
            {
                // An entry without this config, an unmapped row or an out-of-range probe supplies no evidence.
                // Expected Sint32 refusals above are different: their exactness contract is explicitly tested.
                Skip("probe refused: " + refusal);
                if (++tries >= 20) break;
                continue;
            }
            var back = Read(registry, id, element ? list : field);
            var backNode = back is null ? null : JsonNode.Parse(back);
            s["readBack"] = element && backNode is JsonArray ba && ba.Count > 0 && ba[0] is JsonObject bel
                ? Leaf(bel, leafName)?.DeepClone() : backNode?.DeepClone();
            s["rawAfter"] = RawOf(e, key);
            var revert = Finish(handle, true);
            s["reverted"] = revert.Outcome == Reverted;
            s["bytesAfter"] = BytesSha(key);
            return s;
        }
        Log.Info($"registry-proof-test: no sample of {q} ({entries.Count} entries): " + (why.Count == 0 ? "no entry" : string.Join(", ", why.Select(p => $"{p.Key} x{p.Value}")))
                     + (backend == "live" && why.Count == 1 && why.Keys.First().StartsWith("read rc=", StringComparison.Ordinal) ? WriteWhy(registry, entries[0].Id, field, conv) : ""));
        return null; // no entry took a probe: unproven, not a disproof
    }

    /// <summary>Why kiln.registry.live refuses a live field it reads for no entry: one write of a value the field takes (a bool's
    /// true, unlock's always), its refusal for the log; reverted at once if it applies. Never a sample.</summary>
    private string WriteWhy(string registry, string id, string field, string conv)
    {
        JsonNode? v = conv switch { "bool" => JsonValue.Create(true), "unlock" => JsonValue.Create("always"), _ => null };
        if (v is null) return "";
        var (ok, refusal, _) = Change(registry, id, field, v, out var handle);
        if (!ok) return $"; a write to {id} is refused: {refusal}";
        Finish(handle, true);
        return $"; a write to {id} applies (reverted at once): the read alone fails";
    }

    /// <summary>The GUID's 16 bytes in memory order as GUID text (kiln.content's guid_text), next to .NET's ToString,
    /// which turns the first three groups around.</summary>
    private static string BytesText(Guid g)
    {
        var h = Convert.ToHexStringLower(g.ToByteArray());
        return $"{h[..8]}-{h[8..12]}-{h[12..16]}-{h[16..20]}-{h[20..]}";
    }

    private static JsonNode? Leaf(JsonObject el, string leaf)
    {
        JsonNode? cur = el;
        foreach (var part in leaf.Split('.')) cur = (cur as JsonObject)?[part];
        return cur;
    }

    private static void SetLeaf(JsonObject el, string leaf, JsonNode value)
    {
        var parts = leaf.Split('.');
        var cur = el;
        for (var i = 0; i < parts.Length - 1; i++) cur = (JsonObject)cur[parts[i]]!;
        cur[parts[^1]] = value.DeepClone();
    }
}
