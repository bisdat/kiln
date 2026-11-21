/* SPDX-License-Identifier: MIT */
#ifndef SFI_BREACH_STATE_H
#define SFI_BREACH_STATE_H
#include <stdint.h>
#define SFI_BREACH_SHROUD 1u
#define SFI_BREACH_OTHER_RUNTIME 2u
#define SFI_BREACH_OBSERVER 4u
#define SFI_BREACH_DEPENDENCY 8u
uint32_t sfi_breach_name_mask(const uint16_t* name,uint32_t length);
uint32_t sfi_breach_flags(void);
uint32_t sfi_breach_recover(uint32_t scan_ok,uint32_t loaded_banned_mask);
void sfi_breach_observe(const uint16_t* base,uint32_t base_length,const uint16_t* full,uint32_t full_length,uint64_t owner);
void sfi_breach_note(uint32_t kind,const uint16_t* name,uint32_t length,uint64_t owner);
#endif
