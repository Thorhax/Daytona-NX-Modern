// The Model 2 board as the recompiled game runs on it outside lockstep: the
// whole i960 memory map with every device answered natively (no trace, no
// MAME, no emulated CPUs). Frame pacing is the only clock: the game's native
// code runs until it waits for vblank (its idle loop), then the board starts
// vblank: the geometrizer parses the display list, the vblank interrupt is
// raised, and at vblank end the screen is composed.
//
// Devices, each native: RAM/ROM and texture RAM (as the replay bus), the TGP
// and geometry ports (TgpBoard, recompiled TGP), the geometrizer (Geo), the
// screen (Video: tilemaps, palette, 3D layer), the interrupt controller and
// timers, video/render mode registers, the I/O board (IoBoard: its dual-port
// RAM mailbox protocol, inputs, settings EEPROM), the sound UART (bytes to
// the sound runtime), the comm board's shared RAM and backup RAM.
#pragma once

#include "runtime/cpu.h"
#include "runtime/geo.h"
#include "runtime/lockstep.h"
#include "runtime/m2_tgp_board.h"
#include "runtime/video.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "runtime/comm_board.h"

namespace rt {

struct Inputs {
    uint8_t steer = 0x80, accel = 0x20, brake = 0x20; // ADC values, 0x20-0xe0
    uint8_t in0 = 0xff, in1 = 0x8f, in2 = 0xff;       // switches, active low (MAME IN0/IN1/IN2 layout)
    uint8_t sw = 0xff;                                // DIP switches (Model 2A)
};

// 93C46 16-bit serial EEPROM (64 words x 16-bit = 128 bytes)
class Eeprom93C46 {
public:
    explicit Eeprom93C46(std::array<uint8_t, 128> &storage, bool &dirty)
        : storage_(storage), dirty_(dirty) {}

    int do_read() const { return do_bit_; }

    void step(bool cs, bool clk, bool di) {
        if (!cs) {
            state_ = State::WaitStart;
            do_bit_ = 1; // ready
            prev_clk_ = clk;
            return;
        }

        const bool rising = clk && !prev_clk_;
        prev_clk_ = clk;
        if (!rising) return;

        switch (state_) {
        case State::WaitStart:
            if (di) {
                accum_ = 0;
                bits_ = 0;
                state_ = State::WaitCommand;
            }
            break;

        case State::WaitCommand:
            accum_ = (accum_ << 1) | (di ? 1 : 0);
            if (++bits_ == 8) { // 2 opcode bits + 6 address bits
                const uint32_t op = (accum_ >> 6) & 3;
                const uint32_t addr = accum_ & 0x3f;
                if (op == 2) { // READ
                    addr_ = addr;
                    shift_ = word(addr_);
                    state_ = State::Reading;
                    read_bit_ = 0;
                    do_bit_ = 0; // dummy bit 0
                } else if (op == 1) { // WRITE
                    addr_ = addr;
                    shift_ = 0;
                    bits_ = 0;
                    state_ = State::Writing;
                } else if (op == 3) { // ERASE
                    if (unlocked_) {
                        set_word(addr, 0xffff);
                    }
                    state_ = State::WaitStart;
                    do_bit_ = 1;
                } else { // Ext opcodes
                    const uint32_t ext = (addr >> 4) & 3;
                    if (ext == 3) unlocked_ = true;       // EWEN
                    else if (ext == 0) unlocked_ = false;  // EWDS
                    else if (ext == 2 && unlocked_) {     // ERAL
                        for (size_t i = 0; i < 64; ++i) set_word(uint32_t(i), 0xffff);
                    }
                    state_ = State::WaitStart;
                    do_bit_ = 1;
                }
            }
            break;

        case State::Reading:
            if (read_bit_ < 16) {
                do_bit_ = (shift_ >> (15 - read_bit_)) & 1;
                read_bit_++;
            } else {
                addr_ = (addr_ + 1) & 0x3f;
                shift_ = word(addr_);
                read_bit_ = 0;
                do_bit_ = (shift_ >> 15) & 1;
                read_bit_++;
            }
            break;

        case State::Writing:
            shift_ = (shift_ << 1) | (di ? 1 : 0);
            if (++bits_ == 16) {
                if (unlocked_) {
                    set_word(addr_, uint16_t(shift_));
                }
                state_ = State::WaitStart;
                do_bit_ = 1; // ready
            }
            break;
        }
    }

private:
    uint16_t word(uint32_t a) const {
        return uint16_t(storage_[a * 2]) | (uint16_t(storage_[a * 2 + 1]) << 8);
    }
    void set_word(uint32_t a, uint16_t v) {
        storage_[a * 2] = uint8_t(v);
        storage_[a * 2 + 1] = uint8_t(v >> 8);
        dirty_ = true;
    }

