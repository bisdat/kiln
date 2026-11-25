// SPDX-License-Identifier: MIT
// kiln-emf v32 buffs and v35 player_view without the game (buffs_model.h). Ports of EMF 0.6.22's src/core/buffs.cpp
// (type records, read_buffs, check_applied, BuffDiff) and src/core/player_view.cpp (MIT, the EMF contributors), and of
// Kiln.Abilities' ProgramBlob and GuidWithId (managed/services/Kiln.Abilities/Buffs.cs).
#include "buffs_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

namespace kemf::buffs {

namespace {

template <typename T>
T at(const uint8_t* p, size_t off) {
    T v{};
    std::memcpy(&v, p + off, sizeof v);
    return v;
}
template <typename T>
void put(std::vector<uint8_t>& b, size_t off, T v) {
    std::memcpy(b.data() + off, &v, sizeof v);
}

constexpr uint32_t kPrime = 0x01000193u, kInverse = 0x359c449bu, kBasis = 0x811c9dc5u;

}  // namespace

// kiln-emf's programs: "kemf" + "buff", 0x80 variant, 1 = give, 2 = remove (never a game program's GUID)
const uint8_t kApplyProgram[16] = {0x6b, 0x65, 0x6d, 0x66, 0x62, 0x75, 0x66, 0x66, 0x80, 0, 0, 0, 0, 0, 0, 0x01};
const uint8_t kRemoveProgram[16] = {0x6b, 0x65, 0x6d, 0x66, 0x62, 0x75, 0x66, 0x66, 0x80, 0, 0, 0, 0, 0, 0, 0x02};
const uint8_t kGenericImpact[16] = {0xa4, 0x20, 0x7a, 0xe9, 0xa2, 0x4e, 0x67, 0x47,
                                    0x89, 0xae, 0xc4, 0x1f, 0xf4, 0x58, 0x48, 0xc1};

uint32_t fnv1a(const uint8_t* p, size_t n) {
    uint32_t h = kBasis;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * kPrime;
    return h;
}

uint32_t guid_id(const uint8_t* g) {
    static const uint8_t zero[16] = {};
    if (!g || std::memcmp(g, zero, 16) == 0) return 0;
    return fnv1a(g, 16);
}

bool type_of(const uint8_t* r, EmfBuffType* out) {
    if (!r || !out) return false;
    EmfBuffType t{};
    t.size = sizeof t;
    t.id = at<uint32_t>(r, 0x00);  // buffTypeId
    t.slot = r[0x04];              // slot (keen::BuffSlotType: Food, Buff, Debuff, Rested, TerrainDebuff)
    t.apply_type = r[0x05];        // applyType (Invalid, ResetTime, ReplaceAtEnd, Replace)
    t.default_ns = at<int64_t>(r, 0x08);  // defaultLifeTime
    t.name_tag = guid_id(r + 0x10);       // name (NameLocaTag)
    t.desc_tag = guid_id(r + 0x20);       // description
    if (r[0x30]) t.flags |= EMF_BUFF_FOOD_POISON;       // isFoodPoison
    if (r[0x31]) t.flags |= EMF_BUFF_HIDDEN;            // hideInUi
    if (r[0x32]) t.flags |= EMF_BUFF_SHOW_AS_TAG;       // showAsTag
    if (r[0x49]) t.flags |= EMF_BUFF_DESPAWN_ON_DEATH;  // despawnOnDeath
    if (r[0x4A]) t.flags |= EMF_BUFF_SAVED;             // isSaved
    t.faction_exclude = at<uint16_t>(r, 0x5C);          // factionExcludeMask (bit 7 = Player)
    t.follow_up = r[0x98] ? guid_id(r + 0x88) : 0;      // onTimeout's follow-up, when onTimeoutIsSet
    *out = t;
    return true;
}

EmfBuff buff_of(const Row& r, int64_t now) {
    EmfBuff b{};
    b.size = sizeof b;
    b.type = r.type;
    b.entity = r.entity;
    b.slot = r.slot;
    b.index = r.index;
    b.cleansed = r.cleansed ? 1 : 0;
    b.total_ns = b.remaining_ns = -1;
    if (r.end_ns) {  // EMF: known once the buff entity's BuffNetworkData has its end
        b.total_ns = r.life_ns;
        b.remaining_ns = r.end_ns > now ? r.end_ns - now : 0;
        b.level = r.level;
    }
    return b;
}

uint32_t fill_buffs(const Row* rows, uint32_t n, int64_t now, EmfBuff* out, uint32_t cap) {
    if (out)
        for (uint32_t i = 0; i < n && i < cap; ++i) out[i] = buff_of(rows[i], now);
    return n;
}

const char* slot_name(uint8_t slot) {
    switch (slot) {
        case EMF_BUFF_FOOD: return "food";
        case EMF_BUFF_BUFF: return "buff";
        case EMF_BUFF_DEBUFF: return "debuff";
        case EMF_BUFF_RESTED: return "rested";
        case EMF_BUFF_TERRAIN: return "terrain";
        default: return "?";
    }
}

std::vector<EmfBuffEvent> Diff::step(uint32_t entity, const Row* rows, uint32_t n, int64_t now) {
    std::vector<Inst> cur;
    cur.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const Row& r = rows[i];
        if (!r.entity) continue;
        cur.push_back(Inst{r.entity, r.type, r.slot, static_cast<uint8_t>(r.cleansed ? 1 : 0), r.end_ns ? r.end_ns : -1, r.life_ns});
    }
    std::vector<EmfBuffEvent> ev;
    const auto it = last_.find(entity);
    if (it == last_.end()) {
        last_.emplace(entity, std::move(cur));
        return ev;
    }
    auto make = [&](uint32_t kind, const Inst& x) {
        EmfBuffEvent e{};
        e.size = sizeof e;
        e.kind = kind;
        e.target = entity;
        e.type = x.type;
        e.buff_entity = x.buff_entity;
        e.slot = x.slot;
        e.cleansed = x.cleansed;
        e.total_ns = e.remaining_ns = -1;
        if (x.end >= 0) {
            e.total_ns = x.life;
            e.remaining_ns = x.end > now ? x.end - now : 0;
        }
        return e;
    };
    const std::vector<Inst>& old = it->second;
    auto in = [](const std::vector<Inst>& list, uint32_t buff_entity) -> const Inst* {
        for (const Inst& i : list)
            if (i.buff_entity == buff_entity) return &i;
        return nullptr;
    };
    // removals first: a buff given again that the game replaced reads "removed (replaced)", then "applied"
    for (const Inst& o : old) {
        if (in(cur, o.buff_entity)) continue;
        EmfBuffEvent e = make(EMF_BUFF_REMOVED, o);
        e.total_ns = -1;
        e.remaining_ns = 0;
        for (const Inst& x : cur)
            if (x.type == o.type && !in(old, x.buff_entity)) e.replaced = 1;
        ev.push_back(e);
    }
    for (const Inst& x : cur) {
        const Inst* was = in(old, x.buff_entity);
        if (!was)
            ev.push_back(make(EMF_BUFF_APPLIED, x));
        else if (was->end >= 0 && x.end > was->end + kRefreshMinNs)
            ev.push_back(make(EMF_BUFF_REFRESHED, x));
    }
    it->second = std::move(cur);
    return ev;
}

