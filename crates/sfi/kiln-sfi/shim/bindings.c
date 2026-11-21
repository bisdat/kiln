/* SPDX-License-Identifier: MIT
 * Read-only OS binding proofs and a C-only loader-lock transaction. No Rust or
 * Lua callback occurs here. Package DLLs keep normal Windows initialization order.
 */
#include "monitor.h"
#include "breach_state.h"
#include "pin_state.h"
#include "seh_guard.h"
#include "api_set.h"
#include "load_window.h"
#include "pe_identity.h"
#include "copy_window.h"
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#include <stdatomic.h>
#ifndef SEC_IMAGE_NO_EXECUTE
#define SEC_IMAGE_NO_EXECUTE 0x11000000
#endif
_Static_assert(sizeof(SfiBinding)==72 && offsetof(SfiBinding,sha256)==40,"native binding ABI");
_Static_assert(sizeof(SfiMappedProof)==48 && sizeof(SfiApiBinding)==24,"native proof ABI");
typedef LONG (NTAPI* SameMapped)(void*,void*);
typedef LONG (NTAPI* LockLoader)(ULONG,ULONG*,ULONG_PTR*);
typedef LONG (NTAPI* UnlockLoader)(ULONG,ULONG_PTR);
typedef LONG (NTAPI* PinImage)(ULONG,void*);
typedef LONG (NTAPI* FindEntry)(void*,void**);
static _Atomic(PinImage) pin_function;
static _Atomic(FindEntry) find_entry_function;
static _Atomic uintptr_t ntdll_base;
static _Atomic uint32_t on_wine;
static _Atomic(SameMapped) same_function;
static PinImage pin_api(void){PinImage fn=atomic_load(&pin_function);if(!fn){fn=(PinImage)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"LdrAddRefDll");if(fn)atomic_store(&pin_function,fn);}return fn;}
static SameMapped same_api(void) {
    SameMapped fn=atomic_load(&same_function);
    if(!fn) { fn=(SameMapped)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtAreMappedFilesTheSame"); if(fn)atomic_store(&same_function,fn); }
    return fn;
}
uint32_t sfi_image_view(void* file,void** view,uint32_t* error) {
    *view=NULL;*error=0;
    __try {
        if(!same_api()) { *error=ERROR_PROC_NOT_FOUND;return 0; }
        HANDLE map=CreateFileMappingW((HANDLE)file,NULL,PAGE_READONLY|SEC_IMAGE_NO_EXECUTE,0,0,NULL);
        if(!map) { *error=GetLastError();return 0; }
        *view=MapViewOfFile(map,FILE_MAP_READ,0,0,0);if(!*view)*error=GetLastError();CloseHandle(map);return 0;
    } __except(sfi_callback_filter((uint32_t)GetExceptionCode())) { return (uint32_t)GetExceptionCode(); }
}
uint32_t sfi_image_unmap(void* view) {
    __try { if(view)UnmapViewOfFile(view);return 0; } __except(sfi_callback_filter((uint32_t)GetExceptionCode())) { return (uint32_t)GetExceptionCode(); }
}
static int same_view(void* module,void* view,uint32_t* error) {
    SameMapped fn=atomic_load(&same_function);
    if(!fn) { *error=ERROR_PROC_NOT_FOUND;return 0; }
    LONG status=fn(module,view);
    if(status==0)return 1;
    if((uint32_t)status!=0xc00000d4u) *error=(uint32_t)status; /* STATUS_NOT_SAME_DEVICE is an actual mismatch. */
    return 0;
}
uint32_t sfi_same_mapping(void* module,void* view,uint32_t* same,uint32_t* error) {
    *same=0;*error=0;
    __try { *same=(uint32_t)same_view(module,view,error);return 0; }
    __except(sfi_callback_filter((uint32_t)GetExceptionCode())) { return (uint32_t)GetExceptionCode(); }
}
uint32_t sfi_mapped_header_matches(void* module,const uint8_t* disk,size_t length,uint32_t* matches){
    *matches=0;
    __try{const uint8_t* mapped=module;if(!mapped||length<64)return 0;uint32_t pe;CopyMemory(&pe,mapped+0x3c,4);if(pe>0x100000)return 0;uint32_t headers;CopyMemory(&headers,mapped+pe+24+60,4);if(headers<64||headers>0x100000)return 0;*matches=sfi_pe_headers_equal(mapped,headers,disk,length);return 0;}
    __except(sfi_callback_filter((uint32_t)GetExceptionCode())){return (uint32_t)GetExceptionCode();}
}
static size_t length(const WCHAR* s,size_t limit) { size_t n=0;while(n<limit&&s[n])n++;return n; }
static WCHAR lower(WCHAR c) { return c>=L'A'&&c<=L'Z'?(WCHAR)(c+32):c; }
static int equal(const WCHAR* a,size_t an,const WCHAR* b,size_t bn) { if(an!=bn)return 0;for(size_t i=0;i<an;i++)if(lower(a[i])!=lower(b[i]))return 0;return 1; }
static int api_name(const WCHAR* s,size_t n) { return n>=4&&(equal(s,4,L"api-",4)||equal(s,4,L"ext-",4)); }
static uint32_t word(const unsigned char* data,uint32_t offset) { uint32_t value;CopyMemory(&value,data+offset,4);return value; }
static int component(const WCHAR* s,size_t n) { if(!n||n>=260)return 0;for(size_t i=0;i<n;i++)if(s[i]<33||s[i]>126||s[i]==L'/'||s[i]==L'\\'||s[i]==L':')return 0;return 1; }
/* Windows 10+/Wine API-set namespace v6. Offsets and counts are bounds checked;
 * unknown namespace versions fail closed. Reads OS loader metadata, never code. */
static int api_host(const WCHAR* importer,const WCHAR* requested,size_t rn,WCHAR host[260],uint32_t* error) {
    const unsigned char* peb=(const unsigned char*)(uintptr_t)__readgsqword(0x60);
    const unsigned char* ns=*(const unsigned char* const*)(peb+0x68);
    if(!ns||word(ns,0)!=6) { *error=ERROR_NOT_SUPPORTED;return 0; }
    uint32_t size=word(ns,4);if(size<28||size>16*1024*1024){*error=ERROR_INVALID_DATA;return 0;}
    size_t required=0;uint32_t result=sfi_api_host_v6(ns,size,(const uint16_t*)importer,length(importer,260),(const uint16_t*)requested,rn,(uint16_t*)host,260,&required);
    if(result==SFI_API_RESOLVED&&required<=260)return 1;
    *error=result==SFI_API_NOT_FOUND||result==SFI_API_ABSENT?ERROR_MOD_NOT_FOUND:result==SFI_API_UNSUPPORTED?ERROR_NOT_SUPPORTED:ERROR_INVALID_DATA;return 0;
}
uint32_t sfi_system_path(const uint16_t* importer,const uint16_t* requested,uint16_t* output,uint32_t capacity,uint32_t* count,uint32_t* error) {
    *error=0;*count=0;
    __try {
        const WCHAR* raw=(const WCHAR*)requested;size_t rn=length(raw,260);WCHAR host[260];
        if(!component(raw,rn)) { *error=ERROR_INVALID_NAME;return 0; }
        if(api_name(raw,rn)) { if(!api_host((const WCHAR*)importer,raw,rn,host,error))return 0; }
        else { CopyMemory(host,raw,rn*2);host[rn]=0; }
        UINT n=GetSystemDirectoryW((WCHAR*)output,capacity);size_t hn=length(host,260);
        if(!n||n>=capacity||hn>=260||n+hn+2>capacity) { *error=ERROR_INSUFFICIENT_BUFFER;return 0; }
        output[n++]=L'\\';CopyMemory(output+n,host,(hn+1)*2);*count=n+(uint32_t)hn;return 0;
    } __except(sfi_callback_filter((uint32_t)GetExceptionCode())) { return (uint32_t)GetExceptionCode(); }
}

#define MAX_WATCHES 512
#define MAX_ALTERNATIVES 8
/* A watch already holds MAX_ALTERNATIVES distinct identical files of one name (a ninth package ships its own copy):
 * the load is refused before it starts, with *failed naming that binding. Bit 29 (customer code): never a Windows
 * error, so no loader failure reads as this one. windows.rs TOO_MANY_COPIES. */
#define SFI_TOO_MANY_COPIES 0x20000008u
#define MAX_LAZY_OWNERS 32
/* lazy_owners: every package that delay-imports this name (a system watch is shared by all of them, owner 0).
 * lazy_count above MAX_LAZY_OWNERS means "more than recorded": a shadow then stops every package. */
typedef struct DelayWatch {
    uint64_t owner,transaction;uint32_t package,lazy,entry;WCHAR name[260];uint8_t hash[32];
    HANDLE retained[MAX_ALTERNATIVES];void* views[MAX_ALTERNATIVES];uint32_t alternatives;
    uint64_t lazy_owners[MAX_LAZY_OWNERS];_Atomic uint32_t lazy_count;
} DelayWatch;
static DelayWatch watches[MAX_WATCHES];
static _Atomic uint32_t watch_count;
#define MAX_PACKAGE_IMAGES 4096
typedef struct PackageImage { SfiPinState pin;uint64_t owner;void* view;WCHAR name[260];uint8_t hash[32]; } PackageImage;
static PackageImage package_images[MAX_PACKAGE_IMAGES];
static _Atomic uint32_t package_image_count;
/* One SFI load window, not a process-wide loader lock. Callback-visible state
 * is process lifetime; publication and close happen under short loader holds. */
static _Atomic uint64_t next_transaction,current_transaction,current_owner;
static _Atomic uint32_t current_seen,current_poisoned;
static _Atomic int32_t current_bad_pin;
static int hash_equal(const uint8_t* a,const uint8_t* b) { for(size_t i=0;i<32;i++)if(a[i]!=b[i])return 0;return 1; }
/* Read-only x64 Windows loader metadata, under the notification's loader lock.
 * Avoid K32/PSAPI callbacks, path opens and large stack arrays in the notifier. */
static void check_active_namespace(void){
    uint64_t transaction=atomic_load(&current_transaction);if(!transaction||transaction==UINT64_MAX)return;
    const unsigned char* peb=(const unsigned char*)(uintptr_t)__readgsqword(0x60);const unsigned char* ldr=*(const unsigned char* const*)(peb+0x18);if(!ldr)return;
    const LIST_ENTRY* head=(const LIST_ENTRY*)(ldr+0x10);const LIST_ENTRY* link=head->Flink;uint32_t visited=0;
    while(link&&link!=head&&visited++<4096){const unsigned char* entry=(const unsigned char*)link;void* module=*(void* const*)(entry+0x30);USHORT bytes=*(const USHORT*)(entry+0x58);const WCHAR* name=*(const WCHAR* const*)(entry+0x60);
        if(bytes%2||bytes>520||!name){atomic_store(&current_poisoned,1);return;}
        uint32_t count=atomic_load(&watch_count);for(uint32_t i=0;i<count;i++){DelayWatch* w=&watches[i];if(w->transaction!=transaction||!equal(w->name,length(w->name,260),name,bytes/2))continue;
            uint32_t error=0;int matched=0;for(uint32_t j=0;j<w->alternatives;j++){if(same_view(module,w->views[j],&error)){matched=1;break;}if(error)break;}
            /* The same verified-copy predicate as admission (accepted()): another loader's identical file. */
            if(!matched&&!error){uint32_t nc=0;const SfiMappedProof* copies=sfi_copy_window_current(&nc);
                for(uint32_t k=0;copies&&k<nc;k++)if((!copies[k].module||copies[k].module==module)&&hash_equal(copies[k].sha256,w->hash)){if(same_view(module,copies[k].view,&error)){matched=1;break;}if(error)break;}}
            if(!matched){atomic_store(&current_poisoned,1);sfi_report_native_breach(error?2:3,(const uint16_t*)w->name,(uint32_t)length(w->name,260),atomic_load(&current_owner));}
        }link=link->Flink;
    }if(visited>=4096)atomic_store(&current_poisoned,1);
}
static void pin_package_image(DelayWatch* w,void* module,void* view,uint64_t cause){
    uint32_t count=atomic_load(&package_image_count);
    uint64_t active=atomic_load(&current_transaction);int in_window=active&&active!=UINT64_MAX&&w->transaction==active;
    if(in_window)atomic_store(&current_seen,1);
    for(uint32_t i=0;i<count;i++)if(atomic_load(&package_images[i].pin.module)==(uintptr_t)module){uint32_t error=0;if(same_view(module,package_images[i].view,&error)){if(sfi_pin_failed(&package_images[i].pin))sfi_report_native_breach(5,(const uint16_t*)w->name,(uint32_t)length(w->name,260),cause);return;}}
    if(count>=MAX_PACKAGE_IMAGES){sfi_report_native_breach(2,(const uint16_t*)w->name,(uint32_t)length(w->name,260),0);return;}
    PinImage pin=atomic_load(&pin_function);LONG status=pin?pin(1 /* LDR_ADDREF_DLL_PIN */,module):(LONG)0xc0000002u;
    PackageImage* image=&package_images[count];image->owner=in_window?atomic_load(&current_owner):(cause?cause:w->owner);image->view=view;CopyMemory(image->name,w->name,sizeof(w->name));CopyMemory(image->hash,w->hash,32);
    sfi_pin_loaded(&image->pin,(uintptr_t)module,status,in_window?active:0);atomic_store(&package_image_count,count+1);
    if(status<0){current_bad_pin=status;sfi_report_native_breach(6,(const uint16_t*)w->name,(uint32_t)length(w->name,260),image->owner);}
}
/* Written under the loader hold (register_watches); read by LOADED callbacks under the loader lock. */
static void add_lazy_owner(DelayWatch* w,uint64_t owner){
    uint32_t n=atomic_load(&w->lazy_count);
    for(uint32_t i=0;i<n&&i<MAX_LAZY_OWNERS;i++)if(w->lazy_owners[i]==owner)return;
    if(n>=MAX_LAZY_OWNERS){atomic_store(&w->lazy_count,MAX_LAZY_OWNERS+1);return;}
    w->lazy_owners[n]=owner;atomic_store(&w->lazy_count,n+1);
}
/* A loaded module shadows a lazy (delay-import) watch outside a load window: the delay loads of exactly the packages
 * that delay-import this name would bind to it, so those packages stop (kind 3). The package whose thread loaded it
 * stops too (kind 4) when it is not one of them. Packages that import the name ordinarily were bound to the proved
 * image when they loaded, and later loads refuse the shadow in their own window scan, so they keep running. Unknown
 * importers (none recorded or more than recorded) stop every package, as before (owner 0). */
static void lazy_shadowed(DelayWatch* w,const uint16_t* name,uint32_t count,uint64_t cause){
    uint32_t n=atomic_load(&w->lazy_count);
    if(!n||n>MAX_LAZY_OWNERS){sfi_report_native_breach(3,name,count,0);return;}
    int listed=0;
    for(uint32_t i=0;i<n;i++){sfi_report_native_breach(3,name,count,w->lazy_owners[i]);listed|=w->lazy_owners[i]==cause;}
    if(cause&&!listed)sfi_report_native_breach(4,name,count,cause);
}
static void finish_transaction(uint64_t transaction,uint32_t success){
    uint32_t count=atomic_load(&package_image_count);
    for(uint32_t i=0;i<count;i++)if(atomic_load(&package_images[i].pin.transaction)==transaction){sfi_pin_finish(&package_images[i].pin,transaction,success);if(!success)sfi_report_native_breach(5,(const uint16_t*)package_images[i].name,(uint32_t)length(package_images[i].name,260),package_images[i].owner);}
}
void sfi_native_call_complete(uint64_t owner,uint32_t success){
    /* Pending lazy images have transaction zero; a native shim's terminal result
     * supplies the missing post-DllMain evidence. Fields are atomic for readers. */
    uint32_t count=atomic_load(&package_image_count);
    for(uint32_t i=0;i<count;i++)if(package_images[i].owner==owner&&(!success||atomic_load(&package_images[i].pin.transaction)==0)&&atomic_load(&package_images[i].pin.state)==SFI_IMAGE_LOADED){sfi_pin_finish(&package_images[i].pin,atomic_load(&package_images[i].pin.transaction),success);if(!success)sfi_report_native_breach(5,(const uint16_t*)package_images[i].name,(uint32_t)length(package_images[i].name,260),owner);}
}
uint32_t sfi_image_quarantined(void* module,void* view,const uint8_t* hash,uint32_t* quarantined,uint64_t* owner,int32_t* status,uint32_t* error){
    *quarantined=0;*owner=0;*status=0;*error=0;
    __try {uint32_t count=atomic_load(&package_image_count);for(uint32_t i=0;i<count;i++){PackageImage* image=&package_images[i];if(!sfi_pin_failed(&image->pin))continue;
        if((hash&&hash_equal(hash,image->hash))||(module&&same_view(module,image->view,error))||(view&&same_view(view,image->view,error))){*quarantined=1;*owner=image->owner;*status=atomic_load(&image->pin.pin_status);return 0;}if(*error)return 0;}return 0;}
    __except(sfi_callback_filter((uint32_t)GetExceptionCode())){return (uint32_t)GetExceptionCode();}
}
static int add_alternative(DelayWatch* watch,const SfiBinding* binding,uint32_t* error) {
    for(uint32_t i=0;i<watch->alternatives;i++) { if(same_view(watch->views[i],binding->view,error))return 1;if(*error)return 0; }
    if(watch->alternatives>=MAX_ALTERNATIVES) { *error=SFI_TOO_MANY_COPIES;return 0; }
    HANDLE retained=NULL;void* view=NULL;
    if(!DuplicateHandle(GetCurrentProcess(),(HANDLE)binding->file,GetCurrentProcess(),&retained,0,FALSE,DUPLICATE_SAME_ACCESS)) { *error=GetLastError();return 0; }
    uint32_t fault=sfi_image_view(retained,&view,error);
    if(fault||*error) { if(fault)*error=fault;CloseHandle(retained);return 0; }
    uint32_t n=watch->alternatives;watch->retained[n]=retained;watch->views[n]=view;watch->alternatives=n+1;return 1;
}
/* Called only by the OS loader notification, already under its lock. Views and
 * files were prepared beforehand. No Rust, allocation, path opens or waits. */
void sfi_check_delay_mapping(const uint16_t* name,uint32_t count,void* module,uint64_t cause) {
    uint32_t begin=0;for(uint32_t i=0;i<count;i++)if(name[i]=='\\'||name[i]=='/')begin=i+1;
    while(count>begin&&(name[count-1]=='.'||name[count-1]==' '))count--;
    name+=begin;count-=begin;
    uint32_t wc=atomic_load(&watch_count);
    /* The open window's own watches first: the image's pin record must carry the ACTIVE transaction, so its outcome
     * (finish_transaction) reaches it even when an older package's lazy watch for the same bytes also matches. */
    for(uint32_t pass=0;pass<2;pass++)
    for(uint32_t i=0;i<wc;i++) {
        DelayWatch* w=&watches[i];if(!equal(w->name,length(w->name,260),(const WCHAR*)name,count))continue;
        uint64_t active=atomic_load(&current_transaction);int in_window=active&&active!=UINT64_MAX&&w->transaction==active;
        if(in_window!=(pass==0))continue;
        uint32_t error=0;int matched=0;void* matching_view=NULL;
        for(uint32_t j=0;j<w->alternatives;j++) { if(same_view(module,w->views[j],&error)) { matched=1;matching_view=w->views[j];break; }if(error)break; }
        if(w->package&&(matched||in_window))pin_package_image(w,module,matched?matching_view:w->views[0],cause);
        if(!matched&&(w->lazy||in_window)) {
            if(in_window){atomic_store(&current_poisoned,1);sfi_report_native_breach(error?2:3,name,count,atomic_load(&current_owner));}
            else if(error)sfi_report_native_breach(2,name,count,0); /* the mapping proof itself failed: observer breach */
            else lazy_shadowed(w,name,count,cause); /* O2: only the packages that delay-import this name */
        }
        if(in_window&&w->entry)check_active_namespace();
    }
}
static int register_watches(const SfiBinding* bs,uint32_t n,uint64_t owner,uint32_t* failed,uint32_t* error) {
    uint32_t wc=atomic_load(&watch_count);
    for(uint32_t i=0;i<n;i++) {
        const SfiBinding* b=&bs[i];size_t bn=length((const WCHAR*)b->name,260);
        for(uint32_t j=0;j<wc;j++) if(equal(watches[j].name,length(watches[j].name,260),(const WCHAR*)b->name,bn)) {
            if(!hash_equal(watches[j].hash,b->sha256)) { *error=ERROR_INVALID_DATA;return 0; }
            if(!add_alternative(&watches[j],b,error)) { if(*error==SFI_TOO_MANY_COPIES)*failed=i;return 0; }
        }
        uint64_t watch_owner=(b->flags&1)?0:owner;
        int duplicate=0;for(uint32_t j=0;j<wc;j++)if(watches[j].owner==watch_owner&&equal(watches[j].name,length(watches[j].name,260),(const WCHAR*)b->name,bn)){if(b->flags&4){watches[j].lazy=1;add_lazy_owner(&watches[j],owner);}watches[j].entry|=(b->flags&8)!=0;watches[j].transaction=atomic_load(&current_transaction);duplicate=1;break;}
        if(duplicate)continue;
        if(wc>=MAX_WATCHES||!component((const WCHAR*)b->name,bn)) { *error=ERROR_INSUFFICIENT_BUFFER;return 0; }
        DelayWatch* w=&watches[wc];w->owner=watch_owner;w->package=!(b->flags&1);w->lazy=(b->flags&4)!=0;w->entry=(b->flags&8)!=0;w->transaction=atomic_load(&current_transaction);CopyMemory(w->name,b->name,(bn+1)*2);CopyMemory(w->hash,b->sha256,32);w->alternatives=0;
        atomic_store(&w->lazy_count,0);if(w->lazy)add_lazy_owner(w,owner);
        if(!add_alternative(w,b,error))return 0;
        wc++;atomic_store(&watch_count,wc);
    }
    return 1;
}
/* A module its OS loader never initialized: Windows and Wine both list a DLL mapped with DONT_RESOLVE_DLL_REFERENCES
 * without resolving its imports or running DllMain, send no LOADED notice for it natively, and hand later ordinary
 * loads of that file the same uninitialized module (measured on native Windows). Such a module is
 * neither foreign-owned nor loadable. Nor is a pinned image whose DllMain failed. x64 LDR_DATA_TABLE_ENTRY: DllBase
 * +0x30, Flags +0x68; ImageDll 0x4 (native clears it for DONT_RESOLVE_DLL_REFERENCES), ProcessAttachCalled 0x80000 (Wine
 * LDR_PROCESS_ATTACHED; set for entry-less DLLs too) and ProcessAttachFailed 0x100000 (native: a DllMain that returned
 * FALSE leaves ProcessAttachCalled set too; measured on native Windows). Native Windows never marks ntdll
 * attached: the kernel initializes it, not DllMain.
 * Native Windows maps and snaps a DLL outside the loader lock, so a scan under that lock meets other threads' loads and
 * unloads mid-way: ImageDll set, neither attached nor failed (about half of all sightings of a DLL that other threads
 * load and free). The OS loader finishes those itself, so they are SFI_INIT_UNDER_WAY. Wine runs a whole
 * load or unload under its loader lock (no such sighting under Wine), so there an
 * unattached DLL was never initialized or its DllMain failed. */
#define SFI_NOT_INITIALIZED ERROR_INVALID_MODULETYPE
enum { SFI_INIT_NEVER, SFI_INIT_READY, SFI_INIT_UNDER_WAY };
static int init_state(void* module){
    if((uintptr_t)module==atomic_load(&ntdll_base))return SFI_INIT_READY;
    FindEntry find=atomic_load(&find_entry_function);void* entry=NULL;
    if(!find||find(module,&entry)<0||!entry||*(void* const*)((const unsigned char*)entry+0x30)!=module)return SFI_INIT_NEVER;
    ULONG flags=*(const ULONG*)((const unsigned char*)entry+0x68);
    if(flags&0x100000)return SFI_INIT_NEVER;
    if((flags&0x4)&&(flags&0x80000))return SFI_INIT_READY;
    return (flags&0x4)&&!atomic_load(&on_wine)?SFI_INIT_UNDER_WAY:SFI_INIT_NEVER;
}
static int failed_module(void* module,uint32_t* error){uint32_t count=atomic_load(&package_image_count);for(uint32_t i=0;i<count;i++)if(sfi_pin_failed(&package_images[i].pin)&&same_view(module,package_images[i].view,error))return 1;return 0;}
/* The mapping is proved; a pinned failed image, or one its loader never initialized, still refuses. The entry (flags 8)
 * may be handed back as foreign-owned and called, so it must be initialized already. A dependency whose load another
 * thread has under way binds once that load is done: the OS loader makes our load wait for it, and a failure there
 * fails our load. */
static int usable(void* module,const SfiBinding* binding,uint32_t* error){
    if(failed_module(module,error)){*error=ERROR_INVALID_STATE;return 0;}
    if(*error)return 0;
    int state=init_state(module);
    if(state==SFI_INIT_READY||(state==SFI_INIT_UNDER_WAY&&!(binding->flags&8)))return 1;
    *error=SFI_NOT_INITIALIZED;return 0;
}
static int accepted(void* module,const SfiBinding* binding,const SfiMappedProof* copies,uint32_t nc,uint32_t* error) {
    if(same_view(module,binding->view,error))return usable(module,binding,error);if(*error)return 0;
    for(uint32_t i=0;i<nc;i++)if((!copies[i].module||copies[i].module==module)&&hash_equal(copies[i].sha256,binding->sha256)&&same_view(module,copies[i].view,error))return usable(module,binding,error);
    return 0;
}
static int binding_modules(const SfiBinding* bs,uint32_t n,const SfiMappedProof* copies,uint32_t nc,void** entry,uint32_t* failed,uint32_t* error) {
    void* modules[4096];uint32_t count=0;uint32_t fault=sfi_monitor_modules(modules,4096,&count,error);if(fault||*error){if(fault)*error=fault;return 0;}
    for(uint32_t i=0;i<count;i++) {
        WCHAR path[32768];uint32_t pn=0;fault=sfi_monitor_module_path(modules[i],(uint16_t*)path,32768,&pn,error);if(fault||*error){if(fault)*error=fault;return 0;}
        size_t begin=0;for(size_t j=0;j<pn;j++)if(path[j]==L'\\'||path[j]==L'/')begin=j+1;
        while(pn>begin&&(path[pn-1]==L'.'||path[pn-1]==L' '))pn--;
        for(uint32_t j=0;j<n;j++) {
            const SfiBinding* b=&bs[j];int same_name=equal(path+begin,pn-begin,(const WCHAR*)b->name,length((const WCHAR*)b->name,260));
            if((b->flags&8)&&accepted(modules[i],b,copies,nc,error)) {*entry=modules[i];}
            if(*error){*failed=j;return 0;}
            if(same_name&&!accepted(modules[i],b,copies,nc,error)) {*failed=j;if(!*error)*error=ERROR_INVALID_DATA;return 0;}
        }
    }
    return 1;
}
uint32_t sfi_loaded_banned_mask(uint32_t* mask,uint32_t* error) {
    *mask=0;*error=0;
    __try {
        HMODULE nt=GetModuleHandleW(L"ntdll.dll");LockLoader lock=(LockLoader)GetProcAddress(nt,"LdrLockLoaderLock");UnlockLoader unlock=(UnlockLoader)GetProcAddress(nt,"LdrUnlockLoaderLock");
        if(!lock||!unlock){*error=ERROR_PROC_NOT_FOUND;return 0;}
        ULONG disposition=0;ULONG_PTR cookie_value=0;LONG status=lock(0,&disposition,&cookie_value);if(status<0||disposition!=1){*error=status<0?(uint32_t)status:ERROR_NOT_SUPPORTED;return 0;}
        __try {
            void* modules[4096];uint32_t count=0;uint32_t fault=sfi_monitor_modules(modules,4096,&count,error);if(fault||*error){if(fault)*error=fault;return 0;}
            for(uint32_t i=0;i<count;i++){uint16_t path[32768];uint32_t length=0;fault=sfi_monitor_module_path(modules[i],path,32768,&length,error);if(fault||*error){if(fault)*error=fault;return 0;}*mask|=sfi_breach_name_mask(path,length);}
            return 0;
        } __finally {unlock(0,cookie_value);}
    } __except(sfi_callback_filter((uint32_t)GetExceptionCode())){return (uint32_t)GetExceptionCode();}
}
typedef struct LoadContext {
    const SfiBinding* bindings;uint32_t count;const SfiMappedProof* copies;uint32_t copy_count;
    const SfiApiBinding* apis;uint32_t api_count;uint64_t owner,transaction;
    void** output;uint32_t* foreign;uint32_t* failed;uint32_t* error;
    LockLoader lock;UnlockLoader unlock;ULONG_PTR cookie;uint32_t locked;void* existing;
} LoadContext;
static uint32_t window_lock(void* raw){LoadContext* c=raw;ULONG disposition=0;LONG status=c->lock(0,&disposition,&c->cookie);if(status<0||disposition!=1){*c->error=status<0?(uint32_t)status:ERROR_NOT_SUPPORTED;return 0;}c->locked=1;return 1;}
static void window_unlock(void* raw){LoadContext* c=raw;if(c->locked){c->locked=0;c->unlock(0,c->cookie);}}
static uint32_t window_observe(void* raw){LoadContext* c=raw;
    if(sfi_breach_flags()!=0){*c->error=ERROR_NOT_READY;return 0;}
    for(uint32_t i=0;i<c->api_count;i++){WCHAR path[32768];uint32_t count=0;uint32_t fault=sfi_system_path(c->apis[i].importer,c->apis[i].requested,(uint16_t*)path,32768,&count,c->error);if(fault||*c->error){if(fault)*c->error=fault;return 0;}size_t begin=0;for(size_t j=0;j<count;j++)if(path[j]==L'\\'||path[j]==L'/')begin=j+1;if(!equal(path+begin,count-begin,(const WCHAR*)c->apis[i].host,length((const WCHAR*)c->apis[i].host,260))){*c->error=ERROR_INVALID_DATA;return 0;}}
    for(uint32_t i=0;i<c->count;i++)for(uint32_t j=0;j<i;j++)if(equal((const WCHAR*)c->bindings[i].name,length((const WCHAR*)c->bindings[i].name,260),(const WCHAR*)c->bindings[j].name,length((const WCHAR*)c->bindings[j].name,260))&&!hash_equal(c->bindings[i].sha256,c->bindings[j].sha256)){*c->failed=i;*c->error=ERROR_INVALID_DATA;return 0;}
    /* kiln-sfi's own copy of admission's verified copies (copy_window.c): the closing branch that cannot take the
     * hold clears it without leaving a callback reading the caller's list. */
    if(!sfi_copy_window_publish(c->copies,c->copy_count)){*c->error=ERROR_INVALID_PARAMETER;return 0;}
    atomic_store(&current_owner,c->owner);atomic_store(&current_seen,0);atomic_store(&current_bad_pin,0);atomic_store(&current_poisoned,0);atomic_store(&current_transaction,c->transaction);
    if(!register_watches(c->bindings,c->count,c->owner,c->failed,c->error)){atomic_store(&current_transaction,UINT64_MAX);return 0;}
    /* The immutable names/views are published BEFORE the first namespace scan. */
    atomic_thread_fence(memory_order_release);return 1;
}
static uint32_t window_scan(void* raw){LoadContext* c=raw;if(atomic_load(&current_poisoned)||sfi_breach_flags()!=0){*c->error=ERROR_INVALID_STATE;return 0;}c->existing=NULL;return (uint32_t)binding_modules(c->bindings,c->count,c->copies,c->copy_count,&c->existing,c->failed,c->error);}
static uint32_t window_preload(void* raw){LoadContext* c=raw;if(c->existing){*c->output=c->existing;*c->foreign=1;return 1;}
    for(uint32_t i=0;i<c->count;i++)if((c->bindings[i].flags&3)==3){HMODULE module=LoadLibraryExW((const WCHAR*)c->bindings[i].path,NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!module){*c->failed=i;*c->error=GetLastError();return 0;}if(!accepted(module,&c->bindings[i],c->copies,c->copy_count,c->error)){*c->failed=i;if(!*c->error)*c->error=ERROR_INVALID_DATA;return 0;}if(atomic_load(&current_poisoned)||sfi_breach_flags()!=0){*c->error=ERROR_INVALID_STATE;return 0;}}
    return 1;
}
static uint32_t window_load(void* raw){LoadContext* c=raw;if(c->existing){*c->output=c->existing;*c->foreign=1;return 1;}uint32_t root=UINT32_MAX;for(uint32_t i=0;i<c->count;i++)if(c->bindings[i].flags&8){root=i;break;}if(root==UINT32_MAX){*c->error=ERROR_INVALID_DATA;return 0;}
    HMODULE module=LoadLibraryExW((const WCHAR*)c->bindings[root].path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);if(!module){*c->failed=root;*c->error=GetLastError();return 0;}*c->output=module;
    if(!accepted(module,&c->bindings[root],c->copies,c->copy_count,c->error)){*c->failed=root;if(!*c->error)*c->error=ERROR_INVALID_DATA;return 0;}return 1;
}
uint32_t sfi_verified_load(const SfiBinding* bs,uint32_t n,const SfiMappedProof* copies,uint32_t nc,const SfiApiBinding* apis,uint32_t na,uint64_t owner,uint64_t observed,uint64_t epoch,void** output,uint32_t* foreign,uint32_t* may_have_run,int32_t* pin_status,uint32_t* failed,uint32_t* error){
    *output=NULL;*foreign=0;*may_have_run=0;*pin_status=0;*failed=UINT32_MAX;*error=0;(void)observed;(void)epoch;
    LoadContext c={0};c.error=error;volatile uint32_t caught_exception=0;uint32_t completed=0;uint64_t idle=0;
    if(!atomic_compare_exchange_strong(&current_transaction,&idle,UINT64_MAX)){*error=ERROR_BUSY;return 0;}
    __try{
        __try{
            if(!n||n>1024||nc>SFI_COPY_WINDOW_MAX||na>16384||!same_api()||!pin_api()){*error=ERROR_INVALID_PARAMETER;return 0;}
            HMODULE nt=GetModuleHandleW(L"ntdll.dll");
            FindEntry find=nt?(FindEntry)GetProcAddress(nt,"LdrFindEntryForAddress"):NULL;if(!find){*error=ERROR_PROC_NOT_FOUND;return 0;}
            atomic_store(&ntdll_base,(uintptr_t)nt);atomic_store(&find_entry_function,find);atomic_store(&on_wine,GetProcAddress(nt,"wine_get_version")!=NULL);c.lock=(LockLoader)GetProcAddress(nt,"LdrLockLoaderLock");c.unlock=(UnlockLoader)GetProcAddress(nt,"LdrUnlockLoaderLock");if(!c.lock||!c.unlock){*error=ERROR_PROC_NOT_FOUND;return 0;}
            c.bindings=bs;c.count=n;c.copies=copies;c.copy_count=nc;c.apis=apis;c.api_count=na;c.owner=owner;c.transaction=atomic_fetch_add(&next_transaction,1)+1;c.output=output;c.foreign=foreign;c.failed=failed;c.error=error;
            SfiLoadWindowOps ops={&c,window_lock,window_unlock,window_observe,window_scan,window_preload,window_load};completed=sfi_run_load_window(&ops);return 0;
        }__finally{
            /* A short hold closes callback observation before caller buffers can
             * disappear. NO LoadLibrary occurs under this hold. The copy window is
             * cleared BEFORE current_transaction reopens the gate: without the hold, a
             * newer window could otherwise publish in between and lose its proofs. */
            window_unlock(&c);if(c.lock&&c.unlock&&window_lock(&c)){*may_have_run=atomic_load(&current_seen);*pin_status=atomic_load(&current_bad_pin);sfi_copy_window_clear();atomic_store(&current_owner,0);atomic_store(&current_transaction,0);window_unlock(&c);}else{sfi_copy_window_clear();atomic_store(&current_transaction,0);}
            if(!caught_exception){sfi_after_load_error(*error);if(c.transaction)finish_transaction(c.transaction,completed&&*output&&!*error&&*pin_status>=0);}
        }
    }__except((caught_exception=(uint32_t)GetExceptionCode(),sfi_fault_filter(caught_exception))){return sfi_handle_fault((uint32_t)GetExceptionCode());}
}
#else
uint32_t sfi_mapped_header_matches(void* m,const uint8_t* d,size_t n,uint32_t* same){(void)m;(void)d;(void)n;*same=0;return 0;}
uint32_t sfi_image_quarantined(void* m,void* v,const uint8_t* h,uint32_t* q,uint64_t* o,int32_t* p,uint32_t* e){(void)m;(void)v;(void)h;*q=0;*o=0;*p=0;*e=50;return 0;}
void sfi_native_call_complete(uint64_t o,uint32_t s){(void)o;(void)s;}
uint32_t sfi_loaded_banned_mask(uint32_t* m,uint32_t* e){*m=0;*e=50;return 0;}
uint32_t sfi_image_view(void* f,void** v,uint32_t* e){(void)f;*v=NULL;*e=50;return 0;}
uint32_t sfi_image_unmap(void* v){(void)v;return 0;}
uint32_t sfi_same_mapping(void* m,void* v,uint32_t* s,uint32_t* e){(void)m;(void)v;*s=0;*e=50;return 0;}
uint32_t sfi_system_path(const uint16_t* i,const uint16_t* r,uint16_t* p,uint32_t c,uint32_t* n,uint32_t* e){(void)i;(void)r;(void)p;(void)c;*n=0;*e=50;return 0;}
void sfi_check_delay_mapping(const uint16_t* n,uint32_t c,void* m,uint64_t o){(void)n;(void)c;(void)m;(void)o;}
uint32_t sfi_verified_load(const SfiBinding* b,uint32_t n,const SfiMappedProof* p,uint32_t nc,const SfiApiBinding* a,uint32_t na,uint64_t o,uint64_t s,uint64_t epoch,void** m,uint32_t* f,uint32_t* ran,int32_t* pin,uint32_t* failed,uint32_t* e){(void)b;(void)n;(void)p;(void)nc;(void)a;(void)na;(void)o;(void)s;(void)epoch;*m=NULL;*f=0;*ran=0;*pin=0;*failed=UINT32_MAX;*e=50;return 0;}
#endif
