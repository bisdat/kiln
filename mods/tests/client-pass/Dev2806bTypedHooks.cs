// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;

namespace ClientPass;

// Typed hooks (chapter 06b, since 0.2.0-dev.29): what a real game client still has to show. The native typed link ran
// on the client exe in a research session (06b-typed-client, settled); a managed mod's typed hook in a game client has
// not. Order block 2700-2799 (lab port 28070).
//
// - 06b-typed-hooks.client: the sample typed-hook-volume (mods/samples/TypedHookVolume) in a hosted world: hooks.typed
//   and sig/items/GetItemVolume are available in the client's process, the hook installs, and a watering can takes
//   twice the water at a well (GetItemVolume is the game's own cap of that transfer); the game plays on normally.
//
// Not written: 06b-terrain-args (optional, gates nothing; plan §6): its diagnostic needs a client-side fixture that
// hooks rules/TerrainDamage with a pass-through typed link, and this pack does not reference Kiln.Hooks.
public sealed partial class ClientPassMod
{
    [ClientCheck("06b-typed-hooks.client", "Typed hooks: a mod's typed hook runs in the game", Area = "Typed hooks",
        Since = Dev29, Needs = ClientCheckNeeds.Hosted, Order = 2700)]
    public async Task TypedHooksClient(CheckContext c)
    {
        if (!await Need(c, "typed-hook-volume") || !await NeedPlayer(c)) return;
        foreach (var row in new[] { "hooks.typed", "sig/items/GetItemVolume" })
            c.Expect(Capabilities.Is(row), $"{row} is available in this game", Capabilities.Why(row));
        c.Instruct("The sample typed-hook-volume hooks the game's own function that says how much an item holds and " +
                   "doubles its answer. Take an empty watering can to a well (or any water you fill it from) and fill it " +
                   "once; then water plants or soil until it is empty. With the sample, one fill should give about twice " +
                   "the water a fill gives without it (if you know the vanilla amount, compare; else unload the sample, " +
                   "start again and compare the two). Then play on for two minutes: no stutter, no crash, nothing else " +
                   "changed. Pass when the can took about twice the water and the game ran normally; fail with what you " +
                   "saw otherwise.");
        await c.Verdict();
    }
}
