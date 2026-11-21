kiln_service(net net.cpp game.cpp lab.cpp)
target_link_libraries(kiln-net PRIVATE ws2_32)
# Steam peer-to-peer messages (steam_messages.h)
target_sources(kiln-net PRIVATE steam_messages.cpp)
# the stand-in steam_api64.dll for tests (tests/steam_standin), built only when named, never staged
add_subdirectory(tests/steam_standin)
