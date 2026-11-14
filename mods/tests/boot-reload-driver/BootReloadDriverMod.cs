// SPDX-License-Identifier: MIT
// Lab test of a boot mod's frozen inputs (chapter 08 §4.4, fix round 08-onboot.handles). Waits until boot-definitions-test wrote its
// checked.txt in this process, then: unloads it (its committed boot blocks are still found), loads it unchanged (it loads), unloads it,
// adds blocks/edit.json to its folder, loads it (refused: RestartRequiredBoot, with the file named and "restart the game"), removes the file
// and loads it again (it loads). One line per step: "boot-reload-driver: PASS|FAIL <ID> ...", then "boot-reload-driver: DONE".
using Kiln;

namespace BootReloadDriver;

public sealed class DriverMod : Mod
{
    const string Id = "boot-reload-driver", Target = "boot-definitions-test";
    int _pass, _fail;

    bool _ran;

    // once: content-ready handlers run again when a mod loads later (the reloads below)
    public override void OnLoad() => Content.OnReady(e => { if (!_ran) { _ran = true; _ = Run(); } });

    void Check(bool ok, string id, string text)
    {
        if (ok) _pass++; else _fail++;
        var line = $"{Id}: {(ok ? "PASS" : "FAIL")} {id} {text}";
        if (ok) Context.Log.Info(line); else Context.Log.Error(line);
    }

    async Task Run()
    {
        var target = Mods.Get(Target);
        if (target == null)
        {
            Context.Log.Info($"{Id}: NOT RUN: {Target} is not in this start");
            return;
        }
        var dir = target.Directory;
        var data = Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", Target, "checked.txt"));
        for (var i = 0; i < 24 * 120; i++)
        {
            if (File.Exists(data) && File.ReadAllText(data) == $"{Environment.ProcessId}") break;
            await Context.Scheduler.NextTick();
        }
        var edit = Path.Combine(dir, "blocks", "edit.json");
        try
        {
            var unload = await Mods.UnloadAsync(Target);
            var cold = Blocks.Find("cold", Target);
            Check(unload.Ok && !Mods.IsLoaded(Target) && cold != null, "BOOT-FIND-AFTER-UNLOAD",
                $"unload: {unload.Ok} {unload.Message}; Blocks.Find(cold) after it: {(cold == null ? "null" : $"{cold} {cold.Guid}")}");

            var same = await Mods.LoadAsync(Path.GetFileName(dir));
            Check(same.Ok && Mods.IsLoaded(Target), "BOOT-LOAD-UNCHANGED", $"load unchanged: {same.Ok} {same.Message}");

            await Mods.UnloadAsync(Target);
            Directory.CreateDirectory(Path.Combine(dir, "blocks"));
            File.WriteAllText(edit, """{ "statuses": { "edited": { "name": "Edited", "seconds": 3 } } }""");
            var changed = await Mods.LoadAsync(Path.GetFileName(dir));
            Check(!changed.Ok && !Mods.IsLoaded(Target) && changed.Message.StartsWith("RestartRequiredBoot:", StringComparison.Ordinal)
                  && changed.Message.Contains("blocks/edit.json", StringComparison.Ordinal) && changed.Message.Contains("restart the game", StringComparison.Ordinal),
                "BOOT-LOAD-CHANGED", $"load after adding blocks/edit.json: {changed.Ok} {changed.Message}");

            File.Delete(edit);
            var back = await Mods.LoadAsync(Path.GetFileName(dir));
            Check(back.Ok && Mods.IsLoaded(Target), "BOOT-LOAD-RESTORED", $"load with the file removed again: {back.Ok} {back.Message}");
        }
        catch (Exception e)
        {
            Check(false, "BOOT-RELOAD", $"{e.GetType().Name}: {e.Message}");
        }
        finally
        {
            if (File.Exists(edit)) File.Delete(edit);
        }
        Context.Log.Info($"{Id}: DONE: {_pass} PASS, {_fail} FAIL");
    }
}
