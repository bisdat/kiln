// SPDX-License-Identifier: MIT
// Run the SDK sample's real Mode command and its loaded copy's local handlers. The kit has no kiln.client fake:
// Client.Post and pictures are not claimed tested here; the renderer's thread/epoch path has native host tests.
using System.Numerics;
using System.Reflection;
using CameraSample;
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace CameraTests;

public sealed class SampleTests
{
    private static KilnHarness Started(out ModHandle mod)
    {
        var kiln = new KilnHarness();
        mod = kiln.Load<CameraSampleMod>(new ModManifest("camera-sample"));
        kiln.Step(2);
        return kiln;
    }

    private static object? Invoke(ModHandle mod, string name, params object[] args) =>
        mod.Instance!.GetType().GetMethod(name, BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static)!
            .Invoke(mod.Instance, args);

    [Fact]
    public void The_samples_real_command_applies_and_explicitly_ends_a_mode_lease()
    {
        using var kiln = Started(out _);
        var player = kiln.Players.Join("Player", 76561198000000401, PlayerPermissions.KickBan);
        kiln.Step();
        var result = kiln.RunCommand(player, "camera-mode");
        Assert.True(result.Ok, result.Error);
        kiln.StepUntil(() => player.Chat.Any(l => l.Text.Contains("ServerState")), pace: TimeSpan.FromMilliseconds(1));
        var apply = Assert.Single(kiln.Camera.Calls);
        Assert.Equal((FakeCameraCallKind.Apply, 1u, 1u, 5000u, player.Entity), (apply.Kind, apply.Endpoint, apply.Parts, apply.DurationMs, apply.Player));
        Assert.Single(kiln.Camera.Leases);
        Assert.True(kiln.RunCommand(player, "camera-mode-off").Ok);
        kiln.StepUntil(() => player.Chat.Any(l => l.Text.Contains("Stock mode released")), pace: TimeSpan.FromMilliseconds(1));
        Assert.Equal(FakeCameraCallKind.End, kiln.Camera.Calls[^1].Kind);
        Assert.Empty(kiln.Camera.Leases);
    }

    [Fact]
    public void The_mode_command_reports_a_refusal_and_local_commands_do_not_run_on_a_server()
    {
        using var kiln = Started(out _);
        var player = kiln.Players.Join("Player", 76561198000000402, PlayerPermissions.KickBan);
        kiln.Step();
        kiln.Camera.RefuseNext((uint)CameraReason.PeerUnavailable);
        var refused = kiln.RunCommand(player, "camera-mode");
        Assert.True(refused.Ok, refused.Error);
        kiln.StepUntil(() => player.Chat.Any(l => l.Text.Contains("PeerUnavailable")), pace: TimeSpan.FromMilliseconds(1));
        Assert.Empty(kiln.Camera.Leases);
        Assert.False(kiln.RunCommand(player, "camera-mode-off").Ok);
        foreach (var command in new[] { "camera-lens", "camera-path", "camera-local-off" })
        {
            var local = kiln.RunCommand(command);
            Assert.False(local.Ok);
            Assert.Contains("hosting game client", local.Error);
        }
        Assert.Single(kiln.Camera.Calls); // the refused mode only, no accidental local mutation on a server
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Lens_only_and_two_key_path_samples_run_on_the_camera_fake_and_end_their_lease(bool path)
    {
        using var kiln = Started(out var mod);
        var pose = new CameraPose(new WorldPosition(1, 2, 3), Quaternion.Identity);
        var request = (CameraOverride)kiln.As(mod, () => path ? Invoke(mod, "TwoKeyPath", pose) : Invoke(mod, "LensOnly"))!;
        if (path)
        {
            Assert.Equal(2, request.Path!.Keys.Count);
            Assert.Equal(pose, request.Path.Keys[0].Pose);
            Assert.Equal(new WorldPosition(3, 2, 3), request.Path.Keys[1].Pose.Position);
            Assert.Equal(TimeSpan.FromSeconds(2), request.Path.Keys[1].At);
            Assert.Equal(new CameraBlend(TimeSpan.FromMilliseconds(200), TimeSpan.FromMilliseconds(300)), request.Blend);
        }
        else
        {
            kiln.Capabilities.Override("camera.local_pose", false, "lens needs no pose");
            kiln.Capabilities.Override("camera.local_profile", false, "lens needs no profile");
            Assert.Equal(40, request.Lens!.FieldOfViewDegrees);
        }
        kiln.As(mod, () => Invoke(mod, "StartLocal", request));
        kiln.Step();
        kiln.As(mod, () => Invoke(mod, "CompleteLocal"));
        var apply = Assert.Single(kiln.Camera.Calls);
        Assert.Equal((2u, path ? 2u : 16u, path ? 3000u : 5000u), (apply.Endpoint, apply.Parts, apply.DurationMs));
        Assert.Single(kiln.Camera.Leases);
        kiln.As(mod, () => Invoke(mod, "EndLocal"));
        kiln.Step();
        kiln.As(mod, () => Invoke(mod, "CompleteLocal"));
        Assert.Empty(kiln.Camera.Leases);
        Assert.Equal(FakeCameraCallKind.End, kiln.Camera.Calls[^1].Kind);
        Assert.Contains(kiln.LogsOf("camera-sample"), l => l.Text.Contains("released"));
    }

    [Fact]
    public void The_local_sample_logs_a_refusal_and_does_not_claim_a_lease()
    {
        using var kiln = Started(out var mod);
        kiln.Camera.RefuseNext((uint)CameraReason.NoCameraFrame);
        var request = (CameraOverride)kiln.As(mod, () => Invoke(mod, "LensOnly"))!;
        kiln.As(mod, () => Invoke(mod, "StartLocal", request));
        kiln.Step();
        kiln.As(mod, () => Invoke(mod, "CompleteLocal"));
        Assert.Empty(kiln.Camera.Leases);
        Assert.Contains(kiln.LogsOf("camera-sample"), l => l.Text.Contains("NoCameraFrame"));
        kiln.As(mod, () => Invoke(mod, "EndLocal"));
        Assert.Single(kiln.Camera.Calls); // no End on a default lease
    }
}
