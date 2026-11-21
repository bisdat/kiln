/* SPDX-License-Identifier: MIT */
#include "breach_state.h"
#include "owner_faults.h"
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
static _Atomic uint32_t done;
static void* publish(void* context){uint64_t base=(uint64_t)(uintptr_t)context;for(uint64_t i=0;i<256;i++)sfi_owner_fault_note(base+i,SFI_OWNER_DEPENDENCY);atomic_fetch_add(&done,1);return NULL;}
int main(void){static const uint16_t name[]={'f','a','i','l','e','d',0};sfi_breach_note(5,name,6,7);SfiOwnerFault owners[4096];uint32_t count=0;
 if(sfi_breach_flags()!=0){fprintf(stderr,"FAIL clean owner7 initializer globally poisoned loads: flags%u\n",sfi_breach_flags());return 1;}
 if(!sfi_owner_fault_copy(owners,4096,&count)||count!=1||owners[0].owner!=7||owners[0].flags!=SFI_OWNER_INITIALIZER)return 1;
 pthread_t a,b;pthread_create(&a,NULL,publish,(void*)(uintptr_t)100);pthread_create(&b,NULL,publish,(void*)(uintptr_t)1000);while(atomic_load(&done)!=2){if(!sfi_owner_fault_copy(owners,4096,&count))return 1;}pthread_join(a,NULL);pthread_join(b,NULL);if(!sfi_owner_fault_copy(owners,4096,&count)||count!=513)return 1;
 sfi_breach_note(6,name,6,0);if(!(sfi_breach_flags()&SFI_BREACH_OBSERVER))return 1;puts("PASS clean initializer owner-only, internal SEH remains global");return 0;}
