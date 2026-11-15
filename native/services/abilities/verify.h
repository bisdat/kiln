// SPDX-License-Identifier: MIT
// kiln.abilities: verifier for keen::ImpactProgram blobs before they enter the ImpactRegistry. The rules are the
// program/native contract in docs/kiln/services/abilities.md (agreed with tools/impact).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "services/kiln_abilities.h"

namespace kab {

struct VerifyResult {
    bool ok = false;
    std::string error;                    // why it was refused (empty when ok)
    std::vector<uint32_t> unknown_natives; // ECALL hashes nobody registered or declared (the program can wait)
    int max_depth = 0;                    // over code and shutdown
    std::vector<std::string> warnings;    // W_ARGTYPE: a Kiln native declared without argument types (since dev.29)
    uint32_t id = 0;
    uint8_t guid[16]{};
};

// argc_of(hash) -> argc of a known native, or -1 when unknown. Pure function of the bytes (thread-safe).
VerifyResult verify_program(const uint8_t* blob, size_t len, const std::function<int(uint32_t)>& argc_of);

// since 0.2.0-dev.29: what the verifier knows of a Kiln native's arguments. args/count: its typed declaration
// (native_declare_typed, a "sync" entry of a mod's natives.json); count 0 and untyped_kiln: a Kiln native declared by
// argc only (an older natives.json, native_declare) and not registered yet; neither: a vanilla native, a registered
// native without types, or none.
struct TypedArgs {
    const KilnNativeArg* args = nullptr;
    uint32_t count = 0;
    bool untyped_kiln = false;
};
// As above; a program whose ECALL of a typed Kiln native pushes a variable of another type or size, a raw value where a
// variable goes, or nothing (0) for a required argument is refused (E_ARGTYPE). A call of an argc-only Kiln native
// passes with a W_ARGTYPE warning. Only the values pushed right before the ECALL by ICONST/ICONST0/ICONST1 are known
// here; anything else is left to the thunk's check at run time.
VerifyResult verify_program(const uint8_t* blob, size_t len, const std::function<int(uint32_t)>& argc_of,
                            const std::function<TypedArgs(uint32_t)>& typed_of);

}  // namespace kab
