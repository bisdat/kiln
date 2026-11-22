// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// 12a-debugger (chapter 12, part 12a): a debugger on a game client. Both checks are run from an editor (VS Code with
// the Kiln extension, or `kiln debug --client … --dap` from another editor): the game only has to run the counter
// sample (mods/samples/DebugCounter, mod id debug-counter) with kiln.json "developer_debugging": true. The checks show
// the steps and take the answer. They gate the docs' wording ("built, not verified" until both pass), not a capability.
// Machines (DECISIONS.md "Two game clients"): W = kiln-win1 (native Windows), L = this Linux PC (Proton). The Windows
// check runs on W, the Proton check on L; the joined case of each joins a dedicated lab server here (or a world hosted
// on the other machine), the other machine's client needs nothing. Orders 3400-3499.
public sealed partial class ClientPassMod
{
    private const string DebugCounter = "debug-counter";

    private const string DebugSteps =
        "Use a disposable character and world. " +
        "1. Before this start: Settings, kiln.json: Developer debugging on (kiln.json \"developer_debugging\": true), and the " +
        "mod debug-counter in the profile (kiln.log shows \"debug-counter: count N\" every 2 s). " +
        "2. Without a debugger: `kiln debug --client <install> --observe` shows the counter lines and the session mode, and " +
        "starts no debugger (the game never pauses). Stop it with Ctrl-C. " +
        "3. In VS Code open mods/samples/DebugCounter, use the launch configuration \"Kiln: attach to {0} client\" " +
        "(client = this install, allowClientPause: true) and press F5. Without allowClientPause it must refuse with the " +
        "consent message and attach nothing. " +
        "4. Put a breakpoint on the line marked DEBUG-COUNTER-BREAK in DebugCounterMod.cs. The game stops within 2 s: the " +
        "editor shows that line, the local count and the stack (DebugCounterMod.Count above Kiln.Host). " +
        "5. Step once, Continue: the counter goes on in kiln.log. Stop again at the breakpoint, then Stop (detach) while " +
        "stopped: the same game process keeps rendering and takes input (walk, open the inventory). " +
        "6. Leave a breakpoint hit for 10 s: after maxPauseMs (5000) the editor shows a warning that Kiln continued the " +
        "game, and the game runs on. " +
        "7. Reload the mod while attached (kiln reload debug-counter --server <the game folder>): refused (debugger_attached). Detach, reload, attach " +
        "again: the breakpoint stops again. " +
        "8. Hosted (your own world) and joined (a server) are two cases: do 3-6 in each. In the joined case also hold a " +
        "breakpoint 3 s and then 30 s and write down whether the connection held and how long; a dropped connection is a " +
        "documented risk, not a failure, as long as it is reported. ";

    [ClientCheck("12-debug.client-windows", "Windows debugger lifecycle", Area = "Tooling", Since = Dev31,
        Tags = [HostTools, Windows], Order = 3400)]
    public async Task DebugClientWindows(CheckContext c)
    {
        if (!await Need(c, DebugCounter)) return;
        await Outside(c, "On W (kiln-win1, native Windows), with Kiln and the counter sample installed. " + string.Format(DebugSteps, "Windows") +
            "Joined case: W joins a dedicated lab server on L (dev.31 lab ports 31000-31999), or a world hosted on L. " +
            "Also attach to a Windows dedicated server on W (configuration \"Kiln: attach to Windows server\") and repeat " +
            "4-5 there; the server keeps running after the detach. " +
            "Did the debugger attach to this game, stop, step, continue, detach and refuse as described, with the game " +
            "never ended or paused without consent?");
    }

    [ClientCheck("12-debug.client-proton", "Proton debugger lifecycle and runner", Area = "Tooling", Since = Dev31,
        Tags = [HostTools], Order = 3410)]
    public async Task DebugClientProton(CheckContext c)
    {
        if (!await Need(c, DebugCounter)) return;
        await Outside(c, "On L (this Linux PC, the game under Proton from Steam). " + string.Format(DebugSteps, "Proton") +
            "Joined case: L joins a dedicated lab server here, or a world hosted on W. " +
            "Also: `kiln debug --client <install> --allow-client-pause --json` names this game's Windows PID and the Proton " +
            "runner (runnerSha256, env); if it says ambiguous_runner or unsupported_identity, write down its reason. " +
            "Did the debugger run under the game's own Proton, attach to this game, stop, step, continue, detach and " +
            "refuse as described, with the game never ended or paused without consent?");
    }
}
