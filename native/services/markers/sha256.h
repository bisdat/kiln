// SPDX-License-Identifier: MIT
// SHA-256 (FIPS 180-4) for the icon catalog's digest. Header-only, no allocation.
#ifndef KILN_MARKERS_SHA256_H
#define KILN_MARKERS_SHA256_H
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kmk {

class Sha256 {
public:
    void update(const void* data, size_t n) {
        const auto* p = static_cast<const uint8_t*>(data);
        bits_ += static_cast<uint64_t>(n) * 8;
        while (n) {
            const size_t take = n < 64 - used_ ? n : 64 - used_;
            std::memcpy(buf_ + used_, p, take);
            used_ += take;
            p += take;
            n -= take;
            if (used_ == 64) {
                block(buf_);
                used_ = 0;
            }
        }
    }
    std::array<uint8_t, 32> finish() {
        const uint64_t bits = bits_;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (used_ != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        update(len, 8);
        std::array<uint8_t, 32> out{};
        for (int i = 0; i < 8; ++i)
            for (int b = 0; b < 4; ++b) out[4 * i + b] = static_cast<uint8_t>(h_[i] >> (24 - 8 * b));
        return out;
    }

private:
    static uint32_t ror(uint32_t x, int r) { return x >> r | x << (32 - r); }
    void block(const uint8_t* b) {
        static constexpr uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int t = 0; t < 16; ++t)
            w[t] = uint32_t{b[4 * t]} << 24 | uint32_t{b[4 * t + 1]} << 16 | uint32_t{b[4 * t + 2]} << 8 | uint32_t{b[4 * t + 3]};
        for (int t = 16; t < 64; ++t) {
            const uint32_t s0 = ror(w[t - 15], 7) ^ ror(w[t - 15], 18) ^ (w[t - 15] >> 3);
            const uint32_t s1 = ror(w[t - 2], 17) ^ ror(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int t = 0; t < 64; ++t) {
            const uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[t] + w[t];
            const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
            h = g, g = f, f = e, e = d + t1, d = c, c = bb, bb = a, a = t1 + t2;
        }
        h_[0] += a, h_[1] += bb, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
    }
    uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf_[64] = {};
    size_t used_ = 0;
    uint64_t bits_ = 0;
};

}  // namespace kmk
#endif
