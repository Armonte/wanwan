// audio_player.cpp -- see audio_player.h.
#include "audio_player.h"

#include <SDL3/SDL.h>

#include <climits>

#include "../core/audio/audio_convert.h"

namespace studio {

AudioPlayer::~AudioPlayer() { Stop(); }

bool AudioPlayer::Play(const kgt::PcmAudio& in) {
    Stop();

    // Uniform s16 (Normalize converts the u8 path; 16-bit plays as-is,
    // rate and channels always preserved).
    kgt::PcmAudio conv;
    const kgt::PcmAudio* clip = &in;
    if (in.bits != 16) {
        conv = kgt::Normalize(in, 0, 0, 16);
        clip = &conv;
    }
    if (clip->channels == 0 || clip->rate == 0 || clip->pcm.empty() ||
        clip->pcm.size() > size_t(INT_MAX))
        return false;

    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = clip->channels;
    spec.freq = int(clip->rate);
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                        &spec, nullptr, nullptr);
    if (!stream_) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "audio device open failed: %s",
                    SDL_GetError());
        return false;
    }
    if (!SDL_PutAudioStreamData(stream_, clip->pcm.data(),
                                int(clip->pcm.size()))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "audio queue failed: %s",
                    SDL_GetError());
        Stop();
        return false;
    }
    SDL_FlushAudioStream(stream_);  // whole clip queued: let the tail drain
    total_bytes_ = clip->pcm.size();
    byte_rate_ = double(clip->rate) * clip->channels * 2;
    SDL_ResumeAudioStreamDevice(stream_);  // device streams start paused
    return true;
}

void AudioPlayer::Stop() {
    if (!stream_) return;
    SDL_DestroyAudioStream(stream_);  // also closes the bound device
    stream_ = nullptr;
    total_bytes_ = 0;
    byte_rate_ = 0.0;
}

bool AudioPlayer::Playing() const {
    return stream_ && SDL_GetAudioStreamQueued(stream_) > 0;
}

double AudioPlayer::PositionSeconds() const {
    if (!stream_ || byte_rate_ <= 0.0) return 0.0;
    int queued = SDL_GetAudioStreamQueued(stream_);
    if (queued < 0) queued = 0;
    const size_t q = size_t(queued);
    const size_t consumed = q < total_bytes_ ? total_bytes_ - q : total_bytes_;
    return double(consumed) / byte_rate_;
}

}  // namespace studio
