// SPDX-License-Identifier: MIT
// A tiny check helper for kiln.markers' host tests: CHECK records a failure with its line and goes on; a case prints
// "markers-test: PASS <case>" only when it saw no failure, else FAIL with the count, and main returns 1 on any failure.
#ifndef KILN_MARKERS_TEST_CHECK_H
#define KILN_MARKERS_TEST_CHECK_H
#include <cstdio>

namespace kmk_test {
inline int g_failures = 0;
inline int g_case_failures = 0;
inline int g_passed = 0;

inline void begin() { g_case_failures = 0; }
inline void end(const char* name) {
    if (g_case_failures == 0) {
        std::printf("markers-test: PASS %s\n", name);
        ++g_passed;
    } else {
        std::printf("markers-test: FAIL %s (%d failed checks)\n", name, g_case_failures);
    }
}
inline int finish(const char* suite) {
    std::printf("%s: %d cases passed, %d checks failed\n", suite, g_passed, g_failures);
    return g_failures ? 1 : 0;
}
}  // namespace kmk_test

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("  check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            ++kmk_test::g_failures;                                                 \
            ++kmk_test::g_case_failures;                                            \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                     \
    do {                                                                                                                   \
        const auto va_ = (a);                                                                                              \
        const auto vb_ = (b);                                                                                              \
        if (!(va_ == vb_)) {                                                                                               \
            std::printf("  check failed: %s == %s (%lld vs %lld) (%s:%d)\n", #a, #b, static_cast<long long>(va_),          \
                        static_cast<long long>(vb_), __FILE__, __LINE__);                                                  \
            ++kmk_test::g_failures;                                                                                        \
            ++kmk_test::g_case_failures;                                                                                   \
        }                                                                                                                  \
    } while (0)

#endif
