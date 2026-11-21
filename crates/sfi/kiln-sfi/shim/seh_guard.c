/* SPDX-License-Identifier: MIT */
#include "seh_guard.h"
#include "monitor.h"
#include "breach_state.h"
#include <stdatomic.h>
uint32_t sfi_guard_decide(uint32_t code,uint32_t reset_ok,uint32_t interpreter,uint64_t owner){
    if(code==SFI_BREAKPOINT||code==SFI_SINGLE_STEP)return 1; /* continue search */
    if(code!=SFI_STACK_OVERFLOW||reset_ok)return 0; /* return NativeFault */
    return interpreter||!owner?2:3; /* park: all runtimes / this package */
}
#ifdef _WIN32
#include <windows.h>
#include <malloc.h>
static _Atomic uint32_t prepared,interpreter_thread,park_all;
static _Atomic uint64_t park_owner,park_sequence;
static HANDLE parked_event; /* process lifetime; no callback references Rust */
uint32_t sfi_guard_prepare(uint32_t* error){
    *error=0;uint32_t expected=0;
    if(!atomic_compare_exchange_strong(&prepared,&expected,1)){if(expected!=2)*error=ERROR_NOT_READY;return 0;}
    parked_event=CreateEventW(NULL,FALSE,FALSE,NULL);if(!parked_event){*error=GetLastError();atomic_store(&prepared,3);}else atomic_store(&prepared,2);return 0;
}
void sfi_guard_interpreter(uint32_t active){uint32_t tid=GetCurrentThreadId();if(active)atomic_store(&interpreter_thread,tid);else{uint32_t expected=tid;atomic_compare_exchange_strong(&interpreter_thread,&expected,0);}}
uint32_t sfi_guard_interpreter_thread(void){return atomic_load(&interpreter_thread);}
int sfi_fault_filter(uint32_t code){return sfi_guard_decide(code,1,0,0)==1?EXCEPTION_CONTINUE_SEARCH:EXCEPTION_EXECUTE_HANDLER;}
/* An OS loader callback must unwind to our outer guarded operation before a
 * stack is parked, otherwise the thread would retain the loader lock forever. */
int sfi_callback_filter(uint32_t code){return code==SFI_STACK_OVERFLOW?EXCEPTION_CONTINUE_SEARCH:sfi_fault_filter(code);}
static void signal_park(uint32_t all,uint64_t owner){
    uint64_t expected=0;if(owner&&!atomic_compare_exchange_strong(&park_owner,&expected,owner)&&expected!=owner)all=1;
    if(all||!owner)atomic_store(&park_all,1);
    atomic_fetch_add(&park_sequence,1);if(atomic_load(&prepared)==2)SetEvent(parked_event);
}
uint32_t sfi_handle_fault(uint32_t code){
    /* Reset immediately in the handler AFTER unwinding. On failure do only the
     * bounded atomic/event notification and minimal park, not record traversal. */
    uint32_t reset=code==SFI_STACK_OVERFLOW?(uint32_t)_resetstkoflw():1;
    uint64_t owner=sfi_monitor_current_owner();
    if(code==SFI_STACK_OVERFLOW&&!reset){uint32_t decision=sfi_guard_decide(code,0,atomic_load(&interpreter_thread)==GetCurrentThreadId(),owner);signal_park(decision==2,owner);for(;;)Sleep(INFINITE);}
    sfi_native_call_complete(owner,0);
    static const uint16_t name[]={'n','a','t','i','v','e',' ','c','a','l','l',' ','f','a','u','l','t',0};
    sfi_breach_note(6,name,17,0);return code;
}
void sfi_after_load_error(uint32_t error){if(error==1001||error==SFI_STACK_OVERFLOW)sfi_handle_fault(SFI_STACK_OVERFLOW);}
uint32_t sfi_guard_wait(uint64_t after,uint32_t milliseconds,SfiParkedFault* out,uint32_t* available,uint32_t* error){
    *available=0;*error=0;if(atomic_load(&prepared)!=2){*error=ERROR_NOT_READY;return 0;}
    if(atomic_load(&park_sequence)<=after){DWORD result=WaitForSingleObject(parked_event,milliseconds);if(result==WAIT_FAILED){*error=GetLastError();return 0;}}
    uint64_t sequence=atomic_load(&park_sequence);if(sequence>after){out->sequence=sequence;out->owner=atomic_load(&park_owner);out->all=atomic_load(&park_all);out->exception=SFI_STACK_OVERFLOW;sfi_native_call_complete(out->owner,0);*available=1;}return 0;
}
#else
uint32_t sfi_guard_prepare(uint32_t* error){*error=50;return 0;}
void sfi_guard_interpreter(uint32_t active){(void)active;}
uint32_t sfi_guard_interpreter_thread(void){return 0;}
uint32_t sfi_guard_wait(uint64_t after,uint32_t ms,SfiParkedFault* out,uint32_t* available,uint32_t* error){(void)after;(void)ms;(void)out;*available=0;*error=50;return 0;}
#endif
