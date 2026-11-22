// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
namespace kb::blocks {
// Generic, tick-owned terrain health policy. It is not the game's native building crack state.
class TerrainDamage {
    struct Entry {uint8_t material,damage;int64_t expires;};
    std::map<std::array<int32_t,3>,Entry> cells_;
public:
    void clear(){cells_.clear();}
    void forget(const std::array<int32_t,3>& c){cells_.erase(c);}
    uint8_t get(int64_t now,const std::array<int32_t,3>& c,uint8_t material){
        auto it=cells_.find(c);if(it==cells_.end())return 0;
        if(now>=it->second.expires || material!=it->second.material){cells_.erase(it);return 0;}
        return it->second.damage;
    }
    bool add(int64_t now,const std::array<int32_t,3>& c,uint8_t material,uint8_t increment,double seconds,uint8_t& out){
        if(now<0 || !std::isfinite(seconds) || seconds<=0 || seconds>86400)return false;
        const double ns=seconds*1e9;
        if(ns<1)return false;
        const int64_t duration=static_cast<int64_t>(ns); // bounded to one day, well inside int64
        if(now>INT64_MAX-duration)return false; // integer guard also works where long double has only 53 bits
        const auto old=get(now,c,material);
        if(!increment){out=old;return true;}
        if(cells_.size()>=65536 && cells_.find(c)==cells_.end()){
            for(auto i=cells_.begin();i!=cells_.end();)if(now>=i->second.expires)i=cells_.erase(i);else ++i;
            if(cells_.size()>=65536)return false;
        }
        out=static_cast<uint8_t>(std::min(255,static_cast<int>(old)+increment));
        cells_[c]={material,out,now+duration};return true;
    }
};
}
