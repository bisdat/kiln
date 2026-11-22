// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>When an edit of a registry field takes effect: every field of every registry carries one (the schema's
/// "applies", the same numbers as the native KILN_REGISTRY_APPLIES_*). docs/kiln/modding/registries/README.md.</summary>
[System.Diagnostics.CodeAnalysis.Experimental("KILNX031", UrlFormat = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/modding/registries/README.md")]
public enum RegistryApplies
{
    /// <summary>At the next use (the next craft, roll, hit or payment).</summary>
    Now = 1,
    /// <summary>When a world loads (in place before its save is read).</summary>
    WorldLoad = 2,
    /// <summary>After a game restart (the game reads it once at start).</summary>
    Restart = 3,
}