    std::array<uint8_t, 128> &storage_;
    bool &dirty_;
    enum class State { WaitStart, WaitCommand, Reading, Writing };
    State state_ = State::WaitStart;
    bool prev_clk_ = false;
    bool unlocked_ = false;
    int do_bit_ = 1;
    uint32_t accum_ = 0;
    int bits_ = 0;
    uint32_t addr_ = 0;
    uint32_t shift_ = 0;
    int read_bit_ = 0;
};

// Sega 315-5649 I/O Controller (Model 2A/2B/2C)
class Sega315_5649 {
public:
    Inputs &inputs;
    Eeprom93C46 eeprom;
    std::array<uint8_t, 64> regs{};
    uint8_t ctrlmode = 0;
    uint8_t mode = 0;
    uint8_t analog_channel = 0;

    Sega315_5649(Inputs &in, std::array<uint8_t, 128> &eeprom_data, bool &eeprom_dirty)
        : inputs(in), eeprom(eeprom_data, eeprom_dirty) {
        regs.fill(0xff);
    }

    uint8_t read(uint32_t offset) {
        offset &= 0x3f;
        switch (offset) {
        case 0x00: // Port A
            return regs[0];
        case 0x01: // Port B (IN0: coins, start, test + EEPROM DO)
            if (ctrlmode) {
                return uint8_t(0xc0 | (eeprom.do_read() << 5) | 0x10 | (inputs.in0 & 0x0f));
            }
            return inputs.in0;
        case 0x02: // Port C (IN1: Player 1 inputs)
            return inputs.in1;
        case 0x03: // Port D (IN2: Player 2 inputs)
            return inputs.in2;
        case 0x04: // Port E (billboard / drive out)
            return regs[4];
        case 0x05: // Port F (lamps out)
            return regs[5];
        case 0x06: // Port G (SW / DIP switches)
            return inputs.sw;
        case 0x0b: // RS-422 ch1 rd
        case 0x0c: // RS-422 ch2 rd
            return 0;
        case 0x0d: // RS-422 status (MAME: 0x0c)
            return 0x0c;
        case 0x0f: { // Analog read (auto-increment)
            uint8_t val = 0x80;
            switch (analog_channel) {
            case 0: val = inputs.steer; break;
            case 1: val = inputs.accel; break;
            case 2: val = inputs.brake; break;
            default: val = 0xff; break;
            }
            analog_channel = (analog_channel + 1) & 7;
            return val;
        }
        default:
            return regs[offset];
        }
    }

