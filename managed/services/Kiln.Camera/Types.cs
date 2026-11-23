// SPDX-License-Identifier: MIT
// Kiln.Camera's records and enums. Every number is the one of native/core/abi/services/kiln_camera.h.
using System.Numerics;

namespace Kiln;

/// <summary>Whose camera a request is for, and who carries it out.</summary>
public enum CameraEndpoint
{
    /// <summary>The server locks a player's stock camera mode (<see cref="Camera.SetMode"/>). Reaches vanilla clients.</summary>
    ServerMode = 1,
    /// <summary>The render-only camera of this game client's own player (<see cref="Camera.SetLocal"/>).</summary>
    Local = 2,
    /// <summary>A render-only camera the server asks of a player whose client runs Kiln (<see cref="Camera.Set"/>).</summary>
    Remote = 3,
}

/// <summary>What the camera service can do here; one bit per capability row <c>camera.*</c>.</summary>
[Flags]
public enum CameraCapability : ulong
{
    None = 0,
    /// <summary><c>camera.server_mode</c>: a stock camera mode lock from the server.</summary>
    ServerMode = 1 << 0,
    /// <summary><c>camera.local_pose</c>: a keyed path.</summary>
    LocalPose = 1 << 1,
    /// <summary><c>camera.local_fov</c>: a lens (field of view).</summary>
    LocalFov = 1 << 2,
    /// <summary><c>camera.local_profile</c>: a follow profile.</summary>
    LocalProfile = 1 << 3,
    /// <summary><c>camera.local_shake</c>: a shake.</summary>
    LocalShake = 1 << 4,
    /// <summary><c>camera.rendered_snapshot</c>: the camera a frame was drawn with (<see cref="Camera.GetVisual"/>).</summary>
    RenderedSnapshot = 1 << 5,
    /// <summary><c>camera.remote</c>: visual requests from the server to players whose client runs Kiln.</summary>
    Remote = 1 << 6,
    /// <summary><c>camera.resource_profile</c>: reserved; never set.</summary>
    ResourceProfile = 1 << 7,
}

/// <summary>What a request may touch besides the picture.</summary>
public enum CameraInputPolicy
{
    /// <summary>Only what is drawn: the player's input camera and aim stay as they are. Every visual part.</summary>
    VisualOnly = 0,
    /// <summary>A stock mode lock: the game's own camera mode changes, and with it what the game derives from it.</summary>
    StockMode = 1,
    /// <summary>Reserved: refused with <see cref="CameraReason.InputPolicyUnsupported"/>.</summary>
    AimToo = 2,
}

/// <summary>Where a follow profile takes its orientation from.</summary>
public enum CameraRotationMode
{
    /// <summary>The game's own orbit: the player keeps turning the view. <see cref="CameraProfile.Rotation"/> stays identity.</summary>
    FreeOrbit = 0,
    /// <summary>The target's heading, upright (yaw only), turned by <see cref="CameraProfile.Rotation"/>.</summary>
    FollowHeading = 1,
    /// <summary><see cref="CameraProfile.Rotation"/> as a fixed world orientation.</summary>
    FixedWorld = 2,
}

/// <summary>Camera collision. This version has none: an authored view can clip terrain and buildings.</summary>
public enum CameraCollisionPolicy
{
    None = 0,
}

/// <summary>How the game moves into a locked stock mode.</summary>
public enum CameraTransitionOverride
{
    /// <summary>The game's own transition for that mode.</summary>
    None = 0,
    BlinkLerpSpeed = 1,
    CameraTransitionLerpSpeed = 2,
}

/// <summary>What <see cref="Camera.Update"/> does with the lease's clock.</summary>
public enum CameraUpdateTiming
{
    /// <summary>The lease keeps its start: the new request is evaluated at the time already elapsed.</summary>
    Preserve = 0,
    /// <summary>The lease starts again: keys, shake and duration count from the update.</summary>
    Restart = 1,
}

