// Yamaha YMF292-F (SCSP) emulation.
// Adapted from MAME src/devices/sound/scsp.cpp and scspdsp.cpp
// (license: BSD-3-Clause, copyright-holders: ElSemi, R. Belmont; THIRD_PARTY.md).
#include "runtime/scsp.h"
#include <cstdio>

namespace snd {

namespace {

inline int32_t sext(int32_t val, int bits) {
    const int shift = 32 - bits;
    return (val << shift) >> shift;
}

uint16_t PACK(int32_t val) {
    int const sign = (val >> 23) & 1;
    uint32_t temp = (val ^ (val << 1)) & 0xFFFFFF;
    int exponent = 0;
    for (int k = 0; k < 12; k++) {
        if (temp & 0x800000) break;
        temp <<= 1;
        exponent += 1;
    }
    if (exponent < 12) val = (val << exponent) & 0x3FFFFF;
    else val <<= 11;
    val >>= 11;
    val &= 0x7FF;
    val |= sign << 15;
    val |= exponent << 11;
    return uint16_t(val);
}

int32_t UNPACK(uint16_t val) {
    int const sign = (val >> 15) & 1;
    int exponent = (val >> 11) & 0xF;
    int const mantissa = val & 0x7FF;
    int32_t uval = mantissa << 11;
    if (exponent > 11) {
        exponent = 11;
        uval |= sign << 22;
    } else {
        uval |= (sign ^ 1) << 22;
    }
    uval |= sign << 23;
    uval <<= 8;
    uval >>= 8;
    uval >>= exponent;
    return uval;
}

const double ARTimes[64] = {
    100000, 100000, 8100.0, 6900.0, 6000.0, 4800.0, 4000.0, 3400.0, 3000.0, 2400.0, 2000.0, 1700.0, 1500.0,
    1200.0, 1000.0, 860.0, 760.0, 600.0, 500.0, 430.0, 380.0, 300.0, 250.0, 220.0, 190.0, 150.0, 130.0,
    110.0, 95.0, 76.0, 63.0, 55.0, 47.0, 38.0, 31.0, 27.0, 24.0, 19.0, 15.0, 13.0, 12.0, 9.4, 7.9, 6.8,
    6.0, 4.7, 3.8, 3.4, 3.0, 2.4, 2.0, 1.8, 1.6, 1.3, 1.1, 0.93, 0.85, 0.65, 0.53, 0.44, 0.40, 0.35, 0.0, 0.0
};

const double DRTimes[64] = {
    100000, 100000, 118200.0, 101300.0, 88600.0, 70900.0, 59100.0, 50700.0, 44300.0, 35500.0, 29600.0,
    25300.0, 22200.0, 17700.0, 14800.0, 12700.0, 11100.0, 8900.0, 7400.0, 6300.0, 5500.0, 4400.0, 3700.0,
    3200.0, 2800.0, 2200.0, 1800.0, 1600.0, 1400.0, 1100.0, 920.0, 790.0, 690.0, 550.0, 460.0, 390.0, 340.0,
    270.0, 230.0, 200.0, 170.0, 140.0, 110.0, 98.0, 85.0, 68.0, 57.0, 49.0, 43.0, 34.0, 28.0, 25.0, 22.0,
    18.0, 14.0, 12.0, 11.0, 8.5, 7.1, 6.1, 5.4, 4.3, 3.6, 3.1
};

const float SDLT[8] = {-1000000.0f, -36.0f, -30.0f, -24.0f, -18.0f, -12.0f, -6.0f, 0.0f};

const float LFOFreq[32] = {
    0.17f, 0.19f, 0.23f, 0.27f, 0.34f, 0.39f, 0.45f, 0.55f, 0.68f, 0.78f, 0.92f, 1.10f, 1.39f, 1.60f,
    1.87f, 2.27f, 2.87f, 3.31f, 3.92f, 4.79f, 6.15f, 7.18f, 8.60f, 10.8f, 14.4f, 17.2f, 21.5f, 28.7f,
    43.1f, 57.4f, 86.1f, 172.3f
};

const float ASCALE[8] = {0.0f, 0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f};
const float PSCALE[8] = {0.0f, 7.0f, 13.5f, 27.0f, 55.0f, 112.0f, 230.0f, 494.0f};

} // namespace

#define SHIFT 12
#define LFO_SHIFT 8
#define FIX(v) ((uint32_t)((float)(1 << SHIFT) * (v)))
#define EG_SHIFT 16

#define KEYONEX(slot) (((slot)->udata.data[0x0] >> 0x0) & 0x1000)
#define KEYONB(slot)  (((slot)->udata.data[0x0] >> 0x0) & 0x0800)
#define SBCTL(slot)   (((slot)->udata.data[0x0] >> 0x9) & 0x0003)
#define SSCTL(slot)   (((slot)->udata.data[0x0] >> 0x7) & 0x0003)
#define LPCTL(slot)   (((slot)->udata.data[0x0] >> 0x5) & 0x0003)
#define PCM8B(slot)   (((slot)->udata.data[0x0] >> 0x0) & 0x0010)

#define SA(slot)      ((((slot)->udata.data[0x0] & 0xF) << 16) | ((slot)->udata.data[0x1]))
#define LSA(slot)     ((slot)->udata.data[0x2])
#define LEA(slot)     ((slot)->udata.data[0x3])

#define D2R(slot)     (((slot)->udata.data[0x4] >> 0xB) & 0x001F)
#define D1R(slot)     (((slot)->udata.data[0x4] >> 0x6) & 0x001F)
#define EGHOLD(slot)  (((slot)->udata.data[0x4] >> 0x0) & 0x0020)
#define AR(slot)      (((slot)->udata.data[0x4] >> 0x0) & 0x001F)

#define LPSLNK(slot)  (((slot)->udata.data[0x5] >> 0x0) & 0x4000)
#define KRS(slot)     (((slot)->udata.data[0x5] >> 0xA) & 0x000F)
#define DL(slot)      (((slot)->udata.data[0x5] >> 0x5) & 0x001F)
#define RR(slot)      (((slot)->udata.data[0x5] >> 0x0) & 0x001F)

#define STWINH(slot)  (((slot)->udata.data[0x6] >> 0x0) & 0x0200)
#define SDIR(slot)    (((slot)->udata.data[0x6] >> 0x0) & 0x0100)
#define TL(slot)      (((slot)->udata.data[0x6] >> 0x0) & 0x00FF)

#define MDL(slot)     (((slot)->udata.data[0x7] >> 0xC) & 0x000F)
#define MDXSL(slot)   (((slot)->udata.data[0x7] >> 0x6) & 0x003F)
#define MDYSL(slot)   (((slot)->udata.data[0x7] >> 0x0) & 0x003F)

#define OCT(slot)     (((slot)->udata.data[0x8] >> 0xB) & 0x000F)
#define FNS(slot)     (((slot)->udata.data[0x8] >> 0x0) & 0x03FF)

#define LFORE(slot)   (((slot)->udata.data[0x9] >> 0x0) & 0x8000)
#define LFOF(slot)    (((slot)->udata.data[0x9] >> 0xA) & 0x001F)
#define PLFOWS(slot)  (((slot)->udata.data[0x9] >> 0x8) & 0x0003)
#define PLFOS(slot)   (((slot)->udata.data[0x9] >> 0x5) & 0x0007)
#define ALFOWS(slot)  (((slot)->udata.data[0x9] >> 0x3) & 0x0003)
#define ALFOS(slot)   (((slot)->udata.data[0x9] >> 0x0) & 0x0007)

#define ISEL(slot)    (((slot)->udata.data[0xA] >> 0x3) & 0x000F)
#define IMXL(slot)    (((slot)->udata.data[0xA] >> 0x0) & 0x0007)

#define DISDL(slot)   (((slot)->udata.data[0xB] >> 0xD) & 0x0007)
#define DIPAN(slot)   (((slot)->udata.data[0xB] >> 0x8) & 0x001F)
#define EFSDL(slot)   (((slot)->udata.data[0xB] >> 0x5) & 0x0007)
#define EFPAN(slot)   (((slot)->udata.data[0xB] >> 0x0) & 0x001F)

#define DAC18B()      ((udata_.data[0] >> 0x0) & 0x0100)
#define MVOL()        ((udata_.data[0] >> 0x0) & 0x000F)
#define RBL()         ((udata_.data[1] >> 0x7) & 0x0003)
#define RBP()         ((udata_.data[1] >> 0x0) & 0x003F)

#define SCILV0()      ((udata_.data[0x24/2] >> 0x0) & 0xff)
#define SCILV1()      ((udata_.data[0x26/2] >> 0x0) & 0xff)
#define SCILV2()      ((udata_.data[0x28/2] >> 0x0) & 0xff)
#define SCIEX0 0
#define SCIEX1 1
#define SCIEX2 2
#define SCIMID 3
#define SCIDMA 4
#define SCIIRQ 5
#define SCITMA 6
#define SCITMB 7

#define LFIX(v) ((uint32_t)((float)(1 << LFO_SHIFT) * (v)))
#define DB(v)   LFIX(powf(10.0f, (v) / 20.0f))
#define CENTS(v) LFIX(powf(2.0f, (v) / 1200.0f))

void Scsp::SCSPDSP::Init() {
    std::memset(this, 0, sizeof(*this));
    RBL = 8 * 1024;
    Stopped = true;
}

void Scsp::SCSPDSP::SetSample(int32_t sample, int SEL, int) {
    MIXS[SEL] += sample;
}

void Scsp::SCSPDSP::Start() {
    Stopped = false;
    int i;
    for (i = 127; i >= 0; --i) {
        uint16_t const *const IPtr = MPRO + (i * 4);
        if (IPtr[0] || IPtr[1] || IPtr[2] || IPtr[3]) break;
    }
    LastStep = (i >= 0) ? (i + 1) : 0;
    if (LastStep == 0) Stopped = true;
    for (int step = 0; step < LastStep; ++step) {
        uint16_t const *const IPtr = MPRO + (step * 4);
        Op &o = ops[step];
        o.TRA   = (IPtr[0] >>  8) & 0x7f;
        o.TWT   = (IPtr[0] >>  7) & 0x01;
        o.TWA   = (IPtr[0] >>  0) & 0x7f;
        o.XSEL  = (IPtr[1] >> 15) & 0x01;
        o.YSEL  = (IPtr[1] >> 13) & 0x03;
        o.IRA   = (IPtr[1] >>  6) & 0x3f;
        o.IWT   = (IPtr[1] >>  5) & 0x01;
        o.IWA   = (IPtr[1] >>  0) & 0x1f;
        o.TABLE = (IPtr[2] >> 15) & 0x01;
        o.MWT   = (IPtr[2] >> 14) & 0x01;
        o.MRD   = (IPtr[2] >> 13) & 0x01;
        o.EWT   = (IPtr[2] >> 12) & 0x01;
        o.EWA   = (IPtr[2] >>  8) & 0x0f;
        o.ADRL  = (IPtr[2] >>  7) & 0x01;
        o.FRCL  = (IPtr[2] >>  6) & 0x01;
        o.DSHIFT = (IPtr[2] >>  4) & 0x03;
        o.YRL   = (IPtr[2] >>  3) & 0x01;
        o.NEGB  = (IPtr[2] >>  2) & 0x01;
        o.ZERO  = (IPtr[2] >>  1) & 0x01;
        o.BSEL  = (IPtr[2] >>  0) & 0x01;
        o.NOFL  = (IPtr[3] >> 15) & 0x01;
        o.COEF  = (IPtr[3] >>  9) & 0x3f;
        o.MASA  = (IPtr[3] >>  2) & 0x1f;
        o.ADREB = (IPtr[3] >>  1) & 0x01;
        o.NXADR = (IPtr[3] >>  0) & 0x01;
    }
}

void Scsp::SCSPDSP::Step(Scsp &scsp) {
    if (Stopped) return;
    std::fill(std::begin(EFREG), std::end(EFREG), 0);

    int32_t ACC = 0;
    int32_t MEMVAL = 0;
    int32_t FRC_REG = 0;
    int32_t Y_REG = 0;
    uint32_t ADRS_REG = 0;

    for (int step = 0; step < LastStep; ++step) {
        const Op &o = ops[step];
        uint32_t const TRA = o.TRA, TWT = o.TWT, TWA = o.TWA;
        uint32_t const XSEL = o.XSEL, YSEL = o.YSEL, IRA = o.IRA, IWT = o.IWT, IWA = o.IWA;
        uint32_t const TABLE = o.TABLE, MWT = o.MWT, MRD = o.MRD, EWT = o.EWT, EWA = o.EWA, ADRL = o.ADRL;
        uint32_t const FRCL = o.FRCL, DSP_SHIFT = o.DSHIFT, YRL = o.YRL, NEGB = o.NEGB, ZERO = o.ZERO, BSEL = o.BSEL;
        uint32_t const NOFL = o.NOFL, COEF = o.COEF, MASA = o.MASA, ADREB = o.ADREB, NXADR = o.NXADR;

        int32_t INPUTS;
        if (IRA <= 0x1f) INPUTS = MEMS[IRA];
        else if (IRA <= 0x2F) INPUTS = MIXS[IRA - 0x20] << 4;
        else if (IRA <= 0x31) INPUTS = EXTS[IRA - 0x30] << 8;
        else INPUTS = 0;

        INPUTS = sext(INPUTS, 24);

        if (IWT) {
            MEMS[IWA] = MEMVAL;
            if (IRA == IWA) INPUTS = MEMVAL;
        }

        int32_t B;
        if (!ZERO) {
            if (BSEL) B = ACC;
            else B = sext(TEMP[(TRA + DEC) & 0x7f], 24);
            if (NEGB) B = 0 - B;
        } else B = 0;

        int32_t X;
        if (XSEL) X = INPUTS;
        else X = sext(TEMP[(TRA + DEC) & 0x7f], 24);

        int32_t Y = 0;
        if (YSEL == 0) Y = FRC_REG;
        else if (YSEL == 1) Y = this->COEF[COEF] >> 3;
        else if (YSEL == 2) Y = (Y_REG >> 11) & 0x1fff;
        else if (YSEL == 3) Y = (Y_REG >> 4) & 0x0fff;

        if (YRL) Y_REG = INPUTS;

        int32_t SHIFTED = 0;
        if (DSP_SHIFT == 0) SHIFTED = std::clamp<int32_t>(ACC, -0x00800000, 0x007fffff);
        else if (DSP_SHIFT == 1) SHIFTED = std::clamp<int32_t>(ACC * 2, -0x00800000, 0x007fffff);
        else if (DSP_SHIFT == 2) SHIFTED = sext(ACC * 2, 24);
        else if (DSP_SHIFT == 3) SHIFTED = sext(ACC, 24);

        Y = sext(Y, 13);
        int64_t const v = (int64_t(X) * int64_t(Y)) >> 12;
        ACC = int32_t(v + B);

        if (TWT) TEMP[(TWA + DEC) & 0x7f] = SHIFTED;

        if (FRCL) {
            if (DSP_SHIFT == 3) FRC_REG = SHIFTED & 0x0fff;
            else FRC_REG = (SHIFTED >> 11) & 0x1fff;
        }

        if (MRD || MWT) {
            uint32_t ADDR = MADRS[MASA];
            if (!TABLE) ADDR += DEC;
            if (ADREB) ADDR += ADRS_REG & 0x0FFF;
            if (NXADR) ADDR++;
            if (!TABLE) ADDR &= RBL - 1;
            else ADDR &= 0xffff;
            ADDR += RBP << 12;
            ADDR <<= 1;
            if (MRD && (step & 1)) {
                if (NOFL) MEMVAL = scsp.read_word(ADDR) << 8;
                else MEMVAL = UNPACK(uint16_t(scsp.read_word(ADDR)));
            }
            if (MWT && (step & 1)) {
                if (NOFL) scsp.write_word(ADDR, uint16_t(SHIFTED >> 8));
                else scsp.write_word(ADDR, PACK(SHIFTED));
            }
        }

        if (ADRL) {
            if (DSP_SHIFT == 3) ADRS_REG = (SHIFTED >> 12) & 0xfff;
            else ADRS_REG = uint32_t(INPUTS) >> 16;
        }

        if (EWT) EFREG[EWA] += int16_t(SHIFTED >> 8);
    }
    --DEC;
    std::fill(std::begin(MIXS), std::end(MIXS), 0);
}

Scsp::Scsp(uint8_t *sound_ram, uint32_t ram_size)
    : ram_(sound_ram), ram_mask_(ram_size - 1) {
    init();
}

void Scsp::reset() {
    init();
}

void Scsp::init() {
    dsp_.Init();
    irq_tim_a_ = irq_tim_bc_ = irq_midi_ = irq_cpu_ = irq_dma_ = 0;
    irq_lines_ = 0;
    midi_r_ = midi_w_ = 0;
    midi_out_r_ = midi_out_w_ = 0;
    bufptr_ = 0;
    latched_mslc_ = 0;

    udata_ = {};
    std::memset(ringbuf_, 0, sizeof(ringbuf_));

    for (int i = 0; i < 0x400; ++i) {
        float envDB = ((float)(3 * (i - 0x3ff))) / 32.0f;
        float scale = (float)(1 << SHIFT);
        eg_table_[i] = (int32_t)(powf(10.0f, envDB / 20.0f) * scale);
    }

    for (int i = 0; i < 0x10000; ++i) {
        int iTL  = (i >> 0x0) & 0xff;
        int iPAN = (i >> 0x8) & 0x1f;
        int iSDL = (i >> 0xD) & 0x07;
        float SegaDB = 0.0f;

        if (iTL & 0x01) SegaDB -= 0.4f;
        if (iTL & 0x02) SegaDB -= 0.8f;
        if (iTL & 0x04) SegaDB -= 1.5f;
        if (iTL & 0x08) SegaDB -= 3.0f;
        if (iTL & 0x10) SegaDB -= 6.0f;
        if (iTL & 0x20) SegaDB -= 12.0f;
        if (iTL & 0x40) SegaDB -= 24.0f;
        if (iTL & 0x80) SegaDB -= 48.0f;

        float TL = powf(10.0f, SegaDB / 20.0f);

        SegaDB = 0;
        if (iPAN & 0x1) SegaDB -= 3.0f;
        if (iPAN & 0x2) SegaDB -= 6.0f;
        if (iPAN & 0x4) SegaDB -= 12.0f;
        if (iPAN & 0x8) SegaDB -= 24.0f;

        float PAN;
        if ((iPAN & 0xf) == 0xf) PAN = 0.0f;
        else PAN = powf(10.0f, SegaDB / 20.0f);

        float LPAN, RPAN;
        if (iPAN < 0x10) { LPAN = PAN; RPAN = 1.0f; }
        else { RPAN = PAN; LPAN = 1.0f; }

        float fSDL = iSDL ? powf(10.0f, (SDLT[iSDL]) / 20.0f) : 0.0f;

        lpantable_[i] = FIX((4.0f * LPAN * TL * fSDL));
        rpantable_[i] = FIX((4.0f * RPAN * TL * fSDL));
    }

    artable_[0] = drtable_[0] = 0;
    artable_[1] = drtable_[1] = 0;
    for (int i = 2; i < 64; ++i) {
        double t = ARTimes[i];
        if (t != 0.0) {
            double step_val = (1023 * 1000.0) / (44100.0 * t);
            double scale = (double)(1 << EG_SHIFT);
            artable_[i] = (int)(step_val * scale);
        } else {
            artable_[i] = 1024 << EG_SHIFT;
        }

        t = DRTimes[i];
        double step_val = (1023 * 1000.0) / (44100.0 * t);
        double scale = (double)(1 << EG_SHIFT);
        drtable_[i] = (int)(step_val * scale);
    }

    for (int i = 0; i < 32; ++i) {
        slots_[i].slot = i;
        slots_[i].active = 0;
        slots_[i].EG.state = SCSP_RELEASE;
    }

    lfo_init();
    udata_.data[0x20 / 2] = 0;
    tim_cnt_[0] = 0xffff;
    tim_cnt_[1] = 0xffff;
    tim_cnt_[2] = 0xffff;
}

uint8_t Scsp::decode_sci(uint8_t irq) {
    uint8_t SCI = 0;
    uint8_t v = (SCILV0() & (1 << irq)) ? 1 : 0;
    SCI |= v;
    v = (SCILV1() & (1 << irq)) ? 1 : 0;
    SCI |= v << 1;
    v = (SCILV2() & (1 << irq)) ? 1 : 0;
    SCI |= v << 2;
    return SCI;
}

// Interrupts as MAME's CheckPendingIRQ / ResetInterrupts / timer callbacks:
// a pending, enabled source asserts its line (one per check, in this
// priority), and the line stays asserted until SCIRE (or an empty MIDI FIFO)
// clears it. DMA and CPU interrupts are not routed to the 68000 here (VF2
// does not enable them; MAME only holds DMA's line when SCIEB bit 4 is set).
void Scsp::check_pending_irq() {
    uint32_t pend = udata_.data[0x20 / 2];
    const uint32_t en = udata_.data[0x1e / 2];
    if (midi_w_ != midi_r_) {
        udata_.data[0x20 / 2] |= 8;
        pend |= 8;
    }
    if (!pend) return;
    if ((pend & 0x40) && (en & 0x40)) { irq_set(irq_tim_a_, true); return; }
    if ((pend & 0x80) && (en & 0x80)) { irq_set(irq_tim_bc_, true); return; }
    if ((pend & 0x100) && (en & 0x100)) { irq_set(irq_tim_bc_, true); return; }
    if ((pend & 8) && (en & 8)) { irq_set(irq_midi_, true); return; }
}

void Scsp::timer_expire(int t) {
    if (t < 0 || t >= 3) return;
    tim_cnt_[t] = 0xffff;
    udata_.data[0x20 / 2] |= (0x40 << t);
    udata_.data[(0x18 + t * 2) / 2] = (udata_.data[(0x18 + t * 2) / 2] & 0xff00) | 0xff;
    check_pending_irq();
}

void Scsp::reset_interrupts() {
    const uint32_t reset = udata_.data[0x22 / 2];
    if (reset & 0x40) irq_set(irq_tim_a_, false);
    if (reset & 0x180) irq_set(irq_tim_bc_, false);
    if (reset & 0x8) irq_set(irq_midi_, false);
    check_pending_irq();
}

int Scsp::get_ar(int base, int R) {
    int Rate = base + (R << 1);
    return artable_[std::clamp(Rate, 0, 63)];
}

int Scsp::get_dr(int base, int R) {
    int Rate = base + (R << 1);
    return drtable_[std::clamp(Rate, 0, 63)];
}

void Scsp::compute_eg(SCSP_SLOT *slot) {
    int octave = (OCT(slot) ^ 8) - 8;
    int rate = (KRS(slot) != 0xf) ? (octave + 2 * KRS(slot) + ((FNS(slot) >> 9) & 1)) : 0;
    slot->EG.volume = 0x17F << EG_SHIFT;
    slot->EG.AR = get_ar(rate, AR(slot));
    slot->EG.D1R = get_dr(rate, D1R(slot));
    slot->EG.D2R = get_dr(rate, D2R(slot));
    slot->EG.RR = get_dr(rate, RR(slot));
    slot->EG.DL = 0x1f - DL(slot);
    slot->EG.EGHOLD = EGHOLD(slot);
}

int Scsp::eg_update(SCSP_SLOT *slot) {
    switch (slot->EG.state) {
    case SCSP_ATTACK:
        slot->EG.volume += slot->EG.AR;
        if (slot->EG.volume >= (0x3ff << EG_SHIFT)) {
            if (!LPSLNK(slot)) {
                slot->EG.state = SCSP_DECAY1;
                if (slot->EG.D1R >= (1024 << EG_SHIFT)) {
                    slot->EG.state = SCSP_DECAY2;
                }
            }
            slot->EG.volume = 0x3ff << EG_SHIFT;
        }
        if (slot->EG.EGHOLD) return 0x3ff << (SHIFT - 10);
        break;
    case SCSP_DECAY1:
        slot->EG.volume -= slot->EG.D1R;
        if (slot->EG.volume <= 0) slot->EG.volume = 0;
        if ((slot->EG.volume >> (EG_SHIFT + 5)) <= slot->EG.DL) {
            slot->EG.state = SCSP_DECAY2;
        }
        break;
    case SCSP_DECAY2:
        if (D2R(slot) == 0) return (slot->EG.volume >> EG_SHIFT) << (SHIFT - 10);
        slot->EG.volume -= slot->EG.D2R;
        if (slot->EG.volume <= 0) slot->EG.volume = 0;
        break;
    case SCSP_RELEASE:
        slot->EG.volume -= slot->EG.RR;
        if (slot->EG.volume <= 0) {
            slot->EG.volume = 0;
            stop_slot(slot, 0);
        }
        break;
    }
    return (slot->EG.volume >> EG_SHIFT) << (SHIFT - 10);
}

uint32_t Scsp::step(SCSP_SLOT *slot) {
    int octave = (OCT(slot) ^ 8) - 8 + SHIFT - 10;
    uint32_t Fn = FNS(slot) + (1 << 10);
    if (octave >= 0) Fn <<= octave;
    else Fn >>= -octave;
    return Fn;
}

void Scsp::compute_lfo(SCSP_SLOT *slot) {
    if (PLFOS(slot)) lfo_compute_step(&(slot->PLFO), LFOF(slot), PLFOWS(slot), PLFOS(slot), 0);
    if (ALFOS(slot)) lfo_compute_step(&(slot->ALFO), LFOF(slot), ALFOWS(slot), ALFOS(slot), 1);
}

void Scsp::start_slot(SCSP_SLOT *slot) {
    slot->active = 1;
    slot->cur_addr = 0;
    slot->nxt_addr = 1 << SHIFT;
    slot->step = step(slot);
    compute_eg(slot);
    slot->EG.state = SCSP_ATTACK;
    slot->EG.volume = 0x17F << EG_SHIFT;
    slot->Prev = 0;
    slot->Backwards = 0;

    compute_lfo(slot);
}

void Scsp::stop_slot(SCSP_SLOT *slot, int keyoff) {
    if (keyoff) {
        slot->EG.state = SCSP_RELEASE;
    } else {
        slot->active = 0;
        slot->udata.data[0] &= ~0x800;
    }
    slot->udata.data[0] &= ~0x1000;
}

void Scsp::update_slot_reg(int s, int r) {
    SCSP_SLOT *slot = slots_ + s;
    switch (r & 0x3f) {
    case 0:
    case 1:
        if (KEYONEX(slot)) {
            for (int sl = 0; sl < 32; ++sl) {
                SCSP_SLOT *s2 = slots_ + sl;
                if (KEYONB(s2) && s2->EG.state == SCSP_RELEASE) start_slot(s2);
                if (!KEYONB(s2)) stop_slot(s2, 1);
            }
            slot->udata.data[0] &= ~0x1000;
        }
        break;
    case 0x10:
    case 0x11:
        slot->step = step(slot);
        break;
    case 0xA:
    case 0xB:
        slot->EG.RR = get_dr(0, RR(slot));
        slot->EG.DL = 0x1f - DL(slot);
        break;
    case 0x12:
    case 0x13:
        compute_lfo(slot);
        break;
    }
}

void Scsp::update_reg(int reg) {
    switch (reg & 0x3f) {
    case 0x2:
    case 0x3:
        dsp_.RBL = (8 * 1024) << RBL();
        dsp_.RBP = RBP();
        break;
    case 0x6:
    case 0x7: {
        uint8_t data = udata_.data[0x6 / 2] & 0xff;
        midi_out_stack_[midi_out_w_++] = data;
        midi_out_w_ &= 31;
        break;
    }
    case 8:
    case 9:
        latched_mslc_ = (udata_.data[0x8 / 2] & 0xf800) >> 11;
        break;
    case 0x12:
    case 0x13:
        dma_.dmea = (udata_.data[0x12 / 2] & 0xfffe) | (dma_.dmea & 0xf0000);
        break;
    case 0x14:
    case 0x15:
        dma_.dmea = ((udata_.data[0x14 / 2] & 0xf000) << 4) | (dma_.dmea & 0xfffe);
        dma_.drga = (udata_.data[0x14 / 2] & 0x0ffe);
        break;
    case 0x16:
    case 0x17:
        dma_.dtlg = (udata_.data[0x16 / 2] & 0x0ffe);
        dma_.ddir = (udata_.data[0x16 / 2] & 0x2000) >> 13;
        dma_.dgate = (udata_.data[0x16 / 2] & 0x4000) >> 14;
        if (udata_.data[0x16 / 2] & 0x1000) exec_dma();
        break;
    case 0x18:
    case 0x19:
        tim_pris_[0] = 1 << ((udata_.data[0x18 / 2] >> 8) & 0x7);
        tim_cnt_[0] = (udata_.data[0x18 / 2] & 0xff) << 8;
        if (timer_cb_) timer_cb_(0, tim_pris_[0], udata_.data[0x18 / 2] & 0xff);
        break;
    case 0x1a:
    case 0x1b:
        tim_pris_[1] = 1 << ((udata_.data[0x1A / 2] >> 8) & 0x7);
        tim_cnt_[1] = (udata_.data[0x1A / 2] & 0xff) << 8;
        if (timer_cb_) timer_cb_(1, tim_pris_[1], udata_.data[0x1A / 2] & 0xff);
        break;
    case 0x1c:
    case 0x1d:
        tim_pris_[2] = 1 << ((udata_.data[0x1C / 2] >> 8) & 0x7);
        tim_cnt_[2] = (udata_.data[0x1C / 2] & 0xff) << 8;
        if (timer_cb_) timer_cb_(2, tim_pris_[2], udata_.data[0x1C / 2] & 0xff);
        break;
    case 0x1e: // SCIEB
    case 0x1f:
        check_pending_irq();
        break;
    case 0x20: // SCIPD: stored as written (MAME)
    case 0x21:
        break;
    case 0x22: // SCIRE
    case 0x23:
        udata_.data[0x20 / 2] &= ~udata_.data[0x22 / 2];
        reset_interrupts();
        // real hardware (MAME, saturn:sakurat): a timer reset while expired
        // is pending again at once
        if (tim_cnt_[0] == 0xffff) udata_.data[0x20 / 2] |= 0x40;
        if (tim_cnt_[1] == 0xffff) udata_.data[0x20 / 2] |= 0x80;
        if (tim_cnt_[2] == 0xffff) udata_.data[0x20 / 2] |= 0x100;
        break;
    case 0x24:
    case 0x25:
    case 0x26:
    case 0x27:
    case 0x28:
    case 0x29:
        irq_tim_a_ = decode_sci(SCITMA);
        irq_tim_bc_ = decode_sci(SCITMB);
        irq_midi_ = decode_sci(SCIMID);
        irq_dma_ = decode_sci(SCIDMA);
        irq_cpu_ = decode_sci(SCIIRQ);
        break;
    case 0x2a:
    case 0x2b:
        mcieb_ = udata_.data[0x2a / 2];
        break;
    case 0x2c:
    case 0x2d:
        mcipd_ |= udata_.data[0x2c / 2];
        break;
    case 0x2e:
    case 0x2f:
        mcipd_ &= ~udata_.data[0x2e / 2];
        break;
    }
}

void Scsp::update_slot_reg_r(int, int) {}

void Scsp::update_reg_r(int reg) {
    switch (reg & 0x3f) {
    case 4:
    case 5: {
        uint16_t v = udata_.data[0x4 / 2] & 0xff00;
        v |= midi_stack_[midi_r_];
        if (midi_r_ != midi_w_) {
            ++midi_r_;
            midi_r_ &= 31;
        }
        if (midi_r_ == midi_w_) { // FIFO empty: clear the MIDI line
            irq_set(irq_midi_, false);
            udata_.data[0x20 / 2] &= ~8;
        }
        udata_.data[0x4 / 2] = v;
        break;
    }
    case 8:
    case 9: {
        // MSLC monitor: the selected slot's state now. MAME latches it once
        // per sample, which here (samples rendered in batches, the 68000 run
        // ahead of them) would answer for the previously selected slot: VF2
        // writes MSLC, waits ten instructions and reads CA to pick the half
        // of a slot's 8 KB stream buffer to refill from the sample ROMs.
        const SCSP_SLOT *slot = slots_ + latched_mslc_;
        const uint32_t SGC = uint32_t(slot->EG.state) & 3;
        const uint32_t CA = (slot->cur_addr >> (SHIFT + 12)) & 0xf;
        const uint32_t EG = (0x1f - (slot->EG.volume >> (EG_SHIFT + 5))) & 0x1f;
        udata_.data[0x8 / 2] = uint16_t((CA << 7) | (SGC << 5) | EG);
        break;
    }
    case 0x2a:
    case 0x2b:
        udata_.data[0x2a / 2] = mcieb_;
        break;
    case 0x2c:
    case 0x2d:
        udata_.data[0x2c / 2] = mcipd_;
        break;
    }
}

void Scsp::w16(uint32_t addr, uint16_t val, uint16_t mem_mask) {
    const uint32_t reg = (addr & 0xffff) & ~1u;
    if (reg < 0x400) {
        int slot = reg / 0x20;
        uint16_t *p = &slots_[slot].udata.data[(reg & 0x1f) / 2];
        *p = (*p & ~mem_mask) | (val & mem_mask);
        update_slot_reg(slot, reg & 0x1f);
    } else if (reg < 0x600) {
        if (reg < 0x430) {
            uint16_t *p = &udata_.data[(reg & 0x3f) / 2];
            *p = (*p & ~mem_mask) | (val & mem_mask);
            update_reg(reg & 0x3f);
        }
    } else if (reg < 0x700) {
        uint16_t *p = (uint16_t *)&ringbuf_[(reg - 0x600) / 2];
        *p = (*p & ~mem_mask) | (val & mem_mask);
    } else {
        if (reg < 0x780) {
            uint16_t *p = (uint16_t *)(dsp_.COEF + (reg - 0x700) / 2);
            *p = (*p & ~mem_mask) | (val & mem_mask);
        } else if (reg < 0x7c0) {
            uint16_t *p = (uint16_t *)(dsp_.MADRS + (reg - 0x780) / 2);
            *p = (*p & ~mem_mask) | (val & mem_mask);
        } else if (reg < 0x800) {
            // Not a mirror of MADRS (MAME mirrors it): VF2 programs the DSP
            // by writing 0x700-0x7ff in one pass, MADRS at 0x780-0x7bf and
            // then zeros here; as a mirror the zeros wipe every delay-line
            // address and the reverb collapses into a loud smear.
            uint16_t *p = &dsp_regs_7c0_[(reg - 0x7c0) / 2];
            *p = (*p & ~mem_mask) | (val & mem_mask);
        } else if (reg < 0xC00) {
            uint16_t *p = (uint16_t *)(dsp_.MPRO + (reg - 0x800) / 2);
            *p = (*p & ~mem_mask) | (val & mem_mask);
            dsp_.Start();
        }
    }
}

uint16_t Scsp::r16(uint32_t addr) {
    uint16_t v = 0;
    const uint32_t reg = (addr & 0xffff) & ~1u;
    if (reg < 0x400) {
        int slot = reg / 0x20;
        update_slot_reg_r(slot, reg & 0x1f);
        v = slots_[slot].udata.data[(reg & 0x1f) / 2];
    } else if (reg < 0x600) {
        if (reg < 0x430) {
            update_reg_r(reg & 0x3f);
            v = udata_.data[(reg & 0x3f) / 2];
        }
    } else if (reg < 0x700) {
        v = ringbuf_[(reg - 0x600) / 2];
    } else {
        if (reg < 0x780) v = *((uint16_t *)(dsp_.COEF + (reg - 0x700) / 2));
        else if (reg < 0x7c0) v = *((uint16_t *)(dsp_.MADRS + (reg - 0x780) / 2));
        else if (reg < 0x800) v = dsp_regs_7c0_[(reg - 0x7c0) / 2];
        else if (reg < 0xC00) v = *((uint16_t *)(dsp_.MPRO + (reg - 0x800) / 2));
        else if (reg < 0xE00) {
            if (reg & 2) v = dsp_.TEMP[(reg >> 2) & 0x7f] & 0xffff;
            else v = dsp_.TEMP[(reg >> 2) & 0x7f] >> 16;
        } else if (reg < 0xE80) {
            if (reg & 2) v = dsp_.MEMS[(reg >> 2) & 0x1f] & 0xffff;
            else v = dsp_.MEMS[(reg >> 2) & 0x1f] >> 16;
        } else if (reg < 0xEC0) {
            if (reg & 2) v = dsp_.MIXS[(reg >> 2) & 0xf] & 0xffff;
            else v = dsp_.MIXS[(reg >> 2) & 0xf] >> 16;
        } else if (reg < 0xEE0) {
            v = *((uint16_t *)(dsp_.EFREG + (reg - 0xec0) / 2));
        } else if (reg < 0xEE4) {
            v = *((uint16_t *)(dsp_.EXTS + (reg - 0xee0) / 2));
        }
    }
    return v;
}

uint8_t Scsp::read8(uint32_t offset) {
    if (offset & 1) return uint8_t(r16(offset & ~1u) & 0xff);
    return uint8_t(r16(offset) >> 8);
}

uint16_t Scsp::read16(uint32_t offset) {
    return r16(offset);
}

void Scsp::write8(uint32_t offset, uint8_t data) {
    if (offset & 1) w16(offset & ~1u, data & 0xff, 0x00ff);
    else w16(offset, uint16_t(data) << 8, 0xff00);
}

void Scsp::write16(uint32_t offset, uint16_t data, uint16_t mem_mask) {
    w16(offset, data, mem_mask);
}

void Scsp::midi_in(uint8_t byte) {
    midi_stack_[midi_w_++] = byte;
    midi_w_ &= 31;
    check_pending_irq();
}

int32_t Scsp::update_slot(SCSP_SLOT *slot) {
    if (SSCTL(slot) == 3) return 0;

    int32_t sample = 0;
    int step_val = slot->step;
    uint32_t addr1, addr2, addr_select;
    uint32_t *addr[2] = {&addr1, &addr2};
    uint32_t *slot_addr[2] = {&(slot->cur_addr), &(slot->nxt_addr)};

    if (PLFOS(slot) != 0) {
        step_val = step_val * plfo_step(&(slot->PLFO));
        step_val >>= SHIFT;
    }

    if (PCM8B(slot)) {
        addr1 = slot->cur_addr >> SHIFT;
        addr2 = slot->nxt_addr >> SHIFT;
    } else {
        addr1 = (slot->cur_addr >> (SHIFT - 1)) & ~1;
        addr2 = (slot->nxt_addr >> (SHIFT - 1)) & ~1;
    }

    if (MDL(slot) != 0 || MDXSL(slot) != 0 || MDYSL(slot) != 0) {
        int32_t smp = (ringbuf_[(bufptr_ + MDXSL(slot)) & 63] + ringbuf_[(bufptr_ + MDYSL(slot)) & 63]) / 2;
        smp <<= 0xA;
        smp >>= 0x1A - MDL(slot);
        if (!PCM8B(slot)) smp <<= 1;
        addr1 += smp;
        addr2 += smp;
    }

    if (SSCTL(slot) == 0) {
        if (PCM8B(slot)) {
            int8_t p1 = read_byte(SA(slot) + addr1);
            int8_t p2 = read_byte(SA(slot) + addr2);
            int32_t fpart = slot->cur_addr & ((1 << SHIFT) - 1);
            int32_t s = (int)(p1 << 8) * ((1 << SHIFT) - fpart) + (int)(p2 << 8) * fpart;
            sample = (s >> SHIFT);
        } else {
            int16_t p1 = read_word(SA(slot) + addr1);
            int16_t p2 = read_word(SA(slot) + addr2);
            int32_t fpart = slot->cur_addr & ((1 << SHIFT) - 1);
            int32_t s = (int)(p1) * ((1 << SHIFT) - fpart) + (int)(p2) * fpart;
            sample = (s >> SHIFT);
        }
    } else if (SSCTL(slot) == 1) {
        sample = (int16_t)(rand() & 0xffff);
    } else {
        sample = 0;
    }

    if (SBCTL(slot) & 0x1) sample ^= 0x7FFF;
    if (SBCTL(slot) & 0x2) sample = (int16_t)(sample ^ 0x8000);

    if (slot->Backwards) slot->cur_addr -= step_val;
    else slot->cur_addr += step_val;
    slot->nxt_addr = slot->cur_addr + (1 << SHIFT);

    addr1 = slot->cur_addr >> SHIFT;
    addr2 = slot->nxt_addr >> SHIFT;

    if (addr1 >= LSA(slot) && !(slot->Backwards)) {
        if (LPSLNK(slot) && slot->EG.state == SCSP_ATTACK) slot->EG.state = SCSP_DECAY1;
    }

    for (addr_select = 0; addr_select < 2; addr_select++) {
        int32_t rem_addr;
        switch (LPCTL(slot)) {
        case 0:
            if (*addr[addr_select] >= LSA(slot) && *addr[addr_select] >= LEA(slot)) {
                stop_slot(slot, 0);
            }
            break;
        case 1:
            if (*addr[addr_select] >= LEA(slot)) {
                rem_addr = *slot_addr[addr_select] - (LEA(slot) << SHIFT);
                *slot_addr[addr_select] = (LSA(slot) << SHIFT) + rem_addr;
            }
            break;
        case 2:
            if ((*addr[addr_select] >= LSA(slot)) && !(slot->Backwards)) {
                rem_addr = *slot_addr[addr_select] - (LSA(slot) << SHIFT);
                *slot_addr[addr_select] = (LEA(slot) << SHIFT) - rem_addr;
                slot->Backwards = 1;
            } else if ((*addr[addr_select] < LSA(slot) || (*slot_addr[addr_select] & 0x80000000)) && slot->Backwards) {
                rem_addr = (LSA(slot) << SHIFT) - *slot_addr[addr_select];
                *slot_addr[addr_select] = (LEA(slot) << SHIFT) - rem_addr;
            }
            break;
        case 3:
            if (*addr[addr_select] >= LEA(slot)) {
                rem_addr = *slot_addr[addr_select] - (LEA(slot) << SHIFT);
                *slot_addr[addr_select] = (LEA(slot) << SHIFT) - rem_addr;
                slot->Backwards = 1;
            } else if ((*addr[addr_select] < LSA(slot) || (*slot_addr[addr_select] & 0x80000000)) && slot->Backwards) {
                rem_addr = (LSA(slot) << SHIFT) - *slot_addr[addr_select];
                *slot_addr[addr_select] = (LSA(slot) << SHIFT) + rem_addr;
                slot->Backwards = 0;
            }
            break;
        }
    }

    if (!SDIR(slot)) {
        if (ALFOS(slot) != 0) {
            sample = sample * alfo_step(&(slot->ALFO));
            sample >>= SHIFT;
        }
        if (slot->EG.state == SCSP_ATTACK) {
            sample = (sample * eg_update(slot)) >> SHIFT;
        } else {
            sample = (sample * eg_table_[eg_update(slot) >> (SHIFT - 10)]) >> SHIFT;
        }
    }

    if (!STWINH(slot)) {
        uint16_t Enc = SDIR(slot) ? (0x7 << 0xd) : (((TL(slot)) << 0x0) | (0x7 << 0xd));
        ringbuf_[bufptr_] = (sample * lpantable_[Enc]) >> (SHIFT + 1);
    }

    return sample;
}

void Scsp::generate(float *out, int n) {
    const float gain = float(MVOL()) / 15.0f;
    for (int s = 0; s < n; ++s) {
        int32_t smpl = 0, smpr = 0;

        for (int sl = 0; sl < 32; ++sl) {
            if (slots_[sl].active) {
                SCSP_SLOT *slot = slots_ + sl;
                int32_t sample = update_slot(slot);
                if (fx_gain_q8_ != 256 && (SA(slot) >= 0x50000 ? DIPAN(slot) == 0x08 : TL(slot) <= 0x0c))
                    sample = (sample * fx_gain_q8_) >> 8;

                uint16_t eff_tl = SDIR(slot) ? 0 : TL(slot);
                uint16_t Enc = ((eff_tl) << 0x0) | ((IMXL(slot)) << 0xd);
                dsp_.SetSample((sample * lpantable_[Enc]) >> (SHIFT - 2), ISEL(slot), IMXL(slot));

                uint16_t dir_tl = SDIR(slot) ? 0 : TL(slot);
                Enc = ((dir_tl) << 0x0) | ((DIPAN(slot)) << 0x8) | ((DISDL(slot)) << 0xd);
                smpl += (sample * lpantable_[Enc]) >> SHIFT;
                smpr += (sample * rpantable_[Enc]) >> SHIFT;
            }
            ++bufptr_;
            bufptr_ &= 63;
        }

        dsp_.Step(*this);

        for (int i = 0; i < 16; ++i) {
            SCSP_SLOT *slot = slots_ + i;
            if (EFSDL(slot)) {
                uint16_t Enc = ((EFPAN(slot)) << 0x8) | ((EFSDL(slot)) << 0xd);
                smpl += (dsp_.EFREG[i] * lpantable_[Enc]) >> SHIFT;
                smpr += (dsp_.EFREG[i] * rpantable_[Enc]) >> SHIFT;
            }
        }

        // Advance timers once per sample (44.1 kHz) if not driven by external scheduler
        if (!timer_cb_) {
            for (int t = 0; t < 3; ++t) {
                if (tim_pris_[t] > 0) {
                    if (++timer_accum_[t] >= tim_pris_[t]) {
                        timer_accum_[t] = 0;
                        tim_cnt_[t] += 0x100;
                        if ((tim_cnt_[t] >> 8) > 255) {
                            tim_cnt_[t] = 0xffff;
                            udata_.data[0x20 / 2] |= (0x40 << t);
                            check_pending_irq();
                        }
                    }
                }
            }
        }

        float l_f = float(std::clamp<int32_t>(smpl, -131072, 131071)) / 131072.0f;
        float r_f = float(std::clamp<int32_t>(smpr, -131072, 131071)) / 131072.0f;

        out[s * 2 + 0] = l_f * gain;
        out[s * 2 + 1] = r_f * gain;
    }
}

void Scsp::exec_dma() {
    // MAME: DMA cannot overwrite its own parameters (RAM to registers)
    uint16_t saved[3];
    for (int i = 0; i < 3; ++i) saved[i] = udata_.data[(0x12 + i * 2) / 2];
    if (dma_.ddir) {
        if (dma_.dgate) {
            for (int i = 0; i < dma_.dtlg; i += 2) {
                write_word(dma_.dmea, 0);
                dma_.dmea += 2;
            }
        } else {
            for (int i = 0; i < dma_.dtlg; i += 2) {
                uint16_t tmp = r16(dma_.drga);
                write_word(dma_.dmea, tmp);
                dma_.dmea += 2;
                dma_.drga += 2;
            }
        }
    } else {
        if (dma_.dgate) {
            for (int i = 0; i < dma_.dtlg; i += 2) {
                w16(dma_.drga, 0, 0xffff);
                dma_.drga += 2;
            }
        } else {
            for (int i = 0; i < dma_.dtlg; i += 2) {
                uint16_t tmp = read_word(dma_.dmea);
                w16(dma_.drga, tmp, 0xffff);
                dma_.dmea += 2;
                dma_.drga += 2;
            }
        }
    }
    if (!dma_.ddir)
        for (int i = 0; i < 3; ++i) udata_.data[(0x12 + i * 2) / 2] = saved[i];
    udata_.data[0x16 / 2] &= ~0x1000;
}

void Scsp::lfo_init() {
    for (int i = 0; i < 256; ++i) {
        int a = 255 - i;
        int p = (i < 128) ? i : (i - 256);
        alfo_saw_[i] = a;
        plfo_saw_[i] = p;

        if (i < 128) { a = 255; p = 127; }
        else { a = 0; p = -128; }
        alfo_sqr_[i] = a;
        plfo_sqr_[i] = p;

        if (i < 128) a = 255 - (i * 2);
        else a = (i * 2) - 256;
        if (i < 64) p = i * 2;
        else if (i < 128) p = 255 - i * 2;
        else if (i < 192) p = 256 - i * 2;
        else p = i * 2 - 511;
        alfo_tri_[i] = a;
        plfo_tri_[i] = p;

        a = rand() & 0xff;
        p = 128 - a;
        alfo_noi_[i] = a;
        plfo_noi_[i] = p;
    }

    for (int s = 0; s < 8; ++s) {
        float limit = PSCALE[s];
        for (int i = -128; i < 128; ++i) {
            pscales_[s][i + 128] = CENTS(((limit * (float)i) / 128.0f));
        }
        limit = -ASCALE[s];
        for (int i = 0; i < 256; ++i) {
            ascales_[s][i] = DB(((limit * (float)i) / 256.0f));
        }
    }
}

int32_t Scsp::plfo_step(SCSP_LFO_t *LFO) {
    LFO->phase += LFO->phase_step;
    int p = LFO->table[LFO->phase >> LFO_SHIFT];
    p = LFO->scale[p + 128];
    return p << (SHIFT - LFO_SHIFT);
}

int32_t Scsp::alfo_step(SCSP_LFO_t *LFO) {
    LFO->phase += LFO->phase_step;
    int p = LFO->table[LFO->phase >> LFO_SHIFT];
    p = LFO->scale[p];
    return p << (SHIFT - LFO_SHIFT);
}

void Scsp::lfo_compute_step(SCSP_LFO_t *LFO, uint32_t LFOF, uint32_t LFOWS, uint32_t LFOS, int ALFO) {
    float step_val = (float)LFOFreq[LFOF] * 256.0f / 44100.0f;
    LFO->phase_step = (uint32_t)((float)(1 << LFO_SHIFT) * step_val);
    if (ALFO) {
        switch (LFOWS) {
        case 0: LFO->table = alfo_saw_; break;
        case 1: LFO->table = alfo_sqr_; break;
        case 2: LFO->table = alfo_tri_; break;
        case 3: LFO->table = alfo_noi_; break;
        }
        LFO->scale = ascales_[LFOS];
    } else {
        switch (LFOWS) {
        case 0: LFO->table = plfo_saw_; break;
        case 1: LFO->table = plfo_sqr_; break;
        case 2: LFO->table = plfo_tri_; break;
        case 3: LFO->table = plfo_noi_; break;
        }
        LFO->scale = pscales_[LFOS];
    }
}

} // namespace snd
