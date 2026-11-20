// SPDX-License-Identifier: MIT
// Private core/host protocol. No Kiln API or third-party mod ABI changes.
#pragma once
#include <cstdint>

namespace kiln::first_pass {
enum class State { Expected, Running, Done, Bypass, Stopped };
enum class Decision { Wait, Complete, Bypass, Timeout, Cancel };
inline Decision decide(State state, uint64_t started, uint64_t now, uint32_t max_wait_ms) {
    if (state == State::Stopped) return Decision::Cancel;
    if (state == State::Done) return Decision::Complete;
    if (state == State::Bypass) return Decision::Bypass;
    if (state == State::Running && now >= started && now - started >= max_wait_ms) return Decision::Timeout;
    return Decision::Wait;
}
}  // namespace kiln::first_pass
