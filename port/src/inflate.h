// Minimal raw-DEFLATE (RFC 1951) decoder with preset-dictionary support,
// enough for DirectX .x "bzip" (MSZIP) payloads. No external dependencies.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace db {

// Decodes `in` (raw deflate, no zlib header). `dict` is prepended to the
// back-reference window (MSZIP uses the previous block's output).
// Decoded bytes are appended to `out`. Returns false on corrupt data.
bool inflate_raw(const uint8_t* in, size_t in_len, const std::vector<uint8_t>& dict,
                 std::vector<uint8_t>& out, std::string* err);

}  // namespace db
