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

bool current_matches(const Before& before, const GameData& game) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    const bool present = game.read(before.guid, before.type, &data, &size);
    return present == before.present && (!present || (size == before.bytes.size() &&
        (!size || (data && std::memcmp(data, before.bytes.data(), size) == 0))));
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
    return out;
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
            if (mod.authored_body) { refuse(mod.id, "authored roster-body lowering is not installed; no part of this native pack was published"); continue; }

            for (int attempt = 0; attempt != 2; ++attempt) {
                std::map<Key, Before> reads;
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
                    }
                    *data = captured.bytes.data(); *size = captured.bytes.size();
                    return captured.present;
                };
                PackOut pack;
                try { pack = compile_pack({mod}, snapshot); }
                catch (const std::exception& error) { stop(mod.id, std::string("native compilation failed: ") + error.what()); break; }
                out.warnings.insert(out.warnings.end(), pack.warnings.begin(), pack.warnings.end());
                if (pack.roster_refused) { stop(mod.id, pack.roster_refusal, true); break; }
                if (pack.manifest.contains("failed")) { stop(mod.id, pack.manifest["failed"].get<std::string>()); break; }
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
                try {
                    wanted = resources(pack);
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
                        for (const auto& resource : wanted)
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
