// SPDX-License-Identifier: MIT
#pragma once
// The game's reflection records as kiln.content's refl.cpp reads them (type +0x20 name, +0x38 inner, +0x40 size, +0x48
// field count, +0x4c kind, +0x50 hash, +0x58 fields; field +0 name, +0x10 type, +0x18 offset) and as gd::TType, built from
// an independently extracted TypeDb JSON (tools/kfc). Shared by the tool writer and tool content checks.
#include "../../content/gamedata.h"
#include "nlohmann/json.hpp"
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
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
    // gd::TType lists a struct's base chain fields first (its inner is the base), as the production loader does.
    std::map<std::string,bool> flat;
    std::function<void(Type&)> flatten=[&](Type& x){
        if(flat[x.name])return;flat[x.name]=true;
        if(x.walk.kind!=0x12||!x.walk.inner_rec)return;
        auto* base=reinterpret_cast<kc::gd::TType*>(x.walk.inner_rec);
        auto i=types.find(base->name);if(i==types.end()||i->second.walk.kind!=0x12)return;
        flatten(i->second);
        x.walk.fields.insert(x.walk.fields.begin(),i->second.walk.fields.begin(),i->second.walk.fields.end());
    };
    for(auto& [_,x]:types)flatten(x);
}
}
