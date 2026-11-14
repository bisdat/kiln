// SPDX-License-Identifier: MIT
// The catalog reader over resources dumped from a game client's install (not part of run.sh: it needs the game's files).
// Prints the catalog's digest, whether it is a verified build pair, and one row; the dumped files and the output stay out
// of the repository. What a game update needs (until the dev.28 plan's G4 gives the pair a record and a tool): dump the
// two kinds of resources of the new client, run this, and add the printed digest with both new exe hashes to kPairs in
// catalog.cpp.
//
//   catalog_offline <dir> <client exe sha256> [alias]
// <dir> holds <guid>.<type as 8 hex digits>.bin per resource (the registry d165e819-...: 316be6f1; each icon texture:
// f9e86997). Missing files are listed ("missing <guid> <type>") and the exit code is 2: dump them and run again.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "catalog.h"

using namespace kmk;

namespace {

std::string guid_text(const uint8_t g[16]) {
    char s[40];
    std::snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
                  g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return s;
}

struct Dir {
    std::string path;
    std::vector<std::string> missing;
};

bool read_file(const uint8_t g[16], uint32_t type, size_t limit, std::vector<uint8_t>* out, void* ctx) {
    Dir* d = static_cast<Dir*>(ctx);
    char t[16];
    std::snprintf(t, sizeof t, "%08x", type);
    const std::string name = guid_text(g) + "." + t + ".bin";
    std::ifstream f(d->path + "/" + name, std::ios::binary);
    if (!f) {
        d->missing.push_back(guid_text(g) + " " + t);
        // keep going: a placeholder of a valid empty texture lets the reader list every missing image in one run
        if (type == kUiTextureType) {
            out->assign(57, 0);
            (*out)[0x20] = 56 - 0x20;
            (*out)[0x24] = 1;
            (*out)[56] = 'x';
            return true;
        }
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return out->size() <= limit;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: catalog_offline <dir> <client exe sha256> [alias]\n");
        return 64;
    }
    Dir d{argv[1], {}};
    std::string why;
    auto c = read_catalog(&read_file, &d, &why);
    for (const std::string& m : d.missing) std::printf("missing %s\n", m.c_str());
    if (!d.missing.empty()) return 2;
    if (!c) {
        std::printf("catalog: refused: %s\n", why.c_str());
        return 1;
    }
    const BuildPair* pair = pair_of_client(argv[2]);
    const bool verified = pair && c->digest_hex() == pair->catalog_sha256;
    std::printf("catalog: %zu rows, %u with the map surface, sha256 %s, client %s: %s\n", c->rows.size(), c->map_rows(), c->digest_hex().c_str(), argv[2],
                verified ? "a verified build pair" : pair ? "NOT the verified digest of this client" : "no verified build pair for this client");
    const std::string alias = argc > 3 ? argv[3] : "well_plant";
    const IconRow* row = nullptr;
    uint32_t n = 0;
    switch (resolve(*c, alias, &row, &n)) {
        case Resolved::Ok:
            std::printf("%s: %s image %s flags %u surfaces %u texture %s\n", alias.c_str(), row->key.c_str(), guid_text(row->image).c_str(), row->flags,
                        row->surfaces, row->name.c_str());
            break;
        case Resolved::Ambiguous: std::printf("%s: ambiguous (%u types)\n", alias.c_str(), n); break;
        case Resolved::NotFound: std::printf("%s: not found\n", alias.c_str()); break;
        case Resolved::BadSyntax: std::printf("%s: not a name\n", alias.c_str()); break;
    }
    return verified ? 0 : 3;
}
