// SPDX-License-Identifier: MIT
#pragma once
#include "presentation.h"
#include "services/kiln_ui_ammo.h"
#include <algorithm>
namespace kiln::tools {
struct AmmoComposition {
    std::array<SelectorRow,KUA_CAPACITY> rows{};
    uint32_t count{};
    uint64_t selected{};
    bool has_selected{},truncated{};
};
// All inputs are owned copies. Game rows win duplicates in Append; otherwise provider order wins.
inline AmmoComposition compose_ammo(const SelectorRow* game,uint32_t game_count,const KilnUiAmmoRow* supplied,
                                     uint32_t supplied_count,uint32_t mode,uint32_t selected) {
    AmmoComposition out;
    auto add=[&](SelectorRow row) {
        for(uint32_t i=0;i<out.count;++i)if(out.rows[i].item==row.item)return;
        if(out.count==KUA_CAPACITY){out.truncated=true;return;}
        if(row.item==selected){out.selected=out.count;out.has_selected=true;}
        out.rows[out.count++]=row;
    };
    if(mode!=KUA_MODE_REPLACE)for(uint32_t i=0;i<game_count;++i)add(game[i]);
    if(mode!=KUA_MODE_GAME)for(uint32_t i=0;i<supplied_count;++i) {
        SelectorRow row{};row.item=supplied[i].item;row.count=supplied[i].count;
        for(uint32_t j=0;j<game_count;++j)if(game[j].item==row.item){row.source=game[j].source;break;}
        add(row);
    }
    return out;
}
} // namespace kiln::tools