/// <summary>What happened to a lease (<see cref="CameraChange"/>).</summary>
public enum CameraChangeKind
{
    Applied = 1,
    Updated = 2,
    Ended = 3,
}

/// <summary>Why a lease ended.</summary>
public enum CameraEndReason
{
    None = 0,
    /// <summary>Its owner ended it (<see cref="Camera.End"/>).</summary>
    Released = 1,
    Expired = 2,
    TargetLost = 3,
    PlayerLeft = 4,
    WorldStopped = 5,
    /// <summary>Another owner's request with a higher priority took its place.</summary>
    Superseded = 6,
    CapabilityLost = 7,
    /// <summary>The mod that held it was unloaded, or one of its camera callbacks faulted.</summary>
    OwnerRetired = 8,
    PeerDisconnected = 9,
    AuthorityTimeout = 10,
    SafetyRejected = 11,
    TransportTimeout = 12,
    /// <summary>Ended by its owner before it was ever applied.</summary>
    CancelledBeforeApply = 13,
}

/// <summary>Why a request was refused. Decide by this, never by the text of <see cref="CameraResult.Reason"/>.</summary>
public enum CameraReason
{
    None = 0,
    /// <summary>Another request holds this camera with an equal or higher priority, or one is being applied.</summary>
    Busy = 1,
    /// <summary>The caller already holds this camera: use <see cref="Camera.Update"/>.</summary>
    AlreadyOwned = 2,
    UnknownLease = 3,
    ForeignOwner = 4,
    /// <summary>The request does not fit the endpoint (a mode lock through <see cref="Camera.SetLocal"/>, another player's lease).</summary>
    WrongEndpoint = 5,
    InputPolicyUnsupported = 6,
    RequiresKilnClient = 7,
    PeerUnavailable = 8,
    Negotiation = 9,
    TargetUnavailable = 10,
    NoCameraFrame = 11,
    StaleWorld = 12,
    ExpiredUpdate = 13,
    Collision = 14,
    TransportTimeout = 15,
    MalformedRemote = 16,
    ResourceBusy = 17,
    FrameFault = 18,
    SnapshotBusy = 19,
    SnapshotStale = 20,
    Bounds = 21,
    /// <summary>Too many camera requests wait already: ask again in a later step.</summary>
    RateLimit = 22,
    /// <summary>A mode lease stays a mode lease, a visual lease a visual one: end it and set a new one.</summary>
    LeaseKind = 23,
    Cancelled = 24,
    ClientCapability = 25,
    InsufficientLifetime = 26,
    BodyTooLarge = 27,
}

/// <summary>What an <c>Ok</c> result proves.</summary>
public enum CameraCommitScope
{
    None = 0,
    /// <summary>The server's state holds the value. A vanilla client sends no acknowledgement.</summary>
    ServerState = 1,
    /// <summary>A frame of that player's client was drawn with it.</summary>
    ClientFrame = 2,
}

/// <summary>A camera request that was accepted: what <see cref="Camera.Update"/> and <see cref="Camera.End"/> take.
/// The service checks the owner and the number itself; the other fields say where the lease lives.</summary>
/// <param name="Value">The lease's number; never reused while the game runs. 0: none.</param>
/// <param name="Endpoint">Whose camera, and who carries it out.</param>
/// <param name="Player">The player whose camera it is.</param>
/// <param name="SessionGeneration">The world the lease belongs to: it ends when that world stops.</param>
public readonly record struct CameraLease(ulong Value, CameraEndpoint Endpoint, EntityId Player, uint SessionGeneration);

/// <summary>Where a camera is and where it looks: +Z of <paramref name="Rotation"/> is forward, +Y up.</summary>
/// <param name="Position">World metres.</param>
/// <param name="Rotation">A unit quaternion.</param>
public sealed record CameraPose(WorldPosition Position, Quaternion Rotation);

/// <summary>A field of view, alone or next to a path, profile or shake.</summary>
/// <param name="FieldOfViewDegrees">The vertical field of view, 5 to 160 degrees.</param>
public sealed record CameraLens(float FieldOfViewDegrees);

