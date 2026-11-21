// SPDX-License-Identifier: MIT
#include "world_read.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace kmk {

bool position_ok(const KilnVec3& p) {
    auto in = [](double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    return in(p.x, -1000, 12000) && in(p.z, -1000, 12000) && in(p.y, -1000, 5000);
}

KilnVec3 fixed_to_metres(const int64_t p[3]) {
    constexpr double k = 1.0 / 4294967296.0;  // 2^-32
    return KilnVec3{static_cast<double>(p[0]) * k, static_cast<double>(p[1]) * k, static_cast<double>(p[2]) * k};
}

TransformRead decode_transform(const void* data, uint64_t size, const TransformSource& src) {
    TransformRead r;
    if (!data) return r;  // Absent
    if (size != src.size) {
        r.kind = ReadKind::WrongSize;
        return r;
    }
    int64_t fixed[3];
    std::memcpy(fixed, data, sizeof fixed);
    r.position = fixed_to_metres(fixed);
    r.kind = position_ok(r.position) ? ReadKind::Ok : ReadKind::BadPosition;
    return r;
}

namespace {

constexpr uint32_t kCopyMax = 176;  // the larger of the two transforms

// No frame here holds an object with a destructor (as __try requires). False: the call or the copy faulted.
bool guarded_copy(GetComponentFn fn, uintptr_t world, uint32_t hash, uint32_t entity, uint32_t want, uint8_t* buf, bool* present,
                  uint64_t* size) {
#ifdef _WIN32
    __try {
        ComponentSlice s{nullptr, 0};
        fn(&s, world, hash, entity);
        *present = s.data != nullptr;
        *size = s.size;
        if (s.data && s.size == want) std::memcpy(buf, s.data, want);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    try {  // the host tests' stand-in for a fault: a fake that throws
        ComponentSlice s{nullptr, 0};
        fn(&s, world, hash, entity);
        *present = s.data != nullptr;
        *size = s.size;
        if (s.data && s.size == want) std::memcpy(buf, s.data, want);
        return true;
    } catch (...) {
        return false;
    }
#endif
}

}  // namespace

TransformRead read_transform(GetComponentFn fn, uintptr_t world, uint32_t world_kind, uint32_t entity) {
    TransformRead r;
    if (!fn || !world || !entity) return r;
    const TransformSource& src = transform_source(world_kind);
    uint8_t buf[kCopyMax];
    bool present = false;
    uint64_t size = 0;
    if (!guarded_copy(fn, world, src.hash, entity, src.size, buf, &present, &size)) {
        r.kind = ReadKind::Fault;
        return r;
    }
    if (!present) return r;
    return decode_transform(buf, size, src);
}

// ---- the layout check -------------------------------------------------------------------------------------------------

namespace {

std::string_view sv(KilnStr s) { return s.ptr ? std::string_view(s.ptr, s.len) : std::string_view(); }

bool info(const KilnTypesApi* t, const KilnType* type, KilnTypeInfo* out) {
    *out = KilnTypeInfo{};
    out->size = sizeof *out;
    return type && t->type_info(type, out) == KILN_OK;
}

// What a typedef names (a few levels), else the type itself.
const KilnType* through_typedefs(const KilnTypesApi* t, const KilnType* type) {
    for (int i = 0; type && i < 8; ++i) {
        KilnTypeInfo ti{};
        if (!info(t, type, &ti) || ti.kind != KILN_TYPE_TYPEDEF || !ti.inner) return type;
        type = ti.inner;
    }
    return type;
}

// A field of a struct or of its base classes (the struct's inner), offset from the struct's start.
bool field(const KilnTypesApi* t, const KilnType* type, std::string_view name, const KilnType** ftype, uint64_t* off) {
    for (int depth = 0; type && depth < 16; ++depth) {
        type = through_typedefs(t, type);
        KilnTypeInfo ti{};
        if (!info(t, type, &ti)) return false;
        for (uint32_t i = 0; i < ti.field_count; ++i) {
            KilnTypeField f{};
            if (t->type_field(type, i, &f) == KILN_OK && sv(f.name) == name) {
                *ftype = f.type;
                *off = f.offset;
                return true;
            }
        }
        if (ti.kind != KILN_TYPE_STRUCT) return false;
        type = ti.inner;
    }
    return false;
}

struct Want {
    const char* name;
    uint64_t offset;
    uint32_t size;
    uint8_t kind;  // 0: any
};

bool fields_are(const KilnTypesApi* t, const KilnType* type, const char* owner, const Want* w, size_t n, std::string* why) {
    for (size_t i = 0; i < n; ++i) {
        const KilnType* ft = nullptr;
        uint64_t off = 0;
        char line[200];
        if (!field(t, type, w[i].name, &ft, &off)) {
            std::snprintf(line, sizeof line, "%s has no field %s", owner, w[i].name);
            return *why = line, false;
        }
        KilnTypeInfo fi{};
        if (!info(t, through_typedefs(t, ft), &fi)) {
            std::snprintf(line, sizeof line, "%s.%s: kiln.types has no type for it", owner, w[i].name);
            return *why = line, false;
        }
        if (off != w[i].offset || fi.type_size != w[i].size || (w[i].kind && fi.kind != w[i].kind)) {
            std::snprintf(line, sizeof line, "%s.%s is at +%llu with %u bytes (kind %u), expected +%llu with %u bytes", owner, w[i].name,
                          static_cast<unsigned long long>(off), fi.type_size, fi.kind, static_cast<unsigned long long>(w[i].offset),
                          w[i].size);
            return *why = line, false;
        }
    }
    return true;
}

// keen::WorldTransform (56 bytes): position (three s64) +0, orientation +24, scale +40.
bool world_transform_is(const KilnTypesApi* t, const KilnType* wt, const char* owner, std::string* why) {
    KilnTypeInfo ti{};
    char line[200];
    if (!info(t, through_typedefs(t, wt), &ti) || ti.type_size != 56) {
        std::snprintf(line, sizeof line, "%s is not a 56-byte WorldTransform (%u bytes)", owner, ti.type_size);
        return *why = line, false;
    }
    static const Want kTransform[] = {{"position", 0, 24, 0}, {"orientation", 24, 16, 0}, {"scale", 40, 12, 0}};
    if (!fields_are(t, wt, owner, kTransform, 3, why)) return false;
    const KilnType* pos = nullptr;
    uint64_t off = 0;
    field(t, wt, "position", &pos, &off);
    static const Want kPosition[] = {{"x", 0, 8, KILN_TYPE_S64}, {"y", 8, 8, KILN_TYPE_S64}, {"z", 16, 8, KILN_TYPE_S64}};
    const std::string owner_pos = std::string(owner) + ".position";
    return fields_are(t, pos, owner_pos.c_str(), kPosition, 3, why);
}

}  // namespace

bool layout_check(const KilnTypesApi* t, uint32_t world_kind, std::string* why) {
    if (!t || t->header.size < offsetof(KilnTypesApi, type_enum) + sizeof(t->type_enum) || !t->type_by_hash || !t->type_info ||
        !t->type_field)
        return *why = "kiln.types (the game's reflection) is not loaded", false;
    const TransformSource& src = transform_source(world_kind);
    const KilnType* type = t->type_by_hash(src.hash);
    KilnTypeInfo ti{};
    char line[200];
    if (!type || !info(t, type, &ti)) {
        std::snprintf(line, sizeof line, "the game has no reflected type with hash 0x%08x (%s)", src.hash, src.name);
        return *why = line, false;
    }
    if (sv(ti.qualified_name) != src.name || ti.hash != src.hash || ti.type_size != src.size) {
        std::snprintf(line, sizeof line, "hash 0x%08x is %.*s with %u bytes, expected %s with %u bytes", src.hash,
                      static_cast<int>(ti.qualified_name.len), ti.qualified_name.ptr ? ti.qualified_name.ptr : "", ti.type_size,
                      src.name, src.size);
        return *why = line, false;
    }
    const KilnType* wt = nullptr;
    uint64_t off = 0;
    if (!field(t, type, "transform", &wt, &off) || off != 0) {
        std::snprintf(line, sizeof line, "%s has no WorldTransform 'transform' at +0", src.name);
        return *why = line, false;
    }
    const std::string owner = std::string(src.name) + ".transform";
    if (!world_transform_is(t, wt, owner.c_str(), why)) return false;
    if (world_kind == KILN_WORLD_CLIENT) {
        static const Want kRender[] = {{"interpolationSource", 56, 56, 0},
                                       {"interpolationTarget", 112, 56, 0},
                                       {"interpolationSourceTick", 168, 4, 0},
                                       {"interpolationNewestTick", 172, 4, 0}};
        if (!fields_are(t, type, src.name, kRender, 4, why)) return false;
    }
    return true;
}

}  // namespace kmk
