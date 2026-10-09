// Yamaha YMF292-F (SCSP): 32-slot sample and FM synth processor.
// Adapted from MAME src/devices/sound/scsp.cpp and scspdsp.cpp
// (license: BSD-3-Clause, copyright-holders: ElSemi, R. Belmont; THIRD_PARTY.md).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace snd {

class Scsp {
public:
    static constexpr uint32_t kSampleRate = 44100;
    static constexpr uint32_t kClock = 22579200; // 45.1584 MHz / 2

    using TimerCb = std::function<void(int tnum, uint32_t prescale, uint32_t count)>;

    Scsp(uint8_t *sound_ram, uint32_t ram_size);
    ~Scsp() = default;

    void reset();

    uint8_t read8(uint32_t offset);
    uint16_t read16(uint32_t offset);
    void write8(uint32_t offset, uint8_t data);
    void write16(uint32_t offset, uint16_t data, uint16_t mem_mask = 0xffff);

    void midi_in(uint8_t byte);
    // The 68000's interrupt level: the highest line asserted (MAME's scsp
    // irq_cb drives one 68000 input line per level).
    int irq_level() const {
        for (int l = 7; l > 0; --l)
            if (irq_lines_ & (1u << l)) return l;
        return 0;
    }

    void set_timer_cb(TimerCb cb) { timer_cb_ = std::move(cb); }
    // Enhancement (default off, 1.0): extra gain for VF2's sound effects and
    // voices, not its music. VF2 streams everything through per-slot RAM
    // buffers (SA 0x10000-0x4ffff); effects and voices play there at TL
    // 0x00-0x0c (music never louder than 0x0d), and the attack sounds are
    // RAM-resident samples (SA from 0x50000) panned DIPAN 0x08.
    void set_effects_gain(float gain) { fx_gain_q8_ = int32_t(gain * 256.0f + 0.5f); }
    void timer_expire(int t);

    // Renders n stereo samples, output as interleaved floats (L, R, L, R...).
    void generate(float *out, int n);

private:
    enum SCSP_STATE { SCSP_ATTACK, SCSP_DECAY1, SCSP_DECAY2, SCSP_RELEASE };

    struct SCSP_EG_t {
        int volume = 0;
        SCSP_STATE state = SCSP_RELEASE;
        int step = 0;
        int AR = 0, D1R = 0, D2R = 0, RR = 0, DL = 0;
        uint8_t EGHOLD = 0, LPLINK = 0;
    };

    struct SCSP_LFO_t {
        uint16_t phase = 0;
        uint32_t phase_step = 0;
        int *table = nullptr;
        int *scale = nullptr;
    };

    struct SCSP_SLOT {
        union {
            uint16_t data[0x10] = {};
            uint8_t datab[0x20];
        } udata;

        uint8_t Backwards = 0;
        uint8_t active = 0;
        uint32_t cur_addr = 0;
        uint32_t nxt_addr = 0;
        uint32_t step = 0;
        SCSP_EG_t EG;
        SCSP_LFO_t PLFO;
        SCSP_LFO_t ALFO;
        int slot = 0;
        int16_t Prev = 0;
    };

    struct SCSPDSP {
        int16_t COEF[64] = {};
        uint16_t MADRS[32] = {};
        uint16_t MPRO[128 * 4] = {};
        int32_t TEMP[128] = {};
        int32_t MEMS[32] = {};
        uint32_t DEC = 0;
        int32_t MIXS[16] = {};
        int16_t EXTS[2] = {};
        int16_t EFREG[16] = {};

        uint32_t RBP = 0;
        uint32_t RBL = 8 * 1024;
        bool Stopped = true;
        int LastStep = 0;

        // MPRO decoded once per program change (Start), not per sample.
        struct Op {
            uint8_t TRA, TWT, TWA, XSEL, YSEL, IRA, IWT, IWA;
            uint8_t TABLE, MWT, MRD, EWT, EWA, ADRL, FRCL, DSHIFT, YRL, NEGB, ZERO, BSEL;
            uint8_t NOFL, COEF, MASA, ADREB, NXADR;
        };
        Op ops[128] = {};

        void Init();
        void SetSample(int32_t sample, int SEL, int MXL);
        void Step(Scsp &scsp);
        void Start();
    };

