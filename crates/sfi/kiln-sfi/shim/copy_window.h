/* SPDX-License-Identifier: MIT */
#ifndef SFI_COPY_WINDOW_H
#define SFI_COPY_WINDOW_H
#include "monitor.h"
#define SFI_COPY_WINDOW_MAX 8192u
/* The verified-copy proofs admission accepted for the open load window, as the LOADED rescan reads them.
 * Publish under the loader hold that opens the window: kiln-sfi keeps its own copy, so the caller may free its list
 * once the window is closed, with or without the closing hold. 0 when count exceeds the bound or copies is NULL with a
 * count. */
uint32_t sfi_copy_window_publish(const SfiMappedProof* copies, uint32_t count);
/* Close, with or without the loader hold. */
void sfi_copy_window_clear(void);
/* Notification side (under the OS loader lock): the published proofs, or NULL with *count 0. */
const SfiMappedProof* sfi_copy_window_current(uint32_t* count);
#endif
