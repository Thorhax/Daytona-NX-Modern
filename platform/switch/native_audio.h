#pragma once

#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#if defined(__SWITCH__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <switch.h>
#pragma GCC diagnostic pop
#endif

namespace switch_app {

// The device callback owns the native sequencer and mixer. Main only submits
// command bytes to a bounded SPSC queue; graphics frames never clock audio.
// All lifecycle calls belong to main. Close the device before destroying SDL.
template<class Engine> class NativeAudio {
public:
    using Clock = uint64_t (*)();
    static constexpr uint32_t kQueueSize = 16384;
    static constexpr unsigned kChunkFrames = 256;
    struct Stats {
        uint32_t callbacks = 0, frames = 0, queued = 0, overflows = 0;
        uint32_t last_us = 0, peak_us = 0, failed = 0;
        uint32_t unsupported = 0, invalid = 0, notes = 0, voices = 0;
    };
    NativeAudio() = default;
    NativeAudio(const NativeAudio &) = delete;
    NativeAudio &operator=(const NativeAudio &) = delete;
    ~NativeAudio() { close(); }

    bool open(std::unique_ptr<Engine> engine, Clock clock = nullptr) {
        close();
        if (!engine) return false;
        engine_ = std::move(engine);
        clock_ = clock;
        read_ = write_ = callbacks_ = frames_ = overflows_ = last_us_ = peak_us_ = failed_ = 0;
        unsupported_ = invalid_ = notes_ = voices_ = 0;
        SDL_AudioSpec want{}, got{};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = callback;
        want.userdata = this;
        device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
        if (!device_) { engine_.reset(); return false; }
        if (got.freq != want.freq || got.format != want.format || got.channels != want.channels) {
            close(); return false;
        }
        return true;
    }

    bool available() const { return device_ != 0; }
    void resume() { if (device_ && !playing_) { SDL_PauseAudioDevice(device_, 0); playing_ = true; } }
    void pause() { if (device_ && playing_) { SDL_PauseAudioDevice(device_, 1); playing_ = false; } }
    void close() {
        if (device_) SDL_CloseAudioDevice(device_);
        device_ = 0; playing_ = false;
        engine_.reset();
    }
    void volume(float value) {
        if (!std::isfinite(value)) value = 0;
        gain_.store(uint32_t(std::clamp(value, 0.f, 1.f) * 65536.f), std::memory_order_relaxed);
    }
    void mute(bool value) { muted_.store(value, std::memory_order_relaxed); }

    bool send(const uint8_t *data, size_t size) {
        if (!size) return true;
        if (!device_ || !data || size > kQueueSize || failed_.load(std::memory_order_relaxed)) return false;
        const uint32_t write = write_.load(std::memory_order_relaxed);
        const uint32_t read = read_.load(std::memory_order_acquire);
        if (size > kQueueSize - uint32_t(write - read)) {
            overflows_.fetch_add(1, std::memory_order_relaxed); return false;
        }
        for (size_t i = 0; i < size; ++i) queue_[(write + uint32_t(i)) % kQueueSize] = data[i];
        write_.store(write + uint32_t(size), std::memory_order_release);
        return true;
    }

    Stats stats() const {
        Stats s;
        s.callbacks = callbacks_.load(std::memory_order_relaxed);
        s.frames = frames_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        s.queued = write_.load(std::memory_order_acquire) - read;
        s.overflows = overflows_.load(std::memory_order_relaxed);
        s.last_us = last_us_.load(std::memory_order_relaxed);
        s.peak_us = peak_us_.load(std::memory_order_relaxed);
        s.failed = failed_.load(std::memory_order_relaxed);
        s.unsupported = unsupported_.load(std::memory_order_relaxed);
        s.invalid = invalid_.load(std::memory_order_relaxed);
        s.notes = notes_.load(std::memory_order_relaxed);
        s.voices = voices_.load(std::memory_order_relaxed);
        return s;
    }

private:
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "native audio needs lock-free 32-bit atomics");
    SDL_AudioDeviceID device_ = 0;
    bool playing_ = false;
    std::unique_ptr<Engine> engine_;
    Clock clock_ = nullptr;
    std::array<uint8_t, kQueueSize> queue_{};
    std::atomic<uint32_t> read_{0}, write_{0};
    std::atomic<uint32_t> callbacks_{0}, frames_{0}, overflows_{0}, last_us_{0}, peak_us_{0}, failed_{0};
    std::atomic<uint32_t> gain_{52428}, muted_{0};
    std::atomic<uint32_t> unsupported_{0}, invalid_{0}, notes_{0}, voices_{0};

    void receive() {
        uint32_t read = read_.load(std::memory_order_relaxed);
        const uint32_t write = write_.load(std::memory_order_acquire);
        while (read != write) {
            const uint32_t count = std::min(write - read, kQueueSize - read % kQueueSize);
            engine_->send(queue_.data() + read % kQueueSize, count);
            read += count;
        }
        read_.store(read, std::memory_order_release);
    }

    static void callback(void *context, Uint8 *buffer, int bytes) noexcept {
        if (bytes <= 0) return;
#if defined(__SWITCH__)
        static thread_local bool s_affinity_set = false;
        if (!s_affinity_set) {
            s_affinity_set = true;
            svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, 0b0111);
            svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);
        }
#endif
        auto &self = *static_cast<NativeAudio *>(context);
        std::memset(buffer, 0, size_t(bytes));
        if (self.failed_.load(std::memory_order_relaxed)) return;
        const uint64_t begin = self.clock_ ? self.clock_() : 0;
        try {
            self.receive();
            auto *out = reinterpret_cast<int16_t *>(buffer);
            unsigned remaining = unsigned(bytes) / (2u * sizeof(int16_t));
            const unsigned total = remaining;
            float mix[kChunkFrames * 2];
            const float gain = self.muted_.load(std::memory_order_relaxed) ? 0.f
                : float(self.gain_.load(std::memory_order_relaxed)) / 65536.f;
            while (remaining) {
                const unsigned count = std::min(remaining, kChunkFrames);
                self.engine_->render(mix, count);
                for (unsigned i = 0; i < count * 2; ++i) {
                    const float value = std::isfinite(mix[i]) ? mix[i] * gain : 0.f;
                    *out++ = int16_t(std::clamp(value, -1.f, 1.f) * 32767.f);
                }
                remaining -= count;
            }
            if constexpr (requires { self.engine_->stats().unsupported; }) {
                const auto state = self.engine_->stats();
                self.unsupported_.store(uint32_t(state.unsupported), std::memory_order_relaxed);
                self.invalid_.store(uint32_t(state.invalid), std::memory_order_relaxed);
                self.notes_.store(uint32_t(state.notes), std::memory_order_relaxed);
                self.voices_.store(state.voices, std::memory_order_relaxed);
            }
            self.frames_.fetch_add(total, std::memory_order_relaxed);
            self.callbacks_.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            self.failed_.store(1, std::memory_order_relaxed);
            std::memset(buffer, 0, size_t(bytes));
        }
        const uint64_t end = self.clock_ ? self.clock_() : 0;
        const uint32_t elapsed = uint32_t(std::min<uint64_t>(end >= begin ? end - begin : 0, UINT32_MAX));
        self.last_us_.store(elapsed, std::memory_order_relaxed);
        self.peak_us_.store(std::max(self.peak_us_.load(std::memory_order_relaxed), elapsed), std::memory_order_relaxed);
    }
};

} // namespace switch_app
