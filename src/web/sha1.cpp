#include "web/sha1.hpp"
#include <iomanip>
#include <sstream>

namespace grocycle::web {

namespace {

inline uint32_t rol(uint32_t value, size_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

void process_block(const uint8_t *block, uint32_t &h0, uint32_t &h1, uint32_t &h2, uint32_t &h3, uint32_t &h4) {
    uint32_t w[80];
    for (size_t t = 0; t < 16; ++t) {
        w[t] = (static_cast<uint32_t>(block[(t * 4)]) << 24) | (static_cast<uint32_t>(block[(t * 4) + 1]) << 16) |
               (static_cast<uint32_t>(block[(t * 4) + 2]) << 8) | (static_cast<uint32_t>(block[(t * 4) + 3]));
    }
    for (size_t t = 16; t < 80; ++t) {
        w[t] = rol(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    }

    uint32_t a = h0;
    uint32_t b = h1;
    uint32_t c = h2;
    uint32_t d = h3;
    uint32_t e = h4;

    for (size_t t = 0; t < 80; ++t) {
        uint32_t f = 0;
        uint32_t k = 0;

        if (t < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (t < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (t < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        uint32_t temp = rol(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = temp;
    }

    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
}

} // namespace

std::vector<uint8_t> sha1_digest(const uint8_t *data, size_t len) {
    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    uint64_t total_bits = static_cast<uint64_t>(len) * 8;

    std::vector<uint8_t> buffer(data, data + len);
    buffer.push_back(0x80);

    while ((buffer.size() % 64) != 56) {
        buffer.push_back(0x00);
    }

    for (int i = 7; i >= 0; --i) {
        buffer.push_back(static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFF));
    }

    for (size_t i = 0; i < buffer.size(); i += 64) {
        process_block(buffer.data() + i, h0, h1, h2, h3, h4);
    }

    std::vector<uint8_t> digest(20);
    uint32_t h[5] = {h0, h1, h2, h3, h4};
    for (size_t i = 0; i < 5; ++i) {
        digest[(i * 4)] = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
        digest[(i * 4) + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
        digest[(i * 4) + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xFF);
        digest[(i * 4) + 3] = static_cast<uint8_t>(h[i] & 0xFF);
    }

    return digest;
}

std::vector<uint8_t> sha1_digest(const std::string &input) {
    return sha1_digest(reinterpret_cast<const uint8_t *>(input.data()), input.size());
}

std::string sha1_hex(const std::string &input) {
    auto digest = sha1_digest(input);
    std::ostringstream oss;
    for (uint8_t b : digest) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

} // namespace grocycle::web
