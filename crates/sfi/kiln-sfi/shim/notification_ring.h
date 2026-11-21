/* SPDX-License-Identifier: MIT */
#ifndef SFI_NOTIFICATION_RING_H
#define SFI_NOTIFICATION_RING_H
#include "monitor.h"
void sfi_ring_publish(uint32_t kind,const uint16_t* name,uint32_t length,uint64_t owner);
void sfi_ring_poll(uint64_t after,SfiBreach* event,uint32_t* state);
uint64_t sfi_ring_sequence(void);
#endif
