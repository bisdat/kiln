# Terrain-only tool removal

Since 0.2.0-dev.30.5.

A terrain tool's Remove action declares its own removable-material list. This is a per-tool policy, not a global
setting: one tool can remove rubble while another removes mud. Only terrain cell ids 1..127 are eligible. Air,
building-material voxels (128..255) and entities are never added to the filter, even if supplied by a malformed native
caller. Optional materials, such as Underwater Mud (cell 91), must be explicitly included by that tool's configuration.

Removal runs inside the game's authoritative terraforming operation. Kiln filters the native brush's material
coefficients and skips the separate building-voxel removal branch. It does not call `Build.Dig`, whose existing
terrain-and-building semantics are unchanged. The game still computes brush geometry and partial fills, applies its
mining yields and finishes the operation. The selected native brush's radius and shape are not approximated by a
managed cell-edit loop. Mining drops are not necessarily the item used to place that terrain material.

This backend has no independent `Build.CarveTerrain` entry point. Use the tool action policy so the material filter,
permission checks and native operation have the same invocation lifetime. Both removal hooks must be qualified and
installed before a Remove policy can activate; an unavailable mapping must refuse the action, never fall back to an
unrestricted dig. Vanilla tools and Add actions do not use this removal filter.

## Check a tool in the game

Use a copied test save. Try every configured brush size against mixed allowed materials and unrelated ground, with
building blocks and placed props immediately adjacent. Only listed terrain should change, including at partial-fill
boundaries. Compare mined drops with normal mining of each material, especially terrain whose placement item differs
from its mining drop. Check the optional-material setting on and off, then switch to a vanilla tool and verify its
normal removal behaviour. Test normal/remapped building undo and a hosted session separately; unit fixtures cannot
establish those game behaviours.