/// <summary>One key of a <see cref="CameraPath"/>.</summary>
/// <param name="At">When, from the lease's start, in whole milliseconds. The first key is at zero.</param>
/// <param name="Pose">Where the camera is then.</param>
/// <param name="FieldOfViewDegrees">The field of view then. Either every key of a path has one, or none has.</param>
public sealed record CameraKey(TimeSpan At, CameraPose Pose, float? FieldOfViewDegrees = null);

/// <summary>A camera that moves through keys (a cutscene): positions go in straight lines, orientations the short way
/// round; before the first key and after the last the camera stays on that key.</summary>
/// <param name="Keys">1 to 256 keys with rising times; each within 250 metres of the player.</param>
public sealed record CameraPath(IReadOnlyList<CameraKey> Keys);

/// <summary>A camera that follows an entity at a fixed distance. It has no collision: it can clip terrain and
/// buildings.</summary>
/// <param name="Follow">The entity to follow; <see cref="EntityId.None"/>: the local player.</param>
/// <param name="Distance">Metres from the pivot to the camera, 0.25 to 200. Not added to the game's own distance, and
/// not scaled with the target.</param>
/// <param name="OffsetLocal">The pivot's offset from the target in the target's own axes, at most 100 metres long.</param>
/// <param name="OffsetWorldY">The pivot's offset straight up, -100 to 100 metres.</param>
/// <param name="RotationMode">Where the orientation comes from.</param>
/// <param name="Rotation">See <see cref="CameraRotationMode"/>; null is identity.</param>
/// <param name="Collision">Only <see cref="CameraCollisionPolicy.None"/>.</param>
public sealed record CameraProfile(EntityId Follow, float Distance, Vector3 OffsetLocal,
    float OffsetWorldY = 0, CameraRotationMode RotationMode = CameraRotationMode.FreeOrbit,
    Quaternion? Rotation = null, CameraCollisionPolicy Collision = CameraCollisionPolicy.None)
{
    /// <summary>Whether the profile fixes the orientation (any mode but <see cref="CameraRotationMode.FreeOrbit"/>).</summary>
    public bool LockRotation => RotationMode != CameraRotationMode.FreeOrbit;
}

/// <summary>A shake that moves the camera without turning it. Equal seeds give equal shakes.</summary>
/// <param name="AmplitudeMetres">The largest displacement, 0 to 0.5 metres.</param>
/// <param name="FrequencyHz">0.1 to 30.</param>
/// <param name="Falloff">The shake fades over this last part of the duration; zero: full strength until the end.</param>
/// <param name="Seed">Picks the shake's phases.</param>
public sealed record CameraShake(float AmplitudeMetres, float FrequencyHz, TimeSpan Falloff, uint Seed = 0);

/// <summary>How a visual request fades in and out, in whole milliseconds; both together at most the duration.</summary>
public sealed record CameraBlend(TimeSpan In, TimeSpan Out);

/// <summary>A stock camera mode lock.</summary>
public sealed record CameraModeRequest(CameraMode Mode, CameraTransitionOverride Transition = CameraTransitionOverride.None);

/// <summary>
/// One camera request. Either a <see cref="Mode"/> lock alone (with <see cref="CameraInputPolicy.StockMode"/> and no
/// blend), or visual parts: a <see cref="Path"/> or a <see cref="Profile"/> or neither, plus a <see cref="Lens"/>
/// and/or a <see cref="Shake"/>. <c>new CameraOverride(Lens: new CameraLens(40))</c> changes only the field of view.
/// </summary>
/// <param name="Mode">A stock camera mode lock.</param>
/// <param name="Profile">A follow camera.</param>
/// <param name="Path">A keyed camera.</param>
/// <param name="Lens">A field of view. Not together with keys that carry their own.</param>
/// <param name="Shake">A shake on top of whatever the view is.</param>
/// <param name="Duration">How long the lease lasts, 1 ms to 120 s in whole milliseconds; zero means 10 s. Renew a
/// longer scene with <see cref="Camera.Update"/> and <see cref="CameraUpdateTiming.Restart"/>.</param>
/// <param name="Priority">-1000 to 1000. Another mod's request replaces this one only with a higher number.</param>
/// <param name="Blend">Fade in and out; null: none.</param>
/// <param name="InputPolicy">What the request may touch besides the picture.</param>
public sealed partial record CameraOverride(CameraModeRequest? Mode = null, CameraProfile? Profile = null,
    CameraPath? Path = null, CameraLens? Lens = null, CameraShake? Shake = null,
    TimeSpan Duration = default, int Priority = 0, CameraBlend? Blend = null,
    CameraInputPolicy InputPolicy = CameraInputPolicy.VisualOnly);

