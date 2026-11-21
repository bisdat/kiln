// SPDX-License-Identifier: MIT
// kiln.markers' parts and the glue between them (markers.cpp is the glue; dossier chapter 04).
//
//   store.cpp, events.cpp, arena.cpp, text.cpp, catalog.cpp   the logical store, the subscriptions, the icon catalog (S1, S2, S4)
//   anchors.cpp (+ its own files)                           following an entity: the explicit-world reader, the watch table,
//                                                           the create/destroy/network-create observers (S3, §4.2)
//   surfaces.cpp (+ its own files)                          admission and visibility, the native map list, the drawn
//                                                           compass and world labels (S5, S6, §4.5, §4.6)
//   link.cpp (+ its own files)                              wire v1, the server layers, the receiver, the bridge table
//                                                           (S7, S8, §4.4, §4.5)
//
// The glue owns the core's callbacks and calls each part at fixed points; a part never subscribes to the core's world
// lifecycle or a world's Post itself. Order at a client Post: anchors_post, link_client_post, then the logical notes are
// taken and surfaces_client_post adds its visibility notes to the same batch. Order at a server Post: anchors_post,
// link_server_post, then the server's notes. On a world's STOPPING: anchors_world, link_world, surfaces_world, then the
// store clears and the last notes go out on the stopping thread.
#ifndef KILN_MARKERS_GLUE_H
#define KILN_MARKERS_GLUE_H
#include <cstdint>
#include <string>
#include <vector>

#include "kiln_api.h"
#include "services/kiln_caps.h"
#include "services/kiln_markers.h"

#include "catalog.h"
#include "events.h"
#include "store.h"

namespace kmk {

struct Glue {
    const KilnApi* api = nullptr;
    KilnOwner owner = 0;                // kiln-markers.dll's own owner (process-owned work: the receiver's virtual publishers)
    uint32_t side = KILN_SIDE_SERVER;   // KILN_SIDE_CLIENT on a game client (hosted or joined)
    bool lifecycle = false;             // world STARTED / STOPPING are seen
    bool server_post = false;           // the server world's Post is subscribed (the anchors' follow reads and the link's server role)
    bool client_post = false;           // the client world's Post is subscribed (the link's receiver)
    Store* store = nullptr;
    Subscribers* subs = nullptr;
    CatalogHolder* catalog = nullptr;
    Switches* sw = nullptr;
    std::string exe;                    // this process's exe sha256, lower-case hex
};
extern Glue g;

void logf(int32_t level, const char* fmt, ...);
// One capability row (kiln.caps report); a no-op before on_ready found kiln.caps.
void report(const char* name, uint32_t state, uint32_t reason, const char* text);

// ---- S3: anchors (anchors.cpp; the watch table anchors.h, the reader world_read.h) --------------------------------------
void anchors_start();                         // kiln_plugin_main: resolve symbols (no game reads); kiln.markers.lab on a lab
void anchors_ready();                         // on_ready: the layout check through kiln.types, the observers, the store's
                                              // follow callbacks; report markers.anchors.client / .server, set Switches::anchors_client
void anchors_world(const KilnWorldEvent& e);  // STARTED / STOPPING of the server and client worlds
void anchors_post(uint32_t world_kind);       // a completed Post of that world: follow reads for the stored records
KilnResult anchors_capture(KilnOwner owner, uint32_t world_kind, uint64_t expected_epoch, KilnEntity entity, KilnMarkerAnchorResult* out);
KilnResult anchors_release(KilnOwner owner, KilnMarkerAnchor anchor);
void anchors_release_owner(KilnOwner owner);  // release_owner: the owner's leases
// The follow state of a stored record's incarnation (Record::follow) as of that world's last Post: READY with its position,
// FOLLOW_MISSING (no transform now; the same serial may recover), BAD_POSITION (read, but not finite or outside the world;
// may recover) or ANCHOR_ENDED (destroyed, recreated or its world stopped: terminal). Any thread; false for serial 0 or an
// unknown serial (treat as ANCHOR_ENDED).
bool anchors_follow(uint64_t serial, KilnVec3* position, uint32_t* reason);
// A stored reference for a record that follows (the local store takes its own through Store::set_follow; the link's sent
// records use these): the owner's live lease of that world kind and its current world, resolved to its incarnation's
// serial with one reference taken (KILN_MARKER_ACCEPTED), else KILN_MARKER_INVALID_ANCHOR with the words in why. Drop the
// reference when the record goes (removed, cleared, released, its world stopped). Any thread; it takes only the anchors'
// leaf lock, which never calls out, so a store may call it under its own lock.
uint32_t anchors_retain(KilnOwner owner, uint32_t world_kind, KilnMarkerAnchor lease, uint64_t* serial, char* why, size_t cap);
void anchors_drop(uint64_t serial);

// ---- S5, S6: surfaces (surfaces.cpp) ------------------------------------------------------------------------------------
void surfaces_ready();                        // on_ready: kiln.render / kiln.ui, report markers.map, .compass, .world, .nameplates
void surfaces_world(const KilnWorldEvent& e, std::vector<Note>* notes);  // client STOPPING: terminal visibility notes, an empty frame snapshot
void surfaces_client_post(std::vector<Note>* notes);  // admission, visibility notes (appended), the native map list, the frame snapshot

// ---- S7, S8: the link (link.cpp) ----------------------------------------------------------------------------------------
void link_ready();                            // on_ready: report markers.send (and markers.catalog on a dedicated server)
void link_world(const KilnWorldEvent& e);
void link_server_post();
void link_client_post();
KilnResult link_send(KilnOwner owner, KilnEntity recipient, const KilnMarkerSpec* spec, KilnMarkerResult* r);
KilnResult link_unsend(KilnOwner owner, KilnEntity recipient, KilnStr key, KilnMarkerResult* r);
KilnResult link_clear_sent(KilnOwner owner, KilnEntity recipient, KilnMarkerResult* r);
KilnResult link_suppress_sent(KilnOwner owner, KilnEntity recipient, KilnStr key, KilnMarkerResult* r);
void link_release_owner(KilnOwner owner);     // release_owner: the owner's sent records
extern const KilnMarkersBridgeApi kBridgeTable;
// markers.cpp: the link took a verified client's icon catalog (a dedicated server): markers.catalog is available from
// here and CatalogChanged goes out for role 0 at the next server step.
void catalog_from_link();

}  // namespace kmk
#endif
