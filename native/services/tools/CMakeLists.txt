# SPDX-License-Identifier: MIT
# All tool hooks share the policy TLS in this DLL, never a separate hook module.
file(GLOB KILN_TOOL_HOOK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/hooks/*.cpp")
kiln_service(tools tools.cpp policy.cpp history.cpp ${KILN_TOOL_HOOK_SOURCES})
target_include_directories(kiln-tools PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
