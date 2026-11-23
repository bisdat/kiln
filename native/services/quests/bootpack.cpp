// SPDX-License-Identifier: MIT
#include "bootpack.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace kiln::qb::bootpack {
namespace {
struct Key {
    Guid guid{};
    uint32_t type = 0;
    bool operator<(const Key& other) const {
        const int order = std::memcmp(guid.b, other.guid.b, sizeof guid.b);
        return order < 0 || (order == 0 && type < other.type);
    }
};

uint64_t fingerprint(const uint8_t* data, size_t size) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
    return hash;
}
struct Captured : Before { Bytes bytes; };  // private compile snapshot only; released after this mod's preparation

bool same_bytes(const Before& a, const Before& b) {
    return a.present == b.present && (!a.present || (a.size == b.size && a.fingerprint == b.fingerprint));
}
bool current_matches(const Before& before, const GameData& game) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    const bool present = game.read(before.guid, before.type, &data, &size);
    return present == before.present && (!present || (size == before.size &&
        (!size || (data && fingerprint(data, size) == before.fingerprint))));
}

std::vector<Resource> resources(const PackOut& pack) {
    std::vector<Resource> out;
    for (const auto& [guid, type, bytes, name] : {
             std::tuple{kGuidKnowledge, kTypeKnowledge, &pack.res_knowledge, "GameKnowledgeResource"},
             std::tuple{kGuidJournal, kTypeJournal, &pack.res_journal, "JournalRegistryResource"},
             std::tuple{kGuidNpcs, kTypeNpcs, &pack.res_npcs, "NpcCollection"}}) {
        if (bytes->empty()) continue;
        Guid key{};
        if (!parse_guid(guid, &key)) throw std::logic_error("invalid shared-table key");
        out.push_back({key, type, false, bytes, name});
    }
    for (const auto& item : pack.templates) out.push_back({item.guid, kTypeTemplate, item.is_new, &item.data, item.name});
    for (const auto& item : pack.resources) out.push_back({item.guid, item.type, true, &item.data, item.name});  // 16f S4 clones
    return out;
}

// The new resources an admitted body publishes (kiln.assets entries, types by name or 0x hash).
bool body_keys(const ojson& assets, std::vector<Key>* out, std::string* why) {
    for (const auto& entry : assets.value("entries", ojson::array())) {
        if (!entry.is_object() || entry.value("kind", "") != "resource") continue;
        Key key;
        const std::string type = entry.value("type", "");
        if (!parse_guid(entry.value("guid", ""), &key.guid) || type.empty()) { *why = "an admitted body resource has no key"; return false; }
        key.type = type.rfind("0x", 0) == 0 ? static_cast<uint32_t>(std::stoul(type, nullptr, 16)) : fnv1a(type.data(), type.size());
        out->push_back(key);
    }
    return true;
}

void unique_ids(std::vector<uint32_t>* ids) {
    std::sort(ids->begin(), ids->end());
    ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
}
}  // namespace

bool withdraw(const Applied& applied, const Backend& backend, std::string* why) {
    try {
        if (!backend.rollback(applied.owner, why)) return false;
        for (const auto& before : applied.before)
            if (!current_matches(before, backend.game)) {
                *why = "rollback did not restore the effective resource before-image (type " + std::to_string(before.type) + ")";
                return false;
            }
        return true;
    } catch (const std::exception& error) {
        *why = std::string("rollback threw: ") + error.what();
        return false;
    } catch (...) {
        *why = "rollback raised an unknown exception";
        return false;
    }
}

