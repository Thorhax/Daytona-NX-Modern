#pragma once
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif
#include "runtime/sound_board.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#if defined(__SWITCH__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <switch.h>
#pragma GCC diagnostic pop
#endif

namespace switch_app {

class Audio {
public:
    Audio() = default;
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;
    ~Audio() { close(); }

    bool open() {
        SDL_AudioSpec want{}, got{};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = callback;
        want.userdata = this;

        device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
        if (!device_) return false;
        rate_ = got.freq;

        fm_ = SDL_NewAudioStream(AUDIO_F32SYS, 2, int(snd::SoundBoard::kYmClock / 144.0 + 0.5),
                                 AUDIO_F32SYS, 2, rate_);
        pcm_ = SDL_NewAudioStream(AUDIO_F32SYS, 2, int(snd::SoundBoard::kPcmClock / 224.0 + 0.5),
                                  AUDIO_F32SYS, 2, rate_);
        if (!fm_ || !pcm_) {
            close();
            return false;
        }
        return true;
    }

    void push(snd::SoundBoard &board) {
        const auto fm = board.take_fm(), pcm = board.take_pcm();
        if (!device_) return;
        SDL_LockAudioDevice(device_);
        const int limit = rate_ * 2 * int(sizeof(float)) / 4;
        if (SDL_AudioStreamAvailable(fm_) > limit || SDL_AudioStreamAvailable(pcm_) > limit) {
            SDL_AudioStreamClear(fm_);
            SDL_AudioStreamClear(pcm_);
        }
        if (SDL_AudioStreamPut(fm_, fm.data(), int(fm.size() * sizeof(float))) < 0 ||
            SDL_AudioStreamPut(pcm_, pcm.data(), int(pcm.size() * sizeof(float))) < 0) {
            std::fprintf(stderr, "audio queue: %s\n", SDL_GetError());
        }
        const bool ready = SDL_AudioStreamAvailable(fm_) >= 2048 * 2 * int(sizeof(float)) &&
                           SDL_AudioStreamAvailable(pcm_) >= 2048 * 2 * int(sizeof(float));
        SDL_UnlockAudioDevice(device_);
        if (!playing_ && ready) {
            SDL_PauseAudioDevice(device_, 0);
            playing_ = true;
        }
    }

    void pause() {
        if (!device_) return;
        SDL_PauseAudioDevice(device_, 1);
        SDL_LockAudioDevice(device_);
        SDL_AudioStreamClear(fm_);
        SDL_AudioStreamClear(pcm_);
        SDL_UnlockAudioDevice(device_);
        playing_ = false;
    }

    void volume(float value) {
        if (device_) SDL_LockAudioDevice(device_);
        volume_ = std::clamp(value, 0.0f, 1.0f);
        if (device_) SDL_UnlockAudioDevice(device_);
    }

    void mute(bool muted) {
        if (device_) SDL_LockAudioDevice(device_);
        muted_ = muted;
        if (device_) SDL_UnlockAudioDevice(device_);
    }

    void close() {
        if (device_) SDL_CloseAudioDevice(device_);
        device_ = 0;
        if (fm_) SDL_FreeAudioStream(fm_);
        if (pcm_) SDL_FreeAudioStream(pcm_);
        fm_ = pcm_ = nullptr;
        playing_ = false;
    }

private:
    static void callback(void *userdata, Uint8 *buffer, int bytes) {
#if defined(__SWITCH__)
        static thread_local bool s_affinity_set = false;
        if (!s_affinity_set) {
            s_affinity_set = true;
            svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, 0b0111);
            svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);
        }
#endif
        auto &self = *static_cast<Audio *>(userdata);
        std::memset(buffer, 0, size_t(bytes));
        auto *out = reinterpret_cast<int16_t *>(buffer);
        int samples = bytes / int(sizeof(int16_t));
        while (samples > 0) {
            float fm[512]{}, pcm[512]{};
            const int n = std::min(samples, 512);
            auto get = [n](SDL_AudioStream *stream, float *dst) {
                const int available = SDL_AudioStreamAvailable(stream);
                if (available > 0)
                    SDL_AudioStreamGet(stream, dst, std::min(available, n * int(sizeof(float))));
            };
            get(self.fm_, fm);
            get(self.pcm_, pcm);
            for (int i = 0; i < n; ++i) {
                float mixed = self.muted_ ? 0.f : (fm[i] + pcm[i]) * self.volume_;
                if (!std::isfinite(mixed)) mixed = 0.f;
                out[i] = int16_t(std::clamp(mixed, -1.f, 1.f) * 32767.f);
            }
            samples -= n;
            out += n;
        }
    }

    SDL_AudioDeviceID device_ = 0;
    SDL_AudioStream *fm_ = nullptr, *pcm_ = nullptr;
    int rate_ = 48000;
    bool playing_ = false, muted_ = false;
    float volume_ = 0.8f;
};

} // namespace switch_app
