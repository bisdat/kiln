/* SPDX-License-Identifier: MIT
 * Windows loader ABI only, no game code. Notification callbacks execute under
 * loader lock: atomics/POD copies only, never Rust, Lua, logging or allocation.
 */
#include "monitor.h"
#include "notification_ring.h"
#include "breach_state.h"
#include "seh_guard.h"
#ifdef _WIN32
#include <windows.h>
#include <stdatomic.h>
typedef struct SfiUnicode { USHORT Length, MaximumLength; PWSTR Buffer; } SfiUnicode;
typedef struct SfiNotification { ULONG Flags; const SfiUnicode *FullDllName, *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } SfiNotification;
typedef LONG (NTAPI* SfiRegisterNotification)(ULONG, void(NTAPI*)(ULONG,const SfiNotification*,void*),void*,void**);
__declspec(dllimport) BOOL WINAPI K32EnumProcessModules(HANDLE,HMODULE*,DWORD,LPDWORD);
_Static_assert(sizeof(SfiBreach)==152 && offsetof(SfiBreach,module)==24, "monitor event ABI");
_Static_assert(sizeof(SfiFileId)==24, "monitor file identity ABI");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2, "loader callback needs lock-free x64 atomics");
/* Native host APIs may be called by mod-owned threads too. Static TLS is
 * provisioned by the OS before registration; callback reads allocate nothing. */
