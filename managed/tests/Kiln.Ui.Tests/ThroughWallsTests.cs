// SPDX-License-Identifier: MIT
// OutlineOptions.ThroughWalls' silhouette (0.2.0-dev.31, GitHub issue 111 follow-up): the box at the entity's position
// projected with the camera world labels use, its hull clockwise on screen, nothing behind the camera, the pulse.
using System.Numerics;
using Xunit;

namespace Kiln.ThroughWallsTests;

public sealed class ThroughWallsTests
{
    private const float Fov = MathF.PI / 3;

    private static float Area(ReadOnlySpan<Vector2> p)
    {
        var a = 0f;
        for (var i = 0; i < p.Length; i++)
        {
            var (u, v) = (p[i], p[(i + 1) % p.Length]);
            a += u.X * v.Y - v.X * u.Y;
        }
        return a / 2;
    }

    [Fact]
    public void The_hull_drops_inner_points_and_runs_clockwise_on_screen()
    {
        Span<Vector2> pts = [new(10, 10), new(110, 10), new(60, 50), new(110, 90), new(10, 90), new(50, 40)];
        Span<Vector2> hull = stackalloc Vector2[pts.Length + 1];
        var n = ThroughWalls.Hull(pts, hull);
        Assert.Equal(4, n);
        Assert.True(Area(hull[..n]) > 0);  // y grows down: a positive shoelace area is clockwise on screen
        Assert.Equal(0, ThroughWalls.Hull(pts[..2], hull));
    }

    [Fact]
    public void A_box_in_front_of_the_camera_projects_to_a_hull_around_its_centre_and_one_behind_to_nothing()
    {
        var cam = new RenderCamera(new WorldPosition(500, 41, 1000), Quaternion.Identity, Fov);  // looking along +Z
        Assert.True(cam.Valid);
        Span<Vector2> corners = stackalloc Vector2[8];
        Span<Vector2> hull = stackalloc Vector2[9];
        var n = ThroughWalls.Silhouette(cam, new WorldPosition(500, 40, 1010), 1920, 1080, corners, hull);
        Assert.InRange(n, 4, 8);
        Assert.True(cam.Project(new WorldPosition(500, 40 + ThroughWalls.Height / 2, 1010), 1920, 1080, out var mid, out _));
        float minX = float.MaxValue, maxX = float.MinValue, minY = float.MaxValue, maxY = float.MinValue;
        foreach (var p in hull[..n])
            (minX, maxX, minY, maxY) = (MathF.Min(minX, p.X), MathF.Max(maxX, p.X), MathF.Min(minY, p.Y), MathF.Max(maxY, p.Y));
        Assert.InRange(mid.X, minX, maxX);
        Assert.InRange(mid.Y, minY, maxY);
        Assert.InRange(maxX - minX, 50, 400);  // 1.2 m at 10 m on a 1080p screen: about 100 pixels
        Assert.Equal(0, ThroughWalls.Silhouette(cam, new WorldPosition(500, 40, 990), 1920, 1080, corners, hull));  // behind
    }

    [Fact]
    public void The_pulse_breathes_from_full_to_forty_percent_in_one_point_two_seconds()
    {
        Assert.Equal(1f, ThroughWalls.Alpha(false, 600));
        Assert.Equal(1f, ThroughWalls.Alpha(true, 0), 3);
        Assert.Equal(0.4f, ThroughWalls.Alpha(true, 600), 3);
        Assert.Equal(1f, ThroughWalls.Alpha(true, 1200), 3);
    }
}
