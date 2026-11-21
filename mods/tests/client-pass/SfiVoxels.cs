// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;

namespace ClientPass;

// Kiln's interpreter: the voxel grid of ShroudForge-format mods (since 0.2.0-dev.30.3). runtime.world.voxel.get_grid_spec
// in a hosted kiln start, with the voxel probe (mods/tests/sf-voxel-probe, a test package that only reads); cell reads and
// writes still refuse with ShroudForge's reason. docs/kiln/client-pass.md, "Kiln mode's checks since 0.2.0-dev.30.3".
// Order 3914.
public sealed partial class ClientPassMod
{
    private const string SfiVoxelSince = "0.2.0-dev.30.3";
    // the voxel probe (mods/tests/sf-voxel-probe): logs the grid when its answer changes, and a one-cell read's answer
    private const string SfiVoxelProbe = "kiln-sf-voxel-probe";
    // the grid on the current builds (dedicated server 26430253, client 26430625): 0.5 m cells, 80 x 80 tiles of 256 cells,
    // and the engine's y bound (2^31 - 8)
    private const string SfiVoxelGridSpec = "cell 0.5,0.5,0.5 maximum 20480,2147483640,20480";

    /// <summary>The voxel probe's last kiln.log line (`[kiln-sf-voxel-probe] probe ...`) that starts with `what`.</summary>
    private static string? SfiVoxelLine(CheckContext c, string what) =>
        BootLog(c, "kiln.log")?.LastOrDefault(l => l.Contains($"[{SfiVoxelProbe}] probe {what}", StringComparison.Ordinal));

    [ClientCheck("sfi.voxel-grid", "A ShroudForge-format mod reads the voxel grid in a hosted kiln start; cell reads still refuse",
        Area = SfiArea, Since = SfiVoxelSince, Needs = ClientCheckNeeds.Hosted, Order = 3914)]
    public async Task SfiVoxelGrid(CheckContext c)
    {
        if (await NeedKilnStart(c) is not { } s) return;
        if (s.Package(SfiVoxelProbe) is not { Intent: "selected", Skip: null })
        {
            var now = s.Package(SfiVoxelProbe) is { } p ? "it is " + p.State + " in this start" : "this start's record does not list it";
            c.Instruct("This check needs the voxel probe running in this kiln start: copy the folder mods/tests/sf-voxel-probe of " +
                       $"the Kiln repository into <game>/mods, switch it on in the launcher and start again ({now}). It only reads.");
            await c.Skip("needs the voxel probe (mods/tests/sf-voxel-probe) running in a kiln start");
            return;
        }
        c.Instruct("Host a copied world and stay in it for a few seconds: the voxel probe asks for the voxel grid every two seconds.");
        await c.Until(() => SfiVoxelLine(c, "grid: voxel ") is not null, TimeSpan.FromSeconds(180), "the probe got the voxel grid");
        var grid = SfiVoxelLine(c, "grid: ");
        c.Expect(grid?.Contains("grid: voxel origin ", StringComparison.Ordinal) == true &&
                 grid.Contains(SfiVoxelGridSpec, StringComparison.Ordinal),
            $"kiln.log: the grid is the current builds' ({SfiVoxelGridSpec})", grid);
        var read = SfiVoxelLine(c, "read: ");
        c.Expect(read?.Contains("refused: operation runtime.world.voxel.read has not been verified for game build", StringComparison.Ordinal) == true,
            "kiln.log: a cell read still refuses, with ShroudForge's reason", read);
        c.Instruct("Walk around for a few seconds. Pass if the game ran as usual while the probe was on (no stall or hitch when " +
                   "it asked), otherwise fail with a note.");
        await c.Verdict();
    }
}
