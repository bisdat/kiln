/* SPDX-License-Identifier: MIT
 * kiln.skills.client v1 (since 0.2.0-dev.31.13, mod API 0.2.22): a read-only view of the local player's skill tree as the
 * game client itself holds it, for a player who joined someone else's world (where kiln.progression has no server world
 * and answers nothing) and for a hosting player's own client. Served by kiln-skills.dll on the game client. Capability:
 * skills.client_view (sigdb client.d/skills-client-view.json). Managed face: Kiln.Skills' ClientSkills. Guide:
 * docs/kiln/services/skills.md ("On a joined client: the local player's tree"); evidence: docs/re/progression.md
 * section 8.
 *
 * Where it comes from, all in the client's own world:
 *  - the tree: the client world's g38_skilltree::SkillTree and the SkillTreeResource it loaded (Kiln's expanded one when
 *    mods add nodes): every node with its index, id, type, class, cost, level count, screen position and link counts,
 *    and every link. The same data the game's skill-tree screen draws;
 *  - the player's state: the game's skilltree_ui program fills, every client step for the local player, one record per
 *    node in G38UiPlayData (level, unlocked; for shadow nodes Kiln's screen hook writes them from the table the server
 *    sent) from the replicated PlayerSkillsUi component, and its skill points: spent (the game's PointsSpent over those
 *    masks and levels, shadow nodes counted through Kiln's hook) and available (total - spent, at least 0). The total
 *    comes from the player's replicated attributes (player_ui). What the server changes reaches these on the client's
 *    next update of the player; nothing here asks the server.
 *
 * Threads: every function on any thread. The view is a copy the client world's thread takes after its step (Post) while
 * some caller read it in the last KILN_SKILLS_CLIENT_DEMAND_MS milliseconds: the first read after a pause starts the
 * copying and answers from the previous copy (or none: KILN_ERR_NOT_FOUND from state, no rows). Nothing here writes game
 * memory or sends a request: unlocking stays the game's own input or the server's kiln.progression.
 *
 *   state  the points and counts of the copy, and what works. KILN_ERR_NOT_FOUND: no copy yet (no local player, the
 *          tree not loaded). KILN_ERR_UNAVAILABLE: skills.client_view is off, or this is no game client.
 *   nodes  every node of the copy by tree index. links: every link. *out_count = how many. More than capacity: the first
 *          capacity rows are written and KILN_ERR_BUFFER_TOO_SMALL; capacity 0 with out NULL asks for the count.
 * KILN_ERR_INVALID_ARGUMENT: a NULL pointer where a result goes, out NULL with a capacity, a state size that is not
 * sizeof(KilnSkillsClientState). */
#ifndef KILN_SKILLS_CLIENT_H
#define KILN_SKILLS_CLIENT_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_SKILLS_CLIENT_SERVICE "kiln.skills.client"
#define KILN_SKILLS_CLIENT_VERSION 1u
#define KILN_SKILLS_CLIENT_DEMAND_MS 5000u
#define KILN_SKILLS_CLIENT_MAX_NODES 4096u
#define KILN_SKILLS_CLIENT_MAX_LINKS 16384u

/* KilnSkillsClientNode.flags */
enum {
    KILN_SKILLS_CLIENT_UNLOCKED = 0x1u, /* the local player has it (level > 0) */
    KILN_SKILLS_CLIENT_SHADOW = 0x2u,   /* a shadow node: its state is Kiln's table, as the server sent it */
    KILN_SKILLS_CLIENT_RECORD = 0x4u,   /* the screen's record of the node was found (without it: locked, level 0) */
};

typedef struct KilnSkillsClientNode {
    uint32_t id;              /* 0: the game's SkillNodeId */
    uint16_t index;           /* 4: tree index (the mask bit for indices below 256) */
    uint16_t cost;            /* 6: skill points (per level for a node with levels) */
    uint8_t type;             /* 8: the game's SkillNodeType */
    uint8_t class_alignment;  /* 9: 0 Warrior, 1 Mage, 2 Ranger, 3 Neutral */
    uint8_t max_level;        /* 10: 0 = one unlock */
    uint8_t level;            /* 11: 0 locked; 1 for a plain node; 1..max_level */
    uint8_t flags;            /* 12: KILN_SKILLS_CLIENT_* */
    uint8_t level_index;      /* 13 */
    uint16_t reserved;        /* 14 */
    float x, y;               /* 16, 20: the screen position */
    uint32_t incoming;        /* 24: links into it */
    uint32_t outgoing;        /* 28: links out of it */
} KilnSkillsClientNode;       /* 32 */

typedef struct KilnSkillsClientLink {
    uint32_t id;              /* 0 */
    uint32_t from, to;        /* 4, 8: node ids */
    uint8_t bidirectional;    /* 12 */
    uint8_t reserved[3];      /* 13 */
} KilnSkillsClientLink;       /* 16 */

typedef struct KilnSkillsClientState {
    uint32_t size;            /* 0: sizeof(KilnSkillsClientState), set by the caller */
    uint8_t on;               /* 4: skills.client_view resolved here and nothing faulted */
    uint8_t world;            /* 5: a client world runs now */
    uint8_t copying;          /* 6: a caller read in the last KILN_SKILLS_CLIENT_DEMAND_MS */
    uint8_t reserved0;        /* 7 */
    KilnEntity player;        /* 8: the local player of the copy */
    int32_t total;            /* 12: skill points the player has in all */
    int32_t spent;            /* 16: on unlocked nodes, shadow nodes included */
    int32_t available;        /* 20: total - spent, at least 0 (the screen's "available") */
    uint32_t nodes;           /* 24 */
    uint32_t unlocked;        /* 28: nodes with level > 0 */
    uint32_t links;           /* 32 */
    uint32_t reserved1;       /* 36 */
    uint64_t step;            /* 40: the client world's step of the copy */
    int64_t age_ms;           /* 48: since the copy was taken */
    char detail[160];         /* 56: why it is off, in words; "" when it works */
} KilnSkillsClientState;      /* 216 */

typedef struct KilnSkillsClientApi {
    KilnServiceHeader header; /* KILN_SKILLS_CLIENT_VERSION; append-only */
    KilnResult(KILN_CALL* state)(KilnSkillsClientState* out);
    KilnResult(KILN_CALL* nodes)(KilnSkillsClientNode* out, uint32_t capacity, uint32_t* out_count);
    KilnResult(KILN_CALL* links)(KilnSkillsClientLink* out, uint32_t capacity, uint32_t* out_count);
} KilnSkillsClientApi;

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
static_assert(sizeof(KilnSkillsClientNode) == 32 && offsetof(KilnSkillsClientNode, x) == 16, "KilnSkillsClientNode");
static_assert(offsetof(KilnSkillsClientNode, flags) == 12 && offsetof(KilnSkillsClientNode, outgoing) == 28, "node tail");
static_assert(sizeof(KilnSkillsClientLink) == 16 && offsetof(KilnSkillsClientLink, bidirectional) == 12, "KilnSkillsClientLink");
static_assert(sizeof(KilnSkillsClientState) == 216 && offsetof(KilnSkillsClientState, step) == 40, "KilnSkillsClientState");
static_assert(offsetof(KilnSkillsClientState, detail) == 56, "state detail");
static_assert(sizeof(KilnSkillsClientApi) == 32, "KilnSkillsClientApi is 32 bytes on x64");
#endif

#endif /* KILN_SKILLS_CLIENT_H */
