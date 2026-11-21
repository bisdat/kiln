/* SPDX-License-Identifier: MIT - shared by Windows and native sanitizer model. */
#include "pin_state.h"
void sfi_pin_loaded(SfiPinState* s,uintptr_t module,int32_t status,uint64_t transaction){
    atomic_store(&s->module,module);atomic_store(&s->pin_status,status);atomic_store(&s->transaction,transaction);
    atomic_store(&s->state,status<0?SFI_IMAGE_FAILED:SFI_IMAGE_LOADED);
}
void sfi_pin_finish(SfiPinState* s,uint64_t transaction,uint32_t success){
    if(atomic_load(&s->transaction)!=transaction)return;
    if(!success){atomic_store(&s->state,SFI_IMAGE_FAILED);return;}
    uint32_t expected=SFI_IMAGE_LOADED;
    atomic_compare_exchange_strong(&s->state,&expected,SFI_IMAGE_READY); /* never resurrect FAILED */
}
uint32_t sfi_pin_failed(const SfiPinState* s){return atomic_load(&s->state)==SFI_IMAGE_FAILED;}
