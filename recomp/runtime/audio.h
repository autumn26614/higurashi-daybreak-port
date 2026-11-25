// The mixer behind DirectSound: the host pulls interleaved stereo 16-bit frames.
#pragma once

#include <cstdint>

namespace rt {
namespace audio {

constexpr int kRate = 44100;
// Mixes `frames` stereo frames of every playing buffer into `out` (and advances them).
void mix(int16_t *out, int frames);
// A stream the host mixes in as it plays (movie sound): interleaved stereo floats.
void stream_push(const float *interleaved, int frames, int rate, float gain);
double stream_queued();  // seconds not yet played
// Without an audio device: a thread that advances the buffers in real time.
void start_headless();

}  // namespace audio
}  // namespace rt
