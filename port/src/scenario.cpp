#include "scenario.h"

namespace db {
namespace scn {

namespace {

int hex(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void push_text(std::vector<Token>& out, const std::string& s, size_t from, size_t to) {
    if (from >= to) return;
    if (!out.empty() && out.back().kind == TokKind::Text)
        out.back().raw.append(s, from, to - from);
    else
        out.push_back({TokKind::Text, s.substr(from, to - from)});
}

const char kVoice[] = "VOICE ";

}  // namespace

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    size_t i = 0, text_start = 0;
    while (i < s.size()) {
        const uint8_t c = uint8_t(s[i]);
        if (is_lead_byte(c)) {
            i += (i + 1 < s.size()) ? 2 : 1;  // a lone lead byte at the end stays text
            continue;
        }
        if (c != '\\' || i + 1 >= s.size()) {
            ++i;
            continue;
        }
        push_text(out, s, text_start, i);
        const uint8_t k = uint8_t(s[i + 1]);
        Token t;
        size_t len = 2;
        if (k == 'c' && i + 8 <= s.size()) {
            int v[6];
            bool ok = true;
            for (int j = 0; j < 6; ++j) ok = ok && (v[j] = hex(uint8_t(s[i + 2 + j]))) >= 0;
            if (ok) {
                t.kind = TokKind::Color;
                t.r = uint8_t(v[0] << 4 | v[1]);
                t.g = uint8_t(v[2] << 4 | v[3]);
                t.b = uint8_t(v[4] << 4 | v[5]);
                len = 8;
            } else {
                t.kind = TokKind::Unknown;
            }
        } else if (k == 'n') {
            t.kind = TokKind::Newline;
        } else if (k == 'e') {
            t.kind = TokKind::ColorReset;
        } else {
            // "\c" without six hex digits, or any other byte (including a lead byte, which
            // then is not taken as the start of a character): kept as an unknown command.
            t.kind = TokKind::Unknown;
        }
        t.raw = s.substr(i, len);
        out.push_back(std::move(t));
        i += len;
        text_start = i;
    }
    push_text(out, s, text_start, s.size());
    return out;
}

Script parse(const std::string& all) {
    Script sc;
    const size_t nul = all.find('\0');
    const std::string bytes = nul == std::string::npos ? all : all.substr(0, nul);
    if (nul != std::string::npos) sc.trailer = all.substr(nul);
    size_t i = 0;
    while (i < bytes.size()) {
        size_t end = bytes.find("\r\n", i);
        const bool last = end == std::string::npos;
        if (last) end = bytes.size();
        Line ln;
        ln.raw = bytes.substr(i, end - i);
        if (ln.raw.compare(0, sizeof kVoice - 1, kVoice) == 0) {
            ln.voice = true;
            ln.voice_file = ln.raw.substr(sizeof kVoice - 1);
            ln.voice_file = ln.voice_file.substr(0, ln.voice_file.find_first_of(", \t"));
        } else {
            ln.tokens = tokenize(ln.raw);
        }
        sc.lines.push_back(std::move(ln));
        if (last) {
            sc.final_crlf = false;
            break;
        }
        i = end + 2;
    }
    return sc;
}

std::string serialize(const Script& s) {
    std::string out;
    for (size_t i = 0; i < s.lines.size(); ++i) {
        out += s.lines[i].raw;
        if (i + 1 < s.lines.size() || s.final_crlf) out += "\r\n";
    }
    return out + s.trailer;
}

std::vector<Page> pages(const Script& s) {
    std::vector<Page> out;
    std::string voice;
    for (const Line& ln : s.lines) {
        if (ln.voice) {
            voice = ln.voice_file;
            continue;
        }
        if (ln.raw.empty()) continue;
        out.push_back({voice, &ln});
        voice.clear();
    }
    return out;
}

std::string skeleton(const Script& s) {
    std::string out;
    for (const Line& ln : s.lines) {
        if (ln.voice) {
            out += "V:" + ln.voice_file + ";";
            continue;
        }
        if (ln.raw.empty()) continue;
        int n[5] = {0, 0, 0, 0, 0};
        for (const Token& t : ln.tokens) n[int(t.kind)]++;
        out += "T[c" + std::to_string(n[1]) + "e" + std::to_string(n[2]) + "?" +
               std::to_string(n[4]) + "];";
    }
    return out;
}

}  // namespace scn
}  // namespace db
