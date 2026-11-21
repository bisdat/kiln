/* SPDX-License-Identifier: MIT - a load window's copy proofs and a close that cannot take the loader hold. */
#include "copy_window.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Stands in for the OS loader lock: publication, the closing hold and every LOADED callback take it. */
static pthread_mutex_t hold=PTHREAD_MUTEX_INITIALIZER;
static SfiMappedProof proof(uintptr_t module,uint8_t tag){SfiMappedProof p;memset(&p,0,sizeof p);p.module=(void*)module;p.view=(void*)(module+1);memset(p.sha256,tag,32);return p;}
static int intact(const SfiMappedProof* p,uintptr_t module,uint8_t tag){if(p->module!=(void*)module||p->view!=(void*)(module+1))return 0;for(int i=0;i<32;i++)if(p->sha256[i]!=tag)return 0;return 1;}
/* sfi_verified_load's finally when window_lock fails: the window was published under the opening hold, a callback
 * is still comparing against the proofs it read, the close clears them WITHOUT the hold and the caller's list is freed. */
static int no_hold_close(void){
    SfiMappedProof* caller=malloc(2*sizeof *caller);if(!caller)return 1;
    caller[0]=proof(0x1000,0xa1);caller[1]=proof(0x2000,0xb2);
    pthread_mutex_lock(&hold);uint32_t ok=sfi_copy_window_publish(caller,2);pthread_mutex_unlock(&hold);
    if(!ok){fprintf(stderr,"FAIL publish refused\n");return 1;}
    uint32_t n=0;const SfiMappedProof* seen=sfi_copy_window_current(&n);
    if(!seen||n!=2){fprintf(stderr,"FAIL published %p/%u\n",(const void*)seen,n);return 1;}
    sfi_copy_window_clear();
    memset(caller,0xee,2*sizeof *caller);free(caller);
    if(!intact(&seen[0],0x1000,0xa1)||!intact(&seen[1],0x2000,0xb2)){fprintf(stderr,"FAIL a callback read the caller's freed list\n");return 1;}
    uint32_t after=7;if(sfi_copy_window_current(&after)||after){fprintf(stderr,"FAIL closed window still published %u\n",after);return 1;}
    puts("PASS a no-hold close leaves callbacks reading kiln-sfi's own copy, never the caller's list");return 0;
}
static int bounds(void){
    SfiMappedProof one=proof(0x3000,0xc3);
    if(sfi_copy_window_publish(NULL,1)||sfi_copy_window_publish(&one,SFI_COPY_WINDOW_MAX+1)){fprintf(stderr,"FAIL bad publish accepted\n");return 1;}
    uint32_t n=7;if(!sfi_copy_window_publish(NULL,0)||sfi_copy_window_current(&n)||n){fprintf(stderr,"FAIL empty window\n");return 1;}
    SfiMappedProof* full=calloc(SFI_COPY_WINDOW_MAX,sizeof *full);if(!full)return 1;
    for(uint32_t i=0;i<SFI_COPY_WINDOW_MAX;i++)full[i]=proof(0x10000+i*16u,(uint8_t)i);
    if(!sfi_copy_window_publish(full,SFI_COPY_WINDOW_MAX)){fprintf(stderr,"FAIL full window refused\n");free(full);return 1;}
    free(full);
    const SfiMappedProof* seen=sfi_copy_window_current(&n);
    if(!seen||n!=SFI_COPY_WINDOW_MAX||!intact(&seen[SFI_COPY_WINDOW_MAX-1],0x10000+(SFI_COPY_WINDOW_MAX-1)*16u,(uint8_t)(SFI_COPY_WINDOW_MAX-1))){fprintf(stderr,"FAIL full window\n");return 1;}
    sfi_copy_window_clear();
    puts("PASS bounds: NULL with a count and over-limit lists refused, empty and full windows kept");return 0;
}
static _Atomic uint32_t stop,bad,reads;
/* A LOADED callback: under the loader lock, every proof it reads is one whole published proof. */
static void* callback(void* unused){(void)unused;
    while(!atomic_load(&stop)){pthread_mutex_lock(&hold);uint32_t n=0;const SfiMappedProof* p=sfi_copy_window_current(&n);
        for(uint32_t k=0;k<n;k++){uint8_t tag=p[k].sha256[0];if(!intact(&p[k],(uintptr_t)p[k].module,tag)||((uintptr_t)p[k].module&0xf))atomic_store(&bad,1);}
        pthread_mutex_unlock(&hold);atomic_fetch_add(&reads,1);}
    return NULL;}
static int race(void){
    pthread_t reader;if(pthread_create(&reader,NULL,callback,NULL))return 1;
    for(uint32_t round=0;round<20000;round++){
        uint32_t n=1+round%4;SfiMappedProof* caller=malloc(n*sizeof *caller);if(!caller)return 1;
        for(uint32_t k=0;k<n;k++)caller[k]=proof(0x100000+(uintptr_t)(round*4+k)*16u,(uint8_t)(round+k));
        pthread_mutex_lock(&hold);uint32_t ok=sfi_copy_window_publish(caller,n);pthread_mutex_unlock(&hold);
        if(!ok)atomic_store(&bad,1);
        sfi_copy_window_clear();
        memset(caller,0xee,n*sizeof *caller);free(caller);
    }
    atomic_store(&stop,1);pthread_join(reader,NULL);
    if(atomic_load(&bad)||!atomic_load(&reads)){fprintf(stderr,"FAIL torn or freed proofs under the hold (reads %u)\n",atomic_load(&reads));return 1;}
    puts("PASS publish under the hold, close without it: callbacks never see a torn or freed proof");return 0;
}
int main(int argc,char** argv){
    const char* c=argc>1?argv[1]:"";
    if(!strcmp(c,"no-hold-close"))return no_hold_close();
    if(!strcmp(c,"bounds"))return bounds();
    if(!strcmp(c,"race"))return race();
    fprintf(stderr,"unknown case\n");return 2;
}
