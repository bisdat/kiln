/* SPDX-License-Identifier: MIT */
#ifndef SFI_LOAD_WINDOW_H
#define SFI_LOAD_WINDOW_H
#include <stdint.h>
typedef struct SfiLoadWindowOps {
 void* context;
 uint32_t (*lock)(void*);
 void (*unlock)(void*);
 uint32_t (*observe)(void*);
 uint32_t (*scan)(void*);
 uint32_t (*preload)(void*);
 uint32_t (*load)(void*);
} SfiLoadWindowOps;
/* Caller closes the process-lifetime observation window in its SEH finally. */
uint32_t sfi_run_load_window(const SfiLoadWindowOps* ops);
#endif
