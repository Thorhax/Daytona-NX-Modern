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
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#if defined(__SWITCH__)
extern "C" void userAppInit(void) {
    romfsInit();
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/daytona", 0777);
    chdir("sdmc:/switch/daytona");
}

extern "C" void userAppExit(void) {
    romfsExit();
}
#endif

namespace {

constexpr int kDisplayWidth = 1280;
constexpr int kDisplayHeight = 720;
constexpr const char *kDirectory = "sdmc:/switch/daytona";

PadState g_pad;

void init_pad() {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
}

switch_app::Pad read_pad() {
    padUpdate(&g_pad);
    const u64 kDown = padGetButtons(&g_pad);
    const HidAnalogStickState left = padGetStickPos(&g_pad, 0);
    const HidAnalogStickState right = padGetStickPos(&g_pad, 1);

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

} // namespace

int main(int, char **) {
    init_pad();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "Daytona USA",
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

    // Open telemetry performance log file
    FILE *perf_log_file = std::fopen("daytona-perf.log", "a");
    if (perf_log_file) {
        std::fprintf(perf_log_file, "\n=== DAYTONA USA RECOMP SWITCH LOG START ===\n");
        std::fflush(perf_log_file);
    }

    switch_app::Audio audio;
    switch_app::NativeAudio<snd::NativeSoundEngine> native_audio;
    const double perf_freq = double(SDL_GetPerformanceFrequency());
    switch_app::Performance perf(perf_freq);

    std::unique_ptr<rt::GameLoop> game;
    switch_app::Controls controls;
    // Allow up to 2 steps catchup if vsync is engaged
    switch_app::FrameClock clock(rt::GameLoop::kFrameHz, 2);

    constexpr uint64_t kArcadeFrameNs = 17'384'000ULL; // 57.524 Hz arcade frame period
    bool running = true;
    bool menu = true;
    bool have_frame = false;
    bool wait_release = true;
    int selection = 0;
    bool test_switch_held = false;
    bool use_native_audio = true; // Native audio skips 68000 CPU emulation for huge speedup!
    bool show_perf_overlay = false;
    bool vsync_mode = false; // 57.52 Hz native frame pacing by default
    uint8_t pulse = 0;
    uint32_t previous_buttons = 0;
    bool muted = false;

    std::string rom_path = find_rom_file();
    std::string status = "Place daytona.zip in sdmc:/switch/daytona/ then choose START GAME.";

    auto save = [&]() {
        if (!game) return true;
        save_bytes("ioboard_eeprom.bin", game->board().io().eeprom.data(), game->board().io().eeprom.size());
        save_bytes("backup_ram.bin", game->board().backup_ram().data(), game->board().backup_ram().size());
        return true;
    };

    auto load_nvram = [&]() {
        if (!game) return;
        bool loaded = load_bytes("ioboard_eeprom.bin", game->board().io().eeprom.data(), game->board().io().eeprom.size());
        if (!loaded || game->board().io().eeprom[0] != 'S' || game->board().io().eeprom[0x1a] != 0x00) {
            std::copy(std::begin(kDefaultEeprom), std::end(kDefaultEeprom), game->board().io().eeprom.begin());
            save_bytes("ioboard_eeprom.bin", game->board().io().eeprom.data(), game->board().io().eeprom.size());
        }
        load_bytes("backup_ram.bin", game->board().backup_ram().data(), game->board().backup_ram().size());
    };

    auto draw_menu = [&]() {
        SDL_SetRenderDrawColor(renderer, 18, 20, 28, 255);
        SDL_RenderClear(renderer);

        SDL_SetRenderDrawColor(renderer, 240, 240, 245, 255);
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
            "EXIT TO HOMEBREW MENU"
        };
        constexpr int kMenuItems = 10;

        for (int i = 0; i < kMenuItems; ++i) {
            if (i == selection) {
                SDL_SetRenderDrawColor(renderer, 255, 205, 50, 255);
            } else {
                SDL_SetRenderDrawColor(renderer, 210, 210, 220, 255);
            }
            switch_app::text(renderer, (i == selection ? "> " : "  ") + labels[i], 60, 75 + i * 35, 2, 60, 1);
        }

        SDL_SetRenderDrawColor(renderer, 180, 180, 190, 255);
        switch_app::text(renderer, status, 40, 435, 2, 70, 2);

        SDL_SetRenderDrawColor(renderer, 100, 180, 255, 255);
        switch_app::text(renderer, "CONTROLS: ZR=GAS  ZL=BRAKE  L/R=SHIFT  L3=TEST  R3=SERVICE", 40, 490, 2, 70, 1);
        switch_app::text(renderer, "TEST MODE: X=UP  B=DOWN  Y=OPT+  A=OPT-  PLUS=SELECT", 40, 520, 2, 70, 1);
        switch_app::text(renderer, "VR VIEWS: B=BUMPER  A=CHASE  Y=FAR  X=COCKPIT", 40, 550, 2, 70, 1);
        switch_app::text(renderer, "PLUS=START   MINUS=COIN   PLUS+MINUS=MENU", 40, 580, 2, 70, 1);

        SDL_SetRenderDrawColor(renderer, 255, 205, 50, 255);
        switch_app::text(renderer, "A: SELECT   B: RESUME GAME   PLUS+MINUS: MENU", 40, 660, 2, 60, 1);
    };

