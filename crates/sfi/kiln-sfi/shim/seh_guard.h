/* SPDX-License-Identifier: MIT */
#ifndef SFI_SEH_GUARD_H
#define SFI_SEH_GUARD_H
#include <stdint.h>
#define SFI_STACK_OVERFLOW 0xc00000fdu
#define SFI_BREAKPOINT 0x80000003u
#define SFI_SINGLE_STEP 0x80000004u
/* Decisions are shared with the native sanitizer model. */
uint32_t sfi_guard_decide(uint32_t code,uint32_t reset_ok,uint32_t interpreter,uint64_t owner);
typedef struct SfiParkedFault { uint64_t sequence,owner;uint32_t all,exception; } SfiParkedFault;
uint32_t sfi_guard_prepare(uint32_t* error);
void sfi_guard_interpreter(uint32_t active);
/* The marked interpreter thread's id, 0 when none (diagnostics and tests). */
uint32_t sfi_guard_interpreter_thread(void);
uint32_t sfi_guard_wait(uint64_t after,uint32_t milliseconds,SfiParkedFault* fault,uint32_t* available,uint32_t* error);
#ifdef _WIN32
int sfi_fault_filter(uint32_t code);
int sfi_callback_filter(uint32_t code);
uint32_t sfi_handle_fault(uint32_t code);
void sfi_after_load_error(uint32_t error);
#endif
#endif
