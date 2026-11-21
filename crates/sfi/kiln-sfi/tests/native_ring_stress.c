/* SPDX-License-Identifier: MIT
 * Actual ring, not a fake. The deterministic case pauses a publisher across a
 * wrap; stress overlaps callback publication, drains and consumer shutdown.
 */
#include "notification_ring.h"
#include "breach_state.h"
#include "pin_state.h"
#include "seh_guard.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int controlled,paused,resume_step;
void sfi_ring_test_hook(uint64_t n,unsigned step){
    if(!controlled||n!=1)return;
    pthread_mutex_lock(&mutex);paused=(int)step;pthread_cond_broadcast(&changed);
    while(resume_step<(int)step)pthread_cond_wait(&changed,&mutex);
    pthread_mutex_unlock(&mutex);
}
static void publish(uint64_t owner){uint16_t name[2]={(uint16_t)(owner%60000+1000),0};sfi_ring_publish(7,name,1,owner);}
static void* first(void* p){(void)p;publish(1);return NULL;}
static int wrapped(void){
    controlled=1;pthread_t writer;pthread_create(&writer,NULL,first,NULL);
    pthread_mutex_lock(&mutex);while(paused<1)pthread_cond_wait(&changed,&mutex);pthread_mutex_unlock(&mutex);
    for(uint64_t n=2;n<=65;n++)publish(n);
    pthread_mutex_lock(&mutex);resume_step=1;pthread_cond_broadcast(&changed);while(paused<2)pthread_cond_wait(&changed,&mutex);pthread_mutex_unlock(&mutex);
    SfiBreach event={0};uint32_t state=0;sfi_ring_poll(64,&event,&state);
    int valid=state==2||(state==1&&event.sequence==event.owner&&event.module[0]==event.owner%60000+1000);
    pthread_mutex_lock(&mutex);resume_step=2;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&mutex);pthread_join(writer,NULL);
    if(!valid){fprintf(stderr,"FAIL ring accepted mixed ticket/payload: state=%u ticket=%llu owner=%llu\n",state,(unsigned long long)event.sequence,(unsigned long long)event.owner);return 1;}
    puts("PASS stalled publisher cannot corrupt wrapped slot");return 0;
}
static _Atomic int done;
static _Atomic uint64_t produced;
static void* producer(void* p){(void)p;for(uint64_t n=1;n<=200000;n++){publish(n);atomic_store(&produced,n);if(n%97==0)sched_yield();}atomic_store(&done,1);return NULL;}
static int stress(void){
    pthread_t writer;pthread_create(&writer,NULL,producer,NULL);uint64_t* cursor=calloc(1,sizeof *cursor);uint64_t seen=0,overflow=0;int closed=0;
    while(!atomic_load(&done)||*cursor<sfi_ring_sequence()){
        SfiBreach event={0};uint32_t state=0;sfi_ring_poll(*cursor,&event,&state);
        if(state==1){if(event.sequence!=event.owner||event.kind!=7||event.module[0]!=event.owner%60000+1000){fprintf(stderr,"FAIL incoherent notification\n");exit(1);}*cursor=event.sequence;seen++;}
        else if(state==2){*cursor=event.sequence;overflow++;}
        else sched_yield();
        if(!closed&&atomic_load(&produced)>1000){
            /* The Rust enforcement consumer is gone; callback-visible storage
             * must remain valid while producer keeps writing. No pointer to this
             * consumer allocation was ever stored in the actual C ring. */
            free(cursor);cursor=NULL;uint64_t before=atomic_load(&produced);while(!atomic_load(&done)&&atomic_load(&produced)<before+2000)sched_yield();
            cursor=calloc(1,sizeof *cursor);*cursor=sfi_ring_sequence();closed=1;
        }
    }
    pthread_join(writer,NULL);free(cursor);printf("PASS publish/drain/shutdown overlap: observed=%llu overflow=%llu\n",(unsigned long long)seen,(unsigned long long)overflow);return 0;
}
static uint32_t utf16(const char* text,uint16_t* out){uint32_t n=0;while(text[n]){out[n]=(unsigned char)text[n];n++;}return n;}
static int burst(int banned){
    uint16_t base[128],full[128];uint32_t bn=utf16("normal.dll",base),fn=utf16("C:/drivers/normal.dll",full);
    for(uint64_t n=1;n<=4096;n++){
        sfi_breach_observe(base,bn,full,fn,7); /* ordinary callbacks do not signal a breach */
        if(banned&&n==512){uint16_t name[128];uint32_t size=utf16(banned==1?"C:/mods/KFC-RuNtImE.DlL":"SHROUDFORGE-RUNTIME.DLL",name);
            if(banned==1)sfi_breach_observe(base,bn,name,size,7);else sfi_breach_observe(name,size,full,fn,7);
        }
        publish(n); /* force diagnostic loss independently of real breach proof */
    }
    SfiBreach event={0};uint32_t state=0;sfi_ring_poll(0,&event,&state);if(state!=2){fputs("FAIL burst did not overrun diagnostics\n",stderr);return 1;}
    /* A full scan can be empty after a banned DLL unloaded. Sticky evidence must
     * survive that, while a harmless diagnostic burst must not create a breach. */
    uint32_t flags=sfi_breach_recover(1,0);
    if((!banned&&flags)||(banned==1&&!(flags&SFI_BREACH_OTHER_RUNTIME))||(banned==2&&!(flags&SFI_BREACH_SHROUD))){fputs("FAIL diagnostic loss changed breach truth\n",stderr);return 1;}
    printf("PASS burst=%d diagnostics lost, confirmed flags=%u (no false quarantine)\n",banned,flags);return 0;
}
static SfiPinState pin_records[512];static _Atomic uint32_t pin_published;
static void* pin_callbacks(void* p){(void)p;for(uint32_t i=0;i<512;i++){sfi_pin_loaded(&pin_records[i],(uintptr_t)(0x1000+i),i%2?(int32_t)0xc0000001u:0,i+1);atomic_store(&pin_published,i+1);}return NULL;}
static void* pin_finishers(void* p){uint32_t success=(uint32_t)(uintptr_t)p;for(uint32_t i=0;i<512;i++){while(atomic_load(&pin_published)<i+1)sched_yield();sfi_pin_finish(&pin_records[i],i+1,success);}return NULL;}
static int pin_overlap(void){pthread_t callback,failed,succeeded;pthread_create(&callback,NULL,pin_callbacks,NULL);pthread_create(&failed,NULL,pin_finishers,(void*)(uintptr_t)0);pthread_create(&succeeded,NULL,pin_finishers,(void*)(uintptr_t)1);pthread_join(callback,NULL);pthread_join(failed,NULL);pthread_join(succeeded,NULL);for(uint32_t i=0;i<512;i++)if(!sfi_pin_failed(&pin_records[i])||atomic_load(&pin_records[i].module)!=(uintptr_t)(0x1000+i)){fputs("FAIL pin failure was lost/resurrected\n",stderr);return 1;}puts("PASS modeled pin callback/finish/read overlap:512 failures retained");return 0;}
static int guard_decisions(void){if(sfi_guard_decide(SFI_BREAKPOINT,0,1,7)!=1||sfi_guard_decide(SFI_SINGLE_STEP,0,1,7)!=1||sfi_guard_decide(SFI_STACK_OVERFLOW,1,1,7)!=0||sfi_guard_decide(SFI_STACK_OVERFLOW,0,1,7)!=2||sfi_guard_decide(SFI_STACK_OVERFLOW,0,0,7)!=3||sfi_guard_decide(SFI_STACK_OVERFLOW,0,0,0)!=2){fputs("FAIL stack guard reset/park decision\n",stderr);return 1;}puts("PASS debugger pass-through and injected stack-guard reset failure decisions");return 0;}
int main(int argc,char** argv){if(argc!=2)return 2;if(strcmp(argv[1],"guard-decisions")==0)return guard_decisions();if(strcmp(argv[1],"pin-overlap")==0)return pin_overlap();if(strcmp(argv[1],"wrap")==0)return wrapped();if(strcmp(argv[1],"burst-clean")==0)return burst(0);if(strcmp(argv[1],"burst-full-banned")==0)return burst(1);if(strcmp(argv[1],"burst-base-banned")==0)return burst(2);return stress();}
