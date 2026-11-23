// SPDX-License-Identifier: MIT
// Kiln.Camera's mirror of kiln_camera.h: the struct sizes of native/services/camera/abi_asserts.h, and every enum value
// read from the header itself, so a number cannot drift between the service, the wrapper and the wire.
using System.Reflection;
using System.Text.RegularExpressions;
using Kiln;
using Xunit;

namespace CameraTests;

public sealed unsafe class AbiTests
{
    [Fact]
    public void The_native_mirror_has_the_headers_layout()
    {
        Assert.Equal(40, sizeof(CameraNative.Pose));
        Assert.Equal(56, sizeof(CameraNative.Key));
        Assert.Equal(52, sizeof(CameraNative.Profile));
        Assert.Equal(16, sizeof(CameraNative.Shake));
        Assert.Equal(128, sizeof(CameraNative.Request));
        Assert.Equal(72, sizeof(CameraNative.Result));
        Assert.Equal(72, sizeof(CameraNative.Change));
        Assert.Equal(96, sizeof(CameraNative.Visual));
        Assert.Equal(88, sizeof(CameraNative.Table));
        CameraNative.Request r = default;
        var start = (byte*)&r;
        Assert.Equal(32, (int)((byte*)&r.Priority - start));
        Assert.Equal(44, (int)(&r.CameraId - start));
        Assert.Equal(48, (int)((byte*)&r.Profile - start));
        Assert.Equal(100, (int)((byte*)&r.Shake - start));
        Assert.Equal(116, (int)((byte*)&r.KeyCount - start));
        Assert.Equal(120, (int)((byte*)&r.Keys - start));
        CameraNative.Result result = default;
        Assert.Equal(56, (int)((byte*)&result.Reason - (byte*)&result));
        CameraNative.Visual visual = default;
        Assert.Equal(80, (int)((byte*)&visual.FovRadians - (byte*)&visual));
    }

    private static Dictionary<string, long> HeaderValues()
    {
        var repo = typeof(AbiTests).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>().Single(a => a.Key == "KilnRepository").Value!;
        var text = File.ReadAllText(Path.Combine(repo, "native", "core", "abi", "services", "kiln_camera.h"));
        var values = new Dictionary<string, long>(StringComparer.Ordinal);
        foreach (Match m in Regex.Matches(text, @"\bKILN_CAMERA_(\w+)\s*=\s*(\d+)")) values[m.Groups[1].Value] = long.Parse(m.Groups[2].Value);
        foreach (Match m in Regex.Matches(text, @"#define\s+KILN_CAMERA_(CAP_\w+)\s+\(UINT64_C\(1\)\s*<<\s*(\d+)\)")) values[m.Groups[1].Value] = 1L << int.Parse(m.Groups[2].Value);
        return values;
    }

    // "PEER_UNAVAILABLE" for PeerUnavailable
    private static string Upper(string name) => Regex.Replace(name, "(?<=[a-z0-9])(?=[A-Z])", "_").ToUpperInvariant();

    private static void Same<T>(Dictionary<string, long> header, string prefix, Func<T, string>? nameOf = null) where T : struct, Enum
    {
        var seen = 0;
        foreach (var value in Enum.GetValues<T>())
        {
            var name = prefix + (nameOf?.Invoke(value) ?? Upper(value.ToString()));
            Assert.True(header.TryGetValue(name, out var number), $"kiln_camera.h has no KILN_CAMERA_{name} for {typeof(T).Name}.{value}");
            Assert.Equal(number, Convert.ToInt64(value));
            seen++;
        }
        // and the header has no value of the family that the enum lacks
        Assert.Equal(header.Keys.Count(k => k.StartsWith(prefix, StringComparison.Ordinal)), seen);
    }

