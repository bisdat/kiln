/* SPDX-License-Identifier: MIT - bounded OS metadata parser, no Windows calls. */
#include "api_set.h"
#include <string.h>
static uint32_t u32(const uint8_t* p,size_t o){uint32_t v;memcpy(&v,p+o,4);return v;}
static uint16_t u16(const uint8_t* p,size_t o){return (uint16_t)(p[o]|((uint16_t)p[o+1]<<8));}
static int range(size_t n,uint32_t o,uint32_t len){return o<=n&&len<=n-o;}
static uint16_t lower(uint16_t c){return c>='A'&&c<='Z'?(uint16_t)(c+32):c;}
static int equal(const uint8_t* ns,uint32_t at,size_t n,const uint16_t* value,size_t vn){if(n!=vn)return 0;for(size_t i=0;i<n;i++)if(lower(u16(ns,at+i*2))!=lower(value[i]))return 0;return 1;}
static size_t stem(const uint16_t* value,size_t n){if(n>=4&&lower(value[n-4])=='.'&&lower(value[n-3])=='d'&&lower(value[n-2])=='l'&&lower(value[n-1])=='l')return n-4;return n;}
uint32_t sfi_api_host_v6(const uint8_t* ns,size_t length,const uint16_t* importer,size_t in,const uint16_t* requested,size_t rn,uint16_t* host,size_t capacity,size_t* required){
    *required=0;if(!ns||length<28)return SFI_API_MALFORMED;if(u32(ns,0)!=6)return SFI_API_UNSUPPORTED;
    uint32_t size=u32(ns,4),count=u32(ns,12),entries=u32(ns,16);if(size>length||size<28||size>16*1024*1024||count>8192||entries%4||!range(size,entries,count*24))return SFI_API_MALFORMED;
    rn=stem(requested,rn);in=stem(importer,in);
    size_t prefix=rn;while(prefix&&requested[prefix-1]!='-')prefix--;if(!prefix)return SFI_API_NOT_FOUND;prefix--;
    for(uint32_t i=0;i<count;i++){
        uint32_t at=entries+i*24,no=u32(ns,at+4),nl=u32(ns,at+8),hashed=u32(ns,at+12);
        if(no%2||nl%2||hashed%2||hashed>nl||!range(size,no,nl))return SFI_API_MALFORMED;
        if(!hashed||!equal(ns,no,hashed/2,requested,prefix))continue;
        uint32_t values=u32(ns,at+16),vc=u32(ns,at+20),chosen=UINT32_MAX;
        if(!vc||vc>64||values%4||!range(size,values,vc*20))return SFI_API_MALFORMED;
        for(uint32_t j=0;j<vc;j++){uint32_t va=values+j*20,ao=u32(ns,va+4),al=u32(ns,va+8);if(ao%2||al%2||!range(size,ao,al))return SFI_API_MALFORMED;
            if(!al&&chosen==UINT32_MAX)chosen=va;
            size_t alias=al/2;if(alias>=4&&u16(ns,ao+(alias-4)*2)=='.'&&lower(u16(ns,ao+(alias-3)*2))=='d'&&lower(u16(ns,ao+(alias-2)*2))=='l'&&lower(u16(ns,ao+(alias-1)*2))=='l')alias-=4;
            if(al&&equal(ns,ao,alias,importer,in)){chosen=va;break;}
        }
        if(chosen==UINT32_MAX)return SFI_API_NOT_FOUND;
        uint32_t ho=u32(ns,chosen+12),hl=u32(ns,chosen+16);if(ho%2||hl%2||!range(size,ho,hl)||hl/2>=260)return SFI_API_MALFORMED;
        if(!hl)return SFI_API_ABSENT;
        for(size_t j=0;j<hl/2;j++){uint16_t c=u16(ns,ho+j*2);if(c<33||c>126||c=='/'||c=='\\'||c==':')return SFI_API_MALFORMED;}
        *required=hl/2+1;if(!host||capacity<*required)return SFI_API_RESOLVED;
        for(size_t j=0;j<hl/2;j++){host[j]=u16(ns,ho+j*2);}host[hl/2]=0;return SFI_API_RESOLVED;
    }
    return SFI_API_NOT_FOUND;
}
