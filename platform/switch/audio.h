#pragma once
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif
#include "runtime/sound_board.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
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

    bool open(bool is_scsp = false) {
        is_scsp_ = is_scsp;
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

        if (is_scsp_) {
            fm_ = nullptr;
            pcm_ = SDL_NewAudioStream(AUDIO_F32SYS, 2, 44100, AUDIO_F32SYS, 2, rate_);
            if (!pcm_) {
                close();
                return false;
            }
        } else {
            fm_ = SDL_NewAudioStream(AUDIO_F32SYS, 2, int(snd::SoundBoard::kYmClock / 144.0 + 0.5),
                                     AUDIO_F32SYS, 2, rate_);
            pcm_ = SDL_NewAudioStream(AUDIO_F32SYS, 2, int(snd::SoundBoard::kPcmClock / 224.0 + 0.5),
                                      AUDIO_F32SYS, 2, rate_);
            if (!fm_ || !pcm_) {
                close();
                return false;
            }
        }
        return true;
    }

    void push(snd::SoundBoard &board) {
        if (!device_) return;
        if (is_scsp_) {
            auto pcm = board.take_pcm();
            if (pcm.empty()) return;

            // DC blocking filter on SCSP audio:
            // y[n] = x[n] - x[n-1] + R * y[n-1] with R = 0.995f (~35 Hz cutoff)
            // and kScspGain: the SCSP mix peaks near 0.2 of full scale (MAME's
            // level) once its reverb works; this brings it back to a usable
            // loudness, with headroom left (the callback clamps).
            const bool mono = arcade_mono_;
            for (size_t i = 0; i < pcm.size(); i += 2) {
                const float r = pcm[i + 1] * kScspGain;
                const float l = mono ? r : pcm[i] * kScspGain;
                dc_yl_ = l - dc_xl_ + 0.995f * dc_yl_;
                dc_xl_ = l;
                dc_yr_ = r - dc_xr_ + 0.995f * dc_yr_;
                dc_xr_ = r;
                pcm[i] = dc_yl_;
                pcm[i + 1] = dc_yr_;
            }

            SDL_LockAudioDevice(device_);
            // Keep the output queue near kTargetMs. Emulation (57.52 Hz frames
            // on the system tick) and the audio device drift apart slowly; a
            // queue left to grow (the old 2 s cap) delays hit sounds behind
            // the picture. Small drift: drop or repeat a few frames spread
            // over this push (about 0.4%, inaudible). Far off (a stall, a
            // burst of catch-up frames): trim the excess at once.
            constexpr int kBytesPerFrame = 2 * int(sizeof(float));
            const int queued = SDL_AudioStreamAvailable(pcm_) / kBytesPerFrame; // at the device rate
            const int target = rate_ * kTargetMs / 1000;
            const size_t frames = pcm.size() / 2;
            if (queued > target * 4) {
                int excess = (queued - target) * kBytesPerFrame;
                float discard[512];
                while (excess > 0) {
                    const int chunk = std::min(excess, int(sizeof(discard)));
                    SDL_AudioStreamGet(pcm_, discard, chunk);
                    excess -= chunk;
                }
            } else if (frames >= 256 && (queued > target * 3 / 2 || (playing_ && queued < target / 2))) {
                const bool drop = queued > target;
                const size_t n = frames / 256; // frames to drop or repeat
                std::vector<float> adj;
                adj.reserve((frames + n) * 2);
                for (size_t i = 0; i < frames; ++i) {
                    const bool mark = (i % 256) == 128;
                    if (drop && mark) continue;
                    adj.push_back(pcm[i * 2]);
                    adj.push_back(pcm[i * 2 + 1]);
                    if (!drop && mark) {
                        adj.push_back(pcm[i * 2]);
                        adj.push_back(pcm[i * 2 + 1]);
                    }
                }
                pcm.swap(adj);
            }
            if (SDL_AudioStreamPut(pcm_, pcm.data(), int(pcm.size() * sizeof(float))) < 0) {
                std::fprintf(stderr, "audio queue: %s\n", SDL_GetError());
            }
            // Prebuffer 60ms before initiating playback
            const int prebuffer = int(rate_ * 0.06f) * 2 * int(sizeof(float));
            const bool ready = SDL_AudioStreamAvailable(pcm_) >= prebuffer;
            SDL_UnlockAudioDevice(device_);
            if (!playing_ && ready) {
                SDL_PauseAudioDevice(device_, 0);
                playing_ = true;
            }
        } else {
            const auto fm = board.take_fm(), pcm = board.take_pcm();
            SDL_LockAudioDevice(device_);
            const int max_queue = rate_ * 2 * int(sizeof(float)) * 2;
            int avail_fm = SDL_AudioStreamAvailable(fm_);
            int avail_pcm = SDL_AudioStreamAvailable(pcm_);
            if (avail_fm > max_queue || avail_pcm > max_queue) {
                float discard[512];
                int excess_fm = std::max(0, avail_fm - max_queue) & ~7;
                while (excess_fm > 0) {
                    int chunk = std::min(excess_fm, int(sizeof(discard)));
                    SDL_AudioStreamGet(fm_, discard, chunk);
                    excess_fm -= chunk;
                }
                int excess_pcm = std::max(0, avail_pcm - max_queue) & ~7;
                while (excess_pcm > 0) {
                    int chunk = std::min(excess_pcm, int(sizeof(discard)));
                    SDL_AudioStreamGet(pcm_, discard, chunk);
                    excess_pcm -= chunk;
                }
            }
            if (SDL_AudioStreamPut(fm_, fm.data(), int(fm.size() * sizeof(float))) < 0 ||
                SDL_AudioStreamPut(pcm_, pcm.data(), int(pcm.size() * sizeof(float))) < 0) {
                std::fprintf(stderr, "audio queue: %s\n", SDL_GetError());
            }
            const int prebuffer = int(rate_ * 0.06f) * 2 * int(sizeof(float));
            const bool ready = SDL_AudioStreamAvailable(fm_) >= prebuffer &&
                               SDL_AudioStreamAvailable(pcm_) >= prebuffer;
            SDL_UnlockAudioDevice(device_);
            if (!playing_ && ready) {
                SDL_PauseAudioDevice(device_, 0);
                playing_ = true;
            }
        }
    }

    // VF2 pans every attack sound hard right (DIPAN 0x08: left -24 dB) and
    // nothing hard left: mixed for a mono cabinet speaker on the right
    // channel. Arcade mono plays that channel on both speakers; stereo leaves
    // the hits on the right speaker only, about 3 dB weaker than the
    // centred music.
    void set_arcade_mono(bool on) { arcade_mono_ = on; }

    // Audio waiting in the output queue, in ms.
    double queued_ms() {
        if (!device_ || !pcm_) return 0.0;
        SDL_LockAudioDevice(device_);
        const int bytes = SDL_AudioStreamAvailable(pcm_);
        SDL_UnlockAudioDevice(device_);
        return double(bytes) / (2.0 * sizeof(float)) * 1000.0 / double(rate_);
    }

    void pause() {
        if (!device_) return;
        SDL_PauseAudioDevice(device_, 1);
        SDL_LockAudioDevice(device_);
        if (fm_) SDL_AudioStreamClear(fm_);
        if (pcm_) SDL_AudioStreamClear(pcm_);
        dc_xl_ = dc_yl_ = dc_xr_ = dc_yr_ = 0.0f;
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
            svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, 0b0110); // off the emulation core
            svcSetThreadPriority(CUR_THREAD_HANDLE, 0x28);
        }
