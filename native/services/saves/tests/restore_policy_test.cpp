// SPDX-License-Identifier: MIT
// kiln.saves' staged restore: which sidecar files already in place it keeps (restore_policy.h).
#include "../restore_policy.h"
#include <cstdio>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); failures += !ok; };
    using std::filesystem::path;
    check(ksaves::restore_keeps(path("blobs") / "9f" / "9f3c.json", true, 120, 120),
          "a blob already there with the same size is the same file (content-addressed): kept");
    check(!ksaves::restore_keeps(path("blobs") / "9f" / "9f3c.json", true, 119, 120), "a blob of another size is replaced");
    check(!ksaves::restore_keeps(path("blobs") / "9f" / "9f3c.json", false, 0, 120), "a missing blob is copied");
    check(!ksaves::restore_keeps(path("manifests") / "0000000002.pending.json", true, 512, 512),
          "a pending manifest of the same revision and size (another timeline's) is replaced by the backup's");
    check(!ksaves::restore_keeps(path("manifests") / "0000000002-1002.json", true, 512, 512),
          "a confirmed manifest of the same name and size is replaced too");
    check(!ksaves::restore_keeps(path("blobs.json"), true, 10, 10), "only the blobs folder is content-addressed");
    return failures ? 1 : 0;
}
