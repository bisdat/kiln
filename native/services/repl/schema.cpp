// SPDX-License-Identifier: MIT
// kiln.repl.net's view of the game's message codec (kiln_repl.h schema_find / schema_fields / schema_decode /
// schema_encode; the dossier's chapter 15 §4 M4 "Codec"; DESIGN slice C1). Kiln calls the game's own SchemaEncode and
// SchemaDecode (no port): a schema is found by name in the exe's data (records 0x28 bytes apart, msgcore.h), checked
// with msgcore's validate() (every type, shape and element known and consistent, every field inside the struct; a
// schema that fails is refused, never guessed), and coded through a BitCursor over the caller's buffer, under a guard.
// The scan runs at the first lookup, not at load: some schema pointers and enum bit counts are filled in by the game's
// static initializers. Decode goes through a scratch with slack (the game's string case writes its NUL one byte past a
// full char[fixed] on malformed input).
#include <windows.h>

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "msgcore.h"
#include "repl.h"

namespace repl {
namespace {

struct BitCursor {             // the game's bit cursor (0x30 bytes)
    uint64_t parent;
    uint8_t* data;
    uint64_t capacity_bits;
    uint64_t position_bits;
    uint64_t zero;
    uint8_t error;
    uint8_t pad[7];
};
static_assert(sizeof(BitCursor) == 0x30);
using EncodeFn = void (*)(BitCursor* cursor, const void* msg, const void* schema);
using DecodeFn = bool (*)(void* out, BitCursor* cursor, const void* schema);

bool g_resolved = false;
uintptr_t f_encode = 0, f_decode = 0;
std::once_flag g_scan_once;
std::mutex g_mu;  // the map (filled once)
std::unordered_map<std::string, uintptr_t> g_by_name;
std::unordered_map<uintptr_t, bool> g_valid;  // schema -> validate() passed (refused ones: false)
uint32_t g_found = 0, g_refused = 0;

struct Range {
    uintptr_t lo = 0, hi = 0;
};
bool image_ranges(Range* image, Range* data) {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) return false;
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        image->lo = base;
        image->hi = base + nt->OptionalHeader.SizeOfImage;
        const auto* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            if (std::memcmp(sec[i].Name, ".data", 6) == 0) {
                data->lo = base + sec[i].VirtualAddress;
                data->hi = data->lo + sec[i].Misc.VirtualSize;
            }
        return data->lo != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool seh_validate(const uint8_t* s, char* why, size_t cap) {
    __try {
        return msg::validate(s, why, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(why, cap, "faulted while checking");
        return false;
    }
}

// The records in .data that look like schemas (no C++ object here: the loop runs under SEH). Returns how many.
constexpr size_t kMaxRecords = 4096;
uintptr_t g_records[kMaxRecords];
size_t seh_scan(Range image, Range data, bool* faulted) {
    size_t n = 0;
    *faulted = false;
    __try {
        for (uintptr_t p = data.lo; p + 0x28 <= data.hi && n < kMaxRecords; p += 8) {
            const auto* rec = reinterpret_cast<const uint8_t*>(p);
            if (!msg::schema_record_shape(rec, image.lo, image.hi)) continue;
            if (!msg::schema_name_ok(*reinterpret_cast<const char* const*>(rec), 128)) continue;
            g_records[n++] = p;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
    }
    return n;
}

void scan() {
    Range image, data;
    if (!image_ranges(&image, &data)) {
        logf(KILN_LOG_WARN, "repl: schema scan: the exe's sections could not be read");
        return;
    }
    bool faulted = false;
    const size_t n = seh_scan(image, data, &faulted);
    if (faulted) logf(KILN_LOG_WARN, "repl: schema scan faulted after %zu records", n);
    std::vector<std::pair<std::string, uintptr_t>> found;
    for (size_t i = 0; i < n; ++i) found.emplace_back(std::string(*reinterpret_cast<const char* const*>(g_records[i])), g_records[i]);
    std::lock_guard lock(g_mu);
    for (auto& [name, s] : found) {
        char why[160] = {};
        const bool ok = seh_validate(reinterpret_cast<const uint8_t*>(s), why, sizeof why);
        g_valid[s] = ok;
        g_by_name.emplace(name, s);  // the first record of a name wins
        ++(ok ? g_found : g_refused);
    }
    logf(KILN_LOG_INFO, "repl: message schemas: %u the codec can take, %u refused (a type or shape Kiln does not know)", g_found, g_refused);
}

bool valid(uint64_t schema) {
    std::call_once(g_scan_once, scan);
    std::lock_guard lock(g_mu);
    auto it = g_valid.find(static_cast<uintptr_t>(schema));
    return it != g_valid.end() && it->second;
}

bool seh_decode(void* out, BitCursor* c, const void* s) {
    __try {
        return reinterpret_cast<DecodeFn>(f_decode)(out, c, s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool seh_encode(BitCursor* c, const void* msg, const void* s) {
    __try {
        reinterpret_cast<EncodeFn>(f_encode)(c, msg, s);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
size_t seh_strnlen(const char* p, size_t max) {
    if (!p) return 0;
    __try {
        return strnlen(p, max);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;  // a bogus handle from a native caller: no name
    }
}
bool seh_counts(const uint8_t* s, const uint8_t* msg) {
    __try {
        return msg::counts_ok(s, msg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool schema_check(const char* name, const uint8_t* data, uint32_t length) {
    if (!g_resolved || !name || !data || !length) return false;
    uint32_t size = 0;
    uint64_t schema = 0;
    if (t_schema_find(KilnStr{name, std::strlen(name)}, &size, &schema) != KILN_OK || !size) return false;
    std::vector<uint8_t> out(size + 16, 0);
    return t_schema_decode(schema, data, length, out.data(), size) == KILN_OK;
}

void schema_start() {
    auto sym = [](const char* name, uintptr_t* out) { return g_api->symbol(KilnStr{name, std::strlen(name)}, out) == KILN_OK; };
    g_resolved = g_api->feature(KilnStr{"repl.schema", 11}) && sym("repl/SchemaEncode", &f_encode) && sym("repl/SchemaDecode", &f_decode);
    logf(KILN_LOG_INFO, "repl: message codec %s", g_resolved ? "on (the game's own SchemaEncode / SchemaDecode; schemas found at first use)"
                                                           : "off (sigdb feature repl.schema is off on this build)");
}

uint32_t schema_caps() { return g_resolved ? KILN_REPL_NET_CAP_SCHEMA : 0u; }

KilnResult KILN_CALL t_schema_find(KilnStr name, uint32_t* out_size, uint64_t* out_schema) {
    if (out_size) *out_size = 0;
    if (out_schema) *out_schema = 0;
    if (!g_resolved) return KILN_ERR_UNAVAILABLE;
    if (!name.ptr || !name.len || name.len > 128) return KILN_ERR_INVALID_ARGUMENT;
    std::call_once(g_scan_once, scan);
    uintptr_t s = 0;
    bool ok = false;
    {
        std::lock_guard lock(g_mu);
        auto it = g_by_name.find(std::string(name.ptr, name.len));
        if (it == g_by_name.end()) return KILN_ERR_NOT_FOUND;
        s = it->second;
        ok = g_valid[s];
    }
    if (!ok) return KILN_ERR_UNAVAILABLE;  // a type or shape Kiln does not know: refused, never guessed
    if (out_size) *out_size = read<uint32_t>(s + msg::kSchemaSize);
    if (out_schema) *out_schema = s;
    return KILN_OK;
}

KilnResult KILN_CALL t_schema_fields(uint64_t schema, KilnReplSchemaField* out, uint32_t cap, uint32_t* count) {
    if (count) *count = 0;
    if (!g_resolved) return KILN_ERR_UNAVAILABLE;
    if (!count || (cap && !out) || !schema) return KILN_ERR_INVALID_ARGUMENT;
    // a nested schema (a field's 'nested') need not be a named record: it is checked when its parent was
    std::call_once(g_scan_once, scan);
    const uintptr_t s = static_cast<uintptr_t>(schema);
    const uint64_t n = read<uint64_t>(s + msg::kSchemaCount);
    const uintptr_t fields = read<uintptr_t>(s + msg::kSchemaFields);
    if (!n || n > msg::kMaxFields || !fields) return KILN_ERR_INVALID_ARGUMENT;
    for (uint64_t i = 0; i < n && i < cap; ++i) {
        const uintptr_t f = fields + i * msg::kFieldStride;
        KilnReplSchemaField& o = out[i];
        o = KilnReplSchemaField{};
        const char* nm = read<const char*>(f + msg::kFieldName);
        const size_t nlen = seh_strnlen(nm, 128);
        o.name = KilnStr{nlen ? nm : nullptr, nlen};
        o.type = read<uint32_t>(f + msg::kFieldType);
        o.element = read<uint32_t>(f + msg::kFieldElement);
        o.shape = read<uint32_t>(f + msg::kFieldShape);
        o.offset = static_cast<uint32_t>(read<uint64_t>(f + msg::kFieldOffset));
        o.count_offset = static_cast<uint32_t>(read<uint64_t>(f + msg::kFieldCountOff));
        o.fixed_count = static_cast<uint32_t>(read<uint64_t>(f + msg::kFieldFixed));
        o.nested = o.type == 12 ? read<uint64_t>(f + msg::kFieldNested) : 0;  // an enum's descriptor is not a schema
    }
    *count = static_cast<uint32_t>(n);
    return n > cap ? KILN_ERR_BUFFER_TOO_SMALL : KILN_OK;
}

KilnResult KILN_CALL t_schema_decode(uint64_t schema, const void* bytes, uint32_t length, void* out, uint32_t out_size) {
    if (!g_resolved) return KILN_ERR_UNAVAILABLE;
    if (!schema || (!bytes && length) || !out || length > msg::kMaxMessage) return KILN_ERR_INVALID_ARGUMENT;
    if (!valid(schema)) return KILN_ERR_UNAVAILABLE;
    const uint32_t size = read<uint32_t>(static_cast<uintptr_t>(schema) + msg::kSchemaSize);
    if (out_size < size) return KILN_ERR_BUFFER_TOO_SMALL;
    std::vector<uint8_t> in(static_cast<const uint8_t*>(bytes), static_cast<const uint8_t*>(bytes) + length);
    in.resize(length + 8, 0);  // the cursor may read a whole word past the end
    std::vector<uint8_t> scratch(size + 16, 0);  // slack: the game's string case can write one byte past a full field
    BitCursor c{0, in.data(), uint64_t{length} * 8, 0, 0, 0, {}};
    if (!seh_decode(scratch.data(), &c, reinterpret_cast<const void*>(schema)) || c.error) return KILN_ERR_INVALID_ARGUMENT;
    std::memset(out, 0, out_size);
    std::memcpy(out, scratch.data(), size);
    return KILN_OK;
}

KilnResult KILN_CALL t_schema_encode(uint64_t schema, const void* msgp, uint32_t msg_size, void* out, uint32_t cap,
                                     uint32_t* out_length) {
    if (out_length) *out_length = 0;
    if (!g_resolved) return KILN_ERR_UNAVAILABLE;
    if (!schema || !msgp || !out || !cap || !out_length) return KILN_ERR_INVALID_ARGUMENT;
    if (!valid(schema)) return KILN_ERR_UNAVAILABLE;
    const uint32_t size = read<uint32_t>(static_cast<uintptr_t>(schema) + msg::kSchemaSize);
    if (msg_size < size) return KILN_ERR_INVALID_ARGUMENT;
    if (!seh_counts(reinterpret_cast<const uint8_t*>(schema), static_cast<const uint8_t*>(msgp))) return KILN_ERR_INVALID_ARGUMENT;
    std::vector<uint8_t> buf((cap + 7) & ~7u, 0);
    BitCursor c{0, buf.data(), uint64_t{cap} * 8, 0, 0, 0, {}};
    if (!seh_encode(&c, msgp, reinterpret_cast<const void*>(schema))) return KILN_ERR_INVALID_ARGUMENT;
    if (c.error || c.position_bits > uint64_t{cap} * 8) return KILN_ERR_BUFFER_TOO_SMALL;
    const uint32_t n = msg::cursor_bytes(c.position_bits);
    if (n > cap) return KILN_ERR_BUFFER_TOO_SMALL;
    std::memcpy(out, buf.data(), n);
    *out_length = n;
    return KILN_OK;
}

}  // namespace repl