    [Fact]
    public void Every_enum_value_is_the_headers_number()
    {
        var h = HeaderValues();
        Assert.True(h.Count > 70, $"only {h.Count} values were read from kiln_camera.h");
        Same<CameraEndReason>(h, "END_");
        Same<CameraReason>(h, "REASON_", v => v == CameraReason.InputPolicyUnsupported ? "INPUT_POLICY" : Upper(v.ToString()));
        Same<CameraCommitScope>(h, "COMMIT_");
        Assert.Equal(h["CAP_SERVER_MODE"], (long)CameraCapability.ServerMode);
        Assert.Equal(h["CAP_LOCAL_POSE"], (long)CameraCapability.LocalPose);
        Assert.Equal(h["CAP_LOCAL_FOV"], (long)CameraCapability.LocalFov);
        Assert.Equal(h["CAP_LOCAL_PROFILE"], (long)CameraCapability.LocalProfile);
        Assert.Equal(h["CAP_LOCAL_SHAKE"], (long)CameraCapability.LocalShake);
        Assert.Equal(h["CAP_RENDERED_SNAPSHOT"], (long)CameraCapability.RenderedSnapshot);
        Assert.Equal(h["CAP_REMOTE"], (long)CameraCapability.Remote);
        Assert.Equal(h["CAP_RESOURCE_PROFILE"], (long)CameraCapability.ResourceProfile);
        Assert.Equal((h["SERVER_MODE"], h["LOCAL_VISUAL"], h["REMOTE_VISUAL"]), ((long)CameraEndpoint.ServerMode, (long)CameraEndpoint.Local, (long)CameraEndpoint.Remote));
        Assert.Equal((h["VISUAL_ONLY"], h["STOCK_MODE"], h["AIM_TOO"]), ((long)CameraInputPolicy.VisualOnly, (long)CameraInputPolicy.StockMode, (long)CameraInputPolicy.AimToo));
        Assert.Equal((h["FREE_ORBIT"], h["FOLLOW_HEADING"], h["FIXED_WORLD"]),
            ((long)CameraRotationMode.FreeOrbit, (long)CameraRotationMode.FollowHeading, (long)CameraRotationMode.FixedWorld));
        Assert.Equal((h["TRANSITION_NONE"], h["TRANSITION_BLINK"], h["TRANSITION_LERP"]),
            ((long)CameraTransitionOverride.None, (long)CameraTransitionOverride.BlinkLerpSpeed, (long)CameraTransitionOverride.CameraTransitionLerpSpeed));
        Assert.Equal((h["PRESERVE_TIMELINE"], h["RESTART_TIMELINE"]), ((long)CameraUpdateTiming.Preserve, (long)CameraUpdateTiming.Restart));
        Assert.Equal((h["APPLIED"], h["UPDATED"], h["ENDED"]), ((long)CameraChangeKind.Applied, (long)CameraChangeKind.Updated, (long)CameraChangeKind.Ended));
        Assert.Equal(h["COLLISION_NONE"], (long)CameraCollisionPolicy.None);
        Assert.Equal((h["MODE"], h["PATH"], h["PROFILE"], h["SHAKE"], h["LENS"]),
            ((long)CameraNative.PartMode, (long)CameraNative.PartPath, (long)CameraNative.PartProfile, (long)CameraNative.PartShake, (long)CameraNative.PartLens));
        Assert.Equal((h["LENS_NONE"], h["LENS_CONSTANT"], h["LENS_KEYS"]), ((long)CameraNative.LensNone, (long)CameraNative.LensConstant, (long)CameraNative.LensKeys));
        Assert.Equal((h["MAY_HAVE_APPLIED"], h["ALREADY_ENDED"]), ((long)CameraNative.MayHaveApplied, (long)CameraNative.AlreadyEnded));
        // the game's camera ids: 46 of them, numbered without a gap, building first person at 34
        Assert.Equal(Enumerable.Range(0, 46).Select(i => (byte)i), Enum.GetValues<CameraMode>().Select(m => (byte)m));
        Assert.Equal(34, (int)CameraMode.PlayerCameraBuildingFirstPerson);
    }
}
