/* SPDX-License-Identifier: MIT - generated structure only, no Microsoft bytes. */
#include "api_set.h"
#include <stdio.h>
#include <string.h>
static void put(uint8_t* data,size_t at,uint32_t value){memcpy(data+at,&value,4);}
static size_t text(uint16_t* out,const char* s){size_t n=0;while(s[n]){out[n]=(uint8_t)s[n];n++;}return n;}
static uint32_t emit(uint8_t* ns,uint32_t* end,const char* s){uint32_t at=*end;for(size_t i=0;s[i];i++){ns[(*end)++]=(uint8_t)s[i];ns[(*end)++]=0;}return at;}
static uint32_t resolve(uint8_t* ns,const char* importer,const char* requested,uint16_t* host,size_t* required){uint16_t i[260],r[260];size_t in=text(i,importer),rn=text(r,requested);return sfi_api_host_v6(ns,1024,i,in,r,rn,host,260,required);}
int main(int argc,char** argv){
    uint8_t ns[1024]={0};put(ns,0,6);put(ns,4,1024);put(ns,12,1);put(ns,16,28);
    const char* contract="api-ms-win-core-synch-l1-2-1";uint32_t end=128;uint32_t name=emit(ns,&end,contract);put(ns,32,name);put(ns,36,(uint32_t)strlen(contract)*2);put(ns,40,(uint32_t)(strrchr(contract,'-')-contract)*2);put(ns,44,52);put(ns,48,2);
    uint32_t alias=emit(ns,&end,"specific.dll"),host_name=emit(ns,&end,"kernelbase.dll");put(ns,64,host_name);put(ns,68,28);put(ns,76,alias);put(ns,80,24);uint32_t override=emit(ns,&end,"special.dll");put(ns,84,override);put(ns,88,22);
    uint16_t host[260]={0};size_t needed=0;const char* mode=argc>1?argv[1]:"version";
    if(strcmp(mode,"empty")==0){put(ns,68,0);uint32_t result=resolve(ns,"other.dll",contract,host,&needed);if(result!=SFI_API_ABSENT){fprintf(stderr,"FAIL empty-host expected absent, got%u\n",result);return 1;}puts("PASS optional empty host is absent, not malformed");return 0;}
    uint32_t result=resolve(ns,"other.dll","API-MS-WIN-CORE-SYNCH-L1-2-0.DLL",host,&needed);
    if(result!=SFI_API_RESOLVED||needed!=15||host[0]!='k'){fprintf(stderr,"FAIL hashed-prefix version alias expected resolved, got%u required%zu\n",result,needed);return 1;}
    result=resolve(ns,"SPECIFIC.DLL","api-ms-win-core-synch-l1-2-0.dll",host,&needed);if(result!=SFI_API_RESOLVED||host[0]!='s'){fputs("FAIL importer-specific host\n",stderr);return 1;}
    if(resolve(ns,"other.dll","api-ms-win-core-synch-l1-9-0.dll",host,&needed)!=SFI_API_NOT_FOUND)return 1;
    put(ns,40,1000);if(resolve(ns,"other.dll",contract,host,&needed)!=SFI_API_MALFORMED)return 1;
    puts("PASS schema-v6 hashed-prefix revisions/importer/default/bounds");return 0;
}
