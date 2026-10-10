// Nintendo Switch native port for Daytona USA Recomp
#define SDL_MAIN_HANDLED
#include "audio.h"
#include "controls.h"
#include "native_audio.h"
#include "performance.h"
#include "text.h"
#include "runtime/game_loop.h"
#include "runtime/rom_import.h"
#include "runtime/native_sound_engine.h"

#include <switch.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>
#include <thread>
#include <cstring>
#include <vector>

#if defined(__SWITCH__)
extern "C" void userAppInit(void) {
    romfsInit();
    mkdir("sdmc:/switch", 0777);
#if defined(M2_ROMSET_VF2)
    mkdir("sdmc:/switch/vf2", 0777);
    chdir("sdmc:/switch/vf2");
#else
    mkdir("sdmc:/switch/daytona", 0777);
    chdir("sdmc:/switch/daytona");
#endif
}

extern "C" void userAppExit(void) {
    romfsExit();
}
#endif

namespace {

constexpr int kDisplayWidth = 1280;
constexpr int kDisplayHeight = 720;
#if defined(M2_ROMSET_VF2)
constexpr const char *kDirectory = "sdmc:/switch/vf2";
constexpr const char *kAppName = "Virtua Fighter 2";
constexpr const char *kPerfLogName = "vf2-perf.log";
#else
constexpr const char *kDirectory = "sdmc:/switch/daytona";
constexpr const char *kAppName = "Daytona USA";
constexpr const char *kPerfLogName = "daytona-perf.log";
#endif

PadState g_pad[2];

void init_pad() {
    padConfigureInput(2, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad[0]);
    padInitialize(&g_pad[1], HidNpadIdType_No2);
}

switch_app::Pad read_pad(int idx = 0) {
    padUpdate(&g_pad[idx]);
    const u64 kDown = padGetButtons(&g_pad[idx]);
    const HidAnalogStickState left = padGetStickPos(&g_pad[idx], 0);
    const HidAnalogStickState right = padGetStickPos(&g_pad[idx], 1);

    switch_app::Pad pad{};
    pad.lx = float(left.x) / 32767.0f;
    pad.ly = float(left.y) / 32767.0f;
    pad.rx = float(right.x) / 32767.0f;
    pad.ry = float(right.y) / 32767.0f;

    if (kDown & HidNpadButton_A) pad.buttons |= switch_app::A;
    if (kDown & HidNpadButton_B) pad.buttons |= switch_app::B;
    if (kDown & HidNpadButton_X) pad.buttons |= switch_app::X;
    if (kDown & HidNpadButton_Y) pad.buttons |= switch_app::Y;
    if (kDown & HidNpadButton_Up) pad.buttons |= switch_app::Up;
    if (kDown & HidNpadButton_Down) pad.buttons |= switch_app::Down;
    if (kDown & HidNpadButton_Left) pad.buttons |= switch_app::Left;
    if (kDown & HidNpadButton_Right) pad.buttons |= switch_app::Right;
    if (kDown & HidNpadButton_L) pad.buttons |= switch_app::L;
    if (kDown & HidNpadButton_R) pad.buttons |= switch_app::R;
    if (kDown & HidNpadButton_ZL) pad.buttons |= switch_app::ZL;
    if (kDown & HidNpadButton_ZR) pad.buttons |= switch_app::ZR;
    if (kDown & HidNpadButton_Plus) pad.buttons |= switch_app::Plus;
    if (kDown & HidNpadButton_Minus) pad.buttons |= switch_app::Minus;
    if (kDown & HidNpadButton_StickL) pad.buttons |= switch_app::StickL;
    if (kDown & HidNpadButton_StickR) pad.buttons |= switch_app::StickR;

    return pad;
}

bool load_bytes(const std::string &path, uint8_t *data, size_t size) {
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    std::vector<uint8_t> temp(size);
    const bool ok = std::fread(temp.data(), 1, size, file) == size && !std::ferror(file);
    std::fclose(file);
    if (ok) std::copy(temp.begin(), temp.end(), data);
    return ok;
}

bool save_bytes(const std::string &path, const uint8_t *data, size_t size) {
    const std::string temp = path + ".tmp";
    std::FILE *file = std::fopen(temp.c_str(), "wb");
    if (!file) return false;
    bool ok = std::fwrite(data, 1, size, file) == size;
    if (std::fclose(file) != 0) ok = false;
    if (ok) std::rename(temp.c_str(), path.c_str());
    return ok;
}

std::string find_rom_file() {
#if defined(M2_ROMSET_VF2)
    static const char *candidates[] = {
        "sdmc:/switch/vf2/vf2.zip",
        "vf2.zip",
        "romfs:/vf2.zip",
        "/switch/vf2/vf2.zip",
        "sdmc:/switch/vf2.zip",
    };
    for (const char *path : candidates) {
        struct stat st{};
        if (stat(path, &st) == 0 && st.st_size > 0) {
            return std::string(path);
        }
    }
    return "vf2.zip";
#else
    static const char *candidates[] = {
        "sdmc:/switch/daytona/daytona.zip",
        "daytona.zip",
        "romfs:/daytona.zip",
        "sdmc:/switch/daytona/daytona93.zip",
        "daytona93.zip",
        "romfs:/daytona93.zip",
    };
    for (const char *path : candidates) {
        struct stat st{};
        if (stat(path, &st) == 0 && st.st_size > 0) {
            return std::string(path);
        }
    }
    return "daytona.zip";
#endif
}

constexpr uint8_t kDefaultEeprom[128] = {
    0x53, 0x45, 0x47, 0x41, 0x40, 0x82, 0x02, 0x00, 0x39, 0x7c, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x02, 0x14, 0x1c, 0x00, 0x01, 0x00, 0x01, 0x04, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00
};

#if defined(M2_ROMSET_VF2)
class SoundWorker {
public:
    SoundWorker(switch_app::Audio &audio) : audio_(audio) {
        running_ = true;
        thread_ = std::thread(&SoundWorker::loop, this);
    }
    ~SoundWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            has_work_ = true;
        }
        cv_work_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

