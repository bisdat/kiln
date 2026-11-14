// SPDX-License-Identifier: MIT
// The model cook's physics as a unit test sees it (docs/kiln/modding/models.md#physics): the cook features and a recipe's "experimental" key
// (tools/modelcook/cook/features.py, kept equal by Kiln.TestKit.Tests' PhysicsCookHostTests), one model's cooked files committed whole or not at
// all, and the kiln.assets registrations those files make (a content blob per render data, a keen::RenderModel per model). A refused cook, a
// feature that is off or input the cook refuses, commits no file and registers nothing. The cook itself is Python: nothing here cooks.
using System.Security.Cryptography;

namespace Kiln.TestKit;

/// <summary>What <see cref="FakePhysicsCookHost.Cook"/> did: <see cref="Ok"/> with the files it committed, or the reason it refused.</summary>
/// <param name="Ok">The files were committed and registered.</param>
/// <param name="Reason">Why not (the cook's words: the field, the feature and its reason); null when <see cref="Ok"/>.</param>
/// <param name="Files">The files committed by this cook, in the order given.</param>
public sealed record PhysicsCookResult(bool Ok, string? Reason, IReadOnlyList<string> Files) : IKilnResult;

/// <summary>
/// A stand-in for <c>kiln cook</c>'s physics step in unit tests: the two cook features (<c>modelcook.cloth</c>, <c>modelcook.chains2</c>, off
/// until their game test, turned on per mod by the recipe's <c>"experimental"</c> key) and a transaction per model: everything or nothing is
/// committed to <see cref="Files"/>, and only committed files are registered (<see cref="Registrations"/>).
/// </summary>
public sealed class FakePhysicsCookHost
{
    /// <summary>The cook features of physics, as the cook names them.</summary>
    public static IReadOnlyList<string> Features { get; } = ["modelcook.cloth", "modelcook.chains2"];

    /// <summary>Why a feature is off by default (the same text kiln.caps shows for the held row).</summary>
    public const string PendingReason = "game test pending: 16e-cc16e";

    private readonly HashSet<string> _on;
    private readonly Dictionary<string, byte[]> _files = new(StringComparer.Ordinal);
    private readonly List<string> _registrations = [];

    /// <summary>A host for a recipe whose <c>"experimental"</c> key lists <paramref name="experimental"/>.</summary>
    /// <exception cref="ArgumentException">A name that is no cook feature (the cook refuses the recipe so).</exception>
    public FakePhysicsCookHost(params string[] experimental)
    {
        ArgumentNullException.ThrowIfNull(experimental);
        foreach (var n in experimental)
            if (!Features.Contains(n)) throw new ArgumentException($"experimental: \"{n}\" is not a cook feature (known: {string.Join(", ", Features.Order())})", nameof(experimental));
        _on = [.. experimental];
    }

    /// <summary>Whether <paramref name="feature"/> is on for this host's recipe.</summary>
    public bool IsOn(string feature) => Features.Contains(feature) ? _on.Contains(feature) : throw new ArgumentException($"\"{feature}\" is not a cook feature", nameof(feature));

    /// <summary>The files committed so far, by path in the mod.</summary>
    public IReadOnlyDictionary<string, byte[]> Files => _files;

    /// <summary>What kiln.assets registered from them, in order: <c>blob &lt;size&gt;-&lt;h0&gt;-&lt;h1&gt;-&lt;h2&gt;</c> for each <c>.blob</c>,
    /// <c>resource &lt;model&gt; keen::RenderModel</c> for each <c>.bin</c>.</summary>
    public IReadOnlyList<string> Registrations => _registrations;

    /// <summary>One model's cook: refused (nothing committed, nothing registered) when a feature it <paramref name="needs"/> is off or when
    /// <paramref name="refusal"/> says the cook refused its input; else every file of <paramref name="outputs"/> committed, then registered.</summary>
    /// <param name="model">The model's path in the mod, as physics.json names it.</param>
    /// <param name="needs">The cook features its physics needs.</param>
    /// <param name="outputs">The files the cook made, by path.</param>
    /// <param name="refusal">The cook's refusal of the input (a field and what is wrong with it), or null.</param>
    /// <exception cref="ArgumentException">An empty model path or no output.</exception>
    public PhysicsCookResult Cook(string model, IEnumerable<string> needs, IReadOnlyDictionary<string, byte[]> outputs, string? refusal = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(model);
        ArgumentNullException.ThrowIfNull(needs);
        ArgumentNullException.ThrowIfNull(outputs);
        if (outputs.Count == 0) throw new ArgumentException("a cook makes at least one file", nameof(outputs));
        foreach (var f in needs)
            if (!IsOn(f))
                return new(false, $"{model}: needs cook feature {f}, which is off ({PendingReason}); to cook it anyway for this mod, add \"experimental\": [\"{f}\"] to assets/cook.json", []);
        if (refusal != null) return new(false, $"{model}: {refusal}", []);
        foreach (var (path, bytes) in outputs) _files[path] = bytes.ToArray();
        foreach (var (path, bytes) in outputs)
        {
            if (path.EndsWith(".blob", StringComparison.Ordinal)) _registrations.Add($"blob {ContentHashOf(bytes)}");
            else if (path.EndsWith(".bin", StringComparison.Ordinal)) _registrations.Add($"resource {model} keen::RenderModel");
        }
        return new(true, null, [.. outputs.Keys]);
    }

    /// <summary>A new content blob's hash as kiln.assets derives it: the size and the first 12 bytes of SHA-256 as three little-endian words.</summary>
    public static string ContentHashOf(byte[] bytes)
    {
        ArgumentNullException.ThrowIfNull(bytes);
        var d = SHA256.HashData(bytes);
        return $"{bytes.Length}-{BitConverter.ToUInt32(d, 0):x8}-{BitConverter.ToUInt32(d, 4):x8}-{BitConverter.ToUInt32(d, 8):x8}";
    }
}
