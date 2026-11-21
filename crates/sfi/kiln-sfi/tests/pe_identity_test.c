/* SPDX-License-Identifier: MIT - structural synthetic PE headers. */
#include "pe_identity.h"
#include <stdio.h>
#include <string.h>
static void put(uint8_t* p,size_t at,uint32_t value){memcpy(p+at,&value,4);}
int main(void){uint8_t a[512]={0},b[512];a[0]='M';a[1]='Z';put(a,0x3c,0x80);memcpy(a+0x80,"PE\0\0",4);a[0x84]=0x64;a[0x85]=0x86;a[0x86]=1;a[0x94]=240;a[0x98]=0x0b;a[0x99]=2;put(a,0x98+56,0x2000);memcpy(b,a,sizeof(a));if(!sfi_pe_headers_equal(a,512,b,512))return 1;size_t changes[]={0x88,0x98+56,0x98+64,0x188};for(size_t i=0;i<4;i++){b[changes[i]]^=1;if(sfi_pe_headers_equal(a,512,b,512))return 1;b[changes[i]]^=1;}if(sfi_pe_headers_equal(a,1,b,512))return 1;put(b,0x3c,0xffffff00);if(sfi_pe_headers_equal(a,512,b,512))return 1;puts("PASS PE structural prefilter timestamp/image/checksum/sections/bounds");return 0;}
