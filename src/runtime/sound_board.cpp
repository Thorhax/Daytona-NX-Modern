#include "runtime/sound_board.h"

#include <algorithm>
#include <cstdio>

namespace snd {

SoundBoard::SoundBoard(const std::vector<uint8_t> &program, const std::vector<uint8_t> &pcm1, const std::vector<uint8_t> &pcm2)
    : program_(program), pcm1_rom_(pcm1), pcm2_rom_(pcm2),
      pcm1_(kPcmClock, pcm1_rom_.data(), uint32_t(pcm1_rom_.size())),
      pcm2_(kPcmClock, pcm2_rom_.data(), uint32_t(pcm2_rom_.size())) {
    if (program_.size() < 0x40000) throw rt::Fatal("sound program image missing or short (re-run the importer)");

    const uint32_t init_pc = program_.size() >= 8 ? (uint32_t(program_[4]) << 24 | uint32_t(program_[5]) << 16 | uint32_t(program_[6]) << 8 | program_[7]) : 0;
    if ((init_pc & 0xff0000) == 0x600000 || program_.size() >= 0x80000) {
        cpu_.is_scsp = true;
        cpu_.ram.resize(0x80000, 0);
        samples_rom_.resize(0x800000, 0);
        if (!pcm1_rom_.empty()) {
            std::copy_n(pcm1_rom_.data(), std::min<size_t>(pcm1_rom_.size(), 0x400000), samples_rom_.data());
        }
        if (!pcm2_rom_.empty()) {
            std::copy_n(pcm2_rom_.data(), std::min<size_t>(pcm2_rom_.size(), 0x400000), samples_rom_.data() + 0x400000);
        }
        cpu_.samples = samples_rom_.data();
        scsp_ = std::make_unique<Scsp>(cpu_.ram.data(), uint32_t(cpu_.ram.size()));
        scsp_->set_timer_cb([this](int tnum, uint32_t prescale, uint32_t count) {
            scsp_timer_arm(tnum, prescale, count);
        });
    }
    cpu_.rom = program_.data();
    cpu_.dev = this;
    cpu_.reset();
    if (!cpu_.is_scsp) ym_.reset();
}

void SoundBoard::send(const uint8_t *bytes, size_t n) {
    constexpr uint64_t kByteTime = kIps * kByteBits / kBaud; // instructions per byte on the line
    for (size_t i = 0; i < n; ++i) {
        const uint64_t at = std::max(line_free_, sched_.count) + kByteTime;
        line_free_ = at;
        const uint8_t b = bytes[i];
        sched_.at(at, [this, b] { rx_arrive(b); });
    }
}

void SoundBoard::rx_arrive(uint8_t byte) {
    if (cpu_.is_scsp) {
        if (scsp_) {
            scsp_->midi_in(byte);
            cpu_.irq_line = scsp_->irq_level();
        } else {
            cpu_.irq_line = 3;
        }
        sched_.poke();
        ++received_;
        return;
    }
    if (!rx_enable_) return; // receiver off: the byte is lost, as on the chip
    if (rxrdy_) errors_ |= 0x10; // overrun: the new byte replaces the unread one
    rx_hold_ = byte;
    rxrdy_ = true;
    cpu_.irq_line = 2; // RxRDY on IPL level 2
    sched_.poke();
    ++received_;
}

void SoundBoard::advance(double seconds) {
    const double want = seconds * double(kIps) + carry_;
    const uint64_t n = uint64_t(want);
    carry_ = want - double(n);
    run_to(sched_.count + n);
}

void SoundBoard::run_to(uint64_t count) {
    while (sched_.count < count) {
        sched_.end_count = count;
        if (!sndgen::has_code(cpu_.pc)) {
            char b[96];
            std::snprintf(b, sizeof b, "sound 68000: no recompiled code at %06x", cpu_.pc);
            throw rt::Fatal(b);
        }
        sndgen::run(env_);
    }
    render_fm_to(count);
    render_pcm_to(count);
}

void SoundBoard::render_fm_to(uint64_t count) {
    if (cpu_.is_scsp) return;
    const uint64_t due = count * kYmClock / (144 * kIps);
    constexpr int kChunk = 256;
    ymfm::ym3438::output_data out[kChunk];
    while (fm_done_ < due) {
        const int n = int(std::min<uint64_t>(kChunk, due - fm_done_));
        ym_.generate(out, uint32_t(n));
        for (int i = 0; i < n; ++i) {
            fm_out_.push_back(0.30f * float(out[i].data[0]) / 32768.0f);
            fm_out_.push_back(0.30f * float(out[i].data[1]) / 32768.0f);
        }
        fm_done_ += uint64_t(n);
    }
}

void SoundBoard::render_pcm_to(uint64_t count) {
    if (cpu_.is_scsp) {
        if (!scsp_) return;
        const uint64_t due = count * 44100 / kIps;
        constexpr int kChunk = 256;
        float buf[kChunk * 2];
        while (pcm_done_ < due) {
            const int n = int(std::min<uint64_t>(kChunk, due - pcm_done_));
            scsp_->generate(buf, n);
            pcm_out_.insert(pcm_out_.end(), buf, buf + n * 2);
            pcm_done_ += uint64_t(n);
        }
        return;
    }
    const uint64_t due = count * kPcmClock / (224 * kIps);
    constexpr int kChunk = 256;
    float l1[kChunk], r1[kChunk], l2[kChunk], r2[kChunk];
    while (pcm_done_ < due) {
        const int n = int(std::min<uint64_t>(kChunk, due - pcm_done_));
        pcm1_.generate(l1, r1, n);
        pcm2_.generate(l2, r2, n);
        for (int i = 0; i < n; ++i) {
            pcm_out_.push_back(0.5f * l1[i] + 0.5f * l2[i]);
            pcm_out_.push_back(0.5f * r1[i] + 0.5f * r2[i]);
        }
        pcm_done_ += uint64_t(n);
    }
}

uint8_t SoundBoard::read(uint32_t addr) {
    if (cpu_.is_scsp) {
        if (scsp_ && addr >= 0x100000 && addr <= 0x100fff) {
            render_pcm_to(sched_.count);
            uint8_t val = scsp_->read8(addr - 0x100000);
            cpu_.irq_line = scsp_->irq_level();
            return val;
        }
        return 0;
    }
    switch (addr) {
    case 0xc20001: // UART data
        rxrdy_ = false;
        cpu_.irq_line = 0;
        return rx_hold_;
    case 0xc20003: // UART status: DSR, TxEMPTY, TxRDY always; RxRDY; PE/OE/FE
        return uint8_t(0x85 | (rxrdy_ ? 2 : 0) | errors_);
    case 0xd00001: case 0xd00003: case 0xd00005: case 0xd00007:
        return ym_.read((addr - 0xd00001) / 2);
    default:
        return 0; // MultiPCM status reads 0 (as MAME)
    }
}

uint16_t SoundBoard::read16(uint32_t addr) {
    if (cpu_.is_scsp) {
        if (scsp_ && addr >= 0x100000 && addr <= 0x100fff) {
            render_pcm_to(sched_.count); // the 68000 polls slot positions (MSLC/CA): bring them up to now
            uint16_t val = scsp_->read16(addr - 0x100000);
            cpu_.irq_line = scsp_->irq_level();
            return val;
        }
        return 0;
    }
    return (uint16_t(read(addr)) << 8) | read(addr + 1);
}

void SoundBoard::write(uint32_t addr, uint16_t data, uint16_t mem_mask) {
    if (cpu_.is_scsp) {
        if (scsp_ && addr >= 0x100000 && addr <= 0x100fff) {
            render_pcm_to(sched_.count);
            scsp_->write16(addr - 0x100000, data, mem_mask);
            cpu_.irq_line = scsp_->irq_level();
        }
        return;
    }
    const uint8_t d = uint8_t(data);
    switch (addr) {
    case 0xc20001: break; // UART transmit: nothing listens on the i960 side
    case 0xc20003:        // UART mode / command
        if (expect_mode_) {
            expect_mode_ = false;
        } else {
            if (d & 0x10) errors_ = 0; // error reset
            rx_enable_ = d & 0x04;
            if (d & 0x40) expect_mode_ = true; // internal reset: next write is the mode
        }
        break;
    case 0xc40001: case 0xc40003: case 0xc40005: case 0xc40007:
        render_pcm_to(sched_.count);
        pcm1_.write((addr - 0xc40001) / 2, d);
        break;
    case 0xc60001: case 0xc60003: case 0xc60005: case 0xc60007:
        render_pcm_to(sched_.count);
        pcm2_.write((addr - 0xc60001) / 2, d);
        break;
    case 0xc50000: render_pcm_to(sched_.count); pcm1_.set_bank(data & 3); break;
    case 0xc70000: render_pcm_to(sched_.count); pcm2_.set_bank(data & 3); break;
    case 0xd00001: case 0xd00003: case 0xd00005: case 0xd00007:
        render_fm_to(sched_.count);
        ym_.write((addr - 0xd00001) / 2, d);
        break;
    default: break; // 0xc40012: effect DSP (not fitted)
    }
}

// ymfm asks for a timer in YM clocks. Expiries are kept in exact YM clocks
// (a periodic timer re-armed from its own expiry does not drift) and mapped
// to the instruction count at which the 68000 would see them.
void SoundBoard::ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) {
    const uint64_t gen = ++timer_gen_[tnum];
    if (duration_in_clocks < 0) return;
    const uint64_t base = in_timer_ == int(tnum) ? timer_due_clk_[tnum] : ym_clock_at(sched_.count);
    const uint64_t due = base + uint64_t(duration_in_clocks);
    timer_due_clk_[tnum] = due;
    const uint64_t at = (due * kIps + kYmClock - 1) / kYmClock;
    sched_.at(at, [this, tnum, gen] {
        if (timer_gen_[tnum] != gen) return; // re-armed or stopped since
        render_fm_to(sched_.count);
        in_timer_ = int(tnum);
        m_engine->engine_timer_expired(tnum);
        in_timer_ = -1;
    });
}

void SoundBoard::scsp_timer_arm(int tnum, uint32_t prescale, uint32_t count) {
    if (tnum < 0 || tnum >= 3) return;
    if (count == 255) return; // MAME: no new period; a running timer keeps going
    const uint64_t gen = ++scsp_timer_gen_[tnum];
    const uint32_t steps = 255 - count;
    const uint64_t samples = uint64_t(prescale) * steps;
    const uint64_t instr = (samples * kIps + 22050) / 44100;
    const uint64_t at = sched_.count + std::max<uint64_t>(1, instr);
    sched_.at(at, [this, tnum, gen] {
        if (scsp_timer_gen_[tnum] != gen) return;
        render_pcm_to(sched_.count);
        scsp_->timer_expire(tnum);
        cpu_.irq_line = scsp_->irq_level();
        sched_.poke();
    });
}

} // namespace snd