    // Returns how long the caller waited for the previous frame's sound.
    uint64_t submit(rt::GameLoop::SoundPacket packet) {
        const uint64_t before = armGetSystemTick();
        wait();
        const uint64_t waited = armGetSystemTick() - before;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            packet_ = std::move(packet);
            has_work_ = true;
            done_ = false;
        }
        cv_work_.notify_one();
        return waited;
    }
    // System ticks spent running sound frames since the last call.
    uint64_t take_busy() { return busy_.exchange(0); }

    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_done_.wait(lock, [this]() { return done_; });
    }

private:
    void loop() {
#if defined(__SWITCH__)
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 0b0110);
        svcSetThreadPriority(CUR_THREAD_HANDLE, 0x29);
#endif
        std::unique_lock<std::mutex> lock(mutex_);
        while (running_) {
            cv_work_.wait(lock, [this]() { return has_work_; });
            if (!running_) break;
            has_work_ = false;
            auto packet = std::move(packet_);
            lock.unlock();

            const uint64_t begin = armGetSystemTick();
            auto *sound = packet.sound();
            packet.execute();
            if (sound) {
                audio_.push(*sound);
            }
            busy_ += armGetSystemTick() - begin;

            lock.lock();
            done_ = true;
            cv_done_.notify_one();
        }
    }

    switch_app::Audio &audio_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_work_;
    std::condition_variable cv_done_;
    rt::GameLoop::SoundPacket packet_;
    std::atomic<uint64_t> busy_{0};
    bool running_ = false;
    bool has_work_ = false;
    bool done_ = true;
};
#endif

} // namespace

int main(int, char **) {
#if defined(__SWITCH__)
    // Core 0 only: the emulation thread. The higher-priority sound worker
    // and audio callback stay on cores 1-2 so they never preempt it; the
    // lower-priority drawing threads may still use core 0 when it idles.
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 0b0001);
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);
#endif
    init_pad();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        kAppName,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kDisplayWidth, kDisplayHeight,
        SDL_WINDOW_FULLSCREEN
    );
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1,
        SDL_RENDERER_ACCELERATED
    );
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_RenderSetLogicalSize(renderer, kDisplayWidth, kDisplayHeight);

    SDL_Texture *screen_texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        rt::GameLoop::kWidth,
        rt::GameLoop::kHeight
    );
    if (!screen_texture) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetTextureBlendMode(screen_texture, SDL_BLENDMODE_NONE);
    int texture_width = rt::GameLoop::kWidth;

    // Open telemetry performance log file
    FILE *perf_log_file = std::fopen(kPerfLogName, "a");
    if (perf_log_file) {
        std::fprintf(perf_log_file, "\n=== %s RECOMP SWITCH LOG START ===\n", kAppName);
        std::fflush(perf_log_file);
    }

    switch_app::Audio audio;
#if defined(M2_ROMSET_VF2)
    SoundWorker sound_worker(audio);
#else
    switch_app::NativeAudio<snd::NativeSoundEngine> native_audio;
