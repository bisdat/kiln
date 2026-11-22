// SPDX-License-Identifier: MIT
// OutlineOptions.ThroughWalls (0.2.0-dev.31, GitHub issue 111 follow-up): the game's highlight is depth-tested with the
// model and no flag of the game draws it through walls (docs/re/client-render.md "Through walls"), so Kiln draws a
// screen-space silhouette on its own UI layer: kiln.render v3 reads the entity's position for a through-walls request at
// each client step (outline_info), and every overlay frame Kiln projects a box at that position with the camera world
// labels use (kiln.render's camera, RenderCamera.Project), takes the hull of its corners and draws it as a soft fill and
// an outline in the request's colour, breathing with Pulse. The outline's end (Duration, Dispose, despawn) ends it.
using System.Numerics;

namespace Kiln;

internal static unsafe class ThroughWalls
{
    /// <summary>The box drawn around the entity's position (its RenderTransform's origin, at the model's foot): metres.</summary>
    internal const double HalfWidth = 0.6, Height = 1.2;
    internal const float FillShare = 0.28f, Thickness = 2f;

    private static int _drawing;
    private static readonly Action<UiDraw> DrawFn = Draw;

    /// <summary>A through-walls outline was added: draw from the next overlay frame on.</summary>
    internal static void Started()
    {
        if (Interlocked.CompareExchange(ref _drawing, 1, 0) != 0) return;
        UiEngine.EnsureStarted();
        var prev = ModContext.CurrentMod;
        ModContext.CurrentMod = null;  // Kiln's own handler: a mod's unload must not take it
        try { UiEngine.FrameHandlers.Add(DrawFn); }
        finally { ModContext.CurrentMod = prev; }
        UiEngine.Recount();
    }

    private static void StopIfIdle()
    {
        if (WorldOutlines.LiveThroughWalls().Count != 0 || Interlocked.CompareExchange(ref _drawing, 0, 1) != 1) return;
        UiEngine.FrameHandlers.Remove(DrawFn);
        UiEngine.Recount();
        if (WorldOutlines.LiveThroughWalls().Count != 0) Started();  // one came in between
    }

    internal static bool Drawing => Volatile.Read(ref _drawing) != 0;

    // render thread, inside kiln.ui's frame (kiln.render's drawing works there)
    private static void Draw(UiDraw ui)
    {
        var live = WorldOutlines.LiveThroughWalls();
        if (live.Count == 0)
        {
            StopIfIdle();
            return;
        }
        var t = RenderNative.T;
        if (t == null || !Render.CameraAvailable) return;
        var cam = Render.Camera;
        Span<Vector2> corners = stackalloc Vector2[8];
        Span<Vector2> hull = stackalloc Vector2[9];
        var now = Environment.TickCount64;
        foreach (var o in live)
        {
            if (WorldOutlines.Info(o.Handle) is not { } i || i.State == RenderNative.OutlineEnded || i.PositionOk == 0) continue;
            var n = Silhouette(cam, new WorldPosition(i.X, i.Y, i.Z), ui.Width, ui.Height, corners, hull);
            if (n < 3) continue;
            var a = Alpha(o.Options.Pulse, now);
            var c = o.Options.Color;
            fixed (Vector2* p = hull)
            {
                t->Polyline((float*)p, (uint)n, c.WithAlpha((byte)Math.Clamp(c.A * a * FillShare, 0, 255)).Rgba, 1, 1, 1);
                t->Polyline((float*)p, (uint)n, c.WithAlpha((byte)Math.Clamp(c.A * a, 0, 255)).Rgba, Thickness * ui.Scale, 1, 0);
            }
        }
    }

    /// <summary>The breathing of <see cref="OutlineOptions.Pulse"/> as kiln.render's palette does it: a 1.2 s period, 40 % to
    /// 100 %; 1 without it.</summary>
    internal static float Alpha(bool pulse, long nowMs) =>
        pulse ? 0.7f + 0.3f * MathF.Cos(2 * MathF.PI * (nowMs % 1200) / 1200f) : 1f;

    /// <summary>The box at <paramref name="at"/> projected on a <paramref name="width"/> x <paramref name="height"/> screen:
    /// its hull in <paramref name="hull"/> (clockwise on screen, as a filled convex polygon wants it), the count returned;
    /// 0 when a corner is behind the camera's near plane (nothing is drawn rather than a mirrored shape).</summary>
    internal static int Silhouette(in RenderCamera cam, WorldPosition at, float width, float height, Span<Vector2> corners, Span<Vector2> hull)
    {
        var k = 0;
        for (var y = 0; y < 2; y++)
            for (var dx = -1; dx <= 1; dx += 2)
                for (var dz = -1; dz <= 1; dz += 2)
                {
                    var p = new WorldPosition(at.X + dx * HalfWidth, at.Y + y * Height, at.Z + dz * HalfWidth);
                    if (!cam.Project(p, width, height, out var s, out _) || !float.IsFinite(s.X) || !float.IsFinite(s.Y)) return 0;
                    corners[k++] = s;
                }
        return Hull(corners[..k], hull);
    }

    /// <summary>The convex hull of <paramref name="points"/> (Andrew's monotone chain), clockwise on a screen whose y grows
    /// down; the count written to <paramref name="hull"/> (at least points + 1 long).</summary>
    internal static int Hull(Span<Vector2> points, Span<Vector2> hull)
    {
        if (points.Length < 3) return 0;
        points.Sort((a, b) => a.X != b.X ? a.X.CompareTo(b.X) : a.Y.CompareTo(b.Y));
        static float Cross(Vector2 o, Vector2 a, Vector2 b) => (a.X - o.X) * (b.Y - o.Y) - (a.Y - o.Y) * (b.X - o.X);
        var n = 0;
        for (var i = 0; i < points.Length; i++)
        {
            while (n >= 2 && Cross(hull[n - 2], hull[n - 1], points[i]) <= 0) n--;
            hull[n++] = points[i];
        }
        for (int i = points.Length - 2, lower = n + 1; i >= 0; i--)
        {
            while (n >= lower && Cross(hull[n - 2], hull[n - 1], points[i]) <= 0) n--;
            hull[n++] = points[i];
        }
        return Math.Max(0, n - 1);  // the last point is the first again
    }
}
