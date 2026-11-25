// The movie DirectShow is playing, for the host to show on top of the game's frame (the
// game plays its movies in a child window of its own, over a black screen).
#pragma once

#include <cstdint>
#include <vector>

namespace rt {
namespace movie {

struct Frame {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    uint64_t serial = 0;  // changes with every new picture
    int x = 0, y = 0, w = 0, h = 0;  // where, in window (backbuffer) pixels
};
// The latest picture while a movie is playing and visible; false otherwise.
bool current(Frame &out);

}  // namespace movie
}  // namespace rt