#endif
    const double perf_freq = double(SDL_GetPerformanceFrequency());
    switch_app::Performance perf(perf_freq);

    std::unique_ptr<rt::GameLoop> game;
    switch_app::Controls controls;
    // Allow up to 2 steps catchup if vsync is engaged
    switch_app::FrameClock clock(rt::GameLoop::kFrameHz, 2);

    constexpr uint64_t kArcadeFrameNs = 17'384'000ULL; // 57.524 Hz arcade frame period
    uint64_t next_frame_ns = 0;                         // native pacing: when the next frame is due
    // Diagnostics line in the perf log (per 2 s window).
    struct Diag {
        uint64_t frames = 0, instr = 0, capped = 0, core_max = 0, snd_wait = 0, aq_ms = 0, last_instr = 0;
    } diag;
    bool running = true;
    bool menu = true;
    bool have_frame = false;
    bool wait_release = true;
    int selection = 0;
    bool test_switch_held = false;
#if !defined(M2_ROMSET_VF2)
    bool use_native_audio = true; // Native audio skips 68000 CPU emulation for huge speedup!
#endif
    bool show_perf_overlay = false;
    bool vsync_mode = false; // 57.52 Hz native frame pacing by default
    uint8_t pulse = 0;
    uint32_t previous_buttons = 0;
    bool muted = false;
    bool arcade_mono = true; // VF2: see Audio::set_arcade_mono
    bool widescreen = true;  // 16:9: more of the scene at the sides (GameLoop::set_aspect)
    auto screen_aspect = [&]() { return widescreen ? 16.0 / 9.0 : 0.0; };
    std::string banner;       // short in-game message (cheat toggles)
    uint32_t banner_until = 0;

    std::string rom_path = find_rom_file();
#if defined(M2_ROMSET_VF2)
    std::string status = "Place vf2.zip in sdmc:/switch/vf2/ then choose START GAME.";
#else
    std::string status = "Place daytona.zip in sdmc:/switch/daytona/ then choose START GAME.";