void Diff::retain(const std::vector<uint32_t>& present) {
    for (auto it = last_.begin(); it != last_.end();) {
        bool keep = false;
        for (uint32_t e : present) keep |= e == it->first;
        it = keep ? std::next(it) : last_.erase(it);
    }
}

namespace {

// The forward half of the search for one salt: the state after the 12 fixed bytes and two more (x = bytes 12, 13), in
// a flat hash table (linear probing, 2^17 slots). It does not depend on the id, so the first salts' tables are kept
// (about 900 KB each): a new id then costs 65,536 probes, not a new table (about 2 ms, once per buff type: kiln-emf
// keeps each GUID).
struct Forward {
    static constexpr uint32_t kSlots = 1u << 17;
    std::array<uint8_t, 16> prefix{};
    std::vector<uint32_t> keys = std::vector<uint32_t>(kSlots);
    std::vector<uint16_t> xs = std::vector<uint16_t>(kSlots);
    std::vector<uint8_t> used = std::vector<uint8_t>(kSlots);
    static uint32_t slot(uint32_t key) { return (key * 0x9E3779B1u) >> 15; }
};
Forward make_forward(uint32_t salt) {
    Forward f;
    for (int i = 0; i < 12; ++i) f.prefix[i] = static_cast<uint8_t>(0x4b + i * 7 + salt);
    uint32_t a = kBasis;
    for (int i = 0; i < 12; ++i) a = (a ^ f.prefix[i]) * kPrime;
    for (uint32_t x = 0; x < 65536; ++x) {
        const uint32_t key = (((a ^ (x & 0xff)) * kPrime) ^ (x >> 8)) * kPrime;
        uint32_t i = Forward::slot(key);
        while (f.used[i]) i = (i + 1) & (Forward::kSlots - 1);
        f.used[i] = 1;
        f.keys[i] = key;
        f.xs[i] = static_cast<uint16_t>(x);
    }
    return f;
}
constexpr uint32_t kKeptSalts = 4;
std::mutex g_forward_mutex;
std::vector<std::unique_ptr<const Forward>> g_forward;  // salts 0 .. kKeptSalts-1, made when first needed

const Forward* forward_of(uint32_t salt, std::unique_ptr<const Forward>* scratch) {
    if (salt >= kKeptSalts) {
        *scratch = std::make_unique<const Forward>(make_forward(salt));
        return scratch->get();
    }
    std::lock_guard lock(g_forward_mutex);
    while (g_forward.size() <= salt) g_forward.push_back(std::make_unique<const Forward>(make_forward(static_cast<uint32_t>(g_forward.size()))));
    return g_forward[salt].get();
}

}  // namespace

