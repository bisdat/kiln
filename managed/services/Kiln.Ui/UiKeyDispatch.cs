// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

// Pad polling can happen on a worker. Consumption is decided there; mod callbacks run after the next game UI frame,
// on the game's main thread, never on the overlay's render thread. A stalled UI cannot retain unbounded callbacks.
internal static unsafe class UiKeyDispatch
{
    private const int Limit = 4096;
    private static readonly object Lock = new();
    private static readonly Queue<Action> Pending = new();
    private static ulong _tickHandle;

    // The production pad router checks this before taking an action. TestKit instead supplies its own simulated tick.
    internal static bool EnsureStarted()
    {
        lock (Lock)
        {
            if (_tickHandle != 0) return true;
            if (!UiNative.HasGameUi || UiNative.T->SubscribeUiTick == null) return false;
            ulong handle = 0;
            if (UiNative.T->SubscribeUiTick(0, &OnTick, null, &handle) != UiNative.Ok || handle == 0) return false;
            _tickHandle = handle;
            return true;
        }
    }

    internal static bool Post(Action work, bool release = false)
    {
        lock (Lock)
        {
            if (!release && Pending.Count >= Limit) return false; // never lose the release of an accepted hold
            EnsureStarted(); // native ticks when present; TestKit explicitly drains a simulated frame
            Pending.Enqueue(work);
            return true;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnTick(UiNative.Screen* screen, void* user) => Drain();

    internal static void Drain()
    {
        Action[] work;
        lock (Lock)
        {
            work = Pending.ToArray();
            Pending.Clear();
        }
        foreach (var action in work)
        {
            try { action(); }
            catch (Exception e) { UiEngine.Log(LogLevel.Error, $"pad callback: {e}"); }
        }
    }

    internal static void ResetForTests()
    {
        lock (Lock)
        {
            Pending.Clear();
            _tickHandle = 0;
        }
    }
}
