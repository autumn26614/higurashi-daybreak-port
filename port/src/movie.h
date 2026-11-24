// Movies: data/movie/*.mpg in daybreak02.dat (Kai: daybreak06.dat), MPEG-1 program streams,
// 640x480 at 25 fps with MPEG-1 layer II audio (44.1 kHz stereo). iOS has no decoder for
// them, so they are decoded with pl_mpeg (third_party/pl_mpeg.h, MIT, Dominic Szablewski),
// streamed from the archive: the movies are up to 220 MB.
//
// Video and audio are decoded by two independent readers of the same entry, so the player
// can keep audio buffered ahead while it paces the frames.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "archive.h"

namespace db {

class Movie {
public:
    Movie();
    ~Movie();
    Movie(const Movie&) = delete;
    Movie& operator=(const Movie&) = delete;

    // `src` must outlive the movie.
    bool open(const Source& src, const std::string& name, std::string* err);

    int width() const;
    int height() const;
    double fps() const;
    int sample_rate() const;  // 0: no audio

    // The next video frame as RGBA (width * height * 4); false at the end.
    bool next_frame(std::vector<uint8_t>& rgba, double* time = nullptr);
    // The next audio packet as interleaved 16-bit stereo (up to 1152 frames); empty at the end.
    bool next_audio(std::vector<int16_t>& stereo);

private:
    struct Stream;
    std::unique_ptr<Stream> video_, audio_;
};

}  // namespace db
