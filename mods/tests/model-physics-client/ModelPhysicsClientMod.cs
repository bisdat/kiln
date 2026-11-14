// SPDX-License-Identifier: MIT
using System.Collections.Concurrent;
using System.Text.Json;
using Kiln;

namespace ModelPhysicsClient;

/// <summary>
/// The fixture of guided check 16e-cc16e. Its blocks (blocks/fixture.json) and cooked models are the content; this code only watches, on a
/// game client: read-only mid hooks on the game's cloth and chain functions (sigdb <c>models/…</c>, features <c>models.cloth.readers</c> and
/// <c>models.chain2.readers</c>) count how often each runs, and the counts go to <c>kiln/data/model-physics-client/counts.json</c> once a second
/// while they change. A hook reads nothing of the game: no pointer is followed and no register is changed. A count says the game's cloth or
/// chain code ran, not which model it ran for; what the player sees decides the check.
/// </summary>
public sealed class ModelPhysicsClientMod : Mod
{
    private static readonly (string Feature, string[] Symbols)[] Watched =
    [
        ("models.cloth.readers", ["models/ClothInstance", "models/ClothStep"]),
        ("models.chain2.readers", ["models/Chain2Init", "models/Chain2CpuAdd", "models/Chain2DirectStep", "models/Chain2DeferredAdd", "models/Chain2Step"]),
    ];

    private readonly List<HookHandle> _hooks = [];
    private readonly ConcurrentDictionary<string, long> _counts = new(StringComparer.Ordinal);
    private long _written = -1;
    private CancellationTokenSource? _stop;

    public override void OnLoad()
    {
        var expected = Path.Combine(Context.Info.Directory, "assets", "cooked", "expected.json");
        Log.Info(File.Exists(expected)
            ? $"model-physics-client: what the writer made: {File.ReadAllText(expected).ReplaceLineEndings(" ")}"
            : "model-physics-client: assets/cooked is missing: run build-assets.py before deploying");
        if (!Kiln.Client.IsClient) return;   // a dedicated server draws nothing: nothing to count
        foreach (var (feature, symbols) in Watched)
        {
            if (!Capabilities.Is(feature))
            {
                Log.Info($"model-physics-client: no counts for {feature}: {Capabilities.Why(feature)}");
                continue;
            }
            foreach (var symbol in symbols)
            {
                var name = symbol;
                _counts[name] = 0;
                _hooks.Add(Hooks.Mid(symbol, (ref Registers regs) => _counts.AddOrUpdate(name, 1, (key, v) => v + 1)));
            }
        }
        _stop = new CancellationTokenSource();
        _ = WriteCounts(_stop.Token);
    }

    private async Task WriteCounts(CancellationToken stop)
    {
        var file = Path.Combine(Context.DataDirectory, "counts.json");
        Directory.CreateDirectory(Context.DataDirectory);
        while (!stop.IsCancellationRequested)
        {
            var total = _counts.Values.Sum();
            if (total != _written)
            {
                _written = total;
                File.WriteAllText(file, JsonSerializer.Serialize(new SortedDictionary<string, long>(_counts, StringComparer.Ordinal)));
            }
            try { await Task.Delay(1000, stop); } catch (OperationCanceledException) { break; }
        }
    }

    public override void OnUnload()
    {
        _stop?.Cancel();
        foreach (var h in _hooks) h.Dispose();
        _hooks.Clear();
        Log.Info($"model-physics-client: hooks removed; counts {string.Join(", ", _counts.Select(c => $"{c.Key}={c.Value}"))}");
    }
}
