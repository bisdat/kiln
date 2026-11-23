// SPDX-License-Identifier: MIT
// PlayerStorage.Client.ProvideAmmo (0.2.0-dev.30.7, GitHub issues 127 and 128): a storage's arrows and spells in the
// HUD's ammunition and spell lists (Kiln.Ui's Ui.AmmoList, from the kiln:storage copy), and the server half that takes
// the player's pick of such a row. Kiln.Ui references Kiln.Items, so the HUD half is Kiln.Ui's PlayerStorageAmmoUi
// behind IPlayerStorageAmmoUi; the server half is PlayerStorageMirror's pick check.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;

namespace Kiln;

/// <summary>How <see cref="PlayerStorage.Client.ProvideAmmo"/> shows a storage's ammunition and what a pick does. Since
/// 0.2.0-dev.30.7 (mod API 0.2.16).</summary>
public sealed record PlayerStorageAmmoOptions
{
    /// <summary>The storage's rows instead of the game's own rows (the HUD list's Replace mode); default false: after the
    /// game's rows (Append).</summary>
    public bool Replace { get; init; }

    /// <summary>A pick swaps the chosen stack into the backpack and equips it there (<see cref="PlayerStorage.Activate"/>),
    /// instead of equipping it from its storage slot. For a game that does not fire from a stack outside the backpack;
    /// default false.</summary>
    public bool Activate { get; init; }
}

/// <summary>A storage's ammunition in the HUD's lists and the server half that takes its picks
/// (<see cref="PlayerStorage.Client.ProvideAmmo"/>). Dispose it to end both halves; it ends by itself when its mod
/// unloads. Since 0.2.0-dev.30.7 (mod API 0.2.16).</summary>
public sealed class PlayerStorageAmmoProvider : IDisposable
{
    private readonly StorageAmmoAcceptor _acceptor;
    private IDisposable? _hud;
    private int _disposed;

    internal PlayerStorageAmmoProvider(string fullKey, AmmoKinds kinds, PlayerStorageAmmoOptions options, StorageAmmoAcceptor acceptor,
        IDisposable? hud, string? whyNotShown)
    {
        FullKey = fullKey;
        Kinds = kinds;
        Options = options;
        _acceptor = acceptor;
        _hud = hud;
        WhyNotShown = hud is null ? whyNotShown ?? "this game shows no HUD lists" : null;
    }

    /// <summary>The storage's full key.</summary>
    public string FullKey { get; }

    /// <summary>The lists it provides for.</summary>
    public AmmoKinds Kinds { get; }

    /// <summary>How it shows and what a pick does.</summary>
    public PlayerStorageAmmoOptions Options { get; }

    /// <summary>This game shows the storage's rows in the HUD's lists (a game client where <c>ui.ammo.list</c> is on).</summary>
    public bool Shown => _hud is not null && Volatile.Read(ref _disposed) == 0;

    /// <summary>Why the rows are not shown in this process (a dedicated server has no HUD; <c>ui.ammo.list</c> is off);
    /// null when they are.</summary>
    public string? WhyNotShown { get; }

    /// <summary>False once disposed.</summary>
    public bool Active => Volatile.Read(ref _disposed) == 0;

    /// <summary>Ends both halves: the rows leave the HUD at the next UI frame, and this server takes no more picks of the
    /// storage. Any thread.</summary>
    [AnyThread]
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        PlayerStorageMirror.Withdraw(_acceptor);
        Interlocked.Exchange(ref _hud, null)?.Dispose();
    }

    /// <summary>The provider in one line, for a log.</summary>
    public override string ToString() =>
        $"ammunition of {FullKey} ({Kinds}{(Options.Replace ? ", replace" : "")}{(Options.Activate ? ", activate" : "")}): " +
        (Shown ? "shown" : $"not shown here ({WhyNotShown})");
}

/// <summary>The HUD half of ProvideAmmo, implemented by Kiln.Ui (PlayerStorageAmmoUi).</summary>
internal interface IPlayerStorageAmmoUi
{
    /// <summary>Shows the ammunition rows of <paramref name="fullKey"/> from the client copy in the HUD's lists, as
    /// <paramref name="mod"/>, and hands that mod's picks to <see cref="PlayerStorageMirror.Pick"/>. Null, with why, when
    /// the lists are not here.</summary>
    IDisposable? Provide(string fullKey, AmmoKinds kinds, bool replace, ModContext? mod, out string? why);

