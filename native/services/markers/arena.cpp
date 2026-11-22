// SPDX-License-Identifier: MIT
#include "arena.h"

#include <cstring>

namespace kmk {

bool Arena::intern(std::string_view text, uint64_t* charged, std::string_view* out, const char** why) {
    *why = nullptr;
    if (text.empty()) {
        *out = {};
        return true;
    }
    if (auto it = index_.find(text); it != index_.end()) {
        *out = *it;
        return true;
    }
    if (used_ + text.size() > kGlobalBytes) {
        *why = "the process's 4 MiB of marker text is used up (text is kept until the game exits)";
        return false;
    }
    if (*charged + text.size() > kNamespaceBytes) {
        *why = "this namespace's 256 KiB of new marker text is used up (text is kept until the game exits; reuse labels)";
        return false;
    }
    if (left_ < text.size()) {
        chunks_.push_back(std::make_unique<char[]>(kChunkBytes));
        cursor_ = chunks_.back().get();
        left_ = kChunkBytes;
    }
    std::memcpy(cursor_, text.data(), text.size());
    const std::string_view stored(cursor_, text.size());
    cursor_ += text.size();
    left_ -= text.size();
    used_ += text.size();
    *charged += text.size();
    index_.insert(stored);
    *out = stored;
    return true;
}

}  // namespace kmk
