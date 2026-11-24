// Software renderer for headless runs: renders the chosen frames and writes PNG files.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "d3d9.h"

namespace rt {
namespace d3d {

struct SoftOptions {
    std::string dir = ".";            // where frame_NNNNNN.png go
    uint64_t every = 0, first = 0;    // every Nth frame from `first` (0: off)
    std::vector<uint64_t> frames;     // and these frames
};
std::unique_ptr<Renderer> make_soft_renderer(SoftOptions o);

}  // namespace d3d
}  // namespace rt