    /// <summary>The copy of <paramref name="fullKey"/> changed: its rows are asked for again.</summary>
    void Changed(string fullKey);
}

internal static class PlayerStorageAmmo
{
    private static IPlayerStorageAmmoUi? _ui;
    private static bool _looked;

    /// <summary>Set by Kiln.Ui when it starts.</summary>
    internal static IPlayerStorageAmmoUi? Ui
    {
        get => Volatile.Read(ref _ui);
        set => Volatile.Write(ref _ui, value);
    }

    internal static PlayerStorageAmmoProvider Provide(string fullKey, AmmoKinds kinds, PlayerStorageAmmoOptions? options)
    {
        ArgumentNullException.ThrowIfNull(fullKey);
        options ??= new PlayerStorageAmmoOptions();
        if (!PlayerStorage.FullKeyRule.IsMatch(fullKey))
            throw new ArgumentException($"'{fullKey}' is no storage key (\"<mod id>:<name>\", as PlayerStorage.FullKey)", nameof(fullKey));
        if (kinds == 0 || (kinds & ~(AmmoKinds.Arrow | AmmoKinds.Spell)) != 0) throw new ArgumentException("Arrow, Spell or both", nameof(kinds));
        Items.Rows.Ensure();
        PlayerStorageMirror.Start();
        if (!PlayerStorageMirror.Open) throw ServiceCaps.Unavailable(PlayerStorage.Client.Capability);
        var mod = ModContext.CurrentMod ??
                  throw new InvalidOperationException("PlayerStorage.Client.ProvideAmmo must be called from mod code: the storage belongs to the mod");
        if (!mod.Alive) throw new InvalidOperationException($"{mod.Info.Id} is unloading");
        if (!fullKey.StartsWith(mod.Info.Id + ":", StringComparison.Ordinal))
            throw new ArgumentException($"{fullKey} is not one of {mod.Info.Id}'s storages: a mod provides only its own", nameof(fullKey));
        var acceptor = new StorageAmmoAcceptor(fullKey, kinds, options.Activate, mod);
        PlayerStorageMirror.Accept(acceptor);
        IDisposable? hud = null;
        string? why = "Kiln.Ui is not here";
        try
        {
            if (Hud() is { } ui)
            {
                hud = ui.Provide(fullKey, kinds, options.Replace, mod, out why);
                if (hud is null) why ??= "the HUD lists are not here";
            }
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException)
        {
            why = e.Message;
        }
        var provider = new PlayerStorageAmmoProvider(fullKey, kinds, options, acceptor, hud, hud is null ? why : null);
        mod.Retiring += provider.Dispose;
        return provider;
    }

    internal static void Changed(string fullKey)
    {
        try
        {
            Ui?.Changed(fullKey);
        }
        catch (Exception e)
        {
            Backend.Current.Log(0, LogLevel.Warn, $"kiln.items: refreshing the HUD rows of {fullKey} failed: {e.Message}");
        }
    }

    // Kiln.Ui starts with Kiln's first service tick, after the mods' OnLoad: a provider made in OnLoad starts it here.
    private static IPlayerStorageAmmoUi? Hud()
    {
        if (Ui is not null || _looked) return Ui;
        _looked = true;
        try
        {
            var alc = AssemblyLoadContext.GetLoadContext(typeof(PlayerStorageAmmo).Assembly) ?? AssemblyLoadContext.Default;
            var asm = alc.Assemblies.FirstOrDefault(a => a.GetName().Name == "Kiln.Ui") ?? alc.LoadFromAssemblyName(new AssemblyName("Kiln.Ui"));
            if (asm.GetType("Kiln.PlayerStorageAmmoUi", throwOnError: false) is { } type) RuntimeHelpers.RunClassConstructor(type.TypeHandle);
        }
        catch (Exception e) when (e is FileNotFoundException or FileLoadException or BadImageFormatException or TypeInitializationException)
        {
            Backend.Current.Log(0, LogLevel.Info, $"kiln.items: no HUD lists for player storage here (Kiln.Ui: {e.Message})");
        }
        return Ui;
    }

    internal static void ResetForTests() => _looked = false;
}
