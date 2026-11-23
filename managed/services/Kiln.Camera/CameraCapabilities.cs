// SPDX-License-Identifier: MIT
// Kiln.Camera in the capability registry (docs/kiln/services/caps.md). kiln.camera reports camera.service and one row
// per capability bit; on a game client five of them are sigdb features too (the renderer's two places). Kiln.Camera
// adds the two aggregates and, while the native service is not there, says so on every row a mod can ask for.
namespace Kiln;

internal static unsafe class CameraCaps
{
    /// <summary>kiln.camera's table is here.</summary>
    public const string Service = "camera.service";
    /// <summary>Some camera request of a server works here: a mode lock, a remote visual, or the host's own view.</summary>
    public const string Server = "camera.server";
    /// <summary>Some render-only part works for this client's own player.</summary>
    public const string Local = "camera.local";
    public const string ServerMode = "camera.server_mode";
    public const string LocalPose = "camera.local_pose";
    public const string LocalFov = "camera.local_fov";
    public const string LocalProfile = "camera.local_profile";
    public const string LocalShake = "camera.local_shake";
    public const string RenderedSnapshot = "camera.rendered_snapshot";
    public const string Remote = "camera.remote";
    public const string ResourceProfile = "camera.resource_profile";

    private const string Table = CameraNative.Service;
    private static readonly string[] LocalParts = [LocalPose, LocalFov, LocalProfile, LocalShake];
    private static readonly string[] NativeRows = [Service, ServerMode, LocalPose, LocalFov, LocalProfile, LocalShake, RenderedSnapshot, Remote, ResourceProfile];

    internal static readonly ServiceCapRows Rows = new(Table, Report, everySteps: 25, refreshOnlyWithService: true);

    private static bool _lowered;

    private static void Report()
    {
        var here = CameraNative.T != null;
        const string missing = "kiln.camera is not loaded";
        // The native service reports these rows itself. Without it nobody does: say why each is off, and take that back
        // when the table turns up later.
        if (!here)
        {
            ServiceCaps.LowerFeatures(Table, [RenderedSnapshot, .. LocalParts]);
            foreach (var row in NativeRows) ServiceCaps.Static(row, false, CapabilityReason.ServiceMissing, missing, Table);
            _lowered = true;
        }
        else if (_lowered)
        {
            foreach (var row in NativeRows) ServiceCaps.Static(row, true, CapabilityReason.None, "");
            _lowered = false;
        }
        var local = here && LocalParts.Any(Capabilities.Is);
        // A server world can exist on both sides (a client hosts one). The host's own view goes through its local rows.
        var server = here && (Capabilities.Is(ServerMode) || Capabilities.Is(Remote) || local);
        ServiceCaps.Static(Local, local, CapabilityReason.ServiceMissing, here ? "no camera.local_* row is on" : missing, Service);
        ServiceCaps.Static(Server, server, CapabilityReason.ServiceMissing, here ? "neither camera.server_mode nor camera.remote nor a camera.local_* row is on" : missing, Service);
    }

    /// <summary>Throws <see cref="KilnUnavailableException"/> naming <paramref name="capability"/> unless it is available.</summary>
    public static void Require(string capability)
    {
        Rows.Ensure();
        if (CameraNative.T != null && Capabilities.Is(capability)) return;
        throw ServiceCaps.Unavailable(capability);
    }

    /// <summary>The exception for a native "unavailable" under a row that still read available: the rows are reported
    /// again first, so the reason is the one of now.</summary>
    public static KilnUnavailableException Off(string capability)
    {
        Rows.Ensure();
        Report();
        return ServiceCaps.Unavailable(capability, "kiln.camera answered that it is unavailable");
    }

    /// <summary><see cref="Off(string)"/> for the first of <paramref name="candidates"/> that reads off once the rows are
    /// reported again, or <see cref="Service"/> when none does.</summary>
    public static KilnUnavailableException OffOneOf(IEnumerable<string> candidates)
    {
        Rows.Ensure();
        Report();
        return ServiceCaps.Unavailable(candidates.FirstOrDefault(r => !Capabilities.Is(r)) ?? Service, "kiln.camera answered that it is unavailable");
    }

    /// <summary>The rows a request of these parts needs on its endpoint (chapter 03, 4.4: per part, not per entry point).</summary>
    public static IEnumerable<string> RowsOf(CameraEndpoint endpoint, uint parts)
    {
        if (endpoint == CameraEndpoint.ServerMode) return [ServerMode];
        if (endpoint == CameraEndpoint.Remote) return [Remote];
        var rows = new List<string>(4);
        if ((parts & CameraNative.PartPath) != 0) rows.Add(LocalPose);
        if ((parts & CameraNative.PartProfile) != 0) rows.Add(LocalProfile);
        if ((parts & CameraNative.PartLens) != 0) rows.Add(LocalFov);
        if ((parts & CameraNative.PartShake) != 0) rows.Add(LocalShake);
        return rows;
    }
}

/// <summary>Started by Kiln (ServiceTick.AutoStart) at its first service tick: the rows are there before a mod asks.</summary>
internal static class CameraStartup
{
    static CameraStartup() => CameraCaps.Rows.Ensure();
}
