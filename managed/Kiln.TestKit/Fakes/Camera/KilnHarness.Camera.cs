// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessCamera? _camera;

    /// <summary>The fake kiln.camera of this harness: the camera requests mods made, and leases a test lets run out.</summary>
    public HarnessCamera Camera => _camera ??= new HarnessCamera(this);
}

/// <summary>What a camera call asked of the fake kiln.camera.</summary>
public enum FakeCameraCallKind
{
    /// <summary><c>Camera.SetMode</c>, <c>Camera.Set</c>, <c>Camera.SetLocal</c>.</summary>
    Apply,
    /// <summary><c>Camera.Update</c>.</summary>
    Update,
    /// <summary><c>Camera.End</c>.</summary>
    End,
}

/// <summary>A camera call that reached the fake kiln.camera (one the service would have answered through its callback;
/// a call it turns away at once is not recorded).</summary>
/// <param name="Step">The step it was made in.</param>
/// <param name="Kind">Which call.</param>
/// <param name="Lease">The lease's number: the new one for an apply.</param>
/// <param name="Endpoint">1 a server mode lock, 2 the local view, 3 a remote view (<c>CameraEndpoint</c>'s numbers).</param>
/// <param name="Player">The player whose camera it is; none for the local view.</param>
/// <param name="Parts">The request's parts as the service's bits: 1 mode, 2 path, 4 profile, 8 shake, 16 lens. 0 for an end.</param>
/// <param name="DurationMs">The request's duration; 0 for an end.</param>
/// <param name="Priority">The request's priority; 0 for an end.</param>
/// <param name="Refusal">0 when the fake accepted it, else the reason's number (<c>CameraReason</c>).</param>
public readonly record struct FakeCameraCall(ulong Step, FakeCameraCallKind Kind, ulong Lease, uint Endpoint, EntityId Player, uint Parts, uint DurationMs, int Priority,
    uint Refusal);

/// <summary>
/// The fake kiln.camera of a harness (<see cref="KilnHarness.Camera"/>): Kiln.Camera's own code runs on it.
/// </summary>
/// <remarks>
/// <para><b>Modelled.</b> Leases as the service keeps them: one per player and endpoint, the same owner applying again
/// is refused, another owner wins only with a higher priority (the old lease ends superseded first), one apply or update
/// per player and endpoint waits at a time (another is busy until its answer), update and end with their refusals, answers and change events one step after the call, in the order of the calls. The rows
/// <c>camera.server_mode</c> and <c>camera.local_*</c>, which a test can switch off.</para>
/// <para><b>Not modelled.</b> Any picture: a request is "applied" the step after it was made. Time (a lease ends only
/// through <see cref="Expire"/>), blends, the frame snapshot (<c>camera.rendered_snapshot</c>), requests of a server
/// for a player's client (<c>camera.remote</c>), and what a stopping world does to open leases.</para>
/// </remarks>
public sealed class HarnessCamera
{
    private readonly KilnHarness _harness;

    internal HarnessCamera(KilnHarness harness) => _harness = harness;

    private CameraFakeState State => KitFakes.State<CameraFakeState>(_harness);

    /// <summary>Every camera call the fake answered through its callback, in order.</summary>
    public IReadOnlyList<FakeCameraCall> Calls { get { lock (State.Lock) return [.. State.Calls]; } }

    /// <summary>The numbers of the leases that hold a camera now.</summary>
    public IReadOnlyList<ulong> Leases { get { lock (State.Lock) return [.. State.Leases.Keys.Order()]; } }

    /// <summary>Lets a lease run out, as its duration would: its change event (ended, expired) goes out at the next step.</summary>
    /// <returns>False when no such lease holds a camera.</returns>
    public bool Expire(ulong lease) => CameraFake.Expire(State, lease);
}
