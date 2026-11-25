kiln_service(net net.cpp game.cpp lab.cpp session_io.cpp outbound.cpp)  # session_io, outbound: portable (tests/test-net-host.sh)
target_link_libraries(kiln-net PRIVATE ws2_32 bcrypt)  # bcrypt: random Steam lease handles (steam_messages.cpp)
# Steam peer-to-peer messages (steam_messages.h)
target_sources(kiln-net PRIVATE steam_messages.cpp)
# the stand-in steam_api64.dll for tests (tests/steam_standin), built only when named, never staged
add_subdirectory(tests/steam_standin)