    void write(uint32_t offset, uint8_t data) {
        offset &= 0x3f;
        regs[offset] = data;
        switch (offset) {
        case 0x00: { // Port A: EEPROM write
            ctrlmode = data & 1;
            const bool di = (data >> 5) & 1;
            const bool cs = (data >> 6) & 1;
            const bool clk = (data >> 7) & 1;
            eeprom.step(cs, clk, di);
            break;
        }
        case 0x0e: // Mode register
            mode = data;
            break;
        case 0x0f: // Analog channel select
            analog_channel = data & 7;
            break;
        default:
            break;
        }
    }
};

// The I/O board's side of the MB8421 dual-port RAM (2 KB): the game writes a
// command to byte 0x20 and the board answers. 1: latch the inputs into bytes
// 0-10. 3: copy the 128-byte settings EEPROM to bytes 0x100-0x17f. 2: store
// bytes 0x100-0x17f to the EEPROM. The byte returns to 0 when done. Byte 0x11
// is the force feedback drive board's command (rt::DriveBoard): each byte
// written is queued for the host (found by logging the game's writes: it
// carries the drive board's command set).
class IoBoard {
public:
    uint8_t read(uint32_t index) const { return ram_[index & 0x7ff]; }
    void write(uint32_t index, uint8_t v);
    Inputs inputs;
    std::array<uint8_t, 128> eeprom;
    bool eeprom_dirty = false;
    std::vector<uint8_t> drive_commands; // written to byte 0x11 since last taken (at most kMaxDrive kept)
    static constexpr size_t kMaxDrive = 256;
    IoBoard() { ram_.fill(0); eeprom.fill(0xff); }

private:
    std::array<uint8_t, 0x800> ram_;
};

class M2Board : public Bus {
public:
    struct Images {
        std::vector<uint8_t> program, main_data, copro_tables, copro_data, polygons, textures;
        std::vector<uint8_t> sound_program, pcm1, pcm2; // sound board (68000 program, MultiPCM samples)
    };
    explicit M2Board(Images images);

    // The CPU and scheduler (free-run Lockstep) the board raises interrupts on.
    void attach(Cpu &cpu, Lockstep &ls);

    // Bus
    uint32_t fetch(uint32_t addr) override;
    uint8_t read_byte(uint32_t addr) override;
    uint16_t read_word(uint32_t addr) override;
    uint32_t read_dword(uint32_t addr) override;
    void write_byte(uint32_t addr, uint8_t data) override;
    void write_word(uint32_t addr, uint16_t data) override;
    void write_dword(uint32_t addr, uint32_t data) override;
    uint16_t flags(uint32_t addr) override { return page(addr).burst ? Cpu::BURST : 0; }

    // Frame control, called by the runner.
    void vblank_start();       // MAME screen_vblank: geometrizer, vblank IRQ
    void vblank_end();         // screen_update
    bool in_idle_loop() const; // the game is waiting for vblank
    // i960 instructions in a CPU-bound frame (25 MHz / 57.52 Hz at MAME's
    // cycle counts). Daytona's heavy frames (FP, TGP waits) average 110,000;
    // VF2's are its loaders (Huffman-style decompression at 0x4c6e0-0x4c930:
    // ALU 1 cycle, loads 4, stores 2, about 1.5 cycles each), 290,000 in
    // MAME. At 110,000 the stage and title loads took 2.6 times as long.
#if defined(M2_ROMSET_VF2)
    static constexpr uint64_t kFrameInstructions = 290000;
#else
    static constexpr uint64_t kFrameInstructions = 110000;
#endif
    // Board time in i960 instructions: executed ones plus those skipped while
    // the game idles. next_timer_due: the earliest running timer whose
    // interrupt is enabled (UINT64_MAX: none).
    uint64_t vtime() const;
    uint64_t next_timer_due() const;
    void skip_time(uint64_t instructions);
    uint64_t frame() const { return frame_; }

