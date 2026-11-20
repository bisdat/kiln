// SPDX-License-Identifier: MIT
// The fixture mods of boot-required-test: each is a boot mod ("boot": "required" or true) whose boot content is its own files, with one
// bad entry for one collector (fixtures.py). OnBoot stages nothing. A required fixture must never load (the start stops before the
// game's entry point); an optional one loads, and kiln.log names the entry its collector skipped.
using Kiln;

namespace BootRequiredFixture;

public sealed class BootRequiredFixtureMod : Mod
{
    public override void OnBoot(BootContext boot)
    {
        // content-bind: one item through kiln.content's boot collector; the lab switch boot.testFault makes every world refuse it at
        // registration (required: the start stops when the world loads; optional: the mod runs without the item)
        if (boot.ModId != "boot-req-content-bind") return;
        var staged = BootDefinitions.For(boot).Define("bound-token", new ItemBlock { Name = "Bound Token", Stack = 10 });
        if (!staged.Ok) throw new InvalidOperationException(staged.Reason);
    }

    public override void OnLoad() =>
        Context.Log.Info($"boot-required-test: {Context.Info.Id} loaded: boot state {Mods.BootState(Context.Info.Id).State}");
}
