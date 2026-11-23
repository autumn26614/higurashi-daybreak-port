// Sounds as the game stores them:
//   voices and effects: ".wav" names mapped to ".cnv" (WAVEFORMATEX + PCM, cnv.h)
//   music: data/bgm/<name>.ogg (Ogg Vorbis, decoded with stb_vorbis, public domain) and a
//          sibling <name>.sfl, a RIFF "SFPL" file whose cue point is the loop start (in
//          samples) and whose "ltxt" region length makes the loop end: the game plays the
//          intro once and then loops [start, start + length)
// Voice lines in scenario scripts name "<id>.wav"; win quotes (W..) live under
// data/voice/WIN/, story lines (D..) under data/voice/scenario/<first 3 chars>/ (both
// checked against the archive index; the exe's own path builder was not traced).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "archive.h"

namespace db {

struct Pcm {
    int channels = 0;
    int rate = 0;
    std::vector<int16_t> samples;  // interleaved
    int64_t loop_start = -1;       // in frames; -1 = no loop
    int64_t loop_end = -1;
};

// name: a .wav / .cnv entry, or a .ogg entry (its .sfl is looked up next to it).
bool load_sound(const Source& a, const std::string& name, Pcm& out, std::string* err);

// Parses an .sfl; returns false if it is not one.
bool parse_sfl(const std::vector<uint8_t>& d, int64_t& start, int64_t& length);

// Archive entry of a scenario VOICE argument ("W05-08-1.wav", "D60-023-13.wav"), or "".
std::string voice_entry(const Source& a, const std::string& voice_file);

}  // namespace db