    uint8_t *ram_ = nullptr;
    uint32_t ram_mask_ = 0x7ffff;

    union {
        uint16_t data[0x30 / 2] = {};
        uint8_t datab[0x30];
    } udata_;

    SCSP_SLOT slots_[32];
    int16_t ringbuf_[128] = {};
    uint8_t bufptr_ = 0;
    SCSPDSP dsp_;
    int32_t fx_gain_q8_ = 256;
    uint16_t dsp_regs_7c0_[32] = {}; // 0x7c0-0x7ff (see w16)

    uint32_t irq_tim_a_ = 0, irq_tim_bc_ = 0, irq_midi_ = 0, irq_cpu_ = 0, irq_dma_ = 0;
    uint32_t irq_lines_ = 0; // bit n: 68000 IPL line n asserted
    void irq_set(uint32_t level, bool on) {
        if (level == 0) return;
        if (on) irq_lines_ |= 1u << level;
        else irq_lines_ &= ~(1u << level);
    }

    uint8_t latched_mslc_ = 0;

    uint8_t midi_out_stack_[32] = {};
    uint8_t midi_out_w_ = 0, midi_out_r_ = 0;
    uint8_t midi_stack_[32] = {};
    uint8_t midi_w_ = 0, midi_r_ = 0;

    int32_t eg_table_[0x400] = {};
    int lpantable_[0x10000] = {};
    int rpantable_[0x10000] = {};
    int artable_[64] = {};
    int drtable_[64] = {};

    int alfo_saw_[256] = {}, alfo_sqr_[256] = {}, alfo_tri_[256] = {}, alfo_noi_[256] = {};
    int plfo_saw_[256] = {}, plfo_sqr_[256] = {}, plfo_tri_[256] = {}, plfo_noi_[256] = {};
    int pscales_[8][256] = {};
    int ascales_[8][256] = {};

    int tim_pris_[3] = {1, 1, 1};
    int tim_cnt_[3] = {0xffff, 0xffff, 0xffff};
    int timer_accum_[3] = {0, 0, 0};

    struct Dma {
        uint32_t dmea = 0;
        uint16_t drga = 0, dtlg = 0;
        uint8_t dgate = 0, ddir = 0;
    } dma_;

    uint16_t mcieb_ = 0, mcipd_ = 0;
    uint32_t rng_ = 0x12345678;

    uint32_t rand() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return rng_;
    }

    int8_t read_byte(uint32_t addr) const { return int8_t(ram_[addr & ram_mask_]); }
    int16_t read_word(uint32_t addr) const {
        const uint32_t a = addr & ram_mask_;
        return int16_t((uint16_t(ram_[a]) << 8) | ram_[(a + 1) & ram_mask_]);
    }
    void write_word(uint32_t addr, uint16_t val) {
        const uint32_t a = addr & ram_mask_;
        ram_[a] = uint8_t(val >> 8);
        ram_[(a + 1) & ram_mask_] = uint8_t(val);
    }

    void init();
    void check_pending_irq();
    void reset_interrupts();
    uint8_t decode_sci(uint8_t irq);

    int get_ar(int base, int R);
    int get_dr(int base, int R);
    void compute_eg(SCSP_SLOT *slot);
    int eg_update(SCSP_SLOT *slot);
    uint32_t step(SCSP_SLOT *slot);
    void compute_lfo(SCSP_SLOT *slot);
    void start_slot(SCSP_SLOT *slot);
    void stop_slot(SCSP_SLOT *slot, int keyoff);

    void update_slot_reg(int s, int r);
    void update_reg(int reg);
    void update_slot_reg_r(int slot, int reg);
    void update_reg_r(int reg);

    void w16(uint32_t addr, uint16_t val, uint16_t mem_mask);
    uint16_t r16(uint32_t addr);

    int32_t update_slot(SCSP_SLOT *slot);
    void exec_dma();
    void lfo_init();
    int32_t plfo_step(SCSP_LFO_t *LFO);
    int32_t alfo_step(SCSP_LFO_t *LFO);
    void lfo_compute_step(SCSP_LFO_t *LFO, uint32_t LFOF, uint32_t LFOWS, uint32_t LFOS, int ALFO);

    TimerCb timer_cb_;
};

} // namespace snd
