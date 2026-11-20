# SPDX-License-Identifier: MIT
# Shared code of the compatibility plugins for other mod loaders (compat.h) and their host tests. Builds no plugin.
# tests/run.py drives the test host under plain Wine:
#   compathost.exe       a stand-in for the game and kiln-core (minimal KilnApi, kiln.profile, test targets in .text)
#   greeneye_test.dll    a Greeneye mod that checks every GreeneyeHost function (gbad.dll: one whose hook is refused)
#   flame_test.dll       a Flame mod (flame_fault.dll: one whose FlameStartRuntime faults)
#   shroudtopia_test.dll a Shroudtopia mod that checks the whole ModContext (shroudtopia_client.dll: client only;
#                        built with the DLL CRT, as Shroudtopia's own mods are, to cross the CRT boundary)
#   greeneye_lab.dll, shroudtopia_lab.dll  probes for a lab dedicated server (read-only; tests/lab.sh)
#   dummy.dll            an empty DLL (fakeflame.dll: one that carries the text of Flame's entry point, as its loader does)
# Everything goes to bin/compat-tests/.
set(CT_OUT "${CMAKE_BINARY_DIR}/bin/compat-tests")
add_executable(compathost tests/compathost.cpp)
target_include_directories(compathost PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_SOURCE_DIR}/third_party")
target_link_options(compathost PRIVATE /ENTRY:wmainCRTStartup)
target_link_libraries(compathost PRIVATE kiln-safetyhook)
set_target_properties(compathost PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CT_OUT}")

function(compat_test_dll name src)
    add_library(compat-${name} SHARED tests/${src})
    target_compile_definitions(compat-${name} PRIVATE ${ARGN})
    set_target_properties(compat-${name} PROPERTIES OUTPUT_NAME ${name} RUNTIME_OUTPUT_DIRECTORY "${CT_OUT}")
    add_dependencies(compathost compat-${name})
endfunction()
compat_test_dll(greeneye_test greeneye_test.cpp)
compat_test_dll(gbad greeneye_test.cpp GT_BAD)
compat_test_dll(greeneye_lab greeneye_test.cpp GT_LAB)
target_include_directories(compat-greeneye_lab PRIVATE "${CMAKE_SOURCE_DIR}/plugins/greeneye/include")
target_include_directories(compat-greeneye_test PRIVATE "${CMAKE_SOURCE_DIR}/plugins/greeneye/include")
target_include_directories(compat-gbad PRIVATE "${CMAKE_SOURCE_DIR}/plugins/greeneye/include")
compat_test_dll(flame_test flame_test.cpp)
compat_test_dll(flame_fault flame_test.cpp FT_FAULT)
compat_test_dll(flame_dllmain_fault flame_test.cpp FT_DLLMAIN_FAULT)
compat_test_dll(flame_slow flame_test.cpp FT_SLOW)
compat_test_dll(flame_load_loader flame_test.cpp FT_LOAD_LOADER)
compat_test_dll(shroudtopia_test shroudtopia_test.cpp)
compat_test_dll(shroudtopia_client shroudtopia_test.cpp ST_CLIENT_ONLY)
compat_test_dll(shroudtopia_lab shroudtopia_test.cpp ST_LAB)
foreach(t shroudtopia_test shroudtopia_client shroudtopia_lab)
    target_include_directories(compat-${t} PRIVATE "${CMAKE_SOURCE_DIR}/plugins/shroudtopia/include")
    target_compile_definitions(compat-${t} PRIVATE _ITERATOR_DEBUG_LEVEL=0)
endforeach()
set_target_properties(compat-shroudtopia_test PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
compat_test_dll(dummy dummy.cpp)
compat_test_dll(fakeflame dummy.cpp FAKE_FLAME)
compat_test_dll(flame_dependency flame_test.cpp FT_DEPENDENCY)
target_link_libraries(compat-flame_dependency PRIVATE compat-fakeflame)
compat_test_dll(fakeshroud dummy.cpp FAKE_SHROUD)
# The original-code scenario (inbox 26c): st_native.dll entry-hooks compathost's st_text_target first (as Auto Doors);
# istext_named.dll / istext_other.dll are Shroudtopia mods shaped like Item Skill Descriptions (memcmp from vcruntime140,
# an inline hook of their own), the first named by the scenario's compat rule. Their hook library is safetyhook, built
# here with the DLL CRT (kiln-safetyhook uses the static one). istext_exec.dll: see below.
compat_test_dll(st_native st_native.cpp)
target_link_libraries(compat-st_native PRIVATE kiln-safetyhook)
add_library(compat-safetyhook-md STATIC "${CMAKE_SOURCE_DIR}/third_party/safetyhook/safetyhook.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/safetyhook/Zydis.c")
target_include_directories(compat-safetyhook-md PUBLIC "${CMAKE_SOURCE_DIR}/third_party/safetyhook")
target_compile_definitions(compat-safetyhook-md PUBLIC ZYDIS_STATIC_BUILD ZYCORE_STATIC_BUILD)
target_compile_options(compat-safetyhook-md PRIVATE -w)
set_target_properties(compat-safetyhook-md PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
compat_test_dll(istext_named istext_test.cpp)
compat_test_dll(istext_other istext_test.cpp IST_OTHER)
foreach(t istext_named istext_other)
    target_link_libraries(compat-${t} PRIVATE compat-safetyhook-md)
endforeach()
# istext_exec.dll: the import table on an executable page (.rdata merged into .text), a thread running code there while
# kiln-shroudtopia rewrites the memcmp slot (review of 09a2332b, finding 1).
compat_test_dll(istext_exec istext_exec_test.cpp)
target_link_options(compat-istext_exec PRIVATE /MERGE:.rdata=.text)
foreach(t istext_named istext_other istext_exec)
    target_include_directories(compat-${t} PRIVATE "${CMAKE_SOURCE_DIR}/plugins/shroudtopia/include")
    target_compile_definitions(compat-${t} PRIVATE _ITERATOR_DEBUG_LEVEL=0)
    set_target_properties(compat-${t} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endforeach()
add_dependencies(compathost kiln-greeneye kiln-flame kiln-shroudtopia)

# deploy-server.sh and the packager build every native/plugins/<dir> as kiln-<dir>: here that is the host tests (no
# plugin DLL comes out of this folder).
add_custom_target(kiln-compat DEPENDS compathost)
