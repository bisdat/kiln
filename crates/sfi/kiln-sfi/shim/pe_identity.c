/* SPDX-License-Identifier: MIT - structural prefilter only, never identity proof. */
#include "pe_identity.h"
#include <string.h>
static uint32_t u32(const uint8_t* p,size_t o){uint32_t v;memcpy(&v,p+o,4);return v;}
static uint16_t u16(const uint8_t* p,size_t o){uint16_t v;memcpy(&v,p+o,2);return v;}
static int header(const uint8_t* p,size_t n,uint32_t* at,uint16_t* sections,uint16_t* optional){
 if(!p||n<64||p[0]!='M'||p[1]!='Z')return 0;
 *at=u32(p,0x3c);
 if(*at>n||n-*at<24||memcmp(p+*at,"PE\0\0",4))return 0;
 *sections=u16(p,*at+6);*optional=u16(p,*at+20);return *sections>0&&*sections<=96&&*optional>=112&&*at+24u+*optional<=n&&*sections*40u<=n-(*at+24u+*optional);
}
uint32_t sfi_pe_headers_equal(const uint8_t* a,size_t an,const uint8_t* b,size_t bn){
 uint32_t x,y;uint16_t xs,ys,xo,yo;if(!header(a,an,&x,&xs,&xo)||!header(b,bn,&y,&ys,&yo))return 0;
 if(xs!=ys||xo!=yo||u16(a,x+4)!=u16(b,y+4)||u32(a,x+8)!=u32(b,y+8)||u16(a,x+24)!=u16(b,y+24)||u32(a,x+24+56)!=u32(b,y+24+56)||u32(a,x+24+64)!=u32(b,y+24+64))return 0;
 return memcmp(a+x+24+xo,b+y+24+yo,xs*40u)==0;
}
