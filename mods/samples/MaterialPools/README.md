# Ice and Wood pool

Copy this folder into the active mod profile on every participating client and server before starting the game. Craft Ice blocks from rough-stone blocks; the new Ice and Wood Wall opts into exactly Ice and Rough Wood. Existing shapes keep their pools. The three filled cells cost three selected blocks; digging an Ice cell returns one Ice block.

The pool uses two of its 64 entries, and Ice uses one of the shared 49 new building-material IDs (including six reusable deprecated slots). Custom pools do not increase those ID budgets. Code can query `WorldTables.GetMaterialBudget` / `GetPoolBudget` and use the explicit `Blocks.Define(..., DefinitionRegistrationOptions)` receipt overload for late definitions.

This example was validated offline. Actual menu ingestion, order, preview, placement, cost, loot and world lifecycle have not run in a game; the coordinator's `client-pass/17-material-pools` check covers them. There is no preview switch.
