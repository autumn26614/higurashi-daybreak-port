#include "xfile.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "inflate.h"

namespace db {
namespace {

enum class T { Name, String, Num, Guid, LBrace, RBrace, Template, Skip, End };

struct Tok {
    T k;
    std::string s;          // Name / String
    std::vector<double> v;  // Num (one or many)
};

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }

// ---- MSZIP ("bzip"/"tzip") ------------------------------------------------
// After the 16-byte header: u32 total (= decompressed size + 16), then blocks of
// { u16 raw_size, u16 packed_size, "CK", deflate[packed_size-2] }. Each block's
// back-reference window starts with the previous block's output.
bool unmszip(const std::vector<uint8_t>& d, std::vector<uint8_t>& out, std::string* err) {
    if (d.size() < 20) {
        *err = "mszip: too short";
        return false;
    }
    uint32_t total = rd32(&d[16]);
    size_t p = 20;
    std::vector<uint8_t> dict;
    while (p + 4 <= d.size()) {
        uint16_t raw = rd16(&d[p]);
        uint16_t packed = rd16(&d[p + 2]);
        p += 4;
        if (p + packed > d.size() || packed < 2 || d[p] != 'C' || d[p + 1] != 'K') {
            *err = "mszip: bad block header";
            return false;
        }
        std::vector<uint8_t> blk;
        if (!inflate_raw(&d[p + 2], packed - 2u, dict, blk, err)) return false;
        if (blk.size() != raw) {
            *err = "mszip: block size mismatch";
            return false;
        }
        out.insert(out.end(), blk.begin(), blk.end());
        dict = blk.size() > 32768 ? std::vector<uint8_t>(blk.end() - 32768, blk.end()) : blk;
        p += packed;
    }
    if (out.size() + 16 != total) {
        *err = "mszip: total size mismatch";
        return false;
    }
    return true;
}

// ---- binary lexer -----------------------------------------------------------
bool lex_bin(const uint8_t* p, size_t n, int float_bits, std::vector<Tok>& toks, std::string* err) {
    size_t i = 0;
    auto need = [&](size_t k) { return i + k <= n; };
    while (i + 2 <= n) {
        uint16_t t = rd16(p + i);
        i += 2;
        switch (t) {
            case 1:    // TOKEN_NAME
            case 2: {  // TOKEN_STRING (its ; or , terminator follows as a normal token)
                if (!need(4)) goto trunc;
                uint32_t len = rd32(p + i);
                i += 4;
                if (!need(len)) goto trunc;
                toks.push_back({t == 1 ? T::Name : T::String, std::string(reinterpret_cast<const char*>(p + i), len), {}});
                i += len;
                break;
            }
            case 3:  // TOKEN_INTEGER
                if (!need(4)) goto trunc;
                toks.push_back({T::Num, {}, {double(rd32(p + i))}});
                i += 4;
                break;
            case 5:  // TOKEN_GUID
                if (!need(16)) goto trunc;
                i += 16;
                toks.push_back({T::Guid, {}, {}});
                break;
            case 6: {  // TOKEN_INTEGER_LIST
                if (!need(4)) goto trunc;
                uint32_t cnt = rd32(p + i);
                i += 4;
                if (!need(size_t(cnt) * 4)) goto trunc;
                Tok tk{T::Num, {}, {}};
                tk.v.reserve(cnt);
                for (uint32_t k = 0; k < cnt; k++, i += 4) tk.v.push_back(double(rd32(p + i)));
                toks.push_back(std::move(tk));
                break;
            }
            case 7: {  // TOKEN_FLOAT_LIST
                if (!need(4)) goto trunc;
                uint32_t cnt = rd32(p + i);
                i += 4;
                size_t fs = float_bits == 64 ? 8 : 4;
                if (!need(size_t(cnt) * fs)) goto trunc;
                Tok tk{T::Num, {}, {}};
                tk.v.reserve(cnt);
                for (uint32_t k = 0; k < cnt; k++, i += fs) {
                    if (fs == 4) {
                        float f;
                        memcpy(&f, p + i, 4);
                        tk.v.push_back(f);
                    } else {
                        double dd;
                        memcpy(&dd, p + i, 8);
                        tk.v.push_back(dd);
                    }
                }
                toks.push_back(std::move(tk));
                break;
            }
            case 0x0A: toks.push_back({T::LBrace, {}, {}}); break;
            case 0x0B: toks.push_back({T::RBrace, {}, {}}); break;
            case 0x1F: toks.push_back({T::Template, {}, {}}); break;
            default:
                // Punctuation (0x0C-0x14), primitive type keywords (0x28-0x34) and
                // padding carry no payload.
                if (t == 0 || (t >= 0x0C && t <= 0x14) || (t >= 0x28 && t <= 0x34)) {
                    toks.push_back({T::Skip, {}, {}});
                    break;
                }
                *err = "x(bin): unknown token " + std::to_string(t);
                return false;
        }
    }
    toks.push_back({T::End, {}, {}});
    return true;
trunc:
    *err = "x(bin): truncated token";
    return false;
}

// ---- text lexer -------------------------------------------------------------
bool lex_txt(const uint8_t* p, size_t n, std::vector<Tok>& toks, std::string* err) {
    size_t i = 0;
    auto special = [](uint8_t c) { return c == '{' || c == '}' || c == ';' || c == ',' || c == '"' || c == '<' || c == '>' || c == '[' || c == ']'; };
    while (i < n) {
        uint8_t c = p[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        if (c == '#' || (c == '/' && i + 1 < n && p[i + 1] == '/')) {
            while (i < n && p[i] != '\n') i++;
            continue;
        }
        if (c == '{') { toks.push_back({T::LBrace, {}, {}}); i++; continue; }
        if (c == '}') { toks.push_back({T::RBrace, {}, {}}); i++; continue; }
        if (c == ';' || c == ',' || c == '[' || c == ']') { toks.push_back({T::Skip, {}, {}}); i++; continue; }
        if (c == '<') {  // <GUID>
            while (i < n && p[i] != '>') i++;
            i++;
            toks.push_back({T::Guid, {}, {}});
            continue;
        }
        if (c == '"') {
            size_t j = ++i;
            while (j < n && p[j] != '"') j++;
            if (j >= n) { *err = "x(txt): unterminated string"; return false; }
            toks.push_back({T::String, std::string(reinterpret_cast<const char*>(p + i), j - i), {}});
            i = j + 1;
            continue;
        }
        size_t j = i;
        while (j < n && !special(p[j]) && p[j] > ' ') j++;
        std::string w(reinterpret_cast<const char*>(p + i), j - i);
        i = j;
        char* endp = nullptr;
        double d = strtod(w.c_str(), &endp);
        if (!w.empty() && endp && *endp == 0 && (isdigit(uint8_t(w[0])) || w[0] == '-' || w[0] == '+' || w[0] == '.'))
            toks.push_back({T::Num, {}, {d}});
        else if (w == "template")
            toks.push_back({T::Template, {}, {}});
        else
            toks.push_back({T::Name, w, {}});
    }
    toks.push_back({T::End, {}, {}});
    return true;
}

// ---- parser -------------------------------------------------------------------
struct Parser {
    const std::vector<Tok>& t;
    size_t i = 0;
    std::string* err;

    bool skip_template() {
        while (t[i].k != T::LBrace && t[i].k != T::End) i++;
        int depth = 0;
        while (t[i].k != T::End) {
            if (t[i].k == T::LBrace) depth++;
            if (t[i].k == T::RBrace && --depth == 0) { i++; return true; }
            i++;
        }
        *err = "x: unterminated template";
        return false;
    }

    bool object(XObject& o, int depth) {
        if (depth > 64) { *err = "x: nesting too deep"; return false; }
        o.type = t[i++].s;
        if (t[i].k == T::Name) o.name = t[i++].s;
        if (t[i].k == T::Guid) i++;
        if (t[i].k != T::LBrace) { *err = "x: expected '{' after " + o.type; return false; }
        i++;
        for (;;) {
            switch (t[i].k) {
                case T::RBrace: i++; return true;
                case T::Name: {
                    o.kids.emplace_back();
                    if (!object(o.kids.back(), depth + 1)) return false;
                    break;
                }
                case T::LBrace: {  // reference: { name [guid] }
                    i++;
                    while (t[i].k != T::RBrace && t[i].k != T::End) {
                        if (t[i].k == T::Name) o.refs.push_back(t[i].s);
                        i++;
                    }
                    if (t[i].k == T::End) { *err = "x: unterminated reference"; return false; }
                    i++;
                    break;
                }
                case T::Num: o.nums.insert(o.nums.end(), t[i].v.begin(), t[i].v.end()); i++; break;
                case T::String: o.strs.push_back(t[i].s); i++; break;
                case T::End: *err = "x: unexpected end inside " + o.type; return false;
                default: i++; break;
            }
        }
    }

    bool file(std::vector<XObject>& roots) {
        while (t[i].k != T::End) {
            if (t[i].k == T::Template) {
                if (!skip_template()) return false;
            } else if (t[i].k == T::Name) {
                roots.emplace_back();
                if (!object(roots.back(), 0)) return false;
            } else {
                i++;
            }
        }
        return true;
    }
};

}  // namespace

bool parse_x(const std::vector<uint8_t>& data, XFile& out, std::string* err) {
    if (data.size() < 16 || memcmp(data.data(), "xof ", 4) != 0) {
        *err = "not a DirectX .x file";
        return false;
    }
    out.encoding.assign(reinterpret_cast<const char*>(&data[8]), 4);
    out.float_bits = atoi(std::string(reinterpret_cast<const char*>(&data[12]), 4).c_str());
    std::vector<Tok> toks;
    if (out.encoding == "bin ") {
        if (!lex_bin(data.data() + 16, data.size() - 16, out.float_bits, toks, err)) return false;
    } else if (out.encoding == "txt ") {
        if (!lex_txt(data.data() + 16, data.size() - 16, toks, err)) return false;
    } else if (out.encoding == "bzip" || out.encoding == "tzip") {
        std::vector<uint8_t> raw;
        if (!unmszip(data, raw, err)) return false;
        bool ok = out.encoding == "bzip" ? lex_bin(raw.data(), raw.size(), out.float_bits, toks, err)
                                         : lex_txt(raw.data(), raw.size(), toks, err);
        if (!ok) return false;
    } else {
        *err = "unsupported .x encoding '" + out.encoding + "'";
        return false;
    }
    Parser ps{toks, 0, err};
    return ps.file(out.roots);
}

}  // namespace db
