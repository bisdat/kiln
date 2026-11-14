// SPDX-License-Identifier: MIT
// kiln_markers.h's v1 layouts on a 64-bit host: the sizes (the header asserts them too, so this file also proves the
// header's asserts compile) and the offsets the managed mirror (Kiln.Markers/Native.cs) and future readers rely on.
#include <cstddef>
#include <cstdio>

#include "services/kiln_markers.h"

static_assert(sizeof(void*) == 8, "64-bit host");

#define SIZE(t, n) static_assert(sizeof(t) == (n), #t)
#define AT(t, f, n) static_assert(offsetof(t, f) == (n), #t "." #f)

SIZE(KilnMarkerSpec, 96);
SIZE(KilnMarkerResult, 280);
SIZE(KilnMarkerAnchorResult, 320);
SIZE(KilnMarkerIcon, 304);
SIZE(KilnMarkerChange, 192);
SIZE(KilnMarkerVisibility, 208);
SIZE(KilnMarkerCatalogChange, 280);
SIZE(KilnMarkersApi, 144);
SIZE(KilnMarkerPeer, 40);
SIZE(KilnMarkerEnvelope, 40);
SIZE(KilnMarkersBridgeApi, 48);

AT(KilnMarkerSpec, key, 8);
AT(KilnMarkerSpec, position, 24);
AT(KilnMarkerSpec, follow, 48);
AT(KilnMarkerSpec, height, 56);
AT(KilnMarkerSpec, icon_type, 64);
AT(KilnMarkerSpec, priority, 72);
AT(KilnMarkerSpec, map_label_mode, 76);
AT(KilnMarkerSpec, text, 80);
AT(KilnMarkerResult, affected, 20);
AT(KilnMarkerResult, reason, 24);
AT(KilnMarkerAnchorResult, anchor, 8);
AT(KilnMarkerAnchorResult, world_kind, 32);
AT(KilnMarkerAnchorResult, position, 40);
AT(KilnMarkerAnchorResult, reason, 64);
AT(KilnMarkerIcon, texture_guid, 16);
AT(KilnMarkerIcon, key, 32);
AT(KilnMarkerIcon, alias, 46);
AT(KilnMarkerIcon, texture_name, 175);
AT(KilnMarkerChange, world_epoch, 16);
AT(KilnMarkerChange, sequence, 40);
AT(KilnMarkerChange, affected, 48);
AT(KilnMarkerChange, owner, 56);
AT(KilnMarkerChange, key, 121);
AT(KilnMarkerVisibility, world_epoch, 16);
AT(KilnMarkerVisibility, requested, 48);
AT(KilnMarkerVisibility, reason, 60);
AT(KilnMarkerVisibility, owner, 72);
AT(KilnMarkerVisibility, key, 137);
AT(KilnMarkerCatalogChange, generation, 16);
AT(KilnMarkerCatalogChange, reason, 24);
AT(KilnMarkersApi, bind_owner, 8);
AT(KilnMarkersApi, set_local, 72);
AT(KilnMarkersApi, subscribe, 128);
AT(KilnMarkersApi, unsubscribe, 136);
AT(KilnMarkerPeer, generation, 32);
AT(KilnMarkerEnvelope, bytes, 24);
AT(KilnMarkerEnvelope, length, 32);
AT(KilnMarkersBridgeApi, pump, 40);

int main() {
    std::puts("PASS kiln_markers.h: 11 v1 struct and table sizes, 38 field offsets (64-bit host)");
    return 0;
}
