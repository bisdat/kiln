// SPDX-License-Identifier: MIT
// Lab test of authored roster bodies (package 16f, chapter 16f §5-6), on a dedicated server with no player. lab.sh cooks this mod with the
// lab server's files, adds npc-body-broken (the same body under another id whose glTF changed after its cook: a stale cook) and writes
// lab-expected.json beside this file from the server's game files and the cook (lab_expect.py). Then this checks what the boot pack built:
//   the status rows and their change events; the vanilla Fisher template unchanged; the clone template new, with the authored model and graph,
//   without the five human appearance producers, with the body's Size in PathWalker and its colliders; the 21 motion clones served, referenced
//   by the clone and retimed to the authored clips; a game-body NPC of the same pack; nothing of the broken neighbour; a spawned clone's live
//   graph. A second start checks the same clone GUID, NpcId and template bytes (state in the mod's data folder). Without lab-expected.json the
//   checks fail. PASS and FAIL lines in kiln.log, then "npc-body-test done: N PASS, M FAIL".
using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text.Json.Nodes;
using Kiln;

namespace NpcBodyTest;

public sealed class NpcBodyTestMod : Mod
{
    private const string Me = "npc-body-test", Broken = "npc-body-broken", Npc = "figure", Pip = "pip";
    private const uint TemplateType = 0x39768775;  // keen::ecs::TemplateResource
    private static readonly Guid Fisher = new("e7666cd3-e530-4f9d-9586-519e4ab969dc");
    private static readonly Guid FisherGraph = new("402df515-5965-4b01-a17b-17d482a91af4");
    // what the chassis' clone loses for a body of its own (chapter 16f §4.2)
    private static readonly string[] Producers = ["CharacterSetup", "NewCharacterSetup", "RenderEquipment", "EquipmentCheck", "SlotSelection"];
    // template fields that name one of the 21 motion sequences (chapter 16f §2.2)
    private static readonly string[] SequenceFields =
    [
        "ActiveNpcState.sitSequence", "ActiveNpcState.sleepSequence", "ActiveNpcState.inspectSequence", "NpcState.defaultGreetingSequence",
        "NpcActionSequences.defaultInteractionSequence", "PathWalker.jumpSequence", "PathWalker.openDoorSequence",
    ];
    private static readonly WorldPosition Pos = new(3858.0, 833.0, 1402.0);
    private readonly ConcurrentQueue<NpcBodyBuildChangedEvent> _events = new();
    private int _pass, _fail;

    private void Check(bool ok, string what)
    {
        if (ok) _pass++;
        else _fail++;
        Log.Info($"{Me}: {(ok ? "PASS" : "FAIL")} {what}");
    }

    private void Info(string s) => Log.Info($"{Me}: {s}");

    private static string Read(Func<string> read)
    {
        try { return read(); }
        catch (Exception e) when (e is ArgumentException or KeyNotFoundException or InvalidOperationException or NotSupportedException or FormatException)
        {
            return $"<{e.GetType().Name}: {e.Message}>";
        }
    }

    private static double Number(GameDataEntry t, string path)
    {
        try { return t.GetNumber(path); }
        catch (Exception e) when (e is ArgumentException or KeyNotFoundException or InvalidOperationException or NotSupportedException or FormatException)
        {
            return double.NaN;
        }
    }

    // the first of the paths that reads as a number (a keen::Time is a struct with its nanoseconds in "value")
    private static double First(GameDataEntry t, params string[] paths) =>
        paths.Select(p => Number(t, p)).FirstOrDefault(v => !double.IsNaN(v), double.NaN);

    private static bool Absent(GameDataEntry t, string component)
    {
        try { return t.GetJson(component) is not { } j || j["absent"] is not null; }
        catch (Exception e) when (e is ArgumentException or KeyNotFoundException or InvalidOperationException) { return true; }
    }

    private static string Sha(byte[]? bytes) => bytes == null ? "" : Convert.ToHexStringLower(SHA256.HashData(bytes));

    public override void OnLoad()
    {
        Events.On<NpcBodyBuildChangedEvent>(e => _events.Enqueue(e));
        Events.On<ServerStartedEvent>(ev => { _ = Run(); });
    }

    private async Task Run()
    {
        try
        {
            for (var i = 0; !Spawn.WorldRunning && i < 6000; i++) await Context.Scheduler.NextTick();
            for (var i = 0; i < 60; i++) await Context.Scheduler.NextTick();
            var expectedFile = Path.Combine(Context.Info.Directory, "lab-expected.json");
            var expected = File.Exists(expectedFile) ? JsonNode.Parse(File.ReadAllText(expectedFile)) : null;
            Check(expected != null, $"the lab's expectations are there ({expectedFile}): without them every check below fails");
            await Checks(expected ?? new JsonObject());
        }
        catch (Exception e)
        {
            Check(false, $"exception: {e}");
        }
        Log.Info($"{Me} done: {_pass} PASS, {_fail} FAIL");
    }

