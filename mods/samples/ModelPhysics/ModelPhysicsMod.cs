// SPDX-License-Identifier: MIT
using Kiln;

namespace ModelPhysics;

/// <summary>
/// Sample: cloth and bone chains cooked into a mod's models (docs/kiln/modding/models.md#physics).
/// <list type="bullet">
/// <item>models/banner.gltf: a banner hung by its top edge. assets/physics.json makes its one primitive cloth (two pinned corners, 0.3 kg);
/// assets/cook.json's model step cooks it.</item>
/// <item>models/pendant.gltf: three links on a skeleton of its own. blocks/pendant.json's body declares the chain link_0 to link_2; the cook
/// finds the body by itself (its skeleton and model steps), and Kiln.Blocks checks the body when it loads.</item>
/// </list>
/// The game simulates both on players' machines; nothing here runs at run time.
/// </summary>
public sealed class ModelPhysicsMod : Mod
{
    public override void OnLoad()
    {
        var cooked = Path.Combine(Context.Info.Directory, "assets", "cooked", "banner.bin");
        Log.Info(File.Exists(cooked)
            ? "ModelPhysics: the banner and the pendant are cooked (kiln.assets serves them from boot)"
            : "ModelPhysics: assets/cooked missing: run build-assets.sh before deploying");
    }
}
