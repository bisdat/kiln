// SPDX-License-Identifier: MIT
// abilities-test rows for the Codex review of package 06c (dev.29): a native taken off and registered again with
// another argc, the VM pops the new count (fix 1), also when the game's native registry is full.
using Kiln;

namespace AbilitiesTest;

public sealed partial class AbilitiesTestMod
{
    private string HelperData => Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", "natives-helper"));

    private async Task<string> LoadHelper(bool argc2)
    {
        Directory.CreateDirectory(HelperData);
        var marker = Path.Combine(HelperData, "argc2");
        if (argc2) File.WriteAllText(marker, "");
        else File.Delete(marker);
        File.Delete(Path.Combine(HelperData, "result.txt"));
        if (Kiln.Mods.All.Any(m => m.Id == "natives-helper" && m.IsLoaded)) await Kiln.Mods.UnloadAsync("natives-helper");
        var r = await Kiln.Mods.LoadAsync("natives-helper");
        var file = Path.Combine(HelperData, "result.txt");
        return $"{(r.Ok ? "loaded" : "not loaded: " + r.Message)}; RegisterSync said: {(File.Exists(file) ? File.ReadAllText(file) : "nothing")}";
    }

    // Count registered again with argc 2 (it was taken off with argc 1): the VM must pop two. The program is laid out
    // so the verifier's count (2) signals tag 0x77 with the answer, and a stale count (1) would signal another tag.
    private async Task TestArgcRevive(WorldPosition p)
    {
        if (!Kiln.Mods.All.Any(m => m.Id == "natives-helper")) { Info("natives-helper is not deployed: argc revive row skipped"); return; }
        var loaded = await LoadHelper(argc2: true);
        var count = Abilities.HashOf("natives-helper.Count");
        const uint hOut = 0xFFFF0000;
        uint[] code =
        [
            11, 0x77, 11, hOut, 11, 0x5A, 11, hOut, 19, 0x06c00071, count, 27,  // Count(args[0] = Out, args[1] = 0x5A)
            19, 0x06c00072, Abilities.HashOf("kiln.Signal"), 27,                 // kiln.Signal(Out, tag 0x77)
            30,
        ];
        var guid = new Guid("06c0000b-0000-4000-8000-000000000001");
        Abilities.Programs.Register(ProgramBuilder.Raw(guid, code, [30], [("Out::value", Sint32, 4, new byte[4])]), out _);
        _signals.Clear();
        var e = await Abilities.RunProgram(guid, p);
        _spawned.Add(e);
        await Steps(6);
        var sig = _signals.FirstOrDefault(s => s.Impact.Self == e);
        Check(sig is { Tag: 0x77 } && sig.AsInt > 0,
            $"a native taken off and registered again with another argc: the VM pops the new count (signal tag 0x{sig?.Tag:x}, value {sig?.AsInt}; {loaded})");
        await Kiln.Mods.UnloadAsync("natives-helper");
    }

    // After the registry is full: the same name again with the same argc keeps its entry (Ok); with another argc the
    // entry must be written again, which the game may refuse at 128 keys (then Ok = false with the reason).
    private async Task TestReviveAtFullRegistry()
    {
        if (!Kiln.Mods.All.Any(m => m.Id == "natives-helper")) return;
        var other = await LoadHelper(argc2: false);  // dead with argc 2 since TestArgcRevive: now argc 1
        Info($"natives v2: at a full registry, Count again with another argc: {other}");
        await Kiln.Mods.UnloadAsync("natives-helper");
        var same = await LoadHelper(argc2: false);
        Check(same.Contains("RegisterSync said: 1 True"), $"at a full registry, a native taken off and registered again with the same argc keeps its entry: {same}");
        await Kiln.Mods.UnloadAsync("natives-helper");
    }
}
