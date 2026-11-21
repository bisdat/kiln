/* SPDX-License-Identifier: MIT
 * Proof of breach is independent of the lossy diagnostic ring. Process lifetime,
 * lock-free and callback-safe: comparisons, atomics and bounded POD copies only.
 */
#include "breach_state.h"
#include "notification_ring.h"
#include "owner_faults.h"
#include <stdatomic.h>
static _Atomic uint32_t flags;
static const uint16_t shroud[]={'s','h','r','o','u','d','f','o','r','g','e','-','r','u','n','t','i','m','e','.','d','l','l',0};
static const uint16_t other[]={'k','f','c','-','r','u','n','t','i','m','e','.','d','l','l',0};
static int equal(const uint16_t* a,uint32_t n,const uint16_t* b,uint32_t m){if(n!=m)return 0;for(uint32_t i=0;i<n;i++){uint16_t c=a[i];if(c>='A'&&c<='Z')c+=32;if(c!=b[i])return 0;}return 1;}
uint32_t sfi_breach_name_mask(const uint16_t* name,uint32_t n){
    if(!name||!n)return 0;
    uint32_t begin=0;for(uint32_t i=0;i<n;i++)if(name[i]=='\\'||name[i]=='/')begin=i+1;
    while(n>begin&&(name[n-1]=='.'||name[n-1]==' '))n--;
    if(equal(name+begin,n-begin,shroud,sizeof(shroud)/2-1))return SFI_BREACH_SHROUD;
    if(equal(name+begin,n-begin,other,sizeof(other)/2-1))return SFI_BREACH_OTHER_RUNTIME;
    return 0;
}
uint32_t sfi_breach_flags(void){return atomic_load(&flags);}
uint32_t sfi_breach_recover(uint32_t scan_ok,uint32_t present){
    atomic_fetch_or(&flags,scan_ok?(present&(SFI_BREACH_SHROUD|SFI_BREACH_OTHER_RUNTIME)):SFI_BREACH_OBSERVER);
    return atomic_load(&flags);
}
void sfi_breach_note(uint32_t kind,const uint16_t* name,uint32_t length,uint64_t owner){
    uint32_t mask=kind==1?sfi_breach_name_mask(name,length):(kind==2||kind==6)?SFI_BREACH_OBSERVER:0;
    if(kind==3||kind==4||kind==5){if(!owner||!sfi_owner_fault_note(owner,kind==5?SFI_OWNER_INITIALIZER:SFI_OWNER_DEPENDENCY))mask|=SFI_BREACH_DEPENDENCY;}
    if(mask)atomic_fetch_or(&flags,mask); /* proof BEFORE a diagnostic may be lost */
    sfi_ring_publish(kind,name,length,owner);
}
void sfi_breach_observe(const uint16_t* base,uint32_t bn,const uint16_t* full,uint32_t fn,uint64_t owner){
    uint32_t mask=sfi_breach_name_mask(base,bn)|sfi_breach_name_mask(full,fn);
    if(mask)atomic_fetch_or(&flags,mask);
    if(mask&SFI_BREACH_SHROUD)sfi_ring_publish(1,shroud,sizeof(shroud)/2-1,owner);
    if(mask&SFI_BREACH_OTHER_RUNTIME)sfi_ring_publish(1,other,sizeof(other)/2-1,owner);
}
