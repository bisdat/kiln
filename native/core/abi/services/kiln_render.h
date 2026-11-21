/* SPDX-License-Identifier: MIT
 * kiln.render v2 (client only): per-frame drawing for mods on top of the game's picture, in screen space and at world
 * positions; v2 (0.2.0-dev.30.3, appended) outlines an entity's model with the game's own highlight. Registered by kiln-ui.dll next to kiln.ui (native/core/abi/services/kiln_ui.h), whose overlay frame it
 * draws into. Usage: docs/kiln/render.md. RE: docs/re/client-render.md ("The camera").
 *
 * Frames
 *  - Drawing happens inside a kiln.ui frame callback (kiln_ui.h subscribe_frame): the game's render thread, once per
 *    presented frame while the overlay draws. acquire_frames asks for a frame at every present (as a HUD line would);
 *    release gives it back (it does not unsubscribe the callback). Before unloading, unsubscribe from outside a callback,
 *    then release the frame handle. Unsubscribe waits for an in-flight frame callback; core owner_revoke does not clean
 *    up these service handles. With nothing acquired and nothing else on the overlay the presents pass through untouched.
 *  - A frame that draws nothing records no GPU work (kiln.ui skips the submit when ImGui's draw data is empty), so a
 *    per-frame callback that only computes costs CPU time only.
 *  - Everything below is drawn on ImGui's background list: under every Kiln window, screen, HUD line and toast, over
 *    the game's picture (including the game's own HUD).
 * Camera
 *  - The view the game renders with, as the client wrote it this frame (the local player's camera: position, rotation,
 *    vertical field of view, aspect, near and far planes). Read on the game's main thread by a hook where the client
 *    fills it (feature render.camera); camera() returns the latest copy on any thread. Camera shake is not included.
 *  - Spaces: world metres, x east, y up, z north (left-handed); camera space x right, y up, z forward (+z = into the
 *    screen). Pixels: origin top left, y down.
 * Threads: draw functions only inside a frame callback (elsewhere they do nothing); camera, world_to_screen, status,
 * acquire_frames, release: any thread.
 * Outlines (v2, appended after image; check header.size >= offsetof(KilnRenderApi, outline_status) + sizeof(void*))
 *  - The game draws every render instance with a highlight group (keen::HighlightColorIndex, a byte on the instance) and
 *    colours each group from its palette (keen::HighlightColorPaletteSettings in the RenderSystemResource). The game uses
 *    InvalidState 1, ValidState 2 and MutedState 3 for the building cursor's hovered entity (and the ConstructionCursor
 *    preview); Group1 to Group3 (4 to 6) are transparent in its palette and unused; Group4 (7) is orange and unused by the
 *    game itself. RE: docs/re/client-render.md "Highlight groups (outlines)".
 *  - Kiln keeps two groups for mods, Group1 (4) and Group2 (5), and colours them itself (their palette entries are
 *    replaced as the renderer reads them; a pulse breathes the colour). Each distinct (colour, pulse) pair in use takes
 *    one of the two; a third pair at once shares the group whose colour is nearest (reported as merged). Group3 and
 *    Group4 stay free for other tools (Greeneye's Quick Deposit uses 6, Find and The Worst Map Mod use 7).
 *  - outline(owner, entity, opts, out): asks for an outline of the client-world entity (a replicated entity has the
 *    server's id) until outline_remove, outline_clear, duration_ms, the entity's despawn or the end of the client world.
 *    Requests on one entity: the highest priority wins, then the newest; the others wait (state WAITING, reason
 *    OUTSHONE) and take over when it goes. The game's own highlight wins over every mod unless the player's kiln.json
 *    "ui": {"outlines": "mods-first"} says otherwise; "off" turns mod outlines off (requests are accepted and wait).
 *  - Mechanism: the first request installs, on the game's main thread at a safe point, a checked mid hook on each of the
 *    renderer's two palette reads of Group1 and Group2 (feature ui.world_outline) and an After stage on the client
 *    world's dynamic_render_model program (kiln.world game_program_wrap). That stage, once per client step on the world's
 *    thread, sets and restores the group of each outlined entity's model instance through the renderer's own setter, the
 *    way the game's render programs do. It only takes an instance whose group is 0 or Kiln's, and gives it back to 0 when
 *    the outline ends (unless someone else changed it since). With no request left the stage is removed again; the
 *    palette hooks then pass the game's colours through. Nothing is installed for a player whose mods never ask.
 *  - Entities drawn through ModelComponent (props, chests, stations, NPCs, creatures, dropped items) can be outlined;
 *    voxel models and the terrain cannot (state WAITING, reason NO_MODEL). The game's highlight is drawn with the model
 *    and hidden by what stands in front of it: there is no through-walls mode.
 *  - Threads: every outline function on any thread. */
