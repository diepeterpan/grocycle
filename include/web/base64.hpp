#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace grocycle::web {

// Encodes binary data or string into standard RFC 4648 Base64
std::string base64_encode(const uint8_t *data, size_t len);
std::string base64_encode(const std::vector<uint8_t> &data);
std::string base64_encode(const std::string &input);

// Decodes standard Base64 string to original string/bytes
std::string base64_decode(const std::string &input);

} // namespace grocycle::web
