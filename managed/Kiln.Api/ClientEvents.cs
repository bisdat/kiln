// SPDX-License-Identifier: MIT
// Events of a game client's own worlds (package 14, chapter 14 sections 9.1 and 9.3): EcsProgramRefusedEvent and Kiln.World's
// ClientComponentWrittenEvent are raised on the game's main thread, in the client world's next Pre, where a handler may
// touch the client world (on a host the server's tick is another thread: a handler there touching the client world is the
// bug this prevents). They are queued from any thread (a game worker that ran a client program) with the client world
// session they belong to (kiln.client's client_session) and dropped when that session has ended before its next Pre.
// The bus's own handler list runs them (Events.HandlersOf), as Kiln.Ui's render events on the render thread. Nothing of
// it runs before the first such event: no kiln.client subscription, no queue.
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

internal static unsafe class ClientEvents
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeState
    {
        public uint Size, Side, Session, Generation, LocalPlayer, Flags, ClientSession, ServerSession;
        public ulong ClientSteps, ServerSteps;
    }

    // kiln.client v1 (native/core/abi/services/kiln_client.h): the members used here
    [StructLayout(LayoutKind.Sequential)]
    internal struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<NativeState*, int> State;
        public delegate* unmanaged[Cdecl]<ulong, int, delegate* unmanaged[Cdecl]<void*, void*, void>, void*, ulong*, int> SubscribeTick;
        public delegate* unmanaged[Cdecl]<ulong, int> Unsubscribe;
        public nint Post, OnClientThread, ClientWorld;
    }

    private const int MaxQueued = 65536;
    private static readonly ConcurrentQueue<(KilnEvent Event, uint Session)> Queue = new();
    private static readonly object Lock = new();
    private static int _subscribed;  // 0 not yet, 1 subscribed, -1 no kiln.client here
    private static long _dropped, _delivered;
    private static int _queued;  // entries in Queue: taken before an enqueue, given back at each dequeue (the real bound)
    private static int _droppedLogged;

    /// <summary>Run first in each client Pre (EcsPrograms' refusal poll), on the main thread.</summary>
    internal static Action? BeforeEvents;

    private static Table* T => Services.Get<Table>("kiln.client", 1);

    /// <summary>The client world session now (client worlds since the game started), or 0 without kiln.client.</summary>
    internal static uint Session
    {
        get
        {
            var t = T;
            if (t == null) return 0;
            NativeState s;
            s.Size = (uint)sizeof(NativeState);
            return t->State(&s) == 0 ? s.ClientSession : 0;
        }
    }

    /// <summary>Queues <paramref name="ev"/> for the next client Pre of the session that runs now. Any thread. False when
    /// it cannot be delivered (no kiln.client, no client world session yet, the queue full): dropped.</summary>
    internal static bool Enqueue(KilnEvent ev)
    {
        var session = Session;
        if (session == 0 || !EnsureSubscribed())
        {
            Dropped("there is no client world");
            return false;
        }
        if (Interlocked.Increment(ref _queued) > MaxQueued)  // a place is taken before the enqueue: never more than MaxQueued
        {
            Interlocked.Decrement(ref _queued);
            Dropped($"the client's event queue is full ({MaxQueued} waiting)");
            return false;
        }
        Queue.Enqueue((ev, session));
        return true;
    }

    /// <summary>Subscribes the client Pre once (the refusal poll needs it before any event exists).</summary>
    internal static bool EnsureSubscribed()
    {
        if (Volatile.Read(ref _subscribed) != 0) return _subscribed > 0;
        lock (Lock)
        {
            if (_subscribed != 0) return _subscribed > 0;
            var t = T;
            ulong handle = 0;
            _subscribed = t != null && t->SubscribeTick(0, 0 /* KILN_TICK_PRE */, &OnPre, null, &handle) == 0 ? 1 : -1;
            return _subscribed > 0;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnPre(void* tick, void* user)
    {
        try { RunPre(); }
        catch { /* never into the game's frame */ }
    }

    /// <summary>The client world's Pre (main thread): the refusal poll, then every queued event of this session.</summary>
    internal static void RunPre()
    {
        BeforeEvents?.Invoke();
        var session = Session;
        for (var n = Queue.Count; n > 0 && Queue.TryDequeue(out var item); n--)
        {
            Interlocked.Decrement(ref _queued);
            if (item.Session != session)
            {
                Dropped($"its client world (session {item.Session}) has ended");
                continue;
            }
            Dispatch(item.Event);
        }
    }

    /// <summary>Runs <paramref name="ev"/>'s handlers on this thread (the main thread), each as its mod, under its own guard:
    /// a handler that throws five times is removed, as on the bus.</summary>
    internal static void Dispatch(KilnEvent ev)
    {
        var subs = Events.HandlersOf(ev.GetType());
        if (subs.Length == 0) return;
        var outer = ModContext.CurrentMod;
        try
        {
            foreach (var sub in subs)
            {
                if (!sub.Active) continue;
                if (sub.Filter is { } filter && !filter.Passes(ev)) continue;
                var mod = sub.Mod;
                if (mod is { Alive: false }) continue;
                ModContext.CurrentMod = mod;
                var start = Stopwatch.GetTimestamp();
                try
                {
                    sub.Invoke!(ev);
                }
                catch (Exception e)
                {
                    var faults = Interlocked.Increment(ref sub.Faults);
                    Backend.Current.Log(mod?.Owner ?? 0, LogLevel.Error,
                        $"{ev.GetType().Name}: handler {sub.Name} threw ({faults}/{Events.MaxFaults}): {e}");
                    if (faults >= Events.MaxFaults) Events.Remove(sub);
                }
                Events.RecordCall(sub, Stopwatch.GetTimestamp() - start);
            }
            Interlocked.Increment(ref _delivered);
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    private static void Dropped(string why)
    {
        Interlocked.Increment(ref _dropped);
        if (Interlocked.Exchange(ref _droppedLogged, 1) == 0)
            Backend.Current.Log(0, LogLevel.Info, $"client-world events: one was not delivered ({why}); later ones are counted, not logged");
    }

    internal static long DroppedCount => Interlocked.Read(ref _dropped);
    internal static int QueuedCount => Volatile.Read(ref _queued);
    internal static int Capacity => MaxQueued;

    /// <summary>Tests: forget the subscription and the queue (a test's fake kiln.client is gone after it).</summary>
    internal static void ResetForTests()
    {
        lock (Lock) _subscribed = 0;
        while (Queue.TryDequeue(out _)) { }
        Interlocked.Exchange(ref _queued, 0);
        BeforeEvents = null;
        Interlocked.Exchange(ref _droppedLogged, 0);
    }
}
