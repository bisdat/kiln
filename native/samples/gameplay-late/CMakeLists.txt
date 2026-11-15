# SPDX-License-Identifier: MIT
add_library(kiln-gameplay-late SHARED plugin.cpp)
target_include_directories(kiln-gameplay-late PRIVATE "${CMAKE_SOURCE_DIR}/core/abi")
set_target_properties(kiln-gameplay-late PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/samples/gameplay-late")
