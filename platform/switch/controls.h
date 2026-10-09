#pragma once
#include <switch.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace switch_app {

enum Button : uint32_t {
    A = 1u << 0,
    B = 1u << 1,
    X = 1u << 2,
    Y = 1u << 3,
    Up = 1u << 4,
    Down = 1u << 5,
    Left = 1u << 6,
    Right = 1u << 7,
    L = 1u << 8,
    R = 1u << 9,
    ZL = 1u << 10,
    ZR = 1u << 11,
    Plus = 1u << 12,
    Minus = 1u << 13,
    StickL = 1u << 14,
    StickR = 1u << 15,
};

struct Pad {
    uint32_t buttons = 0;
    float lx = 0.0f, ly = 0.0f; // -1.0 to 1.0
    float rx = 0.0f, ry = 0.0f; // -1.0 to 1.0
};

struct Input {
    uint8_t steer = 0x80, accel = 0x20, brake = 0x20;
    uint8_t in0 = 0xff, in1 = 0x8f, in2 = 0xff;
};

inline bool menu_chord(uint32_t buttons) {
    return (buttons & (Plus | Minus)) == (Plus | Minus);
}

class Controls {
public:
#if defined(M2_ROMSET_VF2)
    // B: punch, A: kick, Y: guard, X: guard + kick; ZL/ZR/L/R: guard.
    // Returns the active-low mask to AND in (0x01 punch, 0x02 kick, 0x04 guard).
    static uint8_t action_bits(uint32_t buttons) {
        uint8_t bits = 0;
        if (buttons & B) bits |= 0x01;
        if (buttons & (A | X)) bits |= 0x02;
        if (buttons & (Y | X | ZL | ZR | L | R)) bits |= 0x04;
        return uint8_t(~bits);
    }

