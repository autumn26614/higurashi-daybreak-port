// Generic DirectX .x reader: "txt ", "bin " and "bzip" (MSZIP) encodings.
// Produces an untyped object tree; templates are skipped. Interpretation of
// Frame / Mesh / Material lives in the caller (see stageinfo.cpp).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace db {

struct XObject {
    std::string type;               // template name, e.g. "Frame", "Mesh"
    std::string name;               // optional instance name (raw bytes, cp932)
    std::vector<double> nums;       // all numeric data in file order (ints and floats)
    std::vector<std::string> strs;  // string data in file order
    std::vector<std::string> refs;  // "{ name }" references to other objects
    std::vector<XObject> kids;      // nested data objects
};

struct XFile {
    std::string encoding;  // "txt ", "bin " or "bzip"
    int float_bits = 32;
    std::vector<XObject> roots;
};

bool parse_x(const std::vector<uint8_t>& data, XFile& out, std::string* err);

}  // namespace db