    Video &video() { return *video_; }
    // Widescreen (enhancement): pixels added to each side; 0 = the original screen.
    void set_wide_margin(int pixels);
    // Draw mode (enhancement): draw the screen every (1 + skip)th frame; the
    // frames between keep the last picture. 0 = every frame, as the game does
    // (double buffered, a new 3D picture each frame); 1 = every 2nd; 2 = every
    // 3rd. The game logic and the geometrizer still run every frame.
    void set_frame_skip(int skip) { frame_skip_ = skip < 0 ? 0 : skip > 2 ? 2 : skip; }
    IoBoard &io() { return io_; }
    TgpBoard &tgp() { return tgp_; }
    // Bytes sent to the sound board since the last take.
    std::vector<uint8_t> take_sound_bytes() { sound_total_ += uart_out_.size(); return std::exchange(uart_out_, {}); }
    uint64_t sound_bytes_total() const { return sound_total_ + uart_out_.size(); }
    std::vector<uint8_t> &backup_ram() { return backup_; }
    std::vector<uint8_t> &main_ram() { return ram_; }  // 0x00200000
    std::vector<uint8_t> &work_ram() { return work_; } // 0x00500000
    // Link play: the communication board on a host transport (CommBoard;
    // nullptr: no link, the registers stay plain). Set before the game runs.
    void set_link(LinkTransport *transport, bool framesync = false);
    const CommBoard *comm_board() const { return comm_board_.get(); }

private:
    enum Kind : uint8_t { Unmapped, Rom, Ram, Tex, Dev };
    struct Page {
        Kind kind = Unmapped;
        bool burst = false;
        uint8_t *base = nullptr;
    };
    static constexpr unsigned kPageBits = 12;
    Page &page(uint32_t addr) { return pages_[addr >> kPageBits]; }
    void map(uint32_t start, uint32_t end, Kind k, uint8_t *base, uint32_t mirror = 0, bool burst = true);

    // Device dword access: data in its byte lanes, mask = lanes accessed.
    uint32_t dev_read(uint32_t addr, uint32_t mask);
    void dev_write(uint32_t addr, uint32_t data, uint32_t mask);
    void ram_written(uint32_t addr, uint32_t data, uint32_t mask); // video registers kept in RAM pages
    void tex_write(const Page &p, uint32_t addr, uint32_t lane_data);

    void irq_update();
    void uart_write_data(uint8_t v);
    void uart_txrdy(bool state);

    Images img_;
    std::vector<uint8_t> ram_, work_, cpuctl_, backup_, tile_, chr_, palette_, xlat_, tex0_, tex1_, luma_, fb_a_, fb_b_,
        comm_;
    std::vector<Page> pages_;

    TgpBoard tgp_;
    std::unique_ptr<Geo> geo_;
    std::unique_ptr<Video> video_;
    IoBoard io_;
    bool is_model2a_ = false;
    Sega315_5649 io315_;
    std::unique_ptr<CommBoard> comm_board_; // link play only
    Cpu *cpu_ = nullptr;
    int frame_skip_ = 0;
    uint64_t tex_generation_ = 0; // texture RAM writes so far
    Lockstep *ls_ = nullptr;

    uint32_t intreq_ = 0, intena_ = 0;
    int8_t lines_[4] = {-1, -1, -1, -1};
    uint32_t timervals_[4] = {0, 0, 0, 0};
    uint32_t timer_orig_[4] = {0, 0, 0, 0};
    uint64_t timer_start_[4] = {0, 0, 0, 0}; // vtime at the write
    uint64_t timer_due_[4] = {0, 0, 0, 0};   // vtime at zero
    uint64_t skew_ = 0;                      // instructions skipped by skip_time
    void timer_arm(uint32_t n);
    uint64_t timer_gen_[4] = {0, 0, 0, 0};
    bool timer_run_[4] = {false, false, false, false};
    static constexpr uint64_t kTimerInstrPerSec = kFrameInstructions * 5752 / 100;
    static uint64_t timer_ticks_to_instr(uint64_t ticks);
    uint32_t timer_read(uint32_t n);
    void timer_write(uint32_t n, uint32_t data, uint32_t mask);
    uint32_t videocontrol_ = 0, zclip_ = 0;
    bool render_mode_ = false, render_test_ = false, render_unk_ = false;
    uint8_t comm_cn_ = 0, comm_fg_ = 0;
    uint64_t frame_ = 0;

    // i8251 UART to the sound board (transmit side)
    bool uart_txrdy_ = true, uart_shift_busy_ = false, uart_have_hold_ = false;
    uint8_t uart_hold_ = 0;
    std::vector<uint8_t> uart_out_;
    uint64_t sound_total_ = 0;
    void uart_shift_done();
};

} // namespace rt
