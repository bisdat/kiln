/* SPDX-License-Identifier: MIT
 * Signature declarations adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer),
 * modules/mod-runtime/native_bridge/shroudforge_mod_native.h:79-81,133-136 (1.8.0).
 * Opaque types only: the real host/value/result layouts and bridge are not defined here.
 */
#ifndef KILN_SFI_FAULT_H
#define KILN_SFI_FAULT_H
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#define SFI_CALL __cdecl
#else
#define SFI_CALL
#endif
struct ShroudforgeModHost;
struct ShroudforgeNativeValue;
struct ShroudforgeNativeResults;
typedef int32_t (SFI_CALL* SfiModInit)(const struct ShroudforgeModHost*);
typedef void (SFI_CALL* SfiNativeCall)(void*, const struct ShroudforgeNativeValue*, size_t,
                                    struct ShroudforgeNativeResults*);
/* Returns exception code, zero on no exception. Semantic return is separate. */
uint32_t sfi_seh_init(SfiModInit fn, const struct ShroudforgeModHost* host, int32_t* out);
uint32_t sfi_seh_native(SfiNativeCall fn, void* context, const struct ShroudforgeNativeValue* args,
                        size_t count, struct ShroudforgeNativeResults* results);
uint32_t sfi_seh_load(const uint16_t* path, void** module, uint32_t* error);
#endif
