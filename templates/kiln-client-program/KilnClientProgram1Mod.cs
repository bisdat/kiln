using Kiln;

namespace KilnClientProgram1;

// A client program is code of yours that the game's scheduler runs in YOUR client world: the world your game client
// draws (the local player, the camera, what you see), every client step, for every entity you ask for. It runs in this
// player's game only. Nobody else sees what it does, so it is for what you see (presentation), never for gameplay that
// others must share (that belongs in a server program: the kiln-world-program template).
// It has two halves:
//   kiln.programs.json      what the program may touch, with "world": "client". The game fixes that when Kiln starts,
//                           so after a change to the file restart the game (a reload of the mod is not enough).
//   EcsPrograms.TryBind     the code, given in OnLoad under the name the file uses. A client mod's OnLoad runs on the
//                           game's main thread in the main menu: that is where TryBind for the client world must be called.
// This one is called "lights". Its file says: for every entity that has a RenderTransform (what the client world draws;
// "with" only selects, it reads nothing). The code counts them; the count goes to kiln.log once a minute.
public sealed class KilnClientProgram1Mod : Mod
{
    private IDisposable? _program;
    private long _counting;   // written by the game's worker threads
    private long _lastStep;   // the main thread's copy of one whole client step
    private ulong _steps;

    public override void OnLoad()
    {
        // Kiln runs client programs only in a game client whose build Kiln knows (capability world.programs.client). Say
        // so and go on without: a mod that throws from OnLoad does not load at all.
        if (!Capabilities.Is(EcsPrograms.ClientWorldCapability))
        {
            Log.Warn($"client programs are off: {Capabilities.Why(EcsPrograms.ClientWorldCapability)}");
            return;
        }
        // The callback runs on the game's worker threads (or its main thread), several at once, once for each entity. So:
        // nothing of Kiln's tick-thread API in here, only your own data through Interlocked; UI work through Client.Post.
        var bound = EcsPrograms.TryBind("lights", context => Interlocked.Increment(ref _counting), WorldKind.Client);
        _program = bound.Handle;
        // Not Ok: either nothing was bound (Handle is null: the reason says what to fix), or the client world that runs
        // now refused the program (a name it lacks) and the binding waits for a world that takes it.
        if (!bound.Ok) Log.Warn($"client program 'lights': {bound.Reason}");
        // A later client world can refuse it too (each world is checked when it starts): Kiln says why.
        Events.On<EcsProgramRefusedEvent>(e => Log.Warn($"client program '{e.Key}' refused in this world: {e.Reason}"));
        // After each client step, on the main thread: take what the workers counted in that step.
        Client.Tick += AfterStep;
    }

    private void AfterStep(in TickInfo tick)
    {
        _lastStep = Interlocked.Exchange(ref _counting, 0);
        if (++_steps % 3600 == 0) Log.Info($"{_lastStep} drawn entities in the last client step");
    }

    // Kiln unbinds a mod's programs when it unloads and lets a callback that is running finish first. Saying it here is
    // for the reader; the declaration stays reserved, so the same file binds again after a reload.
    public override void OnUnload()
    {
        Client.Tick -= AfterStep;
        _program?.Dispose();
    }
}