std::array<uint8_t, 16> guid_with_id(uint32_t id) {
    // Each byte step of FNV-1a can be undone (the prime has an inverse mod 2^32): 12 fixed bytes forward, the last
    // four met in the middle (two bytes from each side). Some salt has a match (about one per salt is expected).
    for (uint32_t salt = 0; salt < 255; ++salt) {
        std::unique_ptr<const Forward> scratch;
        const Forward* f = forward_of(salt, &scratch);
        for (uint32_t y = 0; y < 65536; ++y) {
            const uint32_t back = (((id * kInverse) ^ (y >> 8)) * kInverse) ^ (y & 0xff);
            for (uint32_t i = Forward::slot(back); f->used[i]; i = (i + 1) & (Forward::kSlots - 1)) {
                if (f->keys[i] != back) continue;
                std::array<uint8_t, 16> b = f->prefix;
                b[12] = static_cast<uint8_t>(f->xs[i]);
                b[13] = static_cast<uint8_t>(f->xs[i] >> 8);
                b[14] = static_cast<uint8_t>(y);
                b[15] = static_cast<uint8_t>(y >> 8);
                if (fnv1a(b.data(), 16) == id) return b;
            }
        }
    }
    return {};  // not reached: some 16 bytes always have the hash
}

namespace {

struct Var {
    const char* name;
    uint32_t type;
    uint16_t size;
};
constexpr uint32_t kTypeBuffRef = 0x305916d9u;  // keen::BuffTypeReference, 16 bytes
constexpr uint32_t kTypeEntity = 0x737c8f86u;   // ecs EntityId, 4 bytes
constexpr uint32_t kTypeTime = 0xdfe4e404u;     // keen::Time, 8 bytes
constexpr uint32_t kTypeBool = 0xc894953du;

// keen::ImpactProgram (docs/kiln/services/abilities.md "contract"; Kiln.Abilities' ProgramBlob.Build): a 64-byte head
// {u32 id, guid[16], u16 stack size at 0x14, then {s32 self-relative, u32 count} arrays at 0x18 code, 0x20 shutdown,
// 0x28 layout, 0x30 data}, the layout's 24-byte ImpactVariable entries, their names, the zeroed data.
std::vector<uint8_t> blob(const uint8_t guid[16], const std::vector<uint32_t>& code, const std::vector<uint32_t>& shutdown,
                          const std::vector<Var>& vars) {
    size_t names = 0, data = 0;
    for (const Var& v : vars) {
        names += std::strlen(v.name);
        data += v.size;
    }
    const size_t code_off = 64, shut_off = code_off + code.size() * 4, layout_off = shut_off + shutdown.size() * 4,
                 names_off = layout_off + vars.size() * 24, data_off = names_off + names;
    std::vector<uint8_t> b((data_off + data + 3) & ~size_t{3}, 0);
    put(b, 0, fnv1a(guid, 16));
    std::memcpy(b.data() + 4, guid, 16);
    put<uint16_t>(b, 0x14, 256);
    auto arr = [&](size_t field, size_t target, size_t count) {
        put<int32_t>(b, field, count ? static_cast<int32_t>(target - field) : 0);
        put<uint32_t>(b, field + 4, static_cast<uint32_t>(count));
    };
    arr(0x18, code_off, code.size());
    arr(0x20, shut_off, shutdown.size());
    arr(0x28, layout_off, vars.size());
    arr(0x30, data_off, data);
    for (size_t i = 0; i < code.size(); ++i) put(b, code_off + i * 4, code[i]);
    for (size_t i = 0; i < shutdown.size(); ++i) put(b, shut_off + i * 4, shutdown[i]);
    size_t name_at = names_off, data_at = 0;
    for (size_t i = 0; i < vars.size(); ++i) {
        const size_t e = layout_off + i * 24, len = std::strlen(vars[i].name);
        put(b, e, fnv1a(reinterpret_cast<const uint8_t*>(vars[i].name), len));
        put(b, e + 8, vars[i].type);
        put<uint16_t>(b, e + 12, vars[i].size);
        put<uint16_t>(b, e + 14, static_cast<uint16_t>(data_at));
        put<int32_t>(b, e + 16, static_cast<int32_t>(name_at - (e + 16)));
        put<uint32_t>(b, e + 20, static_cast<uint32_t>(len));
        std::memcpy(b.data() + name_at, vars[i].name, len);
        name_at += len;
        data_at += vars[i].size;
    }
    return b;
}

constexpr uint32_t H(uint32_t index) { return 0xFFFF0000u | index; }
// opcodes (docs/re/abilities.md): ICONST 11, ICONST0 12, ICONST1 13, ECALL 19, GSTORE 24, POP 27, RVM 28, DSELF 29, HALT 30
void wait_then_end(std::vector<uint32_t>& code) {
    for (uint32_t i = 0; i < kWaitSteps; ++i) code.push_back(28);  // RVM, one a step: Kiln reads the answer meanwhile
    code.push_back(29);                                            // DSELF, HALT: nobody came
    code.push_back(30);
}

}  // namespace

