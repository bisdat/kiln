/* SPDX-License-Identifier: MIT - the copy proofs a LOADED callback reads for the open load window.
 * Callbacks read kiln-sfi's own process-lifetime copy, never the caller's list. The copy is written only by a publish,
 * which runs under the loader hold that opens a window and so never beside a callback (callbacks hold the loader lock).
 * A close only clears the two atomics: when sfi_verified_load cannot take its closing hold, a callback still comparing
 * against proofs it read keeps reading valid memory after the caller's list is freed. The proofs' views belong to the
 * Rust backend's retained view cache, which outlives every window; a callback only hands a view to
 * NtAreMappedFilesTheSame (a kernel lookup) and never reads through it. sfi_verified_load clears the window before it
 * reopens its load gate, so a late close never clears a newer window's proofs. */
#include "copy_window.h"
#include <stdatomic.h>
#include <string.h>
static SfiMappedProof stored[SFI_COPY_WINDOW_MAX];
static _Atomic(const SfiMappedProof*) current;
static _Atomic uint32_t current_count;
uint32_t sfi_copy_window_publish(const SfiMappedProof* copies,uint32_t count){
    if(count>SFI_COPY_WINDOW_MAX||(!copies&&count))return 0;
    atomic_store(&current,NULL);atomic_store(&current_count,0);
    if(count)memcpy(stored,copies,(size_t)count*sizeof *copies);
    atomic_store(&current_count,count);atomic_store(&current,count?stored:NULL);return 1;
}
void sfi_copy_window_clear(void){atomic_store(&current_count,0);atomic_store(&current,NULL);}
const SfiMappedProof* sfi_copy_window_current(uint32_t* count){
    const SfiMappedProof* copies=atomic_load(&current);uint32_t n=atomic_load(&current_count);
    *count=copies?n:0;return copies;
}
