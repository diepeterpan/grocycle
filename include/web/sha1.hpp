#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace grocycle::web {

// Computes standard SHA-1 hash of an input string and returns 20-byte binary digest
std::vector<uint8_t> sha1_digest(const std::string &input);

// Computes standard SHA-1 hash of input bytes
std::vector<uint8_t> sha1_digest(const uint8_t *data, size_t len);

// Computes SHA-1 and returns 40-character lowercase hex string
std::string sha1_hex(const std::string &input);

} // namespace grocycle::web
