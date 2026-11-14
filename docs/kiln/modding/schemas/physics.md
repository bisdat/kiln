# Kiln model physics (assets/physics.json), format 1

<!-- Generated from schema/physics.schema.json by kiln_manage::lint::docs (cargo test -p kiln-manage --test lint with KILN_LINT_WRITE=1). Do not edit. -->

The cloth and bone chains a mod asks the cook to write into its models (tools/modelcook/cook/physics.py; docs/kiln/modding/models.md#physics). A body block's cloth and chains (Kiln.Blocks ModelClothBlock, ModelChainBlock) become the same tree; a model both name is refused. The game simulates them with its own cloth and chain code on a player's machine; a dedicated server only serves the cooked files. Writing them needs the cook features modelcook.cloth and modelcook.chains2, off until their guided client check passes: a recipe turns them on for one mod with "experimental": ["modelcook.cloth", "modelcook.chains2"] in assets/cook.json. Strict JSON: no comments, no trailing commas, a key twice, NaN and an unknown key are errors. Every refusal of the cook starts with the field path.

- Files: `**/assets/physics.json`
- JSON: comments **not allowed**, trailing commas **not allowed**
- Schema: [`schema/physics.schema.json`](../../../../schema/physics.schema.json); rules: [lint.md](../lint.md)

## Top level

| Key | Type | Required | Notes |
|---|---|---|---|
| `format` | `1` | yes |  |
| `models` | object | yes | Model file -> its cloth and chains. The key is the glTF file's path in the mod (forward slashes, no leading /, no . or .. parts), as a body block's or a recipe step's model names it. One model once (also not in another case: one file on Windows). |

## chain

| Key | Type | Required | Notes |
|---|---|---|---|
| `bendStiffness` | number |  | The game's own coefficient (no unit claimed). Default `5`. min 0 max 1000000 |
| `damping` | number |  | Default `0.01`. min 0 max 0.99 |
| `particleMassKg` | number |  | The mass of one moving joint, not of the chain. Default `1`. min 0.001 max 100 |
| `particleRadius` | number |  | Metres, stored in millimetres. Default `0`. min 0 max 1 |
| `rootJoint` | string | yes | The first joint that swings; its parent stays animated. A name of the glTF's skeleton. min length 1 |
| `stiffness` | number |  | The game's own coefficient (no unit claimed). Default `10000`. min 1 max 1000000 |
| `tipJoint` | string | yes | The last joint, below the root along one branch: 2 to 8 joints from root to tip. min length 1 |

## cloth

| Key | Type | Required | Notes |
|---|---|---|---|
| `colliders` | array of [collider](#collider) |  | Spheres and capsules the cloth does not pass through, in bind pose (metres, glTF axes). The colliders of all cloth entries of a model are its one list, each once: at most 32 spheres, a capsule counting as two. |
| `damping` | number |  | Default `0.4`. min 0 max 0.99 |
| `gravity` | number |  | m/s², downwards. Default `9.81`. min 0 max 50 |
| `massKg` | number |  | The whole cloth's mass, spread over its particles by the area around each. Default `1`. min 0.001 max 100 |
| `mesh` | integer | yes | The glTF mesh, by its index in the file. min 0 |
| `particleIds` | array of integer |  | One id per POSITION element of the primitive. Equal ids weld the copies a UV or normal seam made: they sit within 1e-6 m and carry the same skin. Particles are numbered in the order their id first appears. Absent: every vertex is its own particle. min items 1 max items 65535 |
| `pin` | array of number | yes | One value per particle (in particle order): 1 holds it on the model, 0 lets it hang, between makes it heavier and limits its motion. Every connected piece of cloth needs a particle with 1. min items 1 max items 65535 |
| `primitive` | integer | yes | The primitive of that mesh, by its index. min 0 |
| `stiffness` | number |  | One stiffness for stretch and bend. Default `0.8`. max 1 |

## collider

| Key | Type | Required | Notes |
|---|---|---|---|
| `type` | `"sphere"` \| `"capsule"` | yes |  |

## end

An end of a capsule: a sphere without "type".

Type: [sphere](#sphere)

## model

| Key | Type | Required | Notes |
|---|---|---|---|
| `chains` | array of [chain](#chain) |  | Bone chains (needs a skinned model). |
| `cloth` | array of [cloth](#cloth) |  | Cloth meshes, each a glTF primitive. At most 255 per model; a primitive once. max items 255 |

## sphere

| Key | Type | Required | Notes |
|---|---|---|---|
| `joint` | string \| null |  | The skeleton joint it follows (all its weight), or null: rigid on the model. min length 1 |
| `radius` | number | yes | Metres. max 8 |
| `x` | number | yes |  |
| `y` | number | yes |  |
| `z` | number | yes |  |