#ifndef KILN_RENDER_H
#define KILN_RENDER_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_RENDER_SERVICE "kiln.render"
#define KILN_RENDER_VERSION 2u /* v2 appends outline .. outline_status after image (0.2.0-dev.30.3) */

/* text flags */
enum {
    KILN_RENDER_TEXT_SHADOW = 1u,      /* a dark copy one pixel down-right */
    KILN_RENDER_TEXT_OUTLINE = 2u,     /* a dark outline (four copies) */
    KILN_RENDER_TEXT_CENTER_X = 4u,    /* x is the text's centre */
    KILN_RENDER_TEXT_CENTER_Y = 8u,    /* y is the text's middle */
    KILN_RENDER_TEXT_RIGHT = 16u,      /* x is the text's right edge */
};

typedef struct KilnRenderCamera {
    uint32_t size;          /* set to sizeof(KilnRenderCamera) before calling camera() */
    uint8_t valid;          /* a camera was written in the last 500 ms (may remain valid briefly after leaving play) */
    uint8_t available;      /* the camera hook runs on this build (feature render.camera) */
    uint8_t reserved[2];
    double position[3];     /* metres */
    float rotation[4];      /* quaternion x y z w: camera space -> world (rotate (0,0,1) for the view direction) */
    float fov_y;            /* vertical field of view, radians */
    float aspect;           /* width / height the game projects with */
    float near_plane, far_plane; /* metres */
    float distance;         /* the third-person camera's distance from its pivot (the character), metres */
    float reserved2;
    uint64_t frame;         /* camera writes since start (one per client frame) */
    uint32_t age_ms;        /* since the last write */
    uint32_t reserved3;
} KilnRenderCamera;

typedef struct KilnRenderStatus {
    uint32_t size;          /* set to sizeof(KilnRenderStatus) before calling */
    uint8_t camera;         /* feature render.camera hooked */
    uint8_t camera_valid;   /* see KilnRenderCamera.valid */
    uint8_t reserved[2];
    uint32_t frame_holders; /* acquire_frames handles held */
    uint64_t camera_frames; /* camera writes seen */
    uint64_t draws;         /* draw calls since start */
} KilnRenderStatus;

