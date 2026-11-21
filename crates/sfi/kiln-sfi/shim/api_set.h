/* SPDX-License-Identifier: MIT */
#ifndef SFI_API_SET_H
#define SFI_API_SET_H
#include <stdint.h>
#include <stddef.h>
#define SFI_API_NOT_FOUND 0u
#define SFI_API_RESOLVED 1u
#define SFI_API_ABSENT 2u
#define SFI_API_MALFORMED 3u
#define SFI_API_UNSUPPORTED 4u
uint32_t sfi_api_host_v6(const uint8_t* ns,size_t length,const uint16_t* importer,size_t importer_length,const uint16_t* requested,size_t requested_length,uint16_t* host,size_t capacity,size_t* required);
#endif