std::vector<uint8_t> apply_program() {
    // applyBuff(Buff, Target, -, -, Duration) as Kiln.Abilities.ApplyBuff calls it; the native's answer byte ("out[8]:
    // applied") into Applied, then Done = 1
    std::vector<uint32_t> code = {11, H(kApplyVarDuration), 12, 12, 11, H(kApplyVarTarget), 11, H(kApplyVarBuff),
                                  19, 0x6b656d01u, kApplyBuffNative,
                                  24, H(kApplyVarApplied),
                                  13, 24, H(kApplyVarDone)};
    wait_then_end(code);
    return blob(kApplyProgram, code, {30},
                {{"Buff::value", kTypeBuffRef, 16}, {"Target::value", kTypeEntity, 4}, {"Duration::value", kTypeTime, 8},
                 {"Applied::value", kTypeBool, 1}, {"Done::value", kTypeBool, 1}});
}

std::vector<uint8_t> remove_program() {
    // removeBuff(WasRemoved out, Buff, Target) as Kiln.Abilities.RemoveBuff calls it, then Done = 1
    std::vector<uint32_t> code = {11, H(kRemoveVarTarget), 11, H(kRemoveVarBuff), 11, H(kRemoveVarWas),
                                  19, 0x6b656d02u, kRemoveBuffNative,
                                  27,
                                  13, 24, H(kRemoveVarDone)};
    wait_then_end(code);
    return blob(kRemoveProgram, code, {30},
                {{"Buff::value", kTypeBuffRef, 16}, {"Target::value", kTypeEntity, 4}, {"WasRemoved::value", kTypeBool, 1},
                 {"Done::value", kTypeBool, 1}});
}

