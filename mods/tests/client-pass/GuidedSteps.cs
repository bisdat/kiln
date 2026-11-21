// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;

namespace ClientPass;

// A guided check gets ONE verdict: the player's first answer is the check's (CheckContext.Verdict), and an answer given
// before the body asks for it ends the check. So a check with several steps goes from step to step on what it observes
// (CheckContext.Until on events, counters, item state) and asks for the verdict once, at the end. When a step shows
// itself only in the game's item detail view, DetailWatch observes that view through a tooltip-lines provider that adds
// nothing; where Kiln cannot register one, the step waits a fixed time instead and its instruction says so.
public sealed partial class ClientPassMod
{
    /// <summary>Counts how often the game's item detail view came to one item type: the first time it is drawn, after the
    /// view showed another item, or after a pause in drawing (a re-selection).</summary>
    private sealed class DetailWatch : IDisposable
    {
        private readonly HashSet<uint> _items;
        private readonly UiValueRegistration? _reg;
        private uint _lastItem;
        private ulong _lastFrame;
        private int _views;

        public DetailWatch(params ItemId[] items)
        {
            _items = items.Where(i => i.Value != 0).Select(i => i.Value).ToHashSet();
            if (_items.Count == 0) return;
            try
            {
                _reg = Ui.Tooltip.AddLines((in UiValueContext x, in UiItemView v) =>
                {
                    var item = v.Item.Value;
                    if (_items.Contains(item) && (item != _lastItem || x.Frame > _lastFrame + 30)) Interlocked.Increment(ref _views);
                    _lastItem = item;
                    _lastFrame = x.Frame;
                    return UiTooltipLines.None;
                });
            }
            catch (Exception)  // the row is off or the native service is missing: the steps wait a fixed time
            {
                _reg = null;
            }
        }

        /// <summary>True when the provider is registered, so <see cref="Views"/> counts.</summary>
        public bool Ok => _reg is { Ok: true };

        /// <summary>How often the view came to one of the items.</summary>
        public int Views => Volatile.Read(ref _views);

        public void Dispose() => _reg?.Dispose();
    }

    /// <summary>Starts a DetailWatch for the check, disposed at its end.</summary>
    private static DetailWatch WatchDetail(CheckContext c, params ItemId[] items)
    {
        var w = new DetailWatch(items);
        c.OnEnd(() => w.Dispose());
        if (!w.Ok) c.Log.Info($"client-pass: {c.Id}: the item detail view cannot be watched here; its steps wait a fixed time");
        return w;
    }

    /// <summary>The end of a step instruction: what the check waits for before it goes on.</summary>
    private static string Then(DetailWatch w, string seen, int seconds) =>
        w.Ok ? $"The check goes on by itself once {seen}." : $"The check goes on by itself in {seconds} seconds.";

    /// <summary>Waits until the view came to the watched items <paramref name="views"/> times in all (or, where it cannot
    /// be watched, <paramref name="seconds"/>), then <paramref name="linger"/> seconds more for the player to read it.</summary>
    private static async Task<bool> UntilViews(CheckContext c, DetailWatch w, int views, string what, int seconds, int linger = 0)
    {
        var ok = true;
        if (w.Ok) ok = await c.Until(() => w.Views >= views, TimeSpan.FromMinutes(5), what);
        else await c.Delay(TimeSpan.FromSeconds(seconds));
        if (linger > 0) await c.Delay(TimeSpan.FromSeconds(linger));
        return ok;
    }
}
