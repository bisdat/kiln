// SPDX-License-Identifier: MIT
// Production nc::apply_fields/refl flattening, then the production itemdata walker/leaf encoder, against the actual
// extracted Small brush bytes and reflected type layout. Only memory/resource lookup boundaries are host adapters.
// The input compiler descriptor comes from ToolActionCompilerTests; no hand-written replacement of its fields.
#include <windows.h>
#ifndef _WIN32
inline unsigned long long GetTickCount64(){return 1000;}
inline unsigned long GetExceptionCode(){return 0;}
#define EXCEPTION_EXECUTE_HANDLER 1
#define __except(x) catch (...)
#define __declspec(x)
inline long long _InterlockedOr64(volatile long long* p,long long v){auto old=*p;*p=old|v;return old;}
#endif
#include "../../content/newcontent.cpp"
#include "../../content/gamedata.h"
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fixture {
using json=nlohmann::json;
struct Type {
    std::string name;
    std::array<uint8_t,0x68> raw{};
    std::vector<std::array<uint8_t,0x30>> fields;
    std::vector<std::string> names;
    kc::gd::TType walk;
};
std::map<std::string,Type> types;
std::vector<std::string> sequences;
template<class T>void put(uint8_t* p,size_t offset,T value){std::memcpy(p+offset,&value,sizeof(value));}
void load_types(const json& document){
    for(const auto& t:document["types"]){auto& x=types[t["name"].get<std::string>()];x.name=t["name"];x.walk.name=x.name;x.walk.brief=x.name;x.walk.hash=t["hash"];x.walk.kind=t["kind"];x.walk.size=t["size"];}
    for(const auto& t:document["types"]){
        auto& x=types.at(t["name"]);const auto& fields=t["fields"];
        x.fields.resize(fields.size());x.names.reserve(fields.size());
        for(const auto& f:fields)x.names.push_back(f["name"]);
        for(size_t i=0;i<fields.size();++i){
            auto& target=types.at(fields[i]["type"]);auto* p=x.fields[i].data();
            put(p,0,reinterpret_cast<uintptr_t>(x.names[i].data()));put(p,8,uint64_t(x.names[i].size()));
            put(p,0x10,reinterpret_cast<uintptr_t>(target.raw.data()));put(p,0x18,uint64_t(fields[i]["offset"].get<uint32_t>()));
            x.walk.fields.push_back({x.names[i],reinterpret_cast<uintptr_t>(&target.walk),fields[i]["offset"]});
        }
        auto* p=x.raw.data();put(p,0x20,reinterpret_cast<uintptr_t>(x.name.data()));put(p,0x28,uint64_t(x.name.size()));
        put(p,0x40,x.walk.size);put(p,0x48,uint32_t(fields.size()));put(p,0x4c,x.walk.kind);put(p,0x50,x.walk.hash);
        put(p,0x58,reinterpret_cast<uintptr_t>(x.fields.data()));
        if(!t["inner"].is_null()){
            auto& inner=types.at(t["inner"]);put(p,0x38,reinterpret_cast<uintptr_t>(inner.raw.data()));
            x.walk.inner_rec=reinterpret_cast<uintptr_t>(&inner.walk);
        }
    }
}
}
namespace kc {
const KilnApi* g_api=nullptr;KilnOwner g_owner=1;Engine E;
void logf(int,const char*,...){}
bool rd(uintptr_t p,void* out,size_t n){if(!p)return false;std::memcpy(out,reinterpret_cast<const void*>(p),n);return true;}
std::string read_text(uintptr_t p,uint64_t n){return p?std::string(reinterpret_cast<const char*>(p),static_cast<size_t>(n)):std::string();}
std::string read_blob_string(uintptr_t){return {};}
std::string data_dir(){return {};}
bool path_exists(const std::string&){return false;}
uint32_t fnv1a(const void* p,size_t n){uint32_t h=2166136261u;for(size_t i=0;i<n;++i)h=(h^static_cast<const uint8_t*>(p)[i])*16777619u;return h;}
std::string lower(std::string s){for(char& c:s)if(c>='A'&&c<='Z')c=static_cast<char>(c+'a'-'A');return s;}
uintptr_t type_by_hash(uint32_t h){for(auto& [_,x]:fixture::types)if(x.walk.hash==h)return reinterpret_cast<uintptr_t>(x.raw.data());return 0;}
bool guid_parse(const std::string& s,uint8_t out[16]){
    unsigned a,b,c,d[8];if(std::sscanf(s.c_str(),"%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",&a,&b,&c,&d[0],&d[1],&d[2],&d[3],&d[4],&d[5],&d[6],&d[7])!=11)return false;
    fixture::put(out,0,uint32_t(a));fixture::put(out,4,uint16_t(b));fixture::put(out,6,uint16_t(c));for(int i=0;i<8;++i)out[8+i]=static_cast<uint8_t>(d[i]);return true;
}
std::string guid_text(const uint8_t g[16]){for(const auto& s:fixture::sequences){uint8_t b[16];if(guid_parse(s,b)&&!std::memcmp(b,g,16))return s;}return "unknown";}
const Entry* Registry::lookup(const std::string&)const{return nullptr;}
Registry& reg(uint32_t){static Registry r;return r;}
namespace gd {
const TType* TType::inner()const{return reinterpret_cast<const TType*>(inner_rec);}
const TType* TType::resolved()const{auto* t=this;for(int i=0;i<16&&t&&t->kind==K_TYPEDEF;++i)t=t->inner();return t;}
const TType* TField::type()const{return reinterpret_cast<const TType*>(type_rec);}
const TField* TType::field(const std::string& n)const{for(const auto& f:fields)if(f.name==n)return &f;return nullptr;}
const TType* type_named(const std::string& n){auto i=fixture::types.find(n);return i==fixture::types.end()?nullptr:&i->second.walk;}
const TType* type_hash(uint32_t h){for(const auto& [_,x]:fixture::types)if(x.walk.hash==h)return &x.walk;return nullptr;}
const GEntry* GRegistry::get(uint32_t)const{return nullptr;}
const GEntry* GRegistry::lookup(const std::string&)const{return nullptr;}
GRegistry& greg(uint32_t){static GRegistry r;return r;}
bool buff_guid(uint32_t,uint8_t[16]){return false;}
bool vfx_reference_kept(const uint8_t[16]){return false;}
bool res_list(uint32_t,std::vector<std::array<uint8_t,16>>*){return false;}
uintptr_t res_data(const uint8_t g[16],uint32_t h,uint64_t*){
    if(h!=fnv1a(std::string("keen::actor::ActorSequenceResource")))return 0;
    for(const auto& s:fixture::sequences){uint8_t b[16];if(guid_parse(s,b)&&!std::memcmp(b,g,16))return 1;}return 0;
}
}
}
int main(int argc,char** argv){
    using namespace kc;using namespace kc::gd;
    try{
        if(argc!=4)throw std::runtime_error("arguments: compiler.json types.json brush.bin");
        fixture::json compiler,types;std::ifstream(argv[1])>>compiler;std::ifstream(argv[2])>>types;fixture::load_types(types);
        fixture::sequences={compiler["primary"],compiler["secondary"]};
        std::ifstream input(argv[3],std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
        const auto* item=type_named("keen::ItemInfo");if(!item||bytes.size()<item->size)throw std::runtime_error("real ItemInfo fixture missing");
        const size_t sequences=types["sequencesOffset"],stride=types["sequenceSize"],id=types["idOffset"],reference=types["referenceOffset"];
        uintptr_t data=0;uint32_t count=0;if(!blob_array(reinterpret_cast<uintptr_t>(bytes.data())+sequences,&data,&count)||count!=2||data+stride*2>reinterpret_cast<uintptr_t>(bytes.data()+bytes.size()))throw std::runtime_error("fixture is not the real two-slot Small brush");
        std::vector<uint8_t> original(reinterpret_cast<uint8_t*>(data),reinterpret_cast<uint8_t*>(data)+stride*2);
        Schema schema;std::string why;
        if(!build_schema("keen::ItemInfo",{},&schema,&why))throw std::runtime_error("real schema: "+why);
        if(!apply_fields(schema,compiler["fields"],bytes.data(),"test.tool.brush",&why))throw std::runtime_error("real content apply_fields refused: "+why);
        if(compiler["refs"].is_object())for(const auto& [path,value]:compiler["refs"].items()){
            const auto* f=schema.find(path);if(!f||!f->ref)throw std::runtime_error("real refs schema refused: "+path);
            (void)value;
        }
        int writes=0;
        for(const auto& [path,value]:compiler["itemdata"].items()){
            std::vector<Resolved> resolved;
            if(!resolve(Root::Plain,{item,reinterpret_cast<uintptr_t>(bytes.data())},path,&resolved,&why)||resolved.size()!=1)throw std::runtime_error("real itemdata resolve: "+why);
            Field field;if(!leaf_of(resolved[0].node.t,&field))throw std::runtime_error("real itemdata leaf refused: "+path);
            auto* at=reinterpret_cast<uint8_t*>(resolved[0].node.addr+field.offset);uint8_t value_bytes[16]{};bool reset=false;
            if(!parse_leaf(field,value,at,value_bytes,&reset,&why)||reset)throw std::runtime_error("real itemdata parse_leaf: "+why);
            std::memcpy(at,value_bytes,field.bytes);++writes;
        }
        if(writes!=4)throw std::runtime_error("expected four itemdata writes (two refs and paired ids)");
        for(size_t slot=0;slot<2;++slot){
            uint8_t guid[16];guid_parse(compiler[slot?"primary":"secondary"],guid);auto* p=reinterpret_cast<uint8_t*>(data+stride*slot);
            uint32_t actual=0;std::memcpy(&actual,p+id,4);
            if(actual!=fnv1a(guid,16)||std::memcmp(p+reference,guid,16))throw std::runtime_error("sequence reference/id mismatch");
            for(size_t k=0;k<stride;++k)if(!(k>=id&&k<id+4)&&!(k>=reference&&k<reference+16)&&p[k]!=original[slot*stride+k])throw std::runtime_error("unrelated brush sequence bytes changed");
        }
        std::cout<<"real Small brush: scalar content accepted; 4 native itemdata writes; both reference/id pairs and remaining sequence bytes verified\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