/* ---- outlines (v2) ---- */
/* KilnRenderOutline.flags */
enum {
    KILN_RENDER_OUTLINE_PULSE = 1u, /* the colour breathes (1.2 s period, 40 % to 100 % brightness) */
};
/* KilnRenderOutlineInfo.state */
enum {
    KILN_RENDER_OUTLINE_WAITING = 0, /* accepted, not shown now (reason says why) */
    KILN_RENDER_OUTLINE_SHOWN = 1,   /* the entity's model shows this request's colour */
    KILN_RENDER_OUTLINE_GAME = 2,    /* the game's own highlight (or another tool's group) shows instead, for now */
    KILN_RENDER_OUTLINE_ENDED = 3,   /* over for good (reason says why); the handle stays valid for outline_info */
};
/* KilnRenderOutlineInfo.reason */
enum {
    KILN_RENDER_OUTLINE_OK = 0,
    KILN_RENDER_OUTLINE_PENDING = 1,     /* WAITING: not applied yet (the next client step), or no client world */
    KILN_RENDER_OUTLINE_OUTSHONE = 2,    /* WAITING: another request on the entity wins (priority, then newest) */
    KILN_RENDER_OUTLINE_NO_MODEL = 3,    /* WAITING: the entity has no model instance now (hidden, not streamed, voxel) */
    KILN_RENDER_OUTLINE_SWITCHED_OFF = 4, /* WAITING: the player's kiln.json "ui": {"outlines": "off"} */
    KILN_RENDER_OUTLINE_REMOVED = 16,    /* ENDED: outline_remove or outline_clear */
    KILN_RENDER_OUTLINE_DESPAWNED = 17,  /* ENDED: the entity is gone from the client world */
    KILN_RENDER_OUTLINE_WORLD = 18,      /* ENDED: the client world ended (left the game, joined another) */
    KILN_RENDER_OUTLINE_EXPIRED = 19,    /* ENDED: duration_ms ran out */
    KILN_RENDER_OUTLINE_OWNER = 20,      /* ENDED: the owner was revoked (its mod unloaded) */
    KILN_RENDER_OUTLINE_FAILED = 21,     /* ENDED: the mechanism stopped (a hook or the stage failed); status says why */
};

typedef struct KilnRenderOutline {
    uint32_t size;        /* set to sizeof(KilnRenderOutline) */
    uint32_t rgba;        /* 0xRRGGBBAA; alpha 0 is refused */
    int32_t priority;     /* among mods' requests on the same entity: higher wins; ties: the newest */
    uint32_t flags;       /* KILN_RENDER_OUTLINE_* */
    uint32_t duration_ms; /* 0 = until removed */
    uint32_t reserved;    /* 0 */
} KilnRenderOutline;

typedef struct KilnRenderOutlineInfo {
    uint32_t size;      /* set to sizeof(KilnRenderOutlineInfo) before calling */
    uint32_t state;     /* KILN_RENDER_OUTLINE_WAITING / _SHOWN / _GAME / _ENDED */
    uint32_t reason;    /* KILN_RENDER_OUTLINE_* reason */
    KilnEntity entity;
    uint32_t group;     /* the game's highlight group this request's colour uses now (4 or 5), 0 when none */
    uint32_t merged;    /* 1: more than two colours are in use, this one shows with another's group */
} KilnRenderOutlineInfo;

typedef struct KilnRenderOutlineStatus {
    uint32_t size;      /* set to sizeof(KilnRenderOutlineStatus) before calling */
    uint8_t available;  /* feature ui.world_outline resolved and kiln.world can wrap the client world's programs */
    uint8_t installed;  /* the palette hooks are in */
    uint8_t staged;     /* the After stage on dynamic_render_model is active */
    uint8_t setting;    /* the player's kiln.json ui.outlines: 0 game-first (default), 1 mods-first, 2 off */
    uint32_t requests;  /* live requests (not ended) */
    uint32_t entities;  /* distinct entities with a live request */
    uint32_t shown;     /* entities whose model shows a Kiln group now */
    uint32_t game;      /* entities where the game's (or another tool's) highlight shows instead */
    uint32_t no_model;  /* entities without a model instance now */
    uint64_t sets;      /* renderer setter calls Kiln made (applies and restores) */
    uint64_t steps;     /* client steps the stage worked in */
    char detail[160];   /* why outlines are off or failed, in words, NUL-terminated; "" when all works */
} KilnRenderOutlineStatus;

