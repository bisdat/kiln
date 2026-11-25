// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>
/// Player storage whose world write completed before Kiln.Saves' writer had its staging on disk (since 0.2.0-dev.31.5): what
/// kiln.log says about it. A writer that is only slow is a delay, not an error: on one player's PC every snapshot staging's
/// renames took 6-25 s for hours, nearly every 30 s autosave's world write went ahead after its bounded wait, the writer
/// caught up each time, and kiln.log had an error line per save (the launcher counted 373). Durability does not change
/// (<see cref="PlayerStorageStore.Committed"/>: the pairs stay in memory and the next save retries, exactly as before);
/// only the classification does: the first delay of a world session is a warning, the first catch-up an info line, later
/// ones are counted and summed up at most every <see cref="SummaryEvery"/>. A staging that cannot be written stays an
/// error (the caller's), and so does one still not on disk <see cref="StuckAfter"/> after its world write (a disk that
/// does not answer). Returns the lines; the caller logs them. Tick thread (the pump and the world write's confirmation).
/// </summary>
internal sealed class DurabilityLag(Func<TimeSpan> now)
{
    internal static readonly TimeSpan SummaryEvery = TimeSpan.FromMinutes(5);

    /// <summary>A staging not on disk this long after its world write is no longer a delay but an error.</summary>
    internal static readonly TimeSpan StuckAfter = TimeSpan.FromMinutes(2);

    /// <summary>A monotonic clock for the store's own tracker.</summary>
    internal static TimeSpan SystemClock() => TimeSpan.FromMilliseconds(Environment.TickCount64);

    private const int KeepLate = 8;  // generations; the writer is one staging behind, never more, when it is only slow

    private readonly Dictionary<uint, (TimeSpan Since, bool Stuck, object? Job)> _late = [];
    private bool _warned, _caughtSaid;
    private TimeSpan _lastLine;
    private int _lateCount, _caughtCount;
    private TimeSpan _slowest;

    /// <summary>The world write of <paramref name="generation"/> completed and its staging is not on disk yet.</summary>
    internal bool IsLate(uint generation) => _late.ContainsKey(generation);

    /// <summary>The world write of <paramref name="generation"/> completed while the writer still had its staging
    /// (<paramref name="job"/>: the one the world write waited for) queued or being written. A warning the first time in a
    /// world session; counted after that.</summary>
    internal (LogLevel Level, string Text)? Late(uint generation, object? job = null)
    {
        var t = now();
        _late[generation] = (t, false, job);
        // Bounded, but never by dropping the oldest: a writer that hangs is found by it (Stuck), however often the world saves
        // meanwhile (the barrier is off then, so every world write goes ahead at once).
        while (_late.Count > KeepLate) _late.Remove(_late.Keys.Order().Skip(1).First());
        if (_warned)
        {
            _lateCount++;
            return null;
        }
        _warned = true;
        _lastLine = t;
        return (LogLevel.Warn, $"player-storage: world generation {generation} was written before its storage reached the disk: " +
                               "Kiln.Saves' writer is slow here (a delay, not an error; a staging that cannot be written is logged as an " +
                               "error); pairs kept in memory, next save retries; kiln.log says when it reaches the disk and sums up " +
                               "further delays at most every 5 minutes");
    }

    /// <summary>The writer finished a staging of <paramref name="generation"/> (<paramref name="success"/>: on disk). Only
    /// the late one counts (<paramref name="job"/>; the save request's staging of the same generation can finish after the
    /// world write too, while the snapshot staging it waited for is still queued). A late one's first catch-up of a world
    /// session is an info line, later ones are counted; a failure is the caller's to log.</summary>
    internal (LogLevel Level, string Text)? Landed(uint generation, bool success, object? job = null)
    {
        if (!_late.TryGetValue(generation, out var late) || (late.Job != null && !ReferenceEquals(late.Job, job))) return null;
        _late.Remove(generation);
        if (!success) return null;
        var t = now();
        var lag = t - late.Since;
        if (late.Stuck)
            return (LogLevel.Info, $"player-storage: generation {generation}'s storage reached the disk {lag.TotalSeconds:F1} s after its world " +
                                   "write (reported above as not on disk)");
        if (_caughtSaid)
        {
            _caughtCount++;
            if (lag > _slowest) _slowest = lag;
            return null;
        }
        _caughtSaid = true;
        _lastLine = t;
        return (LogLevel.Info, $"player-storage: generation {generation}'s storage reached the disk {lag.TotalSeconds:F1} s after its world write; " +
                               "its carried copy waits for the next save, as always");
    }

    /// <summary>Late generations still not on disk <see cref="StuckAfter"/> after their world write, each once.</summary>
    internal List<(uint Generation, TimeSpan Age)> Stuck()
    {
        var t = now();
        var stuck = new List<(uint, TimeSpan)>();
        foreach (var (gen, late) in _late.ToArray())
            if (!late.Stuck && t - late.Since >= StuckAfter)
            {
                _late[gen] = late with { Stuck = true };
                stuck.Add((gen, t - late.Since));
            }
        return stuck;
    }

    /// <summary>What was counted since the last line, once <see cref="SummaryEvery"/> has passed since it; counting starts
    /// from zero again.</summary>
    internal (LogLevel Level, string Text)? Summary()
    {
        var t = now();
        if (_lateCount + _caughtCount == 0 || t - _lastLine < SummaryEvery) return null;
        var waiting = _late.Count(l => !l.Value.Stuck);
        var text = $"player-storage: in the last {(t - _lastLine).TotalMinutes:F0} min, " +
                   (_lateCount == 1 ? "1 more world save was written before its storage reached the disk"
                                    : $"{_lateCount} more world saves were written before their storage reached the disk") +
                   (_caughtCount > 0 ? $"; {_caughtCount} reached it later, at worst {_slowest.TotalSeconds:F1} s after its world write" : "") +
                   (waiting > 0 ? $"; {waiting} not yet" : "") +
                   " (Kiln.Saves' writer is slow, not failing: a delay, not an error; summed up at most every 5 minutes)";
        _lateCount = _caughtCount = 0;
        _slowest = TimeSpan.Zero;
        _lastLine = t;
        return (LogLevel.Warn, text);
    }

    /// <summary>A new world session: nothing of the last one counts, and its first ones are said again.</summary>
    internal void Clear()
    {
        _late.Clear();
        _warned = _caughtSaid = false;
        _lateCount = _caughtCount = 0;
        _slowest = TimeSpan.Zero;
    }
}
