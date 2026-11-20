# SPDX-License-Identifier: MIT
# Dedicated-server lab for the native station calls. Built with the rest of native/; not run here.
add_library(station-native-test SHARED mod.cpp)
target_include_directories(station-native-test PRIVATE "${CMAKE_SOURCE_DIR}/core/abi")
set_target_properties(station-native-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/mods/station-native-test")
configure_file(native.json "${CMAKE_BINARY_DIR}/bin/mods/station-native-test/native.json" COPYONLY)
