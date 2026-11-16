# SPDX-License-Identifier: MIT
# kiln.engine: exact-build engine profiles (sigdb/engine/, generated profiles.g.h) and Kiln-internal engine primitives.
kiln_service(engine engine.cpp identity.cpp facts_win.cpp)
target_include_directories(kiln-engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Stand-in Windows host for headless Wine (tests/wine.sh), built explicitly; never a game target or bundle file.
add_executable(engine-host EXCLUDE_FROM_ALL tests/windows_host.cpp)
target_include_directories(engine-host PRIVATE "${CMAKE_SOURCE_DIR}/core/abi")