#endif

    auto save = [&]() {
#if defined(M2_ROMSET_VF2)
        sound_worker.wait();
#endif
        if (!game) return true;
#if defined(M2_ROMSET_VF2)
        save_bytes("backup_ram.bin", game->board().backup_ram().data(), game->board().backup_ram().size());
        save_bytes("ioboard_eeprom.bin", game->board().io().eeprom.data(), game->board().io().eeprom.size());
#else
        save_bytes("ioboard_eeprom.bin", game->board().io().eeprom.data(), game->board().io().eeprom.size());
        save_bytes("backup_ram.bin", game->board().backup_ram().data(), game->board().backup_ram().size());
#endif
        return true;
    };

    auto load_nvram = [&]() {
        if (!game) return;
#if defined(M2_ROMSET_VF2)
        bool loaded = load_bytes("backup_ram.bin", game->board().backup_ram().data(), game->board().backup_ram().size());
        if (!loaded) {
            load_bytes("romfs:/backup_ram.bin", game->board().backup_ram().data(), game->board().backup_ram().size());
        }
        load_bytes("ioboard_eeprom.bin", game->board().io().eeprom.data(), game->board().io().eeprom.size());
#else
        bool loaded = load_bytes("ioboard_eeprom.bin", game->board().io().eeprom.data(), game->board().io().eeprom.size());
        if (!loaded || game->board().io().eeprom[0] != 'S' || game->board().io().eeprom[0x1a] != 0x00) {
            std::copy(std::begin(kDefaultEeprom), std::end(kDefaultEeprom), game->board().io().eeprom.begin());
            save_bytes("ioboard_eeprom.bin", game->board().io().eeprom.data(), game->board().io().eeprom.size());
        }
        load_bytes("backup_ram.bin", game->board().backup_ram().data(), game->board().backup_ram().size());
#endif
    };

    auto draw_menu = [&]() {
        SDL_SetRenderDrawColor(renderer, 18, 20, 28, 255);
        SDL_RenderClear(renderer);

        SDL_SetRenderDrawColor(renderer, 240, 240, 245, 255);
#if defined(M2_ROMSET_VF2)
        switch_app::text(renderer, "VIRTUA FIGHTER 2 - NINTENDO SWITCH", 40, 30, 3, 40, 1);

        const std::string labels[] = {
            game ? "RESUME GAME" : "START GAME",
            "RESET GAME",
            vsync_mode ? "FRAME PACING: 60 HZ VSYNC" : "FRAME PACING: 57.52 HZ NATIVE (RECOMMENDED)",
            show_perf_overlay ? "PERFORMANCE OVERLAY: [ON]" : "PERFORMANCE OVERLAY: [OFF]",
            test_switch_held ? "TEST SWITCH: (ON - HOLDING)" : "TEST SWITCH: (OFF - TAP A TO TOGGLE)",
            "INSERT SERVICE COIN",
            muted ? "SOUND: MUTED" : "SOUND: ON",
            arcade_mono ? "SOUND MIX: ARCADE MONO (RECOMMENDED)" : "SOUND MIX: STEREO",
            widescreen ? "SCREEN: 16:9 WIDESCREEN" : "SCREEN: 4:3 ORIGINAL",
            "EXIT TO HOMEBREW MENU"
        };
        constexpr int kMenuItems = 10;
#else
        switch_app::text(renderer, "DAYTONA USA RECOMP - NINTENDO SWITCH", 40, 30, 3, 40, 1);

        const std::string labels[] = {
            game ? "RESUME GAME" : "START GAME",
            "RESET GAME",
            use_native_audio ? "AUDIO ENGINE: NATIVE (FAST - RECOMMENDED)" : "AUDIO ENGINE: REFERENCE (68000 EMULATOR)",
            vsync_mode ? "FRAME PACING: 60 HZ VSYNC" : "FRAME PACING: 57.52 HZ NATIVE (RECOMMENDED)",
            show_perf_overlay ? "PERFORMANCE OVERLAY: [ON]" : "PERFORMANCE OVERLAY: [OFF]",
            test_switch_held ? "TEST SWITCH: (ON - HOLDING)" : "TEST SWITCH: (OFF - TAP A TO TOGGLE)",
            "INSERT SERVICE COIN",
            "RESET EEPROM TO SINGLE PLAYER",
            muted ? "SOUND: MUTED" : "SOUND: ON",
            widescreen ? "SCREEN: 16:9 WIDESCREEN" : "SCREEN: 4:3 ORIGINAL",
            "EXIT TO HOMEBREW MENU"
        };
        constexpr int kMenuItems = 11;
#endif

        for (int i = 0; i < kMenuItems; ++i) {
            if (i == selection) {
                SDL_SetRenderDrawColor(renderer, 255, 205, 50, 255);
            } else {
                SDL_SetRenderDrawColor(renderer, 210, 210, 220, 255);
            }
            switch_app::text(renderer, (i == selection ? "> " : "  ") + labels[i], 60, 75 + i * (kMenuItems > 10 ? 32 : 35), 2, 60, 1);
        }

        SDL_SetRenderDrawColor(renderer, 180, 180, 190, 255);
        switch_app::text(renderer, status, 40, 435, 2, 70, 2);

        SDL_SetRenderDrawColor(renderer, 100, 180, 255, 255);
#if defined(M2_ROMSET_VF2)
        switch_app::text(renderer, "CONTROLS: B: PUNCH  A: KICK  Y: GUARD  X: GUARD+KICK  (ZL/ZR/L/R: GUARD)", 40, 490, 2, 70, 1);
        switch_app::text(renderer, "MOVEMENT: D-PAD OR LEFT ANALOG STICK (8-WAY)", 40, 520, 2, 70, 1);
        switch_app::text(renderer, "TEST MODE: X=UP  B=DOWN  Y=OPT+  A=OPT-  PLUS=SELECT", 40, 550, 2, 70, 1);
        switch_app::text(renderer, "PLUS=START   MINUS=COIN   PLUS+MINUS=MENU", 40, 580, 2, 70, 1);
#else
        switch_app::text(renderer, "CONTROLS: ZR=GAS  ZL=BRAKE  L/R=SHIFT  L3=TEST  R3=SERVICE", 40, 490, 2, 70, 1);
        switch_app::text(renderer, "TEST MODE: X=UP  B=DOWN  Y=OPT+  A=OPT-  PLUS=SELECT", 40, 520, 2, 70, 1);
        switch_app::text(renderer, "VR VIEWS: B=BUMPER  A=CHASE  Y=FAR  X=COCKPIT", 40, 550, 2, 70, 1);
        switch_app::text(renderer, "PLUS=START   MINUS=COIN   PLUS+MINUS=MENU", 40, 580, 2, 70, 1);
#endif

        SDL_SetRenderDrawColor(renderer, 255, 205, 50, 255);
        switch_app::text(renderer, "A: SELECT   B: RESUME GAME   PLUS+MINUS: MENU", 40, 660, 2, 60, 1);
    };

    auto start_game = [&]() -> bool {
#if defined(M2_ROMSET_VF2)
        sound_worker.wait();
#endif
        save();
        audio.close();
#if !defined(M2_ROMSET_VF2)
        native_audio.close();
#endif
        game.reset();
        have_frame = false;
        status = "Loading ROM set: " + rom_path + "...";
        draw_menu();
        SDL_RenderPresent(renderer);

        try {
            auto images = rt::import_rom_set(rom_path);
#if defined(M2_ROMSET_VF2)
            if (!audio.open(true)) {
                throw std::runtime_error(std::string("Audio open failed: ") + SDL_GetError());
            }
            audio.mute(muted);
            audio.set_arcade_mono(arcade_mono);
            game = std::make_unique<rt::GameLoop>(std::move(images), true);
            // Voices and sound effects +3 dB over the music (see Scsp::set_effects_gain).
            if (game->sound()) game->sound()->set_effects_gain(1.41f);
#else
            if (use_native_audio) {
                auto engine = std::make_unique<snd::NativeSoundEngine>(
                    std::move(images.sound_program),
                    std::move(images.pcm1),
                    std::move(images.pcm2)
                );
                if (!native_audio.open(std::move(engine), []() -> uint64_t {
#if defined(__SWITCH__)
                    return armTicksToNs(armGetSystemTick()) / 1000ULL;
#else
                    return std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
                })) {
                    throw std::runtime_error(std::string("Native audio open failed: ") + SDL_GetError());
                }
                native_audio.volume(1.0f);
                native_audio.mute(muted);
                native_audio.resume();
                // When native audio is active, disable reference sound board to free CPU
                game = std::make_unique<rt::GameLoop>(std::move(images), false);
            } else {
                if (!audio.open()) {
                    throw std::runtime_error(std::string("Reference audio open failed: ") + SDL_GetError());
                }
                audio.mute(muted);
                game = std::make_unique<rt::GameLoop>(std::move(images), true);
            }
#endif

            // Set up high-precision profile clocks for microsecond telemetry
            game->set_profile_clock([]() -> uint64_t { return SDL_GetPerformanceCounter(); });
            game->board().video().set_profile_clock([]() -> uint64_t { return SDL_GetPerformanceCounter(); });
#if defined(M2_ROMSET_VF2)
            // Draw each picture on a worker while the next frame emulates
            // (one frame of extra latency; frees ~9 ms per frame in fights).
            game->board().video().set_threaded(true);
            game->set_aspect(screen_aspect());
            game->set_wrap_backdrop(true); // 2D skies continue into the margins
            game->set_pillarbox_2d(true);  // menus (3D only inside 4:3): black side bars
#else
            // As VF2: draw on a worker while the next frame runs (one frame
            // of extra latency), which pays for the wider 16:9 picture.
            game->board().video().set_threaded(true);
            game->set_aspect(screen_aspect()); // HUD stays centred (no HUD-at-edges)
#endif

            load_nvram();
            controls = switch_app::Controls{};
            pulse = 0;
#if defined(M2_ROMSET_VF2)
            status = "B: PUNCH  A: KICK  Y: GUARD  X: GUARD+KICK  PLUS: START  MINUS: COIN  ZL+ZR+UP: CHEAT";
#else
            status = "ZR: GAS   ZL: BRAKE   L/R: SHIFT   L-STICK: STEER   PLUS: START   MINUS: COIN";
#endif
            clock.reset();
            perf.reset(SDL_GetPerformanceCounter(), menu);
            return true;
        } catch (const std::exception &err) {
            status = std::string("Error: ") + err.what();
            return false;
        }
    };

    using Clock = std::chrono::steady_clock;
    auto last_time = Clock::now();

    while (running && appletMainLoop()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
        }

        const auto pad = read_pad(0);
        const uint32_t pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;

        if (wait_release) {
            controls.latch(pad.buttons);
            if (!pad.buttons) wait_release = false;
        }

        if (!menu && !wait_release && switch_app::menu_chord(pad.buttons)) {
            menu = true;
#if !defined(M2_ROMSET_VF2)
            if (use_native_audio) native_audio.pause();
            else audio.pause();
#else
            sound_worker.wait();
            audio.pause();
#endif
            save();
            clock.reset();
        }