    auto start_game = [&]() -> bool {
        save();
        audio.close();
        native_audio.close();
        game.reset();
        have_frame = false;
        status = "Loading ROM set: " + rom_path + "...";
        draw_menu();
        SDL_RenderPresent(renderer);

        try {
            auto images = rt::import_rom_set(rom_path);
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

            // Set up high-precision profile clocks for microsecond telemetry
            game->set_profile_clock([]() -> uint64_t { return SDL_GetPerformanceCounter(); });
            game->board().video().set_profile_clock([]() -> uint64_t { return SDL_GetPerformanceCounter(); });

            load_nvram();
            controls = switch_app::Controls{};
            pulse = 0;
            status = "ZR: GAS   ZL: BRAKE   L/R: SHIFT   L-STICK: STEER   PLUS: START   MINUS: COIN";
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

        const auto pad = read_pad();
        const uint32_t pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;

        if (wait_release) {
            controls.latch(pad.buttons);
            if (!pad.buttons) wait_release = false;
        }

        if (!menu && !wait_release && switch_app::menu_chord(pad.buttons)) {
            menu = true;
            if (use_native_audio) native_audio.pause();
            else audio.pause();
            save();
            clock.reset();
        }

        if (menu && !wait_release) {
            constexpr int kMenuItems = 10;
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
                    save();
                    running = false;
                    break;
                }
            }
        }

        if (menu) {
            draw_menu();
            SDL_RenderPresent(renderer);
            svcSleepThread(16'666'666ULL);
            continue;
        }

        const uint64_t frame_start_tick = armGetSystemTick();
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
                const auto input = controls.sample(wait_release ? switch_app::Pad{} : pad);
                rt::Inputs mapped;
                mapped.steer = input.steer;
                mapped.accel = input.accel;
                mapped.brake = input.brake;
                mapped.in0 = uint8_t(input.in0 & ~(test_switch_held ? 0x04 : 0x00) & ~pulse);
                mapped.in1 = input.in1;
                mapped.in2 = input.in2;

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
                have_frame = true;

                const uint64_t before_upload = SDL_GetPerformanceCounter();
                SDL_UpdateTexture(
                    screen_texture,
                    nullptr,
                    game->screen().data(),
                    rt::GameLoop::kWidth * int(sizeof(uint32_t))
                );
                const uint64_t after_upload = SDL_GetPerformanceCounter();
                perf.span(switch_app::Performance::Upload, before_upload, after_upload);
            } catch (const std::exception &err) {
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
            const int width = kDisplayHeight * rt::GameLoop::kWidth / rt::GameLoop::kHeight; // 930
            const SDL_Rect dst{(kDisplayWidth - width) / 2, 0, width, kDisplayHeight};
            SDL_RenderCopy(renderer, screen_texture, nullptr, &dst);
            const uint64_t after_draw = SDL_GetPerformanceCounter();
            perf.span(switch_app::Performance::Draw, before_draw, after_draw);

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
                std::fflush(perf_log_file);
            }
            perf.reset(now_ticks, menu);
        }

        // Frame pacing for 57.524 Hz arcade rate
        if (!vsync_mode) {
            const uint64_t frame_end_tick = armGetSystemTick();
            const uint64_t frame_elapsed_ns = armTicksToNs(frame_end_tick - frame_start_tick);
            if (frame_elapsed_ns < kArcadeFrameNs) {
                const uint64_t sleep_ns = kArcadeFrameNs - frame_elapsed_ns;
                if (sleep_ns > 1'000'000ULL) {
                    svcSleepThread(sleep_ns - 800'000ULL);
                }
                while (armTicksToNs(armGetSystemTick() - frame_start_tick) < kArcadeFrameNs) {
                    // Precision spin-wait
                }
            }
        }
    }

    save();
    if (perf_log_file) {
        std::fprintf(perf_log_file, "=== DAYTONA USA RECOMP SWITCH LOG END ===\n");
        std::fclose(perf_log_file);
    }
    audio.close();
    native_audio.close();
    SDL_DestroyTexture(screen_texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
