kiln_service(repl repl.cpp loopback.cpp stats.cpp watch.cpp client.cpp net.cpp track.cpp filter.cpp touch.cpp remap.cpp messages.cpp
             schema.cpp)

# Host tests (no game), run under Wine: native/services/repl/tests/run.sh
#  - repl-host-test: the ABI layout and the fault rule of the observer detours
#  - repl-gate-test: the service's own start against a stand-in KilnApi: the lab functions' gate (policy, lab port)
add_executable(repl-host-test tests/repl_host_test.cpp)
target_include_directories(repl-host-test PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_SOURCE_DIR}/third_party")
set_target_properties(repl-host-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/repl-tests")
# - repl-track-test: kiln.repl.net's sidecar tracking as data (track.h): batch -> events, rings, gaps, the track table
add_executable(repl-track-test tests/repl_track_test.cpp)
target_include_directories(repl-track-test PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_SOURCE_DIR}/third_party")
set_target_properties(repl-track-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/repl-tests")
add_executable(repl-gate-test tests/repl_gate_test.cpp repl.cpp loopback.cpp stats.cpp watch.cpp client.cpp net.cpp track.cpp
               filter.cpp touch.cpp remap.cpp messages.cpp schema.cpp)
target_include_directories(repl-gate-test PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_SOURCE_DIR}/third_party")
set_target_properties(repl-gate-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/repl-tests")

# kiln.repl.net's pure snapshot step (snapcore.h): the remap over a component's sites, the list-1 mask test, a filter call
add_executable(repl-net-test tests/repl_net_test.cpp)
target_include_directories(repl-net-test PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_SOURCE_DIR}/third_party")
set_target_properties(repl-net-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/repl-tests")
#  - repl-watch-test: stats and watch bookkeeping against a stand-in kiln.hooks (refused hooks, ENDED frees a slot,
#    a revoked owner's watches, capture's file names)
add_executable(repl-watch-test tests/repl_watch_test.cpp repl.cpp loopback.cpp stats.cpp watch.cpp client.cpp)
target_include_directories(repl-watch-test PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_SOURCE_DIR}/third_party")
set_target_properties(repl-watch-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/repl-tests")
#  - repl-client-test: a game client's own receive counters (stats_enable / peer_stats on the client world's thread)
add_executable(repl-client-test tests/repl_client_test.cpp repl.cpp loopback.cpp stats.cpp watch.cpp client.cpp)
target_include_directories(repl-client-test PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_SOURCE_DIR}/third_party")
set_target_properties(repl-client-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/repl-tests")

# kiln.repl.net's message hooks and codec as data (msgcore.h): schema checks, channel rules, the dispatch site's register
# edits, the handler chain
add_executable(repl-msg-test tests/repl_msg_test.cpp)
target_include_directories(repl-msg-test PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_SOURCE_DIR}/third_party")
set_target_properties(repl-msg-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/repl-tests")