#if defined(M2_ROMSET_VF2)
        // Cheat: ZL+ZR+D-pad Up toggles player 1 infinite health.
        if (!menu && game && !wait_release) {
            constexpr uint32_t kCheatChord = switch_app::ZL | switch_app::ZR | switch_app::Up;
            if ((pad.buttons & kCheatChord) == kCheatChord && (pressed & kCheatChord)) {
                const bool on = !game->p1_infinite_health();
                game->set_p1_infinite_health(on);
                banner = on ? "CHEAT: P1 INFINITE HEALTH ON" : "CHEAT: P1 INFINITE HEALTH OFF";
                banner_until = SDL_GetTicks() + 2500;
            }
        }
#endif

        if (menu && !wait_release) {
#if defined(M2_ROMSET_VF2)
            constexpr int kMenuItems = 10;
            if (pressed & switch_app::Up) selection = (selection + kMenuItems - 1) % kMenuItems;
            if (pressed & switch_app::Down) selection = (selection + 1) % kMenuItems;
            if ((pressed & switch_app::B) && game) {
                menu = false;
                audio.volume(1.0f);
                clock.reset();
            } else if (pressed & switch_app::A) {
                switch (selection) {
                case 0:
                    if (game || start_game()) {
                        menu = false;
                        clock.reset();
                    }
                    break;
                case 1:
                    if (start_game()) {
                        menu = false;
                        clock.reset();
                    }
                    break;
                case 2:
                    vsync_mode = !vsync_mode;
                    SDL_RenderSetVSync(renderer, vsync_mode ? 1 : 0);
                    status = vsync_mode ? "Frame pacing: 60 Hz VSync enabled." : "Frame pacing: 57.52 Hz Native enabled (recommended).";
                    break;
                case 3:
                    show_perf_overlay = !show_perf_overlay;
                    status = show_perf_overlay ? "Performance overlay ON." : "Performance overlay OFF.";
                    break;
                case 4:
                    test_switch_held = !test_switch_held;
                    status = test_switch_held ? "Test Switch held ON. Resume to enter/stay in Test Mode." : "Test Switch turned OFF.";
                    break;
                case 5:
                    pulse |= 0x08;
                    status = "Service switch coin pulsed.";
                    break;
                case 6:
                    muted = !muted;
                    audio.mute(muted);
                    break;
                case 7:
                    arcade_mono = !arcade_mono;
                    audio.set_arcade_mono(arcade_mono);
                    status = arcade_mono ? "Arcade mono: both speakers play the cabinet channel (hits full level)."
                                         : "Stereo: the SCSP's two channels as emulated (hits panned right).";
                    break;
                case 8:
                    widescreen = !widescreen;
                    if (game) game->set_aspect(screen_aspect());
                    status = widescreen ? "Widescreen 16:9: more of the stage at the sides; the HUD stays centred."
                                        : "Original 4:3 screen.";
                    break;
                case 9:
                    save();
                    running = false;
                    break;
                }
            }
#else
            constexpr int kMenuItems = 11;
            if (pressed & switch_app::Up) selection = (selection + kMenuItems - 1) % kMenuItems;
            if (pressed & switch_app::Down) selection = (selection + 1) % kMenuItems;
            if ((pressed & switch_app::B) && game) {
                menu = false;
                if (use_native_audio) native_audio.resume();
                clock.reset();
            } else if (pressed & switch_app::A) {
                switch (selection) {
                case 0:
                    if (game || start_game()) {
                        menu = false;
                        if (use_native_audio) native_audio.resume();
                        clock.reset();
                    }
                    break;
                case 1:
                    if (start_game()) {
                        menu = false;
                        if (use_native_audio) native_audio.resume();
                        clock.reset();
                    }
                    break;
                case 2:
                    use_native_audio = !use_native_audio;
                    status = use_native_audio ? "Switched to Native Audio (fast). Restarting..." : "Switched to Reference Audio. Restarting...";
                    start_game();
                    menu = false;
                    clock.reset();
                    break;
                case 3:
                    vsync_mode = !vsync_mode;
                    SDL_RenderSetVSync(renderer, vsync_mode ? 1 : 0);
                    status = vsync_mode ? "Frame pacing: 60 Hz VSync enabled." : "Frame pacing: 57.52 Hz Native enabled (recommended).";
                    break;
                case 4:
                    show_perf_overlay = !show_perf_overlay;
                    status = show_perf_overlay ? "Performance overlay ON." : "Performance overlay OFF.";
                    break;
                case 5:
                    test_switch_held = !test_switch_held;
                    status = test_switch_held ? "Test Switch held ON. Resume to enter/stay in Test Mode." : "Test Switch turned OFF.";
                    break;
                case 6:
                    pulse |= 0x08;
                    status = "Service switch coin pulsed.";
                    break;
                case 7:
                    if (game) {
                        std::copy(std::begin(kDefaultEeprom), std::end(kDefaultEeprom), game->board().io().eeprom.begin());
                        save_bytes("ioboard_eeprom.bin", game->board().io().eeprom.data(), game->board().io().eeprom.size());
                    }
                    status = "EEPROM restored to Single Player defaults. Restarting...";
                    start_game();
                    menu = false;
                    clock.reset();
                    break;
                case 8:
                    muted = !muted;
                    if (use_native_audio) native_audio.mute(muted);
                    else audio.mute(muted);
                    break;
                case 9:
                    widescreen = !widescreen;
                    if (game) game->set_aspect(screen_aspect());
                    status = widescreen ? "Widescreen 16:9: more of the road at the sides; the HUD stays centred."
                                        : "Original 4:3 screen.";
                    break;
                case 10:
                    save();
                    running = false;
                    break;
                }
            }
#endif
        }

        if (menu) {
            draw_menu();
            SDL_RenderPresent(renderer);
            svcSleepThread(16'666'666ULL);
            continue;
        }

        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - last_time).count();
        last_time = now;
        const uint64_t now_ticks = SDL_GetPerformanceCounter();

        bool run_step = false;
        if (game) {
            if (!vsync_mode) {
                run_step = true;
            } else {
                run_step = clock.advance(elapsed) > 0;
            }
        }

        if (run_step) {
            try {
#if defined(M2_ROMSET_VF2)
                const auto pad2 = read_pad(1);
                const auto input = controls.sample(wait_release ? switch_app::Pad{} : pad, pad2);
                rt::Inputs mapped;
                mapped.in0 = uint8_t(input.in0 & ~(test_switch_held ? 0x04 : 0x00) & ~pulse);
                mapped.in1 = input.in1;
                mapped.in2 = input.in2;
#else
                const auto input = controls.sample(wait_release ? switch_app::Pad{} : pad);
                rt::Inputs mapped;
                mapped.steer = input.steer;
                mapped.accel = input.accel;
                mapped.brake = input.brake;
                mapped.in0 = uint8_t(input.in0 & ~(test_switch_held ? 0x04 : 0x00) & ~pulse);
                mapped.in1 = input.in1;
                mapped.in2 = input.in2;
#endif

#if defined(M2_ROMSET_VF2)
                auto packet = game->run_frame_sound_packet(mapped);
                diag.snd_wait += sound_worker.submit(std::move(packet));
                pulse = 0;
                {
                    const uint64_t instr = game->instructions() - diag.last_instr;
                    diag.last_instr = game->instructions();
                    ++diag.frames;
                    diag.instr += instr;
                    if (instr >= rt::M2Board::kFrameInstructions * 9 / 10) ++diag.capped;
                    diag.core_max = std::max(diag.core_max, game->last_profile().core());
                    diag.aq_ms += uint64_t(audio.queued_ms());
                }

                perf.frame(game->last_profile());
                perf.video(game->board().video().last_profile());
#else
                game->run_frame(mapped);
                pulse = 0;

                perf.frame(game->last_profile());
                perf.video(game->board().video().last_profile());

                if (use_native_audio) {
                    const auto bytes = game->board().take_sound_bytes();
                    native_audio.send(bytes.data(), bytes.size());
                    const auto astats = native_audio.stats();
                    perf.add(switch_app::Performance::Audio, uint64_t(double(astats.last_us) * perf_freq / 1000000.0));
                } else if (game->sound()) {
                    audio.push(*game->sound());
                }
#endif
                have_frame = true;

                const uint64_t before_upload = SDL_GetPerformanceCounter();
                if (game->screen_width() != texture_width) {
                    // Widescreen toggled: a texture at the new width.
                    SDL_Texture *wider = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                                           SDL_TEXTUREACCESS_STREAMING,
                                                           game->screen_width(), rt::GameLoop::kHeight);
                    if (wider) {
                        SDL_DestroyTexture(screen_texture);
                        screen_texture = wider;
                        SDL_SetTextureBlendMode(screen_texture, SDL_BLENDMODE_NONE);
                        texture_width = game->screen_width();
                    }
                }
                if (game->screen_width() == texture_width)
                    SDL_UpdateTexture(
                        screen_texture,
                        nullptr,
                        game->screen().data(),
                        texture_width * int(sizeof(uint32_t))
                    );
                const uint64_t after_upload = SDL_GetPerformanceCounter();
                perf.span(switch_app::Performance::Upload, before_upload, after_upload);
            } catch (const std::exception &err) {
#if defined(M2_ROMSET_VF2)
                sound_worker.wait();
#endif
                status = std::string("Runtime error: ") + err.what();
                menu = true;
                game.reset();
                have_frame = false;
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (have_frame) {
            const uint64_t before_draw = SDL_GetPerformanceCounter();
            const int width = std::min(kDisplayWidth, kDisplayHeight * texture_width / rt::GameLoop::kHeight); // 930 at 4:3
            const SDL_Rect dst{(kDisplayWidth - width) / 2, 0, width, kDisplayHeight};
            SDL_RenderCopy(renderer, screen_texture, nullptr, &dst);
            const uint64_t after_draw = SDL_GetPerformanceCounter();
            perf.span(switch_app::Performance::Draw, before_draw, after_draw);

            if (!banner.empty() && SDL_GetTicks() < banner_until) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
                SDL_Rect box{(kDisplayWidth - 640) / 2, kDisplayHeight - 70, 640, 36};
                SDL_RenderFillRect(renderer, &box);
                SDL_SetRenderDrawColor(renderer, 255, 220, 60, 255);
                switch_app::text(renderer, banner, box.x + 20, box.y + 9, 2, 60, 1);
            }
            if (show_perf_overlay) {
                char overlay_str[128];
                perf.format_overlay(overlay_str, sizeof(overlay_str), now_ticks);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
                SDL_Rect bar{20, 10, 880, 28};
                SDL_RenderFillRect(renderer, &bar);
                SDL_SetRenderDrawColor(renderer, 50, 255, 100, 255);
                switch_app::text(renderer, overlay_str, 30, 16, 2, 70, 1);
            }
        }

        const uint64_t before_present = SDL_GetPerformanceCounter();
        SDL_RenderPresent(renderer);
        const uint64_t after_present = SDL_GetPerformanceCounter();
        perf.span(switch_app::Performance::Present, before_present, after_present);
        perf.presented();

        // Write telemetry log every 2 seconds during gameplay
        if (perf.ready(now_ticks)) {
            char log_line[512];
            perf.format(log_line, sizeof(log_line), now_ticks);
            if (perf_log_file) {
                std::fputs(log_line, perf_log_file);
#if defined(M2_ROMSET_VF2)
                if (diag.frames) {
                    // Fixed reference work, timed: if these slow down over a
                    // session the console is (clocks, heat), not the port.
                    static std::vector<uint8_t> bench_a(4u << 20, 1), bench_b(4u << 20);
                    uint64_t t0 = armGetSystemTick();
                    volatile uint32_t acc = 1;
                    for (uint32_t i = 0; i < 2'000'000; ++i) acc = acc * 1664525u + 1013904223u;
                    const double alu_ms = double(armTicksToNs(armGetSystemTick() - t0)) / 1e6;
                    t0 = armGetSystemTick();
                    std::memcpy(bench_b.data(), bench_a.data(), bench_a.size());
                    const double mem_ms = double(armTicksToNs(armGetSystemTick() - t0)) / 1e6;
                    const double f = double(diag.frames);
                    std::fprintf(perf_log_file,
                        "diag: instr_k=%.1f capped=%llu core_max_ms=%.2f snd_ms=%.2f snd_wait_ms=%.2f audio_q_ms=%.1f "
                        "sched_pending=%zu bench_alu_ms=%.2f bench_memcpy4m_ms=%.2f\n",
                        double(diag.instr) / f / 1000.0, (unsigned long long)diag.capped,
                        double(diag.core_max) * 1000.0 / perf_freq,
                        double(armTicksToNs(sound_worker.take_busy())) / 1e6 / f,
                        double(armTicksToNs(diag.snd_wait)) / 1e6 / f, double(diag.aq_ms) / f,
                        game ? game->pending_events() : size_t(0), alu_ms, mem_ms);
                }
                const uint64_t keep = diag.last_instr;
                diag = Diag{};
                diag.last_instr = keep;
#endif
                std::fflush(perf_log_file);
            }
            perf.reset(now_ticks, menu);
        }

        // Frame pacing for 57.524 Hz arcade rate, against a running deadline:
        // a frame that overruns is made up by the next ones starting at once,
        // so the game keeps arcade speed (and the audio queue stays fed)
        // when the average frame fits. More than 3 frames behind (a stall,
        // the menu): start counting afresh rather than rush.
        if (!vsync_mode) {
            const uint64_t now_ns = armTicksToNs(armGetSystemTick());
            if (next_frame_ns == 0 || now_ns > next_frame_ns + 3 * kArcadeFrameNs) next_frame_ns = now_ns;
            next_frame_ns += kArcadeFrameNs;
            if (now_ns < next_frame_ns) {
                const uint64_t sleep_ns = next_frame_ns - now_ns;
                if (sleep_ns > 1'000'000ULL) {
                    svcSleepThread(sleep_ns - 800'000ULL);
                }
                while (armTicksToNs(armGetSystemTick()) < next_frame_ns) {
                    // Precision spin-wait
                }
            }
        } else {
            next_frame_ns = 0;
        }
    }

#if defined(M2_ROMSET_VF2)
    sound_worker.wait();
#endif
    save();
    if (perf_log_file) {
        std::fprintf(perf_log_file, "=== %s RECOMP SWITCH LOG END ===\n", kAppName);
        std::fclose(perf_log_file);
    }
    audio.close();
#if !defined(M2_ROMSET_VF2)
    native_audio.close();
#endif
    SDL_DestroyTexture(screen_texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
