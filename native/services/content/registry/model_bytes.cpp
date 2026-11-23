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

// A member as JSON. node_json shows an array of more than 16 bytes as "(N bytes)" (the gamedata API's dumps); a registry field
// can be one (the balance table's per-flame-level altar and water-source counts are byte arrays): every element as its number.
nlohmann::json json_of(const gd::Node& n, int depth) {
    const gd::TType* t = n.t;
    const gd::TType* el = t && t->inner() ? t->inner()->resolved() : nullptr;
    if (t && el && el->kind == gd::K_U8 && (t->kind == gd::K_ARRAY || t->kind == gd::K_BLOBARRAY)) {
        uintptr_t data = n.addr;
        uint32_t count = t->count;
        if (t->kind == gd::K_BLOBARRAY && !gd::blob_array(n.addr, &data, &count)) return nullptr;
        nlohmann::json a = nlohmann::json::array();
        for (uint32_t i = 0; i < count && i < 65536; ++i) a.push_back(reinterpret_cast<const uint8_t*>(data)[i]);
        return a;
    }
    return plain(gd::node_json(n, depth));
}
}  // namespace

bool root(const void* data, size_t len, const std::string& type, Node* out, std::string* why) {
    const gd::TType* t = gd::type_named(type);
    const gd::TType* r = t ? t->resolved() : nullptr;
    if (!r || r->kind != gd::K_STRUCT) return *why = "this game build has no struct type " + type, false;
    if (len < r->size) return *why = "the resource is " + std::to_string(len) + " bytes, which is not a " + type, false;
    *out = Node{r, reinterpret_cast<uintptr_t>(data), type == "keen::ecs::TemplateResource"};
    return true;
}

bool elements(const Node& at, const std::string& path, std::vector<Node>* out, std::string* why) {
    out->clear();
    std::vector<gd::Resolved> rs;
    std::string err;
    if (!gd::resolve(at.tmpl ? gd::Root::Template : gd::Root::Plain, node_of(at), path + "[*]", &rs, &err) && !err.empty()) return *why = err, false;
    for (const gd::Resolved& r : rs) out->push_back(Node{r.node.t, r.node.addr, false});
    return true;
}

bool member(const Node& at, const std::string& path, nlohmann::json* out, std::string* why) {
    if (path.empty()) return *out = json_of(node_of(at), kDepth), true;
    std::vector<gd::Resolved> rs;
    std::string err;
    if (!gd::resolve(at.tmpl ? gd::Root::Template : gd::Root::Plain, node_of(at), path, &rs, &err) || rs.empty())
        return *why = err.empty() ? path + ": nothing there" : err, false;
    if (rs.size() == 1) return *out = json_of(rs[0].node, kDepth), true;
    nlohmann::json a = nlohmann::json::array();
    for (const gd::Resolved& r : rs) a.push_back(json_of(r.node, kDepth));
    *out = std::move(a);
    return true;
}

bool identity(const std::string& name) { return gd::identity_field(name); }

}  // namespace kc::rbytes