__declspec(thread) static uint64_t active_owner;
static _Atomic uint64_t module_epoch;
static _Atomic uint32_t started;
static void* cookie; /* Never unregistered: code and queue remain process-wide. */
static int banned(const WCHAR* name, size_t count) {
    static const WCHAR a[]=L"shroudforge-runtime.dll", b[]=L"kfc-runtime.dll";
    const WCHAR* candidates[2]={a,b}; size_t lengths[2]={sizeof(a)/2-1,sizeof(b)/2-1};
    size_t begin=0;
    for(size_t i=0;i<count;i++) if(name[i]==L'\\'||name[i]==L'/') begin=i+1;
    while(count>begin&&(name[count-1]==L'.'||name[count-1]==L' ')) count--;
    for(size_t j=0;j<2;j++) {
        if(count-begin!=lengths[j]) continue;
        size_t i=0; for(;i<lengths[j];i++) { WCHAR c=name[begin+i]; if(c>=L'A'&&c<=L'Z') c+=32; if(c!=candidates[j][i]) break; }
        if(i==lengths[j]) return 1;
    }
    return 0;
}
static void record(uint32_t kind, const WCHAR* name, size_t length, uint64_t owner) {
    sfi_breach_note(kind,(const uint16_t*)name,(uint32_t)length,owner);
}
void sfi_report_native_breach(uint32_t kind,const uint16_t* name,uint32_t count,uint64_t owner) { record(kind,(const WCHAR*)name,count,owner); }
uint64_t sfi_monitor_sequence(void) { return sfi_ring_sequence(); }
uint64_t sfi_monitor_epoch(void) { return atomic_load(&module_epoch); }
uint64_t sfi_monitor_current_owner(void) { return active_owner; }
static void NTAPI notify(ULONG reason,const SfiNotification* data,void* context) {
    (void)context;
    atomic_fetch_add(&module_epoch,1);
    if(reason!=1) return;
    __try {
        const SfiUnicode* base=data->BaseDllName;const SfiUnicode* full=data->FullDllName;
        if((!base&&!full)||(base&&(!base->Buffer||base->Length%2||base->Length>base->MaximumLength))||(full&&(!full->Buffer||full->Length%2||full->Length>full->MaximumLength))) { record(2,L"invalid loader notification",27,0); return; }
        uint64_t owner=sfi_monitor_current_owner();
        /* Sticky proof is set before lossy diagnostics, checking BOTH names. */
        sfi_breach_observe(base?(const uint16_t*)base->Buffer:NULL,base?base->Length/2:0,full?(const uint16_t*)full->Buffer:NULL,full?full->Length/2:0,owner);
        const SfiUnicode* name=full?full:base;
        sfi_check_delay_mapping((const uint16_t*)name->Buffer,name->Length/2,data->DllBase,owner);
    } __except(sfi_callback_filter((uint32_t)GetExceptionCode())) { record(2,L"loader notification fault",25,0); }
}
uint32_t sfi_monitor_start(uint32_t* error) {
    *error=0;
    __try {
        uint32_t expected=0;
        if(!atomic_compare_exchange_strong(&started,&expected,1)) { if(expected!=2) *error=ERROR_NOT_READY; return 0; }
        HMODULE self=NULL;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCWSTR)(uintptr_t)&notify,&self)) { *error=GetLastError(); atomic_store(&started,3); return 0; }
        HMODULE ntdll=GetModuleHandleW(L"ntdll.dll"); /* already loaded; never LoadLibrary */
        SfiRegisterNotification registration=ntdll?(SfiRegisterNotification)GetProcAddress(ntdll,"LdrRegisterDllNotification"):NULL;
        if(!registration) { *error=ERROR_PROC_NOT_FOUND; atomic_store(&started,3); return 0; }
        LONG rc=registration(0,&notify,NULL,&cookie);
        if(rc<0) { *error=(uint32_t)rc; atomic_store(&started,3); } else atomic_store(&started,2);
        return 0;
    } __except(sfi_callback_filter((uint32_t)GetExceptionCode())) { atomic_store(&started,3); return (uint32_t)GetExceptionCode(); }
}
void sfi_monitor_owner(uint64_t owner) { active_owner=owner; }
uint32_t sfi_monitor_modules(void** modules,uint32_t capacity,uint32_t* count,uint32_t* error) {
    *error=0; *count=0;
    __try {
        DWORD needed=0;
        if(capacity>4096||!K32EnumProcessModules(GetCurrentProcess(),(HMODULE*)modules,capacity*(DWORD)sizeof(void*),&needed)) { *error=GetLastError(); if(!*error)*error=ERROR_INVALID_DATA; return 0; }
        if(needed>capacity*sizeof(void*)||needed%sizeof(void*)) { *error=ERROR_INSUFFICIENT_BUFFER; return 0; }
        *count=needed/(uint32_t)sizeof(void*); return 0;
    } __except(sfi_callback_filter((uint32_t)GetExceptionCode())) { return (uint32_t)GetExceptionCode(); }
}
uint32_t sfi_monitor_module_path(void* module,uint16_t* path,uint32_t capacity,uint32_t* count,uint32_t* error) {
    *error=0; *count=0;
    __try {
        DWORD n=GetModuleFileNameW((HMODULE)module,(WCHAR*)path,capacity);
        if(!n||n>=capacity) { *error=GetLastError(); if(!*error)*error=ERROR_INSUFFICIENT_BUFFER; } else *count=n;
        return 0;
    } __except(sfi_callback_filter((uint32_t)GetExceptionCode())) { return (uint32_t)GetExceptionCode(); }
}
uint32_t sfi_monitor_scan(uint32_t* error) {
    *error=0;
    __try {
        void* modules[4096]; uint32_t count=0;
        uint32_t fault=sfi_monitor_modules(modules,4096,&count,error); if(fault||*error) return fault;
        for(uint32_t i=0;i<count;i++) {
            WCHAR path[32768]; uint32_t length=0;
            fault=sfi_monitor_module_path(modules[i],(uint16_t*)path,32768,&length,error); if(fault||*error)return fault;
            if(banned(path,length)) record(1,path,length,0);
        }
        return 0;
    } __except(sfi_callback_filter((uint32_t)GetExceptionCode())) { return (uint32_t)GetExceptionCode(); }
}
uint32_t sfi_monitor_poll(uint64_t after,SfiBreach* event,uint32_t* state) {
    __try { sfi_ring_poll(after,event,state);return 0; }
    __except(sfi_callback_filter((uint32_t)GetExceptionCode())) { return (uint32_t)GetExceptionCode(); }
}
uint32_t sfi_monitor_file_id(void* file,SfiFileId* id,uint32_t* error) {
    *error=0;
    __try {
        BY_HANDLE_FILE_INFORMATION info;
        if(!GetFileInformationByHandle((HANDLE)file,&info)) { *error=GetLastError(); return 0; }
        id->volume=info.dwVolumeSerialNumber; id->index=((uint64_t)info.nFileIndexHigh<<32)|info.nFileIndexLow; id->size=((uint64_t)info.nFileSizeHigh<<32)|info.nFileSizeLow; return 0;
    } __except(sfi_callback_filter((uint32_t)GetExceptionCode())) { return (uint32_t)GetExceptionCode(); }
}
#else
/* Host tests use an injected backend, never pretend this is Windows enforcement. */
void sfi_report_native_breach(uint32_t k,const uint16_t* n,uint32_t c,uint64_t o){sfi_breach_note(k,n,c,o);}
uint64_t sfi_monitor_sequence(void){return sfi_ring_sequence();}
uint64_t sfi_monitor_epoch(void){return 0;}
uint64_t sfi_monitor_current_owner(void){return 0;}
uint32_t sfi_monitor_start(uint32_t* e) { *e=50; return 0; }
uint32_t sfi_monitor_scan(uint32_t* e) { *e=50; return 0; }
uint32_t sfi_monitor_poll(uint64_t a,SfiBreach* b,uint32_t* s) { sfi_ring_poll(a,b,s);return 0; }
void sfi_monitor_owner(uint64_t owner) { (void)owner; }
uint32_t sfi_monitor_modules(void** m,uint32_t c,uint32_t* n,uint32_t* e) { (void)m;(void)c;*n=0;*e=50;return 0; }
uint32_t sfi_monitor_module_path(void* m,uint16_t* p,uint32_t c,uint32_t* n,uint32_t* e) { (void)m;(void)p;(void)c;*n=0;*e=50;return 0; }
uint32_t sfi_monitor_file_id(void* f,SfiFileId* i,uint32_t* e) { (void)f;(void)i;*e=50;return 0; }
#endif
