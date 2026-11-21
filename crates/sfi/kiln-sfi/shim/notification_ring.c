/* SPDX-License-Identifier: MIT
 * Actual process-lifetime queue used by the Windows notification callback and
 * host sanitizer/stress tests. No consumer pointer or destructible Rust state.
 */
#include "notification_ring.h"
#include <stdatomic.h>
#define CAP 64
_Static_assert(ATOMIC_LLONG_LOCK_FREE==2&&ATOMIC_INT_LOCK_FREE==2&&ATOMIC_SHORT_LOCK_FREE==2,"notification queue must stay lock-free");
typedef struct Slot{_Atomic uint64_t stamp,owner;_Atomic uint32_t kind,claimed;_Atomic uint16_t name[64];}Slot;
static Slot slots[CAP];
static _Atomic uint64_t sequence;
static _Atomic uint32_t damaged;
#ifdef SFI_RING_TEST_HOOKS
void sfi_ring_test_hook(uint64_t sequence, unsigned step);
#define TEST_HOOK(n,s) sfi_ring_test_hook((n),(s))
#else
#define TEST_HOOK(n,s) ((void)0)
#endif
uint64_t sfi_ring_sequence(void){return atomic_load(&sequence);}
void sfi_ring_publish(uint32_t kind,const uint16_t* name,uint32_t length,uint64_t owner){
    uint32_t begin=0;for(uint32_t i=0;i<length;i++)if(name[i]=='\\'||name[i]=='/')begin=i+1;
    name+=begin;length-=begin;
    uint64_t n=atomic_fetch_add(&sequence,1)+1;Slot* s=&slots[(n-1)%CAP];
    /* Never wait under loader lock. A stalled publisher owns this slot until it
     * publishes; contention is explicit diagnostic loss, not a torn overwrite.
     * Breach proof is a separate sticky bitmap, never inferred from queue loss. */
    if(atomic_exchange(&s->claimed,1)){atomic_store(&damaged,1);return;}
    atomic_store(&s->stamp,0);TEST_HOOK(n,1);atomic_store(&s->owner,owner);atomic_store(&s->kind,kind);
    for(size_t i=0;i<64;i++)atomic_store(&s->name[i],i<length&&i<63?name[i]:0);
    TEST_HOOK(n,2);atomic_store(&s->stamp,n);atomic_store(&s->claimed,0);
}
void sfi_ring_poll(uint64_t after,SfiBreach* event,uint32_t* state){
    *state=0;uint64_t current=atomic_load(&sequence);
    if(current<=after)return;
    if(atomic_load(&damaged)){event->sequence=current;*state=2;return;}
    if(current-after>CAP){event->sequence=current;*state=2;return;}
    uint64_t expected=after+1;Slot* slot=&slots[(expected-1)%CAP];
    if(atomic_load(&slot->stamp)!=expected){*state=3;return;}
    event->sequence=expected;event->owner=atomic_load(&slot->owner);event->kind=atomic_load(&slot->kind);event->reserved=0;
    for(size_t i=0;i<64;i++)event->module[i]=atomic_load(&slot->name[i]);
    if(atomic_load(&slot->stamp)!=expected){*state=3;return;}
    *state=1;
}
