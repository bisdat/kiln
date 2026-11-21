/* SPDX-License-Identifier: MIT */
#ifndef SFI_MONITOR_H
#define SFI_MONITOR_H
#include <stdint.h>
#include <stddef.h>
typedef struct SfiBreach {
    uint64_t sequence, owner;
    uint32_t kind, reserved;
    uint16_t module[64];
} SfiBreach;
typedef struct SfiFileId { uint64_t volume, index, size; } SfiFileId;
uint32_t sfi_monitor_start(uint32_t* error);
uint32_t sfi_monitor_scan(uint32_t* error);
uint32_t sfi_monitor_poll(uint64_t after, SfiBreach* event, uint32_t* state);
void sfi_monitor_owner(uint64_t owner);
uint32_t sfi_monitor_modules(void** modules, uint32_t capacity, uint32_t* count, uint32_t* error);
uint32_t sfi_monitor_module_path(void* module, uint16_t* path, uint32_t capacity, uint32_t* count, uint32_t* error);
uint32_t sfi_monitor_file_id(void* file, SfiFileId* id, uint32_t* error);
/* Own read-only image views; never code loads, never DllMain. */
uint32_t sfi_image_view(void* file, void** view, uint32_t* error);
uint32_t sfi_image_unmap(void* view);
uint32_t sfi_same_mapping(void* module, void* view, uint32_t* same, uint32_t* error);
uint32_t sfi_system_path(const uint16_t* importer, const uint16_t* requested, uint16_t* path, uint32_t capacity, uint32_t* count, uint32_t* error);
uint32_t sfi_loaded_banned_mask(uint32_t* mask, uint32_t* error);
/* flags: system=1, ordinary-reachable=2, lazy-reachable=4, entry=8. */
typedef struct SfiBinding {
    const uint16_t *path, *name;
    void *file, *view;
    uint32_t flags, reserved;
    uint8_t sha256[32];
} SfiBinding;
typedef struct SfiMappedProof { void *module, *view; uint8_t sha256[32]; } SfiMappedProof;
typedef struct SfiApiBinding { const uint16_t *importer, *requested, *host; } SfiApiBinding;
uint32_t sfi_verified_load(const SfiBinding* bindings, uint32_t count, const SfiMappedProof* copies, uint32_t copy_count, const SfiApiBinding* apis, uint32_t api_count, uint64_t owner, uint64_t observed_sequence, uint64_t module_epoch, void** module, uint32_t* foreign, uint32_t* may_have_run, int32_t* pin_status, uint32_t* failed_binding, uint32_t* error);
/* Internal C-only callback bridge; no Rust callback under loader lock. */
void sfi_check_delay_mapping(const uint16_t* name, uint32_t length, void* module, uint64_t cause_owner);
void sfi_report_native_breach(uint32_t kind, const uint16_t* name, uint32_t length, uint64_t owner);
uint64_t sfi_monitor_sequence(void);
uint64_t sfi_monitor_epoch(void);
uint64_t sfi_monitor_current_owner(void);
uint32_t sfi_image_quarantined(void* module, void* view, const uint8_t* sha256, uint32_t* quarantined, uint64_t* owner, int32_t* pin_status, uint32_t* error);
void sfi_native_call_complete(uint64_t owner, uint32_t success);
#endif
