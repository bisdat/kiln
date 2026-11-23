// SPDX-License-Identifier: MIT
// Argument/finite-value boundaries of the portable serializer. Full synthetic and local-game roundtrips are
// tools/kfc/tests/test_bodypack.py, using the same bodypack.cpp under ASan/UBSan in the package's host check.
#include "bodypack.h"

#include <array>
#include <cstdio>
#include <limits>

using namespace kiln::qb;
static int checks = 0, failed = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
int main() {
    BodySpec good;
    parse_guid("3cfc8711-f659-5b14-873d-430c16e9c536", &good.model);
    parse_guid("3c61aa14-09ef-590f-a662-c95fd33f0a36", &good.graph);
    good.radius = .75; good.height = 2;
    const Bytes input(24, 0);
    const auto refuses = [&](const BodySpec& spec, const char* field, size_t size = 24) {
        Bytes out{0x5a};
        std::string why;
        CHECK(!lower_body(input.data(), size, spec, &out, &why));
        CHECK(why.find(field) != std::string::npos);
        CHECK(out == Bytes{0x5a});
        CHECK(input == Bytes(24, 0));
    };
    for (double r : {0., -.1, .04999, 4.0001, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        auto s = good; s.radius = r; refuses(s, "body.size.radius");
    }
    for (double h : {0., -.1, .09999, 8.0001, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        auto s = good; s.height = h; refuses(s, "body.size.height");
    }
    for (double r : {.05, 4.}) for (double h : {.1, 8.}) {
        auto s = good; s.radius = r; s.height = h; refuses(s, "body.chassis");
    }
    auto s = good; s.model = {}; refuses(s, "body.model");
    s = good; s.graph = {}; refuses(s, "animation");
    refuses(good, "body.chassis", (64u << 20) + 1);  // rejects before reading beyond the real 24-byte buffer
    Bytes out{0x5a}; std::string why;
    CHECK(!lower_body(nullptr, 24, good, &out, &why));
    CHECK(out == Bytes{0x5a});
    CHECK(!lower_body(input.data(), 24, good, nullptr, &why));
    CHECK(!lower_body(nullptr, 0, good, &out, nullptr));
    std::printf("bodypack: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