    private async Task Checks(JsonNode expected)
    {
        // ---- capability rows ----
        foreach (var row in new[] { "quests.boot_pack", "quests.npc_body", "blocks.npc_body" })
        {
            var c = Capabilities.Get(row);
            Check(c.State == CapabilityState.Available, $"{row} is available: {c.State} {c.ReasonText}");
        }

        // ---- status rows and events ----
        var clone = BlockIds.Derive(Me, "npc", Npc);
        var s = NpcBodies.Status($"{Me}:{Npc}");
        Info($"status {s}");
        Check(s is { Ok: true, State: NpcBodyBuildState.Ready, Reason: null } && s.BlockKey == $"{Me}:{Npc}" && s.ModId == Me,
            $"NpcBodies.Status({Me}:{Npc}) is Ready: {s.State} {s.Reason}");
        Check(s.Template == clone, $"its template is the NPC block's clone {clone}: {s.Template}");
        Check(s.Model == Guid.Parse((string?)expected["model"] ?? Guid.Empty.ToString()) && s.Model != Guid.Empty, $"its model is the cook's RenderModel {expected["model"]}: {s.Model}");
        Check(s.Graph == Guid.Parse((string?)expected["graph"] ?? Guid.Empty.ToString()) && s.Graph != Guid.Empty, $"its graph is the cook's body graph {expected["graph"]}: {s.Graph}");
        Check(s.Generation > 0, $"it has the boot build's generation: {s.Generation}");
        var broken = NpcBodies.Status($"{Broken}:{Npc}");
        Info($"broken status {broken}");
        Check(broken is { Ok: false, State: NpcBodyBuildState.Failed } && broken.Reason?.Contains("changed after the last cook", StringComparison.Ordinal) == true &&
              broken.Reason.Contains($"bootpack:{Broken}:", StringComparison.Ordinal) && broken.Template == Guid.Empty,
            $"the broken neighbour is Failed with its refusal: {broken.State} \"{broken.Reason}\"");
        var pip = NpcBodies.Status($"{Me}:{Pip}");
        Check(pip is { Ok: false, State: NpcBodyBuildState.Failed, Reason: "NoDefinition" }, $"a game-body NPC has no body record: {pip.State} {pip.Reason}");
        var none = NpcBodies.Status($"{Me}:nobody");
        Check(none is { Ok: false, State: NpcBodyBuildState.Failed, Reason: "NoDefinition" }, $"a well-formed key without a record is Failed/NoDefinition: {none.State} {none.Reason}");
        var threw = false;
        try { NpcBodies.Status("not a key"); }
        catch (ArgumentException) { threw = true; }
        Check(threw, "a key that is not mod:block throws ArgumentException");
        for (var i = 0; i < 40 && _events.Count < 2; i++) await Context.Scheduler.NextTick();
        var events = _events.ToArray();
        foreach (var e in events) Info($"event {e.Before.BlockKey}: {e.Before.State}/{e.Before.Reason} -> {e.After.State}/{e.After.Reason} client {e.ClientSide}");
        var mine = events.Where(e => e.After.BlockKey == $"{Me}:{Npc}").ToArray();
        Check(mine.Length == 1 && mine[0].Before is { State: NpcBodyBuildState.Failed, Reason: "NoDefinition" } && mine[0].After.State == NpcBodyBuildState.Ready && !mine[0].ClientSide,
            $"one change event for the body: no record -> Ready, server side ({mine.Length} event(s))");
        var theirs = events.Where(e => e.After.BlockKey == $"{Broken}:{Npc}").ToArray();
        Check(theirs.Length == 1 && theirs[0].After.State == NpcBodyBuildState.Failed, $"one change event for the broken neighbour: -> Failed ({theirs.Length} event(s))");

        // ---- the vanilla Fisher ----
        var fisherBytes = Assets.ReadResource(new AssetKey(Fisher, TemplateType));
        var fisherSha = Sha(fisherBytes);
        Info($"fisher template sha256 {fisherSha} ({fisherBytes?.Length} bytes)");
        Check(fisherSha.Length == 64 && fisherSha == (string?)expected["fisherTemplateSha256"], $"the game's Fisher template is served byte for byte as the game ships it: {fisherSha}");
        var fisher = Content.Templates.Get(Fisher);
        Check(fisher != null, "the Fisher's template is in the game's templates");
        if (fisher != null)
        {
            Check(Read(() => fisher.Get("ModelResource.model").ToString()) == Guid.Empty.ToString(), $"the Fisher's model stays zero: {Read(() => fisher.Get("ModelResource.model").ToString())}");
            Check(Read(() => fisher.Get("AnimationGraphResourceComponent2.animationGraph").ToString()) == FisherGraph.ToString(),
                $"the Fisher's graph stays the game's: {Read(() => fisher.Get("AnimationGraphResourceComponent2.animationGraph").ToString())}");
            Check(!Absent(fisher, "CharacterSetup"), "the Fisher keeps its human appearance (CharacterSetup)");
        }

        // ---- the clone ----
        var tplBytes = Assets.ReadResource(new AssetKey(clone, TemplateType));
        var tpl = Content.Templates.Get(clone);
        // a new key, not one of the game's (lab_expect.py checked the game's files), served by the boot pack
        Check(tplBytes is { Length: > 0 } && tpl != null, $"the clone template {clone} is served and in the game's templates: {tplBytes?.Length} bytes");
        var npcId = "";
        if (tpl != null)
        {
            Check(Read(() => tpl.Get("ModelResource.model").ToString()) == s.Model.ToString(), $"the clone carries the authored model: {Read(() => tpl.Get("ModelResource.model").ToString())}");
            Check(Read(() => tpl.Get("AnimationGraphResourceComponent2.animationGraph").ToString()) == s.Graph.ToString(),
                $"the clone carries the authored graph: {Read(() => tpl.Get("AnimationGraphResourceComponent2.animationGraph").ToString())}");
            var kept = Producers.Where(p => !Absent(tpl, p)).ToArray();
            Check(kept.Length == 0, $"the five human appearance producers are gone from the clone: still there [{string.Join(", ", kept)}]");
            Check(!Absent(tpl, "ActiveNpcState") && !Absent(tpl, "PathWalker") && !Absent(tpl, "NpcState"), "the villager chassis stays (ActiveNpcState, NpcState, PathWalker)");
            var radius = Number(tpl, "PathWalker.horizontalRadius");
            var height = Number(tpl, "PathWalker.height");
            Check(Math.Abs(radius - 0.75) < 1e-4 && Math.Abs(height - 2.0) < 1e-4, $"PathWalker radius/height = Size .75/2: {radius}/{height}");
            var width = Number(tpl, "MovementColliderConfig.width");
            Check(Math.Abs(width - 1.5) < 1e-4, $"MovementColliderConfig.width = 2 x radius: {width}");
            var box = Number(tpl, "ColliderResourceComponent.colliders[0].dataArray[BoxColliderData#0].shape.halfSize.x");
            Info($"interaction box halfSize.x {box}");
            npcId = Read(() => tpl.Get("ActiveNpcState.npcId").ToString());
            var fisherNpcId = fisher == null ? "" : Read(() => fisher.Get("ActiveNpcState.npcId").ToString());
            Check(npcId.Length > 0 && !npcId.StartsWith('<') && npcId != fisherNpcId, $"the clone has an NpcId of its own: {npcId} (Fisher {fisherNpcId})");
        }

        // ---- the motion closure ----
        var clones = expected["clones"] as JsonObject ?? new JsonObject();
        var cloneIds = clones.Select(kv => Guid.Parse((string)kv.Value!)).ToHashSet();
        var seqType = Assets.TypeHash("keen::actor::ActorSequenceResource");
        var served = cloneIds.Count(g => Assets.ReadResource(new AssetKey(g, seqType)) is { Length: > 0 });
        Check(cloneIds.Count == 21 && served == 21, $"the 21 motion clones are served: {served} of {cloneIds.Count}");
        var all = Assets.ListResources(seqType);
        var game = (int?)expected["gameSequences"] ?? -1;
        Check(all.Length == game + 21 && cloneIds.All(g => all.Any(k => k.Guid == g)), $"exactly 21 sequences more than the game's: {all.Length} = {game} + {all.Length - game}");
        if (tpl != null)
        {
            foreach (var field in SequenceFields)
            {
                var value = Read(() => tpl.Get(field).ToString());
                var source = fisher == null ? "" : Read(() => fisher.Get(field).ToString());
                var ok = Guid.TryParse(value, out var g) && cloneIds.Contains(g) && (string?)clones[source] == value;
                Check(ok, $"{field} is the clone of the Fisher's {source}: {value}");
            }
        }
        foreach (var role in new[] { "sit", "sleep" })
        {
            var x = expected[role];
            var seq = Guid.TryParse((string?)clones[(string?)x?["source"] ?? ""], out var g) ? Content.Sequences.Get(g) : null;
            Check(seq != null, $"{role}: the motion clone {g} is in the game's sequences");
            if (seq == null || x == null) continue;
            var states = x["states"]!.AsArray().Select(v => (string)v!).ToArray();
            var times = x["times"]!.AsArray().Select(v => (long)v!).ToArray();
            // Content.Sequences has one entry per sub-sequence (keen::actor::ActorSubSequenceResource): the Fisher's have one each
            var gotStates = Enumerable.Range(0, 3).Select(i => Read(() => seq.Get($"events[SetAnimationEvent#{i}].animationName").ToString())).ToArray();
            var gotTimes = Enumerable.Range(0, 3).Select(i => First(seq, $"events[SetAnimationEvent#{i}].time.value", $"events[SetAnimationEvent#{i}].time")).ToArray();
            var length = First(seq, "length.value", "length");
            Check(gotStates.SequenceEqual(states), $"{role}: enter/loop/exit play the authored states [{string.Join(", ", gotStates)}]");
            Check(gotTimes.Select(t => (long)t).SequenceEqual(times) && (long)length == (long)x["length"]!,
                $"{role}: retimed to the authored clips: events at [{string.Join(", ", gotTimes)}] ns, length {length} (want [{string.Join(", ", times)}], {x["length"]})");
        }

        // ---- the game-body NPC of the same pack, and nothing of the broken mod ----
        var pipTpl = Content.Templates.Get(BlockIds.Derive(Me, "npc", Pip));
        Check(pipTpl != null && Assets.ReadResource(new AssetKey(BlockIds.Derive(Me, "npc", Pip), TemplateType)) is { Length: > 0 }, "the game-body NPC's clone is built and served");
        if (pipTpl != null)
            Check(Read(() => pipTpl.Get("AnimationGraphResourceComponent2.animationGraph").ToString()) == FisherGraph.ToString() &&
                  Read(() => pipTpl.Get("ModelResource.model").ToString()) == Guid.Empty.ToString() && !Absent(pipTpl, "CharacterSetup"),
                "the game-body NPC keeps the villager's graph, model and appearance");
        Check(Content.Templates.Get(BlockIds.Derive(Broken, "npc", Npc)) == null && Content.Templates.Get(BlockIds.Derive(Broken, "npc", Pip)) == null,
            "the broken neighbour published nothing (neither of its NPC clones)");
        Check(Knowledge.FindNpc($"Kiln_{Me}_{Npc}") != null && Knowledge.FindNpc($"Kiln_{Me}_{Pip}") != null && Knowledge.FindNpc($"Kiln_{Broken}_{Npc}") == null,
            "the roster has both NPCs of this mod and none of the broken one");

        // ---- a spawned clone: the server's graph instance and the live template slot ----
        var entity = await Spawn.Entity(clone, Pos, 0, new SpawnOptions { Flags = SpawnFlags.Transient });
        try
        {
            Check(!entity.IsNone, $"the clone spawns: {entity}");
            for (var i = 0; i < 30; i++) await Context.Scheduler.NextTick();
            if (!entity.IsNone)
            {
                var live = Read(() => Creatures.Of(entity).Ai.Get("AnimationGraphResourceComponent2.animationGraph"));
                Check(live.Contains(s.Graph.ToString(), StringComparison.OrdinalIgnoreCase), $"its live template slot selects the authored graph: {live}");
                Check(World.TryRead<uint>(entity, "ServerAnimationGraphComponent2", out var handle) && handle != 0, $"the server made its graph instance: handle {handle}");
                Check(Spawn.Exists(entity), "it is still there after 30 steps");
            }
        }
        finally
        {
            if (!entity.IsNone) Spawn.Destroy(entity);
        }

        // ---- a restart: the same ids and bytes ----
        var statePath = Path.Combine(Context.DataDirectory, "state.json");
        var now = new JsonObject
        {
            ["template"] = s.Template.ToString(), ["npcId"] = npcId, ["templateSha256"] = Sha(tplBytes),
            ["sit"] = tpl == null ? "" : Read(() => tpl.Get("ActiveNpcState.sitSequence").ToString()),
        };
        Info($"facts {now.ToJsonString()}");
        if (File.Exists(statePath))
        {
            var before = JsonNode.Parse(File.ReadAllText(statePath))!;
            Info($"the previous start's facts {before.ToJsonString()}");
            foreach (var key in new[] { "template", "npcId", "templateSha256", "sit" })
                Check((string?)before[key] == (string?)now[key], $"after a restart the {key} is the same: {before[key]} -> {now[key]}");
        }
        else
        {
            Directory.CreateDirectory(Context.DataDirectory);
            File.WriteAllText(statePath, now.ToJsonString());
            Info("first start: facts kept for the next one");
        }
    }
}
