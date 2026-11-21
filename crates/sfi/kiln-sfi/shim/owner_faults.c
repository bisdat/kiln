/* SPDX-License-Identifier: MIT - process-lifetime proof, independent of ring detail. */
#include "owner_faults.h"
#include <stdatomic.h>
#define OWNER_SLOTS 4096
static struct{_Atomic uint64_t owner;_Atomic uint32_t flags;} slots[OWNER_SLOTS];
uint32_t sfi_owner_fault_note(uint64_t owner,uint32_t flags){
 if(!owner)return 0;
 for(uint32_t i=0;i<OWNER_SLOTS;i++){uint32_t at=(uint32_t)((owner+i)%OWNER_SLOTS);uint64_t expected=0;uint64_t found=atomic_load(&slots[at].owner);
  if(found==owner||(!found&&atomic_compare_exchange_strong(&slots[at].owner,&expected,owner))){atomic_fetch_or(&slots[at].flags,flags);return 1;}
  if(expected==owner){atomic_fetch_or(&slots[at].flags,flags);return 1;}
 }return 0;
}
uint32_t sfi_owner_fault_copy(SfiOwnerFault* output,uint32_t capacity,uint32_t* required){
 uint32_t count=0;for(uint32_t i=0;i<OWNER_SLOTS;i++)if(atomic_load(&slots[i].flags))count++;*required=count;if(capacity<count||(!output&&count))return 0;
 uint32_t at=0;for(uint32_t i=0;i<OWNER_SLOTS;i++){uint32_t flags=atomic_load(&slots[i].flags);if(!flags)continue;if(at>=capacity){*required=at+1;return 0;}output[at++]=(SfiOwnerFault){atomic_load(&slots[i].owner),flags,0};}*required=at;return 1;
}
