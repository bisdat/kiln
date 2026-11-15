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
        long sequence = DateTime.UtcNow.Ticks;
        async Task<long[]?> Snapshot(long mode)
        {
            var nonce = ++sequence;
            long[]? reply = null;
            using var listener = c.Mod.Messages.On("typed-proof-result", m =>
            {
                if (m.Sender == "typed-hook-volume" && m.Payload is long[] { Length: 6 } data && data[0] == nonce) reply = data;
            });
            if (!Mods.Send("typed-hook-volume", "typed-proof", new long[] { nonce, mode }))
            { c.Expect(false, "the sample accepts the proof request"); return null; }
            if (!await c.Until(() => reply != null, TimeSpan.FromSeconds(5), "the sample replies with observed handler counts")) return null;
            c.Expect(reply![5] == 1, "the sample's hook is installed");
            return reply;
        }
        try
        {
            var off = await Snapshot(0);
            if (off == null || off[5] != 1 || off[1] != 0) return;
            c.Instruct("CONTROL: the sample now observes calls without changing volume. Empty a watering can, fill it at a well, " +
                "then water until empty. Record the amount. Use this same can and well for the next stage. Confirm only after completing the control.");
            if (await c.Verdict() != ClientCheckVerdict.Pass) return;
            var control = await Snapshot(2);
            if (control == null) return;
            var controlRuns = control[2] - off[2];
            c.Expect(controlRuns > 0, "observed real-item calls with the sample off", $"{controlRuns} calls");
            if (controlRuns <= 0) return;
            var on = await Snapshot(1);
            if (on == null || on[5] != 1 || on[1] != 1) return;
            c.Instruct("The sample is now ON. Repeat with the same empty can and well. One fill must give about twice the CONTROL water. " +
                "Play for two minutes with no crash or stutter. Fail if the control and enabled runs are alike, even if another mod already increased capacity.");
            if (await c.Verdict() != ClientCheckVerdict.Pass) return;
            var enabled = await Snapshot(2);
            if (enabled == null) return;
            var enabledRuns = enabled[3] - on[3];
            var enabledEdits = enabled[4] - on[4];
            c.Expect(enabledRuns > 0 && enabledEdits > 0 && enabledEdits <= enabledRuns,
                "the sample's handler ran and itself changed real-item results", $"{enabledRuns} calls, {enabledEdits} edits");
        }
        finally { Mods.Send("typed-hook-volume", "typed-proof", new long[] { ++sequence, 1 }); }
    }
}
