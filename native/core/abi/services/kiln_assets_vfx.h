/* SPDX-License-Identifier: MIT
 * Private boot coordination between kiln.assets and kiln.vfx. No game patch is installed here. */
#ifndef KILN_ASSETS_VFX_H
#define KILN_ASSETS_VFX_H
#include "kiln_assets.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_ASSETS_VFX_SERVICE "kiln.assets.vfx"
#define KILN_ASSETS_VFX_VERSION 1u

typedef struct KilnAssetsVfxBudget {
    uint32_t size, capacity, game, reserved;
} KilnAssetsVfxBudget;

typedef struct KilnAssetsVfxApi {
    KilnServiceHeader header;
    /* size must be >= sizeof(KilnAssetsVfxBudget). UNAVAILABLE if the census is unknown.
     * Query before serving the cook: reserved includes distinct live unbound additions and slots
     * retained for already bound additions, including foreign/raw assets. Puts recheck atomically. */
    KilnResult(KILN_CALL* budget)(KilnAssetsVfxBudget*);
    /* Records an installed capacity (2048 or 4096), boot only, before resources-ready starts or S exists. */
    KilnResult(KILN_CALL* set_capacity)(KilnOwner, uint32_t);
    /* Cooker registration: retains mod with the resource under the store lock, for late omission reports.
     * Same serving/admission rules as KilnAssetsApi.resource_put; mod may be empty for an unknown origin. */
    KilnResult(KILN_CALL* resource_put)(KilnOwner owner, KilnStr mod, const KilnAssetKey* key,
                                     const void* data, size_t len, uint32_t flags);
} KilnAssetsVfxApi;

#ifdef __cplusplus
}
#endif
#endif
