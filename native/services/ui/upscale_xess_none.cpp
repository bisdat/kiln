// SPDX-License-Identifier: MIT
// kiln.ui upscaler swap: no XeSS backend in this build (upscale_xess.cpp is not here). CMakeLists.txt builds this file
// only while upscale_xess.cpp is missing; Kiln then says XeSS is unavailable and the game's FSR 3 runs.
#include "upscale_backend.h"

namespace kui::upscale {
std::unique_ptr<Backend> make_xess_backend() { return nullptr; }
}  // namespace kui::upscale