Wait judge(bool world_running, bool impact_exists, int done, uint64_t age) {
    if (!world_running) return Wait::Closed;
    if (done == 1) return Wait::Answered;
    if (!impact_exists) return Wait::Gone;
    if (age > kTimeoutSteps) return Wait::TimedOut;
    return Wait::Waiting;
}

const char* wait_why(Wait w) {
    switch (w) {
        case Wait::Gone: return "the request's impact went away before its answer was read (canceled or destroyed)";
        case Wait::TimedOut: return "the game's buff program did not answer within 120 steps";
        case Wait::Closed: return "the server world closed before the request ran";
        default: return "";
    }
}

std::string apply_check(bool native_ok, const Row* rows, uint32_t n, uint32_t type, uint8_t slot, uint16_t faction_exclude,
                        int faction, uint32_t* buff_entity) {
    if (buff_entity) *buff_entity = 0;
    if (!native_ok) return "the game's applyBuff refused it";
    for (uint32_t i = 0; i < n; ++i)
        if (rows[i].type == type && rows[i].slot == slot && rows[i].entity && !rows[i].cleansed) {  // not a copy being removed
            if (buff_entity) *buff_entity = rows[i].entity;
            return {};
        }
    if (faction >= 0 && faction < 16 && ((faction_exclude >> faction) & 1u))
        return "the type is never given to the target's faction (" + std::to_string(faction) + ")";
    if (faction < 0) return "the game did not give it (the target's faction could not be read)";
    return "the game did not give it";
}

uint32_t removal_target(const Row* rows, uint32_t n, uint32_t type) {
    for (uint32_t i = 0; i < n; ++i)
        if (type && rows[i].type == type && !rows[i].cleansed) return rows[i].entity;
    return 0;
}

}  // namespace kemf::buffs

namespace kemf::view {

float heading_from_quat(float x, float y, float z, float w) {
    if (x == 0 && y == 0 && z == 0 && w == 0) return 0.0f;
    // forward = q * (0, 0, 1)
    const double fx = 2.0 * (static_cast<double>(x) * z + static_cast<double>(w) * y);
    const double fz = 1.0 - 2.0 * (static_cast<double>(x) * x + static_cast<double>(y) * y);
    if (!std::isfinite(fx) || !std::isfinite(fz) || (std::fabs(fx) < 1e-9 && std::fabs(fz) < 1e-9)) return 0.0f;
    double d = std::atan2(fx, fz) * 57.29577951308232;
    if (d < 0) d += 360.0;
    if (d >= 360.0) d -= 360.0;
    return static_cast<float>(d);
}

void copy_view(const EmfPlayerView& v, EmfPlayerView* out) {
    if (!out) return;
    const uint32_t n = out->size && out->size < sizeof v ? out->size : static_cast<uint32_t>(sizeof v);
    std::memcpy(out, &v, n);
    out->size = n;
}

}  // namespace kemf::view
