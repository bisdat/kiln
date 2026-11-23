// SPDX-License-Identifier: MIT
// The HUD half of PlayerStorage.Client.ProvideAmmo (Kiln.Items, 0.2.0-dev.30.7, GitHub issues 127 and 128): a storage's
// arrows and spells from the client copy (kiln:storage) as rows of Ui.AmmoList, provided as the mod that asked, and that
// mod's picks of them handed to Kiln.Items, which sends them to the server (or, on a hosting game, to its own server
// tick). Kiln.Items finds this type when its first provider is made (it cannot reference Kiln.Ui); Kiln.Ui's start
// installs it too.
namespace Kiln;

internal sealed class PlayerStorageAmmoUi : IPlayerStorageAmmoUi
{
    internal static readonly PlayerStorageAmmoUi Instance = new();

    static PlayerStorageAmmoUi() => PlayerStorageAmmo.Ui = Instance;

    /// <summary>Kiln.Ui's start: Kiln.Items gets the HUD half.</summary>
    internal static void Install() => PlayerStorageAmmo.Ui = Instance;

    private readonly object _gate = new();
    private readonly List<(string Key, AmmoListProvider Provider)> _live = [];

    public IDisposable? Provide(string fullKey, AmmoKinds kinds, bool replace, ModContext? mod, out string? why)
    {
        why = null;
        if (!Ui.AmmoList.Available)
        {
            why = $"{Ui.AmmoList.ListCapability} is off: {Capabilities.Why(Ui.AmmoList.ListCapability)}";
            return null;
        }
        var lists = ((kinds & AmmoKinds.Arrow) != 0 ? AmmoListKind.Arrow : 0) | ((kinds & AmmoKinds.Spell) != 0 ? AmmoListKind.Spell : 0);
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = mod;  // the rows and the picks are the mod's own (its unload ends them)
        AmmoListProvider provider;
        Action<AmmoListPick>? picked = null;
        try
        {
            provider = Ui.AmmoList.Provide(lists, ctx => Rows(fullKey, ctx.Kind),
                new AmmoListOptions { Mode = replace ? AmmoListMode.Replace : AmmoListMode.Append });
            if (Ui.AmmoList.PicksAvailable)
            {
                picked = p => OnPicked(fullKey, p);
                Ui.AmmoList.Picked += picked;
            }
            else why = $"{Ui.AmmoList.PickCapability} is off: picks of the rows do nothing";
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
        lock (_gate) _live.Add((fullKey, provider));
        return new Handle(this, fullKey, provider, picked);
    }

    public void Changed(string fullKey)
    {
        lock (_gate)
            foreach (var (key, provider) in _live)
                if (key == fullKey) provider.Refresh();
    }

    /// <summary>The rows of one list: the storage's items of that kind, one row per item (the UI thread).</summary>
    internal static IReadOnlyList<AmmoListRow> Rows(string fullKey, AmmoListKind kind) =>
        [.. PlayerStorageMirror.AmmoRows(fullKey, kind == AmmoListKind.Arrow ? AmmoKinds.Arrow : AmmoKinds.Spell)
            .Select(r => new AmmoListRow(r.Item, r.Count))];

    // A pick of any of the mod's rows reaches every handler of that mod: only the storage that shows the item sends it.
    private static void OnPicked(string fullKey, AmmoListPick p)
    {
        var kind = p.Kind == AmmoListKind.Arrow ? AmmoKinds.Arrow : AmmoKinds.Spell;
        if (!PlayerStorageMirror.AmmoRows(fullKey, kind).Exists(r => r.Item == p.Item)) return;
        if (!PlayerStorageMirror.Pick(fullKey, kind, p.Item))
            Backend.Current.Log(0, LogLevel.Info, $"player-storage: a pick of {p.Item} from {fullKey} reached no Kiln server with the storage copy");
    }

    private sealed class Handle(PlayerStorageAmmoUi owner, string key, AmmoListProvider provider, Action<AmmoListPick>? picked) : IDisposable
    {
        private int _done;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _done, 1) != 0) return;
            provider.Dispose();
            if (picked is not null) Ui.AmmoList.Picked -= picked;
            lock (owner._gate) owner._live.RemoveAll(x => x.Key == key && ReferenceEquals(x.Provider, provider));
        }
    }
}
