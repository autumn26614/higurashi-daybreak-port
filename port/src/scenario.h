// Scenario scripts (data/scenario/**.txt), as drawn by the talk scene's text renderer
// (daybreak.exe 0x5E1850; tokenizer 0x5E1A17-0x5E1BAB, lead-byte test 0x5E22B0).
//
// A script is cp932 text in CRLF-separated lines. A line "VOICE <file>.wav" names the
// voice clip for the text line that follows. Inside text, a backslash starts a command:
//   \cRRGGBB   set the text colour (hex pairs read by 0x5E2300; the glyph blend at
//              0x5E1EA8 puts the first pair in bits 16-23 of the ARGB pixel, so RGB)
//   \e         reset the colour to the renderer's default (this+0x20..0x22)
//   \n         new line: y += font height (+0x134) + line spacing (+0x138)
// Inventory over all 504 base and 192 Kai scripts: \c 1710 + 605, \n 2964 + 953, \e 2,
// nothing else. A cp932 lead byte (0x81-0x9F, 0xE0-0xFE) always takes the next byte
// with it, so a trail byte 0x5C is never a backslash.
//
// The English fan translation leaves some scripts without a final CRLF, a few lines
// ending on a lone lead byte, and two backslashes before a non-command byte. parse() is
// lossless for all of these: serialize(parse(x)) == x for any input.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace db {
namespace scn {

enum class TokKind : int32_t { Text = 0, Color = 1, ColorReset = 2, Newline = 3, Unknown = 4 };

struct Token {
    TokKind kind = TokKind::Text;
    std::string raw;     // exact source bytes (the whole "\cRRGGBB" for a colour)
    uint8_t r = 0, g = 0, b = 0;  // Color only
};

struct Line {
    bool voice = false;      // a "VOICE ..." line
    std::string voice_file;  // its argument up to the first comma or blank, e.g. "W05-08-1.wav"
                             // (some English lines read "W01-02-7.wav,,,,,,"; raw keeps them)
    std::string raw;         // exact bytes without the CRLF
    std::vector<Token> tokens;  // text lines only
};

struct Script {
    std::vector<Line> lines;
    bool final_crlf = true;  // the text ended with CRLF (or was empty)
    // Everything from the first NUL byte on. The English patch keeps each script's size
    // by padding with NULs after the last line (124 scripts); the engine reads the text
    // as a C string (inferred), so this is not shown. Kept for a lossless round trip.
    std::string trailer;
};

inline bool is_lead_byte(uint8_t b) { return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFE); }

Script parse(const std::string& bytes);
std::string serialize(const Script& s);
std::vector<Token> tokenize(const std::string& text_line);

// One text line with the voice clip that precedes it (if any): what the talk scene
// shows as one box.
struct Page {
    std::string voice_file;
    const Line* text = nullptr;
};
std::vector<Page> pages(const Script& s);

// The command skeleton of a script: VOICE arguments in order, and for each text line how
// many colour, reset and unknown commands it has. Counts, not order, and no newlines: the
// English translation re-wraps lines and moves colours within a line (sentence order
// differs) but keeps them. A translation should keep the skeleton of the original.
std::string skeleton(const Script& s);

}  // namespace scn
}  // namespace db
