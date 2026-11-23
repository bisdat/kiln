// SPDX-License-Identifier: MIT
// The admin command /playerstorage list <player> | release <player> <key>, Kiln's own (owner "kiln"). It is registered
// late-bound by name like /instances (Kiln.Saves has no reference to Kiln.Chat). "list" shows what this server keeps for
// the character a player joined with: storages, parked keys with their reason, quarantined records. "release" returns a
// parked key (a hold included) to the player's backpack through Items.PlayerStorages.ReturnOrphans: what fits goes in,
// the rest stays parked. The hold notice and the admin log name these exact commands.
using System.Globalization;
using System.Reflection;
using System.Runtime.Loader;

namespace Kiln;

internal static class PlayerStorageCommand
{
    internal const string Name = "playerstorage";
    private static bool _registered, _gaveUp, _tickAdded, _warned;
    private static IDisposable? _tick;
    private static PropertyInfo? _args;
    private static MethodInfo? _reply;
    private static Type? _commandException;

    /// <summary>The release itself (tests replace it).</summary>
    internal static Func<EntityId, string, OrphanTarget, Task<MoveResult>> Release = Items.PlayerStorages.ReturnOrphans;

    /// <summary>Persistence is bound on this server: register now if Kiln.Chat is loaded, else at the first service tick.</summary>
    internal static void Ensure()
    {
        if (_registered || _gaveUp) return;
        if (TryRegister(final: false)) return;
        if (_tickAdded) return;
        _tickAdded = true;
        _tick = ServiceTick.Add("kiln.saves /playerstorage", static (in TickInfo _) =>
        {
            TryRegister(final: true);
            _tick?.Dispose();
            _tick = null;
        });
    }

    internal static bool TryRegister(bool final)
    {
        if (_registered) return true;
        if (_gaveUp) return false;
        var chat = FindChat();
        if (chat is null)
        {
            if (final) GiveUp("Kiln.Chat is not loaded, so there is no /playerstorage command (held storage stays parked)");
            return false;
        }
        try
        {
            var commands = chat.GetType("Kiln.Commands");
            var handlerType = chat.GetType("Kiln.CommandHandler");
            var permissionType = chat.GetType("Kiln.CommandPermission");
            var contextType = chat.GetType("Kiln.CommandContext");
            _commandException = chat.GetType("Kiln.CommandException");
            if (commands is null || handlerType is null || permissionType is null || contextType is null || _commandException is null)
                return GiveUp("Kiln.Chat has no Commands, CommandHandler, CommandPermission, CommandContext or CommandException");
            var register = commands.GetMethod("Register", BindingFlags.Public | BindingFlags.Static,
                [typeof(string), handlerType, permissionType, typeof(string), typeof(string), typeof(string[])]);
            _args = contextType.GetProperty("Args", BindingFlags.Public | BindingFlags.Instance);
            _reply = contextType.GetMethod("Reply", BindingFlags.Public | BindingFlags.Instance, [typeof(string)]);
            if (register is null || _args is null || _reply is null)
                return GiveUp("Kiln.Chat's Commands.Register, CommandContext.Args or Reply has another shape");
            var handler = Delegate.CreateDelegate(handlerType, typeof(PlayerStorageCommand).GetMethod(nameof(Handle), BindingFlags.NonPublic | BindingFlags.Static)!);
            var admin = Enum.Parse(permissionType, "Admin");
            var outer = ModContext.CurrentMod;
            ModContext.CurrentMod = null;  // Kiln's command, not the calling mod's
            try
            {
                register.Invoke(null, [Name, handler, admin, "list <player> | release <player> <key>",
                    "player storage: what this server keeps for a player's character; release a held or parked key to the backpack",
                    Array.Empty<string>()]);
            }
            finally
            {
                ModContext.CurrentMod = outer;
            }
            _registered = true;
            return true;
        }
        catch (TargetInvocationException e) when (e.InnerException is not null)
        {
            return GiveUp($"registering /{Name} failed: {e.InnerException.Message}");
        }
        catch (Exception e) when (e is ArgumentException or MissingMethodException or AmbiguousMatchException or TypeLoadException)
        {
            return GiveUp($"Kiln.Chat's command API has another shape: {e.Message}");
        }
    }

    private static Assembly? FindChat()
    {
        var own = AssemblyLoadContext.GetLoadContext(typeof(PlayerStorageCommand).Assembly);
        foreach (var a in own?.Assemblies ?? [])
            if (a.GetName().Name == "Kiln.Chat") return a;
        foreach (var a in AppDomain.CurrentDomain.GetAssemblies())
            if (a.GetName().Name == "Kiln.Chat") return a;
        return null;
    }

    private static bool GiveUp(string why)
    {
        _gaveUp = true;
        if (!_warned)
        {
            _warned = true;
            SavesRuntime.Log(LogLevel.Warn, $"Kiln.Saves: {why}");
        }
        return false;
    }

    private static Exception Usage(string text) => (Exception)Activator.CreateInstance(_commandException!, text)!;

    private static EntityId PlayerOf(string text)
    {
        if (uint.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var id) && id != 0) return new EntityId(id);
        if (Players.Available && Players.Find(text) is { } p && !p.Entity.IsNone) return p.Entity;
        throw Usage($"no player '{text}'; usage: /{Name} list <player> | release <player> <key>");
    }

    /// <summary>Runs as Kiln's command handler: the context is Kiln.Chat's CommandContext.</summary>
    private static void Handle(object context)
    {
        var args = (IReadOnlyList<string>?)_args!.GetValue(context) ?? [];
        void Reply(string line) => _reply!.Invoke(context, [line]);
        if (args.Count < 2 || args[0] is not ("list" or "release") || (args[0] == "release" && args.Count < 3))
            throw Usage($"usage: /{Name} list <player> | release <player> <key>");
        if (!SavesRuntime.Ensure()) throw Usage("kiln.saves is not loaded");
        var player = PlayerOf(args[1]);
        var storage = SavesRuntime.Store.Storage;
        if (args[0] == "list")
        {
            var lines = storage.Listing(player) ?? throw Usage($"player {player.Value} has no player storage on this server since the world loaded");
            foreach (var line in lines) Reply(line);
            return;
        }
        var key = args[2];
        if (!storage.IsParked(player, key))
            throw Usage($"player {player.Value} has nothing parked under {key} (/{Name} list {player.Value})");
        Task<MoveResult> task;
        try { task = Release(player, key, OrphanTarget.Backpack); }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or ArgumentException)
        {
            throw Usage($"cannot release {key}: {e.Message}");
        }
        SavesRuntime.Log(LogLevel.Warn, $"player-storage: an admin releases {key} of player {player.Value} to the backpack (/{Name})");
        if (task.IsCompletedSuccessfully)
        {
            Reply($"{key} of player {player.Value} to the backpack: {task.Result}");
            SavesRuntime.Log(LogLevel.Info, $"player-storage: release of {key} for player {player.Value}: {task.Result}");
            return;
        }
        Reply($"{key} of player {player.Value} goes to the backpack (what does not fit stays parked); the result is logged");
        task.ContinueWith(t => SavesRuntime.Log(t.IsCompletedSuccessfully ? LogLevel.Info : LogLevel.Warn,
            $"player-storage: release of {key} for player {player.Value}: {(t.IsCompletedSuccessfully ? t.Result.ToString() : t.Exception?.InnerException?.Message ?? "canceled")}"),
            CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
    }

    /// <summary>Tests: forget the registration state.</summary>
    internal static void ResetForTests()
    {
        _registered = _gaveUp = _tickAdded = _warned = false;
        _tick?.Dispose();
        _tick = null;
    }
}
