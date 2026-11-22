// SPDX-License-Identifier: MIT
// The label arena (chapter 04 §2.3, §4.5): marker text interned once for the life of the process, in chunks that never
// move, so a UI snapshot may point at it without a retirement protocol. The same text is stored once; nothing is freed.
// Budgets (policy): 4 MiB of text in all; 256 KiB of new text per namespace, charged to the namespace that first adds a
// text and never given back (a publisher that unbinds and binds again keeps its count). Not thread-safe: the store calls
// it under its lock.
#ifndef KILN_MARKERS_ARENA_H
#define KILN_MARKERS_ARENA_H
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace kmk {

class Arena {
public:
    static constexpr size_t kGlobalBytes = size_t{4} << 20;
    static constexpr size_t kNamespaceBytes = size_t{256} << 10;
    static constexpr size_t kChunkBytes = size_t{64} << 10;

    // The interned copy of `text` (a view into the arena; empty text: an empty view, nothing stored). `charged` is the
    // namespace's count of new bytes so far; a text new to the arena adds its length to it. False when the text is new
    // and either budget would be exceeded: nothing changed then, `why` says which.
    bool intern(std::string_view text, uint64_t* charged, std::string_view* out, const char** why);

    // Whether `text` is already interned (no charge would be made).
    bool contains(std::string_view text) const { return text.empty() || index_.count(text) != 0; }

    size_t used() const { return used_; }
    size_t texts() const { return index_.size(); }

private:
    std::vector<std::unique_ptr<char[]>> chunks_;
    char* cursor_ = nullptr;
    size_t left_ = 0;
    size_t used_ = 0;
    std::unordered_set<std::string_view> index_;  // views into chunks_
};

}  // namespace kmk
#endif
