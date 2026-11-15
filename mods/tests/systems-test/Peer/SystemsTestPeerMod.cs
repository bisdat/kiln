// SPDX-License-Identifier: MIT
// systems-test's peer: once the native part of systems-test is done (kiln/control/systems-native.done), replaces
// fuel_setup (running the game's update from its Replace) for 300 steps, while systems-test asks for a second Replace.
using Kiln;

namespace SystemsTestPeer;

public sealed class SystemsTestPeerMod : Mod
{
    private ulong _t0;
    private ProgramWrap? _wrap;
    private bool _done;

    public override void OnLoad()
    {
        // the boot after the restart (systems-test's native part wrote its mark before the stop): nothing to do
        _done = File.Exists(Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", "systems-test-mark.txt")));
        Context.Tick.Post += OnPost;
    }

    private void OnPost(in TickInfo tick)
    {
        if (_done) return;
        var control = Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", "..", "control", "systems-native.done"));
        if (_t0 == 0)
        {
            if (tick.Step % 25 != 0 || !File.Exists(control)) return;
            _t0 = tick.Step;
            var r = Systems.Wrap("fuel_setup");
            var replaced = r.Wrap?.Replace((ref Slice s) => s.RunOriginal());
            Log.Info($"systems-test-peer: Replace of fuel_setup {(replaced is { Ok: true } ? "made" : "refused: " + (replaced?.Reason ?? r.Reason))}");
            _wrap = r.Wrap;
        }
        else if (tick.Step - _t0 == 300)
        {
            _wrap?.Dispose();
            _done = true;
            Log.Info("systems-test-peer: done");
        }
    }
}