typedef struct KilnRenderApi {
    KilnServiceHeader header; /* { sizeof(KilnRenderApi), KILN_RENDER_VERSION } */
    KilnResult(KILN_CALL* status)(KilnRenderStatus* out);

    /* ---- frames ---- */
    KilnResult(KILN_CALL* acquire_frames)(KilnOwner owner, KilnHandle* out);
    KilnResult(KILN_CALL* release)(KilnHandle handle);

    /* ---- camera (any thread) ----
     * camera: KILN_ERR_UNAVAILABLE when feature render.camera is off (out->available = 0, the rest zero).
     * world_to_screen: pixels of a world point with the latest camera on a screen of width x height (0 = the swapchain's
     *   size). out = {x, y, depth (metres along the view)}. Returns false when the camera is not valid or the point is
     *   behind the near plane (out still holds the projection then, mirrored: do not draw it). */
    KilnResult(KILN_CALL* camera)(KilnRenderCamera* out);
    bool(KILN_CALL* world_to_screen)(const double world[3], float width, float height, float out[3]);

    /* ---- drawing (inside a kiln.ui frame callback; pixels; colours 0xRRGGBBAA) ---- */
    void(KILN_CALL* line)(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness);
    void(KILN_CALL* rect)(float x0, float y0, float x1, float y1, uint32_t rgba, float rounding, float thickness, int32_t filled);
    /* segments 0 = automatic */
    void(KILN_CALL* circle)(float x, float y, float radius, uint32_t rgba, int32_t segments, float thickness, int32_t filled);
    void(KILN_CALL* triangle)(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgba, float thickness, int32_t filled);
    /* xy = count pairs; filled draws a convex polygon */
    void(KILN_CALL* polyline)(const float* xy, uint32_t count, uint32_t rgba, float thickness, int32_t closed, int32_t filled);
    /* size 0 = the default font size; flags KILN_RENDER_TEXT_* */
    void(KILN_CALL* text)(float x, float y, uint32_t rgba, float size, uint32_t flags, KilnStr text);
    void(KILN_CALL* text_size)(KilnStr text, float size, float* width, float* height);
    /* an image from kiln.ui image_load, stretched over the rectangle; uv 0..1; tint 0xFFFFFFFF = as is */
    void(KILN_CALL* image)(KilnHandle image, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t tint);

    /* ---- outlines (v2, since 0.2.0-dev.30.3; check header.size, see "Outlines" above; any thread) ----
     * outline: a new request (out = its handle). KILN_ERR_UNAVAILABLE: a server, feature ui.world_outline off, kiln.world
     *   without client-world wraps, or the mechanism failed (outline_status' detail says which). KILN_ERR_INVALID_ARGUMENT:
     *   owner 0, entity 0, opts NULL or short, alpha 0, an unknown flag, reserved not 0. KILN_ERR_BUFFER_TOO_SMALL: the
     *   owner holds 1024 live requests already.
     * outline_info: a request's state (also after it ended, until the owner's next outline_clear or 4096 newer ended
     *   requests). KILN_ERR_NOT_FOUND: an unknown handle.
     * outline_remove: ends one request (its entity's group is given back at the next client step). KILN_ERR_NOT_FOUND.
     * outline_clear: ends every request of the owner (a mod's unload calls it); KILN_OK also when it had none.
     * outline_status: counts and why it is off. */
    KilnResult(KILN_CALL* outline)(KilnOwner owner, KilnEntity entity, const KilnRenderOutline* opts, KilnHandle* out);
    KilnResult(KILN_CALL* outline_info)(KilnHandle handle, KilnRenderOutlineInfo* out);
    KilnResult(KILN_CALL* outline_remove)(KilnHandle handle);
    KilnResult(KILN_CALL* outline_clear)(KilnOwner owner);
    KilnResult(KILN_CALL* outline_status)(KilnRenderOutlineStatus* out);
} KilnRenderApi;

#ifdef __cplusplus
static_assert(sizeof(KilnRenderOutline) == 24, "KilnRenderOutline is 24 bytes");
static_assert(sizeof(KilnRenderOutlineInfo) == 24, "KilnRenderOutlineInfo is 24 bytes");
static_assert(sizeof(KilnRenderOutlineStatus) == 208, "KilnRenderOutlineStatus is 208 bytes");
static_assert(offsetof(KilnRenderApi, image) == 104, "v1 ends with image at 104 (112 bytes)");
static_assert(offsetof(KilnRenderApi, outline) == 112, "v2 outlines are appended at 112");
static_assert(sizeof(KilnRenderApi) == 152, "kiln.render v2 is 152 bytes");
#endif

#ifdef __cplusplus
}
#endif
#endif
