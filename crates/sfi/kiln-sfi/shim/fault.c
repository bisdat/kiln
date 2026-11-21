/* SPDX-License-Identifier: MIT
 * Direct typed foreign calls only. No Rust thunk between the SEH frame and DLL.
 * No C++ objects/unwinding. A native fault is not proof memory is uncorrupted.
 */
#include "fault.h"
#include "monitor.h"
#include "seh_guard.h"
#ifdef _WIN32
#include <windows.h>
#define SFI_TRY __try
#define SFI_EXCEPT __except(sfi_fault_filter((uint32_t)GetExceptionCode()))
#define SFI_CODE sfi_handle_fault((uint32_t)GetExceptionCode())
_Static_assert(sizeof(wchar_t) == sizeof(uint16_t), "Windows UTF-16 code unit");
#else
/* Host tests exercise signature forwarding only, NOT Windows fault recovery. */
#define SFI_TRY if (1)
#define SFI_EXCEPT else
#define SFI_CODE ((uint32_t)0)
#endif

uint32_t sfi_seh_init(SfiModInit fn, const struct ShroudforgeModHost* host, int32_t* out) {
    *out = 0;
    SFI_TRY { *out = fn(host); sfi_native_call_complete(sfi_monitor_current_owner(),*out!=0); return 0; }
    SFI_EXCEPT { return SFI_CODE; }
}
uint32_t sfi_seh_native(SfiNativeCall fn, void* context, const struct ShroudforgeNativeValue* args,
                        size_t count, struct ShroudforgeNativeResults* results) {
    SFI_TRY { fn(context, args, count, results); sfi_native_call_complete(sfi_monitor_current_owner(),1); return 0; }
    SFI_EXCEPT { return SFI_CODE; }
}
uint32_t sfi_seh_load(const uint16_t* path, void** module, uint32_t* error) {
    *module = NULL;
    *error = 0;
#ifdef _WIN32
    /* Matches SF 1.7.9 native_dlls.rs:383-388 and 1.8.0:397-402.
     * Contract evidence, not copied loader implementation. DllMain can fault
     * under the OS loader lock. Caller quarantines on nonzero exception code;
     * no FreeLibrary path exists, even after partial init or loader faults.
     */
    SFI_TRY {
        *module = (void*)LoadLibraryExW((const wchar_t*)path, NULL,
                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!*module) *error = (uint32_t)GetLastError();
        sfi_after_load_error(*error);
        return 0;
    }
    SFI_EXCEPT { return SFI_CODE; }
#else
    (void)path;
    *error = 50; /* ERROR_NOT_SUPPORTED */
    return 0;
#endif
}
