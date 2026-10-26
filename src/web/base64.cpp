#include "web/base64.hpp"

namespace grocycle::web {

namespace {

constexpr char kBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

std::string base64_encode(const uint8_t *data, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    while (i < len) {
        size_t rem = len - i;
        uint32_t octet_a = data[i++];
        uint32_t octet_b = (rem > 1) ? data[i++] : 0;
        uint32_t octet_c = (rem > 2) ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        result.push_back(kBase64Chars[(triple >> 18) & 0x3F]);
        result.push_back(kBase64Chars[(triple >> 12) & 0x3F]);
        result.push_back((rem > 1) ? kBase64Chars[(triple >> 6) & 0x3F] : '=');
        result.push_back((rem > 2) ? kBase64Chars[triple & 0x3F] : '=');
    }

    return result;
}

std::string base64_encode(const std::vector<uint8_t> &data) {
    return base64_encode(data.data(), data.size());
}

std::string base64_encode(const std::string &input) {
    return base64_encode(reinterpret_cast<const uint8_t *>(input.data()), input.size());
}

std::string base64_decode(const std::string &input) {
    std::string result;
    if (input.empty()) {
        return result;
    }

    auto decode_char = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    };

    uint32_t buffer = 0;
    int bits_collected = 0;

    for (char c : input) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') {
            continue;
        }
        int val = decode_char(c);
        if (val == -1) {
            continue;
        }

        buffer = (buffer << 6) | static_cast<uint32_t>(val);
        bits_collected += 6;

        if (bits_collected >= 8) {
            bits_collected -= 8;
            result.push_back(static_cast<char>((buffer >> bits_collected) & 0xFF));
        }
    }

    return result;
}

} // namespace grocycle::web
