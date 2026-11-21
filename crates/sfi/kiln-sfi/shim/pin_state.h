/* SPDX-License-Identifier: MIT */
#ifndef SFI_PIN_STATE_H
#define SFI_PIN_STATE_H
#include <stdint.h>
#include <stdatomic.h>
typedef struct SfiPinState { _Atomic uintptr_t module; _Atomic uint64_t transaction; _Atomic int32_t pin_status; _Atomic uint32_t state; } SfiPinState;
#define SFI_IMAGE_LOADED 1u
#define SFI_IMAGE_READY 2u
#define SFI_IMAGE_FAILED 3u
void sfi_pin_loaded(SfiPinState* state,uintptr_t module,int32_t pin_status,uint64_t transaction);
void sfi_pin_finish(SfiPinState* state,uint64_t transaction,uint32_t success);
uint32_t sfi_pin_failed(const SfiPinState* state);
#endif
