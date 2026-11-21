// Minimal PNG writer for inspection output (RGBA8, stored/uncompressed deflate blocks,
// so no zlib dependency). Used by the stageinfo CLI, not by the app.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace db {

std::vector<uint8_t> encode_png_rgba(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba);
bool write_file(const std::string& path, const std::vector<uint8_t>& bytes, std::string* err);

}  // namespace db
