// Scenario script parser (src/scenario.*) and its C API, on test-authored bytes only.
// Usage: scenario_test                         synthetic checks (CI)
//        scenario_test --check <dat> [<dat2>]  local only: round-trip every script in a
//                                              real archive; with dat2, also compare
//                                              each script's command skeleton
// Exit status 0 = PASS.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/archive.h"
#include "../src/daybreak_c.h"
#include "../src/scenario.h"

using namespace db;
using namespace db::scn;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

std::string kinds(const std::vector<Token>& t) {
    std::string s;
    for (const auto& k : t) s += "TCENU"[int(k.kind)];
    return s;
}

bool round_trips(const std::string& s) { return serialize(parse(s)) == s; }

int check_archive(const char* path, const char* path2) {
    Archive a, b;
    std::string err;
    if (!a.open(path, &err) || (path2 && !b.open(path2, &err))) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    int scripts = 0, bad_trip = 0, differ = 0, same_skeleton = 0, skel_diff = 0, unknown = 0;
    std::vector<uint8_t> d1, d2;
    for (const Entry& e : a.entries()) {
        const std::string low = lower_ascii(e.name);
        if (low.rfind("data/scenario/", 0) != 0 || low.size() < 4 || low.compare(low.size() - 4, 4, ".txt") != 0)
            continue;
        scripts++;
        if (!a.read(e, d1, &err)) return 1;
        const std::string s1(d1.begin(), d1.end());
        const Script p1 = parse(s1);
        if (serialize(p1) != s1) bad_trip++;
        for (const Line& ln : p1.lines)
            for (const Token& t : ln.tokens) unknown += t.kind == TokKind::Unknown;
        if (!path2) continue;
        const Entry* e2 = b.find(e.name);
        if (!e2 || !b.read(*e2, d2, &err)) return 1;
        const std::string s2(d2.begin(), d2.end());
        const Script p2 = parse(s2);
        if (serialize(p2) != s2) bad_trip++;
        for (const Line& ln : p2.lines)
            for (const Token& t : ln.tokens) unknown += t.kind == TokKind::Unknown;
        if (s1 == s2) continue;
        differ++;
        if (skeleton(p1) == skeleton(p2)) {
            same_skeleton++;
        } else {
            if (skel_diff < 3)
                printf("  %s\n    %s\n    %s\n", e.name.c_str(), skeleton(p1).c_str(), skeleton(p2).c_str());
            skel_diff++;
        }
    }
    printf("%d scripts, %d round-trip failures, %d unknown commands\n", scripts, bad_trip, unknown);
    if (path2) printf("%d differ: %d keep the command skeleton, %d do not\n", differ, same_skeleton, skel_diff);
    return bad_trip ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[1], "--check") == 0) return check_archive(argv[2], argc >= 4 ? argv[3] : nullptr);

    // Typical page: VOICE line, then text with colour, reset and line breaks.
    const std::string sample =
        "VOICE W01-01-1.wav\r\n"
        "\\cFF8000Hello\\e there\\nsecond line\r\n"
        "plain line, no voice\r\n";
    const Script s = parse(sample);
    check(s.lines.size() == 3 && s.final_crlf, "three lines, final CRLF");
    check(s.lines[0].voice && s.lines[0].voice_file == "W01-01-1.wav", "VOICE line and its file");
    {
        const Script commas = parse("VOICE W01-02-7.wav,,,,,,\r\nx\r\n");
        check(commas.lines[0].voice_file == "W01-02-7.wav" && commas.lines[0].raw == "VOICE W01-02-7.wav,,,,,,",
              "trailing commas after the voice file are dropped (raw kept)");
    }
    check(kinds(s.lines[1].tokens) == "CTETNT", "colour, text, reset, text, newline, text");
    check(s.lines[1].tokens[0].r == 0xFF && s.lines[1].tokens[0].g == 0x80 && s.lines[1].tokens[0].b == 0x00,
          "\\cRRGGBB is R, G, B");
    check(s.lines[1].tokens[1].raw == "Hello" && s.lines[1].tokens[0].raw == "\\cFF8000", "raw bytes kept");
    check(round_trips(sample), "round trip");

    const auto pg = pages(s);
    check(pg.size() == 2 && pg[0].voice_file == "W01-01-1.wav" && pg[1].voice_file.empty() &&
              pg[1].text->raw == "plain line, no voice",
          "pages pair a voice with the next text line only");

    // cp932: 0x95 0x5C is one character; its trail byte is not a backslash.
    const std::string kanji = "\x95\x5C" "n\\n";
    check(kinds(tokenize(kanji)) == "TNT" || kinds(tokenize(kanji)) == "TN", "trail byte 0x5C is not a command");
    check(tokenize(kanji)[0].raw == "\x95\x5C" "n", "the 'n' after the kanji is text");
    check(kinds(tokenize("\x83\x5C\x83\x5C")) == "T", "two kanji with 0x5C trail bytes: one text token");

    // Edge cases from the English patch: no final CRLF, lone lead byte at end of line,
    // backslash before a non-command byte. All must survive byte for byte.
    check(round_trips("VOICE a.wav\r\nno final crlf") && !parse("x").final_crlf, "no final CRLF");
    check(round_trips("ends on a lead byte \x82\r\nnext\r\n"), "lone lead byte at end of line");
    check(kinds(tokenize("ab\x82")) == "T", "lone lead byte is text");
    check(kinds(tokenize("a\\\x8d" "b")) == "TUT" && tokenize("a\\\x8d" "b")[1].raw == "\\\x8d",
          "backslash before a lead byte is an unknown command");
    check(kinds(tokenize("\\c12")) == "UT", "\\c without six hex digits is unknown");
    check(kinds(tokenize("\\cGG0000x")) == "UT", "\\c with non-hex digits is unknown");
    check(kinds(tokenize("end\\")) == "T", "backslash at end of line is text");
    check(round_trips("") && parse("").lines.empty(), "empty script");
    {
        const std::string padded = std::string("VOICE a.wav\r\ntext\r\n") + std::string(40, '\0');
        const Script p = parse(padded);
        check(round_trips(padded) && p.lines.size() == 2 && p.final_crlf && p.trailer.size() == 40 &&
                  pages(p).size() == 1,
              "NUL padding after the text is a trailer, not a page");
        check(skeleton(p) == skeleton(parse("VOICE a.wav\r\ntext\r\n")), "padding does not change the skeleton");
        const std::string mid = std::string("a\r\nb\0c\r\n", 8);
        check(round_trips(mid) && parse(mid).lines.size() == 2 && parse(mid).lines[1].raw == "b",
              "text ends at the first NUL");
    }
    check(round_trips("\r\n\r\n") && parse("\r\n\r\n").lines.size() == 2, "blank lines");
    check(round_trips("bare\nLF\r\n") && parse("bare\nLF\r\n").lines.size() == 1, "a bare LF is not a line break");

    // A translation keeps the command skeleton; a dropped colour changes it.
    const std::string jp = "VOICE a.wav\r\n\\c00FF00\x82\xa0\\n\x82\xa2\r\n";
    const std::string en = "VOICE a.wav\r\n\\c00FF00Ah\\nI see\r\n";
    const std::string broken = "VOICE a.wav\r\nAh\\nI see\r\n";
    check(skeleton(parse(jp)) == skeleton(parse(en)), "translation keeps the skeleton");
    check(skeleton(parse(jp)) != skeleton(parse(broken)), "a lost colour changes the skeleton");

    // C API.
    DBScript* c = db_script_parse(reinterpret_cast<const uint8_t*>(sample.data()), int32_t(sample.size()));
    int32_t len = 0;
    const char* voice = db_script_page_voice(c, 0, &len);
    check(c && db_script_page_count(c) == 2 && voice && std::string(voice, size_t(len)) == "W01-01-1.wav",
          "C API pages and voice");
    check(db_script_token_count(c, 0) == 6 && db_script_token_kind(c, 0, 0) == DB_TOK_COLOR &&
              db_script_token_rgb(c, 0, 0) == 0xFF8000 && db_script_token_kind(c, 0, 4) == DB_TOK_NEWLINE,
          "C API tokens");
    const char* text = db_script_token_bytes(c, 0, 1, &len);
    check(text && std::string(text, size_t(len)) == "Hello", "C API token bytes");
    check(db_script_token_kind(c, 0, 99) == -1 && db_script_token_bytes(c, 5, 0, &len) == nullptr &&
              db_script_page_count(nullptr) == 0,
          "C API out-of-range queries are safe");
    db_script_free(c);
    DBScript* empty = db_script_parse(nullptr, 0);
    check(empty && db_script_page_count(empty) == 0, "C API parses an empty script");
    db_script_free(empty);

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
