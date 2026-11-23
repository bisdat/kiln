// SPDX-License-Identifier: MIT
// kiln.content registry stage: the game's bytes through its reflection (model_bytes.h).
#include "model_bytes.h"

#include "../gamedata.h"

namespace kc::rbytes {

namespace {
constexpr int kDepth = 24;  // node_json depth of one member (a drop table's groups nest up to 8)

nlohmann::json plain(const gjson& g) {
    switch (g.type()) {
        case gjson::value_t::object: {
            nlohmann::json o = nlohmann::json::object();
            for (auto it = g.begin(); it != g.end(); ++it) o[it.key()] = plain(it.value());
            return o;
        }
        case gjson::value_t::array: {
            nlohmann::json a = nlohmann::json::array();
            for (const auto& x : g) a.push_back(plain(x));
            return a;
        }
        case gjson::value_t::string: return g.get<std::string>();
        case gjson::value_t::boolean: return g.get<bool>();
        case gjson::value_t::number_integer: return g.get<int64_t>();
        case gjson::value_t::number_unsigned: return g.get<uint64_t>();
        case gjson::value_t::number_float: return g.get<double>();
        default: return nullptr;
    }
}
gd::Node node_of(const Node& n) { return gd::Node{static_cast<const gd::TType*>(n.type), n.addr}; }

// Registry values are not dump summaries: byte arrays, including nested ones, are numbers, never "(N bytes)".
// Every byte read is guarded; buffer-backed nodes keep their owning span through member/element traversal.
bool contains(const Node& root, uintptr_t address, size_t count) {
    return root.size == 0 || (address >= root.begin && address - root.begin <= root.size && count <= root.size - (address - root.begin));
}
nlohmann::json json_of(const gd::Node& n, int depth, const Node& root) {
    const gd::TType* t = n.t ? n.t->resolved() : nullptr;
    if (!t || !contains(root, n.addr, t->size)) return nullptr;
    // a leaf first, as gd::node_json does: one-scalar structs (keen::ItemId, keen::HashKey32), GUIDs, enums read as values
    Field leaf;
    if (t->kind != gd::K_BLOBVAR && gd::leaf_of(n.t, &leaf)) return plain(gd::node_json(n, depth));
    if (t->kind == gd::K_ARRAY || t->kind == gd::K_BLOBARRAY) {
        const gd::TType* el = t->inner() ? t->inner()->resolved() : nullptr;
        if (!el || !el->size) return nullptr;
        uintptr_t data = n.addr;
        uint32_t count = t->count;
        if (t->kind == gd::K_BLOBARRAY && !gd::blob_array(n.addr, &data, &count)) return nullptr;
        if (count > 65536 || (count && (!data || count > SIZE_MAX / el->size || !contains(root, data, static_cast<size_t>(count) * el->size)))) return nullptr;
        nlohmann::json a = nlohmann::json::array();
        if (el->kind == gd::K_U8) {
            std::vector<uint8_t> bytes(count);
            if (count && !rd(data, bytes.data(), count)) return nullptr;
            for (uint8_t byte : bytes) a.push_back(byte);
        } else {
            if (depth <= 0) return nullptr;
            for (uint32_t i = 0; i < count; ++i) a.push_back(json_of(gd::Node{el, data + static_cast<size_t>(i) * el->size}, depth - 1, root));
        }
        return a;
    }
    if (t->kind == gd::K_STRUCT && t->name != "keen::impact::Configurations") {
        if (depth <= 0) return nullptr;
        nlohmann::json o = nlohmann::json::object();
        for (const gd::TField& f : t->fields) o[f.name] = json_of(gd::Node{f.type(), n.addr + f.offset}, depth - 1, root);
        return o;
    }
    return plain(gd::node_json(n, depth));
}
}  // namespace

bool root(const void* data, size_t len, const std::string& type, Node* out, std::string* why) {
    const gd::TType* t = gd::type_named(type);
    const gd::TType* r = t ? t->resolved() : nullptr;
    if (!r || r->kind != gd::K_STRUCT) return *why = "this game build has no struct type " + type, false;
    if (len < r->size) return *why = "the resource is " + std::to_string(len) + " bytes, which is not a " + type, false;
    *out = Node{r, reinterpret_cast<uintptr_t>(data), reinterpret_cast<uintptr_t>(data), len, type == "keen::ecs::TemplateResource"};
    return true;
}

bool elements(const Node& at, const std::string& path, std::vector<Node>* out, std::string* why) {
    out->clear();
    std::vector<gd::Resolved> rs;
    std::string err;
    if (!gd::resolve(at.tmpl ? gd::Root::Template : gd::Root::Plain, node_of(at), path + "[*]", &rs, &err) && !err.empty()) return *why = err, false;
    for (const gd::Resolved& r : rs) out->push_back(Node{r.node.t, r.node.addr, at.begin, at.size, false});
    return true;
}

bool member(const Node& at, const std::string& path, nlohmann::json* out, std::string* why) {
    if (path.empty()) return *out = json_of(node_of(at), kDepth, at), true;
    std::vector<gd::Resolved> rs;
    std::string err;
    if (!gd::resolve(at.tmpl ? gd::Root::Template : gd::Root::Plain, node_of(at), path, &rs, &err) || rs.empty())
        return *why = err.empty() ? path + ": nothing there" : err, false;
    if (rs.size() == 1) return *out = json_of(rs[0].node, kDepth, at), true;
    nlohmann::json a = nlohmann::json::array();
    for (const gd::Resolved& r : rs) a.push_back(json_of(r.node, kDepth, at));
    *out = std::move(a);
    return true;
}

bool identity(const std::string& name) { return gd::identity_field(name); }

}  // namespace kc::rbytes