#endif
        auto &self = *static_cast<Audio *>(userdata);
        auto *out = reinterpret_cast<int16_t *>(buffer);
        const int total_frames = bytes / (2 * int(sizeof(int16_t))); // 2 channels * 2 bytes/sample

        int frames_remaining = total_frames;
        int out_offset = 0;

        while (frames_remaining > 0) {
            constexpr int kChunk = 256;
            const int cur_frames = std::min(frames_remaining, kChunk);
            const int req_bytes = cur_frames * 2 * int(sizeof(float));

            float pcm_buf[kChunk * 2] = {};
            float fm_buf[kChunk * 2] = {};

            int got_pcm = 0;
            if (self.pcm_) {
                // Must be multiple of 8 bytes (stereo float frame)
                const int avail = SDL_AudioStreamAvailable(self.pcm_) & ~7;
                if (avail > 0) {
                    const int to_read = std::min(avail, req_bytes);
                    got_pcm = SDL_AudioStreamGet(self.pcm_, pcm_buf, to_read);
                    if (got_pcm < 0) got_pcm = 0;
                    got_pcm &= ~7;
                }
            }

            int got_fm = 0;
            if (self.fm_) {
                const int avail = SDL_AudioStreamAvailable(self.fm_) & ~7;
                if (avail > 0) {
                    const int to_read = std::min(avail, req_bytes);
                    got_fm = SDL_AudioStreamGet(self.fm_, fm_buf, to_read);
                    if (got_fm < 0) got_fm = 0;
                    got_fm &= ~7;
                }
            }

            const int got_frames = std::max(got_pcm, got_fm) / (2 * int(sizeof(float)));
            if (got_frames == 0) {
                // Buffer starvation: zero-fill remaining buffer and exit without chopping
                std::memset(out + out_offset * 2, 0, size_t(frames_remaining * 2 * sizeof(int16_t)));
                break;
            }

            const float vol = self.muted_ ? 0.0f : self.volume_;
            for (int i = 0; i < got_frames; ++i) {
                float l = (pcm_buf[i * 2 + 0] + fm_buf[i * 2 + 0]) * vol;
                float r = (pcm_buf[i * 2 + 1] + fm_buf[i * 2 + 1]) * vol;
                if (!std::isfinite(l)) l = 0.0f;
                if (!std::isfinite(r)) r = 0.0f;
                out[(out_offset + i) * 2 + 0] = int16_t(std::clamp(l, -1.0f, 1.0f) * 32767.0f);
                out[(out_offset + i) * 2 + 1] = int16_t(std::clamp(r, -1.0f, 1.0f) * 32767.0f);
            }

            out_offset += got_frames;
            frames_remaining -= got_frames;
        }
    }

    static constexpr int kTargetMs = 90; // SCSP output queue (latency) to hold
    static constexpr float kScspGain = 2.5f;

    SDL_AudioDeviceID device_ = 0;
    SDL_AudioStream *fm_ = nullptr, *pcm_ = nullptr;
    int rate_ = 48000;
    bool playing_ = false, muted_ = false;
    bool is_scsp_ = false;
    std::atomic<bool> arcade_mono_{true};
    float volume_ = 0.8f;
    float dc_xl_ = 0.0f, dc_yl_ = 0.0f;
    float dc_xr_ = 0.0f, dc_yr_ = 0.0f;
};

} // namespace switch_app
