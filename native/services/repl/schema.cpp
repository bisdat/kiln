// SPDX-License-Identifier: MIT
// kiln.repl.net's view of the game's message codec (repl.h "schema.cpp").
// Not built yet (package 15 F0): every function answers KILN_ERR_UNAVAILABLE and the capability bits stay 0.
#include "repl.h"

namespace repl {

void schema_start() {}
uint32_t schema_caps() { return 0; }
KilnResult KILN_CALL t_schema_find(KilnStr, uint32_t* out_size, uint64_t* out_schema) {
    if (out_size) *out_size = 0;
    if (out_schema) *out_schema = 0;
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL t_schema_fields(uint64_t, KilnReplSchemaField*, uint32_t, uint32_t* count) {
    if (count) *count = 0;
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL t_schema_decode(uint64_t, const void*, uint32_t, void*, uint32_t) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL t_schema_encode(uint64_t, const void*, uint32_t, void*, uint32_t, uint32_t* out_length) {
    if (out_length) *out_length = 0;
    return KILN_ERR_UNAVAILABLE;
}

}  // namespace repl
