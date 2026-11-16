// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <filesystem>
#include <iterator>

namespace ksaves {
// A staged restore (saves.cpp apply_staged_restore) copies the backup's sidecar files, relative to
// kiln/data/worlds/<id>/, over what is there; each copy is atomic (a temp name, then a write-through rename). True: the
// file already there is kept. Only blobs are content-addressed (blobs/ab/<sha256>.json: the name is the hash of the
// bytes), so a blob already there with the same size is the same file. A manifest's name is only its revision (and file
// time): one already there with the same size can be another timeline's (a pending manifest of the same revision staged
// after an older world file was restored, review 2), and a restore that kept it would load that one. Manifests are
// always replaced.
inline bool restore_keeps(const std::filesystem::path& rel, bool exists, uintmax_t have, uintmax_t want) {
    return exists && have == want && !rel.empty() && *rel.begin() == std::filesystem::path("blobs") &&
           std::distance(rel.begin(), rel.end()) > 1;
}
}  // namespace ksaves
