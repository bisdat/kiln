# SPDX-License-Identifier: MIT
# kiln-emf.dll: loads unmodified EMF mods (EMF 0.3.3 to 0.6.22, EmfApi v49) on Kiln. Deployed to <game>/kiln/plugins/.
# include/emf/emf_api.h is EMF's own header (MIT, EMF contributors; licenses/EMF-LICENSE.txt).
kiln_plugin(emf
    src/plugin.cpp
    src/mods.cpp
    src/settings.cpp
    src/api_core.cpp
    src/api_game.cpp
    src/api_spawn.cpp
    src/api_events.cpp
    src/api_hooks.cpp
    src/commands.cpp
    src/api_attributes.cpp
    src/api_level.cpp
    src/api_components.cpp
    src/api_windows.cpp
    src/api_wheel.cpp
    src/api_net.cpp
    src/api_net_client.cpp
    src/api_portraits.cpp
    src/api_terrain.cpp
    src/api_v40.cpp
    src/api_hits.cpp
    src/hits_model.cpp
    src/api_buffs.cpp
    src/api_view.cpp
    src/buffs_model.cpp
    src/net/net_service.cpp
    src/net/net_link.cpp
    src/net/net_line.cpp
    src/net/net_peers.cpp
    src/v26_model.cpp
    src/seh.cpp
    src/util.cpp
    src/io.cpp)
target_include_directories(kiln-emf PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                            "${CMAKE_SOURCE_DIR}/services/players")  # kiln_progression.h

# The conformance probe (an EMF mod, mods/tests/emf-test/probe): bin/emf-test/emf_probe.dll, for labs only
# (tools/emf/lab-check.py installs it into <game>/emf/mods/emf_probe/). Built against EMF's header alone.
add_library(emf-probe SHARED "${CMAKE_SOURCE_DIR}/../mods/tests/emf-test/probe/emf_probe.cpp")
target_include_directories(emf-probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(emf-probe PROPERTIES OUTPUT_NAME emf_probe RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/emf-test")

# Client-world probe (mods/tests/emf-client-test, since 0.2.0-dev.29): the client pass's programs.client-emf check, never
# packaged as a player mod. Installed by hand into <game>/emf/mods/emf_client_probe/ with its mod.json.
add_library(emf-client-probe SHARED "${CMAKE_SOURCE_DIR}/../mods/tests/emf-client-test/probe.cpp")
target_include_directories(emf-client-probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(emf-client-probe PROPERTIES OUTPUT_NAME emf_client_probe RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/emf-test")

# Mod messages conformance probe (mods/tests/emf-net-test): lab only, never packaged as a player mod.
add_library(emf-net-probe SHARED "${CMAKE_SOURCE_DIR}/../mods/tests/emf-net-test/probe.cpp")
target_include_directories(emf-net-probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(emf-net-probe PROPERTIES OUTPUT_NAME emf_net_probe RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/emf-test")

# Headless stand-in process for the separate GPL portrait DLL export (never starts the game).
add_executable(emf-portrait-bridge-test tests/portrait_wine.cpp)
set_target_properties(emf-portrait-bridge-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/emf-test")

# EmfApi v49 path/move refusals (including v47 and v48 callers) through the production api_v40.cpp; headless native Windows test.
add_executable(emf-path-test tests/path_test.cpp src/api_v40.cpp)
target_include_directories(emf-path-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_SOURCE_DIR}/services/players")
set_target_properties(emf-path-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/emf-test")

# v32 buffs and v35 player_view (0.2.0-dev.31.13): the production adapters with fake services and kiln.abilities' own
# program verifier; headless native Windows test (also tests/run.sh on Linux).
add_executable(emf-buffs-test tests/buffs_test.cpp src/buffs_model.cpp src/api_buffs.cpp src/api_view.cpp src/api_v40.cpp
    src/api_terrain.cpp "${CMAKE_SOURCE_DIR}/services/abilities/verify.cpp")
target_include_directories(emf-buffs-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_SOURCE_DIR}/services/players"
    "${CMAKE_SOURCE_DIR}/services/abilities")
set_target_properties(emf-buffs-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/emf-test")

# Production spawn adapter with fake services; headless native Windows regression test.
add_executable(emf-spawn-test tests/spawn_test.cpp src/api_spawn.cpp)
target_include_directories(emf-spawn-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_SOURCE_DIR}/services/players")
set_target_properties(emf-spawn-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/emf-test")

# Production terrain adapter with fake services; no game or engine code in the test.
add_executable(emf-terrain-test tests/terrain_test.cpp src/api_terrain.cpp)
target_include_directories(emf-terrain-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_SOURCE_DIR}/services/players")
set_target_properties(emf-terrain-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/emf-test")

# Headless actual plugin/Lua boot fixture: a fake core and no game engine.
add_executable(emf-bundled-lua-test tests/bundled_lua_wine.cpp)
target_include_directories(emf-bundled-lua-test PRIVATE "${CMAKE_SOURCE_DIR}/core/abi")
set_target_properties(emf-bundled-lua-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/emf-test")