/// <summary>The answer to a camera request.</summary>
/// <param name="Ok">True when it happened: see <paramref name="CommitScope"/> for what that proves.</param>
/// <param name="Reason">Why not, in words for a log; null when <paramref name="Ok"/>.</param>
/// <param name="ReasonCode">Why not, to decide by.</param>
/// <param name="Lease">The lease. Also set on a refusal that may need <see cref="Camera.End"/>
/// (<paramref name="MayHaveApplied"/>).</param>
/// <param name="Revision">The lease's revision in force: 1 after the first apply, higher after each update.</param>
/// <param name="CommitScope">What an Ok result proves.</param>
/// <param name="MayHaveApplied">Not confirmed, but a frame may have shown it: end the lease.</param>
/// <param name="AlreadyEnded"><see cref="Camera.End"/> of a lease that had ended already; no second change event follows.</param>
public sealed record CameraResult(bool Ok, string? Reason, CameraReason ReasonCode,
    CameraLease Lease, ulong Revision, CameraCommitScope CommitScope,
    bool MayHaveApplied = false, bool AlreadyEnded = false) : IKilnResult;

/// <summary>A lease was applied, updated or ended (<see cref="Camera.Changed"/>).</summary>
/// <param name="Frame">The client frame that showed it, when one confirmed it; else 0.</param>
public sealed record CameraChange(CameraLease Lease, ulong Revision, CameraChangeKind Kind,
    CameraEndReason EndReason, CameraReason ReasonCode, string? Reason, ulong Frame);

/// <summary>The camera a frame was drawn with: after the game's own shake and after any camera override.</summary>
/// <param name="Frame">Counts the frames this was taken from.</param>
/// <param name="TargetSampleStep">The client step whose positions the frame's camera was built from.</param>
/// <param name="SessionGeneration">The client world it belongs to.</param>
/// <param name="Age">How old the snapshot was when it was read; never above 250 ms.</param>
public sealed record CameraVisualSnapshot(CameraPose Pose, float FieldOfViewDegrees,
    float AspectRatio, float NearPlane, float FarPlane, ulong Frame,
    ulong TargetSampleStep, uint SessionGeneration, TimeSpan Age);

/// <summary>A world point on the screen.</summary>
/// <param name="WithinDepth">Between the near and the far plane.</param>
/// <param name="OnScreen">Inside the viewport.</param>
/// <param name="Pixel">Where; null when it is not within depth or the number does not fit a pixel.</param>
/// <param name="DepthMetres">Metres in front of the camera (negative: behind it).</param>
/// <param name="Frame">The snapshot's frame.</param>
public sealed record CameraProjection(bool WithinDepth, bool OnScreen, Vector2? Pixel, float DepthMetres, ulong Frame);

/// <summary>The answer of <see cref="Camera.GetVisual"/>. Not Ok: there is no frame to give
/// (<see cref="CameraReason.NoCameraFrame"/>, <see cref="CameraReason.SnapshotBusy"/>, <see cref="CameraReason.SnapshotStale"/>).</summary>
public sealed record CameraVisualResult(bool Ok, string? Reason, CameraReason ReasonCode, CameraVisualSnapshot? Snapshot) : IKilnResult;
