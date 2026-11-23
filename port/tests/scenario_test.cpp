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
#include "../src/story.h"
#include "../src/talk.h"

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
    // Talk-box layout of every page of the first archive's scripts.
    {
        int pages_n = 0, stopped = 0, hidden = 0, max_lines = 0, wrapped = 0;
        for (const Entry& e : a.entries()) {
            const std::string low = lower_ascii(e.name);
            if (low.rfind("data/scenario/", 0) != 0 || low.size() < 4 || low.compare(low.size() - 4, 4, ".txt") != 0)
                continue;
            if (!a.read(e, d1, &err)) return 1;
            const Script sc = parse(std::string(d1.begin(), d1.end()));
            for (const Page& pg : pages(sc)) {
                pages_n++;
                const talk::Layout l = talk::layout(pg.text->raw);
                int newlines = 0;
                for (const Token& t : pg.text->tokens) newlines += t.kind == TokKind::Newline;
                stopped += l.stopped;
                wrapped += l.lines > newlines + 1;
                max_lines = std::max(max_lines, l.lines);
                for (const talk::Glyph& g : l.glyphs)
                    if (g.y >= 1 + 34 * 4) {  // row 5: beyond the 128 revealed cells
                        hidden++;
                        break;
                    }
            }
        }
        printf("talk box: %d pages, max %d lines, %d auto-wrapped, %d run out of lines, %d need a 5th row\n", pages_n,
               max_lines, wrapped, stopped, hidden);
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

    // Talk box layout (src/talk.h): fixed 12/24 px cells from x = 1, y = 1, lines of 34 px.
    {
        using namespace db::talk;
        Layout l = layout("Hi you");
        check(l.glyphs.size() == 5 && l.glyphs[0].x == 1 && l.glyphs[1].x == 13 && l.glyphs[2].x == 37 &&
                  l.glyphs[0].y == 1 && l.text_bytes == 6,
              "ASCII advances 12 px; a space advances without a glyph");
        l = layout("\x82\xa0" "a\x81\x40" "b");
        check(l.glyphs.size() == 3 && l.glyphs[0].code == 0x82a0 && l.glyphs[0].bytes == 2 && l.glyphs[1].x == 25 &&
                  l.glyphs[2].x == 61,
              "two-byte characters advance 24 px; ideographic space only advances");
        l = layout("a\\nb\\cFF0000c\\ed");
        check(l.glyphs.size() == 4 && l.glyphs[1].x == 1 && l.glyphs[1].y == 35 && l.lines == 2, "\\n starts a line");
        check(l.glyphs[2].rgb[0] == 255 && l.glyphs[2].rgb[1] == 0 && l.glyphs[3].rgb[1] == 255, "\\c colour, \\e reset");
        l = layout(std::string(64, 'a'));
        check(l.glyphs.size() == 64 && l.glyphs[62].x == 1 + 62 * 12 && l.glyphs[63].x == 1 && l.glyphs[63].y == 35,
              "wraps when fewer than 12 px remain of 760: 63 characters per line");
        l = layout("1\\n2\\n3\\n4\\n5\\n6");
        check(l.stopped && l.glyphs.size() == 5 && l.lines == 5, "the fifth line break stops the text (max 5 lines)");
        l = layout(std::string("ab\0cd", 5));
        check(l.text_bytes == 2 && l.glyphs.size() == 2, "text ends at NUL (strlen)");

        Params p;
        const uint8_t red[3] = {255, 0, 0};
        uint8_t px[4];
        glyph_pixel(16, 0, red, p, px);
        check(px[3] == 227 && px[0] == 255 && px[1] == 255, "full coverage: alpha (14*16)|3 = 227; top row white");
        glyph_pixel(1, 24, red, p, px);
        check(px[3] == 15 && px[0] == 255 && px[1] == 0 && px[2] == 0, "coverage 1: alpha 15; bottom row = colour");
        glyph_pixel(8, 12, red, p, px);
        check(px[1] == 255 - (12 * 255) / 24, "gradient halfway");
        glyph_pixel(0, 0, red, p, px);
        check(px[3] == 0, "coverage 0 is transparent");
        p.gradient = false;
        const uint8_t mixed[3] = {10, 20, 30};
        glyph_pixel(16, 5, mixed, p, px);
        check(px[0] == 10 && px[1] == 10 && px[2] == 30, "flat path reads R twice, as the game");

        // Composition: a solid 12x24 glyph mask, white opaque window.
        Params q;
        Layout one = layout("A");
        Mask m;
        m.width = 12, m.height = 24, m.left = 0, m.top = 0;
        m.coverage.assign(12 * 24, 16);
        const std::vector<uint8_t> text = draw_text(one, {{uint16_t('A'), m}}, q);
        check(text[size_t((1 * q.bitmap_width + 1) * 4 + 3)] == 227 && text[3] == 0, "glyph drawn at its cell");
        const std::vector<uint8_t> window(780 * 200 * 4, 255);
        std::vector<uint8_t> f = compose({}, window, 780, 200, text, q, 1, false);
        auto at = [&](int x, int y, int c) { return int(f[size_t((y * Frame::kWidth + x) * 4 + c)]); };
        check(at(5, 5, 0) == 0 && at(15, 445, 0) == 128, "black background, window at (10,440) at 50%");
        check(at(21, 451, 0) > 235, "text pixel (alpha 227) at (20 + 1, 450 + 1)");
        check(at(33, 452 + 20, 0) < 128, "shadow at +3,+3 darkens the window");
        f = compose({}, window, 780, 200, text, q, 0, true);
        check(at(21, 451, 0) == 128 && at(721, 551, 0) == 255, "nothing revealed yet; arrow at (720,550)");
    }

    // Story scenes (src/story.h): commands build the scene; text lines are pages.
    {
        namespace st = db::story;
        const std::string src =
            "#comment\r\ncharacterdir data/talk/\r\nbgm bgm01.ogg\r\nbg data/talk/back12.bmp\r\n"
            "voicedir data/voice/scenario/D60/\r\n"
            "character c13-0.bmp,0,1,300\r\ncharacter c01-0.bmp,2,1,500\r\ncharacter c05-1.bmp,3,0\r\n"
            "VOICE D60-001-05.wav\r\nFirst page\r\n"
            "move 3,-280,30\r\nfront 0\r\nsleep 500\r\nSecond page\r\n"
            "character ,2,0\r\nThird\r\n" + std::string(8, '\0');
        const st::Story s = st::parse(src);
        check(s.has_commands && s.pages.size() == 3, "three text pages, commands detected");
        const st::Page& p0 = s.pages[0];
        check(p0.text == "First page" && p0.voice == "data/voice/scenario/D60/D60-001-05.wav" &&
                  p0.scene.bgm == "data/bgm/bgm01.ogg" && p0.scene.background == "data/talk/back12.bmp",
              "page: text, voice with voicedir, music, background");
        check(p0.scene.slots[0].image == "data/talk/c13-0.bmp" && p0.scene.slots[0].x == 300 &&
                  !p0.scene.slots[0].mirrored && p0.scene.slots[2].mirrored && p0.scene.slots[3].x == 0,
              "character: slot, x when the flag is set, slots > 1 mirrored");
        const st::Page& p1 = s.pages[1];
        check(p1.voice.empty() && p1.scene.slots[3].x == -280 && p1.sleep_ms == 500, "move ends at its x; sleep");
        check(p1.scene.order[5] == 0 && p1.scene.order[0] == 1, "front moves the slot to the end of the draw order");
        check(s.pages[2].scene.slots[2].image.empty(), "an empty file name clears the slot");
        check(!st::parse("VOICE W01-01-1.wav\r\nHello\r\n").has_commands, "win quote: no commands");

        // Compose: a 1x1 red background drawn at (0,0) as is; a
        // 2x2 portrait in slot 2 (mirrored) at x = 400 -> drawn at (144, 88).
        struct Imgs {
            static bool load(void*, const std::string& n, std::vector<uint8_t>& rgba, int& w, int& h) {
                if (n == "bg.bmp") { rgba = {255, 0, 0, 255}; w = h = 1; return true; }
                if (n == "p.bmp") { rgba = {0, 0, 255, 255, 0, 255, 0, 255, 0, 0, 255, 255, 0, 255, 0, 255}; w = h = 2; return true; }
                return false;
            }
        };
        st::Scene sc;
        sc.background = "bg.bmp";
        sc.slots[2] = {"p.bmp", 400, true};
        const std::vector<uint8_t> f = st::compose(sc, Imgs::load, nullptr);
        auto at = [&](int x, int y, int ch) { return int(f[size_t((y * 800 + x) * 4 + ch)]); };
        check(at(0, 0, 0) == 255 && at(1, 0, 0) == 0, "background drawn at (0,0)");
        check(at(144, 88, 1) == 255 && at(145, 88, 2) == 255, "mirrored portrait at (x - 256, 88)");
    }

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
