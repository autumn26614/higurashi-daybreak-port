// Story scenes (data/scenario/D%02d-%01d.txt, prologue.txt): the talk scene's command
// language, as loaded by 0x5E24A5..0x5E2E00 (command objects, type at +0) and run by the
// talk scene's switch at 0x414B70.
//
//   # ...                      comment
//   characterdir <dir>         prefix for character images        (load time)
//   voicedir <dir>             prefix for VOICE files              (load time)
//   bg <path>                  type 1: background, 800x600, drawn at (0,0)
//   bgm <file>                 type 2: music data/bgm/<file>
//   character <file>,a,b[,c]   type 3: slot a (0-5) shows <dir><file> (512x512, origin 256,0,
//                              set at 0x414CFE); slots > 1 are mirrored (scale -1,1, 0x414D2F);
//                              if b > 0 the slot's x becomes c (0x414D51)
//   move a,x,n                 type 5: slot a's x moves to x over n frames (animator +0x6C4)
//   front a                    type 6: slot a drawn last (draw order +0x73C, 6 entries)
//   VOICE <file>               type 4: plays <voicedir><file> with the next text
//   sleep <ms>                 type 7: Win32 Sleep
//   anything else              type 0: a text page (talk.h)
// Draw (0x4147F7..0x4148A5): background at (0,0), then the slots in draw order at
// (x - 256, 88), then the text window (talk.h). Win quotes (no commands) use talk::background.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace db {
namespace story {

struct Slot {
    std::string image;  // archive entry (".bmp" as written; map with cnv_name), "" = empty
    float x = 0;        // centre (where a move ends)
    bool mirrored = false;
    float move_from = 0;  // a move started by this page's commands: from here to x
    int move_frames = 0;  // over this many frames (0 = no move); linear (inferred: the
                          // animator at talk scene +0x6C4 queues (target, frames) entries)
};

// Scene state when a page is shown; moves started for this page are in each slot.
struct Scene {
    std::string background;  // archive entry or ""
    std::string bgm;         // archive entry (data/bgm/<file>) or ""
    Slot slots[6];
    int order[6] = {0, 1, 2, 3, 4, 5};  // drawn first .. last
};

struct Page {
    std::string text;   // raw cp932 text line (talk.h layout)
    std::string voice;  // archive entry of the voice clip, or ""
    Scene scene;
    int sleep_ms = 0;   // sleeps before this page
};

struct Story {
    std::vector<Page> pages;
    bool has_commands = false;  // false: a plain script (win quote)
};

Story parse(const std::string& bytes);

}  // namespace story
}  // namespace db

namespace db {
namespace story {

// 800x600 RGBA: background at (0,0), then each slot's 512x512 image at (x - 256, 88) in
// draw order, mirrored for slots > 1, `frame` frames after the page opened (moves in
// progress are interpolated; pass a large frame for the settled scene). `load_rgba`
// decodes an image entry (.bmp -> .cnv).
std::vector<uint8_t> compose(const Scene& s,
                             bool (*load_rgba)(void* ctx, const std::string& name, std::vector<uint8_t>& rgba, int& w,
                                               int& h),
                             void* ctx, int frame = 1 << 30);

// The slot's x `frame` frames after the page opened.
float slot_x(const Slot& s, int frame);

// Frames until every move of the scene has finished (0 if none).
int settle_frames(const Scene& s);

}  // namespace story
}  // namespace db