Result build(const std::vector<ModInput>& mods, const Backend& backend) {
    Result out;
    ojson manifest_mods = ojson::array();
    std::set<std::string> failed;
    Guid knowledge_guid{};
    parse_guid(kGuidKnowledge, &knowledge_guid);
    Before last_knowledge;
    size_t knowledge_prefix = 0;
    const auto refuse = [&](const std::string& mod, const std::string& why) {
        const std::string text = "bootpack:" + mod + ": " + why;
        out.errors.push_back(text);
        failed.insert(mod);
        manifest_mods.push_back({{"id", mod}, {"ok", false}, {"errors", {text}}});
    };
    const auto stop = [&](const std::string& mod, const std::string& why, bool roster = false) {
        out.stopped = true;
        out.roster_refused = roster;
        out.reason = "bootpack:" + mod + ": " + why;
        out.stopped_mods = {mod};
    };

    // Profiles already have dependency order. This also orders required native dependencies in legacy drop-ins.
    std::map<std::string, size_t> by_id;
    for (size_t i = 0; i < mods.size(); ++i) by_id.emplace(mods[i].id, i);
    std::vector<unsigned> state(mods.size());
    std::vector<size_t> order, path;
    std::set<size_t> cycles;
    std::function<void(size_t)> visit = [&](size_t at) {
        if (state[at] == 2) return;
        if (state[at] == 1) {
            auto start = std::find(path.begin(), path.end(), at);
            cycles.insert(start, path.end());
            return;
        }
        state[at] = 1;
        path.push_back(at);
        for (const auto& dependency : mods[at].required_mods)
            if (auto found = by_id.find(dependency); found != by_id.end()) visit(found->second);
        path.pop_back();
        state[at] = 2;
        order.push_back(at);
    };
    for (size_t i = 0; i < mods.size(); ++i) visit(i);

    for (size_t index : order) {
        const auto& mod = mods[index];
        try {
            if (cycles.count(index)) { refuse(mod.id, "required native dependency cycle"); continue; }
            std::string dependency_failure;
            for (const auto& dependency : mod.required_mods)
                if (failed.count(dependency)) { dependency_failure = dependency; break; }
            if (!dependency_failure.empty()) { refuse(mod.id, "required dependency " + dependency_failure + " did not commit"); continue; }
            // An authored body is admitted once, before compilation, against this machine's cook of the mod; a refusal
            // publishes nothing of the mod. The admitted copy (cooked ids in its NPC bodies) is what compiles.
            ModInput admitted;
            ojson body_assets;
            const ModInput* input = &mod;
            if (mod.authored_body && (!backend.admit_body || !backend.put_body)) {
                refuse(mod.id, "authored roster-body lowering is not installed; no part of this native pack was published");
                continue;
            }
            if (!mod.body_refusal.empty()) { refuse(mod.id, mod.body_refusal); continue; }
            if (mod.authored_body) {
                admitted = mod;
                std::string why;
                bool ok = false;
                try { ok = backend.admit_body(&admitted, &body_assets, &why); }
                catch (const std::exception& error) { why = std::string("body admission failed: ") + error.what(); }
                if (!ok) { refuse(mod.id, why.empty() ? "authored body admission refused the mod" : why); continue; }
                input = &admitted;
            }

            for (int attempt = 0; attempt != 2; ++attempt) {
                std::map<Key, Captured> reads;
                GameData snapshot = backend.game;
                snapshot.read = [&](const Guid& guid, uint32_t type, const uint8_t** data, size_t* size) {
                    auto [it, fresh] = reads.try_emplace(Key{guid, type});
                    auto& captured = it->second;
                    if (fresh) {
                        captured.guid = guid; captured.type = type;
                        const uint8_t* bytes = nullptr;
                        size_t count = 0;
                        captured.present = backend.game.read(guid, type, &bytes, &count);
                        if (captured.present && count) {
                            if (!bytes) throw std::runtime_error("a resource read returned null bytes");
                            captured.bytes.assign(bytes, bytes + count);
                        }
                        captured.size = captured.bytes.size();
                        captured.fingerprint = fingerprint(captured.bytes.data(), captured.bytes.size());
                    }
                    *data = captured.bytes.data(); *size = captured.bytes.size();
                    return captured.present;
                };
                PackOut pack;
                try { pack = compile_pack({*input}, snapshot); }
                catch (const std::exception& error) { stop(mod.id, std::string("native compilation failed: ") + error.what()); break; }
                out.warnings.insert(out.warnings.end(), pack.warnings.begin(), pack.warnings.end());
                if (pack.roster_refused) { stop(mod.id, pack.roster_refusal, true); break; }
                if (!pack.shared_failure.empty()) { stop(mod.id, pack.shared_failure); break; }
                if (pack.manifest.contains("failed")) { refuse(mod.id, pack.manifest["failed"].get<std::string>()); break; }
                if (!pack.ok || !pack.errors.empty()) {
                    refuse(mod.id, pack.errors.empty() ? "native compilation refused the mod" : pack.errors.front());
                    break;
                }
                if (!pack.any) {
                    for (const auto& entry : pack.manifest.value("mods", ojson::array())) manifest_mods.push_back(entry);
                    break;
                }

                std::vector<Resource> wanted;
                std::string refusal;
                size_t next_knowledge_prefix = pack.knowledge_source_size;
                try {
                    if (pack.knowledge_source_size) {
                        const auto& source = reads.at(Key{knowledge_guid, kTypeKnowledge});
                        // Trim only our own known tail. An unrelated writer's different effective blob starts a new
                        // retained prefix, including its unknown fields and appended data.
                        if (last_knowledge.present && same_bytes(source, last_knowledge)) {
                            next_knowledge_prefix = knowledge_prefix;
                            if (!compact_knowledge(&pack.res_knowledge, knowledge_prefix, &refusal))
                                refusal = "knowledge compaction failed: " + refusal;
                        }
                    }
                    wanted = resources(pack);
                    // An admitted body's resources are new keys: present now means another writer has them. Reading
                    // them through the snapshot also records them as absent before-images, so a rollback is checked
                    // to take them away again (withdraw).
                    std::vector<Key> keys;
                    if (mod.authored_body && !body_keys(body_assets, &keys, &refusal)) keys.clear();
                    for (const auto& key : keys) {
                        const uint8_t* bytes = nullptr;
                        size_t count = 0;
                        if (snapshot.read(key.guid, key.type, &bytes, &count)) { refusal = "body resource " + guid_text(key.guid) + ": new key already exists"; break; }
                    }
                    // Capture every overwritten key, not only the keys the compiler happened to read. New keys must be absent.
                    for (const auto& resource : wanted) {
                        const uint8_t* bytes = nullptr;
                        size_t count = 0;
                        const bool present = snapshot.read(resource.guid, resource.type, &bytes, &count);
                        if (resource.is_new == present) {
                            refusal = resource.name + (resource.is_new ? ": new key already exists" : ": override key is missing");
                            break;
                        }
                    }
                } catch (const std::exception& error) { refusal = std::string("preflight failed: ") + error.what(); }
                if (!refusal.empty()) { refuse(mod.id, refusal); break; }
                bool changed = false;
                for (const auto& [key, before] : reads) changed |= !current_matches(before, backend.game);
                if (changed) {
                    if (attempt == 0) continue;
                    refuse(mod.id, "effective resources changed twice during native preparation");
                    break;
                }
                Applied applied{mod.id, 0, {}};
                try { applied.owner = backend.owner(mod.id); }
                catch (const std::exception& error) { stop(mod.id, std::string("process owner creation failed: ") + error.what()); break; }
                if (!applied.owner) { stop(mod.id, "could not create its process-lifetime owner"); break; }
                for (const auto& [key, before] : reads) applied.before.push_back(before);
                bool began = false;
                try {
                    began = backend.begin(applied.owner, mod.id, &refusal);
                    if (began) {
                        if (mod.authored_body && !backend.put_body(applied.owner, *input, body_assets, &refusal) && refusal.empty())
                            refusal = "the authored body's cooked resources were refused";
                        if (refusal.empty()) for (const auto& resource : wanted)
                            if (!backend.put(applied.owner, resource, &refusal)) { if (refusal.empty()) refusal = resource.name + ": resource refused"; break; }
                        if (refusal.empty()) for (const auto& item : pack.content_items)
                            if (!backend.define(applied.owner, true, item, &refusal)) { if (refusal.empty()) refusal = "item definition refused"; break; }
                        if (refusal.empty()) for (const auto& recipe : pack.content_recipes)
                            if (!backend.define(applied.owner, false, recipe, &refusal)) { if (refusal.empty()) refusal = "recipe definition refused"; break; }
                        if (refusal.empty()) for (const auto& text : pack.texts)
                            if (!backend.text(applied.owner, text, &refusal)) { if (refusal.empty()) refusal = "text write refused"; break; }
                        if (refusal.empty() && !backend.finish(applied.owner, &refusal) && refusal.empty()) refusal = "content finish refused the pack";
                    } else if (refusal.empty()) refusal = "content prepare refused the pack";
                } catch (const std::exception& error) { refusal = std::string("publication failed: ") + error.what(); }
                catch (...) { refusal = "publication raised an unknown exception"; }
                if (!refusal.empty()) {
                    std::string rollback_reason;
                    if (!withdraw(applied, backend, &rollback_reason)) {
                        stop(mod.id, "rollback invariant failed after " + refusal + ": " + rollback_reason);
                        break;
                    }
                    refuse(mod.id, refusal);
                    break;
                }
                if (pack.knowledge_source_size) {
                    knowledge_prefix = next_knowledge_prefix;
                    last_knowledge = {knowledge_guid, kTypeKnowledge, true, pack.res_knowledge.size(),
                        fingerprint(pack.res_knowledge.data(), pack.res_knowledge.size())};
                }
                out.applied.push_back(std::move(applied));
                out.manifest = pack.manifest;
                for (const auto& entry : pack.manifest.value("mods", ojson::array())) manifest_mods.push_back(entry);
                if (!pack.roster_kiln_ids.empty() && out.roster_kiln_ids.empty()) out.roster_boot_ids = pack.roster_boot_ids;
                out.roster_kiln_ids.insert(out.roster_kiln_ids.end(), pack.roster_kiln_ids.begin(), pack.roster_kiln_ids.end());
                out.knowledge += pack.knowledge.size(); out.quests += pack.quests.size(); out.npcs += pack.npcs.size();
                out.templates += pack.templates.size(); out.texts += pack.texts.size();
                break;
            }
        } catch (const std::exception& error) {
            stop(mod.id, std::string("native preparation failed: ") + error.what());
        } catch (...) {
            stop(mod.id, "native preparation raised an unknown exception");
        }
        if (out.stopped) break;
    }
    unique_ids(&out.roster_kiln_ids);
    out.manifest["mods"] = std::move(manifest_mods);
    out.manifest["errors"] = out.errors;
    out.manifest["bootpack"] = {{"mode", "per-mod-rollback"}, {"committed", out.applied.size()}};
    if (out.stopped) out.manifest["failed"] = out.reason;
    return out;
}
}  // namespace kiln::qb::bootpack
