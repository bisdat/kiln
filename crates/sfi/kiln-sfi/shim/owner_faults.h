/* SPDX-License-Identifier: MIT */
#ifndef SFI_OWNER_FAULTS_H
#define SFI_OWNER_FAULTS_H
#include <stdint.h>
#define SFI_OWNER_DEPENDENCY 1u
#define SFI_OWNER_INITIALIZER 2u
typedef struct SfiOwnerFault { uint64_t owner;uint32_t flags,reserved; } SfiOwnerFault;
uint32_t sfi_owner_fault_note(uint64_t owner,uint32_t flags);
uint32_t sfi_owner_fault_copy(SfiOwnerFault* output,uint32_t capacity,uint32_t* required);
#endif