    Input sample(Pad pad1, Pad pad2 = {}) {
        Input in;
        in.in0 = 0xff;
        in.in1 = 0xff;
        in.in2 = 0xff;
        held_ = pad1.buttons;

        if (menu_chord(pad1.buttons) || menu_chord(pad2.buttons)) {
            return in;
        }

        // --- Player 1 ---
        // Arcade system buttons (in0 is active low)
        if (pad1.buttons & Minus) in.in0 &= uint8_t(~0x01); // Coin 1
        if (pad1.buttons & Plus)  in.in0 &= uint8_t(~0x10); // Start 1
        if (pad1.buttons & StickL) in.in0 &= uint8_t(~0x04); // Test Switch
        if (pad1.buttons & StickR) in.in0 &= uint8_t(~0x08); // Service Switch

        // Player 1 Action Buttons (in1 is active low): punch, kick, guard
        in.in1 &= action_bits(pad1.buttons);

        // Player 1 Movement: D-Pad and Left Analog Stick. With ZL+ZR held
        // the D-pad's Up belongs to the cheat chord (ZL+ZR+Up), not the game.
        constexpr float kDeadzone = 0.35f;
        const bool cheat_modifier = (pad1.buttons & (ZL | ZR)) == (ZL | ZR);
        if ((pad1.buttons & Down)  || pad1.ly < -kDeadzone) in.in1 &= uint8_t(~0x10);
        if (((pad1.buttons & Up) && !cheat_modifier) || pad1.ly > kDeadzone) in.in1 &= uint8_t(~0x20);
        if ((pad1.buttons & Right) || pad1.lx > kDeadzone)  in.in1 &= uint8_t(~0x40);
        if ((pad1.buttons & Left)  || pad1.lx < -kDeadzone) in.in1 &= uint8_t(~0x80);

        // --- Player 2 ---
        if (pad2.buttons & Minus) in.in0 &= uint8_t(~0x02); // Coin 2
        if (pad2.buttons & Plus)  in.in0 &= uint8_t(~0x20); // Start 2

        in.in2 &= action_bits(pad2.buttons);

        if ((pad2.buttons & Down)  || pad2.ly < -kDeadzone) in.in2 &= uint8_t(~0x10);
        if ((pad2.buttons & Up)    || pad2.ly > kDeadzone)  in.in2 &= uint8_t(~0x20);
        if ((pad2.buttons & Right) || pad2.lx > kDeadzone)  in.in2 &= uint8_t(~0x40);
        if ((pad2.buttons & Left)  || pad2.lx < -kDeadzone) in.in2 &= uint8_t(~0x80);

        return in;
    }
#else
    Input sample(Pad pad) {
        Input in;
        const uint32_t pressed = pad.buttons & ~held_;
        held_ = pad.buttons;

        if (menu_chord(pad.buttons)) {
            set_gear(in);
            return in;
        }

        // Steering: Left stick X
        float steer = pad.lx;
        if (std::abs(steer) < 0.12f) steer = 0.0f;
        else steer = std::copysign((std::abs(steer) - 0.12f) / 0.88f, steer);

        if (pad.buttons & Left) steer = -1.0f;
        if (pad.buttons & Right) steer = 1.0f;

        // Acceleration and Brake
        // Digital triggers ZL/ZR or Right Stick Y
        float accel = 0.0f, brake = 0.0f;
        if (pad.buttons & ZR) accel = 1.0f;
        if (pad.buttons & ZL) brake = 1.0f;

        // Right stick analog pedal support (up = gas, down = brake)
        if (pad.ry > 0.15f) accel = std::max(accel, pad.ry);
        if (pad.ry < -0.15f) brake = std::max(brake, -pad.ry);

        in.steer = uint8_t(std::clamp<int>(std::lround(128.f + 96.f * steer), 0, 255));
        in.accel = uint8_t(std::clamp<int>(std::lround(32.f + 192.f * accel), 0, 255));
        in.brake = uint8_t(std::clamp<int>(std::lround(32.f + 192.f * brake), 0, 255));

        // 4-speed manual gearbox:
        // L / R shoulders: sequential shift down / up
        const int shift = int(bool(pressed & R)) - int(bool(pressed & L));
        gear_ = std::clamp(gear_ + shift, 1, 4);

        // Direct selection with D-pad when not steering
        if (pressed & Up) gear_ = 4;
        if (pressed & Down) gear_ = 1;

        set_gear(in);

        // Arcade controls:
        // Coin: Minus
        if (pad.buttons & Minus) in.in0 &= uint8_t(~0x01);
        // Test Switch: StickL (Left Stick Click / L3)
        if (pad.buttons & StickL) in.in0 &= uint8_t(~0x04);
        // Service Switch: StickR (Right Stick Click / R3)
        if (pad.buttons & StickR) in.in0 &= uint8_t(~0x08);
        // Start: Plus
        if (pad.buttons & Plus) in.in0 &= uint8_t(~0x10);

        // VR camera view buttons:
        // VR1: B (bumper view)
        if (pad.buttons & B) in.in0 &= uint8_t(~0x20);
        // VR2: A (chase view)
        if (pad.buttons & A) in.in0 &= uint8_t(~0x40);
        // VR3: Y (far chase view)
        if (pad.buttons & Y) in.in0 &= uint8_t(~0x80);
        // VR4: X (cockpit view)
        if (pad.buttons & X) in.in1 &= uint8_t(~0x01);

        return in;
    }
#endif

    void latch(uint32_t buttons) { held_ = buttons; }
    int gear() const { return gear_; }

private:
    void set_gear(Input &in) const {
        constexpr uint8_t codes[] = {0, 2, 1, 6, 5};
        in.in1 = uint8_t((in.in1 & ~0x70) | (codes[gear_] << 4));
    }
    uint32_t held_ = 0;
    int gear_ = 1;
};

class FrameClock {
public:
    explicit FrameClock(double hz, int max_steps = 4)
        : step_(1.0 / hz), max_steps_(std::clamp(max_steps, 1, 4)) {}
    int advance(double seconds) {
        if (!std::isfinite(seconds) || seconds < 0) { reset(); return 0; }
        pending_ = std::min(pending_ + seconds, step_ * 4);
        int count = 0;
        while (pending_ >= step_ && count < max_steps_) { pending_ -= step_; ++count; }
        if (pending_ >= step_) pending_ = std::fmod(pending_, step_);
        return count;
    }
    void reset() { pending_ = 0; }
private:
    double step_, pending_ = 0;
    int max_steps_;
};

} // namespace switch_app
