/* SPDX-License-Identifier: MIT */
#ifndef SFI_PE_IDENTITY_H
#define SFI_PE_IDENTITY_H
#include <stdint.h>
#include <stddef.h>
uint32_t sfi_pe_headers_equal(const uint8_t* mapped,size_t mapped_length,const uint8_t* disk,size_t disk_length);
#endif
