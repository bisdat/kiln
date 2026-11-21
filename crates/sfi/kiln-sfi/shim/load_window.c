/* SPDX-License-Identifier: MIT - shared orchestration used by Windows and host tests. */
#include "load_window.h"
uint32_t sfi_run_load_window(const SfiLoadWindowOps* o) {
    if (!o->lock(o->context)) return 0;
    uint32_t ok=o->observe(o->context)&&o->scan(o->context);
    o->unlock(o->context);
    if (!ok||!o->preload(o->context)) return 0;
    if (!o->lock(o->context)) return 0;
    ok=o->scan(o->context);
    o->unlock(o->context);
    if (!ok||!o->load(o->context)) return 0;
    if (!o->lock(o->context)) return 0;
    ok=o->scan(o->context);
    o->unlock(o->context);
    return ok;
}
