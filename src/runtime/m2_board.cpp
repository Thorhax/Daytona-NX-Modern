// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese
//
// The Model 2 board as native runtime (see m2_board.h). Register semantics
// follow MAME's src/mame/sega/model2.cpp at
// dddd73680656e355bb2b5beecab1167c9f07bf81 (BSD-3-Clause; notice above kept):
// irq_request_r/irq_ack_w/irq_enable_w/irq_update, timers_w, videoctl_r/w,
// render_mode_r/w, tgpid_r, the model2o memory map. The I/O board's mailbox
// protocol was read from the game's own traffic (see IoBoard). See
// THIRD_PARTY.md.

#include "runtime/m2_board.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rt {

// ---------------------------------------------------------------------------
// I/O board

void IoBoard::write(uint32_t index, uint8_t v) {
    index &= 0x7ff;
    ram_[index] = v;
    if (index == 0x11 && drive_commands.size() < kMaxDrive) drive_commands.push_back(v); // to the drive board
    if (index != 0x20) return;
    switch (v) {
    case 1: // latch inputs
        ram_[0] = inputs.steer;
        ram_[1] = inputs.accel;
        ram_[2] = inputs.brake;
        for (int i = 3; i < 8; i++) ram_[size_t(i)] = 0xff; // unused ADC channels
        ram_[8] = inputs.in0;
        ram_[9] = inputs.in1;
        ram_[10] = inputs.in2;
        ram_[0x21] = 0x40; // board status the game checks at boot
        ram_[0x20] = 0;
        break;
    case 2: // store settings
        std::memcpy(eeprom.data(), &ram_[0x100], eeprom.size());
        eeprom_dirty = true;
        ram_[0x20] = 0;
        break;
    case 3: // load settings
        std::memcpy(&ram_[0x100], eeprom.data(), eeprom.size());
        ram_[0x20] = 0;
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Board

M2Board::M2Board(Images images)
    : img_(std::move(images)), ram_(0x40000), work_(0x100000), cpuctl_(0x1000), backup_(0x4000, 0xff), tile_(0x10000),
      chr_(0x80000), palette_(0x4000), xlat_(0xc000), tex0_(0x200000), tex1_(0x200000), luma_(0x20000), fb_a_(0x80000),
      fb_b_(0x80000), comm_(0x4000), pages_(size_t(1) << (32 - kPageBits)), tgp_(img_.copro_tables, img_.copro_data),
      io315_(io_.inputs, io_.eeprom, io_.eeprom_dirty) {
    is_model2a_ = (img_.program.size() > 0x100000);
#if defined(M2_ROMSET_VF2)
    is_model2a_ = true;
#endif
    if (is_model2a_) std::fill(std::begin(timervals_), std::end(timervals_), 0xfffffu); // MAME machine_reset

    // memory map
    map(0x00000000, 0x001fffff, Rom, img_.program.data());
    if (is_model2a_) {
        // Model 2A-CRX: 0x00200000-0x0023ffff (256 KB) burst RAM
        map(0x00200000, 0x0023ffff, Ram, ram_.data());
    } else {
        // Model 2 original: 128 KB RAM at 0x200000, ROM mirrored at 0x220000
        map(0x00200000, 0x0021ffff, Ram, ram_.data());
        map(0x00220000, 0x0023ffff, Rom, img_.program.data() + 0x20000);
    }
    map(0x00500000, 0x005fffff, Ram, work_.data());
    map(0x00800000, 0x00807fff, Dev, nullptr, 0, false);
    map(0x00880000, 0x00887fff, Dev, nullptr, 0, false);
    map(0x00900000, 0x0091ffff, Dev, nullptr, 0x60000, true); // buffer RAM (TgpBoard)
    map(0x00980000, 0x00980fff, Dev, nullptr, 0, false);
    map(0x00e00000, 0x00e00fff, Ram, cpuctl_.data(), 0, false);
    map(0x00e80000, 0x00e80fff, Dev, nullptr, 0, false);
    map(0x00f00000, 0x00f00fff, Dev, nullptr, 0, false);
    map(0x01000000, 0x0100ffff, Ram, tile_.data(), 0x110000);
    map(0x01040000, 0x01040fff, Dev, nullptr, 0x100000, false);
    map(0x01060000, 0x01060fff, Dev, nullptr, 0x100000, false);
    map(0x01080000, 0x010fffff, Ram, chr_.data(), 0x100000);
    map(0x01800000, 0x01803fff, Ram, palette_.data());
    map(0x01810000, 0x0181bfff, Ram, xlat_.data());
    map(0x0181c000, 0x0181cfff, Dev, nullptr, 0, false);
    map(0x01a00000, 0x01a03fff, Ram, comm_.data(), 0x10000);
    map(0x01a04000, 0x01a04fff, Dev, nullptr, 0x10000, false);
    map(0x01c00000, 0x01c00fff, Dev, nullptr, 0, false);
    map(0x01c80000, 0x01c80fff, Dev, nullptr, 0, false);
    map(0x01d00000, 0x01d03fff, Ram, backup_.data());
    map(0x02000000, 0x03ffffff, Rom, img_.main_data.data());
    map(0x06000000, 0x06ffffff, Rom, img_.main_data.data() + 0x1000000);
    map(0x10000000, 0x105fffff, Dev, nullptr, 0, false);
    map(0x11600000, 0x1167ffff, Ram, fb_a_.data());
    map(0x11680000, 0x116fffff, Ram, fb_b_.data());
    map(0x12000000, 0x121fffff, Tex, tex0_.data(), 0x200000);
    map(0x12400000, 0x125fffff, Tex, tex1_.data(), 0x200000);
    map(0x12800000, 0x1281ffff, Ram, luma_.data());

    geo_ = std::make_unique<Geo>(img_.polygons, img_.textures, tgp_.buffer_data());
    video_ = std::make_unique<Video>(tile_.data(), chr_.data());
    video_->enable_write_tracking();
}

void M2Board::map(uint32_t start, uint32_t end, Kind k, uint8_t *base, uint32_t mirror, bool burst) {
    for (uint32_t m = 0;; m = (m - mirror) & mirror) {
        for (uint64_t a = start; a <= end; a += (1u << kPageBits)) {
            Page &p = pages_[uint32_t(a | m) >> kPageBits];
            p.kind = k;
            p.burst = burst;
            p.base = base ? base + (a - start) : nullptr;
        }
        if (((m - mirror) & mirror) == 0) break;
    }
}

void M2Board::attach(Cpu &cpu, Lockstep &ls) {
    cpu_ = &cpu;
    ls_ = &ls;
}

// --- interrupts (MAME irq_update) --------------------------------------------

void M2Board::irq_update() {
    const int8_t want[4] = {int8_t((intreq_ & 0x001) ? 1 : 0), int8_t((intreq_ & 0x002) ? 1 : 0),
                            int8_t((intreq_ & 0x3fc) ? 1 : 0), int8_t((intreq_ & 0xc00) ? 1 : 0)};
    for (int l = 0; l < 4; l++)
        if (want[l] != lines_[l]) {
            lines_[l] = want[l];
            cpu_->execute_set_input(l, want[l]);
            ls_->poke();
        }
}

void M2Board::vblank_start() {
    // 60 Hz mode or an even frame: the geometrizer starts a new frame
    if ((videocontrol_ & 1) == 0 || (frame_ & 1) == 0) {
        geo_->zclip_w(zclip_);
        geo_->parse(tgp_.geo_read_start());
        video_->frame_start();
    }
    if (intena_ & 1) {
        intreq_ |= 1;
        irq_update();
    }
    if (comm_board_) comm_board_->vblank(); // MAME check_vint_irq
}

void M2Board::set_link(LinkTransport *transport, bool framesync) {
    if (!transport) {
        comm_board_.reset();
        return;
    }
    comm_board_ = std::make_unique<CommBoard>(comm_.data());
    comm_board_->set_transport(transport);
    comm_board_->set_framesync(framesync);
}

void M2Board::set_wide_margin(int pixels) {
    geo_->set_wide_margin(pixels);
    video_->set_wide_margin(pixels);
}

void M2Board::vblank_end() {
    if (frame_skip_ && frame_ % uint64_t(frame_skip_ + 1) != 0) { // draw mode: keep the last picture
        ++frame_;
        return;
    }
    VideoMem m;
    m.palram = palette_.data();
    m.colorxlat = xlat_.data();
    m.lumaram = luma_.data();
    m.tex0 = reinterpret_cast<const uint32_t *>(tex0_.data());
    m.tex1 = reinterpret_cast<const uint32_t *>(tex1_.data());
    m.tex_generation = tex_generation_;
    video_->screen_update(geo_->polys, geo_->windows(), m);
    ++frame_;
}

bool M2Board::in_idle_loop() const {
    const uint32_t ip = cpu_->m_IP;
    if (is_model2a_) {
        return (ip >= 0x0f7c && ip <= 0x0f8c) || (ip >= 0x10f90 && ip <= 0x10fa4);
    }
    // The game's wait-for-vblank loops (0x12b0: until the frame counter at
    // 0x00500000 changes; 0x12f0: until it reaches 2). MAME takes 99% of
    // vblank interrupts here; the rest land in CPU-bound code such as the
    // boot-time texture upload at 0x1388.
    return (ip >= 0x12b0 && ip <= 0x12bb) || (ip >= 0x12f0 && ip <= 0x12ff);
}

// --- timers (MAME timers_r/timers_w/model2_timer_cb) --------------------------
// Four 20-bit timers count down at 25 MHz from the value written; at zero
// they stop, read 0xfffff, and raise request bit 2+n if it is enabled.
// Daytona reloads timer 0 each frame and never enables its interrupt, so on
// the original Model 2 the value is kept without a clock (as before). VF2
// (Model 2A) runs timer 3 with its interrupt enabled: without it the game's
// timer-driven work (the title's 3D logo, stage set-up) stalls for seconds.
// Time is i960 instructions (kFrameInstructions per 57.52 Hz frame) plus the time the
// game loop skips while the game idles (skip_time): a frame that ends early
// in the wait-for-vblank loop still lasts a whole frame for the timers.

uint64_t M2Board::vtime() const { return ls_->count + skew_; }

uint64_t M2Board::timer_ticks_to_instr(uint64_t ticks) {
    return (ticks * kTimerInstrPerSec + 12'500'000) / 25'000'000;
}

uint32_t M2Board::timer_read(uint32_t n) {
    if (is_model2a_ && timer_run_[n]) {
        const uint64_t elapsed = (vtime() - timer_start_[n]) * 25'000'000 / kTimerInstrPerSec;
        timervals_[n] = elapsed >= timer_orig_[n] ? 0 : uint32_t(timer_orig_[n] - elapsed);
    }
    return timervals_[n];
}

void M2Board::timer_write(uint32_t n, uint32_t data, uint32_t mask) {
    uint32_t &t = timervals_[n];
    t = (t & ~mask) | (data & mask);
    if (!is_model2a_) return;
    timer_orig_[n] = t;
    timer_start_[n] = vtime();
    timer_due_[n] = timer_start_[n] + std::max<uint64_t>(1, timer_ticks_to_instr(t));
    timer_run_[n] = true;
    timer_arm(n);
}

void M2Board::timer_arm(uint32_t n) {
    const uint64_t gen = ++timer_gen_[n];
    const uint64_t at = timer_due_[n] > vtime() ? timer_due_[n] - skew_ : ls_->count + 1;
    ls_->add_callback(at, [this, n, gen] {
        if (timer_gen_[n] != gen || !timer_run_[n]) return;
        const uint32_t line = 1u << (n + 2);
        if (intena_ & line) {
            intreq_ |= line;
            irq_update();
        }
        timervals_[n] = 0xfffff;
        timer_run_[n] = false;
    });
}

uint64_t M2Board::next_timer_due() const {
    uint64_t due = UINT64_MAX;
    for (uint32_t n = 0; n < 4; ++n)
        if (timer_run_[n] && (intena_ & (1u << (n + 2)))) due = std::min(due, timer_due_[n]);
    return due;
}

void M2Board::skip_time(uint64_t instructions) {
    if (!instructions) return;
    skew_ += instructions;
    for (uint32_t n = 0; n < 4; ++n)
        if (timer_run_[n]) timer_arm(n); // due earlier in instruction counts now
}

// --- sound UART (i8251, transmit side) ------------------------------------------
// The game sends bytes from its IRQ3 handler whenever TxRDY is set. With no
// clock, a byte moves to the shifter and out at once; the sound runtime
// receives it immediately.

void M2Board::uart_txrdy(bool state) {
    if (state == uart_txrdy_) return;
    uart_txrdy_ = state;
    // MAME sound_ready_w: TxRDY (or RxRDY) sets request bit 10 if enabled
    if (state && (intena_ & (1u << 10))) {
        intreq_ |= 1u << 10;
        irq_update();
    }
}

void M2Board::uart_write_data(uint8_t v) {
    uart_hold_ = v;
    uart_have_hold_ = true;
    uart_txrdy(false);
    if (!uart_shift_busy_) {
        uart_shift_busy_ = true;
        ls_->add_callback(ls_->count + 1, [this] { uart_shift_done(); });
    }
}

void M2Board::uart_shift_done() {
    uart_shift_busy_ = false;
    if (uart_have_hold_) {
        uart_out_.push_back(uart_hold_);
        uart_have_hold_ = false;
    }
    uart_txrdy(true);
}

// --- device registers --------------------------------------------------------

uint32_t M2Board::dev_read(uint32_t addr, uint32_t mask) {
    if (addr >= 0x00900000 && addr <= 0x0097ffff) {
        tgp_.sync(); // the TGP answers through its mailbox here (0x0091fff0-8)
        return tgp_.buffer_r(addr & 0x1ffff);
    }
    if (addr >= 0x00800000 && addr <= 0x00803fff) return tgp_.geo_r((addr - 0x00800000) >> 2);
    if (addr >= 0x00804000 && addr <= 0x00807fff) return 0xffffffffu; // geo_prg_r
    if (addr >= 0x00884000 && addr <= 0x00887fff) return tgp_.fifo_r();
    switch (addr) {
    case 0x00980000: return tgp_.coproctl_r();
    case 0x00980004: {
        const uint32_t v = tgp_.fifo_out_empty() ? 1 : 0;
        if (std::getenv("M2RUN_VERBOSE")) std::fprintf(stderr, "fifo status read -> %u (frame %llu)\n", v, (unsigned long long)frame_);
        return v;
    }
    case 0x0098000c: { // videoctl_r
        const uint32_t framenum = render_mode_ ? uint32_t((frame_ & 1) << 2) : uint32_t((frame_ & 2) << 1);
        return framenum | (videocontrol_ & 3);
    }
    case 0x00e80000: return intreq_;
    case 0x00e80004: return intena_;
    default: break;
    }
    if (addr >= 0x00980030 && addr <= 0x0098003f) { // tgpid_r, byte-wide
        static const uint8_t id[] = {0, 'T', 'A', 'H', 0, 'A', 'K', 'O', 0, 'Z', 'A', 'K', 0, 'M', 'T', 'K'};
        const uint32_t o = addr - 0x00980030;
        return uint32_t(id[o]) | uint32_t(id[o + 1]) << 8 | uint32_t(id[o + 2]) << 16 | uint32_t(id[o + 3]) << 24;
    }
    if (addr >= 0x00f00000 && addr <= 0x00f0000f) return timer_read((addr >> 2) & 3);
    if ((addr & ~0x10000u) == 0x01a04000) { // cn_r, fg_r
        if (comm_board_) // fg_r takes frames in: only when that lane is read
            return uint32_t(comm_board_->cn_r()) | ((mask & 0xff0000) ? uint32_t(comm_board_->fg_r()) << 16 : 0);
        return uint32_t(comm_cn_ | 0xfe) | uint32_t(comm_fg_) << 16;
    }
    if (is_model2a_) {
        if (addr >= 0x01c00000 && addr <= 0x01c0001f) {
            const uint32_t i = ((addr & 0x1f) >> 2) * 2;
            return uint32_t(io315_.read(i)) | (uint32_t(io315_.read(i + 1)) << 16);
        }
        if (addr >= 0x01c00020 && addr <= 0x01c00fff) return 0;
    } else {
        if (addr >= 0x01c00000 && addr <= 0x01c00fff) { // MB8421 dual-port RAM
            const uint32_t i = ((addr & 0xfff) >> 2) * 2;
            return uint32_t(io_.read(i)) | uint32_t(io_.read(i + 1)) << 16;
        }
    }
    if (addr >= 0x01c80000 && addr <= 0x01c80003) {
        // i8251 UART: lane 0 data (rx), lane 2 status (TxRDY, etc.)
        const uint32_t status = (uart_txrdy_ ? 0x05 : 0x00) | 0x80;
        return status << 16;
    }
    if (addr >= 0x10000000 && addr <= 0x101fffff) return uint32_t(render_unk_) << 14 | uint32_t(render_mode_) << 2 | uint32_t(render_test_);
    if (addr >= 0x10400000 && addr <= 0x105fffff) return uint32_t(geo_->polys.size()); // polygon_count_r
    (void)mask;
    return 0;
}

void M2Board::dev_write(uint32_t addr, uint32_t data, uint32_t mask) {
    if (addr >= 0x00900000 && addr <= 0x0097ffff) { tgp_.buffer_w(addr & 0x1ffff, data, mask); return; }
    if (addr == 0x00980000) { tgp_.coproctl_w(data, mask); return; }
    const uint32_t d = data & mask; // handlers without a mask see unwritten lanes as 0
    if (addr >= 0x00884000 && addr <= 0x00887fff) { tgp_.fifo_w(d); return; }
    if (addr >= 0x00880000 && addr <= 0x00883fff) { tgp_.function_port_w((addr - 0x00880000) >> 2, d); return; }
    if (addr >= 0x00804000 && addr <= 0x00807fff) { tgp_.geo_prg_w(d); return; }
    if (addr >= 0x00800000 && addr <= 0x00803fff) { tgp_.geo_w((addr - 0x00800000) >> 2, d); return; }
    switch (addr) {
    case 0x00980008: tgp_.geoctl_w(d); return;
    case 0x0098000c: videocontrol_ = (videocontrol_ & ~mask) | (data & mask); return;
    case 0x00e80000: intreq_ &= data | ~mask; irq_update(); return; // irq_ack_w
    case 0x00e80004: {                                                // irq_enable_w (MAME delays 80 ns)
        intena_ = (intena_ & ~mask) | (data & mask);
        if (uart_txrdy_ && (intena_ & (1u << 10))) intreq_ |= 1u << 10; // irq_mask_delayed_update
        irq_update();
        return;
    }
    case 0x0181c000: zclip_ = d; return;
    default: break;
    }
    if (addr >= 0x00f00000 && addr <= 0x00f0000f) {
        timer_write((addr >> 2) & 3, data, mask);
        return;
    }
    if ((addr & ~0x100000u) == 0x01040000) { if (mask & 0xffff) video_->xhout_w(uint16_t(data)); return; }
    if ((addr & ~0x100000u) == 0x01060000) { if (mask & 0xffff) video_->xvout_w(uint16_t(data)); return; }
    if ((addr & ~0x10000u) == 0x01a04000) {
        if (comm_board_) {
            if (mask & 0xff) comm_board_->cn_w(uint8_t(data));
            if (mask & 0xff0000) comm_board_->fg_w(uint8_t(data >> 16));
            return;
        }
        if (mask & 0xff) comm_cn_ = uint8_t(data & 1);
        if (mask & 0xff0000) comm_fg_ = uint8_t(data >> 16);
        return;
    }
    if (is_model2a_) {
        if (addr >= 0x01c00000 && addr <= 0x01c0001f) {
            const uint32_t i = ((addr & 0x1f) >> 2) * 2;
            if (mask & 0x000000ff) io315_.write(i, uint8_t(data));
            if (mask & 0x00ff0000) io315_.write(i + 1, uint8_t(data >> 16));
            return;
        }
        if (addr >= 0x01c00020 && addr <= 0x01c00fff) return;
    } else {
        if (addr >= 0x01c00000 && addr <= 0x01c00fff) {
            const uint32_t i = ((addr & 0xfff) >> 2) * 2;
            if (mask & 0x000000ff) io_.write(i, uint8_t(data));
            if (mask & 0x00ff0000) io_.write(i + 1, uint8_t(data >> 16));
            return;
        }
    }
    if (addr >= 0x01c80000 && addr <= 0x01c80003) { // i8251, umask16 0x00ff: lane 0 data, lane 2 control
        if (mask & 0x000000ff) uart_write_data(uint8_t(data));
        return; // mode/command bytes: transmitter enabled by the game at boot
    }
    if (addr >= 0x10000000 && addr <= 0x101fffff) { // render_mode_w
        render_test_ = data & 1;
        render_mode_ = (data >> 2) & 1;
        render_unk_ = (data >> 14) & 1;
        return;
    }
}

// Palette and colour translation live in RAM pages; the video output tracks
// what MAME's handlers do on each write.
void M2Board::ram_written(uint32_t addr, uint32_t data, uint32_t mask) {
    (void)data;
    if ((addr >= 0x01000000 && addr <= 0x0100ffff) ||
        (addr >= 0x01110000 && addr <= 0x0111ffff)) video_->tile_memory_w();
    else if ((addr >= 0x01080000 && addr <= 0x010fffff) ||
             (addr >= 0x01180000 && addr <= 0x011fffff)) video_->character_memory_w();
    if (addr >= 0x01800000 && addr <= 0x01803fff) {
        for (uint32_t lane = 0; lane < 2; lane++)
            if ((mask >> (16 * lane)) & 0xffff) video_->palette_w(((addr & 0x3fff) >> 1) + lane, palette_.data(), xlat_.data());
    } else if (addr >= 0x01810000 && addr <= 0x0181bfff) {
        for (uint32_t lane = 0; lane < 2; lane++)
            if ((mask >> (16 * lane)) & 0xffff) video_->colorxlat_w(((addr - 0x01810000) >> 1) + lane);
    }
}

void M2Board::tex_write(const Page &p, uint32_t addr, uint32_t lane_data) {
    uint8_t *const base = p.base - ((addr & 0x1fffff) & ~((1u << kPageBits) - 1));
    const uint32_t o = (addr & 0x1fffff) >> 2;
    uint8_t *const w = base + (o >> 1) * 4 + (o & 1) * 2;
    w[0] = uint8_t(lane_data);
    w[1] = uint8_t(lane_data >> 8);
    ++tex_generation_; // the hardware renderer re-uploads texture RAM when this moves
}

// --- bus ---------------------------------------------------------------------

uint32_t M2Board::fetch(uint32_t addr) {
    const Page &p = page(addr);
    if (p.kind != Rom && p.kind != Ram) throw Fatal("instruction fetch from a device");
    uint32_t v;
    std::memcpy(&v, p.base + (addr & 0xffc), 4);
    return v;
}

uint8_t M2Board::read_byte(uint32_t addr) {
    const Page &p = page(addr);
    const unsigned sh = (addr & 3) * 8;
    switch (p.kind) {
    case Rom: case Ram: case Tex: return p.base[addr & 0xfff];
    case Dev: return uint8_t(dev_read(addr & ~3u, 0xffu << sh) >> sh);
    default: return 0;
    }
}

uint16_t M2Board::read_word(uint32_t addr) {
    addr &= ~1u;
    const Page &p = page(addr);
    const unsigned sh = (addr & 2) * 8;
    switch (p.kind) {
    case Rom: case Ram: case Tex: {
        uint16_t v;
        std::memcpy(&v, p.base + (addr & 0xfff), 2);
        return v;
    }
    case Dev: return uint16_t(dev_read(addr & ~3u, 0xffffu << sh) >> sh);
    default: return 0;
    }
}

uint32_t M2Board::read_dword(uint32_t addr) {
    addr &= ~3u;
    const Page &p = page(addr);
    switch (p.kind) {
    case Rom: case Ram: case Tex: {
        uint32_t v;
        std::memcpy(&v, p.base + (addr & 0xfff), 4);
        return v;
    }
    case Dev: return dev_read(addr, 0xffffffffu);
    default: return 0;
    }
}

void M2Board::write_byte(uint32_t addr, uint8_t data) {
    const Page &p = page(addr);
    const unsigned sh = (addr & 3) * 8;
    switch (p.kind) {
    case Ram: {
        uint8_t &dst = p.base[addr & 0xfff];
        const bool changed = dst != data;
        dst = data;
        if (changed) ram_written(addr & ~3u, uint32_t(data) << sh, 0xffu << sh);
        return;
    }
    case Tex: tex_write(p, addr, uint32_t(data) << sh); return;
    case Dev: dev_write(addr & ~3u, uint32_t(data) << sh, 0xffu << sh); return;
    default: return;
    }
}

void M2Board::write_word(uint32_t addr, uint16_t data) {
    addr &= ~1u;
    const Page &p = page(addr);
    const unsigned sh = (addr & 2) * 8;
    switch (p.kind) {
    case Ram: {
        uint16_t old; std::memcpy(&old, p.base + (addr & 0xfff), 2);
        std::memcpy(p.base + (addr & 0xfff), &data, 2);
        if (old != data) ram_written(addr & ~3u, uint32_t(data) << sh, 0xffffu << sh);
        return;
    }
    case Tex: tex_write(p, addr, uint32_t(data) << sh); return;
    case Dev: dev_write(addr & ~3u, uint32_t(data) << sh, 0xffffu << sh); return;
    default: return;
    }
}

void M2Board::write_dword(uint32_t addr, uint32_t data) {
    addr &= ~3u;
    const Page &p = page(addr);
    switch (p.kind) {
    case Ram: {
        uint32_t old; std::memcpy(&old, p.base + (addr & 0xfff), 4);
        std::memcpy(p.base + (addr & 0xfff), &data, 4);
        if (old != data) ram_written(addr, data, 0xffffffffu);
        return;
    }
    case Tex: tex_write(p, addr, data); return;
    case Dev: dev_write(addr, data, 0xffffffffu); return;
    default: return;
    }
}

} // namespace rt
