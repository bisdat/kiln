// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessCamera? _camera;

    /// <summary>The fake kiln.camera of this harness: the camera requests mods made, and leases a test lets run out.</summary>
    public HarnessCamera Camera => _camera ??= new HarnessCamera(this);

    internal bool CameraOwnerRevoked(ulong owner)
    {
        lock (_host.Revoked) return _host.Revoked.Contains(owner);
    }
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
/// <c>camera.server_mode</c> and <c>camera.local_*</c>, which a test can switch off. Calls outside mod code (owner 0)
/// are refused as the service refuses them. A mod that unloads: its applied leases end <c>OwnerRetired</c> at the next
/// step. What a test scripts: a lease that ends by itself (<see cref="End"/>, <see cref="Expire"/>), a stopping world
/// (<see cref="StopWorld"/>), and refusals the game or the service would give (<see cref="RefuseNext"/>).</para>
/// <para><b>Remote.</b> A server's visual request for a player's client (<c>camera.remote</c>) reaches only a player the
/// test made a Kiln client (<see cref="KilnClient"/>, with the local parts its client offers): any other player is refused
/// <c>RequiresKilnClient</c>, a part the client does not offer <c>ClientCapability</c>, and
/// <c>Camera.GetCapabilities(Remote, player)</c> answers what that client offered. There is no wire: no ACK, timeout or
/// negotiation.</para>
/// <para><b>Not modelled.</b> Any picture: a request is "applied" the step after it was made. Time (a lease ends only
/// when the test says so), blends, the frame snapshot (<c>camera.rendered_snapshot</c>). The service ends a stopping
/// world's leases on the thread that stops it; the fake answers at the next step.</para>
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
    public bool Expire(ulong lease) => CameraFake.EndLease(State, lease, CameraAbi.EndExpired);

    /// <summary>Ends an applied lease by itself, as the service would for <paramref name="endReason"/> (<c>CameraEndReason</c>'s
    /// numbers: 2 expired, 3 target lost, 4 player left, 7 capability lost, 9 peer disconnected, 10 authority timeout, 11 safety
    /// rejected, 12 transport timeout). Its change event goes out at the next step.</summary>
    /// <returns>False when no such applied lease holds a camera.</returns>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="endReason"/> is not one a lease ends with by itself.</exception>
    public bool End(ulong lease, uint endReason)
    {
        if (endReason is not (2 or 3 or 4 or 7 or 9 or 10 or 11 or 12))
            throw new ArgumentOutOfRangeException(nameof(endReason), endReason, "released, superseded, world stopped, owner retired and cancelled have their own paths");
        return CameraFake.EndLease(State, lease, endReason);
    }

    /// <summary>The world stops: at the next step every applied lease ends <c>WorldStopped</c> and a waiting apply or update
    /// is answered <c>StaleWorld</c>.</summary>
    /// <returns>How many leases it ended.</returns>
    public int StopWorld() => CameraFake.StopWorld(State);

    /// <summary>Makes <paramref name="player"/>'s client a Kiln client that offers <paramref name="parts"/> (<c>CameraCapability</c>'s
    /// local part bits: 2 pose, 4 lens, 8 profile, 16 shake; the default is all four) to a server's visual requests
    /// (<c>camera.remote</c>). 0 makes it a vanilla client again.</summary>
    /// <exception cref="ArgumentException"><paramref name="player"/> is none, or <paramref name="parts"/> has a bit that is
    /// not a local part.</exception>
    public void KilnClient(EntityId player, ulong parts = 30)
    {
        if (player.IsNone) throw new ArgumentException("a player is required", nameof(player));
        if ((parts & ~CameraAbi.LocalParts) != 0) throw new ArgumentException("only the local part bits 2, 4, 8 and 16", nameof(parts));
        lock (State.Lock)
        {
            if (parts == 0) State.KilnClients.Remove(player.Value);
            else State.KilnClients[player.Value] = parts;
        }
    }

    /// <summary>The next apply or update that the lease rules would accept is refused with <paramref name="reason"/> instead
    /// (<c>CameraReason</c>'s numbers, 1 to 27: e.g. 8 peer unavailable, 12 stale world, 18 frame fault), answered at the next
    /// step. Calls queue: two calls refuse the next two.</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="reason"/> is not a refusal.</exception>
    public void RefuseNext(uint reason)
    {
        if (reason is < 1 or > 27) throw new ArgumentOutOfRangeException(nameof(reason), reason, "a CameraReason from 1 to 27");
        lock (State.Lock) State.Refusals.Enqueue(reason);
    }
}
