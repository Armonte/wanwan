// audio_player.h -- one-shot SDL3 playback of a kgt::PcmAudio.
//
// App layer (this is the only audio TU that touches SDL). Play() opens a
// default-device stream in the clip's rate/channels (s16 via Normalize
// when the source is 8-bit), queues the whole clip, and lets it drain.
// The playhead derives from bytes consumed: total queued minus
// SDL_GetAudioStreamQueued.
#pragma once

#include <cstddef>

struct SDL_AudioStream;

namespace kgt {
struct PcmAudio;
}

namespace studio {

class AudioPlayer {
public:
    AudioPlayer() = default;
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool Play(const kgt::PcmAudio& pcm);  // stops any current sound first
    void Stop();
    bool Playing() const;          // true while bytes remain queued
    double PositionSeconds() const;

private:
    SDL_AudioStream* stream_ = nullptr;  // owns its bound device
    size_t total_bytes_ = 0;
    double byte_rate_ = 0.0;
};

}  // namespace studio
